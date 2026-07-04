#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "config.hpp"
#include "utils.hpp"
#include "ini.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace pb {

HINSTANCE g_hinst = nullptr;
bool      g_debug = false;

bool g_keep_fast_travel  = true;
bool g_keep_death        = true;
bool g_log_effects       = false;
bool g_weapon_memory     = false;
bool g_restore_remaining = true;
bool g_session_persist   = false;

std::unordered_set<int> g_force_persist;
std::unordered_map<int, std::string> g_names;

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

std::string named(int id) {
    const auto it = g_names.find(id);
    return it == g_names.end() ? std::to_string(id)
                               : std::to_string(id) + ":" + it->second;
}

std::string join_ids(const std::vector<int>& ids) {
    std::string s;
    for (int id : ids) { s += named(id); s += ' '; }
    return s;
}

void load_config() {
    Ini ini;
    const bool loaded = ini.load(config_path());
    g_debug            = ini.get_bool("general", "debug_console", false);
    g_log_effects      = ini.get_bool("general", "log_effects", false);
    g_keep_fast_travel = ini.get_bool("persistence", "keep_after_fast_travel", true);
    g_keep_death       = ini.get_bool("persistence", "keep_after_death", true);
    g_restore_remaining = ini.get_bool("persistence", "restore_remaining_time", true);
    g_weapon_memory    = ini.get_bool("weapon_memory", "remember_per_weapon", false);
    g_session_persist  = ini.get_bool("session", "remember_across_sessions", false);
    // always_persist is hard-coded (g_always_persist); reapply delay is hard-coded
    // (kReapplyDelayMs). Only force_persist_ids remains ini-tunable.
    g_force_persist = parse_id_list(ini.get_string("persistence", "force_persist_ids", ""));
    const size_t nnames = load_names();
    if (g_debug) { AllocConsole(); FILE* o=nullptr; freopen_s(&o, "CONOUT$", "w", stdout); }
    flog(loaded ? "config loaded" : "[WARN] .ini not found; using defaults");
    flog(nnames ? "loaded %zu SpEffect name(s) from SpEffectParam.txt"
                : "no SpEffectParam.txt next to the DLL -- log will use bare ids (%zu)",
         nnames);
    flog("keep_after_fast_travel=%d keep_after_death=%d restore_remaining_time=%d "
         "reapply_delay_ms=%d(fixed) remember_per_weapon=%d remember_across_sessions=%d "
         "always_persist_ids=%zu(builtin) force_persist_ids=%zu",
         g_keep_fast_travel, g_keep_death, g_restore_remaining, kReapplyDelayMs,
         g_weapon_memory, g_session_persist, g_always_persist.size(),
         g_force_persist.size());
}

} // namespace pb
