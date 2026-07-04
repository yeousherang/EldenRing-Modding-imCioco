#pragma once

#include <unordered_set>

namespace pb {

// ---- source-category buff allowlist (buff_discovery.cpp) ----------------
// Built once at startup (after SoloParamRepository::wait_for_params) by scanning
// the game's param tables, mirroring InfiniteWeaponBuffs' discovery: an active
// SpEffect is treated as a persistable buff ONLY if it is produced by a GREASE,
// a SPELL (sorcery/incantation), or a CONSUMABLE -- ashes of war ride the
// separate g_always_persist allowlist. Everything else (talismans, weapon-innate
// passives, great runes, physick tears, environment/state effects) is dropped.
// is_persistable() (buff_filters.cpp) is the consumer of this set.
extern std::unordered_set<int> g_tracked_speffects;

// Populate g_tracked_speffects. Requires params to be loaded; call once from the
// worker after wait_for_params succeeds. If params never loaded, the set stays
// empty and only force/always-persist ids remain persistable.
void build_tracked_speffects();

} // namespace pb
