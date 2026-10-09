//
//  native_m6_pure.h - the pure half of M6 Stage 1a (kext 0.0.659, an internal design note "M6 study: all three displays GPU-composited in one boot", sections 1, 3 and 4 "Stage 1a"):
//  every DECISION that lets the display pipe serve MORE THAN ONE framebuffer, judged from arguments alone. No kernel header: tests/native_disp_test.cpp (section N) compiles this very file through
//  amd/native_disp_flow.h and drives it, with planted breaks (tests/native_m6_plant.sh).
//
//  Everything here is behind the boot-arg latch navi48-m6=1 (read ONCE, default OFF). With the latch OFF no function of this file is reached by a path that existed in 0.0.658: the flows test the
//  latch FIRST and fall into their 0.0.658 code unchanged (the OFF-identity battery in tests/native_disp_test.cpp hashes that).
//
//  WHAT THE LATCH CHANGES (the M6 study's R1-R5 / D1-D4):
//    R1 aux Navi48Framebuffer starts beside navi48-metal-ws      (aux 0.0.7, tools/native/navi48accel/src/n48accel_pure.h fb_metal_ws_blocks)
//    R2 fbpublish after pipeadopt / a WindowServer session / the Metal nub   (native_fb_pure.h pre_verdict, Pre.m6)
//    R3 the Metal nub publishes while a display nub exists        (native_metal_pure.h publish_verdict, GateIn.m6)
//    R4 pipeadopt / pipearm 1 while a display nub exists          (native_disp_pure.h fb_interlock_refuses, m6 argument)
//  They are REPLACED by the ROUTING GUARD (section 3 of the study): the kernel keeps a table IOSurface ID -> instance, learned in the submit hook from the transaction itself
//  (txn+0x28 the pipe, pipe+0x98 its framebuffer -> the instance, plane 0's IOSurface, IOSurface+0x10 its ID), and REFUSES any present whose surface is not mapped to the instance of the pipe being
//  presented. An ID seen on two pipes is AMBIGUOUS and refused everywhere.
//
//  0.0.660 (queue 290 review fixes, still behind the latch): E1 the AGDC endpoint dword is the INDEX into the 0x980 list (measured); S1 the surface table heals itself (stale owners, LRU eviction, reset at the
//  restart window and at every arm); S2 the table property has ONE publisher at a time (publish_serialised); S5 the restart-loop guard counts an unplaceable pipe; N2 m6stat packing saturates;
//  N4 the downward instance loops are guarded by a static_assert; T1 the pipeagdc collect, the 0x980 list order and the OTG raster acceptance are pure functions the host tests drive; K1 txn_age_ms.
//
//  INSTANCES are the disp2 instances: 0 = the DP display (RDNA4FB, OTG0), 1 = the monitor A (OTG1, Navi48DisplayIndex 2), 2 = the monitor B (OTG2, Navi48DisplayIndex 1).
//
#pragma once
#include <stdint.h>
#include "../Navi48DisplayOps.h"
#include "../apple/display_pipe_guard.h"     // n48_agdc_timing (the 0x921 reply's timing record)

