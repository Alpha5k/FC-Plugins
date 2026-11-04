#include "patch.hpp"

#include <fc/native.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::saber_sound_fix {
namespace {

struct Layout {
    fc::CodeLocation ignition_stop;
    fc::CodeLocation shutdown_stop;
};

struct ProfileSites {
    std::string_view profile;
    std::uint32_t ignition_stop;
    std::uint32_t shutdown_stop;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353", .ignition_stop = 0x0028914F, .shutdown_stop = 0x0028919C},
    ProfileSites{.profile = "BF2.GOG.59EDF52B", .ignition_stop = 0x0028A1DF, .shutdown_stop = 0x0028A22C},
};

// These PUSH true instructions precede Stop on WeaponMelee's transient controller at +1B0.
Layout make_layout(const ProfileSites &sites) {
    return {
        .ignition_stop = {.rva = sites.ignition_stop, .name = "saber ignition replacement stop", .evidence = fc::exact_bytes({0x6A, 0x01})},
        .shutdown_stop = {.rva = sites.shutdown_stop, .name = "saber shutdown replacement stop", .evidence = fc::exact_bytes({0x6A, 0x01})},
    };
}

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (sites.profile == profile) {
            return make_layout(sites);
        }
    }
    return std::nullopt;
}

class Handler final {
  public:
    // Releases each replaced transient to the native mixer; the separate looping hum keeps its stop behavior.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        plan.write(layout->ignition_stop, fc::byte_array<0x6A, 0x00>());
        plan.write(layout->shutdown_stop, fc::byte_array<0x6A, 0x00>());
    }
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "SaberSoundFix",
                     .description = "Prevents lightsaber ignition and shutdown sounds from being cut off.",
                     .category = "Multiplayer",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client)}};
}

} // namespace fc_plugins::saber_sound_fix
