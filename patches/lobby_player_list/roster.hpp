#pragma once

#include "protocol.hpp"
#include <fc/lobby.hpp>
#include <optional>

namespace fc_plugins::lobby_player_list {

// Associates a decoded roster with its requested lobby; invalid records preserve native fallback.
struct Roster {
    fc::LobbyId lobby; // Requested lobby identity; freshness is determined by the request/cache path.
    protocol::Snapshot players;

    // Accepts only a complete supported value for the caller's current lobby.
    static std::optional<Roster> decode(const fc::LobbyRecord &record, fc::LobbyId expected) noexcept {
        if (!expected.value || record.lobby != expected || record.result != fc::LobbyResult::Value ||
            record.version != protocol::kVersion) {
            return std::nullopt;
        }

        protocol::Snapshot snapshot{};
        const std::string_view bytes{reinterpret_cast<const char *>(record.value.data()), record.value.size()};
        if (!protocol::decode(bytes, snapshot)) {
            return std::nullopt;
        }

        return Roster{expected, snapshot};
    }
};

} // namespace fc_plugins::lobby_player_list
