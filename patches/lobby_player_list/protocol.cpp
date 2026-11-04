#include "protocol.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace fc_plugins::lobby_player_list::protocol {
namespace {

// Copies one field if the destination has space.
bool append(std::span<char> output, std::size_t &cursor, std::string_view value) noexcept {
    if (value.size() > output.size() - cursor) {
        return false;
    }
    std::memcpy(output.data() + cursor, value.data(), value.size());
    cursor += value.size();
    return true;
}

// Writes one little-endian integer field, preserving the supplied signed value's bit representation.
bool append_integer(std::span<char> output, std::size_t &cursor, std::uint32_t value, std::size_t width) noexcept {
    if (width > output.size() - cursor) {
        return false;
    }
    for (std::size_t byte = 0; byte < width; ++byte) {
        output[cursor++] = std::bit_cast<char>(static_cast<std::uint8_t>(value >> (byte * 8)));
    }
    return true;
}

// Reads one complete little-endian integer field without consuming a partial field.
bool read_integer(std::string_view input, std::size_t &cursor, std::size_t width, std::uint32_t &value) noexcept {
    if (width > input.size() - cursor) {
        return false;
    }
    value = 0;
    for (std::size_t byte = 0; byte < width; ++byte) {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(input[cursor++])) << (byte * 8);
    }
    return true;
}

} // namespace

bool encode(const Snapshot &snapshot, std::span<char> output, std::size_t &size) noexcept {
    size = 0;
    if (snapshot.count > kMaximumPlayers || !append_integer(output, size, kVersion, 1) ||
        !append_integer(output, size, snapshot.count, 1)) {
        return false;
    }

    // Explicit name lengths preserve arbitrary non-NUL name bytes, including delimiters.
    for (std::size_t ordinal = 0; ordinal < snapshot.count; ++ordinal) {
        const auto &player = snapshot.players[ordinal];
        const auto terminator = std::ranges::find(player.name, '\0');
        if (terminator == player.name.end()) {
            size = 0;
            return false;
        }
        const auto name_size = static_cast<std::size_t>(terminator - player.name.begin());
        if (!append_integer(output, size, player.index, 1) || !append_integer(output, size, static_cast<std::uint32_t>(name_size), 1) ||
            !append(output, size, {player.name.data(), name_size}) ||
            !append_integer(output, size, std::bit_cast<std::uint32_t>(player.kills), 4) ||
            !append_integer(output, size, std::bit_cast<std::uint32_t>(player.deaths), 4) ||
            !append_integer(output, size, std::bit_cast<std::uint16_t>(player.points), 2) ||
            !append_integer(output, size, std::bit_cast<std::uint16_t>(player.hero_points), 2)) {
            size = 0;
            return false;
        }
    }
    return true;
}

bool decode(std::string_view value, Snapshot &output) noexcept {
    // Build a temporary so a malformed property cannot replace the caller's previous decoded roster.
    Snapshot parsed{};
    std::size_t cursor{};
    std::uint32_t version{};
    std::uint32_t count{};
    if (!read_integer(value, cursor, 1, version) || version != kVersion || !read_integer(value, cursor, 1, count) ||
        count > kMaximumPlayers) {
        return false;
    }

    std::array<bool, kMaximumPlayers> seen{};
    for (std::size_t ordinal = 0; ordinal < count; ++ordinal) {
        std::uint32_t index{};
        std::uint32_t name_size{};
        if (!read_integer(value, cursor, 1, index) || index >= kMaximumPlayers || seen[index] ||
            !read_integer(value, cursor, 1, name_size) || !name_size || name_size > kMaximumNameSize || name_size > value.size() - cursor) {
            return false;
        }
        const auto name = value.substr(cursor, name_size);
        cursor += name_size;
        if (name.find('\0') != std::string_view::npos) {
            return false;
        }

        std::uint32_t kills{}, deaths{}, points{}, hero_points{};
        if (!read_integer(value, cursor, 4, kills) || !read_integer(value, cursor, 4, deaths) || !read_integer(value, cursor, 2, points) ||
            !read_integer(value, cursor, 2, hero_points)) {
            return false;
        }
        auto &player = parsed.players[ordinal];
        player.index = static_cast<std::uint8_t>(index);
        std::memcpy(player.name.data(), name.data(), name.size());
        player.kills = std::bit_cast<std::int32_t>(kills);
        player.deaths = std::bit_cast<std::int32_t>(deaths);
        player.points = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(points));
        player.hero_points = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(hero_points));
        seen[index] = true;
    }

    if (cursor != value.size()) {
        return false;
    }
    parsed.count = static_cast<std::uint8_t>(count);
    output = parsed;
    return true;
}

} // namespace fc_plugins::lobby_player_list::protocol
