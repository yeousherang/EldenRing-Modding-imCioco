#pragma once

// Shared, mutex-guarded model of which talismans are enabled + the overlay
// options. Written by the ImGui overlay (user clicks) and read by the worker
// loop (which applies/removes the SpEffects). Also the source the overlay's
// "Save" writes back to the .ini.

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace cte {

struct Talisman {
    int              accessory_id = 0;
    std::string      name;                 // display name (live FMG, else baked) -- .ini key
    std::string      effect;               // human-readable effect (overlay hover text)
    int              group = -1;           // accessoryGroup; -1 == no family
    int              sort_id = 0;          // sortId (EquipParamAccessory) == in-game menu order
    std::vector<int> sp_ids;               // resident SpEffect ids (>0)
    bool             enabled = false;      // desired on/off
    bool             is_base = true;       // id is in the baked base-game table (Base vs Mod-Added tab)
};

struct State {
    std::vector<Talisman> talismans;

    bool allow_stacking = false;           // ignore talisman families when set

    // Progression Mode: only display + apply talismans the player currently
    // owns in their inventory, so the usable pool grows as they find talismans.
    bool progression_mode = false;
    // Accessory ids the player currently possesses (refreshed by the worker loop
    // while progression_mode is on). `possessed_valid` stays false until the
    // first good inventory read, so the gate "fails open" -- if GameDataMan is
    // unresolved or the bag hasn't been read, nothing is hidden/suppressed.
    std::unordered_set<int> possessed_accessories;
    bool possessed_valid = false;

    // Overlay open/close inputs (configurable in the .ini).
    unsigned int   open_vk       = 0x2D;   // VK_INSERT
    unsigned short open_pad_mask = 0x00C0; // XINPUT LEFT_THUMB | RIGHT_THUMB (L3+R3)
    // Human-readable labels of the above (the raw .ini strings), for the
    // overlay's hotkey-hint footer.
    std::string open_key_label = "Insert";
    std::string open_pad_label = "L3+R3";

    bool save_requested = false;           // overlay asked to persist to the .ini

    bool show_descriptions = true;
    int  sort_mode = 0; // 0 = Talisman ID, 1 = Name, 2 = Group (in-game order)

    // True when the loaded regulation has talismans beyond the baked base-game
    // set (an overhaul is active). Drives whether the overlay shows the
    // Base/Mod-Added tabs at all.
    bool has_mod_added = false;

    // SpEffect ids active on the player from a source OTHER than this mod
    // (i.e. an actually-equipped talisman). Published by the worker loop each
    // tick; the overlay uses it to highlight / lock already-equipped talismans.
    std::unordered_set<int> external_active;
};

// Lock this around every access to g_state.
extern std::mutex g_state_mutex;
extern State      g_state;

// --- helpers that assume g_state_mutex is ALREADY held ---------------------

// After enabling talismans[idx], enforce family exclusivity: if stacking is off
// and this talisman has a real group (>= 0), disable every other enabled
// talisman sharing that group.
void apply_exclusivity_locked(size_t idx);

// Collapse every family to at most one enabled member (used when stacking is
// turned back off, and at startup after loading the .ini).
void collapse_groups_locked();

// Re-sort g_state.talismans in place according to g_state.sort_mode
// (0 = Talisman ID, 1 = Name, 2 = In-game menu order / sortId).
void sort_talismans_locked();

} // namespace cte
