#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "session_store.hpp"

#include "game_access.hpp"
#include "ini.hpp"
#include "log.hpp"
#include "state.hpp"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace cte {

namespace {

// On-disk format version. Bump if the [char_*] layout changes incompatibly; a
// mismatch makes presets_startup_load ignore the file (start fresh).
constexpr int kPresetFormatVersion = 1;

// In-memory mirror of the state file: one entry per [char_<key>] section, keyed
// by the FULL section name. Ordered so the rewritten file is deterministic and
// so OTHER characters' sections survive a save untouched. Worker-thread-only.
struct CharPreset {
    long long        saved_unix = 0;
    std::string      display;         // raw display name (UTF-8) for the import UI
    std::vector<int> enabled_ids;     // enabled EquipParamAccessory ids
    bool             allow_stacking = false;
    bool             progression_mode = false;
};
std::map<std::string, CharPreset> g_mirror;

std::string utf16_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                       nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// Parse "id id id ..." into ids; malformed tokens skipped.
std::vector<int> parse_ids(const std::string& s) {
    std::vector<int> out;
    std::istringstream ss(s);
    std::string tok;
    while (ss >> tok) {
        try { out.push_back(std::stoi(tok)); }
        catch (...) { /* skip */ }
    }
    return out;
}

std::string join_ids(const std::vector<int>& ids) {
    std::string out;
    for (int id : ids) {
        if (!out.empty()) out += ' ';
        out += std::to_string(id);
    }
    return out;
}

// Rewrite the whole mirror to <state>.tmp, then atomically replace the real file
// (MoveFileEx WRITE_THROUGH) so a crash mid-write can't corrupt the state.
void write_state_file() {
    const std::wstring final_path = state_path();
    const std::wstring tmp_path   = final_path + L".tmp";
    {
        std::ofstream f(tmp_path, std::ios::out | std::ios::trunc);
        if (!f) {
            flog("[WARN] presets: cannot open state tmp file for write");
            return;
        }
        f << "; CustomTalismanEffects per-character presets -- machine-written; safe to edit or delete.\n";
        f << "; talismans = space-separated EquipParamAccessory ids that are ENABLED for this character.\n";
        f << "[state]\n";
        f << "version = " << kPresetFormatVersion << "\n\n";
        for (const auto& [section, e] : g_mirror) {
            f << '[' << section << "]\n";
            f << "display_name = " << e.display << "\n";
            f << "saved_unix = " << e.saved_unix << "\n";
            f << "talismans = " << join_ids(e.enabled_ids) << "\n";
            f << "allow_stacking = " << (e.allow_stacking ? 1 : 0) << "\n";
            f << "progression_mode = " << (e.progression_mode ? 1 : 0) << "\n\n";
        }
    } // flush + close before the move
    if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        flog("[WARN] presets: atomic state replace failed (err %lu)", GetLastError());
    }
}

// Apply a stored preset to g_state (caller holds g_state_mutex): enable exactly
// its ids, set the two toggles, and collapse families when stacking is off.
void apply_entry_to_state(const CharPreset& e) {
    std::unordered_set<int> on(e.enabled_ids.begin(), e.enabled_ids.end());
    for (auto& t : g_state.talismans)
        t.enabled = on.count(t.accessory_id) != 0;
    g_state.allow_stacking = e.allow_stacking;
    g_state.progression_mode = e.progression_mode;
    if (!g_state.allow_stacking) collapse_groups_locked();
}

} // namespace

std::string char_key(const std::wstring& name) {
    if (name.empty()) return std::string();
    const std::string utf8 = utf16_to_utf8(name);
    std::string san;
    for (char c : utf8) {
        if (san.size() >= 24) break;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        san += ok ? c : '_';
    }
    if (san.empty()) san = "char"; // name sanitized to nothing (all non-ASCII)
    // FNV-1a (32-bit) over the RAW UTF-16 bytes -> 4 hex digits, so two names
    // that sanitize identically still get distinct keys.
    uint32_t h = 0x811c9dc5u;
    const auto* bytes = reinterpret_cast<const unsigned char*>(name.data());
    const size_t nbytes = name.size() * sizeof(wchar_t);
    for (size_t i = 0; i < nbytes; ++i) { h ^= bytes[i]; h *= 0x01000193u; }
    char tag[8];
    std::snprintf(tag, sizeof(tag), "_%04x", static_cast<unsigned>(h & 0xFFFFu));
    return san + tag;
}

