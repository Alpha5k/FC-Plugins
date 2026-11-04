#include "layout.hpp"

#include <array>
#include <string_view>

namespace fc_plugins::late_update_recovery {
namespace {

// Absolute operands are relocated when the executable is mapped; the surrounding instructions remain exact.
constexpr std::array<std::uint8_t, 22> kPipeStateMask = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF,
};
constexpr std::array<std::uint8_t, 12> kLastReceiveMask = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF,
};
constexpr std::array<std::uint8_t, 41> kTimerRefreshMask = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0,    0,    0,    0,    0xFF, 0xFF, 0xFF, 0xFF, 0,    0,    0,    0,
};

// RVAs and expected bytes for ReceiveClient, update parsing and host activity timers.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t receive_client;
    std::uint32_t construct_group;
    std::uint32_t receive_update;
    std::uint32_t get_last_receive;
    std::uint32_t receive_group;
    std::uint32_t get_update_turn;
    std::uint32_t host_timeout_extension;
    std::uint32_t host_timeout_deadline;

    // Expected instruction bytes, including absolute operands ignored by the masks.
    std::array<std::uint8_t, 22> pipe_state_capture_bytes;
    std::array<std::uint8_t, 12> get_last_receive_bytes;
    std::array<std::uint8_t, 41> timer_refresh_bytes;
};

constexpr std::array kProfiles{
    ProfileSites{
        .profile = "BF2.Steam.59EDE353",
        .receive_client = {0x001B8F70},
        .construct_group = {0x001AECA0},
        .receive_update = {0x001BB530},
        .get_last_receive = {0x001B2C90},
        .receive_group = {0x001B2A60},
        .get_update_turn = {0x001BB570},
        .host_timeout_extension = {0x003B221C},
        .host_timeout_deadline = {0x01BA87A0},
        .pipe_state_capture_bytes = {0x33, 0xC9, 0xE8, 0xE0, 0x96, 0xFF, 0xFF, 0x69, 0xD0, 0x08, 0x02,
                                     0x00, 0x00, 0x8B, 0x82, 0xCC, 0xDB, 0xEC, 0x01, 0x89, 0x45, 0xEC},
        .get_last_receive_bytes = {0x8D, 0x04, 0x49, 0xC1, 0xE0, 0x05, 0x05, 0xFC, 0xC0, 0xEC, 0x01, 0xC3},
        .timer_refresh_bytes = {0x8B, 0x4D, 0xEC, 0x83, 0xC1, 0x40, 0xE8, 0x64, 0x9B, 0xFF, 0xFF, 0xF3, 0x0F, 0x10,
                                0x45, 0xF0, 0xF3, 0x0F, 0x11, 0x00, 0xF3, 0x0F, 0x10, 0x45, 0xF0, 0xF3, 0x0F, 0x58,
                                0x05, 0x1C, 0x22, 0x7B, 0x00, 0xF3, 0x0F, 0x11, 0x05, 0xA0, 0x87, 0xFA, 0x01},
    },
    ProfileSites{
        .profile = "BF2.GOG.59EDF52B",
        .receive_client = {0x001B9F20},
        .construct_group = {0x001AFC50},
        .receive_update = {0x001BC4E0},
        .get_last_receive = {0x001B3C30},
        .receive_group = {0x001B3A00},
        .get_update_turn = {0x001BC520},
        .host_timeout_extension = {0x003B3194},
        .host_timeout_deadline = {0x01BA9C54},
        .pipe_state_capture_bytes = {0x33, 0xC9, 0xE8, 0xD0, 0x96, 0xFF, 0xFF, 0x69, 0xD0, 0x08, 0x02,
                                     0x00, 0x00, 0x8B, 0x82, 0x7C, 0xF0, 0xEC, 0x01, 0x89, 0x45, 0xEC},
        .get_last_receive_bytes = {0x8D, 0x04, 0x49, 0xC1, 0xE0, 0x05, 0x05, 0xAC, 0xD5, 0xEC, 0x01, 0xC3},
        .timer_refresh_bytes = {0x8B, 0x4D, 0xEC, 0x83, 0xC1, 0x40, 0xE8, 0x54, 0x9B, 0xFF, 0xFF, 0xF3, 0x0F, 0x10,
                                0x45, 0xF0, 0xF3, 0x0F, 0x11, 0x00, 0xF3, 0x0F, 0x10, 0x45, 0xF0, 0xF3, 0x0F, 0x58,
                                0x05, 0x94, 0x31, 0x7B, 0x00, 0xF3, 0x0F, 0x11, 0x05, 0x54, 0x9C, 0xFA, 0x01},
    },
};

// Describes receive instructions and host activity timers with their evidence for the selected build.
Layout make_layout(const ProfileSites &sites) {
    Layout layout{
        .pipe_state_capture =
            {
                .rva = (sites.receive_client + 0x89),
                .name = "client receive pipe index",
                .evidence = fc::masked_bytes(sites.pipe_state_capture_bytes, kPipeStateMask),
            },
        .timestamp_capture =
            {
                .rva = (sites.receive_client + 0xB6),
                .name = "receive timestamp",
                .evidence = fc::exact_bytes({0xF3, 0x0F, 0x11, 0x45, 0xF0}),
            },
        .stock_drain =
            {
                .rva = (sites.receive_client + 0xBB),
                .name = "discard older queued updates",
                .evidence = fc::exact_bytes({0xBA, 0x01, 0x00, 0x00, 0x00}),
            },
        .receive_resume =
            {
                .rva = (sites.receive_client + 0x1DF),
                .name = "receive path after updates",
                .evidence = fc::exact_bytes({0xC6, 0x45, 0xFE, 0x37}),
            },
        .construct_group =
            {
                .rva = sites.construct_group,
                .name = "NetPktGroup::NetPktGroup",
                .evidence = fc::exact_bytes({0xC6, 0x01, 0x00}),
            },
        .receive_update = {.rva = sites.receive_update, .name = "ReceiveUpdate"},
        .get_last_receive =
            {
                .rva = sites.get_last_receive,
                .name = "NetCommP::GetLastRecv",
                .evidence = fc::masked_bytes(sites.get_last_receive_bytes, kLastReceiveMask),
            },
        .receive_group = {.rva = sites.receive_group, .name = "NetComm::Recv"},
        .get_update_turn = {.rva = sites.get_update_turn, .name = "GetUpdateTurn"},
        .timer_refresh =
            {
                .rva = (sites.receive_client + 0x1B1),
                .name = "host activity timer refresh",
                .evidence = fc::masked_bytes(sites.timer_refresh_bytes, kTimerRefreshMask),
            },
        .host_timeout_extension = {.rva = sites.host_timeout_extension, .name = "host cycle deadline extension"},
        .host_timeout_deadline = {.rva = sites.host_timeout_deadline, .name = "hostCycleTime"},
    };

    return layout;
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

} // namespace fc_plugins::late_update_recovery
