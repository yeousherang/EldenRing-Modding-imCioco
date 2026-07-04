#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "config.hpp" // Ini, HandPair

namespace iwb {

// Add the timed buffs reachable from `entries` to `target` at `dur`.
//   followChain      : follow the SpEffect replace/cycle chain (consumables) vs.
//                      only the entry effect itself (greases / spells).
//   requireSelf      : keep only self/player-targeted effects (consumables) so
//                      enemy debuffs aren't made permanent.
//   requireBuff      : keep only effects that actually improve a stat -- drops
//                      debuffs and system/state effects (e.g. the Roundtable
//                      no-combat zone). `skippedNonBuff` (optional) counts these.
// Protected effects are never added; first writer wins (target.emplace).
int add_buffs(const std::vector<int>& entries, float dur,
              bool followChain, bool requireSelf, bool requireBuff,
              const std::unordered_set<int>& protectedSp,
              std::unordered_map<int, float>& target,
              int* skippedNonBuff = nullptr);

// ---- the param passes ---------------------------------------
void apply(const Ini& ini, const std::unordered_set<int>& extraGoods,
           const std::unordered_set<int>& horseGoods,
           const std::unordered_set<int>& ashIds,
           const std::vector<HandPair>& artPairs);

} // namespace iwb
