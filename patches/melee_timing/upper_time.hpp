#pragma once

#include <bit>
#include <cstdint>

namespace fc_plugins::melee_timing {
namespace detail {

// Orders finite float representations by value, counting both signed zeros as one value.
constexpr std::uint32_t float_order(std::uint32_t bits) noexcept {
    constexpr auto kSign = 0x80000000U;
    const auto magnitude = bits & ~kSign;
    return (bits & kSign) != 0 ? kSign - magnitude : kSign + magnitude;
}

} // namespace detail

// Changes a rejecting COMISS/JA result to equality only within four finite float steps of the upper time.
// Call after native COMISS so its unordered results and floating-point exceptions remain native.
constexpr std::uint32_t correct_upper_comparison(std::uint32_t flags, float elapsed, float upper, bool corrected) noexcept {
    constexpr auto kCarryOrZero = 0x41U;
    constexpr auto kZero = 0x40U;
    constexpr auto kMagnitude = 0x7FFFFFFFU;
    constexpr auto kInfinity = 0x7F800000U;
    // The captured endpoint differs by three steps; four gives one step of margin without a time-step extension.
    constexpr auto kMaxFloatSteps = 4U;
    if (!corrected || (flags & kCarryOrZero) != 0) {
        return flags;
    }

    const auto elapsed_bits = std::bit_cast<std::uint32_t>(elapsed);
    const auto upper_bits = std::bit_cast<std::uint32_t>(upper);
    if ((elapsed_bits & kMagnitude) >= kInfinity || (upper_bits & kMagnitude) >= kInfinity) {
        return flags;
    }

    const auto elapsed_order = detail::float_order(elapsed_bits);
    const auto upper_order = detail::float_order(upper_bits);
    if (elapsed_order > upper_order && elapsed_order - upper_order <= kMaxFloatSteps) {
        return flags | kZero;
    }
    return flags;
}

} // namespace fc_plugins::melee_timing
