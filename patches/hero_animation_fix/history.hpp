#pragma once

#include "../hero_diagnostics/api.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace fc_plugins::hero_animation_fix {

inline constexpr std::size_t kLocalPlayerCount = 2;
inline constexpr std::size_t kNetworkPlayerCount = 64;
// Storage limit for pending transitions; combo IDs also use a five-bit range.
inline constexpr std::size_t kTransitionCapacity = 32;

// Binds history to borrowed native addresses and player slots; address reuse can retain the same identity.
struct HeroIdentity {
    std::uintptr_t weapon{};
    // EntitySoldier's embedded Controllable, rather than the start of the soldier object.
    std::uintptr_t owner{};
    // Shared combo definition for the weapon's loaded asset.
    std::uintptr_t combo{};
    // Native player index, independent of transport peer IDs.
    std::int16_t player_id{-1};
    // Local input slot 0 or 1; 0xFF denotes a remote player.
    std::uint8_t local_player{0xFF};

    // Tests whether the native lookup assigned one of the two local input slots.
    constexpr bool is_local() const noexcept {
        return static_cast<std::size_t>(local_player) < kLocalPlayerCount;
    }

    // Tests the remote sentinel and native player-index bounds.
    constexpr bool is_remote() const noexcept {
        return local_player == 0xFF && player_id >= 0 && static_cast<std::size_t>(player_id) < kNetworkPlayerCount;
    }

    // Requires nonzero addresses and valid slot ranges; native callbacks remain responsible for object lifetime.
    constexpr bool valid() const noexcept {
        return weapon != 0 && owner != 0 && combo != 0 && player_id >= 0 && static_cast<std::size_t>(player_id) < kNetworkPlayerCount &&
               (is_local() || is_remote());
    }

    friend constexpr bool operator==(const HeroIdentity &, const HeroIdentity &) = default;
};

// Classifies presentation only; every result still applies native weapon authority.
enum class AuthorityAction {
    Native,
    Historical,
    Corrected,
};

// Compares server updates with melee transitions already predicted for a local player.
class LocalHistory final {
  public:
    // Copies an already-bound path's frontiers without changing its identity or predictions.
    hero_diagnostics::HistorySnapshot snapshot(const HeroIdentity &identity) const noexcept;
    hero_diagnostics::HistorySnapshot snapshot_weapon(std::uintptr_t weapon) const noexcept;
    // Appends a changed combo ID at its positive prediction turn; returns false if invalid or full.
    bool observe_prediction(const HeroIdentity &identity, int base_state, int target_state, std::int32_t prediction_turn) noexcept;

    // Uses the acknowledged local input turn to confirm a presentation prefix, independently of native gameplay.
    AuthorityAction classify_authority(const HeroIdentity &identity, int current_state, int incoming_state, std::int32_t acknowledged_turn,
                                       hero_diagnostics::HistoryDecision *evidence = nullptr) noexcept;

    // Discards a matching local identity; a replacement identity in the same slot is preserved.
    void clear(const HeroIdentity &identity) noexcept;

    // Discards local histories that still refer to this weapon's native pointer identity.
    void clear_weapon(std::uintptr_t weapon) noexcept;

    // Discards all local prediction history.
    void clear_all() noexcept;

  private:
    struct Transition {
        std::int32_t turn{-1};
        std::int8_t state{-1};
    };

    struct Slot {
        HeroIdentity identity{};

        // Ordered transitions after base_state; only the first count entries are live.
        std::array<Transition, kTransitionCapacity> path{};
        std::uint8_t count{};
        // Combo ID accepted at base_turn, or the native starting state of a newly observed path.
        std::int8_t base_state{-1};
        std::int32_t base_turn{-1};
    };

    // Binds a valid local input slot, replacing history when its hero identity changes.
    Slot &bind(const HeroIdentity &identity) noexcept;

    // Stores a combo and turn as the new start of the path, clearing pending predictions.
    static void seed(Slot &slot, const HeroIdentity &identity, int state, std::int32_t turn) noexcept;

    // Removes confirmed transitions from the start of the path and accepts their last combo and turn.
    static void advance(Slot &slot, std::size_t count, int state, std::int32_t turn) noexcept;

    std::array<Slot, kLocalPlayerCount> slots_{};
};

// Estimates delayed server state by comparing remote combo IDs with predicted transitions.
// Remote snapshots have no local input acknowledgement to distinguish repeated occurrences of a combo ID.
class RemoteHistory final {
  public:
    // Copies an already-bound path's frontiers without changing its identity or predictions.
    hero_diagnostics::HistorySnapshot snapshot(const HeroIdentity &identity) const noexcept;
    hero_diagnostics::HistorySnapshot snapshot_weapon(std::uintptr_t weapon) const noexcept;
    // Appends a remote predicted transition after the last accepted combo or pending prediction.
    bool observe_prediction(const HeroIdentity &identity, int base_state, int target_state, std::int32_t update_turn) noexcept;

    // Uses combo IDs to estimate whether a server state is delayed; idle tolerance counts distinct update turns.
    AuthorityAction classify_authority(const HeroIdentity &identity, int current_state, int incoming_state, std::int32_t update_turn,
                                       hero_diagnostics::HistoryDecision *evidence = nullptr) noexcept;

    // Discards a matching remote identity, preserving any replacement hero in that player slot.
    void clear(const HeroIdentity &identity) noexcept;

    // Discards remote histories that still refer to this weapon's native pointer identity.
    void clear_weapon(std::uintptr_t weapon) noexcept;

    // Discards all remote presentation paths.
    void clear_all() noexcept;

  private:
    struct Transition {
        std::int32_t turn{-1};
        std::int8_t state{-1};
        // This transition left idle (combo 0) for a non-idle combo.
        bool starts_action{};
    };

    struct Slot {
        HeroIdentity identity{};
        // Ordered unconfirmed transitions; only the first count entries are live.
        std::array<Transition, kTransitionCapacity> path{};
        std::uint8_t count{};
        // Accepted combo ID, or -1 until prediction or a server state establishes the starting combo.
        std::int8_t authority_state{-1};
        std::int32_t authority_turn{-1};

        // Distinct later update turns consume idle tolerance; repeated reads at one turn share it.
        std::int32_t last_stale_idle_turn{-1};
        std::uint8_t stale_idle_samples{};
    };

    // Binds a valid native remote player index and replaces history when its hero identity changes.
    Slot &bind(const HeroIdentity &identity) noexcept;

    // Accepts a received or newly observed combo and its update turn, preserving the authority base.
    static void seed_authority(Slot &slot, int state, std::int32_t turn) noexcept;

    // Clears predicted transitions and idle counters, preserving the last accepted combo.
    static void clear_pending(Slot &slot) noexcept;

    // Removes confirmed transitions from the start of the path while keeping later predictions.
    static void consume(Slot &slot, std::size_t count) noexcept;

    // Finds the first matching predicted state, or returns the path length when no match exists.
    static std::size_t find_pending(const Slot &slot, int state) noexcept;

    // Returns the last predicted combo, or the accepted combo when no transitions remain.
    static int expected_state(const Slot &slot) noexcept;

    // Tests whether an accepted idle combo is followed by a predicted transition out of idle.
    static bool has_unconfirmed_start(const Slot &slot) noexcept;

    std::array<Slot, kNetworkPlayerCount> slots_{};
};

} // namespace fc_plugins::hero_animation_fix
