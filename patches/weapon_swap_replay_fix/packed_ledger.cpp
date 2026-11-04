#include "packed_ledger.hpp"

namespace fc_plugins::weapon_swap_replay_fix {

void PackedSwapLedger::observe_frontiers(const TurnFrontiers &frontiers) noexcept {
    for (auto &state : players_) {
        if (state.soldier == nullptr) {
            continue;
        }

        advance(state, frontiers);
        if (turn_distance_greater(state.high_prediction, state.latest_request_turn, kReplayHistoryTurns)) {
            state = {};
        }
    }
}

bool PackedSwapLedger::track_projected(void *soldier, int local_player, int channel, int index, void *weapon, std::int32_t request_turn,
                                       std::uint32_t epoch, std::uint32_t sequence, const TurnFrontiers &frontiers) noexcept {
    if (!valid_owner(soldier, local_player) || !valid_key(channel, index) || weapon == nullptr || request_turn < 0 || epoch == 0 ||
        sequence == 0) {
        return false;
    }

    auto &state = players_[static_cast<std::size_t>(local_player)];
    const int projected = key(channel, index);
    // Sequence IDs order both channels at one turn; replay cannot replace a later request.
    // Retained histories span 32 turns, so modular sequence comparisons stay within half the counter range.
    if (state.soldier == soldier &&
        (turn_after(state.latest_request_turn, request_turn) || (state.latest_request_turn == request_turn && state.sequence != sequence &&
                                                                 std::bit_cast<std::int32_t>(sequence - state.sequence) <= 0))) {
        return false;
    }

    if (state.soldier != soldier || state.epoch != epoch) {
        start(state, soldier, projected, weapon, request_turn, epoch, sequence, frontiers);
        return true;
    }

    advance(state, frontiers);

    auto &known = state.node_weapons[static_cast<std::size_t>(projected)];
    if (known != nullptr && known != weapon) {
        start(state, soldier, projected, weapon, request_turn, epoch, sequence, frontiers);
        return true;
    }

    known = weapon;
    state.projected_key = static_cast<std::uint8_t>(projected);
    state.latest_request_turn = request_turn;
    state.sequence = sequence;
    state.settled = false;
    return true;
}

PackedDecision PackedSwapLedger::record_activation(void *soldier, int local_player, int old_channel, int old_index, void *old_weapon,
                                                   int target_channel, int target_index, void *target_weapon,
                                                   const TurnFrontiers &frontiers) noexcept {
    if (!valid_owner(soldier, local_player) || !valid_key(old_channel, old_index) || !valid_key(target_channel, target_index) ||
        old_weapon == nullptr || target_weapon == nullptr || frontiers.prediction < 0) {
        return {};
    }

    auto &state = players_[static_cast<std::size_t>(local_player)];
    const int source = key(old_channel, old_index);
    const int target = key(target_channel, target_index);
    if (source == target) {
        return {};
    }

    if (state.soldier != soldier) {
        start(state, soldier, target, target_weapon, frontiers.prediction, 0, 0, frontiers);
    }
    advance(state, frontiers);

    const auto *known_source = state.node_weapons[static_cast<std::size_t>(source)];
    const auto *known_target = state.node_weapons[static_cast<std::size_t>(target)];
    if ((known_source != nullptr && known_source != old_weapon) || (known_target != nullptr && known_target != target_weapon) ||
        activation_pointer_changed(state, source, old_weapon) || activation_pointer_changed(state, target, target_weapon)) {
        state = {};
        return {false, true};
    }

    for (const auto &activation : state.activations) {
        if (activation.turn == frontiers.prediction && activation.source == source && activation.target == target) {
            return {true, false};
        }
    }

    // An unexpected transition during replay invalidates the recorded selection.
    if (turn_after(state.high_prediction, frontiers.prediction)) {
        state = {};
        return {};
    }

    // Only a new occurrence replaces the latest projection; exact replay above preserves later manual requests.
    start(state, soldier, target, target_weapon, frontiers.prediction, 0, 0, frontiers);
    state.node_weapons[static_cast<std::size_t>(source)] = old_weapon;
    state.activations[state.next_activation] = {frontiers.prediction, static_cast<std::uint8_t>(source), static_cast<std::uint8_t>(target),
                                                old_weapon, target_weapon};
    state.next_activation = (state.next_activation + 1) % state.activations.size();
    return {};
}

PackedDecision PackedSwapLedger::observe_transition(void *soldier, int local_player, int old_channel, int old_index, void *old_weapon,
                                                    int target_channel, int target_index, void *target_weapon,
                                                    const TurnFrontiers &frontiers) noexcept {
    if (!valid_owner(soldier, local_player) || !valid_key(old_channel, old_index) || !valid_key(target_channel, target_index) ||
        old_weapon == nullptr || target_weapon == nullptr) {
        return {};
    }

    auto &state = players_[static_cast<std::size_t>(local_player)];
    if (state.soldier != soldier) {
        return {};
    }

    advance(state, frontiers);

    const int old_key = key(old_channel, old_index);
    const int target_key = key(target_channel, target_index);
    const bool old_recorded = state.node_weapons[static_cast<std::size_t>(old_key)] != nullptr;
    const bool target_recorded = state.node_weapons[static_cast<std::size_t>(target_key)] != nullptr;
    const bool pointer_invalidated = (old_recorded && state.node_weapons[static_cast<std::size_t>(old_key)] != old_weapon) ||
                                     (target_recorded && state.node_weapons[static_cast<std::size_t>(target_key)] != target_weapon) ||
                                     activation_pointer_changed(state, old_key, old_weapon) ||
                                     activation_pointer_changed(state, target_key, target_weapon);
    const bool new_endpoint = old_recorded != target_recorded;

    if (pointer_invalidated) {
        state = {};
        return {false, true};
    }

    // Earlier automatic occurrences can replay while a newer manual request owns the current projection.
    for (const auto &activation : state.activations) {
        if (activation.turn >= 0 && old_key == activation.target && target_key == activation.source &&
            turn_after(activation.turn, frontiers.update)) {
            return {true, false};
        }
    }

    // Pending manual history may learn one endpoint; settled history requires both recorded manual weapons.
    if (state.epoch == 0 || (!old_recorded && !target_recorded) || (state.settled && new_endpoint)) {
        state = {};
        return {};
    }

    state.node_weapons[static_cast<std::size_t>(old_key)] = old_weapon;
    state.node_weapons[static_cast<std::size_t>(target_key)] = target_weapon;
    return {true, false};
}

PackedDecision PackedSwapLedger::observe_sync(void *soldier, int local_player, int channel, int index, void *weapon,
                                              const TurnFrontiers &frontiers) noexcept {
    if (!valid_owner(soldier, local_player) || !valid_key(channel, index) || weapon == nullptr) {
        return {};
    }

    auto &state = players_[static_cast<std::size_t>(local_player)];
    if (state.soldier != soldier) {
        return {};
    }

    advance(state, frontiers);

    const int observed_key = key(channel, index);
    const auto *known = state.node_weapons[static_cast<std::size_t>(observed_key)];
    if (known == weapon) {
        return {true, false};
    }

    const bool pointer_invalidated = known != nullptr;
    state = {};
    return {false, pointer_invalidated};
}

void PackedSwapLedger::observe_channel_result(int local_player, const AuthorityDecision &decision) noexcept {
    if (local_player < 0 || local_player >= kLocalPlayerCount) {
        return;
    }

    auto &state = players_[static_cast<std::size_t>(local_player)];
    if (state.soldier == nullptr || state.epoch != decision.epoch || state.sequence != decision.sequence) {
        return;
    }

    // Channel acceptance keeps the predicted active weapon available for later packed rollback.
    if (decision.kind == AuthorityKind::Accepted) {
        state.settled = true;
    } else if (decision.kind == AuthorityKind::Rejected || decision.kind == AuthorityKind::Correction) {
        state = {};
    }
}

bool PackedSwapLedger::resolve(void *soldier, int local_player, int actual_channel, int actual_index, void *actual_weapon,
                               PackedSnapshot &snapshot) noexcept {
    snapshot = {};
    if (!valid_owner(soldier, local_player) || !valid_key(actual_channel, actual_index) || actual_weapon == nullptr) {
        return false;
    }

    auto &state = players_[static_cast<std::size_t>(local_player)];
    if (state.soldier != soldier) {
        return false;
    }

    snapshot = make_snapshot(state);
    const int actual_key = key(actual_channel, actual_index);

    // Presentation may differ only while the native selection still names a recorded weapon.
    if (snapshot.node_weapons[static_cast<std::size_t>(actual_key)] != actual_weapon) {
        state = {};
        return false;
    }

    return actual_key != state.projected_key;
}

bool PackedSwapLedger::has_state(void *soldier, int local_player) const noexcept {
    if (!valid_owner(soldier, local_player)) {
        return false;
    }

    const auto &state = players_[static_cast<std::size_t>(local_player)];
    return state.soldier == soldier;
}

bool PackedSwapLedger::find(void *soldier, int local_player, PackedSnapshot &snapshot) const noexcept {
    snapshot = {};
    if (!has_state(soldier, local_player)) {
        return false;
    }

    snapshot = make_snapshot(players_[static_cast<std::size_t>(local_player)]);
    return true;
}

void PackedSwapLedger::clear_player(int local_player) noexcept {
    if (local_player >= 0 && local_player < kLocalPlayerCount) {
        players_[static_cast<std::size_t>(local_player)] = {};
    }
}

void PackedSwapLedger::clear_epoch(void *soldier, int local_player, std::uint32_t epoch) noexcept {
    if (!valid_owner(soldier, local_player) || epoch == 0) {
        return;
    }

    auto &state = players_[static_cast<std::size_t>(local_player)];
    if (state.soldier == soldier && state.epoch == epoch) {
        state = {};
    }
}

void PackedSwapLedger::clear() noexcept {
    players_ = {};
}

bool PackedSwapLedger::has_active() const noexcept {
    for (const auto &state : players_) {
        if (state.soldier != nullptr) {
            return true;
        }
    }
    return false;
}

bool PackedSwapLedger::valid_owner(void *soldier, int local_player) noexcept {
    return soldier != nullptr && local_player >= 0 && local_player < kLocalPlayerCount;
}

bool PackedSwapLedger::valid_key(int channel, int index) noexcept {
    return channel >= 0 && channel < kWeaponChannelCount && index >= 0 && index < kWeaponSlotCount;
}

void PackedSwapLedger::advance(PlayerState &state, const TurnFrontiers &frontiers) noexcept {
    advance_turn(state.high_prediction, frontiers.prediction);
    for (auto &activation : state.activations) {
        if (turn_distance_greater(state.high_prediction, activation.turn, kReplayHistoryTurns)) {
            activation = {};
        }
    }
}

bool PackedSwapLedger::activation_pointer_changed(const PlayerState &state, int key, void *weapon) noexcept {
    for (const auto &activation : state.activations) {
        if (activation.turn >= 0 && ((activation.source == key && activation.source_weapon != weapon) ||
                                     (activation.target == key && activation.target_weapon != weapon))) {
            return true;
        }
    }
    return false;
}

void PackedSwapLedger::start(PlayerState &state, void *soldier, int projected_key, void *weapon, std::int32_t request_turn,
                             std::uint32_t epoch, std::uint32_t sequence, const TurnFrontiers &frontiers) noexcept {
    // Manual projection changes retain automatic occurrences under the same inventory owner and replay window.
    auto activations = state.activations;
    const auto next_activation = state.next_activation;
    const auto high_prediction = state.high_prediction;
    const bool retain_activations = state.soldier == soldier && !activation_pointer_changed(state, projected_key, weapon);
    state = {};
    if (retain_activations) {
        state.activations = activations;
        state.next_activation = next_activation;
        state.high_prediction = high_prediction;
    }
    state.soldier = soldier;
    state.projected_key = static_cast<std::uint8_t>(projected_key);
    state.node_weapons[static_cast<std::size_t>(projected_key)] = weapon;
    state.latest_request_turn = request_turn;
    state.epoch = epoch;
    state.sequence = sequence;

    advance(state, frontiers);
}

PackedSnapshot PackedSwapLedger::make_snapshot(const PlayerState &state) noexcept {
    PackedSnapshot snapshot{state.soldier, state.node_weapons, state.projected_key, state.latest_request_turn, state.epoch, state.sequence};
    for (const auto &activation : state.activations) {
        if (activation.turn >= 0) {
            snapshot.node_weapons[activation.source] = activation.source_weapon;
            snapshot.node_weapons[activation.target] = activation.target_weapon;
        }
    }
    return snapshot;
}

} // namespace fc_plugins::weapon_swap_replay_fix
