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

} // namespace cte::overlay
