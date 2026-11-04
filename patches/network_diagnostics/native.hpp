#pragma once

#include <fc/plugin.hpp>
#include <fc/native.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace fc_plugins::network_diagnostics::native {

// Qualified x86 signatures for the native boundaries observed by the diagnostics plan.
using VoidFunction = void(__cdecl *)() noexcept;
using SubmitMove = void(__cdecl *)(std::int32_t requested) noexcept;
using PacketFunction = void(__cdecl *)(void *packet) noexcept;
using Predict = bool(__cdecl *)() noexcept;
// Soldier Update receives the Controllable subobject at Soldier+240, elapsed seconds, and returns AL.
using SoldierUpdate = bool(__thiscall *)(void *controllable, float delta) noexcept;
using GetJoystickIndex = std::int32_t(__cdecl *)(std::int32_t player) noexcept;
using WriteMove = void(__cdecl *)(void *packet, const void *previous, const void *current) noexcept;
using WriteMiniMove = void(__cdecl *)(void *packet, const void *move) noexcept;

// Publishes a matrix into SmoothInfo's history for subsequent rendering interpolation.
using SetRenderMatrix = void(__thiscall *)(void *smooth, const float *matrix) noexcept;

// Receives the soldier in ECX, with packet stream and object state on the stack.
using SoldierRead = void(__thiscall *)(void *soldier, void *stream, void *object_state) noexcept;

using ReadMove = bool(__cdecl *)(void *packet, const void *previous, void *output) noexcept;
using ReadMiniMove = void(__cdecl *)(void *packet, void *output) noexcept;
using FillTurns = void(__cdecl *)(std::int32_t player, std::uint32_t end_turn) noexcept;

using CreateOrdnanceEvent = void(__cdecl *)(const void *projectile_class, const void *descriptor) noexcept;

// Controller Update delta uses seconds.
using PlayerControllerUpdate = void(__thiscall *)(void *controller, float delta) noexcept;
// Fire can return true during client simulation without creating an authority projectile.
using WeaponFire = bool(__thiscall *)(void *weapon) noexcept;
// Builds an OrdnanceBullet; pool exhaustion can return null.
using BulletBuild = void *(__thiscall *)(void *projectile_class, const void *descriptor) noexcept;
// Bullet Update delta uses seconds; false requests removal.
using BulletUpdate = bool(__thiscall *)(void *projectile, float delta) noexcept;

// Both base damage overloads return the target's alive state after the call.
using ApplyDamage = bool(__thiscall *)(void *damageable, const void *descriptor, std::uint32_t flags) noexcept;
using ApplyDamageAtPosition = bool(__thiscall *)(void *damageable, const void *descriptor, const void *direction,
                                                 const void *impact_position, std::uint32_t damage_type, std::uint32_t flags) noexcept;

inline constexpr std::size_t kPlayerCapacity = 64;
inline constexpr std::size_t kTurnCapacity = 32;
inline constexpr std::size_t kPlayerStateStride = 0x208;

// The recorder packs the primary flags' low 24 bits and secondary flags' low byte into one trace field.
inline constexpr std::uint32_t kRayPrimaryFlagsMask = 0x00FFFFFFU;
inline constexpr std::uint32_t kRaySecondaryFlagsMask = 0xFFU;
inline constexpr std::uint32_t kRaySecondaryFlagsShift = 24U;

// Player moves contain four axes, packed buttons, and the input-device flag in both history rings.
struct PlayerMove {
    std::array<float, 4> axes;

    std::uint32_t input_bits{};
    std::uint8_t joystick{};
    std::array<std::uint8_t, 3> reserved{};
};

static_assert(sizeof(PlayerMove) == 0x18);

// Copies a native field, returning a value-initialized result for a null pointer or zero address.
// Other addresses must remain readable for sizeof(Value) bytes at the signed offset.
template <class Value, class Address> Value read(Address object, std::ptrdiff_t offset = 0) noexcept {
    return object ? fc::read_field<Value>(object, offset) : Value{};
}

// Copies native fields from storage that may be unaligned. A missing input leaves the output unchanged.
template <class Value, std::size_t Size> void copy(std::array<Value, Size> &output, const void *input, std::ptrdiff_t offset = 0) noexcept {
    if (input != nullptr) {
        output = fc::read_field<std::array<Value, Size>>(input, offset);
    }
}

// Copies an opaque x86 address into the trace; object reuse can give the same identity to a later object.
inline std::uint32_t pointer_id(const void *pointer) noexcept {
    return reinterpret_cast<std::uint32_t>(pointer);
}

} // namespace fc_plugins::network_diagnostics::native
