// ============================================================
//  CustomTalismanEffects - apply talisman effects as passive buffs, with a
//  live in-game ImGui overlay to toggle them.
//
//  EquipParamAccessory IS the talisman table; each row's residentSpEffectId1..4
//  are the effects the game applies while the talisman is worn. This mod keeps
//  the enabled talismans' resident effects present on the player -- reproducing
//  an equipped talisman without using a talisman slot.
//
//  The worker loop diffs the desired effects (from the shared state, driven by
//  the .ini + the overlay) against the player's live SpEffect list: newly
//  desired -> ApplySpEffect; newly un-desired -> RemoveSpEffect. Family
//  exclusivity (accessoryGroup) and the global "allow stacking" toggle are
//  enforced in the state layer. Run OFFLINE only (EasyAntiCheat).
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Xinput.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// libER
#include <coresystem/cs_param.hpp>
#include <param/param.hpp>

#include "game_access.hpp"
#include "hooks.hpp"
#include "ini.hpp"
#include "log.hpp"
#include "offsets.hpp"
#include "overlay.hpp"
#include "state.hpp"
#include "talisman_names.hpp"

namespace cte {
namespace {

bool g_log_each = false;
constexpr DWORD kPollMs = 200;

// ---- name matching (must mirror gen_talisman_data.py norm()) ----
std::string normalize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_space = true;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            if (!in_space) { out.push_back(' '); in_space = true; }
        } else {
            out.push_back(static_cast<char>(std::tolower(c)));
            in_space = false;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// ---- .ini key parsing for the overlay hotkeys ----
unsigned int parse_open_key(const std::string& raw, unsigned int def) {
    std::string k = normalize(raw);
    if (k.empty()) return def;
    if (k.size() == 1) {
        char c = static_cast<char>(std::toupper(static_cast<unsigned char>(k[0])));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<unsigned int>(c);
        if (k[0] == '`') return VK_OEM_3;
    }
    if (k[0] == 'f' && k.size() >= 2) { // F1..F24
        int n = std::atoi(k.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + (n - 1);
    }
    if (k == "insert" || k == "ins") return VK_INSERT;
    if (k == "delete" || k == "del") return VK_DELETE;
    if (k == "home") return VK_HOME;
    if (k == "end") return VK_END;
    if (k == "pageup" || k == "pgup" || k == "prior") return VK_PRIOR;
    if (k == "pagedown" || k == "pgdn" || k == "next") return VK_NEXT;
    if (k == "backtick" || k == "grave" || k == "tilde") return VK_OEM_3;
    if (k == "tab") return VK_TAB;
    if (k == "space") return VK_SPACE;
    if (k == "pause") return VK_PAUSE;
    if (k == "scrolllock" || k == "scroll") return VK_SCROLL;
    flog("[WARN] unrecognized open_key \"%s\"; using default", raw.c_str());
    return def;
}

unsigned short parse_pad_mask(const std::string& raw, unsigned short def) {
    if (normalize(raw).empty()) return def;
    unsigned short mask = 0;
    std::stringstream ss(raw);
    std::string tok;
    while (std::getline(ss, tok, '+')) {
        std::string t = normalize(tok);
        if (t == "l3" || t == "ls" || t == "lthumb" || t == "leftthumb") mask |= XINPUT_GAMEPAD_LEFT_THUMB;
        else if (t == "r3" || t == "rs" || t == "rthumb" || t == "rightthumb") mask |= XINPUT_GAMEPAD_RIGHT_THUMB;
        else if (t == "lb" || t == "l1" || t == "leftshoulder") mask |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        else if (t == "rb" || t == "r1" || t == "rightshoulder") mask |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        else if (t == "a") mask |= XINPUT_GAMEPAD_A;
        else if (t == "b") mask |= XINPUT_GAMEPAD_B;
        else if (t == "x") mask |= XINPUT_GAMEPAD_X;
        else if (t == "y") mask |= XINPUT_GAMEPAD_Y;
        else if (t == "start") mask |= XINPUT_GAMEPAD_START;
        else if (t == "back" || t == "select") mask |= XINPUT_GAMEPAD_BACK;
        else if (t == "up" || t == "dpadup") mask |= XINPUT_GAMEPAD_DPAD_UP;
        else if (t == "down" || t == "dpaddown") mask |= XINPUT_GAMEPAD_DPAD_DOWN;
        else if (t == "left" || t == "dpadleft") mask |= XINPUT_GAMEPAD_DPAD_LEFT;
        else if (t == "right" || t == "dpadright") mask |= XINPUT_GAMEPAD_DPAD_RIGHT;
        else if (!t.empty()) flog("[WARN] unrecognized pad button \"%s\" in open_pad_mask", tok.c_str());
    }
    return mask ? mask : def;
}

// Enabled set + options parsed from the .ini (before params resolve).
struct IniConfig {
    std::unordered_set<std::string> enabled; // normalized talisman names set to 1
    bool           allow_stacking = false;
    unsigned int   open_vk = VK_INSERT;
    unsigned short open_pad_mask = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
};

IniConfig load_config(const Ini& ini) {
    IniConfig c;
    c.allow_stacking = ini.get_bool("overlay", "allow_stacking", false);
    c.open_vk = parse_open_key(ini.get_string("overlay", "toggle_key", "Insert"), VK_INSERT);
    c.open_pad_mask = parse_pad_mask(ini.get_string("overlay", "toggle_gamepad_combo", "L3+R3"),
                                     XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB);
    for (const auto& kv : ini.section_items("talismans"))
        if (Ini::as_bool(kv.second))
            c.enabled.insert(normalize(kv.first));
    return c;
}

// After params load, build the shared talisman model: for every talisman in the
// embedded table that exists in this regulation and has a resident effect, read
// its accessoryGroup + residentSpEffectId1..4. Enabled flag comes from the .ini.
void build_state(const IniConfig& cfg) {
    std::lock_guard<std::mutex> lk(g_state_mutex);
    g_state.allow_stacking = cfg.allow_stacking;
    g_state.open_vk = cfg.open_vk;
    g_state.open_pad_mask = cfg.open_pad_mask;
    g_state.talismans.clear();

    // A talisman's effect lives in one (or both) of two places on its accessory
    // row: `refId` -- the SpEffect it grants while worn (the common case; only
    // skipped when refCategory==1, which is a Bullet, not a SpEffect) -- and
    // `residentSpEffectId1..4` (a rarer "extra resident effect" slot). We take the
    // union so every talisman that references a real SpEffect resolves.
    auto sp_exists = [](int id) -> bool {
        if (id <= 0) return false;
        auto [sr, sok] = from::param::SpEffectParam[id];
        (void)sr;
        return sok;
    };

    int missing = 0, noeffect = 0, from_ref = 0, from_resident = 0;
    for (const auto& e : kTalismans) {
        auto [row, ok] = from::param::EquipParamAccessory[e.id];
        if (!ok) { ++missing; continue; } // DLC not owned, etc.

        Talisman t;
        t.accessory_id = e.id;
        t.name = e.name;
        t.group = row.accessoryGroup;

        auto add_id = [&](int id) {
            if (!sp_exists(id)) return false;
            if (std::find(t.sp_ids.begin(), t.sp_ids.end(), id) == t.sp_ids.end())
                t.sp_ids.push_back(id);
            return true;
        };

        bool ref_hit = (row.refCategory != 1) && add_id(row.refId);
        bool res_hit = false;
        for (int sp : { row.residentSpEffectId1, row.residentSpEffectId2,
                        row.residentSpEffectId3, row.residentSpEffectId4 })
            if (add_id(sp)) res_hit = true;

        if (t.sp_ids.empty()) {
            ++noeffect;
            if (noeffect <= 25)
                flog("  no-effect talisman \"%s\" acc=%d refId=%d refCat=%d res=[%d,%d,%d,%d]",
                     t.name.c_str(), t.accessory_id, row.refId, (int)row.refCategory,
                     row.residentSpEffectId1, row.residentSpEffectId2,
                     row.residentSpEffectId3, row.residentSpEffectId4);
            continue;
        }
        if (ref_hit) ++from_ref;
        if (res_hit) ++from_resident;

        t.effect = e.effect; // baked-in hover text (talisman_names.hpp)

        t.enabled = cfg.enabled.count(normalize(t.name)) != 0;
        g_state.talismans.push_back(std::move(t));
    }
    collapse_groups_locked(); // honor exclusivity for the loaded .ini selection
    flog("built %zu talismans (%d not in regulation, %d without effects; "
         "%d have a refId effect, %d have resident effect(s))",
         g_state.talismans.size(), missing, noeffect, from_ref, from_resident);
}

// Rewrite the .ini in place: flip each [talismans] Name to the current enabled
// state and update [overlay] allow_stacking. Preserves comments/formatting; only
// touches the value after '='. Called when the overlay requests a save.
void save_config() {
    const std::wstring path = config_path();
    std::ifstream in(path);
    if (!in) { flog("[WARN] save: cannot open .ini for read"); return; }

    // Snapshot enabled-by-name + allow_stacking under the lock.
    std::unordered_map<std::string, bool> want;
    bool allow_stacking;
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        for (const auto& t : g_state.talismans) want[normalize(t.name)] = t.enabled;
        allow_stacking = g_state.allow_stacking;
    }

    std::vector<std::string> out;
    std::string line, section;
    while (std::getline(in, line)) {
        std::string trimmed = line;
        // find section headers (ignoring leading spaces / trailing comments)
        size_t a = trimmed.find_first_not_of(" \t");
        if (a != std::string::npos && trimmed[a] == '[') {
            size_t close = trimmed.find(']', a);
            if (close != std::string::npos)
                section = trimmed.substr(a + 1, close - a - 1);
        }
        const size_t eq = line.find('=');
        if (eq != std::string::npos && (section == "talismans" || section == "overlay")) {
            // split key / (value + optional comment)
            std::string key = line.substr(0, eq);
            // trim key
            size_t ks = key.find_first_not_of(" \t");
            size_t ke = key.find_last_not_of(" \t");
            std::string key_trim = (ks == std::string::npos) ? "" : key.substr(ks, ke - ks + 1);
            std::string rest = line.substr(eq + 1);
            size_t hash = rest.find_first_of(";#");
            std::string comment = (hash == std::string::npos) ? "" : rest.substr(hash);

            if (section == "talismans") {
                auto it = want.find(normalize(key_trim));
                if (it != want.end()) {
                    std::string nv = it->second ? "1" : "0";
                    line = key + "= " + nv + (comment.empty() ? "" : " " + comment);
                }
            } else if (section == "overlay" && normalize(key_trim) == "allow_stacking") {
                std::string nv = allow_stacking ? "1" : "0";
                line = key + "= " + nv + (comment.empty() ? "" : " " + comment);
            }
        }
        out.push_back(line);
    }
    in.close();

    std::ofstream of(path, std::ios::trunc);
    if (!of) { flog("[WARN] save: cannot open .ini for write"); return; }
    for (const auto& l : out) of << l << '\n';
    flog("saved selections to .ini");
}

// Resolve a game function by AOB. Returns the entry (hit - backset) or 0.
uintptr_t resolve_fn(const char* aob, uintptr_t backset, const char* name) {
    bool multiple = false;
    const uintptr_t hit = mem::aob_scan_unique(g_mod, aob, &multiple);
    if (hit && !multiple) {
        flog("%s resolved: entry=%p", name, reinterpret_cast<void*>(hit - backset));
        return hit - backset;
    }
    if (multiple) flog("[ERROR] %s AOB matched MULTIPLE sites -- refusing to use it", name);
    else          flog("[ERROR] %s AOB not found -- game version mismatch?", name);
    return 0;
}

// The apply/remove diff loop.
void run_loop() {
    std::vector<int> active;
    std::unordered_set<int> active_set;
    std::unordered_set<int> applied;   // effects WE applied (ours to remove)
    bool warned_no_remove = false;

    for (;;) {
        // Snapshot desired effects + drain a save request under the lock.
        std::unordered_set<int> desired;
        bool do_save = false;
        {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            for (const auto& t : g_state.talismans)
                if (t.enabled)
                    for (int sp : t.sp_ids) desired.insert(sp);
            if (g_state.save_requested) { g_state.save_requested = false; do_save = true; }
        }
        if (do_save) save_config();

        const uintptr_t player = get_player_ins();
        if (!player) { Sleep(kPollMs); continue; }

        enumerate_speffects(player, active);
        active_set.clear();
        active_set.insert(active.begin(), active.end());

        // Apply: desired effects not currently on the player.
        if (g_apply) {
            for (int id : desired) {
                if (active_set.count(id)) continue;
                g_apply(reinterpret_cast<void*>(player), id, 1); // unk=1 == self
                if (applied.insert(id).second && g_log_each)
                    flog("applied SpEffect %d", id);
            }
        }

        // Remove: effects we applied that are no longer desired.
        const uintptr_t manager = mem::deref(player + kSpEffectManagerOffset);
        for (auto it = applied.begin(); it != applied.end();) {
            const int id = *it;
            if (desired.count(id)) { ++it; continue; } // still wanted
            if (active_set.count(id)) {
                if (g_remove && manager) {
                    g_remove(reinterpret_cast<void*>(manager), id);
                    if (g_log_each) flog("removed SpEffect %d", id);
                } else if (!warned_no_remove) {
                    flog("[WARN] no remove function -- disabled effects clear on next "
                         "area transition instead of instantly");
                    warned_no_remove = true;
                }
            }
            it = applied.erase(it);
        }

        // Re-read the live effect list AFTER our apply/remove so the "equipped"
        // set reflects the CURRENT state, not the stale pre-removal snapshot (which
        // wrongly flagged an effect we just removed on a family switch). Anything
        // active that we didn't apply comes from an actually-equipped talisman.
        enumerate_speffects(player, active);
        {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            g_state.external_active.clear();
            for (int id : active)
                if (!applied.count(id))
                    g_state.external_active.insert(id);
        }

        Sleep(kPollMs);
    }
}

// ---- worker thread ----
DWORD WINAPI run(LPVOID) {
    Ini ini;
    const bool loaded = ini.load(config_path());
    g_debug    = ini.get_bool("debugging", "debug_console", false);
    g_log_each = ini.get_bool("debugging", "log_each", false);
    if (g_debug) {
        AllocConsole();
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
    }
    flog(loaded ? "config loaded"
                : "[WARN] .ini not found next to the DLL; using defaults");

    const IniConfig cfg = load_config(ini);

    try {
        g_mod = mem::main_module();

        // Resolve the apply / remove game functions.
        g_apply  = reinterpret_cast<ApplySpEffect_t>(
            resolve_fn(kApplySpEffectAob, kApplySpEffectFuncBackset, "ApplySpEffect"));
        g_remove = reinterpret_cast<RemoveSpEffect_t>(
            resolve_fn(kRemoveSpEffectAob, 0, "RemoveSpEffect"));
        if (!g_apply)
            flog("[ERROR] no apply function -- talismans will NOT be applied");

        // In-game overlay: capture DX12 vtables, queue hooks, commit them.
        if (hooks::init()) {
            overlay::setup();
            if (hooks::apply()) flog("hooks enabled");
            else                flog("[WARN] MH_ApplyQueued failed -- overlay disabled");
        } else {
            flog("[WARN] MinHook init failed -- overlay disabled");
        }

        flog("waiting for params...");
        from::CS::SoloParamRepository::wait_for_params(-1);
        flog("params ready -- building talisman model");
        build_state(cfg);
        flog("entering apply/remove loop");
        run_loop();
    } catch (const std::exception& e) {
        flog("[ERROR] exception: %s", e.what());
    } catch (...) {
        flog("[ERROR] unknown exception in worker");
    }
    return 0;
}

} // namespace
} // namespace cte

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        cte::g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
        cte::log_line("==== CustomTalismanEffects loaded (DllMain attach) ====",
                      /*truncate=*/true);
        CreateThread(nullptr, 0, cte::run, nullptr, 0, nullptr);
    }
    return TRUE;
}
