#include "patch.hpp"

#include "handler.hpp"

namespace fc_plugins::weapon_swap_replay_fix {

fc::Patch definition() {
    return fc::Patch{.id = "WeaponSwapReplayFix",
                     .description = "Fixes the duplicated weapon swap sounds and HUD flickering in multiplayer on high ping.",
                     .category = "Multiplayer",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client)}};
}

} // namespace fc_plugins::weapon_swap_replay_fix
