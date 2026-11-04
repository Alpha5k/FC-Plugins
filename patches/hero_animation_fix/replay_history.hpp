#pragma once

#include "history.hpp"
#include "../melee_timing/api.hpp"

#include <fc/native.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace fc_plugins::hero_animation_fix {

// The native gait classifier owns a saved position, movement angle and direction/hysteresis bits.
struct LiveGaitState {
    std::array<float, 3> position{};
    float angle{};
    std::uint8_t classification{};

    // Captures Soldier +764..+773 and the forward/walk/run classification in +9D5.
    static LiveGaitState capture(const void *soldier) noexcept {
        LiveGaitState state;
        std::memcpy(state.position.data(), static_cast<const std::byte *>(soldier) + 0x764, sizeof(state.position));
        state.angle = fc::read_field<float>(soldier, 0x770);
        state.classification = fc::read_field<std::uint8_t>(soldier, 0x9D5) & 0x38U;
        return state;
    }

    // Restores the live gait fields while keeping the classifier's unrelated flags.
    void restore(void *soldier) const noexcept {
        std::memcpy(static_cast<std::byte *>(soldier) + 0x764, position.data(), sizeof(position));
        fc::write_field(soldier, 0x770, angle);
        const auto flags = fc::read_field<std::uint8_t>(soldier, 0x9D5);
        fc::write_field(soldier, 0x9D5, static_cast<std::uint8_t>((flags & ~0x38U) | classification));
    }
};

// Native effects update these bits to match actual sound resources, separately from desired saber state.
struct LiveSoundState {
    std::uint8_t on{};
    std::uint8_t mask{};

    // Reads the flags that record which native sound resources are active.
    static LiveSoundState capture(const void *weapon) noexcept {
        return {.on = static_cast<std::uint8_t>(fc::read_field<std::uint8_t>(weapon, 0x179) & 0x04U),
                .mask = static_cast<std::uint8_t>(fc::read_field<std::uint8_t>(weapon, 0x17D) & 0xF0U)};
    }

    // Restores the active-sound flags while preserving desired saber state and trail masks.
    void restore(void *weapon) const noexcept {
        fc::write_field(weapon, 0x179, static_cast<std::uint8_t>((fc::read_field<std::uint8_t>(weapon, 0x179) & ~0x04U) | on));
        fc::write_field(weapon, 0x17D, static_cast<std::uint8_t>((fc::read_field<std::uint8_t>(weapon, 0x17D) & ~0xF0U) | mask));
    }
};

// Retains the retail melee clocks, input queue and movement values for prediction replay.
struct ReplayState {
    std::array<std::byte, 8> mode_time{};
    std::array<std::byte, 144> melee{};
    std::uint32_t soldier_state{};
    void *owner{};
    float lock_timer{};
    std::uint8_t lock_flags{};
    melee_timing::QueueAges input_ages;
    bool precise_inputs{};

    // +B0/+B4 hold mode/time; +118..+1A7 hold movement, queue, combo and saber timing.
    static ReplayState capture(const void *weapon, const melee_timing::QueueTimingApi *timing = nullptr) noexcept {
        const auto *bytes = static_cast<const std::byte *>(weapon);
        ReplayState state;
        std::memcpy(state.mode_time.data(), bytes + 0xB0, state.mode_time.size());
        std::memcpy(state.melee.data(), bytes + 0x118, state.melee.size());
        state.soldier_state = fc::read_field<std::uint32_t>(weapon, 0xAC) & kSoldierStateMask;
        state.owner = fc::read_field<void *>(weapon, 0x6C);
        if (state.owner != nullptr) {
            state.lock_timer = fc::read_field<float>(state.owner, 0x234);
            state.lock_flags = fc::read_field<std::uint8_t>(state.owner, 0x231) & 0x3CU;
        }
        if (timing != nullptr) {
            state.precise_inputs = timing->capture(const_cast<void *>(weapon), &state.input_ages);
        }
        return state;
    }

