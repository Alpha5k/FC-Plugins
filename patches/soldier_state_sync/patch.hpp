#pragma once

#include <fc/plugin.hpp>

#include <cstdint>

namespace fc_plugins::soldier_state_sync {

// FC.SoldierState32 uses this unassigned native type and retains the tagged fragment index.
inline constexpr std::uint8_t kStateUpdatePacket = 0x1F;

// FC.SoldierPrecision adds exact shared Trigger fields and Soldier energy; PktType assigns neither 0x1F nor 0x20.
inline constexpr std::uint8_t kPrecisionUpdatePacket = 0x20;

// FC.SoldierWeaponState adds conditional exact heat and the active melee phase to Precision.
inline constexpr std::uint8_t kWeaponStateUpdatePacket = 0x21;

// Preserves Soldier update fields and corrects native movement rotation and received trigger timers for each launch role.
fc::Patch definition(fc::Role role);

} // namespace fc_plugins::soldier_state_sync
