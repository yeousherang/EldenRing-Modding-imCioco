#include "buff_filters.hpp"
#include "buff_discovery.hpp"
#include "config.hpp"
#include "utils.hpp"

#include <unordered_set>

namespace pb {

from::paramdef::SP_EFFECT_PARAM_ST* sp_row(int id) {
    if (id < 0) return nullptr;
    auto [row, ok] = from::param::SpEffectParam[id];
    return ok ? &row : nullptr;
}

// Applied to the caster themselves? (self / player target). Ported verbatim from
// InfiniteWeaponBuffs -- used by buff_discovery to keep only self-buffs among
// consumables. Ash/grease/spell sources are trusted and skip this.
bool is_self_buff(const from::paramdef::SP_EFFECT_PARAM_ST* r) {
    return r && (r->effectTargetSelf || r->effectTargetPlayer);
}

// Weapon/shield grease? Ported verbatim from InfiniteWeaponBuffs.
bool is_grease(const from::paramdef::EQUIP_PARAM_GOODS_ST& row) {
    return row.isEnhance || row.isShieldEnchant ||
           static_cast<int>(row.sortGroupId) == kSortGroupGrease;
}

// Engine state / animation / environment effects that must never be persisted --
// the engine applies & clears them itself, so re-applying sticks the player in
// that state. Matched by id only (robust even if SpEffectParam field offsets
// drift across game versions). This is the same id set InfiniteWeaponBuffs uses
// (is_system_effect), plus the two evergaol ids PersistentBuffs already carried.
// The headline case (confirmed in-game via log_effects): 9621 "Disallow Hostile
// Actions" -- Roundtable Hold's no-combat block, which got snapshotted in the
// Hold and re-applied on the way out, locking you out of attacks/spells/arts.
// Ids from soulsmods/Paramdex (ER SpEffectParam names).
bool is_system_effect(int id) {
    if (id >= 100000 && id <= 100999)  return true; // [HKS] state block + grace
    if (id >= 131    && id <= 147)     return true; // jump / attack anim states
    if (id >= 170    && id <= 176)     return true; // guard anim states
    // Spirit-ash summon effects (20207000 "[Spirit Summon] Messmer Soldier Ashes"
    // etc.). These are transient summon state, NOT player buffs -- re-applying one
    // makes the game think a summon is still active, so the bell rings but nothing
    // spawns until restart. The 202xxxxx range is spirit summons; consumable item
    // buffs we DO want live in 205xxxxx, so this range is safe. Confirmed in-game.
    if (id >= 20200000 && id <= 20299999) return true; // spirit ash summon state
    // Negative status ailments (Hemorrhage/Poison/Scarlet Rot/Frostbite/Madness/
    // Sleep/Blight) -- their proc/DoT SpEffects. NEVER persist these: re-applying
    // one after respawn re-inflicts the ailment. The headline case (Nexus bug
    // report): DEATH BLIGHT instantly kills, and re-applying it on respawn caused
    // an inescapable death loop (only removable by quitting + deleting the mod).
    // The allowlist should already drop debuffs, but block them by id as a hard,
    // drift-proof safety net. Ranges/ids from soulsmods/Paramdex (ER SpEffectParam):
    //   70          "Blight Effect"
    //   500-507     status build-up / behavior states (Cycled/Presence of/Behavior)
    //   6400-6805   ailment proc effects ("<Ailment> - Type N - Special M")
    if (id == 70)                       return true; // Blight Effect
    if (id >= 500  && id <= 507)        return true; // status build-up states
    if (id >= 6400 && id <= 6810)       return true; // ailment procs (incl. death blight)
    switch (id) {
        case 45:      // [HKS] Counter Frames
        case 514:     // evargaol
        case 190:     // evargaol
        case 8001:    // [HKS] Is Stealth
        case 9540:    // Spirit Summon Active -- re-applying blocks re-summoning
        case 10665:   // [HKS] Event action not possible
        case 530007:  // [HKS] Goods stamina cost
        case 530012:  // [HKS] Goods stamina cost
        case 9621:    // Disallow Hostile Actions (Roundtable Hold no-combat)
        case 4600:    // Wet (Rain) -- environment, shouldn't follow you out
            return true;
        default:
            return false;
    }
}

// Does this SpEffect actually improve a combat/vitality stat? This is the robust
// auto-filter: engine state / evergaol / no-combat / animation effects change no
// stat, so they're rejected here WITHOUT needing their ids -- which is what stops
// new soft-lock effects from leaking through as the game patches. Copied verbatim
// from InfiniteWeaponBuffs' is_beneficial_buff (proven on this game build). An
// effect that buffs at least one stat counts, even if it also has a downside.
// Known gap: a few buffs (some AoW self-buffs, poise-only effects) don't show up
// in these fields -- those are handled by the g_always_persist / g_force_persist
// allowlists, which bypass this check.
bool is_beneficial_buff(const from::paramdef::SP_EFFECT_PARAM_ST* r) {
    if (!r) return false;
    // Attack up (rates >1, flat >0).
    if (r->physicsAttackPowerRate > 1.f || r->magicAttackPowerRate > 1.f ||
        r->fireAttackPowerRate > 1.f    || r->thunderAttackPowerRate > 1.f)
        return true;
    if (r->physicsAttackPower > 0 || r->magicAttackPower > 0 ||
        r->fireAttackPower > 0    || r->thunderAttackPower > 0)
        return true;
    if (r->physicsAttackRate > 1.f || r->magicAttackRate > 1.f ||
        r->fireAttackRate > 1.f    || r->thunderAttackRate > 1.f ||
        r->staminaAttackRate > 1.f)
        return true;
    // Defense up (rates >1, flat >0) / damage taken down (cut rates <1).
    if (r->physicsDiffenceRate > 1.f || r->magicDiffenceRate > 1.f ||
        r->fireDiffenceRate > 1.f    || r->thunderDiffenceRate > 1.f)
        return true;
    if (r->physicsDiffence > 0 || r->magicDiffence > 0 ||
        r->fireDiffence > 0    || r->thunderDiffence > 0)
        return true;
    if (r->slashDamageCutRate < 1.f  || r->blowDamageCutRate < 1.f   ||
        r->thrustDamageCutRate < 1.f || r->neutralDamageCutRate < 1.f ||
        r->magicDamageCutRate < 1.f  || r->fireDamageCutRate < 1.f   ||
        r->thunderDamageCutRate < 1.f)
        return true;
    // Vitality / regen.
    if (r->maxHpRate > 1.f || r->maxMpRate > 1.f || r->maxStaminaRate > 1.f)
        return true;
    if (r->hpRecoverRate > 0.f || r->mpRecoverChangeSpeed > 0 ||
        r->staminaRecoverChangeSpeed > 0)
        return true;
    // Status resistance up.
    if (r->registPoizonChangeRate > 1.f || r->registDiseaseChangeRate > 1.f ||
        r->registBloodChangeRate > 1.f  || r->registCurseChangeRate > 1.f)
        return true;
    // Rune acquisition up (Gold-Pickled Fowl Foot etc.).
    if (r->haveSoulRate > 1.f || r->soulRate > 0.f)
        return true;
    return false;
}

bool is_persistable(int id) {
    if (g_force_persist.count(id) || g_always_persist.count(id)) return true;
    if (is_system_effect(id))                                   return false;
    // Source-category allowlist: keep only greases / spell buffs / consumables
    // discovered at startup (see buff_discovery). Drops talismans, weapon-innate
    // passives, great runes, physick, environment/state effects.
    return g_tracked_speffects.count(id) != 0;
}

void log_effect_changes(const std::vector<int>& cur_vec) {
    static std::unordered_set<int> prev;
    std::unordered_set<int> cur(cur_vec.begin(), cur_vec.end());
    std::string added, removed;
    for (int id : cur)  if (!prev.count(id)) { added   += named(id); added   += ' '; }
    for (int id : prev) if (!cur.count(id))  { removed += named(id); removed += ' '; }
    if (!added.empty() || !removed.empty())
        flog("effects changed: +[ %s] -[ %s] -> active now [ %s]",
             added.c_str(), removed.c_str(), join_ids(cur_vec).c_str());
    prev = std::move(cur);
}

void log_filter_changes(const std::vector<int>& cur_vec) {
    static std::unordered_set<int> prev_dropped;
    std::unordered_set<int> dropped;
    std::string line;
    for (int id : cur_vec) {
        if (is_persistable(id)) continue;
        dropped.insert(id);
        // Gate is now source-based: either a hard-blocked system id, or an id
        // that isn't sourced from a grease/spell/consumable/ash (add it to
        // force_persist_ids to keep it anyway).
        const char* why = is_system_effect(id) ? "system" : "not-tracked";
        line += named(id); line += '('; line += why; line += ") ";
    }
    if (dropped != prev_dropped)
        flog("filter: dropping %zu effect(s): [ %s]", dropped.size(), line.c_str());
    prev_dropped = std::move(dropped);
}

} // namespace pb
