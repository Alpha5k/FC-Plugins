#include "frame_layout.hpp"

#include <array>
#include <cstdint>

namespace fc_plugins::network_diagnostics {
namespace {

struct Sites {
    std::string_view profile;
    std::uint32_t frame;
    std::array<std::uint8_t, 5> frame_bytes;
    std::uint32_t background_wait;
    std::uint32_t pacing;
    std::uint32_t game;
    std::array<std::uint8_t, 5> game_end_bytes;
    std::uint32_t pacing_target;
    std::uint32_t network_flush;
    std::array<std::uint8_t, 5> flush_bytes;
};

constexpr std::array kProfiles{
    Sites{.profile = "BF2.Steam.59EDE353",
          .frame = 0x217A97,
          .frame_bytes = {0xE8, 0xF4, 0x33, 0xF2, 0xFF},
          .background_wait = 0x217A8A,
          .pacing = 0x13AEC3,
          .game = 0x13AEE9,
          .game_end_bytes = {0xE8, 0x0A, 0x04, 0x1F, 0x00},
          .pacing_target = 0x13AB8E,
          .network_flush = 0x13AB58,
          .flush_bytes = {0xE8, 0xB3, 0x7B, 0x09, 0x00}},
    Sites{.profile = "BF2.GOG.59EDF52B",
          .frame = 0x218B07,
          .frame_bytes = {0xE8, 0xE4, 0x30, 0xF2, 0xFF},
          .background_wait = 0x218AFA,
          .pacing = 0x13BC23,
          .game = 0x13BC49,
          .game_end_bytes = {0xE8, 0x7A, 0x07, 0x1F, 0x00},
          .pacing_target = 0x13B8EE,
          .network_flush = 0x13B8B8,
          .flush_bytes = {0xE8, 0xD3, 0x7D, 0x09, 0x00}},
};

// Each observation covers complete instructions at the selected native boundary.
FrameLayout make_layout(const Sites &sites) {
    return {
        .frame = {.rva = sites.frame, .name = "MainFrame call", .evidence = fc::exact_bytes(sites.frame_bytes)},
        .background_wait = {.rva = sites.background_wait,
                            .name = "Inactive window sleep decision",
                            .evidence = fc::exact_bytes({0x85, 0xC0, 0x0F, 0x95, 0xC0})},
        .pacing = {.rva = sites.pacing, .name = "UpdateTime call", .evidence = fc::exact_bytes({0xE8, 0x58, 0xFC, 0xFF, 0xFF})},
        // These sites bracket the indirect game callback without interpreting its changing vtable target.
        .game = {.rva = sites.game, .name = "Game frame argument store", .evidence = fc::exact_bytes({0xF3, 0x0F, 0x11, 0x04, 0x24})},
        .game_end = {.rva = sites.game + 8, .name = "Game frame callback return", .evidence = fc::exact_bytes(sites.game_end_bytes)},
        .pacing_target = {.rva = sites.pacing_target,
                          .name = "Frame interval division result",
                          .evidence = fc::exact_bytes({0x89, 0x45, 0xFC, 0x89, 0x55, 0xF4})},
        .network_flush = {.rva = sites.network_flush,
                          .name = "Network flush during frame pacing",
                          .evidence = fc::exact_bytes(sites.flush_bytes)},
    };
}

} // namespace

std::optional<FrameLayout> select_frame_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (sites.profile == profile) {
            return make_layout(sites);
        }
    }
    return std::nullopt;
}

} // namespace fc_plugins::network_diagnostics
