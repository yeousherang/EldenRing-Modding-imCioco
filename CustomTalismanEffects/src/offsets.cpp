#include "offsets.hpp"

namespace cte {

// Resolved at startup by AOB scan (see main.cpp). Null until then.
ApplySpEffect_t  g_apply  = nullptr;
RemoveSpEffect_t g_remove = nullptr;

// Address of the GameDataMan global pointer (see main.cpp resolve_global_ptr).
// 0 until resolved / if the AOB drifts.
uintptr_t g_gamedataman_var = 0;

} // namespace cte
