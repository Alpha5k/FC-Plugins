#pragma once

#include <fc/native.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::shot_sequence {

// GetJoystickIndex uses cdecl and returns local input slot 0/1, or -1 for a remote player.
using GetJoystickIndex = std::int32_t(__cdecl *)(std::int32_t player) noexcept;

struct Layout {
    fc::CodeLocation compare;
    fc::CodeLocation incomplete_branch;
    fc::CodeLocation pattern_store;
    fc::FunctionLocation<GetJoystickIndex> get_joystick_index;
    fc::DataLocation<const std::uint8_t> network_enabled;
    fc::DataLocation<const std::uint8_t> client_world;
    fc::DataLocation<const std::uint8_t> local_turn;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::shot_sequence
