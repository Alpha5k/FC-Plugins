#include "patch.hpp"

#include <fc/native.hpp>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::server_loop_rate {
namespace {

// Locates the PUSH imm8 divisor in GameState::UpdateTime's performance-counter pacing path.
struct Layout {
    fc::CodeLocation divisor;
};

struct ProfileSites {
    std::string_view profile;
    std::uint32_t divisor;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.GOG.59EDF52B", .divisor = 0x0013B8DB},
    ProfileSites{.profile = "BF2.Steam.59EDE353", .divisor = 0x0013AB7B},
};

// Both retail builds divide the performance-counter frequency by this positive, sign-extended imm8.
Layout make_layout(const ProfileSites &sites) {
    return {.divisor = {.rva = sites.divisor, .name = "Server loop rate divisor", .evidence = fc::exact_bytes({0x6A, 0x50})}};
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
    // Changes the outer loop's pacing divisor from 80 to 120 iterations per second.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        // UpdateTime waits for frequency / divisor ticks; the simulation turn rate is configured separately.
        plan.operand(layout->divisor, 0, fc::immediate(std::uint8_t{120}));
    }
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "ServerLoopRate",
                     .description = "Raises the server outer loop cap from 80 to 120 iterations per second.",
                     .category = "Networking",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.GOG.59EDF52B", "BF2.Steam.59EDE353"}, fc::Role::Server)}};
}

} // namespace fc_plugins::server_loop_rate
