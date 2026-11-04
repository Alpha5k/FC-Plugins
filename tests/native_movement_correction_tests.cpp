#include "../patches/soldier_state_sync/timer.hpp"
#include "../patches/soldier_state_sync/rotation.hpp"
#include "../patches/soldier_state_sync/format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace state_sync = fc_plugins::soldier_state_sync;

namespace {

// Executes the qualified native decoder after its unchanged wire-state conversion, with the selected AND operand.
std::uint32_t decode_trigger(std::uint32_t previous, std::uint32_t state, std::uint32_t coarse,
                             std::uint32_t timer_mask = state_sync::kReceivedCoarseTimerMask) noexcept {
    std::uint32_t word = (previous & ~0x1FU) | state;
    word = (word & ~0x1F000000U) | (coarse << 24);
    if ((word & 0x1F000000U) != 0) {
        // Native XOR/AND/XOR replaces bits 8..28 while retaining the other state and reserved bits.
        return word ^ ((((word & timer_mask) + 0x00800000U) ^ word) & 0x1FFFFF00U);
    }
    return (word & 0x1FU) == 0x0CU ? ((word & ~3U) | 0x1CU) : word;
}

// Measures the horizontal scale in the pair used to construct the native yaw matrix.
double length_squared(float sine, float cosine) noexcept {
    return static_cast<double>(sine) * sine + static_cast<double>(cosine) * cosine;
}

} // namespace

TEST_CASE("Repeated saturated trigger packets retain one midpoint instead of wrapping to a fresh press", "[plugins][native_trigger]") {
    // Captured Grievous forward-trigger words at turn 17661 reproduce the retail decoder failure.
    REQUIRE(decode_trigger(0x1F000004, 4, 31, 0xFFFFFF00) == 0x1F800004);
    REQUIRE(decode_trigger(0x1F800004, 4, 31, 0xFFFFFF00) == 4);
    std::uint32_t trigger = 0x1F000004;
    for (int receive{}; receive < 4; ++receive) {
        trigger = decode_trigger(trigger, 4, 31);
        CHECK(trigger == 0x1F800004);
    }
    // This old press remains older than the captured 50 ms timer that admitted the false double tap.
    CHECK((trigger & 0x1FFFFF00U) > 0x01800000U);
}

TEST_CASE("Trigger midpoint reconstruction keeps converted states and reserved flags", "[plugins][native_trigger]") {
    // These are the native conversion function's possible outputs for the four-bit wire states.
    constexpr std::array<std::uint32_t, 16> states{0, 3, 1, 6, 4, 9, 10, 8, 19, 17, 22, 20, 25, 26, 24, 12};
    constexpr std::uint32_t preserved = 0xE00000E0;
    for (const auto state : states) {
        for (std::uint32_t coarse = 1; coarse < 32; ++coarse) {
            const auto decoded = decode_trigger(preserved | 0x00FFFF00U | 4, state, coarse);
            CHECK(decoded == (preserved | (coarse << 24) | 0x00800000U | state));
        }
    }
}

TEST_CASE("Zero-coarse trigger packets retain native fraction and low-0C conversion", "[plugins][native_trigger]") {
    constexpr std::uint32_t old_fraction = 0xE0ABCDEF;
    CHECK(decode_trigger(old_fraction, 4, 0) == 0xE0ABCDE4);
    CHECK(decode_trigger(old_fraction, 12, 0) == 0xE0ABCDFC);
    CHECK(decode_trigger(old_fraction, 12, 1) == 0xE18000EC);
}

TEST_CASE("Exact Trigger restoration preserves the authority duration while the native codec reconstructs a midpoint",
          "[plugins][native_trigger]") {
    constexpr std::uint32_t previous = 0xE08000E4;
    constexpr std::uint32_t authoritative = 0x1F000009;
    const auto code = state_sync::trigger_code(authoritative);
    CHECK(state_sync::restore_trigger(previous, code & 31, code >> 5) == 0xFF0000E9);
    CHECK(decode_trigger(previous, 4, 31) == 0xFF8000E4);
}

TEST_CASE("Precise block restoration preserves the captured hold classification after four 60 TPS steps", "[plugins][native_trigger]") {
    constexpr std::uint32_t authority = 0x07000001;
    const auto code = state_sync::trigger_code(authority);
    auto exact = state_sync::restore_trigger(0, code & 31, code >> 5);
    auto coarse = decode_trigger(0, authority & 31, authority >> 24);
    REQUIRE(exact == authority);
    REQUIRE(coarse == 0x07800001);
    constexpr std::array expected{0x07800001U, 0x08000001U, 0x08800001U, 0x09000001U};
    // Native duration advances 0x8000 units at 60 TPS and sets held only above 0.30 s (0x90000 units).
    const auto advance = [](std::uint32_t trigger) noexcept {
        const auto duration = ((trigger >> 8) & 0x1FFFFF) + 0x8000;
        const auto state = (trigger & 31) | (duration > 0x90000 ? 8U : 0U);
        return state_sync::restore_trigger(trigger, state, duration);
    };
    for (const auto word : expected) {
        exact = advance(exact);
        coarse = advance(coarse);
        CHECK(exact == word);
    }
    CHECK(exact == 0x09000001);
    CHECK(coarse == 0x09800009);
    CHECK((exact & 8) == 0);
    CHECK((coarse & 8) != 0);
}

TEST_CASE("Captured negative and positive ground helper pairs produce unit rotations", "[plugins][ground_rotation]") {
    constexpr std::array pairs{
        std::array{-0.03987237066030502F, 0.9992142915725708F},
        std::array{0.19809451699256897F, 0.9801828861236572F},
        std::array{-0.0796816274523735F, 0.9968298673629761F},
    };
    REQUIRE(length_squared(pairs[0][0], pairs[0][1]) > 1.000019);
    REQUIRE(length_squared(pairs[1][0], pairs[1][1]) < 1.0);
    for (auto pair : pairs) {
        const double direction = std::atan2(static_cast<double>(pair[0]), static_cast<double>(pair[1]));
        state_sync::normalize_rotation(pair[0], pair[1]);
        CHECK(std::abs(length_squared(pair[0], pair[1]) - 1.0) < 0.0000001);
        // The normalized outputs return to native float storage, so direction retains float precision.
        CHECK(std::abs(std::atan2(static_cast<double>(pair[0]), static_cast<double>(pair[1])) - direction) <
              std::numeric_limits<float>::epsilon());
    }
}

TEST_CASE("Repeated captured ground turns no longer accumulate native table scale", "[plugins][ground_rotation]") {
    float sine = -0.03987237066030502F;
    float cosine = 0.9992142915725708F;
    REQUIRE(std::pow(length_squared(sine, cosine), 10000) > 1.20);
    state_sync::normalize_rotation(sine, cosine);
    float x = 1.0F;
    float z = 0.0F;
    for (int turn{}; turn < 10000; ++turn) {
        const float next_x = cosine * x - sine * z;
        z = sine * x + cosine * z;
        x = next_x;
    }
    CHECK(std::abs(length_squared(x, z) - 1.0) < 0.001);
}
