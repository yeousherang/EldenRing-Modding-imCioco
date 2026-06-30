// ============================================================
//  ER Infinite Weapon Buffs - a libER param-patcher DLL
//
//  Everything is done by rewriting param tables in memory after
//  the game loads them -- no regulation.bin edits. Two passes:
//
//    1. EquipParamWeapon.isEnhance = true on every weapon
//       => any grease / spell buff can be applied to any weapon.
//
//    2. SpEffectParam.effectEndurance = <configured value> for the
//       buff SpEffects discovered per category
//       => greases / spell buffs / consumable buffs last as long
//          as you want (-1 = permanent).
//
//  Categories are derived live from the params, code-driven (no
//  goodsType lists to maintain):
//    * greases     = goods that enchant a weapon/shield (isEnhance /
//                    isShieldEnchant) or sit in sort group 70.
//    * spell buffs = SpEffects referenced by the Magic param.
//    * consumables = buff/heal goods (sort group 20) + an explicit
//                    allowlist (e.g. the DLC Golden Vow pot). The buff
//                    SpEffect is found by following the item's
//                    behavior -> bullet -> shooter chain and the
//                    SpEffect replace/cycle chain, then keeping only
//                    self/player-targeted timed effects.
//
//  Horse-summon items (Spectral Steed Whistle) are protected: every
//  SpEffect they can reach is collected up-front and never patched, so
//  Torrent's "active" state can never get stuck.
//
//  Writes a log next to the DLL (logs/InfiniteWeaponBuffs.log) every
//  launch so you can see exactly what happened. Run OFFLINE only.
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// libER: SoloParamRepository::wait_for_params
#include <coresystem/cs_param.hpp>
// libER: from::param::EquipParamWeapon / SpEffectParam, etc.
#include <param/param.hpp>

#include "ini.hpp"

namespace {

HINSTANCE g_hinst = nullptr;
bool      g_debug = false;

// ---- known vanilla item ids --------------------------------
// Used in addition to the live param flags, so categorization stays
// correct even if a regulation doesn't set the expected flag.
//   130     = Spectral Steed Whistle (Torrent) -- always protected.
//   2003170 = DLC "Golden Vow" thrown pot -- buffs the thrower via a
//             bullet, so it isn't a plain sort-group-20 consumable.
const int kHorseSummonGoodsBuiltin[]   = { 130 };
const int kExtraConsumableGoodsBuiltin[] = { 2003170 };

// Built-in SpEffect ids for Ash-of-War self-buffs. Ashes apply their buff
// through a weapon-skill behavior that can't be reached cleanly from the gem
// param (SwordArtsParam has no SpEffect ref), so they're driven by this
// allowlist; extend with [ashes_of_war] speffect_ids. Ids are the "Damage Buff"
// effect rows (incl. No-FP variants) from soulsmods/Paramdex ER SpEffectParam
// names -- the same set PersistentBuffs uses. Element weapon-enchant arts
// (Cragblade, Flaming Strike, infusions) are intentionally excluded: like
// greases they belong to the weapon, not the character. Add them via the .ini
// if you want them too.
const std::vector<int> kAshOfWarBuffSpEffectsBuiltin = {
    841, 843, 846, 848,           // Roar
    1586, 1588,                   // Jellyfish Shield
    1650, 1651, 1655, 1656,       // Endure (poise)
    1681, 1683, 1686, 1688,       // Barbaric/Milos Roar
    1691, 1693, 1696, 1698,       // Determination
    1701, 1703, 1706, 1708,       // Royal Knight's Resolve
    1730, 1732,                   // Golden Vow
    1811, 1813, 1816, 1818,       // War Cry
    1861, 1863, 1866, 1868,       // Braggart's Roar
};

// ---- dual-wield off-hand mirror: weapon-art enchant pairs ----
// A weapon buff is two SpEffect rows: Right (wepParamChange==1) enchants the
// right weapon, Left (==2) the left. Greases are paired dynamically from the
// live params (see build_dualwield_mirror); weapon-skill (Ash of War) enchants
// aren't goods, so their Right->Left pairs are listed here (ids from the
// soulsmods/Paramdex-based reference set). Extend via [dual_wield] extra_pairs.
// NB: these are hand-targeted *weapon enchant* arts -- a different set from
// kAshOfWarBuffSpEffectsBuiltin above, which are character self-buffs (Roar,
// Determination, ...) that don't target a hand and need no mirroring.
struct HandPair { int right; int left; };
const HandPair kDualWieldArtPairsBuiltin[] = {
    { 821, 823 },              // Sacred Blade
    { 826, 828 },              // Chilling Mist
    { 831, 833 },              // Poison Mist
    { 1676, 1678 },            // Lightning Slash
    { 1721, 1723 },            // Moonlight Greatsword
    { 1755, 1758 },            // Seppuku
    { 1776, 1778 },            // Flaming Strike
    { 1806, 1808 },            // Ruinous Ghostflame
    { 1821, 1823 },            // Cragblade
    { 1891, 1893 },            // Ice Lightning Sword
    { 20000891, 20000896 },    // Flame Skewer
    { 20000961, 20000966 },    // Flame Spear
};

// Sort groups (EquipParamGoods.sortGroupId) we treat as categories.
constexpr int kSortGroupGrease     = 70; // greases (vanilla + DLC)
constexpr int kSortGroupConsumable = 20; // buff/heal foods, livers, boluses...

// Guard against pathological / cyclic SpEffect chains.
constexpr int kChainMaxDepth = 8;

// ---- paths -------------------------------------------------
// config.ini sits next to the DLL; the log goes in a logs/ subfolder
// alongside the DLL (the convention other mods use).
std::wstring module_path() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(g_hinst, buf, MAX_PATH);
    return std::wstring(buf);
}
std::wstring dir_of(const std::wstring& p) {
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}
std::wstring stem_of(const std::wstring& p) {
    const size_t slash = p.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? p : p.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}
