#include "../patches/weapon_swap_replay_fix/packed_ledger.hpp"

#include <catch2/catch_test_macros.hpp>

namespace weapon_swap = fc_plugins::weapon_swap_replay_fix;

TEST_CASE("Weapon swap requests distinguish replay from a new transition", "[plugins][weapon_swap]") {
    int soldier{};
    int weapons[3]{};
    weapon_swap::LocalSwapLedger ledger;

    const auto first = ledger.record_local(&soldier, 0, 0, 0, 1, &weapons[0], &weapons[1], 100, {95, 100, 95});
    REQUIRE(first.kind == weapon_swap::RequestKind::New);
    REQUIRE(first.epoch != 0);

    const auto replay = ledger.record_local(&soldier, 0, 0, 0, 1, &weapons[0], &weapons[1], 100, {96, 100, 96});
    CHECK(replay.kind == weapon_swap::RequestKind::Replay);
    CHECK(replay.epoch == first.epoch);

    SECTION("Exact replay preserves the latest target in a request chain") {
        const auto next = ledger.record_local(&soldier, 0, 0, 1, 2, &weapons[1], &weapons[2], 101, {97, 101, 97});
        CHECK(next.kind == weapon_swap::RequestKind::New);
        weapon_swap::EpochSnapshot snapshot;
        REQUIRE(ledger.find(&soldier, 0, 0, snapshot));
        CHECK(snapshot.final_index == 2);

        ledger.observe_frontiers({105, 105, 97});
        const auto historical = ledger.record_local(&soldier, 0, 0, 1, 2, &weapons[1], &weapons[2], 101, {101, 101, 97});
        CHECK(historical.kind == weapon_swap::RequestKind::Replay);
        const auto earlier = ledger.record_local(&soldier, 0, 0, 0, 1, &weapons[0], &weapons[1], 100, {100, 100, 97});
        CHECK(earlier.kind == weapon_swap::RequestKind::Replay);
        REQUIRE(ledger.resolve(&soldier, 0, 0, 1, snapshot));
        CHECK(snapshot.final_index == 2);
    }

    SECTION("A changed target at the same turn clears contradictory history") {
        const auto corrected = ledger.record_local(&soldier, 0, 0, 1, 0, &weapons[1], &weapons[0], 100, {99, 100, 99});
        CHECK(corrected.kind == weapon_swap::RequestKind::HistoricalConflict);
        CHECK(corrected.epoch == first.epoch);
        CHECK_FALSE(ledger.has_active());
    }

    SECTION("A changed target in a later retained request also clears history") {
        REQUIRE(ledger.record_local(&soldier, 0, 0, 1, 2, &weapons[1], &weapons[2], 101, {97, 101, 97}).kind ==
                weapon_swap::RequestKind::New);
        ledger.observe_frontiers({105, 105, 97});
        const auto corrected = ledger.record_local(&soldier, 0, 0, 2, 1, &weapons[2], &weapons[1], 101, {101, 101, 97});
        CHECK(corrected.kind == weapon_swap::RequestKind::HistoricalConflict);
        CHECK(corrected.epoch == first.epoch);
        CHECK_FALSE(ledger.has_active());
    }

    SECTION("A fresh return to the projected weapon starts a new request after correction") {
        // Authority restores the old weapon while presentation still retains the acknowledged request.
        const auto authority = ledger.observe_authority(&soldier, 0, 0, 1, 0, &weapons[1], &weapons[0], {105, 105, 100});
        REQUIRE(authority.kind == weapon_swap::AuthorityKind::Pending);
        const auto fresh = ledger.record_local(&soldier, 0, 0, 0, 1, &weapons[0], &weapons[1], 106, {105, 106, 100});
        CHECK(fresh.kind == weapon_swap::RequestKind::New);
        CHECK(fresh.epoch != first.epoch);
        weapon_swap::EpochSnapshot snapshot;
        REQUIRE(ledger.find(&soldier, 0, 0, snapshot));
        CHECK(snapshot.latest_request_turn == 106);
        CHECK(snapshot.final_index == 1);

        const auto repeated = ledger.record_local(&soldier, 0, 0, 0, 1, &weapons[0], &weapons[1], 106, {105, 106, 100});
        CHECK(repeated.kind == weapon_swap::RequestKind::Replay);
        CHECK(repeated.epoch == fresh.epoch);
    }
}

