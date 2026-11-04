#include "handler.hpp"

#include "native.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace fc_plugins::network_diagnostics {

void Handler::plan_server(fc::Plan &plan, const ServerLayout &layout) {
    server_native_ = layout.native;
    if (settings_.capture == CaptureMode::Full) {
        plan_host_lag(plan, layout.lag);
    }

    // Reset the frame at GameLoop entry, before instruction modifiers and the displaced prologue run.
    plan.observe_instruction(layout.game_frame, fc::before([this](const fc::CpuContext &) noexcept {
                                 auto &state = thread_state();
                                 begin_server_frame(state, stamp());
                             }));

    // SubmitMove requests authority turns from the accumulator and may be called more than once per frame.
    plan.observe<MoveSubmissionInvocation>(
        layout.submit_move, fc::before([this](std::int32_t requested, MoveSubmissionInvocation &invocation) noexcept {
            invocation.start = stamp();
            invocation.turn_before = client_turn_ != nullptr ? *client_turn_ : -1;

            auto &state = thread_state();
            if (!state.server.frame_active) {
                begin_server_frame(state, invocation.start);
            }

            if (state.server.frame_active && requested > 0) {
                // Multiple requests share one frame; saturate the trace's 16-bit total without changing native input handling.
                const auto total = static_cast<std::uint32_t>(state.server.frame.requested_turns) + static_cast<std::uint32_t>(requested);
                state.server.frame.requested_turns =
                    static_cast<std::uint16_t>((std::min)(total, static_cast<std::uint32_t>((std::numeric_limits<std::uint16_t>::max)())));
            }
        }),
        fc::after([this](std::int32_t requested, const MoveSubmissionInvocation &invocation) noexcept {
            const trace::MoveSubmissionRecord record{
                .requested = requested,
                .turn_before = invocation.turn_before,
                .turn_after = client_turn_ != nullptr ? *client_turn_ : -1,
                .accumulator = accumulator_ != nullptr ? *accumulator_ : 0.0F,
                .duration = stamp() - invocation.start,
            };

            write(trace::RecordKind::MoveSubmission, record);
        }));

    // Accumulate receive time and newly set required input bits for the host turn awaiting simulation.
    // A final receive after FinishTurn can contribute to a host turn that is never simulated.
    plan.observe<ReceiveHostInvocation>(layout.receive_host, fc::before([this](ReceiveHostInvocation &invocation) noexcept {
                                            invocation.start = stamp();
                                            invocation.turn = host_turn_ != nullptr ? *host_turn_ : -1;
                                            invocation.required = required_mask_ != nullptr ? *required_mask_ : 0;
                                            invocation.received_before = received_mask(invocation.turn) & invocation.required;
                                        }),
                                        fc::after([this](const ReceiveHostInvocation &invocation) noexcept {
                                            auto &state = thread_state();
                                            if (!state.server.frame_active) {
                                                return;
                                            }

                                            ++state.server.frame.receive_calls;
                                            state.server.turn.receive_duration += stamp() - invocation.start;

                                            const auto received = received_mask(invocation.turn) & invocation.required;
                                            state.server.turn.new_input_mask |= received & ~invocation.received_before;
                                        }));

    // Measure the ApplyLateMoves call even when waitlate or another native gate prevents historical replay.
    plan.observe<ReplayInvocation>(layout.rollback_late_moves, fc::before([this](ReplayInvocation &invocation) noexcept {
                                       invocation.start = stamp();
                                       invocation.record.host_turn = static_cast<std::uint32_t>(*host_turn_);
                                       invocation.record.network_enabled = *network_enabled_;
                                       invocation.record.waitlate = *waitlate_;
                                       if (settings_.capture != CaptureMode::Standard) {
                                           const auto turn = invocation.record.host_turn;
                                           invocation.record.required_mask = *previous_required_;
                                           for (std::uint32_t i = 0; i != 3; ++i) {
                                               invocation.record.late_masks[i] = late_masks_[(turn - 3 + i) & 7];
                                           }
                                       }
                                       auto &state = thread_state();
                                       invocation.previous = state.replay;
                                       state.replay = &invocation;
                                   }),
                                   fc::after([this](const ReplayInvocation &invocation) noexcept {
                                       auto &state = thread_state();
                                       state.replay = invocation.previous;
                                       auto record = invocation.record;
                                       record.duration = stamp() - invocation.start;
                                       if (state.server.frame_active) {
                                           state.server.turn.rollback_duration += record.duration;
                                       }
                                       if (settings_.capture != CaptureMode::Standard) {
                                           write(trace::RecordKind::LateReplay, record, record.host_turn);
                                       }
                                   }));

    // GameLoop reaches SaveLateState after readiness succeeds, marking a simulation step ready to run.
    // The call can return without saving history when waitlate is enabled.
    plan.observe<SaveLateInvocation>(layout.save_late_state, fc::before([this](SaveLateInvocation &invocation) noexcept {
                                         invocation.start = stamp();
                                         auto &state = thread_state();

                                         const auto candidate = host_turn_ != nullptr ? *host_turn_ : -1;
                                         const auto required = required_mask_ != nullptr ? *required_mask_ : 0;
                                         const auto received = received_mask(candidate) & required;

                                         auto &turn = state.server.turn;
                                         turn.candidate_turn = candidate;
                                         turn.client_turn = client_turn_ != nullptr ? *client_turn_ : -1;
                                         turn.requested_turns = static_cast<std::int32_t>(state.server.frame.requested_turns);
                                         turn.catch_up_ordinal = static_cast<std::uint32_t>(state.server.frame.turns_advanced) + 1U;
                                         turn.required_mask = required;
                                         turn.received_mask = received;
                                         turn.missing_mask = required & ~received;
                                     }),
                                     fc::after([this](const SaveLateInvocation &invocation) noexcept {
                                         auto &state = thread_state();
                                         state.server.save_end = stamp();
                                         state.server.turn.save_duration = state.server.save_end - invocation.start;
                                         state.server.turn_active = true;
                                         ++state.server.frame.turns_advanced;
                                     }));

    // FinishTurn completes the simulated turn by advancing the host turn and clearing old input masks.
    plan.observe<TimedInvocation>(layout.finish_turn, fc::before([this](TimedInvocation &invocation) noexcept {
                                      invocation.start = stamp();
                                  }),
                                  fc::after([this](const TimedInvocation &invocation) noexcept {
                                      auto &state = thread_state();
                                      if (!state.server.turn_active) {
                                          return;
                                      }

                                      const auto end = stamp();
                                      auto &turn = state.server.turn;
                                      // This interval includes synchronous simulation work and nested diagnostic callbacks.
                                      turn.simulation_duration = invocation.start - state.server.save_end;
                                      turn.finish_duration = end - invocation.start;
                                      turn.host_turn_after = host_turn_ != nullptr ? *host_turn_ : -1;

                                      write(trace::RecordKind::AuthorityTurn, turn, static_cast<std::uint32_t>(turn.candidate_turn),
                                            trace::Accepted);

                                      if (settings_.capture != CaptureMode::Standard) {
                                          write_authoritative_poses(state, turn.candidate_turn);
                                      }

                                      turn = {};
                                      state.server.turn_active = false;
                                  }));

    // Bracket one destination's update construction; only calls nested in SendHost contribute to the frame's output totals.
    plan.observe<WriteUpdateInvocation>(layout.write_update, fc::before([this](void *packet, WriteUpdateInvocation &invocation) noexcept {
                                            invocation.start = stamp();
                                            auto &state = thread_state();
                                            invocation.destination = current_destination_ != nullptr ? *current_destination_ : -1;
                                            begin_destination_update(state, invocation.destination, invocation.destination_state);
                                            invocation.destination_state.packet = native::pointer_id(packet);
                                        }),
                                        fc::after([this](void *, const WriteUpdateInvocation &invocation) noexcept {
                                            auto &state = thread_state();
                                            finish_destination_update(state, invocation.destination_state);
                                            if (state.send_host == nullptr) {
                                                return;
                                            }

                                            ++state.server.frame.write_updates;
                                            state.server.frame.write_update_duration += stamp() - invocation.start;
                                            if (invocation.destination >= 0 &&
                                                invocation.destination < static_cast<std::int32_t>(native::kPlayerCapacity)) {
                                                state.server.frame.destination_mask |= std::uint64_t{1} << invocation.destination;
                                            }
                                        }));

    // SendHost completes the frame aggregate after its destination updates; early frame exits can omit this call.
    plan.observe<SendHostInvocation>(
        layout.send_host, fc::before([this](SendHostInvocation &invocation) noexcept {
            invocation.start = stamp();
            auto &state = thread_state();
            invocation.previous = state.send_host;
            state.send_host = &invocation;
        }),
        fc::after([this](const SendHostInvocation &invocation) noexcept {
            auto &state = thread_state();
            if (state.send_host == &invocation) {
                state.send_host = invocation.previous;
            }

            if (!state.server.frame_active) {
                return;
            }

            auto &server = state.server;
            const auto snapshot = readiness();
            server.frame.host_turn_after = host_turn_ != nullptr ? *host_turn_ : -1;
            server.frame.client_turn = client_turn_ != nullptr ? *client_turn_ : -1;
            server.frame.send_duration = stamp() - invocation.start;
            server.frame.required_mask = snapshot.required_mask;
            server.frame.missing_mask = snapshot.missing_mask;
            server.frame.policy = policy_bits(snapshot);

            // SendHost still sees the last simulation delta, or the outer frame delta if no simulation step ran.
            server.frame.accumulator = accumulator_ != nullptr ? *accumulator_ : 0.0F;
            server.frame.fixed_delta = fixed_delta_ != nullptr ? *fixed_delta_ : 0.0F;
            server.frame.simulation_delta = simulation_delta_ != nullptr ? *simulation_delta_ : 0.0F;
            server.frame.time_scale = time_scale_ != nullptr ? *time_scale_ : 1.0F;
            write_policy_if_changed(state, snapshot);

            const auto flags = static_cast<std::uint16_t>(
                trace::End | (server.frame.turns_advanced == 0 && server.frame.missing_mask != 0 ? trace::Anomaly : trace::None));
            write(trace::RecordKind::ServerFrame, server.frame, static_cast<std::uint32_t>(server.frame.host_turn_after), flags);

            const auto &scheduling = server.scheduling;
            if (scheduling.ack_slots != 0 || scheduling.create_fences != 0 || scheduling.blocked_destinations != 0 ||
                scheduling.timed_out_fences != 0) {
                write(trace::RecordKind::SendPass, scheduling, static_cast<std::uint32_t>(server.frame.host_turn_after));
            }

            server.frame_active = false;
        }));

    if (settings_.capture == CaptureMode::Standard) {
        return;
    }

    // Retain successful ReadMove results; its false stream terminator produces no move record.
    // Full capture also counts decoded structures inside ReceivePrimary.
    plan.observe<TimedInvocation>(
        layout.read_move, fc::before([this](void *, const void *, void *, TimedInvocation &invocation) noexcept {
            invocation.start = settings_.capture == CaptureMode::Full ? stamp() : 0;
        }),
        fc::after([this](void *packet, const void *previous, void *output, bool decoded, const TimedInvocation &invocation) noexcept {
            auto &state = thread_state();
            if (state.input_row_pending) {
                if (!decoded) {
                    state.input_row.disposition = trace::InputDisposition::Terminator;
                } else {
                    const auto move = native::read<native::PlayerMove>(output);
                    state.input_row.axes = move.axes;
                    state.input_row.input_bits = move.input_bits;
                    state.input_row.joystick = move.joystick;
                }
            }
            if (!decoded) {
                return;
            }

            if (state.input != nullptr) {
                ++state.input->record.moves;
            }

            if (!state.input_row_pending) {
                record_move(trace::RecordKind::MoveDecoding, packet, previous, output, trace::MoveCodecOperation::DecodeMove,
                            host_turn_ != nullptr ? *host_turn_ : -1, invocation.start);
            }
        }));

    plan.observe<TimedInvocation>(layout.read_mini_move, fc::before([this](void *, void *, TimedInvocation &invocation) noexcept {
                                      invocation.start = settings_.capture == CaptureMode::Full ? stamp() : 0;
                                  }),
                                  fc::after([this](void *packet, void *output, const TimedInvocation &invocation) noexcept {
                                      // ReadMiniMove materializes compressed input in a default move structure.
                                      if (auto &state = thread_state(); state.input != nullptr) {
                                          ++state.input->record.mini_moves;
                                      }

                                      auto &state = thread_state();
                                      if (state.input_row_pending) {
                                          const auto move = native::read<native::PlayerMove>(output);
                                          state.input_row.axes = move.axes;
                                          state.input_row.input_bits = move.input_bits;
                                          state.input_row.joystick = move.joystick;
                                          return;
                                      }
                                      record_move(trace::RecordKind::MoveDecoding, packet, nullptr, output,
                                                  trace::MoveCodecOperation::DecodeMiniMove, host_turn_ != nullptr ? *host_turn_ : -1,
                                                  invocation.start);
                                  }));

    // Record ordnance event construction and its ring-head change at the queueing stage.
    // Delivery requires later output and transport work.
    plan.observe<OrdnanceInvocation>(
        layout.create_ordnance_event, fc::before([this](const void *, const void *, OrdnanceInvocation &invocation) noexcept {
            invocation.event_head = event_head_ != nullptr ? *event_head_ : 0;
        }),
        fc::after([this](const void *projectile_class, const void *descriptor, const OrdnanceInvocation &invocation) noexcept {
            auto &state = thread_state();
            const auto index = invocation.event_head;
            ++state.ordinary_generations[index];
            state.ordinary_created_turns[index] = static_cast<std::uint32_t>(*host_turn_);
            const auto *event = ordinary_ring_ + index * 80;
            trace::OrdnanceEventRecord record{
                .projectile_class = native::pointer_id(projectile_class),
                .descriptor = native::pointer_id(descriptor),
                .head_before = invocation.event_head,
                .head_after = event_head_ != nullptr ? *event_head_ : 0,
                .event = native::pointer_id(event),
                .type = native::read<std::uint32_t>(event),
                .source_turn = state.ordinary_created_turns[index],
                .generation = state.ordinary_generations[index],
            };

            native::copy(record.origin, descriptor);
            native::copy(record.direction, descriptor, server_native_.descriptor_direction);

            write(trace::RecordKind::OrdnanceEvent, record, record.descriptor, trace::Combat | trace::End);
        }));

    // Snapshot controller inputs and raw primary/secondary trigger state around Update.
    plan.observe<ControllerInvocation>(
        layout.controller_update, fc::before([this](void *controller, float delta, ControllerInvocation &invocation) noexcept {
            invocation.start = stamp();
            invocation.controllable = native::read<void *>(controller, server_native_.controller_controllable);
            auto *input = native::read<void *>(controller, server_native_.controller_input);
            invocation.record = {
                .controller = native::pointer_id(controller),
                .controllable = native::pointer_id(invocation.controllable),
                .player = native::read<std::int32_t>(invocation.controllable, server_native_.controllable_player),
                .delta = delta,
                .input_bits = native::read<std::uint32_t>(input, server_native_.input_buttons),
                .joystick = native::read<std::uint8_t>(input, server_native_.input_joystick),
                .primary_switch_before = native::read<std::uint32_t>(invocation.controllable, server_native_.primary_switch),
                .secondary_switch_before = native::read<std::uint32_t>(invocation.controllable, server_native_.secondary_switch),
            };
            native::copy(invocation.record.axes, input);
        }),
        fc::after([this](void *, float, const ControllerInvocation &invocation) noexcept {
            auto record = invocation.record;
            record.primary_switch_after = native::read<std::uint32_t>(invocation.controllable, server_native_.primary_switch);
            record.secondary_switch_after = native::read<std::uint32_t>(invocation.controllable, server_native_.secondary_switch);
            record.duration = stamp() - invocation.start;

            // Remember exact matches to the soldier vtable for FinishTurn; these borrowed pointers acquire no object ownership.
            auto &state = thread_state();
            const auto is_soldier =
                invocation.controllable != nullptr && native::read<const void *>(invocation.controllable) == soldier_controllable_vtable_;
            if (is_soldier && record.player >= 0 && record.player < static_cast<std::int32_t>(native::kPlayerCapacity)) {
                state.soldiers[static_cast<std::size_t>(record.player)] = invocation.controllable;
            }

            write(trace::RecordKind::ControllerState, record, record.controllable, trace::Combat);
        }));

    if (settings_.capture == CaptureMode::Full) {
        // Count FillTurns deltas under ReceivePrimary; IsTurnReady also fills history directly outside these calls.
        plan.observe<FillTurnsInvocation>(layout.fill_turns,
                                          fc::before([this](std::int32_t player, std::uint32_t, FillTurnsInvocation &invocation) noexcept {
                                              const auto valid = player >= 0 && player < static_cast<std::int32_t>(native::kPlayerCapacity);
                                              invocation.counter = valid ? filled_turn_counts_[static_cast<std::size_t>(player)] : nullptr;
                                              invocation.before = invocation.counter != nullptr ? *invocation.counter : 0;
                                          }),
                                          fc::after([this](std::int32_t, std::uint32_t, const FillTurnsInvocation &invocation) noexcept {
                                              auto &state = thread_state();
                                              if (state.input == nullptr || invocation.counter == nullptr) {
                                                  return;
                                              }

                                              const auto after = *invocation.counter;
                                              const auto filled = after >= invocation.before ? after - invocation.before : 0;
                                              if (filled != 0) {
                                                  // filled_players counts positive FillTurns calls, including repeats for one player.
                                                  ++state.input->record.filled_players;
                                                  state.input->record.filled_turns += filled;
                                              }
                                          }));

        // ReceivePrimary owns decoded moves, FillTurns deltas, and attempted reconstructed acknowledgements.
        plan.observe<InputInvocation>(layout.receive_primary, fc::before([this](InputInvocation &invocation) noexcept {
                                          invocation.start = stamp();
                                          auto &state = thread_state();
                                          invocation.previous = state.input;
                                          state.input = &invocation;
                                      }),
                                      fc::after([this](const InputInvocation &invocation) noexcept {
                                          if (auto &state = thread_state(); state.input == &invocation) {
                                              state.input = invocation.previous;
                                          }

                                          auto record = invocation.record;
                                          record.duration = stamp() - invocation.start;

                                          write(trace::RecordKind::ServerInputPass, record, 0, trace::End);
                                      }));

        // ReadTurnLowBits has reconstructed the ACK, but native high-water acceptance still follows this site.
        // Count attempts, including zero, old, and duplicate turns, before instruction modifiers and displaced instructions.
        plan.observe_instruction(layout.ack_reconstruction, fc::before([this](const fc::CpuContext &context) noexcept {
                                     auto &state = thread_state();
                                     if (state.input == nullptr || context.ebp == 0) {
                                         return;
                                     }

                                     auto turn = native::read<std::int32_t>(context.ebp - server_native_.acknowledgement_turn_local);
                                     if (turn == -1) {
                                         turn = 0;
                                     }

                                     const auto player =
                                         native::read<std::int32_t>(context.ebp - server_native_.acknowledgement_player_local);

                                     auto &input = state.input->record;
                                     if (input.acknowledgements++ == 0) {
                                         input.oldest_acknowledged_turn = turn;
                                         input.newest_acknowledged_turn = turn;
                                     } else {
                                         input.oldest_acknowledged_turn = (std::min)(input.oldest_acknowledged_turn, turn);
                                         input.newest_acknowledged_turn = (std::max)(input.newest_acknowledged_turn, turn);
                                     }

                                     if (player >= 0 && player < static_cast<std::int32_t>(native::kPlayerCapacity)) {
                                         input.acknowledged_players |= std::uint64_t{1} << player;
                                     }
                                 }));
    }

    plan_combat(plan, layout.combat);
}

