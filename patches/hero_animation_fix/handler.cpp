#include "handler.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <bit>
#include <cstddef>
#include <expected>
#include <format>
#include <utility>

namespace fc_plugins::hero_animation_fix {
namespace {

// Qualified retail offsets: owner addresses EntitySoldier's embedded Controllable.
constexpr std::size_t kWeaponOwnerOffset = 0x6C;
constexpr std::size_t kComboOffset = 0x114;
constexpr std::size_t kComboStateOffset = 0x180;
constexpr std::size_t kSoldierControllableOffset = 0x240;
constexpr std::size_t kPlayerIdOffset = 0xD4;
constexpr std::size_t kAliveFlagsOffset = 0x1FC;
constexpr std::uint8_t kAliveFlag = 1U << 3;
constexpr std::size_t kWeaponArrayOffset = 0x720;
constexpr std::size_t kPackedSelectionOffset = 0x742;
constexpr int kWeaponSlotCount = 8;

// Checks the packet's five-bit combo ID range, independently of asset contents.
constexpr bool valid_state(int state) noexcept {
    return state >= 0 && state < 32;
}

// Reconciliation and passive diagnostics preserve the game's thread-local errors.
struct AmbientErrors {
    DWORD win32{};
    int winsock{};

    // Captures both native error values before an observation.
    static AmbientErrors capture() noexcept {
        return {.win32 = GetLastError(), .winsock = WSAGetLastError()};
    }

    // Win32 is restored last because Winsock can share its backing error storage.
    void restore() const noexcept {
        WSASetLastError(winsock);
        SetLastError(win32);
    }
};

} // namespace

void Handler::plan(fc::Plan &plan) {
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }
    get_joystick_index_ = plan.function(layout->get_joystick_index);
    turn_duration_memory_ = plan.access(layout->turn_duration);
    prediction_turn_memory_ = plan.access(layout->prediction_turn);
    acknowledged_turn_memory_ = plan.access(layout->acknowledged_turn);
    update_turn_memory_ = plan.access(layout->update_turn);
    local_prediction_pass_memory_ = plan.access(layout->local_prediction_pass);
    network_enabled_memory_ = plan.access(layout->network_enabled);
    network_client_active_memory_ = plan.access(layout->network_client_active);

    plan.observe(layout->predict_turn, fc::before([this]() noexcept {
                     const auto errors = AmbientErrors::capture();
                     before_predict_turn();
                     errors.restore();
                 }));
    plan.observe_instruction(layout->completed_turn, fc::before([this](const fc::CpuContext &) noexcept {
                                 const auto errors = AmbientErrors::capture();
                                 after_predict_turn();
                                 errors.restore();
                             }));
    plan.observe_instruction(layout->fallback, fc::before([this](const fc::CpuContext &) noexcept {
                                 fallback_pending_ = true;
                             }));
    collision_return_memory_ =
        plan.access<const std::byte>(plan.at(layout->collision_return.rva, layout->collision_return.name),
                                     layout->collision_return.evidence.bytes.size(), layout->collision_return.evidence);
    plan.hook_instruction(layout->collision_start, [this](fc::CpuContext &context) noexcept {
        const bool bypass = historical_update(reinterpret_cast<void *>(context.edi));
        const hero_diagnostics::OwnerDecision decision{.words = {context.eip, reinterpret_cast<std::uint32_t>(collision_return_)},
                                                       .kind = hero_diagnostics::CauseKind::NativeBypass,
                                                       .stage = 0,
                                                       .result = static_cast<std::uint8_t>(bypass)};
        owner_decision(reinterpret_cast<void *>(context.edi), decision);
        if (bypass) {
            // The SDK preserves the native frame and registers; the selected epilogue returns true.
            context.eip = reinterpret_cast<std::uint32_t>(collision_return_);
        }
    });
    effects_timer_memory_ = plan.access<const std::byte>(plan.at(layout->effects_timer.rva, layout->effects_timer.name),
                                                         layout->effects_timer.evidence.bytes.size(), layout->effects_timer.evidence);
    plan.hook_instruction(layout->effects_start, [this](fc::CpuContext &context) noexcept {
        const bool bypass = historical_update(reinterpret_cast<void *>(context.ebx));
        const hero_diagnostics::OwnerDecision decision{.words = {context.eip, reinterpret_cast<std::uint32_t>(effects_timer_)},
                                                       .kind = hero_diagnostics::CauseKind::NativeBypass,
                                                       .stage = 1,
                                                       .result = static_cast<std::uint8_t>(bypass)};
        owner_decision(reinterpret_cast<void *>(context.ebx), decision);
        if (bypass) {
            context.eip = reinterpret_cast<std::uint32_t>(effects_timer_);
        }
    });

