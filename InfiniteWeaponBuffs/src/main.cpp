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
//       SpEffect IDs listed per category in the .ini
//       => greases / spell buffs / consumable buffs last as long
//          as you want (-1 = permanent).
//
//  Writes a log next to the DLL (InfiniteWeaponBuffs.log) every
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

// ---- parse "0,3,10" into a vector of ints --------------------
std::vector<int> parse_int_list(const std::string& spec) {
    std::vector<int> out;
    std::stringstream ss(spec);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        const size_t a = tok.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        const size_t b = tok.find_last_not_of(" \t");
        try { out.push_back(std::stoi(tok.substr(a, b - a + 1))); }
        catch (...) {}
    }
    return out;
}

// Collect the SpEffect ids referenced by goods of the given goodsType(s).
// `valid` gates refIds to real SpEffects; first writer wins (target.emplace),
// so a higher-priority category added earlier is never overwritten.
//
// Horse-summon items (the Spectral Steed Whistle) share the consumable
// goodsType bucket but are stateful toggles, not buffs: forcing their SpEffect
// duration to a fixed/infinite value leaves the "Torrent active" state stuck,
// so you can only summon once per session. EquipParamGoods.isSummonHorse marks
// these, so we always skip them. `skipped_summon` (optional) counts them.
int collect_goods_speffects(const std::vector<int>& types, float dur,
                            const std::unordered_set<int>& valid,
                            std::unordered_map<int, float>& target,
                            int* skipped_summon = nullptr) {
    int added = 0;
    for (auto [id, row] : from::param::EquipParamGoods) {
        const int gt = static_cast<int>(row.goodsType);
        bool match = false;
        for (int t : types) if (t == gt) { match = true; break; }
        if (!match) continue;
        if (row.isSummonHorse) {
            if (skipped_summon) ++*skipped_summon;
            flog("  skip horse-summon goods=%d (refId_default=%d refId_1=%d)"
                 " -- stateful toggle, not a buff",
                 static_cast<int>(id), row.refId_default, row.refId_1);
            continue;
        }
        const int refs[2] = { row.refId_default, row.refId_1 };
        for (int r : refs)
            if (r >= 0 && valid.count(r) && target.emplace(r, dur).second)
                ++added;
    }
    return added;
}

// Same, for SpEffects referenced by the Magic param (spell buffs).
int collect_magic_speffects(float dur, const std::unordered_set<int>& valid,
                            std::unordered_map<int, float>& target) {
    int added = 0;
    for (auto [id, row] : from::param::Magic) {
        const int refs[] = {
            row.refId1, row.refId2, row.refId3, row.refId4,  row.refId5,
            row.refId6, row.refId7, row.refId8, row.refId9,  row.refId10,
        };
        for (int r : refs)
            if (r >= 0 && valid.count(r) && target.emplace(r, dur).second)
                ++added;
    }
    return added;
}

// ---- the param passes ---------------------------------------
void apply(const Ini& ini) {
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

    // Pass 2: auto-categorized durations. We derive the SpEffects for each
    // category straight from the params -- no ID lists to maintain.
    std::unordered_set<int> valid;
    for (auto [id, row] : from::param::SpEffectParam)
        valid.insert(static_cast<int>(id));

    const std::vector<int> greaseTypes =
        parse_int_list(ini.get_string("advanced", "grease_goods_types", "10"));
    const std::vector<int> consumableTypes =
        parse_int_list(ini.get_string("advanced", "consumable_goods_types", "0,3"));
    const bool onlyTimed = ini.get_bool("advanced", "only_timed_effects", true);

    // Optional: zero SpEffectParam.spCategory on the managed buffs so buffs in
    // the same exclusivity category stop replacing one another and can be active
    // at the same time.
    const bool noOverwrite = ini.get_bool("stacking", "no_overwrite", false);

    // speffect id -> target duration. Priority (first writer wins on overlap):
    // greases, then spell buffs, then consumables.
    std::unordered_map<int, float> target;

    if (ini.get_bool("greases", "enabled", true)) {
        const float d = ini.get_float("greases", "duration", -1.0f);
        const int a = collect_goods_speffects(greaseTypes, d, valid, target);
        flog("greases: %d effects (duration=%.1f)", a, d);
    }
    if (ini.get_bool("spell_buffs", "enabled", true)) {
        const float d = ini.get_float("spell_buffs", "duration", -1.0f);
        const int a = collect_magic_speffects(d, valid, target);
        flog("spell_buffs: %d effects (duration=%.1f)", a, d);
    }
    if (ini.get_bool("consumables", "enabled", true)) {
        const float d = ini.get_float("consumables", "duration", 300.0f);
        int skipped_summon = 0;
        const int a = collect_goods_speffects(consumableTypes, d, valid, target,
                                              &skipped_summon);
        flog("consumables: %d effects (duration=%.1f), %d horse-summon item(s) skipped",
             a, d, skipped_summon);
    }

    if (target.empty()) {
        flog("durations: nothing to do (all categories disabled)");
        return;
    }

    // Apply. With only_timed_effects on, skip anything not already on a finite
    // timer -- leaves instant heals, passives and permanent effects untouched.
    int patched = 0, skipped = 0;
    for (auto [id, row] : from::param::SpEffectParam) {
        const auto it = target.find(static_cast<int>(id));
        if (it == target.end()) continue;
        if (onlyTimed && !(row.effectEndurance > 0.0f)) { ++skipped; continue; }
        row.effectEndurance = it->second;
        if (noOverwrite) row.spCategory = 0;
        ++patched;
    }
    flog("durations: patched %d effects (%d skipped as non-timed)",
         patched, skipped);
    if (noOverwrite)
        flog("stacking: no_overwrite ON -- spCategory zeroed on patched buffs (no mutual exclusion)");
}