void Handler::plan_host_lag(fc::Plan &plan, const HostLagLayout &layout) {
    plan.observe<LagInvocation>(layout.estimator, fc::before([this](std::int32_t destination, LagInvocation &invocation) noexcept {
                                    invocation.record.destination = destination;
                                    invocation.record.host_turn = static_cast<std::uint32_t>(*host_turn_);
                                    invocation.record.lagversion = *lagversion_;
                                    invocation.record.drop_lag_jumps = *drop_lag_jumps_;
                                }),
                                fc::after([this](std::int32_t destination, std::int32_t result, const LagInvocation &invocation) noexcept {
                                    auto &state = thread_state();
                                    if (state.destination != nullptr && state.destination->record.destination == destination) {
                                        state.destination->lag = invocation.record;
                                        state.destination->lag.estimator_result = result;
                                        state.destination->lag_known = true;
                                    }
                                }));

    // EAX already contains native result + 128; the following write consumes its low eight bits.
    plan.observe_instruction(layout.serialized_lag, fc::before([this](const fc::CpuContext &context) noexcept {
                                 observe_serialized_lag(context.eax);
                             }));
}

void Handler::observe_serialized_lag(std::uint32_t biased_value) noexcept {
    auto &state = thread_state();
    const auto *destination = state.destination;
    if (destination == nullptr || !destination->lag_known) {
        return;
    }
    auto record = destination->lag;
    if (record.destination < 0 || record.destination >= static_cast<std::int32_t>(native::kPlayerCapacity)) {
        return;
    }
    record.wire_value = biased_value & 0xFFU;
    record.transmitted_lag = static_cast<std::int32_t>(record.wire_value) - 128;
    const auto now = stamp();
    auto &summary = state.lag_summaries[record.destination];
    auto &seen = state.lag_seen[record.destination];
    const bool changed = !seen || summary.lagversion != record.lagversion || summary.drop_lag_jumps != record.drop_lag_jumps ||
                         summary.estimator_result != record.estimator_result || summary.wire_value != record.wire_value;
    if (changed && seen && summary.writes != 0) {
        write(trace::RecordKind::HostLag, summary, static_cast<std::uint32_t>(record.destination));
    }
    record.writes = changed ? 1 : summary.writes + 1;
    record.first_timestamp = changed || summary.writes == 0 ? now : summary.first_timestamp;
    record.last_timestamp = now;
    summary = record;
    seen = true;
    if (changed || summary.writes >= 20) {
        write(trace::RecordKind::HostLag, summary, static_cast<std::uint32_t>(record.destination));
        summary.writes = 0;
    }
}

