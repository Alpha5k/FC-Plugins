#include "../patches/network_diagnostics/progress.hpp"

#include <catch2/catch_test_macros.hpp>

namespace network = fc_plugins::network_diagnostics;

TEST_CASE("Network diagnostics combines empty passes and keeps consecutive service gaps", "[network_diagnostics]") {
    network::trace::ClientReceivePassRecord summary{};
    std::uint64_t last_entry{};
    const network::trace::ClientReceivePassRecord first{.turn_before = 42,
                                                        .turn_after = 42,
                                                        .duration = 3,
                                                        .first_pass = 7,
                                                        .last_pass = 7,
                                                        .calls = 1,
                                                        .entered_qpc = 100,
                                                        .returned_qpc = 102,
                                                        .maximum_duration = 3};
    REQUIRE(network::append_empty_receive(summary, last_entry, first));
    auto next = first;
    next.first_pass = next.last_pass = 8;
    next.entered_qpc = 120;
    next.returned_qpc = 123;
    next.duration = 9;
    REQUIRE(network::append_empty_receive(summary, last_entry, next));
    next.first_pass = next.last_pass = 9;
    next.entered_qpc = 130;
    next.returned_qpc = 131;
    next.duration = 2;
    REQUIRE(network::append_empty_receive(summary, last_entry, next));
    CHECK(summary.calls == 3);
    CHECK(summary.first_pass == 7);
    CHECK(summary.last_pass == 9);
    CHECK(summary.entered_qpc == 100);
    CHECK(summary.returned_qpc == 131);
    CHECK(summary.maximum_gap == 20);
    CHECK(summary.duration == 14);
    CHECK(summary.maximum_duration == 9);

    SECTION("A parsed group stays outside the empty interval") {
        next.groups = 1;
        CHECK_FALSE(network::append_empty_receive(summary, last_entry, next));
        CHECK(summary.calls == 3);
    }
    SECTION("A changed frontier starts another interval") {
        next.turn_before = next.turn_after = 43;
        CHECK_FALSE(network::append_empty_receive(summary, last_entry, next));
        CHECK(summary.turn_after == 42);
    }
    SECTION("Nested completion preserves bounds without an unsigned gap") {
        next.entered_qpc = 90;
        next.returned_qpc = 150;
        REQUIRE(network::append_empty_receive(summary, last_entry, next));
        CHECK(summary.entered_qpc == 90);
        CHECK(summary.returned_qpc == 150);
        CHECK(summary.maximum_gap == 20);
        next.entered_qpc = 140;
        next.returned_qpc = 145;
        REQUIRE(network::append_empty_receive(summary, last_entry, next));
        CHECK(summary.maximum_gap == 20);
    }
}
