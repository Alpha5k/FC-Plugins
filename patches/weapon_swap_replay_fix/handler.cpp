#include "handler.hpp"

#include <bit>
#include <cstddef>
#include <format>
#include <expected>

namespace fc_plugins::weapon_swap_replay_fix {
namespace {

// Both retail layouts use these member, subobject and hook-stack offsets.
constexpr std::size_t kControllableOffset = 0x240;
constexpr std::size_t kPlayerIdOffset = 0x314;
constexpr std::size_t kWeaponArrayOffset = 0x720;
constexpr std::size_t kSelectedIndexOffset = 0x740;
constexpr std::size_t kPackedSelectionOffset = 0x742;

constexpr std::size_t kLocalChannelStackOffset = 0x20;
constexpr std::size_t kLocalOldIndexStackOffset = 0x28;
constexpr std::size_t kAuthorityServerIndexStackOffset = 0x1C;

constexpr std::size_t kRenderSoldierAdjustment = 0x94;
constexpr std::size_t kHudChannelArgumentOffset = 0x24;

constexpr std::size_t kReturnAddressOffset = 0x04;
constexpr std::size_t kSwitchTriggerOffset = 0x78;

constexpr std::size_t kMuzzleFlashStartTimeOffset = 0xC4;
constexpr std::size_t kSelectChannelArgumentOffset = 0x08;
constexpr std::uintptr_t kZeroFlagMask = 1U << 6;

// Replaces only the selection byte while preserving the register bits unrelated to presentation.
std::uintptr_t with_low_byte(std::uintptr_t value, std::uint8_t low_byte) noexcept {
    return (value & ~std::uintptr_t{0xFF}) | low_byte;
}

} // namespace

void Handler::plan(fc::Plan &plan) {
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    // Access reservations retain the validated native globals for the installed handler's lifetime.
    update_turn_memory_ = plan.access(layout->update_turn);
    prediction_turn_memory_ = plan.access(layout->prediction_turn);
    acknowledged_turn_memory_ = plan.access(layout->acknowledged_turn);

    network_enabled_memory_ = plan.access(layout->network_enabled);
    network_client_active_memory_ = plan.access(layout->network_client_active);

    client_turn_memory_ = plan.access(layout->client_turn);
    local_move_history_memory_ = plan.access(layout->local_move_history);

    get_joystick_index_ = plan.function(layout->get_joystick_index);
    get_local_player_ = plan.function(layout->get_local_player);

    plan.observe(layout->reset_soldier, fc::before([this](void *soldier) noexcept {
                     invalidate_soldier(soldier);
                 }));
    plan.observe(layout->destroy_soldier, fc::before([this](void *soldier) noexcept {
                     invalidate_soldier(soldier);
                 }));
    // EBP locals retain local slot at -8 and its source pointer at -0x30 + slot*4 until this jump.
    plan.observe_instruction(layout->submitted_move, fc::before([this](const fc::CpuContext &context) noexcept {
                                 observe_lifecycle();
                                 if (context.ebp == 0 || session_ == 0 || *network_enabled_ == 0) {
                                     return;
                                 }

                                 const int local_player = fc::read_field<int>(context.ebp - 8);
                                 if (local_player < 0 || local_player >= kLocalPlayerCount) {
                                     return;
                                 }
                                 const auto player = get_local_player_.call(local_player);
                                 const auto source =
                                     fc::read_field<std::uint32_t>(context.ebp - 0x30 + static_cast<std::uintptr_t>(local_player) * 4);
                                 recorded_moves_.record(local_player, *client_turn_, player ? *player : -1, source != 0);
                             }));

    plan.observe<RenderInvocation>(
        layout->render, fc::before([this](void *renderable, std::uint32_t, float, std::uint32_t, RenderInvocation &invocation) noexcept {
            invocation.previous = render_invocation_;
            if (presentation_active_ && reinterpret_cast<std::uintptr_t>(renderable) >= kRenderSoldierAdjustment) {
                invocation.view =
                    presentation_view(reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(renderable) - kRenderSoldierAdjustment));
            }
            render_invocation_ = &invocation;
        }),
        fc::after([this](void *, std::uint32_t, float, std::uint32_t, const RenderInvocation &invocation) noexcept {
            render_invocation_ = invocation.previous;
        }));