TEST_CASE("Weapon swap authority accepts only after more than two settlement turns", "[plugins][weapon_swap]") {
    int soldier{};
    int weapons[3]{};
    weapon_swap::LocalSwapLedger ledger;
    REQUIRE(ledger.record_local(&soldier, 0, 0, 1, 2, &weapons[1], &weapons[2], 100, {95, 100, 95}).valid());

    // Update and acknowledgement have reached the request; settlement starts at the observed update turn 110.
    auto decision = ledger.observe_authority(&soldier, 0, 0, 1, 2, &weapons[1], &weapons[2], {110, 110, 100});
    CHECK(decision.kind == weapon_swap::AuthorityKind::Pending);
    CHECK(decision.mute_select);

    decision = ledger.observe_authority(&soldier, 0, 0, 1, 2, &weapons[1], &weapons[2], {112, 112, 100});
    CHECK(decision.kind == weapon_swap::AuthorityKind::Pending);
    CHECK(ledger.has_active());

    decision = ledger.observe_authority(&soldier, 0, 0, 1, 2, &weapons[1], &weapons[2], {113, 113, 100});
    CHECK(decision.kind == weapon_swap::AuthorityKind::Accepted);
    CHECK_FALSE(ledger.has_active());
}

TEST_CASE("Weapon swap reconciliation fails open after inventory replacement", "[plugins][weapon_swap]") {
    int soldier{};
    int weapons[4]{};
    weapon_swap::LocalSwapLedger ledger;
    REQUIRE(ledger.record_local(&soldier, 0, 0, 1, 2, &weapons[1], &weapons[2], 200, {190, 200, 190}).valid());

    // The server names a slot whose pointer no longer matches retained inventory, so replay cannot be muted.
    const auto decision = ledger.observe_authority(&soldier, 0, 0, 2, 1, &weapons[2], &weapons[3], {195, 200, 195});
    CHECK(decision.kind == weapon_swap::AuthorityKind::Correction);
    CHECK(decision.pointer_invalidated);
    CHECK_FALSE(decision.mute_select);
    CHECK_FALSE(ledger.has_active());
}

TEST_CASE("Packed weapon selection remains projected after channel acceptance", "[plugins][weapon_swap]") {
    int soldier{};
    int weapons[3]{};
    weapon_swap::PackedSwapLedger ledger;
    REQUIRE(ledger.track_projected(&soldier, 0, 1, 2, &weapons[2], 300, 7, 8, {299, 300, 299}));
    const auto replay = ledger.observe_transition(&soldier, 0, 1, 2, &weapons[2], 0, 0, &weapons[0], {301, 301, 299});
    REQUIRE(replay.mute_select);

    // One channel can settle before the combined channel-and-slot value catches up.
    ledger.observe_channel_result(0, {weapon_swap::AuthorityKind::Accepted, 7, 8, true, false});
    REQUIRE(ledger.has_active());

    weapon_swap::PackedSnapshot snapshot;
    REQUIRE(ledger.resolve(&soldier, 0, 0, 0, &weapons[0], snapshot));
    CHECK(snapshot.projected_key == weapon_swap::PackedSwapLedger::key(1, 2));
    CHECK(weapon_swap::PackedSwapLedger::project_selection(0xC0, snapshot.projected_key) == 0xD2);

    SECTION("Pending ownership learns one unknown endpoint from a recorded weapon") {
        weapon_swap::PackedSwapLedger pending;
        REQUIRE(pending.track_projected(&soldier, 0, 1, 2, &weapons[2], 300, 7, 8, {299, 300, 299}));
        const auto decision = pending.observe_transition(&soldier, 0, 1, 2, &weapons[2], 0, 0, &weapons[0], {301, 301, 299});
        CHECK(decision.mute_select);
        CHECK_FALSE(decision.pointer_invalidated);
        REQUIRE(pending.resolve(&soldier, 0, 0, 0, &weapons[0], snapshot));
        CHECK(snapshot.node_weapons[weapon_swap::PackedSwapLedger::key(0, 0)] == &weapons[0]);
    }

    SECTION("Settled ownership rejects an unknown endpoint without reporting replacement") {
        weapon_swap::PackedSwapLedger settled;
        REQUIRE(settled.track_projected(&soldier, 0, 1, 2, &weapons[2], 300, 7, 8, {299, 300, 299}));
        settled.observe_channel_result(0, {weapon_swap::AuthorityKind::Accepted, 7, 8, true, false});
        const auto decision = settled.observe_transition(&soldier, 0, 1, 2, &weapons[2], 0, 0, &weapons[0], {301, 301, 299});
        CHECK_FALSE(decision.mute_select);
        CHECK_FALSE(decision.pointer_invalidated);
        CHECK_FALSE(settled.has_active());
    }
}

