// Read the live player's current INT / FAI for highest-stat mode.
//
// There are two important copies of player data:
//   GameDataMan + 0x08 is persistent/menu-facing data and can briefly describe
//   the character-selection preview or the previously loaded character.
//   WorldChrMan + 0x1E508 is the current in-world PlayerIns; PlayerIns + 0x580
//   points at that character's PlayerGameData.
//
// PlayerGameData's block at +0x288 is labelled "effective" by community
// structure research, but in-game logs show that it does not include every
// live correction (notably the +5 attributes from Godrick's Great Rune).
// Status-screen attributes are therefore derived from the current character's
// base values plus the addMagicStatus/addFaithStatus fields on the active
// SpecialEffect entries attached to PlayerIns. This uses the game's live effect
// instances and param rows; there is no hardcoded list of buffs or equipment.
#pragma once

#include <cstdint>

namespace omni {

// GameDataMan remains useful as a diagnostic comparison with the live-player
// path. Same signature used by The Grand Archives' cheat table.
constexpr const char* kGameDataManAob =
    "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3";
constexpr uintptr_t kGameDataManPlayerDataOffset = 0x08;

// Current in-world character chain, cross-checked against fromsoftware-rs and
// TGA's Current Session table.
constexpr uintptr_t kWorldChrManMainPlayerOffset = 0x1E508;
constexpr uintptr_t kPlayerInsPlayerDataOffset   = 0x580;
constexpr uintptr_t kPlayerInsSpecialEffectOffset = 0x178;

// PlayerGameData fields.
constexpr uintptr_t kBaseStatBlockOffset   = 0x3C;  // 8 u32: VIG..ARC
constexpr uintptr_t kStoredStatBlockOffset = 0x288; // 9 u32, incomplete for some live effects
constexpr uintptr_t kBaseIntOffset         = 0x50;
constexpr uintptr_t kBaseFaithOffset       = 0x54;
constexpr uintptr_t kStoredIntOffset       = 0x2A0;
constexpr uintptr_t kStoredFaithOffset     = 0x2A4;
constexpr uintptr_t kIsMainPlayerOffset    = 0x8F0;

// Active SpecialEffect linked list.
constexpr uintptr_t kSpecialEffectHeadOffset      = 0x08;
constexpr uintptr_t kSpEffectEntryParamDataOffset = 0x00;
constexpr uintptr_t kSpEffectEntryNextOffset      = 0x30;

struct CasterStats {
    int eff_int = 0, eff_fai = 0;       // status-screen values used for the choice
    int base_int = 0, base_fai = 0;     // level-up values
    int bonus_int = 0, bonus_fai = 0;   // sum from current active effects
    int stored_int = 0, stored_fai = 0; // PlayerGameData +0x2A0/+0x2A4, diagnostic
    uintptr_t player_ins = 0;
    uintptr_t player_game_data = 0;
};

// Resolve the globals used to find the current player. Returns false only when
// neither the live-player nor diagnostic route can be initialized.
bool player_stats_init();

// Read the current in-world character and all active attribute corrections.
// Returns false in menus/loading screens or if a pointer/read is inconsistent.
bool read_caster_stats(CasterStats& out);

// Log live base/stored/computed blocks for verification.
void dump_stat_block();

} // namespace omni
