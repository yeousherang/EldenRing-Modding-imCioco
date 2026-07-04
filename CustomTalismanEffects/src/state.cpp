#include "state.hpp"

#include <unordered_set>

namespace cte {

std::mutex g_state_mutex;
State      g_state;

void apply_exclusivity_locked(size_t idx) {
    if (g_state.allow_stacking) return;
    if (idx >= g_state.talismans.size()) return;
    const int group = g_state.talismans[idx].group;
    if (group < 0) return; // no family -> freely combinable
    for (size_t i = 0; i < g_state.talismans.size(); ++i) {
        if (i == idx) continue;
        if (g_state.talismans[i].group == group)
            g_state.talismans[i].enabled = false;
    }
}

void collapse_groups_locked() {
    std::unordered_set<int> seen; // groups already given their one enabled member
    for (auto& t : g_state.talismans) {
        if (!t.enabled || t.group < 0) continue;
        if (seen.count(t.group))
            t.enabled = false;      // a member of this family is already on
        else
            seen.insert(t.group);
    }
}

} // namespace cte
