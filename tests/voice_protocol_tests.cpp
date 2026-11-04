#include "../patches/voice_chat/state.hpp"
#include "../patches/voice_chat/microphone_test.hpp"
#include "../patches/voice_chat/transport.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>
#include <array>
#include <span>

namespace voice = fc_plugins::voice_chat;

TEST_CASE("Voice routing allows playback without capture and preserves saved policy", "[voice]") {
    const auto playback = voice::local_routing_flags(true, false, true);
    CHECK((playback & (voice::Talk | voice::Listen)) == (voice::Talk | voice::Listen));
    CHECK_FALSE(voice::can_send_voice(playback));
    CHECK(voice::can_receive_voice(playback));

    const auto capture = voice::local_routing_flags(true, true, false);
    CHECK(voice::can_send_voice(capture));
    CHECK_FALSE(voice::can_receive_voice(capture));

    const auto failed = voice::local_routing_flags(true, false, false);
    CHECK(failed == (voice::Talk | voice::Listen));
    CHECK_FALSE(voice::can_send_voice(failed));
    CHECK_FALSE(voice::can_receive_voice(failed));

    const auto disabled = voice::local_routing_flags(false, true, true);
    CHECK_FALSE(voice::can_send_voice(disabled));
    CHECK_FALSE(voice::can_receive_voice(disabled));

    // Dedicated relay capability remains independent of a player row's physical devices.
    CHECK(voice::can_send_voice(voice::kRoutingFlags));
    CHECK(voice::can_receive_voice(voice::kRoutingFlags));
}

TEST_CASE("Each selected native voice queue receives recovery followed by the current frame", "[voice]") {
    voice::NativeFrame frame{};
    const std::array payload{std::byte{0x32}, std::byte{0x47}};
    REQUIRE(voice::build_frame(frame, payload));
    std::uint32_t app_data{};
    voice::AudioPacket current{
        .data = frame.data(), .size = static_cast<std::uint32_t>(frame.size()), .app_data = &app_data, .discontinuity = 1};
    const auto missing = GENERATE(std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{3}, std::uint16_t{4}, std::uint16_t{5});
    for (int local = 0; local < 2; ++local) {
        std::array<voice::NativeFrame, 5> copies{};
        std::array<std::uint8_t, 5> discontinuities{};
        std::array<std::uint32_t, 5> sizes{};
        std::size_t count{};
        const auto accepted = voice::enqueue_recovered_audio(current, missing, [&](voice::AudioPacket &packet) noexcept {
            CHECK(packet.app_data == &app_data);
            sizes[count] = packet.size;
            discontinuities[count] = packet.discontinuity;
            if (packet.size != 0) {
                std::copy_n(static_cast<const std::byte *>(packet.data), packet.size, copies[count].begin());
            }
            ++count;
            // A refused recovery copy must still leave the current frame to native queue handling.
            return packet.data == current.data;
        });
        CHECK(accepted);
        REQUIRE(count == (missing > 4 ? std::size_t{2} : static_cast<std::size_t>(missing) + 1));
        CHECK(copies[count - 1] == frame);
        CHECK(discontinuities[count - 1] == 1);
        if (missing > 4) {
            CHECK(sizes[0] == 0);
            CHECK(discontinuities[0] == 1);
        } else if (missing != 0) {
            for (std::size_t index = 0; index + 1 < count; ++index) {
                const auto parsed = voice::parse_native_frame(copies[index]);
                REQUIRE(parsed);
                CHECK(parsed->mode == (index + 2 == count ? voice::FrameMode::Fec : voice::FrameMode::Plc));
                CHECK(discontinuities[index] == 0);
            }
        }
    }
    CHECK(current.data == frame.data());
    CHECK(current.app_data == &app_data);
    CHECK(current.discontinuity == 1);
}

