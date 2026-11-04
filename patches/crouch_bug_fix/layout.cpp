#include "layout.hpp"

#include <array>
#include <string>

namespace fc_plugins::crouch_bug_fix {
namespace {

// RVAs of the crouch trigger, player update and input lookup.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t trigger_update;
    std::uint32_t get_joystick_index;
    std::uint32_t player_update;
};

constexpr std::array kProfiles{
    ProfileSites{
        .profile = "BF2.Steam.59EDE353", .trigger_update = 0x0003A950, .get_joystick_index = 0x001B73C0, .player_update = 0x0021A2B0},
    ProfileSites{
        .profile = "BF2.GOG.59EDF52B", .trigger_update = 0x0003A940, .get_joystick_index = 0x001B8370, .player_update = 0x0021B320},
};

// Describes native entries, crouch arguments and the expected crouch CALL destination.
Layout make_layout(const ProfileSites &sites) {
    return {
        .get_joystick_index = {.rva = sites.get_joystick_index, .name = "NetGame::GetJoystickIndex"},
        // PlayerController::Update passes Controllable + 0x48 and bit four of its processed inputs here.
        .crouch_arguments = {.rva = sites.player_update + 0x978,
                             .name = "crouch input arguments",
                             .evidence = fc::exact_bytes({0x8B, 0x4F, 0x04, 0xF3, 0x0F, 0x10, 0x4D, 0x08, 0x83, 0xC1, 0x48, 0x8B,
                                                          0x40, 0x10, 0xC1, 0xE8, 0x04, 0x24, 0x01, 0x0F, 0xB6, 0xC0, 0x50})},
        .crouch_update = {.rva = sites.player_update + 0x98F,
                          .name = "player crouch update",
                          .evidence = fc::direct_call_to(fc::Location{.image = std::string{sites.profile}, .rva = sites.trigger_update})},
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

} // namespace fc_plugins::crouch_bug_fix