    // Restore before the base reader mutates generic mode/time, then retain every native authority setter.
    read_ = plan.hook(layout->read, [this](void *weapon, void *packet) noexcept {
        const auto incoming = AmbientErrors::capture();
        ReadWindow window{.weapon = weapon, .previous = read_window_};
        bool outer = true;
        for (auto *read = read_window_; read != nullptr; read = read->previous) {
            if (read->weapon == weapon) {
                outer = false;
                break;
            }
        }
        read_window_ = &window;
        bool ready{};
        const bool active = outer && network_prediction_active();
        if (active) {
            window.identity = capture_identity(weapon, ready);
        }
        const auto turn = ready ? *update_turn_ : -1;
        bool restored{};
        ContactScope contacts;
        auto *previous_contacts = contact_scope_;
        LiveSoundState sound;
        if (ready) {
            sound = LiveSoundState::capture(weapon);
            contacts.begin(weapon, previous_contacts);
            contact_decision(weapon, contacts, 0);
            contact_scope_ = &contacts;
            replay_sample(weapon, window.identity, turn, 3, false);
            const auto state = replay_history_.restore(window.identity, turn);
            lock_decision(weapon, turn, 0, state.has_value());
            restored = state.has_value();
            if (state) {
                state->restore(weapon, true, queue_timing());
            }
            lock_decision(weapon, turn, 1, state.has_value());
            replay_sample(weapon, window.identity, turn, 4, state.has_value());
        } else if (active) {
            clear_weapon(reinterpret_cast<std::uintptr_t>(weapon), hero_diagnostics::HistoryReason::NotReady);
        }
        incoming.restore();
        const auto result = read_.call(weapon, packet);
        const auto errors = AmbientErrors::capture();
        if (ready) {
            // ClearInput may retire continuity inside Read; destruction also disarms the contact scope.
            if (window.weapon != nullptr) {
                sound.restore(weapon);
            }
            if (result && restored && window.identity.valid()) {
                replay_history_.capture(window.identity, turn, ReplayState::capture(weapon, queue_timing()));
                replay_sample(weapon, window.identity, turn, 5, true);
            }
            contacts.finish();
            contact_decision(weapon, contacts, 1);
            contact_scope_ = previous_contacts;
        }
        if (result) {
            jump_history_.rewind(reinterpret_cast<std::uintptr_t>(weapon));
            presentation_history_.rewind(reinterpret_cast<std::uintptr_t>(weapon));
        } else {
            clear_weapon(reinterpret_cast<std::uintptr_t>(weapon), hero_diagnostics::HistoryReason::CallFailed);
        }
        read_window_ = window.previous;
        errors.restore();
    });
    update_ = plan.hook(layout->update, [this](void *weapon, float delta) noexcept {
        const auto incoming = AmbientErrors::capture();
        UpdateWindow window{.weapon = weapon, .previous = update_window_};
        update_window_ = &window;
        bool ready{};
        const bool prediction_active = network_prediction_active();
        if (prediction_active) {
            window.identity = capture_identity(weapon, ready);
            window.frontier = ready && *local_prediction_pass_ != 0;
            window.historical = ready && !window.frontier;
        }
        if (prediction_active && !ready) {
            clear_weapon(reinterpret_cast<std::uintptr_t>(weapon), hero_diagnostics::HistoryReason::NotReady);
        }
        if (const auto *output = diagnostics()) {
            output->update_decision(weapon, delta, delta, hero_diagnostics::MeleeUpdateMode::Native);
        }
        ContactScope contacts;
        auto *previous_contacts = contact_scope_;
        const bool isolated = window.historical;
        if (isolated) {
            contacts.begin(weapon, previous_contacts);
            contact_decision(weapon, contacts, 0);
            contact_scope_ = &contacts;
        }
        incoming.restore();
        const auto result = update_.call(weapon, delta);
        const auto outgoing = AmbientErrors::capture();
        if (isolated) {
            contacts.finish();
            contact_decision(weapon, contacts, 1);
            contact_scope_ = previous_contacts;
        }
        if (result && *result && window.identity.valid() && *prediction_turn_ == observed_prediction_turn_) {
            replay_history_.advanced(window.identity, *prediction_turn_);
        }
        if (!result || !*result) {
            clear_weapon(reinterpret_cast<std::uintptr_t>(weapon), hero_diagnostics::HistoryReason::CallFailed);
        }
        update_window_ = window.previous;
        outgoing.restore();
        return result.value_or(false);
    });
    set_network_state_ = plan.hook(layout->set_network_state, [this](void *weapon, std::int32_t state, bool flag) noexcept {
        const auto incoming = AmbientErrors::capture();
        AuthorityWindow window{.weapon = weapon, .previous = authority_window_};
        authority_window_ = &window;
        bool ready{};
        if (network_prediction_active()) {
            window.identity = capture_identity(weapon, ready);
        }
        if (ready && valid_state(state)) {
            const auto &identity = window.identity;
            const auto before = identity.is_local() ? local_history_.snapshot(identity) : remote_history_.snapshot(identity);
            // Native weapon fields may already be rewound. Classification refers to the recorded presentation path.
            const int frontier = before.last_state >= 0 ? before.last_state : fc::read_field<int>(weapon, kComboStateOffset);
            hero_diagnostics::HistoryDecision decision;
            const auto turn = identity.is_local() ? *acknowledged_turn_ : *update_turn_;
            const auto action = identity.is_local() ? local_history_.classify_authority(identity, frontier, state, turn, &decision)
                                                    : remote_history_.classify_authority(identity, frontier, state, turn, &decision);
            // Native movement authority applies below; presentation follows the combo occurrence matched at this turn.
            std::uint32_t cardinality{};
            const bool attempted = action != AuthorityAction::Corrected && decision.selected_prefix != 0;
            if (attempted) {
                window.entry = presentation_history_.confirmed(identity, decision.selected_turn, state, &cardinality);
            }
            window.entry = presentation_history_.authority_entry(
                identity, *update_turn_, fc::read_field<int>(weapon, kComboStateOffset), state, fc::read_field<std::int32_t>(weapon, 0xB0),
                fc::read_field<float>(weapon, 0xB4), fc::read_field<int>(weapon, kComboStateOffset) != state, *turn_duration_,
                window.entry);
            const auto matched = window.entry.value_or(PresentationEntry{});
            const hero_diagnostics::OwnerDecision lookup{
                .values = {fc::read_field<float>(weapon, 0xB4), *turn_duration_,
                           static_cast<float>(fc::read_field<std::int32_t>(weapon, 0xB0))},
                .words = {static_cast<std::uint32_t>(decision.selected_turn), static_cast<std::uint32_t>(state), cardinality,
                          static_cast<std::uint32_t>(attempted), static_cast<std::uint32_t>(matched.turn), matched.order,
                          static_cast<std::uint32_t>(matched.source), static_cast<std::uint32_t>(matched.target),
                          static_cast<std::uint32_t>(action), static_cast<std::uint32_t>(identity.is_remote()),
                          static_cast<std::uint32_t>(matched.authoritative), static_cast<std::uint32_t>(*update_turn_)},
                .kind = hero_diagnostics::CauseKind::PresentationLookup,
                .result = static_cast<std::uint8_t>(window.entry.has_value())};
            owner_decision(weapon, lookup);
            presentation_history_.authority(identity, action, window.entry);
            decision.before = before;
            decision.after = identity.is_local() ? local_history_.snapshot(identity) : remote_history_.snapshot(identity);
            decision.turn = turn;
            decision.source = frontier;
            decision.target = state;
            decision.operation = hero_diagnostics::HistoryOperation::Authority;
            decision.reason = action == AuthorityAction::Corrected    ? hero_diagnostics::HistoryReason::Corrected
                              : action == AuthorityAction::Historical ? hero_diagnostics::HistoryReason::Matched
                                                                      : hero_diagnostics::HistoryReason::Untracked;
            decision.result = static_cast<std::uint8_t>(action);
            decision.remote = static_cast<std::uint8_t>(identity.is_remote());
            if (const auto *output = diagnostics()) {
                output->history_decision(weapon, &decision);
            }
        } else {
            clear_weapon(reinterpret_cast<std::uintptr_t>(weapon), hero_diagnostics::HistoryReason::NotReady);
        }
        if (const auto *output = diagnostics()) {
            output->network_state_decision(weapon, state, flag ? 1U : 0U, hero_diagnostics::NetworkStateMode::Native);
        }
        incoming.restore();
        const auto result = set_network_state_.call(weapon, state, flag);
        const auto outgoing = AmbientErrors::capture();
        if (!result) {
            clear_weapon(reinterpret_cast<std::uintptr_t>(weapon), hero_diagnostics::HistoryReason::CallFailed);
        }
        authority_window_ = window.previous;
        outgoing.restore();
    });
    plan.observe(layout->enter_state, fc::before([this](void *weapon, std::int32_t state) noexcept {
                     const auto errors = AmbientErrors::capture();
                     observe_enter_state(weapon, state);
                     errors.restore();
                 }));
    for (const auto &[site, reason] : {std::pair{&layout->clear_input, hero_diagnostics::HistoryReason::ClearInput},
                                       std::pair{&layout->destroy, hero_diagnostics::HistoryReason::Destroy}}) {
        plan.observe(*site, fc::before([this, reason](void *weapon) noexcept {
            const auto errors = AmbientErrors::capture();
            if (reason == hero_diagnostics::HistoryReason::Destroy) {
                if (contact_scope_ != nullptr) {
                    contact_scope_->destroy(weapon);
                    contact_decision(weapon, *contact_scope_, 2);
                }
                for (auto *window = read_window_; window != nullptr; window = window->previous) {
                    if (window->weapon == weapon) {
                        window->weapon = nullptr;
                    }
                }
                for (auto *window = update_window_; window != nullptr; window = window->previous) {
                    if (window->weapon == weapon) {
                        window->historical = false;
                    }
                }
            }
            clear_weapon(reinterpret_cast<std::uintptr_t>(weapon), reason);
            errors.restore();
        }));
    }
    for (const auto *site : {&layout->prediction_pass_context, &layout->authoritative_pass_context, &layout->acknowledged_turn_context,
                             &layout->network_state_context}) {
        plan.check(*site, site->evidence.bytes.size());
    }

