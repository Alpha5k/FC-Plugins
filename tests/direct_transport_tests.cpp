#include "../patches/direct_transport/association.hpp"
#include "../patches/direct_transport/protocol.hpp"
#include "../patches/direct_transport/security.hpp"
#include "../patches/direct_transport/socket.hpp"
#include "../patches/direct_transport/receive_diagnostics.hpp"
#include "../patches/direct_transport/control.hpp"
#include "../patches/direct_transport/game.hpp"
#include "../patches/direct_transport/server/groups.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace {

namespace direct = fc_plugins::direct_transport;

constexpr std::uint32_t kHalfSequenceRange = std::uint32_t{1} << 31;

struct SequentialRandom {
    std::uint8_t next{1};
};

// Supplies increasing bytes so connection-ID tests can force and observe collisions.
bool fill_sequence(void *context, std::uint8_t *output, std::size_t bytes) noexcept {
    auto &source = *static_cast<SequentialRandom *>(context);
    for (std::size_t index = 0; index < bytes; ++index) {
        output[index] = source.next++;
    }
    return true;
}

TEST_CASE("Direct Transport encodes and validates setup and UDP messages", "[direct_transport]") {
    direct::SessionKey key{};
    for (std::uint8_t index = 0; index < key.size(); ++index) {
        key[index] = index;
    }
    std::array<std::uint8_t, direct::kOfferBytes> offer{};
    REQUIRE(direct::write_offer(offer, 0x99AABBCC, 0x04030201, 3658, key));
    constexpr std::array<std::uint8_t, direct::kOfferBytes> expected_offer{
        1, 0x99, 0xAA, 0xBB, 0xCC, 1, 2, 3, 4, 0x0E, 0x4A, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    };
    CHECK(offer == expected_offer);
    direct::ParsedControl parsed{};
    REQUIRE(direct::parse_control(offer, parsed) == direct::ParseStatus::Ok);
    CHECK(parsed.kind == direct::ControlKind::Offer);
    CHECK(parsed.connection_id == 0x99AABBCC);
    CHECK(parsed.server_ipv4_network_order == 0x04030201);
    CHECK(parsed.server_port == 3658);
    CHECK(parsed.session_key == key);

    // Ready and Commit share the activation shape; the parser must still retain their message kind.
    std::array<std::uint8_t, direct::kActivationBytes> ready{};
    REQUIRE(direct::write_activation(ready, direct::ControlKind::Ready, 0xDEADBEEF));
    REQUIRE(direct::parse_control(ready, parsed) == direct::ParseStatus::Ok);
    CHECK(parsed.kind == direct::ControlKind::Ready);
    CHECK(parsed.connection_id == 0xDEADBEEF);

    // Multiplayer owns framing and version matching; this parser validates the patch-owned payload.
    auto malformed = offer;
    malformed[0] = 0;
    CHECK(direct::parse_control(malformed, parsed) == direct::ParseStatus::UnknownKind);
    malformed = offer;
    CHECK(direct::parse_control(std::span<const std::uint8_t>(malformed).first(malformed.size() - 1), parsed) ==
          direct::ParseStatus::BadLength);
    CHECK(direct::parse_control({}, parsed) == direct::ParseStatus::TooShort);

    // Data carries a native packet; proof packets use a fixed-size zero payload to prove full datagram delivery.
    std::array<std::uint8_t, direct::kMaximumDirectDatagramBytes> datagram{};
    REQUIRE(direct::write_direct_header(datagram, direct::DirectKind::Data, 0x12345678, 0x90ABCDEF, 0x0102030405060708ULL));
    const std::array<std::uint8_t, direct::kNativeHeaderBytes> payload{0x0B, 1, 2, 3, 4};
    std::ranges::copy(payload, datagram.begin() + direct::kDirectHeaderBytes);
    direct::ParsedDirect direct_packet{};
    const auto data = std::span<const std::uint8_t>(datagram).first(direct::kDirectHeaderBytes + payload.size());
    REQUIRE(direct::parse_direct(data, direct_packet) == direct::ParseStatus::Ok);
    CHECK(direct_packet.kind == direct::DirectKind::Data);
    CHECK(direct_packet.connection_id == 0x12345678);
    CHECK(direct_packet.datagram_sequence == 0x90ABCDEF);
    CHECK(std::ranges::equal(direct_packet.payload, payload));

    datagram = {};
    REQUIRE(direct::write_direct_header(datagram, direct::DirectKind::Probe, 7, 11, 0));
    auto proof = std::span<std::uint8_t>(datagram).first(direct::kProbeDatagramBytes);
    REQUIRE(direct::parse_direct(proof, direct_packet) == direct::ParseStatus::Ok);
    CHECK(direct_packet.kind == direct::DirectKind::Probe);
    proof.back() = 1;
    CHECK(direct::parse_direct(proof, direct_packet) == direct::ParseStatus::InvalidPayload);
}

