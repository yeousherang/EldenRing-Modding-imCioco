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

uintptr_t get_player_game_data() {
    if (!g_gamedataman_var) return 0;
    const uintptr_t gdm = mem::deref(g_gamedataman_var);
    if (!gdm) return 0;
    return mem::deref(gdm + kPlayerGameDataOffset);
}

bool enumerate_inventory_accessories(std::vector<int>& out) {
    out.clear();
    const uintptr_t pgd = get_player_game_data();
    if (!pgd) return false;
    const uintptr_t bag = mem::deref(pgd + kEquipInventoryDataOffset);
    if (!bag) return false;

    // NormalInventory is an inline InventoryItemList, not a pointer.
    const uintptr_t list = bag + kNormalInventoryOffset;
    uint32_t cap = 0, entries = 0;
    if (!mem::safe_read(list + kInvListCapOffset, cap)) return false;
    if (!mem::safe_read(list + kInvListEntriesOffset, entries)) return false;
    const uintptr_t arr = mem::deref(list + kInvListPointerOffset);
    if (!arr || cap == 0 || cap > static_cast<uint32_t>(kInvMaxEntriesGuard))
        return false;
    if (entries > cap) entries = cap;

    // The array is sparse: walk slots, skip empties, stop once every live entry
    // has been seen (or the guard trips).
    uint32_t seen = 0;
    for (uint32_t i = 0; i < cap && seen < entries; ++i) {
        int32_t raw = 0;
        if (!mem::safe_read(arr + i * kInvEntryStride + kInvEntryItemIdOffset, raw))
            continue;
        if (raw == 0 || raw == -1) continue; // empty slot -- not a live entry
        ++seen;
        const uint32_t u = static_cast<uint32_t>(raw);
        if ((u & kItemCategoryMask) != kCategoryAccessory) continue;
        out.push_back(static_cast<int>(u & kItemParamIdMask));
    }
    return true;
}

} // namespace cte
