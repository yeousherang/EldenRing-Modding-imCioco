// ============================================================
//  PersistentBuffs - keep buffs through fast travel / death (Elden Ring)
//
//  Unlike the param-patcher mods in this repo, the engine WIPES active
//  SpEffects on fast travel / death regardless of duration (hardcoded; not
//  controllable via params -- confirmed by research, see CLAUDE.md). So this
//  mod works at RUNTIME: it remembers your active buff SpEffects and re-applies
//  them after the engine clears them on a transition.
//
//  STATUS: SCAFFOLD / WORK IN PROGRESS.
//    Working now:   loads, logs, resolves the player, ENUMERATES active
//                   SpEffects each tick (this verifies the offsets in-game).
//    TODO (needs RE / in-game verification -- see CLAUDE.md):
//      - confirm WorldChrMan / PlayerIns offsets for the target game version
//      - resolve the "apply SpEffect" function (AOB) so re-apply actually works
//      - distinguish fast-travel vs death so the two toggles are independent
//      - filter "persistable" buffs (exclude debuffs / system effects)
//
//  OFFLINE ONLY (EAC must be off). See README.md.
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <MinHook.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// libER: SoloParamRepository::wait_for_params + from::param::SpEffectParam.
// Used only to read SpEffectParam for the field-based buff filter (see
// is_persistable). Player access / re-apply still go through raw memory + AOB.
#include <coresystem/cs_param.hpp>
#include <param/param.hpp>

#include "ini.hpp"
#include "scan.hpp"

