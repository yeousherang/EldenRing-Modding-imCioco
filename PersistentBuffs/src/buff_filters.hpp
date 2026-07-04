#pragma once

#include <string>
#include <vector>

#include <param/param.hpp>

namespace pb {

// EquipParamGoods.sortGroupId categories treated as buff sources by
// buff_discovery (see the full category table there). 20 = buff/heal
// consumables, 70 = greases.
constexpr int kSortGroupGrease     = 70;
constexpr int kSortGroupConsumable = 20;
// Guard against pathological / cyclic SpEffect replace/cycle chains during
// consumable discovery.
constexpr int kChainMaxDepth       = 8;

// SpEffectParam row by id (binary search), or nullptr if it doesn't exist /
// params not loaded yet. Mirrors InfiniteWeaponBuffs' sp_row.
from::paramdef::SP_EFFECT_PARAM_ST* sp_row(int id);

// True if the effect targets the caster (self / player). Used by buff_discovery
// to keep only self-buffs when scanning consumables. Ported from IWB.
bool is_self_buff(const from::paramdef::SP_EFFECT_PARAM_ST* r);

// True if the goods row is a weapon/shield grease (isEnhance / isShieldEnchant,
// or sortGroupId == kSortGroupGrease). Used by buff_discovery. Ported from IWB.
bool is_grease(const from::paramdef::EQUIP_PARAM_GOODS_ST& row);

// Engine state / animation / environment effects that must never be persisted --
// the engine applies & clears them itself, so re-applying sticks the player in
// that state. Matched by id only (robust even if SpEffectParam field offsets
// drift across game versions). See CLAUDE.md "Buff filtering" for the full list
// of confirmed ids and why each one is here.
bool is_system_effect(int id);

// Does this SpEffect actually improve a combat/vitality stat? This is the robust
// auto-filter: engine state / evergaol / no-combat / animation effects change no
// stat, so they're rejected here WITHOUT needing their ids. Copied verbatim from
// InfiniteWeaponBuffs' is_beneficial_buff (proven on this game build).
bool is_beneficial_buff(const from::paramdef::SP_EFFECT_PARAM_ST* r);

// Decide whether an active SpEffect should be snapshotted & re-applied after a
// wipe. Precedence:
//   1. force/always      -> keep  (trusted allowlist; ashes live in g_always_persist)
//   2. is_system_effect  -> drop  (known engine-state ids; fast, drift-proof)
//   3. source allowlist  -> keep iff in g_tracked_speffects (a grease/spell/
//                           consumable buff discovered at startup; see buff_discovery)
// Anything else (talismans, weapon-innate passives, great runes, physick,
// environment/state effects) -> dropped.
bool is_persistable(int id);

// Diagnostic: log which effect ids appeared/disappeared since last call (raw,
// unfiltered) so system effects can be discovered from the log. No-op unless
// g_log_effects is on.
void log_effect_changes(const std::vector<int>& cur_vec);

// Diagnostic (behind g_log_effects): log which active effects the filter DROPS
// and why, de-duped so it only fires when the dropped set changes. Use it to
// spot a wanted buff being filtered out (-> add it to force_persist_ids) or to
// confirm a system effect is being rejected.
void log_filter_changes(const std::vector<int>& cur_vec);

} // namespace pb
