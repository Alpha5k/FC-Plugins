#include "patch.hpp"

#include "handler.hpp"

namespace fc_plugins::collision_history_fix {

fc::Patch definition() {
    return fc::Patch{.id = "CollisionHistoryFix",
                     .description = "Fixes missed player collisions when replaying predicted movement.",
                     .category = "Multiplayer",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client)}};
}

} // namespace fc_plugins::collision_history_fix
