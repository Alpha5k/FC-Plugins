#include "../patches/network_diagnostics/patch.hpp"
#include "../patches/hero_diagnostics/patch.hpp"
#include "../patches/sound_diagnostics/patch.hpp"
#include "../patches/voice_diagnostics/patch.hpp"

#include <fc/plugin.hpp>

namespace fc_plugins {
namespace {

// Declares the optional network, hero, voice and sound diagnostic captures.
fc::Plugin build_plugin() {
    return {
        .id = "Diagnostics",
        .version = "development",
        .categories = {{.id = "Diagnostics", .order = 400}},
        .patches = {network_diagnostics::definition(), hero_diagnostics::definition(), voice_diagnostics::definition(),
                    sound_diagnostics::definition()},
    };
}

} // namespace
} // namespace fc_plugins

FC_PLUGIN(fc_plugins::build_plugin)
