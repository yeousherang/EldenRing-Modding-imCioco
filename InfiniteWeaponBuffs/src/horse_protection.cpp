#include "horse_protection.hpp"
#include "speffect_lookup.hpp"

#include <vector>

#include <param/param.hpp>

namespace iwb {

void build_protected_set(const std::unordered_set<int>& horseGoods,
                         std::unordered_set<int>& out) {
    for (auto [id, row] : from::param::EquipParamGoods) {
        if (!row.isSummonHorse && !horseGoods.count(static_cast<int>(id)))
            continue;
        std::vector<int> entries;
        gather_goods_entry_speffects(row, entries);
        for (int e : entries) collect_all_chain(e, out);
    }
}

} // namespace iwb