TEST_CASE("Direct Transport matches SipHash vectors and separates packet directions", "[direct_transport]") {
    // These published SipHash-2-4 vectors catch algorithm and byte-order drift independently of our packet format.
    direct::SessionKey key{};
    std::array<std::uint8_t, 64> message{};
    for (std::uint8_t index = 0; index < key.size(); ++index) {
        key[index] = index;
    }
    for (std::uint8_t index = 0; index < message.size(); ++index) {
        message[index] = index;
    }
    CHECK(direct::sip_hash24(key, std::span(message).first(0)) == 0x726FDB47DD0E0E31ULL);
    CHECK(direct::sip_hash24(key, std::span(message).first(15)) == 0xA129CA6149BE45E5ULL);
    CHECK(direct::sip_hash24(key, std::span(message).first(63)) == 0x958A324CEB064572ULL);

    // Direction is authenticated domain data, so reflecting a valid datagram cannot validate in reverse.
    std::array<std::uint8_t, direct::kDirectHeaderBytes> header{};
    REQUIRE(direct::write_direct_header(header, direct::DirectKind::Data, 7, 11, 0));
    const std::array<std::uint8_t, direct::kNativeHeaderBytes> payload{1, 2, 3, 4, 5};
    const auto client = direct::compute_authentication_tag(direct::Direction::ClientToServer, key, header, payload);
    const auto server = direct::compute_authentication_tag(direct::Direction::ServerToClient, key, header, payload);
    CHECK(client != server);
    direct::store_big_endian64(header.data() + offsetof(direct::DirectHeaderV1, authentication_tag), client);
    CHECK(direct::verify_authentication_tag(direct::Direction::ClientToServer, key, header, payload));
    CHECK_FALSE(direct::verify_authentication_tag(direct::Direction::ServerToClient, key, header, payload));
}

TEST_CASE("Direct Transport replay admission handles reordering and wraparound", "[direct_transport]") {
    direct::ReplayWindow replay;
    // The retained window accepts unseen reordering but rejects duplicates, old values, and an unorderable distance.
    CHECK(replay.admit(100) == direct::ReplayResult::AcceptedFirst);
    CHECK(replay.admit(102) == direct::ReplayResult::AcceptedNew);
    CHECK(replay.admit(101) == direct::ReplayResult::AcceptedReordered);
    CHECK(replay.admit(101) == direct::ReplayResult::Duplicate);
    CHECK(replay.admit(102U - direct::ReplayWindow::kWidth) == direct::ReplayResult::Stale);
    CHECK(replay.admit(102U + kHalfSequenceRange) == direct::ReplayResult::InvalidJump);

    // Zero follows the largest 32-bit sequence when the counter wraps.
    replay.reset();
    constexpr auto maximum_sequence = (std::numeric_limits<std::uint32_t>::max)();
    CHECK(replay.admit(maximum_sequence - 1) == direct::ReplayResult::AcceptedFirst);
    CHECK(replay.admit(maximum_sequence) == direct::ReplayResult::AcceptedNew);
    CHECK(replay.admit(0) == direct::ReplayResult::AcceptedNew);

    // Retained sequences cross word boundaries and leave the window at age 256.
    for (const auto distance : {1U, 63U, 64U, 65U, 127U, 128U, 255U, 256U, 257U}) {
        replay.reset();
        REQUIRE(replay.admit(1000) == direct::ReplayResult::AcceptedFirst);
        REQUIRE(replay.admit(1001) == direct::ReplayResult::AcceptedNew);
        REQUIRE(replay.admit(1065) == direct::ReplayResult::AcceptedNew);
        REQUIRE(replay.admit(1065 + distance) == direct::ReplayResult::AcceptedNew);
        CHECK(replay.admit(1065) == (distance < 256 ? direct::ReplayResult::Duplicate : direct::ReplayResult::Stale));
        CHECK(replay.admit(1001) == (distance + 64 < 256 ? direct::ReplayResult::Duplicate : direct::ReplayResult::Stale));
        CHECK(replay.admit(1000) == (distance + 65 < 256 ? direct::ReplayResult::Duplicate : direct::ReplayResult::Stale));
        CHECK(replay.admit(1033) == (distance + 32 < 256 ? direct::ReplayResult::AcceptedReordered : direct::ReplayResult::Stale));
    }
}

