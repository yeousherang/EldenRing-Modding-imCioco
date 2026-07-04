#pragma once

#include <cstdint>
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

} // namespace cte