void Handler::begin_server_frame(ThreadState &state, std::uint64_t now) noexcept {
    // Clear the frame and its pending turn, including scheduling totals and any unfinished prior aggregate.
    // Keep the last reported policy so unchanged frames do not repeat the policy record.
    const auto previous = state.server.frame_start;
    const auto last_policy = state.server.last_policy;
    const auto has_policy = state.server.has_policy;

    state.server = {};
    state.server.last_policy = last_policy;
    state.server.has_policy = has_policy;

    state.server.frame = {
        .host_turn_before = host_turn_ != nullptr ? *host_turn_ : -1,
        .frame_gap = previous == 0 ? 0 : now - previous,
    };
    state.server.frame_start = now;
    state.server.frame_active = true;

    // Clear prior controller pointers; this frame's callbacks supply the later FinishTurn pose sample.
    state.soldiers.fill(nullptr);
}

std::uint64_t Handler::received_mask(std::int32_t turn) const noexcept {
    return turn < 0 || received_masks_ == nullptr ? 0 : received_masks_[static_cast<std::uint32_t>(turn) & (native::kTurnCapacity - 1)];
}

Handler::ReadinessSnapshot Handler::readiness() const noexcept {
    // Copy the current input masks and policy at SendHost completion.
    ReadinessSnapshot result{
        .host_turn = host_turn_ != nullptr ? *host_turn_ : -1,
        .required_mask = required_mask_ != nullptr ? *required_mask_ : 0,
        .grace_turns = waitlate_grace_.load(std::memory_order_relaxed),
        .waitlate = waitlate_ != nullptr && *waitlate_ != 0,
        .network_enabled = network_enabled_ != nullptr && *network_enabled_ != 0,
    };

    const auto received = received_mask(result.host_turn) & result.required_mask;
    result.missing_mask = result.required_mask & ~received;

    return result;
}

