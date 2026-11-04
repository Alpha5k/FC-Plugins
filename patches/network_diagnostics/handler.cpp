#include "handler.hpp"

#include "../soldier_state_sync/patch.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <algorithm>
#include <expected>
#include <format>
#include <intrin.h>
#include <utility>

namespace fc_plugins::network_diagnostics {
namespace {

// Samples the process QPC clock without changing the native caller's Win32 or Winsock error.
std::uint64_t qpc_stamp() noexcept {
    const auto win32 = GetLastError();
    const auto winsock = WSAGetLastError();
    LARGE_INTEGER value{};
    const auto valid = QueryPerformanceCounter(&value);
    WSASetLastError(winsock);
    SetLastError(win32);
    return valid ? static_cast<std::uint64_t>(value.QuadPart) : 0;
}

// Maps the producer's carrier to the serialized trace value, including unknown enum values.
trace::Carrier trace_carrier(PacketCarrier carrier) noexcept {
    using Source = PacketCarrier;
    switch (carrier) {
    case Source::Native:
        return trace::Carrier::Native;
    case Source::Direct:
        return trace::Carrier::Direct;
    case Source::NotSent:
        return trace::Carrier::NotSent;
    }

    return trace::Carrier::Unknown;
}

// Maps a producer route to the trace enum, whose numeric values differ from the public API.
trace::DirectRoute direct_route(DirectRoute route) noexcept {
    using Source = DirectRoute;
    switch (route) {
    case Source::Unclassified:
        return trace::DirectRoute::Unclassified;
    case Source::Negotiating:
        return trace::DirectRoute::Negotiating;
    case Source::Native:
        return trace::DirectRoute::Native;
    case Source::Direct:
        return trace::DirectRoute::Direct;
    }

    return trace::DirectRoute::Unclassified;
}

// Maps the association phase to its trace value; unknown phases use Terminal.
trace::DirectAssociationPhase direct_phase(DirectAssociationPhase phase) noexcept {
    using Source = DirectAssociationPhase;
    switch (phase) {
    case Source::Started:
        return trace::DirectAssociationPhase::Started;
    case Source::Terminal:
        return trace::DirectAssociationPhase::Terminal;
    case Source::Ended:
        return trace::DirectAssociationPhase::Ended;
    }

    return trace::DirectAssociationPhase::Terminal;
}

// Returns the selected carrier when the association has a native or direct route.
trace::Carrier association_carrier(trace::DirectRoute route) noexcept {
    if (route == trace::DirectRoute::Native) {
        return trace::Carrier::Native;
    }

    if (route == trace::DirectRoute::Direct) {
        return trace::Carrier::Direct;
    }

    return trace::Carrier::Unknown;
}

} // namespace

Handler::Handler(const Settings &settings, fc::Role role) noexcept : settings_(settings), role_(role) {}

void Handler::plan(fc::Plan &plan) {
    const auto network = select_network_layout(plan.profile());
    if (!network) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    const auto simulation = select_simulation_layout(plan.profile());
    if (!simulation) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }
    maximum_update_steps_memory_ = plan.access(simulation->max_update_steps);
    simulation_prediction_turn_memory_ = plan.access(simulation->prediction_turn);
    for (std::size_t index = 0; index < world_pass_memory_.size(); ++index) {
        world_pass_memory_[index] = plan.access(simulation->world_pass[index]);
    }
    get_joystick_index_ = plan.function(simulation->get_joystick_index);

    plan_pipeline(plan);
    if (settings_.capture == CaptureMode::Full) {
        plan_frame_timing(plan);
    }

    // Aggregate nested native sends and receives. Optional transport producers report their carrier and intake separately.
    plan.observe<GroupInvocation>(
        network->group_send,
        fc::before([this](std::int32_t destination, std::int32_t packet_type, void *group, GroupInvocation &invocation) noexcept {
            begin_group(destination, invocation);
            invocation.record.group = native::pointer_id(group);
            invocation.record.packet_type = packet_type;
            invocation.record.host_turn = host_turn_ != nullptr ? static_cast<std::uint32_t>(*host_turn_) : 0;
        }),
        fc::after([this](std::int32_t destination, std::int32_t, void *, const GroupInvocation &invocation) noexcept {
            finish_group(destination, invocation);
        }));

    plan.observe<SendInvocation>(
        network->final_send, fc::before([this](std::int32_t, const void *bytes, std::int32_t length, SendInvocation &invocation) noexcept {
            begin_send(bytes, length, invocation);
        }),
        fc::after([this](std::int32_t destination, const void *, std::int32_t length, std::int32_t result,
                         const SendInvocation &invocation) noexcept {
            finish_send(destination, length > 0 ? static_cast<std::uint32_t>(length) : 0, result, invocation);
        }));

    plan.observe<ReceiveInvocation>(network->receive_service, fc::before([this](ReceiveInvocation &invocation) noexcept {
                                        begin_receive(invocation);
                                    }),
                                    fc::after([this](const ReceiveInvocation &invocation) noexcept {
                                        finish_receive(invocation);
                                    }));

