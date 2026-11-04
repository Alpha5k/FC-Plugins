#include "patch.hpp"

#include <fc/native.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::name_bug_fix {
namespace {

// Disconnect receives a native player index in ECX; SetNotPlaying receives it on the caller-cleaned stack.
using Disconnect = fc::NativeCall<void(std::int32_t player) noexcept, fc::abi::Ecx>;
using SetNotPlaying = void(__cdecl *)(std::int32_t player) noexcept;

// Pairs native membership cleanup with warmup and post-load disconnect calls.
struct Layout {
    fc::FunctionLocation<SetNotPlaying> set_not_playing;
    fc::CallLocation<Disconnect> warmup_departure;
    fc::CallLocation<Disconnect> post_load_timeout;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile) {
    if (profile != "BF2.GOG.59EDF52B") {
        return std::nullopt;
    }

    return Layout{
        .set_not_playing = {.rva = 0x001B9440},
        .warmup_departure = {.rva = 0x001DDFDF,
                             .name = "warmup dropped player",
                             .evidence = fc::direct_call_to(
                                 fc::Location{.image = "BF2.GOG.59EDF52B", .rva = 0x001B3A90, .name = "NetComm::Disconnect"})},
        .post_load_timeout = {.rva = 0x001E42E4,
                              .name = "post-load player timeout",
                              .evidence = fc::direct_call_to(
                                  fc::Location{.image = "BF2.GOG.59EDF52B", .rva = 0x001B3A90, .name = "NetComm::Disconnect"})},
    };
}

class Handler {
  public:
    // Replaces the warmup departure and post-load timeout calls with native membership cleanup.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        set_not_playing_ = plan.function(layout->set_not_playing);

        // These callers already clear shell names. SetNotPlaying clears pending playing membership,
        // calls Disconnect once, then clears the mapped guest's pending membership.
        const auto cleanup = [this](std::int32_t player) noexcept {
            static_cast<void>(set_not_playing_.call(player));
        };

        plan.hook_call(layout->warmup_departure, cleanup);

        plan.hook_call(layout->post_load_timeout, cleanup);
    }

  private:
    fc::NativeFunction<SetNotPlaying> set_not_playing_;
};

} // namespace

fc::Patch definition() {
    return fc::Patch{
        .id = "NameBugFix",
        .description =
            "Fixes the bug where players that leave during warmup are not properly cleaned up and another player can inherit their name.",
        .category = "Server",
        .enabled = true,
        .variants = {fc::variant<Handler>({"BF2.GOG.59EDF52B"}, fc::Role::Server)}};
}

} // namespace fc_plugins::name_bug_fix
