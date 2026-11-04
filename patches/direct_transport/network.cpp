#include "network.hpp"

#include <array>

namespace fc_plugins::direct_transport {
namespace {

// Stores separate RVAs for the equivalent networking functions in each retail build.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t final_send;
    std::uint32_t group_send;
    std::uint32_t receive_service;
    std::uint32_t packet_intake;
};

constexpr std::array kProfiles{
    ProfileSites{
        .profile = "BF2.Steam.59EDE353",
        .final_send = {0x00217E50},
        .group_send = {0x001B2900},
        .receive_service = {0x001B31C0},
        .packet_intake = {0x001B3290},
    },
    ProfileSites{
        .profile = "BF2.GOG.59EDF52B",
        .final_send = {0x00218EC0},
        .group_send = {0x001B38A0},
        .receive_service = {0x001B4170},
        .packet_intake = {0x001B4240},
    },
};

// Associates each retail entry with its x86 ABI.
NetworkLayout make_layout(const ProfileSites &sites) {
    return {
        .final_send = {.rva = sites.final_send, .name = "NetSocket::SendSocket"},
        .group_send = {.rva = sites.group_send, .name = "NetComm::Send(int, PktType, NetPktGroup*)"},
        .receive_service = {.rva = sites.receive_service, .name = "DoRecvs"},
        .packet_intake = {.rva = sites.packet_intake, .name = "NetCommP::RecvPacket"},
    };
}

} // namespace

std::optional<NetworkLayout> select_network_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (sites.profile == profile) {
            return make_layout(sites);
        }
    }

    return std::nullopt;
}

} // namespace fc_plugins::direct_transport
