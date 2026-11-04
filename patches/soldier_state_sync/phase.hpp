#pragma once

#include "../melee_timing/api.hpp"

#include <fc/native.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace fc_plugins::soldier_state_sync {

// Native combo metadata selects optional fields and validates a decoded phase before restoration.
struct PhaseProfile {
    std::uint32_t combo{};
    std::uint8_t states{};
    bool alignment{};
    bool deflect{};
};

struct QueueEvent {
    std::uint32_t time{};
    std::uint8_t button{};
    std::uint8_t action{};
};

// Carries the active combo phase, live chronological inputs and optional precise elapsed ages.
struct MeleePhase {
    std::uint32_t age{};
    std::uint32_t previous{};
    std::uint32_t previous_duration{};
    std::uint32_t clock{};
    std::uint32_t blend{};
    std::array<QueueEvent, 8> events{};
    std::uint8_t count{};
    std::uint8_t held{};
    bool gravity{};
    bool flee{};
    std::uint32_t alignment_z{};
    std::uint32_t alignment_x{};
    std::uint32_t deflect_age{};
    std::uint8_t deflect_variant{};
    bool precise_ages{};
    melee_timing::QueueAges ages;

    // Decoded presence bits keep parsing independent of the receiver's active combo.
    bool previous_is_current{};
    bool alignment{};
    bool deflect{};
};

// Uses the complete Soldier's primary selection and excludes AI and non-melee weapons.
inline void *active_melee(void *soldier, const void *vtable) noexcept {
    if (fc::read_field<std::int32_t>(soldier, 0x314) < 0) {
        return nullptr;
    }
    // Primary and secondary selections share a byte; primary nibble values 8..15 are invalid signed indices.
    const auto packed = fc::read_field<std::uint8_t>(soldier, 0x742);
    const auto slot = static_cast<int>(packed & 0x0F);
    if (slot >= 8) {
        return nullptr;
    }
    auto *weapon = fc::read_field<void *>(soldier, 0x720 + 4 * slot);
    return weapon != nullptr && fc::read_field<const void *>(weapon, 0) == vtable ? weapon : nullptr;
}

// Retail combo entries occupy 0x34 bytes; count is a byte and the active index is a four-byte enum.
inline std::optional<PhaseProfile> phase_profile(const void *weapon) noexcept {
    const auto *definition = fc::read_field<const void *>(weapon, 0x114);
    if (definition == nullptr) {
        return std::nullopt;
    }
    const auto states = fc::read_field<std::uint8_t>(definition, 0x21);
    const auto combo = fc::read_field<std::uint32_t>(weapon, 0x180);
    const auto *entries = fc::read_field<const std::byte *>(definition, 0x10);
    if (states == 0 || states > 32 || combo >= states || entries == nullptr) {
        return std::nullopt;
    }
    const auto *entry = entries + combo * 0x34;
    return PhaseProfile{.combo = combo,
                        .states = states,
                        .alignment = (fc::read_field<std::uint32_t>(entry, 0x28) & (1U << 21)) != 0,
                        .deflect = fc::read_field<std::uint32_t>(entry, 0x14) != 0};
}

// Copies live ring entries in chronological order and validates the compact native ranges.
inline std::optional<MeleePhase> capture_phase(const void *weapon, const PhaseProfile &profile) noexcept {
    MeleePhase phase{
        .age = fc::read_field<std::uint32_t>(weapon, 0x188),
        .previous = fc::read_field<std::uint32_t>(weapon, 0x184),
        .previous_duration = fc::read_field<std::uint32_t>(weapon, 0x18C),
        .clock = fc::read_field<std::uint32_t>(weapon, 0x130),
        .blend = fc::read_field<std::uint32_t>(weapon, 0x19C),
        .count = fc::read_field<std::uint8_t>(weapon, 0x174),
        .held = fc::read_field<std::uint8_t>(weapon, 0x176),
        .gravity = (fc::read_field<std::uint8_t>(weapon, 0x179) & 0x40) != 0,
        .flee = (fc::read_field<std::uint8_t>(weapon, 0x17A) & 2) != 0,
        .alignment_z = fc::read_field<std::uint32_t>(weapon, 0x190),
        .alignment_x = fc::read_field<std::uint32_t>(weapon, 0x194),
        .deflect_age = fc::read_field<std::uint32_t>(weapon, 0x198),
        .deflect_variant = fc::read_field<std::uint8_t>(weapon, 0x17B),
        .alignment = profile.alignment,
        .deflect = profile.deflect,
    };
    if (phase.previous >= profile.states || phase.count > 8) {
        return std::nullopt;
    }
    const auto head = fc::read_field<std::uint8_t>(weapon, 0x175);
    for (std::size_t i = 0; i < phase.count; ++i) {
        const auto offset = 0x134 + ((head + i) & 7) * 8;
        phase.events[i] = {fc::read_field<std::uint32_t>(weapon, offset), fc::read_field<std::uint8_t>(weapon, offset + 4),
                           fc::read_field<std::uint8_t>(weapon, offset + 5)};
        if (phase.events[i].button == 0 || phase.events[i].button > 7 || phase.events[i].action > 7) {
            return std::nullopt;
        }
    }
    return phase;
}