TEST_CASE("Weapon swap authority waits for acknowledgement and preserves corrective selection", "[plugins][weapon_swap]") {
    int soldier{};
    int weapons[3]{};
    weapon_swap::LocalSwapLedger ledger;
    REQUIRE(ledger.record_local(&soldier, 0, 1, 0, 2, &weapons[0], &weapons[2], 100, {95, 100, 95}).valid());

    // A newer update alone cannot settle the request before its input has been acknowledged.
    auto decision = ledger.observe_authority(&soldier, 0, 1, 2, 0, &weapons[2], &weapons[0], {110, 110, 99});
    CHECK(decision.kind == weapon_swap::AuthorityKind::Pending);
    CHECK(decision.mute_select);
    decision = ledger.observe_authority(&soldier, 0, 1, 2, 0, &weapons[2], &weapons[0], {111, 111, 100});
    CHECK(decision.kind == weapon_swap::AuthorityKind::Pending);
    decision = ledger.observe_authority(&soldier, 0, 1, 2, 0, &weapons[2], &weapons[0], {114, 114, 100});
    CHECK(decision.kind == weapon_swap::AuthorityKind::Rejected);
    CHECK_FALSE(decision.mute_select);
    CHECK_FALSE(ledger.has_active());
}

TEST_CASE("Weapon swap channels and local players retain independent requests", "[plugins][weapon_swap]") {
    int soldiers[2]{};
    int weapons[4]{};
    weapon_swap::LocalSwapLedger ledger;
    for (int player = 0; player < 2; ++player) {
        for (int channel = 0; channel < 2; ++channel) {
            REQUIRE(ledger
                        .record_local(&soldiers[player], player, channel, 0, channel + 1, &weapons[0], &weapons[channel + 1], 100,
                                      {95, 100, 95})
                        .valid());
        }
    }

    ledger.clear_player(0);
    weapon_swap::EpochSnapshot snapshot;
    CHECK_FALSE(ledger.find(&soldiers[0], 0, 0, snapshot));
    CHECK_FALSE(ledger.find(&soldiers[0], 0, 1, snapshot));
    REQUIRE(ledger.find(&soldiers[1], 1, 0, snapshot));
    CHECK(snapshot.final_index == 1);
    REQUIRE(ledger.find(&soldiers[1], 1, 1, snapshot));
    CHECK(snapshot.final_index == 2);

    // Reusing a local slot for another soldier replaces that channel's old inventory ownership.
    const auto replacement = ledger.record_local(&soldiers[0], 1, 1, 0, 3, &weapons[0], &weapons[3], 101, {96, 101, 96});
    REQUIRE(replacement.kind == weapon_swap::RequestKind::New);
    CHECK_FALSE(ledger.find(&soldiers[1], 1, 1, snapshot));
    REQUIRE(ledger.find(&soldiers[0], 1, 1, snapshot));
    CHECK(snapshot.final_index == 3);
}