std::wstring config_path() {
    const std::wstring m = module_path();
    return dir_of(m) + L"\\" + stem_of(m) + L".ini";
}
std::wstring log_path() {
    const std::wstring m = module_path();
    return dir_of(m) + L"\\logs\\" + stem_of(m) + L".log";
}

// ---- logging: ALWAYS writes logs/<DllName>.log near the DLL --
void log_line(const std::string& msg, bool truncate = false) {
    const std::wstring path = log_path();
    CreateDirectoryW(dir_of(path).c_str(), nullptr); // ensure logs/ exists
    std::ofstream f(path,
                    std::ios::out | (truncate ? std::ios::trunc : std::ios::app));
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char ts[24];
        std::snprintf(ts, sizeof(ts), "[%02d:%02d:%02d.%03d] ",
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        f << ts << msg << '\n';
    }
    if (g_debug)
        std::cout << "[InfiniteWeaponBuffs] " << msg << std::endl;
}

void flog(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log_line(buf);
}

// ---- parse "130, 2003170" into a set of ints ----------------
void parse_int_list(const std::string& spec, std::unordered_set<int>& out) {
    std::stringstream ss(spec);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        const size_t a = tok.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        const size_t b = tok.find_last_not_of(" \t");
        try { out.insert(std::stoi(tok.substr(a, b - a + 1))); }
        catch (...) {}
    }
}

// ---- parse "821:823, 1821:1823" into right:left pairs -------
void parse_pair_list(const std::string& spec, std::vector<HandPair>& out) {
    std::stringstream ss(spec);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        const size_t colon = tok.find(':');
        if (colon == std::string::npos) continue;
        try {
            const int r = std::stoi(tok.substr(0, colon));
            const int l = std::stoi(tok.substr(colon + 1));
            out.push_back({ r, l });
        } catch (...) {}
    }
}

// ---- param row accessors (binary search by id) --------------
// Each returns a pointer to the live row, or nullptr if it doesn't exist.
from::paramdef::SP_EFFECT_PARAM_ST* sp_row(int id) {
    if (id < 0) return nullptr;
    auto [row, ok] = from::param::SpEffectParam[id];
    return ok ? &row : nullptr;
}
from::paramdef::BULLET_PARAM_ST* bullet_row(int id) {
    if (id < 0) return nullptr;
    auto [row, ok] = from::param::Bullet[id];
    return ok ? &row : nullptr;
}
// Player behaviors live in BehaviorParam_PC; fall back to BehaviorParam.
from::paramdef::BEHAVIOR_PARAM_ST* behavior_row(int id) {
    if (id < 0) return nullptr;
    { auto [row, ok] = from::param::BehaviorParam_PC[id]; if (ok) return &row; }
    { auto [row, ok] = from::param::BehaviorParam[id];    if (ok) return &row; }
    return nullptr;
}