    // RecvPacket can queue or discard the packet; returning from this void function measures attempted intake.
    if (settings_.capture == CaptureMode::Full) {
        plan.observe(network->packet_intake, fc::before([](void *, void *) noexcept {
                         ++thread_state().intake_depth;
                     }),
                     fc::after([this](void *, void *endpoint) noexcept {
                         --thread_state().intake_depth;
                         observe_intake(endpoint);
                     }));
    } else {
        plan.observe(network->packet_intake, fc::after([this](void *, void *endpoint) noexcept {
                         observe_intake(endpoint);
                     }));
    }

    plan.observe(network->disconnect, fc::before([this](std::int32_t destination) noexcept {
                     observe_disconnect(destination, false);
                 }),
                 fc::after([this](std::int32_t destination) noexcept {
                     observe_disconnect(destination, true);
                 }));

    plan.observe(network->create, fc::before([this](bool preserve) noexcept {
                     observe_network_create(preserve);
                 }));

    if (settings_.capture == CaptureMode::Full) {
        plan.observe<TimedInvocation>(
            network->receive_group, fc::before([this](void *, std::int32_t, TimedInvocation &invocation) noexcept {
                invocation.start = stamp();
            }),
            fc::after([this](void *group, std::int32_t packet_type, bool complete, const TimedInvocation &invocation) noexcept {
                finish_receive_group(group, packet_type, complete, invocation.start);
            }));
    }

    get_packet_ = plan.function(network->get_packet);

    // Select observers by startup role. A player-hosted game uses the client set even while running native authority passes.
    if (role_ == fc::Role::Client) {
        const auto layout = select_client_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        plan_client(plan, *layout);

        host_turn_memory_ = plan.access(layout->host_turn);
        client_turn_memory_ = plan.access(layout->client_turn);
        update_turn_memory_ = plan.access(layout->update_turn);
        predict_turn_memory_ = plan.access(layout->predict_turn);
        local_predict_turn_memory_ = plan.access(layout->local_predict_turn);
        move_ring_memory_ = plan.access(layout->move_ring);

        accumulator_memory_ = plan.access(layout->accumulator);
        adjust_time_memory_ = plan.access(layout->adjust_time);
        stable_count_memory_ = plan.access(layout->stable_count);
        time_scale_memory_ = plan.access(layout->time_scale);
        client_lag_memory_ = plan.access(layout->client_lag);

        reliable_frontier_memory_ = plan.access(layout->reliable_frontier);

        if (settings_.capture == CaptureMode::Full) {
            for (std::size_t index = 0; index < clock_values_memory_.size(); ++index) {
                clock_values_memory_[index] = plan.access(layout->clock.values[index]);
            }
            acknowledged_turn_memory_ = plan.access(layout->clock.acknowledged_turn);
        }
        if (settings_.capture != CaptureMode::Standard) {
            plan.observe<SoldierReadState>(
                layout->soldier_read, fc::before([this](void *soldier, void *stream, void *object_state, SoldierReadState &state) noexcept {
                    before_soldier_read(soldier, stream, object_state, state);
                }),
                fc::after([this](void *soldier, void *stream, void *object_state, const SoldierReadState &state) noexcept {
                    after_soldier_read(soldier, stream, object_state, state);
                }));
        }

        return;
    }

    const auto layout = select_server_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    plan_server(plan, *layout);

    if (settings_.capture == CaptureMode::Full) {
        lagversion_memory_ = plan.access(layout->lag.lagversion);
        drop_lag_jumps_memory_ = plan.access(layout->lag.drop_lag_jumps);
    }

    host_turn_memory_ = plan.access(layout->host_turn);
    client_turn_memory_ = plan.access(layout->client_turn);
    move_ring_memory_ = plan.access(layout->move_ring);

    accumulator_memory_ = plan.access(layout->accumulator);
    simulation_delta_memory_ = plan.access(layout->simulation_delta);
    fixed_delta_memory_ = plan.access(layout->fixed_delta);
    time_scale_memory_ = plan.access(layout->time_scale);

    required_mask_memory_ = plan.access(layout->required_mask);
    received_masks_memory_ = plan.access(layout->received_masks);
    current_destination_memory_ = plan.access(layout->current_destination);

    event_head_memory_ = plan.access(layout->event_head);
    network_enabled_memory_ = plan.access(layout->network_enabled);
    waitlate_memory_ = plan.access(layout->waitlate);
    lead_turns_memory_ = plan.access(layout->lead_turns);
    soldier_controllable_vtable_memory_ =
        plan.access<const std::uint32_t>(plan.at(layout->soldier_controllable_vtable.rva, layout->soldier_controllable_vtable.name), 1);

