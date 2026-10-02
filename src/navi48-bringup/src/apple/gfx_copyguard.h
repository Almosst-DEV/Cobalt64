// gfx_copyguard.h — 0.0.435 (notes/design/PGMID-COPYGUARD.md Part 2). THE COPY-OVERLAP REFUSAL, PURE HALF.
//
// VERIFIED the hazard live: inside one decide37 2-IB pass the copier rewrote VA 0x4000b0000 (VRAM
// [0x10010000,0x10018000)) TWICE while the policy pass could be reading the same range, `residency_copy_to_vram`
// returns true even when the read-back finds mismatches, and a FAILED copy leaves the destination half-written for
// good. This header is the whole decision's PURE half — every function takes only the scalars/state the caller
// already has, touches no register, no page table and nothing of Apple's, and is compiled BOTH into the kext and
// into tests/gfx_copyguard_test.cpp so the two can never disagree about the arithmetic (the gfx_mmprio.h / ws_resprov.h
// convention). Storage (the slot table, the ring, the poison table, the page recorder, the live counters) and the
// RAII `Navi48CopyScope` that drives OPEN/BEGIN/END/CLOSE live in Navi48Bringup.cpp (declared in Navi48Ttl.hpp);
// this header only says what those calls DO once handed the state.
//
// THE WRITER SIDE (one residency copy): OPEN claims a slot {lo, hi}, publishes it (seq odd), then pushes a BEGIN
// event into a 64-entry ring built exactly like ws_resprov.h's clear log (stamp written LAST, a seqlock per slot).
// WA/WB are the copy's own MM writes (Navi48AccelPeer.cpp navi48_vram_write_mm calls), unchanged by this header.
// CLOSE pushes END, poisons the range on FAILED / a read-back mismatch / a partial write, THEN publishes the slot
// closed (seq even). The ORDER (END before "seq even") is load-bearing: see n48_cg_check's comment below for the
// race it closes. If no slot is free the copy still runs — UNTRACKED — and every check refuses while any untracked
// copy could still be in flight (fail-closed: there is no way to know when an untracked copy ends).
//
// THE READER SIDE (once per segment, gfxsrc_policy/AppleHardwareHook.cpp, after `xlat12_ib_translate_draw_ex`):
// a page recorder (n48_cg_pagerec) is reset and the ring position marked BEFORE the translate call; gfxc_read_rs
// (AppleHardwareHook.cpp) feeds it every VRAM page the descriptor-read and program-identity callbacks touch, and
// ONLY those — every other reader (gfxc_read itself, IB fetch, etc.) is untouched. AFTER the translate call,
// n48_cg_check runs the checks in the BINDING order the design fixes: a SEQ_CST fence; the poison table; the slot
// table; ONLY THEN the ring position; the ring. A segment that fails is refused with the kext-only
// N48_SEG_COPY_OVERLAP status (never one of xlat12's own), the existing restore puts Apple's bytes back, the LUT
// learn is skipped and gXpAcc.over is set for that segment (all at the call site, not here).
//
// ALWAYS ON. It never reads gMmPrioOn/the switch-37 state: a copier paused mid-copy under 37 is still caught (its
// slot is still odd — IN_FLIGHT); with 37 off, true interleaving is caught by the ring (EVENT). It covers our own
// CPU writers only — `navi48_vram_write_mm` itself pushes a WRITE event for any write NOT covered by an open scope
// while a pass is active (Navi48Bringup.cpp), so a future write_mm caller that forgets to open a scope is still
// caught, not silently trusted.
//
// Plain C that also compiles as C++ (the xlat12/gfx_mmprio/ws_resprov convention); no heap, no floating point, no
// libc beyond stdint, __atomic builtins only (available to both the kext, built -fno-exceptions -fno-rtti, and a
// plain host g++/clang++ test binary).
#ifndef N48_GFX_COPYGUARD_H
#define N48_GFX_COPYGUARD_H

#include <stdint.h>
#include "xlat12.h"     // XLAT12_OK / XLAT12_ERR_* — N48_SEG_COPY_OVERLAP must be distinct from every one of these
#include "xlat12_ib.h"  // XLAT12_IB_ERR_* — same requirement

// ---- capacities -----------------------------------------------------------------------------------------------
#define N48_CG_SLOTS     16u   // concurrent open residency-copy scopes this boot can track
// 0.0.437 — raised from 64: ONE GUARD WINDOW PER FRAME means the ring window a segment's check
// scans now spans every event since the PASS began (navi48_cg_seg_begin is called once, before the segment loop),
// not just since that one segment's own translate - so a multi-segment pass needs more headroom before WRAP.
#define N48_CG_RING      256u  // the design's original figure (64) x4 for the whole-pass window, /item 4
#define N48_CG_POISON    16u   // distinct poisoned rows tracked before they must be merged (never silently dropped)
// 0.0.437 — raised from 32 for the same reason: the recorder now accumulates every page read
// since the PASS began, across every segment, not just the one segment being checked.
#define N48_CG_REC_PAGES 128u  // distinct 4 KiB VRAM pages the pass's descriptor+identity reads can name before
                                // the recorder overflows and the segment is refused rather than trusted incomplete

// ---- the kext-only refusal status ------------------------------------------------------------------------------
// Chosen far outside xlat12_status (0..10) and xlat12_ib.h's XLAT12_IB_ERR_* (20..29) so no future addition to
// either enum can collide with it by growing into this value; the static_asserts below make that a compile-time
// fact, not a hope.
enum { N48_SEG_COPY_OVERLAP = 0x4000 };

#ifdef __cplusplus
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_OK,                  "N48_SEG_COPY_OVERLAP collides with an XLAT12 status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_ERR_ARG,              "N48_SEG_COPY_OVERLAP collides with an XLAT12 status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_ERR_BAD_PACKET,       "N48_SEG_COPY_OVERLAP collides with an XLAT12 status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_ERR_TRUNCATED,        "N48_SEG_COPY_OVERLAP collides with an XLAT12 status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_ERR_RANGE,            "N48_SEG_COPY_OVERLAP collides with an XLAT12 status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_ERR_BAD_INDEX,        "N48_SEG_COPY_OVERLAP collides with an XLAT12 status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_ERR_UNKNOWN_REG,      "N48_SEG_COPY_OVERLAP collides with an XLAT12 status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_ERR_LEGACY_VS,        "N48_SEG_COPY_OVERLAP collides with an XLAT12 status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_ERR_VS_MODE_UNKNOWN,  "N48_SEG_COPY_OVERLAP collides with an XLAT12 status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_ERR_MEMLOADED,        "N48_SEG_COPY_OVERLAP collides with an XLAT12 status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_ERR_CAPACITY,         "N48_SEG_COPY_OVERLAP collides with an XLAT12 status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_IB_ERR_UNLISTED,      "N48_SEG_COPY_OVERLAP collides with an XLAT12_IB status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_IB_ERR_REG_OPERAND,   "N48_SEG_COPY_OVERLAP collides with an XLAT12_IB status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_IB_ERR_COND_EXEC,     "N48_SEG_COPY_OVERLAP collides with an XLAT12_IB status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_IB_ERR_TOO_LONG,      "N48_SEG_COPY_OVERLAP collides with an XLAT12_IB status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_IB_ERR_DRAW_SHAPE,    "N48_SEG_COPY_OVERLAP collides with an XLAT12_IB status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_IB_ERR_VERIFY,        "N48_SEG_COPY_OVERLAP collides with an XLAT12_IB status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_IB_ERR_RING,          "N48_SEG_COPY_OVERLAP collides with an XLAT12_IB status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_IB_ERR_PAIR,          "N48_SEG_COPY_OVERLAP collides with an XLAT12_IB status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_IB_ERR_INTERP,        "N48_SEG_COPY_OVERLAP collides with an XLAT12_IB status");
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_ERR_SCISSOR,          "N48_SEG_COPY_OVERLAP collides with an XLAT12 status");   // 0.0.499
static_assert(N48_SEG_COPY_OVERLAP != XLAT12_IB_ERR_DESC,          "N48_SEG_COPY_OVERLAP collides with an XLAT12_IB status");
#endif

// ---- overlap, the one primitive everything else is built from ---------------------------------------------------
static inline int n48_cg_overlap(uint64_t lo1, uint64_t hi1, uint64_t lo2, uint64_t hi2) {
    return lo1 < hi2 && lo2 < hi1;
}

// ---- the slot table (writer: OPEN/CLOSE; reader: slot scan) -----------------------------------------------------
// `claimed` gates exclusive access to lo/hi while a slot transitions from free to open (CAS-guarded); `seq` is the
// PUBLISHED state — odd means open with lo/hi valid and stable, even means closed. Because `claimed` is set before
// lo/hi are written and `seq` goes odd only AFTER lo/hi are written, any reader that observes `seq` odd is
// guaranteed (every field here is SEQ_CST) to see the matching lo/hi, not a stale pair from a previous occupant.
// 0.0.438: `owner` names the thread whose OPEN published this slot - current_thread() in the kext
// (navi48_cg_open, Navi48Bringup.cpp), an injectable scalar here so a host test can model two threads without any
// real concurrency. It is written before `seq` goes odd, under the same `claimed` CAS as lo/hi, so the seqlock
// bracket that already guards lo/hi against a torn read covers it too - a reader that sees `seq` stable sees the
// matching owner, never a stale one from a previous occupant.
// build 0.0.529 (notes/design/CG84.md item 8): `dlo`/`dhi`, the would-be DELTA of a residency copy (switch 84 SHADOW), or the
// delta itself (ON: then equal to lo/hi). INSTRUMENT ONLY: written with lo/hi, before `seq` goes odd; read only by the switch-84
// SHADOW instrument check (n48_cg_check_fx with `used` 1). 0/0 = none (every writer but a switch-84 residency copy).
typedef struct {
    uint32_t claimed;
    uint64_t seq;
    uint64_t lo, hi;
    uintptr_t owner;
    uint64_t dlo, dhi;
} n48_cg_slot;