// All self/ally SpEffects a bullet can apply: the shooter effect (a thrown
// self-buff pot like Golden Vow) plus the on-hit effects (the buff lands on
// whoever stands in the AoE, including the thrower). Enemy-only hit effects are
// dropped later by the self/player target filter.
void add_bullet_speffects(int bulletId, std::vector<int>& out) {
    auto* bl = bullet_row(bulletId);
    if (!bl) return;
    const int ids[] = { bl->spEffectIDForShooter, bl->spEffectId0,
                        bl->spEffectId1, bl->spEffectId2,
                        bl->spEffectId3, bl->spEffectId4 };
    for (int s : ids) if (s >= 0) out.push_back(s);
}

// ---- SpEffect discovery from a goods row --------------------
// "Entry" SpEffects are the ones an item points at directly, before any
// replace/cycle chaining. A goods refId means different things per refCategory
// ([0]=attack [1]=projectile/bullet [2]=special/SpEffect): we always try it as
// a SpEffect (foods etc.), and for projectile items also follow it as a Bullet
// (thrown buff pots). Items that act through a behavior add the behavior's
// SpEffect (refType 2) or bullet effects (refType 1) too.
void gather_goods_entry_speffects(const from::paramdef::EQUIP_PARAM_GOODS_ST& row,
                                  std::vector<int>& out) {
    const int refs[2] = { row.refId_default, row.refId_1 };
    for (int r : refs) {
        if (r < 0) continue;
        if (sp_row(r)) out.push_back(r);            // refId as SpEffect
        if (row.refCategory == 1) add_bullet_speffects(r, out); // refId as Bullet
    }

    if (row.behaviorId > 0) {
        if (auto* b = behavior_row(row.behaviorId)) {
            if (b->refType == 2 && b->refId >= 0) out.push_back(b->refId);
            else if (b->refType == 1)             add_bullet_speffects(b->refId, out);
        }
    }
}

// Collect EVERY SpEffect reachable from `startId` through the replace/cycle
// chain (used to fence off horse-summon state effects). Bounded + de-duped.
void collect_all_chain(int startId, std::unordered_set<int>& out, int depth = 0) {
    if (startId < 0 || depth > kChainMaxDepth) return;
    if (!out.insert(startId).second) return; // already visited
    auto* r = sp_row(startId);
    if (!r) return;
    collect_all_chain(r->replaceSpEffectId,         out, depth + 1);
    collect_all_chain(r->cycleOccurrenceSpEffectId, out, depth + 1);
}

// Collect the SpEffect ids in `startId`'s chain that are currently on a finite
// timer (effectEndurance > 0) -- i.e. the actual buffs we can extend.
void collect_timed_chain(int startId, std::vector<int>& out,
                         std::unordered_set<int>& visited, int depth = 0) {
    if (startId < 0 || depth > kChainMaxDepth) return;
    if (!visited.insert(startId).second) return;
    auto* r = sp_row(startId);
    if (!r) return;
    if (r->effectEndurance > 0.0f) out.push_back(startId);
    collect_timed_chain(r->replaceSpEffectId,         out, visited, depth + 1);
    collect_timed_chain(r->cycleOccurrenceSpEffectId, out, visited, depth + 1);
}

bool is_self_buff(const from::paramdef::SP_EFFECT_PARAM_ST* r) {
    return r && (r->effectTargetSelf || r->effectTargetPlayer);
}

// Engine state / animation / environment effects that must never be made
// permanent (re-using PersistentBuffs' Paramdex-sourced blocklist). These are
// matched by id only -- robust even if struct field offsets drift across game
// versions. 9621 = Roundtable "Disallow Hostile Actions" (the no-combat lock).
bool is_system_effect(int id) {
    if (id >= 100000 && id <= 100999) return true; // [HKS] state block + grace
    if (id >= 131    && id <= 147)    return true; // jump / attack anim states
    if (id >= 170    && id <= 176)    return true; // guard anim states
    switch (id) {
        case 45:      // [HKS] Counter Frames
        case 8001:    // [HKS] Is Stealth
        case 10665:   // [HKS] Event action not possible
        case 530007:  // [HKS] Goods stamina cost
        case 530012:  // [HKS] Goods stamina cost
        case 9621:    // Disallow Hostile Actions (Roundtable Hold no-combat)
        case 4600:    // Wet (Rain) -- environment
            return true;
        default:
            return false;
    }
}

