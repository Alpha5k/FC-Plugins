#include "handler.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <cstddef>

namespace fc_plugins::weapon_swap_replay_fix {
namespace {

constexpr std::size_t kPackedSelectionOffset = 0x742;
constexpr std::size_t kSelectedIndexOffset = 0x740;

} // namespace

void Handler::selection_decision(void *soldier, hero_diagnostics::SelectionDecision decision) const noexcept {
    if (diagnostics_ready_.load(std::memory_order_acquire)) {
        const auto observed = frontiers();
        decision.update_turn = observed.update;
        decision.prediction_turn = observed.prediction;
        decision.acknowledged_turn = observed.acknowledged;
        const auto win32 = GetLastError();
        const auto winsock = WSAGetLastError();
        diagnostics_.selection_decision(soldier, &decision);
        WSASetLastError(winsock);
        SetLastError(win32);
    }
}

bool Handler::track_matching_projection(void *soldier, int local_player, int channel, int index, void *weapon,
                                        const TurnFrontiers &frontiers) noexcept {
    if (index < 0 || index >= kWeaponSlotCount) {
        return false;
    }

    // The channel history supplies the predicted weapon and IDs that link the packed history to this request.
    EpochSnapshot projection;
    if (!ledger_.find(soldier, local_player, channel, projection) || projection.final_index != index ||
        projection.node_weapons[static_cast<std::size_t>(index)] != weapon) {
        return false;
    }

    return packed_ledger_.track_projected(soldier, local_player, channel, index, weapon, projection.latest_request_turn, projection.epoch,
                                          projection.sequence, frontiers);
}

bool Handler::track_local_soldier(void *soldier, int local_player) noexcept {
    // Move this soldier's ownership out of a previous local slot before recording its new assignment.
    for (int previous = 0; previous < kLocalPlayerCount; ++previous) {
        if (previous != local_player && local_soldiers_[static_cast<std::size_t>(previous)] == soldier) {
            invalidate_player(previous);
        }
    }

    // A different soldier in the same local input slot starts with no retained channel or packed history.
    auto &tracked = local_soldiers_[static_cast<std::size_t>(local_player)];
    if (tracked != nullptr && tracked != soldier) {
        invalidate_player(local_player);
    }
    tracked = soldier;
    return validate_player_history(soldier, local_player);
}

void Handler::handle_local_selection(void *adjusted_soldier, int channel, int old_index, int target_index) noexcept {
    select_intent_.clear();
    observe_lifecycle();

    auto *soldier = soldier_from_controllable(adjusted_soldier);
    const int local_player = local_player_index(soldier);
    if (!network_prediction_active() || local_player < 0 || local_player >= kLocalPlayerCount || channel < 0 ||
        channel >= kWeaponChannelCount) {
        return;
    }

    if (!track_local_soldier(soldier, local_player)) {
        return;
    }

    auto *old_weapon = weapon_at(soldier, old_index);
    auto *target_weapon = weapon_at(soldier, target_index);
    if (old_weapon == nullptr || target_weapon == nullptr) {
        invalidate_player(local_player);
        return;
    }

    // Record the input's prediction turn before deciding whether the immediate Select repeats known input.
    const auto observed = frontiers();
    EpochSnapshot request_before;
    ledger_.find(soldier, local_player, channel, request_before);
    const auto decision = ledger_.record_local(soldier, local_player, channel, old_index, target_index, old_weapon, target_weapon,
                                               observed.prediction, observed);
    EpochSnapshot request_snapshot;
    ledger_.find(soldier, local_player, channel, request_snapshot);
    selection_decision(soldier, {.request_turn = observed.prediction,
                                 .epoch = decision.epoch,
                                 .sequence = request_snapshot.soldier != nullptr ? request_snapshot.sequence : request_before.sequence,
                                 .actual_weapon = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(old_weapon)),
                                 .target_weapon = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(target_weapon)),
                                 .local_player = static_cast<std::int8_t>(local_player),
                                 .old_channel = static_cast<std::int8_t>(channel),
                                 .channel = static_cast<std::int8_t>(channel),
                                 .old_slot = static_cast<std::int8_t>(old_index),
                                 .target_slot = static_cast<std::int8_t>(target_index),
                                 .stage = 1,
                                 .verdict = static_cast<std::uint8_t>(decision.kind),
                                 .invalidated = static_cast<std::uint8_t>(decision.failed_open()),
                                 .mute = static_cast<std::uint8_t>(decision.replay())});
    if (!decision.valid()) {
        return;
    }

    if (decision.failed_open()) {
        invalidate_player(local_player);
        return;
    }

    // If this is the active channel, retain its predicted slot in the player's combined channel/slot history.
    const auto packed = fc::read_field<std::uint8_t>(soldier, kPackedSelectionOffset);
    EpochSnapshot projected;
    if (packed_weapon_channel(packed) == channel && ledger_.find(soldier, local_player, channel, projected)) {
        packed_ledger_.track_projected(soldier, local_player, channel, projected.final_index,
                                       projected.node_weapons[static_cast<std::size_t>(projected.final_index)],
                                       projected.latest_request_turn, projected.epoch, projected.sequence, observed);
    }

    // Only a replay mutes this Select; new input keeps the weapon's change sound.
    select_intent_.arm(target_weapon, channel, decision.replay());

    update_presentation_activity();
}

