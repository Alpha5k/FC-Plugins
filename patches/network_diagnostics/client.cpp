#include "handler.hpp"

#include "../direct_transport/receive_diagnostics.hpp"

#include "native.hpp"
#include "progress.hpp"

#include <algorithm>
#include <bit>

namespace fc_plugins::network_diagnostics {

void Handler::plan_client(fc::Plan &plan, const ClientLayout &layout) {
    client_native_ = layout.native;

    // Retail GameLoop::Update receives float deltas in seconds in XMM0/XMM1; sample before modifiers and the displaced prologue.
    // The gap measures consecutive entries on this thread; the record does not measure frame completion.
    plan.observe_instruction(layout.game_frame, fc::before([this](const fc::CpuContext &context) noexcept {
                                 auto &state = thread_state();
                                 const auto now = stamp();
                                 if (settings_.capture != CaptureMode::Standard) {
                                     begin_presentation_frame(state);
                                 }

                                 const trace::ClientFrameRecord record{
                                     .xmm0_delta = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[0].low)),
                                     .xmm1_delta = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[1].low)),
                                     .gap = state.last_frame == 0 ? 0 : now - state.last_frame,
                                 };
                                 state.last_frame = now;

                                 write(trace::RecordKind::ClientFrame, record);
                             }));

    // Compare the requested input count with the resulting local turn and accumulator; native code can clamp the request.
    plan.observe<MoveSubmissionInvocation>(layout.submit_move,
                                           fc::before([this](std::int32_t, MoveSubmissionInvocation &invocation) noexcept {
                                               if (settings_.capture != CaptureMode::Standard) {
                                                   advance_receive_window();
                                               }
                                               invocation.start = stamp();
                                               invocation.turn_before = client_turn_ != nullptr ? *client_turn_ : -1;
                                           }),
                                           fc::after([this](std::int32_t requested, const MoveSubmissionInvocation &invocation) noexcept {
                                               if (settings_.capture != CaptureMode::Standard) {
                                                   advance_receive_window();
                                               }
                                               const trace::MoveSubmissionRecord record{
                                                   .requested = requested,
                                                   .turn_before = invocation.turn_before,
                                                   .turn_after = client_turn_ != nullptr ? *client_turn_ : -1,
                                                   .accumulator = accumulator_ != nullptr ? *accumulator_ : 0.0F,
                                                   .duration = stamp() - invocation.start,
                                               };

                                               write(trace::RecordKind::MoveSubmission, record);
                                           }));

    // Aggregate actual parser calls and their native unsigned-turn guard outcomes during this ReceiveClient call.
    plan.observe<ClientReceiveInvocation>(layout.receive_client, fc::before([this](ClientReceiveInvocation &invocation) noexcept {
                                              if (settings_.capture != CaptureMode::Standard) {
                                                  advance_receive_window();
                                                  ++thread_state().receive_window.receive_passes;
                                              }
                                              const auto ordinal = ++thread_state().receive_pass_ordinal;
                                              invocation.record.first_pass = ordinal;
                                              invocation.record.last_pass = ordinal;
                                              invocation.record.calls = 1;
                                              invocation.record.entered_qpc = direct_transport::receive_stamp();
                                              invocation.record.turn_before = host_turn_ != nullptr ? *host_turn_ : -1;
                                              invocation.start = stamp();

                                              auto &state = thread_state();
                                              invocation.previous = state.client_receive;
                                              state.client_receive = &invocation;
                                          }),
                                          fc::after([this](const ClientReceiveInvocation &invocation) noexcept {
                                              if (auto &state = thread_state(); state.client_receive == &invocation) {
                                                  state.client_receive = invocation.previous;
                                              }

                                              auto record = invocation.record;
                                              record.turn_after = host_turn_ != nullptr ? *host_turn_ : -1;
                                              record.duration = stamp() - invocation.start;
                                              record.maximum_duration = record.duration;
                                              record.returned_qpc = direct_transport::receive_stamp();
                                              observe_client_receive(record);
                                          }));

    // Capture group identity before ReadUpdate can consume its packets; nested parsers keep their own outcome.
    plan.observe<ReadUpdateInvocation>(
        layout.read_update, fc::before([this](void *group, ReadUpdateInvocation &invocation) noexcept {
            invocation.start = settings_.capture == CaptureMode::Full ? stamp() : 0;
            invocation.record.turn_before = host_turn_ != nullptr ? *host_turn_ : -1;
            invocation.record.group = native::pointer_id(group);
            invocation.record.entered_qpc = settings_.capture == CaptureMode::Full ? direct_transport::receive_stamp() : 0;
            auto &state = thread_state();
            invocation.bits = settings_.capture == CaptureMode::Full ? 0 : UINT32_MAX;
            invocation.previous = state.read_update;
            state.read_update = &invocation;
            identify_group(group, invocation.record);
            if (settings_.capture != CaptureMode::Standard) {
                begin_authoritative_update(state);
            }
        }),
        fc::after([this](void *, const ReadUpdateInvocation &invocation) noexcept {
            auto &state = thread_state();
            if (state.read_update == &invocation) {
                state.read_update = invocation.previous;
            }
            auto record = invocation.record;
            record.returned_qpc = settings_.capture == CaptureMode::Full ? direct_transport::receive_stamp() : 0;
            record.turn_after = host_turn_ != nullptr ? *host_turn_ : -1;
            record.duration = settings_.capture == CaptureMode::Full ? stamp() - invocation.start : 0;
            if (state.client_receive != nullptr) {
                auto &batch = state.client_receive->record;
                ++batch.groups;
                batch.accepted += record.outcome == trace::UpdateOutcome::NewerTurn ? 1U : 0U;
                batch.rejected += record.outcome == trace::UpdateOutcome::OldOrEqualTurn ? 1U : 0U;
                batch.unclassified += record.outcome == trace::UpdateOutcome::GuardNotReached ? 1U : 0U;
                if (record.outcome != trace::UpdateOutcome::GuardNotReached) {
                    if (batch.turns_known++ == 0) {
                        batch.first_turn = record.advertised_turn;
                    }
                    batch.last_turn = record.advertised_turn;
                }
            }
            if (settings_.capture != CaptureMode::Standard) {
                advance_receive_window();
                ++state.receive_window.parsed_groups;
                state.receive_window.accepted_groups += record.outcome == trace::UpdateOutcome::NewerTurn;
                state.receive_window.rejected_groups += record.outcome == trace::UpdateOutcome::OldOrEqualTurn;
                finish_authoritative_update(state);
            }
            const auto flags = record.outcome == trace::UpdateOutcome::NewerTurn        ? trace::Accepted
                               : record.outcome == trace::UpdateOutcome::OldOrEqualTurn ? trace::Rejected
                                                                                        : trace::None;
            if (settings_.capture == CaptureMode::Standard) {
                const trace::UpdateAcceptanceRecord result{
                    .turn_before = record.turn_before,
                    .turn_after = record.turn_after,
                    .group = record.group,
                    .advertised_turn = record.advertised_turn,
                    .outcome = record.outcome,
                    .identity = record.identity,
                    .receive_pass = state.receive_pass_ordinal,
                };
                write(trace::RecordKind::UpdateAcceptance, result, record.group, flags);
            } else {
                write(trace::RecordKind::UpdateRead, record, record.group,
                      settings_.capture == CaptureMode::Full ? flags : flags | trace::Partial);
            }
        }));

    // Observe the actual unsigned JA result and its decoded turn; no parser or turn peek is repeated.
    plan.observe_instruction(layout.update_turn_result, fc::before([this](const fc::CpuContext &context) noexcept {
                                 if (auto *invocation = thread_state().read_update; invocation != nullptr) {
                                     invocation->record.advertised_turn = native::read<std::uint32_t>(context.ebp, -0x1C);
                                     constexpr std::uint32_t kUnsignedNotAbove = (1U << 0) | (1U << 6);
                                     invocation->record.outcome = (context.eflags & kUnsignedNotAbove) == 0
                                                                      ? trace::UpdateOutcome::NewerTurn
                                                                      : trace::UpdateOutcome::OldOrEqualTurn;
                                 }
                             }));

    // Prediction records replay frontiers and the native result that says whether it caught up to input.
    plan.observe<PredictionInvocation>(layout.predict, fc::before([this](PredictionInvocation &invocation) noexcept {
                                           invocation.start = stamp();
                                           invocation.turn_before = predict_turn_ != nullptr ? *predict_turn_ : -1;
                                           auto &state = thread_state();
                                           invocation.previous = state.prediction;
                                           state.prediction = &invocation;
                                       }),
                                       fc::after([this](bool result, const PredictionInvocation &invocation) noexcept {
                                           thread_state().prediction = invocation.previous;
                                           auto record = invocation.record;
                                           record.update_turn = update_turn_ != nullptr ? *update_turn_ : -1;
                                           record.predict_before = invocation.turn_before;
                                           record.predict_after = predict_turn_ != nullptr ? *predict_turn_ : -1;
                                           record.local_turn = local_predict_turn_ != nullptr ? *local_predict_turn_ : -1;
                                           record.caught_up = static_cast<std::uint32_t>(result);
                                           record.duration = stamp() - invocation.start;
                                           record.max_update_steps = *maximum_update_steps_;

                                           if (settings_.capture == CaptureMode::Standard) {
                                               const trace::PredictionProgressRecord progress{
                                                   .update_turn = record.update_turn,
                                                   .predict_before = record.predict_before,
                                                   .predict_after = record.predict_after,
                                                   .local_turn = record.local_turn,
                                                   .caught_up = record.caught_up,
                                                   .max_update_steps = record.max_update_steps,
                                                   .duration = record.duration,
                                               };
                                               write(trace::RecordKind::PredictionProgress, progress, 0, trace::End);
                                           } else {
                                               write(trace::RecordKind::Prediction, record, 0, trace::End);
                                           }
                                       }));

    // Adjustment time and lag are turn counts; scale is dimensionless.
    plan.observe<ClockInvocation>(layout.adjust_clock, fc::before([this](ClockInvocation &invocation) noexcept {
                                      if (settings_.capture == CaptureMode::Full) {
                                          invocation.phase = clock_phase(trace::ClockPhase::ScaleBefore);
                                      }
                                      invocation.record = {
                                          .adjust_before = adjust_time_ != nullptr ? *adjust_time_ : 0,
                                          .stable_before = stable_count_ != nullptr ? *stable_count_ : 0,
                                          .lag_before = client_lag_ != nullptr ? *client_lag_ : 0,
                                          .scale_before = time_scale_ != nullptr ? *time_scale_ : 1.0F,
                                      };
                                  }),
                                  fc::after([this](const ClockInvocation &invocation) noexcept {
                                      auto record = invocation.record;
                                      record.adjust_after = adjust_time_ != nullptr ? *adjust_time_ : 0;
                                      record.stable_after = stable_count_ != nullptr ? *stable_count_ : 0;
                                      record.lag_after = client_lag_ != nullptr ? *client_lag_ : 0;
                                      record.scale_after = time_scale_ != nullptr ? *time_scale_ : 1.0F;

                                      observe_clock_adjustment(record);
                                      if (settings_.capture == CaptureMode::Full && record.scale_before != record.scale_after) {
                                          auto &state = thread_state();
                                          ++state.clock_episode;
                                          auto before = invocation.phase;
                                          before.episode = state.clock_episode;
                                          write(trace::RecordKind::ClockPhase, before, state.clock_episode);
                                          write(trace::RecordKind::ClockPhase, clock_phase(trace::ClockPhase::ScaleAfter),
                                                state.clock_episode);
                                          state.presentation_pending = true;
                                      }
                                  }));

    if (settings_.capture == CaptureMode::Standard) {
        return;
    }

    if (settings_.capture == CaptureMode::Full) {
        plan_client_clock(plan, layout.clock);
    }

    const auto simulation = *select_simulation_layout(plan.profile());
    // This boundary runs inside Predict after its modifier has selected the remote-step divisor.
    plan.observe<SoldierUpdateInvocation>(
        simulation.soldier_update, fc::before([this](void *controllable, float delta, SoldierUpdateInvocation &invocation) noexcept {
            auto *soldier = static_cast<std::byte *>(controllable) - 0x240;
            invocation.record = soldier_posture(soldier, 2);
            invocation.record.delta = delta;
            auto *prediction = thread_state().prediction;
            if (prediction == nullptr) {
                return;
            }
            auto &record = prediction->record;
            const auto steps = *maximum_update_steps_;
            if (record.soldier_updates++ == 0) {
                record.first_update_steps = steps;
            }
            record.last_update_steps = steps;
            if (invocation.record.local_slot >= 0) {
                return;
            }

            const auto turn = invocation.record.prediction_turn;
            if (record.remote_updates++ == 0) {
                record.remote_minimum_delta = delta;
                record.remote_maximum_delta = delta;
                record.remote_first_turn = turn;
                record.remote_minimum_turn = turn;
                record.remote_maximum_turn = turn;
                record.remote_turn_runs = 1;
            } else {
                record.remote_minimum_delta = (std::min)(record.remote_minimum_delta, delta);
                record.remote_maximum_delta = (std::max)(record.remote_maximum_delta, delta);
                record.remote_minimum_turn = (std::min)(record.remote_minimum_turn, turn);
                record.remote_maximum_turn = (std::max)(record.remote_maximum_turn, turn);
                record.remote_turn_runs += record.remote_last_turn != turn;
            }
            record.remote_last_turn = turn;
            const auto player = invocation.record.player;
            if (player >= 0 && player < static_cast<std::int32_t>(native::kPlayerCapacity)) {
                record.remote_players |= std::uint64_t{1} << player;
            }
        }),
        fc::after([this](void *controllable, float, bool result, const SoldierUpdateInvocation &invocation) noexcept {
            auto record = invocation.record;
            const auto *soldier = static_cast<const std::byte *>(controllable) - 0x240;
            record.crouch_after = native::read<std::uint32_t>(soldier, 0x288);
            record.state_after = native::read<std::int32_t>(soldier, 0x744);
            record.result = static_cast<std::uint32_t>(result);
            write(trace::RecordKind::SoldierPosture, record, record.soldier, trace::End);
        }));

    if (settings_.capture == CaptureMode::Full) {
        // Copy native move structures after encoding, including null WriteMove terminators.
        plan.observe<TimedInvocation>(
            layout.write_move, fc::before([this](void *, const void *, const void *, TimedInvocation &invocation) noexcept {
                invocation.start = stamp();
            }),
            fc::after([this](void *packet, const void *previous, const void *current, const TimedInvocation &invocation) noexcept {
                record_move(trace::RecordKind::MoveEncoding, packet, previous, current, trace::MoveCodecOperation::EncodeMove,
                            client_turn_ != nullptr ? *client_turn_ : -1, invocation.start);
            }));

        plan.observe<TimedInvocation>(
            layout.write_mini_move, fc::before([this](void *, const void *, TimedInvocation &invocation) noexcept {
                invocation.start = stamp();
            }),
            fc::after([this](void *packet, const void *move, const TimedInvocation &invocation) noexcept {
                record_move(trace::RecordKind::MoveEncoding, packet, nullptr, move, trace::MoveCodecOperation::EncodeMiniMove,
                            client_turn_ != nullptr ? *client_turn_ : -1, invocation.start);
            }));

        // Compare the expanded reliable sequence frontier; native code can parse old batches without applying their actions again.
        plan.observe<ReliableInvocation>(layout.receive_reliable_events,
                                         fc::before([this](void *, ReliableInvocation &invocation) noexcept {
                                             invocation.start = stamp();
                                             invocation.frontier_before = reliable_frontier_ != nullptr ? *reliable_frontier_ : 0;
                                         }),
                                         fc::after([this](void *packet, const ReliableInvocation &invocation) noexcept {
                                             const trace::ReliableEventBatchRecord record{
                                                 .packet = native::pointer_id(packet),
                                                 .frontier_before = invocation.frontier_before,
                                                 .frontier_after = reliable_frontier_ != nullptr ? *reliable_frontier_ : 0,
                                                 .duration = stamp() - invocation.start,
                                             };

                                             write(trace::RecordKind::ReliableEventBatch, record, record.packet, trace::End);
                                         }));
    }

    // Sample the input published to SmoothInfo's matrix/time history. Callers may already have smoothed this matrix.
    // Later render-time history selection determines the displayed pose.
    plan.observe<TimedInvocation>(layout.set_render_matrix, fc::before([this](void *, const float *, TimedInvocation &invocation) noexcept {
                                      invocation.start = stamp();
                                  }),
                                  fc::after([this](void *smooth, const float *matrix, const TimedInvocation &invocation) noexcept {
                                      if (smooth == nullptr || matrix == nullptr) {
                                          return;
                                      }

                                      auto &state = thread_state();
                                      if (native::read<std::uint32_t>(smooth, client_native_.smooth_type) != 0 ||
                                          !select_pose(state.presentation_pose)) {
                                          return;
                                      }

                                      trace::ClientPoseRecord record{
                                          .object = native::pointer_id(smooth),
                                          .related = native::pointer_id(matrix),
                                          .stage = trace::ClientPoseStage::SmoothingHistory,
                                          .player = native::read<std::int32_t>(smooth, client_native_.smooth_player),
                                          .duration = stamp() - invocation.start,
                                      };
                                      record.matrix = fc::read_field<decltype(record.matrix)>(matrix);

                                      write(trace::RecordKind::ClientSoldierPose, record, record.object);
                                  }));

    plan_combat(plan, layout.combat);
}

