#include "handler.hpp"

#include "native.hpp"

namespace fc_plugins::hero_diagnostics {

void Handler::plan_server(fc::Plan &plan, const ServerLayout &layout) {
    plan_lifetime(plan, layout.lifetime);

    const auto observe_fill = [this, &plan](const fc::CodeLocation &location, trace::InputOrigin origin) {
        plan.observe_instruction(location, fc::before([this, origin](const fc::CpuContext &context) noexcept {
                                     // IsTurnReady owns both stack locals until its native fill loop returns.
                                     const auto player = native::read<std::int32_t>(context.ebp, -4);
                                     if (auto *subject = subjects_.find_player(player)) {
                                         subject->input_turn = native::read<std::int32_t>(context.ebp, -0xC);
                                         subject->input_origin = origin;
                                     }
                                 }));
    };
    observe_fill(layout.readiness_retained, trace::InputOrigin::ReadinessRetained);
    observe_fill(layout.readiness_default, trace::InputOrigin::ReadinessDefault);
    plan.observe_instruction(layout.readiness_publication, fc::before([this](const fc::CpuContext &context) noexcept {
                                 const auto player = native::read<std::int32_t>(context.ebp, -4);
                                 if (auto *subject = subjects_.find_player(player)) {
                                     const auto turn = *host_turn_;
                                     if (subject->input_turn != turn) {
                                         subject->input_origin = trace::InputOrigin::CurrentRow;
                                     }
                                     subject->input_turn = turn;
                                 }
                             }));
    // Late replay restores active joystick values using its independent fence cursor.
    // Retire readiness attribution on both sides rather than labeling it with today's host turn.
    plan.observe(layout.late_replay, fc::before([this]() noexcept {
                     subjects_.clear_input_origins();
                 }),
                 fc::after([this]() noexcept {
                     subjects_.clear_input_origins();
                 }));

    // The common invocation scope links server input, native predicates and nested effects to the same Update.
    plan.observe<UpdateInvocation>(layout.update, fc::before([this](void *weapon, float delta, UpdateInvocation &invocation) noexcept {
                                       begin_update(weapon, delta, delta, MeleeMode::Native, invocation);
                                       invocation.flags = trace::Authority;
                                       if (auto *subject = subjects_.find_weapon(weapon)) {
                                           record_server_move(*subject);
                                       }
                                   }),
                                   fc::after([this](void *weapon, float, bool result, const UpdateInvocation &invocation) noexcept {
                                       finish_update(weapon, result, invocation);
                                   }));

    // EnterState receives the requested state; ExitState samples the state being left.
    plan.observe<TransitionInvocation>(layout.enter_state,
                                       fc::before([this](void *weapon, std::int32_t state, TransitionInvocation &invocation) noexcept {
                                           begin_enter_state(weapon, state, invocation);
                                           invocation.flags = trace::Authority;
                                       }),
                                       fc::after([this](void *weapon, std::int32_t, const TransitionInvocation &invocation) noexcept {
                                           finish_transition(weapon, invocation);
                                       }));

    plan.observe<TransitionInvocation>(layout.exit_state, fc::before([this](void *weapon, TransitionInvocation &invocation) noexcept {
                                           invocation.subject = snapshot_subject(bind(weapon));
                                           if (!invocation.subject || !subjects_.live(*invocation.subject)) {
                                               return;
                                           }

                                           const auto state = native::read<std::int32_t>(weapon, native::kStateOffset);
                                           invocation.record = trace::TransitionRecord{
                                               .turn = turn_context(),
                                               .requested_state = state,
                                               .state_before = state,
                                               .state_after = -1,
                                               .previous_state = native::read<std::int32_t>(weapon, native::kPreviousStateOffset),
                                               .state_time_before = native::read<float>(weapon, native::kStateTimeOffset),
                                               .kind = trace::TransitionKind::Exit,
                                           };
                                           invocation.flags = trace::Authority;
                                       }),
                                       fc::after([this](void *weapon, const TransitionInvocation &invocation) noexcept {
                                           finish_transition(weapon, invocation);
                                       }));

    // The velocity override exposes forward/lateral/vertical speeds and the dimensionless override blend.
    if (settings_.capture != CaptureMode::Standard) {
        plan_controls(plan, layout.controls);
        plan_velocity(plan, layout.velocity);
    }

    // Write emits an entry snapshot after serialization; destination is the sampled native selection for this call.
    plan.observe<ReadInvocation>(
        layout.write, fc::before([this](void *weapon, void *stream, ReadInvocation &invocation) noexcept {
            invocation.subject = snapshot_subject(bind(weapon));
            if (!invocation.subject || !subjects_.live(*invocation.subject)) {
                return;
            }

            const auto state = native::read<std::int32_t>(weapon, native::kStateOffset);
            invocation.record = trace::NetworkStateRecord{
                .turn = turn_context(),
                .state_before = state,
                .requested_state = state,
                .state_after = state,
                .state_time_before = native::read<float>(weapon, native::kStateTimeOffset),
                .state_time_after = native::read<float>(weapon, native::kStateTimeOffset),

                .stream = native::pointer_id(stream),
                .action = native::read<std::uint16_t>(weapon, native::kActionOffset),
                .state_flags = native::read<std::uint8_t>(weapon, native::kStateFlagsOffset),
                .native_called = 1,
                .operation = trace::NetworkStateOperation::Write,
            };
            auto &record = invocation.record;
            record.mode_before = record.mode_after = native::read<std::int32_t>(weapon, native::kModeOffset);
            record.mode_time_before = record.mode_time_after = native::read<float>(weapon, native::kModeTimeOffset);
            record.input_time_before = record.input_time_after = native::read<float>(weapon, native::kInputTimeOffset);
            record.notified_time_before = record.notified_time_after = native::read<float>(weapon, native::kNotifiedStateTimeOffset);
        }),
        fc::after([this](void *, void *, const ReadInvocation &invocation) noexcept {
            if (invocation.subject && subjects_.live(*invocation.subject)) {
                write_subject(trace::RecordKind::NetworkState, invocation.record, *invocation.subject,
                              trace::Authority | trace::NativeCalled);
            }
        }));

    if (settings_.capture != CaptureMode::Standard) {
        // Combat capture samples projectile deflection operands and the hook's returned bool.
        plan_contact(plan, layout.contact);
    }
}

void Handler::record_server_move(HeroSubject &subject) noexcept {
    if (active_inputs_ == nullptr || subject.player < 0) {
        return;
    }

    // IsTurnReady and late replay prepare this native source before controller/soldier simulation.
    const auto context = turn_context();
    const auto *input = active_inputs_[static_cast<std::size_t>(subject.player)];
    if (input == nullptr) {
        return;
    }
    const auto move = *input;
    const auto changed = !subject.consumed_move_seen || move != subject.last_consumed_move;
    subject.last_consumed_move = move;
    subject.consumed_move_seen = true;

    const trace::PreparedUseRecord record{.turn = context,
                                          .axes = move.axes,
                                          .input_bits = move.input_bits,
                                          .row = native::pointer_id(input),
                                          .scope = thread_state_.update ? thread_state_.update->scope : 0,
                                          .source_turn = subject.input_turn,
                                          .origin = subject.input_origin,
                                          .joystick = move.joystick,
                                          .changed = static_cast<std::uint8_t>(changed)};
    write_subject(trace::RecordKind::PreparedUse, record, subject, trace::Authority);
}

} // namespace fc_plugins::hero_diagnostics
