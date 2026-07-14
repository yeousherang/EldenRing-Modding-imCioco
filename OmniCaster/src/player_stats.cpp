#include "player_stats.hpp"

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

// Stats are 1..99 in practice; 148 leaves headroom for buffed oddballs.
bool plausible_stat(int v) { return v >= 1 && v <= 148; }

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
    int ei = 0, ef = 0, bi = 0, bf = 0;
    if (!mem::safe_read(pgd + kEffIntOffset, ei)) return false;
    if (!mem::safe_read(pgd + kEffFaithOffset, ef)) return false;
    if (!plausible_stat(ei) || !plausible_stat(ef)) return false; // garbage read
    mem::safe_read(pgd + kBaseIntOffset, bi);   // log-only, best effort
    mem::safe_read(pgd + kBaseFaithOffset, bf);
    out.eff_int  = ei;
    out.eff_fai  = ef;
    out.base_int = bi;
    out.base_fai = bf;
    return true;
}

void dump_stat_block() {
    const uintptr_t pgd = player_game_data();
    if (!pgd) {
        flog("[dump] stat block: no PlayerGameData (main menu?)");
        return;
    }
    int b[8]{}, e[9]{};
    for (int k = 0; k < 8; ++k)
        mem::safe_read(pgd + kBaseStatBlockOffset + k * 4, b[k]);
    for (int k = 0; k < 9; ++k)
        mem::safe_read(pgd + kEffStatBlockOffset + k * 4, e[k]);
    unsigned char is_main = 0xFF;
    mem::safe_read(pgd + kIsMainPlayerOffset, is_main);
    flog("[dump] base stats  (+0x3C):  VIG=%d MND=%d END=%d STR=%d DEX=%d INT=%d FAI=%d ARC=%d",
         b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    flog("[dump] effective   (+0x288): VIG=%d MND=%d END=%d VIT=%d STR=%d DEX=%d INT=%d FAI=%d ARC=%d"
         "  <- should match the status screen",
         e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8]);
    flog("[dump] is_main_player(+0x8F0)=%u (expect 1)", is_main);
}

} // namespace omni