    // Check the instructions that reference each global before callbacks use the retained addresses.
    for (const auto *site : {&layout->update_turn_context, &layout->prediction_turn_context, &layout->acknowledged_turn_context,
                             &layout->network_state_context, &layout->client_turn_context, &layout->move_history_context}) {
        plan.check(*site, site->evidence.bytes.size());
    }

    primary_switch_call_memory_ = plan.access<const std::byte>(plan.at(layout->primary_switch_call.rva, layout->primary_switch_call.name),
                                                               5, layout->primary_switch_call.evidence);
    secondary_switch_call_memory_ = plan.access<const std::byte>(
        plan.at(layout->secondary_switch_call.rva, layout->secondary_switch_call.name), 5, layout->secondary_switch_call.evidence);

    // Observe local swaps after Deselect, before the channel byte changes and the new weapon's Select runs.
    plan.observe_instruction(layout->local_selection, fc::before([this](const fc::CpuContext &context) noexcept {
                                 if (context.edi == 0 || fc::stack_pointer(context) == 0) {
                                     return;
                                 }

                                 const auto stack = fc::stack_pointer(context);
                                 handle_local_selection(
                                     reinterpret_cast<void *>(context.edi), fc::read_field<int>(stack, kLocalChannelStackOffset),
                                     fc::read_field<int>(stack, kLocalOldIndexStackOffset), static_cast<int>(context.eax));
                             }));

    // EDI names the Controllable and ECX the target channel; the packed byte still names the source weapon.
    plan.observe_instruction(layout->packed_sync, fc::before([this](const fc::CpuContext &context) noexcept {
                                 if (context.edi != 0) {
                                     handle_packed_sync(reinterpret_cast<void *>(context.edi), static_cast<int>(context.ecx));
                                 }
                             }));

    plan.observe_instruction(
        layout->authority_selection, fc::before([this](const fc::CpuContext &context) noexcept {
            if (context.edi == 0 || context.edx == 0 || fc::stack_pointer(context) == 0) {
                return;
            }

            const auto selected_offset = static_cast<std::intptr_t>(context.edx) - static_cast<std::intptr_t>(context.edi) -
                                         static_cast<std::intptr_t>(kSelectedIndexOffset);

            // EDX points to the current channel slot; the server update's new slot is on the stack.
            handle_authority_selection(reinterpret_cast<void *>(context.edi), static_cast<int>(selected_offset),
                                       fc::read_field<std::int8_t>(context.edx),
                                       fc::read_field<std::int8_t>(fc::stack_pointer(context), kAuthorityServerIndexStackOffset));
        }));

    plan.observe_instruction(layout->packed_authority, fc::before([this](const fc::CpuContext &context) noexcept {
                                 if (context.edi != 0) {
                                     auto *soldier = reinterpret_cast<void *>(context.edi);

                                     // ECX indexes the old weapon array entry; EDX is the channel passed to its native deselection.
                                     handle_packed_authority(soldier, static_cast<int>(context.edx), static_cast<int>(context.ecx),
                                                             fc::read_field<std::uint8_t>(soldier, kPackedSelectionOffset));
                                 }
                             }));

    // Mute the base Select's change sound when the selection repeats; its native timestamp write still runs.
    // Deselect and derived Select still run, including melee input resets.
    plan.hook_instruction(layout->weapon_select, [this](fc::CpuContext &context) noexcept {
        suppress_duplicate_select(context);
    });

