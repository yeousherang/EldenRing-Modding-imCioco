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
// ChrIns + this -> SpecialEffect manager (head of the active-effect list).
constexpr uintptr_t kSpEffectListOffset = 0x178;
// Within a SpEffect list entry:
constexpr uintptr_t kSpEffectIdOffset   = 0x8;   // int  effect id
constexpr uintptr_t kSpEffectNextOffset = 0x30;  // ptr  next entry

// "Apply SpEffect" function. Signature & AOB are a TODO -- left empty so the
// scaffold compiles and runs inertly until resolved. See CLAUDE.md.
using ApplySpEffect_t = void(*)(void* chr_ins, int sp_effect_id, char unk);
ApplySpEffect_t g_apply = nullptr;
constexpr const char* kApplySpEffectAob = ""; // TODO: fill from RE

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

// Walk the player's active SpEffect linked list, collecting effect ids.
void enumerate_speffects(uintptr_t player, std::vector<int>& out) {
    out.clear();
    if (!player) return;
    uintptr_t node = mem::deref(player + kSpEffectListOffset);
    // The first node may be a header; walk `next` until null or a sane cap.
    int guard = 0;
    while (node && guard++ < 512) {
        int id = -1;
        if (mem::safe_read(node + kSpEffectIdOffset, id) && id > 0)
            out.push_back(id);
        node = mem::deref(node + kSpEffectNextOffset);
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
        g_apply(reinterpret_cast<void*>(player), id, 0);
        ++n;
    }
    flog("reapply: re-applied %d buff(s)", n);
}

// ---- worker: poll player, remember buffs, re-apply after a wipe --------
// NOTE: this poll loop is the scaffold's engine. The "hooks" infrastructure
// (MinHook) is initialized below and is the intended home for precise
// transition detection (hook the SpEffect-clear / respawn / warp). See CLAUDE.md.
DWORD WINAPI run(LPVOID) {
    g_mod = mem::main_module();
    flog("module base=%p size=0x%zX", reinterpret_cast<void*>(g_mod.base), g_mod.size);

    // Resolve the apply function (no-op while the AOB is empty).
    if (kApplySpEffectAob[0]) {
        g_apply = reinterpret_cast<ApplySpEffect_t>(mem::aob_scan(g_mod, kApplySpEffectAob));
        flog("apply function: %s", g_apply ? "resolved" : "NOT FOUND (check AOB)");
    } else {
        flog("apply function: AOB not set yet (re-apply is inert) -- see CLAUDE.md");
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
                    flog("transition detected -> attempting re-apply of %zu buff(s)",
                         remembered.size());
                    reapply(player, remembered);
                }
                had_player = true;
            } else {
                // Stable: snapshot what we currently have so we can restore it
                // after the next wipe. (Only persistable ids are kept.)
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
        if (g_debug) { AllocConsole(); FILE* o=nullptr; freopen_s(&o, "CONOUT$", "w", stdout); }
        flog(loaded ? "config loaded" : "[WARN] .ini not found; using defaults");
        flog("keep_after_fast_travel=%d keep_after_death=%d",
             g_keep_fast_travel, g_keep_death);

        // MinHook ready for the transition hooks documented in CLAUDE.md.
        if (MH_Initialize() == MH_OK)
            flog("MinHook initialized (no hooks created yet -- see CLAUDE.md)");
        else
            flog("[WARN] MinHook init failed");

        CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
    }
    return TRUE;
}
