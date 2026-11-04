#include "handler.hpp"
#include "pipeline_layout.hpp"

#include "../direct_transport/receive_diagnostics.hpp"
#include "../soldier_state_sync/patch.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace fc_plugins::network_diagnostics {
namespace {

// Header fields are already materialized by BeginRead when native intake appends the packet.
trace::GroupIdentity packet_identity(const void *packet) noexcept {
    return {.sequence = native::read<std::uint16_t>(packet, 0x0C),
            .packet_type = native::read<std::uint8_t>(packet, 0x0E),
            .fragment_index = native::read<std::uint8_t>(packet, 0x10),
            .fragment_count = native::read<std::uint8_t>(packet, 0x11),
            .valid = static_cast<std::uint8_t>(packet != nullptr),
            .source_slot = native::read<std::uint8_t>(packet, 0x0F)};
}

// Hash explicit control fields without padding. Native equality separately compares axes and buttons, excluding joystick.
std::uint32_t move_hash(const native::PlayerMove &move) noexcept {
    auto hash = 2166136261U;
    const auto mix = [&hash](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash = (hash ^ ((value >> shift) & 0xFFU)) * 16777619U;
        }
    };
    for (const auto axis : move.axes) {
        mix(std::bit_cast<std::uint32_t>(axis));
    }
    mix(move.input_bits);
    mix(move.joystick);
    return hash;
}

} // namespace

