#include "handler.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace fc_plugins::hero_diagnostics {

void Handler::plan_controls(fc::Plan &plan, const fc::FunctionLocation<ControlsFunction> &location) {
    plan.observe<ControlsInvocation>(location, fc::before([this](void *weapon, ControlsInvocation &call) noexcept {
                                         call.previous = thread_state_.controls;
                                         call.parent = current_scope().operation_scope;
                                         call.scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
                                         call.subject = snapshot_subject(bind(weapon));
                                         thread_state_.controls = &call;
                                         if (call.subject) {
                                             call.record = {.turn = turn_context(),
                                                            .state = native::read<std::int32_t>(weapon, native::kStateOffset),
                                                            .scope = call.scope,
                                                            .kind = trace::MovementKind::Controls};
                                             native::copy_floats(call.record.before, call.subject->owner, native::kControlOffset);
                                             call.record.parent_scope = call.parent;
                                             call.record.read_scope = thread_state_.read_scope;
                                         }
                                     }),
                                     fc::after([this](void *, bool result, const ControlsInvocation &call) noexcept {
                                         if (call.subject && subjects_.live(*call.subject)) {
                                             auto record = call.record;
                                             native::copy_floats(record.after, call.subject->owner, native::kControlOffset);
                                             native::copy_floats(record.position, call.subject->soldier, native::kPositionOffset);
                                             native::copy_floats(record.velocity, call.subject->soldier, native::kVelocityOffset);
                                             record.result = static_cast<std::uint8_t>(result);
                                             write_subject(trace::RecordKind::MovementFrame, record, *call.subject, trace::NativeCalled);
                                         }
                                         thread_state_.controls = call.previous;
                                     }));
}