    // Each airborne saber remains visible until its own catch, even when another projectile shares its throw weapon.
    ordnance_render_ =
        plan.hook(layout->ordnance_render, [this](void *renderable, std::uint8_t view, float time, std::uint32_t flags) noexcept {
            if (fc::read_field<std::uint8_t>(renderable, kOrdnanceCaughtOffset) != 0) {
                return;
            }

            // Keep the native owner and weapon checks before changing the shared flags.
            const auto *owner = fc::read_field<const void *>(renderable, kOrdnanceOwnerOffset);
            if (owner != nullptr && fc::read_field<std::uint32_t>(owner, kOwnerGenerationOffset) !=
                                        fc::read_field<std::uint32_t>(renderable, kOrdnanceGenerationOffset)) {
                return;
            }
            auto *throw_weapon = fc::read_field<void *>(renderable, kOrdnanceThrowWeaponOffset);
            if (throw_weapon == nullptr || fc::read_field<const void *>(throw_weapon, kThrowPrimaryWeaponOffset) == nullptr) {
                return;
            }

            // Another projectile's catch must not hide this airborne draw. Held rendering sees the restored shared flags.
            const auto saved_flags = fc::read_field<std::uint32_t>(throw_weapon, kThrowFlagsOffset);
            fc::write_field(throw_weapon, kThrowFlagsOffset, saved_flags & ~kThrowReturnedFlag);
            static_cast<void>(ordnance_render_.call(renderable, view, time, flags));
            fc::write_field(throw_weapon, kThrowFlagsOffset, saved_flags);
        });

    // Render supplies animator requests; retention changes these arguments, never weapon dispatch or animation clocks.
    animator_state_ = plan.hook(layout->animator_state, [this](void *animator, std::int32_t mode, bool restart, std::uint8_t primary,
                                                               std::uint8_t secondary, bool full_body) noexcept {
        const auto incoming = AmbientErrors::capture();
        AnimationRequest request{mode, restart, primary, secondary, full_body};
        if (animator != nullptr && network_prediction_active()) {
            const auto identity = soldier_identity(animator_soldier(animator));
            if (identity.valid()) {
                const auto before = presentation_history_.snapshot(identity);
                const auto requested = request;
                request = presentation_history_.animation(identity, request);
                if (requested != before.requested || requested != request) {
                    animation_decision(identity, requested, 0);
                    animation_decision(identity, request, 1);
                }
            }
        }
        incoming.restore();
        static_cast<void>(
            animator_state_.call(animator, request.mode, request.restart, request.primary, request.secondary, request.full_body));
    });
    fire_audio_ = plan.hook(layout->fire_audio, [this](void *weapon, bool secondary) noexcept {
        const auto errors = AmbientErrors::capture();
        std::uint32_t order{};
        const auto kind = static_cast<std::uint8_t>(secondary ? 5 : 1);
        const bool suppressed = suppress_audio(weapon, kind, order);
        const bool melee = weapon != nullptr && fc::read_field<std::uint32_t>(weapon) == fc::read_field<std::uint32_t>(melee_vtable_);
        const auto ambient = melee ? AmbientAssignment::capture(weapon, secondary) : AmbientAssignment{};
        errors.restore();
        if (!suppressed) {
            const auto result = fire_audio_.call(weapon, secondary);
            const auto outgoing = AmbientErrors::capture();
            if (melee && secondary) {
                const auto descriptor = fc::read_field<std::uint32_t>(weapon, 0xD8);
                const auto type = fc::read_field<std::uint8_t>(weapon, 0xDC);
                const auto controller = fc::read_field<std::uint16_t>(weapon, 0xCC);
                const auto flags = fc::read_field<std::uint8_t>(weapon, 0xCE);
                const bool restored = result && ambient.restore(weapon);
                if (diagnostics() != nullptr) {
                    const auto *properties =
                        diagnostic_capture_ == hero_diagnostics::CaptureMode::Full ? fc::read_field<void *>(weapon, 0x64) : nullptr;
                    const hero_diagnostics::OwnerDecision decision{
                        .values = {static_cast<float>(ambient.desired_mask),
                                   static_cast<float>(fc::read_field<std::uint8_t>(weapon, 0x17C) & 0xF0U),
                                   static_cast<float>(fc::read_field<std::uint8_t>(weapon, 0x179))},
                        .words = {ambient.descriptor, ambient.type, descriptor, type, fc::read_field<std::uint32_t>(weapon, 0xD8),
                                  fc::read_field<std::uint8_t>(weapon, 0xDC),
                                  ambient.controller | (static_cast<std::uint32_t>(ambient.controller_flags) << 16U),
                                  controller | (static_cast<std::uint32_t>(flags) << 16U),
                                  properties != nullptr ? fc::read_field<std::uint32_t>(properties, 0x264) : 0U,
                                  properties != nullptr ? fc::read_field<std::uint8_t>(properties, 0x268) : 0U,
                                  properties != nullptr ? fc::read_field<std::uint32_t>(properties, 0x120) : 0U,
                                  properties != nullptr ? fc::read_field<std::uint8_t>(properties, 0x124) : 0U},
                        .kind = hero_diagnostics::CauseKind::AmbientAssignment,
                        .result = static_cast<std::uint8_t>(restored),
                        .reason = static_cast<std::uint8_t>(ambient.qualified)};
                    owner_decision(weapon, decision);
                }
            }
            if (result) {
                record_audio(weapon, kind, order);
            } else {
                audio_decision(weapon, kind, order, 0, 1, false);
            }
            outgoing.restore();
        }
    });
    for (std::size_t index{}; index < combo_audio_.size(); ++index) {
        combo_audio_[index] = plan.hook_call<ComboAudioFunction>(
            plan.at(layout->combo_audio[index].rva, layout->combo_audio[index].name),
            [this, index](void *sound, const void *position, const void *velocity, std::uint32_t reserved, bool force) noexcept {
                const auto incoming = AmbientErrors::capture();
                void *weapon = authority_window_ != nullptr ? authority_window_->weapon
                               : update_window_ != nullptr  ? update_window_->weapon
                                                            : nullptr;
                std::uint32_t order{};
                const auto kind = static_cast<std::uint8_t>(2 + index);
                const auto variant = sound != nullptr ? fc::read_field<std::uint32_t>(sound) : 0U;
                const bool suppressed = suppress_audio(weapon, kind, order, variant);
                incoming.restore();
                if (!suppressed) {
                    const auto result = combo_audio_[index].call(sound, position, velocity, reserved, force);
                    const auto outgoing = AmbientErrors::capture();
                    if (result) {
                        record_audio(weapon, kind, order, variant);
                    } else {
                        audio_decision(weapon, kind, order, variant, 1, false);
                    }
                    outgoing.restore();
                }
            },
            layout->combo_audio[index].evidence);
    }
    plan_jump_presentation(plan, *layout);
}