void Handler::plan_client_clock(fc::Plan &plan, const ClientClockLayout &layout) {
    // This instruction is reached only when the native reset getter returned true.
    plan.observe_instruction(layout.reset_accumulator, fc::before([this](const fc::CpuContext &) noexcept {
                                 auto &state = thread_state();

                                 state.reset_pending = true;
                                 state.presentation_pending = true;
                                 write(trace::RecordKind::ClockPhase, clock_phase(trace::ClockPhase::ResetBefore), state.clock_episode);
                             }));
    plan.observe_instruction(layout.resume_accumulator, fc::before([this](const fc::CpuContext &) noexcept {
                                 auto &state = thread_state();
                                 if (state.reset_pending) {
                                     state.reset_pending = false;
                                     write(trace::RecordKind::ClockPhase, clock_phase(trace::ClockPhase::ResetAfter), state.clock_episode);
                                 }
                             }));

    // Observe only the actual clock publication path, after native storage and its preceding negative-delta clamp.
    plan.observe_instruction(layout.presentation_complete, fc::after([this](const fc::CpuContext &) noexcept {
                                 auto &state = thread_state();
                                 if (state.presentation_pending) {
                                     state.presentation_pending = false;
                                     write(trace::RecordKind::ClockPhase, clock_phase(trace::ClockPhase::PresentationNext),
                                           state.clock_episode);
                                 }
                             }));
}

