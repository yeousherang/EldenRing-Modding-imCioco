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
#include "messages.hpp"
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
    const std::string norm = normalize(raw);
    if (norm.empty()) return def;
    // Explicit opt-out: "none"/"off"/"disabled"/"disable" turns the gamepad combo
    // off entirely (0 == no button can ever satisfy the combo check), instead of
    // falling back to the default combo like an empty/unrecognized value does.
    if (norm == "none" || norm == "off" || norm == "disabled" || norm == "disable")
        return 0;
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
    bool           progression_mode = false;
    unsigned int   open_vk = VK_INSERT;
    unsigned short open_pad_mask = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
    std::string    open_key_label = "Insert"; // raw .ini strings, for the overlay footer
    std::string    open_pad_label = "L3+R3";
    bool show_descriptions = true;
    int  sort_mode = 0;
};

IniConfig load_config(const Ini& ini) {
    IniConfig c;
    c.allow_stacking = ini.get_bool("overlay", "allow_stacking", false);
    c.progression_mode = ini.get_bool("overlay", "progression_mode", false);
    c.open_key_label = ini.get_string("overlay", "toggle_key", "Insert");
    c.open_pad_label = ini.get_string("overlay", "toggle_gamepad_combo", "L3+R3");
    c.open_vk = parse_open_key(c.open_key_label, VK_INSERT);
    c.open_pad_mask = parse_pad_mask(c.open_pad_label,
                                     XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB);
    c.show_descriptions = ini.get_bool("overlay", "show_descriptions", true);
    c.sort_mode = ini.get_int("overlay", "sort_mode", 0);
    for (const auto& kv : ini.section_items("talismans"))
        if (Ini::as_bool(kv.second))
            c.enabled.insert(normalize(kv.first));
    return c;
}

// id -> baked table entry (curated name + effect). Built once. Doubles as the
// set of "base game" ids used to split the overlay into Base vs Mod-Added.
const std::unordered_map<int, const TalismanEntry*>& baked_by_id() {
    static const std::unordered_map<int, const TalismanEntry*> m = [] {
        std::unordered_map<int, const TalismanEntry*> t;
        t.reserve(kTalismanCount);
        for (const auto& e : kTalismans) t.emplace(e.id, &e);
        return t;
    }();
    return m;
}

// EquipParamAccessory rows to never surface: cut / unused content that isn't
// obtainable in-game. 6100 "Entwining Umbilical Cord" is a cut duplicate whose
// live name resolves to a broken "[ERROR]..." string (and, being unmatched to the
// real .ini entry, otherwise gets re-appended on every save).
const std::unordered_set<int> kBlacklistedAccessories = { 6100 };

