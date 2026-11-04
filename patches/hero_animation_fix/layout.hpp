#pragma once

#include <fc/native.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::hero_animation_fix {

// Resolves the animator's borrowed complete Soldier pointer; native detach clears it.
// Steam 0x63EA69 and GOG 0x63FB09 store the owner at +0x50; the animator has separate storage.
inline void *animator_soldier(void *animator) noexcept {
    return animator != nullptr ? fc::read_field<void *>(animator, 0x50) : nullptr;
}

// GetJoystickIndex maps a native player index to local input slot 0/1, or -1, using cdecl.
using GetJoystickIndex = std::int32_t(__cdecl *)(std::int32_t player_id) noexcept;

// PredictTurn restores controller input before the turn's pass flags are assigned.
using PredictFunction = bool(__cdecl *)() noexcept;
using PredictTurnFunction = void(__cdecl *)() noexcept;

// Melee functions use thiscall. Update takes elapsed seconds and returns whether the scheduled weapon survives.
using UpdateFunction = bool(__thiscall *)(void *weapon, float delta) noexcept;

// Read takes a NetPktGroup pointer and returns after the base read, combo setter and remaining flags.
using ReadFunction = void(__thiscall *)(void *weapon, void *packet) noexcept;

// ClearInput and the destructor body receive only the melee object in ECX.
using ResetFunction = void(__thiscall *)(void *weapon) noexcept;

// SetNetworkState receives a combo ID and the animated-movement flag from WeaponMelee::Read.
using SetNetworkStateFunction = void(__thiscall *)(void *weapon, std::int32_t state, bool flag) noexcept;

// EnterState selects the combo's animation and generic weapon mode and resets combo elapsed time.
using EnterStateFunction = void(__thiscall *)(void *weapon, std::int32_t state) noexcept;

// Direction uses XMM1/2; observers preserve those registers and leave the native AL result unchanged.
using JumpRestartFunction = bool(__thiscall *)(void *animator, bool force) noexcept;
using SoldierUpdateFunction = bool(__thiscall *)(void *controllable, float delta) noexcept;
// ReadObject receives the complete Soldier, packet stream and object state; RET 8 consumes both arguments.
using SoldierReadFunction = void(__thiscall *)(void *soldier, void *stream, void *object_state) noexcept;
// ClassifyGait receives the complete Soldier in ECX and elapsed seconds in XMM1, with no stack arguments.
using GaitFunction =
    fc::NativeCall<void(void *soldier, float delta) noexcept,
                   fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Xmm1>, fc::abi::Result<fc::abi::Eax>, fc::abi::NoCleanup>>;
// Native rendering takes four stack words; direction and timing also arrive in XMM registers.
using AnimationUpdateFunction = bool(__thiscall *)(void *animator, std::int32_t action, float, std::uint32_t, bool) noexcept;
using FireAudioFunction = void(__thiscall *)(void *weapon, bool secondary) noexcept;
// Render passes WeaponState, restart, current/next animation IDs and full-body to the animator.
using AnimatorStateFunction = void(__thiscall *)(void *, std::int32_t, bool, std::uint8_t, std::uint8_t, bool) noexcept;
using ComboAudioFunction = void(__thiscall *)(void *sound, const void *position, const void *velocity, std::uint32_t, bool) noexcept;

// OrdnanceMeleeThrow::Render receives its +0x98 Renderable in ECX; RET 0xC consumes view, time and flags.
using OrdnanceRenderFunction = void(__thiscall *)(void *renderable, std::uint8_t view, float time, std::uint32_t flags) noexcept;

// Retail airborne rendering reads these offsets relative to the embedded Renderable.
inline constexpr std::size_t kOrdnanceOwnerOffset = 0x58;
inline constexpr std::size_t kOrdnanceGenerationOffset = 0x5C;
inline constexpr std::size_t kOrdnanceThrowWeaponOffset = 0x60;
// Update marks complete OrdnanceMeleeThrow +0x16C when it catches; Renderable begins at +0x98.
inline constexpr std::size_t kOrdnanceCaughtOffset = 0xD4;
// The owner stores its generation at +0x204; the throw weapon stores its primary melee link at +0x110.
inline constexpr std::size_t kOwnerGenerationOffset = 0x204;
inline constexpr std::size_t kThrowPrimaryWeaponOffset = 0x110;
// WeaponMeleeThrow::NotifyReturned sets mask 0x04 on shared weapon state, independently of each projectile's lifetime.
inline constexpr std::size_t kThrowFlagsOffset = 0x114;
inline constexpr std::uint32_t kThrowReturnedFlag = 0x04;

// Groups retail melee hook locations, game counters/flags and the instructions used to qualify them.
struct Layout {
    fc::CodeLocation completed_turn;
    fc::CodeLocation fallback;
    fc::DataLocation<const float> turn_duration;
    fc::FunctionLocation<PredictFunction> predict;
    fc::FunctionLocation<PredictTurnFunction> predict_turn;
    fc::FunctionLocation<UpdateFunction> update;
    fc::FunctionLocation<ReadFunction> read;
    fc::FunctionLocation<SetNetworkStateFunction> set_network_state;
    fc::FunctionLocation<EnterStateFunction> enter_state;
    fc::FunctionLocation<ResetFunction> clear_input;
    fc::FunctionLocation<ResetFunction> destroy;
    fc::FunctionLocation<SoldierUpdateFunction> soldier_update;
    fc::FunctionLocation<SoldierReadFunction> soldier_read;
    fc::FunctionLocation<GaitFunction> gait;
    fc::FunctionLocation<ResetFunction> soldier_reset;
    fc::FunctionLocation<ResetFunction> soldier_destroy;
    fc::FunctionLocation<JumpRestartFunction> jump_restart;
    fc::FunctionLocation<AnimationUpdateFunction> animation_update;
    fc::FunctionLocation<FireAudioFunction> fire_audio;
    fc::FunctionLocation<AnimatorStateFunction> animator_state;
    fc::FunctionLocation<OrdnanceRenderFunction> ordnance_render;
    std::array<fc::CodeLocation, 2> combo_audio;
    std::array<fc::CodeLocation, 2> animation_selection;
    std::array<fc::CodeLocation, 2> animation_installed;
    std::array<fc::CodeLocation, 2> animation_unchanged;
    fc::DataLocation<const std::byte, 4> melee_vtable;
    fc::CodeLocation collision_start;
    fc::CodeLocation collision_return;
    fc::CodeLocation effects_start;
    fc::CodeLocation effects_timer;

    fc::FunctionLocation<GetJoystickIndex> get_joystick_index;

    // Current replay-loop turn, last acknowledged local input turn and advancing client update frontier.
    fc::DataLocation<const std::int32_t> prediction_turn;
    fc::DataLocation<const std::int32_t> acknowledged_turn;
    fc::DataLocation<const std::int32_t> update_turn;

    // netIsLocalTurn and netIsUpdateTurn can both be true in the same NetGame::Predict iteration.
    fc::DataLocation<const std::uint8_t> local_prediction_pass;

    fc::DataLocation<const std::uint8_t> network_enabled;
    // netOnClient identifies the currently selected native object world.
    fc::DataLocation<const std::uint8_t> network_client_active;

    // These instruction checks verify where native code reads or writes the counters and flags above.
    fc::CodeLocation prediction_pass_context;
    fc::CodeLocation authoritative_pass_context;
    fc::CodeLocation acknowledged_turn_context;
    fc::CodeLocation network_state_context;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::hero_animation_fix