void Handler::plan_jump_presentation(fc::Plan &plan, const Layout &layout) {
    melee_vtable_memory_ = plan.access(layout.melee_vtable);
    plan.observe<GaitReadWindow>(layout.soldier_read, fc::before([this](void *soldier, void *, void *, GaitReadWindow &window) noexcept {
                                     window = {.previous = gait_read_window_, .soldier = soldier};
                                     gait_read_window_ = &window;
                                 }),
                                 fc::after([this](void *, void *, void *, const GaitReadWindow &window) noexcept {
                                     gait_read_window_ = window.previous;
                                 }));
    plan.observe<GaitWindow>(layout.gait, fc::before([this](void *soldier, float delta, GaitWindow &window) noexcept {
                                 const auto errors = AmbientErrors::capture();
                                 window.previous = gait_window_;
                                 gait_window_ = &window;
                                 if (network_prediction_active()) {
                                     const auto identity = soldier_identity(soldier);
                                     for (auto *read = gait_read_window_; read != nullptr; read = read->previous) {
                                         window.reading |= read->soldier == soldier;
                                     }
                                     if (identity.is_local() && (window.reading || *local_prediction_pass_ == 0)) {
                                         window.identity = identity;
                                         window.live = LiveGaitState::capture(soldier);
                                         window.delta = delta;
                                         gait_decision(window, window.reading ? 2 : 0, false);
                                     }
                                 }
                                 errors.restore();
                             }),
                             fc::after([this](void *soldier, float, const GaitWindow &window) noexcept {
                                 const auto errors = AmbientErrors::capture();
                                 if (window.identity.valid()) {
                                     gait_decision(window, window.reading ? 3 : 1, false);
                                     window.live.restore(soldier);
                                     gait_decision(window, window.reading ? 3 : 1, true);
                                 }
                                 gait_window_ = window.previous;
                                 errors.restore();
                             }));
    plan.observe<SimulationWindow>(layout.soldier_update, fc::before([this](void *controllable, float, SimulationWindow &window) noexcept {
                                       const auto errors = AmbientErrors::capture();
                                       window.previous = simulation_window_;
                                       window.controllable = controllable;
                                       if (network_prediction_active()) {
                                           window.turn = *prediction_turn_;
                                       }
                                       simulation_window_ = &window;
                                       errors.restore();
                                   }),
                                   fc::after([this](void *, float, bool, const SimulationWindow &window) noexcept {
                                       simulation_window_ = window.previous;
                                   }));

    plan.observe<JumpInvocation>(
        layout.jump_restart, fc::before([this](void *animator, bool, JumpInvocation &invocation) noexcept {
            const auto errors = AmbientErrors::capture();
            auto *window = simulation_window_;
            if (animator != nullptr && window != nullptr && window->turn >= 0) {
                auto *soldier = animator_soldier(animator);
                if (soldier != nullptr && static_cast<std::byte *>(soldier) + kSoldierControllableOffset == window->controllable) {
                    invocation.identity = soldier_identity(soldier);
                    invocation.turn = window->turn;
                    invocation.order = jump_history_.next_order(invocation.identity, invocation.turn);
                    invocation.action = fc::read_field<std::int32_t>(animator, 0x1FEC);
                    invocation.timer = fc::read_field<float>(animator, 0x1FF0);
                    invocation.historical = *local_prediction_pass_ == 0;
                }
            }
            errors.restore();
        }),
        fc::after([this](void *animator, bool, bool result, const JumpInvocation &invocation) noexcept {
            const auto errors = AmbientErrors::capture();
            if (result && invocation.identity.valid() && network_prediction_active() &&
                soldier_identity(animator_soldier(animator)) == invocation.identity) {
                const auto action = fc::read_field<std::int32_t>(animator, 0x1FEC);
                const bool presented = jump_history_.already_presented(invocation.identity, invocation.turn, invocation.order, action,
                                                                       invocation.action == action);
                const float native_timer = fc::read_field<float>(animator, 0x1FF0);
                const bool preserve =
                    preserve_jump_clock(animator, invocation.action, invocation.timer, result, invocation.historical, presented);
                const hero_diagnostics::OwnerDecision clock{
                    .values = {invocation.timer, native_timer, fc::read_field<float>(animator, 0x1FF0)},
                    .words = {static_cast<std::uint32_t>(invocation.turn), invocation.order, static_cast<std::uint32_t>(invocation.action),
                              static_cast<std::uint32_t>(action), static_cast<std::uint32_t>(invocation.historical),
                              static_cast<std::uint32_t>(presented)},
                    .kind = hero_diagnostics::CauseKind::JumpClock,
                    .result = static_cast<std::uint8_t>(preserve)};
                owner_decision(reinterpret_cast<void *>(invocation.identity.weapon), clock);
                const hero_diagnostics::OwnerDecision decision{
                    .words = {static_cast<std::uint32_t>(invocation.turn), invocation.order, static_cast<std::uint32_t>(invocation.action),
                              static_cast<std::uint32_t>(action), static_cast<std::uint32_t>(invocation.action == action)},
                    .kind = hero_diagnostics::CauseKind::JumpReset,
                    .stage = 0,
                    .result = static_cast<std::uint8_t>(presented)};
                owner_decision(reinterpret_cast<void *>(invocation.identity.weapon), decision);
            }
            errors.restore();
        }));

    plan.observe<AnimationWindow>(
        layout.animation_update,
        fc::before([this](void *animator, std::int32_t action, float, std::uint32_t, bool, AnimationWindow &window) noexcept {
            const auto errors = AmbientErrors::capture();
            window = {.previous = animation_window_, .animator = animator, .action = action};
            animation_window_ = &window;
            if (animator != nullptr && network_prediction_active()) {
                const auto identity = soldier_identity(animator_soldier(animator));
                const auto pending = fc::read_field<std::uint16_t>(animator, 0x201C);
                window.reset_lanes = jump_history_.consume(identity, action, (pending & 4U) != 0);
                if (identity.valid()) {
                    const hero_diagnostics::OwnerDecision decision{
                        .words = {static_cast<std::uint32_t>(action), pending, window.reset_lanes},
                        .kind = hero_diagnostics::CauseKind::JumpReset,
                        .stage = 1};
                    owner_decision(reinterpret_cast<void *>(identity.weapon), decision);
                }
            }
            errors.restore();
        }),
        fc::after([this](void *, std::int32_t, float, std::uint32_t, bool, bool, const AnimationWindow &window) noexcept {
            animation_window_ = window.previous;
        }));

    for (std::size_t index{}; index < layout.animation_selection.size(); ++index) {
        // Diagnostics observes the same prologue as an instruction site, preserving hidden XMM1 arguments.
        plan.observe_instruction(layout.animation_selection[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                                     auto *animator = reinterpret_cast<void *>(context.ecx);
                                     if (animation_window_ != nullptr && animation_window_->animator == animator &&
                                         (animation_window_->reset_lanes & (1U << index)) == 0) {
                                         const auto flags = fc::read_field<std::uint16_t>(animator, 0x201A);
                                         const auto after = static_cast<std::uint16_t>(flags & ~(0x1000U << index));
                                         fc::write_field(animator, 0x201A, after);
                                         const auto identity = soldier_identity(animator_soldier(animator));
                                         if (identity.valid()) {
                                             const hero_diagnostics::OwnerDecision decision{
                                                 .words = {flags, after, static_cast<std::uint32_t>(index), animation_window_->reset_lanes},
                                                 .kind = hero_diagnostics::CauseKind::JumpReset,
                                                 .stage = 2,
                                                 .result = static_cast<std::uint8_t>(flags != after)};
                                             owner_decision(reinterpret_cast<void *>(identity.weapon), decision);
                                         }
                                     }
                                 }));
        for (const auto &[site, installed] :
             {std::pair{&layout.animation_installed[index], true}, std::pair{&layout.animation_unchanged[index], false}}) {
            plan.observe_instruction(*site, fc::before([this, index, installed](const fc::CpuContext &context) noexcept {
                const auto errors = AmbientErrors::capture();
                auto *animator = reinterpret_cast<void *>(context.edi);
                if (network_prediction_active()) {
                    // Both native paths select a valid clip; descriptor failures never reach these sites.
                    const auto identity = soldier_identity(animator_soldier(animator));
                    if (identity.valid()) {
                        const auto lane = static_cast<std::uint8_t>(1U << index);
                        // Ground and jump updaters share these selectors; only jump resets require its update window.
                        presentation_history_.present_animation(identity, lane, true);
                        const auto snapshot = presentation_history_.snapshot(identity);
                        animation_decision(identity, snapshot.requested, installed ? 2 : 3, lane, true);
                        if (installed && animation_window_ != nullptr && animation_window_->animator == animator) {
                            jump_history_.present(identity, lane, animation_window_->action, true, true);
                        }
                    }
                }
                errors.restore();
            }));
        }
    }

    for (const auto *location : {&layout.soldier_reset, &layout.soldier_destroy}) {
        plan.observe(*location, fc::before([this](void *soldier) noexcept {
            const auto errors = AmbientErrors::capture();
            // Reset/destruction can replace a soldier before its selected weapon reports destruction.
            jump_history_.clear_owner(reinterpret_cast<std::uintptr_t>(soldier) + kSoldierControllableOffset);
            for (int index{}; index < kWeaponSlotCount; ++index) {
                const auto weapon = fc::read_field<std::uintptr_t>(soldier, kWeaponArrayOffset + index * sizeof(void *));
                if (weapon != 0) {
                    clear_weapon(weapon, hero_diagnostics::HistoryReason::Destroy);
                }
            }
            errors.restore();
        }));
    }
}

