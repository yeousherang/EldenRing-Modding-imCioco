#pragma once

#include <cstdint>

namespace cte {

// ============================================================
//  VERSION-SPECIFIC OFFSETS / SIGNATURES -- VERIFY before trusting!
//  Trimmed copy of PersistentBuffs/src/offsets.hpp: we only need to reach the
//  local player, walk its active SpEffect list, and call the game's
//  "apply SpEffect" function. All values are seeded from libER's symbol table +
//  The Grand Archives CT; they WILL drift across game patches. Confirm in-game
//  via the enumeration log (debug_console = 1) before trusting them.
// ============================================================

// WorldChrMan singleton pointer = module_base + this (libER GLOBAL_WorldChrMan).
constexpr uintptr_t kWorldChrManOffset = 0x3D65F88;
// WorldChrMan + this -> local PlayerIns (ChrIns*). Confirmed (TGA CT getPlayerIns).
constexpr uintptr_t kPlayerInsOffset   = 0x1E508;

// ChrIns + this -> SpecialEffect manager (the SpEffect list owner).
constexpr uintptr_t kSpEffectManagerOffset = 0x178;
// SpEffect manager + this -> first list slot (pointer). NOTE: the active-effect
// list lives one indirection BELOW the manager -- the manager holds a pointer to
// the head slot, it is not the head itself. (Confirmed: TGA CT SpEffect.eraseAll.)
constexpr uintptr_t kSpEffectFirstSlotOffset = 0x8;
// Within a SpEffect list slot:
constexpr uintptr_t kSpEffectIdOffset   = 0x8;   // int  effect id
constexpr uintptr_t kSpEffectNextOffset = 0x30;  // ptr  next slot

// "Apply SpEffect" function: void ApplySpEffect(ChrIns* chr, int id, char unk).
// Resolved by AOB scan; the scanned instruction sits INSIDE the function, so the
// real entry point is `match - kApplySpEffectFuncBackset`. Called with unk=1
// (== "self"), matching TGA CT's SpEffect.addForSelf.
using ApplySpEffect_t = void(*)(void* chr_ins, int sp_effect_id, char unk);
extern ApplySpEffect_t g_apply;
// Source: The Grand Archives CT, Global Functions/SpEffect_code.cea (add_call).
constexpr const char* kApplySpEffectAob =
    "0f 28 0d ?? ?? ?? ?? ?? 8d ?? ?? 0f 29 ?? ?? ?? 0f b6 d8";
constexpr uintptr_t kApplySpEffectFuncBackset = 0x1D;

// "Remove SpEffect" function: void RemoveSpEffect(SpecialEffect* mgr, int id).
// NOTE: unlike apply, this takes the SpEffect MANAGER (chr + kSpEffectManagerOffset),
// NOT the ChrIns. The AOB lands on the function prologue (sub rsp,28), so there is
// NO backset. Called RCX=manager, RDX=id, matching TGA CT's SpEffect.remove
// (executeCodeEx(..., remove_call, SpecialEffect, id)).
using RemoveSpEffect_t = void(*)(void* special_effect_mgr, int sp_effect_id);
extern RemoveSpEffect_t g_remove;
// Source: The Grand Archives CT, Global Functions/SpEffect_code.cea (remove_call).
constexpr const char* kRemoveSpEffectAob =
    "48 83 EC 28 8B C2 48 8B 51 08 48 85 D2 ?? ?? 90";

} // namespace cte