void Handler::plan_pipeline(fc::Plan &plan) {
    if (settings_.capture == CaptureMode::Full) {
        plan_velocity(plan);
    }
    if (settings_.capture != CaptureMode::Standard) {
        plan_replication(plan);
    }
    const auto locations = select_pipeline_layout(plan.profile());
    pipeline_time_memory_ = plan.access(locations.network_time);

    if (settings_.capture == CaptureMode::Full) {
        // Restrict Append observations to intake: the same native queue class also constructs outgoing groups.
        const fc::FunctionLocation<AppendPacket> append = locations.queue_append;
        plan.observe<QueueInvocation>(append, fc::before([this](void *queue, void *packet, QueueInvocation &invocation) noexcept {
                                          auto &state = thread_state();
                                          if (state.intake_depth == 0) {
                                              return;
                                          }
                                          const auto identity = packet_identity(packet);
                                          const auto update = identity.packet_type == 0x0C ||
                                                              identity.packet_type == soldier_state_sync::kStateUpdatePacket ||
                                                              identity.packet_type == soldier_state_sync::kPrecisionUpdatePacket ||
                                                              identity.packet_type == soldier_state_sync::kWeaponStateUpdatePacket;
                                          if (identity.packet_type != 0x0B && !update) {
                                              return;
                                          }
                                          invocation.record = {.packet = native::pointer_id(packet),
                                                               .identity = identity,
                                                               .queue = native::pointer_id(queue),
                                                               .count_before = native::read<std::uint32_t>(queue, 0x0C),
                                                               .entered_qpc = direct_transport::receive_stamp()};
                                          if (role_ == fc::Role::Client && update) {
                                              advance_receive_window();
                                              ++state.receive_window.received_fragments;
                                          }
                                      }),
                                      fc::after([this](void *queue, void *, const QueueInvocation &invocation) noexcept {
                                          if (invocation.record.packet == 0) {
                                              return;
                                          }
                                          auto record = invocation.record;
                                          record.count_after = native::read<std::uint32_t>(queue, 0x0C);
                                          record.returned_qpc = direct_transport::receive_stamp();
                                          write(trace::RecordKind::QueuePacket, record, record.packet);
                                      }));
    }

    if (role_ == fc::Role::Client) {
        if (settings_.capture == CaptureMode::Standard) {
            return;
        }
        const auto sample = [this, &plan](fc::CodeLocation site, bool prepared) {
            plan.observe_instruction(site, fc::before([this, prepared](const fc::CpuContext &context) noexcept {
                                         const auto player = native::read<std::int32_t>(context.ebp, -8);
                                         const auto pointer = native::read<std::uint32_t>(context.ebp, -0x30 + player * 4);
                                         if (pointer == 0) {
                                             return;
                                         }
                                         advance_receive_window();
                                         const auto turn = static_cast<std::uint32_t>(*client_turn_);
                                         const auto move =
                                             prepared ? move_ring_[(turn & 31) * 2 + player] : native::read<native::PlayerMove>(pointer);
                                         const trace::PreparedInputRecord record{.candidate_turn = turn,
                                                                                 .player = player,
                                                                                 .active_input = pointer,
                                                                                 .source_turn = turn,
                                                                                 .source = trace::InputSource::Received,
                                                                                 .source_turn_known = 1,
                                                                                 .axes = move.axes,
                                                                                 .input_bits = move.input_bits,
                                                                                 .joystick = move.joystick};
                                         write(trace::RecordKind::InputSample, record, turn, prepared ? trace::End : trace::Begin);
                                     }));
        };
        sample(locations.input_raw, false);
        sample(locations.input_prepared, true);
        plan.observe_instruction(locations.input_frontier, fc::before([this](const fc::CpuContext &) noexcept {
                                     advance_receive_window();
                                 }));
        if (settings_.capture == CaptureMode::Full) {
            const auto observe_turn = [this, &plan](fc::CodeLocation site, bool mini) {
                plan.observe_instruction(site, fc::before([this, mini](const fc::CpuContext &context) noexcept {
                                             observe_encoded_turn(context, mini);
                                         }));
            };
            observe_turn(locations.encode_first_turn, false);
            observe_turn(locations.encode_repeat_turn, false);
            observe_turn(locations.encode_mini_turn, true);
        }
        return;
    }

    const fc::FunctionLocation<IsTurnReady> ready = locations.is_turn_ready;
    plan.observe<ReadinessInvocation>(
        ready, fc::before([this](ReadinessInvocation &invocation) noexcept {
            invocation.start = stamp();
            invocation.record = {.candidate_turn = static_cast<std::uint32_t>(*host_turn_),
                                 .client_turn = static_cast<std::uint32_t>(*client_turn_),
                                 .lead_turns = *lead_turns_,
                                 .grace_turns = waitlate_grace_.load(std::memory_order_relaxed),
                                 .required_mask = *required_mask_,
                                 .received_mask = received_mask(*host_turn_),
                                 .peer_ready = 2,
                                 .waitlate = *waitlate_,
                                 .native_time = *pipeline_time_,
                                 .network_enabled = *network_enabled_,
                                 .host_turn = static_cast<std::uint32_t>(*host_turn_)};
            thread_state().readiness = &invocation;
        }),
        fc::after([this](bool result, const ReadinessInvocation &invocation) noexcept {
            thread_state().readiness = nullptr;
            auto record = invocation.record;
            record.result = result;
            record.duration = stamp() - invocation.start;
            if (settings_.capture == CaptureMode::Standard) {
                const trace::ReadinessResultRecord outcome{
                    .candidate_turn = record.candidate_turn,
                    .client_turn = record.client_turn,
                    .lead_turns = record.lead_turns,
                    .grace_turns = record.grace_turns,
                    .required_mask = record.required_mask,
                    .received_mask = record.received_mask,
                    .result = record.result,
                    .waitlate = record.waitlate,
                    .network_enabled = record.network_enabled,
                    .native_time = record.native_time,
                };
                write(trace::RecordKind::ReadinessResult, outcome, record.candidate_turn, result ? trace::Accepted : trace::Rejected);
            } else {
                write(trace::RecordKind::Readiness, record, record.candidate_turn, result ? trace::Accepted : trace::Rejected);
            }
        }));
    plan.observe_instruction(locations.readiness_peer_result, fc::before([this](const fc::CpuContext &context) noexcept {
                                 if (auto *invocation = thread_state().readiness; invocation != nullptr) {
                                     invocation->record.peer_ready = context.eax & 0xFFU;
                                     invocation->record.candidate_turn = native::read<std::uint32_t>(context.ebp, -0x0C);
                                     invocation->record.received_mask = received_masks_[invocation->record.candidate_turn & 31];
                                 }
                             }));

    if (settings_.capture == CaptureMode::Standard) {
        return;
    }

    late_masks_memory_ = plan.access(locations.late_masks);
    active_inputs_memory_ = plan.access(locations.active_inputs);
    player_states_memory_ = plan.access(locations.player_states);
    previous_required_memory_ = plan.access(locations.previous_required);
    readiness_roles_memory_ = plan.access(locations.readiness_roles);
    replay_turn_memory_ = plan.access(locations.replay_turn);
    if (settings_.capture == CaptureMode::Full) {
        pipe_bits_memory_ = plan.access(locations.pipe_bits);
        bandwidth_memory_ = plan.access(locations.bandwidth);
        allowance_memory_ = plan.access(locations.allowance);
    }

    // The callbacks surround the real branch operands; comparisons and object lookups are never repeated.
    const auto row = [this, &plan](fc::CodeLocation site, bool mini) {
        plan.observe_instruction(site, fc::before([this, mini](const fc::CpuContext &context) noexcept {
                                     begin_input_row(context, mini);
                                 }));
    };
    row(locations.input_first, false);
    row(locations.input_trailing, false);
    row(locations.input_mini, true);

    const auto finish = [this, &plan](fc::CodeLocation site) {
        plan.observe_instruction(site, fc::before([this](const fc::CpuContext &) noexcept {
                                     finish_input_row();
                                 }));
    };
    finish(locations.input_first_finish);
    finish(locations.input_trailing_finish);
    finish(locations.input_mini_finish);
    finish(locations.input_terminator);

    const auto compared = [this, &plan](fc::CodeLocation site) {
        plan.observe_instruction(site, fc::before([](const fc::CpuContext &context) noexcept {
                                     auto &state = thread_state();
                                     if (state.input_row_pending) {
                                         state.input_row.disposition = (context.eax & 0xFFU) != 0 ? trace::InputDisposition::UnchangedLate
                                                                                                  : trace::InputDisposition::ChangedLate;
                                     }
                                 }));
    };
    compared(locations.input_first_compare);
    compared(locations.input_trailing_compare);
    const auto object_gate = [this, &plan](fc::CodeLocation site) {
        plan.observe_instruction(site, fc::before([](const fc::CpuContext &context) noexcept {
                                     auto &state = thread_state();
                                     if (state.input_row_pending && (context.eflags & (1U << 6)) != 0) {
                                         state.input_row.disposition = trace::InputDisposition::ObjectUnavailable;
                                     }
                                 }));
    };
    object_gate(locations.input_first_object_gate);
    object_gate(locations.input_trailing_object_gate);

    const auto header = [this, &plan](fc::CodeLocation site, std::uint32_t reason) {
        plan.observe_instruction(
            site, fc::before([this, reason](const fc::CpuContext &context) noexcept {
                const auto rejected = reason == 1   ? (context.eflags & 1U) != 0
                                      : reason == 2 ? (context.eflags & ((1U << 6) | 1U)) == 0
                                                    : true;
                if (!rejected) {
                    return;
                }
                trace::UpdateReadRecord group{};
                identify_group(reinterpret_cast<void *>(context.ebp - 0x68), group);
                auto &state = thread_state();
                state.input_group = group.identity;
                const auto player = *current_destination_;
                trace::InputHeaderRecord record{.identity = group.identity,
                                                .player = current_destination_ != nullptr ? *current_destination_ : player,
                                                .advertised_turn = native::read<std::uint32_t>(context.ebp, -0x18),
                                                .host_turn = static_cast<std::uint32_t>(*host_turn_),
                                                .frontier =
                                                    native::read<std::uint32_t>(player_states_ + player * native::kPlayerStateStride, 0x2C),
                                                .reason = reason};
                write(trace::RecordKind::InputHeader, record, record.advertised_turn, reason == 0 ? trace::Accepted : trace::Rejected);
            }));
    };
    header(locations.input_old_header_gate, 1);
    header(locations.input_ahead_header_gate, 2);
    // The following MOV reads a relocated absolute address; observe the preceding branch's passing target instead.
    header(locations.input_header_admitted, 0);

    plan.observe_instruction(locations.readiness_force_block, fc::before([this](const fc::CpuContext &context) noexcept {
                                 if (auto *invocation = thread_state().readiness; invocation != nullptr) {
                                     invocation->record.force_block = native::read<std::uint32_t>(readiness_roles_, context.edx);
                                 }
                             }));
    plan.observe_instruction(locations.replay_cutoff, fc::before([](const fc::CpuContext &context) noexcept {
                                 if (auto *invocation = thread_state().replay; invocation != nullptr) {
                                     invocation->record.cutoff_player = native::read<std::int32_t>(context.ebp, -4);
                                     invocation->record.cutoff_turn = context.ecx;
                                 }
                             }));
    plan.observe_instruction(locations.readiness_grace, fc::before([](const fc::CpuContext &context) noexcept {
                                 if (auto *invocation = thread_state().readiness; invocation != nullptr) {
                                     invocation->record.grace_turns = native::read<std::uint32_t>(context.ebp, -0x18);
                                 }
                             }));
    plan.observe_instruction(locations.readiness_retained, fc::before([](const fc::CpuContext &context) noexcept {
                                 auto &state = thread_state();
                                 const auto turn = native::read<std::uint32_t>(context.ebp, -0x0C);
                                 const auto player = native::read<std::uint32_t>(context.ebp, -4);
                                 state.source_turns[turn & 31][player] = state.source_turns[(turn - 1) & 31][player];
                                 state.source_known[turn & 31][player] = state.source_known[(turn - 1) & 31][player] &&
                                                                         state.source_generation[(turn - 1) & 31][player] == turn - 1;
                                 state.source_generation[turn & 31][player] = turn;
                                 state.source_types[turn & 31][player] = trace::InputSource::Retained;
                                 if (state.readiness != nullptr) {
                                     state.readiness->record.forced = 1;
                                 }
                             }));
    plan.observe_instruction(locations.active_controls_complete, fc::before([this](const fc::CpuContext &context) noexcept {
                                 const auto turn = static_cast<std::uint32_t>(*host_turn_);
                                 const auto player = native::read<std::int32_t>(context.ebp, -4);
                                 const auto pointer = active_inputs_[player];
                                 if (pointer == 0) {
                                     return;
                                 }
                                 const auto move = native::read<native::PlayerMove>(pointer);
                                 auto &state = thread_state();
                                 const auto copied = (*previous_required_ & (std::uint64_t{1} << player)) != 0;
                                 if (copied) {
                                     state.active_source_turns[player] = state.source_turns[turn & 31][player];
                                     state.active_source_known[player] =
                                         state.source_known[turn & 31][player] && state.source_generation[turn & 31][player] == turn;
                                 }
                                 const trace::PreparedInputRecord record{
                                     .candidate_turn = turn,
                                     .player = player,
                                     .active_input = pointer,
                                     .source_turn = state.active_source_turns[player],
                                     .source = copied ? state.source_generation[turn & 31][player] == turn &&
                                                                state.source_types[turn & 31][player] != trace::InputSource{}
                                                            ? state.source_types[turn & 31][player]
                                                            : trace::InputSource::Unknown
                                                      : trace::InputSource::Retained,
                                     .source_turn_known = state.active_source_known[player],
                                     .axes = move.axes,
                                     .input_bits = move.input_bits,
                                     .joystick = move.joystick,
                                     .history_copied = copied};
                                 write(trace::RecordKind::PreparedInput, record, pointer);
                             }));

    plan.observe_instruction(locations.readiness_default, fc::before([](const fc::CpuContext &context) noexcept {
                                 auto &state = thread_state();
                                 const auto turn = native::read<std::uint32_t>(context.ebp, -0x0C);
                                 const auto player = native::read<std::uint32_t>(context.ebp, -4);
                                 state.source_turns[turn & 31][player] = 0;
                                 state.source_known[turn & 31][player] = false;
                                 state.source_types[turn & 31][player] = trace::InputSource::Default;
                                 state.source_generation[turn & 31][player] = turn;
                                 if (state.readiness != nullptr) {
                                     state.readiness->record.forced = 1;
                                 }
                             }));
    // The native player gate decides whether this replay copies history or retains the active controls.
    plan.observe_instruction(locations.replay_player_gate, fc::before([](const fc::CpuContext &context) noexcept {
                                 if (auto *invocation = thread_state().replay; invocation != nullptr && (context.eax & 0xFFU) != 0) {
                                     invocation->copied_players |= std::uint64_t{1} << native::read<std::uint32_t>(context.ebp, -4);
                                 }
                             }));
    plan.observe_instruction(locations.replay_step, fc::before([this](const fc::CpuContext &context) noexcept {
                                 if (auto *invocation = thread_state().replay; invocation != nullptr) {
                                     invocation->record.last_turn = *replay_turn_;
                                     ++invocation->record.steps;
                                     auto &state = thread_state();
                                     const auto turn = *replay_turn_;
                                     for (std::int32_t player = 0; player != native::read<std::int32_t>(context.ebp, -4); ++player) {
                                         const auto pointer = active_inputs_[player];
                                         if (pointer == 0) {
                                             continue;
                                         }
                                         const auto copied = (invocation->copied_players & (std::uint64_t{1} << player)) != 0;
                                         if (copied) {
                                             state.active_source_turns[player] = state.source_turns[turn & 31][player];
                                             state.active_source_known[player] = state.source_known[turn & 31][player] &&
                                                                                 state.source_generation[turn & 31][player] == turn;
                                         }
                                         const auto move = native::read<native::PlayerMove>(pointer);
                                         const trace::PreparedInputRecord record{
                                             .candidate_turn = turn,
                                             .player = player,
                                             .active_input = pointer,
                                             .source_turn = state.active_source_turns[player],
                                             .source = copied ? state.source_generation[turn & 31][player] == turn &&
                                                                        state.source_types[turn & 31][player] != trace::InputSource{}
                                                                    ? state.source_types[turn & 31][player]
                                                                    : trace::InputSource::Unknown
                                                              : trace::InputSource::Retained,
                                             .source_turn_known = state.active_source_known[player],
                                             .axes = move.axes,
                                             .input_bits = move.input_bits,
                                             .joystick = move.joystick,
                                             .history_copied = copied};
                                         write(trace::RecordKind::PreparedInput, record, pointer, trace::Begin);
                                     }
                                     invocation->copied_players = 0;
                                 }
                             }));
    plan.observe_instruction(locations.replay_body, fc::before([this](const fc::CpuContext &) noexcept {
                                 if (auto *invocation = thread_state().replay; invocation != nullptr) {
                                     invocation->record.body_entered = 1;
                                     invocation->record.first_turn = *replay_turn_;
                                 }
                             }));

    if (settings_.capture == CaptureMode::Full) {
        // Finalize the preceding candidate when the next one starts or the native scan returns.
        plan.observe(locations.send_to_clients, fc::after([this]() noexcept {
                         finish_scheduling_candidate();
                     }));
        plan.observe_instruction(locations.candidate_entry, fc::before([this](const fc::CpuContext &context) noexcept {
                                     auto &state = thread_state();
                                     finish_scheduling_candidate();
                                     state.candidate_pending = true;
                                     const auto destination = native::read<std::int32_t>(context.ebp, -0x10);
                                     state.candidate = {.destination = destination,
                                                        .host_turn = static_cast<std::uint32_t>(*host_turn_),
                                                        .reason = 1,
                                                        .next_turn = native::read<std::uint32_t>(
                                                            player_states_ + destination * native::kPlayerStateStride, 0x24),
                                                        .native_time = *pipeline_time_};
                                 }));
        const auto stage = [this, &plan](fc::CodeLocation site, std::uint32_t reason) {
            plan.observe_instruction(site, fc::before([this, reason](const fc::CpuContext &context) noexcept {
                                         auto &state = thread_state();
                                         state.candidate.reason = reason;
                                         if (reason >= 7) {
                                             const auto pipe = native::read<std::int32_t>(context.ebp, -0x20);
                                             state.candidate.pipe = pipe;
                                             state.candidate.ticket = native::read<std::uint8_t>(context.ebp, pipe - 8);
                                             state.candidate.pipe_full = state.pipe_full[pipe];
                                             state.candidate.pipe_bits = pipe_bits_[pipe];
                                             state.candidate.bandwidth = bandwidth_[pipe];
                                             state.candidate.allowance = *allowance_;
                                         }
                                     }));
        };
        stage(locations.candidate_connection, 2);
        stage(locations.candidate_active, 3);
        stage(locations.candidate_primary, 4);
        stage(locations.candidate_role, 5);
        stage(locations.candidate_due, 6);
        plan.observe_instruction(locations.candidate_ticket, fc::before([this](const fc::CpuContext &context) noexcept {
                                     auto &candidate = thread_state().candidate;
                                     candidate.pipe = native::read<std::int32_t>(context.ebp, -0x20);
                                     candidate.ticket = native::read<std::uint8_t>(context.ebp, candidate.pipe - 8);
                                     candidate.pipe_full = thread_state().pipe_full[candidate.pipe];
                                     candidate.pipe_bits = pipe_bits_[candidate.pipe];
                                     candidate.bandwidth = bandwidth_[candidate.pipe];
                                     candidate.allowance = *allowance_;
                                 }));
        stage(locations.candidate_ticket_passed, 7);
        stage(locations.candidate_pipe, 8);
        stage(locations.candidate_window, 9);
        stage(locations.candidate_eligible, 10);
        plan.observe_instruction(locations.publication_selected, fc::before([this](const fc::CpuContext &context) noexcept {
                                     const auto destination = native::read<std::int32_t>(context.ebp, -0x14);
                                     trace::SchedulingDecisionRecord record{
                                         .destination = destination,
                                         .host_turn = static_cast<std::uint32_t>(*host_turn_),
                                         .reason = 0,
                                         .next_turn =
                                             native::read<std::uint32_t>(player_states_ + destination * native::kPlayerStateStride, 0x24),
                                         .native_time = *pipeline_time_,
                                         .checks = 1};
                                     write(trace::RecordKind::SchedulingDecision, record, destination, trace::Accepted);
                                 }));
        plan.observe_instruction(locations.pipe_full_result, fc::before([](const fc::CpuContext &context) noexcept {
                                     const auto pipe = native::read<std::uint32_t>(context.ebp, -0x10);
                                     thread_state().pipe_full[pipe] = context.eax & 0xFFU;
                                 }));
    }

    plan.observe_instruction(locations.retained_fill, fc::before([](const fc::CpuContext &context) noexcept {
                                 auto &state = thread_state();
                                 const auto turn = native::read<std::uint32_t>(context.ebp, -4);
                                 const auto player = native::read<std::uint32_t>(context.ebp, 8);
                                 state.source_turns[turn & 31][player] = state.source_turns[(turn - 1) & 31][player];
                                 state.source_known[turn & 31][player] = state.source_known[(turn - 1) & 31][player] &&
                                                                         state.source_generation[(turn - 1) & 31][player] == turn - 1;
                                 state.source_generation[turn & 31][player] = turn;
                                 state.source_types[turn & 31][player] = trace::InputSource::Retained;
                             }));
    if (settings_.capture == CaptureMode::Full) {
        const fc::FunctionLocation<ReadResponse> response = locations.read_response;
        plan.observe<CreateResponseInvocation>(
            response, fc::before([this](void *packet, void *player, CreateResponseInvocation &invocation) noexcept {
                const auto active = native::read<std::uint32_t>(player);
                const auto pending = native::read<std::uint32_t>(player, 4);
                invocation.record = {.player = native::pointer_id(player),
                                     .destination = static_cast<std::int32_t>((static_cast<const std::byte *>(player) - player_states_) /
                                                                              native::kPlayerStateStride),
                                     .packet = native::pointer_id(packet),
                                     .active_before = active,
                                     .pending_before = pending,
                                     .active_turn_before = native::read<std::uint32_t>(active, 0x104),
                                     .pending_turn_before = native::read<std::uint32_t>(pending, 0x104)};
                thread_state().create_response = &invocation;
            }),
            fc::after([this](void *, void *player, const CreateResponseInvocation &invocation) noexcept {
                thread_state().create_response = nullptr;
                auto record = invocation.record;
                record.active_after = native::read<std::uint32_t>(player);
                record.pending_after = native::read<std::uint32_t>(player, 4);
                record.active_turn_after = native::read<std::uint32_t>(record.active_after, 0x104);
                record.pending_turn_after = native::read<std::uint32_t>(record.pending_after, 0x104);
                write(trace::RecordKind::CreateResponse, record, record.destination);
            }));
        plan.observe_instruction(locations.response_decision, fc::before([this](const fc::CpuContext &context) noexcept {
                                     auto *invocation = thread_state().create_response;
                                     auto record = invocation != nullptr ? invocation->record : trace::CreateResponseRecord{};
                                     record.packet = native::read<std::uint32_t>(context.ebp, 8);
                                     record.response_turn = native::read<std::uint32_t>(context.ebp, -8);
                                     record.success = native::read<std::uint8_t>(context.ebp, -1);
                                     record.decision = native::read<std::uint32_t>(context.ebp, -0x0C);
                                     if (invocation != nullptr) {
                                         invocation->record = record;
                                     } else {
                                         record.pending_turn_before = native::read<std::uint32_t>(context.ebp, 0x0C);
                                         record.active_turn_before = native::read<std::uint32_t>(context.ebp, 0x10);
                                         write(trace::RecordKind::CreateResponse, record, record.response_turn, trace::Rejected);
                                     }
                                 }));

        const fc::FunctionLocation<ReceiveAck> ack = locations.receive_ack;
        plan.observe<AckInvocation>(
            ack, fc::before([this](std::int32_t destination, std::uint32_t turn, AckInvocation &invocation) noexcept {
                auto *player = player_states_ + destination * native::kPlayerStateStride;
                invocation.record = {.destination = destination,
                                     .turn = turn,
                                     .native_slot = 2,
                                     .mean_before = native::read<float>(player, 0x1C4),
                                     .deviation_before = native::read<float>(player, 0x1C8),
                                     .sent_turns = native::read<std::array<std::uint32_t, 2>>(player, 0x1B4),
                                     .sent_times = native::read<std::array<float, 2>>(player, 0x1BC)};
                thread_state().ack = &invocation;
            }),
            fc::after([this](std::int32_t destination, std::uint32_t, const AckInvocation &invocation) noexcept {
                thread_state().ack = nullptr;
                auto record = invocation.record;
                auto *player = player_states_ + destination * native::kPlayerStateStride;
                record.mean_after = native::read<float>(player, 0x1C4);
                record.deviation_after = native::read<float>(player, 0x1C8);
                record.samples = native::read<std::uint32_t>(player, 0x1B0);
                record.outstanding = static_cast<std::uint32_t>(native::read<std::uint32_t>(player, 0x1B4) != 0) +
                                     static_cast<std::uint32_t>(native::read<std::uint32_t>(player, 0x1B8) != 0);
                write(trace::RecordKind::UpdateAck, record, record.turn, record.accepted ? trace::Accepted : trace::Rejected);
            }));
        plan.observe_instruction(locations.ack_sample, fc::before([](const fc::CpuContext &context) noexcept {
                                     if (auto *invocation = thread_state().ack; invocation != nullptr) {
                                         invocation->record.accepted = 1;
                                         invocation->record.native_slot = native::read<std::uint32_t>(context.ebp, -8);
                                         invocation->record.sample = native::read<float>(context.ebp, -0x1C);
                                     }
                                 }));
    }
}

