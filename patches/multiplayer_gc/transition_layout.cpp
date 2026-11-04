#include "transition_layout.hpp"

#include <array>

namespace fc_plugins::multiplayer_gc {
namespace {

struct Profile {
    std::string_view profile;
    TransitionLayout sites;
};

// Preferred-base VAs were checked in both retail images; all entries below are RVAs.
constexpr std::array kProfiles{
    Profile{.profile = "BF2.Steam.59EDE353",
            .sites = {.create = 0x1B5890,
                      .host_delete = 0x1DC210,
                      .join_delete = 0x1DFB10,
                      .stats_cancel = 0x13334E,
                      .next_mission_call = 0x133320,
                      .next_mission = 0x1A47D0,
                      .postload_host = 0x1AAED44,
                      .postload_join = 0x1AAED48,
                      .player_init_call = 0x1B5DCD,
                      .player_init = 0x1B5280,
                      .host_init_call = 0x1B5E27,
                      .host_init = 0x1B5820,
                      .host_index_store = 0x1B5E36,
                      .host_index = 0x1A62F0C,
                      .comm_create_call = 0x1B5F5D,
                      .comm_create = 0x1B2430,
                      .driver_clear = 0x1B6034,
                      .host_global = 0x1AAED3C,
                      .join_global = 0x1AAED38,
                      .setup_mode = 0x3E8EE0,
                      .setup_mode_store = 0x1E23A1,
                      .allocate = 0x2C3540,
                      .time = 0x1B28A0,
                      .connected = 0x1B2720,
                      .drop_player = 0x1DCEB0,
                      .player_reset = 0x1DB2C0,
                      .ban_ip = 0x1DC4D0,
                      .host_update = 0x1DC4E0,
                      .set_team = 0x1C44B0,
                      .postload = 0x131A70,
                      .translate_side = 0x2536A0,
                      .teams = 0x3E9AA0,
                      .teams_reference = 0x05234D,
                      .character_change = 0x052330,
                      .character_slot_store = 0x052EA8,
                      .character_change_call = 0x052EB1,
                      .spawn_team = 0x02B740,
                      .net_players = 0x1ACDAA0}},
    Profile{.profile = "BF2.GOG.59EDF52B",
            .sites = {.create = 0x1B6840,
                      .host_delete = 0x1DD2C0,
                      .join_delete = 0x1E0BC0,
                      .stats_cancel = 0x1340B3,
                      .next_mission_call = 0x134085,
                      .next_mission = 0x1A5780,
                      .postload_host = 0x1AB01F8,
                      .postload_join = 0x1AB01FC,
                      .player_init_call = 0x1B6D7D,
                      .player_init = 0x1B6230,
                      .host_init_call = 0x1B6DD7,
                      .host_init = 0x1B67D0,
                      .host_index_store = 0x1B6DE6,
                      .host_index = 0x1A643BC,
                      .comm_create_call = 0x1B6F0D,
                      .comm_create = 0x1B33D0,
                      .driver_clear = 0x1B6FE4,
                      .host_global = 0x1AB01F0,
                      .join_global = 0x1AB01EC,
                      .setup_mode = 0x3E9EE0,
                      .setup_mode_store = 0x1E3451,
                      .allocate = 0x2C45D0,
                      .time = 0x1B3840,
                      .connected = 0x1B36C0,
                      .drop_player = 0x1DDF60,
                      .player_reset = 0x1DC370,
                      .ban_ip = 0x1DD580,
                      .host_update = 0x1DD590,
                      .set_team = 0x1C5440,
                      .postload = 0x1327D0,
                      .translate_side = 0x254740,
                      .teams = 0x3EAAA0,
                      .teams_reference = 0x05232D,
                      .character_change = 0x052310,
                      .character_slot_store = 0x052E88,
                      .character_change_call = 0x052E91,
                      .spawn_team = 0x02B700,
                      .net_players = 0x1ACEF50}},
};

} // namespace

std::optional<TransitionLayout> select_transition_layout(std::string_view profile) {
    for (const auto &candidate : kProfiles) {
        if (candidate.profile == profile) {
            return candidate.sites;
        }
    }
    return std::nullopt;
}

} // namespace fc_plugins::multiplayer_gc