void Handler::handle_packed_sync(void *adjusted_soldier, int channel) noexcept {
    select_intent_.clear();
    observe_lifecycle();
    if (adjusted_soldier == nullptr || channel < 0 || channel >= kWeaponChannelCount || !network_prediction_active()) {
        return;
    }

    auto *soldier = soldier_from_controllable(adjusted_soldier);
    const int local_player = local_player_index(soldier);
    if (local_player < 0 || local_player >= kLocalPlayerCount || !track_local_soldier(soldier, local_player)) {
        return;
    }

    const auto source = fc::read_field<std::uint8_t>(soldier, kPackedSelectionOffset);
    const int old_channel = packed_weapon_channel(source);
    const int old_index = packed_weapon_index(source);
    const int target_index = fc::read_field<std::int8_t>(soldier, kSelectedIndexOffset + static_cast<std::size_t>(channel));
    auto *old_weapon = weapon_at(soldier, old_index);
    auto *target_weapon = weapon_at(soldier, target_index);
    if (old_channel < 0 || old_channel >= kWeaponChannelCount || old_weapon == nullptr || target_weapon == nullptr) {
        invalidate_player(local_player);
        return;
    }

    const auto observed = frontiers();
    PackedDecision packed_decision;
    const bool channel_request =
        old_channel == channel && ledger_.is_known_selection(soldier, local_player, channel, target_index, target_weapon);
    if (channel_request) {
        track_matching_projection(soldier, local_player, channel, target_index, target_weapon, observed);
        packed_decision = packed_ledger_.observe_sync(soldier, local_player, channel, target_index, target_weapon, observed);
    } else {
        packed_decision = packed_ledger_.record_activation(soldier, local_player, old_channel, old_index, old_weapon, channel, target_index,
                                                           target_weapon, observed);
    }
    if (packed_decision.pointer_invalidated) {
        invalidate_player(local_player);
    }

    // A manual channel request keeps its existing policy; automatic selections mute only exact recorded occurrences.
    const bool mute = packed_decision.mute_select && !packed_decision.pointer_invalidated;
    PackedSnapshot snapshot;
    packed_ledger_.find(soldier, local_player, snapshot);
    selection_decision(soldier, {.request_turn = observed.prediction,
                                 .epoch = snapshot.epoch,
                                 .sequence = snapshot.sequence,
                                 .actual_weapon = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(old_weapon)),
                                 .target_weapon = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(target_weapon)),
                                 .local_player = static_cast<std::int8_t>(local_player),
                                 .old_channel = static_cast<std::int8_t>(old_channel),
                                 .channel = static_cast<std::int8_t>(channel),
                                 .old_slot = static_cast<std::int8_t>(old_index),
                                 .target_slot = static_cast<std::int8_t>(target_index),
                                 .stage = 3,
                                 .verdict = static_cast<std::uint8_t>(packed_decision.mute_select),
                                 .invalidated = static_cast<std::uint8_t>(packed_decision.pointer_invalidated),
                                 .mute = static_cast<std::uint8_t>(mute)});
    select_intent_.arm(target_weapon, channel, mute);
    update_presentation_activity();
}