static inline void n48_cg_slot_init(n48_cg_slot *slots, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        slots[i].claimed = 0u; slots[i].seq = 0ull; slots[i].lo = 0ull; slots[i].hi = 0ull; slots[i].owner = 0u;
        slots[i].dlo = 0ull; slots[i].dhi = 0ull;
    }
}

// Claim a free slot and publish it OPEN over [lo, hi), owned by `owner`. Returns the slot index, or -1 if every
// slot is busy (UNTRACKED — the caller still performs the copy; see the header comment).
// build 0.0.529: the open with the switch-84 instrument range (dlo/dhi, written before `seq` goes odd, like lo/hi).
static inline int32_t n48_cg_slot_open_d(n48_cg_slot *slots, uint32_t n, uint64_t lo, uint64_t hi, uintptr_t owner,
                                         uint64_t dlo, uint64_t dhi) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t exp = 0u;
        if (!__atomic_compare_exchange_n(&slots[i].claimed, &exp, 1u, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            continue;
        __atomic_store_n(&slots[i].lo, lo, __ATOMIC_SEQ_CST);
        __atomic_store_n(&slots[i].hi, hi, __ATOMIC_SEQ_CST);
        __atomic_store_n(&slots[i].owner, owner, __ATOMIC_SEQ_CST);
        __atomic_store_n(&slots[i].dlo, dlo, __ATOMIC_SEQ_CST);
        __atomic_store_n(&slots[i].dhi, dhi, __ATOMIC_SEQ_CST);
        const uint64_t s = __atomic_load_n(&slots[i].seq, __ATOMIC_SEQ_CST) + 1ull;   // -> odd: publish OPEN
        __atomic_store_n(&slots[i].seq, s, __ATOMIC_SEQ_CST);
        return (int32_t)i;
    }
    return -1;
}
static inline int32_t n48_cg_slot_open(n48_cg_slot *slots, uint32_t n, uint64_t lo, uint64_t hi, uintptr_t owner) {
    return n48_cg_slot_open_d(slots, n, lo, hi, owner, 0ull, 0ull);
}

// Publish a slot CLOSED (seq -> even) and free it for reuse. The caller (Navi48CopyScope::~Navi48CopyScope) must
// have already pushed the ring's END event for this slot — see n48_cg_check's comment for why that order matters.
// idx < 0 (an UNTRACKED copy) is a no-op: there was never a slot to release.
static inline void n48_cg_slot_close(n48_cg_slot *slots, int32_t idx) {
    if (idx < 0) return;
    const uint64_t s = __atomic_load_n(&slots[idx].seq, __ATOMIC_SEQ_CST) + 1ull;     // -> even: publish CLOSED
    __atomic_store_n(&slots[idx].seq, s, __ATOMIC_SEQ_CST);
    __atomic_store_n(&slots[idx].claimed, 0u, __ATOMIC_SEQ_CST);
}

enum { N48_CG_SLOT_NONE = 0, N48_CG_SLOT_INFLIGHT = 1, N48_CG_SLOT_TORN = 2 };

// One slot's seqlock read: s1/s2 bracket the lo/hi read so a slot opening or closing mid-read is TORN, never a
// guess. TORN and INFLIGHT are both refusals at the call site; NONE means this slot said nothing about [lo, hi).
static inline int n48_cg_slot_scan_one(const n48_cg_slot *slots, uint32_t i, uint64_t lo, uint64_t hi) {
    const uint64_t s1 = __atomic_load_n(&slots[i].seq, __ATOMIC_SEQ_CST);
    const uint64_t l  = __atomic_load_n(&slots[i].lo,  __ATOMIC_SEQ_CST);
    const uint64_t h  = __atomic_load_n(&slots[i].hi,  __ATOMIC_SEQ_CST);
    const uint64_t s2 = __atomic_load_n(&slots[i].seq, __ATOMIC_SEQ_CST);
    if (s1 != s2) return N48_CG_SLOT_TORN;
    if ((s1 & 1ull) && n48_cg_overlap(l, h, lo, hi)) return N48_CG_SLOT_INFLIGHT;
    return N48_CG_SLOT_NONE;
}
static inline int n48_cg_slot_scan(const n48_cg_slot *slots, uint32_t n, uint64_t lo, uint64_t hi) {
    for (uint32_t i = 0; i < n; i++) {
        const int r = n48_cg_slot_scan_one(slots, i, lo, hi);
        if (r != N48_CG_SLOT_NONE) return r;
    }
    return N48_CG_SLOT_NONE;
}

// 0.0.437 — CONTAINMENT, THE PURE PRIMITIVE navi48_vram_write_mm'S SELF-SCOPING DECISION NEEDS.
// NOT overlap: a write only half inside an open slot must not be treated as covered by it, or the sliver outside
// the slot would land with no BEGIN/END trail at all (the exact hole found: "an untracked/unscoped write
// leaves no scope"). Is [lo2, hi2) FULLY inside [lo1, hi1)?
static inline int n48_cg_contains(uint64_t lo1, uint64_t hi1, uint64_t lo2, uint64_t hi2) {
    return lo1 <= lo2 && hi2 <= hi1;
}
// Is [lo, hi) fully contained in ANY single currently-open slot OWNED BY `owner`? Same seqlock bracket as
// n48_cg_slot_scan_one, so a slot opening or closing mid-read can never be mistaken for containment; a torn read
// answers NOT CONTAINED — the caller then self-scopes, which only costs extra bookkeeping, never a missed guard
// (fail-safe, not fail-open).
// 0.0.438: a write is only "already scoped" when the OPEN slot covering it belongs to the
// SAME thread that is about to do the write - a slot opened by another thread's residency copy (or, on hardware,
// the switch-39 phantom scope) can close and release at any point this caller does not control, and a write whose
// self-scoping depended on someone else's still-open slot could then land with no scope of its own at all the
// instant that slot closes first. `owner` is read inside the same seqlock bracket as lo/hi, so a torn read (the
// slot changing hands mid-check) answers NOT CONTAINED, never a stale owner match.
static inline int n48_cg_slot_contains_one(const n48_cg_slot *slots, uint32_t i, uint64_t lo, uint64_t hi, uintptr_t owner) {
    const uint64_t s1 = __atomic_load_n(&slots[i].seq, __ATOMIC_SEQ_CST);
    const uint64_t l  = __atomic_load_n(&slots[i].lo,  __ATOMIC_SEQ_CST);
    const uint64_t h  = __atomic_load_n(&slots[i].hi,  __ATOMIC_SEQ_CST);
    const uintptr_t o = __atomic_load_n(&slots[i].owner, __ATOMIC_SEQ_CST);
    const uint64_t s2 = __atomic_load_n(&slots[i].seq, __ATOMIC_SEQ_CST);
    if (s1 != s2) return 0;
    return (s1 & 1ull) && o == owner && n48_cg_contains(l, h, lo, hi);
}
static inline int n48_cg_slot_contains(const n48_cg_slot *slots, uint32_t n, uint64_t lo, uint64_t hi, uintptr_t owner) {
    for (uint32_t i = 0; i < n; i++)
        if (n48_cg_slot_contains_one(slots, i, lo, hi, owner)) return 1;
    return 0;
}

// 0.0.435 review fix — THE LIVE-OPEN COUNT, PURE. Scopes open RIGHT NOW: every slot whose seq is currently odd,
// plus `untracked` (a copy that could not claim a slot still counts as open until its own close). This is what the
// `cguard:` report's "in flight" field must be, computed FRESH at snapshot time (navi48_cg_snapshot,
// Navi48Bringup.cpp) — never an accumulated per-refusal counter, which double-counts against `refused[IN_FLIGHT]`
// and cannot fall back to 0 once every scope has closed. The design's pass rule ("opened == closed + in flight")
// only holds against this live count.
static inline uint32_t n48_cg_live_open(const n48_cg_slot *slots, uint32_t n, uint32_t untracked) {
    uint32_t live = untracked;
    for (uint32_t i = 0; i < n; i++)
        if (__atomic_load_n(&slots[i].seq, __ATOMIC_SEQ_CST) & 1ull) live++;
    return live;
}

// ---- the ring (BEGIN/END from the writer, WRITE from an unscoped navi48_vram_write_mm) --------------------------
// Modelled directly on ws_resprov.h's n48_rp_clr_push/n48_rp_clr_clean: a fixed ring, `next` the count published so
// far (event n lives in slot n % N48_CG_RING), each slot a seqlock with `stamp` written LAST.
enum { N48_CG_EV_BEGIN = 1u, N48_CG_EV_END = 2u, N48_CG_EV_WRITE = 3u };
typedef struct {
    uint64_t stamp;   // the event's number + 1, published LAST. 0 = being written / never written.
    uint64_t lo, hi;
    uint32_t kind;
    uint32_t slot;    // which slot this BEGIN/END names; ~0u for an unscoped WRITE event
    // build 0.0.523 (RING-NEUTER-FORGIVE.md item 8): the pushing thread (current_thread() in the kext; 0 through the
    // plain n48_cg_ring_push). Written BEFORE the stamp, like every other field, so a reader that sees the stamp sees it.
    // An INSTRUMENT: only the per-refusal line reads it; no check decides on it.
    uintptr_t owner;
    // build 0.0.529 (CG84.md item 8): the switch-84 instrument range (see n48_cg_slot), written BEFORE the stamp.
    uint64_t dlo, dhi;
} n48_cg_ev;
typedef struct {
    n48_cg_ev s[N48_CG_RING];
    uint64_t next;
} n48_cg_ring;

