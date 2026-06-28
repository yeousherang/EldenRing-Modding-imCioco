// ============================================================
//  Adjustable Summon Cost - a libER param-patcher DLL
//
//  Divides the FP (and HP) cost of spirit-ash summons by a number
//  you set in the .ini, so summons stay affordable without heavy
//  Mind / INT investment. All done in memory -- no regulation.bin.
//
//    EquipParamGoods.consumeMP = FP cost  -> divided
//    EquipParamGoods.consumeHP = HP cost  -> divided (Mimic Tear,
//                                            Soldjars, Land Squirts, ...)
//
//  Only goods of the configured summon goodsType(s) that actually
//  have a cost (> 0) are touched. Run OFFLINE only (EasyAntiCheat).
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// libER: wait_for_params
#include <coresystem/cs_param.hpp>
// libER: from::param::EquipParamGoods
#include <param/param.hpp>

#include "ini.hpp"

namespace {

HINSTANCE g_hinst = nullptr;
bool      g_debug = false;

// ---- paths: config next to the DLL, log in a logs/ subfolder ----
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
        std::cout << "[AdjustableSummonCost] " << msg << std::endl;
}

void flog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log_line(buf);
}

// ---- "7,8" -> {7, 8} ----------------------------------------
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

// Divide a positive cost by `divisor`, rounded, clamped to >= minCost.
short divide_cost(short cost, float divisor, int minCost) {
    int v = static_cast<int>(std::lround(cost / divisor));
    if (v < minCost) v = minCost;
    if (v > 32767)   v = 32767;
    return static_cast<short>(v);
}

void apply(const Ini& ini) {
    const float divisor = ini.get_float("summons", "divisor", 1.0f);
    const std::vector<int> summonTypes =
        parse_int_list(ini.get_string("advanced", "summon_goods_types", "7,8"));
    const int  minCost = static_cast<int>(ini.get_float("advanced", "min_cost", 1.0f));
    const bool logEach = ini.get_bool("advanced", "log_each", false);

    if (divisor <= 0.0f) {
        flog("[WARN] divisor must be > 0 (got %.2f); nothing changed", divisor);
        return;
    }
    if (std::fabs(divisor - 1.0f) < 0.001f) {
        flog("divisor = 1.0 -> costs unchanged");
        return;
    }

    auto is_summon = [&](int gt) {
        for (int t : summonTypes) if (t == gt) return true;
        return false;
    };

    int fpChanged = 0, hpChanged = 0, summonsSeen = 0;
    for (auto [id, row] : from::param::EquipParamGoods) {
        if (!is_summon(static_cast<int>(row.goodsType))) continue;

        const short oldMP = row.consumeMP;
        const short oldHP = row.consumeHP;
        if (oldMP <= 0 && oldHP <= 0) continue; // free / no cost -> skip
        ++summonsSeen;

        short newMP = oldMP, newHP = oldHP;
        if (oldMP > 0) {
            newMP = divide_cost(oldMP, divisor, minCost);
            if (newMP != oldMP) { row.consumeMP = newMP; ++fpChanged; }
        }
        if (oldHP > 0) {
            newHP = divide_cost(oldHP, divisor, minCost);
            if (newHP != oldHP) { row.consumeHP = newHP; ++hpChanged; }
        }
        if (logEach)
            flog("summon goods=%d  FP %d->%d  HP %d->%d",
                 static_cast<int>(id), oldMP, newMP, oldHP, newHP);
    }

    flog("divisor=%.2f: %d summons with a cost -> FP changed on %d, HP changed on %d",
         divisor, summonsSeen, fpChanged, hpChanged);
    if (summonsSeen == 0)
        flog("[WARN] no summon costs found -- check 'summon_goods_types' in the .ini");
}

// ---- worker thread (param load blocks; never do that in DllMain)
DWORD WINAPI run(LPVOID) {
    Ini ini;
    const bool loaded = ini.load(config_path());

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
        log_line("==== AdjustableSummonCost loaded (DllMain attach) ====",
                 /*truncate=*/true);
        CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
    }
    return TRUE;
}