HeroIdentity Handler::soldier_identity(void *soldier) noexcept {
    if (soldier == nullptr) {
        return {};
    }

    const auto packed = fc::read_field<std::uint8_t>(soldier, kPackedSelectionOffset);
    const int index = static_cast<int>(packed & 7U);
    if ((packed & 8U) != 0) {
        return {};
    }
    auto *weapon = fc::read_field<void *>(soldier, kWeaponArrayOffset + static_cast<std::size_t>(index) * sizeof(void *));
    if (weapon == nullptr || fc::read_field<std::uint32_t>(weapon) != fc::read_field<std::uint32_t>(melee_vtable_)) {
        return {};
    }

    bool ready{};
    const auto identity = capture_identity(weapon, ready);
    return ready && identity.owner == reinterpret_cast<std::uintptr_t>(soldier) + kSoldierControllableOffset ? identity : HeroIdentity{};
}

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    game_ = context.game();
    auto melee_vtable = melee_vtable_memory_.view();
    if (!melee_vtable) {
        return std::unexpected(melee_vtable.error());
    }
    melee_vtable_ = melee_vtable->data();
    // Resolve borrowed global views before the native callbacks can run.
    const auto bind_view = []<class T>(const fc::Memory<const T> &memory, const T *&pointer) -> fc::Result<> {
        auto view = memory.view();
        if (!view) {
            return std::unexpected(view.error());
        }
        pointer = view->data();
        return {};
    };
    if (auto result = bind_view(turn_duration_memory_, turn_duration_); !result) {
        return result;
    }
    if (auto result = bind_view(prediction_turn_memory_, prediction_turn_); !result) {
        return result;
    }
    if (auto result = bind_view(acknowledged_turn_memory_, acknowledged_turn_); !result) {
        return result;
    }
    if (auto result = bind_view(update_turn_memory_, update_turn_); !result) {
        return result;
    }
    if (auto result = bind_view(local_prediction_pass_memory_, local_prediction_pass_); !result) {
        return result;
    }
    if (auto result = bind_view(network_enabled_memory_, network_enabled_); !result) {
        return result;
    }
    if (auto result = bind_view(network_client_active_memory_, network_client_active_); !result) {
        return result;
    }
    if (auto result = bind_view(collision_return_memory_, collision_return_); !result) {
        return result;
    }
    if (auto result = bind_view(effects_timer_memory_, effects_timer_); !result) {
        return result;
    }
    if (auto result = context.on_available("MeleeTiming", melee_timing::api_contract(),
                                           [this](melee_timing::QueueTimingApi api) noexcept {
                                               queue_timing_ = api;
                                               queue_timing_ready_.store(true, std::memory_order_release);
                                           });
        !result) {
        return result;
    }
    if (auto result = context.publish(api_contract(), ReplayApi{.received = fc::bind<&Handler::received>(*this)}); !result) {
        return result;
    }
    return context.on_available("Diagnostics.HeroDiagnostics", hero_diagnostics::api_contract(), [this](DiagnosticsApi api) noexcept {
        connect_diagnostics(api);
    });
}

const melee_timing::QueueTimingApi *Handler::queue_timing() const noexcept {
    return queue_timing_ready_.load(std::memory_order_acquire) ? &queue_timing_ : nullptr;
}