TEST_CASE("Microphone tests use local recipient delivery and restore multiplayer relay", "[voice]") {
    CHECK(voice::MicrophoneTestRoute::method(4) == 4);
    CHECK_FALSE(voice::MicrophoneTestRoute::captured());
    CHECK(voice::MicrophoneTestRoute::network_delivery(true, 3));
    {
        voice::MicrophoneTestRoute test{3, true};
        CHECK(voice::MicrophoneTestRoute::captured());
        CHECK(voice::MicrophoneTestRoute::method(4) == 2);
        CHECK(voice::MicrophoneTestRoute::method(3) == 2);
        CHECK_FALSE(voice::MicrophoneTestRoute::network_delivery(false, 3));
        CHECK_FALSE(voice::MicrophoneTestRoute::network_delivery(true, 3));
        CHECK(voice::MicrophoneTestRoute::network_delivery(true, 8));
        CHECK(voice::MicrophoneTestRoute::permits_send(3, 3));
        CHECK_FALSE(voice::MicrophoneTestRoute::permits_send(3, 8));
        CHECK(voice::MicrophoneTestRoute::permits_send(8, 3));
        {
            voice::MicrophoneTestRoute other_send{8, false};
            CHECK(voice::MicrophoneTestRoute::method(4) == 4);
            CHECK(voice::MicrophoneTestRoute::network_delivery(true, 3));
            CHECK(voice::MicrophoneTestRoute::permits_send(3, 8));
        }
        CHECK(voice::MicrophoneTestRoute::method(4) == 2);
        CHECK_FALSE(voice::MicrophoneTestRoute::network_delivery(true, 3));
    }
    // Relay calls outside the captured send retain the current method and network destination.
    CHECK_FALSE(voice::MicrophoneTestRoute::captured());
    CHECK(voice::MicrophoneTestRoute::method(4) == 4);
    CHECK(voice::MicrophoneTestRoute::method(3) == 3);
    CHECK(voice::MicrophoneTestRoute::network_delivery(true, 3));
    CHECK(voice::MicrophoneTestRoute::network_delivery(true, 0xFF));
    CHECK(voice::MicrophoneTestRoute::permits_send(3, 8));
    voice::MicrophoneTestRoute no_player{0xFF, true};
    CHECK(voice::MicrophoneTestRoute::method(4) == 4);
    CHECK(voice::MicrophoneTestRoute::network_delivery(true, 0xFF));
}

TEST_CASE("Voice chat frames fit the native and wire packet limits") {
    std::array<std::byte, 120> payload{};
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::byte>(index);
    }

    voice::NativeFrame native{};
    REQUIRE(voice::build_frame(native, payload));
    const auto size = voice::wire_size(native);
    REQUIRE(size == 132U);

    // A received aligned prefix must reconstruct the exact internal frame that
    // the six-call native decoder bridge consumes.
    const auto wire = std::span<const std::byte>{native}.first(*size);
    const auto parsed = voice::parse_wire_frame(wire);
    REQUIRE(parsed.has_value());
    CHECK(parsed->mode == voice::FrameMode::Normal);
    CHECK(std::ranges::equal(parsed->payload, payload));

    voice::NativeFrame expanded{};
    REQUIRE(voice::expand_wire_frame(wire, expanded));
    CHECK(std::ranges::equal(expanded, native));

    // Recovery modes are local-only, so a nonzero wire flag must be rejected.
    auto malformed = native;
    malformed[5] = std::byte{0x7F};
    CHECK_FALSE(voice::parse_wire_frame(std::span<const std::byte>{malformed}.first(*size)));

    const auto recovery = voice::fec_frame(native);
    REQUIRE(recovery.has_value());
    const auto recovered = voice::parse_native_frame(*recovery);
    REQUIRE(recovered.has_value());
    CHECK(recovered->mode == voice::FrameMode::Fec);
    const auto concealment = voice::plc_frame();
    const auto concealed = voice::parse_native_frame(concealment);
    REQUIRE(concealed.has_value());
    CHECK(concealed->mode == voice::FrameMode::Plc);

    std::array<std::byte, voice::kMaximumPayloadBytes> maximum_payload{};
    REQUIRE(voice::build_frame(native, maximum_payload));
    CHECK(voice::wire_size(native) == voice::kMaximumWireBytes);
}

TEST_CASE("Voice state records reject unknown flags and versions", "[voice]") {
    const voice::StateMessage state{
        .kind = voice::StateKind::ClientSnapshot,
        .flags = voice::kRoutingFlags,
        .player = 3,
        .revision = 7,
    };
    const auto parsed = voice::parse_state(voice::bytes(state));
    REQUIRE(parsed);
    CHECK(parsed->header.revision == 7);
    CHECK(parsed->header.player == 3);

    auto invalid = state;
    invalid.flags = 0x80;
    CHECK_FALSE(voice::parse_state(voice::bytes(invalid)));
    invalid = state;
    invalid.version = 0xFF;
    CHECK_FALSE(voice::parse_state(voice::bytes(invalid)));
}

