#include "history.hpp"

#include <algorithm>
#include <bit>

namespace fc_plugins::hero_animation_fix {
namespace {

// Tests the five-bit combo ID range, independently of the loaded asset's number of states.
constexpr bool valid_state(int state) noexcept {
    return state >= 0 && state < 32;
}

// Computes a signed turn difference without overflowing C++ signed subtraction.
constexpr std::int32_t turn_difference(std::int32_t turn, std::int32_t reference) noexcept {
    const auto difference = static_cast<std::uint32_t>(turn) - static_cast<std::uint32_t>(reference);
    return std::bit_cast<std::int32_t>(difference);
}

// Compares valid nonnegative turns using the overflow-safe difference.
constexpr bool turn_before(std::int32_t turn, std::int32_t reference) noexcept {
    return turn >= 0 && reference >= 0 && turn_difference(turn, reference) < 0;
}

// Tests whether a valid turn follows the reference under the same difference rule.
constexpr bool turn_after(std::int32_t turn, std::int32_t reference) noexcept {
    return turn >= 0 && reference >= 0 && turn_difference(turn, reference) > 0;
}

} // namespace

hero_diagnostics::HistorySnapshot LocalHistory::snapshot(const HeroIdentity &identity) const noexcept {
    if (!identity.is_local()) {
        return {};
    }
    const auto &slot = slots_[identity.local_player];
    if (slot.identity != identity) {
        return {};
    }
    return {.base_turn = slot.base_turn,
            .last_turn = slot.count != 0 ? slot.path[slot.count - 1].turn : slot.base_turn,
            .base_state = slot.base_state,
            .last_state = slot.count != 0 ? slot.path[slot.count - 1].state : slot.base_state,
            .pending = slot.count};
}

hero_diagnostics::HistorySnapshot RemoteHistory::snapshot(const HeroIdentity &identity) const noexcept {
    if (!identity.is_remote()) {
        return {};
    }
    const auto &slot = slots_[static_cast<std::size_t>(identity.player_id)];
    if (slot.identity != identity) {
        return {};
    }
    return {.base_turn = slot.authority_turn,
            .last_turn = slot.count != 0 ? slot.path[slot.count - 1].turn : slot.authority_turn,
            .base_state = slot.authority_state,
            .last_state = slot.count != 0 ? slot.path[slot.count - 1].state : slot.authority_state,
            .pending = slot.count,
            .last_stale_idle_turn = slot.last_stale_idle_turn,
            .stale_idle_samples = slot.stale_idle_samples};
}

hero_diagnostics::HistorySnapshot LocalHistory::snapshot_weapon(std::uintptr_t weapon) const noexcept {
    for (const auto &slot : slots_) {
        if (slot.identity.weapon == weapon && weapon != 0) {
            return snapshot(slot.identity);
        }
    }
    return {};
}

hero_diagnostics::HistorySnapshot RemoteHistory::snapshot_weapon(std::uintptr_t weapon) const noexcept {
    for (const auto &slot : slots_) {
        if (slot.identity.weapon == weapon && weapon != 0) {
            return snapshot(slot.identity);
        }
    }
    return {};
}

bool LocalHistory::observe_prediction(const HeroIdentity &identity, int base_state, int target_state,
                                      std::int32_t prediction_turn) noexcept {
    if (!identity.is_local() || !valid_state(base_state) || !valid_state(target_state) || base_state == target_state ||
        prediction_turn <= 0) {
        return false;
    }

    auto &slot = bind(identity);

    // A different native combo or an earlier turn starts a fresh path from the state before EnterState.
    const auto predicted_state = slot.count == 0 ? slot.base_state : slot.path[slot.count - 1].state;
    if (slot.base_state < 0 || predicted_state != base_state ||
        (slot.count != 0 && turn_before(prediction_turn, slot.path[slot.count - 1].turn))) {
        seed(slot, identity, base_state, prediction_turn - 1);
    }

    if (slot.count == slot.path.size()) {
        // Discard ambiguous history when the bounded path cannot retain another transition.
        slot = {};
        return false;
    }

    slot.path[slot.count++] = {.turn = prediction_turn, .state = static_cast<std::int8_t>(target_state)};
    return true;
}

AuthorityAction LocalHistory::classify_authority(const HeroIdentity &identity, int current_state, int incoming_state,
                                                 std::int32_t acknowledged_turn, hero_diagnostics::HistoryDecision *evidence) noexcept {
    const auto observe = [evidence](std::uint8_t branch, int expected = -1, std::size_t prefix = 0, std::int32_t turn = -1) noexcept {
        if (evidence != nullptr) {
            evidence->branch = branch;
            evidence->expected = expected;
            evidence->selected_prefix = static_cast<std::uint8_t>(prefix);
            evidence->selected_turn = turn;
        }
    };
    if (!identity.is_local() || !valid_state(current_state) || !valid_state(incoming_state) || acknowledged_turn < 0) {
        observe(1);
        return AuthorityAction::Native;
    }

    auto &slot = bind(identity);

    // An earlier acknowledgement cannot confirm this path; accept authority as a fresh base instead.
    if (slot.base_state < 0 || turn_before(acknowledged_turn, slot.base_turn)) {
        observe(2, slot.base_state, 0, slot.base_turn);
        seed(slot, identity, incoming_state, acknowledged_turn);
        return current_state == incoming_state ? AuthorityAction::Native : AuthorityAction::Corrected;
    }

    // Native changes outside the retained path require authority to replace the pending predictions.
    if (slot.count != 0 && current_state != slot.path[slot.count - 1].state) {
        observe(3, slot.path[slot.count - 1].state, slot.count, slot.path[slot.count - 1].turn);
        seed(slot, identity, incoming_state, acknowledged_turn);
        return AuthorityAction::Corrected;
    }

    // Advance through turns before this acknowledgement, then inspect every transition produced on the same turn.
    auto expected = static_cast<int>(slot.base_state);
    std::size_t acknowledged{};
    while (acknowledged < slot.count && turn_before(slot.path[acknowledged].turn, acknowledged_turn)) {
        expected = slot.path[acknowledged].state;
        ++acknowledged;
    }

    const auto accept_match = [&](std::size_t count) noexcept {
        observe(count < slot.count ? 4 : 5, expected, count, count != 0 ? slot.path[count - 1].turn : slot.base_turn);
        // A matching prefix is historical while later predictions remain; preserve their presentation path.
        if (count < slot.count) {
            advance(slot, count, expected, acknowledged_turn);
            return AuthorityAction::Historical;
        }

        const auto action = current_state == incoming_state ? AuthorityAction::Native : AuthorityAction::Corrected;
        seed(slot, identity, incoming_state, acknowledged_turn);
        return action;
    };

    const auto same_turn_matches =
        std::count_if(slot.path.begin() + acknowledged, slot.path.begin() + slot.count, [=](const Transition &entry) {
            return entry.turn == acknowledged_turn && entry.state == incoming_state;
        });
    if (same_turn_matches > 1 || (same_turn_matches != 0 && incoming_state == expected)) {
        observe(6, expected, acknowledged, acknowledged_turn);
        seed(slot, identity, incoming_state, acknowledged_turn);
        return AuthorityAction::Corrected;
    }

    // Native Update can enter several combos in one turn. After rejecting ambiguous matches,
    // accept the state before those entries or an unambiguous same-turn prefix.
    if (incoming_state == expected) {
        return accept_match(acknowledged);
    }

    while (acknowledged < slot.count && slot.path[acknowledged].turn == acknowledged_turn) {
        expected = slot.path[acknowledged].state;
        ++acknowledged;
        if (incoming_state == expected) {
            return accept_match(acknowledged);
        }
    }

    observe(6, expected, acknowledged, acknowledged != 0 ? slot.path[acknowledged - 1].turn : slot.base_turn);

    // An unmatched state is a correction and becomes the new authority base for subsequent predictions.
    seed(slot, identity, incoming_state, acknowledged_turn);
    return AuthorityAction::Corrected;
}

void LocalHistory::clear(const HeroIdentity &identity) noexcept {
    if (identity.is_local() && slots_[identity.local_player].identity == identity) {
        slots_[identity.local_player] = {};
    }
}

void LocalHistory::clear_weapon(std::uintptr_t weapon) noexcept {
    for (auto &slot : slots_) {
        if (slot.identity.weapon == weapon) {
            slot = {};
        }
    }
}

void LocalHistory::clear_all() noexcept {
    slots_ = {};
}

LocalHistory::Slot &LocalHistory::bind(const HeroIdentity &identity) noexcept {
    auto &slot = slots_[identity.local_player];
    if (slot.identity != identity) {
        slot = {};
        slot.identity = identity;
    }

    return slot;
}

void LocalHistory::seed(Slot &slot, const HeroIdentity &identity, int state, std::int32_t turn) noexcept {
    slot = {};
    slot.identity = identity;

    slot.base_state = static_cast<std::int8_t>(state);
    slot.base_turn = turn;
}

void LocalHistory::advance(Slot &slot, std::size_t count, int state, std::int32_t turn) noexcept {
    if (count != 0) {
        std::shift_left(slot.path.begin(), slot.path.begin() + slot.count, static_cast<std::ptrdiff_t>(count));
        slot.count = static_cast<std::uint8_t>(slot.count - count);
    }

    slot.base_state = static_cast<std::int8_t>(state);
    slot.base_turn = turn;
}

bool RemoteHistory::observe_prediction(const HeroIdentity &identity, int base_state, int target_state, std::int32_t update_turn) noexcept {
    if (!identity.is_remote() || !valid_state(base_state) || !valid_state(target_state) || base_state == target_state || update_turn < 0) {
        return false;
    }

    auto &slot = bind(identity);

    // Extend the last retained combo, starting from native state when the path or turn order changes.
    if (slot.authority_state < 0) {
        seed_authority(slot, base_state, update_turn);
    } else if (expected_state(slot) != base_state || (slot.count != 0 && turn_before(update_turn, slot.path[slot.count - 1].turn))) {
        clear_pending(slot);
        seed_authority(slot, base_state, update_turn);
    }

    if (slot.count == slot.path.size()) {
        // Drop pending predictions when the bounded path is full, retaining the authority base.
        clear_pending(slot);
        return false;
    }

    slot.path[slot.count++] = {
        .turn = update_turn,
        .state = static_cast<std::int8_t>(target_state),
        .starts_action = base_state == 0 && target_state != 0,
    };
    return true;
}

AuthorityAction RemoteHistory::classify_authority(const HeroIdentity &identity, int current_state, int incoming_state,
                                                  std::int32_t update_turn, hero_diagnostics::HistoryDecision *evidence) noexcept {
    const auto observe = [evidence](std::uint8_t branch, int expected = -1, std::size_t prefix = 0, std::int32_t turn = -1) noexcept {
        if (evidence != nullptr) {
            evidence->branch = branch;
            evidence->expected = expected;
            evidence->selected_prefix = static_cast<std::uint8_t>(prefix);
            evidence->selected_turn = turn;
        }
    };
    if (!identity.is_remote() || !valid_state(current_state) || !valid_state(incoming_state) || update_turn < 0) {
        observe(11);
        return AuthorityAction::Native;
    }

    auto &slot = bind(identity);

    // Remote turns can reset or move backward. Rebase the combo path without retaining presentation.
    if (slot.authority_state >= 0 && turn_before(update_turn, slot.authority_turn)) {
        observe(12, slot.authority_state, 0, slot.authority_turn);
        clear_pending(slot);
        seed_authority(slot, incoming_state, update_turn);
        return current_state == incoming_state ? AuthorityAction::Native : AuthorityAction::Corrected;
    }

    if (slot.authority_state < 0 || slot.count == 0) {
        observe(13, slot.authority_state, slot.count, slot.authority_turn);
        seed_authority(slot, incoming_state, update_turn);
        return current_state == incoming_state ? AuthorityAction::Native : AuthorityAction::Corrected;
    }

    // Native changes outside this path invalidate even a repeated authority base.
    if (current_state != expected_state(slot)) {
        observe(14, expected_state(slot), slot.count, slot.path[slot.count - 1].turn);
        clear_pending(slot);
        seed_authority(slot, incoming_state, update_turn);
        return current_state == incoming_state ? AuthorityAction::Native : AuthorityAction::Corrected;
    }

    // A repeated server combo can precede pending predictions; remote matching remains a combo-ID heuristic.
    if (incoming_state == slot.authority_state) {
        slot.authority_turn = update_turn;
        // A preceding authority sample cannot disprove a native occurrence predicted at a later turn.
        if (turn_before(update_turn, slot.path[0].turn)) {
            observe(23, slot.authority_state, 0, slot.authority_turn);
            return AuthorityAction::Historical;
        }
        if (has_unconfirmed_start(slot) && incoming_state == 0) {
            // Match incoming idle to the first predicted return to idle, consuming that prefix and keeping later combos.
            const auto completion = find_pending(slot, 0);
            const auto completions = std::count_if(slot.path.begin(), slot.path.begin() + slot.count, [](const Transition &entry) {
                return entry.state == 0;
            });
            if (completions > 1) {
                observe(21, expected_state(slot), slot.count, update_turn);
                clear_pending(slot);
                seed_authority(slot, incoming_state, update_turn);
                return AuthorityAction::Corrected;
            }
            if (completion < slot.count) {
                observe(15, slot.path[completion].state, completion + 1, slot.path[completion].turn);
                consume(slot, completion + 1);
                slot.stale_idle_samples = 0;
                slot.last_stale_idle_turn = -1;
                return current_state == 0 ? AuthorityAction::Native : AuthorityAction::Historical;
            }
        }

        if (incoming_state == current_state) {
            observe(16, slot.authority_state, 0, slot.authority_turn);
            clear_pending(slot);
            return AuthorityAction::Native;
        }

        if (has_unconfirmed_start(slot)) {
            // Idle at or before the action start remains historical. One distinct later update turn
            // is also tolerated; another later turn corrects, while repeats at the same turn share tolerance.
            const auto start_turn = slot.path[0].turn;
            if (!turn_after(update_turn, start_turn) || slot.last_stale_idle_turn == update_turn) {
                observe(17, slot.path[0].state, 0, start_turn);
                return AuthorityAction::Historical;
            }

            slot.last_stale_idle_turn = update_turn;
            if (slot.stale_idle_samples++ == 0) {
                observe(18, slot.path[0].state, 0, start_turn);
                return AuthorityAction::Historical;
            }

            observe(19, slot.path[0].state, 0, start_turn);
            clear_pending(slot);
            seed_authority(slot, incoming_state, update_turn);
            return AuthorityAction::Corrected;
        }

        observe(20, slot.authority_state, 0, slot.authority_turn);

        clear_pending(slot);
        seed_authority(slot, incoming_state, update_turn);
        return AuthorityAction::Corrected;
    }

    // A unique remote prediction can confirm a prefix; repeated IDs leave the occurrence ambiguous.
    const auto match = find_pending(slot, incoming_state);
    const auto matches = std::count_if(slot.path.begin(), slot.path.begin() + slot.count, [incoming_state](const Transition &entry) {
        return entry.state == incoming_state;
    });
    if (match == slot.count || matches != 1) {
        observe(21, expected_state(slot), slot.count, slot.path[slot.count - 1].turn);
        // Unmatched authority replaces the pending combo path.
        clear_pending(slot);
        seed_authority(slot, incoming_state, update_turn);
        return current_state == incoming_state ? AuthorityAction::Native : AuthorityAction::Corrected;
    }

    observe(22, slot.path[match].state, match + 1, slot.path[match].turn);

    // Confirm through the matched combo and preserve predictions after it.
    slot.authority_state = static_cast<std::int8_t>(incoming_state);
    slot.authority_turn = update_turn;
    consume(slot, match + 1);
    slot.stale_idle_samples = 0;
    slot.last_stale_idle_turn = -1;

    return slot.count == 0 && current_state == incoming_state ? AuthorityAction::Native : AuthorityAction::Historical;
}

void RemoteHistory::clear(const HeroIdentity &identity) noexcept {
    if (identity.is_remote() && slots_[identity.player_id].identity == identity) {
        slots_[identity.player_id] = {};
    }
}

void RemoteHistory::clear_weapon(std::uintptr_t weapon) noexcept {
    for (auto &slot : slots_) {
        if (slot.identity.weapon == weapon) {
            slot = {};
        }
    }
}

void RemoteHistory::clear_all() noexcept {
    slots_ = {};
}

RemoteHistory::Slot &RemoteHistory::bind(const HeroIdentity &identity) noexcept {
    auto &slot = slots_[identity.player_id];
    if (slot.identity != identity) {
        slot = {};
        slot.identity = identity;
    }

    return slot;
}

void RemoteHistory::seed_authority(Slot &slot, int state, std::int32_t turn) noexcept {
    slot.authority_state = static_cast<std::int8_t>(state);
    slot.authority_turn = turn;
    slot.stale_idle_samples = 0;
    slot.last_stale_idle_turn = -1;
}

void RemoteHistory::clear_pending(Slot &slot) noexcept {
    // Only the first count transitions are live. Clearing them keeps the accepted combo.
    slot.count = 0;
    slot.stale_idle_samples = 0;
    slot.last_stale_idle_turn = -1;
}

void RemoteHistory::consume(Slot &slot, std::size_t count) noexcept {
    std::shift_left(slot.path.begin(), slot.path.begin() + slot.count, static_cast<std::ptrdiff_t>(count));
    slot.count = static_cast<std::uint8_t>(slot.count - count);
}

std::size_t RemoteHistory::find_pending(const Slot &slot, int state) noexcept {
    const auto end = slot.path.begin() + slot.count;
    return static_cast<std::size_t>(std::find_if(slot.path.begin(), end,
                                                 [state](const Transition &transition) {
                                                     return transition.state == state;
                                                 }) -
                                    slot.path.begin());
}

int RemoteHistory::expected_state(const Slot &slot) noexcept {
    return slot.count == 0 ? slot.authority_state : slot.path[slot.count - 1].state;
}

bool RemoteHistory::has_unconfirmed_start(const Slot &slot) noexcept {
    return slot.authority_state == 0 && slot.count != 0 && slot.path[0].starts_action;
}

} // namespace fc_plugins::hero_animation_fix