// After params load, build the shared talisman model by walking EVERY row of the
// live EquipParamAccessory -- so mod-added talismans are picked up automatically,
// not just the baked base-game set. For each row we read accessoryGroup +
// resident/refId SpEffects live, and resolve the display NAME (and, as a
// fallback, the effect text) from the game's live message repository so renamed /
// repurposed talismans (Reforged, The Convergence) show correctly. Enabled flag
// comes from the .ini (keyed by name).
void build_state(const IniConfig& cfg) {
    // Live talisman names/effects from the game's message repository (mod-aware).
    // Feed it the full live accessory id set -- it fingerprints the description
    // FMG slots by matching their resolving-string id sets against these ids.
    std::vector<int> all_ids;
    for (auto [pid, row] : from::param::EquipParamAccessory) {
        (void)row;
        all_ids.push_back(static_cast<int>(pid));
    }
    const bool live = messages::init(all_ids);

    std::lock_guard<std::mutex> lk(g_state_mutex);
    g_state.allow_stacking = cfg.allow_stacking;
    g_state.progression_mode = cfg.progression_mode;
    g_state.open_vk = cfg.open_vk;
    g_state.open_pad_mask = cfg.open_pad_mask;
    g_state.open_key_label = cfg.open_key_label;
    g_state.open_pad_label = cfg.open_pad_label;
    g_state.show_descriptions = cfg.show_descriptions;
    g_state.sort_mode = cfg.sort_mode;
    g_state.talismans.clear();

    const auto& baked = baked_by_id();

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

    int noeffect = 0, noname = 0, from_ref = 0, from_resident = 0, mod_added = 0;
    for (auto [pid, row] : from::param::EquipParamAccessory) {
        Talisman t;
        t.accessory_id = static_cast<int>(pid);
        if (kBlacklistedAccessories.count(t.accessory_id)) continue; // cut content
        t.group = row.accessoryGroup;
        t.sort_id = row.sortId;

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

        if (t.sp_ids.empty()) { ++noeffect; continue; } // not a passive-buff talisman

        const auto it = baked.find(t.accessory_id);
        t.is_base = it != baked.end();

        // Name: live FMG (correct for any mod / rename) -> baked -> skip the row.
        // A row with neither a live nor a baked name is a template/system entry.
        if (live) t.name = messages::accessory_name(t.accessory_id);
        if (t.name.empty() && it != baked.end()) t.name = it->second->name;
        if (t.name.empty()) { ++noname; continue; }
        // A live name that resolves to the game's "[ERROR]" placeholder marks a
        // broken/cut row; drop it (a leading '[' would also be misread as a
        // section header when written to the .ini). Prefer the baked name if any.
        if (t.name.rfind("[ERROR]", 0) == 0) {
            if (it != baked.end() && it->second->name[0] != '\0') t.name = it->second->name;
            else { ++noname; continue; }
        }

        // Effect: curated baked text preferred; else live AccessoryInfo/Caption.
        if (it != baked.end() && it->second->effect[0] != '\0')
            t.effect = it->second->effect;
        else if (live)
            t.effect = messages::accessory_effect(t.accessory_id);

        if (ref_hit) ++from_ref;
        if (res_hit) ++from_resident;
        if (!t.is_base) ++mod_added;

        t.enabled = cfg.enabled.count(normalize(t.name)) != 0;
        g_state.talismans.push_back(std::move(t));
    }

    g_state.has_mod_added = mod_added > 0;

    // With an overhaul loaded, it typically also retunes VANILLA talismans, so
    // our baked descriptions can go stale. Read EVERY description live from the
    // game in that case (keeping the baked text only where the game has none).
    // On the plain vanilla game we keep the curated baked descriptions.
    if (live && mod_added > 0) {
        for (auto& t : g_state.talismans) {
            std::string le = messages::accessory_effect(t.accessory_id);
            if (!le.empty()) t.effect = std::move(le);
        }
    }

    if (!cfg.allow_stacking) {
        collapse_groups_locked(); // honor exclusivity for the loaded .ini selection /+ unless stacking option is enabled.
    }
    sort_talismans_locked();
    flog("built %zu talismans (%d mod-added, %d without effects, %d unnamed; "
         "%d have a refId effect, %d have resident effect(s))",
         g_state.talismans.size(), mod_added, noeffect, noname, from_ref, from_resident);
}

