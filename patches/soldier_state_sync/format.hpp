#pragma once

#include "patch.hpp"

#include <cstdint>

namespace fc_plugins::soldier_state_sync {

// Native count bit 7 means compression; index bit 7 identifies a negotiated Soldier format.
inline constexpr std::uint8_t kRawMagnitude = 0x80;
inline constexpr std::uint8_t kIndexMask = 0x7F;
inline constexpr std::uint8_t kUpdatePacket = 0x0C;

enum class Format : std::uint8_t {
    Native,
    Velocity32,
    State32,
    Precision,
    WeaponState
};

// The tag remains in packet storage; only fragment ordering removes it.
constexpr std::uint8_t fragment_index(std::uint8_t index) noexcept {
    return index & kIndexMask;
}

// All update formats share native receive processing and FIFO order.
constexpr bool update_packet(std::uint8_t type) noexcept {
    return type == kUpdatePacket || type == kStateUpdatePacket || type == kPrecisionUpdatePacket || type == kWeaponStateUpdatePacket;
}

// Each retained packet describes its own representation without consulting current agreement.
constexpr Format packet_format(std::uint8_t type, std::uint8_t index) noexcept {
    if ((index & kRawMagnitude) == 0) {
        return Format::Native;
    }
    if (type == kStateUpdatePacket) {
        return Format::State32;
    }
    if (type == kWeaponStateUpdatePacket) {
        return Format::WeaponState;
    }
    if (type == kPrecisionUpdatePacket) {
        return Format::Precision;
    }
    return type == kUpdatePacket ? Format::Velocity32 : Format::Native;
}

// The state format includes the previously negotiated raw velocity magnitude.
constexpr bool raw_magnitude(std::uint8_t type, std::uint8_t index) noexcept {
    return packet_format(type, index) != Format::Native;
}

// New clock writes require their own agreement; old peers keep their velocity-only format.
constexpr Format select_format(bool velocity_compatible, bool state_compatible, bool precision_compatible = false,
                               bool weapon_state_compatible = false) noexcept {
    return weapon_state_compatible ? Format::WeaponState
           : precision_compatible  ? Format::Precision
           : state_compatible      ? Format::State32
           : velocity_compatible   ? Format::Velocity32
                                   : Format::Native;
}

// Selects only the Soldier state-clock bit count at its qualified native callsites.
constexpr std::int32_t state_clock_bits(Format format) noexcept {
    return format == Format::State32 || format == Format::Precision || format == Format::WeaponState ? 32 : 13;
}

// Precision and WeaponState retain all shared Trigger durations in the selected update group and the Soldier energy scalar.
constexpr bool precise_gameplay(Format format) noexcept {
    return format == Format::Precision || format == Format::WeaponState;
}

// Native WriteBits writes the low bits first: five state bits, then all 21 duration bits.
constexpr std::uint32_t trigger_code(std::uint32_t trigger) noexcept {
    return (trigger & 0x1FU) | (((trigger >> 8) & 0x1FFFFFU) << 5);
}

// Replaces native state bits 0..4 and duration bits 8..28, retaining the other six flags.
constexpr std::uint32_t restore_trigger(std::uint32_t destination, std::uint32_t state, std::uint32_t duration) noexcept {
    return (destination & ~0x1FFFFF1FU) | (state & 0x1FU) | ((duration & 0x1FFFFFU) << 8);
}

// Count shares its wire byte with compression, allowing at most 127 fragments and index 126.
constexpr bool valid_fragment(std::uint8_t index, std::uint8_t count) noexcept {
    return count != 0 && count <= kIndexMask && fragment_index(index) < count;
}

// The state type requires its tag; tagged updates must fit the native fragment range.
constexpr bool valid_update_fragment(std::uint8_t type, std::uint8_t index, std::uint8_t count) noexcept {
    return ((type != kStateUpdatePacket && type != kPrecisionUpdatePacket && type != kWeaponStateUpdatePacket) ||
            (index & kRawMagnitude) != 0) &&
           (!raw_magnitude(type, index) || valid_fragment(index, count));
}

// Native sequence/source checks accompany this additional format and tagged-count comparison.
constexpr bool same_group_format(std::uint8_t incoming_type, std::uint8_t incoming_index, std::uint8_t incoming_count,
                                 std::uint8_t pending_type, std::uint8_t pending_index, std::uint8_t pending_count) noexcept {
    return incoming_type == pending_type && ((incoming_index ^ pending_index) & kRawMagnitude) == 0 &&
           ((incoming_index & kRawMagnitude) == 0 || incoming_count == pending_count);
}

// Binding a frozen send decision to one stream prevents nested receives from borrowing its format.
struct SendFormat {
    void *group{};
    Format format{};

    // Records the first stream serialized by this invocation.
    void bind(void *stream) noexcept {
        if (group == nullptr) {
            group = stream;
        }
    }

    // Enables an extended representation only for this invocation's bound stream.
    bool applies(const void *stream) const noexcept {
        return format != Format::Native && group != nullptr && group == stream;
    }
};

} // namespace fc_plugins::soldier_state_sync