// ---- discovery: dump which SpEffect IDs greases / spell buffs /
//      consumables actually reference, so we can fill the .ini.
//      Enum-free: a refId "is a SpEffect" iff it exists in SpEffectParam.
//      Nothing is patched here -- this only writes to the log for review.
void dump_candidates() {
    struct SpInfo { float dur; bool self, frnd, enemy, player; };
    std::unordered_map<int, SpInfo> sp;
    for (auto [id, row] : from::param::SpEffectParam)
        sp[static_cast<int>(id)] = { row.effectEndurance,
            row.effectTargetSelf, row.effectTargetFriend,
            row.effectTargetEnemy, row.effectTargetPlayer };
    flog("discover: SpEffectParam has %zu rows", sp.size());

    // Target flags as a compact string, e.g. "SP" = self+player, "E" = enemy.
    auto tgt = [](const SpInfo& s) {
        std::string t;
        if (s.self)   t += 'S';
        if (s.frnd)   t += 'F';
        if (s.enemy)  t += 'E';
        if (s.player) t += 'P';
        return t.empty() ? std::string("-") : t;
    };

    flog("discover: tgt flags: S=self F=friend E=enemy P=player");

    flog("discover: ---- EquipParamGoods -> SpEffect (grease/consumable candidates) ----");
    flog("discover: fmt: goods=<row> goodsType=<t> speffect=<id> dur=<s> tgt=<flags>");
    int gn = 0;
    for (auto [id, row] : from::param::EquipParamGoods) {
        const int refs[2] = { row.refId_default, row.refId_1 };
        for (int r : refs) {
            const auto it = sp.find(r);
            if (r >= 0 && it != sp.end()) {
                flog("goods=%d goodsType=%d speffect=%d dur=%.1f tgt=%s",
                     static_cast<int>(id), static_cast<int>(row.goodsType),
                     r, it->second.dur, tgt(it->second).c_str());
                ++gn;
            }
        }
    }
    flog("discover: %d goods->speffect references", gn);

    flog("discover: ---- Magic -> SpEffect (spell-buff candidates) ----");
    flog("discover: fmt: magic=<row> speffect=<id> dur=<s> tgt=<flags>");
    int mn = 0;
    for (auto [id, row] : from::param::Magic) {
        const int refs[] = {
            row.refId1, row.refId2, row.refId3, row.refId4,  row.refId5,
            row.refId6, row.refId7, row.refId8, row.refId9,  row.refId10,
        };
        for (int r : refs) {
            const auto it = sp.find(r);
            if (r >= 0 && it != sp.end()) {
                flog("magic=%d speffect=%d dur=%.1f tgt=%s",
                     static_cast<int>(id), r, it->second.dur,
                     tgt(it->second).c_str());
                ++mn;
            }
        }
    }
    flog("discover: %d magic->speffect references", mn);
    flog("discover: review the tgt/dur columns above, then set dump=0");
}

// ---- worker thread (param load blocks; never do that in DllMain)
DWORD WINAPI run(LPVOID) {
    Ini ini;
    const std::wstring cfg = config_path();
    const bool loaded = ini.load(cfg);

    g_debug = ini.get_bool("general", "debug_console", false);
    if (g_debug) {
        AllocConsole();
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
    }

    flog(loaded ? "config loaded"
                : "[WARN] .ini not found next to the DLL; using defaults");

    try {
        flog("waiting for params...");
        from::CS::SoloParamRepository::wait_for_params(-1);
        flog("params ready -- applying edits");
        if (ini.get_bool("discover", "dump", false)) {
            flog("DISCOVER MODE ON -- dumping candidate ids (durations not applied)");
            dump_candidates();
        }
        apply(ini);
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