void Handler::finish_scheduling_candidate() noexcept {
    auto &state = thread_state();
    if (!state.candidate_pending) {
        return;
    }
    state.candidate_pending = false;
    auto next = state.candidate;
    const auto destination = static_cast<std::uint32_t>(next.destination);
    auto &previous = state.schedule_last[destination];
    auto &seen = state.schedule_seen[destination];
    const auto now = stamp();
    next.checks = 1;
    next.first_timestamp = now;
    next.last_timestamp = now;
    // Empty slots add no causal evidence until a previously observed peer disappears.
    if (!seen && next.reason == 1) {
        return;
    }
    const auto stable_role = next.reason <= 4;
    const auto same = seen && previous.reason == next.reason && (stable_role || previous.host_turn == next.host_turn) &&
                      previous.next_turn == next.next_turn && previous.pipe == next.pipe && previous.ticket == next.ticket &&
                      previous.pipe_full == next.pipe_full;
    if (same) {
        ++previous.checks;
        previous.last_timestamp = now;
        previous.last_host_turn = next.host_turn;
        return;
    }
    if (seen && previous.checks > 1) {
        write(trace::RecordKind::SchedulingDecision, previous, destination, trace::End);
    }
    next.last_host_turn = next.host_turn;
    previous = next;
    seen = true;
    write(trace::RecordKind::SchedulingDecision, next, destination, trace::Begin);
}