void Handler::observe_client_receive(const trace::ClientReceivePassRecord &record) noexcept {
    auto &pending = thread_state().empty_client_receive;
    const auto empty = record.groups == 0 && record.turn_before == record.turn_after;
    const auto same = empty && pending.turn_after == record.turn_before;
    if (pending.calls != 0 && (!same || settings_.capture == CaptureMode::Full)) {
        write(trace::RecordKind::ClientReceivePass, pending, pending.last_pass, trace::End | trace::Empty);
        pending = {};
    }
    if (settings_.capture == CaptureMode::Full || !empty) {
        write(trace::RecordKind::ClientReceivePass, record, record.last_pass, trace::End);
        return;
    }
    append_empty_receive(pending, thread_state().last_empty_client_entry, record);
    if (pending.calls == 128) {
        write(trace::RecordKind::ClientReceivePass, pending, pending.last_pass, trace::End | trace::Empty);
        pending = {};
    }
}

void Handler::observe_clock_adjustment(trace::ClockAdjustmentRecord record) noexcept {
    auto &state = thread_state();
    auto &previous = state.clock_adjustment;
    const auto same = state.clock_adjustment_seen && previous.adjust_before == record.adjust_before &&
                      previous.adjust_after == record.adjust_after && previous.stable_before == record.stable_before &&
                      previous.stable_after == record.stable_after && previous.lag_before == record.lag_before &&
                      previous.lag_after == record.lag_after &&
                      std::bit_cast<std::uint32_t>(previous.scale_before) == std::bit_cast<std::uint32_t>(record.scale_before) &&
                      std::bit_cast<std::uint32_t>(previous.scale_after) == std::bit_cast<std::uint32_t>(record.scale_after);
    const auto now = direct_transport::receive_stamp();
    if (same) {
        if (previous.calls++ == 0) {
            previous.first_qpc = now;
            previous.host_turn = static_cast<std::uint32_t>(*host_turn_);
        }
        previous.last_qpc = now;
        previous.last_host_turn = static_cast<std::uint32_t>(*host_turn_);
        if (previous.calls == 128) {
            write(trace::RecordKind::ClockAdjustment, previous);
            previous.calls = 0;
        }
        return;
    }
    if (previous.calls != 0) {
        write(trace::RecordKind::ClockAdjustment, previous);
    }
    record.host_turn = static_cast<std::uint32_t>(*host_turn_);
    record.last_host_turn = record.host_turn;
    record.calls = 1;
    record.first_qpc = now;
    record.last_qpc = now;
    write(trace::RecordKind::ClockAdjustment, record);
    previous = record;
    previous.calls = 0;
    state.clock_adjustment_seen = true;
}

