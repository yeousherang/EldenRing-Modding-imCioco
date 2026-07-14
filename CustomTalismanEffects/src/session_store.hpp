#pragma once

// ---- per-character presets --------------------------------------------------
// Each character gets its own preset: which talismans are enabled + the two
// gameplay toggles (allow_stacking, progression_mode). Stored SPARSE -- only the
// enabled talismans (by EquipParamAccessory id) are written -- in a machine-
// written, atomically-rewritten CustomTalismanEffects.state.ini next to the DLL
// (keyed [char_<key>] sections). The human-edited CustomTalismanEffects.ini keeps
// the FULL talisman list and serves as the default template + first-character seed.
//
// Ported from PersistentBuffs/src/session_store.cpp (character-name key, the
// keyed-ini mirror + atomic tmp+MoveFileEx rewrite). The 0x9C name offset is
// UNVERIFIED (see offsets.hpp) -- a bad read degrades to the global .ini.

#include <string>
#include <utility>
#include <vector>

namespace cte {

// Derive a stable, ini/filesystem-safe key from a character name: sanitized UTF-8
// (<=24 chars, [A-Za-z0-9_-], others -> '_') + '_' + 4 hex of FNV-1a over the raw
// UTF-16 bytes. Empty name -> "" (caller treats as "identity unavailable").
std::string char_key(const std::wstring& name);

// Resolve the current character's key from get_character_name(). On a FAILED read
// (empty name): keep `prev_key` if we have one -- a transient bad read must never
// look like a character switch -- else "". `*name_ok`, if given, reports whether a
// real name was read this call.
std::string current_char_key(const std::string& prev_key, bool* name_ok);

// Parse CustomTalismanEffects.state.ini into the in-memory mirror. Call once at
// startup (after GameDataMan resolves). Missing file / version mismatch -> fresh.
// Mirror access below is worker-thread-only; no lock needed for those.
void presets_startup_load();

// True if ANY [char_*] preset exists (first-character seeding vs import prompt).
bool presets_any();

// True if a preset section exists for `key`.
bool presets_has(const std::string& key);

// (section key, display name) for every stored character -- feeds the import UI.
std::vector<std::pair<std::string, std::string>> presets_list();

// --- the following touch g_state; the CALLER must hold g_state_mutex ---------

// Apply character `key`'s stored preset to g_state: enable exactly its stored
// ids (all other talismans off), set allow_stacking / progression_mode, and (when
// stacking is off) collapse families. A missing key clears to all-off. Returns
// true iff a section existed.
bool presets_load_into(const std::string& key);

// Clear g_state to the blank default: every talisman off, allow_stacking off,
// progression_mode off. Used for a new character that will show the import prompt.
void presets_clear_state();

// Save g_state's CURRENT selections (enabled ids + the two toggles) under `key`,
// atomically rewriting the whole state file. `display` is the raw character name
// stored for the import UI. Only enabled ids are written. No-op on an empty key.
void presets_save(const std::string& key, const std::wstring& display);

// Copy character `src_key`'s stored preset into g_state (enabled ids + toggles).
// Returns true iff `src_key` existed.
bool presets_import_into(const std::string& src_key);

} // namespace cte
