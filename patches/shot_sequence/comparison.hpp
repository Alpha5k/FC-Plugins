#pragma once

#include <bit>
#include <cstdint>

namespace fc_plugins::shot_sequence {

// Accepts elapsed cooldowns just below a positive finite limit using four times its upward float spacing.
// Call after native COMISS so unordered results and floating-point exceptions remain native.
constexpr std::uint32_t correct_cooldown_comparison(std::uint32_t flags, float elapsed, float limit, bool corrected) noexcept {
    constexpr auto kCarry = 0x1U;
    constexpr auto kParity = 0x4U;
    constexpr auto kZero = 0x40U;
    constexpr auto kMagnitude = 0x7FFFFFFFU;
    constexpr auto kInfinity = 0x7F800000U;
    constexpr auto kMaxFinite = 0x7F7FFFFFU;
    if (!corrected || (flags & kCarry) == 0 || (flags & kParity) != 0) {
        return flags;
    }

    const auto elapsed_bits = std::bit_cast<std::uint32_t>(elapsed);
    const auto limit_bits = std::bit_cast<std::uint32_t>(limit);
    if ((elapsed_bits & kMagnitude) >= kInfinity || limit_bits == 0 || limit_bits >= kMaxFinite || elapsed >= limit) {
        return flags;
    }

    // Spacing above a power-of-two limit is twice its spacing below; counting lower representations would halve the allowance.
    const float next = std::bit_cast<float>(limit_bits + 1);
    const float allowance = (next - limit) * 4.0F;
    const float threshold = limit - allowance;
    if (elapsed >= threshold) {
        return (flags & ~kCarry) | kZero;
    }
    return flags;
}

} // namespace fc_plugins::shot_sequence
