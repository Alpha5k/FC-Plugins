#include "../patches/update_scheduling/send_window.hpp"
#include "../patches/update_scheduling/slot_peers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace scheduling = fc_plugins::update_scheduling;

TEST_CASE("Update Scheduling sizes the compatible client window from latency") {
    CHECK(scheduling::PeerSendWindow::capacity(0.05F, 0.0F, 20) == 2);
    CHECK(scheduling::PeerSendWindow::capacity(0.08F, 0.01F, 60) == 6);
    CHECK(scheduling::PeerSendWindow::capacity(0.3F, 0.0F, 60) == 18);
    CHECK(scheduling::PeerSendWindow::capacity(1.0F, 0.0F, 20) == 16);
    CHECK(scheduling::PeerSendWindow::capacity(1.0F, 0.0F, 30) == 16);
    CHECK(scheduling::PeerSendWindow::capacity(1.0F, 0.0F, 31) == 17);
    CHECK(scheduling::PeerSendWindow::capacity(1.0F, 0.0F, 45) == 24);
    CHECK(scheduling::PeerSendWindow::capacity(1.0F, 0.0F, 60) == 32);
    CHECK(scheduling::PeerSendWindow::capacity(1.0F, 0.5F, 90) == 32);
}

TEST_CASE("Update Scheduling bounds a full 60 TPS window through acknowledgement and refill") {
    scheduling::PeerSendWindow window;
    window.synchronize(7);
    for (std::uint32_t turn = 1; turn <= 32; ++turn) {
        const auto now = 1.0F + static_cast<float>(turn - 1) / 60.0F;
        REQUIRE(window.can_send(now, 1.0F, 0.0F, 60));
        window.record(turn, now);
    }

    CHECK(window.count() == 32);
    CHECK_FALSE(window.can_send(1.55F, 1.0F, 0.0F, 60));
    window.record(33, 1.55F);
    CHECK(window.count() == 32);
    CHECK_FALSE(window.sent_at(33));

    window.acknowledge(16);

    CHECK(window.count() == 16);
    for (std::uint32_t turn = 33; turn <= 48; ++turn) {
        const auto now = 1.55F + static_cast<float>(turn - 33) / 60.0F;
        REQUIRE(window.can_send(now, 1.0F, 0.0F, 60));
        window.record(turn, now);
    }

    CHECK(window.count() == 32);
    CHECK(window.sent_at(48));
    CHECK_FALSE(window.can_send(1.85F, 1.0F, 0.0F, 60));
    window.record(49, 1.85F);
    CHECK(window.count() == 32);
    CHECK_FALSE(window.sent_at(49));
}

TEST_CASE("Update Scheduling clears updates through the acknowledged turn") {
    scheduling::PeerSendWindow window;
    window.synchronize(4);
    window.record(20, 1.0F);
    window.record(21, 1.01F);
    window.record(22, 1.02F);

    window.acknowledge(21);

    CHECK(window.count() == 1);
}

TEST_CASE("Update Scheduling reopens a stalled compatible client window") {
    scheduling::PeerSendWindow window;
    window.synchronize(7);
    window.record(20, 1.0F);
    window.record(21, 1.01F);

    CHECK_FALSE(window.can_send(1.05F, 0.05F, 0.0F, 20));
    CHECK(window.can_send(1.12F, 0.05F, 0.0F, 20));
}

TEST_CASE("Update Scheduling resets state when a client slot is reused") {
    scheduling::PeerSendWindow window;
    window.synchronize(0x0000000100000002ULL);
    window.record(20, 1.0F);

    // The upper identity bits distinguish reused connections even when their low 32 bits match.
    window.synchronize(0x0000000200000002ULL);

    CHECK(window.peer_identity() == 0x0000000200000002ULL);
    CHECK(window.count() == 0);
}

TEST_CASE("Update Scheduling resets map timestamps while preserving the connection") {
    scheduling::PeerSendWindow window;
    window.synchronize(7);
    window.record(100, 5.0F);
    window.record(101, 5.01F);

    window.synchronize(7);
    window.acknowledge(1);

    CHECK(window.count() == 2);
    CHECK_FALSE(window.can_send(0.1F, 0.05F, 0.0F, 20));

    window.reset_world();

    CHECK(window.peer_identity() == 7);
    CHECK(window.count() == 0);
    CHECK_FALSE(window.sent_at(100));
    CHECK(window.can_send(0.1F, 0.05F, 0.0F, 20));
    window.record(1, 0.1F);
    window.acknowledge(1);
    CHECK(window.count() == 0);
}

TEST_CASE("Update Scheduling samples only the exact pending acknowledged turn") {
    scheduling::PeerSendWindow window;
    window.synchronize(7);
    window.record(20, 1.0F);
    window.record(21, 1.01F);
    window.record(22, 1.02F);

    CHECK(window.sent_at(22) == 1.02F);
    CHECK_FALSE(window.sent_at(23));
    CHECK(window.count() == 3);

    window.acknowledge(22);

    CHECK(window.count() == 0);
    CHECK_FALSE(window.sent_at(20));
    CHECK_FALSE(window.sent_at(22));
}

TEST_CASE("Update Scheduling finds exact timestamps after the ring wraps and expires") {
    scheduling::PeerSendWindow window;
    window.synchronize(7);
    window.record(20, 1.0F);
    window.record(21, 1.01F);
    window.acknowledge(20);
    for (std::uint32_t turn = 22; turn <= 52; ++turn) {
        window.record(turn, 1.02F);
    }

    CHECK(window.count() == 32);
    CHECK(window.sent_at(21) == 1.01F);
    CHECK(window.sent_at(52) == 1.02F);
    CHECK_FALSE(window.sent_at(20));
    CHECK(window.can_send(1.2F, 0.05F, 0.0F, 20));
    CHECK_FALSE(window.sent_at(52));
}

TEST_CASE("Update Scheduling preserves the current slot after an older peer ends") {
    scheduling::SlotPeers<64> peers;
    constexpr std::uint64_t old_peer = 0x0000000100000007ULL;
    constexpr std::uint64_t new_peer = 0x0000000200000007ULL;
    peers.publish(0, old_peer);
    peers.publish(0, new_peer);

    peers.withdraw(old_peer);

    CHECK(peers.current(0) == new_peer);
    peers.withdraw(new_peer);
    CHECK(peers.current(0) == 0);
}

TEST_CASE("Update Scheduling releases acknowledged sends across turn wrap") {
    scheduling::PeerSendWindow window;
    window.synchronize(1);
    window.record(0xFFFFFFFEU, 1.0F);
    window.record(0xFFFFFFFFU, 1.01F);
    window.record(0, 1.02F);
    window.record(1, 1.03F);

    window.acknowledge(0);

    CHECK(window.count() == 1);
}
