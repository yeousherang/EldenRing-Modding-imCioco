#pragma once

// In-game config overlay: Dear ImGui drawn into Elden Ring's own DX12 swapchain.
// Opens with the configured key (default Insert) or gamepad combo (default L3+R3),
// lets the player toggle talisman effects live, and enforces the family
// exclusivity / stacking rules. Ported/trimmed from ERR-MapForGoblins-DLL.

namespace cte::overlay {

// Capture the DXGI swapchain + D3D12 command-queue vtables (via a throwaway
// device/swapchain) and QUEUE MinHook hooks on Present / ResizeBuffers /
// ExecuteCommandLists (+ raw input + XInput). The hooks are committed by
// cte::hooks::apply(). Safe no-op (logs) if D3D12 init fails. Call before apply().
void setup();

} // namespace cte::overlay