    for (std::size_t player = 0; player < native::kPlayerCapacity; ++player) {
        event_cursors_memory_[player] = plan.access(layout->event_cursors[player]);
        filled_turn_counts_memory_[player] = plan.access(layout->filled_turn_counts[player]);
    }
}

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    // Resolve the prepared memory reservations to read-only pointers into the mapped native image.
    const auto bind_view = []<class T>(const fc::Memory<const T> &memory, const T *&pointer) -> fc::Result<> {
        auto view = memory.view();
        if (!view) {
            return std::unexpected(view.error());
        }

        pointer = view->data();
        return {};
    };

    if (auto result = bind_view(pipeline_time_memory_, pipeline_time_); !result) {
        return result;
    }
    if (settings_.capture == CaptureMode::Full) {
        if (auto result = bind_view(scalar_raw_memory_, scalar_raw_); !result) {
            return result;
        }
        if (auto result = bind_view(direction_mode_memory_, direction_mode_); !result) {
            return result;
        }
        if (auto result = bind_view(velocity_factor_memory_, velocity_factor_); !result) {
            return result;
        }
    }
    if (settings_.capture != CaptureMode::Standard) {
        if (role_ == fc::Role::Server) {
            if (auto result = bind_view(ordinary_ring_memory_, ordinary_ring_); !result) {
                return result;
            }
            if (auto result = bind_view(reliable_ring_memory_, reliable_ring_); !result) {
                return result;
            }
            if (auto result = bind_view(reliable_head_memory_, reliable_head_); !result) {
                return result;
            }
            if (auto result = bind_view(reliable_free_memory_, reliable_free_); !result) {
                return result;
            }
            if (settings_.capture == CaptureMode::Full) {
                if (auto result = bind_view(object_list_memory_, object_list_); !result) {
                    return result;
                }
                if (auto result = bind_view(object_count_memory_, object_count_); !result) {
                    return result;
                }
            }
            if (auto result = bind_view(late_masks_memory_, late_masks_); !result) {
                return result;
            }
            if (auto result = bind_view(active_inputs_memory_, active_inputs_); !result) {
                return result;
            }
            if (auto result = bind_view(player_states_memory_, player_states_); !result) {
                return result;
            }
            if (auto result = bind_view(previous_required_memory_, previous_required_); !result) {
                return result;
            }
            if (auto result = bind_view(readiness_roles_memory_, readiness_roles_); !result) {
                return result;
            }
            if (auto result = bind_view(replay_turn_memory_, replay_turn_); !result) {
                return result;
            }
            if (settings_.capture == CaptureMode::Full) {
                if (auto result = bind_view(pipe_bits_memory_, pipe_bits_); !result) {
                    return result;
                }
                if (auto result = bind_view(bandwidth_memory_, bandwidth_); !result) {
                    return result;
                }
                if (auto result = bind_view(allowance_memory_, allowance_); !result) {
                    return result;
                }
            }
        }
    }

    if (auto result = bind_view(maximum_update_steps_memory_, maximum_update_steps_); !result) {
        return result;
    }
    if (auto result = bind_view(simulation_prediction_turn_memory_, simulation_prediction_turn_); !result) {
        return result;
    }
    for (std::size_t index = 0; index < world_pass_memory_.size(); ++index) {
        if (auto result = bind_view(world_pass_memory_[index], world_pass_[index]); !result) {
            return result;
        }
    }

    if (auto result = bind_view(host_turn_memory_, host_turn_); !result) {
        return result;
    }

    if (auto result = bind_view(client_turn_memory_, client_turn_); !result) {
        return result;
    }

    if (auto result = bind_view(move_ring_memory_, move_ring_); !result) {
        return result;
    }

    if (auto result = bind_view(accumulator_memory_, accumulator_); !result) {
        return result;
    }

    if (auto result = bind_view(time_scale_memory_, time_scale_); !result) {
        return result;
    }

    if (role_ == fc::Role::Client) {
        if (auto result = bind_view(update_turn_memory_, update_turn_); !result) {
            return result;
        }

        if (auto result = bind_view(predict_turn_memory_, predict_turn_); !result) {
            return result;
        }

        if (auto result = bind_view(local_predict_turn_memory_, local_predict_turn_); !result) {
            return result;
        }

        if (auto result = bind_view(adjust_time_memory_, adjust_time_); !result) {
            return result;
        }

        if (auto result = bind_view(stable_count_memory_, stable_count_); !result) {
            return result;
        }

        if (auto result = bind_view(client_lag_memory_, client_lag_); !result) {
            return result;
        }

        if (auto result = bind_view(reliable_frontier_memory_, reliable_frontier_); !result) {
            return result;
        }
        if (settings_.capture == CaptureMode::Full) {
            for (std::size_t index = 0; index < clock_values_memory_.size(); ++index) {
                if (auto result = bind_view(clock_values_memory_[index], clock_values_[index]); !result) {
                    return result;
                }
            }
            if (auto result = bind_view(acknowledged_turn_memory_, acknowledged_turn_); !result) {
                return result;
            }
        }
    } else {
        if (settings_.capture == CaptureMode::Full) {
            if (auto result = bind_view(lagversion_memory_, lagversion_); !result) {
                return result;
            }
            if (auto result = bind_view(drop_lag_jumps_memory_, drop_lag_jumps_); !result) {
                return result;
            }
        }
        if (auto result = bind_view(simulation_delta_memory_, simulation_delta_); !result) {
            return result;
        }

        if (auto result = bind_view(fixed_delta_memory_, fixed_delta_); !result) {
            return result;
        }

        if (auto result = bind_view(required_mask_memory_, required_mask_); !result) {
            return result;
        }

        if (auto result = bind_view(received_masks_memory_, received_masks_); !result) {
            return result;
        }

        if (auto result = bind_view(current_destination_memory_, current_destination_); !result) {
            return result;
        }

        if (auto result = bind_view(event_head_memory_, event_head_); !result) {
            return result;
        }

        if (auto result = bind_view(network_enabled_memory_, network_enabled_); !result) {
            return result;
        }

        if (auto result = bind_view(waitlate_memory_, waitlate_); !result) {
            return result;
        }

        if (auto result = bind_view(lead_turns_memory_, lead_turns_); !result) {
            return result;
        }

        auto vtable = soldier_controllable_vtable_memory_.view();
        if (!vtable) {
            return std::unexpected(vtable.error());
        }

        soldier_controllable_vtable_ = vtable->data();

        for (std::size_t player = 0; player < native::kPlayerCapacity; ++player) {
            if (auto result = bind_view(event_cursors_memory_[player], event_cursors_[player]); !result) {
                return result;
            }

            if (auto result = bind_view(filled_turn_counts_memory_[player], filled_turn_counts_[player]); !result) {
                return result;
            }
        }
    }

    // Prepare the runtime channel before publishing callbacks that can submit records to it.
    auto prepared_trace = context.trace({
        .name = "NetworkDiagnostics",
        .format_version = trace::kSchemaVersion,
        .buffer_bytes = trace::kBufferBytes,
    });
    if (!prepared_trace) {
        return std::unexpected(prepared_trace.error());
    }

    trace_ = std::move(*prepared_trace);

    return context.publish(api_contract(), DiagnosticsApi{
                                               .capture_mode = fc::bind<&Handler::capture_mode>(*this),
                                               .transport_send = fc::bind<&Handler::observe_transport_send>(*this),
                                               .direct_intake = fc::bind<&Handler::observe_direct_intake>(*this),
                                               .direct_association = fc::bind<&Handler::observe_direct_association>(*this),
                                               .direct_receive = fc::bind<&Handler::observe_direct_receive>(*this),
                                               .direct_pump = fc::bind<&Handler::observe_direct_pump>(*this),
                                               .direct_datagram = fc::bind<&Handler::observe_direct_datagram>(*this),
                                               .direct_socket = fc::bind<&Handler::observe_direct_socket>(*this),
                                               .destination_gate = fc::bind<&Handler::observe_destination_gate>(*this),
                                               .direct_send = fc::bind<&Handler::observe_direct_send>(*this),
                                               .recovery_drain = fc::bind<&Handler::observe_recovery_drain>(*this),
                                               .send_storage = fc::bind<&Handler::observe_send_storage>(*this),
                                               .create_transition = fc::bind<&Handler::observe_create_transition>(*this),
                                               .codec_position = fc::bind<&Handler::codec_position>(*this),
                                               .waitlate_grace = fc::bind<&Handler::observe_waitlate_grace>(*this),
                                           });
}

