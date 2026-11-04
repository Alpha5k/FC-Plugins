#include "../patches/stable_player_ids/patch.hpp"

#include <fc/plugin.hpp>

namespace fc_plugins {
namespace {

// Declares the Classic Collection Server plugin's persistent player identity patch.
fc::Plugin build_plugin() {
    return {
        .id = "Server",
        .version = "development",
        .categories = {{.id = "Server", .order = 300}},
        .patches = {stable_player_ids::definition()},
    };
}

} // namespace
} // namespace fc_plugins

FC_PLUGIN(fc_plugins::build_plugin)
