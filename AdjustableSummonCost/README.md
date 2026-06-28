# Adjustable Summon Cost

A native C++ DLL mod for Elden Ring that **divides the FP (and HP) cost of spirit-ash summons** by a number you choose, so summons stay affordable without heavy Mind/INT investment.

It rewrites two param fields in memory at runtime (via [libER](https://github.com/Dasaav-dsv/libER)) — **no `regulation.bin` edit**:

| Field (`EquipParamGoods`) | Meaning | Action |
|---|---|---|
| `consumeMP` | FP cost of the summon | divided by `divisor` |
| `consumeHP` | HP cost (Mimic Tear, Soldjars of Fortune, Land Squirts, Miranda Sprouts…) | divided by `divisor` |

Only goods of the configured summon `goodsType`s (7 and 8) that actually have a cost are touched; costs are rounded and never drop below `min_cost`.

> ⚠️ **Offline only.** Elden Ring runs EasyAntiCheat. Run with a mod loader that disables it (ModEngine3/me3, ModEngine2, or Elden Mod Loader). Single-player / Seamless Co-op only.

---

## Configure

Edit `AdjustableSummonCost.ini` (next to the DLL, same base name):

```ini
[summons]
divisor = 3      ; FP/HP cost ÷ 3. 2 = half, 1 = no change, decimals allowed.
```

`[advanced]` lets you change the summon `goodsType` numbers, the minimum cost floor, and turn on `log_each = 1` to write every summon's before/after cost to the log (handy for verifying against the in-game summon menu).

---

## Build

**Prerequisites:** Visual Studio 2022 (Desktop C++ workload), CMake ≥ 3.15, Git.

```sh
git init
git submodule add https://github.com/Dasaav-dsv/libER external/libER
git submodule update --init --recursive

cmake -B build -A x64
cmake --build build --config Release
```

Output: `build/Release/AdjustableSummonCost.dll` — a single self-contained DLL (libER is linked statically, so there's no `libER.dll` to ship).

## Install

Copy `AdjustableSummonCost.dll` + `AdjustableSummonCost.ini` into your mod loader's native-mod folder (e.g. the me3 profile folder), launch offline.

## Logs

Every launch writes `logs/AdjustableSummonCost.log` next to the DLL. A success run ends with, e.g.:

```
divisor=3.00: 412 summons with a cost -> FP changed on 405, HP changed on 7
```

`FP changed on 0` would mean the `summon_goods_types` are wrong for your version — flip `log_each = 1` to inspect.
