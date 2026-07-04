#pragma once

// Tiny MinHook wrapper. Same queue-then-apply model MapForGoblins' `modutils`
// uses: create() queues a hook, apply() commits all queued hooks at once.

namespace cte::hooks {

// MH_Initialize. Safe to call once; returns false on failure.
bool init();

// MH_CreateHook + MH_QueueEnableHook. `original` receives the trampoline.
bool create(void* target, void* detour, void** original);

// MH_ApplyQueued -- enable every queued hook.
bool apply();

// MH_Uninitialize (best-effort; called on shutdown paths, usually never).
void deinit();

} // namespace cte::hooks
