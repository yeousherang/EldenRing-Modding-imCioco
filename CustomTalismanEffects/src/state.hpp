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
    std::string      name;                 // Paramdex display name (.ini key)
    std::string      effect;               // human-readable effect (overlay hover text)
    int              group = -1;           // accessoryGroup; -1 == no family
    std::vector<int> sp_ids;               // resident SpEffect ids (>0)
    bool             enabled = false;      // desired on/off
};

struct State {
    std::vector<Talisman> talismans;

    bool allow_stacking = false;           // ignore talisman families when set

    // Overlay open/close inputs (configurable in the .ini).
    unsigned int   open_vk       = 0x2D;   // VK_INSERT
    unsigned short open_pad_mask = 0x00C0; // XINPUT LEFT_THUMB | RIGHT_THUMB (L3+R3)

    bool save_requested = false;           // overlay asked to persist to the .ini

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

} // namespace cte