void Handler::received(void *soldier, std::int32_t turn) noexcept {
    const auto errors = AmbientErrors::capture();
    if (network_prediction_active()) {
        const auto identity = soldier_identity(soldier);
        if (identity.valid()) {
            auto *weapon = reinterpret_cast<void *>(identity.weapon);
            const auto before = diagnostics() != nullptr ? replay_history_.tags(identity, turn) : std::array<std::int32_t, 2>{};
            const auto state = ReplayState::capture(weapon, queue_timing());
            replay_history_.capture(identity, turn, state);
            presentation_history_.rewind(identity.weapon);
            jump_history_.rewind(identity.weapon);
            if (diagnostics() != nullptr) {
                const auto after = replay_history_.tags(identity, turn);
                // Stage 6 retains before tags in words[7..8], precise ages in [9], mode/lock flags in [10..11].
                // Values copy the final mode/input/combo/notified/lock clocks; Combat and Full diagnostics copy occupied queue rows.
                const hero_diagnostics::OwnerDecision decision{
                    .values = {fc::read_field<float>(weapon, 0xB4), fc::read_field<float>(weapon, 0x130),
                               fc::read_field<float>(weapon, 0x188), fc::read_field<float>(weapon, 0x118), state.lock_timer},
                    .words = {static_cast<std::uint32_t>(turn), static_cast<std::uint32_t>(after[0]), static_cast<std::uint32_t>(after[1]),
                              fc::read_field<std::uint32_t>(weapon, 0xAC), fc::read_field<std::uint32_t>(weapon, 0x180),
                              static_cast<std::uint32_t>(identity.is_remote()), static_cast<std::uint32_t>(identity.valid()),
                              static_cast<std::uint32_t>(before[0]), static_cast<std::uint32_t>(before[1]),
                              static_cast<std::uint32_t>(state.precise_inputs), fc::read_field<std::uint32_t>(weapon, 0xB0),
                              state.lock_flags},
                    .kind = hero_diagnostics::CauseKind::Replay,
                    .stage = 6,
                    .result = static_cast<std::uint8_t>(after[0] == turn && after[1] == turn)};
                owner_decision(weapon, decision);
            }
        }
    }
    errors.restore();
}

void Handler::connect_diagnostics(DiagnosticsApi diagnostics) noexcept {
    diagnostics_ = diagnostics;
    diagnostic_capture_ = diagnostics.capture_mode();
    diagnostics_ready_.store(true, std::memory_order_release);
}

const Handler::DiagnosticsApi *Handler::diagnostics() const noexcept {
    return diagnostics_ready_.load(std::memory_order_acquire) ? &diagnostics_ : nullptr;
}

void Handler::before_predict_turn() noexcept {
    if (!network_prediction_active()) {
        return;
    }
    // Pass flags still refer to the previous iteration here; only the native turn identifies this start.
    const auto turn = *prediction_turn_;
    observed_prediction_turn_ = turn;
    for (std::size_t player{}; player < kNetworkPlayerCount; ++player) {
        const auto tracked = replay_history_.tracked(player);
        if (!tracked.valid()) {
            continue;
        }
        auto *weapon = reinterpret_cast<void *>(tracked.weapon);
        bool ready{};
        const auto identity = capture_identity(weapon, ready);
        if (!ready || identity != tracked) {
            clear_weapon(tracked.weapon, hero_diagnostics::HistoryReason::NotReady);
            continue;
        }
        if (fallback_pending_ || replay_history_.needs_rewind(identity, turn)) {
            const auto state = replay_history_.restore(identity, turn);
            lock_decision(weapon, turn, 2, state.has_value());
            if (state) {
                // Translate contact timestamps to the restored combo clock.
                ContactScope contacts;
                contacts.begin(weapon, contact_scope_);
                contact_decision(weapon, contacts, 3);
                state->restore(weapon, false, queue_timing());
                if (fallback_pending_) {
                    presentation_history_.rewind(identity.weapon);
                }
                contacts.finish();
                contact_decision(weapon, contacts, 4);
            }
            lock_decision(weapon, turn, 3, state.has_value());
            replay_sample(weapon, identity, turn, 7, state.has_value());
        }
        replay_history_.capture(identity, turn, ReplayState::capture(weapon, queue_timing()));
        replay_sample(weapon, identity, turn, 0, true);
    }
    fallback_pending_ = false;
}

void Handler::after_predict_turn() noexcept {
    if (!network_prediction_active() || update_window_ != nullptr || read_window_ != nullptr) {
        return;
    }
    const auto next_start = *prediction_turn_;
    for (std::size_t player{}; player < kNetworkPlayerCount; ++player) {
        const auto tracked = replay_history_.tracked(player);
        if (!tracked.valid()) {
            continue;
        }
        auto *weapon = reinterpret_cast<void *>(tracked.weapon);
        bool ready{};
        const auto identity = capture_identity(weapon, ready);
        if (!ready || identity != tracked) {
            clear_weapon(tracked.weapon, hero_diagnostics::HistoryReason::NotReady);
            continue;
        }
        const bool captured = replay_history_.complete(identity, next_start, weapon, queue_timing());
        replay_sample(weapon, identity, next_start, captured ? 1 : 2, true);
    }
}

void Handler::owner_decision(void *weapon, const hero_diagnostics::OwnerDecision &decision) noexcept {
    if (const auto *output = diagnostics()) {
        output->owner_decision(weapon, &decision);
    }
}

void Handler::replay_sample(void *weapon, const HeroIdentity &identity, std::int32_t turn, std::uint8_t stage, bool result) noexcept {
    if (diagnostics() == nullptr) {
        return;
    }
    const auto tags = replay_history_.tags(identity, turn);
    const hero_diagnostics::OwnerDecision decision{
        .values = {fc::read_field<float>(weapon, 0xB4), fc::read_field<float>(weapon, 0x130), fc::read_field<float>(weapon, 0x188),
                   fc::read_field<float>(weapon, 0x118)},
        .words = {static_cast<std::uint32_t>(turn), static_cast<std::uint32_t>(tags[0]), static_cast<std::uint32_t>(tags[1]),
                  fc::read_field<std::uint32_t>(weapon, 0xAC), fc::read_field<std::uint32_t>(weapon, 0x180),
                  static_cast<std::uint32_t>(identity.is_remote()), static_cast<std::uint32_t>(identity.valid())},
        .kind = hero_diagnostics::CauseKind::Replay,
        .stage = stage,
        .result = static_cast<std::uint8_t>(result)};
    owner_decision(weapon, decision);
}

void Handler::lock_decision(void *weapon, std::int32_t turn, std::uint8_t stage, bool restored) noexcept {
    if (diagnostics() == nullptr) {
        return;
    }
    auto *owner = fc::read_field<void *>(weapon, kWeaponOwnerOffset);
    const auto flags = fc::read_field<std::uint8_t>(owner, 0x231);
    const hero_diagnostics::OwnerDecision decision{.values = {fc::read_field<float>(owner, 0x234)},
                                                   .words = {flags, flags & 0x3CU, static_cast<std::uint32_t>(turn)},
                                                   .kind = hero_diagnostics::CauseKind::MovementLock,
                                                   .stage = stage,
                                                   .result = static_cast<std::uint8_t>(restored)};
    owner_decision(weapon, decision);
}