namespace {

HINSTANCE g_hinst = nullptr;
bool      g_debug = false;

// ---- config ------------------------------------------------------------
bool g_keep_fast_travel = true;
bool g_keep_death       = true;
// Diagnostic: log every SpEffect id that appears/disappears on the player (raw,
// unfiltered). Use it to discover system effect ids -- e.g. walk into Roundtable
// Hold and see which id the no-combat state adds. Off by default (chatty).
bool g_log_effects      = false;
// Wait this long after the player reappears (transition finished) before
// re-applying, so the world/character is fully settled and the engine has
// already done its wipe. Avoids re-applying into a half-loaded ChrIns.
// Hard-coded (no longer .ini-configurable) -- 500 ms is the tuned value.
constexpr int kReapplyDelayMs = 500;
// Death pre-strip handling. On DEATH the engine strips weapon/AoW buffs at the
// killing blow, but the player ChrIns stays valid through the whole "YOU DIED" /
// fade / reload sequence -- MANY seconds later the ptr finally nulls. During that
// window the poll keeps seeing the depleted set, so a plain "remember the latest
// snapshot" (or any short history ring) gets eroded to the post-strip set well
// before the transition fires -> weapon/AoW buffs are lost on death (fast travel
// is unaffected: it nulls the ptr in the SAME tick as the wipe, so the depleted
// set is never observed while the player is valid).
//
// Fix: when a sudden mass buff-loss is seen (>= kDeathDropThreshold persistable
// buffs vanish in one tick) we treat it as the death pre-strip and FREEZE
// `remembered` at the pre-strip set until the transition re-applies it. If the
// player instead keeps playing past kFreezeMaxTicks with no transition, the drop
// was a real change (not a death) -> we unfreeze and resync. (A rare false
// positive -- e.g. several physick tears expiring together -- at worst re-applies
// a couple of just-expired buffs on the next transition, which is benign.)
constexpr size_t kDeathDropThreshold = 2;
constexpr int    kFreezeMaxTicks     = 100; // ~20s at the 200ms poll
// Experimental: remember which weapon-bound buffs (greases / blade spells) were
// on each right-hand weapon, and re-apply them when you swap back to that weapon
// (vanilla drops them on a weapon swap). Default off. Right-hand only for now.
bool g_weapon_memory    = false;
// Buffs to treat as CHARACTER-WIDE rather than weapon-bound: AoW self-buffs
// (Endure, Determination, Royal Knight's Resolve, Roars, War Cry, Golden Vow...)
// are stat buffs that merely happen to be cast from a weapon art -- they should
// survive any weapon swap, not get re-bound to the casting weapon like a grease,
// AND they must survive death/fast travel. HARD-CODED (not .ini-configurable):
// the "Damage Buff" rows (incl. No-FP variants) from soulsmods/Paramdex, same set
// InfiniteWeaponBuffs uses. Element weapon-enchants (Sacred Blade, Chilling Mist,
// Cragblade, ...) are deliberately excluded -- those belong to the weapon.
const std::unordered_set<int> g_always_persist = {
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
// SpEffect ids to ALWAYS persist, bypassing the field-based buff gate. Safety
// valve for a genuine buff the SpEffectParam check misreads as "not a buff" (the
// check has known gaps -- e.g. poise-only effects). Found via the filter log;
// no rebuild needed ([persistence] force_persist_ids). See is_persistable.
std::unordered_set<int> g_force_persist;
// Optional id -> human name map for the log, loaded from a Paramdex-format names
// file next to the DLL (see load_names). Empty if the file is absent -> the log
// falls back to bare ids. Purely cosmetic (makes the log readable).
std::unordered_map<int, std::string> g_names;

// ============================================================
//  VERSION-SPECIFIC OFFSETS / SIGNATURES -- VERIFY before trusting!
//  Seeded from libER's symbol table + community RE. They WILL drift across
//  game patches; treat the values below as starting points and confirm with
//  the in-game enumeration log (and Cheat Engine / Nordgaren's Debug Tool).
//  See CLAUDE.md "Offsets & signatures".
// ============================================================
// WorldChrMan singleton pointer = module_base + this (libER GLOBAL_WorldChrMan).
constexpr uintptr_t kWorldChrManOffset = 0x3D65F88;
// WorldChrMan + this -> local PlayerIns (ChrIns*). CLASSIC value; VERIFY.
constexpr uintptr_t kPlayerInsOffset   = 0x1E508;
// ChrIns + this -> SpecialEffect manager (the SpEffect list owner).
constexpr uintptr_t kSpEffectManagerOffset = 0x178;
// SpEffect manager + this -> first list slot (pointer). NOTE: the active-effect
// list lives one indirection BELOW the manager -- the manager holds a pointer to
// the head slot, it is not the head itself. (Confirmed: TGA CT SpEffect.eraseAll.)
constexpr uintptr_t kSpEffectFirstSlotOffset = 0x8;
// Within a SpEffect list slot:
constexpr uintptr_t kSpEffectIdOffset   = 0x8;   // int  effect id
constexpr uintptr_t kSpEffectNextOffset = 0x30;  // ptr  next slot

// "Apply SpEffect" function: void ApplySpEffect(ChrIns* chr, int id, char unk).
// Resolved by AOB scan; the scanned instruction sits INSIDE the function, so the
// real entry point is `match - kApplySpEffectFuncBackset`. Called with unk=1
// (== "self"), matching TGA CT's SpEffect.addForSelf. See CLAUDE.md.
using ApplySpEffect_t = void(*)(void* chr_ins, int sp_effect_id, char unk);
ApplySpEffect_t g_apply = nullptr;
// Source: The Grand Archives CT, Global Functions/SpEffect_code.cea (add_call).
constexpr const char* kApplySpEffectAob =
    "0f 28 0d ?? ?? ?? ?? ?? 8d ?? ?? 0f 29 ?? ?? ?? 0f b6 d8";
constexpr uintptr_t kApplySpEffectFuncBackset = 0x1D;

// ---- weapon-slot identity (for per-weapon buff memory) -----------------
// GameDataMan singleton, resolved by AOB (the `mov rax,[rip+gdm]` lands at the
// match; the global pointer var = rip-relative @ off 3, len 7). Then:
//   GameDataMan  + 0x08  -> PlayerGameData
//   PlayerGameData+0x32C -> active right-hand slot index (0=Primary/1/2) (int)
//   PlayerGameData+0x328 -> active left-hand  slot index (int)
//   PlayerGameData+0x324 -> ArmStyle byte (0 empty, 1 one-hand, 2 L-2H, 3 R-2H)
//   PlayerGameData+0x5FC + slot*0x8 -> equipped right weapon id (int)
//   PlayerGameData+0x5F8 + slot*0x8 -> equipped left  weapon id (int)
// Source: TGA CT (root .cea baseData; Hero/ChrAsm CurrentWepSlotOffset; ChrAsm 2).
constexpr const char* kGameDataManAob =
    "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3";
constexpr uintptr_t kPlayerGameDataOffset  = 0x08;
constexpr uintptr_t kCurRightWepSlotOffset = 0x32C;
constexpr uintptr_t kCurLeftWepSlotOffset  = 0x328;
constexpr uintptr_t kArmStyleOffset        = 0x324;
constexpr uintptr_t kPrimaryRightWepOffset = 0x5FC;
constexpr uintptr_t kPrimaryLeftWepOffset  = 0x5F8;
constexpr uintptr_t kWepSlotStride         = 0x8;
uintptr_t g_gamedataman_var = 0; // address of the GameDataMan global pointer

// ---- paths / logging (logs/PersistentBuffs.log next to the DLL) --------
std::wstring module_path() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(g_hinst, buf, MAX_PATH);
    return std::wstring(buf);
}
std::wstring dir_of(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}
std::wstring stem_of(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    std::wstring name = s == std::wstring::npos ? p : p.substr(s + 1);
    const size_t d = name.find_last_of(L'.');
    return d == std::wstring::npos ? name : name.substr(0, d);
}
std::wstring config_path() {
    const std::wstring m = module_path();
    return dir_of(m) + L"\\" + stem_of(m) + L".ini";
}
std::wstring log_path() {
    const std::wstring m = module_path();
    return dir_of(m) + L"\\logs\\" + stem_of(m) + L".log";
}
// Optional Paramdex names file next to the DLL, e.g. from soulsmods/Paramdex
// ER/Names/SpEffectParam.txt (lines "id name"). Absent -> log uses bare ids.
std::wstring names_path() {
    return dir_of(module_path()) + L"\\SpEffectParam.txt";
}

void log_line(const std::string& msg, bool truncate = false) {
    const std::wstring path = log_path();
    CreateDirectoryW(dir_of(path).c_str(), nullptr);
    std::ofstream f(path, std::ios::out | (truncate ? std::ios::trunc : std::ios::app));
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        char ts[24];
        std::snprintf(ts, sizeof(ts), "[%02d:%02d:%02d.%03d] ",
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        f << ts << msg << '\n';
    }
    if (g_debug) std::printf("[PersistentBuffs] %s\n", msg.c_str());
}
void flog(const char* fmt, ...) {
    char buf[1024];
    va_list a; va_start(a, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    log_line(buf);
}

// Parse a list of integer ids from a string (any non-digit is a separator).
std::unordered_set<int> parse_id_list(const std::string& s) {
    std::unordered_set<int> out;
    std::string num;
    for (char c : s) {
        if (c >= '0' && c <= '9') { num += c; continue; }
        if (!num.empty()) { out.insert(std::atoi(num.c_str())); num.clear(); }
    }
    if (!num.empty()) out.insert(std::atoi(num.c_str()));
    return out;
}

// Load the optional Paramdex names file (id -> name) for readable logs. Each line
// is "<id> <name>" (leading integer, rest of the line is the name), matching
// soulsmods/Paramdex ER/Names/SpEffectParam.txt. Returns the count loaded.
size_t load_names() {
    g_names.clear();
    std::ifstream f(names_path());
    if (!f) return 0;
    std::string line;
    while (std::getline(f, line)) {
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        size_t start = i;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
        if (i == start) continue;                        // no leading id
        const int id = std::atoi(line.substr(start, i - start).c_str());
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        size_t end = line.find_last_not_of(" \t\r\n");   // trim trailing
        if (end == std::string::npos || end < i) continue;
        g_names[id] = line.substr(i, end - i + 1);
    }
    return g_names.size();
}

// Format an id for the log: "id" or, if a name is known, "id:Name".
std::string named(int id) {
    const auto it = g_names.find(id);
    return it == g_names.end() ? std::to_string(id)
                               : std::to_string(id) + ":" + it->second;
}

// ---- game access -------------------------------------------------------
mem::Module g_mod;

// Resolve the local player ChrIns via WorldChrMan. Returns 0 if not in-game.
uintptr_t get_player_ins() {
    const uintptr_t wcm = mem::deref(g_mod.base + kWorldChrManOffset);
    if (!wcm) return 0;
    return mem::deref(wcm + kPlayerInsOffset);
}

// Resolve PlayerGameData via GameDataMan. Returns 0 if unavailable.
uintptr_t get_player_game_data() {
    if (!g_gamedataman_var) return 0;
    const uintptr_t gdm = mem::deref(g_gamedataman_var);
    if (!gdm) return 0;
    return mem::deref(gdm + kPlayerGameDataOffset);
}

// Item id of the weapon in the currently-active hand slot, or -1 if none.
// `slot_off` selects the active slot index field; `prim_off` is the slot-0 id.
int get_active_weapon_id(uintptr_t slot_off, uintptr_t prim_off) {
    const uintptr_t pgd = get_player_game_data();
    if (!pgd) return -1;
    int slot = -1;
    if (!mem::safe_read(pgd + slot_off, slot)) return -1;
    if (slot < 0 || slot > 2) return -1;
    int id = -1;
    if (!mem::safe_read(pgd + prim_off + slot * kWepSlotStride, id)) return -1;
    return id;
}
int get_active_right_weapon_id() {
    return get_active_weapon_id(kCurRightWepSlotOffset, kPrimaryRightWepOffset);
}
int get_active_left_weapon_id() {
    return get_active_weapon_id(kCurLeftWepSlotOffset, kPrimaryLeftWepOffset);
}
int get_arm_style() {
    const uintptr_t pgd = get_player_game_data();
    unsigned char v = 0;
    if (!pgd || !mem::safe_read(pgd + kArmStyleOffset, v)) return -1;
    return v;
}

// Walk the player's active SpEffect linked list, collecting effect ids.
//   player + 0x178      -> SpEffect manager
//   manager + 0x8       -> first slot (the list head is one indirection below)
//   slot + 0x8 (int)    -> effect id
//   slot + 0x30 (ptr)   -> next slot
void enumerate_speffects(uintptr_t player, std::vector<int>& out) {
    out.clear();
    if (!player) return;
    const uintptr_t manager = mem::deref(player + kSpEffectManagerOffset);
    if (!manager) return;
    uintptr_t slot = mem::deref(manager + kSpEffectFirstSlotOffset);
    int guard = 0;
    while (slot && guard++ < 512) {
        int id = -1;
        if (mem::safe_read(slot + kSpEffectIdOffset, id) && id > 0)
            out.push_back(id);
        slot = mem::deref(slot + kSpEffectNextOffset);
    }
}

// Space-separated id (or id:Name) list for logging.
std::string join_ids(const std::vector<int>& ids) {
    std::string s;
    for (int id : ids) { s += named(id); s += ' '; }
    return s;
}

// Diagnostic: log which effect ids appeared/disappeared since last call (raw,
// unfiltered) so system effects can be discovered from the log. No-op unless
// g_log_effects is on.
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

// SpEffectParam row by id (binary search), or nullptr if it doesn't exist /
// params not loaded yet. Mirrors InfiniteWeaponBuffs' sp_row.
from::paramdef::SP_EFFECT_PARAM_ST* sp_row(int id) {
    if (id < 0) return nullptr;
    auto [row, ok] = from::param::SpEffectParam[id];
    return ok ? &row : nullptr;
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

// Decide whether an active SpEffect should be snapshotted & re-applied after a
// wipe. Precedence:
//   1. force/always     -> keep  (trusted allowlist; bypasses the field gate)
//   2. is_system_effect -> drop  (known engine-state ids; fast, drift-proof)
//   3. field gate       -> keep iff is_beneficial_buff (real stat buff)
// Unknown ids (not in SpEffectParam) fall through the gate -> dropped (safe).
bool is_persistable(int id) {
    if (g_force_persist.count(id) || g_always_persist.count(id)) return true;
    if (is_system_effect(id))                                   return false;
    return is_beneficial_buff(sp_row(id));
}

// Diagnostic (behind g_log_effects): log which active effects the filter DROPS
// and why, de-duped so it only fires when the dropped set changes. Use it to
// spot a wanted buff being filtered out (-> add it to force_persist_ids) or to
// confirm a system effect is being rejected. Reason tags mirror is_persistable:
// user (exclude_ids) / system (id blocklist) / not-buff (failed field gate) /
// unknown (no SpEffectParam row).
void log_filter_changes(const std::vector<int>& cur_vec) {
    static std::unordered_set<int> prev_dropped;
    std::unordered_set<int> dropped;
    std::string line;
    for (int id : cur_vec) {
        if (is_persistable(id)) continue;
        dropped.insert(id);
        const char* why = is_system_effect(id) ? "system"
                        : !sp_row(id)          ? "unknown"
                                               : "not-buff";
        line += named(id); line += '('; line += why; line += ") ";
    }
    if (dropped != prev_dropped)
        flog("filter: dropping %zu effect(s): [ %s]", dropped.size(), line.c_str());
    prev_dropped = std::move(dropped);
}

void reapply(uintptr_t player, const std::vector<int>& ids) {
    if (!g_apply) { // function not resolved yet -> inert, but log intent
        flog("reapply: SKIPPED (%zu buff(s)) -- apply function not resolved (TODO AOB)",
             ids.size());
        return;
    }
    int n = 0;
    std::string applied;
    for (int id : ids) {
        if (!is_persistable(id)) continue;
        g_apply(reinterpret_cast<void*>(player), id, 1); // unk=1 == "self"
        applied += named(id); applied += ' ';
        ++n;
    }
    flog("reapply: re-applied %d buff(s): [ %s]", n, applied.c_str());
}

// ---- per-weapon buff memory --------------------------------------------
// Greases / blade buffs bind to the active weapon (in practice the right hand),
// so the engine drops them on a weapon swap AND on a "loadout change" such as
// bringing a left-hand weapon into play (dual wield) or two-handing -- even when
// the right weapon itself doesn't change. We restore them per weapon:
//
//  * Ownership is learned at APPLICATION: when a buff first appears, tag it with
//    the active weapon (right hand preferred, else left -- matching how greases /
//    blade spells target the right armament).
//  * A buff is treated as weapon-bound only once it's observed to DROP within a
//    short window after a loadout change. Body buffs (Golden Vow, consumables)
//    don't drop on a loadout change, so they're never confirmed -> never touched.
//  * Confirmed buffs are re-applied while their owner weapon is in hand. A buff
//    that vanishes with NO recent loadout change = natural expiry -> forgotten
//    (so we don't resurrect an expired grease, matching the prior right-hand feel).
//
// "Loadout" = (active right wep id, active left wep id, ArmStyle).
constexpr int kReapplyWindowTicks = 3;  // ~600ms at the 200ms poll; covers swap lag

std::unordered_map<int,int> g_applied_owner; // buff id -> weapon active when it appeared
// confirmed weapon-bound buff -> the SET of weapons that have it. A set (not a
// single weapon) so the SAME grease applied to two weapons is remembered for
// both -- otherwise switching between them would drop it from one.
std::unordered_map<int, std::unordered_set<int>> g_buff_owner;
std::unordered_set<int>     g_prev_buffs;
std::unordered_set<int>     g_restored_event; // buffs already restored since last change
int  g_prev_right = -1, g_prev_left = -1, g_prev_arm = -2;
bool g_loadout_valid = false;
int  g_ticks_since_change = 1 << 20;

void weapon_memory_reset() { g_loadout_valid = false; } // call across transitions

void weapon_memory_tick(uintptr_t player, const std::vector<int>& current) {
    if (!g_weapon_memory) return;
    const int right = get_active_right_weapon_id();
    const int left  = get_active_left_weapon_id();
    const int arm   = get_arm_style();
    // Only track real buffs -- never engine state effects (so we don't bind/
    // re-apply Roundtable's no-combat block etc. through weapon swaps).
    std::unordered_set<int> cur;
    for (int id : current) if (is_persistable(id)) cur.insert(id);
    const int owner_now = (right > 0) ? right : left;

    if (!g_loadout_valid) {
        // Seed: tag already-active buffs, but don't confirm any as weapon-bound.
        if (owner_now > 0) for (int id : cur) g_applied_owner[id] = owner_now;
        g_prev_buffs = cur;
        g_prev_right = right; g_prev_left = left; g_prev_arm = arm;
        g_ticks_since_change = 1 << 20;
        g_loadout_valid = true;
        return;
    }

    const bool changed = (right != g_prev_right) || (left != g_prev_left) || (arm != g_prev_arm);
    if (changed) { g_ticks_since_change = 0; g_restored_event.clear(); }
    else if (g_ticks_since_change < (1 << 20)) ++g_ticks_since_change;
    const bool recent = g_ticks_since_change <= kReapplyWindowTicks;

    // 1) Learn tentative ownership for newly-applied buffs.
    if (owner_now > 0)
        for (int id : cur)
            if (!g_prev_buffs.count(id)) g_applied_owner[id] = owner_now;

    // 2) Handle buffs that vanished this tick.
    for (int id : g_prev_buffs) {
        if (cur.count(id)) continue;
        if (recent) {
            // Dropped together with a loadout change -> weapon-bound. Add the
            // weapon it was applied on to this buff's owner set (a buff can live
            // on several weapons, e.g. the same grease on two of them).
            auto a = g_applied_owner.find(id);
            if (a != g_applied_owner.end() && a->second > 0) {
                if (g_buff_owner[id].insert(a->second).second)
                    flog("weapon-memory: buff %d bound to weapon %d", id, a->second);
            }
        } else {
            // Vanished with no recent loadout change -> natural expiry. Drop the
            // in-hand weapon(s) from this buff's owners (their copy ran out);
            // forget the buff entirely once no weapon owns it / if always-persist.
            auto it = g_buff_owner.find(id);
            if (it != g_buff_owner.end()) {
                if (g_always_persist.count(id)) {
                    g_buff_owner.erase(it);
                } else {
                    it->second.erase(right);
                    it->second.erase(left);
                    if (it->second.empty()) g_buff_owner.erase(it);
                }
            }
            g_applied_owner.erase(id);
        }
    }

    // 3) Within the post-change window, restore dropped confirmed buffs. A normal
    //    weapon buff (grease) is restored only while its owner weapon is back in
    //    hand (owner not in hand => we swapped away on purpose => leave dropped).
    //    An "always-persist" buff (AoW self-buff) is restored regardless of which
    //    weapon is in hand, so it survives any swap like a body buff.
    //    De-duped per change event: in a dual-wield state the engine may keep our
    //    re-applied buff under a different id, so it never re-appears in `cur` --
    //    without this guard we'd re-apply (and log) every tick of the window.
    // if (recent && g_apply) {
    //     int n = 0;
    //     for (const auto& kv : g_buff_owner) {
    //         const int id = kv.first, owner = kv.second;
    //         const bool in_hand = (owner == right || owner == left);
    //         const bool always  = g_always_persist.count(id) != 0;
    //         if ((in_hand || always) && !cur.count(id) && !g_restored_event.count(id)) {
    //             g_apply(reinterpret_cast<void*>(player), id, 1);
    //             g_restored_event.insert(id);
    //             ++n;
    //         }
    //     }
    //     if (n) flog("weapon-memory: restored %d weapon buff(s) after loadout change", n);
    // }
    if (recent && g_apply) {
        int n = 0;
        for (const auto& kv : g_buff_owner) {
            const int id = kv.first;
            const auto& weapons = kv.second;          // now a set of weapon IDs
            bool in_hand = (weapons.find(right) != weapons.end()) ||
                        (weapons.find(left)  != weapons.end());
            bool always = g_always_persist.count(id) != 0;
            if ((in_hand || always) && !cur.count(id) && !g_restored_event.count(id)) {
                g_apply(reinterpret_cast<void*>(player), id, 1);
                g_restored_event.insert(id);
                ++n;
            }
        }
        if (n) flog("weapon-memory: restored %d weapon buff(s) after loadout change", n);
    }

    g_prev_buffs = cur;
    g_prev_right = right; g_prev_left = left; g_prev_arm = arm;
}

// ---- worker: poll player, remember buffs, re-apply after a wipe --------
// NOTE: this poll loop is the scaffold's engine. The "hooks" infrastructure
// (MinHook) is initialized below and is the intended home for precise
// transition detection (hook the SpEffect-clear / respawn / warp). See CLAUDE.md.
DWORD WINAPI run(LPVOID) {
    g_mod = mem::main_module();
    flog("module base=%p size=0x%zX", reinterpret_cast<void*>(g_mod.base), g_mod.size);

    // Resolve the apply function. The AOB lands inside the function body, so
    // back up to the real entry point. Refuse a non-unique match (would risk
    // calling the wrong function and crashing the game).
    if (kApplySpEffectAob[0]) {
        bool multiple = false;
        const uintptr_t hit = mem::aob_scan_unique(g_mod, kApplySpEffectAob, &multiple);
        if (hit && !multiple) {
            g_apply = reinterpret_cast<ApplySpEffect_t>(hit - kApplySpEffectFuncBackset);
            flog("apply function: resolved hit=%p entry=%p",
                 reinterpret_cast<void*>(hit), reinterpret_cast<void*>(g_apply));
        } else if (multiple) {
            flog("[WARN] apply function: AOB matched MULTIPLE sites -- refusing to "
                 "use (re-apply stays inert). Signature needs tightening.");
        } else {
            flog("[WARN] apply function: NOT FOUND (AOB drifted for this game version)");
        }
    } else {
        flog("apply function: AOB not set (re-apply is inert)");
    }

    // Resolve GameDataMan (only needed for the weapon-memory feature).
    if (g_weapon_memory) {
        bool multiple = false;
        const uintptr_t site = mem::aob_scan_unique(g_mod, kGameDataManAob, &multiple);
        if (site && !multiple) {
            g_gamedataman_var = mem::rip_relative(site, 3, 7);
            flog("weapon-memory: GameDataMan ptr var=%p",
                 reinterpret_cast<void*>(g_gamedataman_var));
        } else {
            flog("[WARN] weapon-memory: GameDataMan %s -- feature disabled",
                 multiple ? "AOB matched MULTIPLE sites" : "AOB NOT FOUND");
            g_weapon_memory = false;
        }
    }

    // Wait for params so the field-based buff filter (is_persistable -> sp_row ->
    // SpEffectParam) works. Safe from this worker thread (not DllMain, not a
    // thread the main thread waits on). The player only exists after params load,
    // so this returns well before the first re-apply.
    flog("waiting for params (buff filter needs SpEffectParam)...");
    if (from::CS::SoloParamRepository::wait_for_params(-1))
        flog("params ready -- field-based buff filter active");
    else
        flog("[WARN] wait_for_params timed out -- buff filter degraded "
             "(unknown effects will be dropped)");

    std::vector<int> remembered;   // buffs to re-apply on the next transition
    std::vector<int> current;
    bool had_player   = false;
    bool frozen       = false;     // holding the pre-strip set through a death
    int  frozen_ticks = 0;

    for (;;) {
        const uintptr_t player = get_player_ins();

        if (player) {
            enumerate_speffects(player, current);
            if (g_log_effects) { log_effect_changes(current); log_filter_changes(current); }

            if (!had_player) {
                // Just (re)entered a playable state -> a load/fast-travel/respawn
                // just completed and the engine has wiped effects. Re-apply.
                // TODO: split fast-travel vs death to honor the two toggles
                //       independently (needs death/respawn detection).
                if ((g_keep_fast_travel || g_keep_death) && !remembered.empty()) {
                    flog("transition detected -> re-applying %zu buff(s) after %d ms settle",
                         remembered.size(), kReapplyDelayMs);
                    if (kReapplyDelayMs > 0) Sleep(kReapplyDelayMs);
                    // Re-resolve the player after the settle wait -- the pointer
                    // can move while the world finishes loading.
                    const uintptr_t p = get_player_ins();
                    if (p) reapply(p, remembered);
                    else   flog("reapply: SKIPPED -- player vanished during settle");
                }
                had_player = true;
                // Fresh life: don't treat the weapon as "swapped" across the
                // transition, and stop holding the pre-death set so `remembered`
                // tracks the new life from here.
                weapon_memory_reset();
                frozen = false; frozen_ticks = 0;
            } else {
                // Stable gameplay: handle weapon swaps, then update `remembered`
                // with this tick's persistable buffs -- unless we're holding the
                // pre-strip set through a death (see kDeathDropThreshold).
                weapon_memory_tick(player, current);
                std::vector<int> snap;
                for (int id : current)
                    if (is_persistable(id)) snap.push_back(id);

                // How many currently-remembered buffs vanished this tick?
                size_t lost = 0;
                if (!remembered.empty()) {
                    std::unordered_set<int> have(snap.begin(), snap.end());
                    for (int id : remembered) if (!have.count(id)) ++lost;
                }

                if (frozen) {
                    // Held since a suspected death pre-strip. Two ways out:
                    //  * buffs recovered to ~the pre-drop richness -> NOT a death
                    //    (the player re-buffed / weapon-memory restored the dropped
                    //    weapon buffs); resync now. On a real death the set stays
                    //    depleted through "YOU DIED", so we keep holding.
                    //  * failsafe: give up after kFreezeMaxTicks so a real drop that
                    //    never recovers can't freeze us indefinitely.
                    if (snap.size() >= remembered.size() ||
                        ++frozen_ticks > kFreezeMaxTicks) {
                        remembered = std::move(snap);
                        frozen = false; frozen_ticks = 0;
                    }
                } else if (lost >= kDeathDropThreshold) {
                    // Sudden mass buff-loss = the death pre-strip. Keep the current
                    // (pre-strip) `remembered` and stop updating it until the
                    // transition re-applies it.
                    frozen = true; frozen_ticks = 0;
                    flog("death pre-strip detected (%zu buff(s) dropped this tick) "
                         "-> holding %zu buff(s) for re-apply", lost, remembered.size());
                } else {
                    remembered = std::move(snap);
                }
            }
        } else {
            had_player = false; // loading screen / not in game -> preserve `remembered`
        }

        Sleep(200);
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);

        log_line("==== PersistentBuffs loaded (DllMain attach) ====", /*truncate=*/true);

        // Config.
        Ini ini;
        const bool loaded = ini.load(config_path());
        g_debug           = ini.get_bool("general", "debug_console", false);
        g_log_effects     = ini.get_bool("general", "log_effects", false);
        g_keep_fast_travel = ini.get_bool("persistence", "keep_after_fast_travel", true);
        g_keep_death       = ini.get_bool("persistence", "keep_after_death", true);
        g_weapon_memory    = ini.get_bool("weapon_memory", "remember_per_weapon", false);
        // always_persist is hard-coded (g_always_persist); reapply delay is hard-coded
        // (kReapplyDelayMs). Only force_persist_ids remains ini-tunable.
        g_force_persist = parse_id_list(ini.get_string("persistence", "force_persist_ids", ""));
        const size_t nnames = load_names();
        if (g_debug) { AllocConsole(); FILE* o=nullptr; freopen_s(&o, "CONOUT$", "w", stdout); }
        flog(loaded ? "config loaded" : "[WARN] .ini not found; using defaults");
        flog(nnames ? "loaded %zu SpEffect name(s) from SpEffectParam.txt"
                    : "no SpEffectParam.txt next to the DLL -- log will use bare ids (%zu)",
             nnames);
        flog("keep_after_fast_travel=%d keep_after_death=%d reapply_delay_ms=%d(fixed) "
             "remember_per_weapon=%d always_persist_ids=%zu(builtin) force_persist_ids=%zu",
             g_keep_fast_travel, g_keep_death, kReapplyDelayMs, g_weapon_memory,
             g_always_persist.size(), g_force_persist.size());

        // MinHook ready for the transition hooks documented in CLAUDE.md.
        if (MH_Initialize() == MH_OK)
            flog("MinHook initialized (no hooks created yet -- see CLAUDE.md)");
        else
            flog("[WARN] MinHook init failed");

        CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
    }
    return TRUE;
}
