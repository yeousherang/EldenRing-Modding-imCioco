#include "hooks.hpp"

#include <MinHook.h>

namespace cte::hooks {

bool init() {
    const MH_STATUS s = MH_Initialize();
    return s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED;
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
