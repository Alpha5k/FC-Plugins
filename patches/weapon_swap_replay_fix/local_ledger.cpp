#include "local_ledger.hpp"

namespace fc_plugins::weapon_swap_replay_fix {

void LocalSwapLedger::observe_frontiers(const TurnFrontiers &frontiers) noexcept {
    for (auto &epoch : epochs_) {
        if (epoch.soldier == nullptr) {
            continue;
        }

        advance(epoch, frontiers);
        if (turn_distance_greater(epoch.high_prediction, epoch.latest_request_turn, kReplayHistoryTurns)) {
            epoch = {};
        }
    }
}

RequestDecision LocalSwapLedger::record_local(void *soldier, int local_player, int channel, int old_index, int target_index,
                                              void *old_weapon, void *target_weapon, std::int32_t request_turn,
                                              const TurnFrontiers &frontiers) noexcept {
    if (!valid_owner(soldier, local_player, channel) || !valid_index(old_index) || !valid_index(target_index) ||
        old_index == target_index || old_weapon == nullptr || target_weapon == nullptr || request_turn < 0) {
        return {};
    }

    auto &epoch = epoch_at(local_player, channel);
    if (epoch.soldier != soldier) {
        // Start this soldier's history with the old and requested weapons so later pointer changes can be detected.
        return start(epoch, soldier, old_index, target_index, old_weapon, target_weapon, request_turn, frontiers);
    }

    advance(epoch, frontiers);
    const bool historical = turn_after(epoch.high_prediction, request_turn);

    // A recorded input repeats only its original target; known pointers must still agree.
    for (std::size_t index = 0; index < epoch.step_count; ++index) {
        if (epoch.request_turns[index] != request_turn) {
            continue;
        }

        if (epoch.request_targets[index] != target_index) {
            return abandon(epoch, RequestKind::HistoricalConflict);
        }

        auto &known_old = epoch.node_weapons[static_cast<std::size_t>(old_index)];
        auto &known_target = epoch.node_weapons[static_cast<std::size_t>(target_index)];
        if ((known_old != nullptr && known_old != old_weapon) || (known_target != nullptr && known_target != target_weapon)) {
            if (historical) {
                return abandon(epoch, RequestKind::HistoricalConflict);
            }

            return start(epoch, soldier, old_index, target_index, old_weapon, target_weapon, request_turn, frontiers);
        }

        known_old = old_weapon;
        known_target = target_weapon;
        return {RequestKind::Replay, epoch.id};
    }

    // An older unrecorded turn counts as replay only when both weapons are already known.
    if (historical) {
        if (matches_node(epoch, old_index, old_weapon) && matches_node(epoch, target_index, target_weapon)) {
            return {RequestKind::Replay, epoch.id};
        }

        return abandon(epoch, RequestKind::HistoricalConflict);
    }

    // Continue a request chain only with a newer input turn; other turns start a separate history.
    if (!turn_after(request_turn, epoch.latest_request_turn)) {
        return start(epoch, soldier, old_index, target_index, old_weapon, target_weapon, request_turn, frontiers);
    }

    if (old_index != epoch.final_index || epoch.node_weapons[static_cast<std::size_t>(old_index)] != old_weapon) {
        return start(epoch, soldier, old_index, target_index, old_weapon, target_weapon, request_turn, frontiers);
    }

    if (epoch.step_count == epoch.request_turns.size()) {
        return start(epoch, soldier, old_index, target_index, old_weapon, target_weapon, request_turn, frontiers);
    }

    auto &known_target = epoch.node_weapons[static_cast<std::size_t>(target_index)];
    if (known_target != nullptr && known_target != target_weapon) {
        return start(epoch, soldier, old_index, target_index, old_weapon, target_weapon, request_turn, frontiers);
    }

    // Extend the predicted selection from its previous final weapon and restart the settlement delay.
    known_target = target_weapon;
    epoch.request_targets[epoch.step_count] = static_cast<std::uint8_t>(target_index);
    epoch.request_turns[epoch.step_count++] = request_turn;
    epoch.final_index = static_cast<std::uint8_t>(target_index);
    epoch.latest_request_turn = request_turn;
    epoch.latest_sequence = next_nonzero(next_sequence_);
    epoch.settlement_start = -1;
    return {RequestKind::New, epoch.id};
}

AuthorityDecision LocalSwapLedger::observe_authority(void *soldier, int local_player, int channel, int current_index, int server_index,
                                                     void *current_weapon, void *server_weapon, const TurnFrontiers &frontiers) noexcept {
    if (!valid_owner(soldier, local_player, channel) || !valid_index(current_index) || !valid_index(server_index) ||
        current_weapon == nullptr || server_weapon == nullptr) {
        return {};
    }

    auto &epoch = epoch_at(local_player, channel);
    if (epoch.soldier != soldier) {
        return {};
    }

    advance(epoch, frontiers);

    const auto *known_current = epoch.node_weapons[static_cast<std::size_t>(current_index)];
    const auto *known_server = epoch.node_weapons[static_cast<std::size_t>(server_index)];

    // An unknown slot or changed weapon pointer ends this history and leaves the server selection's effects unchanged.
    const bool pointer_invalidated =
        (known_current != nullptr && known_current != current_weapon) || (known_server != nullptr && known_server != server_weapon);
    if (known_current == nullptr || known_server == nullptr || pointer_invalidated) {
        const auto id = epoch.id;
        const auto sequence = epoch.latest_sequence;
        epoch = {};
        return {AuthorityKind::Correction, id, sequence, false, pointer_invalidated};
    }

    // Start the delay when update and input acknowledgement counters reach the request turn; require more than two later turns.
    // The delay uses client update turns, which also advance during prediction.
    if (epoch.settlement_start < 0 && turn_at_or_after(epoch.high_update, epoch.latest_request_turn) &&
        turn_at_or_after(epoch.high_acknowledged, epoch.latest_request_turn)) {
        epoch.settlement_start = epoch.high_update;
    }

    const bool mismatch = current_index != server_index;
    const bool settled = epoch.settlement_start >= 0 && turn_distance_greater(epoch.high_update, epoch.settlement_start, kSettlementTurns);
    const auto id = epoch.id;
    const auto sequence = epoch.latest_sequence;
    if (!settled) {
        return {AuthorityKind::Pending, id, sequence, mismatch, false};
    }

    // After the delay, classify a known server selection by whether it matches the predicted final weapon.
    if (server_index == epoch.final_index) {
        epoch = {};
        return {AuthorityKind::Accepted, id, sequence, mismatch, false};
    }

    epoch = {};
    return {AuthorityKind::Rejected, id, sequence, false, false};
}

bool LocalSwapLedger::is_known_selection(void *soldier, int local_player, int channel, int target_index,
                                         void *target_weapon) const noexcept {
    if (!valid_owner(soldier, local_player, channel) || !valid_index(target_index) || target_weapon == nullptr) {
        return false;
    }

    const auto &epoch = epoch_at(local_player, channel);
    return epoch.soldier == soldier && epoch.node_weapons[static_cast<std::size_t>(target_index)] == target_weapon;
}

bool LocalSwapLedger::resolve(void *soldier, int local_player, int channel, int actual_index, EpochSnapshot &snapshot) noexcept {
    snapshot = {};
    if (!valid_owner(soldier, local_player, channel) || !valid_index(actual_index)) {
        return false;
    }

    auto &epoch = epoch_at(local_player, channel);
    if (epoch.soldier != soldier) {
        return false;
    }

    // Keep the snapshot available even when the native selection already matches the predicted weapon.
    snapshot = make_snapshot(epoch);
    if (actual_index == epoch.final_index) {
        return false;
    }

    if (epoch.node_weapons[static_cast<std::size_t>(actual_index)] != nullptr) {
        return true;
    }

    epoch = {};
    return false;
}

bool LocalSwapLedger::find(void *soldier, int local_player, int channel, EpochSnapshot &snapshot) const noexcept {
    snapshot = {};
    if (!valid_owner(soldier, local_player, channel)) {
        return false;
    }

    const auto &epoch = epoch_at(local_player, channel);
    if (epoch.soldier != soldier) {
        return false;
    }

    snapshot = make_snapshot(epoch);
    return true;
}

void LocalSwapLedger::clear_slot(void *soldier, int local_player, int channel) noexcept {
    if (!valid_owner(soldier, local_player, channel)) {
        return;
    }

    auto &epoch = epoch_at(local_player, channel);
    if (epoch.soldier == soldier) {
        epoch = {};
    }
}

void LocalSwapLedger::clear_player(int local_player) noexcept {
    if (local_player < 0 || local_player >= kLocalPlayerCount) {
        return;
    }

    for (int channel = 0; channel < kWeaponChannelCount; ++channel) {
        epoch_at(local_player, channel) = {};
    }
}

void LocalSwapLedger::clear() noexcept {
    epochs_ = {};
}

bool LocalSwapLedger::has_active() const noexcept {
    for (const auto &epoch : epochs_) {
        if (epoch.soldier != nullptr) {
            return true;
        }
    }
    return false;
}

bool LocalSwapLedger::valid_owner(void *soldier, int local_player, int channel) noexcept {
    return soldier != nullptr && local_player >= 0 && local_player < kLocalPlayerCount && channel >= 0 && channel < kWeaponChannelCount;
}

bool LocalSwapLedger::valid_index(int index) noexcept {
    return index >= 0 && index < kWeaponSlotCount;
}

void LocalSwapLedger::advance(Epoch &epoch, const TurnFrontiers &frontiers) noexcept {
    advance_turn(epoch.high_update, frontiers.update);
    advance_turn(epoch.high_prediction, frontiers.prediction);
    advance_turn(epoch.high_acknowledged, frontiers.acknowledged);
}

std::uint32_t LocalSwapLedger::next_nonzero(std::uint32_t &value) noexcept {
    if (++value == 0) {
        ++value;
    }

    return value;
}

bool LocalSwapLedger::matches_node(const Epoch &epoch, int index, void *weapon) noexcept {
    return epoch.node_weapons[static_cast<std::size_t>(index)] == weapon;
}

RequestDecision LocalSwapLedger::abandon(Epoch &epoch, RequestKind kind) noexcept {
    const auto id = epoch.id;
    epoch = {};
    return {kind, id};
}

RequestDecision LocalSwapLedger::start(Epoch &epoch, void *soldier, int old_index, int target_index, void *old_weapon, void *target_weapon,
                                       std::int32_t request_turn, const TurnFrontiers &frontiers) noexcept {
    epoch = {};
    epoch.soldier = soldier;
    epoch.final_index = static_cast<std::uint8_t>(target_index);
    epoch.node_weapons[static_cast<std::size_t>(old_index)] = old_weapon;
    epoch.node_weapons[static_cast<std::size_t>(target_index)] = target_weapon;
    epoch.latest_request_turn = request_turn;
    epoch.id = next_nonzero(next_epoch_);
    epoch.latest_sequence = next_nonzero(next_sequence_);
    epoch.request_turns[0] = request_turn;
    epoch.request_targets[0] = static_cast<std::uint8_t>(target_index);
    epoch.step_count = 1;

    advance(epoch, frontiers);
    return {RequestKind::New, epoch.id};
}

EpochSnapshot LocalSwapLedger::make_snapshot(const Epoch &epoch) noexcept {
    return {epoch.soldier, epoch.node_weapons, epoch.final_index, epoch.latest_request_turn, epoch.id, epoch.latest_sequence};
}

std::size_t LocalSwapLedger::slot(int local_player, int channel) noexcept {
    return static_cast<std::size_t>(local_player) * kWeaponChannelCount + static_cast<std::size_t>(channel);
}

LocalSwapLedger::Epoch &LocalSwapLedger::epoch_at(int local_player, int channel) noexcept {
    return epochs_[slot(local_player, channel)];
}

const LocalSwapLedger::Epoch &LocalSwapLedger::epoch_at(int local_player, int channel) const noexcept {
    return epochs_[slot(local_player, channel)];
}

} // namespace fc_plugins::weapon_swap_replay_fix
