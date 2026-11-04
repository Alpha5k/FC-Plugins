#include "layout.hpp"

#include <array>
#include <string_view>

namespace fc_plugins::galaxy_buddies {
namespace {

// Stores each executable's RVAs and required images. Derived wrapper RVAs share verified spacing.
struct ProfileSites {
    std::string_view profile;
    bool needs_peer;
    std::uint32_t native_update;
    std::uint32_t get_friends;
    std::uint32_t get_error;
    std::uint32_t get_galaxy_instance;
    std::uint32_t request_accepted;
    std::uint32_t lobby_count;
    std::uint32_t lobby_player_name;
    std::uint32_t is_player_buddy;
    std::uint32_t get_player;
    std::uint32_t map_buddy_color, name_buddy_color, voice_buddy_color;
    std::uint32_t set_special_join_pending;
    std::uint32_t publish_friends;
    std::uint32_t add_lobby;
    std::uint32_t lobby_id_format;
    std::uint32_t lobby_players;
    std::uint32_t accepted_ready;
    std::uint32_t requested_lobby;
    std::uint32_t special_join_target;
    std::optional<std::uint32_t> local_display_name;
};

// Steam requires a peer image and provides its platform name through SteamAuthProvider; GOG uses Galaxy's persona.
constexpr std::array kProfiles{
    ProfileSites{
        .profile = "BF2.Steam.59EDE353",
        .needs_peer = true,
        .native_update = {0x0013AE90},
        .get_friends = {0x0012E530},
        .get_error = {0x0012E550},
        .get_galaxy_instance = {0x0036B090},
        .request_accepted = {0x0012F000},
        .lobby_count = {0x00192020},
        .lobby_player_name = {0x001C36A0},
        .is_player_buddy = {0x0012E190},
        .get_player = {0x00029470},
        .map_buddy_color = {0x001534F4},
        .name_buddy_color = {0x001AB11D},
        .voice_buddy_color = {0x00251932},
        .set_special_join_pending = {0x001E7380},
        .publish_friends = {0x00195B70},
        .add_lobby = {0x001E73B0},
        .lobby_id_format = {0x003A8A74},
        .lobby_players = {0x01ACDC08},
        .accepted_ready = {0x01A55FB6},
        .requested_lobby = {0x01AE7D48},
        .special_join_target = {0x01AF254C},
        .local_display_name = std::uint32_t{0x0012ED50},
    },
    ProfileSites{
        .profile = "BF2.GOG.59EDF52B",
        .needs_peer = false,
        .native_update = {0x0013BBF0},
        .get_friends = {0x0012E530},
        .get_error = {0x0012E550},
        .get_galaxy_instance = {0x0036C090},
        .request_accepted = {0x0012E860},
        .lobby_count = {0x00192FC0},
        .lobby_player_name = {0x001C4630},
        .is_player_buddy = {0x0012E190},
        .get_player = {0x00029430},
        .map_buddy_color = {0x00154264},
        .name_buddy_color = {0x001AC0CD},
        .voice_buddy_color = {0x002529D2},
        .set_special_join_pending = {0x001E8420},
        .publish_friends = {0x00196B20},
        .add_lobby = {0x001E8450},
        .lobby_id_format = {0x003A9968},
        .lobby_players = {0x01ACF0B8},
        .accepted_ready = {0x01A5744D},
        .requested_lobby = {0x01AE91F8},
        .special_join_target = {0x01AF3A00},
        .local_display_name = std::nullopt,
    },
};

// Builds the native locations and checks the raw player stride, shared format and Steam context path.
Layout make_layout(const ProfileSites &sites) {
    const auto buddy_call = [&sites](std::uint32_t rva) {
        const auto displacement = sites.is_player_buddy - (rva + 5);
        return fc::exact_bytes({0xE8, static_cast<std::uint8_t>(displacement), static_cast<std::uint8_t>(displacement >> 8),
                                static_cast<std::uint8_t>(displacement >> 16), static_cast<std::uint8_t>(displacement >> 24)});
    };
    return {
        .needs_peer = sites.needs_peer,
        .native_update = {.rva = sites.native_update, .name = "game state update"},
        .get_friends = {.rva = sites.get_friends, .name = "Galaxy friends interface getter"},
        .get_error = {.rva = sites.get_error, .name = "last Galaxy API error getter"},
        // This IAT entry is the same GalaxyFactory::GetInstance call used by
        // the game's self-registering friend-list listener.
        .get_galaxy_instance = {.rva = sites.get_galaxy_instance, .name = "GalaxyFactory::GetInstance"},
        .request_accepted = {.rva = sites.request_accepted, .name = "gf::friends::start"},
        // These accepted-cache wrappers retain the listed spacing after gf::friends::start.
        // GameBridge calls IFriends directly for invitations, presence and relationship changes.
        .accepted_count = {.rva = (sites.request_accepted + 0x60), .name = "gf::friends::friendCount"},
        .accepted_by_index = {.rva = (sites.request_accepted + 0x90), .name = "gf::friends::friendByIndex"},
        .accepted_name = {.rva = (sites.request_accepted + 0xE0), .name = "gf::friends::friendNameByIndex"},
        .is_accepted = {.rva = (sites.request_accepted + 0x140), .name = "gf::friends::isFriend"},
        .is_joinable = {.rva = (sites.request_accepted + 0x1A0), .name = "gf::friends::isJoinableByIndex"},
        .buddy_connection = {.rva = sites.request_accepted + 0x210, .name = "gf::friends::connectionStringByIndex"},
        .lobby_count = {.rva = sites.lobby_count, .name = "active player count"},
        // The ordinal-to-slot helper follows the player-count helper by 0x50 bytes.
        .lobby_index = {.rva = sites.lobby_count + 0x50, .name = "active ordinal to native player slot"},
        .lobby_player_name =
            {
                .rva = sites.lobby_player_name,
                .name = "NetGame player name",
                // IMUL by 0x208 matches the stride of the directly accessed lobby_players view.
                .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x69, 0x45, 0x08, 0x08, 0x02, 0x00, 0x00}),
            },
        .lobby_display_name = {.rva = sites.lobby_count + 0xB0, .name = "native lobby player display name"},
        .is_player_buddy = {.rva = sites.is_player_buddy, .name = "FriendBuddyList::IsFriendBuddy"},
        .get_player = {.rva = sites.get_player,
                       .name = "resolve live player slot",
                       .evidence = fc::masked_bytes({0x8B, 0x15, 0, 0, 0, 0, 0x85, 0xD2, 0x74, 0x1C},
                                                    {0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF})},
        // Relative CALL bytes are unchanged when the executable is relocated.
        .map_buddy_color = {.rva = sites.map_buddy_color, .name = "map buddy color call", .evidence = buddy_call(sites.map_buddy_color)},
        .name_buddy_color = {.rva = sites.name_buddy_color,
                             .name = "name buddy color call",
                             .evidence = buddy_call(sites.name_buddy_color)},
        .voice_buddy_color = {.rva = sites.voice_buddy_color,
                              .name = "voice buddy color call",
                              .evidence = buddy_call(sites.voice_buddy_color)},
        // These cdecl setters write Steam 01EAD929 / GOG 01EAEDDC; the target getters copy two dwords.
        .set_special_join_pending = {.rva = sites.set_special_join_pending, .name = "set native special join pending"},
        .publish_friends = {.rva = sites.publish_friends, .name = "publish native friends table"},
        .add_lobby = {.rva = sites.add_lobby, .name = "add lobby to search results"},
        .lobby_id_format =
            {
                .rva = sites.lobby_id_format,
                .name = "shared Galaxy ID decimal format",
                .evidence = fc::exact_bytes({0x25, 0x75, 0x6C, 0x6C, 0x00}),
            },
        .lobby_players = {.rva = sites.lobby_players, .name = "NetGame player Galaxy IDs"},
        .accepted_ready = {.rva = sites.accepted_ready, .name = "game accepted buddy list ready"},
        .requested_lobby = {.rva = sites.requested_lobby, .name = "requested Galaxy lobby"},
        .special_join_target = {.rva = sites.special_join_target, .name = "native special join lobby target"},
        // This SteamAuthProvider member obtains the Steam context from globals and never reads ECX
        // in the supported body, allowing the no-argument call used to copy the platform name.
        .local_display_name = sites.local_display_name
                                  ? std::optional{fc::FunctionLocation<LocalDisplayName>{
                                        .rva = *sites.local_display_name,
                                        .name = "gf::SteamAuthProvider::GetPersonaName",
                                        .evidence = fc::masked_bytes({0x56, 0x8B, 0x35, 0, 0, 0, 0, 0x68, 0, 0, 0, 0, 0xFF, 0xD6},
                                                                     {0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF}),
                                    }}
                                  : std::nullopt,
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

} // namespace fc_plugins::galaxy_buddies
