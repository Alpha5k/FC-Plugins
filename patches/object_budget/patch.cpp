#include "patch.hpp"

#include <fc/native.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::object_budget {
namespace {

// Locates the event and object construction budget operands.
struct Layout {
    fc::CodeLocation event_budget;
    fc::CodeLocation object_budget;
    std::uint32_t stock_budget;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile) {
    if (profile != "BF2.GOG.59EDF52B") {
        return std::nullopt;
    }

    return Layout{
        .event_budget = {.rva = 0x001BFE4A, .name = "event update budget", .evidence = fc::exact_bytes({0x8B, 0x0D})},
        .object_budget = {.rva = 0x001CE71C,
                          .name = "object update budget",
                          .evidence = fc::masked_bytes({0x69, 0x05, 0, 0, 0, 0, 0x20, 0x03, 0x00, 0x00},
                                                       {0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF})},
        .stock_budget = 0x003E9268,
    };
}

struct Handler {
    // Raises ordinary object and ordnance-event construction thresholds using FC-owned storage.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        const auto update_size = plan.allocate(std::array{std::int32_t{2048}});

        // Both instructions read the same budget. Check their targets at the executable's actual load address.
        const auto budget = fc::points_to(plan.at(layout->stock_budget, "stock update budget"));
        plan.check(plan.at(layout->event_budget.rva + 2, "event budget reference"), 4, budget);
        plan.check(plan.at(layout->object_budget.rva + 2, "object budget reference"), 4, budget);

        // Thresholds use bytes already written to the packet group: 2040 for ranked ordnance events and 1600
        // for regular object state. Native fragments still hold 998 stream bytes plus their 14-byte header.
        plan.operand(layout->event_budget, 1, update_size.address());
        plan.operand(layout->object_budget, 1, update_size.address());
    }
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "ObjectBudget",
                     .description = "Give ordinary server updates more room for object state and nearby ordnance events.",
                     .category = "Networking",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.GOG.59EDF52B"}, fc::Role::Server)}};
}

} // namespace fc_plugins::object_budget
