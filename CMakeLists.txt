cmake_minimum_required(VERSION 3.28)

project(FusionCutterPlugins LANGUAGES C CXX)

include(CTest)
find_package(FusionCutter CONFIG REQUIRED)

add_subdirectory(patches)
add_subdirectory(plugins)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
