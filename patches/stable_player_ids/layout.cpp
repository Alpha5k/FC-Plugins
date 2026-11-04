#include "layout.hpp"

namespace fc_plugins::stable_player_ids {

std::optional<GameLayout> select_game_layout(std::string_view profile) {
    if (profile != "BF2.Classic.66702CD2") {
        return std::nullopt;
    }

    // The three calls hash player UUIDs; the separate PASSWORD authentication caller keeps native MD5.
    const auto md5_call = fc::direct_call_to({.image = "BF2.Classic.66702CD2", .rva = 0x0024B8C0, .name = "MD5"});
    return GameLayout{
        .md5 = {.rva = 0x0024B8C0, .name = "MD5"},
        .admin_hashes = {{{.rva = 0x0025D0C5, .name = "List player keyhash", .evidence = md5_call},
                          {.rva = 0x0025D6A7, .name = "Ban player keyhash", .evidence = md5_call},
                          {.rva = 0x0025D954, .name = "Find player by keyhash", .evidence = md5_call}}},
    };
}

std::optional<BootstrapLayout> select_bootstrap_layout(std::string_view profile) {
    if (profile != "Classic.Bootstrap.66702CD7") {
        return std::nullopt;
    }

    return BootstrapLayout{
        // Room creation clears EDX before the publishUserID setter, which consumes only DL.
        .publish_user_id = {.rva = 0x0003EB7E, .name = "Publish Photon UserIDs", .evidence = fc::exact_bytes({0x33, 0xD2})},
        // Launcher main retains this RedNet object until the game loop returns.
        .rednet = {.rva = 0x000F5828, .name = "RedNet owner"},
        // Native service uses these import thunks with RedNetClient's embedded mutex.
        .lock = {.rva = 0x0007995D, .name = "_Mtx_lock", .evidence = fc::exact_bytes({0xFF, 0x25, 0x55, 0x28, 0x04, 0x00})},
        .unlock = {.rva = 0x00079963, .name = "_Mtx_unlock", .evidence = fc::exact_bytes({0xFF, 0x25, 0x47, 0x28, 0x04, 0x00})},
    };
}

} // namespace fc_plugins::stable_player_ids
