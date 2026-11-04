#pragma once

#include <fc/lobby.hpp>
#include <fc/native.hpp>
#include <cstdint>

namespace fc_plugins::lobby_player_list::browser {

// Resolves the selected valid Galaxy row in the retail CNetFindGame list on the browser's owner thread.
// The caller must supply live game-owned storage for the object and its linked nodes.
inline fc::LobbyId selected(const void *object, const void *expected_vtable) noexcept {
    if (!object || fc::read_field<const void *>(object) != expected_vtable) {
        return {};
    }

    const auto selected = fc::read_field<std::int32_t>(object, 0x64);
    const auto count = fc::read_field<std::int32_t>(object, 0x1c);

    // CNetFindGame::AddSession admits at most 2000 raw rows.
    if (selected < 0 || selected >= count || count > 2000) {
        return {};
    }

    auto *node = fc::read_field<const void *>(object, 0x10);
    auto visible = 0;

    // The selected ordinal counts valid payloads, not raw list nodes; the terminal node has no payload.
    for (auto visited = 0; node && visited < count; ++visited) {
        const auto *row = fc::read_field<const void *>(node, 0xc);
        if (!row) {
            break;
        }
        if (fc::read_field<std::uint8_t>(row, 0x10) && visible++ == selected) {
            // Galaxy discoveries store the lobby ID in NetAddress's first eight bytes; LAN addresses have flag 1.
            if (fc::read_field<std::uint8_t>(row, 0x1c) != 0) {
                return {};
            }
            return {fc::read_field<std::uint64_t>(row, 0x14)};
        }

        node = fc::read_field<const void *>(node, 4);
    }

    return {};
}

} // namespace fc_plugins::lobby_player_list::browser
