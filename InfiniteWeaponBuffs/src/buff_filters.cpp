#include "buff_filters.hpp"
#include "config.hpp"

namespace iwb {

bool is_self_buff(const from::paramdef::SP_EFFECT_PARAM_ST* r) {
    return r && (r->effectTargetSelf || r->effectTargetPlayer);
}

bool is_system_effect(int id) {
    if (id >= 100000 && id <= 100999) return true; // [HKS] state block + grace
    if (id >= 131    && id <= 147)    return true; // jump / attack anim states
    if (id >= 170    && id <= 176)    return true; // guard anim states
    switch (id) {
        case 45:      // [HKS] Counter Frames
        case 8001:    // [HKS] Is Stealth
        case 10665:   // [HKS] Event action not possible
        case 530007:  // [HKS] Goods stamina cost
        case 530012:  // [HKS] Goods stamina cost
        case 9621:    // Disallow Hostile Actions (Roundtable Hold no-combat)
        case 4600:    // Wet (Rain) -- environment
            return true;
        default:
            return false;
    }
}

bool is_beneficial_buff(const from::paramdef::SP_EFFECT_PARAM_ST* r) {
    if (!r) return false;
    // Attack up (rates >1, flat >0).
    if (r->physicsAttackPowerRate > 1.f || r->magicAttackPowerRate > 1.f ||
        r->fireAttackPowerRate > 1.f    || r->thunderAttackPowerRate > 1.f)
        return true;
    if (r->physicsAttackPower > 0 || r->magicAttackPower > 0 ||
        r->fireAttackPower > 0    || r->thunderAttackPower > 0)
        return true;
    if (r->physicsAttackRate > 1.f || r->magicAttackRate > 1.f ||
        r->fireAttackRate > 1.f    || r->thunderAttackRate > 1.f ||
        r->staminaAttackRate > 1.f)
        return true;
    // Defense up (rates >1, flat >0) / damage taken down (cut rates <1).
    if (r->physicsDiffenceRate > 1.f || r->magicDiffenceRate > 1.f ||
        r->fireDiffenceRate > 1.f    || r->thunderDiffenceRate > 1.f)
        return true;
    if (r->physicsDiffence > 0 || r->magicDiffence > 0 ||
        r->fireDiffence > 0    || r->thunderDiffence > 0)
        return true;
    if (r->slashDamageCutRate < 1.f  || r->blowDamageCutRate < 1.f   ||
        r->thrustDamageCutRate < 1.f || r->neutralDamageCutRate < 1.f ||
        r->magicDamageCutRate < 1.f  || r->fireDamageCutRate < 1.f   ||
        r->thunderDamageCutRate < 1.f)
        return true;
    // Vitality / regen.
    if (r->maxHpRate > 1.f || r->maxMpRate > 1.f || r->maxStaminaRate > 1.f)
        return true;
    if (r->hpRecoverRate > 0.f || r->mpRecoverChangeSpeed > 0 ||
        r->staminaRecoverChangeSpeed > 0)
        return true;
    // Status resistance up.
    if (r->registPoizonChangeRate > 1.f || r->registDiseaseChangeRate > 1.f ||
        r->registBloodChangeRate > 1.f  || r->registCurseChangeRate > 1.f)
        return true;
    // Rune acquisition up (Gold-Pickled Fowl Foot etc.).
    if (r->haveSoulRate > 1.f || r->soulRate > 0.f)
        return true;
    return false;
}

bool is_grease(const from::paramdef::EQUIP_PARAM_GOODS_ST& row) {
    return row.isEnhance || row.isShieldEnchant ||
           static_cast<int>(row.sortGroupId) == kSortGroupGrease;
}

} // namespace iwb
