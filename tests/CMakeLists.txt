cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FusionCutterPluginTests LANGUAGES C CXX)
    include(CTest)
endif()

# The existing behavior tests exercise x86 patches and their transport and codec dependencies.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    return()
endif()

if(NOT TARGET FusionCutter::SDK)
    find_package(FusionCutter CONFIG REQUIRED)
endif()

# Independent test configuration also supplies the common transport helpers and pinned Opus codec.
if(NOT TARGET fusioncutter_direct_transport)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../patches" "${CMAKE_CURRENT_BINARY_DIR}/patches")
endif()

# Catch2 3.15.3, revision 8b08d4d79514f45f7e4ce2a607ac9c94e920d1bb.
# Compile its string-view formatter and use the same static CRT as the SDK.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CATCH_INSTALL_DOCS OFF)
set(CATCH_INSTALL_EXTRAS OFF)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../vendor/Catch2" "${CMAKE_CURRENT_BINARY_DIR}/Catch2" EXCLUDE_FROM_ALL)
target_compile_features(Catch2 PRIVATE cxx_std_17)

# Exercise production state, protocols and codec behavior; native hooks are checked against game binaries separately.
add_executable(fc_plugin_tests
    mpgc_battle_roles_tests.cpp
    mpgc_state_tests.cpp
    mpgc_participants_tests.cpp
    mpgc_protocol_tests.cpp
    ../patches/multiplayer_gc/state.cpp
    ../patches/multiplayer_gc/participants.cpp
    ../patches/multiplayer_gc/protocol.cpp
    sound_diagnostics_tests.cpp
    soldier_state_sync_tests.cpp
    spectator_motion_tests.cpp
    hero_animation_fix_tests.cpp
    melee_queue_timing_tests.cpp
    melee_upper_time_tests.cpp
    shot_sequence_tests.cpp
    input_sync_tests.cpp
    ../patches/input_sync/history.cpp
    native_movement_correction_tests.cpp
    collision_history_fix_tests.cpp
    weapon_swap_replay_fix_tests.cpp
    late_update_recovery_ordering_tests.cpp
    network_diagnostics_progress_tests.cpp
    update_scheduling_window_tests.cpp
    direct_transport_tests.cpp
    galaxy_buddies_state_tests.cpp
    lobby_player_list_tests.cpp
    voice_protocol_tests.cpp
    voice_codec_tests.cpp
    ../patches/lobby_player_list/protocol.cpp
    ../patches/voice_chat/transport.cpp
    ../patches/voice_chat/codec.cpp
    ../patches/hero_animation_fix/history.cpp
    ../patches/weapon_swap_replay_fix/local_ledger.cpp
    ../patches/weapon_swap_replay_fix/packed_ledger.cpp
    ../patches/update_scheduling/send_window.cpp
    ../patches/spectator_camera_smoothing/motion.cpp
)
target_link_libraries(fc_plugin_tests PRIVATE FusionCutter::SDK Catch2::Catch2WithMain fusioncutter_direct_transport opus)
target_compile_options(fc_plugin_tests PRIVATE /W4)

# The SDK supplies the CRT options; omit CMake's competing runtime flag.
set_target_properties(fc_plugin_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY ""
    CXX_EXTENSIONS OFF
    CXX_SCAN_FOR_MODULES OFF
)
add_test(NAME plugin_behavior COMMAND fc_plugin_tests)