void Handler::finish_scheduling_summary(std::uint32_t destination) noexcept {
    auto &state = thread_state();
    if (state.schedule_seen[destination]) {
        write(trace::RecordKind::SchedulingDecision, state.schedule_last[destination], destination, trace::End);
        state.schedule_seen[destination] = false;
        state.schedule_last[destination] = {};
    }
}

void Handler::close_receive_window() noexcept {
    auto &state = thread_state();
    if (state.receive_window_open) {
        state.receive_window.closed_qpc = direct_transport::receive_stamp();
        write(trace::RecordKind::ReceiveWindow, state.receive_window, state.receive_window.window, trace::End | trace::Partial);
    }
    const auto ordinal = state.receive_window.window;
    state.receive_window = {.window = ordinal};
    state.receive_window_open = false;
    state.empty_windows = 0;
}

void Handler::begin_input_row(const fc::CpuContext &context, bool mini) noexcept {
    finish_input_row();
    auto &state = thread_state();
    const auto turn = native::read<std::uint32_t>(context.ebp, mini ? -0x44 : -0x18);
    const auto player = mini ? native::read<std::int32_t>(context.ebp, -0x10) : *current_destination_;
    const auto host = static_cast<std::uint32_t>(*host_turn_);
    const auto bit = std::uint64_t{1} << player;
    const auto mask = received_masks_[turn & 31];
    const auto &old = move_ring_[(turn & 31) * native::kPlayerCapacity + player];
    trace::UpdateReadRecord group{};
    identify_group(reinterpret_cast<void *>(context.ebp - 0x68), group);
    state.input_row = {.identity = group.identity,
                       .player = player,
                       .input_turn = turn,
                       .host_turn = host,
                       .disposition = turn + 3U < host    ? trace::InputDisposition::TooOld
                                      : turn >= host      ? trace::InputDisposition::CurrentOrFuture
                                      : (mask & bit) != 0 ? trace::InputDisposition::Duplicate
                                                          : trace::InputDisposition::ChangedLate,
                       .received_before = mask,
                       .late_before = late_masks_[turn & 7],
                       .old_hash = settings_.capture == CaptureMode::Full ? move_hash(old) : 0};
    state.input_row_pending = true;
}

