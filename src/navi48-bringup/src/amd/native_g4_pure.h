//
//  native_g4_pure.h - the PURE half of GPU-apps stage G4 (kext 0.0.640): ordinary user applications as a third client role, APP (an internal design note section 4, G4,
//  and the G1 / G2 reviews). No kernel header is included: tests/native_g4_test.cpp compiles this file and drives the very functions the kext calls
//  (amd/native_s1c.cpp, Navi48NativeClient.cpp, Navi48Bringup.cpp, Navi48UserClient.cpp).
//
//  DEFAULT OFF. Two boot-args, both latched once: navi48-multisession=1 (G2, the session array) AND navi48-apps=1 (this file). With either absent nothing here is reachable:
//  the open policy is today's (app_refine passes every caller through), accel action 104 is BadArgument, QueryInfo reports no budget, the close path frees every BO as 0.0.632 did
//  (close_vram_return_ok and the close-time unmap sweep are keyed on apps_on(), i.e. on BOTH latches - navi48-multisession=1 alone does not reach them), and the 0.0.641 rules below
//  (visible-pool cap, WindowServer slot reserve, strike count) are all gated the same way. The one 0.0.641 change that is NOT gated is the rate limit on the "open refused" log line
//  (log_gate_step): it only drops repeated lines of that one message.
//
//  With both ON:
//    * a process with uid >= 501 that is not root (euid != 0: kIOClientPrivilegeAdministrator is "root") is admitted as an APP when its name is on the kernel allow-list (default deny; max 32 entries; empty at every boot;
//      managed only by accel 104 `appallow`, which the root-only legacy user client carries). See Navi48AppKey.h for what the name is worth.
//    * an APP session is an ordinary non-WindowServer session: the 25 % VRAM budget (G2), the system-memory cap (R4), poisoning and recovery (R2/R3) all apply unchanged. Besides
//      that it never reaches ReadRegs, the scanout set or selectors 15..20 (native_open_policy_pure.h selector_allowed_for_app), and the engine refuses them again by itself
//      (app_call_allowed: defence in depth, so a future dispatch mistake in the client class does not open them).
//    * QueryInfo reports the session's VRAM budget (ABI 1.10, reserved[3..7]): the cap for all non-WindowServer sessions together and what is still available.
//    * Close path of R6: before the session's BOs go, the client class removes every CPU mapping that is still held for the session's visible-VRAM BOs (IOUserClient::
//      removeMappingForDescriptor, unmap_attempts below), so a normal exit or a kill -9 no longer leaves a mapping that pins the range. A visible-VRAM BO whose CPU mapping STILL outlives
//      the closing session after that is NOT returned to the shared pool (close_vram_return_ok): its range is leaked until reboot and stays counted against the budget, so another
//      process can never be handed memory a client can still read and write.
//    * 0.0.641 (review): non-WindowServer sessions together hold at most a cap of the VISIBLE pool (navi48-appvis=<MiB> 16..128, default 64) and a NO_CPU_ACCESS BO is placed in the hi
//      pool first; one slot is reserved for WindowServer (open_pick); an executable name whose session was blamed for 3 automatic recoveries is refused (strike table).
//
#pragma once
#include <stdint.h>

#include "native_g2_pure.h"
#include "native_open_policy_pure.h"
#include "../Navi48AppKey.h"