TEST_CASE("Direct Transport leaves datagrams beyond the receive budget for the next service", "[direct_transport]") {
    direct::UdpSocket sender;
    direct::UdpSocket receiver;
    std::int32_t error{};
    std::int32_t buffer_error{};
    REQUIRE(sender.start(0, error, buffer_error));
    REQUIRE(receiver.start(0, error, buffer_error));
    const direct::Endpoint destination{0x0100007F, receiver.bound_port()};
    constexpr std::uint8_t datagrams = 83;
    for (std::uint8_t sequence = 0; sequence < datagrams; ++sequence) {
        const std::array payload{sequence};
        REQUIRE(sender.send(payload, destination).succeeded());
    }

    std::array<std::uint8_t, 1> payload{};
    std::array<bool, datagrams> seen{};
    std::uint32_t handled{};
    const auto receive = [&](direct::Endpoint &source) noexcept {
        return receiver.receive(payload, source);
    };
    const auto handle = [&](std::int32_t bytes, const direct::Endpoint &source) {
        REQUIRE(bytes == 1);
        REQUIRE(source.port_host_order == sender.bound_port());
        REQUIRE(payload[0] < seen.size());
        CHECK_FALSE(seen[payload[0]]);
        seen[payload[0]] = true;
        ++handled;
    };

    const auto first = direct::drain_datagrams(receive, handle);
    CHECK(handled == 80);
    CHECK(first.discarded == 0);
    CHECK(first.error == 0);
    CHECK(first.limit_reached);
    CHECK(first.reason == direct::DatagramDrainEnd::Budget);

    const auto second = direct::drain_datagrams(receive, handle);
    CHECK(handled == datagrams);
    CHECK(second.discarded == 0);
    CHECK(second.error == 0);
    CHECK_FALSE(second.limit_reached);
    CHECK(second.reason == direct::DatagramDrainEnd::Empty);
    CHECK(std::ranges::all_of(seen, [](bool received) {
        return received;
    }));
}

TEST_CASE("Direct Transport bounds receive attempts including oversized datagrams", "[direct_transport]") {
    std::uint32_t reads{};
    std::uint32_t handled{};
    // Oversized datagrams consume native socket work without reaching the parser.
    const auto drained = direct::drain_datagrams(
        [&reads](direct::Endpoint &) noexcept {
            ++reads;
            return direct::SocketResult{-1, direct::kSocketMessageTooLarge};
        },
        [&handled](std::int32_t, const direct::Endpoint &) noexcept {
            ++handled;
        });

    CHECK(reads == 80);
    CHECK(handled == 0);
    CHECK(drained.error == 0);
    CHECK(drained.limit_reached);
    CHECK(drained.reason == direct::DatagramDrainEnd::Budget);

    // Every successful receive within the same attempt budget reaches the parser.
    reads = 0;
    handled = 0;
    const auto saturated = direct::drain_datagrams(
        [&reads](direct::Endpoint &) noexcept {
            ++reads;
            return direct::SocketResult{5, 0};
        },
        [&handled](std::int32_t, const direct::Endpoint &) noexcept {
            ++handled;
        });

    CHECK(reads == 80);
    CHECK(handled == 80);
    CHECK(saturated.discarded == 0);
    CHECK(saturated.limit_reached);
}

