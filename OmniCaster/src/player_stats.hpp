// Read the player's EFFECTIVE INT / FAI for highest-stat mode.
//
// Base stats:
//   GameDataMan (global, AOB-resolved) + 0x08 -> PlayerGameData
//   PlayerGameData + 0x50 -> Intelligence, + 0x54 -> Faith
//   (stat block at +0x3C: VIG,MND,END,STR,DEX,INT,FAI,ARC -- VERIFIED in-game
//   2026-07-14 against the status screen)
//
// Equipment/buff bonuses (talismans, physick, buffs) are SpEffects: walk the
// player's active SpEffect list (WorldChrMan -> PlayerIns -> effect list, the
// same offsets CustomTalismanEffects uses, confirmed in-game there) and sum
// SpEffectParam.addMagicStatus / addFaithStatus. effective = base + sum.
#pragma once

#include <cstdint>

namespace omni {

// AOB for the GameDataMan global pointer (`mov rax,[rip+disp32]`, disp at +3,
// insn len 7). Same signature CustomTalismanEffects uses.
constexpr const char* kGameDataManAob =
    "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3";
constexpr uintptr_t kPlayerGameDataOffset = 0x08;
constexpr uintptr_t kStatBlockOffset      = 0x3C; // vigor..arcane, 8 ints
constexpr uintptr_t kIntOffset            = 0x50;
constexpr uintptr_t kFaithOffset          = 0x54;

// WorldChrMan singleton pointer = module_base + this (libER GLOBAL_WorldChrMan;
// exe-version specific). Then the active SpEffect list, all as verified by
// CustomTalismanEffects in-game:
constexpr uintptr_t kWorldChrManOffset       = 0x3D65F88;
constexpr uintptr_t kPlayerInsOffset         = 0x1E508;
constexpr uintptr_t kSpEffectManagerOffset   = 0x178;
constexpr uintptr_t kSpEffectFirstSlotOffset = 0x8;
constexpr uintptr_t kSpEffectIdOffset        = 0x8;  // int  effect id
constexpr uintptr_t kSpEffectNextOffset      = 0x30; // ptr  next slot

struct CasterStats {
    int base_int = 0, base_fai = 0;   // level-up stats
    int bonus_int = 0, bonus_fai = 0; // from talismans / physick / buffs
    int eff_int() const { return base_int + bonus_int; }
    int eff_fai() const { return base_fai + bonus_fai; }
};

// Resolve the GameDataMan global. Returns false (and logs) when the AOB
// doesn't resolve -- highest-stat mode then stays on its INT default.
bool player_stats_init();

// Read base + bonus INT/FAI. Returns false when no character is loaded (main
// menu) or a read faults; `out` is untouched then. If only the bonus walk
// fails, bonuses are 0 and base stats are still returned.
bool read_caster_stats(CasterStats& out);

// Log the raw 8-int stat block for offset verification (dump mode).
void dump_stat_block();

} // namespace omni