    // HUD uses the substituted weapon for identity events and ammo/charge values.
    plan.hook_instruction(layout->hud_weapon, [this](fc::CpuContext &context) noexcept {
        if (!presentation_active_ || context.esi == 0 || context.ebx == 0) {
            return;
        }

        // EBX retains the original HUD stack frame; ESI still names the Controllable before MOV ESI,EAX.
        const int channel = fc::read_field<int>(context.ebx, kHudChannelArgumentOffset);
        context.eax = reinterpret_cast<std::uintptr_t>(
            resolve_hud_weapon(reinterpret_cast<void *>(context.esi), channel, reinterpret_cast<void *>(context.eax)));
    });

    // Every active and companion lookup consumes the same view within this native Render frame.
    plan.hook_instruction(layout->render_selection, [this](fc::CpuContext &context) noexcept {
        if (const auto *view = render_view(context)) {
            context.ecx = with_low_byte(context.ecx, view->packed);
        }
    });

    plan.hook_instruction(layout->render_channel, [this](fc::CpuContext &context) noexcept {
        if (const auto *view = render_view(context)) {
            context.eax = with_low_byte(context.eax, view->packed);
        }
    });

    plan.hook_instruction(layout->model_selection, [this](fc::CpuContext &context) noexcept {
        if (const auto *view = render_view(context)) {
            context.edx = with_low_byte(context.edx, view->packed);
        }
    });

    for (const auto &site : layout->render_companion) {
        plan.hook_instruction(site, [this](fc::CpuContext &context) noexcept {
            if (const auto *view = render_view(context)) {
                context.eax = with_low_byte(context.eax, static_cast<std::uint8_t>(view->slots[0]));
            }
        });
    }
    plan.hook_instruction(layout->model_companion, [this](fc::CpuContext &context) noexcept {
        if (const auto *view = render_view(context)) {
            const int companion_channel = packed_weapon_channel(view->packed) == 0 ? 1 : 0;
            context.eax = with_low_byte(context.eax, static_cast<std::uint8_t>(view->slots[static_cast<std::size_t>(companion_channel)]));
        }
    });

    // Restrict held-switch suppression to PlayerController callers; other SetSwitch users keep their direction.
    plan.hook_instruction(layout->switch_latch, [this](fc::CpuContext &context) noexcept {
        if (context.ecx == 0 || context.ebp == 0) {
            return;
        }

        const auto return_address = fc::read_field<std::uint32_t>(context.ebp, kReturnAddressOffset);
        if (return_address != primary_switch_return_ && return_address != secondary_switch_return_) {
            return;
        }

        if (suppress_held_switch(reinterpret_cast<void *>(context.ecx), static_cast<int>(context.edx),
                                 static_cast<std::uint32_t>(context.xmm[1].low))) {
            // SetSwitch stores XMM1's low float next; zero the direction while preserving its other bits.
            context.xmm[1].low &= 0xFFFFFFFF00000000ULL;
        }
    });
}

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    // Prepared views borrow image storage; only game-thread callbacks read mutable native values.
    const auto bind_view = []<class T>(const fc::Memory<const T> &memory, const T *&pointer) -> fc::Result<> {
        auto view = memory.view();
        if (!view) {
            return std::unexpected(view.error());
        }

        pointer = view->data();
        return {};
    };

    if (auto result = bind_view(update_turn_memory_, update_turn_); !result) {
        return result;
    }

    if (auto result = bind_view(prediction_turn_memory_, prediction_turn_); !result) {
        return result;
    }

    if (auto result = bind_view(acknowledged_turn_memory_, acknowledged_turn_); !result) {
        return result;
    }

    if (auto result = bind_view(network_enabled_memory_, network_enabled_); !result) {
        return result;
    }

    if (auto result = bind_view(network_client_active_memory_, network_client_active_); !result) {
        return result;
    }

    if (auto result = bind_view(client_turn_memory_, client_turn_); !result) {
        return result;
    }

    if (auto result = bind_view(local_move_history_memory_, local_move_history_); !result) {
        return result;
    }

    const std::byte *primary_call{};
    const std::byte *secondary_call{};

    if (auto result = bind_view(primary_switch_call_memory_, primary_call); !result) {
        return result;
    }

    if (auto result = bind_view(secondary_switch_call_memory_, secondary_call); !result) {
        return result;
    }

    // A native frame's return address distinguishes these two PlayerController calls from other latch users.
    primary_switch_return_ = reinterpret_cast<std::uintptr_t>(primary_call + 5);
    secondary_switch_return_ = reinterpret_cast<std::uintptr_t>(secondary_call + 5);

    game_ = context.game();
    return context.on_available("Diagnostics.HeroDiagnostics", hero_diagnostics::api_contract(),
                                [this](hero_diagnostics::HeroDiagnosticsApi api) noexcept {
                                    diagnostics_ = api;
                                    diagnostics_ready_.store(true, std::memory_order_release);
                                });
}

