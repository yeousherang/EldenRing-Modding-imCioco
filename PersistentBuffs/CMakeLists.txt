cmake_minimum_required(VERSION 3.15)
project(PersistentBuffs LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# REQUIRED for ELDEN RING ABI compatibility -- libER's STL containers only match
# the game's layout when iterator debugging is disabled (same as the sibling mods).
add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)

# --- MinHook (static), used for function hooking ----------------------------
add_subdirectory(external/minhook)

# --- libER (static): SpEffectParam access for the buff filter ----------------
# Lives as a git submodule under external/libER (same repo/commit the sibling
# mods use):
#   git submodule add https://github.com/Dasaav-dsv/libER external/libER
#   git submodule update --init --recursive
add_subdirectory(external/libER libER)

# --- the mod DLL ------------------------------------------------------------
add_library(PersistentBuffs SHARED
    src/ini.hpp
    src/scan.hpp
    src/utils.hpp
    src/utils.cpp
    src/config.hpp
    src/config.cpp
    src/offsets.hpp
    src/offsets.cpp
    src/game_access.hpp
    src/game_access.cpp
    src/buff_filters.hpp
    src/buff_filters.cpp
    src/buff_discovery.hpp
    src/buff_discovery.cpp
    src/buff_timing.hpp
    src/buff_timing.cpp
    src/weapon_memory.hpp
    src/weapon_memory.cpp
    src/session_store.hpp
    src/session_store.cpp
    src/main.cpp
)

target_include_directories(PersistentBuffs PRIVATE
    src
    external/minhook/include
)

# Link both statically -> a single self-contained PersistentBuffs.dll.
target_link_libraries(PersistentBuffs PRIVATE minhook libER_static)

# Single self-contained DLL named exactly "PersistentBuffs.dll".
set_target_properties(PersistentBuffs PROPERTIES
    PREFIX ""
    OUTPUT_NAME "PersistentBuffs"
)
