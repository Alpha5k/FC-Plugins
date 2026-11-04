#include "layout.hpp"

#include <array>
#include <string>

namespace fc_plugins::reload_duration_fix {
namespace {

// RVAs of the weapon-state reader and local input lookup in each supported image.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t weapon_read;
    std::uint32_t get_joystick_index;
    std::uint32_t melee_vtable_operand;
    std::uint32_t melee_vtable;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353",
                 .weapon_read = 0x001E84D0,
                 .get_joystick_index = 0x001B73C0,
                 .melee_vtable_operand = 0x002882AB,
                 .melee_vtable = 0x003B1578},
    ProfileSites{.profile = "BF2.GOG.59EDF52B",
                 .weapon_read = 0x001E9570,
                 .get_joystick_index = 0x001B8370,
                 .melee_vtable_operand = 0x0028932B,
                 .melee_vtable = 0x003B24F0},
};

// Describes the reader and local input lookup for the selected build.
Layout make_layout(const ProfileSites &sites) {
    return {
        .weapon_read = {sites.weapon_read, "Weapon::Read"},
        .get_joystick_index = {sites.get_joystick_index, "NetGame::GetJoystickIndex"},
        .melee_vtable = {.rva = sites.melee_vtable_operand,
                         .name = "WeaponMelee constructor vtable operand",
                         .evidence = fc::points_to(fc::Location{.image = std::string(sites.profile), .rva = sites.melee_vtable})},
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

} // namespace fc_plugins::reload_duration_fix
