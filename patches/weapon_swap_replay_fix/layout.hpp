#pragma once

#include "state.hpp"

#include <fc/native.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::weapon_swap_replay_fix {

// Maps a native player ID to its local input slot, or returns -1 for a nonlocal player.
using GetJoystickIndex = int(__cdecl *)(int player_id) noexcept;

// GetLocalPlayer takes its local slot in ECX and returns the native player ID in EAX, or -1.
using GetLocalPlayer = int(__fastcall *)(int local_player) noexcept;

// ResetObject and the destructor receive the complete EntitySoldier in ECX and take no stack arguments.
using SoldierLifetime = void(__thiscall *)(void *soldier) noexcept;

// Render receives its Renderable subobject in ECX and three four-byte stack arguments, cleaned by RET 0x0C.
using SoldierRender = void(__thiscall *)(void *renderable, std::uint32_t first_person, float opacity, std::uint32_t flags) noexcept;

// One player's queued input record; the native ring holds 32 turns with two 0x18-byte records per turn.
struct LocalMove {
    // Four processed analog inputs precede the button bitmask.
    std::array<float, 4> input_values;
    std::uint32_t buttons;
    // Native joystick-usage state copied with the input.
    std::array<std::byte, 4> device_state;
};

static_assert(offsetof(LocalMove, buttons) == 0x10);
static_assert(sizeof(LocalMove) == 0x18);

using LocalMoveTurn = std::array<LocalMove, kLocalPlayerCount>;
using LocalMoveHistory = std::array<LocalMoveTurn, kMoveHistoryTurnCount>;

static_assert(sizeof(LocalMoveHistory) == 0x600);

// Gives selection hooks, presentation lookups and queued-input globals for one retail executable.
struct Layout {
    fc::FunctionLocation<SoldierLifetime> reset_soldier;
    fc::FunctionLocation<SoldierLifetime> destroy_soldier;
    fc::CodeLocation submitted_move;
    fc::FunctionLocation<SoldierRender> render;
    fc::CodeLocation local_selection;
    fc::CodeLocation packed_sync;
    fc::CodeLocation authority_selection;
    fc::CodeLocation packed_authority;

    fc::CodeLocation weapon_select;
    fc::CodeLocation hud_weapon;
    fc::CodeLocation render_selection;
    fc::CodeLocation render_channel;
    fc::CodeLocation model_selection;
    std::array<fc::CodeLocation, 2> render_companion;
    fc::CodeLocation model_companion;

    fc::CodeLocation switch_latch;
    fc::CodeLocation primary_switch_call;
    fc::CodeLocation secondary_switch_call;

    fc::CodeLocation update_turn_context;
    fc::CodeLocation prediction_turn_context;
    fc::CodeLocation acknowledged_turn_context;
    fc::CodeLocation network_state_context;
    fc::CodeLocation client_turn_context;
    fc::CodeLocation move_history_context;

    fc::DataLocation<const std::int32_t> update_turn;
    fc::DataLocation<const std::int32_t> prediction_turn;
    fc::DataLocation<const std::int32_t> acknowledged_turn;

    fc::DataLocation<const std::uint8_t> network_enabled;
    // netOnClient selects the native client world, including in a player-hosted game.
    fc::DataLocation<const std::uint8_t> network_client_active;

    // Next turn SubmitMove will write into the ring.
    fc::DataLocation<const std::int32_t> client_turn;
    fc::DataLocation<const LocalMoveHistory> local_move_history;

    fc::FunctionLocation<GetJoystickIndex> get_joystick_index;
    fc::FunctionLocation<GetLocalPlayer> get_local_player;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::weapon_swap_replay_fix
