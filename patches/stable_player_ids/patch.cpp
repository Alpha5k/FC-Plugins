#include "patch.hpp"

#include "handler.hpp"

namespace fc_plugins::stable_player_ids {

fc::Patch definition() {
    return fc::Patch{.id = "StablePlayerIds",
                     .description = "Uses published Photon account IDs for persistent admin keyhashes.",
                     .category = "Server",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({fc::ImageRequirement{"game", {"BF2.Classic.66702CD2"}},
                                                        fc::ImageRequirement{"bootstrap", {"Classic.Bootstrap.66702CD7"}}},
                                                       fc::Role::Server)}};
}

} // namespace fc_plugins::stable_player_ids
