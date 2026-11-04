#include "layout.hpp"

#include <array>

namespace fc_plugins::shot_sequence {
namespace {

struct ProfileSites {
    std::string_view profile;
    std::uint32_t incomplete_branch;
    std::uint32_t pattern_store;
    std::uint32_t get_joystick_index;
    std::uint32_t network_enabled;
    std::uint32_t local_turn;
};

// Retail Fire checks these network/world/pass bytes before publishing a projectile.
constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353",
                 .incomplete_branch = 0x0027F229,
                 .pattern_store = 0x27F3B0,
                 .get_joystick_index = 0x1B73C0,
                 .network_enabled = 0x1A62EA9,
                 .local_turn = 0x1A62F10},
    ProfileSites{.profile = "BF2.GOG.59EDF52B",
                 .incomplete_branch = 0x002802A9,
                 .pattern_store = 0x280430,
                 .get_joystick_index = 0x1B8370,
                 .network_enabled = 0x1A64359,
                 .local_turn = 0x1A643C0},
};

// Qualifies the cooldown COMISS/JC pair and the pattern store in Fire.
Layout make_layout(const ProfileSites &sites) {
    return {
        .compare = {.rva = sites.incomplete_branch - 7,
                    .name = "Cannon cooldown completion comparison",
                    .evidence = fc::exact_bytes({0x0F, 0x2F, 0x86, 0xB8, 0x00, 0x00, 0x00})},
        .incomplete_branch = {.rva = sites.incomplete_branch,
                              .name = "Cannon incomplete cooldown rejection",
                              .evidence = fc::exact_bytes({0x72, 0xD3})},
        // Fire keeps old in ECX, computes old+1 in EAX, then stores and wraps the pattern at +11C.
        .pattern_store = {.rva = sites.pattern_store,
                          .name = "Cannon pattern advance",
                          .evidence = fc::exact_bytes({0x89, 0x86, 0x1C, 0x01, 0x00, 0x00})},
        .get_joystick_index = {.rva = sites.get_joystick_index, .name = "NetGame::GetJoystickIndex"},
        .network_enabled = {.rva = sites.network_enabled, .name = "network enabled"},
        .client_world = {.rva = sites.network_enabled + 2, .name = "client object world active"},
        .local_turn = {.rva = sites.local_turn, .name = "local prediction pass"},
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

} // namespace fc_plugins::shot_sequence