TEST_CASE("Direct Transport times socket receives and reports effective buffers without changing delivery", "[direct_transport]") {
    direct::UdpSocket sender;
    direct::UdpSocket receiver;
    std::int32_t error{};
    std::int32_t buffer_error{};
    REQUIRE(sender.start(0, error, buffer_error));
    REQUIRE(receiver.start(0, error, buffer_error));
    const auto &configuration = receiver.capture_configuration();
    CHECK(configuration.socket_id != 0);
    CHECK(configuration.bound_port == receiver.bound_port());
    CHECK(configuration.requested_send_bytes == 64 * 1024);
    CHECK(configuration.requested_receive_bytes == 32 * 1024);
    REQUIRE(configuration.send_set_error == 0);
    REQUIRE(configuration.receive_set_error == 0);
    REQUIRE(configuration.send_read_error == 0);
    REQUIRE(configuration.receive_read_error == 0);
    CHECK(configuration.actual_send_bytes > 0);
    CHECK(configuration.actual_receive_bytes > 0);

    const direct::Endpoint destination{0x0100007F, receiver.bound_port()};
    const std::array<std::uint8_t, 2> message{12, 34};
    REQUIRE(sender.send(message, destination).succeeded());
    std::array<std::uint8_t, 2> payload{};
    direct::Endpoint source{};
    const auto before = direct::receive_stamp();
    const auto received = receiver.receive(payload, source, true);
    const auto after = direct::receive_stamp();
    REQUIRE(received.succeeded());
    CHECK(received.value == 2);
    CHECK(payload == message);
    CHECK(source.port_host_order == sender.bound_port());
    CHECK(received.entered_qpc >= before);
    CHECK(received.returned_qpc >= received.entered_qpc);
    CHECK(received.returned_qpc <= after);

    const auto empty = receiver.receive(payload, source, true);
    CHECK_FALSE(empty.succeeded());
    CHECK(empty.error == direct::kSocketWouldBlock);
    CHECK(WSAGetLastError() == empty.error);
    CHECK(empty.entered_qpc != 0);
    CHECK(empty.returned_qpc >= empty.entered_qpc);
    CHECK(source == direct::Endpoint{});

    // A truncated datagram consumes socket work, then the next receive observes the empty queue.
    REQUIRE(sender.send(message, destination).succeeded());
    const auto oversized = receiver.receive(std::span(payload).first(1), source, true);
    CHECK_FALSE(oversized.succeeded());
    CHECK(oversized.error == direct::kSocketMessageTooLarge);
    CHECK(WSAGetLastError() == oversized.error);
    CHECK(oversized.entered_qpc != 0);
    CHECK(oversized.returned_qpc >= oversized.entered_qpc);
    CHECK(receiver.receive(payload, source).error == direct::kSocketWouldBlock);

    fc_plugins::network_diagnostics::DirectPumpObservation pump{};
    direct::observe_receive_attempt(pump, received);
    direct::observe_receive_attempt(pump, oversized);
    direct::observe_receive_attempt(pump, empty);
    CHECK(pump.attempts == 3);
    CHECK(pump.datagrams == 1);
    CHECK(pump.first_empty_qpc == empty.returned_qpc);
    CHECK(pump.last_empty_qpc == empty.returned_qpc);
    CHECK(pump.maximum_receive_duration >= received.returned_qpc - received.entered_qpc);
    CHECK(pump.maximum_receive_duration >= oversized.returned_qpc - oversized.entered_qpc);
    CHECK(pump.maximum_receive_duration >= empty.returned_qpc - empty.entered_qpc);

    receiver.stop();
    const auto invalid = receiver.receive(payload, source, true);
    CHECK_FALSE(invalid.succeeded());
    CHECK(invalid.error == 10022);
    CHECK(invalid.entered_qpc == 0);
    CHECK(invalid.returned_qpc == 0);
}

