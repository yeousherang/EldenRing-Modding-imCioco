// ============================================================
//  PersistentBuffs - keep buffs through fast travel / death (Elden Ring)
//
//  Unlike the param-patcher mods in this repo, the engine WIPES active
//  SpEffects on fast travel / death regardless of duration (hardcoded; not
//  controllable via params -- confirmed by research, see CLAUDE.md). So this
//  mod works at RUNTIME: it remembers your active buff SpEffects and re-applies
//  them after the engine clears them on a transition.
//
//  STATUS: SCAFFOLD / WORK IN PROGRESS.
//    Working now:   loads, logs, resolves the player, ENUMERATES active
//                   SpEffects each tick (this verifies the offsets in-game).
//    TODO (needs RE / in-game verification -- see CLAUDE.md):
//      - confirm WorldChrMan / PlayerIns offsets for the target game version
//      - resolve the "apply SpEffect" function (AOB) so re-apply actually works
//      - distinguish fast-travel vs death so the two toggles are independent
//      - filter "persistable" buffs (exclude debuffs / system effects)
//
//  OFFLINE ONLY (EAC must be off). See README.md.
//
//  The code is split across src/*.cpp by feature -- config.cpp owns the .ini
//  options, offsets.hpp is the version-specific memory layout, game_access.cpp
//  reads the player/weapon state, buff_filters.cpp decides what to keep, and
//  weapon_memory.cpp is the [weapon_memory] feature. This file is just the
//  poll loop (run()) and the DLL entry point.
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <MinHook.h>

#include <string>
#include <unordered_set>
#include <vector>

// libER: SoloParamRepository::wait_for_params + from::param::SpEffectParam.
// Used only to read SpEffectParam for the field-based buff filter (see
// is_persistable). Player access / re-apply still go through raw memory + AOB.
#include <coresystem/cs_param.hpp>

#include "config.hpp"
#include "utils.hpp"
#include "offsets.hpp"
#include "game_access.hpp"
#include "buff_filters.hpp"
#include "buff_discovery.hpp"
#include "buff_timing.hpp"
#include "weapon_memory.hpp"
#include "session_store.hpp"

#include <cmath>
#include <cstdio>

