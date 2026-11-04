#include "patch.hpp"

#include <fc/native.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::distance_lag_fix {
namespace {

// Locates the native nearest-player move limit operand.
struct Layout {
    fc::CodeLocation move_limit;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile) {
    if (profile != "BF2.GOG.59EDF52B") {
        return std::nullopt;
    }

    return Layout{
        .move_limit = {.rva = 0x001D38B5,
                       .name = "nearest player move limit",
                       .evidence = fc::exact_bytes({0xC7, 0x45, 0xE4, 0x05, 0x00, 0x00, 0x00})},
    };
}

struct Handler {
    // Raises WritePlayerMoves' cap on the number of nearest players selected.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        // WritePlayerMoves ranks other eligible players by distance and relays their latest inputs for prediction.
        // Its receiver reads to a player-index sentinel, so raising this count keeps the native packet format.
        plan.operand(layout->move_limit, 1, fc::immediate(std::uint32_t{32}));
    }
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "DistanceLagFix",
                     .description = "Increases the default player move limit from 5 to 32.",
                     .category = "Networking",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.GOG.59EDF52B"}, fc::Role::Server)}};
}

} // namespace fc_plugins::distance_lag_fix