void Handler::finish_input_row() noexcept {
    auto &state = thread_state();
    if (!state.input_row_pending) {
        return;
    }
    auto &record = state.input_row;
    const auto turn = record.input_turn;
    const auto player = static_cast<std::uint32_t>(record.player);
    record.received_after = received_masks_[turn & 31];
    record.late_after = late_masks_[turn & 7];
    if (settings_.capture == CaptureMode::Full) {
        record.history_hash = move_hash(move_ring_[(turn & 31) * native::kPlayerCapacity + player]);
    }
    if (record.disposition == trace::InputDisposition::CurrentOrFuture || record.disposition == trace::InputDisposition::ChangedLate) {
        state.source_turns[turn & 31][player] = turn;
        state.source_known[turn & 31][player] = true;
        state.source_types[turn & 31][player] = trace::InputSource::Received;
        state.source_generation[turn & 31][player] = turn;
    }
    write(trace::RecordKind::InputDisposition, record, turn, settings_.capture == CaptureMode::Full ? trace::None : trace::Partial);
    state.input_row_pending = false;
}

void Handler::observe_encoded_turn(const fc::CpuContext &context, bool mini) noexcept {
    auto &state = thread_state();
    state.encoded_turn = mini ? static_cast<std::uint32_t>(*client_turn_) - 1U : native::read<std::uint32_t>(context.ebp, -8);
    state.encoded_turn_known = true;
}