// Rewrite the .ini in place: flip each [talismans] Name to the current enabled
// state and update [overlay] allow_stacking. Preserves comments/formatting; only
// touches the value after '='. Called when the overlay requests a save.
void save_config() {
    const std::wstring path = config_path();
    std::ifstream in(path);
    if (!in) { flog("[WARN] save: cannot open .ini for read"); return; }

    // Snapshot selections (display name + enabled) + overlay options under the
    // lock. Keep display names so mod-added talismans absent from the .ini can be
    // appended as new lines below.
    struct Sel { std::string display, norm; bool enabled; };
    std::vector<Sel> sels;
    std::unordered_map<std::string, bool> want; // normalized name -> enabled
    bool allow_stacking;
    bool progression_mode;
    bool show_descriptions;
    int  sort_mode;
    std::string open_key_label, open_pad_label;
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        for (const auto& t : g_state.talismans) {
            sels.push_back({t.name, normalize(t.name), t.enabled});
            want[normalize(t.name)] = t.enabled;
        }
        allow_stacking = g_state.allow_stacking;
        progression_mode = g_state.progression_mode;
        show_descriptions = g_state.show_descriptions;
        sort_mode = g_state.sort_mode;
        open_key_label = g_state.open_key_label;
        open_pad_label = g_state.open_pad_label;
    }

    // Every [overlay] option we manage, with its CURRENT value + a default
    // comment. Used both to update existing lines (below) and, crucially, to
    // APPEND any option missing from an older player's .ini so a new DLL version
    // self-heals the file instead of the player having to replace it. Keyed by
    // normalized key (lowercased); see normalize().
    struct OverlayOpt { const char* key; std::string value; const char* comment; };
    const std::vector<OverlayOpt> overlay_opts = {
        {"toggle_key", open_key_label, "; key to open/close the in-game panel"},
        {"toggle_gamepad_combo", open_pad_label,
         "; controller combo to open/close (buttons joined with +, or \"none\")"},
        {"allow_stacking", allow_stacking ? "1" : "0",
         "; 1 = ignore talisman families (stack anything)"},
        {"progression_mode", progression_mode ? "1" : "0",
         "; 1 = only show/apply talismans you currently own in your inventory"},
        {"show_descriptions", show_descriptions ? "1" : "0",
         "; 1 = show the effect description pane at the bottom of the overlay"},
        {"sort_mode", std::to_string(sort_mode),
         "; overlay list order: 0 = Talisman ID, 1 = Name (A-Z), 2 = In-game menu order"},
    };

    std::vector<std::string> out;
    std::unordered_set<std::string> present; // normalized [talismans] keys already in the file
    std::unordered_set<std::string> overlay_present; // normalized [overlay] keys already in the file
    int tal_end = -1;                        // out index just after the last [talismans] entry
    int overlay_end = -1;                    // out index just after the last [overlay] key line
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
                present.insert(normalize(key_trim));
                auto it = want.find(normalize(key_trim));
                if (it != want.end()) {
                    std::string nv = it->second ? "1" : "0";
                    line = key + "= " + nv + (comment.empty() ? "" : " " + comment);
                }
            } else if (section == "overlay") {
                overlay_present.insert(normalize(key_trim));
                if (normalize(key_trim) == "allow_stacking") {
                    line = key + "= " + (allow_stacking ? "1" : "0") +
                           (comment.empty() ? "" : " " + comment);
                } else if (normalize(key_trim) == "progression_mode") {
                    line = key + "= " + (progression_mode ? "1" : "0") +
                           (comment.empty() ? "" : " " + comment);
                } else if (normalize(key_trim) == "show_descriptions") {
                    line = key + "= " + (show_descriptions ? "1" : "0") +
                           (comment.empty() ? "" : " " + comment);
                } else if (normalize(key_trim) == "sort_mode") {
                    line = key + "= " + std::to_string(sort_mode) +
                           (comment.empty() ? "" : " " + comment);
                }
            }
        }
        out.push_back(line);
        if (section == "talismans" && eq != std::string::npos)
            tal_end = static_cast<int>(out.size()); // insert new entries after the last one
        if (section == "overlay" && eq != std::string::npos)
            overlay_end = static_cast<int>(out.size()); // insert missing options after the last one
    }
    in.close();

    // Append talismans that aren't in the file yet (mod-added ones enabled via the
    // overlay). Name-keyed, so two talismans sharing a normalized name collapse to
    // one line (a known limitation of name keying) -- dedup on the normalized key.
    std::vector<std::string> added;
    std::unordered_set<std::string> added_norm;
    for (const auto& s : sels) {
        if (present.count(s.norm) || !added_norm.insert(s.norm).second) continue;
        added.push_back(s.display + " = " + (s.enabled ? "1" : "0"));
    }
    if (!added.empty()) {
        if (tal_end < 0 || tal_end > static_cast<int>(out.size())) { // no [talismans] section found
            out.push_back("");
            out.push_back("[talismans]");
            tal_end = static_cast<int>(out.size());
        }
        out.insert(out.begin() + tal_end, added.begin(), added.end());
        flog("added %zu mod-added talisman(s) to the .ini", added.size());
    }

    // Self-migration: append any managed [overlay] option missing from the file
    // (e.g. a new option shipped in a DLL update) with its current/default value,
    // so upgrading players keep their settings and just gain the new line. Done
    // after the talismans insert above; overlay_end < tal_end, so that insert did
    // not shift it.
    std::vector<std::string> new_opts;
    for (const auto& o : overlay_opts)
        if (!overlay_present.count(normalize(o.key)))
            new_opts.push_back(std::string(o.key) + " = " + o.value + " " + o.comment);
    if (!new_opts.empty()) {
        if (overlay_end < 0 || overlay_end > static_cast<int>(out.size())) { // no [overlay] section
            out.push_back("");
            out.push_back("[overlay]");
            overlay_end = static_cast<int>(out.size());
        }
        out.insert(out.begin() + overlay_end, new_opts.begin(), new_opts.end());
        flog("added %zu missing [overlay] option(s) to the .ini", new_opts.size());
    }

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

