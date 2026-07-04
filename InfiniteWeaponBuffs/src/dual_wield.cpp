#include "dual_wield.hpp"
#include "speffect_lookup.hpp"
#include "buff_filters.hpp"
#include "utils.hpp"

#include <param/param.hpp>

namespace iwb {

int sp_enchant_id(int spId) {
    auto* r = sp_row(spId);
    if (!r || r->vfxId < 0) return -1;
    auto [v, ok] = from::param::SpEffectVfxParam[r->vfxId];
    if (!ok) return -1;
    const int e = v.soulParamIdForWepEnchant;        // unsigned char (0/255 = none)
    return (e > 0 && e < 255) ? e : -1;
}

float sp_endurance(int spId) {
    auto* r = sp_row(spId);
    return r ? r->effectEndurance : 0.0f;
}

void build_dualwield_mirror(const std::vector<HandPair>& artPairs,
                            const std::unordered_set<int>& protectedSp,
                            std::unordered_map<int, int>& mirror,
                            bool logDetail) {
    // Index every Left (wepParamChange==2) enchant effect by its enchant id, so a
    // grease Right whose Left isn't in its own refs can still find a partner.
    std::unordered_map<int, std::vector<int>> leftByEnchant;
    for (auto [id, row] : from::param::SpEffectParam) {
        if (row.wepParamChange != 2) continue;
        const int e = sp_enchant_id(static_cast<int>(id));
        if (e >= 0) leftByEnchant[e].push_back(static_cast<int>(id));
    }

    // Nearest-(vanilla)-duration Left sharing the Right's enchant id -- this
    // disambiguates the full vs. drawstring variants, which share an element id
    // but differ in duration (60s vs 11s, etc.).
    auto find_left = [&](int rightId) -> int {
        const int e = sp_enchant_id(rightId);
        if (e < 0) return -1;
        const auto it = leftByEnchant.find(e);
        if (it == leftByEnchant.end()) return -1;
        const float rd = sp_endurance(rightId);
        int best = -1;
        float bestDiff = 1e30f;
        for (int lid : it->second) {
            const float ld = sp_endurance(lid);
            const float diff = ld > rd ? ld - rd : rd - ld;
            if (diff < bestDiff) { bestDiff = diff; best = lid; }
        }
        return best;
    };

    auto try_add = [&](int r, int l, const char* src) {
        if (r < 0 || l < 0 || r == l || !sp_row(r) || !sp_row(l)) return;
        if (protectedSp.count(r) || protectedSp.count(l)) return;
        if (is_system_effect(r) || is_system_effect(l)) return;
        const bool added = mirror.emplace(r, l).second;
        if (logDetail)
            flog("dual pair %-12s right=%d -> left=%d (enchant=%d, curCyc=%d, curInterval=%.2f)%s",
                 src, r, l, sp_enchant_id(r), sp_row(r)->cycleOccurrenceSpEffectId,
                 sp_row(r)->motionInterval, added ? "" : " [dup, kept first]");
    };

    // Greases: reuse is_grease discovery; pair each Right to a Left.
    for (auto [id, row] : from::param::EquipParamGoods) {
        if (!is_grease(row)) continue;
        const int refs[2] = { row.refId_default, row.refId_1 };
        std::vector<int> rights, lefts;
        for (int rf : refs) {
            auto* sr = sp_row(rf);
            if (!sr || sr->effectEndurance <= 0.0f) continue;
            if (sr->wepParamChange == 1)      rights.push_back(rf);
            else if (sr->wepParamChange == 2) lefts.push_back(rf);
        }
        for (int rg : rights) {
            int lf = -1;
            const char* src = "grease/goods";
            const int e = sp_enchant_id(rg);             // primary: Left in same goods
            for (int c : lefts) if (sp_enchant_id(c) == e) { lf = c; break; }
            if (lf < 0) { lf = find_left(rg); src = "grease/sig"; } // fallback: index
            if (lf < 0) {
                if (logDetail)
                    flog("dual MISS  grease goods=%d right=%d (no left, enchant=%d)",
                         static_cast<int>(id), rg, e);
                continue;
            }
            try_add(rg, lf, src);
        }
    }

    // Weapon-skill enchants: explicit pairs (built-in + .ini extra_pairs).
    for (const auto& p : artPairs) try_add(p.right, p.left, "art");
}

} // namespace iwb