    // Preserves selected/hide/fired bits and live audio bookkeeping while restoring simulation values.
    // Weapon Read passes authoritative_timer to keep the timer already supplied by Soldier Read.
    void restore(void *weapon, bool authoritative_timer = false, const melee_timing::QueueTimingApi *timing = nullptr) const noexcept {
        auto *bytes = static_cast<std::byte *>(weapon);
        const auto sound = LiveSoundState::capture(weapon);
        std::memcpy(bytes + 0xB0, mode_time.data(), mode_time.size());
        std::memcpy(bytes + 0x118, melee.data(), melee.size());
        sound.restore(weapon);
        if (timing != nullptr) {
            timing->restore(weapon, precise_inputs ? &input_ages : nullptr);
        }
        const auto flags = fc::read_field<std::uint32_t>(weapon, 0xAC);
        fc::write_field(weapon, 0xAC, (flags & ~kSoldierStateMask) | soldier_state);
        // Soldier Read supplies +474 before Weapon Read. Native lock expiry clears only the four bits in mask 3C.
        if (owner != nullptr && fc::read_field<void *>(weapon, 0x6C) == owner) {
            if (!authoritative_timer) {
                fc::write_field(owner, 0x234, lock_timer);
            }
            const auto movement_flags = fc::read_field<std::uint8_t>(owner, 0x231);
            fc::write_field(owner, 0x231, static_cast<std::uint8_t>((movement_flags & ~0x3CU) | lock_flags));
        }
    }

  private:
    // Native melee conditions pair these six Soldier-state bits with the state age at +118.
    static constexpr std::uint32_t kSoldierStateMask = 0x000001F8;
};

// Stores exact start-of-turn values for native player weapons within the native 32-turn horizon.
class ReplayHistory final {
  public:
    // Returns a retained identity for pre-controller capture; lifecycle callbacks retire borrowed pointers.
    HeroIdentity tracked(std::size_t player) const noexcept {
        return slots_[player].identity;
    }

    // Looks up the exact native turn, retaining later checkpoints for consecutive authority reads.
    std::optional<ReplayState> restore(const HeroIdentity &identity, std::int32_t turn) noexcept {
        if (!identity.valid() || turn < 0) {
            return std::nullopt;
        }
        auto &slot = bind(identity);
        const auto &entry = slot.entries[index(turn)];
        const auto state = entry.turn == turn ? std::optional{entry.state} : std::nullopt;
        if (!state) {
            slot.entries = {};
        }
        slot.resident_turn = turn;
        return state;
    }

    // A revised native read seed owns this boundary; sequential replay must use its preceding native result.
    bool needs_rewind(const HeroIdentity &identity, std::int32_t turn) const noexcept {
        if (!identity.valid()) {
            return false;
        }
        const auto &slot = slots_[static_cast<std::size_t>(identity.player_id)];
        return slot.identity == identity && turn >= 0 && turn < slot.resident_turn;
    }

    // Captures a start boundary or the result of the full native reader under the actual host turn.
    void capture(const HeroIdentity &identity, std::int32_t turn, const ReplayState &state) noexcept {
        if (!identity.valid() || turn < 0) {
            return;
        }
        auto &slot = bind(identity);
        for (auto &entry : slot.entries) {
            if (entry.turn >= 0 && turn > entry.turn && static_cast<std::uint32_t>(turn) - entry.turn >= 32U) {
                entry = {};
            }
        }
        slot.entries[index(turn)] = {.turn = turn, .state = state};
        slot.resident_turn = turn;
    }

    // Retires the future after native Update advances a captured start; fallback updates create no checkpoint.
    void advanced(const HeroIdentity &identity, std::int32_t turn) noexcept {
        if (!identity.valid()) {
            return;
        }
        auto &slot = bind(identity);
        if (slot.resident_turn == turn) {
            // Authority reads can arrive together; only completed simulation supersedes their future values.
            for (auto &saved : slot.entries) {
                if (saved.turn > turn) {
                    saved = {};
                }
            }
            slot.resident_turn = turn + 1;
        }
    }

    // Saves the known resident boundary after the completed native turn, including scheduled collision work.
    bool complete(const HeroIdentity &identity, std::int32_t next_start, const void *weapon,
                  const melee_timing::QueueTimingApi *timing = nullptr) noexcept {
        if (!identity.valid() || next_start < 0 || reinterpret_cast<std::uintptr_t>(weapon) != identity.weapon) {
            return false;
        }
        const auto &slot = slots_[static_cast<std::size_t>(identity.player_id)];
        if (slot.identity != identity || slot.resident_turn != next_start) {
            return false;
        }
        capture(identity, slot.resident_turn, ReplayState::capture(weapon, timing));
        return true;
    }

