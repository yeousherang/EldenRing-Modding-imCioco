cmake_minimum_required(VERSION 3.15)
project(CustomTalismanEffects LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# REQUIRED for ELDEN RING ABI compatibility.
add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)

# ---- third-party (fetched at configure time) ------------------------------
include(FetchContent)

# MinHook: hooks the DX12 swapchain / raw-input / XInput functions for the overlay.
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        master)

# Dear ImGui: the in-game overlay UI. Source-only; we compile the core + the
# DX12/Win32 backends straight into the DLL (see target_sources below).
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.90.9
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     "")

FetchContent_MakeAvailable(minhook)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
  FetchContent_Populate(imgui)
endif()

# libER as a git submodule under external/libER (param access), static-linked.
add_subdirectory("external/libER" "libER")

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/log.cpp
    src/offsets.cpp
    src/game_access.cpp
    src/state.cpp
    src/hooks.cpp
    src/overlay.cpp
    src/messages.cpp
    src/session_store.cpp
    # Dear ImGui core + DX11/Win32 backends compiled into the DLL. The overlay
    # renders in its OWN window (D3D11 + DirectComposition), not the game's DX12
    # swapchain, so it never fights other overlay mods / frame-gen / Special K.
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${minhook_SOURCE_DIR}/include
)

# We feed the gamepad ourselves (from the hooked XInput), so disable ImGui's
# built-in Win32 gamepad polling.
target_compile_definitions(${PROJECT_NAME} PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

# Static-link libER + MinHook -> a single self-contained DLL. The overlay uses
# D3D11 + DXGI + DirectComposition (own window), not D3D12. dinput8/dxguid are
# for the DirectInput8 pad-neutralization detours (GUID_SysKeyboard, the IIDs).
target_link_libraries(${PROJECT_NAME} PRIVATE
    libER_static minhook d3d11 dxgi dcomp dinput8 dxguid)

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "CustomTalismanEffects")
