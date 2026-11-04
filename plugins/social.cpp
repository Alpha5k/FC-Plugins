#include "../patches/galaxy_buddies/patch.hpp"
#include "../patches/lobby_player_list/patch.hpp"
#include "../patches/voice_chat/patch.hpp"

#include <fc/plugin.hpp>

namespace fc_plugins {
namespace {

// Declares buddy, lobby and voice features.
fc::Plugin build_plugin() {
    return {
        .id = "Social",
        .version = "development",
        .categories = {{.id = "Social", .order = 300}},
        .patches =
            {
                galaxy_buddies::definition(),
                lobby_player_list::definition(),
                voice_chat::definition(),
            },
    };
}

} // namespace
} // namespace fc_plugins

FC_PLUGIN(fc_plugins::build_plugin)
