#include "layout.hpp"

namespace fc_plugins::direct_transport::server {

std::optional<ServerLayout> select_server_layout(std::string_view profile) {
    if (profile != "BF2.GOG.59EDF52B") {
        return std::nullopt;
    }

    return ServerLayout{
        .game_port = {.rva = {0x003E9EF4}, .name = "configured game port"},
        .is_playing = {.rva = {0x001C4530}, .name = "NetGame::IsPlaying"},
        .update_boot_player = {.rva = {0x001D1D90}, .name = "UpdateBootPlayer"},
        .set_not_playing = {.rva = {0x001B9440}, .name = "NetGameP::SetNotPlaying"},
        // UpdateBootPlayer reads this cdecl guest-owner accessor before starting the boot timer.
        .get_guest_owner = {.rva = {0x001C47D0},
                            .name = "NetGame::GetGuestOwner",
                            .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0x7D, 0x08, 0x00})},
        // Disconnect clears the endpoint before returning, including same-user reconnects between receives.
        .disconnect = {.rva = {0x001B3A90},
                       .name = "NetComm::Disconnect",
                       .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8})},
    };
}

} // namespace fc_plugins::direct_transport::server
