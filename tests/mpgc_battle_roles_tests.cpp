#include "../patches/multiplayer_gc/battle_roles.hpp"
#include "../patches/multiplayer_gc/participants.hpp"

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

TEST_CASE("GC temporary roles cover a full co-op or versus roster", "[mpgc]") {
    for (const bool cooperative : {true, false}) {
        CAPTURE(cooperative);
        const std::uint64_t capacity = cooperative ? 8 : 16;
        gc::Participants participants;
        participants.start({1}, "Host", 1, cooperative);
        for (std::uint64_t id = 2; id <= capacity; ++id) {
            REQUIRE(participants.ready({id}, "Player"));
            if (!cooperative) {
                REQUIRE(participants.choose({id}, id <= 8 ? 1 : 2));
            }
        }
        REQUIRE(participants.accepted_count() == capacity);

        gc::Message roster{.kind = gc::MessageKind::Roster};
        std::optional<gc::BattleRoles> roles{std::in_place};
        for (const auto &member : participants.list()) {
            const auto id = member.peer.value;
            const auto slot = static_cast<std::uint8_t>(id == capacity ? 63 : (id - 1) * 4);
            roster.roster.players.push_back({.slot = slot, .faction = member.faction, .host = id == 1, .name = member.name});
            REQUIRE(roles->set(slot, id % 2 == 0 ? 1 : 2));
        }
        const auto decoded = gc::decode_message(gc::encode_message(roster));
        REQUIRE(decoded);
        const auto &players = decoded->roster.players;
        REQUIRE(players == roster.roster.players);
        REQUIRE(roles->matches(players));
        const std::array<std::uint32_t, 2> teams{2, 1};
        for (std::size_t i = 0; i < players.size(); ++i) {
            CHECK(gc::battle_team(players[i], teams, roles) == (i % 2 == 0 ? 1 : 2));
        }
        CHECK_FALSE(roles->matches(std::span{players}.first(players.size() - 1)));
    }
}

} // namespace
