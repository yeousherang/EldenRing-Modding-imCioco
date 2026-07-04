#include "hooks.hpp"

#include <MinHook.h>

namespace cte::hooks {

bool init() {
    return MH_Initialize() == MH_OK;
}

bool create(void* target, void* detour, void** original) {
    if (MH_CreateHook(target, detour, original) != MH_OK) return false;
    return MH_QueueEnableHook(target) == MH_OK;
}

bool apply() {
    return MH_ApplyQueued() == MH_OK;
}

void deinit() {
    MH_Uninitialize();
}

} // namespace cte::hooks
