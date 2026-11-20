cmake_minimum_required(VERSION 3.24)
project(RefunctUtility VERSION 1.0.0 LANGUAGES C CXX)

option(REFUNCT_BUILD_TESTS "Build portable algorithm tests" ON)

add_library(refunct_pure STATIC src/pure.cpp src/section_heights.cpp)
target_include_directories(refunct_pure PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(refunct_pure PUBLIC cxx_std_20)

if(REFUNCT_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        add_executable(refunct_pure_tests tests/pure_tests.cpp)
        target_link_libraries(refunct_pure_tests PRIVATE refunct_pure)
        add_test(NAME refunct_pure_tests COMMAND refunct_pure_tests)
        add_executable(refunct_section_heights_tests tests/section_heights_tests.cpp)
        target_link_libraries(refunct_section_heights_tests PRIVATE refunct_pure)
        add_test(NAME refunct_section_heights_tests COMMAND refunct_section_heights_tests)
        add_executable(refunct_ghost_tests tests/ghost_data_tests.cpp)
        target_include_directories(refunct_ghost_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_compile_features(refunct_ghost_tests PRIVATE cxx_std_20)
        add_test(NAME refunct_ghost_tests COMMAND refunct_ghost_tests)
    endif()
endif()

if(NOT WIN32)
    message(STATUS "Non-Windows host: building portable tests only. ASI target needs Windows/x86.")
    return()
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Refunct is 32-bit: configure Visual Studio with -A Win32, not x64.")
endif()

# Use a local MinHook checkout if supplied, otherwise fetch a fixed released version.
set(REFUNCT_MINHOOK_SOURCE "" CACHE PATH "Path to a local MinHook source checkout (optional)")
if(NOT REFUNCT_MINHOOK_SOURCE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/CMakeLists.txt")
    set(REFUNCT_MINHOOK_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook")
endif()
if(REFUNCT_MINHOOK_SOURCE)
    if(NOT EXISTS "${REFUNCT_MINHOOK_SOURCE}/CMakeLists.txt")
        message(FATAL_ERROR "REFUNCT_MINHOOK_SOURCE must point to the MinHook source directory")
    endif()
    add_subdirectory("${REFUNCT_MINHOOK_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/minhook")
else()
    include(FetchContent)
    FetchContent_Declare(
        minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG v1.3.4
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(minhook)
endif()

# Fail early if the new source files were not fully extracted.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/jumppads.cpp")
    message(FATAL_ERROR "src/jumppads.cpp is missing. Extract the Jump Pad feature ZIP into the project before building.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/water.cpp")
    message(FATAL_ERROR "src/water.cpp is missing. Extract the Water Colours ZIP into the project before building.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/time_of_day.cpp")
    message(FATAL_ERROR "Missing src/time_of_day.cpp (Time Customisation update not extracted)")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/maps.cpp")
    message(FATAL_ERROR "Missing src/maps.cpp (Maps menu update not extracted)")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/fog.cpp")
    message(FATAL_ERROR "Missing src/fog.cpp (Fog Haze Colours update not extracted)")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/shadows.cpp")
    message(FATAL_ERROR "Missing src/shadows.cpp (Shadow Customisation update not extracted)")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/pillars.cpp")
    message(FATAL_ERROR "Missing src/pillars.cpp (Pillar Colours update not extracted)")
endif()

add_library(RefunctUtility SHARED
    src/bootstrap.cpp
    src/checkpoint.cpp
    src/compatibility.cpp
    src/core.cpp
    src/game_tick.cpp
    src/performance.cpp
    src/hud.cpp
    src/input.cpp
    src/menu.cpp
    src/movement.cpp
    src/practice.cpp
    src/sections.cpp
    src/records.cpp
    src/ghost.cpp
    src/reflection.cpp
    src/restart.cpp
    src/state.cpp
    src/storage.cpp
    src/customisation_profiles.cpp
    src/world_profiles.cpp
    src/hotkey_profiles.cpp
    src/hud_profiles.cpp
    src/timer.cpp
    src/tiles.cpp
    src/cubes.cpp
    src/tubes.cpp
    src/platforms.cpp
    src/pillars.cpp
    src/world_object_removal.cpp
    src/jumppads.cpp
    src/water.cpp
    src/time_of_day.cpp
    src/sky.cpp
    src/fog.cpp
    src/shadows.cpp
    src/maps.cpp
)
# This message must appear in configure.log. If absent, the project-root
# CMakeLists.txt was not replaced by the Time Customisation update.
message(STATUS "Refunct time-of-day: src/time_of_day.cpp included in RefunctUtility ASI")
message(STATUS "Refunct custom maps: src/maps.cpp included in RefunctUtility ASI")
target_compile_features(RefunctUtility PRIVATE cxx_std_20)
target_include_directories(RefunctUtility PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(RefunctUtility PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(RefunctUtility PRIVATE minhook refunct_pure bcrypt Comdlg32)
if(MSVC)
    target_compile_options(RefunctUtility PRIVATE /W4 /permissive- /EHsc)
endif()
set_target_properties(RefunctUtility PROPERTIES PREFIX "" OUTPUT_NAME "RefunctUtility" SUFFIX ".asi")
install(TARGETS RefunctUtility RUNTIME DESTINATION . LIBRARY DESTINATION .)

# Control build: untouched original single-file source, same compiler and MinHook.
# Never install it together with the modular ASI; the loader would load both.
option(REFUNCT_BUILD_BASELINE "Compile original v9.30 for comparison" OFF)
if(REFUNCT_BUILD_BASELINE)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/reference/original_mod_v9.30.cpp")
        message(FATAL_ERROR "REFUNCT_BUILD_BASELINE requires reference/original_mod_v9.30.cpp, which is not included in this archive.")
    endif()
    add_library(RefunctUtilityBaseline SHARED reference/original_mod_v9.30.cpp)
    target_compile_features(RefunctUtilityBaseline PRIVATE cxx_std_20)
    target_compile_definitions(RefunctUtilityBaseline PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(RefunctUtilityBaseline PRIVATE minhook)
    if(MSVC)
        target_compile_options(RefunctUtilityBaseline PRIVATE /W4 /permissive- /EHsc)
    endif()
    set_target_properties(RefunctUtilityBaseline PROPERTIES
        PREFIX "" OUTPUT_NAME "RefunctUtilityBaseline" SUFFIX ".asi")
endif()