namespace n48native {
namespace g4 {

// ---- the switch -----------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kLatchUnset = 0u, kLatchOff = 1u, kLatchOn = 2u;
constexpr uint32_t latch_value(bool present, uint32_t value) { return (present && value == 1u) ? kLatchOn : kLatchOff; }
constexpr bool latch_is_on(uint32_t latch) { return latch == kLatchOn; }
// The APP role exists only with BOTH latches: the G2 session array is what gives an app its own VMID.
constexpr bool apps_enabled(bool appsLatchOn, bool multiLatchOn) { return appsLatchOn && multiLatchOn; }

// ---- the allow-list (accel action 104 `appallow`) -------------------------------------------------------------------------------------------------------
// kMaxEntries slots of 60-bit keys (Navi48AppKey.h); 0 = an empty slot. The kernel's list is a static array (zero at every boot, never persisted). Lock-free: add claims an
// empty slot with a compare-and-swap, remove clears every slot holding the key (a racing double add is harmless), contains / list only load.
constexpr uint32_t kMaxEntries = 32u;
constexpr uint32_t kListPerPage = 9u;    // outputs o[4..12]
constexpr uint32_t kListPages = (kMaxEntries + kListPerPage - 1u) / kListPerPage;   // 4
static_assert(kListPages == 4u && kListPages * kListPerPage >= kMaxEntries, "four pages of nine hold the 32 entries");

inline bool al_contains(const uint64_t *l, uint64_t key) {
    if (key == 0ull) return false;
    for (uint32_t i = 0; i < kMaxEntries; i++) if (__atomic_load_n(&l[i], __ATOMIC_ACQUIRE) == key) return true;
    return false;
}
inline uint32_t al_count(const uint64_t *l) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kMaxEntries; i++) if (__atomic_load_n(&l[i], __ATOMIC_ACQUIRE) != 0ull) n++;
    return n;
}
enum AllowCode : uint32_t { kAllowOk = 0u, kAllowOff = 1u, kAllowBadArg = 2u, kAllowFull = 3u, kAllowExists = 4u, kAllowNotFound = 5u };
// The two hooks (test seams, a no-op in the kernel) run where a second adder can interleave: hook(0) after the contains check and before the claim, hook(1) after the claim and before the settle.
template <class Hook>
inline uint32_t al_add_h(uint64_t *l, uint64_t key, Hook &&hook) {
    if (key == 0ull || (key & ~N48A_KEY_MASK) != 0ull) return kAllowBadArg;
    if (al_contains(l, key)) return kAllowExists;
    // 0.0.642: a slot is claimed with ONE compare-and-swap 0 -> key, so two adders can never write the same slot; a slot that another adder just filled with the SAME key means the key
    // is present (success). Two adders of the same key may still claim two different slots (both passed the contains check): the LOWEST slot wins, every adder settles it after its
    // claim (it retreats from its own slot when a lower slot holds the key, and clears any higher duplicate), so a duplicate never keeps a spare slot and never starves a distinct key.
    // A full scan is retried a few times: the duplicates it saw may be about to retreat.
    hook(0u);
    for (uint32_t attempt = 0; attempt < 4u; attempt++) {
        for (uint32_t i = 0; i < kMaxEntries; i++) {
            uint64_t e = 0ull;
            if (!__atomic_compare_exchange_n(&l[i], &e, key, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                if (e == key) return kAllowExists;   // another adder of the same key got this slot: the key is present
                continue;
            }
            hook(1u);
            for (uint32_t j = 0; j < kMaxEntries; j++) {
                if (j == i || __atomic_load_n(&l[j], __ATOMIC_ACQUIRE) != key) continue;
                if (j < i) { uint64_t k2 = key; __atomic_compare_exchange_n(&l[i], &k2, 0ull, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); return kAllowExists; }
                uint64_t k3 = key; __atomic_compare_exchange_n(&l[j], &k3, 0ull, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
            }
            return kAllowOk;
        }
        if (al_contains(l, key)) return kAllowExists;
    }
    return kAllowFull;
}
inline uint32_t al_add(uint64_t *l, uint64_t key) { return al_add_h(l, key, [](uint32_t) {}); }
inline uint32_t al_remove(uint64_t *l, uint64_t key) {
    if (key == 0ull || (key & ~N48A_KEY_MASK) != 0ull) return kAllowBadArg;
    bool hit = false;
    for (uint32_t i = 0; i < kMaxEntries; i++) {
        uint64_t e = key;
        if (__atomic_compare_exchange_n(&l[i], &e, 0ull, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) hit = true;
    }
    return hit ? kAllowOk : kAllowNotFound;
}

// ---- 0.0.641: the strike table --------------------------------------------------------------------------------------------------------------------------
// Per executable name (the allow-list key): how many automatic recoveries (G2) were blamed on an APP session of that name this boot. At kStrikeLimit the name is refused at open
// (kReasonAppNotAllowed). kStrikeSlots > kMaxAutoRecoveries: the kernel never runs more than g2::kMaxAutoRecoveries recoveries per boot, so the table cannot fill; if it somehow did,
// strike_add answers kStrikeLimit (fail closed). Lock-free (CAS claim of an empty slot, atomic add), zero at every boot, never persisted.
constexpr uint32_t kStrikeSlots = 4u, kStrikeLimit = 3u;
static_assert(kStrikeSlots > g2::kMaxAutoRecoveries && kStrikeLimit == g2::kMaxAutoRecoveries, "the table holds a slot for every possible recovery; the limit is the per-boot budget");
struct StrikeTab { uint64_t key[kStrikeSlots]; uint32_t n[kStrikeSlots]; };
inline uint32_t strike_count(const StrikeTab &t, uint64_t key) {
    if (key == 0ull) return 0u;
    for (uint32_t i = 0; i < kStrikeSlots; i++) if (__atomic_load_n(&t.key[i], __ATOMIC_ACQUIRE) == key) return __atomic_load_n(&t.n[i], __ATOMIC_ACQUIRE);
    return 0u;
}
// One strike for `key`; returns the count after it.
inline uint32_t strike_add(StrikeTab &t, uint64_t key) {
    if (key == 0ull) return kStrikeLimit;
    for (uint32_t i = 0; i < kStrikeSlots; i++) {
        uint64_t e = 0ull;
        if (__atomic_load_n(&t.key[i], __ATOMIC_ACQUIRE) == key || __atomic_compare_exchange_n(&t.key[i], &e, key, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) || e == key)
            return __atomic_add_fetch(&t.n[i], 1u, __ATOMIC_ACQ_REL);
    }
    return kStrikeLimit;
}
constexpr bool strike_refused(uint32_t count) { return count >= kStrikeLimit; }
// A name is admitted as an APP only when it is on the allow-list AND not struck out.
constexpr bool app_admit(bool onAllowList, uint32_t strikes) { return onAllowList && !strike_refused(strikes); }

// ---- the verb ---------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kActAppAllow = 104u;
constexpr bool is_g4_action(uint32_t a) { return a == kActAppAllow; }
constexpr bool action_admitted(bool appsEnabled, uint32_t a) { return appsEnabled && is_g4_action(a); }
constexpr uint32_t arg_op(uint64_t arg) { return (uint32_t)(arg >> N48A_OP_SHIFT); }
constexpr uint64_t arg_payload(uint64_t arg) { return arg & N48A_KEY_MASK; }
constexpr bool args_ok(uint32_t a, uint64_t arg) {
    return a == kActAppAllow && ((arg_op(arg) == N48A_OP_ADD || arg_op(arg) == N48A_OP_REMOVE) ? arg_payload(arg) != 0ull
                                                                                                 : arg_op(arg) == N48A_OP_STRIKES ? arg_payload(arg) == 0ull   // 0.0.641
                                                                                                 : (arg_op(arg) == N48A_OP_LIST && arg_payload(arg) < (uint64_t)kListPages));
}
constexpr bool native_exempt(bool appsEnabled, uint32_t a, uint64_t arg) { return action_admitted(appsEnabled, a) && args_ok(a, arg); }
constexpr uint32_t kOutN = 13u;
// Runs one `appallow` call against list l and fills o[0..12]: o[0] = AllowCode, o[1] = entries after the call, o[2] = the operation, o[3] = max entries (32).
// add / remove: o[4] = the key. list: o[4..12] = the entries of that page (the slot order; 0 = empty slot). strikes (0.0.641): o[4 + 2i] / o[5 + 2i] = key / strikes of table slot i, o[12] = the limit.
inline uint32_t verb_run(bool appsEnabled, uint64_t *l, uint64_t arg, uint64_t *o, const StrikeTab *st = nullptr) {
    for (uint32_t i = 0; i < kOutN; i++) o[i] = 0ull;
    o[3] = kMaxEntries;
    if (!appsEnabled) { o[0] = kAllowOff; return kAllowOff; }
    if (!args_ok(kActAppAllow, arg)) { o[0] = kAllowBadArg; return kAllowBadArg; }
    const uint32_t op = arg_op(arg);
    const uint64_t pl = arg_payload(arg);
    uint32_t rc = kAllowOk;
    o[2] = op;
    if (op == N48A_OP_ADD) { rc = al_add(l, pl); o[4] = pl; }
    else if (op == N48A_OP_REMOVE) { rc = al_remove(l, pl); o[4] = pl; }
    else if (op == N48A_OP_STRIKES) {   // 0.0.641: o[4 + 2i] = the key, o[5 + 2i] = its strikes (slot order, 0 = empty slot); o[12] = the limit
        for (uint32_t i = 0; i < kStrikeSlots && st != nullptr; i++) { o[4 + 2 * i] = __atomic_load_n(&st->key[i], __ATOMIC_ACQUIRE); o[5 + 2 * i] = st->key[i] != 0ull ? __atomic_load_n(&st->n[i], __ATOMIC_ACQUIRE) : 0ull; }
        o[12] = kStrikeLimit;
    } else {
        for (uint32_t i = 0; i < kListPerPage; i++) {
            const uint32_t idx = (uint32_t)pl * kListPerPage + i;
            o[4 + i] = idx < kMaxEntries ? __atomic_load_n(&l[idx], __ATOMIC_ACQUIRE) : 0ull;
        }
    }
    o[0] = rc; o[1] = al_count(l);
    return rc;
}

// ---- the budget report (QueryInfo, ABI 1.10) ----------------------------------------------------------------------------------------------------------------
struct BudgetReport { uint32_t flags; uint64_t cap, avail; };
// OFF (not multi-session) or WindowServer's session: no budget applies, everything 0. Otherwise: the group's cap (25 %), and what is available to this session now =
// min(cap - held by all non-WindowServer sessions, free bytes of the VRAM pools).
inline BudgetReport budget_report(bool multi, bool isWs, bool isApp, uint64_t othersHeld, uint64_t vramTotal, uint64_t poolFree) {
    BudgetReport r { 0u, 0ull, 0ull };
    if (!multi || isWs) return r;
    const uint64_t cap = g2::others_vram_cap(vramTotal);
    const uint64_t room = othersHeld >= cap ? 0ull : cap - othersHeld;
    r.flags = (uint32_t)N48N_BUDGET_VALID | (isApp ? (uint32_t)N48N_BUDGET_APP : 0u);
    r.cap = cap; r.avail = room < poolFree ? room : poolFree;
    return r;
}
inline void budget_pack(const BudgetReport &b, uint32_t *reserved) {
    reserved[N48N_INFO_R_BUDGET_FLAGS] = b.flags;
    reserved[N48N_INFO_R_BUDGET_CAP_LO] = (uint32_t)(b.cap & 0xFFFFFFFFull);   reserved[N48N_INFO_R_BUDGET_CAP_HI] = (uint32_t)(b.cap >> 32);
    reserved[N48N_INFO_R_BUDGET_AVAIL_LO] = (uint32_t)(b.avail & 0xFFFFFFFFull); reserved[N48N_INFO_R_BUDGET_AVAIL_HI] = (uint32_t)(b.avail >> 32);
}

// ---- defence in depth: what an APP session's engine calls refuse by themselves ------------------------------------------------------------------------------
enum AppCall : uint32_t { kCallReadRegs = 0u, kCallScanout = 1u, kCallOther = 2u };
constexpr bool app_call_allowed(bool isApp, uint32_t call) { return !isApp || call == (uint32_t)kCallOther; }

// ---- R6 on the CLOSE path -----------------------------------------------------------------------------------------------------------------------------------
// Called with the descriptor's retain count read BEFORE the closing session drops its own reference. A visible-VRAM BO whose descriptor has a reference beyond the BO record's
// own is still CPU-mapped by some process: with the switch ON its range must not go back to the pool (the mapping can read and write whoever is handed that range next).
// OFF: as 0.0.629 (always returned). Everything that is not visible VRAM (GTT pages belong to the descriptor, host imports to the caller, the hi pool is not CPU-mappable) returns.
// 0.0.641 (G4 review HIGH 1a): `appsOn` is apps_on() (navi48-apps AND navi48-multisession), NOT multi_on(): with navi48-apps absent the close path frees every BO exactly as 0.0.632 did.
constexpr bool close_vram_return_ok(bool appsOn, bool isVisVram, bool hasDescriptor, uint32_t descriptorRefs) {
    return !(appsOn && isVisVram && hasDescriptor && descriptorRefs > 1u);
}
// 0.0.641 (G4 review HIGH 1b): how many IOUserClient::removeMappingForDescriptor calls the close-time sweep makes for one BO. descriptorRefs = the descriptor's retain count read BEFORE the
// sweep takes its own reference: 1 (the BO record's, IODeviceMemory::withRange) + one per live IOMemoryMap made from it (each map retains its descriptor; clientMemoryForType's own +1 is
// released by XNU's mapClientMemory64 right after createMappingInTask). So refs - 1 maps exist at most, each removal returns one retained map, releasing it drops that map's reference
// (and unmaps it from the task if the task is alive). 0 = nothing to do: apps OFF, not visible VRAM, no descriptor, or no mapping. Bounded so a corrupt count cannot loop.
constexpr uint32_t kUnmapMaxPerBo = 256u;
constexpr uint32_t unmap_attempts(bool appsOn, bool isVisVram, bool hasDescriptor, uint32_t descriptorRefs) {
    return (appsOn && isVisVram && hasDescriptor && descriptorRefs > 1u) ? (descriptorRefs - 1u < kUnmapMaxPerBo ? descriptorRefs - 1u : kUnmapMaxPerBo) : 0u;
}

// ---- 0.0.641 (G4 review): the visible-VRAM pool -------------------------------------------------------------------------------------------------------------
// The visible pool (BAR0, about 217 MiB after the carve-outs) is what WindowServer's scanout and CPU-mapped BOs need. With apps ON a non-WindowServer session (an APP, a root tool) may hold at
// most kAppVis*MiB of it TOGETHER (a cap on top of the 25 % budget, which is measured against visible + hi VRAM), and a NO_CPU_ACCESS BO (place_decide's hiOk) is tried in the hi pool first.
constexpr uint32_t kAppVisDefaultMiB = 64u, kAppVisMinMiB = 16u, kAppVisMaxMiB = 128u;
// boot-arg navi48-appvis=<MiB>: in range 16..128 it is the cap; absent or out of range, the default (64 MiB).
constexpr uint64_t appvis_cap_bytes(bool present, uint32_t mib) { return (uint64_t)((present && mib >= kAppVisMinMiB && mib <= kAppVisMaxMiB) ? mib : kAppVisDefaultMiB) << 20; }
constexpr bool appvis_ok(bool appsOn, bool isWs, uint64_t held, uint64_t add, uint64_t cap) { return !appsOn || isWs || !s1c::would_exceed(held, add, cap); }
constexpr bool hi_first(bool appsOn, bool isWs, bool hiOk) { return appsOn && !isWs && hiOk; }
// Does a non-WindowServer BO count against the visible cap? Yes when it was placed in the visible pool (not hi, not GTT) with apps ON.
constexpr bool appvis_counts(bool appsOn, bool isWs, bool placedVisible) { return appsOn && !isWs && placedVisible; }

// ---- 0.0.641: the "open refused" log line --------------------------------------------------------------------------------------------------------------------
// Every Metal-enumerating process of a user session probes the N48N open (the bundle's gate), so with the switches off the kernel logged one line per process. At most kLogPerSec lines
// per second; a window that dropped lines reports "N suppressed" once, at the start of the next window. One 64-bit word (second | lines used << 32 | suppressed << 48) so a CAS loop can
// advance it without a lock. A log-only rate limiter: nothing else reads it.
constexpr uint32_t kLogPerSec = 5u;
inline uint64_t log_gate_step(uint64_t w, uint64_t nowSec, bool *admit, uint32_t *suppressedBefore) {
    const uint64_t sec = w & 0xFFFFFFFFull; const uint32_t used = (uint32_t)((w >> 32) & 0xFFFFu), supp = (uint32_t)((w >> 48) & 0xFFFFu);
    *suppressedBefore = 0u;
    if ((nowSec & 0xFFFFFFFFull) != sec) { *suppressedBefore = supp; *admit = true; return (nowSec & 0xFFFFFFFFull) | (1ull << 32); }
    if (used < kLogPerSec) { *admit = true; return w + (1ull << 32); }
    *admit = false; return supp < 0xFFFFu ? w + (1ull << 48) : w;
}

} // namespace g4
} // namespace n48native
