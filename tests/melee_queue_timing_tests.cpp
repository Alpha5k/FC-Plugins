#include "../patches/melee_timing/time.hpp"
#include "../patches/melee_timing/queue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>

namespace melee_input = fc_plugins::melee_timing;

TEST_CASE("Melee input accepts Vader's fresh press at both recorded clock origins", "[plugins][melee_input]") {
    constexpr float age = 5.250001907348633F;
    constexpr float client = 63.49931716918945F;
    constexpr float server = 63.54931640625F;
    const float native_client = (client - age) + age;
    const float native_server = (server - age) + age;
    REQUIRE(native_client < client);
    REQUIRE(native_server == server);

    CHECK(melee_input::absolute_time(client, age, age) == client);
    CHECK(melee_input::absolute_time(server, age, age) == server);
}

TEST_CASE("Melee input converts both decoded condition bounds before inclusive queue testing", "[plugins][melee_input]") {
    constexpr float clock = 63.49931716918945F;
    constexpr float end = 5.250001907348633F;
    const float lower = melee_input::absolute_time(clock, end - 0.25F, end);
    const float upper = melee_input::absolute_time(clock, end, end);
    CHECK(lower == clock - 0.25F);
    CHECK(upper == clock);
    const auto inside = [lower, upper](float time) {
        return lower <= time && time <= upper;
    };
    CHECK(inside(lower));
    CHECK(inside(upper));
    CHECK_FALSE(inside(std::nextafter(lower, -std::numeric_limits<float>::infinity())));
    CHECK_FALSE(inside(std::nextafter(upper, std::numeric_limits<float>::infinity())));
}

TEST_CASE("Melee input consumes Maul's fresh press at both recorded clock origins", "[plugins][melee_input]") {
    constexpr float condition = 1.8499984741210938F;
    constexpr float age = 1.8499993085861206F;
    constexpr float client = 128.45323181152344F;
    constexpr float server = 128.50323486328125F;
    const float native_client = (client + condition) - age;
    const float native_server = (server + condition) - age;
    REQUIRE(native_client < client);
    REQUIRE(native_server == server);

    CHECK(melee_input::absolute_time(client, condition, age) == client);
    CHECK(melee_input::absolute_time(server, condition, age) == server);
}

TEST_CASE("Melee input preserves earlier and future condition times without widening the cutoff", "[plugins][melee_input]") {
    constexpr float clock = 128.45323181152344F;
    const float earlier = melee_input::absolute_time(clock, 1.5F, 2.0F);
    const float future = melee_input::absolute_time(clock, 2.5F, 2.0F);
    CHECK(earlier == clock - 0.5F);
    CHECK(future == clock + 0.5F);
    CHECK(clock > earlier);
    CHECK(clock < future);
    CHECK(std::nextafter(earlier, std::numeric_limits<float>::infinity()) > earlier);
}

TEST_CASE("Melee input retains signed decoded offsets and zero clock origins", "[plugins][melee_input]") {
    CHECK(melee_input::absolute_time(0.0F, -0.25F, 0.5F) == -0.75F);
    CHECK(melee_input::absolute_time(32.0F, -0.25F, 0.5F) == 31.25F);
    CHECK(melee_input::absolute_time(32.0F, 0.25F, 0.0F) == 32.25F);
}

TEST_CASE("Native input projection keeps exact elapsed ages across variable updates and circular restore", "[plugins][melee_input]") {
    std::array<std::byte, 0x200> weapon{};
    fc::write_field(weapon.data(), 0x174, std::uint8_t{1});
    fc::write_field(weapon.data(), 0x175, std::uint8_t{7});
    fc::write_field(weapon.data(), 0x134 + 7 * 8, -0.05F);
    melee_input::QueueTiming timing(weapon.data());
    const double initial = static_cast<double>(0.05F);
    for (const float elapsed : {1.0F / 60.0F, 0.2F, 1.0F / 20.0F}) {
        timing.project(weapon.data());
        fc::write_field(weapon.data(), 0x130, elapsed);
        timing.advance(weapon.data());
    }
    const double expected = initial + static_cast<double>(1.0F / 60.0F) + static_cast<double>(0.2F) + static_cast<double>(1.0F / 20.0F);
    const auto saved = timing.capture(weapon.data());
    CHECK(saved.values[0] == expected);
    CHECK(fc::read_field<float>(weapon.data(), 0x134 + 7 * 8) == static_cast<float>(-expected));
    fc::write_field(weapon.data(), 0x175, std::uint8_t{0});
    timing.restore(weapon.data(), saved);
    CHECK(timing.capture(weapon.data()).values[0] == expected);
    CHECK(fc::read_field<float>(weapon.data(), 0x130) == 0.0F);
}

TEST_CASE("Consumption retains the winning input while later queries lose or fail", "[plugins][melee_input]") {
    melee_input::WinningInput winner;
    winner.query = -0.05000000074505806F;
    winner.select();
    winner.query = -0.01F;
    CHECK(*winner.selected == -0.05000000074505806F);
    winner.query.reset();
    CHECK(*winner.selected == -0.05000000074505806F);
    winner.select();
    CHECK_FALSE(winner.selected);
}
