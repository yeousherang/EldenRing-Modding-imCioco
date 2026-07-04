#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "ini.hpp"

namespace iwb {

extern HINSTANCE g_hinst;
extern bool      g_debug;

// ---- known vanilla item ids --------------------------------
// Used in addition to the live param flags, so categorization stays
// correct even if a regulation doesn't set the expected flag.
//   130     = Spectral Steed Whistle (Torrent) -- always protected.
//   2003170 = DLC "Golden Vow" thrown pot -- buffs the thrower via a
//             bullet, so it isn't a plain sort-group-20 consumable.
inline const int kHorseSummonGoodsBuiltin[]      = { 130 };
inline const int kExtraConsumableGoodsBuiltin[]  = { 2003170 };

// Built-in SpEffect ids for Ash-of-War self-buffs. Ashes apply their buff
// through a weapon-skill behavior that can't be reached cleanly from the gem
// param (SwordArtsParam has no SpEffect ref), so they're driven by this
// allowlist; extend with [ashes_of_war] speffect_ids. Ids are the "Damage Buff"
// effect rows (incl. No-FP variants) from soulsmods/Paramdex ER SpEffectParam
// names -- the same set PersistentBuffs uses. Element weapon-enchant arts
// (Cragblade, Flaming Strike, infusions) are intentionally excluded: like
// greases they belong to the weapon, not the character. Add them via the .ini
// if you want them too.
inline const std::vector<int> kAshOfWarBuffSpEffectsBuiltin = {
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

// ---- dual-wield off-hand mirror: weapon-art enchant pairs ----
// A weapon buff is two SpEffect rows: Right (wepParamChange==1) enchants the
// right weapon, Left (==2) the left. Greases are paired dynamically from the
// live params (see build_dualwield_mirror); weapon-skill (Ash of War) enchants
// aren't goods, so their Right->Left pairs are listed here (ids from the
// soulsmods/Paramdex-based reference set). Extend via [dual_wield] extra_pairs.
// NB: these are hand-targeted *weapon enchant* arts -- a different set from
// kAshOfWarBuffSpEffectsBuiltin above, which are character self-buffs (Roar,
// Determination, ...) that don't target a hand and need no mirroring.
struct HandPair { int right; int left; };
inline const HandPair kDualWieldArtPairsBuiltin[] = {
    { 821, 823 },              // Sacred Blade
    { 826, 828 },              // Chilling Mist
    { 831, 833 },              // Poison Mist
    { 1676, 1678 },            // Lightning Slash
    { 1721, 1723 },            // Moonlight Greatsword
    { 1755, 1758 },            // Seppuku
    { 1776, 1778 },            // Flaming Strike
    { 1806, 1808 },            // Ruinous Ghostflame
    { 1821, 1823 },            // Cragblade
    { 1891, 1893 },            // Ice Lightning Sword
    { 20000891, 20000896 },    // Flame Skewer
    { 20000961, 20000966 },    // Flame Spear
};

// Sort groups (EquipParamGoods.sortGroupId) we treat as categories.
constexpr int kSortGroupGrease     = 70; // greases (vanilla + DLC)
constexpr int kSortGroupConsumable = 20; // buff/heal foods, livers, boluses...

// Guard against pathological / cyclic SpEffect chains.
constexpr int kChainMaxDepth = 8;

// ---- parse "130, 2003170" into a set of ints ----------------
void parse_int_list(const std::string& spec, std::unordered_set<int>& out);
// ---- parse "821:823, 1821:1823" into right:left pairs -------
void parse_pair_list(const std::string& spec, std::vector<HandPair>& out);

// Everything derived from config.ini + the built-in lists above, ready to
// hand to apply() / dump_candidates().
struct Config {
    Ini ini;
    std::unordered_set<int> extraGoods, horseGoods, ashIds;
    std::vector<HandPair> artPairs;
};

// Loads config.ini (or falls back to built-in defaults if it's missing),
// sets up the debug console, and resolves the built-in + .ini-configured id
// lists. Call once from the worker thread (blocking file I/O).
Config load_config();

} // namespace iwb