    // Copies resident and saved tags without binding a new native identity.
    std::array<std::int32_t, 2> tags(const HeroIdentity &identity, std::int32_t turn) const noexcept {
        if (!identity.valid()) {
            return {-1, -1};
        }
        const auto &slot = slots_[static_cast<std::size_t>(identity.player_id)];
        if (slot.identity != identity) {
            return {-1, -1};
        }
        return {slot.resident_turn, turn >= 0 ? slot.entries[index(turn)].turn : -1};
    }

    // Retires checkpoints before selection, cancellation or native lifetime reuse.
    void clear_weapon(std::uintptr_t weapon) noexcept {
        for (auto &slot : slots_) {
            if (slot.identity.weapon == weapon) {
                slot = {};
            }
        }
    }

    // Retires every borrowed weapon at a session boundary.
    void clear_all() noexcept {
        slots_ = {};
    }

  private:
    struct Entry {
        std::int32_t turn{-1};
        ReplayState state;
    };

    struct Slot {
        HeroIdentity identity;
        std::array<Entry, 32> entries;
        std::int32_t resident_turn{-1};
    };

    // Mirrors the native input ring's modulo index while each entry retains its complete turn tag.
    static std::size_t index(std::int32_t turn) noexcept {
        return static_cast<std::uint32_t>(turn) & 31U;
    }

    // Replaces an old selected weapon or player generation without retaining its values.
    Slot &bind(const HeroIdentity &identity) noexcept {
        auto &slot = slots_[static_cast<std::size_t>(identity.player_id)];
        if (slot.identity != identity) {
            slot = {.identity = identity};
        }
        return slot;
    }

    std::array<Slot, kNetworkPlayerCount> slots_{};
};

// Detaches live native contacts only for synchronous scalar recomputation; no node values are snapshotted.
struct ContactSnapshot {
    std::uintptr_t head{};
    float age{};
    float delta{};
    std::uint32_t count{UINT32_MAX};
    bool owns{};
    bool destroyed{};
};

class ContactScope final {
  public:
    ContactScope() = default;
    ContactScope(const ContactScope &) = delete;
    ContactScope &operator=(const ContactScope &) = delete;

    // The outer matching scope owns the detached head; nested scopes keep its isolation active.
    void begin(void *weapon, ContactScope *previous) noexcept {
        weapon_ = weapon;
        snapshot_ = {};
        previous_ = previous;
        for (auto *scope = previous; scope != nullptr; scope = scope->previous_) {
            if (scope->weapon_ == weapon) {
                return;
            }
        }
        owns_head_ = true;
        age_ = fc::read_field<float>(weapon, 0x188);
        head_ = fc::read_field<void *>(weapon, 0x1A8);
        snapshot_ = {.head = reinterpret_cast<std::uintptr_t>(head_), .age = age_, .owns = true};
        fc::write_field<void *>(weapon, 0x1A8, nullptr);
    }

    // Reattaches the live contacts and translates timestamps so each contact keeps its age.
    void finish() noexcept {
        if (weapon_ != nullptr && owns_head_) {
            const float age_change = fc::read_field<float>(weapon_, 0x188) - age_;
            snapshot_.delta = age_change;
            snapshot_.count = 0;
            for (auto *node = head_; node != nullptr; node = fc::read_field<void *>(node, 0x2C)) {
                fc::write_field(node, 0, fc::read_field<float>(node) + age_change);
                ++snapshot_.count;
            }
            fc::write_field(weapon_, 0x1A8, head_);
        }
        weapon_ = nullptr;
        owns_head_ = false;
    }

    // Exposes the detached list before true native destruction and disarms every later restoration.
    void destroy(void *weapon) noexcept {
        for (auto *scope = this; scope != nullptr; scope = scope->previous_) {
            if (scope->weapon_ == weapon) {
                if (scope->owns_head_) {
                    fc::write_field(weapon, 0x1A8, scope->head_);
                }
                scope->snapshot_.destroyed = true;
                scope->weapon_ = nullptr;
                scope->owns_head_ = false;
            }
        }
    }

    // Copies the isolation owner and summary produced during its existing timestamp translation walk.
    ContactSnapshot snapshot() const noexcept {
        return snapshot_;
    }

    ~ContactScope() {
        finish();
    }

  private:
    void *weapon_{};
    ContactScope *previous_{};
    void *head_{};
    float age_{};
    bool owns_head_{};
    ContactSnapshot snapshot_{};
};

} // namespace fc_plugins::hero_animation_fix