TEST_CASE("Weapon swap projections expire after the native replay history", "[plugins][weapon_swap]") {
    int soldier{};
    int weapons[2]{};
    weapon_swap::LocalSwapLedger channels;
    weapon_swap::PackedSwapLedger packed;
    REQUIRE(channels.record_local(&soldier, 1, 1, 0, 1, &weapons[0], &weapons[1], 100, {95, 100, 95}).valid());
    REQUIRE(packed.track_projected(&soldier, 1, 1, 1, &weapons[1], 100, 7, 8, {95, 100, 95}));

    channels.observe_frontiers({132, 132, 132});
    packed.observe_frontiers({132, 132, 132});
    CHECK(channels.has_active());
    CHECK(packed.has_active());
    channels.observe_frontiers({133, 133, 133});
    packed.observe_frontiers({133, 133, 133});
    CHECK_FALSE(channels.has_active());
    CHECK_FALSE(packed.has_active());
}

TEST_CASE("Historical weapon replacement abandons both ownership scopes", "[plugins][weapon_swap]") {
    int soldier{};
    int weapons[3]{};
    weapon_swap::LocalSwapLedger channels;
    weapon_swap::PackedSwapLedger packed;
    const auto request = channels.record_local(&soldier, 0, 0, 0, 1, &weapons[0], &weapons[1], 100, {95, 100, 95});
    REQUIRE(request.valid());
    weapon_swap::EpochSnapshot snapshot;
    REQUIRE(channels.find(&soldier, 0, 0, snapshot));
    REQUIRE(packed.track_projected(&soldier, 0, 0, 1, &weapons[1], 100, snapshot.epoch, snapshot.sequence, {95, 100, 95}));

    const auto replay = channels.record_local(&soldier, 0, 0, 0, 1, &weapons[0], &weapons[2], 100, {105, 105, 95});
    CHECK(replay.kind == weapon_swap::RequestKind::HistoricalConflict);
    CHECK(replay.epoch == request.epoch);
    CHECK_FALSE(channels.has_active());
    packed.clear_epoch(&soldier, 0, replay.epoch);
    CHECK_FALSE(packed.has_active());
}

TEST_CASE("Packed weapon ownership ignores stale settlement and clears replaced inventory", "[plugins][weapon_swap]") {
    int soldier{};
    int weapons[4]{};
    weapon_swap::PackedSwapLedger ledger;
    REQUIRE(ledger.track_projected(&soldier, 1, 0, 1, &weapons[1], 100, 7, 8, {95, 100, 95}));
    REQUIRE(ledger.track_projected(&soldier, 1, 1, 2, &weapons[2], 101, 9, 10, {96, 101, 96}));
    ledger.observe_channel_result(1, {weapon_swap::AuthorityKind::Rejected, 7, 8, false, false});
    CHECK(ledger.has_active());
    CHECK_FALSE(ledger.track_projected(&soldier, 1, 0, 1, &weapons[1], 100, 7, 8, {96, 101, 96}));

    const auto replaced = ledger.observe_sync(&soldier, 1, 1, 2, &weapons[3], {97, 102, 97});
    CHECK(replaced.pointer_invalidated);
    CHECK_FALSE(replaced.mute_select);
    CHECK_FALSE(ledger.has_active());
}

TEST_CASE("Weapon select intent mutes one matching call and cancels unrelated calls", "[plugins][weapon_swap]") {
    int weapons[2]{};
    weapon_swap::SelectIntent intent;
    intent.arm(&weapons[0], 1, true);
    CHECK(intent.consume_duplicate(&weapons[0], 1));
    CHECK_FALSE(intent.consume_duplicate(&weapons[0], 1));
    intent.arm(&weapons[0], 1, true);
    CHECK_FALSE(intent.consume_duplicate(&weapons[1], 1));
    CHECK_FALSE(intent.consume_duplicate(&weapons[0], 1));
    intent.arm(&weapons[0], 1, true);
    CHECK_FALSE(intent.consume_duplicate(&weapons[0], 0));
    CHECK_FALSE(intent.consume_duplicate(&weapons[0], 1));
}

