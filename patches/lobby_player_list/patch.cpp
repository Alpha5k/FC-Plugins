#include "patch.hpp"
#include "handler.hpp"

namespace fc_plugins::lobby_player_list {

fc::Patch definition() {
    return fc::Patch{.id = "LobbyPlayerList",
                     .description = "Restores the server player list in the multiplayer lobby browser.",
                     .category = "Social",
                     .enabled = true,
                     .depends_on = {"core.lua_bindings"},
                     .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::All)}};
}

} // namespace fc_plugins::lobby_player_list
