#include "patch.hpp"

#include <fc/native.hpp>

#include <atomic>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::map_hang_fix {
namespace {

// Counts predicate calls while a winning team exists, including calls that would already succeed.
constexpr std::uint32_t kReadinessCheckLimit = 100;

// Pairs the winning-team mask with the end-of-round network completion check.
struct Layout {
    fc::DataLocation<const std::uint32_t> winning_teams;
    fc::CodeLocation completion_check;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile) {
    if (profile != "BF2.GOG.59EDF52B") {
        return std::nullopt;
    }

    return Layout{
        .winning_teams = {.rva = 0x01AB1054, .name = "Team::sWinningTeams"},
        .completion_check = {.rva = 0x001B607D,
                             .name = "NetGame::IsGameOver network completion check",
                             .evidence = fc::exact_bytes({0x85, 0xC0, 0x75, 0x07, 0xB0, 0x01, 0xE9, 0xC9, 0x00, 0x00, 0x00})},
    };
}

class Handler {
  public:
    // Reads the winner mask and intercepts NetGame::IsGameOver's end-of-round network check.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        winning_teams_ = plan.access(layout->winning_teams);

        plan.hook_instruction(layout->completion_check, [this](fc::CpuContext &context) noexcept {
            apply_timeout(context);
        });
    }

    // Reports pending completion overrides before resetting the call count when the winner mask clears.
    void update(const fc::UpdateContext &context) noexcept {
        if (warning_pending_.exchange(false, std::memory_order_acq_rel)) {
            context.log().warning("Bypassed end-of-round network completion after {} calls with a winning team", kReadinessCheckLimit);
        }

        if (winning_teams_.read().value_or(0) == 0) {
            readiness_checks_.store(0, std::memory_order_release);
        }
    }

  private:
    // Counts calls with a winner; zero EAX selects NetGame::IsGameOver's no-network path, which returns true.
    void apply_timeout(fc::CpuContext &context) noexcept {
        if (winning_teams_.read().value_or(0) == 0) {
            readiness_checks_.store(0, std::memory_order_release);
            return;
        }

        const auto checks = readiness_checks_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (checks == kReadinessCheckLimit) {
            warning_pending_.store(true, std::memory_order_release);
        }

        if (checks >= kReadinessCheckLimit) {
            // EAX holds netEnabled at this point. Override only the saved register to bypass connected-player
            // and final-data acknowledgement waits; the game's networking flag stays unchanged.
            context.eax = 0;
        }
    }

    fc::Memory<const std::uint32_t> winning_teams_;

    // Native callbacks increment this count while FC's control-thread updates can reset it.
    std::atomic_uint32_t readiness_checks_{};
    // The native callback defers logging to the next control-thread update.
    std::atomic_bool warning_pending_{};
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "MapHangFix",
                     .description = "Prevents the server from getting stuck in-between maps waiting for client packets.",
                     .category = "Server",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.GOG.59EDF52B"}, fc::Role::Server)}};
}

} // namespace fc_plugins::map_hang_fix