void Handler::write_status(fc::StatusContext &context) const noexcept {
    const auto health = trace_.health();
    context.line("Records: {}; dropped: {}", health.written, health.dropped);
}

Handler::ThreadState &Handler::thread_state() noexcept {
    // Module TLS keeps nested invocation pointers on their calling thread; it lasts until that thread exits.
    static thread_local ThreadState state{};
    return state;
}

std::uint64_t Handler::stamp() const noexcept {
    // Fence the RDTSCP read to order elapsed tick measurements, which include callbacks and nested trace overhead.
    unsigned int processor{};
    _mm_lfence();
    const auto result = __rdtscp(&processor);
    _mm_lfence();

    return result;
}

trace::RecordHeader Handler::record_header(trace::RecordKind kind, std::uint32_t subject, std::uint16_t flags,
                                           trace::Carrier carrier) const noexcept {
    return {
        .subject = subject,
        .kind = kind,
        .flags = flags,
        .carrier = carrier,
    };
}

void Handler::begin_group(std::int32_t destination, GroupInvocation &invocation) noexcept {
    invocation.record.entered_qpc = qpc_stamp();
    invocation.record.destination = destination;
    invocation.start = stamp();

    auto &state = thread_state();
    invocation.previous = state.group;
    state.group = &invocation;
}

void Handler::finish_group(std::int32_t destination, const GroupInvocation &invocation) noexcept {
    const auto returned_qpc = qpc_stamp();
    if (auto &state = thread_state(); state.group == &invocation) {
        state.group = invocation.previous;
    }

    auto record = invocation.record;
    record.duration = stamp() - invocation.start;
    record.returned_qpc = returned_qpc;
    const auto flags = static_cast<std::uint16_t>(trace::End | (record.failed_packets != 0 ? trace::Anomaly : trace::None));

    write(trace::RecordKind::NativeSendGroup, record, static_cast<std::uint32_t>(destination), flags);
}

void Handler::begin_send(const void *payload, std::int32_t length, SendInvocation &invocation) noexcept {
    const auto *bytes = static_cast<const std::uint8_t *>(payload);
    if (bytes != nullptr && length >= 5 &&
        (bytes[0] < 0x1E || bytes[0] == soldier_state_sync::kStateUpdatePacket || bytes[0] == soldier_state_sync::kPrecisionUpdatePacket ||
         bytes[0] == soldier_state_sync::kWeaponStateUpdatePacket)) {
        invocation.identity = {
            .sequence = static_cast<std::uint16_t>(bytes[1] | (static_cast<std::uint16_t>(bytes[2]) << 8)),
            .packet_type = bytes[0],
            .fragment_index = bytes[3],
            .fragment_count = static_cast<std::uint8_t>(bytes[4] & 0x7F),
            .valid = 1,
            .source_slot = role_ == fc::Role::Server ? 64 : -1,
        };
    }
    auto &state = thread_state();
    invocation.previous = state.send;
    state.send = &invocation;
}