std::uint32_t Handler::policy_bits(const ReadinessSnapshot &snapshot) noexcept {
    return (snapshot.waitlate ? 1U : 0U) | (static_cast<std::uint32_t>(snapshot.grace_turns) << 8U) |
           (snapshot.network_enabled ? 1U << 16U : 0U);
}

void Handler::write_policy_if_changed(ThreadState &state, const ReadinessSnapshot &snapshot) noexcept {
    // Compare the exact float bits and packed policy values so unchanged frames do not repeat the policy record.
    const auto fixed_delta = fixed_delta_ != nullptr ? *fixed_delta_ : 0.0F;
    const auto delta_bits = std::bit_cast<std::uint32_t>(fixed_delta);

    const auto key = (static_cast<std::uint64_t>(delta_bits) << 32U) | policy_bits(snapshot);
    if (state.server.has_policy && state.server.last_policy == key) {
        return;
    }

    state.server.has_policy = true;
    state.server.last_policy = key;

    const trace::NetworkPolicyRecord record{
        .waitlate = static_cast<std::uint8_t>(snapshot.waitlate),
        .network_enabled = static_cast<std::uint8_t>(snapshot.network_enabled),
        .grace_turns = snapshot.grace_turns,
        .fixed_delta = fixed_delta,
    };

    write(trace::RecordKind::NetworkPolicy, record);
}