TEST_CASE("Voice host snapshots replace a bounded player-state table", "[voice]") {
    voice::HostSnapshot snapshot{
        .header =
            {
                .kind = voice::StateKind::HostSnapshot,
                .flags = voice::kRoutingFlags,
                .player = 2,
                .revision = 9,
            },
    };
    snapshot.players[0] = {.player = 4, .flags = voice::kRoutingFlags, .identity = 0x100000004};
    snapshot.players[1] = {
        .player = 12,
        .flags = static_cast<std::uint8_t>(voice::kRoutingFlags | voice::Speaking),
        .identity = 0x20000000C,
    };

    const auto parsed = voice::parse_state(voice::bytes(snapshot));
    REQUIRE(parsed);
    CHECK(parsed->count == 2);
    CHECK(parsed->players[0].player == 4);
    CHECK((parsed->players[1].flags & voice::Speaking) != 0);
    CHECK(parsed->players[0].identity == 0x100000004);
    CHECK(parsed->players[1].identity == 0x20000000C);
    CHECK(voice::bytes(snapshot).size() == 36);

    snapshot.players[1].player = 4;
    CHECK_FALSE(voice::parse_state(voice::bytes(snapshot)));
    snapshot.players[1].player = 12;
    snapshot.players[1].identity = 0;
    CHECK_FALSE(voice::parse_state(voice::bytes(snapshot)));
}

TEST_CASE("Voice ownership survives unchanged claims and distinguishes occupied disabled slots", "[voice]") {
    voice::ServerPeers peers;
    const voice::PeerBinding first{.id = {0x10000002A}, .slot = 3};
    REQUIRE(peers.begin(first));
    REQUIRE(peers.update(first, 3, 0, 7, true));
    std::array<voice::PlayerState, voice::kMaximumPlayers> rows{};
    REQUIRE(peers.copy_players(rows) == 1);
    CHECK(rows[0].player == 3);
    CHECK(rows[0].flags == 0);
    CHECK(rows[0].identity == first.id.value);
    REQUIRE(peers.begin(first));
    CHECK(peers.identity(3) == first.id.value);
    const auto repeated = peers.update(first, 3, 0, 7, false);
    REQUIRE(repeated);
    CHECK_FALSE(repeated->changed);

    const voice::PeerBinding replacement{.id = {0x20000002A}, .slot = 3};
    REQUIRE(peers.begin(replacement));
    CHECK(peers.identity(3) == replacement.id.value);
    REQUIRE(peers.copy_players(rows) == 1);
    CHECK(rows[0].identity == replacement.id.value);
    CHECK_FALSE(peers.clear(first));
    REQUIRE(peers.clear(replacement));
    CHECK(peers.identity(3) == 0);
    CHECK(peers.copy_players(rows) == 0);
}

TEST_CASE("Voice control records carry full connection identities", "[voice]") {
    const voice::StateMessage delta{
        .kind = voice::StateKind::Delta, .flags = voice::kRoutingFlags, .player = 3, .revision = 9, .identity = 0xFEDCBA9876543210};
    const auto encoded = voice::bytes(delta);
    REQUIRE(encoded.size() == 16);
    CHECK(encoded[1] == std::byte{2});
    CHECK(encoded[8] == std::byte{0x10});
    CHECK(encoded[15] == std::byte{0xFE});
    const auto decoded = voice::parse_state(encoded);
    REQUIRE(decoded);
    CHECK(decoded->header.identity == delta.identity);
    CHECK_FALSE(voice::parse_state(encoded.first(8)));
}

TEST_CASE("Voice resynchronization records have no player state", "[voice]") {
    voice::StateMessage resync{
        .kind = voice::StateKind::Resync,
        .player = voice::kInvalidPlayer,
        .revision = 3,
    };
    REQUIRE(voice::parse_state(voice::bytes(resync)));

    resync.player = 0;
    CHECK_FALSE(voice::parse_state(voice::bytes(resync)));
    resync.player = voice::kInvalidPlayer;
    resync.flags = voice::Talk;
    CHECK_FALSE(voice::parse_state(voice::bytes(resync)));
}

