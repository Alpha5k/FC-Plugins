#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace fc_plugins::weapon_swap_replay_fix {

inline constexpr int kLocalPlayerCount = 2;
inline constexpr int kWeaponChannelCount = 2;
inline constexpr int kWeaponSlotCount = 8;
inline constexpr std::size_t kMoveHistoryTurnCount = 32;

// Requires both adjacent samples to precede the native next-write turn and remain in its 32-turn ring.
constexpr bool move_pair_resident(std::int32_t next_write, std::int32_t prediction) noexcept {
    const auto age = static_cast<std::int64_t>(next_write) - prediction;
    return prediction > 0 && age >= 1 && age < static_cast<std::int64_t>(kMoveHistoryTurnCount);
}

// Qualifies populated native input slots by exact turn and native player ID, including absent/reassigned local slots.
class RecordedMoves final {
  public:
    // Called after each SubmitMove slot, before its next-write counter advances; missing input invalidates that turn.
    void record(int local_player, std::int32_t turn, int player_id, bool populated) noexcept {
        if (local_player < 0 || local_player >= kLocalPlayerCount || turn < 0) {
            return;
        }

        auto &player = players_[static_cast<std::size_t>(local_player)];
        if (player.player_id != player_id) {
            player = {};
            player.player_id = player_id;
        }
        player.turns[static_cast<std::size_t>(turn) & (kMoveHistoryTurnCount - 1)] = populated && player_id >= 0 ? turn : -1;
    }

    // Refuses unwritten, overwritten or previous-owner samples before the handler reads their buttons.
    bool has_pair(int local_player, int player_id, std::int32_t next_write, std::int32_t prediction) const noexcept {
        if (local_player < 0 || local_player >= kLocalPlayerCount || player_id < 0 || !move_pair_resident(next_write, prediction)) {
            return false;
        }

        const auto &player = players_[static_cast<std::size_t>(local_player)];
        return player.player_id == player_id &&
               player.turns[static_cast<std::size_t>(prediction) & (kMoveHistoryTurnCount - 1)] == prediction &&
               player.turns[static_cast<std::size_t>(prediction - 1) & (kMoveHistoryTurnCount - 1)] == prediction - 1;
    }

    // Invalidates this local slot after its soldier resets or is destroyed.
    void clear_player(int local_player) noexcept {
        if (local_player >= 0 && local_player < kLocalPlayerCount) {
            players_[static_cast<std::size_t>(local_player)] = {};
        }
    }

    // Invalidates input provenance when the FC session or native prediction lifecycle ends.
    void clear() noexcept {
        players_ = {};
    }

  private:
    struct Player {
        int player_id{-1};
        std::array<std::int32_t, kMoveHistoryTurnCount> turns = [] {
            std::array<std::int32_t, kMoveHistoryTurnCount> initial_turns;
            initial_turns.fill(-1);
            return initial_turns;
        }();
    };

    std::array<Player, kLocalPlayerCount> players_{};
};

