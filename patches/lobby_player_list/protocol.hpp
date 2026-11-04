#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fc_plugins::lobby_player_list::protocol {

inline constexpr std::size_t kMaximumPlayers = 64;
inline constexpr std::size_t kMaximumNameSize = 31;
inline constexpr std::uint8_t kVersion = 2;
inline constexpr std::size_t kMaximumValueSize = 2 + kMaximumPlayers * (2 + kMaximumNameSize + 12);

// Stores one sparse native slot, copied name and the original GameSpy browser statistics.
struct Player {
    std::array<char, kMaximumNameSize + 1> name{}; // Native name bytes with a trailing NUL; the limit counts bytes.
    std::uint8_t index{};                          // Native player slot, independent of this player's ordinal in the roster.
    std::int32_t kills{};                          // Native network totals, including the game's signed kill adjustment.
    std::int32_t deaths{};
    std::int16_t points{}; // GetPoints(true); hero_points is the separate GetPoints(false) counter.
    std::int16_t hero_points{};
    bool operator==(const Player &) const = default;
};

// Keeps a bounded roster whose binary payload fits the FC lobby property's base64 envelope.
struct Snapshot {
    std::array<Player, kMaximumPlayers> players{};
    std::uint8_t count{}; // Number of populated entries at the start of players.
    bool operator==(const Snapshot &) const = default;
};

// Writes version/count bytes, then slot/length/name, little-endian int32 kills/deaths and int16 player/hero scores.
// Requires a captured roster with valid slots/names; failure may leave partial bytes and size in the output.
bool encode(const Snapshot &snapshot, std::span<char> output, std::size_t &size) noexcept;

// Parses a complete version-2 roster, rejecting duplicate slots and empty/overlong names; failure leaves output unchanged.
bool decode(std::string_view value, Snapshot &output) noexcept;

} // namespace fc_plugins::lobby_player_list::protocol
