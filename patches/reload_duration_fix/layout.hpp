#pragma once

#include <fc/native.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::reload_duration_fix {

struct NetPktGroup;
struct Weapon;

// Weapon::Read takes its weapon in ECX and one packet pointer on the callee-cleaned stack.
using WeaponRead = void(__thiscall *)(Weapon *weapon, NetPktGroup *packet) noexcept;

// The cdecl lookup maps a player ID to its local input slot, or returns -1.
using GetJoystickIndex = std::int32_t(__cdecl *)(std::int32_t player_id) noexcept;

// WeaponClass pointer used to retrieve reload settings.
inline constexpr std::size_t kWeaponClassOffset = 0x64;
// Owning Controllable pointer used to identify the player.
inline constexpr std::size_t kWeaponOwnerOffset = 0x6C;
// Integer WeaponState decoded by Weapon::Read.
inline constexpr std::size_t kWeaponStateOffset = 0xB0;
// Floating-point state duration in seconds, compared with elapsed time by ReloadDone.
inline constexpr std::size_t kWeaponStateLimitOffset = 0xB8;

// Floating-point reload duration in seconds on WeaponClass.
inline constexpr std::size_t kClassReloadTimeOffset = 0x9C;

// Signed player index on the owning Controllable.
inline constexpr std::size_t kPlayerIdOffset = 0xD4;

// WeaponState::RELOAD in both retail builds.
inline constexpr std::int32_t kReloadState = 4;

// Supplies the native state decoder and per-player local input lookup for a selected image.
struct Layout {
    fc::FunctionLocation<WeaponRead> weapon_read;
    fc::FunctionLocation<GetJoystickIndex> get_joystick_index;
    // Relocated WeaponMelee vtable operand in its constructor.
    fc::DataLocation<const std::byte, 4> melee_vtable;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::reload_duration_fix
