// Weapon names for readable logs. The Paramdex ER/Names/EquipParamWeapon.txt
// list is embedded into the DLL at build time (see cmake/embed_names.cmake).
#pragma once

#include <string>

namespace omni {

// Parse the embedded name blob. Call once at startup (before wep_label).
void names_init();

// "Erdtree Seal (17070000)" -- or "Erdtree Seal +5 (17070005)" for upgrade
// rows, or "#<id>" when the id is unknown (modded regulation).
std::string wep_label(int id);

} // namespace omni
