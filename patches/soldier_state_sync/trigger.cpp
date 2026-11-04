#include "corrections.hpp"

#include "timer.hpp"

#include <fc/native.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::soldier_state_sync {
namespace {

struct ProfileSites {
    std::string_view profile;
    std::uint32_t timer_mask;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353", .timer_mask = 0x001F615F},
    ProfileSites{.profile = "BF2.GOG.59EDF52B", .timer_mask = 0x001F71FF},
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<fc::CodeLocation> select_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (sites.profile == profile) {
            // Trigger::Read's nonzero-coarse branch uses this AND before ADD EAX,00800000.
            return fc::CodeLocation{.rva = sites.timer_mask,
                                    .name = "Trigger::Read received timer mask",
                                    .evidence = fc::exact_bytes({0x25, 0x00, 0xFF, 0xFF, 0xFF})};
        }
    }
    return std::nullopt;
}

} // namespace

void plan_trigger(fc::Plan &plan) {
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    // The following XOR/AND/XOR merge retains state and reserved bits from ECX.
    plan.write(*layout, fc::byte_array<0x25, (kReceivedCoarseTimerMask & 0xFF), ((kReceivedCoarseTimerMask >> 8) & 0xFF),
                                       ((kReceivedCoarseTimerMask >> 16) & 0xFF), (kReceivedCoarseTimerMask >> 24)>());
}

} // namespace fc_plugins::soldier_state_sync
