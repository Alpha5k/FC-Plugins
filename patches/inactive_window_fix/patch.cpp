#include "patch.hpp"

#include <fc/native.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::inactive_window_fix {
namespace {

struct Layout {
    fc::CodeLocation sleep_argument;
};

struct ProfileSites {
    std::string_view profile;
    std::uint32_t sleep_argument;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353", .sleep_argument = 0x00217A93},
    ProfileSites{.profile = "BF2.GOG.59EDF52B", .sleep_argument = 0x00218B03},
    ProfileSites{.profile = "BF2.ModTools.43EBD102", .sleep_argument = 0x003383EE},
};

// The inactive, empty-message branch pushes ten milliseconds before calling Sleep and then MainFrame.
Layout make_layout(const ProfileSites &sites) {
    return {.sleep_argument = {
                .rva = sites.sleep_argument, .name = "Inactive window Sleep argument", .evidence = fc::exact_bytes({0x6A, 0x0A})}};
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

struct Handler {
    // Retains the native message loop and yields with Sleep(0) when the window is inactive.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        plan.operand(layout->sleep_argument, 0, fc::immediate(std::uint8_t{0}));
    }
};

} // namespace

fc::Patch definition(fc::Role role) {
    fc::Patch patch{.id = "InactiveWindowFix",
                    .description = "Removes the extra ten-millisecond wait while the window is inactive.",
                    .category = "Networking",
                    .enabled = true,
                    .configurable = true};
    if (role != fc::Role::Server) {
        patch.variants.push_back(
            fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B", "BF2.ModTools.43EBD102"}, fc::Role::Client));
    }
    if (role != fc::Role::Client) {
        patch.variants.push_back(fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Server));
    }
    return patch;
}

} // namespace fc_plugins::inactive_window_fix
