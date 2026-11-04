#include "../patches/unlimited_bandwidth/patch.hpp"
#include "../patches/inactive_window_fix/patch.hpp"
#include "../patches/input_sync/patch.hpp"
#include "../patches/crouch_bug_fix/patch.hpp"
#include "../patches/door_corpse_fix/patch.hpp"
#include "../patches/reload_duration_fix/patch.hpp"
#include "../patches/hero_animation_fix/patch.hpp"
#include "../patches/melee_timing/patch.hpp"
#include "../patches/host_random_state_fix/patch.hpp"
#include "../patches/shot_sequence/patch.hpp"
#include "../patches/soldier_state_sync/patch.hpp"
#include "../patches/collision_history_fix/patch.hpp"
#include "../patches/weapon_swap_replay_fix/patch.hpp"
#include "../patches/spectator_camera_smoothing/patch.hpp"
#include "../patches/late_update_recovery/patch.hpp"
#include "../patches/aerial_gravity/patch.hpp"
#include "../patches/direct_transport/patch.hpp"
#include "../patches/saber_sound_fix/patch.hpp"
#include "../patches/footstep_tracking_fix/patch.hpp"

#include <fc/plugin.hpp>

namespace fc_plugins {
namespace {

// Declares Multiplayer patches and their configuration categories.
fc::Plugin build_plugin() {
    return {
        .id = "Multiplayer",
        .version = "development",
        .categories = {{.id = "Multiplayer", .order = 100}, {.id = "Networking", .order = 200}},
        .patches =
            {
                unlimited_bandwidth::definition(),
                inactive_window_fix::definition(fc::Role::Client),
                input_sync::definition(fc::Role::Client),
                crouch_bug_fix::definition(),
                door_corpse_fix::definition(),
                reload_duration_fix::definition(),
                hero_animation_fix::definition(),
                melee_timing::definition(fc::Role::Client),
                host_random_state_fix::definition(),
                shot_sequence::definition(fc::Role::Client),
                soldier_state_sync::definition(fc::Role::Client),
                collision_history_fix::definition(),
                weapon_swap_replay_fix::definition(),
                spectator_camera_smoothing::definition(),
                late_update_recovery::definition(),
                aerial_gravity::definition(fc::Role::Client),
                direct_transport::definition(fc::Role::Client),
                saber_sound_fix::definition(),
                footstep_tracking_fix::definition(),
            },
    };
}

} // namespace
} // namespace fc_plugins

FC_PLUGIN(fc_plugins::build_plugin)
