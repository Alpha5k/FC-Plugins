#include "patch.hpp"

#include "handler.hpp"

namespace fc_plugins::late_update_recovery {

fc::Patch definition() {
    return fc::Patch{.id = "LateUpdateRecovery",
                     .description = "Recover multiple updates received in a single turn instead of only parsing the newest.",
                     .category = "Networking",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client)}};
}

} // namespace fc_plugins::late_update_recovery