int Handler::local_player_index(void *soldier) const noexcept {
    if (soldier == nullptr) {
        return -1;
    }

    const auto index = get_joystick_index_.call(fc::read_field<int>(soldier, kPlayerIdOffset));
    return index ? *index : -1;
}

int Handler::tracked_local_player(void *soldier) const noexcept {
    for (int index = 0; index < kLocalPlayerCount; ++index) {
        if (local_soldiers_[static_cast<std::size_t>(index)] == soldier) {
            return index;
        }
    }

    return -1;
}

bool Handler::is_tracked_local_soldier(void *soldier, int local_player) const noexcept {
    return local_player >= 0 && local_player < kLocalPlayerCount && local_soldiers_[static_cast<std::size_t>(local_player)] == soldier;
}

bool Handler::network_prediction_active() const noexcept {
    const auto state = game_.current();
    return state && state->session != 0 && state->session == session_ && state->mode == fc::GameMode::Client && *network_enabled_ != 0 &&
           *network_client_active_ != 0 && *update_turn_ > 0 && *prediction_turn_ > 0 && *acknowledged_turn_ >= 0;
}

void Handler::observe_lifecycle() noexcept {
    const auto state = game_.current();
    if (!state || state->session == 0 || state->mode != fc::GameMode::Client) {
        clear_tracking();
        session_ = 0;
        return;
    }

    // Session IDs prevent retained identities crossing sessions even when native turn counters remain positive.
    if (session_ != state->session) {
        clear_tracking();
        session_ = state->session;
    }

    if (*network_enabled_ == 0) {
        clear_tracking();
        return;
    }

    // netOnClient selects a world inside the same session; host-world callbacks leave client history intact.
    if (*network_client_active_ == 0) {
        select_intent_.clear();
        return;
    }

    const auto observed = frontiers();
    if (lifecycle_.observe(observed)) {
        clear_prediction_state();
    }

    if (network_prediction_active()) {
        ledger_.observe_frontiers(observed);
        packed_ledger_.observe_frontiers(observed);
        update_presentation_activity();
    }
}

void Handler::update_presentation_activity() noexcept {
    presentation_active_ = ledger_.has_active() || packed_ledger_.has_active();
}

void Handler::clear_prediction_state() noexcept {
    ledger_.clear();
    packed_ledger_.clear();
    local_soldiers_ = {};
    presentation_active_ = false;
    select_intent_.clear();
    recorded_moves_.clear();
    for (auto *invocation = render_invocation_; invocation != nullptr; invocation = invocation->previous) {
        invocation->view.reset();
    }
}

void Handler::clear_tracking() noexcept {
    clear_prediction_state();
    lifecycle_.clear();
}

TurnFrontiers Handler::frontiers() const noexcept {
    return {*update_turn_, *prediction_turn_, *acknowledged_turn_};
}

