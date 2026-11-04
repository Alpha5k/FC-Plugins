#include "../patches/multiplayer_gc/patch.hpp"

#include <fc/plugin.hpp>

namespace fc_plugins {
namespace {

// Composes the optional Galactic Conquest addon bridge.
fc::Plugin build_plugin() {
    return {.id = "MPGC", .version = "development", .patches = {multiplayer_gc::definition()}};
}

} // namespace
} // namespace fc_plugins

FC_PLUGIN(fc_plugins::build_plugin)