void Handler::advance_receive_window() noexcept {
    auto &state = thread_state();
    const auto turn = *client_turn_;
    if (state.receive_window_open && state.receive_window.input_turn == turn) {
        return;
    }
    const auto now = direct_transport::receive_stamp();
    const auto next = state.receive_window.window + 1;
    if (state.receive_window_open) {
        state.receive_window.closed_qpc = now;
        write(trace::RecordKind::ReceiveWindow, state.receive_window, state.receive_window.window,
              settings_.capture == CaptureMode::Full ? trace::End : trace::End | trace::Partial);
        state.empty_windows = state.receive_window.parsed_groups == 0 ? state.empty_windows + 1 : 0;
    }
    state.receive_window = {
        .window = next, .input_turn = turn, .host_turn = *host_turn_, .empty_windows_before = state.empty_windows, .opened_qpc = now};
    state.receive_window_open = true;
}

void Handler::observe_direct_send(const DirectSendObservation *observation) noexcept {
    if (observation == nullptr || (settings_.capture != CaptureMode::Full && observation->returned_bytes >= 0)) {
        return;
    }
    trace::DirectSendRecord record{.slot = observation->slot,
                                   .peer_id = observation->peer_id,
                                   .connection_id = observation->connection_id,
                                   .sequence = observation->sequence,
                                   .entered_qpc = observation->entered_qpc,
                                   .returned_qpc = observation->returned_qpc,
                                   .requested_bytes = observation->requested_bytes,
                                   .returned_bytes = observation->returned_bytes,
                                   .error = observation->error,
                                   .phase = observation->phase,
                                   .rejection = observation->rejection,
                                   .socket_id = observation->socket_id,
                                   .encoding_qpc = observation->encoding_qpc,
                                   .destination_ipv4_network_order = observation->destination_ipv4_network_order,
                                   .destination_port = observation->destination_port,
                                   .bound_port = observation->bound_port};
    if (observation->payload != nullptr && observation->payload_bytes >= 5 &&
        (observation->payload[0] < 0x1E || observation->payload[0] == soldier_state_sync::kStateUpdatePacket ||
         observation->payload[0] == soldier_state_sync::kPrecisionUpdatePacket ||
         observation->payload[0] == soldier_state_sync::kWeaponStateUpdatePacket)) {
        const auto *bytes = observation->payload;
        record.identity = {.sequence = static_cast<std::uint16_t>(bytes[1] | (static_cast<std::uint16_t>(bytes[2]) << 8)),
                           .packet_type = bytes[0],
                           .fragment_index = bytes[3],
                           .fragment_count = static_cast<std::uint8_t>(bytes[4] & 0x7F),
                           .valid = 1,
                           .source_slot = role_ == fc::Role::Server ? 64 : -1};
    }
    write(trace::RecordKind::DirectSend, record, observation->sequence,
          static_cast<std::uint16_t>((observation->returned_bytes < 0 ? trace::Anomaly : trace::None) |
                                     (settings_.capture == CaptureMode::Full ? trace::None : trace::Partial)),
          trace::Carrier::Direct);
}

