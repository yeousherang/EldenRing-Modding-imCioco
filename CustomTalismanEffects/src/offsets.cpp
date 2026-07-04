#include "offsets.hpp"

namespace cte {

// Resolved at startup by AOB scan (see main.cpp). Null until then.
ApplySpEffect_t  g_apply  = nullptr;
RemoveSpEffect_t g_remove = nullptr;

} // namespace cte
