#include "player_stats.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <detail/symbols.hpp>
#include <param/paramdef/SP_EFFECT_PARAM_ST.hpp>

#include "scan.hpp"
#include "utils.hpp"

namespace {

mem::Module g_mod;
uintptr_t g_gamedataman_var = 0; // diagnostic persistent/menu copy
uintptr_t g_worldchrman_var = 0; // address of libER's WorldChrMan global pointer

constexpr size_t kMaxActiveEffects = 1024;
constexpr int kMinAttribute = 1;
constexpr int kMaxAttribute = 99;

uintptr_t persistent_player_game_data() {
    if (!g_gamedataman_var) return 0;
    const uintptr_t gdm = mem::deref(g_gamedataman_var);
    return gdm ? mem::deref(gdm + omni::kGameDataManPlayerDataOffset) : 0;
}

uintptr_t live_player_ins() {
    if (!g_worldchrman_var) return 0;
    const uintptr_t wcm = mem::deref(g_worldchrman_var);
    return wcm ? mem::deref(wcm + omni::kWorldChrManMainPlayerOffset) : 0;
}

bool plausible_base_stat(int value) {
    return value >= kMinAttribute && value <= kMaxAttribute;
}

// Walk the active effects attached to the current PlayerIns. param_data points
// directly at the game's live SP_EFFECT_PARAM_ST row, so overhaul-mod changes
// and newly added effects are naturally included.
bool read_active_attribute_bonuses(uintptr_t player_ins, int& int_bonus,
                                   int& faith_bonus, size_t& effect_count) {
    int_bonus = 0;
    faith_bonus = 0;
    effect_count = 0;

    const uintptr_t special_effect =
        mem::deref(player_ins + omni::kPlayerInsSpecialEffectOffset);
    if (!special_effect) return false;

    uintptr_t entry = mem::deref(special_effect + omni::kSpecialEffectHeadOffset);
    while (entry) {
        if (effect_count++ >= kMaxActiveEffects) return false; // corrupt/cyclic list

        const uintptr_t param =
            mem::deref(entry + omni::kSpEffectEntryParamDataOffset);
        if (param) {
            int8_t add_int = 0;
            int8_t add_faith = 0;
            if (!mem::safe_read(
                    param + offsetof(from::paramdef::SP_EFFECT_PARAM_ST,
                                     addMagicStatus),
                    add_int) ||
                !mem::safe_read(
                    param + offsetof(from::paramdef::SP_EFFECT_PARAM_ST,
                                     addFaithStatus),
                    add_faith))
                return false;
            int_bonus += static_cast<int>(add_int);
            faith_bonus += static_cast<int>(add_faith);
        }

        const uintptr_t next =
            mem::deref(entry + omni::kSpEffectEntryNextOffset);
        if (next == entry) return false;
        entry = next;
    }
    return true;
}

int effective_attribute(int base, int bonus) {
    return std::clamp(base + bonus, kMinAttribute, kMaxAttribute);
}

} // namespace

namespace omni {

bool player_stats_init() {
    g_mod = mem::main_module();

    // libER already carries the versioned WorldChrMan singleton symbol used by
    // the rest of the project. Take the address of the global pointer itself.
    g_worldchrman_var = reinterpret_cast<uintptr_t>(
        &liber::symbol<"GLOBAL_WorldChrMan">::as<uintptr_t>());

    bool multiple = false;
    const uintptr_t hit = mem::aob_scan_unique(g_mod, kGameDataManAob, &multiple);
    if (hit && !multiple)
        g_gamedataman_var = mem::rip_relative(hit, 3, 7);
    else
        flog("[WARN] GameDataMan diagnostic AOB %s",
             multiple ? "matched multiple sites" : "not found");

    flog("player stats initialized: WorldChrMan var=%p GameDataMan var=%p",
         reinterpret_cast<void*>(g_worldchrman_var),
         reinterpret_cast<void*>(g_gamedataman_var));
    return g_worldchrman_var != 0 || g_gamedataman_var != 0;
}

bool read_caster_stats(CasterStats& out) {
    const uintptr_t player = live_player_ins();
    if (!player) return false; // title/menu/loading: never use preview data

    const uintptr_t pgd = mem::deref(player + kPlayerInsPlayerDataOffset);
    if (!pgd) return false;

    int bi = 0, bf = 0, si = 0, sf = 0;
    if (!mem::safe_read(pgd + kBaseIntOffset, bi) ||
        !mem::safe_read(pgd + kBaseFaithOffset, bf) ||
        !plausible_base_stat(bi) || !plausible_base_stat(bf))
        return false;

    int int_bonus = 0, faith_bonus = 0;
    size_t effect_count = 0;
    if (!read_active_attribute_bonuses(player, int_bonus, faith_bonus,
                                       effect_count))
        return false;

    // Diagnostic only. Failure is harmless and leaves zeroes in the dump.
    mem::safe_read(pgd + kStoredIntOffset, si);
    mem::safe_read(pgd + kStoredFaithOffset, sf);

    CasterStats next;
    next.base_int = bi;
    next.base_fai = bf;
    next.bonus_int = int_bonus;
    next.bonus_fai = faith_bonus;
    next.eff_int = effective_attribute(bi, int_bonus);
    next.eff_fai = effective_attribute(bf, faith_bonus);
    next.stored_int = si;
    next.stored_fai = sf;
    next.player_ins = player;
    next.player_game_data = pgd;
    out = next;
    return true;
}

void dump_stat_block() {
    CasterStats stats;
    if (!read_caster_stats(stats)) {
        flog("[dump] stat block: no live in-world player (menu/loading?)");
        return;
    }

    int b[8]{}, stored[9]{};
    for (int k = 0; k < 8; ++k)
        mem::safe_read(stats.player_game_data + kBaseStatBlockOffset + k * 4,
                       b[k]);
    for (int k = 0; k < 9; ++k)
        mem::safe_read(stats.player_game_data + kStoredStatBlockOffset + k * 4,
                       stored[k]);

    unsigned char is_main = 0xFF;
    mem::safe_read(stats.player_game_data + kIsMainPlayerOffset, is_main);
    const uintptr_t persistent = persistent_player_game_data();

    flog("[dump] live player: PlayerIns=%p PlayerGameData=%p persistent=%p%s",
         reinterpret_cast<void*>(stats.player_ins),
         reinterpret_cast<void*>(stats.player_game_data),
         reinterpret_cast<void*>(persistent),
         persistent == stats.player_game_data ? " (same PGD)" : "");
    flog("[dump] base stats: VIG=%d MND=%d END=%d STR=%d DEX=%d INT=%d FAI=%d ARC=%d",
         b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    flog("[dump] stored +0x288: VIG=%d MND=%d END=%d VIT=%d STR=%d DEX=%d INT=%d FAI=%d ARC=%d (diagnostic only)",
         stored[0], stored[1], stored[2], stored[3], stored[4], stored[5],
         stored[6], stored[7], stored[8]);
    flog("[dump] current caster stats: INT=%d%+d=%d FAI=%d%+d=%d (base + active effects)",
         stats.base_int, stats.bonus_int, stats.eff_int,
         stats.base_fai, stats.bonus_fai, stats.eff_fai);
    flog("[dump] is_main_player(+0x8F0)=%u", is_main);
}

} // namespace omni
