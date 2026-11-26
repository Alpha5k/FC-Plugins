#include "../patches/multiplayer_gc/battle_roles.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <optional>

namespace {
namespace gc = fc_plugins::multiplayer_gc;

TEST_CASE("GC battle roles accept sparse integer native slots", "[mpgc]") {
    gc::BattleRoles roles;
    CHECK_FALSE(roles.set(-1, 1));
    CHECK_FALSE(roles.set(64, 1));
    CHECK_FALSE(roles.set(0.5, 1));
    CHECK_FALSE(roles.set(std::numeric_limits<double>::quiet_NaN(), 1));
    CHECK_FALSE(roles.set(0, 0));
    CHECK_FALSE(roles.set(0, 1.5));
    CHECK_FALSE(roles.set(0, 3));
    REQUIRE(roles.set(0, 2));
    REQUIRE(roles.set(63, 1));
    CHECK_FALSE(roles.set(63, 2));
    CHECK(roles.role(0) == 2);
    CHECK(roles.role(1) == 0);
    CHECK(roles.role(63) == 1);
    CHECK(roles.role(64) == 0);
}

TEST_CASE("GC battle roles must match the complete admitted roster", "[mpgc]") {
    const std::array players{
        gc::Player{.slot = 0, .faction = 1, .host = true},
        gc::Player{.slot = 5, .faction = 1},
        gc::Player{.slot = 19, .faction = 1},
        gc::Player{.slot = 63, .faction = 1},
        gc::Player{.slot = 7},
    };
    gc::BattleRoles roles;
    CHECK_FALSE(roles.matches({}));
    REQUIRE(roles.set(0, 2));
    REQUIRE(roles.set(5, 1));
    REQUIRE(roles.set(19, 1));
    CHECK_FALSE(roles.matches(players));
    REQUIRE(roles.set(63, 1));
    CHECK(roles.matches(players));
    CHECK_FALSE(roles.matches(std::span{players}.first(3)));
    REQUIRE(roles.set(7, 2));
    CHECK_FALSE(roles.matches(players));
}

TEST_CASE("GC temporary roles use the resolved mission teams and preserve stock assignment", "[mpgc]") {
    const gc::Player host{.slot = 0, .faction = 1, .host = true};
    const gc::Player guest{.slot = 63, .faction = 1};
    std::optional<gc::BattleRoles> roles{std::in_place};
    REQUIRE(roles->set(0, 2));
    REQUIRE(roles->set(63, 1));
    const std::array<std::uint32_t, 2> teams{2, 1};
    CHECK(gc::battle_team(host, teams, roles) == 1);
    CHECK(gc::battle_team(guest, teams, roles) == 2);
    CHECK(gc::battle_team({.slot = 0}, teams, roles) == 0);
    CHECK(gc::battle_team({.slot = 5, .faction = 2}, teams, roles) == 0);
    CHECK(gc::battle_team(host, teams, std::nullopt) == 2);
    CHECK(gc::battle_team({.slot = 5, .faction = 2}, teams, std::nullopt) == 1);
}

} // namespace
