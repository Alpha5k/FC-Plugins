#include "patch.hpp"
#include "handler.hpp"

namespace fc_plugins::multiplayer_gc {

fc::Patch definition() {
    return fc::Patch{.id = "MultiplayerGC",
                     .description = "Enables multiplayer Galactic Conquest campaigns.",
                     .enabled = true,
                     .depends_on = {"core.lua_bindings"},
                     .variants = {fc::variant<Handler>({fc::ImageRequirement{"game", {"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}},
                                                        fc::ImageRequirement{"galaxy", {"Galaxy.59E63011"}}},
                                                       fc::Role::Client)}};
}

} // namespace fc_plugins::multiplayer_gc
