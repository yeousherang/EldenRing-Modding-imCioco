#pragma once

#include <param/param.hpp>

namespace iwb {

bool is_self_buff(const from::paramdef::SP_EFFECT_PARAM_ST* r);

// Engine state / animation / environment effects that must never be made
// permanent (re-using PersistentBuffs' Paramdex-sourced blocklist). These are
// matched by id only -- robust even if struct field offsets drift across game
// versions. 9621 = Roundtable "Disallow Hostile Actions" (the no-combat lock).
bool is_system_effect(int id);

// Does this SpEffect actually improve a combat/vitality stat? Used to keep only
// genuine *positive* buffs and drop (a) debuffs -- effects that only worsen
// stats -- and (b) system/state effects with no stat change at all, such as the
// Roundtable "no-combat" zone state. An effect that buffs at least one stat
// counts, even if it also has a downside (e.g. Shabriri Grape).
bool is_beneficial_buff(const from::paramdef::SP_EFFECT_PARAM_ST* r);

bool is_grease(const from::paramdef::EQUIP_PARAM_GOODS_ST& row);

} // namespace iwb