void Handler::observe_recovery_drain(const RecoveryDrainObservation *observation) noexcept {
    if (observation == nullptr) {
        return;
    }
    auto &state = thread_state();
    if (settings_.capture != CaptureMode::Standard) {
        advance_receive_window();
        auto &window = state.receive_window;
        ++window.recovery_calls;
        if (window.first_drain_qpc == 0) {
            window.first_drain_qpc = observation->entered_qpc;
        }
        if (window.last_drain_qpc != 0) {
            window.maximum_drain_gap = std::max(window.maximum_drain_gap, observation->entered_qpc - window.last_drain_qpc);
        }
        window.last_drain_qpc = observation->entered_qpc;
    }
    const auto ordinal = ++state.drain_ordinal;
    if (observation->groups == 0) {
        return;
    }
    const trace::RecoveryDrainRecord record{.ordinal = ordinal,
                                            .receive_pass = state.receive_pass_ordinal,
                                            .groups = observation->groups,
                                            .oldest_turn = observation->oldest_turn,
                                            .newest_turn = observation->newest_turn,
                                            .entered_qpc = observation->entered_qpc,
                                            .returned_qpc = observation->returned_qpc};
    write(trace::RecordKind::RecoveryDrain, record, record.ordinal,
          settings_.capture == CaptureMode::Full ? trace::Complete : trace::Complete | trace::Partial);
}