void Handler::plan_causes(fc::Plan &plan, const DetailLayout &layout) {
    plan.observe<ControllerInvocation>(
        layout.controller,
        fc::before([this](const fc::NativeContext &, void *controller, float delta, ControllerInvocation &call) noexcept {
            call.previous = thread_state_.controller;
            call.controller = controller;
            call.parent = current_scope().operation_scope != 0 ? current_scope().operation_scope : thread_state_.predict_scope;
            call.scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
            call.delta = delta;
            const auto owner = native::read<std::uintptr_t>(controller, 4);
            call.subject = snapshot_subject(owner >= native::kControllableOffset
                                                ? subjects_.find_soldier(reinterpret_cast<void *>(owner - native::kControllableOffset))
                                                : nullptr);
            thread_state_.controller = &call;
            if (call.subject) {
                call.input = {.turn = turn_context(), .scope = call.scope, .parent_scope = call.parent, .delta = delta};
                const std::array<std::size_t, 4> offsets{native::kPrimaryTriggerOffset, native::kSecondaryTriggerOffset,
                                                         native::kSprintTriggerOffset, native::kJumpTriggerOffset};
                for (std::size_t index{}; index < offsets.size(); ++index) {
                    call.input.triggers_before[index] = native::read<std::uint32_t>(call.subject->owner, offsets[index]);
                }
            }
        }),
        fc::after([this](void *, float, const ControllerInvocation &call) noexcept {
            if (call.subject && subjects_.live(*call.subject)) {
                auto record = call.input;
                const std::array<std::size_t, 4> offsets{native::kPrimaryTriggerOffset, native::kSecondaryTriggerOffset,
                                                         native::kSprintTriggerOffset, native::kJumpTriggerOffset};
                for (std::size_t index{}; index < offsets.size(); ++index) {
                    record.triggers_after[index] = native::read<std::uint32_t>(call.subject->owner, offsets[index]);
                }
                write_subject(trace::RecordKind::InputUse, record, *call.subject, trace::End | trace::NativeCalled);
            }
            thread_state_.controller = call.previous;
        }));
    plan.observe_instruction(layout.controller_selected, fc::before([this](const fc::CpuContext &) noexcept {
                                 auto *call = thread_state_.controller;
                                 if (call == nullptr || !call->subject || !subjects_.live(*call->subject)) {
                                     return;
                                 }
                                 const auto input = native::read<const native::PlayerMove *>(call->controller, 0x88);
                                 const auto move = native::read<native::PlayerMove>(input);
                                 auto &record = call->input;
                                 record.axes = move.axes;
                                 record.input_bits = move.input_bits;
                                 record.row = native::pointer_id(input);
                                 record.joystick = move.joystick;
                                 record.selected = static_cast<std::uint8_t>(input != nullptr);
                                 record.source_turn = input != nullptr ? call->subject->input_turn : -1;
                                 record.origin = input != nullptr ? call->subject->input_origin : trace::InputOrigin::Unknown;
                                 // The copied prediction row has a qualified source turn; other client paths remain unknown.
                                 if (input != nullptr && role_ == fc::Role::Client && thread_state_.predict_scope != 0 &&
                                     turn_context().local_turn != 0) {
                                     record.source_turn = turn_context().predict_turn;
                                     record.origin = trace::InputOrigin::PredictCopied;
                                 }
                             }));
    if (settings_.capture == CaptureMode::Standard) {
        return;
    }
    plan.observe_instruction(layout.controller_reset, fc::before([this](const fc::CpuContext &) noexcept {
                                 const auto *call = thread_state_.controller;
                                 if (call && call->subject) {
                                     const OwnerDecision decision{.words = {native::read<std::uint32_t>(call->subject->owner, 0x4C),
                                                                            native::read<std::uint32_t>(call->subject->owner, 0x68)},
                                                                  .occurrence = call->scope,
                                                                  .kind = CauseKind::Controller,
                                                                  .stage = 2,
                                                                  .reason = 1};
                                     observe_owner_decision(call->subject->weapon, &decision);
                                 }
                             }));
    plan.observe_instruction(layout.forward_neutral, fc::before([this](const fc::CpuContext &context) noexcept {
                                 const auto *call = thread_state_.controller;
                                 if (call && call->subject && native::pointer_id(call->subject->owner) == context.ecx) {
                                     const OwnerDecision decision{.values = {call->delta},
                                                                  .words = {0x68, native::read<std::uint32_t>(call->subject->owner, 0x68),
                                                                            context.eax | 0x1CU, context.eip},
                                                                  .occurrence = call->scope,
                                                                  .kind = CauseKind::TriggerReset,
                                                                  .reason = 1};
                                     observe_owner_decision(call->subject->weapon, &decision);
                                 }
                             }));
    // Every invocation gets a stack marker, including untracked triggers nested under an enrolled controller.
    plan.observe_instruction(layout.trigger_entry, fc::before([this](const fc::CpuContext &context) noexcept {
                                 if (thread_state_.trigger_depth >= thread_state_.triggers.size()) {
                                     return;
                                 }
                                 auto &call = thread_state_.triggers[thread_state_.trigger_depth++];
                                 call = {};
                                 call.stack = static_cast<std::uint32_t>(fc::stack_pointer(context));
                                 const auto *controller = thread_state_.controller;
                                 if (controller == nullptr || !controller->subject) {
                                     return;
                                 }
                                 for (const std::uint32_t offset : {0x4CU, 0x68U, 0x44U, 0x3CU}) {
                                     if (context.ecx != native::pointer_id(controller->subject->owner) + offset) {
                                         continue;
                                     }
                                     call.subject = controller->subject;
                                     call.scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
                                     call.decision = {.values = {std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[1].low))},
                                                      .words = {offset, native::read<std::uint32_t>(controller->subject->owner, offset), 0,
                                                                native::read<std::uint8_t>(call.stack, 4),
                                                                native::read<std::uint32_t>(call.stack), controller->scope},
                                                      .occurrence = call.scope,
                                                      .kind = CauseKind::Trigger};
                                     if (settings_.capture == CaptureMode::Full) {
                                         observe_owner_decision(call.subject->weapon, &call.decision);
                                     }
                                     break;
                                 }
                             }));
    for (std::size_t exit{}; exit < layout.trigger_returns.size(); ++exit) {
        plan.observe_instruction(layout.trigger_returns[exit], fc::before([this, exit](const fc::CpuContext &context) noexcept {
                                     if (thread_state_.trigger_depth == 0) {
                                         return;
                                     }
                                     auto &call = thread_state_.triggers[thread_state_.trigger_depth - 1];
                                     // ESI and EBP are still saved on the stack at this epilogue probe.
                                     if (call.stack != fc::stack_pointer(context) + 8) {
                                         return;
                                     }
                                     if (call.subject && subjects_.live(*call.subject)) {
                                         call.decision.words[2] = native::read<std::uint32_t>(call.subject->owner, call.decision.words[0]);
                                         call.decision.words[6] = static_cast<std::uint32_t>(exit);
                                         call.decision.stage = 1;
                                         observe_owner_decision(call.subject->weapon, &call.decision);
                                     }
                                     call = {};
                                     --thread_state_.trigger_depth;
                                 }));
    }
    for (std::size_t stage{}; stage < layout.sprint_routes.size(); ++stage) {
        plan.observe_instruction(
            layout.sprint_routes[stage], fc::before([this, stage](const fc::CpuContext &context) noexcept {
                const auto *simulation = thread_state_.simulation;
                if (simulation == nullptr || !simulation->subject) {
                    return;
                }
                const auto &subject = *simulation->subject;
                const auto forward = native::read<std::uint32_t>(subject.owner, 0x68);
                if (stage == 0) {
                    thread_state_.sprint_route = (forward & 31U) == 0x13 ? 1 : 0;
                }
                if (stage == 1) {
                    thread_state_.sprint_route = 2;
                }
                const OwnerDecision decision{
                    .words = {forward, native::read<std::uint32_t>(subject.owner, 0x4C), thread_state_.sprint_route, context.eax,
                              context.eip, native::read<std::uint32_t>(subject.soldier, native::kEnergyFlagsOffset)},
                    .occurrence = simulation->scope,
                    .kind = CauseKind::SprintRoute,
                    .stage = static_cast<std::uint8_t>(stage),
                    .result = static_cast<std::uint8_t>(stage == 3 ? (context.eax & 0xFF) : ((forward & 31U) == 0x13))};
                observe_owner_decision(subject.weapon, &decision);
            }));
    }
    for (std::size_t stage{}; stage < layout.controls_gates.size(); ++stage) {
        plan.observe_instruction(
            layout.controls_gates[stage], fc::before([this, stage](const fc::CpuContext &context) noexcept {
                auto *call = thread_state_.controls;
                if (call == nullptr || !call->subject || !subjects_.live(*call->subject)) {
                    return;
                }
                const auto &subject = *call->subject;
                if (settings_.capture != CaptureMode::Full) {
                    const OwnerDecision decision{
                        .words = {static_cast<std::uint32_t>(native::read<std::int32_t>(subject.weapon, native::kStateOffset)),
                                  context.eflags, call->parent, thread_state_.read_scope},
                        .occurrence = call->scope,
                        .kind = CauseKind::Controls,
                        .stage = static_cast<std::uint8_t>(stage + 1),
                        .result = static_cast<std::uint8_t>(stage == 6   ? (context.eax & 0xFFU)
                                                            : stage == 8 ? ((context.eflags & 0x40U) != 0)
                                                                         : 0)};
                    if ((stage == 3 || stage == 7) && call->lock_compared) {
                        auto branch = decision;
                        branch.stage = 8;
                        branch.result = static_cast<std::uint8_t>((context.eflags & 0x41U) != 0);
                        observe_owner_decision(subject.weapon, &branch);
                        call->lock_compared = false;
                    }
                    if (stage == 1) {
                        call->lock_compared = true;
                    }
                    if (stage != 7) {
                        observe_owner_decision(subject.weapon, &decision);
                    }
                    if (stage == 2) {
                        for (const std::uint32_t offset : {0x4CU, 0x68U, 0x44U, 0x3CU}) {
                            if (context.eax == native::pointer_id(subject.owner) + offset) {
                                const OwnerDecision reset{.words = {offset, native::read<std::uint32_t>(subject.owner, offset),
                                                                    (context.ecx & 0xFFFFFFECU) | 0x0CU, context.eip},
                                                          .occurrence = call->scope,
                                                          .kind = CauseKind::TriggerReset,
                                                          .reason = 2};
                                observe_owner_decision(subject.weapon, &reset);
                            }
                        }
                    }
                    return;
                }
                const auto combo = native::read<std::int32_t>(subject.weapon, native::kStateOffset);
                const auto count = native::read<std::uint8_t>(subject.combo, native::kComboCountOffset);
                const auto rows = native::read<const std::byte *>(subject.combo, native::kComboRowsOffset);
                const auto *row = rows && combo >= 0 && combo < count ? rows + combo * native::kComboRowSize : nullptr;
                const auto lock = native::read<std::uint32_t>(row, 0x28);
                const OwnerDecision decision{
                    .values = {native::read<float>(subject.weapon, 0x188), static_cast<float>((lock >> 8) & 0x3FF) * 0.01F,
                               std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[1].low))},
                    .words = {native::pointer_id(row), static_cast<std::uint32_t>(combo), lock, native::read<std::uint32_t>(row, 0x24),
                              native::read<std::uint8_t>(row, 0x1D), native::read<std::uint32_t>(subject.weapon, 0xAC), context.eflags,
                              stage == 5 ? native::read<std::uint32_t>(context.eax, 0xAC) : context.eax, context.ecx, call->parent,
                              thread_state_.read_scope},
                    .occurrence = call->scope,
                    .kind = CauseKind::Controls,
                    .stage = static_cast<std::uint8_t>(stage + 1),
                    .result = static_cast<std::uint8_t>(stage == 6   ? (context.eax & 0xFF)
                                                        : stage == 8 ? ((context.eflags & 0x40) != 0)
                                                                     : 0)};
                // Both destinations preserve COMISS flags. The flag distinguishes this comparison from paths that skip it.
                if ((stage == 3 || stage == 7) && call->lock_compared) {
                    auto branch = decision;
                    branch.stage = 8;
                    branch.result = static_cast<std::uint8_t>((context.eflags & 0x41) != 0);
                    observe_owner_decision(subject.weapon, &branch);
                    call->lock_compared = false;
                }
                if (stage == 1) {
                    call->lock_compared = true;
                }
                if (stage != 7) {
                    observe_owner_decision(subject.weapon, &decision);
                }
                if (stage == 2) {
                    const auto address = context.eax;
                    for (const std::uint32_t offset : {0x4CU, 0x68U, 0x44U, 0x3CU}) {
                        if (address == native::pointer_id(subject.owner) + offset) {
                            const OwnerDecision reset{.words = {offset, native::read<std::uint32_t>(subject.owner, offset),
                                                                (context.ecx & 0xFFFFFFECU) | 0x0CU, context.eip},
                                                      .occurrence = call->scope,
                                                      .kind = CauseKind::TriggerReset,
                                                      .reason = 2};
                            observe_owner_decision(subject.weapon, &reset);
                        }
                    }
                }
            }));
    }
    for (std::size_t stage{}; stage < layout.automatic_decisions.size(); ++stage) {
        if (stage == 4 && role_ == fc::Role::Client) {
            continue;
        }
        plan.observe_instruction(
            layout.automatic_decisions[stage], fc::before([this, stage](const fc::CpuContext &context) noexcept {
                const auto *call = thread_state_.update;
                if (call == nullptr || !call->subject || !subjects_.live(*call->subject)) {
                    return;
                }
                const auto &subject = *call->subject;
                if (settings_.capture != CaptureMode::Full) {
                    const OwnerDecision decision{
                        .values = {native::read<float>(subject.weapon, 0x188),
                                   std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[1].low)),
                                   native::read<float>(context.ebp, -0x14), stage >= 2 ? native::read<float>(context.ebp, -0x10) : 0.0F},
                        .words = {native::read<std::uint32_t>(subject.weapon, 0x180), context.esi, context.edi, call->condition_ordinal,
                                  stage >= 2 ? native::read<std::uint32_t>(context.ebp, -0x24) : 0,
                                  thread_state_.condition.parent_scope == call->scope ? thread_state_.condition.scope : 0, context.eflags},
                        .occurrence = call->scope,
                        .kind = CauseKind::AutomaticTransition,
                        .stage = static_cast<std::uint8_t>(stage),
                        .result = static_cast<std::uint8_t>(stage == 0   ? (context.eflags & 1U)
                                                            : stage == 2 ? context.esi != 0
                                                                         : 1),
                        .reason = static_cast<std::uint8_t>(stage == 1   ? 1
                                                            : stage == 2 ? (context.esi == 0 ? 2 : 3)
                                                            : stage == 3 ? 4
                                                                         : 0)};
                    observe_owner_decision(subject.weapon, &decision);
                    return;
                }
                const auto row = native::read<std::uint32_t>(context.ebp - 8);
                const OwnerDecision decision{
                    .values = {native::read<float>(subject.weapon, 0x188),
                               std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[1].low)),
                               native::read<float>(context.ebp - 0x14), stage >= 2 ? native::read<float>(context.ebp - 0x10) : 0.0F,
                               call->record.effective_delta},
                    .words = {native::read<std::uint32_t>(subject.weapon, 0x180), context.esi, context.edi, row,
                              native::read<std::uint32_t>(row, 0x20), context.eflags, call->condition_ordinal,
                              stage >= 2 ? native::read<std::uint32_t>(context.ebp - 0x24) : 0,
                              stage >= 2 ? static_cast<std::uint32_t>(native::read<std::uint8_t>(context.ebp + 0xB)) : 0,
                              thread_state_.condition.parent_scope == call->scope ? thread_state_.condition.scope : 0, context.eip},
                    .occurrence = call->scope,
                    .kind = CauseKind::AutomaticTransition,
                    .stage = static_cast<std::uint8_t>(stage),
                    .result = static_cast<std::uint8_t>(stage == 0   ? (context.eflags & 1)
                                                        : stage == 2 ? (context.esi != 0)
                                                                     : 1),
                    .reason = static_cast<std::uint8_t>(stage == 1   ? 1
                                                        : stage == 2 ? (context.esi == 0 ? 2 : 3)
                                                        : stage == 3 ? 4
                                                                     : 0)};
                observe_owner_decision(subject.weapon, &decision);
            }));
    }
    if (role_ != fc::Role::Client) {
        return;
    }
    for (std::size_t lane{}; lane < layout.animation_entry.size(); ++lane) {
        plan.observe_instruction(layout.animation_entry[lane], fc::before([this, lane](const fc::CpuContext &context) noexcept {
                                     begin_animation_selection(context, lane);
                                 }));
        for (std::size_t path{}; path < 2; ++path) {
            plan.observe_instruction(
                layout.selector_returns[lane * 2 + path], fc::before([this, lane, path](const fc::CpuContext &context) noexcept {
                    auto &subject = thread_state_.selector_subjects[lane];
                    // EBP still identifies the native frame before the epilogue restores ESP.
                    if (!subject || thread_state_.selector_stack[lane] != context.ebp + 4 || !subjects_.live(*subject)) {
                        return;
                    }
                    const auto *animator = subject->animator;
                    const auto descriptor = native::read<void *>(animator, lane == 0 ? 0x1FC8 : 0x1FC4);
                    const auto metadata = native::read<void *>(descriptor, 8);
                    const OwnerDecision decision{
                        .values = {native::read<float>(animator, lane == 0 ? 0x1600 : 0x1FB0),
                                   native::read<float>(animator, lane == 0 ? 0x1FD0 : 0x1FCC),
                                   native::read<float>(animator, lane == 0 ? 0x1FE4 : 0x1FE0),
                                   native::read<float>(animator, lane == 0 ? 0x1FD8 : 0x1FD4), native::read<float>(animator, 0x1FDC)},
                        .words = {native::pointer_id(descriptor),
                                  native::pointer_id(native::read<void *>(animator, lane == 0 ? 0x15E4 : 0x1F94)),
                                  thread_state_.selector_descriptor[lane], native::read<std::uint32_t>(metadata),
                                  native::read<std::uint16_t>(animator, 0x201A), native::read<std::uint16_t>(animator, 0x201C),
                                  static_cast<std::uint32_t>(lane), thread_state_.selector_caller[lane], thread_state_.pose_scope,
                                  thread_state_.render ? thread_state_.render->scope : current_scope().operation_scope},
                        .occurrence = thread_state_.selector_scope[lane],
                        .kind = CauseKind::InstalledAnimation,
                        .stage = static_cast<std::uint8_t>(path),
                        .result = 1};
                    observe_owner_decision(subject->weapon, &decision);
                    subject.reset();
                }));
        }
    }
}

} // namespace fc_plugins::hero_diagnostics
