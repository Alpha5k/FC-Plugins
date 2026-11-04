#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace fc_plugins::collision_history_fix {

inline constexpr std::size_t kLocalPlayerCount = 2;

// The retail CollisionObject stores three x86 pointers in their native insertion order.
using Contacts = std::array<std::uint32_t, 3>;

// Binds movement history to a complete Soldier and its native local input slot, without a weapon.
struct SoldierIdentity {
    std::uintptr_t soldier{};
    std::int32_t player{-1};
    std::uint8_t local_player{0xFF};

    // Requires a player-owned soldier assigned to one of the two native input slots.
    constexpr bool valid() const noexcept {
        return soldier != 0 && player >= 0 && player < 64 && local_player < kLocalPlayerCount;
    }

    friend constexpr bool operator==(const SoldierIdentity &, const SoldierIdentity &) = default;
};

// Retains exact movement boundaries within the native 32-turn input ring.
class History final {
  public:
    SoldierIdentity tracked(std::size_t local_player) const noexcept {
        return slots_[local_player].identity;
    }

    // Establishes a current native frontier after first observation or a missing historical boundary.
    void start_frontier(const SoldierIdentity &identity, std::int32_t turn, const Contacts &contacts) noexcept {
        if (!identity.valid() || turn < 0) {
            return;
        }
        auto &slot = bind(identity);
        if (slot.suspended) {
            slot.entries = {};
            slot.suspended = false;
        }
        capture(identity, turn, contacts);
    }

    // Restores only a complete turn tag. A miss suspends capture until a native frontier is observed.
    std::optional<Contacts> rewind(const SoldierIdentity &identity, std::int32_t turn) noexcept {
        if (!identity.valid() || turn < 0) {
            return std::nullopt;
        }
        auto &slot = bind(identity);
        const auto &entry = slot.entries[index(turn)];
        if (slot.suspended || entry.turn != turn) {
            slot.entries = {};
            slot.resident_turn = -1;
            slot.suspended = true;
            return std::nullopt;
        }
        slot.resident_turn = turn;
        return entry.contacts;
    }

    // Saves the resident boundary; an unresolved rewind never acquires a false historical tag.
    void capture(const SoldierIdentity &identity, std::int32_t turn, const Contacts &contacts) noexcept {
        if (!identity.valid() || turn < 0) {
            return;
        }
        auto &slot = bind(identity);
        if (slot.suspended) {
            return;
        }
        for (auto &entry : slot.entries) {
            if (entry.turn >= 0 && turn > entry.turn && static_cast<std::uint32_t>(turn) - entry.turn >= 32U) {
                entry = {};
            }
        }
        slot.entries[index(turn)] = {.turn = turn, .contacts = contacts};
        slot.resident_turn = turn;
    }

    // Tests whether the current native list belongs to a later movement boundary.
    bool needs_rewind(const SoldierIdentity &identity, std::int32_t turn) const noexcept {
        if (!identity.valid()) {
            return false;
        }
        const auto &slot = slots_[identity.local_player];
        return slot.identity == identity && !slot.suspended && turn >= 0 && turn < slot.resident_turn;
    }

    // Completed simulation replaces speculative suffixes while consecutive reads retain their exact seeds.
    void advanced(const SoldierIdentity &identity, std::int32_t turn) noexcept {
        if (!identity.valid()) {
            return;
        }
        auto &slot = slots_[identity.local_player];
        if (slot.identity != identity || slot.suspended || slot.resident_turn != turn) {
            return;
        }
        for (auto &entry : slot.entries) {
            if (entry.turn > turn) {
                entry = {};
            }
        }
        slot.resident_turn = turn + 1;
    }

    // Captures the final full-Predict result only when its resident turn is known.
    void complete(const SoldierIdentity &identity, std::int32_t turn, const Contacts &contacts) noexcept {
        if (!identity.valid()) {
            return;
        }
        const auto &slot = slots_[identity.local_player];
        if (slot.identity == identity && !slot.suspended && slot.resident_turn == turn) {
            capture(identity, turn, contacts);
        }
    }

    // Retires a soldier before reset or destruction can reuse its native identity.
    void clear_soldier(std::uintptr_t soldier) noexcept {
        for (auto &slot : slots_) {
            if (slot.identity.soldier == soldier) {
                slot = {};
            }
        }
    }

    // Removes a destroyed CollisionObject from saved lists without dereferencing it or moving survivors.
    void forget_object(std::uint32_t object) noexcept {
        if (object == 0) {
            return;
        }
        for (auto &slot : slots_) {
            for (auto &entry : slot.entries) {
                forget_contact(entry.contacts, object);
            }
        }
    }

    // Removes only references to this retired object, preserving native priority slots.
    static void forget_contact(Contacts &contacts, std::uint32_t object) noexcept {
        for (auto &contact : contacts) {
            if (contact == object) {
                contact = 0;
            }
        }
    }

    // Releases every borrowed identity when the native session ends or changes.
    void clear_all() noexcept {
        slots_ = {};
    }

  private:
    struct Entry {
        std::int32_t turn{-1};
        Contacts contacts{};
    };

    struct Slot {
        SoldierIdentity identity;
        std::array<Entry, 32> entries{};
        std::int32_t resident_turn{-1};
        bool suspended{true};
    };

    // The full turn tag distinguishes native ring wrap from an exact retained boundary.
    static std::size_t index(std::int32_t turn) noexcept {
        return static_cast<std::uint32_t>(turn) & 31U;
    }

    // A different soldier or player assignment starts with no retained collision history.
    Slot &bind(const SoldierIdentity &identity) noexcept {
        for (std::size_t player{}; player < slots_.size(); ++player) {
            if (player != identity.local_player && slots_[player].identity.soldier == identity.soldier) {
                slots_[player] = {};
            }
        }
        auto &slot = slots_[identity.local_player];
        if (slot.identity != identity) {
            slot = {.identity = identity};
        }
        return slot;
    }

    std::array<Slot, kLocalPlayerCount> slots_{};
};

} // namespace fc_plugins::collision_history_fix
