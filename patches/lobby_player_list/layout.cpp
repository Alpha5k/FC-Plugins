#include "layout.hpp"

#include <array>
#include <string_view>

namespace fc_plugins::lobby_player_list {
namespace {

// Stores the game-update boundary, roster/statistics sources and browser identities for one executable.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t native_update;
    std::uint32_t player_count;
    std::uint32_t player_name;
    std::uint32_t browser_operation;
    std::uint32_t browser_vtable;
    std::uint32_t browser_update;
    std::uint32_t browser_dirty_lookup;
    std::uint32_t character_storage;
    std::uint32_t player_totals;
    std::uint32_t character_lookup;
    std::uint32_t stats_lookup;
    std::uint32_t points_lookup;
    std::uint32_t kills_lookup;
    std::uint32_t deaths_store;
};

constexpr std::array kProfiles{
    ProfileSites{
        .profile = "BF2.Steam.59EDE353",
        .native_update = {0x0013AE90},
        .player_count = {0x00192020},
        .player_name = {0x001C36A0},
        .browser_operation = {0x01AAED40},
        .browser_vtable = {0x003AC340},
        .browser_update = 0x001DDC90,
        .browser_dirty_lookup = 0x001DF460,
        .character_storage = 0x01A30330,
        .player_totals = 0x01ACC108,
        .character_lookup = 0x00037510,
        .stats_lookup = 0x00053530,
        .points_lookup = 0x0021DAD0,
        .kills_lookup = 0x001B35B0,
        .deaths_store = 0x001B35E0,
    },
    ProfileSites{
        .profile = "BF2.GOG.59EDF52B",
        .native_update = {0x0013BBF0},
        .player_count = {0x00192FC0},
        .player_name = {0x001C4630},
        .browser_operation = {0x01AB01F4},
        .browser_vtable = {0x003AD2AC},
        .browser_update = 0x001DED40,
        .browser_dirty_lookup = 0x001E0510,
        .character_storage = 0x01A317D0,
        .player_totals = 0x01ACD5B8,
        .character_lookup = 0x00037500,
        .stats_lookup = 0x00053510,
        .points_lookup = 0x0021EB30,
        .kills_lookup = 0x001B4560,
        .deaths_store = 0x001B4590,
    },
};

// Associates retail RVAs with native roster/statistics sources and browser identity.
Layout make_layout(const ProfileSites &sites) {
    return {
        .native_update = {.rva = sites.native_update, .name = "game state update"},
        .player_count = {.rva = sites.player_count, .name = "Lua_Callbacks::GetLobbyPlayerCount"},
        // The ordinal-to-slot helper follows the player-count helper by 0x50 bytes.
        .player_index = {.rva = (sites.player_count + 0x50), .name = "Lua_Callbacks::GetLobbyPlayerIndex"},
        .player_name = {.rva = sites.player_name, .name = "NetGame::GetPlayerName"},
        .browser_operation = {.rva = sites.browser_operation, .name = "current find or join operation"},
        .browser_vtable = {.rva = sites.browser_vtable, .name = "CNetFindGame vtable"},
        .browser_update = {.rva = sites.browser_update,
                           .name = "CNetFindGame::Update",
                           .evidence =
                               fc::exact_bytes({0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xA0, 0x02, 0x00, 0x00, 0x56, 0x57, 0x89, 0x4D, 0xF0})},
        .browser_dirty_lookup = {.rva = sites.browser_dirty_lookup, .name = "CNetFindGame dirty byte getter"},
        // GetPtr reads count/base; GetPlayerStats maps the same slot into 0x1F8-byte records.
        .character_storage = {.rva = sites.character_storage, .name = "Character count and storage pointers"},
        // GetPLKills and AddDeath's setter use signed 32-bit totals separated by four bytes and a 0x60 slot stride.
        .player_totals = {.rva = sites.player_totals, .name = "NetComm player kills and deaths"},
        .character_lookup = {.rva = sites.character_lookup, .name = "Character::GetPtr storage references"},
        .stats_lookup = {.rva = sites.stats_lookup, .name = "Character::GetPlayerStats stride and storage reference"},
        .points_lookup = {.rva = sites.points_lookup, .name = "PlayerStats::GetPoints signed score reads"},
        .kills_lookup = {.rva = sites.kills_lookup, .name = "NetComm::GetPLKills stride and field reference"},
        .deaths_store = {.rva = sites.deaths_store, .name = "NetComm::SetPLDeaths stride and field reference"},
    };
}

} // namespace

std::optional<Layout> select_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (sites.profile == profile) {
            return make_layout(sites);
        }
    }

    return std::nullopt;
}

} // namespace fc_plugins::lobby_player_list