TEST_CASE("Automatic secondary selection owns exact replay without a manual slot request", "[plugins][weapon_swap]") {
    int soldier{};
    int weapons[3]{};
    weapon_swap::PackedSwapLedger ledger;

    CHECK_FALSE(ledger.record_activation(&soldier, 0, 0, 0, &weapons[0], 1, 2, &weapons[2], {23, 23, 0}).mute_select);
    REQUIRE(ledger.has_state(&soldier, 0));
    CHECK(ledger.observe_transition(&soldier, 0, 1, 2, &weapons[2], 0, 0, &weapons[0], {16, 25, 15}).mute_select);
    CHECK(ledger.record_activation(&soldier, 0, 0, 0, &weapons[0], 1, 2, &weapons[2], {23, 23, 15}).mute_select);

    SECTION("A later input keeps its first selection sound") {
        CHECK_FALSE(ledger.record_activation(&soldier, 0, 0, 0, &weapons[0], 1, 2, &weapons[2], {26, 26, 16}).mute_select);
        CHECK(ledger.record_activation(&soldier, 0, 0, 0, &weapons[0], 1, 2, &weapons[2], {26, 26, 16}).mute_select);
    }
    SECTION("Authority at the action turn is an audible correction") {
        CHECK_FALSE(ledger.observe_transition(&soldier, 0, 1, 2, &weapons[2], 0, 0, &weapons[0], {23, 25, 22}).mute_select);
        CHECK_FALSE(ledger.has_active());
    }
    SECTION("Different authority endpoints remain audible") {
        CHECK_FALSE(ledger.observe_transition(&soldier, 0, 1, 2, &weapons[2], 0, 1, &weapons[1], {16, 25, 15}).mute_select);
        CHECK_FALSE(ledger.has_active());
    }
    SECTION("A contradictory historical activation abandons ownership") {
        CHECK_FALSE(ledger.record_activation(&soldier, 0, 0, 1, &weapons[1], 1, 2, &weapons[2], {23, 23, 15}).mute_select);
        CHECK_FALSE(ledger.has_active());
    }
    SECTION("Replacing an endpoint prevents replay muting") {
        const auto replaced = ledger.record_activation(&soldier, 0, 0, 0, &weapons[1], 1, 2, &weapons[2], {23, 23, 15});
        CHECK(replaced.pointer_invalidated);
        CHECK_FALSE(replaced.mute_select);
        CHECK_FALSE(ledger.has_active());
    }
    SECTION("Invalidation and the native replay window end ownership") {
        ledger.clear_player(0);
        CHECK_FALSE(ledger.record_activation(&soldier, 0, 0, 0, &weapons[0], 1, 2, &weapons[2], {23, 23, 15}).mute_select);
        ledger.observe_frontiers({56, 56, 55});
        CHECK_FALSE(ledger.has_active());
    }
}

TEST_CASE("Automatic activations at one input turn retain their distinct endpoints", "[plugins][weapon_swap]") {
    int soldier{};
    int weapons[3]{};
    weapon_swap::PackedSwapLedger ledger;
    CHECK_FALSE(ledger.record_activation(&soldier, 0, 0, 0, &weapons[0], 1, 2, &weapons[2], {23, 23, 0}).mute_select);
    CHECK_FALSE(ledger.record_activation(&soldier, 0, 1, 2, &weapons[2], 0, 1, &weapons[1], {23, 23, 0}).mute_select);
    CHECK(ledger.record_activation(&soldier, 0, 0, 0, &weapons[0], 1, 2, &weapons[2], {23, 23, 0}).mute_select);
    CHECK(ledger.record_activation(&soldier, 0, 1, 2, &weapons[2], 0, 1, &weapons[1], {23, 23, 0}).mute_select);

    weapon_swap::PackedSnapshot snapshot;
    REQUIRE(ledger.find(&soldier, 0, snapshot));
    CHECK(snapshot.projected_key == weapon_swap::PackedSwapLedger::key(0, 1));
}

