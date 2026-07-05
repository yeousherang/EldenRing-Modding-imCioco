#include "messages.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <unordered_set>
#include <utility>
#include <vector>

#include "log.hpp"
#include "scan.hpp"

namespace cte::messages {
namespace {

// MsgRepositoryImp global: `mov rdi, [rip+disp32]` (48 8B 3D ....) followed by
// `movzx r14d, byte [rax]; test rdi,rdi; jnz ...`. disp32 at +3, insn len 7.
// (Same signature the ERR DLL uses.)
constexpr const char* kMsgRepoAob =
    "48 8B 3D ?? ?? ?? ?? 44 0F B6 30 48 85 FF 75";

// FMG "AccessoryName" lives in these MsgRepository slots, highest layer first
// (DLC02, DLC01, base) -- the game merges them in this priority. Confirmed in
// the ERR DLL (accessory_slots{416, 316, 13}).
constexpr int kNameSlots[] = {416, 316, 13};

// A FMG "group": a contiguous [first_id, last_id] id range mapping to a run of
// string-offset-table indices starting at string_index. (SoulsFormats layout;
// field order verified against the ERR DLL.)
struct FmgGroup {
    int32_t string_index;
    int32_t first_id;
    int32_t last_id;
    int32_t pad;
};

uintptr_t g_slots_addr = 0;   // address of the slot -> FMG* pointer array
int32_t   g_slot_count = 0;   // number of slots
bool      g_resolved = false; // resolve_repo() has run (success or fail)
bool      g_ready = false;    // name slots readable
std::vector<int> g_effect_slots; // detected AccessoryInfo/Caption slots, Info first

// FMG* for a slot, via guarded reads. 0 if out of range / unmapped.
uintptr_t fmg_ptr(int slot) {
    if (slot < 0 || slot >= g_slot_count || !g_slots_addr) return 0;
    return mem::deref(g_slots_addr + static_cast<uintptr_t>(slot) * sizeof(void*));
}

// Walk a FMG's group table for `id`, returning a pointer to its UTF-16 string
// in game memory (nullptr on miss). POD-only + SEH-guarded: a stale/foreign
// slot pointer must never crash the game.
const wchar_t* fmg_lookup_raw(uintptr_t fmg, int32_t id) noexcept {
    if (!fmg) return nullptr;
    __try {
        auto* p = reinterpret_cast<uint8_t*>(fmg);
        uint32_t grp_cnt = *reinterpret_cast<uint32_t*>(p + 0x0C);
        uint32_t str_cnt = *reinterpret_cast<uint32_t*>(p + 0x10);
        uint64_t raw_off = *reinterpret_cast<uint64_t*>(p + 0x18);
        // Sanity-guard the header before walking (implausible = wrong slot).
        // Real item FMGs have at most a few thousand groups; a stale slot
        // (Reforged loader) can carry a huge bogus count -- cap it hard.
        if (grp_cnt == 0 || grp_cnt > 0x10000 || str_cnt > 0x100000 || raw_off == 0)
            return nullptr;

        uint8_t* off_ptr = (raw_off > 0x1000000)
            ? reinterpret_cast<uint8_t*>(raw_off)
            : p + raw_off;
        auto* groups = reinterpret_cast<FmgGroup*>(p + 0x28);
        auto* str_offs = reinterpret_cast<uint64_t*>(off_ptr);

        for (uint32_t g = 0; g < grp_cnt; ++g) {
            const int32_t first = groups[g].first_id;
            const int32_t last = groups[g].last_id;
            if (id < first || id > last) continue;
            const int32_t si = groups[g].string_index + (id - first);
            if (si < 0 || si >= static_cast<int32_t>(str_cnt)) return nullptr;
            const uint64_t s_off = str_offs[si];
            if (s_off == 0) return nullptr;
            const wchar_t* text = (s_off > 0x1000000)
                ? reinterpret_cast<const wchar_t*>(s_off)
                : reinterpret_cast<const wchar_t*>(p + s_off);
            return (text && text[0]) ? text : nullptr;
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Count the ids in a FMG that resolve to a NON-EMPTY string, in one pass over
// its group/offset tables. Early-exits at `cap` (a bank far bigger than the
// talisman set can't be the accessory-description bank). POD + SEH-guarded.
// Returns -1 if the header is unreadable/implausible.
//
// HARD ITERATION BUDGET: some runtimes (notably Elden Ring Reforged's loader)
// leave STALE pointers in MsgRepository slots. A garbage "FMG" can pass the
// header sanity check yet declare id ranges billions wide; without a budget
// this walk never finishes and the worker thread hangs before build_state, so
// the overlay showed 0 talismans on ERR. A real item FMG covers its ids in
// well under the budget; blowing it means "not our bank" (-1).
int fmg_count_resolving(uintptr_t fmg, int cap) noexcept {
    if (!fmg) return -1;
    __try {
        auto* p = reinterpret_cast<uint8_t*>(fmg);
        uint32_t grp_cnt = *reinterpret_cast<uint32_t*>(p + 0x0C);
        uint32_t str_cnt = *reinterpret_cast<uint32_t*>(p + 0x10);
        uint64_t raw_off = *reinterpret_cast<uint64_t*>(p + 0x18);
        if (grp_cnt == 0 || grp_cnt > 0x10000 || str_cnt > 0x100000 || raw_off == 0)
            return -1;
        // A genuine FMG's group ranges are dense: total ids ~ string count.
        // Budget generously above that, then bail.
        int64_t budget = static_cast<int64_t>(str_cnt) * 4 + 0x10000;
        uint8_t* off_ptr = (raw_off > 0x1000000)
            ? reinterpret_cast<uint8_t*>(raw_off)
            : p + raw_off;
        auto* groups = reinterpret_cast<FmgGroup*>(p + 0x28);
        auto* str_offs = reinterpret_cast<uint64_t*>(off_ptr);
        int count = 0;
        for (uint32_t g = 0; g < grp_cnt; ++g) {
            int32_t si = groups[g].string_index;
            const int32_t first = groups[g].first_id;
            const int32_t last = groups[g].last_id;
            if (last < first) continue;
            if ((budget -= (static_cast<int64_t>(last) - first + 1)) < 0)
                return -1; // absurd range -> stale/garbage slot
            // int64 loop var: with a garbage group whose last_id is near
            // INT32_MAX, an int32 `id <= last` NEVER goes false (++id wraps
            // negative) -- this exact overflow hung the scan on Reforged.
            for (int64_t id = first; id <= static_cast<int64_t>(last); ++id, ++si) {
                if (si < 0 || si >= static_cast<int32_t>(str_cnt)) continue;
                const uint64_t s_off = str_offs[si];
                if (s_off == 0) continue;
                const wchar_t* text = (s_off > 0x1000000)
                    ? reinterpret_cast<const wchar_t*>(s_off)
                    : reinterpret_cast<const wchar_t*>(p + s_off);
                if (text && text[0] && ++count >= cap) return count;
            }
        }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

std::string to_utf8(const wchar_t* w) {
    if (!w) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

// Collapse the game's newlines/tabs/runs of spaces to single spaces + trim, so
// multi-line caption text reads cleanly in the (wrapped) overlay pane.
std::string sanitize_inline(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool sp = true;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ') { if (!sp) { out.push_back(' '); sp = true; } }
        else { out.push_back(c); sp = false; }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// First non-empty live string for `id` across a slot list (game merge order).
std::string lookup_across(const int* slots, size_t n, int id) {
    for (size_t i = 0; i < n; ++i) {
        const wchar_t* w = fmg_lookup_raw(fmg_ptr(slots[i]), id);
        if (w) {
            std::string s = sanitize_inline(to_utf8(w));
            if (!s.empty()) return s;
        }
    }
    return {};
}
std::string lookup_across(const std::vector<int>& slots, int id) {
    return slots.empty() ? std::string{} : lookup_across(slots.data(), slots.size(), id);
}

bool resolve_repo() {
    if (g_resolved) return g_slots_addr != 0;
    g_resolved = true;

    const mem::Module m = mem::main_module();
    bool multiple = false;
    const uintptr_t hit = mem::aob_scan_unique(m, kMsgRepoAob, &multiple);
    if (!hit) { flog("[messages] MsgRepositoryImp AOB not found -- live names disabled"); return false; }
    if (multiple) flog("[messages] [WARN] MsgRepositoryImp AOB matched multiple sites -- using first");

    const uintptr_t global = mem::rip_relative(hit, 3, 7); // &MsgRepositoryImp*
    if (!global) return false;

    uintptr_t repo = 0;
    for (int i = 0; i < 100 && !repo; ++i) { // bounded wait (~5s) for it to populate
        repo = mem::deref(global);
        if (!repo) Sleep(50);
    }
    if (!repo) { flog("[messages] MsgRepositoryImp still null -- live names disabled"); return false; }

    // repo+0x08 -> array-of-arrays; [0] -> the slot -> FMG* pointer array.
    const uintptr_t base_array = mem::deref(repo + 0x08);
    const uintptr_t slots = base_array ? mem::deref(base_array) : 0;
    int32_t count = 0;
    mem::safe_read(repo + 0x14, count);
    if (!slots || count <= 13 || count > 0x100000) {
        flog("[messages] MsgRepository layout unexpected (count=%d) -- live names disabled", count);
        return false;
    }
    g_slots_addr = slots;
    g_slot_count = count;
    return true;
}

// Detect the AccessoryInfo/Caption slots WITHOUT hard-coding slot numbers.
//
// The fingerprint is anchored on the ONE thing we know exactly: the talisman id
// set from the live EquipParamAccessory. A slot is an accessory-description
// bank iff BOTH:
//   (a) it resolves strings for (nearly) all the talisman ids a NAME layer
//       carries -- high coverage; and
//   (b) it resolves almost NOTHING besides them -- so a subtitle/dialogue bank
//       (which has strings at thousands of sequential low ids, e.g. the "Bear
//       witness!" bank) is rejected even though it covers the talisman range.
// Scored as Jaccard( ids-resolving-in-slot, talisman-ids-resolving-in-name-layer )
// using RESOLVING-string counts on both sides. An earlier version compared raw
// group-id RANGES (full of unused holes), which deflated the true banks' scores
// to ~0.02 and rejected them.
//
// Scored per name layer (base / dlc01 / dlc02) so a DLC-only description layer
// still matches its DLC name layer at ~1.0.
void discover_effect_slots(const std::vector<int>& accessory_ids) {
    if (!g_slots_addr || accessory_ids.empty()) return;

    // Per name layer: the talisman ids that layer actually carries.
    std::vector<std::vector<int>> layer_sets;
    size_t max_layer = 0;
    for (int ns : kNameSlots) {
        const uintptr_t fmg = fmg_ptr(ns);
        if (!fmg) continue;
        std::vector<int> ids;
        for (int id : accessory_ids)
            if (fmg_lookup_raw(fmg, id)) ids.push_back(id);
        if (ids.size() < 4) continue; // stale/empty layer
        max_layer = std::max(max_layer, ids.size());
        layer_sets.push_back(std::move(ids));
    }
    if (layer_sets.empty()) {
        flog("[messages] [WARN] no name layer resolves talisman ids -- effect detection skipped");
        return;
    }

    // A real description layer can't have many more strings than the largest
    // name layer. Cap the resolving-count walk there (+ slack) so huge banks
    // are rejected cheaply without walking them fully.
    const int cap = static_cast<int>(max_layer * 2) + 64;
    flog("[messages] scanning %d msg slots for accessory description banks...", g_slot_count);

    // Absolute wall-clock ceiling on the scan. Whatever garbage a runtime's
    // stale slots throw at us, the worker must reach build_state -- on timeout
    // we keep any candidates found so far and descriptions fall back to baked.
    const ULONGLONG scan_start = GetTickCount64();
    constexpr ULONGLONG kScanBudgetMs = 5000;

    struct Cand { int slot; size_t len; double score; };
    std::vector<Cand> cands;
    for (int slot = 0; slot < g_slot_count; ++slot) {
        if (GetTickCount64() - scan_start > kScanBudgetMs) {
            flog("[messages] [WARN] slot scan timed out at slot %d -- continuing with %zu candidate(s)",
                 slot, cands.size());
            break;
        }
        bool is_name = false;
        for (int ns : kNameSlots) if (ns == slot) { is_name = true; break; }
        if (is_name) continue;

        const uintptr_t fmg = fmg_ptr(slot);
        if (!fmg) continue;

        // (b) total resolving strings in the slot; too many -> not ours.
        const int total = fmg_count_resolving(fmg, cap);
        if (total <= 0 || total >= cap) continue;

        // (a) coverage of the talisman ids + avg text length (Info vs Caption).
        std::unordered_set<int> matched;
        size_t total_len = 0;
        for (int id : accessory_ids) {
            const wchar_t* w = fmg_lookup_raw(fmg, id);
            if (w && matched.insert(id).second) total_len += wcslen(w);
        }
        if (matched.empty()) continue;

        double best = 0.0;
        for (const auto& set : layer_sets) {
            size_t inter = 0;
            for (int id : set) if (matched.count(id)) ++inter;
            const double uni = static_cast<double>(set.size() + total) - static_cast<double>(inter);
            if (uni > 0.0) best = std::max(best, static_cast<double>(inter) / uni);
        }
        if (best >= 0.5)
            cands.push_back({slot, total_len / matched.size(), best});
    }

    // Shorter average text = AccessoryInfo (effect summary) first; longer =
    // AccessoryCaption (flavour). accessory_effect() returns the first non-empty.
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.len < b.len; });
    for (const auto& c : cands) {
        g_effect_slots.push_back(c.slot);
        flog("[messages] effect slot %d (score %.2f, avg len %zu)", c.slot, c.score, c.len);
    }
    if (g_effect_slots.empty())
        flog("[messages] [WARN] no accessory effect slot detected -- descriptions fall back to baked");
    flog("[messages] slot scan finished in %llu ms", GetTickCount64() - scan_start);
}

} // namespace

bool init(const std::vector<int>& accessory_ids) {
    if (g_resolved) return g_ready;
    if (!resolve_repo()) return false;
    discover_effect_slots(accessory_ids);
    // Ready if a known base talisman resolves a name.
    g_ready = !lookup_across(kNameSlots, static_cast<size_t>(std::size(kNameSlots)), 1000).empty();
    flog(g_ready ? "[messages] live talisman names available"
                 : "[messages] [WARN] name slots present but no base name resolved");
    return g_ready;
}

std::string accessory_name(int id) {
    if (!g_slots_addr) return {};
    return lookup_across(kNameSlots, static_cast<size_t>(std::size(kNameSlots)), id);
}

std::string accessory_effect(int id) {
    if (g_effect_slots.empty()) return {};
    return lookup_across(g_effect_slots, id);
}

} // namespace cte::messages
