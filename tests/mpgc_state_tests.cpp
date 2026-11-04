#include "../patches/multiplayer_gc/state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace {
namespace gc = fc_plugins::multiplayer_gc;

TEST_CASE("GC capture copies used binary bytes and accepts absent mission setup", "[mpgc]") {
    std::array<char, 512> campaign{};
    std::array<char, 256> mission{};
    campaign[0] = 'x';
    campaign[2] = 'y';
    const auto result = gc::capture_state(campaign, 3, mission, 0);
    REQUIRE(result);
    CHECK(result->campaign == std::string("x\0y", 3));
    CHECK(result->mission.empty());
    CHECK_FALSE(gc::capture_state(campaign, 0, mission, 0));
    CHECK_FALSE(gc::capture_state(campaign, 513, mission, 0));
    CHECK_FALSE(gc::capture_state(campaign, 3, mission, 257));
}

TEST_CASE("GC install validates both payloads before changing native storage", "[mpgc]") {
    std::array<char, 512> campaign{};
    std::array<char, 256> mission{};
    campaign.fill('a');
    mission.fill('b');
    std::uint32_t campaign_length = 5;
    std::uint32_t mission_length = 6;
    const gc::State oversized{"ok", std::string(257, 'm')};
    CHECK_FALSE(gc::install_state(campaign, campaign_length, mission, mission_length, oversized));
    CHECK(campaign[0] == 'a');
    CHECK(mission[0] == 'b');
    CHECK(campaign_length == 5);
    CHECK(mission_length == 6);

    const gc::State source{std::string("c\0x", 3), std::string("m\0z", 3)};
    REQUIRE(gc::install_state(campaign, campaign_length, mission, mission_length, source));
    CHECK(campaign_length == 3);
    CHECK(mission_length == 3);
    CHECK(std::string(campaign.data(), campaign_length) == source.campaign);
    CHECK(std::string(mission.data(), mission_length) == source.mission);
    CHECK(campaign[3] == 'a');
    CHECK(mission[3] == 'b');
}

} // namespace