static inline void n48_cg_ring_init(n48_cg_ring *g) {
    g->next = 0ull;
    for (uint32_t i = 0; i < N48_CG_RING; i++) {
        g->s[i].stamp = 0ull; g->s[i].lo = g->s[i].hi = 0ull; g->s[i].kind = 0u; g->s[i].slot = 0u; g->s[i].owner = 0u;
        g->s[i].dlo = g->s[i].dhi = 0ull;
    }
}

// build 0.0.523: the push with the pushing thread's identity (navi48_cg_open/close pass current_thread()). `owner` is
// written with the other fields, BEFORE the stamp is published; n48_cg_ring_push is this with owner 0.
// build 0.0.529: + the switch-84 instrument range, written with the other fields BEFORE the stamp.
static inline uint64_t n48_cg_ring_push_od(n48_cg_ring *g, uint32_t kind, uint32_t slot, uint64_t lo, uint64_t hi,
                                           uintptr_t owner, uint64_t dlo, uint64_t dhi) {
    const uint64_t n = __atomic_fetch_add(&g->next, 1ull, __ATOMIC_SEQ_CST);
    n48_cg_ev *d = &g->s[n % N48_CG_RING];
    __atomic_store_n(&d->stamp, 0ull, __ATOMIC_SEQ_CST);     // readers must not trust the fields while we write them
    d->kind = kind; d->slot = slot; d->lo = lo; d->hi = hi; d->owner = owner; d->dlo = dlo; d->dhi = dhi;
    __atomic_store_n(&d->stamp, n + 1ull, __ATOMIC_SEQ_CST);
    return n + 1ull;
}
static inline uint64_t n48_cg_ring_push_o(n48_cg_ring *g, uint32_t kind, uint32_t slot, uint64_t lo, uint64_t hi,
                                          uintptr_t owner) {
    return n48_cg_ring_push_od(g, kind, slot, lo, hi, owner, 0ull, 0ull);
}
static inline uint64_t n48_cg_ring_push(n48_cg_ring *g, uint32_t kind, uint32_t slot, uint64_t lo, uint64_t hi) {
    return n48_cg_ring_push_o(g, kind, slot, lo, hi, 0u);
}
// Where the ring stands now — what a segment marks BEFORE its translate call, so the reader check knows which
// events are new.
static inline uint64_t n48_cg_ring_mark(const n48_cg_ring *g) {
    return __atomic_load_n(&g->next, __ATOMIC_SEQ_CST);
}
static inline int n48_cg_ring_wrapped(uint64_t since, uint64_t now) {
    return now < since || now - since > N48_CG_RING;
}
enum { N48_CG_RING_NONE = 0, N48_CG_RING_EVENT = 1, N48_CG_RING_TORN = 2 };
// Scan events [since, now) for one that overlaps [lo, hi). Caller has already ruled out WRAP (n48_cg_ring_wrapped).
static inline int n48_cg_ring_scan_range(const n48_cg_ring *g, uint64_t since, uint64_t now, uint64_t lo, uint64_t hi) {
    for (uint64_t n = since; n < now; n++) {
        const n48_cg_ev *e = &g->s[n % N48_CG_RING];
        const uint64_t s1 = __atomic_load_n(&e->stamp, __ATOMIC_SEQ_CST);
        const uint64_t l = e->lo, h = e->hi;
        const uint64_t s2 = __atomic_load_n(&e->stamp, __ATOMIC_SEQ_CST);
        if (s1 != n + 1ull || s2 != s1) return N48_CG_RING_TORN;    // overwritten or half-written: unknown, refuse
        if (n48_cg_overlap(l, h, lo, hi)) return N48_CG_RING_EVENT;
    }
    return N48_CG_RING_NONE;
}

// ---- the poison table (writer: mark on FAILED/mismatch/partial write; a later clean covering copy clears it) ----
// build 0.0.510 A2 (the 0.0.509 review's LOW): THE ROWS ARE CLAIMED BY COMPARE-AND-SWAP. Through 0.0.509 the
// table had no lock and no CAS: two markers could both see one row empty and both write it (the last store wins, so a sticky
// range could be lost), and a clear could read a row's `valid` 1, lose the CPU while a sticky range was folded into that row,
// then store 0 over it. Every change to a row now goes through ONE protocol:
//   - a row's `valid` is EMPTY (0), VALID (1), STICKY (N48_CG_POISON_STICKY, 2) or CLAIMING (N48_CG_POISON_CLAIMING, 3);
//   - an actor first CLAIMS the row with a CAS of `valid` from the value it read (0, 1 or 2) to CLAIMING - only one actor can
//     win it - then reads/writes lo and hi, then PUBLISHES the row's final `valid` with one store;
//   - a marker claims an EMPTY row (0 -> CLAIMING), writes lo/hi, publishes 1 (or 2); with no empty row it FOLDS into the first
//     row it can claim from 1/2 (row 0 first, as 0.0.509), widens lo/hi, and publishes STICKY if the row or the new range was
//     sticky: a fold never demotes a sticky row;
//   - a clear claims only a VALID row (1 -> CLAIMING: a STICKY or CLAIMING row is never touched), reads lo/hi it now owns, and
//     publishes 0 if the clean copy covers them, else 1 again;
//   - a reader (n48_cg_poison_overlaps) treats a CLAIMING row as poisoned over EVERY range (fail-closed while it is written).
// The steps are a small machine (n48_cg_pm) so tests/gfx_copyguard_test.cpp (T25) can interleave two actors step by step over
// every schedule; the kext's functions below run one machine to completion. Single-threaded, every function leaves the table
// exactly as 0.0.509's did (T25 compares them over random sequences). The fold's retry is bounded: an actor that cannot claim
// any row after N48_CG_PM_PASSES passes sets `over`, which poisons every range for the rest of the boot (never dropped).
typedef struct { uint64_t lo, hi; uint32_t valid; } n48_cg_poison_row;
typedef struct { n48_cg_poison_row s[N48_CG_POISON]; uint32_t over; } n48_cg_poison;

#define N48_CG_POISON_STICKY   2u
#define N48_CG_POISON_CLAIMING 3u
#define N48_CG_PM_PASSES       4096u

static inline void n48_cg_poison_init(n48_cg_poison *p) {
    for (uint32_t i = 0; i < N48_CG_POISON; i++) { p->s[i].lo = p->s[i].hi = 0ull; p->s[i].valid = 0u; }
    p->over = 0u;
}

// One actor. `kind`: N48_CG_PM_MARK (v = 1 or STICKY) or N48_CG_PM_CLEAR. `nrows` is the table size the actor walks (the kext:
// N48_CG_POISON; a test may use a prefix so every schedule can be enumerated).
enum { N48_CG_PM_MARK = 1u, N48_CG_PM_CLEAR = 2u };
enum { N48_CG_PM_SCAN = 0u, N48_CG_PM_WLO, N48_CG_PM_WHI, N48_CG_PM_PUB, N48_CG_PM_FOLD, N48_CG_PM_FLO, N48_CG_PM_FHI,
       N48_CG_PM_CLOOK, N48_CG_PM_CREAD, N48_CG_PM_DONE };
typedef struct {
    uint32_t kind, v, pc, i, nrows, cur, passes, pad;
    uint64_t lo, hi, l, h;
} n48_cg_pm;

static inline void n48_cg_pm_init(n48_cg_pm *m, uint32_t kind, uint32_t v, uint64_t lo, uint64_t hi, uint32_t nrows) {
    m->kind = kind; m->v = v; m->i = 0u; m->nrows = nrows; m->cur = 0u; m->passes = 0u; m->pad = 0u;
    m->lo = lo; m->hi = hi; m->l = 0ull; m->h = 0ull;
    m->pc = kind == N48_CG_PM_CLEAR ? N48_CG_PM_CLOOK : N48_CG_PM_SCAN;
    if (!nrows) m->pc = N48_CG_PM_DONE;
}

