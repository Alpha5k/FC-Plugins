#pragma once

#include <fc/native.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::crouch_bug_fix {

struct Trigger;

// Trigger::Update takes this in ECX, elapsed seconds in XMM1 and a stack Boolean that it removes on return.
using TriggerUpdate =
    fc::NativeCall<void(Trigger *trigger, float elapsed_time, bool down) noexcept, fc::abi::Ecx, fc::abi::Xmm1, fc::abi::CalleeCleanup>;

// Maps a player ID to its local input slot, or returns -1 when the player has no local input.
using GetJoystickIndex = std::int32_t(__cdecl *)(std::int32_t player_id) noexcept;

// Identifies the player controller's crouch call and the lookup of its local input slot.
struct Layout {
    // Controllable::mControlCrouch, passed by PlayerController::Update.
    static constexpr std::size_t kCrouchTriggerOffset = 0x48;
    // Signed native player index on Controllable.
    static constexpr std::size_t kPlayerIdOffset = 0xD4;

    // The first Trigger dword contains the retained down level in bit zero.
    static constexpr std::uint32_t kTriggerLevel = 0x01;

    static constexpr std::size_t kArgumentSize = 23;
    fc::FunctionLocation<GetJoystickIndex> get_joystick_index;

    fc::CodeLocation crouch_arguments;
    fc::CallLocation<TriggerUpdate> crouch_update;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::crouch_bug_fix
