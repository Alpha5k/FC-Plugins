#include "layout.hpp"

#include <array>
#include <cstdint>

namespace fc_plugins::input_sync {
namespace {

struct ProfileSites {
    std::string_view profile;
    std::uint32_t retire_received;
    std::uint32_t input_selection;
    std::uint32_t full_input;
    std::uint32_t completed_input;
    std::uint32_t prediction_input;
    std::uint32_t create;
    std::uint32_t input_rows;
    std::uint32_t current_turn;
    std::uint32_t authority_turn;
    std::uint32_t prediction_turn;
    std::uint32_t turns_per_second;
    std::uint32_t players;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353",
                 .retire_received = 0x001BA677,
                 .input_selection = 0x001B8AD2,
                 .full_input = 0x001B8BA4,
                 .completed_input = 0x001B999C,
                 .prediction_input = 0x001BE418,
                 .create = 0x001B2430,
                 .input_rows = 0x01AE4B40,
                 .current_turn = 0x01A64BA4,
                 .authority_turn = 0x01A64DE4,
                 .prediction_turn = 0x01BA87B4,
                 .turns_per_second = 0x01A64B54,
                 .players = 0x01BA3D58},
    ProfileSites{.profile = "BF2.GOG.59EDF52B",
                 .retire_received = 0x001BB627,
                 .input_selection = 0x001B9A82,
                 .full_input = 0x001B9B54,
                 .completed_input = 0x001BA94C,
                 .prediction_input = 0x001BF3A8,
                 .create = 0x001B33D0,
                 .input_rows = 0x01AE5FF0,
                 .current_turn = 0x01A66054,
                 .authority_turn = 0x01A66294,
                 .prediction_turn = 0x01BA9C68,
                 .turns_per_second = 0x01A66048,
                 .players = 0x01BA5208},
};

// SubmitMove has completed both slots before loading the turn to increment; PredictTurn has chosen the local slot in EBP-8.
Layout make_layout(const ProfileSites &sites) {
    return {
        .retire_received = {.rva = sites.retire_received,
                            .name = "server received input retirement",
                            .evidence = fc::exact_bytes({0x76, 0x4F})},
        // SendClient reaches this choice for each new input; the destination sets smallGroup before native construction.
        .input_selection = {.rva = sites.input_selection,
                            .name = "new input packet selection",
                            .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x4D, 0xE0})},
        .full_input = {.rva = sites.full_input,
                       .name = "complete input packet construction",
                       .evidence = fc::exact_bytes({0xC6, 0x45, 0xB4, 0x01})},
        .completed_input = {.rva = sites.completed_input, .name = "completed local input turn", .evidence = fc::exact_bytes({0xA1})},
        .prediction_input = {.rva = sites.prediction_input,
                             .name = "local prediction input read",
                             .evidence = fc::exact_bytes({0x8B, 0x4D, 0xF0, 0x8B, 0x10})},
        .create = {.rva = sites.create, .name = "NetworkCreate", .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC})},
        .input_rows = {.rva = sites.input_rows, .name = "processed local inputs"},
        .current_turn = {.rva = sites.current_turn, .name = "current local input turn"},
        .authority_turn = {.rva = sites.authority_turn, .name = "accepted authority turn"},
        .prediction_turn = {.rva = sites.prediction_turn, .name = "prediction turn"},
        .turns_per_second = {.rva = sites.turns_per_second, .name = "netTurnsPerSecond"},
        .players = {.rva = sites.players, .name = "local player IDs"},
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

} // namespace fc_plugins::input_sync