namespace n48m6 {

// ---- the boot-arg latch navi48-m6 (same shape as every latch of the kext: present AND == 1) ------------------------------------------------------------------------------------------------
constexpr uint32_t kLatchUnset = 0u, kLatchOff = 1u, kLatchOn = 2u;
constexpr uint32_t latch_value(bool present, uint32_t value) { return (present && value == 1u) ? kLatchOn : kLatchOff; }
constexpr bool latch_is_on(uint32_t latch) { return latch == kLatchOn; }
// The latch is READ ONCE: the first caller runs `read` (which parses the boot-arg and returns kLatchOn / kLatchOff), the winner of the compare-and-swap stores it, and every later call - including the losers of a race -
// returns the stored word without touching the boot-arg again. A reader that is called twice is a defect (tests/native_disp_test.cpp N1 counts the calls). native_disp.cpp's n48m6_latched_on is this function.
template <class Read> inline uint32_t latch_get(volatile uint32_t *slot, Read read) {
    uint32_t l = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (l == kLatchUnset) {
        const uint32_t v = read();
        uint32_t exp = kLatchUnset;
        (void)__atomic_compare_exchange_n(slot, &exp, v, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        l = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    }
    return l;
}

// ---- instances ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kInstDp = 0u, kInstMonA = 1u, kInstMonB = 2u, kInstCount = 3u, kInstNone = 0xFFFFFFFFu;
constexpr bool inst_valid(uint32_t i) { return i < kInstCount; }
// Navi48DisplayIndex of an instance (0 = none: the DP is RDNA4FB, not a nub), and back. ONE source: Navi48DisplayOps.h' index table.
constexpr uint32_t index_of_inst(uint32_t inst) { return inst == kInstMonB ? N48_DISP_INDEX : inst == kInstMonA ? N48_DISPA_INDEX : 0u; }
constexpr uint32_t inst_of_index(uint32_t index) { return index == N48_DISP_INDEX ? kInstMonB : index == N48_DISPA_INDEX ? kInstMonA : kInstNone; }
constexpr uint32_t otg_of_inst(uint32_t inst) { return inst == kInstMonB ? N48_DISP_OTG : inst == kInstMonA ? N48_DISPA_OTG : 0u; }
static_assert(inst_of_index(index_of_inst(kInstMonB)) == kInstMonB && inst_of_index(index_of_inst(kInstMonA)) == kInstMonA, "the instance <-> index tables agree");
static_assert(N48_DISP_INST_MONB == kInstMonB && N48_DISP_INST_MONA == kInstMonA, "the instance numbers are Navi48DisplayOps.h's");

// ---- the framebuffer map: which IOFramebuffer pointer is which instance --------------------------------------------------------------------------------------------------------------------
// Filled by adopt from the registry (RDNA4FB for instance 0, the framebuffer child of each published display nub for the others) and READ by the hooks (pipe+0x98 -> instance). A pointer that is
// not in the map is kInstNone: its pipe is never copied and never presented (fail closed).
struct FbMap { uint64_t fb[kInstCount]; };
inline uint32_t inst_of_fb(const FbMap &m, uint64_t fb) {
    if (fb == 0ull) return kInstNone;
    for (uint32_t i = 0; i < kInstCount; ++i) if (m.fb[i] == fb) return i;
    return kInstNone;
}

// ---- adopt: EVERY pipe is checked, not dm+0x88[0] only (the walk's order is not established) ------------------------------------------------------------------------------------------------
constexpr uint32_t kMaxEnt = 4u;
struct PipeEnt {
    bool     have;        // the slot holds a kernel pointer
    bool     nullp;       // the slot holds exactly NULL (found_framebuffer stores a failed init's NULL and still counts it)
    bool     classOurs;   // Navi48DisplayPipe
    bool     traced;      // the aux kext's newDisplayPipe trace named it
    bool     backAccel, backDm;   // pipe+0x88 / +0x90 are the accelerator / the display machine
    bool     fbOk;        // pipe+0x98 is a framebuffer we know (RDNA4FB or a published nub's child)
    uint32_t inst;        // that framebuffer's instance (kInstNone when unknown)
    uint64_t fb;          // pipe+0x98 as read (recorded by record_adopted)
};
struct EntsIn { uint32_t n; PipeEnt e[kMaxEnt]; uint32_t expectMask; };   // n = the display machine's pipe count (0 = the legacy single-pipe probe); expectMask = bit i set for each instance that must have a pipe
constexpr uint32_t expect_mask(bool monb, bool mona) { return 1u | (mona ? (1u << kInstMonA) : 0u) | (monb ? (1u << kInstMonB) : 0u); }
enum EntsV : uint32_t { kEntsOk = 0, kEntsCount = 1, kEntsNull = 2, kEntsNoPipe = 3, kEntsNotOurs = 4, kEntsMismatch = 5, kEntsInst = 6 };
constexpr bool ents_any_null(const EntsIn &x) {
    for (uint32_t i = 0; i < x.n && i < kMaxEnt; ++i) if (x.e[i].nullp) return true;
    return false;
}
constexpr uint32_t ents_verdict(const EntsIn &x) {
    if (x.n == 0u || x.n > kMaxEnt) return kEntsCount;
    uint32_t seen = 0u;
    for (uint32_t i = 0; i < x.n; ++i) {                                  // EVERY pipe, in slot order: a bad pipe 2 is as fatal as a bad pipe 0
        const PipeEnt &p = x.e[i];
        if (p.nullp) return kEntsNull;
        if (!p.have) return kEntsNoPipe;
        if (!p.classOurs || !p.traced) return kEntsNotOurs;
        if (!p.backAccel || !p.backDm || !p.fbOk) return kEntsMismatch;
        if (!inst_valid(p.inst) || (seen & (1u << p.inst)) != 0u) return kEntsInst;      // an unknown framebuffer, or two pipes on one instance
        seen |= 1u << p.inst;
    }
    return seen == x.expectMask ? kEntsOk : kEntsInst;                    // exactly the instances that have a published framebuffer, the DP always
}

// ---- the surface table: IOSurface ID -> instance, learned in the submit hook ---------------------------------------------------------------------------------------------------------------
// 3 surfaces per display are expected (CoreDisplay's triple buffer): 16 entries cover three displays with margin. One writer at a time (the CAS on `busy`); readers take the id word last-published.
//
// 0.0.660 (queue 290 S1): THE TABLE HEALS ITSELF. IOSurface IDs are reused lowest-free (re-s3: alloc_surfaceid's hint, free_surface_handle lowers it), so a surface WindowServer frees on one display and reallocates on
// another comes back under the SAME ID. 0.0.659 turned that into AMBIGUOUS forever (and a full table refused every new ID forever). Now:
//   (a) every entry remembers when it was last seen, per instance: the instance's own submit count (tx[]), the table's event count (sq[]), and `seen` (the latest of them, the LRU key);
//   (b) an ID re-learned on ANOTHER instance replaces the entry when its old owner has gone STALE (below); a genuinely simultaneous two-pipe ID (the old owner is still being seen) stays AMBIGUOUS;
//   (c) an ambiguous entry whose other owners have all gone stale becomes the re-learning instance's (healed);
//   (d) a full table evicts the entry NOT SEEN FOR THE LONGEST TIME instead of refusing;
//   (e) reset_table() empties it (the operator restart window and every arm call it: a new WindowServer start allocates its surfaces afresh).
// STALE (an owner O no longer holds an entry): O's pipe has learned >= kStaleSubmits IDs since it last saw this one, OR the table has seen >= kStaleEvents events since O last saw it.
//   kStaleSubmits = 4 x kMaxSurf = 64: an owner cycles through at most kMaxSurf live IDs (the table cannot hold more), so a live one reappears within 16 of that pipe's submits; x4 is margin for bursty reuse.
//   kStaleEvents = 256: IDs are unique among LIVE surfaces, so an ID that shows up on another pipe is a reused one unless one surface is mirrored on both pipes, and a mirror keeps submitting to its owner. The
//   event clause frees an ID whose owner pipe is IDLE (a static monitor B stops submitting: its own count never advances): ~5 s of the DP's 47 stamps/s, bounded, so a DP that reallocated into a freed monitor B ID is
//   refused (fail closed) for at most ~256 events, never forever.
constexpr uint32_t kMaxSurf = 16u;
constexpr uint32_t kEntAmbiguous = 1u;
constexpr uint32_t kStaleSubmits = 4u * kMaxSurf;
constexpr uint32_t kStaleEvents = 256u;
struct SurfEnt {
    volatile uint32_t id; volatile uint32_t inst; volatile uint32_t flags; uint32_t pad;     // id 0 = a free entry
    volatile uint64_t seen;                                                                   // LRU key: the table's event number at the last sighting (learn or routed present) on ANY instance
    volatile uint32_t tx[kInstCount]; volatile uint32_t mask;                                 // per instance: that pipe's submit count at its last sighting; mask = the instances that have seen this ID
    volatile uint64_t sq[kInstCount];                                                         // per instance: the table's event number at its last sighting
};
enum Learn : uint32_t { kLNew = 0, kLSame = 1, kLBecameAmbiguous = 2, kLWasAmbiguous = 3, kLFull = 4, kLBadId = 5, kLBadInst = 6, kLBusy = 7, kLReplaced = 8, kLEvicted = 9, kLearnCount = 10 };
// kLFull is no longer returned (0.0.660: a full table evicts); the slot is kept so the counters' indices do not move.
struct SurfTable {
    SurfEnt  e[kMaxSurf];
    volatile uint32_t n, busy;
    volatile uint32_t txn[kInstCount];      // learned submits per instance (this pipe's own clock)
    volatile uint64_t seq;                  // the table's event clock
    uint64_t learn[kLearnCount];
    uint64_t resets, touches;
};
// Does this result CHANGE what the table says (the property must be republished)?
constexpr bool learn_changed(uint32_t r) { return r == kLNew || r == kLBecameAmbiguous || r == kLReplaced || r == kLEvicted; }
// Does this result count as a learned submit (statistics)?
constexpr bool learn_counted(uint32_t r) { return r == kLNew || r == kLSame || r == kLBecameAmbiguous || r == kLWasAmbiguous || r == kLReplaced || r == kLEvicted; }
inline uint32_t ent_bit(uint32_t inst) { return 1u << inst; }
// Has instance `o` stopped seeing entry `e`? (see the rule above; `now` = the table's event clock, `tn` = o's submit count)
inline bool owner_stale(const SurfEnt &e, uint32_t o, uint32_t tn, uint64_t now) {
    if (!inst_valid(o) || (__atomic_load_n(&e.mask, __ATOMIC_RELAXED) & ent_bit(o)) == 0u) return true;       // never seen there: nothing to protect
    if ((uint32_t)(tn - __atomic_load_n(&e.tx[o], __ATOMIC_RELAXED)) >= kStaleSubmits) return true;
    return now - __atomic_load_n(&e.sq[o], __ATOMIC_RELAXED) >= kStaleEvents;
}
// Writers hold `busy`. A whole-entry rewrite goes through id 0 (id first, the new id last). That does NOT make a reader's view atomic: a reader that matched the OLD id can read inst / flags of the NEW entry (0.0.661, R4: lookup_surface re-reads the id after the fields and retries / answers unknown).
inline void ent_fill(SurfEnt &e, uint32_t id, uint32_t inst, uint32_t flags, uint32_t tx, uint64_t sq) {
    __atomic_store_n(&e.id, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&e.inst, inst, __ATOMIC_RELAXED);
    __atomic_store_n(&e.flags, flags, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < kInstCount; ++i) { __atomic_store_n(&e.tx[i], 0u, __ATOMIC_RELAXED); __atomic_store_n(&e.sq[i], 0ull, __ATOMIC_RELAXED); }
    __atomic_store_n(&e.tx[inst], tx, __ATOMIC_RELAXED); __atomic_store_n(&e.sq[inst], sq, __ATOMIC_RELAXED);
    __atomic_store_n(&e.mask, ent_bit(inst), __ATOMIC_RELAXED);
    __atomic_store_n(&e.seen, sq, __ATOMIC_RELAXED);
    __atomic_store_n(&e.id, id, __ATOMIC_RELEASE);                    // the id last: a reader that finds it finds its instance
}
inline void ent_mark(SurfEnt &e, uint32_t inst, uint32_t tx, uint64_t sq) {
    __atomic_store_n(&e.tx[inst], tx, __ATOMIC_RELAXED); __atomic_store_n(&e.sq[inst], sq, __ATOMIC_RELAXED);
    __atomic_store_n(&e.mask, __atomic_load_n(&e.mask, __ATOMIC_RELAXED) | ent_bit(inst), __ATOMIC_RELAXED);
    __atomic_store_n(&e.seen, sq, __ATOMIC_RELAXED);
}
inline uint32_t lru_slot(const SurfTable &t) {                         // the entry not seen for the longest time (the lowest `seen`; ties: the lowest index)
    uint32_t best = 0u; uint64_t bs = __atomic_load_n(&t.e[0].seen, __ATOMIC_RELAXED);
    for (uint32_t i = 1; i < kMaxSurf; ++i) { const uint64_t s = __atomic_load_n(&t.e[i].seen, __ATOMIC_RELAXED); if (s < bs) { bs = s; best = i; } }
    return best;
}
// 0.0.661 (R3): the eviction victim of a full table. `seen` only moves on learns (in GPU mode the perform-side touch is dormant), so keying on it evicts a static display's LIVE IDs first once dead IDs pile up.
// The victim is the entry whose OWNER has gone longest without a learned submit: the owner's own distance txn[o] - tx[o] (wrapping), largest first; ties: the lowest `seen`, then the lowest index. An unreadable owner counts as infinitely far.
inline uint32_t victim_slot(const SurfTable &t) {
    uint32_t best = 0u, bd = 0u; uint64_t bs = 0ull; bool have = false;
    for (uint32_t i = 0; i < kMaxSurf; ++i) {
        const SurfEnt &e = t.e[i];
        const uint32_t o = __atomic_load_n(&e.inst, __ATOMIC_RELAXED);
        const uint32_t d = inst_valid(o) ? (uint32_t)(__atomic_load_n(&t.txn[o], __ATOMIC_RELAXED) - __atomic_load_n(&e.tx[o], __ATOMIC_RELAXED)) : 0xFFFFFFFFu;
        const uint64_t sn = __atomic_load_n(&e.seen, __ATOMIC_RELAXED);
        if (!have || d > bd || (d == bd && sn < bs)) { have = true; bd = d; bs = sn; best = i; }
    }
    return best;
}
inline bool acquire_busy(SurfTable &t) { uint32_t exp = 0u; return __atomic_compare_exchange_n(&t.busy, &exp, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }
inline uint32_t learn_surface(SurfTable &t, uint32_t id, uint32_t inst, bool m6flip = false) {   // m6flip (0.0.661): the eviction victim is victim_slot (R3); false = 0.0.660's lru_slot
    if (id == 0u) { __atomic_add_fetch(&t.learn[kLBadId], 1ull, __ATOMIC_RELAXED); return kLBadId; }
    if (!inst_valid(inst)) { __atomic_add_fetch(&t.learn[kLBadInst], 1ull, __ATOMIC_RELAXED); return kLBadInst; }
    if (!acquire_busy(t)) { __atomic_add_fetch(&t.learn[kLBusy], 1ull, __ATOMIC_RELAXED); return kLBusy; }
    const uint32_t tx = __atomic_add_fetch(&t.txn[inst], 1u, __ATOMIC_RELAXED);      // this pipe's own clock (counts this submit)
    const uint64_t sq = __atomic_add_fetch(&t.seq, 1ull, __ATOMIC_RELAXED);          // the table's clock
    uint32_t r = kLNew;
    const uint32_t n = __atomic_load_n(&t.n, __ATOMIC_ACQUIRE);
    uint32_t hit = kMaxSurf;
    for (uint32_t i = 0; i < n && i < kMaxSurf; ++i) if (__atomic_load_n(&t.e[i].id, __ATOMIC_ACQUIRE) == id) { hit = i; break; }
    if (hit < kMaxSurf) {
        SurfEnt &e = t.e[hit];
        const uint32_t owner = __atomic_load_n(&e.inst, __ATOMIC_RELAXED);
        if ((__atomic_load_n(&e.flags, __ATOMIC_RELAXED) & kEntAmbiguous) != 0u) {
            uint32_t live = 0u;                                                       // the OTHER instances still seeing this ID
            for (uint32_t o = 0; o < kInstCount; ++o) if (o != inst && (e.mask & ent_bit(o)) != 0u && !owner_stale(e, o, __atomic_load_n(&t.txn[o], __ATOMIC_RELAXED), sq)) live |= ent_bit(o);
            if (live == 0u) { ent_fill(e, id, inst, 0u, tx, sq); r = kLReplaced; }       // every other owner is gone: the ID is this instance's again
            else { __atomic_store_n(&e.mask, live, __ATOMIC_RELAXED); ent_mark(e, inst, tx, sq); r = kLWasAmbiguous; }
        } else if (owner == inst) { ent_mark(e, inst, tx, sq); r = kLSame; }
        else if (owner_stale(e, owner, __atomic_load_n(&t.txn[owner], __ATOMIC_RELAXED), sq)) { ent_fill(e, id, inst, 0u, tx, sq); r = kLReplaced; }     // the old owner went stale: the ID was freed and reused
        else { ent_mark(e, inst, tx, sq); __atomic_store_n(&e.flags, kEntAmbiguous, __ATOMIC_RELEASE); r = kLBecameAmbiguous; }                          // seen on two LIVE pipes: refused everywhere from now on
    } else if (n < kMaxSurf) {
        ent_fill(t.e[n], id, inst, 0u, tx, sq);
        __atomic_store_n(&t.n, n + 1u, __ATOMIC_RELEASE);
        r = kLNew;
    } else {
        ent_fill(t.e[m6flip ? victim_slot(t) : lru_slot(t)], id, inst, 0u, tx, sq);                          // full: the entry not seen for the longest time makes room
        r = kLEvicted;
    }
    __atomic_store_n(&t.busy, 0u, __ATOMIC_RELEASE);
    __atomic_add_fetch(&t.learn[r], 1ull, __ATOMIC_RELAXED);
    return r;
}
// A routed present of `id` (the DP's pipe, verdict kRouteOk): the ID was seen again NOW. Best effort (a learn in flight wins the CAS and has just seen it itself).
inline bool touch_surface(SurfTable &t, uint32_t id) {
    if (id == 0u || !acquire_busy(t)) return false;
    bool done = false;
    const uint32_t n = __atomic_load_n(&t.n, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n && i < kMaxSurf; ++i) {
        if (__atomic_load_n(&t.e[i].id, __ATOMIC_ACQUIRE) != id) continue;
        const uint32_t o = __atomic_load_n(&t.e[i].inst, __ATOMIC_RELAXED);
        if (inst_valid(o)) { ent_mark(t.e[i], o, __atomic_load_n(&t.txn[o], __ATOMIC_RELAXED), __atomic_load_n(&t.seq, __ATOMIC_RELAXED)); done = true; }
        break;
    }
    __atomic_store_n(&t.busy, 0u, __ATOMIC_RELEASE);
    if (done) t.touches++;
    return done;
}
// EMPTY the table (a new WindowServer start allocates its surfaces afresh). Waits (bounded) for a learn in flight; false = it could not (the caller counts and the next arm / window retries).
inline bool reset_table(SurfTable &t) {
    uint32_t tries = 0u;
    while (!acquire_busy(t)) { if (++tries > (1u << 20)) return false; }
    __atomic_store_n(&t.n, 0u, __ATOMIC_RELEASE);                            // readers see an empty table first, then the entries are cleared
    for (uint32_t i = 0; i < kMaxSurf; ++i) {
        __atomic_store_n(&t.e[i].id, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&t.e[i].inst, kInstNone, __ATOMIC_RELAXED); __atomic_store_n(&t.e[i].flags, 0u, __ATOMIC_RELAXED); __atomic_store_n(&t.e[i].mask, 0u, __ATOMIC_RELAXED); __atomic_store_n(&t.e[i].seen, 0ull, __ATOMIC_RELAXED);
        for (uint32_t k = 0; k < kInstCount; ++k) { __atomic_store_n(&t.e[i].tx[k], 0u, __ATOMIC_RELAXED); __atomic_store_n(&t.e[i].sq[k], 0ull, __ATOMIC_RELAXED); }
    }
    for (uint32_t k = 0; k < kInstCount; ++k) __atomic_store_n(&t.txn[k], 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&t.seq, 0ull, __ATOMIC_RELAXED);
    t.resets++;
    __atomic_store_n(&t.busy, 0u, __ATOMIC_RELEASE);
    return true;
}
struct Look { bool found; bool ambiguous; uint32_t inst; };
// `mid(i, tries)` runs between the field reads and the second read of the id (the host test uses it to play a writer that rewrites the entry at that very instant); the kernel's lookup_surface passes an empty one.
template <class Mid> inline Look lookup_surface_mid(const SurfTable &t, uint32_t id, Mid mid) {
    Look l { false, false, kInstNone };
    if (id == 0u) return l;
    const uint32_t n = __atomic_load_n(&t.n, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n && i < kMaxSurf; ++i) {
        if (__atomic_load_n(&t.e[i].id, __ATOMIC_ACQUIRE) != id) continue;
        // 0.0.661 (R4): the entry is read as inst / flags and the id is then READ AGAIN: a writer rewrites an entry through id 0 (ent_fill), so an id that changed (or went 0) means the fields were torn: retry, a few times,
        // then answer "not found" (UNKNOWN is the fail-safe verdict: the bundle refreshes and retries, never presents on a torn read).
        for (uint32_t tries = 0; tries < 4u; ++tries) {
            const uint32_t in = __atomic_load_n(&t.e[i].inst, __ATOMIC_RELAXED), fl = __atomic_load_n(&t.e[i].flags, __ATOMIC_ACQUIRE);
            mid(i, tries);
            if (__atomic_load_n(&t.e[i].id, __ATOMIC_ACQUIRE) == id) { l.found = true; l.inst = in; l.ambiguous = (fl & kEntAmbiguous) != 0u; return l; }
        }
        return l;
    }
    return l;
}
inline Look lookup_surface(const SurfTable &t, uint32_t id) { return lookup_surface_mid(t, id, [](uint32_t, uint32_t) {}); }
// How many distinct surface IDs the table holds for one instance, and how many are ambiguous (the ambiguous ones are counted under their FIRST instance too).
inline uint32_t count_for_inst(const SurfTable &t, uint32_t inst) {
    uint32_t c = 0; const uint32_t n = __atomic_load_n(&t.n, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n && i < kMaxSurf; ++i) if (t.e[i].id != 0u && t.e[i].inst == inst) c++;
    return c;
}
inline uint32_t count_ambiguous(const SurfTable &t) {
    uint32_t c = 0; const uint32_t n = __atomic_load_n(&t.n, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n && i < kMaxSurf; ++i) if (t.e[i].id != 0u && (t.e[i].flags & kEntAmbiguous) != 0u) c++;
    return c;
}

// ---- THE ROUTING GUARD -------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// A present of surface `id` on the pipe of instance `pipeInst` is allowed only when the table maps that ID to exactly that instance and the ID is not ambiguous. Every other answer is a refusal
// with its own reason (counted by `m6stat`).
enum Route : uint32_t { kRouteOk = 0, kRouteBadPipe = 1, kRouteNoId = 2, kRouteUnknown = 3, kRouteAmbiguous = 4, kRouteWrongInst = 5, kRouteCount = 6 };
constexpr const char *route_name(uint32_t r) {
    return r == kRouteOk ? "routed" : r == kRouteBadPipe ? "the pipe's framebuffer is not a known instance" : r == kRouteNoId ? "the surface has no readable ID" : r == kRouteUnknown ? "the surface ID was never seen on a pipe" :
           r == kRouteAmbiguous ? "the surface ID was seen on two pipes (AMBIGUOUS)" : r == kRouteWrongInst ? "the surface is mapped to another instance" : "unknown";
}
constexpr uint32_t route_verdict(const Look &l, bool haveId, uint32_t pipeInst) {
    if (!inst_valid(pipeInst)) return kRouteBadPipe;
    if (!haveId) return kRouteNoId;
    if (!l.found) return kRouteUnknown;
    if (l.ambiguous) return kRouteAmbiguous;
    if (l.inst != pipeInst) return kRouteWrongInst;
    return kRouteOk;
}

// ---- the layout facts the learn reads (each with its source) -----------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kTxnPipe = 0x28u;       // txn+0x28: the transaction's pipe (study section 3: SUSPECTED until a Stage 1a log shows it equal to the hooked pipe; the learn REFUSES a transaction whose +0x28 is not the hooked pipe)
constexpr uint32_t kSurfId = 0x10u;        // IOSurface+0x10: the surface ID. CONFIRMED (study section 3): __ZNK9IOSurface12getSurfaceIDEv is `movl 0x10(%rdi),%eax` in com.apple.iokit.IOSurface, the build of 11h.1 F2
constexpr char kSurfClass[] = "IOSurface"; // the identity check before the ID is read (class_derives)
enum LearnSkip : uint32_t { kSkNone = 0, kSkTxn = 1, kSkPipeMismatch = 2, kSkPlane = 3, kSkClass = 4, kSkIdRead = 5, kSkCount = 6 };

// ---- the property the bundle reads (study section 3: "the bundle asks for each surface's instance once") ----------------------------------------------------------------------------
// The bundle runs in WindowServer and owns no N48N connection of its own (WindowServer's is exclusive), and the shipped Mesa exports no passthrough: the table goes out as a registry property of
// the Metal nub, the channel "Navi48,Ready" / "Navi48,AutoDisarmed" already use. Layout (little endian): u32 version (1), u32 n, then n x { u32 id, u32 inst, u32 flags }.
#define N48M6_PROP_LATCH   "Navi48,M6"        /* OSNumber 1 on the Metal nub while the latch is ON and a pipe is adopted */
#define N48M6_PROP_SURF    "Navi48,M6Surf"    /* OSData, the table above */
constexpr uint32_t kBlobVersion = 1u, kBlobHdr = 8u, kBlobEnt = 12u, kBlobMax = kBlobHdr + kMaxSurf * kBlobEnt;
inline uint32_t blob_build(const SurfTable &t, uint8_t *out, uint32_t cap) {
    if (!out || cap < kBlobMax) return 0u;
    const uint32_t n = __atomic_load_n(&t.n, __ATOMIC_ACQUIRE) > kMaxSurf ? kMaxSurf : __atomic_load_n(&t.n, __ATOMIC_ACQUIRE);
    auto w32 = [&](uint32_t off, uint32_t v) { out[off] = (uint8_t)v; out[off + 1] = (uint8_t)(v >> 8); out[off + 2] = (uint8_t)(v >> 16); out[off + 3] = (uint8_t)(v >> 24); };
    w32(0, kBlobVersion); w32(4, n);
    for (uint32_t i = 0; i < n; ++i) { w32(kBlobHdr + i * kBlobEnt, t.e[i].id); w32(kBlobHdr + i * kBlobEnt + 4, t.e[i].inst); w32(kBlobHdr + i * kBlobEnt + 8, t.e[i].flags); }
    return kBlobHdr + n * kBlobEnt;
}

// 0.0.661 (R2 / R4, with the navi48-m6flip latch): blob version 2 = u32 version (2), u32 n, u32 GENERATION, then n x { id, inst, flags }. The generation is the PubGate request count the build covers, written INSIDE the build under the
// publisher's gate, so a blob and the generation Status2 reports (the last PUBLISHED one) can never disagree: the bundle keeps a read only when its blob generation >= the cached one. The build takes the table's busy flag (a bounded
// spin) so the entries it copies are not torn by a learn / reset in flight; false = the table stayed busy (the caller republishes on the next request).
constexpr uint32_t kBlobVersion2 = 2u, kBlobHdr2 = 12u, kBlobMax2 = kBlobHdr2 + kMaxSurf * kBlobEnt;
// 0.0.662 (HW1): the kernel's publish buffer holds the LARGEST blob any version builds (0.0.661 sized it kBlobMax = the v1 blob, so blob_build_v2 refused with cap < kBlobMax2 and the property was never written).
constexpr uint32_t kBlobBufBytes = kBlobMax2 > kBlobMax ? kBlobMax2 : kBlobMax;
inline uint32_t blob_build_v2(SurfTable &t, uint32_t gen, uint8_t *out, uint32_t cap) {
    if (!out || cap < kBlobMax2) return 0u;
    uint32_t tries = 0u;
    while (!acquire_busy(t)) { if (++tries > (1u << 16)) return 0u; }
    const uint32_t n = __atomic_load_n(&t.n, __ATOMIC_ACQUIRE) > kMaxSurf ? kMaxSurf : __atomic_load_n(&t.n, __ATOMIC_ACQUIRE);
    auto w32 = [&](uint32_t off, uint32_t v) { out[off] = (uint8_t)v; out[off + 1] = (uint8_t)(v >> 8); out[off + 2] = (uint8_t)(v >> 16); out[off + 3] = (uint8_t)(v >> 24); };
    w32(0, kBlobVersion2); w32(4, n); w32(8, gen);
    for (uint32_t i = 0; i < n; ++i) { w32(kBlobHdr2 + i * kBlobEnt, t.e[i].id); w32(kBlobHdr2 + i * kBlobEnt + 4, t.e[i].inst); w32(kBlobHdr2 + i * kBlobEnt + 8, t.e[i].flags); }
    __atomic_store_n(&t.busy, 0u, __ATOMIC_RELEASE);
    return kBlobHdr2 + n * kBlobEnt;
}
// 0.0.661 (R5): at most kPubPerSec table publishes per second while the table thrashes (an eviction storm); a change inside the gap is DEFERRED (dirty) and flushed by the next change or by Status2's keep-alive, so the LAST state is always published.
constexpr uint32_t kPubPerSec = 20u;
constexpr uint64_t kPubGapNs = 1000000000ull / kPubPerSec;
// ATOMIC (review M1): two pipes' submit hooks, the DP's status call and Status2 all touch it; a lost `dirty` would leave the last change of a burst unpublished.
struct PubRate { volatile uint64_t lastNs; volatile uint32_t dirty; };
// true = publish now (the caller won the slot); false = deferred (dirty set). The winner clears `dirty` BEFORE it builds, so a deferral that arrives after that is flushed later, never lost.
inline bool pub_rate_admit(PubRate &r, uint64_t nowNs) {
    for (;;) {
        uint64_t last = __atomic_load_n(&r.lastNs, __ATOMIC_ACQUIRE);
        if (last != 0ull && nowNs >= last && nowNs - last < kPubGapNs) { __atomic_store_n(&r.dirty, 1u, __ATOMIC_RELEASE); return false; }
        if (__atomic_compare_exchange_n(&r.lastNs, &last, nowNs, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { (void)__atomic_exchange_n(&r.dirty, 0u, __ATOMIC_ACQ_REL); return true; }
    }
}
// a flush is due when something was deferred and the gap has passed (or the clock went backwards)
inline bool pub_rate_flush_due(const PubRate &r, uint64_t nowNs) {
    const uint64_t last = __atomic_load_n(&r.lastNs, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&r.dirty, __ATOMIC_ACQUIRE) != 0u && (nowNs < last || nowNs - last >= kPubGapNs);
}
// The flush: whoever wins the slot publishes (publish() is called at most once per call). Callable from ANY path: the next submit of any pipe, the DP's 1 s status call, Status2.
template <class Publish> inline bool pub_rate_flush(PubRate &r, uint64_t nowNs, Publish publish) {
    if (!pub_rate_flush_due(r, nowNs)) return false;
    uint64_t last = __atomic_load_n(&r.lastNs, __ATOMIC_ACQUIRE);
    if (!__atomic_compare_exchange_n(&r.lastNs, &last, nowNs, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
    (void)__atomic_exchange_n(&r.dirty, 0u, __ATOMIC_ACQ_REL);
    publish();
    return true;
}
// A log line at most once per second (review S-2): true = print it. Atomic like the limiter above.
inline bool log_admit(volatile uint64_t &lastNs, uint64_t nowNs) {
    for (;;) {
        uint64_t last = __atomic_load_n(&lastNs, __ATOMIC_ACQUIRE);
        if (last != 0ull && nowNs >= last && nowNs - last < 1000000000ull) return false;
        if (__atomic_compare_exchange_n(&lastNs, &last, nowNs, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return true;
    }
}

// ---- the perform gate -------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// perform may COPY a plane into the DP console buffer only for the DP's pipe. A pipe on instance 1 / 2 completes without any copy (kPfOtherInst); a pipe whose framebuffer is not a known instance
// completes without a copy too (it is the routing guard's refusal, kPfRouteRefused).
constexpr bool pipe_may_copy(uint32_t pipeInst) { return pipeInst == kInstDp; }

// ---- AGDC: 0x921 / 0x711 answer PER ENDPOINT (study D3) ----------------------------------------------------------------------------------------------------------------------------------------
// IOPresentment issues each call once per IOPresentmentCapabilities object with an endpoint dword already at out+0 (`movl 0x50(%r15),%eax; movl %eax,0x2c(%rbx)` at 0x7ff81599cb72 in
// __GatherAGDCLogicalDeviceCapabilities, CONFIRMED by the study; the 0x711 site is 0x7ff81599cce4). 0.0.660: THE ENDPOINT DWORD IS THE INDEX INTO THE 0x980 LIST - MEASURED on hardware (queue 291, the
// Stage 1a run on 0.0.659: with a 2-entry list [DP, monitor B] IOPresentment's 0x921 / 0x711 calls carried endpoint dwords 0 and 1, notes/logs/runs/m6-s1a/20261006-034405, ep.d). 0.0.659 accepted both the index
// and a mask-bit reading, which answered the DP for BOTH endpoints (harmless while the monitor B's constants equalled the DP's raster; wrong from Stage 2):
//    ep < nfb      -> the framebuffer at list position ep
//    anything else -> position 0 (the DP: 0.0.658's answer for every call), AND counted (AgdcList::nOor / lastOor, m6stat page 3): an endpoint the list does not have is a surprise worth seeing
// With nfb == 1 (no latch) EVERY endpoint answers the DP, exactly 0.0.658.
constexpr uint32_t agdc_position(uint32_t ep, uint32_t nfb) { return (nfb > 1u && ep < nfb) ? ep : 0u; }
constexpr bool agdc_ep_in_range(uint32_t ep, uint32_t nfb) { return ep < nfb; }
// the instance behind a list position (the list is [DP, monitor B?, monitor A?] in the order pipeagdc built it)
constexpr uint32_t agdc_instance(const uint32_t *listInst, uint32_t nfb, uint32_t ep) {
    return (listInst != nullptr && nfb >= 1u && agdc_position(ep, nfb) < nfb) ? listInst[agdc_position(ep, nfb)] : kInstDp;
}

// The 0x921 timing of a non-DP display, from the per-index geometry table of Navi48DisplayOps.h (active size, totals, pixel clock are the table's: N48_DISP_* for the monitor B, N48_DISPA_* for the monitor A). The porches are
// the CTA-861 / RB1 values of those raster totals (2720 x 1481 at 241.5 MHz: front 48 / sync 32, front 3 / sync 5, the dcn41_modes.h row with the same totals; 2200 x 1125 at 148.5 MHz: 1080p60 front 88 / sync 44, front 4 / sync 5).
// SUSPECTED for the monitor B's own EDID: only the ACTIVE size and a non-zero pixel clock are load-bearing for IOPresentment (display_pipe_guard.h, "THE FIX" fields), and the refresh derived from them is 59.95 / 60 Hz.
// false = not a non-DP instance (the DP's timing is the live-raster path in DisplayPipeGuard.cpp, untouched).
inline bool agdc_timing_for_inst(uint32_t inst, n48_agdc_timing *t) {
    if (!t) return false;
    N48DispGeom g;
    if (inst == kInstDp || !n48disp_geom_for_index(index_of_inst(inst), &g)) return false;
    const bool monb = inst == kInstMonB;
    const uint32_t hTotal = monb ? (uint32_t)N48_DISP_HTOTAL : (uint32_t)N48_DISPA_HTOTAL, vTotal = monb ? (uint32_t)N48_DISP_VTOTAL : (uint32_t)N48_DISPA_VTOTAL;
    t->w = g.w; t->h = g.h; t->h_blank = hTotal - g.w; t->v_blank = vTotal - g.h;
    t->h_sync_off = monb ? 48u : 88u; t->h_sync_w = monb ? 32u : 44u; t->v_sync_off = monb ? 3u : 4u; t->v_sync_w = 5u;
    t->pixel_clock = g.pixHz; t->live = 0u;
    return true;
}

// ---- the AGDC service's M6 state and answers (DisplayPipeGuard.cpp keeps ONE instance and calls agdc_answer; tests/native_disp_test.cpp runs the SAME code over a buffer) ----------------------------------------------
// fb[i] / inst[i] describe list entry i of the 0x980 reply: the DP first, then the monitor B, then the monitor A, as many as are published (nfb <= 3 <= N48_AGDC_MAX_FB). nfb <= 1 means "no M6 list": agdc_answer is not asked.
struct AgdcList {
    uint32_t nfb;
    uint32_t inst[kInstCount];
    uint64_t fb[kInstCount];
    uint32_t lastEp, lastNfb;      // statistics (m6stat page 1): the last endpoint dword seen and the nfb of the last 0x980 reply
    uint64_t n921, n711;           // replies for a NON-DP endpoint
    uint64_t nOor, lastOor;        // 0.0.660: 0x921 / 0x711 calls whose endpoint dword is outside the list (answered as the DP), and the last such dword
    uint64_t n921i[kInstCount], n711i[kInstCount];   // 0.0.662 (Stage 2, item 10): the 0x921 / 0x711 replies PER INSTANCE (index 0 = the DP's, which agdc_answer leaves to 0.0.658's code but counts here); m6stat page 4
    uint32_t epSeen;               // 0.0.662: bit e = an endpoint dword e (< 31) arrived with a 0x921 / 0x711; bit 31 = any larger one. K7 wants exactly {0, 1, 2} on a three-entry list
};
constexpr uint32_t agdc_ep_bit(uint32_t ep) { return ep < 31u ? (1u << ep) : (1u << 31); }
// true = answered here (*kr is the IOReturn: 0 or kIOReturnBadArgument); false = the DP's endpoint (or a command this list does not own): the caller runs 0.0.658's code unchanged.
// The endpoint dword is read from out+0 BEFORE any fill zeroes the buffer (IOPresentment calls with in == out).
inline bool agdc_answer(AgdcList &g, uint32_t cmd, uint8_t *out, size_t len, uint64_t pci, uint32_t *kr) {
    if (g.nfb <= 1u || g.nfb > kInstCount || !out || !kr) return false;
    if (cmd == N48_AGDC_CMD_GPU_CAPABILITY) {
        g.lastNfb = g.nfb;
        *kr = (pci != 0ull && !n48_agdc_fill_gpu_capability(out, len, pci, g.fb, g.nfb)) ? 0u : 0xe00002c2u;
        return true;
    }
    if (cmd != N48_AGDC_CMD_LINK_CONFIG && cmd != N48_AGDC_CMD_PIPELINE_CAPS) return false;
    if (len < 4u) return false;
    const uint32_t ep = n48_agdc_rd32(out, 0u);
    g.lastEp = ep;
    if (!agdc_ep_in_range(ep, g.nfb)) { g.nOor++; g.lastOor = ep; }       // 0.0.660: an endpoint the 0x980 list does not have: the DP answers it (position 0), and it is counted
    const uint32_t inst = agdc_instance(g.inst, g.nfb, ep);
    g.epSeen |= agdc_ep_bit(ep);                                         // 0.0.662 (item 10): counters only - no reply byte depends on them
    if (inst < kInstCount) { if (cmd == N48_AGDC_CMD_LINK_CONFIG) g.n921i[inst]++; else g.n711i[inst]++; }
    if (inst == kInstDp) return false;                                  // the DP: 0.0.658's live-raster answers, untouched
    if (cmd == N48_AGDC_CMD_LINK_CONFIG) {
        n48_agdc_timing t {};
        g.n921++;
        *kr = (agdc_timing_for_inst(inst, &t) && !n48_agdc_fill_link_config_t(out, len, &t)) ? 0u : 0xe00002c2u;
    } else {
        N48DispGeom geo;
        g.n711++;
        *kr = (n48disp_geom_for_index(index_of_inst(inst), &geo) && !n48_agdc_fill_pipeline_caps(out, len, geo.w, geo.h)) ? 0u : 0xe00002c2u;
    }
    return true;
}

// ---- 0.0.660 (queue 290 S2): the table property is published by ONE publisher at a time, and the LAST publish carries the newest table ---------------------------------------------------------------------------
// Two pipes' submit hooks can both change the table (a learn, a reset) and both publish. 0.0.659 built the ~200-byte blob on the hook's stack and wrote it with no ordering, so an older table could be the last
// property written. publish_serialised: every caller first bumps `gen`; only the caller that wins `busy` publishes, and it builds the blob (from the table AS IT IS THEN) again whenever `gen` moved since that build began
// (checked AFTER releasing `busy`; a caller that lost the CAS has already bumped `gen` before it tried, so the owner cannot miss it). A caller that loses returns at once (its change is carried by the owner's
// next build): the hook never blocks. Returns how many publishes THIS caller made (0 = coalesced into the owner's).
struct PubGate { volatile uint32_t gen, busy; uint64_t published, coalesced; volatile uint32_t curGen, pubGen; };   // 0.0.661 (R2): curGen = the request count the build in flight covers (set under the gate); pubGen = the last PUBLISHED generation (what Status2 reports)
// `Seam` runs once per round, after the owner's publish and BEFORE it lets go of the gate: the host test uses it to play "another pipe's hook bumped the generation and lost the CAS" at the one instant the
// owner's post-release check exists for. The kernel passes an empty one.
template <class Build, class Publish, class Seam> inline uint32_t publish_serialised_seam(PubGate &g, Build build, Publish publish, Seam seam) {
    __atomic_add_fetch(&g.gen, 1u, __ATOMIC_SEQ_CST);                           // BEFORE the CAS: a caller that loses has already told the owner
    uint32_t done = 0u;
    for (;;) {
        uint32_t exp = 0u;
        if (!__atomic_compare_exchange_n(&g.busy, &exp, 1u, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) { __atomic_add_fetch(&g.coalesced, 1ull, __ATOMIC_RELAXED); return done; }
        const uint32_t seen = __atomic_load_n(&g.gen, __ATOMIC_SEQ_CST);        // every request counted so far is covered by the build below
        __atomic_store_n(&g.curGen, seen, __ATOMIC_SEQ_CST);                    // 0.0.661 (R2): the build reads it (blob version 2 carries it)
        const uint32_t n = build();
        if (n != 0u && publish(n)) { g.published++; done++; __atomic_store_n(&g.pubGen, seen, __ATOMIC_SEQ_CST); }
        seam();
        __atomic_store_n(&g.busy, 0u, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&g.gen, __ATOMIC_SEQ_CST) == seen) return done;      // nobody asked since the build started; otherwise (a request arrived while we published, or while we let go) go round again
    }
}
template <class Build, class Publish> inline uint32_t publish_serialised(PubGate &g, Build build, Publish publish) { return publish_serialised_seam(g, build, publish, []() {}); }

// ---- 0.0.660 (queue 290 N2): m6stat packs several counters into one 64-bit word; each field SATURATES instead of spilling into its neighbour ----------------------------------------------------------------
constexpr uint64_t sat_bits(uint64_t v, uint32_t bits) { return (bits >= 64u || v < (1ull << bits)) ? v : ((1ull << bits) - 1ull); }
inline uint64_t pack_sat(const uint64_t *v, const uint8_t *bits, uint32_t n) {    // fields from bit 0 upward, in order
    uint64_t out = 0ull; uint32_t sh = 0u;
    for (uint32_t i = 0; i < n && sh < 64u; ++i) { out |= sat_bits(v[i], bits[i]) << sh; sh += bits[i]; }
    return out;
}
constexpr uint32_t kStatPages = 5u;                                    // m6stat pages: 0 pipes, 1 stamps / guard reasons / AGDC, 2 the IDs, 3 (0.0.660) the pipes' clocks and the self-healing counters, 4 (0.0.662) the AGDC replies per instance and the endpoint values seen
constexpr uint8_t kPackRoute[4]     = { 16, 16, 16, 16 };               // page 1 out[11]: unknown | ambiguous << 16 | wrong-instance << 32 | no-id << 48
constexpr uint8_t kPackLearnSkip[8] = { 8, 8, 8, 8, 8, 8, 8, 8 };       // page 2 out[12]: txn | pipe mismatch | plane | class | id read | evicted | bad instance | busy (8 bits each)
constexpr uint8_t kPackResetTouch[2] = { 16, 48 };              // page 3 out[10]: table resets | routed-present sightings << 16
constexpr uint8_t kPackPair32[2]    = { 32, 32 };                   // page 3 out[11] (writes | coalesced << 32) and out[12] (AGDC out-of-range endpoints | the last one << 32)
constexpr uint8_t kPackAgdc[4]      = { 8, 16, 16, 24 };                // page 1 out[12]: nfb | 0x921 non-DP << 8 | 0x711 non-DP << 24 | last endpoint dword << 40

// ---- 0.0.660 (queue 290 K1): how long ago a pipe's last transaction was (m6stat page 3) ------------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kAgeNever = ~0ull;                                   // the pipe has not submitted since the boot
constexpr uint64_t txn_age_ms(uint64_t nowNs, uint64_t lastNs) { return lastNs == 0ull ? kAgeNever : (nowNs <= lastNs ? 0ull : (nowNs - lastNs) / 1000000ull); }

// ---- 0.0.660 (queue 290 S5): the restart-loop guard counts WindowServer STARTS (slot 267, once per display pipe) -------------------------------------------------------------------------------------------
// FAIL CLOSED: with the latch ON only the pipes KNOWN to be the monitor A's or the monitor B's are exempt; the DP's AND a pipe on no known framebuffer (kInstNone: a start the map cannot place) are counted, so a
// WindowServer crash loop can never hide behind an unplaceable pipe. With the latch OFF every pipe is counted (0.0.658).
constexpr bool restart_guard_counts(bool m6, uint32_t pipeInst) { return !m6 || (pipeInst != kInstMonA && pipeInst != kInstMonB); }

// ---- 0.0.660 (queue 290 T1): the glue decisions that the 0.0.659 tests could not reach, as pure functions -----------------------------------------------------------------------------------------------------
// (1) pipeagdc's collect: the DP plus the framebuffer of EVERY published display nub, the monitor B (index 1) before the monitor A (index 2). A published nub whose framebuffer has not started is a REFUSAL (the list
//     would disagree with the display machine's pipes), never a shorter list and never a skipped entry. DisplayPipeGuard.cpp's collect_m6 runs collect_flow over its own environment.
//     The loops count DOWN over unsigned instances: they stop only because kInstMonA >= 1 (a kInstMonA of 0 would wrap and never end).
static_assert(kInstMonA >= 1u && kInstMonB > kInstMonA, "the downward instance loops (monitor B, then monitor A) end because kInstMonA >= 1");
enum Collect : uint32_t { kColSkip = 0, kColUse = 1, kColRefuse = 2 };
constexpr uint32_t collect_verdict(bool nubPublished, bool fbStarted) { return !nubPublished ? kColSkip : fbStarted ? kColUse : kColRefuse; }
template <class E> inline bool collect_flow(E &e, uint32_t *count) {          // e.nub_published(inst), e.fb_started(inst) (looks the framebuffer up and keeps it), e.refuse(inst)
    uint32_t n = 1u;                                                          // the DP
    for (uint32_t inst = kInstMonB; inst >= kInstMonA; --inst) {
        const bool pub = e.nub_published(inst);
        const bool started = pub && e.fb_started(inst);
        const uint32_t v = collect_verdict(pub, started);
        if (v == kColRefuse) { e.refuse(inst); return false; }
        if (v == kColUse) n++;
    }
    *count = n;
    return true;
}
// (2) the order of the 0x980 list: the DP, then the monitor B, then the monitor A, as many as have a framebuffer. Returns the entry count.
inline uint32_t list_insts(const bool have[kInstCount], uint32_t *instOut) {
    uint32_t k = 0u;
    instOut[k++] = kInstDp;
    for (uint32_t inst = kInstMonB; inst >= kInstMonA; --inst) if (have[inst]) instOut[k++] = inst;
    return k;
}
// (3) the raster of a non-DP OTG is accepted only when it is LIT with exactly the display's active size (navi48_dcn.cpp vbl_refresh_inst runs raster_flow over the DCN accessors).
constexpr bool raster_accepted(bool lit, uint32_t aw, uint32_t ah, uint32_t w, uint32_t h) { return lit && w != 0u && h != 0u && aw == w && ah == h; }
template <class E> inline bool raster_flow(E &e, uint32_t otg, uint32_t w, uint32_t h, uint32_t *hTot, uint32_t *vTot) {     // e.active_size(otg, &lit, &aw, &ah), e.totals(otg, &ht1, &vt1): true = the accessor read it
    bool lit = false; uint32_t aw = 0u, ah = 0u;
    if (!e.active_size(otg, &lit, &aw, &ah) || !raster_accepted(lit, aw, ah, w, h)) return false;
    uint32_t ht1 = 0u, vt1 = 0u;
    if (!e.totals(otg, &ht1, &vt1)) return false;
    *hTot = ht1 + 1u; *vTot = vt1 + 1u;
    return true;
}

} // namespace n48m6
