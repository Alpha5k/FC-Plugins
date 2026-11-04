#include "../patches/collision_history_fix/history.hpp"

#include <catch2/catch_test_macros.hpp>

namespace collision_history = fc_plugins::collision_history_fix;

namespace {

constexpr collision_history::SoldierIdentity kSoldier{.soldier = 0x1000, .player = 7, .local_player = 0};
constexpr collision_history::Contacts kWallFirst{0x2000, 0x3000, 0x4000};
constexpr collision_history::Contacts kSlopeFirst{0x5000, 0x2000, 0x3000};

} // namespace

TEST_CASE("Earlier collision replay restores its exact contact slots and replaces the speculative suffix", "[plugins][collision_history]") {
    collision_history::History history;
    history.start_frontier(kSoldier, 22257, kWallFirst);
    history.advanced(kSoldier, 22257);
    history.capture(kSoldier, 22258, kWallFirst);
    history.advanced(kSoldier, 22258);
    history.capture(kSoldier, 22259, kWallFirst);
    history.advanced(kSoldier, 22259);
    history.complete(kSoldier, 22260, kSlopeFirst);

    REQUIRE(history.needs_rewind(kSoldier, 22257));
    REQUIRE(history.rewind(kSoldier, 22257) == kWallFirst);
    REQUIRE(history.rewind(kSoldier, 22258) == kWallFirst);
    // Native replay advances from its restored list; the old post-slope boundary cannot override it.
    history.advanced(kSoldier, 22258);
    history.complete(kSoldier, 22259, kWallFirst);
    REQUIRE(history.rewind(kSoldier, 22259) == kWallFirst);
    CHECK_FALSE(history.rewind(kSoldier, 22260));
}

TEST_CASE("Authority read replaces its matching seed before the same turn simulates", "[plugins][collision_history]") {
    collision_history::History history;
    history.start_frontier(kSoldier, 100, kWallFirst);
    history.advanced(kSoldier, 100);
    history.complete(kSoldier, 101, kSlopeFirst);
    REQUIRE(history.rewind(kSoldier, 100) == kWallFirst);
    // The completed native reader owns this boundary, including changes caused inside the reader.
    const collision_history::Contacts read_result{0x2000, 0x6000, 0x4000};
    history.capture(kSoldier, 100, read_result);
    REQUIRE(history.rewind(kSoldier, 101) == kSlopeFirst);
    REQUIRE(history.rewind(kSoldier, 100) == read_result);
    history.advanced(kSoldier, 100);
    history.complete(kSoldier, 101, read_result);
    CHECK(history.rewind(kSoldier, 101) == read_result);
}

TEST_CASE("A collision history miss suspends capture until a live frontier establishes a new baseline", "[plugins][collision_history]") {
    collision_history::History history;
    history.start_frontier(kSoldier, 100, kWallFirst);
    history.capture(kSoldier, 132, kSlopeFirst);
    REQUIRE_FALSE(history.rewind(kSoldier, 100));
    history.capture(kSoldier, 100, kSlopeFirst);
    history.advanced(kSoldier, 100);
    history.complete(kSoldier, 101, kSlopeFirst);
    CHECK_FALSE(history.rewind(kSoldier, 101));
    CHECK_FALSE(history.rewind(kSoldier, 132));
    history.start_frontier(kSoldier, 140, kWallFirst);
    CHECK(history.rewind(kSoldier, 140) == kWallFirst);
    history.capture(kSoldier, 180, kSlopeFirst);
    CHECK_FALSE(history.rewind(kSoldier, 140));
}

TEST_CASE("Collision destruction retires saved references while survivor priorities keep their slots", "[plugins][collision_history]") {
    collision_history::History history;
    history.start_frontier(kSoldier, 100, kWallFirst);
    history.forget_object(0x3000);
    const collision_history::Contacts surviving{0x2000, 0, 0x4000};
    CHECK(history.rewind(kSoldier, 100) == surviving);
    auto resident = kWallFirst;
    collision_history::History::forget_contact(resident, 0x3000);
    CHECK(resident == surviving);
    // Reusing the contact's old address cannot resurrect it in an older checkpoint.
    history.capture(kSoldier, 101, kWallFirst);
    CHECK(history.rewind(kSoldier, 100) == surviving);
    CHECK(history.rewind(kSoldier, 101) == kWallFirst);
}

TEST_CASE("Soldier reset, local reassignment and session replacement discard collision history", "[plugins][collision_history]") {
    collision_history::History history;
    history.start_frontier(kSoldier, 100, kWallFirst);
    auto identity = kSoldier;
    SECTION("Reset retires the same address before native reuse") {
        history.clear_soldier(kSoldier.soldier);
    }
    SECTION("Another soldier takes the same local input slot") {
        identity.soldier = 0x1100;
    }
    SECTION("Another native player takes the same soldier") {
        ++identity.player;
    }
    SECTION("The same soldier moves to another local input slot") {
        identity.local_player = 1;
    }
    SECTION("The native session changes") {
        history.clear_all();
    }
    CHECK_FALSE(history.rewind(identity, 100));
    const collision_history::SoldierIdentity second{.soldier = 0x1200, .player = 8, .local_player = 1};
    history.start_frontier(second, 100, kSlopeFirst);
    CHECK(history.rewind(second, 100) == kSlopeFirst);
}