TEST_CASE("Replaying automatic selection preserves a later manual projection", "[plugins][weapon_swap]") {
    int soldier{};
    int weapons[4]{};
    weapon_swap::PackedSwapLedger ledger;
    std::int32_t manual_turn = 24;
    SECTION("Manual selection follows at the next turn") {}
    SECTION("Manual selection follows within the same turn") {
        manual_turn = 23;
    }

    CHECK_FALSE(ledger.record_activation(&soldier, 0, 0, 0, &weapons[0], 1, 2, &weapons[2], {23, 23, 0}).mute_select);
    REQUIRE(ledger.track_projected(&soldier, 0, 1, 3, &weapons[3], manual_turn, 7, 8, {manual_turn, manual_turn, 15}));
    CHECK(ledger.record_activation(&soldier, 0, 0, 0, &weapons[0], 1, 2, &weapons[2], {23, 23, 15}).mute_select);

    weapon_swap::PackedSnapshot snapshot;
    REQUIRE(ledger.find(&soldier, 0, snapshot));
    CHECK(snapshot.projected_key == weapon_swap::PackedSwapLedger::key(1, 3));
    CHECK(snapshot.request_turn == manual_turn);
    CHECK(snapshot.epoch == 7);

    CHECK(ledger.observe_transition(&soldier, 0, 1, 2, &weapons[2], 0, 0, &weapons[0], {16, 25, 15}).mute_select);
    REQUIRE(ledger.resolve(&soldier, 0, 0, 0, &weapons[0], snapshot));
    CHECK(snapshot.projected_key == weapon_swap::PackedSwapLedger::key(1, 3));
    CHECK(snapshot.epoch == 7);
}

TEST_CASE("Same-turn channel replay preserves the later player-wide request", "[plugins][weapon_swap]") {
    int soldier{};
    int weapons[3]{};
    weapon_swap::PackedSwapLedger ledger;
    REQUIRE(ledger.track_projected(&soldier, 0, 0, 1, &weapons[1], 100, 7, 8, {95, 100, 95}));
    REQUIRE(ledger.track_projected(&soldier, 0, 1, 2, &weapons[2], 100, 9, 10, {95, 100, 95}));
    CHECK_FALSE(ledger.track_projected(&soldier, 0, 0, 1, &weapons[1], 100, 7, 8, {95, 100, 95}));

    REQUIRE(ledger.observe_transition(&soldier, 0, 1, 2, &weapons[2], 0, 0, &weapons[0], {96, 101, 96}).mute_select);
    weapon_swap::PackedSnapshot snapshot;
    REQUIRE(ledger.resolve(&soldier, 0, 0, 0, &weapons[0], snapshot));
    CHECK(snapshot.projected_key == weapon_swap::PackedSwapLedger::key(1, 2));
}

TEST_CASE("Same-turn player-wide ordering follows nonzero request sequence wrap", "[plugins][weapon_swap]") {
    int soldier{};
    int weapons[2]{};
    weapon_swap::PackedSwapLedger ledger;
    REQUIRE(ledger.track_projected(&soldier, 1, 0, 0, &weapons[0], 100, 7, 0xFFFFFFFFU, {95, 100, 95}));
    REQUIRE(ledger.track_projected(&soldier, 1, 1, 1, &weapons[1], 100, 8, 1, {95, 100, 95}));
    CHECK_FALSE(ledger.track_projected(&soldier, 1, 0, 0, &weapons[0], 100, 7, 0xFFFFFFFFU, {95, 100, 95}));
}

TEST_CASE("Packed inventory contradiction overrides a known target in the channel history", "[plugins][weapon_swap]") {
    int soldier{};
    int weapons[4]{};
    weapon_swap::LocalSwapLedger channels;
    weapon_swap::PackedSwapLedger packed;
    REQUIRE(channels.record_local(&soldier, 0, 0, 0, 1, &weapons[0], &weapons[1], 100, {95, 100, 95}).valid());
    REQUIRE(packed.track_projected(&soldier, 0, 0, 1, &weapons[1], 100, 7, 8, {95, 100, 95}));
    REQUIRE(packed.observe_transition(&soldier, 0, 0, 1, &weapons[1], 1, 2, &weapons[2], {96, 100, 95}).mute_select);

    const auto decision = packed.observe_transition(&soldier, 0, 1, 2, &weapons[3], 0, 1, &weapons[1], {96, 100, 95});
    REQUIRE(decision.pointer_invalidated);
    const bool known_target = channels.is_known_selection(&soldier, 0, 0, 1, &weapons[1]);
    REQUIRE(known_target);
    CHECK_FALSE(decision.may_mute(known_target));
    CHECK_FALSE(packed.has_active());
}