void Handler::finish_send(std::int32_t destination, std::uint32_t size, std::int32_t result, const SendInvocation &invocation) noexcept {
    // A producer report already counted this attempt with its selected carrier, including NotSent.
    if (!invocation.transport_reported) {
        observe_send(destination, size, PacketCarrier::Native, result);
    }
    if (auto &state = thread_state(); state.send == &invocation) {
        state.send = invocation.previous;
    }
}

void Handler::observe_transport_send(std::int32_t destination, std::uint32_t size, PacketCarrier carrier, std::int32_t result) noexcept {
    if (auto &state = thread_state(); state.send != nullptr) {
        state.send->transport_reported = true;
    }

    observe_send(destination, size, carrier, result);
}

void Handler::observe_send(std::int32_t destination, std::uint32_t size, PacketCarrier carrier, std::int32_t result) noexcept {
    auto &state = thread_state();
    if (state.group != nullptr) {
        // Count observed attempts and requested payload bytes; carrier envelopes and network headers are excluded.
        auto &group = state.group->record;
        ++group.packets;
        group.bytes += size;
        group.direct_packets += carrier == PacketCarrier::Direct ? 1U : 0U;
        group.native_packets += carrier == PacketCarrier::Native ? 1U : 0U;
        // Galaxy can return zero without sending, so only negative results contribute to failed_packets.
        group.failed_packets += result < 0 ? 1U : 0U;
        if (group.identity.valid == 0 && state.send != nullptr) {
            group.identity = state.send->identity;
        }
        if ((settings_.capture != CaptureMode::Full || carrier == PacketCarrier::Direct) && result >= 0) {
            return;
        }
    }

    // Full retains fragment outcomes; failures and sends outside a group remain visible in every tier.
    const trace::NativePacketRecord record{
        .destination = destination,
        .bytes = size,
        .result = result,
        .group = state.group != nullptr ? state.group->record.group : 0,
        .identity = state.send != nullptr ? state.send->identity : trace::GroupIdentity{},
    };
    write(trace::RecordKind::NativePacket, record, static_cast<std::uint32_t>(destination), result < 0 ? trace::Anomaly : trace::None,
          trace_carrier(carrier));
}

void Handler::begin_receive(ReceiveInvocation &invocation) noexcept {
    invocation.start = stamp();

    auto &state = thread_state();
    invocation.record.service_gap = state.last_receive_service == 0 ? 0 : invocation.start - state.last_receive_service;
    state.last_receive_service = invocation.start;
    state.receive_maximum_gap = (std::max)(state.receive_maximum_gap, invocation.record.service_gap);
    invocation.previous = state.receive;
    state.receive = &invocation;
}

void Handler::finish_receive(const ReceiveInvocation &invocation) noexcept {
    auto &state = thread_state();
    if (state.receive == &invocation) {
        state.receive = invocation.previous;
    }

    auto record = invocation.record;
    if (record.total_packets == 0 && record.direct_data_packets == 0) {
        if (++state.empty_receive_calls < 128) {
            return;
        }
    }
    record.empty_calls = state.empty_receive_calls;
    record.maximum_service_gap = state.receive_maximum_gap;
    record.duration = stamp() - invocation.start;
    state.empty_receive_calls = 0;
    state.receive_maximum_gap = 0;
    write(trace::RecordKind::TransportReceivePass, record, 0, trace::End);
}

void Handler::observe_intake(void *endpoint) noexcept {
    auto &state = thread_state();
    if (state.receive != nullptr) {
        ++state.receive->record.total_packets;
        return;
    }

    const trace::NativePacketRecord record{.endpoint = native::pointer_id(endpoint), .destination = -1};
    write(trace::RecordKind::NativePacket, record, record.endpoint, trace::None, trace::Carrier::Unknown);
}

void Handler::observe_direct_intake(void *) noexcept {
    // The native observer already counted this attempted handoff. Direct is its subset, including silent native rejection.
    if (auto *receive = thread_state().receive; receive != nullptr) {
        ++receive->record.direct_packets;
    }
}

void Handler::observe_disconnect(std::int32_t destination, std::uint32_t after) noexcept {
    const trace::ConnectionRecord record{
        .destination = destination,
        .operation = after ? trace::ConnectionOperation::DisconnectEnd : trace::ConnectionOperation::DisconnectBegin,
    };

    write(trace::RecordKind::Connection, record, static_cast<std::uint32_t>(destination), after ? trace::End : trace::Begin);
    if (after && role_ == fc::Role::Client && destination == 0x40) {
        // Slot 64 is the host route; retire its pending clock observations before another connection can publish a phase.
        auto &state = thread_state();
        flush_client_progress();
        close_receive_window();

        state.reset_pending = false;
        state.presentation_pending = false;
    }
    if (after && destination >= 0 && destination < static_cast<std::int32_t>(native::kPlayerCapacity)) {
        auto &state = thread_state();
        finish_scheduling_summary(static_cast<std::uint32_t>(destination));
        if (state.lag_summaries[destination].writes != 0) {
            write(trace::RecordKind::HostLag, state.lag_summaries[destination], static_cast<std::uint32_t>(destination));
        }

        state.lag_summaries[destination] = {};
        state.lag_seen[destination] = false;
    }
}

