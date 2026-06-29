cmake_minimum_required(VERSION 3.15)
project(PersistentBuffs CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- MinHook (static), used for function hooking ----------------------------
add_subdirectory(external/minhook)

# --- the mod DLL ------------------------------------------------------------
add_library(PersistentBuffs SHARED
    src/main.cpp
)

target_include_directories(PersistentBuffs PRIVATE
    external/minhook/include
)

target_link_libraries(PersistentBuffs PRIVATE minhook)

# Single self-contained DLL named exactly "PersistentBuffs.dll".
set_target_properties(PersistentBuffs PROPERTIES
    PREFIX ""
    OUTPUT_NAME "PersistentBuffs"
)