// Does this SpEffect actually improve a combat/vitality stat? Used to keep only
// genuine *positive* buffs and drop (a) debuffs -- effects that only worsen
// stats -- and (b) system/state effects with no stat change at all, such as the
// Roundtable "no-combat" zone state. An effect that buffs at least one stat
// counts, even if it also has a downside (e.g. Shabriri Grape).
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

bool is_grease(const from::paramdef::EQUIP_PARAM_GOODS_ST& row) {
    return row.isEnhance || row.isShieldEnchant ||
           static_cast<int>(row.sortGroupId) == kSortGroupGrease;
}

// ---- build the set of SpEffects we must never touch ---------
// Anything a horse-summon item can reach: its state effect must keep its
// original (stateful) duration or Torrent gets stuck "active".
void build_protected_set(const std::unordered_set<int>& horseGoods,
                         std::unordered_set<int>& out) {
    for (auto [id, row] : from::param::EquipParamGoods) {
        if (!row.isSummonHorse && !horseGoods.count(static_cast<int>(id)))
            continue;
        std::vector<int> entries;
        gather_goods_entry_speffects(row, entries);
        for (int e : entries) collect_all_chain(e, out);
    }
}

// Add the timed buffs reachable from `entries` to `target` at `dur`.
//   followChain      : follow the SpEffect replace/cycle chain (consumables) vs.
//                      only the entry effect itself (greases / spells).
//   requireSelf      : keep only self/player-targeted effects (consumables) so
//                      enemy debuffs aren't made permanent.
//   requireBuff      : keep only effects that actually improve a stat -- drops
//                      debuffs and system/state effects (e.g. the Roundtable
//                      no-combat zone). `skippedNonBuff` (optional) counts these.
// Protected effects are never added; first writer wins (target.emplace).
int add_buffs(const std::vector<int>& entries, float dur,
              bool followChain, bool requireSelf, bool requireBuff,
              const std::unordered_set<int>& protectedSp,
              std::unordered_map<int, float>& target,
              int* skippedNonBuff = nullptr) {
    int added = 0;
    for (int e : entries) {
        std::vector<int> timed;
        if (followChain) {
            std::unordered_set<int> visited;
            collect_timed_chain(e, timed, visited);
        } else {
            auto* r = sp_row(e);
            if (r && r->effectEndurance > 0.0f) timed.push_back(e);
        }
        for (int t : timed) {
            if (protectedSp.count(t) || is_system_effect(t)) continue;
            auto* r = sp_row(t);
            if (requireSelf && !is_self_buff(r)) continue;
            if (requireBuff && !is_beneficial_buff(r)) {
                if (skippedNonBuff) ++*skippedNonBuff;
                continue;
            }
            if (target.emplace(t, dur).second) ++added;
        }
    }
    return added;
}

// SpEffects referenced by the Magic param (spell buffs).
void gather_magic_entries(const from::paramdef::MAGIC_PARAM_ST& row,
                          std::vector<int>& out) {
    const int refs[] = {
        row.refId1, row.refId2, row.refId3, row.refId4,  row.refId5,
        row.refId6, row.refId7, row.refId8, row.refId9,  row.refId10,
    };
    for (int r : refs) if (r >= 0) out.push_back(r);
}

// ---- dual-wield off-hand mirroring --------------------------
// Weapon buffs come as a Right (wepParamChange==1) + Left (==2) SpEffect; the
// Left enchants the off-hand weapon. Setting Right.cycleOccurrenceSpEffectId =
// Left makes the engine re-apply the Left every cycle while the main-hand buff is
// active, so a single buff lands on BOTH weapons when dual-wielding.

// The weapon-enchant id behind a SpEffect (via its vfx -> SpEffectVfxParam), or
// -1 if it carries no weapon enchant. Right and Left share this per element, so
// it pairs them (e.g. 1=magic, 3=lightning, 5=poison, 10=holy, 17=black flame).
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

