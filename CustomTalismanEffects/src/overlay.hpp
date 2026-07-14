#pragma once

// In-game config overlay: Dear ImGui drawn in its OWN transparent top-most window
// (D3D11 + DirectComposition) on its own thread -- it does NOT touch the game's
// swapchain, so it coexists with other overlay mods / frame-gen / Special K.
// Opens with the configured key (default Insert) or gamepad combo (default L3+R3),
// lets the player toggle talisman effects live, and enforces the family
// exclusivity / stacking rules.

namespace cte::overlay {

// Initialize MinHook, detour the gamepad APIs so the game reads a neutral pad
// while the menu is open (dinput8 GetDeviceState/GetDeviceData now; XInput at
// menu-open), and spawn the dedicated overlay thread that owns the window +
// D3D11 + DirectComposition + ImGui. Keyboard/mouse are captured focus-free by
// re-targeting the process's raw-input registration at the overlay window while
// open -- the overlay never takes focus, so frame-gen mods don't re-init. This
// owns the MinHook lifecycle; the worker does not need to call hooks::apply().
void setup();

// Re-copy the open key / gamepad combo from g_state into the overlay's runtime
// globals. setup() runs before params are ready (so before build_state() has
// applied the .ini's toggle_key/toggle_gamepad_combo to g_state) -- call this
// AFTER build_state() so the real configured combo takes effect instead of the
// hardcoded defaults setup() saw.
void sync_open_keys();

} // namespace cte::overlay