// Rejects incompatible decoded phases before writing; otherwise restores fields after native selection and reads,
// preserving unrelated flags and sound resources.
inline bool restore_phase(void *weapon, const PhaseProfile &profile, const MeleePhase &phase) noexcept {
    const auto previous = phase.previous_is_current ? profile.combo : phase.previous;
    if (phase.alignment != profile.alignment || phase.deflect != profile.deflect || previous >= profile.states) {
        return false;
    }

    fc::write_field(weapon, 0x188, phase.age);
    fc::write_field(weapon, 0x184, previous);
    fc::write_field(weapon, 0x18C, phase.previous_duration);
    fc::write_field(weapon, 0x130, phase.clock);
    fc::write_field(weapon, 0x19C, phase.blend);
    for (std::size_t i = 0; i < phase.count; ++i) {
        const auto offset = 0x134 + i * 8;
        fc::write_field(weapon, offset, phase.events[i].time);
        fc::write_field(weapon, offset + 4, phase.events[i].button);
        fc::write_field(weapon, offset + 5, phase.events[i].action);
    }
    fc::write_field(weapon, 0x174, phase.count);
    fc::write_field<std::uint8_t>(weapon, 0x175, 0);
    fc::write_field(weapon, 0x176, phase.held);
    const auto flags = fc::read_field<std::uint8_t>(weapon, 0x179);
    const auto flags2 = fc::read_field<std::uint8_t>(weapon, 0x17A);
    fc::write_field<std::uint8_t>(weapon, 0x179, (flags & ~0x40U) | (phase.gravity ? 0x40U : 0U));
    fc::write_field<std::uint8_t>(weapon, 0x17A, (flags2 & ~2U) | (phase.flee ? 2U : 0U));
    if (profile.alignment) {
        fc::write_field(weapon, 0x190, phase.alignment_z);
        fc::write_field(weapon, 0x194, phase.alignment_x);
    }
    if (profile.deflect) {
        fc::write_field(weapon, 0x198, phase.deflect_age);
        fc::write_field(weapon, 0x17B, phase.deflect_variant);
    }

    return true;
}

// Writes the transmitted melee fields; optional-group bits let readers consume them without weapon metadata.
template <class Put> void write_phase(const PhaseProfile &profile, const MeleePhase &phase, Put put) noexcept {
    put(phase.age, 32);
    const bool different = phase.previous != profile.combo;
    put(different, 1);
    if (different) {
        put(phase.previous, 5);
        put(phase.previous_duration, 32);
    }
    put(phase.clock, 32);
    put(phase.count, 4);
    put(phase.held >> 1, 7);
    put(phase.precise_ages, 1);
    for (std::size_t i = 0; i < phase.count; ++i) {
        put(phase.events[i].time, 32);
        put(phase.events[i].button, 3);
        put(phase.events[i].action, 3);
        if (phase.precise_ages) {
            const auto age = std::bit_cast<std::uint64_t>(phase.ages.values[i]);
            put(static_cast<std::uint32_t>(age), 32);
            put(static_cast<std::uint32_t>(age >> 32), 32);
        }
    }
    put(phase.blend, 32);
    put(phase.gravity, 1);
    put(phase.flee, 1);
    put(profile.alignment, 1);
    put(profile.deflect, 1);
    if (profile.alignment) {
        put(phase.alignment_z, 32);
        put(phase.alignment_x, 32);
    }
    if (profile.deflect) {
        put(phase.deflect_age, 32);
        put(phase.deflect_variant, 8);
    }
}

// Reads every transmitted melee field without receiver weapon metadata; invalid queue fields return nullopt.
template <class Get> std::optional<MeleePhase> read_phase(Get get) noexcept {
    MeleePhase phase;
    phase.age = get(32);
    const bool different = get(1) != 0;
    phase.previous_is_current = !different;
    phase.previous = different ? get(5) : 0;
    phase.previous_duration = different ? get(32) : 0;
    phase.clock = get(32);
    phase.count = static_cast<std::uint8_t>(get(4));
    phase.held = static_cast<std::uint8_t>(get(7) << 1);
    phase.precise_ages = get(1) != 0;
    bool valid = phase.count <= 8;
    for (std::size_t i = 0; i < phase.count; ++i) {
        QueueEvent event{get(32), static_cast<std::uint8_t>(get(3)), static_cast<std::uint8_t>(get(3))};
        std::uint64_t age{};
        if (phase.precise_ages) {
            age = get(32);
            age |= std::uint64_t{get(32)} << 32;
        }
        if (i < phase.events.size()) {
            phase.events[i] = event;
            phase.ages.values[i] = std::bit_cast<double>(age);
        }
        valid = valid && event.button != 0;
    }
    phase.blend = get(32);
    phase.gravity = get(1) != 0;
    phase.flee = get(1) != 0;
    phase.alignment = get(1) != 0;
    phase.deflect = get(1) != 0;
    if (phase.alignment) {
        phase.alignment_z = get(32);
        phase.alignment_x = get(32);
    }
    if (phase.deflect) {
        phase.deflect_age = get(32);
        phase.deflect_variant = static_cast<std::uint8_t>(get(8));
    }
    return valid ? std::optional{phase} : std::nullopt;
}

} // namespace fc_plugins::soldier_state_sync
