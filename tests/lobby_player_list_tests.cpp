#include "../patches/lobby_player_list/protocol.hpp"
#include "../patches/lobby_player_list/roster.hpp"
#include "../patches/lobby_player_list/browser.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

namespace {
namespace lobby = fc_plugins::lobby_player_list::protocol;
namespace roster = fc_plugins::lobby_player_list;

TEST_CASE("Lobby player data round-trips bounded names and signed statistics", "[social][lobby]") {
    lobby::Snapshot source{};
    source.count = 2;
    source.players[0].index = 3;
    std::ranges::copy(std::string_view{"Alpha;One"}, source.players[0].name.begin());
    source.players[1].index = 63;
    std::ranges::copy(std::string_view{"Bravo,Two"}, source.players[1].name.begin());
    source.players[0].kills = -2;
    source.players[0].deaths = 42;
    source.players[0].points = -17;
    source.players[0].hero_points = 300;
    source.players[1].kills = std::numeric_limits<std::int32_t>::min();
    source.players[1].deaths = std::numeric_limits<std::int32_t>::max();
    source.players[1].points = std::numeric_limits<std::int16_t>::min();
    source.players[1].hero_points = std::numeric_limits<std::int16_t>::max();

    std::array<char, lobby::kMaximumValueSize> encoded{};
    std::size_t size{};
    REQUIRE(lobby::encode(source, encoded, size));
    const std::array<std::uint8_t, 12> first_stats{0xFE, 0xFF, 0xFF, 0xFF, 0x2A, 0, 0, 0, 0xEF, 0xFF, 0x2C, 0x01};
    for (std::size_t byte = 0; byte < first_stats.size(); ++byte) {
        CHECK(std::bit_cast<std::uint8_t>(encoded[13 + byte]) == first_stats[byte]);
    }

    lobby::Snapshot decoded{};
    REQUIRE(lobby::decode({encoded.data(), size}, decoded));
    CHECK(decoded.count == 2);
    CHECK(decoded.players[0].index == 3);
    CHECK(std::strcmp(decoded.players[0].name.data(), "Alpha;One") == 0);
    CHECK(decoded.players[1].index == 63);
    CHECK(std::strcmp(decoded.players[1].name.data(), "Bravo,Two") == 0);
    CHECK(decoded == source);
}

TEST_CASE("Lobby player data covers the empty and full roster bounds", "[social][lobby]") {
    std::array<char, lobby::kMaximumValueSize> encoded{};
    std::size_t size{};

    lobby::Snapshot empty{};
    REQUIRE(lobby::encode(empty, encoded, size));
    lobby::Snapshot decoded{};
    REQUIRE(lobby::decode({encoded.data(), size}, decoded));
    CHECK(decoded.count == 0);

    lobby::Snapshot full{};
    full.count = static_cast<std::uint8_t>(lobby::kMaximumPlayers);
    for (std::size_t index = 0; index < lobby::kMaximumPlayers; ++index) {
        full.players[index].index = static_cast<std::uint8_t>(index);
        std::ranges::fill_n(full.players[index].name.begin(), lobby::kMaximumNameSize, static_cast<char>('A' + index % 26));
    }
    REQUIRE(lobby::encode(full, encoded, size));
    REQUIRE(lobby::decode({encoded.data(), size}, decoded));
    REQUIRE(decoded.count == lobby::kMaximumPlayers);
    CHECK(std::char_traits<char>::length(decoded.players.back().name.data()) == lobby::kMaximumNameSize);
    CHECK(decoded.players.back().index == lobby::kMaximumPlayers - 1);
    CHECK(size == 2882);
    // FC encodes the binary payload as FC1:2:<base64> in the 4095-character Galaxy value.
    CHECK(6 + 4 * ((size + 2) / 3) <= 4095);
}

TEST_CASE("Lobby player data rejects a name without a terminator", "[social][lobby]") {
    lobby::Snapshot snapshot{};
    snapshot.count = 1;
    std::ranges::fill(snapshot.players[0].name, 'A');

    std::array<char, lobby::kMaximumValueSize> encoded{};
    std::size_t size{};
    CHECK_FALSE(lobby::encode(snapshot, encoded, size));
    CHECK(size == 0);
}

TEST_CASE("Lobby roster rejects stale, failed, and malformed records", "[social][lobby]") {
    lobby::Snapshot source{};
    source.count = 1;
    source.players[0].index = 3;
    std::ranges::copy(std::string_view{"Alpha"}, source.players[0].name.begin());
    std::array<char, lobby::kMaximumValueSize> wire{};
    std::size_t size{};
    REQUIRE(lobby::encode(source, wire, size));
    fc::LobbyRecord record{{100}, fc::LobbyResult::Value, lobby::kVersion, {}, {}};
    record.value.assign(reinterpret_cast<const std::byte *>(wire.data()), reinterpret_cast<const std::byte *>(wire.data() + size));
    REQUIRE(roster::Roster::decode(record, {100}));
    CHECK_FALSE(roster::Roster::decode(record, {200}));
    for (auto failure : {fc::LobbyResult::Missing, fc::LobbyResult::Malformed, fc::LobbyResult::Unsupported, fc::LobbyResult::Unavailable,
                         fc::LobbyResult::Failed}) {
        record.result = failure;
        CHECK_FALSE(roster::Roster::decode(record, {100}));
    }
    record.result = fc::LobbyResult::Value;
    record.version = 1;
    CHECK_FALSE(roster::Roster::decode(record, {100}));
    record.version = lobby::kVersion;
    record.value.push_back(std::byte{0});
    CHECK_FALSE(roster::Roster::decode(record, {100}));
}

TEST_CASE("Malformed roster leaves the previous decoded value intact", "[social][lobby]") {
    lobby::Snapshot source{};
    source.count = 2;
    source.players[0].index = 3;
    source.players[1].index = 63;
    std::ranges::copy(std::string_view{"Alpha"}, source.players[0].name.begin());
    std::ranges::copy(std::string_view{"Bravo"}, source.players[1].name.begin());
    std::array<char, lobby::kMaximumValueSize> encoded{};
    std::size_t size{};
    REQUIRE(lobby::encode(source, encoded, size));
    lobby::Snapshot output{};
    output.count = 1;
    output.players[0].index = 7;
    const auto previous = output;
    for (std::size_t truncated = 0; truncated < size; ++truncated) {
        CHECK_FALSE(lobby::decode({encoded.data(), truncated}, output));
        CHECK(output == previous);
    }
    const auto valid = encoded;
    for (const auto [offset, value] :
         std::array<std::pair<std::size_t, char>, 7>{{{0, 1}, {1, 65}, {2, 64}, {3, 0}, {3, 32}, {4, 0}, {21, 3}}}) {
        encoded = valid;
        encoded[offset] = value;
        CHECK_FALSE(lobby::decode({encoded.data(), size}, output));
        CHECK(output == previous);
    }
    encoded = valid;
    encoded[size] = 0;
    CHECK_FALSE(lobby::decode({encoded.data(), size + 1}, output));
    CHECK(output == previous);
    CHECK_FALSE(lobby::decode("1;1;3,5:Alpha;", output));
    CHECK(output == previous);
}

// Constructs native-sized records so the production selector, including visibility filtering, is exercised.
template <class T, std::size_t N> void put(std::array<std::byte, N> &object, std::size_t offset, T value) {
    std::memcpy(object.data() + offset, &value, sizeof(value));
}

TEST_CASE("Browser roster resolves the selected visible Galaxy server", "[social][lobby]") {
    std::array<std::byte, 0x80> object{};
    std::array<std::array<std::byte, 32>, 4> nodes{};
    std::array<std::array<std::byte, 0x180>, 3> rows{};
    std::array<const void *, 1> vtable{};
    std::array<const void *, 1> other_vtable{};
    put(object, 0, vtable.data());
    put(object, 0x10, nodes[0].data());
    put(object, 0x1c, std::int32_t{3});
    put(object, 0x64, std::int32_t{1});
    for (int i = 0; i < 3; ++i) {
        put(nodes[i], 4, nodes[i + 1].data());
        put(nodes[i], 0xc, rows[i].data());
        put(rows[i], 0x10, std::uint8_t{static_cast<std::uint8_t>(i != 1)});
        put(rows[i], 0x14, std::uint64_t{100 + static_cast<unsigned>(i)});
    }
    CHECK(roster::browser::selected(object.data(), vtable.data()).value == 102);
    CHECK(roster::browser::selected(object.data(), other_vtable.data()).value == 0);
    put(rows[2], 0x1c, std::uint8_t{1});
    CHECK(roster::browser::selected(object.data(), vtable.data()).value == 0);
    put(object, 0x64, std::int32_t{-1});
    CHECK(roster::browser::selected(object.data(), vtable.data()).value == 0);
}

} // namespace
