#include "patch.hpp"

#include "handler.hpp"

namespace fc_plugins::update_scheduling {

fc::Patch definition() {
    return fc::Patch{.id = "UpdateScheduling",
                     .description = "Send game updates to every client every turn instead of staggering them.",
                     .category = "Networking",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.GOG.59EDF52B"}, fc::Role::Server)}};
}

} // namespace fc_plugins::update_scheduling
