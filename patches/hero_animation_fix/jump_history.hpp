#pragma once

#include "history.hpp"

#include <fc/native.hpp>

#include <array>
#include <algorithm>
#include <cstdint>
#include <new>
#include <vector>

namespace fc_plugins::hero_animation_fix {

// Preserves the presentation clock only after an accepted historical jump repeats its displayed occurrence.
inline bool preserve_jump_clock(void *animator, std::int32_t before_action, float before_timer, bool accepted, bool historical,
                                bool presented) noexcept {
    if (!accepted || !historical || !presented || fc::read_field<std::int32_t>(animator, 0x1FEC) != before_action) {
        return false;
    }
    fc::write_field(animator, 0x1FF0, before_timer);
    return true;
}

// Keeps accepted jump requests until both native animation lanes select them or the replay window expires.
class JumpHistory final {
  public:
    // Repeated Soldier invocations in one turn retain distinct accepted jump occurrences.
    std::uint32_t next_order(const HeroIdentity &identity, std::int32_t turn) noexcept {
        if (!identity.valid() || !identity.is_local()) {
            return 0;
        }
        auto &slot = slots_[identity.local_player];
        if (slot.identity != identity) {
            slot = {.identity = identity};
        }
        if (slot.order_turn != turn) {
            slot.order_turn = turn;
            slot.order = 0;
        }
        return slot.order++;
    }

    // A matching authority read starts another traversal without erasing actually presented requests.
    void rewind(std::uintptr_t weapon) noexcept {
        for (auto &slot : slots_) {
            if (slot.identity.weapon == weapon) {
                slot.order_turn = -1;
                slot.order = 0;
            }
        }
    }

    // Returns true only for a matching turn/order/action whose pending reset reached presentation.
    bool already_presented(const HeroIdentity &identity, std::int32_t turn, std::uint32_t order, std::int32_t action,
                           bool reset_latch = true) noexcept {
        if (!identity.valid() || !identity.is_local() || turn < 0) {
            return false;
        }

        auto &slot = slots_[identity.local_player];
        if (slot.identity != identity) {
            slot = {.identity = identity};
        }

        std::erase_if(slot.entries, [turn](const Entry &entry) {
            return entry.turn >= 0 && turn > entry.turn && turn - entry.turn >= kReplayTurns;
        });
        for (auto &entry : slot.entries) {
            if (entry.turn == turn && entry.order == order) {
                if (entry.action != action) {
                    entry = {.turn = turn, .order = order, .action = action, .pending = true, .reset_latch = reset_latch};
                }
                entry.waiting = true;
                return entry.presented;
            }
        }

        try {
            slot.entries.push_back({.turn = turn, .order = order, .action = action, .pending = true, .reset_latch = reset_latch});
        } catch (const std::bad_alloc &) {
            // An unrecorded occurrence remains eligible for native presentation.
        }
        return false;
    }

    // Binds only requests transferred by this native consumer; early descriptor failure retains each lane.
    std::uint8_t consume(const HeroIdentity &identity, std::int32_t action, bool reset_latch) noexcept {
        if (!identity.valid() || !identity.is_local()) {
            return 3;
        }
        auto &slot = slots_[identity.local_player];
        if (slot.identity != identity) {
            return 3;
        }
        std::uint8_t needed{};
        bool matched{};
        for (auto &entry : slot.entries) {
            if (entry.action != action) {
                entry.armed = 0;
                if (!entry.reset_latch || reset_latch) {
                    entry.waiting = false;
                }
                continue;
            }
            if (entry.waiting && entry.action == action && (!entry.reset_latch || reset_latch)) {
                entry.armed |= static_cast<std::uint8_t>(3U & ~entry.lanes);
                needed |= entry.armed;
                matched = true;
                entry.waiting = false;
            }
        }
        return matched && reset_latch ? needed : 3;
    }

    // Records each lane only after its native selector actually installs an animation.
    void present(const HeroIdentity &identity, std::uint8_t lane, std::int32_t action, bool reset_consumed = true,
                 bool clip_changed = false) noexcept {
        if (!identity.valid() || !identity.is_local()) {
            return;
        }

        auto &slot = slots_[identity.local_player];
        if (slot.identity != identity) {
            return;
        }
        for (auto &entry : slot.entries) {
            if (entry.pending && entry.action == action && (entry.armed & lane) != 0 &&
                (entry.reset_latch ? reset_consumed : (clip_changed || reset_consumed))) {
                entry.lanes |= lane;
                entry.armed &= static_cast<std::uint8_t>(~lane);
                entry.presented = entry.lanes == 3;
                entry.pending = !entry.presented;
            }
        }
    }

    // Native weapon and soldier lifecycle observers retire entries before storage can be reused.
    void clear_weapon(std::uintptr_t weapon) noexcept {
        for (auto &slot : slots_) {
            if (slot.identity.weapon == weapon) {
                slot = {};
            }
        }
    }

    // Soldier reset/destruction retires only the matching embedded Controllable owner.
    void clear_owner(std::uintptr_t owner) noexcept {
        for (auto &slot : slots_) {
            if (slot.identity.owner == owner) {
                slot = {};
            }
        }
    }

    // Session changes and native prediction resets discard every retained occurrence.
    void clear_all() noexcept {
        slots_ = {};
    }

  private:
    static constexpr std::int32_t kReplayTurns = 32;

    struct Entry {
        std::int32_t turn{-1};
        std::uint32_t order{};
        std::int32_t action{-1};
        bool pending{};
        bool reset_latch{true};
        bool waiting{true};
        bool presented{};
        std::uint8_t lanes{};
        std::uint8_t armed{};
    };

    struct Slot {
        HeroIdentity identity;
        std::vector<Entry> entries;
        std::int32_t order_turn{-1};
        std::uint32_t order{};
    };

    std::array<Slot, kLocalPlayerCount> slots_{};
};

} // namespace fc_plugins::hero_animation_fix