TEST_CASE("Direct Transport returns receive errors without consuming the remaining attempt budget", "[direct_transport]") {
    std::uint32_t attempts{};
    const auto drained = direct::drain_datagrams(
        [&attempts](direct::Endpoint &) noexcept {
            ++attempts;
            return direct::SocketResult{-1, 10054};
        },
        [](std::int32_t, const direct::Endpoint &) noexcept {});
    CHECK(attempts == 1);
    CHECK(drained.error == 10054);
    CHECK_FALSE(drained.limit_reached);
    CHECK(drained.reason == direct::DatagramDrainEnd::Error);
}

TEST_CASE("Direct Transport correlates timed sends and raw receives with their socket endpoints", "[direct_transport]") {
    direct::UdpSocket sender;
    direct::UdpSocket receiver;
    std::int32_t error{};
    std::int32_t buffer_error{};
    REQUIRE(sender.start(0, error, buffer_error));
    REQUIRE(receiver.start(0, error, buffer_error));
    const direct::Endpoint destination{0x0100007F, receiver.bound_port()};
    const direct::SessionKey key{};
    std::array<std::uint8_t, direct::kMaximumDirectDatagramBytes> output{};
    const std::array<std::uint8_t, direct::kNativeHeaderBytes> payload{11, 1, 0, 0, 1};
    fc_plugins::network_diagnostics::DirectSendObservation sent{};
    const auto result =
        direct::send_direct_data(sender, output, direct::Direction::ClientToServer, key, 17, 39, destination, payload, &sent);
    REQUIRE(result.result == static_cast<std::int32_t>(payload.size()));
    CHECK(sent.phase == 1);
    CHECK(sent.socket_id == sender.configuration().socket_id);
    CHECK(sent.destination_ipv4_network_order == destination.ipv4_network_order);
    CHECK(sent.destination_port == receiver.bound_port());
    CHECK(sent.bound_port == sender.bound_port());
    CHECK(sent.encoding_qpc != 0);
    CHECK(sent.encoding_qpc <= sent.entered_qpc);
    CHECK(sent.entered_qpc <= sent.returned_qpc);

    std::array<std::uint8_t, direct::kMaximumDirectDatagramBytes> input{};
    direct::Endpoint source{};
    const auto received = receiver.receive(input, source, true);
    REQUIRE(received.succeeded());
    fc_plugins::network_diagnostics::DirectPumpObservation pump{.socket_id = receiver.configuration().socket_id, .pump_id = 3};
    direct::observe_receive_attempt(pump, received);
    const auto raw = direct::datagram_observation(pump, received, source, receiver.bound_port(), std::span(input).first(received.value));
    CHECK(raw.socket_id == receiver.configuration().socket_id);
    CHECK(raw.pump_id == 3);
    CHECK(raw.attempt == 1);
    CHECK(raw.connection_id == 17);
    CHECK(raw.sequence == 39);
    CHECK(raw.header_available == 1);
    CHECK(raw.source_ipv4_network_order == destination.ipv4_network_order);
    CHECK(raw.source_port == sender.bound_port());
    CHECK(raw.bound_port == receiver.bound_port());
    CHECK(raw.returned_qpc == received.returned_qpc);

    // Later receives may overwrite the shared byte buffer; the raw metadata remains an independent value.
    input.fill(0xFF);
    CHECK(raw.connection_id == 17);
    CHECK(raw.sequence == 39);
}

TEST_CASE("Direct Transport pins one route across a nested native send", "[direct_transport]") {
    direct::TransmitGroup group;
    REQUIRE(group.begin(7, 12, direct::Carrier::Direct));
    CHECK(group.belongs_to(7, 12));
    CHECK(group.carrier() == direct::Carrier::Direct);

    // Nested native work inherits the outer packet's association instead of splitting its fragments.
    REQUIRE(group.begin(9, 18, direct::Carrier::Galaxy));
    CHECK(group.belongs_to(7, 12));
    CHECK_FALSE(group.belongs_to(9, 18));
    CHECK(group.carrier() == direct::Carrier::Direct);

    REQUIRE(group.end());
    CHECK(group.active());
    REQUIRE(group.end());
    CHECK_FALSE(group.active());
    CHECK(group.carrier() == direct::Carrier::Galaxy);
    CHECK_FALSE(group.end());
}