// Build the right->left mirror map. Greases are discovered dynamically (the same
// is_grease() set the duration pass uses); weapon-art enchants come from the
// explicit pair list (built-in + .ini extra_pairs). Pairs touching a
// protected/system effect are skipped. With logDetail, each decision is logged
// (dump mode) and the resulting map can be discarded.
void build_dualwield_mirror(const std::vector<HandPair>& artPairs,
                            const std::unordered_set<int>& protectedSp,
                            std::unordered_map<int, int>& mirror,
                            bool logDetail = false) {
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
            flog("dual pair %-12s right=%d -> left=%d (enchant=%d, curCyc=%d)%s",
                 src, r, l, sp_enchant_id(r), sp_row(r)->cycleOccurrenceSpEffectId,
                 added ? "" : " [dup, kept first]");
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

// ---- the param passes ---------------------------------------
void apply(const Ini& ini, const std::unordered_set<int>& extraGoods,
           const std::unordered_set<int>& horseGoods,
           const std::unordered_set<int>& ashIds,
           const std::vector<HandPair>& artPairs) {
    // Pass 1: make every weapon buffable.
    if (ini.get_bool("general", "all_weapons_buffable", true)) {
        int n = 0;
        for (auto [id, row] : from::param::EquipParamWeapon) {
            row.isEnhance = true;
            ++n;
        }
        flog("all_weapons_buffable: isEnhance set on %d weapon rows", n);
    } else {
        flog("all_weapons_buffable: disabled in config");
    }

    // Fence off everything a horse-summon item can reach.
    std::unordered_set<int> protectedSp;
    build_protected_set(horseGoods, protectedSp);
    flog("protected: %zu SpEffect(s) fenced off from horse-summon items",
         protectedSp.size());

    const bool stackingBonuses = ini.get_bool("stacking", "stacking_bonuses", false);

    // speffect id -> target duration. Priority (first writer wins on overlap):
    // greases, then spell buffs, then consumables.
    std::unordered_map<int, float> target;

    if (ini.get_bool("greases", "enabled", true)) {
        const float d = ini.get_float("greases", "duration", -1.0f);
        int added = 0;
        for (auto [id, row] : from::param::EquipParamGoods) {
            if (!is_grease(row)) continue;
            std::vector<int> entries = { row.refId_default, row.refId_1 };
            added += add_buffs(entries, d, /*followChain*/false,
                               /*requireSelf*/false, /*requireBuff*/false,
                               protectedSp, target);
        }
        flog("greases: %d effect(s) (duration=%.1f)", added, d);
    }
    if (ini.get_bool("spell_buffs", "enabled", true)) {
        const float d = ini.get_float("spell_buffs", "duration", -1.0f);
        int added = 0;
        for (auto [id, row] : from::param::Magic) {
            std::vector<int> entries;
            gather_magic_entries(row, entries);
            added += add_buffs(entries, d, /*followChain*/false,
                               /*requireSelf*/false, /*requireBuff*/false,
                               protectedSp, target);
        }
        flog("spell_buffs: %d effect(s) (duration=%.1f)", added, d);
    }
    if (ini.get_bool("consumables", "enabled", true)) {
        const float d = ini.get_float("consumables", "duration", 300.0f);
        int added = 0, skippedHorse = 0, skippedNonBuff = 0;
        for (auto [id, row] : from::param::EquipParamGoods) {
            const bool inScope =
                static_cast<int>(row.sortGroupId) == kSortGroupConsumable ||
                extraGoods.count(static_cast<int>(id));
            if (!inScope) continue;
            if (row.isSummonHorse || horseGoods.count(static_cast<int>(id))) {
                ++skippedHorse;
                continue;
            }
            std::vector<int> entries;
            gather_goods_entry_speffects(row, entries);
            added += add_buffs(entries, d, /*followChain*/true,
                               /*requireSelf*/true, /*requireBuff*/true,
                               protectedSp, target, &skippedNonBuff);
        }
        flog("consumables: %d effect(s) (duration=%.1f), %d horse-summon skipped, "
             "%d non-buff skipped (debuffs/system effects)",
             added, d, skippedHorse, skippedNonBuff);
    }
    if (ini.get_bool("ashes_of_war", "enabled", true)) {
        const float d = ini.get_float("ashes_of_war", "duration", -1.0f);
        // Curated allowlist (built-in + .ini): trusted positive self-buffs, so
        // like greases/spells they skip the self/beneficial field checks (those
        // misread these effects) -- only the id's own finite timer is required,
        // and system/protected effects are still excluded.
        std::vector<int> entries(ashIds.begin(), ashIds.end());
        const int added = add_buffs(entries, d, /*followChain*/false,
                                    /*requireSelf*/false, /*requireBuff*/false,
                                    protectedSp, target);
        flog("ashes_of_war: %d effect(s) (duration=%.1f) from %d allowlisted id(s)",
             added, d, static_cast<int>(ashIds.size()));
    }

    // Decide the dual-wield pairs BEFORE rewriting durations: the off-hand match
    // disambiguates full vs. drawstring variants by nearest duration, which must
    // be read while the rows still hold their vanilla effectEndurance.
    const bool mirrorOn = ini.get_bool("dual_wield", "mirror_to_offhand", false);
    std::unordered_map<int, int> mirror;
    if (mirrorOn) build_dualwield_mirror(artPairs, protectedSp, mirror);

    // Apply durations. `target` already excludes protected/non-timed effects, so
    // every entry here is a buff we mean to change.
    if (!target.empty()) {
        int patched = 0;
        for (auto [id, row] : from::param::SpEffectParam) {
            const auto it = target.find(static_cast<int>(id));
            if (it == target.end()) continue;
            row.effectEndurance = it->second;
            if (stackingBonuses) row.spCategory = 0;
            ++patched;
        }
        flog("durations: patched %d SpEffect(s)", patched);
        if (stackingBonuses)
            flog("stacking: stacking_bonuses ON -- spCategory zeroed on patched buffs (no mutual exclusion)");
    } else {
        flog("durations: nothing to do (all categories disabled)");
    }

    // Dual-wield: wire the off-hand mirror. Done after durations are final so the
    // off-hand inherits the main hand's configured duration. Opt-in.
    if (mirrorOn) {
        int wired = 0;
        for (const auto& [r, l] : mirror) {
            auto* rr = sp_row(r);
            auto* ll = sp_row(l);
            if (!rr || !ll) continue;
            rr->cycleOccurrenceSpEffectId = l;   // main-hand active -> off-hand each cycle
            // Keep the off-hand lasting at least as long as the main hand; never
            // shrink an off-hand that's already longer or already infinite.
            if (rr->effectEndurance < 0.0f)
                ll->effectEndurance = -1.0f;
            else if (ll->effectEndurance >= 0.0f &&
                     rr->effectEndurance > ll->effectEndurance)
                ll->effectEndurance = rr->effectEndurance;
            ++wired;
        }
        flog("dual_wield: wired %d offhand mirror(s) (right->left)", wired);
    }
}

// ---- discovery: dump how each category resolves, so coverage can be
//      verified against the live regulation. Nothing is patched here.
void dump_candidates(const std::unordered_set<int>& extraGoods,
                     const std::unordered_set<int>& horseGoods,
                     const std::unordered_set<int>& ashIds,
                     const std::vector<HandPair>& artPairs) {
    flog("discover: tgt flags: S=self P=player (only self/player buffs are kept for consumables)");

    // --- protected (horse-summon) effects -------------------
    std::unordered_set<int> protectedSp;
    build_protected_set(horseGoods, protectedSp);
    {
        std::string ids;
        for (int id : protectedSp) ids += std::to_string(id) + " ";
        flog("discover: ---- PROTECTED SpEffects (horse-summon, never patched) ----");
        flog("discover: %zu protected: %s", protectedSp.size(), ids.c_str());
    }

    auto tgt = [](const from::paramdef::SP_EFFECT_PARAM_ST* r) {
        std::string t;
        if (r && r->effectTargetSelf)   t += 'S';
        if (r && r->effectTargetPlayer) t += 'P';
        return t.empty() ? std::string("-") : t;
    };
    auto resolved = [&](const std::vector<int>& entries, bool followChain) {
        std::string s;
        for (int e : entries) {
            std::vector<int> timed;
            if (followChain) {
                std::unordered_set<int> v;
                collect_timed_chain(e, timed, v);
            } else {
                auto* r = sp_row(e);
                if (r && r->effectEndurance > 0.0f) timed.push_back(e);
            }
            for (int t : timed) {
                auto* r = sp_row(t);
                char buf[96];
                // NB = non-beneficial (debuff/system effect, dropped for consumables)
                std::snprintf(buf, sizeof(buf), "%d(dur=%.1f tgt=%s%s%s) ", t,
                              r ? r->effectEndurance : 0.0f, tgt(r).c_str(),
                              protectedSp.count(t) ? " PROT" : "",
                              is_beneficial_buff(r) ? "" : " NB");
                s += buf;
            }
        }
        return s.empty() ? std::string("-") : s;
    };

    // --- greases --------------------------------------------
    flog("discover: ---- GREASES (isEnhance/isShieldEnchant/sortGroup 70) ----");
    int gn = 0;
    for (auto [id, row] : from::param::EquipParamGoods) {
        if (!is_grease(row)) continue;
        std::vector<int> entries = { row.refId_default, row.refId_1 };
        flog("grease goods=%d sortGrp=%d enh=%d/%d -> %s",
             static_cast<int>(id), static_cast<int>(row.sortGroupId),
             row.isEnhance ? 1 : 0, row.isShieldEnchant ? 1 : 0,
             resolved(entries, false).c_str());
        ++gn;
    }
    flog("discover: %d grease goods", gn);

    // --- consumables in scope -------------------------------
    flog("discover: ---- CONSUMABLES (sortGroup 20 + extra_goods allowlist) ----");
    int cn = 0;
    for (auto [id, row] : from::param::EquipParamGoods) {
        const bool inScope =
            static_cast<int>(row.sortGroupId) == kSortGroupConsumable ||
            extraGoods.count(static_cast<int>(id));
        if (!inScope) continue;
        const bool horse =
            row.isSummonHorse || horseGoods.count(static_cast<int>(id));
        std::vector<int> entries;
        gather_goods_entry_speffects(row, entries);
        flog("consumable goods=%d type=%d sortGrp=%d refCat=%d refs=[%d,%d] behav=%d%s -> %s",
             static_cast<int>(id), static_cast<int>(row.goodsType),
             static_cast<int>(row.sortGroupId), static_cast<int>(row.refCategory),
             row.refId_default, row.refId_1, row.behaviorId,
             horse ? " HORSE(skipped)" : "",
             resolved(entries, true).c_str());
        ++cn;
    }
    flog("discover: %d consumable goods in scope", cn);

    // --- potential misses -----------------------------------
    // Goods NOT in any category that still reach a self/player timed buff:
    // candidates we currently skip. Only effects lasting >= kMissDurFloor are
    // shown, to skip the flood of 0.1s instant cures / craft / summon triggers.
    constexpr float kMissDurFloor = 5.0f;
    flog("discover: ---- POTENTIAL MISSES (uncategorized self buffs, dur>=%.0fs) ----",
         kMissDurFloor);
    int mn = 0;
    for (auto [id, row] : from::param::EquipParamGoods) {
        if (is_grease(row)) continue;
        if (static_cast<int>(row.sortGroupId) == kSortGroupConsumable) continue;
        if (extraGoods.count(static_cast<int>(id))) continue;
        if (row.isSummonHorse || horseGoods.count(static_cast<int>(id))) continue;
        std::vector<int> entries;
        gather_goods_entry_speffects(row, entries);
        std::vector<int> hits;
        for (int e : entries) {
            std::vector<int> timed; std::unordered_set<int> v;
            collect_timed_chain(e, timed, v);
            for (int t : timed) {
                auto* r = sp_row(t);
                if (!protectedSp.count(t) && is_self_buff(r) &&
                    r && r->effectEndurance >= kMissDurFloor)
                    hits.push_back(t);
            }
        }
        if (hits.empty()) continue;
        std::string s;
        for (int t : hits) {
            auto* r = sp_row(t);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%d(dur=%.1f) ", t,
                          r ? r->effectEndurance : 0.0f);
            s += buf;
        }
        flog("miss? goods=%d type=%d sortGrp=%d -> %s",
             static_cast<int>(id), static_cast<int>(row.goodsType),
             static_cast<int>(row.sortGroupId), s.c_str());
        ++mn;
    }
    flog("discover: %d potential-miss goods (review for coverage)", mn);

    // --- spell buffs ----------------------------------------
    flog("discover: ---- Magic -> SpEffect (spell-buff candidates) ----");
    int sn = 0;
    for (auto [id, row] : from::param::Magic) {
        std::vector<int> entries;
        gather_magic_entries(row, entries);
        std::string s = resolved(entries, false);
        if (s == "-") continue;
        flog("magic=%d -> %s", static_cast<int>(id), s.c_str());
        ++sn;
    }
    flog("discover: %d magic rows with timed buffs", sn);

    // --- ash-of-war allowlist check -------------------------
    // The Ash-of-War category is id-driven (built-in + [ashes_of_war]
    // speffect_ids), because the activated buff can't be reached from the gem
    // param. Verify each allowlisted id against THIS regulation: present + timed
    // means it'll be extended; missing means a different id (e.g. on a mod) --
    // look it up in soulsmods/Paramdex SpEffectParam names and add it.
    flog("discover: ---- ASH-OF-WAR ALLOWLIST CHECK (%zu id(s)) ----", ashIds.size());
    int aok = 0;
    for (int id : ashIds) {
        auto* r = sp_row(id);
        const bool willExtend = r && r->effectEndurance > 0.0f &&
                                !protectedSp.count(id) && !is_system_effect(id);
        const char* status =
            !r                          ? "MISSING (not in SpEffectParam)" :
            protectedSp.count(id)       ? "skipped (protected)" :
            is_system_effect(id)        ? "skipped (system effect)" :
            (r->effectEndurance > 0.0f) ? "OK (timed -> will extend)" :
                                          "skipped (not on a timer)";
        flog("ash id=%d dur=%.1f %s", id, r ? r->effectEndurance : 0.0f, status);
        if (willExtend) ++aok;
    }
    flog("discover: %d/%zu ash allowlist id(s) will be extended", aok, ashIds.size());

    // --- dual-wield off-hand mirror -------------------------
    // What [dual_wield] mirror_to_offhand=1 would wire: each grease/weapon-art
    // Right -> its Left (off-hand) enchant. "curCyc" is the right row's current
    // cycleOccurrenceSpEffectId -- if it's not -1, a vanilla periodic effect
    // would be overwritten (review before enabling).
    flog("discover: ---- DUAL-WIELD PAIRS (right -> left offhand mirror) ----");
    std::unordered_map<int, int> dwMirror;
    build_dualwield_mirror(artPairs, protectedSp, dwMirror, /*logDetail*/ true);
    flog("discover: %zu dual-wield mirror pair(s) would be wired", dwMirror.size());

    flog("discover: review the sections above, then set dump=0");
}