// Resolve a global POINTER VARIABLE referenced by a `mov reg,[rip+disp32]` (disp32
// at instruction offset 3, instruction length 7). Unlike resolve_fn this returns
// the address of the variable, not a function entry. Returns 0 if not found/unique.
uintptr_t resolve_global_ptr(const char* aob, const char* name) {
    bool multiple = false;
    const uintptr_t hit = mem::aob_scan_unique(g_mod, aob, &multiple);
    if (!hit || multiple) {
        flog("[WARN] %s AOB %s", name, multiple ? "matched MULTIPLE sites" : "not found");
        return 0;
    }
    const uintptr_t var = mem::rip_relative(hit, 3, 7);
    flog("%s resolved: var=%p", name, reinterpret_cast<void*>(var));
    return var;
}

// The apply/remove diff loop.
void run_loop() {
    std::vector<int> active;
    std::unordered_set<int> active_set;
    std::unordered_set<int> applied;   // effects WE applied (ours to remove)
    bool warned_no_remove = false;

    for (;;) {
        // Progression Mode: refresh the possessed-talisman set before diffing so
        // the gate below reflects the current inventory. A failed read keeps the
        // previous set (and possessed_valid), so a transient miss can't wrongly
        // wipe out every effect.
        bool progression = false;
        {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            progression = g_state.progression_mode;
        }
        if (progression) {
            std::vector<int> owned;
            if (enumerate_inventory_accessories(owned)) {
                std::lock_guard<std::mutex> lk(g_state_mutex);
                g_state.possessed_accessories.clear();
                g_state.possessed_accessories.insert(owned.begin(), owned.end());
                g_state.possessed_valid = true;
            }
        }

        // Snapshot desired effects + drain a save request under the lock. In
        // Progression Mode, suppress talismans the player doesn't currently own
        // (gate fails open until the first good inventory read).
        std::unordered_set<int> desired;
        bool do_save = false;
        {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            const bool gate = g_state.progression_mode && g_state.possessed_valid;
            for (const auto& t : g_state.talismans) {
                if (!t.enabled) continue;
                if (gate && !g_state.possessed_accessories.count(t.accessory_id)) continue;
                for (int sp : t.sp_ids) desired.insert(sp);
            }
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

        // Resolve the GameDataMan global (for Progression Mode inventory reads).
        // Optional: if it fails, Progression Mode safely no-ops (gate fails open).
        g_gamedataman_var = resolve_global_ptr(kGameDataManAob, "GameDataMan");
        if (!g_gamedataman_var)
            flog("[WARN] GameDataMan unresolved -- Progression Mode disabled");

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

        // Self-heal the .ini: if a managed [overlay] option is missing (e.g. a
        // new option added in this DLL version), write it out now with its
        // default so upgrading players gain the line without losing settings --
        // even if they never open the overlay. save_config preserves everything
        // else. Only when the .ini actually exists (else save_config warns).
        if (loaded) {
            static const char* kOverlayKeys[] = {
                "toggle_key", "toggle_gamepad_combo", "allow_stacking",
                "progression_mode", "show_descriptions", "sort_mode",
            };
            bool needs_migration = false;
            for (const char* k : kOverlayKeys)
                if (!ini.has("overlay", k)) { needs_migration = true; break; }
            if (needs_migration) {
                flog("migrating .ini: adding missing [overlay] option(s)");
                save_config();
            }
        }

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
