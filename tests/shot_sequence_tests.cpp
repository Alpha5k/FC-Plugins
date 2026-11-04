#include "../patches/shot_sequence/comparison.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

namespace cooldown = fc_plugins::shot_sequence;

TEST_CASE("Cannon cooldown accepts the captured completion boundary only when enabled", "[plugins][cannon_cooldown]") {
    const float elapsed = 0.9999997019767761F;
    const float limit = 1.0F;

    CHECK(cooldown::correct_cooldown_comparison(0xA03U, elapsed, limit, true) == 0xA42U);
    CHECK(cooldown::correct_cooldown_comparison(0xA03U, elapsed, limit, false) == 0xA03U);
    CHECK(cooldown::correct_cooldown_comparison(0x203U, 0.99999F, limit, true) == 0x203U);
}

TEST_CASE("Cannon cooldown measures its four-step allowance above each limit", "[plugins][cannon_cooldown]") {
    struct Boundary {
        float limit;
        float threshold;
        float earlier;
    };

    // At powers of two, spacing below the limit is half the spacing above it.
    constexpr std::array boundaries{
        Boundary{0.1F, 0.09999997168779373F, 0.09999996423721313F},
        Boundary{0.5F, 0.4999997615814209F, 0.4999997317790985F},
        Boundary{1.0F, 0.9999995231628418F, 0.9999994635581970F},
        Boundary{2.0F, 1.9999990463256836F, 1.9999989271163940F},
    };
    for (const auto &boundary : boundaries) {
        CAPTURE(boundary.limit);
        CHECK(cooldown::correct_cooldown_comparison(0x203U, boundary.threshold, boundary.limit, true) == 0x242U);
        CHECK(cooldown::correct_cooldown_comparison(0x203U, boundary.earlier, boundary.limit, true) == 0x203U);
    }
}

TEST_CASE("Cannon cooldown preserves native accepted, unordered and nonfinite results", "[plugins][cannon_cooldown]") {
    constexpr auto infinity = std::numeric_limits<float>::infinity();
    constexpr auto nan = std::numeric_limits<float>::quiet_NaN();

    CHECK(cooldown::correct_cooldown_comparison(0x202U, 1.1F, 1.0F, true) == 0x202U);
    CHECK(cooldown::correct_cooldown_comparison(0x242U, 1.0F, 1.0F, true) == 0x242U);
    CHECK(cooldown::correct_cooldown_comparison(0x247U, nan, 1.0F, true) == 0x247U);
    CHECK(cooldown::correct_cooldown_comparison(0x247U, 0.9999997F, 1.0F, true) == 0x247U);
    for (const float elapsed : {infinity, -infinity, nan}) {
        CHECK(cooldown::correct_cooldown_comparison(0x203U, elapsed, 1.0F, true) == 0x203U);
    }
    for (const float limit : {0.0F, -1.0F, infinity, -infinity, nan, std::numeric_limits<float>::max()}) {
        CHECK(cooldown::correct_cooldown_comparison(0x203U, 0.0F, limit, true) == 0x203U);
    }
}
