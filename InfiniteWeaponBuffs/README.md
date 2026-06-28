# ER Infinite Weapon Buffs

A native C++ DLL mod for Elden Ring that:

1. **Makes every weapon buffable** — any grease or spell buff can be applied to any weapon.
2. **Sets buff durations** — greases, spell buffs and consumable buffs last as long as you configure (including permanent), per category, via `InfiniteWeaponBuffs.ini`.

It does this entirely by **rewriting param tables in memory at runtime** (via [libER](https://github.com/Dasaav-dsv/libER)). **No `regulation.bin` edit is involved.**

> ⚠️ **Offline only.** Elden Ring runs EasyAntiCheat. Any memory-editing mod must run with EAC disabled (the mod loaders below handle that). Using this online risks a soft-ban. Single-player or Seamless Co-op only.

---

## How it works

| Feature | What it changes in memory |
|---|---|
| All weapons buffable | `EquipParamWeapon.isEnhance = true` on every row |
| Buff durations | `SpEffectParam.effectEndurance` on the IDs you list (`-1` = permanent, else seconds) |

Edits are applied once, on a worker thread, right after `SoloParamRepository::wait_for_params()` reports the param tables are loaded. They persist for the rest of the session.

### What "infinite" really means
With infinite duration, buffs **persist through resting at a grace and fast travel**. The engine still clears active SpEffects on **death** regardless of duration, so in practice `infinite` lasts until you die — then re-apply.

---

## Build

**Prerequisites:** Visual Studio 2022 (Desktop C++ workload), CMake ≥ 3.15, Git.

```sh
# 1. Get the code + libER
git init
git submodule add https://github.com/Dasaav-dsv/libER external/libER
git submodule update --init --recursive

# 2. Configure + build (Release)
cmake -B build -A x64
cmake --build build --config Release
```

Output: `build/Release/InfiniteWeaponBuffs.dll`

---

## Install

Works with any native-DLL loader — **ModEngine3 (me3)**, ModEngine2, or Elden Mod Loader. Run offline.

- **me3:** add `InfiniteWeaponBuffs.dll` as a native mod in your profile (the same folder as your other native mods, e.g. `...\me3\config\profiles\<profile>\`). Put `InfiniteWeaponBuffs.ini` right next to the DLL.
- **ModEngine2:** add the DLL to `external_dlls` in the TOML, or drop it in a folder Elden Mod Loader scans.
- **Elden Mod Loader:** drop the DLL + `.ini` in `mods/`.

The `.ini` **must** sit next to the DLL and share its base name (`InfiniteWeaponBuffs.dll` → `InfiniteWeaponBuffs.ini`).

---

## Configure

Edit `InfiniteWeaponBuffs.ini` (kept next to the DLL, sharing its base name). There are **no ID lists to maintain** — you only set a duration per category:

```ini
[greases]
duration = infinite      ; seconds, or "infinite" (= permanent)
[spell_buffs]
duration = infinite
[consumables]
duration = 300
```

### How categories are derived

The mod finds the SpEffects for each category automatically from the params, so it stays correct across game patches and alongside other mods:

- **Greases** = SpEffects referenced by `EquipParamGoods` rows of `goodsType 10`.
- **Consumables** = SpEffects referenced by `EquipParamGoods` of `goodsType 0` and `3` (normal consumables + physick tears).
- **Spell buffs** = SpEffects referenced by the `Magic` param.

A refId only counts if it's a real `SpEffectParam` row, and with `only_timed_effects = 1` (in `[advanced]`) only effects already on a finite timer are changed — so instant heals, passives and already-permanent buffs are left alone.

> **Note on debuffs:** the effects come from `Magic.refId` / `Goods.refId`, which are the *self-applied* effects of the spell/item; enemy debuffs are delivered via Bullets, which this mod never touches. SpEffect target flags (`effectTargetSelf/Enemy/...`) are permissive capability flags, *not* a buff-vs-debuff label, so they can't be used to filter debuffs out.

> **Scope note:** "spell buffs" and "consumables" are broad — they cover *all* timed buff effects those sources apply, not only weapon-enchant spells / stat-buff foods.

### Experimental: stacking

One optional extra (**default off**). Only touches the buffs this mod already manages, and is a plain param edit — so **test in-game**.

```ini
[stacking]
no_overwrite = 0     ; let buffs coexist instead of replacing each other
```

- **`no_overwrite`** zeroes `SpEffectParam.spCategory`, removing the mutual-exclusion that makes some buffs replace one another — so you can stack buffs that normally can't coexist.

### Diagnostic dump

Set `dump = 1` under `[discover]` to log every goods/magic → SpEffect reference (and change nothing) — handy if a patch shifts the `goodsType` numbers. Set `debug_console = 1` to also mirror the log to a console window.

---

## Logs / troubleshooting

Every launch writes **`logs/InfiniteWeaponBuffs.log`** (a `logs/` subfolder next to the DLL, created automatically; always on, no config needed). It's the fastest way to see what happened:

- **No `.log` file at all** → the DLL never loaded. Check the loader actually lists it, and that it's not failing with a missing dependency (error 126).
- Log stops after `waiting for params...` → `wait_for_params` never returned. Usually a **libER symbol/version mismatch** (see below).
- `isEnhance set on N weapon rows` and `patched N ... SpEffectParam rows` → it worked; check in-game.

### ⚠️ libER symbol/version caveat
libER resolves game functions from symbol tables baked in at compile time; libER's repo was last updated **2025-04-24**. If your game build (e.g. **1.16.2.0**) is newer than libER's symbols, the param calls can resolve to the wrong addresses — expect a hang at `waiting for params...`, a crash, or `0 rows` patched. If that happens, you need libER symbols updated for your game version (or pin the game to a version libER supports).

---

## Notes / roadmap

- Targets a specific game build; FromSoft patches can shift internal signatures. libER abstracts most of that, but pin a tested game version.
- Possible future enhancement: auto-discover category IDs by scanning `EquipParamGoods` / `Magic` for buff references, instead of hand-listing them.
