#include "patch.hpp"

#include <fc/native.hpp>
#include <array>
#include <cstdint>
#include <limits>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::spawn_delay {
namespace {

// Locates SetSpawnDelay's network delay load instruction.
struct Layout {
    fc::CodeLocation spawn_delay;
    std::uint32_t stock_delay;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile) {
    if (profile != "BF2.GOG.59EDF52B") {
        return std::nullopt;
    }

    return Layout{
        .spawn_delay = {.rva = 0x0018D605, .name = "spawn delay load", .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x0D})},
        .stock_delay = 0x003B3228,
    };
}

// Replaces SetSpawnDelay's network cycle interval for teams 1 and 2.
class Handler {
  public:
    struct Settings {
        // Captured at startup; SetSpawnDelay stores it for later team wave cycles.
        float seconds = 15.0F;
    };

    Handler(const fc::CreateContext &, const Settings &settings) : seconds_(settings.seconds) {}

    // Redirects the network branch's MOVSS operand to the configured delay in seconds.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        const auto delay = plan.allocate(std::array{seconds_});

        // Windows relocates MOVSS's address operand; it must still point to the game's default delay.
        plan.check(plan.at(layout->spawn_delay.rva + 4, "spawn delay reference"), 4,
                   fc::points_to(plan.at(layout->stock_delay, "stock spawn delay")));

        // The offline branch uses the script's cycle argument. SetSpawnDelayTeam is a separate callback
        // that still uses the stock network interval and can overwrite this value for either team.
        plan.operand(layout->spawn_delay, 1, delay.address());
    }

  private:
    float seconds_;
};

} // namespace

fc::Patch definition() {
    return fc::Patch{
        .id = "SpawnDelay",
        .description = "Sets the default spawn delay value for singleplayer and hosted games.",
        .category = "Server",
        .enabled = true,
        .settings = fc::settings<Handler::Settings>({fc::setting("Seconds", &Handler::Settings::seconds,
                                                                 "Seconds between team spawn waves; mission scripts may override this.\n"
                                                                 "0 removes the wave delay; other spawn requirements still apply.")
                                                         .environment("SPAWN_TIMER")
                                                         .range(0.0F, std::numeric_limits<float>::max())}),
        .variants = {fc::variant<Handler>({"BF2.GOG.59EDF52B"}, fc::Role::Server)}};
}

} // namespace fc_plugins::spawn_delay
