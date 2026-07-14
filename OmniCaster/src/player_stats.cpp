#include "player_stats.hpp"

// libER: from::param::SpEffectParam (for stat-bonus effects)
#include <param/param.hpp>

#include "scan.hpp"
#include "utils.hpp"

namespace {

mem::Module g_mod;
uintptr_t   g_gamedataman_var = 0; // address of the global pointer; 0 = unresolved

uintptr_t player_game_data() {
    if (!g_gamedataman_var) return 0;
    const uintptr_t gdm = mem::deref(g_gamedataman_var);
    if (!gdm) return 0;
    return mem::deref(gdm + omni::kPlayerGameDataOffset);
}

// Local player ChrIns via WorldChrMan. 0 when not in-game.
uintptr_t player_ins() {
    const uintptr_t wcm = mem::deref(g_mod.base + omni::kWorldChrManOffset);
    if (!wcm) return 0;
    return mem::deref(wcm + omni::kPlayerInsOffset);
}

// Base stats are 1..99 (149 leaves headroom for oddball regulations).
bool plausible_stat(int v) { return v >= 1 && v <= 148; }

// Sum addMagicStatus / addFaithStatus over the player's active SpEffects
// (talismans, physick, buffs -- every stat bonus in the game is a SpEffect).
// Returns false when the list can't be walked (bonuses then count as 0).
bool sum_stat_bonuses(int& bonus_int, int& bonus_fai) {
    bonus_int = 0;
    bonus_fai = 0;
    const uintptr_t player = player_ins();
    if (!player) return false;
    const uintptr_t manager = mem::deref(player + omni::kSpEffectManagerOffset);
    if (!manager) return false;
    uintptr_t slot = mem::deref(manager + omni::kSpEffectFirstSlotOffset);
    int guard = 0;
    while (slot && guard++ < 512) {
        int id = -1;
        if (mem::safe_read(slot + omni::kSpEffectIdOffset, id) && id > 0) {
            auto [row, ok] = from::param::SpEffectParam[id];
            if (ok) {
                bonus_int += row.addMagicStatus; // "Magic" == INT internally
                bonus_fai += row.addFaithStatus;
            }
        }
        slot = mem::deref(slot + omni::kSpEffectNextOffset);
    }
    return true;
}

} // namespace

namespace omni {

bool player_stats_init() {
    g_mod = mem::main_module();
    bool multiple = false;
    const uintptr_t hit = mem::aob_scan_unique(g_mod, kGameDataManAob, &multiple);
    if (!hit || multiple) {
        flog("[WARN] GameDataMan AOB %s -- highest-stat mode will stay on INT",
             multiple ? "matched MULTIPLE sites" : "not found");
        return false;
    }
    g_gamedataman_var = mem::rip_relative(hit, 3, 7);
    flog("GameDataMan resolved: var=%p", reinterpret_cast<void*>(g_gamedataman_var));
    return g_gamedataman_var != 0;
}

bool read_caster_stats(CasterStats& out) {
    const uintptr_t pgd = player_game_data();
    if (!pgd) return false;
    int i = 0, f = 0;
    if (!mem::safe_read(pgd + kIntOffset, i)) return false;
    if (!mem::safe_read(pgd + kFaithOffset, f)) return false;
    if (!plausible_stat(i) || !plausible_stat(f)) return false; // garbage read
    out.base_int = i;
    out.base_fai = f;
    sum_stat_bonuses(out.bonus_int, out.bonus_fai); // best-effort; 0s on failure
    return true;
}

void dump_stat_block() {
    const uintptr_t pgd = player_game_data();
    if (!pgd) {
        flog("[dump] stat block: no PlayerGameData (main menu?)");
        return;
    }
    int s[8]{};
    for (int k = 0; k < 8; ++k)
        if (!mem::safe_read(pgd + kStatBlockOffset + k * 4, s[k])) {
            flog("[dump] stat block: read fault at +0x%zX",
                 kStatBlockOffset + static_cast<size_t>(k) * 4);
            return;
        }
    flog("[dump] base stat block @PlayerGameData+0x3C: VIG=%d MND=%d END=%d "
         "STR=%d DEX=%d INT=%d FAI=%d ARC=%d (before equipment bonuses)",
         s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
}

} // namespace omni
