#pragma once

#include <unordered_set>

namespace iwb {

// ---- build the set of SpEffects we must never touch ---------
// Anything a horse-summon item can reach: its state effect must keep its
// original (stateful) duration or Torrent gets stuck "active".
void build_protected_set(const std::unordered_set<int>& horseGoods,
                         std::unordered_set<int>& out);

} // namespace iwb
