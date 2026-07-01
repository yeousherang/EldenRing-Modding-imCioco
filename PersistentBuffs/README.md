# PersistentBuffs

A **runtime** native DLL mod for Elden Ring that keeps your active buffs through
**fast travel** and **death** — the two transitions where the engine wipes all
active SpEffects regardless of their duration.

> ✅ **Working:** buffs persist through **fast travel** and **death**
> (confirmed in-game). System/state effects (Roundtable Hold's no-combat block,
> evergaols, …) are **filtered out** so they can't follow you out and lock you
> out of attacks or Torrent — only genuine stat buffs are re-applied. An
> experimental, opt-in `[weapon_memory]` feature also keeps **weapon buffs
> (greases / blade spells) through weapon swaps and dual-wielding**. Roadmap:
> independent fast-travel/death toggles — see [CLAUDE.md](CLAUDE.md).
> Offsets/signatures are game-version specific; verify via the log if your build
> differs.

> ⚠️ **Offline only.** Memory-editing/hooking mod — run with EasyAntiCheat
> disabled (ModEngine3/2, Elden Mod Loader). Online use risks a ban.

## Why a runtime mod (and not a param edit)?

The companion mod *InfiniteWeaponBuffs* makes buff durations infinite by editing
`SpEffectParam`. But fast travel / death clearing buffs is **hardcoded engine
behavior** — no param controls it (confirmed by research; even the popular
"forever buffs" mods can't do it via params). The only way is at runtime:
remember the player's active buffs and **re-apply** them right after the engine
clears them on a transition.

## Configure

`PersistentBuffs.ini` (next to the DLL, same base name):

```ini
[persistence]
keep_after_fast_travel = 1
keep_after_death       = 1

; Only genuine stat buffs are persisted; system/state effects are auto-filtered.
; force_persist_ids = ids to ALWAYS persist (rescue a real buff the filter drops
; by mistake). Comma-separated; find the id in the log with log_effects = 1.
force_persist_ids =

[weapon_memory]
; EXPERIMENTAL. Remembers greases/blade buffs per weapon and restores them after
; a loadout change (swapping weapons, or bringing a left-hand weapon into play /
; dual-wielding, which vanilla drops the buff on). Body buffs (Golden Vow,
; consumables) are left alone.
remember_per_weapon = 1
```

The AoW self-buff list (Endure, Determination, Roars, …) and the re-apply delay
are now **hard-coded** in `src/main.cpp` (no longer `.ini` options).

**Readable logs (optional):** drop a Paramdex `SpEffectParam.txt` (id → name list,
from soulsmods/Paramdex) next to the DLL and the log prints `id:Name` instead of
bare ids.

## Build

Prerequisites: Visual Studio 2022 (Desktop C++), CMake ≥ 3.15, Git.

```sh
git submodule update --init --recursive   # pulls MinHook + libER
```

Then run `build.bat`, or:

```sh
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output: `build/Release/PersistentBuffs.dll` (links MinHook + libER statically — no
extra DLLs to ship). libER is used only to read `SpEffectParam` for the buff
filter.

## Logs

Every launch writes `logs/PersistentBuffs.log` next to the DLL — including the
list of active SpEffects it sees, which is how you verify the offsets are right
for your game version.

## Status / internals

See **[CLAUDE.md](CLAUDE.md)** for the architecture, the exact offsets/signatures
used, what's verified vs. what still needs RE, and the step-by-step plan to
finish it.
