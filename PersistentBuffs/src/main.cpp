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

#include "ini.hpp"
#include "scan.hpp"

namespace {

HINSTANCE g_hinst = nullptr;
bool      g_debug = false;

// ---- config ------------------------------------------------------------
bool g_keep_fast_travel = true;
bool g_keep_death       = true;
// Wait this long after the player reappears (transition finished) before
// re-applying, so the world/character is fully settled and the engine has
// already done its wipe. Avoids re-applying into a half-loaded ChrIns.
int  g_reapply_delay_ms = 500;
// Experimental: remember which weapon-bound buffs (greases / blade spells) were
// on each right-hand weapon, and re-apply them when you swap back to that weapon
// (vanilla drops them on a weapon swap). Default off. Right-hand only for now.
bool g_weapon_memory    = false;

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

// TODO: real filter. For now treat every enumerated effect as persistable.
// Should exclude debuffs / status build-ups / system effects -- e.g. cross-ref
// SpEffectParam (effectTargetSelf + finite/positive effectEndurance), or keep a
// curated allow-list of buff ids. See CLAUDE.md.
bool is_persistable(int /*sp_effect_id*/) { return true; }

void reapply(uintptr_t player, const std::vector<int>& ids) {
    if (!g_apply) { // function not resolved yet -> inert, but log intent
        flog("reapply: SKIPPED (%zu buff(s)) -- apply function not resolved (TODO AOB)",
             ids.size());
        return;
    }
    int n = 0;
    for (int id : ids) {
        if (!is_persistable(id)) continue;
        g_apply(reinterpret_cast<void*>(player), id, 1); // unk=1 == "self"
        ++n;
    }
    flog("reapply: re-applied %d buff(s)", n);
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
std::unordered_map<int,int> g_buff_owner;    // confirmed weapon-bound buff -> owner weapon
std::unordered_set<int>     g_prev_buffs;
int  g_prev_right = -1, g_prev_left = -1, g_prev_arm = -2;
bool g_loadout_valid = false;
int  g_ticks_since_change = 1 << 20;

void weapon_memory_reset() { g_loadout_valid = false; } // call across transitions

void weapon_memory_tick(uintptr_t player, const std::vector<int>& current) {
    if (!g_weapon_memory) return;
    const int right = get_active_right_weapon_id();
    const int left  = get_active_left_weapon_id();
    const int arm   = get_arm_style();
    const std::unordered_set<int> cur(current.begin(), current.end());
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
    if (changed) g_ticks_since_change = 0;
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
            // Dropped together with a loadout change -> weapon-bound. Confirm it
            // against the weapon it was applied on.
            auto a = g_applied_owner.find(id);
            if (a != g_applied_owner.end() && a->second > 0 &&
                g_buff_owner.find(id) == g_buff_owner.end()) {
                g_buff_owner[id] = a->second;
                flog("weapon-memory: buff %d bound to weapon %d", id, a->second);
            }
        } else {
            // Vanished with no recent loadout change -> natural expiry -> forget.
            g_buff_owner.erase(id);
            g_applied_owner.erase(id);
        }
    }

    // 3) Within the post-change window, restore confirmed weapon buffs whose owner
    //    is back in hand but whose effect got dropped. (Owner not in hand => we
    //    intentionally swapped away => leave it dropped until we return.)
    if (recent && g_apply) {
        int n = 0;
        for (const auto& kv : g_buff_owner)
            if ((kv.second == right || kv.second == left) && !cur.count(kv.first)) {
                g_apply(reinterpret_cast<void*>(player), kv.first, 1);
                ++n;
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

    std::vector<int> remembered;   // buffs held before the last wipe
    std::vector<int> current;
    bool had_player = false;

    for (;;) {
        const uintptr_t player = get_player_ins();

        if (player) {
            enumerate_speffects(player, current);

            if (!had_player) {
                // Just (re)entered a playable state -> a load/fast-travel/respawn
                // just completed and the engine has wiped effects. Re-apply.
                // TODO: split fast-travel vs death to honor the two toggles
                //       independently (needs death/respawn detection).
                if ((g_keep_fast_travel || g_keep_death) && !remembered.empty()) {
                    flog("transition detected -> re-applying %zu buff(s) after %d ms settle",
                         remembered.size(), g_reapply_delay_ms);
                    if (g_reapply_delay_ms > 0) Sleep(g_reapply_delay_ms);
                    // Re-resolve the player after the settle wait -- the pointer
                    // can move while the world finishes loading.
                    const uintptr_t p = get_player_ins();
                    if (p) reapply(p, remembered);
                    else   flog("reapply: SKIPPED -- player vanished during settle");
                }
                had_player = true;
                // Don't treat the weapon as "swapped" across the transition.
                weapon_memory_reset();
            } else {
                // Stable gameplay: handle weapon swaps, then snapshot what we
                // currently have so we can restore it after the next wipe.
                weapon_memory_tick(player, current);
                remembered.clear();
                for (int id : current)
                    if (is_persistable(id)) remembered.push_back(id);
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
        g_keep_fast_travel = ini.get_bool("persistence", "keep_after_fast_travel", true);
        g_keep_death       = ini.get_bool("persistence", "keep_after_death", true);
        g_reapply_delay_ms = ini.get_int("persistence", "reapply_delay_ms", 500);
        g_weapon_memory    = ini.get_bool("weapon_memory", "remember_per_weapon", false);
        if (g_debug) { AllocConsole(); FILE* o=nullptr; freopen_s(&o, "CONOUT$", "w", stdout); }
        flog(loaded ? "config loaded" : "[WARN] .ini not found; using defaults");
        flog("keep_after_fast_travel=%d keep_after_death=%d reapply_delay_ms=%d "
             "remember_per_weapon=%d",
             g_keep_fast_travel, g_keep_death, g_reapply_delay_ms, g_weapon_memory);

        // MinHook ready for the transition hooks documented in CLAUDE.md.
        if (MH_Initialize() == MH_OK)
            flog("MinHook initialized (no hooks created yet -- see CLAUDE.md)");
        else
            flog("[WARN] MinHook init failed");

        CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
    }
    return TRUE;
}
