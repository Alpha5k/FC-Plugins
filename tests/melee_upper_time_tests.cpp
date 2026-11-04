#include "../patches/melee_timing/upper_time.hpp"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <limits>

namespace melee_timing = fc_plugins::melee_timing;

TEST_CASE("Melee timing accepts Grievous's captured three-step endpoint error", "[plugins][melee_timing]") {
    constexpr float elapsed = 0.9000001549720764F;
    constexpr float upper = 0.8999999761581421F;
    REQUIRE(elapsed > upper);
    REQUIRE(std::bit_cast<std::uint32_t>(elapsed) - std::bit_cast<std::uint32_t>(upper) == 3);

    // Native COMISS clears CF and ZF for this rejecting comparison; unrelated flags retain their native values.
    constexpr auto native_flags = 0x202U;
    CHECK(melee_timing::correct_upper_comparison(native_flags, elapsed, upper, true) == (native_flags | 0x40U));
    CHECK(melee_timing::correct_upper_comparison(native_flags, elapsed, upper, false) == native_flags);
}

TEST_CASE("Melee timing bounds its correction to four float steps", "[plugins][melee_timing]") {
    constexpr float upper = 0.8999999761581421F;
    const auto bits = std::bit_cast<std::uint32_t>(upper);
    const float fourth = std::bit_cast<float>(bits + 4);
    const float fifth = std::bit_cast<float>(bits + 5);
    CHECK(melee_timing::correct_upper_comparison(0x202U, fourth, upper, true) == 0x242U);
    CHECK(melee_timing::correct_upper_comparison(0x202U, fifth, upper, true) == 0x202U);
    CHECK(melee_timing::correct_upper_comparison(0x202U, 0.9099999666213989F, upper, true) == 0x202U);
}

TEST_CASE("Melee timing retains native equality, earlier, unordered and infinite comparisons", "[plugins][melee_timing]") {
    constexpr float upper = 0.8999999761581421F;
    CHECK(melee_timing::correct_upper_comparison(0x242U, upper, upper, true) == 0x242U);
    CHECK(melee_timing::correct_upper_comparison(0x203U, 0.8500001430511475F, upper, true) == 0x203U);
    CHECK(melee_timing::correct_upper_comparison(0x247U, std::numeric_limits<float>::quiet_NaN(), upper, true) == 0x247U);
    CHECK(melee_timing::correct_upper_comparison(0x202U, std::numeric_limits<float>::infinity(), upper, true) == 0x202U);
    CHECK(melee_timing::correct_upper_comparison(0x202U, 0.0F, -std::numeric_limits<float>::infinity(), true) == 0x202U);
}
