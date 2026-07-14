// The param work: classify catalysts, enable cross-casting, mirror the
// magic/holy scaling sides, and (optionally) retarget all catalyst scaling to
// the player's highest casting stat.
#pragma once

#include <cstdint>

namespace omni {

enum class ScalingMode {
    Off,         // cross-casting only, scaling untouched
    Equipped,    // spells scale with whatever catalyst is held (mirror)
    HighestStat, // all catalysts scale both spell types off max(INT, FAI)
};

struct Config {
    bool        cast_anything = true;
    ScalingMode mode          = ScalingMode::Equipped;
    bool        dump          = false;
};

// One-shot param pass after wait_for_params(). Safe to call exactly once.
void apply_all(const Config& cfg);

// Highest-stat mode poll body (call ~1x/s from the worker thread): reads the
// player's INT/FAI and flips catalyst stat-correction bits when the higher
// stat changes. No-ops in other modes.
void highest_stat_tick(const Config& cfg);

} // namespace omni
