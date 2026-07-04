#pragma once

#include <unordered_set>
#include <vector>

#include "config.hpp" // HandPair

namespace iwb {

// ---- discovery: dump how each category resolves, so coverage can be
//      verified against the live regulation. Nothing is patched here.
void dump_candidates(const std::unordered_set<int>& extraGoods,
                     const std::unordered_set<int>& horseGoods,
                     const std::unordered_set<int>& ashIds,
                     const std::vector<HandPair>& artPairs);

} // namespace iwb
