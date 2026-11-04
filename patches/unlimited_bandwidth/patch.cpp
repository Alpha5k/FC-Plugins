#include "patch.hpp"

#include <fc/native.hpp>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::unlimited_bandwidth {
namespace {

// Identifies NetComm::IsPipeFull's entry in an exact image profile.
struct Layout {
    fc::CodeLocation bandwidth_limit;
};

// RVA of NetComm::IsPipeFull in each supported image.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t bandwidth_limit;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353", .bandwidth_limit = 0x001B3490},
    ProfileSites{.profile = "BF2.GOG.59EDF52B", .bandwidth_limit = 0x001B4440},
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (sites.profile == profile) {
            // The opcode, ModRM and SIB select a nine-byte MOVSS; its absolute load address is bypassed.
            return Layout{.bandwidth_limit = {.rva = sites.bandwidth_limit,
                                              .name = "software bandwidth limit",
                                              .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x14, 0x8D})}};
        }
    }

    return std::nullopt;
}

// Bypasses the software budget check in the host's native update sender.
struct Handler {
    // Plans a false result across the complete nine-byte entry instruction.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        // SendToClients still tests its connection, turn, ticket and send-window conditions.
        // Pad xor eax,eax; ret to replace the whole MOVSS instruction.
        plan.write(layout->bandwidth_limit, fc::byte_array<0x33, 0xC0, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90>());
    }
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "UnlimitedBandwidth",
                     .description = "Disable the legacy multiplayer bandwidth limit in both client and server.",
                     .category = "Networking",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::All)}};
}

} // namespace fc_plugins::unlimited_bandwidth
