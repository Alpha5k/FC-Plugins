#include "../patches/multiplayer_gc/participants.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {
namespace gc = fc_plugins::multiplayer_gc;

TEST_CASE("GC co-op assigns the saved host faction and includes the host in its capacity", "[mpgc]") {
    gc::Participants players;
    players.start({1}, "Host", 2, true);
    for (std::uint64_t peer = 2; peer <= 8; ++peer) {
        REQUIRE(players.ready({peer}, "Player"));
        CHECK(players.find({peer})->faction == 2);
    }
    CHECK_FALSE(players.ready({9}, "Ninth player"));
    CHECK(players.list().size() == 8);
    CHECK(players.host_faction() == 2);
    CHECK_FALSE(players.choose({1}, 1));
}

TEST_CASE("GC versus waits for a faction choice and preserves it until departure", "[mpgc]") {
    gc::Participants players;
    players.start({1}, "Host", 1, false);
    REQUIRE(players.ready({2}, "Visitor"));
    REQUIRE(players.find({2}));
    CHECK(players.find({2})->faction == 0);
    CHECK(players.accepted_count() == 1);
    CHECK_FALSE(players.choose({2}, 0));
    CHECK_FALSE(players.choose({3}, 2));
    REQUIRE(players.choose({2}, 2));
    CHECK(players.accepted_count() == 2);
    CHECK(players.choose({2}, 2));
    CHECK_FALSE(players.choose({2}, 1));

    REQUIRE(players.ready({2}, "Renamed player"));
    CHECK(players.find({2})->faction == 2);
    CHECK(players.find({2})->name == "Renamed player");
    CHECK(players.list().size() == 2);

    players.remove({2});
    CHECK(players.accepted_count() == 1);
    REQUIRE(players.ready({3}, "Returning player"));
    CHECK(players.find({3})->faction == 0);
    REQUIRE(players.choose({3}, 1));
}

TEST_CASE("GC versus refuses a full faction while the other faction still has room", "[mpgc]") {
    gc::Participants players;
    players.start({1}, "Host", 2, false);
    for (std::uint64_t peer = 2; peer <= 8; ++peer) {
        REQUIRE(players.ready({peer}, "Player"));
        REQUIRE(players.choose({peer}, 2));
    }
    REQUIRE(players.ready({9}, "Visitor"));
    CHECK_FALSE(players.choose({9}, 2));
    CHECK(players.find({9})->faction == 0);
    REQUIRE(players.choose({9}, 1));

    players.remove({4});
    REQUIRE(players.ready({10}, "Replacement"));
    REQUIRE(players.choose({10}, 2));
    CHECK(players.list().back().peer == fc::PeerId{10});
    players.clear();
    CHECK(players.list().empty());
}

TEST_CASE("GC commanders rotate once per faction turn and choose a successor on departure", "[mpgc]") {
    gc::Participants players;
    players.start({1}, "Host", 1, false);
    REQUIRE(players.ready({2}, "Ally"));
    REQUIRE(players.choose({2}, 1));
    REQUIRE(players.ready({3}, "Opponent"));
    REQUIRE(players.choose({3}, 2));
    CHECK(players.commander(1) == fc::PeerId{1});
    CHECK(players.commander(2) == fc::PeerId{3});
    players.begin_turn(1, 4);
    players.begin_turn(1, 4);
    CHECK(players.commander(1) == fc::PeerId{1});
    players.begin_turn(2, 5);
    players.begin_turn(1, 6);
    CHECK(players.commander(1) == fc::PeerId{2});
    CHECK(players.commander(2) == fc::PeerId{3});
    players.remove({2});
    CHECK(players.commander(1) == fc::PeerId{1});
    players.remove({3});
    CHECK(players.commander(2) == fc::PeerId{});
    REQUIRE(players.ready({4}, "Replacement"));
    REQUIRE(players.choose({4}, 2));
    CHECK(players.commander(2) == fc::PeerId{4});
    players.begin_turn(2, 7);
    CHECK(players.commander(2) == fc::PeerId{4});
}

} // namespace