void Handler::observe_network_create(bool preserve) noexcept {
    // Mark the requested native reset/preserve operation; sampler sequences and frame gaps continue across this marker.
    const trace::ConnectionRecord record{
        .destination = -1,
        .operation = preserve ? trace::ConnectionOperation::CreatePreserve : trace::ConnectionOperation::CreateReset,
    };

    write(trace::RecordKind::Connection, record);
    auto &state = thread_state();
    flush_client_progress();
    close_receive_window();
    finish_scheduling_candidate();
    for (std::uint32_t destination = 0; destination != native::kPlayerCapacity; ++destination) {
        finish_scheduling_summary(destination);
    }
    // Every Create rebases native network time, even when preserving routes. Delimit observations from the continuous frame timer.

    state.reset_pending = false;
    state.presentation_pending = false;
    state.active_source_known = {};
    state.active_source_turns = {};
    state.source_known = {};
    state.source_generation = {};
    state.source_types = {};
    if (!preserve) {
        for (const auto &summary : state.lag_summaries) {
            if (summary.writes != 0) {
                write(trace::RecordKind::HostLag, summary, static_cast<std::uint32_t>(summary.destination));
            }
        }

        state.lag_summaries = {};
        state.lag_seen = {};
    }
}

void Handler::observe_direct_association(const DirectAssociationObservation *observation) noexcept {
    if (observation == nullptr) {
        return;
    }

    // Copy the producer's borrowed counters and identities synchronously; their scope is this Direct association.
    const trace::DirectAssociationRecord record{
        .slot = observation->slot,
        .peer_id = observation->peer_id,
        .connection_id = observation->connection_id,
        .phase = direct_phase(observation->phase),
        .route = direct_route(observation->route),
        .route_reason = static_cast<trace::DirectRouteReason>(observation->route_reason),
        .end_reason = static_cast<trace::AssociationEndReason>(observation->end_reason),
        .attempts = observation->attempts,
        .tx_datagrams = observation->tx_datagrams,
        .rx_datagrams = observation->rx_datagrams,
        .send_failures = observation->send_failures,
        .endpoint_rejects = observation->endpoint_rejects,
        .authentication_rejects = observation->authentication_rejects,
        .replay_rejects = observation->replay_rejects,
        .invalid_rejects = observation->invalid_rejects,
        .elapsed_ms = observation->elapsed_ms,
        .direct_ms = observation->direct_ms,
    };

    auto flags = trace::None;
    if (observation->phase == DirectAssociationPhase::Started) {
        flags = trace::Begin;
    } else if (observation->phase == DirectAssociationPhase::Ended) {
        flags = trace::End;
    }

    write(trace::RecordKind::DirectAssociation, record, static_cast<std::uint32_t>(observation->slot), flags,
          association_carrier(record.route));
}

void Handler::observe_direct_receive(const DirectReceiveObservation *observation) noexcept {
    if (observation == nullptr) {
        return;
    }

    auto &state = thread_state();
    // Count only attempted injections in the pass summary; failed rebuilds retain their own detail.
    if (state.receive != nullptr && observation->rebuild_result == 0 && observation->native_packet != 0) {
        auto &record = state.receive->record;
        if (record.direct_data_packets++ == 0) {
            record.direct_slot = observation->slot;
            record.direct_peer_id = observation->peer_id;
            record.direct_connection_id = observation->connection_id;
            record.first_direct_sequence = observation->sequence;
        } else if (record.direct_slot != observation->slot || record.direct_connection_id != observation->connection_id ||
                   record.direct_peer_id != observation->peer_id) {
            record.direct_slot = -2;
            record.direct_peer_id = 0;
            record.direct_connection_id = 0;
        }
        record.last_direct_sequence = observation->sequence;
        if (observation->slot >= 0 && observation->slot < static_cast<std::int32_t>(native::kPlayerCapacity)) {
            record.direct_player_mask |= std::uint64_t{1} << static_cast<std::uint32_t>(observation->slot);
        }
    }

    if (settings_.capture != CaptureMode::Full && state.receive != nullptr && observation->rebuild_result == 0) {
        return;
    }
    trace::DirectReceiveRecord record{
        .slot = observation->slot,
        .peer_id = observation->peer_id,
        .connection_id = observation->connection_id,
        .sequence = observation->sequence,
        .received_qpc = observation->received_qpc,
        .injected_qpc = observation->injected_qpc,
        .native_packet = observation->native_packet,
        .rebuild_result = observation->rebuild_result,
    };
    // BeginRead decodes these existing bytes into packet +C/+E/+10/+11 in both retail builds.
    if (observation->payload != nullptr && observation->payload_bytes >= 5 &&
        (observation->payload[0] < 0x1E || observation->payload[0] == soldier_state_sync::kStateUpdatePacket ||
         observation->payload[0] == soldier_state_sync::kPrecisionUpdatePacket ||
         observation->payload[0] == soldier_state_sync::kWeaponStateUpdatePacket)) {
        const auto *bytes = observation->payload;
        record.identity = {
            .sequence = static_cast<std::uint16_t>(bytes[1] | (static_cast<std::uint16_t>(bytes[2]) << 8)),
            .packet_type = bytes[0],
            .fragment_index = bytes[3],
            .fragment_count = static_cast<std::uint8_t>(bytes[4] & 0x7F),
            .valid = 1,
            .source_slot = observation->slot,
        };
    }
    write(trace::RecordKind::DirectReceive, record, static_cast<std::uint32_t>(observation->slot),
          settings_.capture == CaptureMode::Full ? trace::None : trace::Partial, trace::Carrier::Direct);
}

