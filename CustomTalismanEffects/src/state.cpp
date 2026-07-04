#include "state.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace cte {

namespace {
std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

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

void sort_talismans_locked() {
    auto& v = g_state.talismans;
    switch (g_state.sort_mode) {
    case 1: // Name (alphabetical)
        std::stable_sort(v.begin(), v.end(), [](const Talisman& a, const Talisman& b) {
            return to_lower(a.name) < to_lower(b.name);
        });
        break;
    case 2: // In-game menu order
        std::stable_sort(v.begin(), v.end(), [](const Talisman& a, const Talisman& b) {
            return a.sort_id < b.sort_id;
        });
        break;
    default: // 0 = Talisman ID
        std::stable_sort(v.begin(), v.end(), [](const Talisman& a, const Talisman& b) {
            return a.accessory_id < b.accessory_id;
        });
        break;
    }
}

} // namespace cte
