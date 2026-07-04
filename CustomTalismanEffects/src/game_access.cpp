#include "game_access.hpp"
#include "offsets.hpp"

namespace cte {

mem::Module g_mod;

uintptr_t get_player_ins() {
    const uintptr_t wcm = mem::deref(g_mod.base + kWorldChrManOffset);
    if (!wcm) return 0;
    return mem::deref(wcm + kPlayerInsOffset);
}

void enumerate_speffects(uintptr_t player, std::vector<int>& out) {
    out.clear();
    if (!player) return;
    const uintptr_t manager = mem::deref(player + kSpEffectManagerOffset);
    if (!manager) return;
    uintptr_t slot = mem::deref(manager + kSpEffectFirstSlotOffset);
    int guard = 0;
    while (slot && guard++ < 512) {
        int id = -1;
        if (mem::safe_read(slot + kSpEffectIdOffset, id) && id > 0)
            out.push_back(id);
        slot = mem::deref(slot + kSpEffectNextOffset);
    }
}

} // namespace cte