TEST_CASE("Client Direct Transport accepts a late Commit for an authenticated connection", "[direct_transport]") {
    constexpr std::uint32_t connection = 0x12345678;

    CHECK(direct::valid_commit(direct::RouteState::AwaitCommit, connection, connection));
    CHECK(direct::valid_commit(direct::RouteState::DirectLocked, connection, connection));
    CHECK_FALSE(direct::valid_commit(direct::RouteState::DirectLocked, connection, connection + 1));
    CHECK_FALSE(direct::valid_commit(direct::RouteState::GalaxyLocked, connection, connection));
}

TEST_CASE("Direct Transport copies the complete native source address and rejects disconnected endpoints", "[direct_transport]") {
    constexpr std::uint64_t lobby_id = 0x200000001ULL;
    constexpr std::uint64_t owner_user_id = 0x100000001ULL;
    const direct::NativeAddress expected{lobby_id, 0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    auto native = expected;
    std::byte connected{1};
    const direct::NativeEndpoint endpoint{&connected, native.data()};

    std::uint64_t identity{};
    REQUIRE(direct::read_native_identity(endpoint, identity));
    CHECK(identity == lobby_id);
    CHECK(identity != owner_user_id);
    direct::NativeAddress address{};
    REQUIRE(direct::read_native_address(endpoint, address));
    CHECK(address == expected);

    // The copied source remains complete after later endpoint mutation.
    native = {};
    CHECK(address == expected);
    CHECK_FALSE(direct::read_native_address(endpoint, address));
    CHECK(address == direct::NativeAddress{});

    native = expected;
    connected = {};
    CHECK_FALSE(direct::read_native_address(endpoint, address));
    connected = std::byte{1};
    CHECK_FALSE(direct::read_native_address({nullptr, native.data()}, address));
    CHECK_FALSE(direct::read_native_address({&connected, nullptr}, address));
}

TEST_CASE("Server Direct Transport keeps grouped fragments on the original association after disconnect or alias remap",
          "[direct_transport]") {
    namespace server = fc_plugins::direct_transport::server;
    server::GroupDestinations destinations;
    direct::TransmitGroup group;
    constexpr std::int32_t alias = 65;
    constexpr std::int32_t original = 7;
    constexpr std::uint64_t generation = 0x100000001ULL;

    REQUIRE(group.begin(original, generation, direct::Carrier::Direct));
    REQUIRE(destinations.begin(alias, original));
    CHECK(server::group_destination_matches(group, destinations.primary(alias), original, true, generation));

    // Nested receive disconnects the endpoint or remaps the alias after the first fragment has used Direct.
    for (const auto changed_primary : {-1, 9}) {
        bool lookup_called{};
        const auto selected = destinations.resolve(alias, [&](std::int32_t) {
            lookup_called = true;
            return changed_primary;
        });
        CHECK(selected == original);
        CHECK_FALSE(lookup_called);
        CHECK_FALSE(server::group_destination_matches(group, selected, changed_primary, changed_primary >= 0, generation));
    }
    CHECK_FALSE(server::group_destination_matches(group, destinations.primary(alias), -1, false, 0));
    CHECK_FALSE(server::group_destination_matches(group, destinations.primary(alias), 9, true, generation));
    CHECK_FALSE(server::group_destination_matches(group, destinations.primary(alias), original, true, generation + 1));
    CHECK(destinations.primary(alias) == original);
    CHECK(group.carrier() == direct::Carrier::Direct);

    // A nested invocation cannot overwrite the outer alias mapping; each exit balances the original group.
    REQUIRE(group.begin(original, generation + 1, direct::Carrier::Galaxy));
    REQUIRE(destinations.begin(alias, 9));
    CHECK(destinations.primary(alias) == original);
    destinations.end(alias);
    REQUIRE(group.end());
    CHECK(destinations.primary(alias) == original);
    CHECK(group.active());
    destinations.end(alias);
    REQUIRE(group.end());
    CHECK(destinations.primary(alias) == -1);
    CHECK_FALSE(group.active());
    CHECK(destinations.primary(8) == -1);
    CHECK(destinations.resolve(alias, [](std::int32_t) {
        return 9;
    }) == 9);
}

TEST_CASE("Direct Transport policies preserve mixed peers and handle failed negotiation", "[direct_transport]") {
    using Action = direct::PolicyAction;

    CHECK(direct::policy_action(direct::Policy::Disabled, true) == Action::Native);
    CHECK(direct::policy_action(direct::Policy::Disabled, false) == Action::Native);
    CHECK(direct::policy_action(direct::Policy::Disabled, true, true) == Action::Native);

    CHECK(direct::policy_action(direct::Policy::PreferDirect, true) == Action::Negotiate);
    CHECK(direct::policy_action(direct::Policy::PreferDirect, false) == Action::Native);
    CHECK(direct::policy_action(direct::Policy::PreferDirect, true, true) == Action::Native);

    CHECK(direct::policy_action(direct::Policy::RequireDirectPatched, true) == Action::Negotiate);
    CHECK(direct::policy_action(direct::Policy::RequireDirectPatched, false) == Action::Native);
    CHECK(direct::policy_action(direct::Policy::RequireDirectPatched, true, true) == Action::Remove);

    CHECK(direct::policy_action(direct::Policy::RequireDirectAll, true) == Action::Negotiate);
    CHECK(direct::policy_action(direct::Policy::RequireDirectAll, false) == Action::Remove);
    CHECK(direct::policy_action(direct::Policy::RequireDirectAll, true, true) == Action::Remove);
}

TEST_CASE("Direct Transport control handoff preserves full peer identities and rejects stale slot withdrawals", "[direct_transport]") {
    direct::ControlMailbox mailbox;
    const direct::Peer first{{0x100000001ULL}, 99, 7};
    const direct::Peer replacement{{0x200000001ULL}, 99, 7};
    mailbox.publish(first, fc::Agreement::Compatible);
    mailbox.receive(first.id, {.kind = direct::ControlKind::Offer, .connection_id = 4});

    CHECK_FALSE(mailbox.take_if(7, [](const direct::ControlMailbox::Publication &) {
        return false;
    }));

    auto publication = mailbox.take(7);
    REQUIRE(publication);
    CHECK(publication->peer.id == first.id);
    CHECK(publication->controls[0].connection_id == 4);
    CHECK(publication->present[0]);
    CHECK_FALSE(mailbox.take(7));

    mailbox.receive(first.id, {.kind = direct::ControlKind::Commit, .connection_id = 4});
    mailbox.publish(replacement, fc::Agreement::Compatible);
    mailbox.withdraw(first.id);
    mailbox.receive(first.id, {.kind = direct::ControlKind::Commit, .connection_id = 4});
    publication = mailbox.take(7);
    REQUIRE(publication);
    CHECK(publication->peer.id == replacement.id);
    CHECK(publication->agreement == fc::Agreement::Compatible);
    CHECK_FALSE(publication->present[2]);

    mailbox.receive(replacement.id, {.kind = direct::ControlKind::Offer, .connection_id = 8});
    mailbox.receive(replacement.id, {.kind = direct::ControlKind::Offer, .connection_id = 9});
    publication = mailbox.take(7);
    REQUIRE(publication);
    CHECK(publication->controls[0].connection_id == 8);
    mailbox.withdraw(replacement.id);
    publication = mailbox.take(7);
    REQUIRE(publication);
    CHECK(publication->agreement == fc::Agreement::Unavailable);
}

TEST_CASE("Direct Transport authenticates the full native payload and never changes a terminal carrier", "[direct_transport]") {
    direct::SessionKey key{};
    std::array<std::uint8_t, direct::kMaximumDirectDatagramBytes> bytes{};
    std::array<std::uint8_t, direct::kMaximumNativeBytes> payload{};
    payload[0] = 0x0B;
    payload.back() = 0xAB;
    REQUIRE(direct::write_data_datagram(bytes, direct::Direction::ClientToServer, key, 12, 30, payload) == bytes.size());
    direct::ParsedDirect parsed;
    REQUIRE(direct::parse_direct(bytes, parsed) == direct::ParseStatus::Ok);
    CHECK(direct::verify_authentication_tag(direct::Direction::ClientToServer, key, std::span(bytes).first(direct::kDirectHeaderBytes),
                                            parsed.payload));
    bytes.back() ^= 1;
    CHECK_FALSE(direct::verify_authentication_tag(direct::Direction::ClientToServer, key,
                                                  std::span(bytes).first(direct::kDirectHeaderBytes), parsed.payload));

    // Proof serialization overwrites every byte, including a previous Data payload.
    for (const auto kind : {direct::DirectKind::Probe, direct::DirectKind::ProbeAck}) {
        bytes.fill(0xAB);
        REQUIRE(direct::write_probe_datagram(bytes, kind, direct::Direction::ClientToServer, key, 12, 31));
        REQUIRE(direct::parse_direct(bytes, parsed) == direct::ParseStatus::Ok);
        CHECK(parsed.kind == kind);
        CHECK(std::ranges::all_of(parsed.payload, [](std::uint8_t byte) {
            return byte == 0;
        }));
        CHECK(direct::verify_authentication_tag(direct::Direction::ClientToServer, key, std::span(bytes).first(direct::kDirectHeaderBytes),
                                                parsed.payload));
    }

    CHECK_FALSE(direct::valid_route_transition(direct::RouteState::DirectLocked, direct::RouteState::GalaxyLocked));
    CHECK_FALSE(direct::valid_route_transition(direct::RouteState::GalaxyLocked, direct::RouteState::Negotiating));

    // The internal association generation also remains full-width when it pins fragmented native sends.
    direct::TransmitGroup group;
    REQUIRE(group.begin(64, 0x100000001ULL, direct::Carrier::Direct));
    CHECK_FALSE(group.belongs_to(64, 1));
    CHECK(group.belongs_to(64, 0x100000001ULL));
}

TEST_CASE("Direct Transport generates collision-free connections and requires the proved endpoint", "[direct_transport]") {
    SequentialRandom random;
    const std::array<std::uint32_t, 1> live_ids{0x01020304};
    std::uint32_t connection{};
    REQUIRE(direct::generate_connection_id({&random, &fill_sequence}, live_ids, connection));
    CHECK(connection == 0x05060708);

    const direct::Endpoint expected{0x04030201, 3658};
    CHECK(direct::endpoint_admits_source({}, expected, true));
    CHECK_FALSE(direct::endpoint_admits_source({}, expected, false));
    CHECK(direct::endpoint_admits_source(expected, expected, false));
    CHECK_FALSE(direct::endpoint_admits_source(expected, {0x08070605, 3658}, true));
    CHECK_FALSE(direct::endpoint_admits_source(expected, {0x04030201, 3659}, false));
}

} // namespace

