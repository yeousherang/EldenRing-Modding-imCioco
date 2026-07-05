// ============================================================
//  In-process reader for the game's live message repository
//  (CS::MsgRepositoryImp). Lets the mod pull the CURRENT talisman name +
//  effect text straight out of whatever regulation/msgbnd is loaded -- so
//  mod-added talismans (Reforged, The Convergence, ...) and RENAMED vanilla
//  talismans show correctly, in the player's language, with no baked per-mod
//  data. Names/effects are read from the AccessoryName / AccessoryInfo /
//  AccessoryCaption FMGs (base + DLC layers), matching the game's own merge
//  priority.
//
//  The raw MsgRepositoryImp navigation + FMG group-table walk is ported from
//  the sibling ERR-MapForGoblins-DLL (src/goblin_messages.cpp), trimmed to a
//  read-only lookup. Single-threaded use only (called from the worker while
//  building the talisman model).
// ============================================================
#pragma once

#include <string>
#include <vector>

namespace cte::messages {

// Locate MsgRepositoryImp and discover the accessory text slots. Idempotent;
// does the work once. Returns true if talisman NAMES are readable (the minimum
// for the live-name feature). Safe to call before params/msg are ready -- it
// waits (bounded) for the repository pointer to populate.
//
// `accessory_ids` = every row id of the live EquipParamAccessory. Used to
// fingerprint the AccessoryInfo/Caption slots: the real description banks
// resolve strings for (nearly) exactly this id set and almost nothing else.
bool init(const std::vector<int>& accessory_ids);

// Live talisman display name for an EquipParamAccessory id, in the player's
// language. "" if the repository/slot is unavailable or the id has no entry.
std::string accessory_name(int id);

// Live talisman effect/description text (AccessoryInfo, falling back to
// AccessoryCaption). "" if unavailable. Used only as a fallback when the
// baked curated effect text is empty (e.g. mod-added talismans).
std::string accessory_effect(int id);

} // namespace cte::messages
