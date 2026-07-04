#pragma once

#include <unordered_set>
#include <vector>

#include <param/param.hpp>

namespace iwb {

// ---- param row accessors (binary search by id) --------------
// Each returns a pointer to the live row, or nullptr if it doesn't exist.
from::paramdef::SP_EFFECT_PARAM_ST* sp_row(int id);
from::paramdef::BULLET_PARAM_ST* bullet_row(int id);
// Player behaviors live in BehaviorParam_PC; fall back to BehaviorParam.
from::paramdef::BEHAVIOR_PARAM_ST* behavior_row(int id);

// All self/ally SpEffects a bullet can apply: the shooter effect (a thrown
// self-buff pot like Golden Vow) plus the on-hit effects (the buff lands on
// whoever stands in the AoE, including the thrower). Enemy-only hit effects are
// dropped later by the self/player target filter.
void add_bullet_speffects(int bulletId, std::vector<int>& out);

// ---- SpEffect discovery from a goods row --------------------
// "Entry" SpEffects are the ones an item points at directly, before any
// replace/cycle chaining. A goods refId means different things per refCategory
// ([0]=attack [1]=projectile/bullet [2]=special/SpEffect): we always try it as
// a SpEffect (foods etc.), and for projectile items also follow it as a Bullet
// (thrown buff pots). Items that act through a behavior add the behavior's
// SpEffect (refType 2) or bullet effects (refType 1) too.
void gather_goods_entry_speffects(const from::paramdef::EQUIP_PARAM_GOODS_ST& row,
                                  std::vector<int>& out);

// Collect EVERY SpEffect reachable from `startId` through the replace/cycle
// chain (used to fence off horse-summon state effects). Bounded + de-duped.
void collect_all_chain(int startId, std::unordered_set<int>& out, int depth = 0);

// Collect the SpEffect ids in `startId`'s chain that are currently on a finite
// timer (effectEndurance > 0) -- i.e. the actual buffs we can extend.
void collect_timed_chain(int startId, std::vector<int>& out,
                         std::unordered_set<int>& visited, int depth = 0);

// SpEffects referenced by the Magic param (spell buffs).
void gather_magic_entries(const from::paramdef::MAGIC_PARAM_ST& row,
                          std::vector<int>& out);

} // namespace iwb
