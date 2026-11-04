#include "patch.hpp"

#include "../network_diagnostics/api.hpp"

#include <fc/native.hpp>

#include <WinSock2.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::waitlate_grace {
namespace {

using SetWaitlate = void(__cdecl *)(bool enabled) noexcept;
using DiagnosticsApi = network_diagnostics::NetworkDiagnosticsApi;

// IsTurnReady keeps its four-byte grace local below EBP.
constexpr std::ptrdiff_t kGraceLocalOffset = -0x18;

// Groups waitlate control, the live turn rate, and the native grace calculation.
struct Layout {
    fc::FunctionLocation<SetWaitlate> set_waitlate;
    fc::CodeLocation nowaitlate_argument;
    fc::CodeLocation nowaitlate_reply;
    fc::CodeLocation startup_nowaitlate_store;
    std::uint32_t disabled_reply;
    std::uint32_t enabled_reply;
    fc::DataLocation<const std::int32_t> turn_rate;
    fc::CodeLocation grace_calculation;
    std::uint32_t waitlate;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile) {
    if (profile != "BF2.GOG.59EDF52B") {
        return std::nullopt;
    }

    return Layout{
        .set_waitlate = {.rva = 0x001BB520},
        .nowaitlate_argument = {.rva = 0x001B01DF, .name = "nowaitlate argument", .evidence = fc::exact_bytes({0x6A, 0x00})},
        // The remote branch pushes the disabled reply; the startup parser directly clears the same waitlate flag.
        .nowaitlate_reply = {.rva = 0x001B01E9,
                             .name = "nowaitlate reply",
                             .evidence = fc::masked_bytes({0x68, 0, 0, 0, 0}, {0xFF, 0, 0, 0, 0})},
        .startup_nowaitlate_store = {.rva = 0x00079C0E,
                                     .name = "startup nowaitlate store",
                                     .evidence = fc::masked_bytes({0xC6, 0x05, 0, 0, 0, 0, 0x00}, {0xFF, 0xFF, 0, 0, 0, 0, 0xFF})},
        .disabled_reply = 0x003ABC78,
        .enabled_reply = 0x003ABC9C,
        .turn_rate = {.rva = 0x01A66048, .name = "netTurnsPerSecond"},
        .grace_calculation = {.rva = 0x001BAAD4,
                              .name = "late input grace calculation",
                              .evidence = fc::masked_bytes(
                                  {0x0F, 0xB6, 0x05, 0, 0, 0, 0, 0x85, 0xC0, 0x75, 0x07, 0xC7, 0x45, 0xE8, 0x00, 0x00, 0x00, 0x00},
                                  {0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})},
        .waitlate = 0x01A64400,
    };
}

class Handler {
  public:
    // Forces native startup and remote-admin waitlate policy, aligns the reply, and replaces IsTurnReady's grace count.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        set_waitlate_ = plan.function(layout->set_waitlate);

        plan.operand(layout->nowaitlate_argument, 0, fc::immediate(std::uint8_t{1}));

        // Validate the original relocated pointer before selecting the native enabled reply.
        plan.check(plan.at(layout->nowaitlate_reply.rva + 1, "disabled waitlate reply reference"), 4,
                   fc::points_to(plan.at(layout->disabled_reply, "disabled waitlate reply")));
        plan.operand(layout->nowaitlate_reply, 0, fc::address(plan.at(layout->enabled_reply, "enabled waitlate reply")));

        // Preserve forced waitlate when command-line parsing follows activation; the store's address relocates.
        plan.check(plan.at(layout->startup_nowaitlate_store.rva + 2, "startup waitlate flag reference"), 4,
                   fc::points_to(plan.at(layout->waitlate, "waitlate flag")));
        plan.operand(layout->startup_nowaitlate_store, 1, fc::immediate(std::uint8_t{1}));

        turns_per_second_ = plan.access(layout->turn_rate);

        // The masked address must still name the waitlate flag at the executable's actual load address.
        plan.check(plan.at(layout->grace_calculation.rva + 3, "waitlate flag reference"), 4,
                   fc::points_to(plan.at(layout->waitlate, "waitlate flag")));

        plan.hook_instruction(layout->grace_calculation, [this](fc::CpuContext &context) noexcept {
            const auto grace = calculate_grace();

            // Set IsTurnReady's four-byte grace local at EBP-0x18 before its waitlate test.
            // Native code overwrites it with zero if waitlate is disabled.
            fc::write_field(context.ebp, kGraceLocalOffset, grace);

            report_grace(grace);
        });
    }

    // Publishes optional diagnostics callbacks so the native hook can report grace counts.
    fc::Result<> prepare(fc::PrepareContext &context) {
        return context.on_available("Diagnostics.NetworkDiagnostics", network_diagnostics::api_contract(),
                                    [this](const DiagnosticsApi &api) noexcept {
                                        diagnostics_ = api;
                                        diagnostics_ready_.store(true, std::memory_order_release);

                                        report_grace(calculate_grace());
                                    });
    }

    // Enables waitlate at startup and frees saved late-state groups. This policy waits before filling missing
    // input from prior turns and disables native late-input replay and missing-move object suppression.
    void activate(fc::ActivateContext &) noexcept {
        static_cast<void>(set_waitlate_.call(true));
    }

  private:
    // Uses rate / 20 with a one-turn minimum. IsTurnReady fills missing input only when the host falls
    // more than this many turns behind the input clock, so this is a turn threshold rather than a fixed delay.
    std::uint32_t calculate_grace() const noexcept {
        return static_cast<std::uint32_t>(std::max(turns_per_second_.read().value_or(0) / 20, 1));
    }

    // Reports only changed grace values and preserves ambient errors around the optional observer.
    void report_grace(std::uint32_t grace) noexcept {
        if (!diagnostics_ready_.load(std::memory_order_acquire) || reported_grace_.exchange(grace, std::memory_order_acq_rel) == grace) {
            return;
        }

        const auto winsock_error = WSAGetLastError();
        const auto windows_error = GetLastError();

        diagnostics_.waitlate_grace(grace);

        WSASetLastError(winsock_error);
        SetLastError(windows_error);
    }

    fc::NativeFunction<SetWaitlate> set_waitlate_;
    // Live signed simulation rate in turns per second.
    fc::Memory<const std::int32_t> turns_per_second_;

    // Copied provider callbacks remain valid while the diagnostics provider is installed.
    DiagnosticsApi diagnostics_{};
    // Release/acquire publication makes the complete callback table visible to the native hook.
    std::atomic_bool diagnostics_ready_{};
    // Last reported policy count; a changed native turn rate is reported on the next calculation.
    std::atomic_uint32_t reported_grace_{};
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "WaitlateGrace",
                     .description = "Enables waitlate and sets its grace from the simulation turn rate.",
                     .category = "Networking",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.GOG.59EDF52B"}, fc::Role::Server)}};
}

} // namespace fc_plugins::waitlate_grace