// ---- worker thread (param load blocks; never do that in DllMain)
DWORD WINAPI run(LPVOID) {
    Ini ini;
    const std::wstring cfg = config_path();
    const bool loaded = ini.load(cfg);

    g_debug = ini.get_bool("discover", "debug_console", false);
    if (g_debug) {
        AllocConsole();
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
    }

    flog(loaded ? "config loaded"
                : "[WARN] .ini not found next to the DLL; using defaults");

    // Built-in id lists unioned with the user's allowlists.
    //   extraGoods : extra buff consumables.  horseGoods : protected horses.
    //   ashIds     : Ash-of-War buff SpEffect ids.
    std::unordered_set<int> extraGoods, horseGoods, ashIds;
    for (int id : kExtraConsumableGoodsBuiltin)   extraGoods.insert(id);
    for (int id : kHorseSummonGoodsBuiltin)       horseGoods.insert(id);
    for (int id : kAshOfWarBuffSpEffectsBuiltin)  ashIds.insert(id);
    parse_int_list(ini.get_string("general", "extra_goods", ""), extraGoods);
    parse_int_list(ini.get_string("ashes_of_war", "speffect_ids", ""), ashIds);

    // Dual-wield off-hand mirror: built-in weapon-art pairs + .ini extra_pairs.
    // (Greases are paired dynamically at apply time, so none are listed here.)
    std::vector<HandPair> artPairs;
    for (const auto& p : kDualWieldArtPairsBuiltin) artPairs.push_back(p);
    parse_pair_list(ini.get_string("dual_wield", "extra_pairs", ""), artPairs);

    try {
        flog("waiting for params...");
        from::CS::SoloParamRepository::wait_for_params(-1);
        flog("params ready -- applying edits");
        if (ini.get_bool("discover", "dump", false)) {
            flog("DISCOVER MODE ON -- dumping candidates (durations not applied)");
            dump_candidates(extraGoods, horseGoods, ashIds, artPairs);
        }
        apply(ini, extraGoods, horseGoods, ashIds, artPairs);
        flog("done.");
    } catch (const std::exception& e) {
        flog("[ERROR] exception: %s", e.what());
    } catch (...) {
        flog("[ERROR] unknown exception while applying edits");
    }
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
        // Fresh log each launch + hard proof the DLL actually loaded.
        log_line("==== InfiniteWeaponBuffs loaded (DllMain attach) ====",
                 /*truncate=*/true);
        CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
    }
    return TRUE;
}