TEST_CASE("Voice claims require the qualified native connection and full FC identity", "[voice]") {
    SECTION("A zero-flags snapshot establishes the row and retains its accepted revision") {
        voice::ServerPeers peers;
        const voice::PeerBinding peer{.id = {0x10000002A}, .slot = 3};
        REQUIRE(peers.begin(peer));
        CHECK_FALSE(peers.update(peer, 3, 0, 7, false));
        const auto accepted = peers.update(peer, 3, 0, 7, true);
        REQUIRE(accepted);
        CHECK(accepted->changed);
        const auto duplicate = peers.update(peer, 3, 0, 7, false);
        REQUIRE(duplicate);
        CHECK_FALSE(duplicate->changed);
        CHECK_FALSE(peers.update(peer, 3, voice::Talk, 7, false));
        CHECK_FALSE(peers.update(peer, 3, 0, 6, false));
        CHECK(peers.clear(peer) == 3);
    }

    voice::ServerPeers peers;
    const voice::PeerBinding first{.id = {0x10000002A}, .slot = 3};
    REQUIRE(peers.begin(first));
    CHECK_FALSE(peers.update(first, 3, voice::kRoutingFlags, 1, false));
    CHECK_FALSE(peers.update(first, 5, voice::kRoutingFlags, 1, true));
    REQUIRE(peers.update(first, 3, voice::kRoutingFlags, 1, true));
    CHECK(peers.flags(3) == voice::kRoutingFlags);
    CHECK_FALSE(peers.update(first, 3, voice::Talk, 1, false));
    CHECK_FALSE(peers.update(first, 3, voice::Talk, 0, false));

    // Reusing a native slot invalidates old FC identities, including IDs sharing the same low 32 bits.
    const voice::PeerBinding replacement{.id = {0x20000002A}, .slot = 3};
    const auto started = peers.begin(replacement);
    REQUIRE(started);
    CHECK(started->previous == 3);
    CHECK(peers.flags(3) == 0);
    CHECK_FALSE(peers.update(first, 3, voice::kRoutingFlags, 2, true));
    REQUIRE(peers.update(replacement, 3, voice::kRoutingFlags, 1, true));
    CHECK_FALSE(peers.clear(first));
    CHECK(peers.flags(3) == voice::kRoutingFlags);
    REQUIRE(peers.find(replacement.id));
    CHECK_FALSE(peers.find(first.id));
    peers.reset();
    CHECK(peers.flags(3) == 0);
    CHECK_FALSE(peers.matches(replacement));
    CHECK_FALSE(peers.begin({.id = {42}, .slot = voice::kHostSlot}));
}

TEST_CASE("Voice replacement snapshots remain pending until an accepted submission", "[voice]") {
    voice::ServerPeers peers;
    const voice::PeerBinding peer{.id = {42}, .slot = 9};
    REQUIRE(peers.begin(peer));
    REQUIRE(peers.update(peer, 9, voice::kRoutingFlags, 1, true));
    peers.require_snapshot(9);
    bool pending{};
    peers.for_each([&](auto, bool snapshot) noexcept {
        pending = snapshot;
    });
    CHECK(pending);
    peers.snapshot_sent(9);
    peers.for_each([&](auto, bool snapshot) noexcept {
        pending = snapshot;
    });
    CHECK_FALSE(pending);

    // A new native connection identity must start from a client snapshot again.
    const voice::PeerBinding replacement{.id = {43}, .slot = 9};
    REQUIRE(peers.begin(replacement));
    CHECK(peers.flags(9) == 0);
    CHECK_FALSE(peers.update(replacement, 9, voice::kRoutingFlags, 2, false));
    CHECK(peers.update(replacement, 9, voice::kRoutingFlags, 2, true));
}

TEST_CASE("Voice frames reject trailing bytes, recovery wire modes and invalid payload lengths", "[voice]") {
    voice::NativeFrame frame{};
    std::array<std::byte, voice::kMaximumPayloadBytes + 1> oversized{};
    CHECK_FALSE(voice::build_frame(frame, oversized));
    CHECK_FALSE(voice::build_frame(frame, {}));
    const std::array payload{std::byte{1}, std::byte{2}};
    REQUIRE(voice::build_frame(frame, payload));
    const auto size = voice::wire_size(frame);
    REQUIRE(size);
    frame[*size - 1] = std::byte{1};
    CHECK_FALSE(voice::parse_wire_frame(std::span{frame}.first(*size)));
    REQUIRE(voice::build_frame(frame, payload));
    CHECK_FALSE(voice::parse_wire_frame(std::span{frame}.first(*size - 1)));
    frame[4] = std::byte{2};
    CHECK_FALSE(voice::parse_native_frame(frame));
}