void Handler::observe_send_storage(const SendStorageObservation *observation) noexcept {
    if (observation == nullptr) {
        return;
    }
    auto &server = thread_state().server;
    if (server.frame_active && observation->phase == 1) {
        ++server.scheduling.ack_slots;
        server.scheduling.invalid_slots += observation->native_slot < 0 || observation->native_slot >= 2;
    }
    if (settings_.capture != CaptureMode::Full) {
        return;
    }
    const trace::SendStorageRecord record{.destination = observation->destination,
                                          .turn = observation->turn,
                                          .native_slot = observation->native_slot,
                                          .phase = observation->phase,
                                          .peer_id = observation->peer_id,
                                          .network_time = observation->network_time,
                                          .before_count = observation->before_count,
                                          .after_count = observation->after_count,
                                          .extended_recorded = observation->extended_recorded,
                                          .native_write_skipped = observation->native_write_skipped};
    write(trace::RecordKind::SendStorage, record, record.turn);
}

void Handler::observe_create_transition(const CreateFenceObservation *observation) noexcept {
    if (observation == nullptr) {
        return;
    }
    auto &server = thread_state().server;
    if (server.frame_active && observation->phase == 1) {
        ++server.scheduling.create_fences;
        if (observation->destination >= 0 && observation->destination < 64) {
            server.scheduling.destination_mask |= std::uint64_t{1} << observation->destination;
        }
    }
    if (settings_.capture != CaptureMode::Full) {
        return;
    }
    const trace::CreateFenceRecord record{.destination = observation->destination,
                                          .phase = observation->phase,
                                          .map_before = observation->map_before,
                                          .map_after = observation->map_after,
                                          .turn_before = observation->turn_before,
                                          .turn_after = observation->turn_after,
                                          .sent_time = observation->sent_time,
                                          .network_time = observation->network_time,
                                          .average_rtt = observation->average_rtt,
                                          .deviation_rtt = observation->deviation_rtt};
    write(trace::RecordKind::CreateFence, record, record.destination);
}

} // namespace fc_plugins::network_diagnostics
