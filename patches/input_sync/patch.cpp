#include "patch.hpp"

#include "handler.hpp"

namespace fc_plugins::input_sync {

fc::Patch definition(fc::Role role) {
    fc::Patch patch{
        .id = "InputSync",
        .description = "Sends complete controls, retains prediction input and prevents stale server input reuse.",
        .category = "Networking",
        .enabled = true,
    };
    if (role == fc::Role::Server) {
        patch.variants = {fc::variant<ServerHandler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, role)};
    } else {
        patch.variants = {fc::variant<ClientHandler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, role)};
    }
    return patch;
}

} // namespace fc_plugins::input_sync
