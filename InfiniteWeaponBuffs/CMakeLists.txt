cmake_minimum_required(VERSION 3.15)
project(InfiniteWeaponBuffs LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# REQUIRED for ELDEN RING ABI compatibility -- libER's STL containers
# only match the game's layout when iterator debugging is disabled.
add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)

# libER lives as a git submodule under external/libER:
#   git submodule add https://github.com/Dasaav-dsv/libER external/libER
#   git submodule update --init --recursive
add_subdirectory("external/libER" "libER")

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE src)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

# Link libER STATICALLY -> a single self-contained InfiniteWeaponBuffs.dll
# with no libER.dll dependency. (Dynamic linking caused LoadLibrary error
# 126 because libER.dll wasn't on the mod loader's search path.)
target_link_libraries(${PROJECT_NAME} PRIVATE libER_static)

# Final artifact: InfiniteWeaponBuffs.dll
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "InfiniteWeaponBuffs")
