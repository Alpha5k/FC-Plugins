#pragma once

#include "schema.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

namespace fc_plugins::sound_diagnostics {

// Retains the last arming transition and completed native or continuity height writer for one registered Hero identity.
struct FootHistory {
    std::uint32_t subject{};
    std::uint32_t generation{};
    std::uint32_t soldier{};
    std::uint32_t weapon{};
    std::array<trace::FootGate, 2> arms{};
    std::uint32_t writer_scope{};
    std::array<float, 2> heights{};
    std::uint16_t flags{};
    bool writer_seen{};
    std::uint8_t writer_source{};
    std::uint32_t other_invocations{};
    std::uint32_t writer_other_invocations{};
    std::uint32_t other_scope{};
    bool maintenance_seen{};
    std::uint8_t maintenance_reason{}, maintenance_support{};
    std::uint16_t maintenance_flags{};
};

// Owns one height or distance update; pending notifications belong only to this native invocation.
struct FootInvocation {
    FootInvocation *previous{};
    FootHistory *history{};
    std::uint32_t soldier{};
    std::uint32_t pose{};
    std::array<trace::FootGate, 2> gates{};
    std::array<float, 2> old_heights{};
    std::uintptr_t pending_site{};
    std::uint32_t pending_foot{};
    bool writes_completed{};
};

// Uses the native player slot so concurrently live diagnostic IDs cannot evict each other's retained evidence.
inline FootHistory *select_foot_history(std::array<FootHistory, 64> &histories, std::int32_t player, std::uint32_t subject,
                                        std::uint32_t generation, std::uint32_t soldier, std::uint32_t weapon) noexcept {
    if (player < 0 || player >= static_cast<std::int32_t>(histories.size())) {
        return nullptr;
    }
    auto &history = histories[static_cast<std::size_t>(player)];
    if (history.subject != subject || history.generation != generation || history.soldier != soldier || history.weapon != weapon) {
        history = {.subject = subject, .generation = generation, .soldier = soldier, .weapon = weapon};
    }
    return &history;
}

// Computes the native strict COMISS gates; unordered values fail the same above comparisons.
inline std::uint8_t foot_comparisons(float old_height, float current_height, float epsilon, float threshold) noexcept {
    return static_cast<std::uint8_t>((old_height - current_height > epsilon ? 1 : 0) | (current_height - old_height > epsilon ? 2 : 0) |
                                     (old_height < threshold ? 4 : 0));
}

// Saves only an executed native 0-to-armed mutation; repeated descent with an armed bit retains the original episode.
inline void retain_foot_arm(FootHistory &history, std::uint32_t foot, trace::FootGate gate, std::uint16_t after) noexcept {
    const auto bit = static_cast<std::uint16_t>(1U << foot);
    if ((gate.flags_before & bit) == 0 && (after & bit) != 0) {
        gate.flags_after = after;
        gate.observed = 1;
        history.arms[foot] = gate;
    }
}

// Tests bitwise writer continuity, preserving signed zero and NaN payload evidence.
inline bool foot_baseline_matches(const FootHistory &history, const std::array<float, 2> &heights) noexcept {
    return history.writer_seen && std::bit_cast<std::uint32_t>(history.heights[0]) == std::bit_cast<std::uint32_t>(heights[0]) &&
           std::bit_cast<std::uint32_t>(history.heights[1]) == std::bit_cast<std::uint32_t>(heights[1]);
}

// Copies and consumes the active height notification without relying on its relocated CALL return address.
// Callers supply the selected history and native foot index (0 or 1).
inline void attach_foot_notification(FootInvocation *invocation, const FootHistory *history, std::uint32_t soldier, std::uint32_t foot,
                                     trace::FootstepRecord &record) noexcept {
    if (!invocation || invocation->history != history || invocation->soldier != soldier || !invocation->pending_site ||
        invocation->pending_foot != foot) {
        return;
    }
    record.site = invocation->pending_site;
    invocation->pending_site = 0;
    std::memcpy(record.old_heights, invocation->old_heights.data(), sizeof(record.old_heights));
    record.sampled_heights[0] = invocation->gates[0].current_height;
    record.sampled_heights[1] = invocation->gates[1].current_height;
    record.sampled_observed = static_cast<std::uint8_t>((invocation->gates[0].observed ? 1 : 0) | (invocation->gates[1].observed ? 2 : 0));
    record.current = invocation->gates[foot];
    record.gate_observed = record.current.observed;
    record.baseline_matches = foot_baseline_matches(*history, invocation->old_heights);
}

} // namespace fc_plugins::sound_diagnostics