// PlayerController stores switch direction as raw +/-1.0 float bits and assigns each channel distinct buttons.
inline constexpr std::uint32_t kPositiveDirection = std::bit_cast<std::uint32_t>(1.0F);
inline constexpr std::uint32_t kNegativeDirection = std::bit_cast<std::uint32_t>(-1.0F);
inline constexpr std::array<std::uint32_t, kWeaponChannelCount> kPositiveButtons{0x2000U, 0x8000U};
inline constexpr std::array<std::uint32_t, kWeaponChannelCount> kNegativeButtons{0x4000U, 0x1'0000U};

// Decodes the synchronized selection byte's signed four-bit inventory slot.
constexpr int packed_weapon_index(std::uint8_t packed) noexcept {
    const auto index = static_cast<int>(packed & 0x0F);
    return (index & 0x08) == 0 ? index : index - 16;
}

// Decodes the synchronized selection byte's signed two-bit weapon channel.
constexpr int packed_weapon_channel(std::uint8_t packed) noexcept {
    const auto channel = static_cast<int>((packed >> 4) & 0x03);
    return (channel & 0x02) == 0 ? channel : channel - 4;
}

// Captures client simulation and input acknowledgement counters used to retain and settle weapon requests.
struct TurnFrontiers {
    // Client update turn; also advances during prediction.
    std::int32_t update{-1};
    // Input turn currently being replayed.
    std::int32_t prediction{-1};
    // Latest local input turn acknowledged by the server update.
    std::int32_t acknowledged{-1};
};

// Subtracts modulo 32 bits, interpreting the result as a signed distance between recent turns.
constexpr std::int32_t turn_difference(std::int32_t turn, std::int32_t frontier) noexcept {
    const auto difference = static_cast<std::uint32_t>(turn) - static_cast<std::uint32_t>(frontier);
    return std::bit_cast<std::int32_t>(difference);
}

// Rejects unavailable frontiers before comparing recent turns.
constexpr bool turn_at_or_after(std::int32_t turn, std::int32_t frontier) noexcept {
    return turn >= 0 && frontier >= 0 && turn_difference(turn, frontier) >= 0;
}

// Tests whether a valid turn has advanced beyond a valid frontier.
constexpr bool turn_after(std::int32_t turn, std::int32_t frontier) noexcept {
    return turn >= 0 && frontier >= 0 && turn_difference(turn, frontier) > 0;
}

// Tests a strict turn interval while treating negative counters as unavailable.
constexpr bool turn_distance_greater(std::int32_t turn, std::int32_t frontier, std::int32_t distance) noexcept {
    if (turn < 0 || frontier < 0) {
        return false;
    }

    const auto difference = turn_difference(turn, frontier);
    return difference >= 0 && distance >= 0 && difference > distance;
}

// Keeps the highest observed turn when prediction replays older input.
inline void advance_turn(std::int32_t &high, std::int32_t observed) noexcept {
    if (observed >= 0 && (high < 0 || turn_after(observed, high))) {
        high = observed;
    }
}

// Classifies a local selection as new input, prediction replay, or unsafe history.
enum class RequestKind : std::uint8_t {
    Invalid,
    New,
    Replay,
    // A recorded target changed, or an older sample has unknown or replaced weapons; clear contradictory history.
    HistoricalConflict,
};

// Classifies a server selection as pending replay, accepted, rejected, or corrective.
enum class AuthorityKind : std::uint8_t {
    Native,
    Pending,
    Accepted,
    Rejected,
    Correction,
};

struct RequestDecision {
    RequestKind kind{RequestKind::Invalid};
    // ID of the related local request history, including history cleared by a conflict.
    std::uint32_t epoch{};

    bool valid() const noexcept {
        return kind != RequestKind::Invalid;
    }

    bool replay() const noexcept {
        return kind == RequestKind::Replay;
    }

    bool failed_open() const noexcept {
        return kind == RequestKind::HistoricalConflict;
    }
};

struct AuthorityDecision {
    AuthorityKind kind{AuthorityKind::Native};
    // IDs let the packed ledger apply this result only to the matching channel request.
    std::uint32_t epoch{};
    std::uint32_t sequence{};
    // Whether the next base Select should skip a change sound already played during prediction.
    bool mute_select{};
    // A recorded inventory slot now contains a different weapon pointer.
    bool pointer_invalidated{};
};

// Copies the predicted channel selection and borrowed pointers for the handler to validate before presentation.
struct EpochSnapshot {
    void *soldier{};
    // Recorded inventory slots; null means the history has not learned that slot.
    std::array<void *, kWeaponSlotCount> node_weapons{};
    // Predicted final inventory slot for this channel.
    std::uint8_t final_index{};
    std::int32_t latest_request_turn{-1};
    std::uint32_t epoch{};
    std::uint32_t sequence{};
};

// Remembers active prediction until all three native counters are nonpositive.
class NetworkLifecycle final {
  public:
    // Returns true when previously active prediction reaches the all-nonpositive reset state.
    bool observe(const TurnFrontiers &frontiers) noexcept {
        const bool active_now = frontiers.update > 0 && frontiers.prediction > 0 && frontiers.acknowledged >= 0;
        const bool reset_state = frontiers.update <= 0 && frontiers.prediction <= 0 && frontiers.acknowledged <= 0;

        // Partial counter resets keep the previous active state until all three meet the reset condition.
        const bool reset = active_ && reset_state;
        if (reset) {
            active_ = false;
        }

        if (active_now) {
            active_ = true;
        }

        return reset;
    }

    // Forgets whether active prediction has been observed.
    void clear() noexcept {
        active_ = false;
    }

    bool active() const noexcept {
        return active_;
    }

  private:
    bool active_{};
};

// Carries a one-use weapon/channel decision from a selection callback to its immediate base Weapon::Select call.
class SelectIntent final {
  public:
    // Records whether the following weapon/channel Select should mute its repeated change sound.
    void arm(void *weapon, int channel, bool duplicate) noexcept {
        weapon_ = weapon;
        channel_ = channel;
        duplicate_ = duplicate;
    }

    // Consumes the intent once; an unrelated weapon or channel cancels it.
    bool consume_duplicate(void *weapon, int channel) noexcept {
        const bool matched = weapon_ == weapon && channel_ == channel;
        const bool duplicate = matched && duplicate_;
        clear();
        return duplicate;
    }

    // Cancels the pending Select decision.
    void clear() noexcept {
        weapon_ = nullptr;
        channel_ = -1;
        duplicate_ = false;
    }

  private:
    void *weapon_{};
    int channel_{-1};
    bool duplicate_{};
};

// Returns the button mask for this channel and raw native float direction, or zero if either is invalid.
constexpr std::uint32_t switch_button_mask(int channel, std::uint32_t direction) noexcept {
    if (channel < 0 || channel >= kWeaponChannelCount) {
        return 0;
    }

    if (direction == kPositiveDirection) {
        return kPositiveButtons[static_cast<std::size_t>(channel)];
    }

    if (direction == kNegativeDirection) {
        return kNegativeButtons[static_cast<std::size_t>(channel)];
    }

    return 0;
}

// Treats a native press edge as held-input replay when the same switch button is set in both recorded moves.
constexpr bool held_switch_edge(int channel, std::uint32_t trigger, std::uint32_t direction, std::uint32_t previous_buttons,
                                std::uint32_t current_buttons) noexcept {
    const auto mask = switch_button_mask(channel, direction);
    return (trigger & 3U) == 3U && mask != 0 && (previous_buttons & mask) != 0 && (current_buttons & mask) != 0;
}

static_assert(packed_weapon_index(0x12) == 2);
static_assert(packed_weapon_channel(0x12) == 1);

} // namespace fc_plugins::weapon_swap_replay_fix