std::string current_char_key(const std::string& prev_key, bool* name_ok) {
    const std::wstring name = get_character_name();
    if (name_ok) *name_ok = !name.empty();
    if (name.empty()) return prev_key; // keep the previous key on a failed read
    return char_key(name);
}

void presets_startup_load() {
    g_mirror.clear();
    Ini ini;
    if (!ini.load(state_path())) {
        flog("presets: no state file yet -- starting fresh");
        return;
    }
    const int ver = ini.get_int("state", "version", 0);
    if (ver != kPresetFormatVersion) {
        flog("[WARN] presets: state file version %d != %d -- ignoring (will overwrite)",
             ver, kPresetFormatVersion);
        return;
    }
    size_t n = 0;
    for (const std::string& sec : ini.section_names()) {
        if (sec.rfind("char_", 0) != 0) continue; // only per-character sections
        CharPreset e;
        try { e.saved_unix = std::stoll(ini.get_string(sec, "saved_unix", "0")); }
        catch (...) { e.saved_unix = 0; }
        e.display          = ini.get_string(sec, "display_name", "");
        e.enabled_ids      = parse_ids(ini.get_string(sec, "talismans", ""));
        e.allow_stacking   = ini.get_bool(sec, "allow_stacking", false);
        e.progression_mode = ini.get_bool(sec, "progression_mode", false);
        g_mirror[sec] = std::move(e);
        ++n;
    }
    flog("presets: loaded %zu character preset%s from disk", n, n == 1 ? "" : "s");
}

bool presets_any() { return !g_mirror.empty(); }

bool presets_has(const std::string& key) {
    return g_mirror.find("char_" + key) != g_mirror.end();
}

std::vector<std::pair<std::string, std::string>> presets_list() {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(g_mirror.size());
    for (const auto& [section, e] : g_mirror) {
        // Strip the "char_" prefix -> the bare key the lifecycle uses.
        std::string key = section.rfind("char_", 0) == 0 ? section.substr(5) : section;
        out.emplace_back(std::move(key), e.display);
    }
    return out;
}

bool presets_load_into(const std::string& key) {
    const auto it = g_mirror.find("char_" + key);
    if (it == g_mirror.end()) { presets_clear_state(); return false; }
    apply_entry_to_state(it->second);
    return true;
}

void presets_clear_state() {
    for (auto& t : g_state.talismans) t.enabled = false;
    g_state.allow_stacking = false;
    g_state.progression_mode = false;
}

void presets_save(const std::string& key, const std::wstring& display) {
    if (key.empty()) return; // no character identity -> nothing to key on
    CharPreset e;
    e.saved_unix       = static_cast<long long>(std::time(nullptr));
    e.display          = utf16_to_utf8(display);
    e.allow_stacking   = g_state.allow_stacking;
    e.progression_mode = g_state.progression_mode;
    for (const auto& t : g_state.talismans)
        if (t.enabled) e.enabled_ids.push_back(t.accessory_id);
    // Keep the previously-stored display name if we couldn't read one this time
    // (e.g. saving on the way out, PlayerGameData already gone).
    if (e.display.empty()) {
        const auto prev = g_mirror.find("char_" + key);
        if (prev != g_mirror.end()) e.display = prev->second.display;
    }
    g_mirror["char_" + key] = std::move(e);
    write_state_file();
    flog("presets: saved character '%s' (%zu talisman(s) on)",
         key.c_str(), g_mirror["char_" + key].enabled_ids.size());
}

bool presets_import_into(const std::string& src_key) {
    const auto it = g_mirror.find("char_" + src_key);
    if (it == g_mirror.end()) return false;
    apply_entry_to_state(it->second);
    return true;
}

} // namespace cte