bool Handler::validate_snapshot(const EpochSnapshot &snapshot) const noexcept {
    if (snapshot.soldier == nullptr) {
        return false;
    }

    for (int index = 0; index < kWeaponSlotCount; ++index) {
        const auto *known = snapshot.node_weapons[static_cast<std::size_t>(index)];
        if (known != nullptr && weapon_at(snapshot.soldier, index) != known) {
            return false;
        }
    }
    return true;
}

bool Handler::validate_packed_snapshot(const PackedSnapshot &snapshot) const noexcept {
    if (snapshot.soldier == nullptr || snapshot.projected_key >= PackedSwapLedger::kKeyCount) {
        return false;
    }

    if (snapshot.node_weapons[static_cast<std::size_t>(snapshot.projected_key)] == nullptr) {
        return false;
    }

    // Both channel keys refer to the soldier's same eight-entry weapon array.
    for (int key = 0; key < PackedSwapLedger::kKeyCount; ++key) {
        const auto *known = snapshot.node_weapons[static_cast<std::size_t>(key)];
        if (known != nullptr && weapon_at(snapshot.soldier, key % kWeaponSlotCount) != known) {
            return false;
        }
    }
    return true;
}

bool Handler::validate_player_history(void *soldier, int local_player) noexcept {
    for (int channel = 0; channel < kWeaponChannelCount; ++channel) {
        EpochSnapshot snapshot;
        if (ledger_.find(soldier, local_player, channel, snapshot) && !validate_snapshot(snapshot)) {
            selection_decision(soldier, {.request_turn = snapshot.latest_request_turn,
                                         .epoch = snapshot.epoch,
                                         .sequence = snapshot.sequence,
                                         .local_player = static_cast<std::int8_t>(local_player),
                                         .channel = static_cast<std::int8_t>(channel),
                                         .stage = 6,
                                         .invalidated = 1});
            invalidate_player(local_player);
            return false;
        }
    }

    PackedSnapshot snapshot;
    if (packed_ledger_.find(soldier, local_player, snapshot) && !validate_packed_snapshot(snapshot)) {
        invalidate_player(local_player);
        return false;
    }
    return true;
}

void *Handler::soldier_from_controllable(void *controllable) noexcept {
    return controllable == nullptr ? nullptr : static_cast<std::byte *>(controllable) - kControllableOffset;
}

void *Handler::weapon_at(void *soldier, int index) noexcept {
    if (soldier == nullptr || index < 0 || index >= kWeaponSlotCount) {
        return nullptr;
    }

    return fc::read_field<void *>(soldier, kWeaponArrayOffset + static_cast<std::size_t>(index) * sizeof(void *));
}

bool Handler::held_switch_replay(int local_player, int player_id, int channel, std::uint32_t trigger,
                                 std::uint32_t direction) const noexcept {
    // SubmitMove advances the next-write turn after recording; age zero has no current sample.
    const auto prediction_turn = *prediction_turn_;
    if (!recorded_moves_.has_pair(local_player, player_id, *client_turn_, prediction_turn)) {
        return false;
    }

    const auto read_buttons = [this, local_player](int turn) noexcept {
        const auto turn_index = static_cast<std::size_t>(turn) & (kMoveHistoryTurnCount - 1);
        return (*local_move_history_)[turn_index][static_cast<std::size_t>(local_player)].buttons;
    };

    return held_switch_edge(channel, trigger, direction, read_buttons(prediction_turn - 1), read_buttons(prediction_turn));
}

bool Handler::suppress_held_switch(void *controllable, int channel, std::uint32_t direction) const noexcept {
    if (controllable == nullptr || channel < 0 || channel >= kWeaponChannelCount || !network_prediction_active()) {
        return false;
    }

    auto *soldier = soldier_from_controllable(controllable);
    const int local_player = local_player_index(soldier);
    const auto trigger =
        fc::read_field<std::uint32_t>(controllable, kSwitchTriggerOffset + static_cast<std::size_t>(channel) * sizeof(std::uint32_t));
    return held_switch_replay(local_player, fc::read_field<int>(soldier, kPlayerIdOffset), channel, trigger, direction);
}

