#include "handler.hpp"

#include "frame_layout.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <format>

namespace fc_plugins::network_diagnostics {
namespace {

// The native message pump and MainFrame run serially on the main thread.
struct FrameState {
    trace::FrameTimingRecord record{};
    std::uint64_t background_begin{};
    std::uint64_t pacing_begin{};
    std::uint64_t game_begin{};
    std::uint64_t network_flush_begin{};
};

thread_local FrameState frame_state;

// Uses the same clock as the native frame interval and transport arrival records.
std::uint64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

} // namespace

void Handler::plan_frame_timing(fc::Plan &plan) {
    const auto layout = select_frame_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    // Borrow the frame call's address; reserving the preceding Sleep operand would conflict with its modifier.
    const auto frame_call = plan.access<const std::byte>(layout->frame.rva);

    // Inactive windows sleep only when PeekMessage found no message. The frame call ends that interval.
    plan.observe_instruction(layout->background_wait, fc::before([frame_call](const fc::CpuContext &context) noexcept {
                                 if (context.eax == 0) {
                                     frame_state.background_begin = qpc_now();
                                     if (const auto call = frame_call.view()) {
                                         // PUSH imm8 precedes CALL EDI and MainFrame; read its live, sign-extended argument.
                                         frame_state.record.background_sleep_ms = static_cast<std::uint32_t>(
                                             static_cast<std::int32_t>(fc::read_field<std::int8_t>(call->data(), -3)));
                                     }
                                 }
                             }));
    plan.observe_call(layout->frame, fc::before([]() noexcept {
                          auto &state = frame_state;
                          state.record.begin_qpc = qpc_now();
                          if (state.background_begin != 0) {
                              state.record.background_sleep_ticks = state.record.begin_qpc - state.background_begin;
                              state.background_begin = 0;
                          }
                      }),
                      fc::after([this](bool) noexcept {
                          auto &record = frame_state.record;
                          record.end_qpc = qpc_now();
                          write(trace::RecordKind::FrameTiming, record, host_turn_ ? static_cast<std::uint32_t>(*host_turn_) : 0);
                          record = {};
                      }));

    plan.observe_call(layout->pacing, fc::before([]() noexcept {
                          frame_state.pacing_begin = qpc_now();
                      }),
                      fc::after([]() noexcept {
                          frame_state.record.pacing_ticks = qpc_now() - frame_state.pacing_begin;
                      }));
    plan.observe_instruction(layout->game, fc::before([](const fc::CpuContext &) noexcept {
                                 frame_state.game_begin = qpc_now();
                             }));
    plan.observe_instruction(layout->game_end, fc::before([](const fc::CpuContext &) noexcept {
                                 frame_state.record.game_ticks = qpc_now() - frame_state.game_begin;
                             }));

    // The actual division result also covers an installed frame-rate patch's chosen interval.
    plan.observe_instruction(layout->pacing_target, fc::before([](const fc::CpuContext &context) noexcept {
                                 frame_state.record.pacing_target_ticks = (static_cast<std::uint64_t>(context.edx) << 32U) | context.eax;
                                 frame_state.record.target_known = 1;
                             }));

    plan.observe_call(layout->network_flush, fc::before([]() noexcept {
                          frame_state.network_flush_begin = qpc_now();
                      }),
                      fc::after([]() noexcept {
                          frame_state.record.network_flush_ticks = qpc_now() - frame_state.network_flush_begin;
                      }));
}

} // namespace fc_plugins::network_diagnostics
