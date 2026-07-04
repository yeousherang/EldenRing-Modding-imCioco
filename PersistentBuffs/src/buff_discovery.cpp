#include "buff_discovery.hpp"

#include "buff_filters.hpp"   // sp_row, is_self_buff, is_beneficial_buff, is_grease, kSortGroup*/kChainMaxDepth
#include "utils.hpp"          // flog

#include <vector>

#include <param/param.hpp>

namespace pb {

std::unordered_set<int> g_tracked_speffects;

namespace {

// EquipParamGoods.sortGroupId categories (from InfiniteWeaponBuffs' RE +
// VirusAlex/MapForGoblins data). We treat 20 (buff consumables) and 70 (greases)
// as buff sources. To persist another category later, add a scan loop below:
//   10  = stat boosts: Rune Arc, GREAT RUNE effects, Starlight Shards
//         <-- add a 4th loop here to persist great-rune / rune-arc buffs
//   20  = buff/heal consumables (Exalted Flesh, cured meats, livers, boluses)  [TRACKED]
//   50  = throwables / thrown pots (incl. DLC Golden Vow 2003170)
//   60  = reusable toggles (Mimic Veil, shackles)
//   70  = greases (all vanilla + DLC)                                          [TRACKED]
//   80  = utilities (soap, glowstone)
//   200 = fingers / effigies

// Built-in id lists (mirror IWB defaults; no .ini needed):
//   130     = Spectral Steed Whistle (Torrent) -- horse summon, never a buff.
//   2003170 = DLC "Golden Vow" thrown pot -- buffs the thrower via a bullet, so
//             it isn't a plain sortGroup-20 consumable; include it explicitly.
const std::unordered_set<int> kHorseSummonGoods = { 130 };
const std::unordered_set<int> kExtraConsumables = { 2003170 };

from::paramdef::BULLET_PARAM_ST* bullet_row(int id) {
    if (id < 0) return nullptr;
    auto [row, ok] = from::param::Bullet[id];
    return ok ? &row : nullptr;
}
from::paramdef::BEHAVIOR_PARAM_ST* behavior_row(int id) {
    if (id < 0) return nullptr;
    { auto [row, ok] = from::param::BehaviorParam_PC[id]; if (ok) return &row; }
    { auto [row, ok] = from::param::BehaviorParam[id];    if (ok) return &row; }
    return nullptr;
}

void add_bullet_speffects(int bulletId, std::vector<int>& out) {
    auto* bl = bullet_row(bulletId);
    if (!bl) return;
    const int ids[] = { bl->spEffectIDForShooter, bl->spEffectId0,
                        bl->spEffectId1, bl->spEffectId2,
                        bl->spEffectId3, bl->spEffectId4 };
    for (int s : ids) if (s >= 0) out.push_back(s);
}

// Resolve the SpEffect(s) a goods row can apply: its refId(s) as a SpEffect, as a
// Bullet when refCategory==1 (projectile), and the behaviorId -> BehaviorParam
// path. Mirrors IWB's gather_goods_entry_speffects.
void gather_goods_entry_speffects(const from::paramdef::EQUIP_PARAM_GOODS_ST& row,
                                  std::vector<int>& out) {
    const int refs[2] = { row.refId_default, row.refId_1 };
    for (int r : refs) {
        if (r < 0) continue;
        if (sp_row(r)) out.push_back(r);                        // refId as SpEffect
        if (row.refCategory == 1) add_bullet_speffects(r, out); // refId as Bullet
    }
    if (row.behaviorId > 0) {
        if (auto* b = behavior_row(row.behaviorId)) {
            if (b->refType == 2 && b->refId >= 0) out.push_back(b->refId);
            else if (b->refType == 1)             add_bullet_speffects(b->refId, out);
        }
    }
}

// Walk the SpEffect replace/cycle chain, collecting EVERY reachable id (any
// duration -- unlike IWB's timed-only walk, since PersistentBuffs must keep
// buffs an infinite-duration mod like IWB may already have patched to -1).
void collect_all_chain(int startId, std::unordered_set<int>& out, int depth = 0) {
    if (startId < 0 || depth > kChainMaxDepth) return;
    if (!out.insert(startId).second) return; // already visited
    auto* r = sp_row(startId);
    if (!r) return;
    collect_all_chain(r->replaceSpEffectId,         out, depth + 1);
    collect_all_chain(r->cycleOccurrenceSpEffectId, out, depth + 1);
}

void gather_magic_entries(const from::paramdef::MAGIC_PARAM_ST& row,
                          std::vector<int>& out) {
    const int refs[] = {
        row.refId1, row.refId2, row.refId3, row.refId4,  row.refId5,
        row.refId6, row.refId7, row.refId8, row.refId9,  row.refId10,
    };
    for (int r : refs) if (r >= 0) out.push_back(r);
}

} // namespace

void build_tracked_speffects() {
    g_tracked_speffects.clear();
    int n_grease = 0, n_spell = 0, n_consum = 0, n_great_rune = 0;

    // 1. Greases (weapon/shield buffs) -- trusted narrow source, no field filter.
    for (auto [id, row] : from::param::EquipParamGoods) {
        if (!is_grease(row)) continue;
        for (int r : { row.refId_default, row.refId_1 })
            if (r >= 0 && sp_row(r) && g_tracked_speffects.insert(r).second) ++n_grease;
    }

    // 2. Spell buffs (sorceries / incantations) -- trusted (Magic refId1..10).
    for (auto [id, row] : from::param::Magic) {
        std::vector<int> entries;
        gather_magic_entries(row, entries);
        for (int r : entries)
            if (sp_row(r) && g_tracked_speffects.insert(r).second) ++n_spell;
    }

    
    // 3. Consumables (buff/heal foods) -- filtered self + beneficial (drops cures
    //    and debuff consumables), following the replace/cycle chain plus the
    //    bullet/behavior indirection so projectile pots (DLC Golden Vow) count.
    for (auto [id, row] : from::param::EquipParamGoods) {
        const bool in_scope = static_cast<int>(row.sortGroupId) == kSortGroupConsumable ||
        kExtraConsumables.count(static_cast<int>(id)) != 0;
        if (!in_scope) continue;
        if (row.isSummonHorse || kHorseSummonGoods.count(static_cast<int>(id))) continue;
        
        std::vector<int> entries;
        gather_goods_entry_speffects(row, entries);
        std::unordered_set<int> chain;
        for (int e : entries) collect_all_chain(e, chain);
        for (int node : chain) {
            const auto* r = sp_row(node);
            if (is_self_buff(r) && is_beneficial_buff(r) &&
            g_tracked_speffects.insert(node).second)
            ++n_consum;
        }
    }


    // 4. Great Runes
    const int great_rune_ids[] = { 602, 603, 604, 605, 606, 607 };
    for (int id : great_rune_ids) {
        if (sp_row(id)) {
            g_tracked_speffects.insert(id);
            ++n_great_rune;
        }
    }
    
    flog("buff filter: tracked %zu SpEffect(s) [greases %d, spells %d, consumables %d, great_rune %d] "
         "-- source-category allowlist",
         g_tracked_speffects.size(), n_grease, n_spell, n_consum, n_great_rune);
}

} // namespace pb
