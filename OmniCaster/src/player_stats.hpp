// Read the player's EFFECTIVE INT / FAI for highest-stat mode.
//
//   GameDataMan (global, AOB-resolved) + 0x08 -> main_player_game_data
//   PlayerGameData + 0x2A0 -> effective Intelligence
//   PlayerGameData + 0x2A4 -> effective Faith
//
// "Effective" = the numbers the status screen shows: base level-up stats plus
// every talisman / physick / buff / great-rune bonus, computed by the game
// itself. The base stats live at +0x3C (VIG,MND,END,STR,DEX,INT,FAI,ARC) and
// the effective block at +0x288 (VIG,MND,END,VIT,STR,DEX,INT,FAI,ARC).
// Layout source: vswarte/fromsoftware-rs crates/eldenring PlayerGameData
// (offsets cross-anchored by its unk2ac / unk8e8 field names, and by the
// character name at +0x9C matching Erd-Tools / CustomTalismanEffects).
#pragma once

#include <cstdint>

namespace omni {

// AOB for the GameDataMan global pointer (`mov rax,[rip+disp32]`, disp at +3,
// insn len 7). Same signature The Grand Archives CT and CTE use.
constexpr const char* kGameDataManAob =
    "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3";
constexpr uintptr_t kPlayerGameDataOffset = 0x08;  // GameDataMan.main_player_game_data
constexpr uintptr_t kBaseStatBlockOffset  = 0x3C;  // 8 u32: VIG..ARC (base)
constexpr uintptr_t kEffStatBlockOffset   = 0x288; // 9 u32: VIG,MND,END,VIT,STR,DEX,INT,FAI,ARC
constexpr uintptr_t kEffIntOffset         = 0x2A0; // effective Intelligence
constexpr uintptr_t kEffFaithOffset       = 0x2A4; // effective Faith
constexpr uintptr_t kBaseIntOffset        = 0x50;
constexpr uintptr_t kBaseFaithOffset      = 0x54;
constexpr uintptr_t kIsMainPlayerOffset   = 0x8F0; // bool, diagnostic only

struct CasterStats {
    int eff_int = 0, eff_fai = 0;   // what the status screen shows
    int base_int = 0, base_fai = 0; // level-up stats (for the log)
};

// Resolve the GameDataMan global. Returns false (and logs) when the AOB
// doesn't resolve -- highest-stat mode then stays on its INT default.
bool player_stats_init();

// Read effective (+ base, for logging) INT/FAI. Returns false when no
// character is loaded (main menu) or a read faults; `out` untouched then.
bool read_caster_stats(CasterStats& out);

// Log the raw base + effective stat blocks for verification (dump mode).
void dump_stat_block();

} // namespace omni
