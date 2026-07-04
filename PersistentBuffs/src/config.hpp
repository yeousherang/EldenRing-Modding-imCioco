#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pb {

extern HINSTANCE g_hinst;
extern bool      g_debug;

// ---- config ------------------------------------------------------------
extern bool g_keep_fast_travel;
extern bool g_keep_death;
// Diagnostic: log every SpEffect id that appears/disappears on the player (raw,
// unfiltered). Use it to discover system effect ids -- e.g. walk into Roundtable
// Hold and see which id the no-combat state adds. Off by default (chatty).
extern bool g_log_effects;
// Wait this long after the player reappears (transition finished) before
// re-applying, so the world/character is fully settled and the engine has
// already done its wipe. Avoids re-applying into a half-loaded ChrIns.
// Hard-coded (no longer .ini-configurable) -- 500 ms is the tuned value.
constexpr int kReapplyDelayMs = 500;
// Death pre-strip handling. On DEATH the engine strips weapon/AoW buffs at the
// killing blow, but the player ChrIns stays valid through the whole "YOU DIED" /
// fade / reload sequence -- MANY seconds later the ptr finally nulls. During that
// window the poll keeps seeing the depleted set, so a plain "remember the latest
// snapshot" (or any short history ring) gets eroded to the post-strip set well
// before the transition fires -> weapon/AoW buffs are lost on death (fast travel
// is unaffected: it nulls the ptr in the SAME tick as the wipe, so the depleted
// set is never observed while the player is valid).
//
// Fix: when a sudden mass buff-loss is seen (>= kDeathDropThreshold persistable
// buffs vanish in one tick) we treat it as the death pre-strip and FREEZE
// `remembered` at the pre-strip set until the transition re-applies it. If the
// player instead keeps playing past kFreezeMaxTicks with no transition, the drop
// was a real change (not a death) -> we unfreeze and resync. (A rare false
// positive -- e.g. several physick tears expiring together -- at worst re-applies
// a couple of just-expired buffs on the next transition, which is benign.)
constexpr size_t kDeathDropThreshold = 2;
constexpr int    kFreezeMaxTicks     = 100; // ~20s at the 200ms poll
// ---- buff timing (buff_timing.cpp) --------------------------------------
// Restore buffs with their REMAINING time instead of a fresh full duration
// ([persistence] restore_remaining_time). The timing module tracks per-buff
// elapsed time (clock frozen while a buff is absent -- death strip, loading)
// and reapply/weapon-memory refuse to resurrect a buff whose own timer ran
// out (the "expired 15s grease came back on fast travel" bug).
extern bool g_restore_remaining;
// A buff counts as expired once its remaining time is at/below this margin
// (absorbs the 200ms poll granularity + the 500ms settle wait). Also the
// minimum duration ever handed to a patch-around restore.
constexpr float kExpiryMarginS = 1.0f;
// Per-tick dt cap for the elapsed clocks: a debugger break / process suspend
// must not eat a buff's whole remaining time in one tick.
constexpr double kMaxTickDtS = 1.0;
// After our own re-apply, the id should reappear within this many ticks; a
// reappearance carrying the mark keeps its elapsed clock (the engine was
// given the remaining, not the full, duration).
constexpr int kExpectReappearTicks = 15; // ~3s at the 200ms poll
// Refresh self-heal: a finite tracked buff still PRESENT this many ticks past
// its computed expiry means the engine disagrees with our clock -> the player
// refreshed it (a recast resets the engine timer without the id ever
// disappearing) -> restart our clock. Fails safe: never wrongly vetoes.
constexpr int kRefreshGraceTicks = 10; // ~2s at the 200ms poll
// Experimental: remember which weapon-bound buffs (greases / blade spells) were
// on each right-hand weapon, and re-apply them when you swap back to that weapon
// (vanilla drops them on a weapon swap). Default off. Right-hand only for now.
extern bool g_weapon_memory;
// ---- cross-session persistence (session_store.cpp) ----------------------
// EXPERIMENTAL, default off. Save the character's active persistable buffs with
// their REMAINING seconds to PersistentBuffs.state.ini next to the DLL, and
// restore them on the next game launch for the same character (time does NOT
// pass while the game is closed; infinite buffs stay infinite). Keyed by
// character name ([session] remember_across_sessions). See session_store.hpp.
extern bool g_session_persist;
// Autosave cadence (crash insurance) + a hard cap on how many buffs are stored
// per character so a corrupt/edited file can't blow up the restore.
constexpr int    kSessionSaveTicks = 25; // ~5s at the 200ms poll
constexpr size_t kSessionMaxBuffs  = 64;
// Buffs to treat as CHARACTER-WIDE rather than weapon-bound: AoW self-buffs
// (Endure, Determination, Royal Knight's Resolve, Roars, War Cry, Golden Vow...)
// are stat buffs that merely happen to be cast from a weapon art -- they should
// survive any weapon swap, not get re-bound to the casting weapon like a grease,
// AND they must survive death/fast travel. HARD-CODED (not .ini-configurable):
// the "Damage Buff" rows (incl. No-FP variants) from soulsmods/Paramdex, same set
// InfiniteWeaponBuffs uses. Element weapon-enchants (Sacred Blade, Chilling Mist,
// Cragblade, ...) are deliberately excluded -- those belong to the weapon.
inline const std::unordered_set<int> g_always_persist = {
    841, 843, 846, 848,           // Roar
    1586, 1588,                   // Jellyfish Shield
    1650, 1651, 1655, 1656,       // Endure (poise)
    1681, 1683, 1686, 1688,       // Barbaric/Milos Roar
    1691, 1693, 1696, 1698,       // Determination
    1701, 1703, 1706, 1708,       // Royal Knight's Resolve
    1730, 1732,                   // Golden Vow
    1811, 1813, 1816, 1818,       // War Cry
    1861, 1863, 1866, 1868,       // Braggart's Roar
};
// SpEffect ids to ALWAYS persist, bypassing the field-based buff gate. Safety
// valve for a genuine buff the SpEffectParam check misreads as "not a buff" (the
// check has known gaps -- e.g. poise-only effects). Found via the filter log;
// no rebuild needed ([persistence] force_persist_ids). See is_persistable.
extern std::unordered_set<int> g_force_persist;
// Optional id -> human name map for the log, loaded from a Paramdex-format names
// file next to the DLL (see load_names). Empty if the file is absent -> the log
// falls back to bare ids. Purely cosmetic (makes the log readable).
extern std::unordered_map<int, std::string> g_names;

// Parse a list of integer ids from a string (any non-digit is a separator).
std::unordered_set<int> parse_id_list(const std::string& s);

// Load the optional Paramdex names file (id -> name) for readable logs. Each line
// is "<id> <name>" (leading integer, rest of the line is the name), matching
// soulsmods/Paramdex ER/Names/SpEffectParam.txt. Returns the count loaded.
size_t load_names();

// Format an id for the log: "id" or, if a name is known, "id:Name".
std::string named(int id);

// Space-separated id (or id:Name) list for logging.
std::string join_ids(const std::vector<int>& ids);

// Loads config.ini (or falls back to defaults if it's missing), sets the
// toggles above, loads the optional names file, and logs a startup summary.
// Call once from DllMain.
void load_config();

} // namespace pb
