#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::multiplayer_gc {

// Lists the independently inspected Steam and GOG shell, cleanup and mission-team sites.
struct TransitionLayout {
    std::uint32_t create, host_delete, join_delete, stats_cancel;
    std::uint32_t next_mission_call, next_mission, postload_host, postload_join;
    std::uint32_t player_init_call, player_init, host_init_call, host_init, host_index_store, host_index;
    std::uint32_t comm_create_call, comm_create, driver_clear;
    std::uint32_t host_global, join_global, setup_mode, setup_mode_store;
    std::uint32_t allocate, time, connected, drop_player, player_reset;
    std::uint32_t ban_ip, host_update, set_team, postload, translate_side;
    std::uint32_t teams, teams_reference, character_change, character_slot_store, character_change_call;
    std::uint32_t spawn_team;
    // NetPlayer[64], stride 0x208; isTeam1 is +0x158 and teamSelected is +0x1D5.
    std::uint32_t net_players;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<TransitionLayout> select_transition_layout(std::string_view profile);

} // namespace fc_plugins::multiplayer_gc
