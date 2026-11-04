#pragma once

#include "state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace fc_plugins::weapon_swap_replay_fix {

// Retains predicted weapon selections separately for each local player and primary/secondary channel.
class LocalSwapLedger final {
  public:
    static constexpr std::size_t kMaximumSteps = 32;
    static constexpr std::int32_t kSettlementTurns = 2;
    static constexpr std::int32_t kReplayHistoryTurns = 32;

    // Keeps the highest observed turns and clears history more than 32 prediction turns after its latest request.
    void observe_frontiers(const TurnFrontiers &frontiers) noexcept;

    // Classifies input by recorded turn, target and weapon pointers.
    // A changed target or an unknown or replaced historical weapon clears the history.
    RequestDecision record_local(void *soldier, int local_player, int channel, int old_index, int target_index, void *old_weapon,
                                 void *target_weapon, std::int32_t request_turn, const TurnFrontiers &frontiers) noexcept;

    // Keeps known selections pending until update and acknowledgement reach the request, then more than two update turns pass.
    // Unknown or replaced weapons clear this history and leave selection effects unchanged.
    AuthorityDecision observe_authority(void *soldier, int local_player, int channel, int current_index, int server_index,
                                        void *current_weapon, void *server_weapon, const TurnFrontiers &frontiers) noexcept;

    // Tests whether this soldier's channel history contains the slot and the same weapon pointer.
    bool is_known_selection(void *soldier, int local_player, int channel, int target_index, void *target_weapon) const noexcept;

    // Copies the predicted slot, recorded pointers and request IDs.
    // Returns true when a known native slot differs from the predicted final slot.
    bool resolve(void *soldier, int local_player, int channel, int actual_index, EpochSnapshot &snapshot) noexcept;

    // Copies this soldier's channel history without changing it.
    bool find(void *soldier, int local_player, int channel, EpochSnapshot &snapshot) const noexcept;

    // Drops this channel only when the retained soldier identity matches.
    void clear_slot(void *soldier, int local_player, int channel) noexcept;

    // Drops both channels for the local input slot.
    void clear_player(int local_player) noexcept;

    // Drops all channel histories while preserving nonzero epoch and sequence allocation.
    void clear() noexcept;

    // Reports whether any channel retains a soldier's request history.
    bool has_active() const noexcept;

  private:
    struct Epoch {
        // Borrowed soldier pointer; the handler checks its local input slot before presenting this history.
        void *soldier{};
        // Each non-null pointer records the weapon last learned at that inventory slot.
        std::array<void *, kWeaponSlotCount> node_weapons{};

        // Retain each input turn with its target slot so corrected outcomes cannot reuse its presentation.
        std::array<std::int32_t, kMaximumSteps> request_turns{};
        std::array<std::uint8_t, kMaximumSteps> request_targets{};
        std::size_t step_count{};

        // Inventory slot requested by the latest new input in this history.
        std::uint8_t final_index{};

        std::int32_t latest_request_turn{-1};
        // Highest observed counters stay unchanged when prediction replays older input.
        std::int32_t high_update{-1};
        std::int32_t high_prediction{-1};
        std::int32_t high_acknowledged{-1};
        // Update turn when both update and acknowledgement first reached the latest request; -1 means waiting.
        std::int32_t settlement_start{-1};

        // Nonzero IDs link this history and its latest request to the combined channel/slot history.
        std::uint32_t id{};
        std::uint32_t latest_sequence{};
    };

    // Requires a non-null soldier and in-range local input slot and channel.
    static bool valid_owner(void *soldier, int local_player, int channel) noexcept;

    // Checks whether the slot belongs to the eight-entry inventory.
    static bool valid_index(int index) noexcept;

    // Keeps each observed frontier at its highest recent turn.
    static void advance(Epoch &epoch, const TurnFrontiers &frontiers) noexcept;

    // Allocates a nonzero identity even when the sequence counter wraps.
    static std::uint32_t next_nonzero(std::uint32_t &value) noexcept;

    // Tests a validated slot against its non-null weapon identity.
    static bool matches_node(const Epoch &epoch, int index, void *weapon) noexcept;

    // Clears conflicting history and returns its ID so the handler can clear the matching packed history.
    static RequestDecision abandon(Epoch &epoch, RequestKind kind) noexcept;

    // Starts a new request epoch with both inventory endpoints and its originating turn.
    RequestDecision start(Epoch &epoch, void *soldier, int old_index, int target_index, void *old_weapon, void *target_weapon,
                          std::int32_t request_turn, const TurnFrontiers &frontiers) noexcept;

    // Copies the recorded weapon pointers and request IDs for presentation.
    static EpochSnapshot make_snapshot(const Epoch &epoch) noexcept;

    // Maps a local player and channel to their independent retained epoch.
    static std::size_t slot(int local_player, int channel) noexcept;

    // Returns the epoch for a previously validated player and channel.
    Epoch &epoch_at(int local_player, int channel) noexcept;
    const Epoch &epoch_at(int local_player, int channel) const noexcept;

    std::array<Epoch, kLocalPlayerCount * kWeaponChannelCount> epochs_{};

    std::uint32_t next_epoch_{};
    std::uint32_t next_sequence_{};
};

} // namespace fc_plugins::weapon_swap_replay_fix
