#pragma once

#include <fc/native.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace fc_plugins::hero_diagnostics::native {

// Retail melee combo IDs index the combo definition's rows; state and input timers use seconds.
constexpr std::size_t kModeOffset = 0xB0;
constexpr std::size_t kModeTimeOffset = 0xB4;
constexpr std::size_t kStateOffset = 0x180;
constexpr std::size_t kPreviousStateOffset = 0x184;
constexpr std::size_t kStateTimeOffset = 0x188;
constexpr std::size_t kInputTimeOffset = 0x130;

// The Controllable owner stores dimensionless thrust, strafe, turn, and pitch for OverrideSoldierControls.
constexpr std::size_t kControlOffset = 0x80;

// Soldier position uses native distance units; velocity uses those units per second.
constexpr std::size_t kPositionOffset = 0x120;
constexpr std::size_t kVelocityOffset = 0x4DC;

// The weapon embeds an eight-entry queue with count, head index, and held-button mask after its entries.
constexpr std::size_t kInputQueueOffset = 0x134;
constexpr std::size_t kInputQueueCountOffset = 0x40;
constexpr std::size_t kInputQueueHeadOffset = 0x41;
constexpr std::size_t kInputQueueDownOffset = 0x42;

constexpr std::size_t kComboRowsOffset = 0x10;
constexpr std::size_t kComboCountOffset = 0x21;
constexpr std::size_t kComboRowSize = 0x34;

// Bits: animated move, saber on, sound on, input lock, update/soldier reachability, gravity done, AI deflect.
constexpr std::size_t kStateFlagsOffset = 0x179;
// Four packed trail, saber-on, previous-on, and sound masks.
constexpr std::size_t kActionOffset = 0x17C;

constexpr std::size_t kRemoteMoveStride = 0x18;
constexpr std::size_t kPlayerCapacity = 64;
constexpr std::size_t kMoveHistoryTurns = 32;
constexpr std::uint32_t kMoveHistoryTurnMask = 0x1F;

// Controllable Trigger words contain button state and edges.
constexpr std::size_t kPrimaryTriggerOffset = 0x38;
constexpr std::size_t kSecondaryTriggerOffset = 0x3C;
constexpr std::size_t kJumpTriggerOffset = 0x44;
constexpr std::size_t kSprintTriggerOffset = 0x4C;

constexpr std::size_t kMovementFlagsOffset = 0x471;
constexpr std::size_t kLocomotionStateOffset = 0x744;
// Steam/GOG Soldier::Update increments Controllable +0x50C; the soldier embeds it at +0x240.
constexpr std::size_t kLocomotionStateTimeOffset = 0x74C;
// NotifySoldierState retains its packed posture in the base weapon and elapsed soldier time in melee storage.
constexpr std::size_t kPostureOffset = 0xAC;
constexpr std::size_t kNotifiedStateTimeOffset = 0x118;

// Energy is a native resource amount and can fall below zero; bit 0 marks exhaustion/overheat.
constexpr std::size_t kEnergyOffset = 0xA14;
constexpr std::size_t kEnergyFlagsOffset = 0xA18;
constexpr std::uint32_t kEnergyExhausted = 1U;
// Movement flag bit 5 identifies airborne soldiers.
constexpr std::uint8_t kAirborne = 1U << 5U;

// The owner points to EntitySoldier's embedded Controllable; its player index selects a native slot.
constexpr std::size_t kWeaponOwnerOffset = 0x6C;
constexpr std::size_t kComboOffset = 0x114;

constexpr std::size_t kControllableOffset = 0x240;
constexpr std::size_t kPlayerOffset = 0xD4;

constexpr std::size_t kAliveFlagsOffset = 0x1FC;
constexpr std::uint8_t kAliveFlag = 1U << 3U;

constexpr std::size_t kWeaponArrayOffset = 0x720;
constexpr std::size_t kPackedSelectionOffset = 0x742;

constexpr std::size_t kSoldierAnimatorOffset = 0x750;

constexpr std::uint8_t kPackedWeaponIndexMask = 0x0F;
constexpr std::uint8_t kPackedWeaponSign = 0x08;
constexpr std::int32_t kPackedWeaponRange = 16;
constexpr std::int32_t kWeaponSlots = 8;

// Processed input rows hold four axes, a packed input word, a device byte, and three remaining bytes.
struct PlayerMove {
    std::array<float, 4> axes;
    std::uint32_t input_bits{};
    std::uint8_t joystick{};
    std::array<std::uint8_t, 3> reserved{};

    // Change detection compares every field, including reserved bytes, using ordinary float equality.
    bool operator==(const PlayerMove &) const noexcept = default;
};

static_assert(sizeof(PlayerMove) == kRemoteMoveStride);

// Copies a field from borrowed game storage; a null pointer or zero address produces a zero sample.
// Other addresses must remain readable for sizeof(Value) bytes at the signed offset.
template <class Value, class Address> Value read(Address object, std::ptrdiff_t offset = 0) noexcept {
    return object ? fc::read_field<Value>(object, offset) : Value{};
}

// Copies floats from live game storage that may be unaligned; null leaves the output unchanged.
template <std::size_t Size> void copy_floats(std::array<float, Size> &output, const void *object, std::ptrdiff_t offset) noexcept {
    if (object != nullptr) {
        output = fc::read_field<std::array<float, Size>>(object, offset);
    }
}

// Samples forward velocity, lateral velocity, override blend, and vertical velocity through scalar references.
inline std::array<float, 4> vector_arguments(float *first, float *second, float *third, float *fourth) noexcept {
    return {first != nullptr ? *first : 0.0F, second != nullptr ? *second : 0.0F, third != nullptr ? *third : 0.0F,
            fourth != nullptr ? *fourth : 0.0F};
}

// Encodes the borrowed x86 address; equal addresses across allocations or sessions can refer to different objects.
inline std::uint32_t pointer_id(const void *pointer) noexcept {
    return reinterpret_cast<std::uint32_t>(pointer);
}

} // namespace fc_plugins::hero_diagnostics::native
