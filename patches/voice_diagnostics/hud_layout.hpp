#pragma once

#include "layout.hpp"

namespace fc_plugins::voice_diagnostics {

using HudCreate = void *(__thiscall *)(void *, void *) noexcept;
using HudMethod = void(__thiscall *)(void *) noexcept;
using HudBind = void(__thiscall *)(void *, std::int32_t) noexcept;
using StatusUpdate = bool(__thiscall *)(void *, float) noexcept;
using PlayerIndex = std::int32_t(__cdecl *)(std::int32_t) noexcept;
using PlayerQuery = bool(__thiscall *)(void *, std::uint8_t) noexcept;
using ReceiveQuery = bool(__thiscall *)(void *, std::uint8_t, std::uint8_t) noexcept;
using DrawElement = void(__thiscall *)(void *, const float *, const std::uint8_t *) noexcept;
using MenuQuery = bool(__cdecl *)() noexcept;

enum class HudGlobal : std::size_t {
    Clock,
    Characters,
    CharacterCount,
    ForcedHide,
    RenderSuppressed,
    InterfaceEnabled,
    ViewportCount,
    VoiceVtable,
    ViewportFlags,
    ViewportRoots,
    Count
};

// All offsets belong to the retail PC HUD and are qualified separately for Steam and GOG.
struct HudLayout {
    fc::FunctionLocation<HudCreate> create;
    fc::FunctionLocation<StatusUpdate> status_update;
    fc::FunctionLocation<HudBind> bind;
    fc::FunctionLocation<HudMethod> update, add_players, node_update;
    fc::FunctionLocation<DrawElement> group_draw, text_draw;
    fc::CallLocation<PlayerIndex> status_local, status_spectated, hud_local;
    fc::CallLocation<MenuQuery> menu_hidden;
    fc::CallLocation<GetNumNodes> node_count;
    fc::CallLocation<PlayerQuery> sending;
    fc::CallLocation<ReceiveQuery> permission;
    fc::CodeLocation character, timestamps, glyph_submit, render_gate;
    std::array<std::uint32_t, static_cast<std::size_t>(HudGlobal::Count)> globals;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<HudLayout> select_hud_layout(std::string_view profile);

} // namespace fc_plugins::voice_diagnostics
