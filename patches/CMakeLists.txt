cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FusionCutterPluginPatches LANGUAGES C CXX)
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The patch implementations support x86 and x64 builds. Use a matching MSVC developer shell.")
endif()

if(NOT TARGET FusionCutter::SDK)
    find_package(FusionCutter CONFIG REQUIRED)
endif()

# Classic Collection uses x64; its Server plugin contains only patches qualified for that architecture.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_library(fusioncutter_server_patches STATIC
        stable_player_ids/patch.cpp stable_player_ids/layout.cpp stable_player_ids/handler.cpp
    )
    target_link_libraries(fusioncutter_server_patches PUBLIC FusionCutter::SDK)
    target_compile_options(fusioncutter_server_patches PRIVATE /W4)

    # Use the C runtime options supplied by the FC SDK.
    set_target_properties(fusioncutter_server_patches PROPERTIES
        MSVC_RUNTIME_LIBRARY ""
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES OFF
    )
    return()
endif()

# Keep vendor options local so plugin tests remain enabled in their parent directory.
if(NOT TARGET opus)
    block(SCOPE_FOR VARIABLES)
        set(BUILD_TESTING OFF)
        set(BUILD_SHARED_LIBS OFF)
        set(OPUS_BUILD_SHARED_LIBRARY OFF)
        set(OPUS_BUILD_PROGRAMS OFF)
        set(OPUS_BUILD_TESTING OFF)
        set(OPUS_CUSTOM_MODES OFF)
        set(OPUS_DRED OFF)
        set(OPUS_DEEP_PLC ON)
        set(OPUS_OSCE ON)
        set(OPUS_STATIC_RUNTIME ON)
        set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
        set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)
        add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../vendor/opus"
                         "${CMAKE_CURRENT_BINARY_DIR}/opus" EXCLUDE_FROM_ALL)
        # Enable Opus's blind bandwidth extension for voice decoding.
        target_compile_definitions(opus PRIVATE ENABLE_OSCE_BWE)
    endblock()
endif()

# Both plugins and focused tests share the transport implementation and helpers.
add_library(fusioncutter_direct_transport STATIC
    direct_transport/association.cpp direct_transport/datagram.cpp direct_transport/game.cpp
    direct_transport/network.cpp direct_transport/protocol.cpp direct_transport/security.cpp direct_transport/socket.cpp
    direct_transport/patch.cpp
    direct_transport/client/associations.cpp direct_transport/client/handler.cpp direct_transport/client/native.cpp
    direct_transport/server/associations.cpp direct_transport/server/handler.cpp direct_transport/server/layout.cpp
    direct_transport/server/native.cpp direct_transport/server/removals.cpp
    direct_transport/server/galaxy_peer.cpp
)
target_link_libraries(fusioncutter_direct_transport PRIVATE ws2_32 bcrypt)

# Plugin patch libraries group their implementations and link shared transport code where needed.
add_library(fusioncutter_multiplayer_patches STATIC
    inactive_window_fix/patch.cpp
    unlimited_bandwidth/patch.cpp
    input_sync/patch.cpp input_sync/layout.cpp input_sync/handler.cpp input_sync/history.cpp
    crouch_bug_fix/patch.cpp crouch_bug_fix/layout.cpp
    door_corpse_fix/patch.cpp door_corpse_fix/layout.cpp
    reload_duration_fix/patch.cpp reload_duration_fix/layout.cpp
    hero_animation_fix/patch.cpp hero_animation_fix/layout.cpp
    hero_animation_fix/handler.cpp hero_animation_fix/history.cpp
    melee_timing/patch.cpp
    host_random_state_fix/patch.cpp
    shot_sequence/patch.cpp shot_sequence/handler.cpp shot_sequence/layout.cpp
    collision_history_fix/patch.cpp collision_history_fix/layout.cpp collision_history_fix/handler.cpp
    weapon_swap_replay_fix/patch.cpp weapon_swap_replay_fix/layout.cpp
    weapon_swap_replay_fix/handler.cpp weapon_swap_replay_fix/reconciliation.cpp
    weapon_swap_replay_fix/local_ledger.cpp weapon_swap_replay_fix/packed_ledger.cpp
    spectator_camera_smoothing/patch.cpp spectator_camera_smoothing/layout.cpp
    spectator_camera_smoothing/handler.cpp spectator_camera_smoothing/motion.cpp spectator_camera_smoothing/presentation.cpp
    late_update_recovery/patch.cpp late_update_recovery/handler.cpp late_update_recovery/layout.cpp
    aerial_gravity/patch.cpp
    soldier_state_sync/patch.cpp soldier_state_sync/layout.cpp soldier_state_sync/rotation.cpp soldier_state_sync/trigger.cpp
    saber_sound_fix/patch.cpp
    footstep_tracking_fix/patch.cpp
)
target_link_libraries(fusioncutter_multiplayer_patches PRIVATE ws2_32 fusioncutter_direct_transport)

