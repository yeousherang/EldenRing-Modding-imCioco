#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "config.hpp" // HandPair

namespace iwb {

// ---- dual-wield off-hand mirroring --------------------------
// Weapon buffs come as a Right (wepParamChange==1) + Left (==2) SpEffect; the
// Left enchants the off-hand weapon. Setting Right.cycleOccurrenceSpEffectId =
// Left makes the engine re-apply the Left every cycle while the main-hand buff is
// active, so a single buff lands on BOTH weapons when dual-wielding. The Left
// SpEffect gets a short, continuously-refreshed "bridge" effectEndurance (not
// the main hand's full duration) so it tracks the main hand's expiry closely
// instead of outliving it once the main-hand buff ends -- see the
// kDualWieldCycleInterval/kDualWieldBridgeDuration constants in apply.cpp.

// The weapon-enchant id behind a SpEffect (via its vfx -> SpEffectVfxParam), or
// -1 if it carries no weapon enchant. Right and Left share this per element, so
// it pairs them (e.g. 1=magic, 3=lightning, 5=poison, 10=holy, 17=black flame).
int sp_enchant_id(int spId);

float sp_endurance(int spId);

// Build the right->left mirror map. Greases are discovered dynamically (the same
// is_grease() set the duration pass uses); weapon-art enchants come from the
// explicit pair list (built-in + .ini extra_pairs). Pairs touching a
// protected/system effect are skipped. With logDetail, each decision is logged
// (dump mode) and the resulting map can be discarded.
void build_dualwield_mirror(const std::vector<HandPair>& artPairs,
                            const std::unordered_set<int>& protectedSp,
                            std::unordered_map<int, int>& mirror,
                            bool logDetail = false);

} // namespace iwb