void *Handler::resolve_hud_weapon(void *controllable, int channel, void *actual_weapon) noexcept {
    if (channel < 0 || channel >= kWeaponChannelCount) {
        return actual_weapon;
    }

    // HUD accepts generic Controllable objects; only an already tracked soldier has an inventory we may project.
    const auto view = presentation_view(soldier_from_controllable(controllable));
    if (!view || actual_weapon == nullptr) {
        return actual_weapon;
    }

    // Supply the predicted weapon to HUD without writing the soldier's selected slot.
    const int index = view->slots[static_cast<std::size_t>(channel)];
    auto *returned = index >= 0 && index < kWeaponSlotCount ? view->weapons[static_cast<std::size_t>(index)] : actual_weapon;
    if (returned != actual_weapon) {
        selection_decision(view->soldier, {.actual_weapon = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(actual_weapon)),
                                           .target_weapon = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(returned)),
                                           .channel = static_cast<std::int8_t>(channel),
                                           .target_slot = static_cast<std::int8_t>(index),
                                           .stage = 5});
    }
    return returned;
}

std::optional<Handler::PresentationView> Handler::presentation_view(void *soldier) noexcept {
    observe_lifecycle();
    if (!network_prediction_active()) {
        return std::nullopt;
    }

    const int local_player = tracked_local_player(soldier);
    if (local_player < 0) {
        return std::nullopt;
    }

    // Discard the old projection when the soldier loses its local input slot.
    if (local_player_index(soldier) != local_player) {
        invalidate_player(local_player);
        return std::nullopt;
    }

    PresentationView view;
    view.soldier = soldier;
    view.packed = fc::read_field<std::uint8_t>(soldier, kPackedSelectionOffset);
    for (int index = 0; index < kWeaponSlotCount; ++index) {
        view.weapons[static_cast<std::size_t>(index)] = weapon_at(soldier, index);
    }
    for (int channel = 0; channel < kWeaponChannelCount; ++channel) {
        const int actual = fc::read_field<std::int8_t>(soldier, kSelectedIndexOffset + static_cast<std::size_t>(channel));
        view.slots[static_cast<std::size_t>(channel)] = static_cast<std::int8_t>(actual);
        EpochSnapshot snapshot;
        const bool projected = ledger_.resolve(soldier, local_player, channel, actual, snapshot);
        if (snapshot.soldier == nullptr) {
            continue;
        }

        if (!validate_snapshot(snapshot)) {
            selection_decision(soldier, {.request_turn = snapshot.latest_request_turn,
                                         .epoch = snapshot.epoch,
                                         .sequence = snapshot.sequence,
                                         .local_player = static_cast<std::int8_t>(local_player),
                                         .channel = static_cast<std::int8_t>(channel),
                                         .stage = 6,
                                         .invalidated = 1});
            invalidate_player(local_player);
            return std::nullopt;
        }

        // A no-weapon channel stays native; only known endpoints permit a predicted channel slot.
        if (projected) {
            view.slots[static_cast<std::size_t>(channel)] = static_cast<std::int8_t>(snapshot.final_index);
        }
    }

    const auto packed = view.packed;
    const int actual_channel = packed_weapon_channel(packed);
    const int actual_index = packed_weapon_index(packed);
    auto *actual_weapon = weapon_at(soldier, actual_index);
    if (actual_channel < 0 || actual_channel >= kWeaponChannelCount || actual_index < 0 || actual_weapon == nullptr) {
        invalidate_player(local_player);
        return std::nullopt;
    }

    PackedSnapshot snapshot;
    const bool presented = packed_ledger_.resolve(soldier, local_player, actual_channel, actual_index, actual_weapon, snapshot);
    if (snapshot.soldier != nullptr && !validate_packed_snapshot(snapshot)) {
        invalidate_player(local_player);
        return std::nullopt;
    }

    if (presented) {
        view.packed = PackedSwapLedger::project_selection(packed, snapshot.projected_key);
        // Packed ownership can outlive channel acceptance; HUD and companion reads must still agree with its active weapon.
        view.slots[static_cast<std::size_t>(packed_weapon_channel(view.packed))] =
            static_cast<std::int8_t>(packed_weapon_index(view.packed));
    }

    update_presentation_activity();
    return view;
}

