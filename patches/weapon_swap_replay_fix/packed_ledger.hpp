#pragma once

#include "local_ledger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace fc_plugins::weapon_swap_replay_fix {

// Reports whether this ledger permits muting Select and whether a recorded weapon pointer changed.
struct PackedDecision {
    bool mute_select{};
    bool pointer_invalidated{};

    // A native pointer contradiction overrides permission from either history.
    bool may_mute(bool channel_duplicate) const noexcept {
        return !pointer_invalidated && (mute_select || channel_duplicate);
    }
};

// Copies recorded weapon pointers so the handler can check them before substituting the render selection.
struct PackedSnapshot {
    void *soldier{};
    // Indexed by channel * eight inventory slots + slot; pointers remain borrowed from the game.
    std::array<void *, kWeaponChannelCount * kWeaponSlotCount> node_weapons{};
    // Combined channel/slot key for the predicted weapon.
    std::uint8_t projected_key{};
    // Manual request IDs identify the channel history; automatic activations use zero IDs.
    std::int32_t request_turn{-1};
    std::uint32_t epoch{};
    std::uint32_t sequence{};
};

// Retains one predicted active channel and inventory slot per player alongside the two channel histories.
class PackedSwapLedger final {
  public:
    static constexpr int kKeyCount = kWeaponChannelCount * kWeaponSlotCount;
    static constexpr std::int32_t kReplayHistoryTurns = LocalSwapLedger::kReplayHistoryTurns;

    // Clears the predicted selection more than 32 prediction turns after its latest request.
    void observe_frontiers(const TurnFrontiers &frontiers) noexcept;

    // Records the active request; equal turns follow the shared sequence order, including nonzero counter wrap.
    bool track_projected(void *soldier, int local_player, int channel, int index, void *weapon, std::int32_t request_turn,
                         std::uint32_t epoch, std::uint32_t sequence, const TurnFrontiers &frontiers) noexcept;

    // Records an automatic activation without a channel-slot request; only its exact input turn and endpoints repeat.
    PackedDecision record_activation(void *soldier, int local_player, int old_channel, int old_index, void *old_weapon, int target_channel,
                                     int target_index, void *target_weapon, const TurnFrontiers &frontiers) noexcept;

    // Automatic ownership mutes only an exact rollback from authority before the action; other results clear it.
    // Manual ownership permits recorded weapons, learning one unknown endpoint only while its channel request is pending.
    PackedDecision observe_transition(void *soldier, int local_player, int old_channel, int old_index, void *old_weapon, int target_channel,
                                      int target_index, void *target_weapon, const TurnFrontiers &frontiers) noexcept;

    // Permits muting manual synchronization when its channel, slot and weapon pointer are already recorded.
    PackedDecision observe_sync(void *soldier, int local_player, int channel, int index, void *weapon,
                                const TurnFrontiers &frontiers) noexcept;

    // Marks a matching accepted request settled; a matching rejection or correction clears the packed history.
    void observe_channel_result(int local_player, const AuthorityDecision &decision) noexcept;

    // Copies the predicted selection and recorded pointers.
    // Returns true when the native selection names a recorded weapon and differs from prediction.
    bool resolve(void *soldier, int local_player, int actual_channel, int actual_index, void *actual_weapon,
                 PackedSnapshot &snapshot) noexcept;

    // Tests whether this local player's packed history belongs to the soldier.
    bool has_state(void *soldier, int local_player) const noexcept;

    // Copies existing ownership without resolving a native selection or changing its history.
    bool find(void *soldier, int local_player, PackedSnapshot &snapshot) const noexcept;

    // Drops the packed projection for this local input slot.
    void clear_player(int local_player) noexcept;

    // Drops the packed projection only when both soldier and originating epoch match.
    void clear_epoch(void *soldier, int local_player, std::uint32_t epoch) noexcept;

    // Drops both local players' packed selection histories.
    void clear() noexcept;

    // Reports whether either local player retains packed selection history.
    bool has_active() const noexcept;

    // Combines a validated channel and slot into the player-wide inventory key.
    static constexpr int key(int channel, int index) noexcept {
        return channel * kWeaponSlotCount + index;
    }

    // Replaces channel and slot bits while retaining the two unrelated high bits.
    static constexpr std::uint8_t project_selection(std::uint8_t packed, int projected_key) noexcept {
        if (projected_key < 0 || projected_key >= kKeyCount) {
            return packed;
        }
        return static_cast<std::uint8_t>((packed & 0xC0U) | (static_cast<std::uint8_t>(projected_key / kWeaponSlotCount) << 4) |
                                         static_cast<std::uint8_t>(projected_key % kWeaponSlotCount));
    }

  private:
    // Retains exact input and endpoint identities even when a later manual request replaces the projected selection.
    struct Activation {
        std::int32_t turn{-1};
        std::uint8_t source{};
        std::uint8_t target{};
        void *source_weapon{};
        void *target_weapon{};
    };

    struct PlayerState {
        void *soldier{};

        // Each non-null pointer records a weapon learned for a combined channel/slot key.
        std::array<void *, kKeyCount> node_weapons{};

        // Render hooks use this predicted selection when the native selection names another recorded weapon.
        std::uint8_t projected_key{};
        std::int32_t latest_request_turn{-1};
        std::int32_t high_prediction{-1};

        // Epoch and sequence tie the player-wide selection to its channel request.
        std::uint32_t epoch{};
        std::uint32_t sequence{};

        // The channel ledger accepted the request; later transitions must use already recorded weapons.
        bool settled{};

        // Automatic activations share this owner, inventory and expiry; epoch zero means no manual channel request.
        std::array<Activation, kMoveHistoryTurnCount> activations{};
        std::size_t next_activation{};
    };

    // Requires a non-null soldier and an in-range local input slot.
    static bool valid_owner(void *soldier, int local_player) noexcept;

    // Checks channel and slot bounds before computing a packed key.
    static bool valid_key(int channel, int index) noexcept;

    // Retains the highest prediction turn and expires automatic occurrences outside the native replay window.
    static void advance(PlayerState &state, const TurnFrontiers &frontiers) noexcept;

    // Detects an inventory pointer that contradicts any retained automatic occurrence at this channel/slot key.
    static bool activation_pointer_changed(const PlayerState &state, int key, void *weapon) noexcept;

    // Starts packed ownership with its predicted weapon; zero IDs identify an automatic activation.
    static void start(PlayerState &state, void *soldier, int projected_key, void *weapon, std::int32_t request_turn, std::uint32_t epoch,
                      std::uint32_t sequence, const TurnFrontiers &frontiers) noexcept;

    // Copies the predicted selection and recorded weapon pointers for the handler to validate.
    static PackedSnapshot make_snapshot(const PlayerState &state) noexcept;

    std::array<PlayerState, kLocalPlayerCount> players_{};
};

} // namespace fc_plugins::weapon_swap_replay_fix