void Handler::gait_decision(const GaitWindow &window, std::uint8_t stage, bool restored) noexcept {
    if (diagnostics() == nullptr) {
        return;
    }
    auto *soldier = reinterpret_cast<void *>(window.identity.owner - kSoldierControllableOffset);
    const bool full = diagnostic_capture_ == hero_diagnostics::CaptureMode::Full;
    const auto current =
        full ? LiveGaitState::capture(soldier)
             : LiveGaitState{.classification = static_cast<std::uint8_t>(fc::read_field<std::uint8_t>(soldier, 0x9D5) & 0x38U)};
    const auto flags = fc::read_field<std::uint8_t>(soldier, 0x9D5);
    const auto *soldier_class = full ? fc::read_field<void *>(soldier, 0x440) : nullptr;
    // LiveGait words 7/8 retain the saved/current movement-angle float bits.
    const hero_diagnostics::OwnerDecision decision{
        .values = {window.live.position[0], window.live.position[1], window.live.position[2], current.position[0], current.position[1],
                   current.position[2], window.delta, full ? fc::read_field<float>(soldier_class, 0x69C) : 0.0F},
        .words = {flags, current.classification, window.live.classification, static_cast<std::uint32_t>(window.reading),
                  *local_prediction_pass_, static_cast<std::uint32_t>(*prediction_turn_), fc::read_field<std::uint32_t>(soldier, 0x744),
                  full ? std::bit_cast<std::uint32_t>(window.live.angle) : 0U, full ? std::bit_cast<std::uint32_t>(current.angle) : 0U},
        .kind = hero_diagnostics::CauseKind::LiveGait,
        .stage = stage,
        .result = static_cast<std::uint8_t>(restored)};
    owner_decision(reinterpret_cast<void *>(window.identity.weapon), decision);
}

bool Handler::historical_update(void *weapon) const noexcept {
    for (auto *window = update_window_; window != nullptr; window = window->previous) {
        if (window->weapon == weapon && window->historical) {
            return true;
        }
    }
    return false;
}

void Handler::observe_enter_state(void *weapon, std::int32_t state) noexcept {
    if (authority_window_ != nullptr && authority_window_->weapon == weapon) {
        return;
    }
    auto *window = update_window_;
    if (!valid_state(state) || window == nullptr || window->weapon != weapon || !window->identity.valid()) {
        clear_weapon(reinterpret_cast<std::uintptr_t>(weapon));
        return;
    }
    const auto &identity = window->identity;
    const auto source = fc::read_field<int>(weapon, kComboStateOffset);
    const PresentationEntry entry{.turn = *prediction_turn_,
                                  .order = presentation_history_.next_order(identity, *prediction_turn_),
                                  .source = source,
                                  .target = state};
    const auto confirmation_turn = window->frontier ? (identity.is_local() ? *prediction_turn_ : *update_turn_) : -1;
    const bool replay = presentation_history_.enter(identity, entry, confirmation_turn);
    window->entry = entry;
    window->audio_order = 0;
    // Catch-up traversals retain entry occurrences but cannot append a second presentation frontier.
    if (!window->frontier || replay) {
        return;
    }
    const auto before = identity.is_local() ? local_history_.snapshot(identity) : remote_history_.snapshot(identity);
    const bool recorded = identity.is_local() ? local_history_.observe_prediction(identity, source, state, *prediction_turn_)
                                              : remote_history_.observe_prediction(identity, source, state, *update_turn_);
    if (const auto *output = diagnostics()) {
        const hero_diagnostics::HistoryDecision decision{.before = before,
                                                         .after = identity.is_local() ? local_history_.snapshot(identity)
                                                                                      : remote_history_.snapshot(identity),
                                                         .turn = identity.is_local() ? *prediction_turn_ : *update_turn_,
                                                         .source = source,
                                                         .target = state,
                                                         .operation = hero_diagnostics::HistoryOperation::Prediction,
                                                         .result = static_cast<std::uint8_t>(recorded),
                                                         .remote = static_cast<std::uint8_t>(identity.is_remote())};
        output->history_decision(weapon, &decision);
    }
}

HeroIdentity Handler::capture_identity(void *weapon, bool &ready) const noexcept {
    ready = false;
    if (weapon == nullptr) {
        return {};
    }

    auto *owner = fc::read_field<void *>(weapon, kWeaponOwnerOffset);
    auto *combo = fc::read_field<void *>(weapon, kComboOffset);
    if (owner == nullptr || combo == nullptr) {
        return {};
    }

    // Negative player indices belong to native AI; reconciliation tracks player-owned melee only.
    const auto player_id = fc::read_field<int>(owner, kPlayerIdOffset);
    if (player_id < 0 || player_id >= static_cast<int>(kNetworkPlayerCount)) {
        return {};
    }

    const auto joystick_result = get_joystick_index_.call(player_id);
    if (!joystick_result) {
        return {};
    }

    // GetJoystickIndex returns local slot 0 or 1, or -1 for a remote player.
    const auto joystick = *joystick_result;
    const HeroIdentity identity{
        .weapon = reinterpret_cast<std::uintptr_t>(weapon),
        .owner = reinterpret_cast<std::uintptr_t>(owner),
        .combo = reinterpret_cast<std::uintptr_t>(combo),
        .player_id = static_cast<std::int16_t>(player_id),
        .local_player = joystick >= 0 && joystick < static_cast<int>(kLocalPlayerCount) ? static_cast<std::uint8_t>(joystick)
                                                                                        : static_cast<std::uint8_t>(0xFF),
    };

    // Read readiness through the containing soldier; the game owns the weapon, owner and combo definitions.
    auto *soldier = static_cast<std::byte *>(owner) - kSoldierControllableOffset;
    const bool alive = (fc::read_field<std::uint8_t>(soldier, kAliveFlagsOffset) & kAliveFlag) != 0;
    ready = identity.valid() && alive && selected_weapon(identity) == weapon;
    return identity;
}

void *Handler::selected_weapon(const HeroIdentity &identity) noexcept {
    if (!identity.valid()) {
        return nullptr;
    }

    const auto soldier = identity.owner - kSoldierControllableOffset;

    // Sign-extend the low nibble used by EntitySoldier's active-weapon selection.
    const auto packed = fc::read_field<std::uint8_t>(soldier, kPackedSelectionOffset);
    auto index = static_cast<int>(packed & 0x0F);
    if ((index & 0x08) != 0) {
        index -= 16;
    }

    if (index < 0 || index >= kWeaponSlotCount) {
        return nullptr;
    }

    return fc::read_field<void *>(soldier, kWeaponArrayOffset + static_cast<std::size_t>(index) * sizeof(void *));
}

bool Handler::network_prediction_active() noexcept {
    const auto state = game_.current();
    if (!state || state->session == 0 || (state->mode != fc::GameMode::Client && state->mode != fc::GameMode::Host)) {
        clear_tracking();
        session_ = 0;
        return false;
    }

    if (session_ != state->session) {
        clear_tracking();
        session_ = state->session;
    }

    // Ordinary host frames switch object worlds without ending the session or its client-world history.
    return *network_enabled_ != 0 && *network_client_active_ != 0 && *prediction_turn_ > 0 && *acknowledged_turn_ >= 0 &&
           *update_turn_ >= 0;
}