void Handler::flush_client_progress() noexcept {
    auto &state = thread_state();
    if (state.empty_client_receive.calls != 0) {
        write(trace::RecordKind::ClientReceivePass, state.empty_client_receive, state.empty_client_receive.last_pass,
              trace::End | trace::Empty);
        state.empty_client_receive = {};
    }
    if (state.clock_adjustment.calls != 0) {
        write(trace::RecordKind::ClockAdjustment, state.clock_adjustment);
    }
    state.clock_adjustment = {};
    state.clock_adjustment_seen = false;
}

float Handler::clock_value(ClockField field) const noexcept {
    return *clock_values_[static_cast<std::size_t>(field)];
}

trace::ClockPhaseRecord Handler::clock_phase(trace::ClockPhase phase) const noexcept {
    return {
        .phase = phase,
        .episode = thread_state().clock_episode,
        .received_host_turn = *host_turn_,
        .acknowledged_turn = *acknowledged_turn_,
        .input_turn = *client_turn_,
        .update_turn = *update_turn_,
        .predicted_turn = *predict_turn_,
        .local_predicted_turn = *local_predict_turn_,
        .accumulator = *accumulator_,
        .presentation_clock = clock_value(ClockField::PresentationClock),
        .presentation_delta = clock_value(ClockField::PresentationDelta),
        .interpolation = clock_value(ClockField::Interpolation),
        .frame_delta = clock_value(ClockField::FrameDelta),
        .scale = *time_scale_,
        .additive_correction = clock_value(ClockField::AdditiveCorrection),
        .native_time = clock_value(ClockField::NativeTime),
        .decision_deadline = clock_value(ClockField::DecisionDeadline),
    };
}