void Handler::handle_authority_selection(void *soldier, int channel, int current_index, int server_index) noexcept {
    select_intent_.clear();
    observe_lifecycle();
    if (!network_prediction_active()) {
        return;
    }

    const int local_player = local_player_index(soldier);
    if (!is_tracked_local_soldier(soldier, local_player) || channel < 0 || channel >= kWeaponChannelCount) {
        return;
    }
    if (!validate_player_history(soldier, local_player)) {
        return;
    }
    if (current_index < 0 || current_index >= kWeaponSlotCount || server_index < 0 || server_index >= kWeaponSlotCount) {
        invalidate_player(local_player);
        return;
    }

    auto *current_weapon = weapon_at(soldier, current_index);
    auto *target_weapon = weapon_at(soldier, server_index);
    EpochSnapshot before;
    ledger_.find(soldier, local_player, channel, before);
    const auto decision =
        ledger_.observe_authority(soldier, local_player, channel, current_index, server_index, current_weapon, target_weapon, frontiers());
    selection_decision(soldier, {.request_turn = before.latest_request_turn,
                                 .epoch = decision.epoch,
                                 .sequence = decision.sequence,
                                 .actual_weapon = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(current_weapon)),
                                 .target_weapon = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(target_weapon)),
                                 .local_player = static_cast<std::int8_t>(local_player),
                                 .old_channel = static_cast<std::int8_t>(channel),
                                 .channel = static_cast<std::int8_t>(channel),
                                 .old_slot = static_cast<std::int8_t>(current_index),
                                 .target_slot = static_cast<std::int8_t>(server_index),
                                 .stage = 2,
                                 .verdict = static_cast<std::uint8_t>(decision.kind),
                                 .invalidated = static_cast<std::uint8_t>(decision.pointer_invalidated),
                                 .mute = static_cast<std::uint8_t>(decision.mute_select)});
    if (decision.pointer_invalidated) {
        invalidate_player(local_player);
        return;
    }

    // A matching channel acceptance retains packed history; rejection or correction clears it.
    packed_ledger_.observe_channel_result(local_player, decision);
    if (current_index != server_index && target_weapon != nullptr) {
        select_intent_.arm(target_weapon, channel, decision.mute_select);
    }

    update_presentation_activity();
}

void Handler::handle_packed_authority(void *soldier, int old_channel, int old_index, std::uint8_t server_packed) noexcept {
    select_intent_.clear();
    observe_lifecycle();
    if (!network_prediction_active()) {
        return;
    }

    const int server_index = packed_weapon_index(server_packed);
    const int server_channel = packed_weapon_channel(server_packed);
    const int local_player = local_player_index(soldier);
    if (!is_tracked_local_soldier(soldier, local_player)) {
        return;
    }
    if (!validate_player_history(soldier, local_player)) {
        return;
    }
    // Native signed none selections terminate this retained active-weapon ownership.
    if (old_channel < 0 || old_channel >= kWeaponChannelCount || server_channel < 0 || server_channel >= kWeaponChannelCount ||
        old_index < 0 || old_index >= kWeaponSlotCount || server_index < 0 || server_index >= kWeaponSlotCount) {
        invalidate_player(local_player);
        return;
    }
    if (old_channel == server_channel && old_index == server_index) {
        return;
    }

    auto *old_weapon = weapon_at(soldier, old_index);
    auto *target_weapon = weapon_at(soldier, server_index);
    if (old_weapon == nullptr || target_weapon == nullptr) {
        invalidate_player(local_player);
        return;
    }

    const auto observed = frontiers();

    // Start packed history from either weapon if it matches a retained channel's predicted final selection.
    if (!packed_ledger_.has_state(soldier, local_player)) {
        if (!track_matching_projection(soldier, local_player, old_channel, old_index, old_weapon, observed)) {
            track_matching_projection(soldier, local_player, server_channel, server_index, target_weapon, observed);
        }
    }

    // A conflicting old endpoint must fail open even when the target is known to its channel.
    PackedSnapshot before;
    packed_ledger_.find(soldier, local_player, before);
    const auto packed_decision = packed_ledger_.observe_transition(soldier, local_player, old_channel, old_index, old_weapon,
                                                                   server_channel, server_index, target_weapon, observed);
    if (packed_decision.pointer_invalidated) {
        invalidate_player(local_player);
    }

    const bool channel_duplicate = (before.soldier == nullptr || before.epoch != 0) &&
                                   ledger_.is_known_selection(soldier, local_player, server_channel, server_index, target_weapon);
    const bool mute = packed_decision.may_mute(channel_duplicate);
    selection_decision(soldier,
                       {.request_turn = before.request_turn,
                        .epoch = before.epoch,
                        .sequence = before.sequence,
                        .actual_weapon = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(old_weapon)),
                        .target_weapon = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(target_weapon)),
                        .local_player = static_cast<std::int8_t>(local_player),
                        .old_channel = static_cast<std::int8_t>(old_channel),
                        .channel = static_cast<std::int8_t>(server_channel),
                        .old_slot = static_cast<std::int8_t>(old_index),
                        .target_slot = static_cast<std::int8_t>(server_index),
                        .stage = 4,
                        .verdict = static_cast<std::uint8_t>((packed_decision.mute_select ? 1U : 0U) | (channel_duplicate ? 2U : 0U)),
                        .invalidated = static_cast<std::uint8_t>(packed_decision.pointer_invalidated),
                        .mute = static_cast<std::uint8_t>(mute)});
    select_intent_.arm(target_weapon, server_channel, mute);
}

} // namespace fc_plugins::weapon_swap_replay_fix