// ONE atomic step of the actor. Returns 1 when the actor is done.
static inline int n48_cg_pm_step(n48_cg_pm *m, n48_cg_poison *p) {
    n48_cg_poison_row *r = &p->s[m->i < N48_CG_POISON ? m->i : 0u];
    switch (m->pc) {
    case N48_CG_PM_SCAN: {                               // MARK: claim the next EMPTY row (0 -> CLAIMING)
        uint32_t exp = 0u;
        if (__atomic_compare_exchange_n(&r->valid, &exp, N48_CG_POISON_CLAIMING, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            m->pc = N48_CG_PM_WLO; return 0;
        }
        if (++m->i >= m->nrows) { m->i = 0u; m->pc = N48_CG_PM_FOLD; }
        return 0;
    }
    case N48_CG_PM_WLO: __atomic_store_n(&r->lo, m->lo, __ATOMIC_SEQ_CST); m->pc = N48_CG_PM_WHI; return 0;
    case N48_CG_PM_WHI: __atomic_store_n(&r->hi, m->hi, __ATOMIC_SEQ_CST); m->pc = N48_CG_PM_PUB; return 0;
    case N48_CG_PM_PUB: __atomic_store_n(&r->valid, m->v, __ATOMIC_SEQ_CST); m->pc = N48_CG_PM_DONE; return 1;
    case N48_CG_PM_FOLD: {                               // MARK, table full: claim a row from what it holds (row 0 first)
        uint32_t cur = __atomic_load_n(&r->valid, __ATOMIC_SEQ_CST);
        if (cur != N48_CG_POISON_CLAIMING &&
            __atomic_compare_exchange_n(&r->valid, &cur, N48_CG_POISON_CLAIMING, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            m->cur = cur;
            m->pc = cur == 0u ? N48_CG_PM_WLO : N48_CG_PM_FLO;   // emptied meanwhile: a fresh write; else widen what it holds
            return 0;
        }
        if (++m->i >= m->nrows) {
            m->i = 0u;
            if (++m->passes >= N48_CG_PM_PASSES) {       // never dropped: the whole table answers "poisoned" for the boot
                __atomic_store_n(&p->over, 1u, __ATOMIC_SEQ_CST);
                m->pc = N48_CG_PM_DONE; return 1;
            }
        }
        return 0;
    }
    case N48_CG_PM_FLO: {                                // widen lo (the row is ours: CLAIMING)
        const uint64_t l0 = __atomic_load_n(&r->lo, __ATOMIC_SEQ_CST);
        __atomic_store_n(&r->lo, m->lo < l0 ? m->lo : l0, __ATOMIC_SEQ_CST);
        m->pc = N48_CG_PM_FHI; return 0;
    }
    case N48_CG_PM_FHI: {                                // widen hi, then publish: a fold never demotes STICKY
        const uint64_t h0 = __atomic_load_n(&r->hi, __ATOMIC_SEQ_CST);
        __atomic_store_n(&r->hi, m->hi > h0 ? m->hi : h0, __ATOMIC_SEQ_CST);
        m->v = (m->cur == N48_CG_POISON_STICKY || m->v == N48_CG_POISON_STICKY) ? N48_CG_POISON_STICKY : 1u;
        m->pc = N48_CG_PM_PUB; return 0;
    }
    case N48_CG_PM_CLOOK: {                              // CLEAR: claim the next VALID row (1 -> CLAIMING); STICKY is never touched
        uint32_t exp = 1u;
        if (__atomic_compare_exchange_n(&r->valid, &exp, N48_CG_POISON_CLAIMING, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            m->pc = N48_CG_PM_CREAD; return 0;
        }
        if (++m->i >= m->nrows) { m->pc = N48_CG_PM_DONE; return 1; }
        return 0;
    }
    case N48_CG_PM_CREAD: {                              // the row is ours: read what it holds, publish 0 if covered, else 1 again
        m->l = __atomic_load_n(&r->lo, __ATOMIC_SEQ_CST);
        m->h = __atomic_load_n(&r->hi, __ATOMIC_SEQ_CST);
        __atomic_store_n(&r->valid, (m->lo <= m->l && m->h <= m->hi) ? 0u : 1u, __ATOMIC_SEQ_CST);
        if (++m->i >= m->nrows) { m->pc = N48_CG_PM_DONE; return 1; }
        m->pc = N48_CG_PM_CLOOK; return 0;
    }
    default: return 1;
    }
}
static inline void n48_cg_pm_run(n48_cg_pm *m, n48_cg_poison *p) { while (!n48_cg_pm_step(m, p)) { } }

// Mark [lo, hi) poisoned. If every row is already in use, the range is folded into row 0 rather than dropped
// (fail-closed: this table must never silently forget a poisoned range for want of space).
static inline void n48_cg_poison_mark(n48_cg_poison *p, uint64_t lo, uint64_t hi) {
    n48_cg_pm m; n48_cg_pm_init(&m, N48_CG_PM_MARK, 1u, lo, hi, N48_CG_POISON); n48_cg_pm_run(&m, p);
}
// build 0.0.509 item 2 (F-2 of the 0.0.496 review,): a STICKY row (valid == N48_CG_POISON_STICKY) marks a range a
// late DMA may still write - an SDMA chunk whose fence did not land - and stays poisoned for the rest of the boot: no clean
// copy clears it. Folded into row 0 when the table is full, row 0 becomes sticky (fail-closed).
static inline void n48_cg_poison_mark_sticky(n48_cg_poison *p, uint64_t lo, uint64_t hi) {
    n48_cg_pm m; n48_cg_pm_init(&m, N48_CG_PM_MARK, N48_CG_POISON_STICKY, lo, hi, N48_CG_POISON); n48_cg_pm_run(&m, p);
}
// A later CLEAN copy that fully covers [lo, hi) clears every poisoned row [lo, hi) fully covers. A copy that only
// partly covers a poisoned row must never clear it — the uncovered remainder is still exactly as suspect as before.
// 0.0.509: a sticky row is never cleared. 0.0.510 A2: only a row claimed from VALID (CAS 1 -> CLAIMING) is ever cleared.
static inline void n48_cg_poison_clear_covered(n48_cg_poison *p, uint64_t lo, uint64_t hi) {
    n48_cg_pm m; n48_cg_pm_init(&m, N48_CG_PM_CLEAR, 0u, lo, hi, N48_CG_POISON); n48_cg_pm_run(&m, p);
}
// 0.0.510 A2: a CLAIMING row answers "overlaps" for every range (it is being written), and so does a saturated table (`over`).
static inline int n48_cg_poison_overlaps(const n48_cg_poison *p, uint64_t lo, uint64_t hi) {
    if (__atomic_load_n(&p->over, __ATOMIC_SEQ_CST)) return 1;
    for (uint32_t i = 0; i < N48_CG_POISON; i++) {
        const uint32_t v = __atomic_load_n(&p->s[i].valid, __ATOMIC_SEQ_CST);
        if (!v) continue;
        if (v == N48_CG_POISON_CLAIMING) return 1;
        const uint64_t l = __atomic_load_n(&p->s[i].lo, __ATOMIC_SEQ_CST);
        const uint64_t h = __atomic_load_n(&p->s[i].hi, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&p->s[i].valid, __ATOMIC_SEQ_CST) != v) return 1;   // changed under the read: fail-closed
        if (n48_cg_overlap(l, h, lo, hi)) return 1;
    }
    return 0;
}

// 0.0.435 review fix — THE CLOSE DECISION, PURE. `wrote` is 0 for a scope that never wrote a byte (the switch-39
// phantom scope, "writing nothing" by construction): such a close must neither mark NOR clear poison, because
// "clear" would wrongly vouch for a range this scope never touched — a page a FAILED copy half-wrote earlier would
// be trusted again for free. `wrote` is 1 for every real residency copy and the standalone kernel-substitution
// scope. Called ONLY by navi48_cg_close (Navi48Bringup.cpp).
static inline void n48_cg_close_poison(n48_cg_poison *p, uint64_t lo, uint64_t hi, int wrote, int failed, int mismatch) {
    if (!wrote) return;
    if (failed || mismatch) n48_cg_poison_mark(p, lo, hi);
    else n48_cg_poison_clear_covered(p, lo, hi);
}

// ---- the per-segment page recorder (reader side) -----------------------------------------------------------------
// `gfxc_read_rs` (AppleHardwareHook.cpp) notes every VRAM page it actually reads, for the descriptor-read and
// program-identity callbacks only. A future per-pass memo (design Part 1's M, not built this brief) that answers a
// program-identity ask WITHOUT re-reading must call n48_cg_pagerec_note itself with the pages its cached answer
// depends on, or those pages are invisible to this check — see tests/gfx_copyguard_test.cpp's memo-hit test.
// build 0.0.523 (RING-NEUTER-FORGIVE.md rev 2 item 9, switch 78): APPENDED - `since[i]`, the ring mark in force when
// page i was FIRST recorded (`cur_since` at that moment); `per_page`, set ONLY by the switch-78 redo (n48_cg_redo_rebase),
// makes n48_cg_check_ex scan page i over [since[i], now) instead of [pass since, now). A deduplicated page keeps its OLDER
// since. per_page 0 (every reset, and every pass while 78 is OFF) is the unchanged whole-pass window, check for check.
// build 0.0.529 (notes/design/CG84.md items 2-3, switch 84) - APPENDED: a 16-byte GRANULE mask per recorded page
// (`gm[i]`, 256 bits = 4 KiB / 16), `fine` (latched by navi48_cg_seg_begin as "switch 84 is ON"), the latched switch-84 mode
// `m84` (instrument only), a count of whole-page notes (`nfull`, instrument only) and the CENSUS list (`xpg`, contract item 5 /
// X2): whole pages noted by a reader that does not go through gfxc_read_rs. RULES:
//   - n48_cg_pagerec_note sets exactly the granules of [off, off + bytes), clipped per page; n48_cg_pagerec_note_page (a whole
//     page) sets all 256; a deduplicated page ORs its bits; a NEWLY taken page index zeroes its mask first.
//   - `fine` 0 is 0.0.528's check, verdict for verdict (n48_cg_rec_hit answers the page test; the census list is not read, and
//     `xover` is not read). tests/gfx_cg84_test.cpp compares it against a frozen 0.0.528 over random schedules.
//   - the census list is read ONLY by a fine check, as whole pages (every granule set); a census page that overflows the list sets
//     `xover`, which a fine check answers OVERFLOW (fail-closed).
// Size: 128 x 32 B of masks + 8 census pages + 6 u32: the recorder grows from 2,072 B to 6,256 B. Every instance is a file-scope static
// in the kext (gCgSegRec, gPgmProfileScratchTM, gPgmProfileScratchShadowM): no stack growth.
#define N48_CG_XPAGES 8u
typedef struct {
    uint64_t page[N48_CG_REC_PAGES];   // page-aligned VRAM byte offsets, each covering [page, page + 4096)
    uint32_t n;
    uint32_t overflow;                 // 1 = more distinct pages were touched than this recorder can hold
    uint64_t since[N48_CG_REC_PAGES];  // 0.0.523: the ring mark before page i was first read (valid with per_page)
    uint64_t cur_since;                // 0.0.523: what a NEW page's since is set to (the pass mark, then a redo's mark)
    uint32_t per_page;                 // 0.0.523: 1 only after a switch-78 redo re-based this pass (n48_cg_redo_rebase)
    uint32_t pad0;
    uint64_t gm[N48_CG_REC_PAGES][4];  // 0.0.529: one bit per 16-byte granule of page i (bit g of word g >> 6)
    uint32_t fine;                     // 0.0.529: 1 = the check tests granules (switch 84 ON, latched per pass)
    uint32_t m84;                      // 0.0.529: switch 84's mode latched per pass (1 SHADOW, 2 OFF, 3 ON; 0 = never latched)
    uint32_t nfull;                    // 0.0.529: whole-page notes this pass (instrument)
    uint32_t xn;                       // 0.0.529: census pages held
    uint32_t xover;                    // 0.0.529: a census page could not be held (a fine check refuses OVERFLOW)
    uint32_t pad1;
    uint64_t xpg[N48_CG_XPAGES];       // 0.0.529: census pages (whole pages; read only by a fine check)
} n48_cg_pagerec;

static inline void n48_cg_pagerec_reset(n48_cg_pagerec *r) {
    r->n = 0u; r->overflow = 0u; r->per_page = 0u; r->cur_since = 0ull;
    r->nfull = 0u; r->xn = 0u; r->xover = 0u;   // 0.0.529 (`fine` and `m84` are the latch's, set by the pass top)
}
// build 0.0.523: the pass top's reset, with the pass mark as every first page's since.
static inline void n48_cg_pagerec_reset_at(n48_cg_pagerec *r, uint64_t since) { n48_cg_pagerec_reset(r); r->cur_since = since; }

// 0.0.529: the page's index, taking a NEW index (its mask zeroed first) when the page is not held yet. -1: the recorder is full
// (overflow set). A deduplicated page keeps its index, its older since and its bits.
static inline int32_t n48_cg_pagerec_take(n48_cg_pagerec *r, uint64_t pageOff) {
    for (uint32_t i = 0; i < r->n; i++) if (r->page[i] == pageOff) return (int32_t)i;   // dedupe: a segment re-reads pages often
    if (r->n >= N48_CG_REC_PAGES) { r->overflow = 1u; return -1; }
    const uint32_t i = r->n;
    r->gm[i][0] = r->gm[i][1] = r->gm[i][2] = r->gm[i][3] = 0ull;              // 0.0.529: a new index starts with no granule
    r->since[i] = r->cur_since;                                                 // 0.0.523: a NEW page's window start
    r->page[i] = pageOff;
    r->n = i + 1u;
    return (int32_t)i;
}
// 0.0.529: set granules [g0, g1) of page i (0 <= g0 < g1 <= 256): OR, never overwrite.
static inline void n48_cg_gm_set(n48_cg_pagerec *r, uint32_t i, uint32_t g0, uint32_t g1) {
    for (uint32_t g = g0; g < g1; g++) r->gm[i][g >> 6] |= 1ull << (g & 63u);
}
// The WHOLE page: all 256 granules (the memo hits, the T+M copy of a scratch recorder's pages, any page-level note).
static inline void n48_cg_pagerec_note_page(n48_cg_pagerec *r, uint64_t pageOff) {
    const int32_t i = n48_cg_pagerec_take(r, pageOff);
    if (i < 0) return;
    r->gm[i][0] = r->gm[i][1] = r->gm[i][2] = r->gm[i][3] = ~0ull;
    r->nfull++;
}
// Note every 4 KiB page touched by [off, off + bytes) - and, 0.0.529, exactly the 16-byte granules of that range in each page
// (clipped to the page): g0 = (clo - p) >> 4, g1 = (chi - p + 15) >> 4.
static inline void n48_cg_pagerec_note(n48_cg_pagerec *r, uint64_t off, uint32_t bytes) {
    if (!bytes) return;
    const uint64_t start = off & ~4095ull;
    const uint64_t end   = (off + (uint64_t)bytes + 4095ull) & ~4095ull;
    const uint64_t hi    = off + (uint64_t)bytes;
    for (uint64_t p = start; p < end; p += 4096ull) {
        const int32_t i = n48_cg_pagerec_take(r, p);
        if (i < 0) continue;
        const uint64_t clo = off > p ? off : p;
        const uint64_t chi = hi < p + 4096ull ? hi : p + 4096ull;
        const uint32_t g0 = (uint32_t)((clo - p) >> 4), g1 = (uint32_t)((chi - p + 15ull) >> 4);
        n48_cg_gm_set(r, (uint32_t)i, g0, g1);
        if (g0 == 0u && g1 == 256u) r->nfull++;
    }
}
// 0.0.529 (CG84.md item 5, X2): a CENSUS note - a whole page read by a reader that does not go through gfxc_read_rs. Held in the
// census list, which only a fine check reads; a page 0.0.528 would record is untouched by this, so OFF (and SHADOW's counted
// page check) is unchanged by construction.
static inline void n48_cg_pagerec_note_census(n48_cg_pagerec *r, uint64_t off) {
    const uint64_t p = off & ~4095ull;
    for (uint32_t j = 0; j < r->xn && j < N48_CG_XPAGES; j++) if (r->xpg[j] == p) return;
    if (r->xn >= N48_CG_XPAGES) { r->xover = 1u; return; }
    r->xpg[r->xn++] = p;
    r->nfull++;
}
// The index space a check walks: the recorded pages, then (a fine check only) the census pages.
static inline uint32_t n48_cg_rec_count(const n48_cg_pagerec *r, uint32_t fine) {
    return r->n + (fine ? (r->xn < N48_CG_XPAGES ? r->xn : N48_CG_XPAGES) : 0u);
}
static inline uint64_t n48_cg_rec_page(const n48_cg_pagerec *r, uint32_t i) {
    return i < r->n ? r->page[i] : r->xpg[(i - r->n) % N48_CG_XPAGES];
}
// 0.0.529 (CG84.md item 3) - THE HIT TEST. `fine` 0: today's page test (page i's 4 KiB overlaps [lo, hi)). `fine` 1: clip [lo, hi)
// to the page, g0 = (clo - p) >> 4, g1 = (chi - p + 15) >> 4, and answer whether any granule bit in [g0, g1) is set. A census page
// (i >= n) is a whole page: every granule set.
static inline int n48_cg_rec_hit_f(const n48_cg_pagerec *r, uint32_t i, uint64_t lo, uint64_t hi, uint32_t fine) {
    const uint64_t p = n48_cg_rec_page(r, i);
    if (!fine || i >= r->n) return n48_cg_overlap(p, p + 4096ull, lo, hi);
    const uint64_t clo = lo > p ? lo : p;
    const uint64_t chi = hi < p + 4096ull ? hi : p + 4096ull;
    if (clo >= chi) return 0;
    const uint32_t g0 = (uint32_t)((clo - p) >> 4), g1 = (uint32_t)((chi - p + 15ull) >> 4);
    for (uint32_t g = g0; g < g1 && g < 256u; g++) if ((r->gm[i][g >> 6] >> (g & 63u)) & 1ull) return 1;
    return 0;
}
static inline int n48_cg_rec_hit(const n48_cg_pagerec *r, uint32_t i, uint64_t lo, uint64_t hi) {
    return n48_cg_rec_hit_f(r, i, lo, hi, r->fine);
}
// The first granule of page i that [lo, hi) hits (the `why` instrument); 256 = page-level / none.
static inline uint32_t n48_cg_rec_first_granule(const n48_cg_pagerec *r, uint32_t i, uint64_t lo, uint64_t hi, uint32_t fine) {
    if (!fine || i >= r->n) return 256u;
    const uint64_t p = r->page[i];
    const uint64_t clo = lo > p ? lo : p;
    const uint64_t chi = hi < p + 4096ull ? hi : p + 4096ull;
    if (clo >= chi) return 256u;
    const uint32_t g0 = (uint32_t)((clo - p) >> 4), g1 = (uint32_t)((chi - p + 15ull) >> 4);
    for (uint32_t g = g0; g < g1 && g < 256u; g++) if ((r->gm[i][g >> 6] >> (g & 63u)) & 1ull) return g;
    return 256u;
}
static inline int n48_cg_pagerec_overlaps(const n48_cg_pagerec *r, uint64_t lo, uint64_t hi) {
    if (r->overflow) return 1;   // fail-closed: cannot say the recorder saw everything, so assume it could overlap
    for (uint32_t i = 0; i < r->n; i++)
        if (n48_cg_overlap(r->page[i], r->page[i] + 4096ull, lo, hi)) return 1;
    return 0;
}

// ---- refusal reasons and live counters -----------------------------------------------------------------------
enum {
    N48_CG_OK = 0,
    N48_CG_POISONED = 1,
    N48_CG_IN_FLIGHT = 2,
    N48_CG_TORN = 3,
    N48_CG_WRAP = 4,
    N48_CG_EVENT = 5,
    N48_CG_UNTRACKED = 6,
    N48_CG_OVERFLOW = 7,
    N48_CG_REASONS = 8,
};
static inline const char *n48_cg_reason_name(uint32_t r) {
    switch (r) {
        case N48_CG_OK:        return "OK";
        case N48_CG_POISONED:  return "POISONED";
        case N48_CG_IN_FLIGHT: return "IN_FLIGHT";
        case N48_CG_TORN:      return "TORN";
        case N48_CG_WRAP:      return "WRAP";
        case N48_CG_EVENT:     return "EVENT";
        case N48_CG_UNTRACKED: return "UNTRACKED";
        case N48_CG_OVERFLOW:  return "RECORDER_OVERFLOW";
        default:                return "?";
    }
}

typedef struct {
    uint64_t opened, closed, inFlight, poisoned, untracked, unscoped, checked;
    uint64_t refused[N48_CG_REASONS];
} n48_cg_stats;

static inline void n48_cg_stats_init(n48_cg_stats *s) {
    s->opened = s->closed = s->inFlight = s->poisoned = s->untracked = s->unscoped = s->checked = 0ull;
    for (uint32_t i = 0; i < N48_CG_REASONS; i++) s->refused[i] = 0ull;
}

// ---- THE READER CHECK, in the binding order --------------------------------------------------------------------
// SEQ_CST fence; poison; slot scan; THEN load the ring position; ring scan. The order between the slot scan and
// loading the ring position is load-bearing, not stylistic: consider a copy that OPENs (BEGIN pushed, slot odd),
// writes, and fully CLOSEs (END pushed, slot even) entirely inside this check's own window.
//   - If the ring position were loaded BEFORE the slot scan (the planted break), "now" could be captured before
//     BEGIN/END even exist; the slot scan then finds the slot already even (missed); the ring scan's window
//     [since, now) ends before BEGIN's index. Neither catches it: the whole open-write-close cycle is invisible.
//   - Loading the ring position AFTER the slot scan means "now" is captured no earlier than the slot scan's own
//     read of this slot; if the copy had not yet closed when the slot scan ran, IN_FLIGHT catches it directly; if
//     it closed during or just after the slot scan, its END event's index is already <= "now" (END is published,
//     by the writer's own CLOSE order, before seq goes even — see n48_cg_slot_close's comment — so by the time a
//     reader can observe "closed", END already exists), so the ring scan's window includes it.
// The same reasoning is why CLOSE must push END before publishing "seq even": doing it the other way (a planted
// break) reopens exactly this gap from the writer's side.
// build 0.0.523 (RING-NEUTER-FORGIVE.md item 8 / rev 2 item 9): WHAT A REFUSAL NAMED - filled by n48_cg_check_ex for the
// per-refusal line and the switch-78 redo's plan. `has` 0: the reason names no page (OK, OVERFLOW, UNTRACKED, WRAP).
typedef struct {
    uint32_t reason, has;
    uint32_t idx, nrec;          // the page's index in the recorder, and the recorder's count
    uint64_t page;
    uint64_t ev;                 // EVENT / ring TORN: the event's index (its number, 0-based)
    uint32_t kind, slot;         // EVENT: the event's kind and slot; IN_FLIGHT / slot TORN: 0 and the slot index
    uint64_t lo, hi;             // the event's / the open slot's range
    uintptr_t owner;             // the event's / the open slot's owner
    uint64_t since, now;         // the window start this page was scanned from, and the ring position loaded
    uint32_t fine;               // 0.0.529: the check that filled this was a granule (fine) check
    uint32_t gran;               // 0.0.529: the first granule of the page the refusing range hit (256: page-level / none)
} n48_cg_why;

// build 0.0.523: the first slot overlapping [lo, hi) that is open (or torn), with its range and owner. -1: none.
static inline int32_t n48_cg_slot_first_overlap(const n48_cg_slot *slots, uint32_t n, uint64_t lo, uint64_t hi,
                                                uint64_t *slo, uint64_t *shi, uintptr_t *owner) {
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t s1 = __atomic_load_n(&slots[i].seq, __ATOMIC_SEQ_CST);
        const uint64_t l  = __atomic_load_n(&slots[i].lo,  __ATOMIC_SEQ_CST);
        const uint64_t h  = __atomic_load_n(&slots[i].hi,  __ATOMIC_SEQ_CST);
        const uintptr_t o = __atomic_load_n(&slots[i].owner, __ATOMIC_SEQ_CST);
        const uint64_t s2 = __atomic_load_n(&slots[i].seq, __ATOMIC_SEQ_CST);
        if (s1 != s2 || ((s1 & 1ull) && n48_cg_overlap(l, h, lo, hi))) {
            if (slo) *slo = l;
            if (shi) *shi = h;
            if (owner) *owner = o;
            return (int32_t)i;
        }
    }
    return -1;
}

// The ring scan with the event it found (index + fields) - the same seqlock read, the same answers.
static inline int n48_cg_ring_scan_range_ex(const n48_cg_ring *g, uint64_t since, uint64_t now, uint64_t lo, uint64_t hi,
                                            n48_cg_why *why) {
    for (uint64_t n = since; n < now; n++) {
        const n48_cg_ev *e = &g->s[n % N48_CG_RING];
        const uint64_t s1 = __atomic_load_n(&e->stamp, __ATOMIC_SEQ_CST);
        const uint64_t l = e->lo, h = e->hi;
        const uint32_t k = e->kind, sl = e->slot;
        const uintptr_t o = e->owner;
        const uint64_t s2 = __atomic_load_n(&e->stamp, __ATOMIC_SEQ_CST);
        if (s1 != n + 1ull || s2 != s1) { if (why) why->ev = n; return N48_CG_RING_TORN; }   // overwritten or half-written
        if (n48_cg_overlap(l, h, lo, hi)) {
            if (why) { why->ev = n; why->kind = k; why->slot = sl; why->lo = l; why->hi = h; why->owner = o; }
            return N48_CG_RING_EVENT;
        }
    }
    return N48_CG_RING_NONE;
}

// ---- THE READER CHECK, in the binding order --------------------------------------------------------------------
// SEQ_CST fence; poison; slot scan; THEN load the ring position; ring scan. The order between the slot scan and
// loading the ring position is load-bearing, not stylistic: consider a copy that OPENs (BEGIN pushed, slot odd),
// writes, and fully CLOSEs (END pushed, slot even) entirely inside this check's own window.
//   - If the ring position were loaded BEFORE the slot scan (the planted break), "now" could be captured before
//     BEGIN/END even exist; the slot scan then finds the slot already even (missed); the ring scan's window
//     [since, now) ends before BEGIN's index. Neither catches it: the whole open-write-close cycle is invisible.
//   - Loading the ring position AFTER the slot scan means "now" is captured no earlier than the slot scan's own
//     read of this slot; if the copy had not yet closed when the slot scan ran, IN_FLIGHT catches it directly; if
//     it closed during or just after the slot scan, its END event's index is already <= "now" (END is published,
//     by the writer's own CLOSE order, before seq goes even — see n48_cg_slot_close's comment — so by the time a
//     reader can observe "closed", END already exists), so the ring scan's window includes it.
// The same reasoning is why CLOSE must push END before publishing "seq even": doing it the other way (a planted
// break) reopens exactly this gap from the writer's side.
// build 0.0.523 (switch 78): n48_cg_check_ex is the SAME check in the SAME order, plus two things. (1) `why` (may be
// NULL) names what refused. (2) With rec->per_page set (only a switch-78 redo sets it), page i's ring window starts at its OWN
// since[i] - the ring mark loaded before that page was first read - instead of the pass mark; the WRAP test uses the
// MINIMUM since over the recorded pages. per_page 0 is the unchanged code path: `since` for every page and for WRAP.
// build 0.0.529 (CG84.md item 3): THE SLOT AND RING SCANS AGAINST ONE RECORDED PAGE, through n48_cg_rec_hit_f. `fine` 0 is the
// page test, so these answer exactly n48_cg_slot_scan / n48_cg_ring_scan_range_ex over [page, page + 4096) (TORN on any torn slot
// or event, as before). `used` 1 (the switch-84 SHADOW instrument ONLY) tests a slot's / event's dlo/dhi when it carries one
// (dhi != 0), else its lo/hi; every counted check passes `used` 0 - the range the writer announced for the bytes it writes.
static inline int n48_cg_slot_scan_rec(const n48_cg_slot *slots, uint32_t nSlots, const n48_cg_pagerec *rec, uint32_t i,
                                       uint32_t fine, uint32_t used, int32_t *which, uint64_t *wlo, uint64_t *whi,
                                       uintptr_t *wown) {
    for (uint32_t k = 0; k < nSlots; k++) {
        const uint64_t s1 = __atomic_load_n(&slots[k].seq, __ATOMIC_SEQ_CST);
        const uint64_t l  = __atomic_load_n(&slots[k].lo,  __ATOMIC_SEQ_CST);
        const uint64_t h  = __atomic_load_n(&slots[k].hi,  __ATOMIC_SEQ_CST);
        const uintptr_t o = __atomic_load_n(&slots[k].owner, __ATOMIC_SEQ_CST);
        const uint64_t dl = __atomic_load_n(&slots[k].dlo, __ATOMIC_SEQ_CST);
        const uint64_t dh = __atomic_load_n(&slots[k].dhi, __ATOMIC_SEQ_CST);
        const uint64_t s2 = __atomic_load_n(&slots[k].seq, __ATOMIC_SEQ_CST);
        const uint64_t el = (used && dh) ? dl : l, eh = (used && dh) ? dh : h;
        const int torn = s1 != s2;
        if (torn || ((s1 & 1ull) && n48_cg_rec_hit_f(rec, i, el, eh, fine))) {
            if (which) *which = (int32_t)k;
            if (wlo) *wlo = el;
            if (whi) *whi = eh;
            if (wown) *wown = o;
            return torn ? N48_CG_SLOT_TORN : N48_CG_SLOT_INFLIGHT;
        }
    }
    return N48_CG_SLOT_NONE;
}
static inline int n48_cg_ring_scan_rec(const n48_cg_ring *g, uint64_t since, uint64_t now, const n48_cg_pagerec *rec, uint32_t i,
                                       uint32_t fine, uint32_t used, n48_cg_why *why) {
    for (uint64_t n = since; n < now; n++) {
        const n48_cg_ev *e = &g->s[n % N48_CG_RING];
        const uint64_t s1 = __atomic_load_n(&e->stamp, __ATOMIC_SEQ_CST);
        const uint64_t l = e->lo, h = e->hi, dl = e->dlo, dh = e->dhi;
        const uint32_t k = e->kind, sl = e->slot;
        const uintptr_t o = e->owner;
        const uint64_t s2 = __atomic_load_n(&e->stamp, __ATOMIC_SEQ_CST);
        if (s1 != n + 1ull || s2 != s1) { if (why) why->ev = n; return N48_CG_RING_TORN; }   // overwritten or half-written
        const uint64_t el = (used && dh) ? dl : l, eh = (used && dh) ? dh : h;
        if (n48_cg_rec_hit_f(rec, i, el, eh, fine)) {
            if (why) {
                why->ev = n; why->kind = k; why->slot = sl; why->lo = el; why->hi = eh; why->owner = o;
                why->gran = n48_cg_rec_first_granule(rec, i, el, eh, fine);
            }
            return N48_CG_RING_EVENT;
        }
    }
    return N48_CG_RING_NONE;
}

// build 0.0.529: a TEST-ONLY step point between the check's steps (tests/gfx_cg84_test.cpp defines it to run a writer there, so
// the binding order is proved on THIS function, not on a copy). The kext never defines it: it compiles to nothing.
#ifndef N48_CG_CHECK_STEP
#define N48_CG_CHECK_STEP(at) ((void)0)
#endif
// build 0.0.529 (CG84.md items 3-4): THE CHECK WITH AN EXPLICIT `fine` AND `used`. Same order as ever (fence; OVERFLOW;
// UNTRACKED; poison; slot scan; THEN the ring position; WRAP; ring), same TORN, poison (page-level), OVERFLOW, UNTRACKED and WRAP
// rules. What `fine` changes: the IN_FLIGHT slot test and the ring EVENT test go through n48_cg_rec_hit_f (granules), the census
// pages are walked too (as whole pages), and a census overflow refuses OVERFLOW. `fine` 0 (with `used` 0) is 0.0.528's
// n48_cg_check_ex, verdict for verdict: tests/gfx_cg84_test.cpp runs both over random schedules. New contract (item 4): OK means no
// slot open at the slot scan, and no event in [since, now), overlaps any RECORDED GRANULE - so every writer must announce a
// [lo, hi) that covers every byte it writes (a residency copy's delta scope does: CG84.md item 6).
static inline uint32_t n48_cg_check_fx(const n48_cg_pagerec *rec, uint32_t fine, uint32_t used, const n48_cg_poison *poison,
                                       const n48_cg_slot *slots, uint32_t nSlots,
                                       const n48_cg_ring *ring, uint64_t since,
                                       uint32_t untrackedCount, n48_cg_why *why) {
    if (why) {
        why->reason = N48_CG_OK; why->has = 0u; why->idx = 0u; why->nrec = rec->n; why->page = 0ull; why->ev = 0ull;
        why->kind = 0u; why->slot = 0u; why->lo = why->hi = 0ull; why->owner = 0u; why->since = since; why->now = 0ull;
        why->fine = fine ? 1u : 0u; why->gran = 256u;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (rec->overflow || (fine && rec->xover)) { if (why) why->reason = N48_CG_OVERFLOW; return N48_CG_OVERFLOW; }
    if (untrackedCount) { if (why) why->reason = N48_CG_UNTRACKED; return N48_CG_UNTRACKED; }
    const uint32_t cnt = n48_cg_rec_count(rec, fine);
    N48_CG_CHECK_STEP(1);
    for (uint32_t i = 0; i < cnt; i++) {
        const uint64_t pg = n48_cg_rec_page(rec, i);
        if (n48_cg_poison_overlaps(poison, pg, pg + 4096ull)) {
            if (why) { why->reason = N48_CG_POISONED; why->has = 1u; why->idx = i; why->page = pg; }
            return N48_CG_POISONED;
        }
    }
    for (uint32_t i = 0; i < cnt; i++) {
        int32_t sl = -1;
        uint64_t sl_lo = 0ull, sl_hi = 0ull;
        uintptr_t sl_own = 0u;
        const int sr = n48_cg_slot_scan_rec(slots, nSlots, rec, i, fine, used, &sl, &sl_lo, &sl_hi, &sl_own);
        if (sr != N48_CG_SLOT_NONE) {
            const uint32_t r = (sr == N48_CG_SLOT_TORN) ? (uint32_t)N48_CG_TORN : (uint32_t)N48_CG_IN_FLIGHT;
            if (why) {
                why->reason = r; why->has = 1u; why->idx = i; why->page = n48_cg_rec_page(rec, i);
                why->slot = sl < 0 ? ~0u : (uint32_t)sl; why->lo = sl_lo; why->hi = sl_hi; why->owner = sl_own;
                why->gran = n48_cg_rec_first_granule(rec, i, sl_lo, sl_hi, fine);
            }
            return r;
        }
    }
    N48_CG_CHECK_STEP(2);
    const uint64_t now = n48_cg_ring_mark(ring);                       // THEN load the ring position
    N48_CG_CHECK_STEP(3);
    uint64_t wsince = since;
    if (rec->per_page && rec->n) {
        wsince = rec->since[0];
        for (uint32_t i = 1; i < rec->n; i++) if (rec->since[i] < wsince) wsince = rec->since[i];
        if (cnt > rec->n && since < wsince) wsince = since;           // 0.0.529: census pages scan from the pass mark
    }
    if (why) why->now = now;
    if (n48_cg_ring_wrapped(wsince, now)) { if (why) { why->reason = N48_CG_WRAP; why->since = wsince; } return N48_CG_WRAP; }
    for (uint32_t i = 0; i < cnt; i++) {
        const uint64_t si = (rec->per_page && i < rec->n) ? rec->since[i] : since;
        const int rr = n48_cg_ring_scan_rec(ring, si, now, rec, i, fine, used, why);
        if (rr != N48_CG_RING_NONE) {
            const uint32_t r = (rr == N48_CG_RING_TORN) ? (uint32_t)N48_CG_TORN : (uint32_t)N48_CG_EVENT;
            if (why) { why->reason = r; why->has = 1u; why->idx = i; why->page = n48_cg_rec_page(rec, i); why->since = si; }
            return r;
        }
    }
    return N48_CG_OK;
}
// The counted check: `fine` is the recorder's latch (navi48_cg_seg_begin), `used` 0.
static inline uint32_t n48_cg_check_ex(const n48_cg_pagerec *rec, const n48_cg_poison *poison,
                                       const n48_cg_slot *slots, uint32_t nSlots,
                                       const n48_cg_ring *ring, uint64_t since,
                                       uint32_t untrackedCount, n48_cg_why *why) {
    return n48_cg_check_fx(rec, rec->fine, 0u, poison, slots, nSlots, ring, since, untrackedCount, why);
}
static inline uint32_t n48_cg_check(const n48_cg_pagerec *rec, const n48_cg_poison *poison,
                                     const n48_cg_slot *slots, uint32_t nSlots,
                                     const n48_cg_ring *ring, uint64_t since,
                                     uint32_t untrackedCount) {
    return n48_cg_check_ex(rec, poison, slots, nSlots, ring, since, untrackedCount, (n48_cg_why *)0);
}

// =====================================================================================================================
// build 0.0.523 (notes/design/RING-NEUTER-FORGIVE.md rev 2 items 9-11) — SWITCH 78, THE COPY-GUARD REDO, PURE HALF.
// One guard window per PASS (0.0.437): a refused segment cannot simply be re-checked from a fresh pass-wide mark (that drops
// every earlier segment's window: fail-open), and keeping the old mark refuses again. So the redo is per PAGE:
//   (1) PLAN (n48_cg_redo_plan): only an EVENT or IN_FLIGHT refusal, in pass 0, of a single or a unit (never a switch-56
//       retried single), not redone before, fewer than N48_CG_REDO_FRAME_MAX redos this frame. IN_FLIGHT waits only with
//       switch 37 OFF and only for a copy another thread owns, bounded by N48_CG_REDO_WAIT_US per frame (n48_cg_redo_wait).
//   (2) UNDO exactly as gfxsrc_defer_take does (gfx_cgredo.h n48_cg_redo_undo): Apple's bytes back, the unit's pool AND spill
//       records back, the frame-local list and the carry rolled back, both program memos cleared (the kext).
//   (3) REBASE (n48_cg_redo_rebase): drop ONLY the pages segment k added (rec.n = nk0), per_page 1, a SEQ_CST fence, and the
//       redo's own ring mark as every NEW page's since - all BEFORE the re-read (n48_cg_redo_seq orders it).
//   (4) RE-TRANSLATE with the same call. The ONLY admit is the unchanged, counted navi48_cg_seg_check at its original site.
// WHY AN IN-FLIGHT COPY CANNOT BE ADMITTED: every page the redo reads has a since loaded BEFORE the read. A copy overlapping
// it is open at the check (IN_FLIGHT), or pushed BEGIN/END at or after that since (EVENT), or pushed END before it - and END
// is pushed only after its writes landed (navi48_cg_close; SDMA chunks whose fence did not land are poisoned sticky, and
// poison is asked first). Pages earlier segments recorded keep their older since (the pass mark).
// =====================================================================================================================
#define N48_CG_REDO_FRAME_MAX 4u      // redos per frame
#define N48_CG_REDO_WAIT_US   2000u   // IN_FLIGHT wait budget per frame (switch 37 OFF only)
#define N48_CG_REDO_POLL_US   50u     // the poll step

enum { N48_CG_RD_NO = 0u, N48_CG_RD_REDO = 1u, N48_CG_RD_WAIT = 2u };
enum { N48_CG_RDW_GO = 0u, N48_CG_RDW_OFF, N48_CG_RDW_REASON, N48_CG_RDW_PASS1, N48_CG_RDW_UNIT, N48_CG_RDW_SEG_DONE,
       N48_CG_RDW_FRAME_CAP, N48_CG_RDW_MM37, N48_CG_RDW_SELF, N48_CG_RDW_REASONS };
typedef struct {
    uint32_t on;            // switch 78, latched for the pass
    uint32_t reason;        // the uncounted peek's answer
    uint32_t pass;          // switch 70's pass (0 = every segment)
    uint32_t is_unit;       // 0 single, 1 unit, 2 a switch-56 retried single
    uint32_t seg_redone;    // this segment was redone already
    uint32_t frame_redos;   // redos this frame so far
    uint32_t mm37;          // switch 37 (the MM-window priority) ON: a copier yields to the pass, a wait is futile
    uint32_t self_owned;    // the IN_FLIGHT slot's owner is the current thread
} n48_cg_redo_in;

static inline uint32_t n48_cg_redo_plan(const n48_cg_redo_in *in, uint32_t *why) {
    uint32_t w = N48_CG_RDW_GO, a = N48_CG_RD_NO;
    if (!in || !in->on) w = N48_CG_RDW_OFF;
    else if (in->reason != N48_CG_EVENT && in->reason != N48_CG_IN_FLIGHT) w = N48_CG_RDW_REASON;
    else if (in->pass != 0u) w = N48_CG_RDW_PASS1;
    else if (in->is_unit > 1u) w = N48_CG_RDW_UNIT;
    else if (in->seg_redone) w = N48_CG_RDW_SEG_DONE;
    else if (in->frame_redos >= N48_CG_REDO_FRAME_MAX) w = N48_CG_RDW_FRAME_CAP;
    else if (in->reason == N48_CG_IN_FLIGHT && in->mm37) w = N48_CG_RDW_MM37;
    else if (in->reason == N48_CG_IN_FLIGHT && in->self_owned) w = N48_CG_RDW_SELF;
    else a = (in->reason == N48_CG_IN_FLIGHT) ? N48_CG_RD_WAIT : N48_CG_RD_REDO;
    if (why) *why = w;
    return a;
}

// THE WAIT BOUND: keep polling only while the time already waited is under what this frame has left.
static inline int n48_cg_wait_more(uint64_t waited_us, uint64_t left_us) { return waited_us < left_us; }
// The bounded IN_FLIGHT wait: poll `busy` every `step` us (the kext: navi48_cg_rec_inflight and IODelay) until no recorded
// page overlaps an open slot (1) or the budget is spent (0). *waited = the time charged (count x step).
typedef int (*n48_cg_busy_fn)(void *ud);
typedef void (*n48_cg_delay_fn)(void *ud, uint32_t us);
static inline int n48_cg_redo_wait(n48_cg_busy_fn busy, n48_cg_delay_fn delay, void *ud, uint64_t left_us, uint32_t step_us,
                                   uint64_t *waited) {
    uint64_t w = 0ull;
    int clear = 0;
    if (!busy || !delay || !step_us) { if (waited) *waited = 0ull; return 0; }
    for (;;) {
        if (!busy(ud)) { clear = 1; break; }
        if (!n48_cg_wait_more(w, left_us)) break;
        delay(ud, step_us);
        w += step_us;
    }
    if (waited) *waited = w;
    return clear;
}

// (3) THE REBASE, before any re-read: drop only segment k's new pages, then per_page, a fence, and the redo's own mark.
// Returns the mark. nk0 above the count (impossible: pages are only appended) leaves the count as it is.
static inline uint64_t n48_cg_redo_rebase(n48_cg_pagerec *rec, const n48_cg_ring *ring, uint32_t nk0) {
    if (nk0 <= rec->n) rec->n = nk0;
    rec->per_page = 1u;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    rec->cur_since = n48_cg_ring_mark(ring);
    return rec->cur_since;
}
// (2)-(4) IN ORDER: undo, rebase, re-translate. `undo` and `translate` are the kext's (the host test's fakes).
typedef void (*n48_cg_undo_fn)(void *ud);
typedef uint32_t (*n48_cg_xlat_fn)(void *ud);
static inline uint32_t n48_cg_redo_seq(n48_cg_pagerec *rec, const n48_cg_ring *ring, uint32_t nk0, n48_cg_undo_fn undo,
                                       n48_cg_xlat_fn translate, void *ud, uint64_t *mark) {
    if (undo) undo(ud);
    const uint64_t m = n48_cg_redo_rebase(rec, ring, nk0);
    if (mark) *mark = m;
    return translate ? translate(ud) : (uint32_t)XLAT12_ERR_ARG;
}

static inline const char *n48_cg_redo_why_name(uint32_t w) {
    switch (w) {
        case N48_CG_RDW_GO: return "go"; case N48_CG_RDW_OFF: return "off"; case N48_CG_RDW_REASON: return "reason";
        case N48_CG_RDW_PASS1: return "pass1"; case N48_CG_RDW_UNIT: return "retried-single";
        case N48_CG_RDW_SEG_DONE: return "seg-redone"; case N48_CG_RDW_FRAME_CAP: return "frame-cap";
        case N48_CG_RDW_MM37: return "37-on"; case N48_CG_RDW_SELF: return "self-owned";
        default: return "?";
    }
}

typedef struct {
    uint64_t asked, event, inflight, inflight37NoWait, waitTimeout, selfOwned, ineligible, capped, redone, thenOk, thenRefused;
    uint64_t thenEvent, thenInflight, thenOther, waits, waitUsMax;
    uint64_t waitUs[4];   // buckets: 0, <= 500, <= 1000, > 1000 us
} n48_cg_redo_stats;

// ---- 0.0.523 lines (each bounded under 511 bytes at its widest by tests/gfx_cgredo_test.cpp) ----------------------------
#define N48_CG_REDO_FMT \
    "cguard-redo: switch 78 %s. peeks %llu (EVENT %llu IN_FLIGHT %llu); redone %llu -> passed full check %llu, refused %llu " \
    "(EVENT %llu IN_FLIGHT %llu other %llu); IN_FLIGHT not redone under 37 %llu, self-owned %llu; not eligible %llu; " \
    "frame budget %llu."
#define N48_CG_REDO2_FMT \
    "cguard-redo: switch 78%s; waits %llu timeouts %llu max %llu us (0 %llu, <=500 %llu, <=1000 %llu, >1000 %llu)."
// (b) THE D4' SEGMENT-REFUSED ATTRIBUTION (item 12(b); decision-inert): appended to the capped `d4f:` line, and a companion
// line after a capped WOULD line whose dep label is consumer-inputs-unproven. n48_dep_reason_name is unchanged.
#define N48_CG_D4F_FMT \
    "d4f: frame %llu D4' consumer n %u nptr %u over %u waits %u memw %u -> %s (unproven %llu) [segment-refused: copy-guard " \
    "%s x%u (first seg %u), translator x%u, other x%u]"
#define N48_CG_WOULD_SEG_FMT \
    "gfx-commit: WOULD #%llu frame %llu dep label: consumer-inputs-unproven [segment-refused: copy-guard %s x%u (first seg " \
    "%u), translator x%u, other x%u]"
#define N48_CG_REF_FMT \
    "cguard-ref: f%llu seg %u/%u %s page %#llx (#%u of %u, %s) ev %llu kind %u slot %u [%#llx,%#llx) self %u; since pass " \
    "%llu seg %llu page %llu now %llu; redo %s; refusals %llu"

// ---- the always-on report line -----------------------------------------------------------------------------------
// Measured worst case (every %llu at UINT64_MAX, 20 digits): 141 literal bytes + 14*20 = 421 B, under the kext's
// 512-byte cap (tests/gfx_copyguard_test.cpp asserts this against the real format string, not this comment).
#define N48_CG_FMT \
    "cguard: opened %llu closed %llu in-flight %llu poisoned %llu untracked %llu unscoped %llu checked %llu; " \
    "refused poison %llu inflight %llu torn %llu wrap %llu event %llu untracked %llu overflow %llu"
#define N48_CG_ARGS(s) \
    (unsigned long long)(s)->opened, (unsigned long long)(s)->closed, (unsigned long long)(s)->inFlight, \
    (unsigned long long)(s)->poisoned, (unsigned long long)(s)->untracked, (unsigned long long)(s)->unscoped, \
    (unsigned long long)(s)->checked, \
    (unsigned long long)(s)->refused[N48_CG_POISONED], (unsigned long long)(s)->refused[N48_CG_IN_FLIGHT], \
    (unsigned long long)(s)->refused[N48_CG_TORN], (unsigned long long)(s)->refused[N48_CG_WRAP], \
    (unsigned long long)(s)->refused[N48_CG_EVENT], (unsigned long long)(s)->refused[N48_CG_UNTRACKED], \
    (unsigned long long)(s)->refused[N48_CG_OVERFLOW]

#endif // N48_GFX_COPYGUARD_H