TEST_CASE("Held weapon switch reads only populated ages one through thirty-one", "[plugins][weapon_swap]") {
    weapon_swap::RecordedMoves moves;
    for (int turn = 68; turn < 100; ++turn) {
        moves.record(0, turn, 7, true);
    }
    CHECK_FALSE(moves.has_pair(0, 7, 100, 100));
    CHECK(moves.has_pair(0, 7, 100, 99));
    CHECK(moves.has_pair(0, 7, 100, 70));
    CHECK(moves.has_pair(0, 7, 100, 69));
    CHECK_FALSE(moves.has_pair(0, 7, 100, 68));
    CHECK_FALSE(moves.has_pair(1, 7, 100, 99));
    CHECK_FALSE(moves.has_pair(0, 8, 100, 99));

    SECTION("A newly assigned slot needs both turns from its new player") {
        moves.record(0, 100, 8, true);
        CHECK_FALSE(moves.has_pair(0, 8, 101, 100));
        moves.record(0, 101, 8, true);
        CHECK(moves.has_pair(0, 8, 102, 101));
    }
    SECTION("Skipping a slot invalidates stale input at the same ring position") {
        moves.record(0, 100, 7, false);
        CHECK_FALSE(moves.has_pair(0, 7, 101, 100));
        moves.record(0, 101, 7, true);
        CHECK_FALSE(moves.has_pair(0, 7, 102, 101));
    }
    SECTION("Resetting one player preserves the other local input slot") {
        moves.record(1, 98, 8, true);
        moves.record(1, 99, 8, true);
        moves.clear_player(0);
        CHECK_FALSE(moves.has_pair(0, 7, 100, 99));
        CHECK(moves.has_pair(1, 8, 100, 99));
        moves.clear();
        CHECK_FALSE(moves.has_pair(1, 8, 100, 99));
    }
}

TEST_CASE("Held weapon switch replay requires the same channel and direction in adjacent moves", "[plugins][weapon_swap]") {
    for (int channel = 0; channel < 2; ++channel) {
        for (const auto direction : {weapon_swap::kPositiveDirection, weapon_swap::kNegativeDirection}) {
            const auto held = channel == 0 ? (direction == weapon_swap::kPositiveDirection ? 0x2000U : 0x4000U)
                                           : (direction == weapon_swap::kPositiveDirection ? 0x8000U : 0x10000U);
            CHECK(weapon_swap::held_switch_edge(channel, 3, direction, held, held));
            CHECK_FALSE(weapon_swap::held_switch_edge(channel, 3, direction, 0, held));
            CHECK_FALSE(weapon_swap::held_switch_edge(channel, 3, direction, held, 0));
            CHECK_FALSE(weapon_swap::held_switch_edge(channel, 1, direction, held, held));
            CHECK_FALSE(weapon_swap::held_switch_edge(1 - channel, 3, direction, held, held));
        }
    }
}

TEST_CASE("Weapon prediction lifecycle detects a zeroed reset after active turns", "[plugins][weapon_swap]") {
    weapon_swap::NetworkLifecycle lifecycle;
    CHECK_FALSE(lifecycle.observe({0, 0, 0}));
    CHECK_FALSE(lifecycle.observe({10, 12, 9}));
    CHECK(lifecycle.active());
    CHECK_FALSE(lifecycle.observe({10, 0, 9}));
    CHECK(lifecycle.observe({0, 0, 0}));
    CHECK_FALSE(lifecycle.active());
    CHECK_FALSE(lifecycle.observe({0, 0, 0}));
}
