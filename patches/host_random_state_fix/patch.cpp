#include "patch.hpp"

#include <fc/native.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::host_random_state_fix {
namespace {

struct Layout {
    fc::CodeLocation restore_load;
    std::uint32_t client_random;
    std::uint32_t server_random;
};

struct ProfileSites {
    std::string_view profile;
    std::uint32_t restore_load;
    std::uint32_t client_random;
    std::uint32_t server_random;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353", .restore_load = 0x001B7594, .client_random = 0x01A64B68, .server_random = 0x01A64B64},
    ProfileSites{.profile = "BF2.GOG.59EDF52B", .restore_load = 0x001B8544, .client_random = 0x01A66010, .server_random = 0x01A6600C},
};

// LeaveClient saves the updated client seed before this MOV EAX,[absolute] restores the active seed.
Layout make_layout(const ProfileSites &sites) {
    return {
        .restore_load = {.rva = sites.restore_load, .name = "server random state restoration", .evidence = fc::exact_bytes({0xA1})},
        .client_random = sites.client_random,
        .server_random = sites.server_random,
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

struct Handler {
    // Restores the saved server seed while retaining the client's progress for its next world visit.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        // The original absolute operand is unaligned; check its relocated address separately from the opcode.
        plan.check(layout->restore_load.rva + 1, 4, fc::points_to(plan.at(layout->client_random)));
        plan.operand(layout->restore_load, 1, fc::address(plan.at(layout->server_random)));
    }
};

} // namespace

fc::Patch definition() {
    return {
        .id = "HostRandomStateFix",
        .description = "Preserves the host's random state in multiplayer.",
        .category = "Multiplayer",
        .enabled = true,
        .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client)},
    };
}

} // namespace fc_plugins::host_random_state_fix