void Handler::begin_destination_update(ThreadState &state, std::int32_t destination, DestinationInvocation &invocation) noexcept {
    if (settings_.capture == CaptureMode::Standard) {
        return;
    }

    // Ordinary event-ring cursors include consumed and culled events; movement alone cannot establish transmission.
    const auto valid = destination >= 0 && destination < static_cast<std::int32_t>(native::kPlayerCapacity);
    invocation.record = {
        .destination = destination,
        .event_cursor_before =
            settings_.capture == CaptureMode::Full && valid && event_cursors_[static_cast<std::size_t>(destination)] != nullptr
                ? *event_cursors_[static_cast<std::size_t>(destination)]
                : 0,
    };

    invocation.bits = settings_.capture == CaptureMode::Full ? 0 : UINT32_MAX;
    invocation.previous = state.destination;
    state.destination = &invocation;
}

void Handler::finish_destination_update(ThreadState &state, const DestinationInvocation &invocation) noexcept {
    if (state.destination != &invocation) {
        return;
    }

    state.destination = invocation.previous;
    if (settings_.capture != CaptureMode::Full) {
        return;
    }
    auto output = invocation.record;

    if (output.destination >= 0 && output.destination < static_cast<std::int32_t>(native::kPlayerCapacity)) {
        const auto *cursor = event_cursors_[static_cast<std::size_t>(output.destination)];
        output.event_cursor_after = cursor != nullptr ? *cursor : 0;
    }

    output.event_head = event_head_ != nullptr ? *event_head_ : 0;

    write(trace::RecordKind::DestinationUpdate, output, static_cast<std::uint32_t>(output.destination), trace::End);
}

