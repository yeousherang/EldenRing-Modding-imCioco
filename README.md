# EldenRing-Modding

My own collection of mods for EldenRing. Made for my own playthroughs and shared with the community (see Nexus).
I am always open to improvements and/or new mod ideas.

Each mod edits param tables in memory at runtime — no `regulation.bin` edits.

> ⚠️ **Offline only.** These memory-editing mods must run with EasyAntiCheat
> disabled (the usual mod loaders handle that). Using them online risks a ban.

## Mods

| Mod | What it does |
|---|---|
| [AdjustableSpellCost](AdjustableSpellCost/) | Adjusts spells FP cost and the stat (INT/FAITH) requirements. |
| [AdjustableSummonCost](AdjustableSummonCost/) | Adjusts summon (Spirit Ash) costs. |
| [CustomTalismanEffects](CustomTalismanEffects/) | In-game menu that allows passive use of every talisman effect from the game. |
| [InfiniteWeaponBuffs](InfiniteWeaponBuffs/) | Makes every weapon buffable and sets buff durations (greases / spell buffs / consumables), incl. permanent. Optional buff stacking. |
| [PersistentBuffs](PersistentBuffs/) | Keeps active buffs through **fast travel** and **death** (engine wipes them otherwise) by re-applying them at runtime. Optional per-weapon buff memory across weapon swaps / dual-wielding. |
| [OmniCaster](OmniCaster/) | Let's you cast spells with any catalyst (seal/staff) and also allows scaling both sorceries & incants by the same weapon or by the highest stat. |

See each mod's own `README.md` for configuration and install details.

## Building

Prerequisites: Visual Studio 2022 (Desktop C++), CMake ≥ 3.15, Git.

```sh
# clone with submodules
git clone --recursive https://github.com/imCioco/EldenRing-Modding.git
# or, if already cloned:
git submodule update --init --recursive
```

Then build a mod via its `build.bat` (recommended), or with CMake:

```sh
cmake -S InfiniteWeaponBuffs -B InfiniteWeaponBuffs/build -A x64
cmake --build InfiniteWeaponBuffs/build --config Release
```

Each mod links suhmodules statically into a single self-contained DLL.
