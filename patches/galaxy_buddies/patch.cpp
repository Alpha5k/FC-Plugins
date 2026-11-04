#include "patch.hpp"
#include "handler.hpp"

namespace fc_plugins::galaxy_buddies {

fc::Patch definition() {
    return fc::Patch{.id = "GalaxyBuddies",
                     .description = "Restores the GameSpy buddy system using the Galaxy Friends backend.",
                     .category = "Social",
                     .enabled = true,
                     .depends_on = {"core.lua_bindings"},
                     // Ordinary client launches include player hosts. Steam also waits for the peer
                     // image; only its 2017 revision receives the additional broker startup hook.
                     .variants = {
                         fc::variant<Handler>({"BF2.GOG.59EDF52B"}, fc::Role::Client),
                         fc::variant<Handler>({fc::ImageRequirement{"game", {"BF2.Steam.59EDE353"}},
                                               fc::ImageRequirement{"peer", {"GalaxyPeer.59E6304A", "GalaxyPeer.5BBE22A6"}}},
                                              fc::Role::Client),
                     }};
}

} // namespace fc_plugins::galaxy_buddies