bool Handler::select_pose(PoseSelection &sampler) noexcept {
    return sampler.observed++ == sampler.selected;
}

void Handler::begin_authoritative_update(ThreadState &state) noexcept {
    // Select one pose by rotating through the previous update's count; changing counts can skip a selection.
    auto &sample = state.authority_pose;
    sample.selected = sample.previous_count == 0 ? 0 : static_cast<std::uint32_t>(sample.sequence % sample.previous_count);
    ++sample.sequence;
    sample.observed = 0;
    sample.captured = false;
}

void Handler::finish_authoritative_update(ThreadState &state) noexcept {
    auto &sample = state.authority_pose;
    sample.previous_count = sample.observed;

    if (sample.captured) {
        write(trace::RecordKind::ClientSoldierPose, sample.record, sample.record.object);
    }
}

void Handler::begin_presentation_frame(ThreadState &state) noexcept {
    // Rotate through the previous frame's object count; changing counts can repeat or skip a selection.
    auto &sample = state.presentation_pose;
    sample.previous_count = sample.observed;
    sample.selected = sample.previous_count == 0 ? 0 : static_cast<std::uint32_t>(sample.sequence % sample.previous_count);
    ++sample.sequence;
    sample.observed = 0;
}

void Handler::before_soldier_read(void *soldier, void *, void *, SoldierReadState &state) noexcept {
    // Selection counts non-null soldier reader entries, including reads in an update later classified stale.
    auto &thread = thread_state();
    if (soldier == nullptr || !select_pose(thread.authority_pose)) {
        return;
    }

    state.started = stamp();
    state.tracked = true;
}