namespace pb {

void reapply(uintptr_t player, const std::vector<int>& ids) {
    if (!g_apply) { // function not resolved yet -> inert, but log intent
        flog("reapply: SKIPPED (%zu buff(s)) -- apply function not resolved (TODO AOB)",
             ids.size());
        return;
    }
    int n = 0, vetoed = 0;
    std::string applied;
    for (int id : ids) {
        if (!is_persistable(id)) continue;
        const double rem = timing_remaining(id); // read before apply, for the log
        if (!apply_persisted(player, id)) {      // veto: already past its own timer
            flog("reapply: veto %s -- already past its own duration", named(id).c_str());
            ++vetoed;
            continue;
        }
        char tail[24];
        if (std::isfinite(rem)) std::snprintf(tail, sizeof(tail), "(%.1fs) ", rem);
        else                    std::snprintf(tail, sizeof(tail), "(inf) ");
        applied += named(id); applied += tail;
        ++n;
    }
    flog("reapply: re-applied %d buff(s), vetoed %d expired: [ %s]",
         n, vetoed, applied.c_str());
}

// ---- worker: poll player, remember buffs, re-apply after a wipe --------
// NOTE: this poll loop is the scaffold's engine. The "hooks" infrastructure
// (MinHook) is initialized below and is the intended home for precise
// transition detection (hook the SpEffect-clear / respawn / warp). See CLAUDE.md.
DWORD WINAPI run(LPVOID) {
    g_mod = mem::main_module();
    flog("module base=%p size=0x%zX", reinterpret_cast<void*>(g_mod.base), g_mod.size);

    // Resolve the apply function. The AOB lands inside the function body, so
    // back up to the real entry point. Refuse a non-unique match (would risk
    // calling the wrong function and crashing the game).
    if (kApplySpEffectAob[0]) {
        bool multiple = false;
        const uintptr_t hit = mem::aob_scan_unique(g_mod, kApplySpEffectAob, &multiple);
        if (hit && !multiple) {
            g_apply = reinterpret_cast<ApplySpEffect_t>(hit - kApplySpEffectFuncBackset);
            flog("apply function: resolved hit=%p entry=%p",
                 reinterpret_cast<void*>(hit), reinterpret_cast<void*>(g_apply));
        } else if (multiple) {
            flog("[WARN] apply function: AOB matched MULTIPLE sites -- refusing to "
                 "use (re-apply stays inert). Signature needs tightening.");
        } else {
            flog("[WARN] apply function: NOT FOUND (AOB drifted for this game version)");
        }
    } else {
        flog("apply function: AOB not set (re-apply is inert)");
    }

    // Hook ApplySpEffect so a recast of a still-active buff resets its timer
    // to full duration (the poll can't see a mid-duration recast -- the id
    // never leaves the active list). No-op if g_apply didn't resolve.
    timing_install_hook();

    // Resolve GameDataMan -- needed for weapon-memory (weapon slots) AND for
    // session persistence (character-name key at PlayerGameData + kCharNameOffset).
    if (g_weapon_memory || g_session_persist) {
        bool multiple = false;
        const uintptr_t site = mem::aob_scan_unique(g_mod, kGameDataManAob, &multiple);
        if (site && !multiple) {
            g_gamedataman_var = mem::rip_relative(site, 3, 7);
            flog("GameDataMan ptr var=%p", reinterpret_cast<void*>(g_gamedataman_var));
        } else {
            flog("[WARN] GameDataMan %s",
                 multiple ? "AOB matched MULTIPLE sites" : "AOB NOT FOUND");
            if (g_weapon_memory) {
                flog("[WARN] weapon-memory: disabled (needs GameDataMan)");
                g_weapon_memory = false;
            }
            if (g_session_persist)
                flog("[WARN] session: GameDataMan unavailable -- all characters "
                     "share the 'default' key");
        }
    }

    // Cross-session persistence: parse the saved state into the in-memory mirror
    // (needs no params; is_persistable/timing seeding happen later, on restore).
    // Loaded for weapon-memory too: the state file is WRITTEN whenever either
    // feature is on (so a restart can't erase per-weapon buffs), and the mirror
    // must start from the on-disk content or the first save would clobber the
    // other characters' sections. Restoring (reading buffs back into the
    // character) stays gated on g_session_persist.
    if (g_session_persist || g_weapon_memory) session_startup_load();

    // Wait for params so the field-based buff filter (is_persistable -> sp_row ->
    // SpEffectParam) works. Safe from this worker thread (not DllMain, not a
    // thread the main thread waits on). The player only exists after params load,
    // so this returns well before the first re-apply.
    flog("waiting for params (buff filter needs SpEffectParam)...");
    if (from::CS::SoloParamRepository::wait_for_params(-1)) {
        flog("params ready -- building source-category buff allowlist");
        build_tracked_speffects(); // scans Goods/Magic; needs params loaded
    } else {
        flog("[WARN] wait_for_params timed out -- buff filter degraded "
             "(allowlist empty; only force/always-persist ids will persist)");
    }

    std::vector<int> remembered;   // buffs to re-apply on the next transition
    std::vector<int> current;
    bool had_player   = false;
    bool frozen       = false;     // holding the pre-strip set through a death
    int  frozen_ticks = 0;
    std::string last_char_key;     // session: current character key ("" = none yet)
    int         save_ticks = 0;    // session: autosave counter

    for (;;) {
        const uintptr_t player = get_player_ins();

        if (player) {
            enumerate_speffects(player, current);
            if (g_log_effects) { log_effect_changes(current); log_filter_changes(current); }

            std::vector<int> snap;
            for (int id : current)
                if (is_persistable(id)) snap.push_back(id);
            timing_tick(true, snap);
            // Drop buffs whose own timer ran out (expired AND absent) so a
            // stale death-freeze snapshot can't resurrect them -- this is the
            // "expired 15s grease came back on fast travel" fix.
            timing_prune_expired(remembered);

            if (!had_player) {
                // Just (re)entered a playable state -> a load/fast-travel/respawn
                // just completed and the engine has wiped effects. Settle, then
                // (optionally) restore this character's cross-session buffs and
                // re-apply `remembered`.
                // TODO: split fast-travel vs death to honor the two toggles
                //       independently (needs death/respawn detection).
                const bool session_track = g_session_persist || g_weapon_memory;
                const bool reapply_path = (g_keep_fast_travel || g_keep_death) &&
                                          !remembered.empty();
                if (reapply_path || session_track) {
                    flog("transition detected (settle %d ms)%s", kReapplyDelayMs,
                         session_track ? " [session]" : "");
                    if (kReapplyDelayMs > 0) Sleep(kReapplyDelayMs);
                    // Re-resolve the player after the settle wait -- the pointer
                    // can move while the world finishes loading. Also required for
                    // reading the character key (PlayerGameData must be populated).
                    const uintptr_t p = get_player_ins();
                    if (!p) {
                        flog("reapply: SKIPPED -- player vanished during settle");
                    } else {
                        if (session_track) {
                            bool name_ok = false;
                            const std::string key =
                                session_current_key(last_char_key, &name_ok);
                            if (key != last_char_key) {
                                // Real character change (incl. the first load):
                                // never inherit the previous character's buffs.
                                if (g_session_persist) {
                                    // Restore from disk if we have a saved entry,
                                    // else session_restore clears remembered +
                                    // timing + weapon bindings.
                                    const bool had_entry = session_restore(key, remembered);
                                    flog("session: character key='%s' (%s) -- %s",
                                         key.c_str(),
                                         name_ok ? "name read ok"
                                                 : "name read failed, key kept/default",
                                         had_entry ? "restored saved buffs" : "no saved buffs");
                                } else {
                                    // Weapon-memory only: the state file is written
                                    // under this key but nothing is read back
                                    // (restore needs remember_across_sessions=1).
                                    // Still never inherit the previous character's
                                    // weapon bindings.
                                    weapon_memory_clear_owners();
                                    flog("session: character key='%s' (%s) -- "
                                         "weapon-memory save only (no restore)",
                                         key.c_str(),
                                         name_ok ? "name read ok"
                                                 : "name read failed, key kept/default");
                                }
                                last_char_key = key;
                                save_ticks = 0;
                            }
                            // Same key (e.g. quit-to-menu -> same character): the
                            // in-memory `remembered` wins; don't re-read disk.
                        }
                        // Drop anything that expired while we were away (also
                        // guards a stale death-freeze snapshot -- see CLAUDE.md).
                        timing_prune_expired(remembered);
                        if ((g_keep_fast_travel || g_keep_death || g_session_persist) &&
                            !remembered.empty())
                            reapply(p, remembered);
                    }
                }
                had_player = true;
                // Fresh life: don't treat the weapon as "swapped" across the
                // transition, and stop holding the pre-death set so `remembered`
                // tracks the new life from here.
                weapon_memory_reset();
                frozen = false; frozen_ticks = 0;
            } else {
                // Stable gameplay: handle weapon swaps, then update `remembered`
                // with this tick's persistable buffs -- unless we're holding the
                // pre-strip set through a death (see kDeathDropThreshold).
                weapon_memory_tick(player, current);

                // How many currently-remembered buffs vanished this tick?
                // Expired ones don't count: a buff whose own timer ran out
                // when it vanished EXPIRED, it wasn't death-stripped -- so a
                // multi-id grease expiring naturally can no longer fake the
                // death pre-strip (the trigger half of the 15s-grease bug; a
                // real death drops MID-duration buffs, which still count).
                size_t lost = 0;
                if (!remembered.empty()) {
                    std::unordered_set<int> have(snap.begin(), snap.end());
                    for (int id : remembered)
                        if (!have.count(id) && !timing_is_expired(id)) ++lost;
                }

                if (frozen) {
                    // Held since a suspected death pre-strip. Two ways out:
                    //  * buffs recovered to ~the pre-drop richness -> NOT a death
                    //    (the player re-buffed / weapon-memory restored the dropped
                    //    weapon buffs); resync now. On a real death the set stays
                    //    depleted through "YOU DIED", so we keep holding.
                    //  * failsafe: give up after kFreezeMaxTicks so a real drop that
                    //    never recovers can't freeze us indefinitely.
                    // (Safe under timing_prune_expired: it only removes ids already
                    // gone from the live list, so it shrinks `remembered` toward
                    // `snap` -- it can release a false freeze (natural expiry)
                    // early, never extend one. Mid-duration ids stripped by a death
                    // have frozen clocks and are never pruned.)
                    if (snap.size() >= remembered.size() ||
                        ++frozen_ticks > kFreezeMaxTicks) {
                        remembered = std::move(snap);
                        frozen = false; frozen_ticks = 0;
                    }
                } else if (lost >= kDeathDropThreshold) {
                    // Sudden mass buff-loss = the death pre-strip. Keep the current
                    // (pre-strip) `remembered` and stop updating it until the
                    // transition re-applies it.
                    frozen = true; frozen_ticks = 0;
                    flog("death pre-strip detected (%zu buff(s) dropped this tick) "
                         "-> holding %zu buff(s) for re-apply", lost, remembered.size());
                } else {
                    remembered = std::move(snap);
                }
            }

            // Session autosave (crash insurance): persist `remembered` (and the
            // weapon-memory bindings) with their current remaining times every
            // kSessionSaveTicks while the player is valid. The clean save happens
            // on the valid->null edge below.
            if ((g_session_persist || g_weapon_memory) && !last_char_key.empty() &&
                ++save_ticks >= kSessionSaveTicks) {
                save_ticks = 0;
                session_save(last_char_key, remembered);
            }
        } else {
            // Valid -> null edge: save once before the load screen, using the
            // CACHED key (PlayerGameData may already be gone). During a death
            // freeze `remembered` holds the pre-strip set -- exactly what we want
            // persisted if the user Alt-F4s mid-"YOU DIED".
            if (had_player && (g_session_persist || g_weapon_memory) &&
                !last_char_key.empty()) {
                session_save(last_char_key, remembered);
                save_ticks = 0;
            }
            timing_tick(false, {}); // freeze the elapsed clocks through the load
            had_player = false; // loading screen / not in game -> preserve `remembered`
        }

        Sleep(200);
    }
}

} // namespace pb

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        pb::g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);

        pb::log_line("==== PersistentBuffs loaded (DllMain attach) ====", /*truncate=*/true);

        pb::load_config();

        if (MH_Initialize() == MH_OK)
            pb::flog("MinHook initialized");
        else
            pb::flog("[WARN] MinHook init failed");

        CreateThread(nullptr, 0, pb::run, nullptr, 0, nullptr);
    }
    return TRUE;
}
