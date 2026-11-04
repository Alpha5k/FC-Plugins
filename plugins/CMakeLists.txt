cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FusionCutterPluginLibraries LANGUAGES C CXX)
endif()

if(NOT TARGET fusioncutter_server_patches)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../patches" "${CMAKE_CURRENT_BINARY_DIR}/patches")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_library(FusionCutterServer MODULE server_classic.cpp)
    set(plugin_targets FusionCutterServer)
else()
    add_library(FusionCutterMultiplayer MODULE multiplayer.cpp)
    target_link_libraries(FusionCutterMultiplayer PRIVATE fusioncutter_multiplayer_patches)

    add_library(FusionCutterServer MODULE server.cpp)

    add_library(FusionCutterDiagnostics MODULE diagnostics.cpp)
    target_link_libraries(FusionCutterDiagnostics PRIVATE fusioncutter_diagnostics_patches)

    add_library(FusionCutterSocial MODULE social.cpp)
    target_link_libraries(FusionCutterSocial PRIVATE fusioncutter_social_patches)

    add_library(FusionCutterMPGC MODULE mpgc.cpp)
    target_link_libraries(FusionCutterMPGC PRIVATE fusioncutter_mpgc_patches)

    set(plugin_targets FusionCutterMultiplayer FusionCutterServer FusionCutterDiagnostics FusionCutterSocial
                       FusionCutterMPGC)
endif()
target_link_libraries(FusionCutterServer PRIVATE fusioncutter_server_patches)

foreach(plugin_target IN LISTS plugin_targets)
    string(REPLACE "FusionCutter" "FC." plugin_filename "${plugin_target}")
    target_compile_options(${plugin_target} PRIVATE /W4)

    # Use the C runtime options supplied by the FC SDK.
    set_target_properties(${plugin_target} PROPERTIES
        OUTPUT_NAME "${plugin_filename}"
        PDB_NAME "${plugin_filename}"
        MSVC_RUNTIME_LIBRARY ""
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES OFF
    )

    install(TARGETS ${plugin_target} RUNTIME DESTINATION . LIBRARY DESTINATION .)
    install(FILES "$<TARGET_PDB_FILE:${plugin_target}>" DESTINATION symbols
        CONFIGURATIONS Debug RelWithDebInfo)
endforeach()