void Handler::observe_direct_pump(const DirectPumpObservation *observation) noexcept {
    if (observation == nullptr) {
        return;
    }
    auto &state = thread_state();
    auto &record = state.direct_pump;
    const auto empty = observation->datagrams == 0 && observation->oversized == 0 && observation->error == 0 &&
                       observation->budget_reached == 0 && observation->terminal_reason == 0;
    // Flush pending empty calls before recording a nonempty pass, so each datagram retains its own pump bounds.
    if (record.calls != 0 && (!empty || record.socket_id != observation->socket_id)) {
        write(trace::RecordKind::DirectPump, record, record.last_pump_id,
              settings_.capture == CaptureMode::Full ? trace::End : trace::End | trace::Partial, trace::Carrier::Direct);
        record = {};
    }
    if (record.calls++ == 0) {
        record.socket_id = observation->socket_id;
        record.first_pump_id = observation->pump_id;
        record.started_qpc = observation->started_qpc;
    }
    record.last_pump_id = observation->pump_id;
    record.finished_qpc = observation->finished_qpc;
    if (observation->service_gap > record.maximum_service_gap) {
        record.maximum_service_gap = observation->service_gap;
        record.maximum_service_gap_qpc = observation->started_qpc;
    }
    if (observation->first_empty_qpc != 0 && (record.first_empty_qpc == 0 || observation->first_empty_qpc < record.first_empty_qpc)) {
        record.first_empty_qpc = observation->first_empty_qpc;
    }
    record.last_empty_qpc = (std::max)(record.last_empty_qpc, observation->last_empty_qpc);
    record.maximum_receive_duration = (std::max)(record.maximum_receive_duration, observation->maximum_receive_duration);
    record.attempts += observation->attempts;
    record.datagrams += observation->datagrams;
    record.oversized += observation->oversized;
    record.accepted += observation->accepted;
    record.rejected += observation->rejected;
    record.budget_reached += observation->budget_reached;
    record.error = observation->error;
    record.terminal_reason = observation->terminal_reason;
    record.empty_calls += empty ? 1U : 0U;
    // Emitting after 128 empty calls bounds trace volume; it imposes no socket or receive-work limit.
    if (empty && record.calls < 128) {
        return;
    }
    write(trace::RecordKind::DirectPump, record, record.last_pump_id,
          settings_.capture == CaptureMode::Full ? trace::End : trace::End | trace::Partial, trace::Carrier::Direct);
    record = {};
}

void Handler::observe_direct_datagram(const DirectDatagramObservation *observation) noexcept {
    if (observation == nullptr || (settings_.capture != CaptureMode::Full && (observation->disposition == DirectDisposition::Arrived ||
                                                                              observation->disposition == DirectDisposition::Control ||
                                                                              observation->disposition == DirectDisposition::Data))) {
        return;
    }

    const trace::DirectDatagramRecord record{
        .socket_id = observation->socket_id,
        .pump_id = observation->pump_id,
        .attempt = observation->attempt,
        .entered_qpc = observation->entered_qpc,
        .returned_qpc = observation->returned_qpc,
        .peer_id = observation->peer_id,
        .slot = observation->slot,
        .connection_id = observation->connection_id,
        .sequence = observation->sequence,
        .source_ipv4_network_order = observation->source_ipv4_network_order,
        .source_port = observation->source_port,
        .bound_port = observation->bound_port,
        .bytes = observation->bytes,
        .disposition = static_cast<std::uint8_t>(observation->disposition),
        .kind = observation->kind,
        .header_available = observation->header_available,
        .detail = observation->detail,
    };
    const auto arrived = observation->disposition == DirectDisposition::Arrived;
    const auto accepted = observation->disposition == DirectDisposition::Data || observation->disposition == DirectDisposition::Control;
    const auto flags = static_cast<std::uint16_t>((arrived ? trace::Begin : trace::End | (accepted ? trace::Accepted : trace::Rejected)) |
                                                  (settings_.capture == CaptureMode::Full ? trace::None : trace::Partial));
    write(trace::RecordKind::DirectDatagram, record, record.pump_id, flags, trace::Carrier::Direct);
}

void Handler::observe_direct_socket(const DirectSocketObservation *observation) noexcept {
    if (observation == nullptr) {
        return;
    }

    const trace::DirectSocketRecord record{
        .socket_id = observation->socket_id,
        .requested_send_bytes = observation->requested_send_bytes,
        .requested_receive_bytes = observation->requested_receive_bytes,
        .actual_send_bytes = observation->actual_send_bytes,
        .actual_receive_bytes = observation->actual_receive_bytes,
        .send_set_error = observation->send_set_error,
        .receive_set_error = observation->receive_set_error,
        .send_read_error = observation->send_read_error,
        .receive_read_error = observation->receive_read_error,
        .bound_port = observation->bound_port,
    };
    write(trace::RecordKind::DirectSocket, record, record.bound_port, trace::None, trace::Carrier::Direct);
}

