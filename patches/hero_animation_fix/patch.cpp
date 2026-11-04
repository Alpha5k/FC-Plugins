#include "patch.hpp"

#include "handler.hpp"

namespace fc_plugins::hero_animation_fix {

fc::Patch definition() {
    return fc::Patch{.id = "HeroAnimationFix",
                     .description = "Prevents repeated hero animations and sounds in multiplayer.",
                     .category = "Multiplayer",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client)}};
}

} // namespace fc_plugins::hero_animation_fix
