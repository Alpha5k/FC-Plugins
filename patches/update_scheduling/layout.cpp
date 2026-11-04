#include "layout.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fc_plugins::update_scheduling {
namespace {

constexpr std::string_view kGogImage = "BF2.GOG.59EDF52B";

// GOG NetPlayer records store nextObjState and the RTT mean/deviation at these offsets.
constexpr std::uint32_t kPlayerStates{0x01AC'EF50};
constexpr std::int32_t kPlayerStateStride = 0x208;
constexpr std::int32_t kPendingStateOffset = 0x4;
constexpr std::int32_t kRoundTripOffset = 0x1C4;

constexpr std::uint32_t kGameLoopUpdate{0x0013'3140};
constexpr std::uint32_t kSendToClients{0x001C'9C00};
constexpr std::uint32_t kSentUpdate{0x001D'2DB0};
constexpr std::uint32_t kReceivedUpdateAck{0x001D'2EB0};
constexpr std::uint32_t kWriteObjects{0x001C'D6A0};

} // namespace

std::optional<Layout> select_layout(std::string_view profile) {
    if (profile != kGogImage) {
        return std::nullopt;
    }

    Layout layout{
        .dedicated_render =
            {
                .rva = (kGameLoopUpdate + 0x7BA),
                .name = "dedicated renderer call",
                .evidence = fc::exact_bytes({0xE8, 0x11, 0xA8, 0x18, 0x00}),
            },
        .client_visit_count =
            {
                .rva = (kSendToClients + 0x19),
                .name = "halve the send allowance per pass",
                .evidence = fc::exact_bytes({0x99, 0x2B, 0xC2, 0xD1, 0xF8}),
            },
        .acknowledgement_guard =
            {
                .rva = (kSentUpdate + 0x41),
                .name = "send timestamp for ACK storage",
                .evidence = fc::exact_bytes({0xE8, 0x4A, 0x0A, 0xFE, 0xFF}),
            },
        .acknowledgement_resume =
            {
                .rva = (kSentUpdate + 0x71),
                .name = "send interval calculation after ACK storage",
                // Resume at the absolute MOVSS load; its constant address is independent of the callback.
                .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x05}),
            },
        .untracked_acknowledgement =
            {
                .rva = (kReceivedUpdateAck + 0x74),
                .name = "ACK without a native send slot",
                .evidence = fc::exact_bytes({0xE9, 0x39, 0x01, 0x00, 0x00}),
            },
        .round_trip_update =
            {
                .rva = (kReceivedUpdateAck + 0xAD),
                .name = "native RTT update from elapsed sample",
                // The estimator reads the player at EBP-4 and the elapsed seconds at EBP-1C.
                .evidence = fc::exact_bytes({0x8B, 0x45, 0xFC, 0xF3, 0x0F, 0x10, 0x45, 0xE4}),
            },
        .create_marker =
            {
                .rva = (kWriteObjects + 0xEE2),
                .name = "WriteObjects continuation after CREATE",
                .evidence = fc::exact_bytes({0xC7, 0x85, 0x7C, 0xFF, 0xFF, 0xFF, 0x06, 0x00, 0x00, 0x00}),
            },
        .send_window_result =
            {
                .rva = (kSendToClients + 0x151),
                .name = "native send window result",
                .evidence = fc::exact_bytes({0x0F, 0xB6, 0xD0, 0x85, 0xD2}),
            },
        .send_window =
            {
                .rva = (kSendToClients + 0x156),
                .name = "client send decision",
                .evidence = fc::exact_bytes({0x75, 0x05}),
            },
        .continue_destination_checks =
            {
                .rva = (kSendToClients + 0x15D),
                .name = "continue destination checks",
                .evidence = fc::exact_bytes({0xE8, 0xBE, 0x7E, 0xF6, 0xFF}),
            },
        .next_destination =
            {
                .rva = (kSendToClients + 0x98),
                .name = "continue client scan",
                .evidence = fc::exact_bytes({0x8B, 0x55, 0xF0, 0x83, 0xC2, 0x01, 0x89, 0x55, 0xF0}),
            },
        .next_update_turn =
            {
                .rva = (kSentUpdate + 0xDF),
                .name = "next client update turn",
                .evidence = fc::exact_bytes({0x03, 0x4D, 0xE8}),
            },
        .current_destination =
            {
                .rva = {0x01BA'9C2C},
                .name = "curDst",
            },
        .host_turn =
            {
                .rva = {0x01BA'9C4C},
                .name = "current host turn",
            },
        .turn_rate =
            {
                .rva = {0x01A6'6048},
                .name = "simulation turns per second",
            },
        .network_time =
            {
                .rva = {0x01BA'5214},
                .name = "netTime",
            },
        .allocate_object_state = {.rva = {0x001B'5670}, .name = "NetObjStateMap::Alloc"},
        .free_object_state = {.rva = {0x001B'5720}, .name = "NetObjStateMap::Free"},
        .received_update_ack = {.rva = kReceivedUpdateAck, .name = "ReceivedUpdateAck"},
        .network_create =
            {
                .rva = {0x001B'33D0},
                .name = "NetComm::Create",
                .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8}),
            },
    };

    // Reserve only the pending-map pointer and RTT pair, leaving other NetPlayer fields to their owners.
    for (std::size_t player = 0; player < kMaximumPlayers; ++player) {
        const auto player_rva = (kPlayerStates + static_cast<std::int32_t>(player * kPlayerStateStride));
        layout.pending_states[player] = {
            .rva = (player_rva + kPendingStateOffset),
            .name = "pending object state map",
        };
        layout.round_trip_estimates[player] = {
            .rva = (player_rva + kRoundTripOffset),
            .name = "round-trip time estimate",
        };
    }

    return layout;
}

} // namespace fc_plugins::update_scheduling
