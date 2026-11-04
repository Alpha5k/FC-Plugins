#include "../patches/infinite_sprint_patch/patch.hpp"
#include "../patches/speedpack_patch/patch.hpp"
#include "../patches/spawn_delay/patch.hpp"
#include "../patches/spawn_jump_fix/patch.hpp"
#include "../patches/distance_lag_fix/patch.hpp"
#include "../patches/object_budget/patch.hpp"
#include "../patches/name_bug_fix/patch.hpp"
#include "../patches/map_hang_fix/patch.hpp"
#include "../patches/vote_exploit_fix/patch.hpp"
#include "../patches/waitlate_grace/patch.hpp"
#include "../patches/update_scheduling/patch.hpp"
#include "../patches/server_loop_rate/patch.hpp"
#include "../patches/inactive_window_fix/patch.hpp"
#include "../patches/aerial_gravity/patch.hpp"
#include "../patches/soldier_state_sync/patch.hpp"
#include "../patches/melee_timing/patch.hpp"
#include "../patches/shot_sequence/patch.hpp"
#include "../patches/input_sync/patch.hpp"
#include "../patches/direct_transport/patch.hpp"

#include <fc/plugin.hpp>

namespace fc_plugins {
namespace {

// Declares the Server plugin's gameplay corrections and network behavior.
fc::Plugin build_plugin() {
    return {
        .id = "Server",
        .version = "development",
        .categories = {{.id = "Multiplayer", .order = 100}, {.id = "Networking", .order = 200}, {.id = "Server", .order = 300}},
        .patches =
            {
                infinite_sprint_patch::definition(),
                speedpack_patch::definition(),
                spawn_delay::definition(),
                spawn_jump_fix::definition(),
                distance_lag_fix::definition(),
                object_budget::definition(),
                name_bug_fix::definition(),
                map_hang_fix::definition(),
                vote_exploit_fix::definition(),
                waitlate_grace::definition(),
                update_scheduling::definition(),
                server_loop_rate::definition(),
                inactive_window_fix::definition(fc::Role::Server),
                aerial_gravity::definition(fc::Role::Server),
                soldier_state_sync::definition(fc::Role::Server),
                melee_timing::definition(fc::Role::Server),
                shot_sequence::definition(fc::Role::Server),
                input_sync::definition(fc::Role::Server),
                direct_transport::definition(fc::Role::Server),
            },
    };
}

} // namespace
} // namespace fc_plugins

FC_PLUGIN(fc_plugins::build_plugin)
