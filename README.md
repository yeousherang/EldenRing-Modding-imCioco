# EldenRing_Mods

A collection of native C++ DLL mods for Elden Ring, built on
[libER](https://github.com/Dasaav-dsv/libER). Each mod edits param tables in
memory at runtime — no `regulation.bin` edits.

> ⚠️ **Offline only.** These memory-editing mods must run with EasyAntiCheat
> disabled (the usual mod loaders handle that). Using them online risks a ban.

## Mods

| Mod | What it does |
|---|---|
| [InfiniteWeaponBuffs](InfiniteWeaponBuffs/) | Makes every weapon buffable and sets buff durations (greases / spell buffs / consumables), incl. permanent. Optional buff stacking. |
| [AdjustableSummonCost](AdjustableSummonCost/) | Adjusts summon (Spirit Ash) costs. |

See each mod's own `README.md` for configuration and install details.

## Building

Prerequisites: Visual Studio 2022 (Desktop C++), CMake ≥ 3.15, Git.

```sh
# clone with submodules (libER)
git clone --recursive https://github.com/<you>/EldenRing_Mods.git
# or, if already cloned:
git submodule update --init --recursive
```

Then build a mod via its `build.bat`, or with CMake:

```sh
cmake -S InfiniteWeaponBuffs -B InfiniteWeaponBuffs/build -A x64
cmake --build InfiniteWeaponBuffs/build --config Release
```

Each mod links libER statically into a single self-contained DLL.