void Handler::write_authoritative_poses(ThreadState &state, std::int32_t turn) noexcept {
    // At FinishTurn, batch the soldiers remembered by player controllers; each record shares this authority turn.
    // These pointers must remain alive from the controller callback until this delayed read.
    trace::ServerPoseBatch batch{.turn = turn};
    for (std::size_t player = 0; player < state.soldiers.size(); ++player) {
        auto *controllable = state.soldiers[player];
        if (controllable == nullptr) {
            continue;
        }

        auto &pose = batch.soldiers[batch.count++];
        pose.player = static_cast<std::int32_t>(player);

        // The stored pointer addresses the soldier's Controllable subobject; subtract its offset to reach the entity fields.
        const auto *entity = static_cast<const std::byte *>(controllable) - server_native_.controllable_subobject;
        pose.position = fc::read_field<decltype(pose.position)>(entity, server_native_.soldier_position);
        pose.velocity = fc::read_field<decltype(pose.velocity)>(entity, server_native_.soldier_velocity);

        if (batch.count == batch.soldiers.size()) {
            write(trace::RecordKind::ServerSoldierPose, batch, static_cast<std::uint32_t>(turn), trace::Combat);
            batch.count = 0;
        }
    }

    if (batch.count != 0) {
        write(trace::RecordKind::ServerSoldierPose, batch, static_cast<std::uint32_t>(turn), trace::Combat);
    }
}

} // namespace fc_plugins::network_diagnostics