TEST_CASE("Direct Transport reserves the game header prefix for Soldier state packets", "[direct_transport][soldier_velocity]") {
    std::array<std::uint8_t, 1010> bytes{};
    void *packet = reinterpret_cast<void *>(1);
    direct::NativePacketFactory factory;
    for (const auto type : {std::uint8_t{0x1F}, std::uint8_t{0x20}, std::uint8_t{0x21}}) {
        bytes[0] = type;
        factory.prefix = nullptr;
        CHECK(direct::build_native_packet(factory, std::span(bytes).first(5), packet) == direct::NativePacketResult::InvalidPrefix);
        CHECK(packet == nullptr);

        const std::uint32_t prefix = 3;
        factory.prefix = &prefix;
        CHECK(direct::build_native_packet(factory, std::span(bytes).first(1009), packet) == direct::NativePacketResult::FactoryUnavailable);
        CHECK(direct::build_native_packet(factory, bytes, packet) == direct::NativePacketResult::InvalidPrefix);
    }

    // PKT_NONE and unrelated high types retain their native no-prefix classification.
    for (const auto type : {std::uint8_t{0x1E}, std::uint8_t{0x22}, std::uint8_t{0xFE}}) {
        bytes[0] = type;
        factory.prefix = nullptr;
        CHECK(direct::build_native_packet(factory, std::span(bytes).first(5), packet) == direct::NativePacketResult::FactoryUnavailable);
    }
}
