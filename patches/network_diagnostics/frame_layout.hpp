#pragma once

#include <fc/native.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::network_diagnostics {

using MainFrame = bool(__cdecl *)() noexcept;
using FrameStep = void(__cdecl *)() noexcept;

// Native main-loop boundaries separate background wait, frame pacing, game work and network flush.
struct FrameLayout {
    fc::CallLocation<MainFrame> frame;
    fc::CodeLocation background_wait;
    fc::CallLocation<FrameStep> pacing;
    fc::CodeLocation game;
    fc::CodeLocation game_end;
    fc::CodeLocation pacing_target;
    fc::CallLocation<FrameStep> network_flush;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<FrameLayout> select_frame_layout(std::string_view profile);

} // namespace fc_plugins::network_diagnostics