void Handler::finish_receive_group(void *group, std::int32_t packet_type, bool complete, std::uint64_t start) noexcept {
    auto &state = thread_state();
    // The native update query stays 0x0C; its FIFO search also returns negotiated Soldier state packets.
    if (complete && role_ == fc::Role::Client && packet_type == 0x0C) {
        advance_receive_window();
        ++state.receive_window.retrieved_groups;
    }
    if (settings_.capture != CaptureMode::Full) {
        return;
    }
    if (packet_type < 0 || packet_type >= static_cast<std::int32_t>(state.empty_group_polls.size())) {
        return;
    }
    auto &empty = state.empty_group_polls[static_cast<std::size_t>(packet_type)];
    if (!complete && ++empty < 128) {
        return;
    }
    trace::NativeReceiveGroupRecord record{
        .group = complete ? native::pointer_id(group) : 0,
        .packet_type = packet_type,
        .complete = static_cast<std::uint32_t>(complete),
        .empty_polls = empty,
        .duration = stamp() - start,
    };
    empty = 0;
    if (complete) {
        trace::UpdateReadRecord identity{};
        identify_group(group, identity);
        record.first_packet = identity.first_packet;
        record.packets = identity.packets;
        record.identity = identity.identity;
    }

    write(trace::RecordKind::NativeReceiveGroup, record, record.group, complete ? trace::Complete : trace::Empty);
}

void Handler::identify_group(void *group, trace::UpdateReadRecord &record) noexcept {
    // Init/Append retain a first pool index at +4 and packet count at +C. Get is a read-only bounded lookup.
    record.packets = native::read<std::uint32_t>(group, 0x0C);
    if (record.packets == 0) {
        return;
    }
    const auto *packet = get_packet_.call(native::read<std::int32_t>(group, 0x04)).value_or(nullptr);
    record.first_packet = native::pointer_id(packet);
    if (packet != nullptr) {
        // GetGroup matches sequence, type and source slot, then orders the existing fragment indices.
        record.identity = {
            .sequence = native::read<std::uint16_t>(packet, 0x0C),
            .packet_type = native::read<std::uint8_t>(packet, 0x0E),
            .fragment_index = native::read<std::uint8_t>(packet, 0x10),
            .fragment_count = native::read<std::uint8_t>(packet, 0x11),
            .valid = 1,
            .source_slot = native::read<std::uint8_t>(packet, 0x0F),
        };
    }
}

void Handler::observe_destination_gate(const DestinationGateObservation *observation) noexcept {
    if (observation == nullptr || observation->destination < 0 ||
        observation->destination >= static_cast<std::int32_t>(native::kPlayerCapacity)) {
        return;
    }
    auto &state = thread_state();
    const auto destination = static_cast<std::uint32_t>(observation->destination);
    const auto blocked = observation->reason != DestinationGateReason::Open;
    if (state.server.frame_active) {
        state.server.scheduling.blocked_destinations += blocked ? 1U : 0U;
        state.server.scheduling.timed_out_fences += observation->timed_out ? 1U : 0U;
        if (blocked || observation->timed_out) {
            state.server.scheduling.destination_mask |= std::uint64_t{1} << destination;
        }
    }

    const auto now = stamp();
    trace::DestinationGateRecord next{
        .destination = observation->destination,
        .peer_id = observation->peer_id,
        .reason = static_cast<std::uint32_t>(observation->reason),
        .outstanding = observation->outstanding,
        .capacity = observation->capacity,
        .fence_map = observation->fence_map,
        .fence_turn = observation->fence_turn,
        .network_time = observation->network_time,
        .fence_sent_time = observation->fence_sent_time,
        .average_rtt = observation->average_rtt,
        .deviation_rtt = observation->deviation_rtt,
        .native_window_open = observation->native_window_open,
        .timed_out = observation->timed_out,
        .checks = 1,
        .first_timestamp = now,
        .last_timestamp = now,
    };
    auto &previous = state.gates[destination];
    auto &seen = state.gate_seen[destination];
    // Compare decision-defining values, excluding advancing time and estimates that leave capacity unchanged.
    const auto same = seen && previous.peer_id == next.peer_id && previous.reason == next.reason &&
                      previous.outstanding == next.outstanding && previous.capacity == next.capacity &&
                      previous.fence_map == next.fence_map && previous.fence_turn == next.fence_turn &&
                      previous.fence_sent_time == next.fence_sent_time && previous.native_window_open == next.native_window_open &&
                      previous.timed_out == next.timed_out;
    if (same) {
        if (!blocked && !observation->timed_out) {
            return;
        }
        ++previous.checks;
        if (previous.checks == 1) {
            previous.first_timestamp = now;
        }
        previous.network_time = next.network_time;
        previous.last_timestamp = now;
        if (previous.checks == 128) {
            write(trace::RecordKind::DestinationGate, previous, destination, trace::End);
            previous.checks = 0;
            previous.first_timestamp = now;
        }
        return;
    }
    if (seen && previous.checks != 0 &&
        (previous.reason != static_cast<std::uint32_t>(DestinationGateReason::Open) || previous.timed_out != 0)) {
        write(trace::RecordKind::DestinationGate, previous, destination, trace::End);
    }
    previous = next;
    seen = true;
    write(trace::RecordKind::DestinationGate, next, destination, trace::Begin);
    previous.checks = 0;
}

void Handler::observe_waitlate_grace(std::uint32_t turns) noexcept {
    // The native default is three turns; an optional policy patch reports its current replacement.
    waitlate_grace_ = turns;
}

} // namespace fc_plugins::network_diagnostics