void Handler::after_soldier_read(void *soldier, void *stream, void *, const SoldierReadState &state) noexcept {
    auto &thread = thread_state();
    auto posture = soldier_posture(soldier, 1);
    posture.stream = native::pointer_id(stream);
    write(trace::RecordKind::SoldierPosture, posture, posture.soldier, trace::End);
    if (!state.tracked) {
        return;
    }

    // Copy the soldier after its reader or function modifier returns; later work in ReadUpdate can still change it.
    auto &sample = thread.authority_pose;
    sample.record = {
        .object = native::pointer_id(soldier),
        .related = native::pointer_id(stream),
        .stage = trace::ClientPoseStage::Authoritative,
        .player = native::read<std::int32_t>(soldier, client_native_.soldier_player),
        .duration = stamp() - state.started,
    };
    sample.record.matrix = fc::read_field<decltype(sample.record.matrix)>(soldier, client_native_.soldier_matrix);
    sample.captured = true;
}

std::uint32_t Handler::world_pass() const noexcept {
    std::uint32_t result{};
    for (std::size_t index = 0; index < world_pass_.size(); ++index) {
        result |= static_cast<std::uint32_t>(*world_pass_[index]) << (index * 8);
    }
    return result;
}

trace::SoldierPostureRecord Handler::soldier_posture(void *soldier, std::uint32_t stage) const noexcept {
    const auto player = native::read<std::int32_t>(soldier, 0x314);
    const auto crouch = native::read<std::uint32_t>(soldier, 0x288);
    const auto state = native::read<std::int32_t>(soldier, 0x744);
    const auto *read = thread_state().read_update;
    return {
        .soldier = native::pointer_id(soldier),
        .stage = stage,
        .player = player,
        .local_slot = get_joystick_index_.call(player).value_or(-1),
        .crouch_before = crouch,
        .crouch_after = crouch,
        .state_before = state,
        .state_after = state,
        .authority_turn = stage == 1 && read != nullptr ? static_cast<std::int32_t>(read->record.advertised_turn) : *update_turn_,
        .prediction_turn = *simulation_prediction_turn_,
        .world_pass = world_pass(),
    };
}