void Handler::clear_weapon(std::uintptr_t weapon, hero_diagnostics::HistoryReason reason) noexcept {
    replay_history_.clear_weapon(weapon);
    local_history_.clear_weapon(weapon);
    remote_history_.clear_weapon(weapon);
    presentation_history_.clear_weapon(weapon);
    jump_history_.clear_weapon(weapon);
    for (auto *window = gait_window_; window != nullptr; window = window->previous) {
        if (window->identity.weapon == weapon) {
            window->identity = {};
        }
    }
    for (auto *window = read_window_; window != nullptr; window = window->previous) {
        if (window->identity.weapon == weapon) {
            window->identity = {};
        }
    }
    for (auto *window = update_window_; window != nullptr; window = window->previous) {
        if (reinterpret_cast<std::uintptr_t>(window->weapon) == weapon) {
            window->identity = {};
            window->entry.reset();
        }
    }
    for (auto *window = authority_window_; window != nullptr; window = window->previous) {
        if (reinterpret_cast<std::uintptr_t>(window->weapon) == weapon) {
            window->identity = {};
            window->entry.reset();
        }
    }
    if (const auto *output = diagnostics()) {
        const hero_diagnostics::HistoryDecision decision{.operation = hero_diagnostics::HistoryOperation::Clear, .reason = reason};
        output->history_decision(reinterpret_cast<void *>(weapon), &decision);
    }
}

void Handler::clear_tracking() noexcept {
    observed_prediction_turn_ = -1;
    fallback_pending_ = false;
    replay_history_.clear_all();
    local_history_.clear_all();
    remote_history_.clear_all();
    presentation_history_.clear_all();
    jump_history_.clear_all();
    for (auto *window = gait_window_; window != nullptr; window = window->previous) {
        window->identity = {};
    }
    for (auto *window = read_window_; window != nullptr; window = window->previous) {
        window->identity = {};
    }
    for (auto *window = update_window_; window != nullptr; window = window->previous) {
        window->identity = {};
        window->entry.reset();
    }
    for (auto *window = authority_window_; window != nullptr; window = window->previous) {
        window->identity = {};
        window->entry.reset();
    }
}

bool Handler::suppress_audio(void *weapon, std::uint8_t kind, std::uint32_t &order, std::uint64_t variant) noexcept {
    bool suppressed{};
    if (authority_window_ != nullptr && authority_window_->weapon == weapon) {
        auto &window = *authority_window_;
        order = window.audio_order++;
        suppressed = window.identity.valid() && window.entry &&
                     presentation_history_.audio_presented(window.identity, *window.entry, kind, order, variant);
    } else if (update_window_ != nullptr && update_window_->weapon == weapon) {
        auto &window = *update_window_;
        order = window.audio_order++;
        suppressed = window.identity.valid() && window.entry &&
                     presentation_history_.audio_presented(window.identity, *window.entry, kind, order, variant);
    }
    audio_decision(weapon, kind, order, variant, 0, suppressed);
    return suppressed;
}

void Handler::audio_decision(void *weapon, std::uint8_t kind, std::uint32_t order, std::uint64_t variant, std::uint8_t stage,
                             bool result) noexcept {
    if (diagnostics() == nullptr) {
        return;
    }

    HeroIdentity identity;
    std::optional<PresentationEntry> entry;
    std::uint8_t source{};
    if (authority_window_ != nullptr && authority_window_->weapon == weapon) {
        identity = authority_window_->identity;
        entry = authority_window_->entry;
        source = 2;
    } else if (update_window_ != nullptr && update_window_->weapon == weapon) {
        identity = update_window_->identity;
        entry = update_window_->entry;
        source = 1;
    }
    const auto key = entry.value_or(PresentationEntry{});
    const hero_diagnostics::OwnerDecision decision{
        .words = {static_cast<std::uint32_t>(key.turn), key.order, static_cast<std::uint32_t>(key.source),
                  static_cast<std::uint32_t>(key.target), kind, order, static_cast<std::uint32_t>(variant),
                  static_cast<std::uint32_t>(variant >> 32), static_cast<std::uint32_t>(identity.valid()),
                  static_cast<std::uint32_t>(entry.has_value()), source, static_cast<std::uint32_t>(key.authoritative)},
        .kind = hero_diagnostics::CauseKind::Audio,
        .stage = stage,
        .result = static_cast<std::uint8_t>(result)};
    owner_decision(weapon, decision);
}

void Handler::contact_decision(void *weapon, const ContactScope &contacts, std::uint8_t stage) noexcept {
    if (diagnostics() == nullptr) {
        return;
    }

    const auto snapshot = contacts.snapshot();
    const hero_diagnostics::OwnerDecision decision{.values = {snapshot.age, snapshot.delta},
                                                   .words = {static_cast<std::uint32_t>(snapshot.head), snapshot.count,
                                                             static_cast<std::uint32_t>(snapshot.owns),
                                                             static_cast<std::uint32_t>(snapshot.destroyed)},
                                                   .kind = hero_diagnostics::CauseKind::ContactIsolation,
                                                   .stage = stage};
    owner_decision(weapon, decision);
}

void Handler::animation_decision(const HeroIdentity &identity, const AnimationRequest &request, std::uint8_t stage, std::uint8_t lane,
                                 bool consumed) noexcept {
    if (diagnostics() == nullptr) {
        return;
    }

    const auto snapshot = presentation_history_.snapshot(identity);
    const auto key = (stage < 2 ? snapshot.current : snapshot.rendering).value_or(PresentationEntry{});
    const hero_diagnostics::OwnerDecision decision{
        .words = {static_cast<std::uint32_t>(request.mode), static_cast<std::uint32_t>(request.restart), request.primary, request.secondary,
                  static_cast<std::uint32_t>(request.full_body), static_cast<std::uint32_t>(key.turn), key.order,
                  static_cast<std::uint32_t>(key.source), static_cast<std::uint32_t>(key.target), snapshot.lanes,
                  static_cast<std::uint32_t>(snapshot.retaining) | (snapshot.displayed ? 2U : 0U) | (snapshot.current ? 4U : 0U) |
                      (snapshot.rendering ? 8U : 0U) | (key.authoritative ? 16U : 0U),
                  lane},
        .kind = stage < 2 ? hero_diagnostics::CauseKind::AnimationRequest : hero_diagnostics::CauseKind::AnimationLane,
        .stage = stage,
        .result = static_cast<std::uint8_t>(consumed)};
    owner_decision(reinterpret_cast<void *>(identity.weapon), decision);
}

void Handler::record_audio(void *weapon, std::uint8_t kind, std::uint32_t order, std::uint64_t variant) noexcept {
    audio_decision(weapon, kind, order, variant, 1, true);
    // Only observed prediction or qualified authority occurrences can retain a completed audio leaf.
    if (authority_window_ != nullptr && authority_window_->weapon == weapon) {
        const auto &window = *authority_window_;
        if (window.identity.valid() && window.entry) {
            presentation_history_.present_audio(window.identity, *window.entry, kind, order, variant);
        }
    } else if (update_window_ != nullptr && update_window_->weapon == weapon) {
        const auto &window = *update_window_;
        if (window.identity.valid() && window.entry) {
            presentation_history_.present_audio(window.identity, *window.entry, kind, order, variant);
        }
    }
}

} // namespace fc_plugins::hero_animation_fix