add_library(fusioncutter_server_patches STATIC
    input_sync/patch.cpp input_sync/layout.cpp input_sync/handler.cpp input_sync/history.cpp
    inactive_window_fix/patch.cpp
    melee_timing/patch.cpp
    shot_sequence/patch.cpp shot_sequence/handler.cpp shot_sequence/layout.cpp
    infinite_sprint_patch/patch.cpp
    speedpack_patch/patch.cpp
    spawn_delay/patch.cpp
    spawn_jump_fix/patch.cpp
    distance_lag_fix/patch.cpp
    object_budget/patch.cpp
    name_bug_fix/patch.cpp
    map_hang_fix/patch.cpp
    vote_exploit_fix/patch.cpp
    waitlate_grace/patch.cpp
    server_loop_rate/patch.cpp
    update_scheduling/patch.cpp update_scheduling/handler.cpp update_scheduling/layout.cpp update_scheduling/send_window.cpp
    aerial_gravity/patch.cpp
    soldier_state_sync/patch.cpp soldier_state_sync/layout.cpp soldier_state_sync/rotation.cpp soldier_state_sync/trigger.cpp
)
target_link_libraries(fusioncutter_server_patches PRIVATE ws2_32 fusioncutter_direct_transport)

add_library(fusioncutter_diagnostics_patches STATIC
    network_diagnostics/client.cpp
    network_diagnostics/combat.cpp
    network_diagnostics/frame_layout.cpp
    network_diagnostics/frame_timing.cpp
    network_diagnostics/handler.cpp
    network_diagnostics/layout.cpp
    network_diagnostics/patch.cpp
    network_diagnostics/pipeline.cpp
    network_diagnostics/pipeline_layout.cpp
    network_diagnostics/replication.cpp
    network_diagnostics/server.cpp
    network_diagnostics/velocity.cpp
    hero_diagnostics/client.cpp
    hero_diagnostics/causes.cpp
    hero_diagnostics/handler.cpp
    hero_diagnostics/layout.cpp
    hero_diagnostics/patch.cpp
    hero_diagnostics/server.cpp
    hero_diagnostics/subject.cpp
    voice_diagnostics/patch.cpp voice_diagnostics/handler.cpp voice_diagnostics/layout.cpp
    voice_diagnostics/hud.cpp voice_diagnostics/hud_layout.cpp
    sound_diagnostics/patch.cpp sound_diagnostics/handler.cpp sound_diagnostics/causes.cpp sound_diagnostics/layout.cpp
    sound_diagnostics/footsteps.cpp
)
target_link_libraries(fusioncutter_diagnostics_patches PRIVATE ws2_32)

add_library(fusioncutter_social_patches STATIC
    galaxy_buddies/game.cpp galaxy_buddies/handler.cpp galaxy_buddies/layout.cpp galaxy_buddies/patch.cpp
    galaxy_buddies/listeners.cpp galaxy_buddies/lua.cpp galaxy_buddies/names.cpp
    galaxy_buddies/relationships.cpp galaxy_buddies/reporting.cpp
    galaxy_buddies/broker.cpp
    lobby_player_list/handler.cpp lobby_player_list/layout.cpp lobby_player_list/patch.cpp lobby_player_list/protocol.cpp
    voice_chat/patch.cpp voice_chat/handler.cpp voice_chat/native.cpp
    voice_chat/settings.cpp voice_chat/routing.cpp voice_chat/codec.cpp voice_chat/ui_diagnostics.cpp
    voice_chat/transport.cpp voice_chat/options.cpp voice_chat/layout.cpp
)
target_link_libraries(fusioncutter_social_patches PRIVATE opus dsound)

add_library(fusioncutter_mpgc_patches STATIC
    multiplayer_gc/patch.cpp multiplayer_gc/layout.cpp multiplayer_gc/handler.cpp multiplayer_gc/state.cpp
    multiplayer_gc/participants.cpp multiplayer_gc/session.cpp multiplayer_gc/session_actions.cpp multiplayer_gc/protocol.cpp
    multiplayer_gc/shell.cpp multiplayer_gc/shell_chat.cpp multiplayer_gc/discovery.cpp
    multiplayer_gc/handler_campaign.cpp multiplayer_gc/handler_browser.cpp multiplayer_gc/handler_chat.cpp
    multiplayer_gc/transition.cpp multiplayer_gc/transition_layout.cpp
)

foreach(patch_target IN ITEMS fusioncutter_mpgc_patches fusioncutter_multiplayer_patches fusioncutter_server_patches
                              fusioncutter_diagnostics_patches
                              fusioncutter_social_patches fusioncutter_direct_transport)
    target_link_libraries(${patch_target} PUBLIC FusionCutter::SDK)
    target_compile_options(${patch_target} PRIVATE /W4)

    # Use the C runtime options supplied by the FC SDK.
    set_target_properties(${patch_target} PROPERTIES
        MSVC_RUNTIME_LIBRARY ""
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES OFF
    )
endforeach()