std::pair<std::int32_t, std::int32_t> Handler::identify_move(const void *move) const noexcept {
    if (move == nullptr) {
        return {-1, -1};
    }

    // History is ordered by turn slot then player. An aligned address identifies a ring slot, not the full move turn.
    const std::size_t players = role_ == fc::Role::Client ? 2 : native::kPlayerCapacity;
    const auto begin = reinterpret_cast<std::uintptr_t>(move_ring_);
    const auto candidate = reinterpret_cast<std::uintptr_t>(move);
    const auto byte_size = sizeof(native::PlayerMove) * native::kTurnCapacity * players;
    if (candidate < begin || candidate >= begin + byte_size || (candidate - begin) % sizeof(native::PlayerMove) != 0) {
        return {-1, -1};
    }

    const auto index = static_cast<std::size_t>((candidate - begin) / sizeof(native::PlayerMove));
    return {static_cast<std::int32_t>(index % players), static_cast<std::int32_t>(index / players)};
}

void Handler::record_move(trace::RecordKind kind, void *packet, const void *previous, const void *move, trace::MoveCodecOperation operation,
                          std::int32_t reference, std::uint64_t start) noexcept {
    auto identity = identify_move(move);
    if (role_ == fc::Role::Server && identity.first < 0 && *current_destination_ >= 0 &&
        *current_destination_ < static_cast<std::int32_t>(native::kPlayerCapacity)) {
        // A late move decodes into a stack temporary, so the receiving destination supplies its player identity.
        identity.first = *current_destination_;
    }

    // The reference is the current global frontier, independent of the turn represented by this history slot.
    trace::MoveCodecRecord record{
        .packet = native::pointer_id(packet),
        .previous = native::pointer_id(previous),
        .move = native::pointer_id(move),
        .operation = operation,
        .player = identity.first,
        .turn_slot = identity.second,
        .turn_reference = reference,
        .full_turn = thread_state().encoded_turn,
        .full_turn_known = static_cast<std::uint32_t>(role_ == fc::Role::Client && thread_state().encoded_turn_known),
        .duration = settings_.capture == CaptureMode::Full ? stamp() - start : 0,
    };

    if (move != nullptr) {
        const auto input = fc::read_field<native::PlayerMove>(move);
        record.axes = input.axes;
        record.input_bits = input.input_bits;
        record.joystick = input.joystick;
    }

    write(kind, record, record.packet);
}

} // namespace fc_plugins::network_diagnostics
