#include "handler.hpp"

#include "native.hpp"

#include <algorithm>
#include <bit>

namespace fc_plugins::hero_diagnostics {

void Handler::plan_client(fc::Plan &plan, const ClientLayout &layout) {
    plan_lifetime(plan, layout.lifetime);

    plan.observe<PredictionReturnInvocation>(layout.predict, fc::before([this](PredictionReturnInvocation &invocation) noexcept {
                                                 invocation.previous_scope = thread_state_.predict_scope;
                                                 invocation.scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
                                                 thread_state_.predict_scope = invocation.scope;
                                                 std::size_t index{};
                                                 subjects_.for_each_local([&](HeroSubject &subject) noexcept {
                                                     if (index < invocation.subjects.size()) {
                                                         invocation.subjects[index++] = subject;
                                                     }
                                                 });
                                             }),
                                             fc::after([this](bool result, const PredictionReturnInvocation &invocation) noexcept {
                                                 thread_state_.predict_scope = invocation.previous_scope;
                                                 const auto turn = turn_context();
                                                 for (auto &subject : invocation.subjects) {
                                                     if (subject && subjects_.live(*subject)) {
                                                         // Native true reports completion against clientTurn; it is not a count of
                                                         // replayed turns.
                                                         record_local_state(*subject, trace::LocalStage::PredictionReturn, invocation.scope,
                                                                            turn, trace::End | trace::NativeCalled, result);
                                                     }
                                                 }
                                             }));

    plan.observe<CameraInvocation>(
        layout.chase_setup, fc::before([this](void *camera, void *red_camera, CameraInvocation &invocation) noexcept {
            const auto trackable = native::read<std::uintptr_t>(camera, 0x0C);
            if (trackable < 0x258 || red_camera == nullptr) {
                return;
            }
            invocation.subject = snapshot_subject(subjects_.find_soldier(reinterpret_cast<void *>(trackable - 0x258)));
            invocation.red_camera = red_camera;
            invocation.scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
        }),
        fc::after([this](void *, void *, const CameraInvocation &invocation) noexcept {
            if (invocation.subject && invocation.subject->local_player != trace::kNoLocalPlayer && subjects_.live(*invocation.subject)) {
                record_display(*invocation.subject, trace::LocalStage::CameraOutput, invocation.scope,
                               reinterpret_cast<const float *>(static_cast<const std::byte *>(invocation.red_camera) + 0x30),
                               invocation.red_camera);
            }
        }));

    // Function observers bracket the selected behavior owner, including invocations that omit the original native body.
    plan.observe<UpdateInvocation>(layout.update, fc::before([this](void *weapon, float delta, UpdateInvocation &invocation) noexcept {
                                       begin_update(weapon, delta, delta, MeleeMode::Native, invocation);
                                   }),
                                   fc::after([this](void *weapon, float, bool result, const UpdateInvocation &invocation) noexcept {
                                       finish_update(weapon, result, invocation);
                                   }));

    plan.observe<NetworkInvocation>(layout.set_network_state,
                                    fc::before([this](void *weapon, std::int32_t state, bool flag, NetworkInvocation &invocation) noexcept {
                                        begin_network_state(weapon, state, flag, NetworkMode::Native, invocation);
                                    }),
                                    fc::after([this](void *weapon, std::int32_t, bool, const NetworkInvocation &invocation) noexcept {
                                        finish_network_state(weapon, invocation);
                                    }));

    plan.observe<TransitionInvocation>(layout.enter_state,
                                       fc::before([this](void *weapon, std::int32_t state, TransitionInvocation &invocation) noexcept {
                                           begin_enter_state(weapon, state, invocation);
                                       }),
                                       fc::after([this](void *weapon, std::int32_t, const TransitionInvocation &invocation) noexcept {
                                           finish_transition(weapon, invocation);
                                       }));

    if (settings_.capture != CaptureMode::Standard) {
        plan.observe<AnimatorInvocation>(
            layout.animator_state,
            fc::before([this](void *animator, std::int32_t state, bool active, std::uint8_t primary, std::uint8_t secondary, bool blend,
                              AnimatorInvocation &invocation) noexcept {
                begin_animator_state(animator, state, active, primary, secondary, blend, invocation);
            }),
            fc::after([this](void *, std::int32_t, bool, std::uint8_t, std::uint8_t, bool, const AnimatorInvocation &invocation) noexcept {
                finish_animator_state(invocation);
            }));
    }

    if (settings_.capture != CaptureMode::Standard) {
        // Before observers, modifiers, and after observers all run before displaced instructions resume.
        plan.observe_instruction<InputInvocation>(
            layout.input_queue, fc::before([this](const fc::CpuContext &context, InputInvocation &invocation) noexcept {
                begin_input_queue(context, invocation);
            }),
            fc::after([this](const fc::CpuContext &context, const InputInvocation &invocation) noexcept {
                finish_input_queue(context, invocation);
            }));

        plan.observe_instruction<PredictionInvocation>(
            layout.prediction_transition, fc::before([this](const fc::CpuContext &context, PredictionInvocation &invocation) noexcept {
                begin_prediction_transition(context, invocation);
            }),
            fc::after([this](const fc::CpuContext &context, const PredictionInvocation &invocation) noexcept {
                finish_prediction_transition(context, invocation);
            }));
    }
    plan.observe_call(layout.transition_sound, fc::before([this](void *sound, const void *position, const void *velocity,
                                                                 std::uint32_t reserved, bool force) noexcept {
                          observe_transition_sound(sound, position, velocity, reserved, force);
                      }));

    plan.observe<SoldierReadInvocation>(layout.soldier_read,
                                        fc::before([this](void *soldier, void *, void *, SoldierReadInvocation &invocation) noexcept {
                                            before_soldier_read(soldier, invocation);
                                        }),
                                        fc::after([this](void *, void *, void *, const SoldierReadInvocation &invocation) noexcept {
                                            after_soldier_read(invocation);
                                        }));

    // WeaponMelee::Read applies serialized state; nested reads temporarily replace this thread's setter correlation scope.
    plan.observe<ReadInvocation>(layout.read, fc::before([this](void *weapon, void *stream, ReadInvocation &invocation) noexcept {
                                     invocation.subject = snapshot_subject(bind(weapon));
                                     if (!invocation.subject || !subjects_.live(*invocation.subject)) {
                                         return;
                                     }

                                     invocation.previous_scope = thread_state_.read_scope;
                                     thread_state_.read_scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
                                     invocation.record = trace::NetworkStateRecord{
                                         .turn = turn_context(),
                                         .state_before = native::read<std::int32_t>(weapon, native::kStateOffset),
                                         .requested_state = -1,
                                         .state_after = -1,
                                         .state_time_before = native::read<float>(weapon, native::kStateTimeOffset),

                                         .scope = thread_state_.read_scope,
                                         .stream = native::pointer_id(stream),
                                     };
                                     invocation.record.mode_before = native::read<std::int32_t>(weapon, native::kModeOffset);
                                     invocation.record.mode_time_before = native::read<float>(weapon, native::kModeTimeOffset);
                                     invocation.record.input_time_before = native::read<float>(weapon, native::kInputTimeOffset);
                                     invocation.record.notified_time_before = native::read<float>(weapon, native::kNotifiedStateTimeOffset);
                                 }),
                                 fc::after([this](void *weapon, void *, const ReadInvocation &invocation) noexcept {
                                     // Scope cleanup is required even when nested native lifetime changes retire the identity.
                                     if (invocation.subject) {
                                         thread_state_.read_scope = invocation.previous_scope;
                                     }
                                     if (!invocation.subject || !subjects_.live(*invocation.subject)) {
                                         return;
                                     }

                                     auto record = invocation.record;
                                     record.state_after = native::read<std::int32_t>(weapon, native::kStateOffset);
                                     record.state_time_after = native::read<float>(weapon, native::kStateTimeOffset);
                                     record.action = native::read<std::uint16_t>(weapon, native::kActionOffset);
                                     record.state_flags = native::read<std::uint8_t>(weapon, native::kStateFlagsOffset);
                                     record.mode_after = native::read<std::int32_t>(weapon, native::kModeOffset);
                                     record.mode_time_after = native::read<float>(weapon, native::kModeTimeOffset);
                                     record.input_time_after = native::read<float>(weapon, native::kInputTimeOffset);
                                     record.notified_time_after = native::read<float>(weapon, native::kNotifiedStateTimeOffset);
                                     record.native_called = 1;
                                     record.operation = trace::NetworkStateOperation::Read;

                                     auto flags = static_cast<std::uint16_t>(trace::Authority | trace::NativeCalled);
                                     if (record.state_before != record.state_after) {
                                         flags = static_cast<std::uint16_t>(flags | trace::StateChanged);
                                     }

                                     write_subject(trace::RecordKind::NetworkState, record, *invocation.subject, flags);
                                 }));

    // ExitState derives the state being left from the weapon, then records the resulting state and motion.
    plan.observe<TransitionInvocation>(layout.exit_state, fc::before([this](void *weapon, TransitionInvocation &invocation) noexcept {
                                           invocation.subject = snapshot_subject(bind(weapon));
                                           if (!invocation.subject || !subjects_.live(*invocation.subject)) {
                                               return;
                                           }

                                           const auto state = native::read<std::int32_t>(weapon, native::kStateOffset);
                                           const trace::TransitionRecord record{
                                               .turn = turn_context(),
                                               .requested_state = state,
                                               .state_before = state,
                                               .state_after = -1,
                                               .previous_state = native::read<std::int32_t>(weapon, native::kPreviousStateOffset),
                                               .state_time_before = native::read<float>(weapon, native::kStateTimeOffset),
                                               .kind = trace::TransitionKind::Exit,
                                           };
                                           invocation.record = record;
                                           invocation.flags = transition_flags(weapon, record.turn);
                                       }),
                                       fc::after([this](void *weapon, const TransitionInvocation &invocation) noexcept {
                                           finish_transition(weapon, invocation);
                                       }));

    if (settings_.capture != CaptureMode::Standard) {
        plan_controls(plan, layout.controls);
        plan_velocity(plan, layout.velocity);
    }

    if (settings_.capture != CaptureMode::Standard) {
        // Combat capture adds parsed remote input, projectile deflection, and energy-gated jump and roll attempts.
        plan.observe_instruction(layout.parsed_move, fc::before([this](const fc::CpuContext &context) noexcept {
                                     observe_parsed_move(context);
                                 }));

        plan_contact(plan, layout.contact);

        const auto observe_locomotion_attempt = [this, &plan](const fc::FunctionLocation<LocomotionFunction> &location,
                                                              trace::LocomotionOperation operation) {
            plan.observe<LocomotionInvocation>(
                location, fc::before([this](void *soldier, LocomotionInvocation &invocation) noexcept {
                    invocation.subject = snapshot_subject(subjects_.find_soldier(soldier));
                    if (invocation.subject && subjects_.live(*invocation.subject)) {
                        invocation.before = locomotion_state(soldier);
                    }
                }),
                fc::after([this, operation](void *soldier, bool result, const LocomotionInvocation &invocation) noexcept {
                    if (invocation.subject && subjects_.live(*invocation.subject)) {
                        record_locomotion(*invocation.subject, operation, invocation.before, locomotion_state(soldier), result);
                    }
                }));
        };

        observe_locomotion_attempt(layout.jump, trace::LocomotionOperation::Jump);

        observe_locomotion_attempt(layout.roll, trace::LocomotionOperation::Roll);
    }

    if (settings_.capture != CaptureMode::Standard) {
        // Action and Pose use separate entry/exit samples; SetupPose may reuse a cached pose without advancing animation.
        plan.observe<PresentationInvocation>(
            layout.action,
            fc::before([this](const fc::NativeContext &native_context, void *animator, std::uint32_t state, const float *matrix,
                              float blend, PresentationInvocation &invocation) noexcept {
                // SetAction's final word is an animation hash; the float parameter retains its bits through bit_cast.
                invocation = {.subject = snapshot_subject(subjects_.find_animator(animator)),
                              .argument0 = state,
                              .argument1 = std::bit_cast<std::uint32_t>(blend)};
                invocation.scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
                invocation.previous_scope = static_cast<std::uint32_t>(native_context.return_address());
                if (invocation.subject && subjects_.live(*invocation.subject)) {
                    invocation.requested_position = native::read<std::array<float, 3>>(matrix, 0x30);
                    record_action(*invocation.subject, invocation, trace::Begin);
                }
            }),
            fc::after([this](void *, std::uint32_t, const float *, float, const PresentationInvocation &invocation) noexcept {
                if (invocation.subject && subjects_.live(*invocation.subject)) {
                    record_action(*invocation.subject, invocation, trace::End | trace::NativeCalled);
                }
            }));
    }
    if (settings_.capture == CaptureMode::Full) {
        plan.observe<PresentationInvocation>(
            layout.pose, fc::before([this](void *animator, void *pose, PresentationInvocation &invocation) noexcept {
                invocation = {
                    .subject = snapshot_subject(subjects_.find_animator(animator)), .argument0 = native::pointer_id(pose), .argument1 = 0};
                invocation.previous_scope = thread_state_.pose_scope;
                invocation.scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
                thread_state_.pose_scope = invocation.scope;
                if (auto *subject = subjects_.find_animator(animator)) {
                    subject->pose_scope = invocation.scope;
                }
            }),
            fc::after([this](void *, void *, const PresentationInvocation &invocation) noexcept {
                thread_state_.pose_scope = invocation.previous_scope;
                if (invocation.subject && subjects_.live(*invocation.subject)) {

                    auto record = presentation_sample(*invocation.subject, trace::PresentationKind::Pose);
                    record.argument0 = invocation.argument0;
                    record.argument1 = invocation.scope;

                    write_subject(trace::RecordKind::PresentationFrame, record, *invocation.subject, trace::End | trace::NativeCalled);
                }
            }));
    }
}

void Handler::observe_parsed_move(const fc::CpuContext &context) noexcept {
    if (remote_moves_ == nullptr || context.ebp < sizeof(std::int32_t)) {
        return;
    }

    // EBP-4 holds the player slot whose move row was just parsed; its device byte may retain an earlier value.
    const auto player = fc::read_field<std::int32_t>(context.ebp - sizeof(std::int32_t));
    auto *subject = subjects_.find_player(player);
    if (subject == nullptr || subject->local_player != trace::kNoLocalPlayer) {
        return;
    }

    const auto move = remote_moves_[static_cast<std::size_t>(player)];
    const auto changed = !subject->move_seen || move != subject->last_move;
    subject->last_move = move;
    subject->move_seen = true;

    // Trigger words and soldier motion are sampled now, separately from the parsed axes and packed input word.
    trace::NetworkMovementRecord record{
        .turn = turn_context(),
        .axes = move.axes,
        .input_bits = move.input_bits,
        .primary_fire_trigger = native::read<std::uint32_t>(subject->owner, native::kPrimaryTriggerOffset),
        .secondary_fire_trigger = native::read<std::uint32_t>(subject->owner, native::kSecondaryTriggerOffset),
        .sprint_trigger = native::read<std::uint32_t>(subject->owner, native::kSprintTriggerOffset),
        .jump_trigger = native::read<std::uint32_t>(subject->owner, native::kJumpTriggerOffset),
        .state = native::read<std::int32_t>(subject->weapon, native::kStateOffset),
        .joystick = move.joystick,
        .kind = trace::NetworkMovementKind::Parsed,
        .changed = static_cast<std::uint8_t>(changed),
    };
    native::copy_floats(record.position, subject->soldier, native::kPositionOffset);
    native::copy_floats(record.velocity, subject->soldier, native::kVelocityOffset);

    write_subject(trace::RecordKind::NetworkMovement, record, *subject, trace::Authority);
}

} // namespace fc_plugins::hero_diagnostics
