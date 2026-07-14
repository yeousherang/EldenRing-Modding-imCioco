#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "scan.hpp"

namespace cte {

// ---- game access -------------------------------------------------------
extern mem::Module g_mod;

// Resolve the local player ChrIns via WorldChrMan. Returns 0 if not in-game.
uintptr_t get_player_ins();

// Walk the player's active SpEffect linked list, collecting effect ids.
//   player + 0x178      -> SpEffect manager
//   manager + 0x8       -> first slot (the list head is one indirection below)
//   slot + 0x8 (int)    -> effect id
//   slot + 0x30 (ptr)   -> next slot
void enumerate_speffects(uintptr_t player, std::vector<int>& out);

// Resolve PlayerGameData via the GameDataMan global. Returns 0 if unavailable
// (GameDataMan AOB unresolved, or not yet in a game).
uintptr_t get_player_game_data();

// Read the loaded character's display name (PlayerGameData + kCharNameOffset,
// UTF-16, <=16 chars). Empty string when unavailable: GameDataMan unresolved,
// no character loaded (main menu), or the read tripped a control char (wrong
// offset for this build). Used to key per-character presets. ⚠ the offset is
// UNVERIFIED -- see offsets.hpp / the `characters: key=` log line.
std::wstring get_character_name();

// Collect the EquipParamAccessory ids of the talismans the player currently
// POSSESSES (walking the normal-item inventory; equipped talismans still count).
// Returns false on any bad read so the caller can keep its previous set rather
// than wrongly clearing it; on success `out` is the possessed accessory-id list.
bool enumerate_inventory_accessories(std::vector<int>& out);

} // namespace cte
