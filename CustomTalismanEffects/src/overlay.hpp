#pragma once

// In-game config overlay: Dear ImGui drawn in its OWN transparent top-most window
// (D3D11 + DirectComposition) on its own thread -- it does NOT touch the game's
// swapchain, so it coexists with other overlay mods / frame-gen / Special K.
// Opens with the configured key (default Insert) or gamepad combo (default L3+R3),
// lets the player toggle talisman effects live, and enforces the family
// exclusivity / stacking rules.

namespace cte::overlay {

// Queue MinHook hooks on the input APIs (GetRawInputData / SetCursorPos /
// ClipCursor / XInputGetState) and spawn the dedicated overlay thread that owns
// the window + D3D11 + DirectComposition + ImGui. The hooks are committed by
// cte::hooks::apply(). Call before apply().
void setup();

// Re-copy the open key / gamepad combo from g_state into the overlay's runtime
// globals. setup() runs before params are ready (so before build_state() has
// applied the .ini's toggle_key/toggle_gamepad_combo to g_state) -- call this
// AFTER build_state() so the real configured combo takes effect instead of the
// hardcoded defaults setup() saw.
void sync_open_keys();

} // namespace cte::overlay