const Handler::PresentationView *Handler::render_view(const fc::CpuContext &context) noexcept {
    if (!network_prediction_active() || render_invocation_ == nullptr || !render_invocation_->view) {
        return nullptr;
    }

    const auto &view = *render_invocation_->view;
    if (reinterpret_cast<std::uintptr_t>(view.soldier) + kRenderSoldierAdjustment != context.edi) {
        return nullptr;
    }
    const int local_player = tracked_local_player(view.soldier);
    if (local_player < 0 || local_player_index(view.soldier) != local_player) {
        invalidate_player(local_player);
        return nullptr;
    }

    // Virtual presentation calls can release inventory; never supply a captured slot whose pointer has changed.
    for (int index = 0; index < kWeaponSlotCount; ++index) {
        if (weapon_at(view.soldier, index) != view.weapons[static_cast<std::size_t>(index)]) {
            invalidate_player(local_player);
            return nullptr;
        }
    }
    return &view;
}

void Handler::invalidate_player(int local_player) noexcept {
    if (local_player < 0 || local_player >= kLocalPlayerCount) {
        return;
    }

    auto *soldier = local_soldiers_[static_cast<std::size_t>(local_player)];
    ledger_.clear_player(local_player);
    packed_ledger_.clear_player(local_player);
    local_soldiers_[static_cast<std::size_t>(local_player)] = nullptr;
    select_intent_.clear();
    for (auto *invocation = render_invocation_; invocation != nullptr; invocation = invocation->previous) {
        if (invocation->view && invocation->view->soldier == soldier) {
            invocation->view.reset();
        }
    }
    update_presentation_activity();
}

void Handler::invalidate_soldier(void *soldier) noexcept {
    if (soldier == nullptr) {
        return;
    }

    // Clear every retained association; local slot ownership can change before a reset or destruction.
    for (int local_player = 0; local_player < kLocalPlayerCount; ++local_player) {
        if (local_soldiers_[static_cast<std::size_t>(local_player)] == soldier) {
            invalidate_player(local_player);
            recorded_moves_.clear_player(local_player);
        }
    }

    // A reset can precede the first swap; only the client world's live soldier may own local input samples.
    if (*network_client_active_ != 0) {
        recorded_moves_.clear_player(local_player_index(soldier));
    }
}

void Handler::suppress_duplicate_select(fc::CpuContext &context) noexcept {
    observe_lifecycle();
    if (!network_prediction_active() || context.edi == 0 || context.ebp == 0) {
        return;
    }

    auto *weapon = reinterpret_cast<void *>(context.edi);
    const int channel = fc::read_field<int>(context.ebp, kSelectChannelArgumentOffset);
    const bool duplicate = select_intent_.consume_duplicate(weapon, channel);
    if (diagnostics_ready_.load(std::memory_order_acquire)) {
        const hero_diagnostics::PhysicsDecision decision{.values = {fc::read_field<float>(weapon, kMuzzleFlashStartTimeOffset),
                                                                    std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[0].low))},
                                                         .words = {static_cast<std::uint32_t>(channel), context.eflags, context.edi},
                                                         .operation = hero_diagnostics::PhysicsOperation::WeaponSelection,
                                                         .result = static_cast<std::uint8_t>(duplicate)};
        diagnostics_.physics_decision(weapon, 0, &decision);
    }
    if (!duplicate) {
        return;
    }

    // The displaced MOVSS resets native flash timing; only the following sound branch is omitted.
    context.eflags &= ~kZeroFlagMask;
}

} // namespace fc_plugins::weapon_swap_replay_fix
