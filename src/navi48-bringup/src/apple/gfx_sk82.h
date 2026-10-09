// gfx_sk82.h — build 0.0.527 (an internal design note): SWITCH 82, SKIP A BYTE-IDENTICAL RESIDENCY
// RE-COPY. THE PURE HALF: the write counters' arithmetic, the invalidation event ring, the entry table, the skip decision, the
// establishment and the log/report formats. No register, no page table, nothing of Apple's; plain C that also compiles as C++,
// __atomic builtins only, no libc beyond stdint (the gfx_copyguard.h convention). Compiled into the kext (Navi48Bringup.cpp owns
// the storage and the lock; Navi48AccelPeer.cpp calls it from the copier) and into tests/gfx_sk82_test.cpp.
//
// THE SELECTOR: `gfxneuter 82 | M << 8`: M1 MEASURE (338) compares and counts, never skips and never forces a full verify; M2 OFF
// (594, the default and the boot value); M3 SKIP (850). Bare 82 reads. Any other M is refused unchanged (st 11).
//
// THE INVARIANT a skip rests on: VRAM [vram, vram + bytes) still holds exactly the entry's stored image. It holds from the
// ESTABLISHMENT (a copy that wrote every byte and read every dword back equal, with nothing else of ours or of the GPU able to have
// written that range between the copy's G0 and the check) until any of:
//   - one of OUR writes touches the range: every write opens a copy-guard scope (navi48_cg_open, the single choke point,
//     fact 3), which bumps the range's 64 KiB bucket counters AFTER its slot (or the untracked count) is published open and
//     before its first write - so a write in progress is always visible as either an open slot or a moved counter (D1 below);
//   - an unmapVA overlapping the entry's VA (any context: a superset of the (context, VA) rule), a WindowServer drop, a context
//     release, a binding or arm-level change (the snapshot), a committed frame (the gate), a client IB the ring walk did not NOP
//     (GFX writeTail), or any Apple SDMA ring submission: these are EVENTS in a lock-free ring drained under the table's lock;
//     every event but the unmap clears ALL entries (the unknown-write rule: shader stores are not enumerated anywhere);
//   - a poison row over the range, an open or untracked scope, a switch or state snapshot change.
// A skip re-checks all of it (slots, then counters, then poison, then a final drain) after a FULL memcmp of the source against
// the stored image. Any doubt copies as today.
//
// DEVIATION D1 (from SKIP82.md item 3, reported to the reviewer): the contract bumps each bucket ONCE in navi48_cg_open BEFORE
// n48_cg_slot_open. A writer that has bumped but not yet published its slot is then invisible to both halves of a check: its bump
// is already inside the baseline, and its slot is not open yet. tests/gfx_sk82_test.cpp S7 found the schedule: the bump before our
// G0, the slot and the write between a later skip's slot scan and its counter read -> a skip over rewritten VRAM. Bumping AFTER
// the slot is published open closes it (a writer that bumped has an open slot until its close; one that has not bumped has not
// written), and keeps the contract's "+1 per bucket (our own open)" at establishment. Reader orders: G0 reads counters THEN
// slots; the establishment and the skip read slots THEN counters (S7 walks every schedule of a foreign writer's four steps).
#ifndef N48_GFX_SK82_H
#define N48_GFX_SK82_H

#include <stdint.h>
#include "gfx_copyguard.h"   // n48_cg_slot / n48_cg_slot_scan / n48_cg_poison / n48_cg_poison_overlaps / n48_cg_overlap

// ---- the switch --------------------------------------------------------------------------------------------------------------
#define N48_SK82_SWITCH   82u
#define N48_SK82_OFF      0u      // M2 (594): OFF, the default and the boot value
#define N48_SK82_MEASURE  1u      // M1 (338)
#define N48_SK82_SKIP     3u      // M3 (850)
#define N48_SK82_M_READ   0x100u  // bare 82
#define N48_SK82_M_BAD    0x101u  // any other M: refused, unchanged (st 11)
static inline uint32_t n48_sk82_mode_of_m(uint32_t m) {
    return m == 0u ? N48_SK82_M_READ : m == 1u ? N48_SK82_MEASURE : m == 2u ? N48_SK82_OFF : m == 3u ? N48_SK82_SKIP : N48_SK82_M_BAD;
}
static inline const char *n48_sk82_mode_name(uint32_t mode) {
    return mode == N48_SK82_MEASURE ? "MEASURE(338)" : mode == N48_SK82_SKIP ? "SKIP(850)" : "OFF(594)";
}

// ---- capacities (SKIP82.md item 2) --------------------------------------------------------------------------------------------
#define N48_SK82_ENTRIES   16u          // x 64 KiB images = 1 MiB
#define N48_SK82_MAX_BYTES 0x10000u     // eligibility: bytes <= 64 KiB (also the compare scratch buffer's size)
#define N48_SK82_BSHIFT    16u          // one u32 write counter per 64 KiB of VRAM (1 MiB at 16 GiB)
#define N48_SK82_SPAN_MAX  64u          // a range touching more buckets bumps the everything counter instead
#define N48_SK82_EV        256u         // the invalidation event ring (wrap = every entry invalid)
#define N48_SK82_THR       8u           // concurrent copies with a pending establishment (a full table: no pending, fail-safe)
#define N48_SK82_QUIET_US  100000u      // G0/establishment: no unknown-write event (ring escape / SDMA) in the last 100 ms
#define N48_SK82_LOG_FIRST 32u          // SKIP / DIFFER lines: the first 32, then every 256th
#define N48_SK82_LOG_EVERY 256u

// ---- the write counters (item 3) ----------------------------------------------------------------------------------------------
// [lo, hi) touches buckets lo >> 16 .. (hi - 1) >> 16. More than 64 buckets, a bucket past the array, an empty range or the
// match-all range: the everything counter instead. `cnt` null (before the first ON) is the caller's one load; nothing here runs.
static inline void n48_sk82_bump(uint32_t *cnt, uint64_t nb, uint64_t *every, uint64_t lo, uint64_t hi) {
    if (!cnt) return;
    if (hi <= lo) { __atomic_fetch_add(every, 1ull, __ATOMIC_SEQ_CST); return; }
    const uint64_t b0 = lo >> N48_SK82_BSHIFT, b1 = (hi - 1ull) >> N48_SK82_BSHIFT;
    if (b1 - b0 + 1ull > N48_SK82_SPAN_MAX || b1 >= nb) { __atomic_fetch_add(every, 1ull, __ATOMIC_SEQ_CST); return; }
    for (uint64_t b = b0; b <= b1; b++) __atomic_fetch_add(&cnt[b], 1u, __ATOMIC_SEQ_CST);
}
static inline uint32_t n48_sk82_nbk(uint64_t lo, uint64_t bytes) {
    return bytes ? (uint32_t)(((lo + bytes - 1ull) >> N48_SK82_BSHIFT) - (lo >> N48_SK82_BSHIFT) + 1ull) : 0u;
}

// ---- the invalidation events (item 4) -----------------------------------------------------------------------------------------
enum {
    N48_SK82_EV_UNMAP  = 1u,   // unmapVA [va, va + size) of any context (size 0: the whole context, taken as everything)
    N48_SK82_EV_WS     = 2u,   // WindowServer's binding dropped
    N48_SK82_EV_COMMIT = 3u,   // a frame the COMMIT gate answered yes for (its colour/depth targets and stores: not enumerated)
    N48_SK82_EV_RUN    = 4u,   // a GFX ring writeTail let a client IB run un-NOPed (spared, neuter disarmed, or not walked)
    N48_SK82_EV_SDMA   = 5u,   // an Apple SDMA ring submission (drain_write_tail's rings): destinations not enumerated
    N48_SK82_EV_CTX    = 6u,   // a VM context released
};
typedef struct { uint64_t stamp, va, size; uint32_t kind, pad; } n48_sk82_evt;
typedef struct { n48_sk82_evt s[N48_SK82_EV]; uint64_t next; } n48_sk82_evring;
// Any thread, no lock (the n48_cg_ring discipline: stamp 0 while written, the event number + 1 published last).
static inline uint64_t n48_sk82_ev_push(n48_sk82_evring *g, uint32_t kind, uint64_t va, uint64_t size) {
    const uint64_t n = __atomic_fetch_add(&g->next, 1ull, __ATOMIC_SEQ_CST);
    n48_sk82_evt *d = &g->s[n % N48_SK82_EV];
    __atomic_store_n(&d->stamp, 0ull, __ATOMIC_SEQ_CST);
    d->kind = kind; d->va = va; d->size = size;
    __atomic_store_n(&d->stamp, n + 1ull, __ATOMIC_SEQ_CST);
    return n + 1ull;
}

// ---- causes (the report's "invalid by cause") ---------------------------------------------------------------------------------
enum {
    N48_SK82_C_COUNTER = 0u,  // a bucket counter moved (one of OUR writes touched the range's 64 KiB bucket)
    N48_SK82_C_EVERY,         // the everything counter moved (untracked, match-all or > 64-bucket write)
    N48_SK82_C_UNMAP,         // an unmapVA overlapping the VA, or a context release
    N48_SK82_C_TARGET,        // a committed frame (targets and shader stores: the unknown-write rule clears all)
    N48_SK82_C_POISON,        // a copy-guard poison row overlaps
    N48_SK82_C_OPEN,          // an overlapping open (or torn) scope, or an untracked copy
    N48_SK82_C_STATE,         // switch 11/46/59/62/63, kernsub, shadercache arm, arm level, neuter, binding changed; dest check
    N48_SK82_C_UNKNOWN,       // a client IB ran un-NOPed, or an Apple SDMA submission
    N48_SK82_C_WS,            // WindowServer's binding dropped
    N48_SK82_C_WRAP,          // the event ring wrapped between drains
    N48_SK82_C_FLIGHT,        // G0/establishment: a committed frame still in flight, or an unknown write within 100 ms
    N48_SK82_C_KEY,           // the resource came back with another dstMem / VRAM / bytes / offset / VA / pid
    N48_SK82_C_TAINT,         // the establishing copy wrote more than the source (shadercache / kernsub) or was not eligible
    N48_SK82_C_SRC,           // the source changed during the establishing copy (pre- and post-copy images differ)
    N48_SK82_C_RACE,          // another copy of the same entry began, or the entry was evicted, during the copy
    N48_SK82_CAUSES
};

// ---- the state snapshot (item 5): taken at G0 and at establishment, compared at the skip ----------------------------------------
#define N48_SK82_SNAP_N 10u
enum { N48_SK82_SN_S11 = 0u, N48_SK82_SN_S46, N48_SK82_SN_S59, N48_SK82_SN_S62, N48_SK82_SN_S63, N48_SK82_SN_KSUB,
       N48_SK82_SN_SC, N48_SK82_SN_ARM, N48_SK82_SN_NEUTER, N48_SK82_SN_WS };
typedef struct { uint64_t v[N48_SK82_SNAP_N]; } n48_sk82_snap;
static inline int n48_sk82_snap_eq(const n48_sk82_snap *a, const n48_sk82_snap *b) {
    for (uint32_t i = 0; i < N48_SK82_SNAP_N; i++) if (a->v[i] != b->v[i]) return 0;
    return 1;
}

// ---- the key (item 6) ---------------------------------------------------------------------------------------------------------
typedef struct { uint64_t res, dstMem, vram, bytes, boff, va; int64_t pid; } n48_sk82_key;
static inline int n48_sk82_key_eq(const n48_sk82_key *a, const n48_sk82_key *b) {
    return a->res == b->res && a->dstMem == b->dstMem && a->vram == b->vram && a->bytes == b->bytes && a->boff == b->boff &&
           a->va == b->va && a->pid == b->pid;
}

// ---- the table ----------------------------------------------------------------------------------------------------------------
enum { N48_SK82_S_FREE = 0u, N48_SK82_S_SHADOW = 1u, N48_SK82_S_TRUSTED = 2u };
typedef struct {
    uint32_t state;            // FREE / SHADOW (image kept for the census, never skipped on) / TRUSTED
    uint32_t pend;             // a copy of it is running (between its G0 and its scope's close)
    uintptr_t pthr;            // that copy's thread
    uint64_t pgen;             // moved on every new pending, eviction and reset: a stale thread slot never establishes
    uint32_t ptaint;           // cause + 1 of an event that hit it while pending (0 = none)
    uint32_t nb;               // buckets its range touches (1 or 2)
    uint64_t cnt[2], every;    // TRUSTED: the counters at establishment
    uint64_t g0[2], g0every;   // pending: the counters at G0
    n48_sk82_snap snap;        // pending: at G0; TRUSTED: at establishment
    n48_sk82_key k;
    uint64_t fromCopy, use;    // the establishing copy's number; LRU tick
    uint8_t *img;              // N48_SK82_MAX_BYTES of the pool (set once at allocation)
} n48_sk82_ent;
typedef struct {
    uintptr_t thr;             // 0 = free
    uint32_t ent, cand, ok, taint;   // ok: 0 not noted, 1 fully read back and clean, 2 anything else
    uint64_t gen, lo, hi, copyNo, boff;
    void *md;                  // the copy's backing (alive until its scope closes: the establishment re-reads it)
} n48_sk82_thr;
typedef struct {
    uint64_t considered, eligible, noEntry, equal, differ, skipped, skippedBytes, wouldSkip;
    uint64_t established, candidate, fullForced, evicted, notFull, readFail, noThr;
    uint64_t inval[N48_SK82_CAUSES];
} n48_sk82_stats;
typedef struct {
    n48_sk82_ent e[N48_SK82_ENTRIES];
    n48_sk82_thr t[N48_SK82_THR];
    uint64_t evMark, tick, skipN, differN;
    n48_sk82_stats st;
} n48_sk82_tab;

// What the table's decisions read of the copy guard and the counters. Pointers to the kext's own storage (tests build their own).
typedef struct {
    uint32_t *cnt; uint64_t nb; uint64_t *every;
    const n48_cg_slot *slots; uint32_t nslots; const uint32_t *untracked;
    const n48_cg_poison *poison;
    n48_sk82_evring *ev;
    // Nonzero = BUSY: a committed frame is still PENDING/COMMITTED in the flight ring, or an unknown-write event (a ring escape,
    // an SDMA submission, a commit) was published in the last N48_SK82_QUIET_US. Asked AFTER the drain that precedes G0: an
    // event that drain absorbed (so it can never taint this copy's pending entry) may still have GPU work running (item 4).
    uint32_t (*busy)(void *ctx);
    void *busyCtx;
    // TESTS ONLY (null in the kext): called between the two reads of each decision, so tests/gfx_sk82_test.cpp S7 can put a
    // foreign writer's steps there.
    void (*step)(void *ctx, uint32_t at);
    void *stepCtx;
} n48_sk82_world;
enum { N48_SK82_AT_G0_MID = 1u, N48_SK82_AT_SKIP_MID = 2u, N48_SK82_AT_EST_MID = 3u };
static inline uint32_t n48_sk82_busy(const n48_sk82_world *w) { return w->busy ? w->busy(w->busyCtx) : 1u; }
static inline void n48_sk82_step(const n48_sk82_world *w, uint32_t at) { if (w->step) w->step(w->stepCtx, at); }

// Every entry to FREE, every pending gone, the event mark at the ring's head: every mode change (a table that was not kept current
// while OFF must never be trusted again). The images keep their pool pointers.
static inline void n48_sk82_reset(n48_sk82_tab *t, const n48_sk82_evring *ev) {
    for (uint32_t i = 0; i < N48_SK82_ENTRIES; i++) {
        n48_sk82_ent *e = &t->e[i];
        e->state = N48_SK82_S_FREE; e->pend = 0u; e->pthr = 0u; e->pgen++; e->ptaint = 0u;
    }
    for (uint32_t i = 0; i < N48_SK82_THR; i++) t->t[i].thr = 0u;
    t->evMark = ev ? __atomic_load_n(&ev->next, __ATOMIC_SEQ_CST) : 0ull;
}

// Trust lost (TRUSTED -> SHADOW), counted once per entry; a pending entry records the cause for its establishment.
static inline void n48_sk82_inval(n48_sk82_tab *t, uint32_t i, uint32_t cause) {
    n48_sk82_ent *e = &t->e[i];
    if (e->state == N48_SK82_S_TRUSTED) { e->state = N48_SK82_S_SHADOW; if (cause < N48_SK82_CAUSES) t->st.inval[cause]++; }
    if (e->pend && !e->ptaint) e->ptaint = cause + 1u;
}
static inline uint32_t n48_sk82_ev_cause(uint32_t kind) {
    return kind == N48_SK82_EV_UNMAP || kind == N48_SK82_EV_CTX ? N48_SK82_C_UNMAP : kind == N48_SK82_EV_WS ? N48_SK82_C_WS
         : kind == N48_SK82_EV_COMMIT ? N48_SK82_C_TARGET : N48_SK82_C_UNKNOWN;
}
// Apply every event published since the last drain. A wrap or a torn slot invalidates everything (fail-closed). Under the lock.
static inline void n48_sk82_drain(n48_sk82_tab *t, n48_sk82_evring *g) {
    const uint64_t now = __atomic_load_n(&g->next, __ATOMIC_SEQ_CST);
    uint64_t from = t->evMark;
    if (now < from || now - from > N48_SK82_EV) {
        for (uint32_t i = 0; i < N48_SK82_ENTRIES; i++) n48_sk82_inval(t, i, N48_SK82_C_WRAP);
        t->evMark = now;
        return;
    }
    for (uint64_t n = from; n < now; n++) {
        const n48_sk82_evt *d = &g->s[n % N48_SK82_EV];
        const uint64_t s1 = __atomic_load_n(&d->stamp, __ATOMIC_SEQ_CST);
        const uint32_t kind = d->kind; const uint64_t va = d->va, size = d->size;
        const uint64_t s2 = __atomic_load_n(&d->stamp, __ATOMIC_SEQ_CST);
        if (s1 != n + 1ull || s2 != s1) {   // being written or overwritten: everything, and stop here (the rest is re-read)
            for (uint32_t i = 0; i < N48_SK82_ENTRIES; i++) n48_sk82_inval(t, i, N48_SK82_C_WRAP);
            t->evMark = n;
            return;
        }
        const uint32_t cause = n48_sk82_ev_cause(kind);
        for (uint32_t i = 0; i < N48_SK82_ENTRIES; i++) {
            const n48_sk82_ent *e = &t->e[i];
            if (e->state == N48_SK82_S_FREE) continue;
            const int hit = kind != N48_SK82_EV_UNMAP || size == 0ull || va + size < va ||
                            n48_cg_overlap(e->k.va, e->k.va + e->k.bytes, va, va + size);
            if (hit) n48_sk82_inval(t, i, cause);
        }
    }
    t->evMark = now;
}

// ---- the reads, in the orders the header comment fixes ------------------------------------------------------------------------
static inline void n48_sk82_read_cnt(const n48_sk82_world *w, uint64_t vram, uint32_t nb, uint64_t out[2], uint64_t *every) {
    const uint64_t b0 = vram >> N48_SK82_BSHIFT;
    out[0] = out[1] = 0ull;
    for (uint32_t i = 0; i < nb && i < 2u; i++)
        out[i] = (b0 + i < w->nb) ? __atomic_load_n(&w->cnt[b0 + i], __ATOMIC_SEQ_CST) : ~0ull;
    *every = __atomic_load_n(w->every, __ATOMIC_SEQ_CST);
}
static inline int n48_sk82_open_now(const n48_sk82_world *w, uint64_t lo, uint64_t hi) {
    if (w->untracked && __atomic_load_n(w->untracked, __ATOMIC_SEQ_CST)) return 1;
    return n48_cg_slot_scan(w->slots, w->nslots, lo, hi) != N48_CG_SLOT_NONE;
}

// ---- the full compare (item 7: never sampled) ---------------------------------------------------------------------------------
// Returns the number of differing BYTES; *first / *last the first and last differing byte offsets (0 when equal).
// Every byte is compared: a dword at a time (n is a multiple of 4 for every eligible copy; a tail is compared byte by byte), and
// only a differing dword is looked at byte by byte.
static inline uint64_t n48_sk82_cmp(const uint8_t *a, const uint8_t *b, uint64_t n, uint64_t *first, uint64_t *last) {
    uint64_t nd = 0ull, f = 0ull, l = 0ull, i = 0ull;
    for (; i + 4ull <= n; i += 4ull) {
        const uint32_t x = (uint32_t)a[i] | (uint32_t)a[i + 1] << 8 | (uint32_t)a[i + 2] << 16 | (uint32_t)a[i + 3] << 24;
        const uint32_t y = (uint32_t)b[i] | (uint32_t)b[i + 1] << 8 | (uint32_t)b[i + 2] << 16 | (uint32_t)b[i + 3] << 24;
        if (x == y) continue;
        for (uint64_t j = i; j < i + 4ull; j++) if (a[j] != b[j]) { if (!nd) f = j; l = j; nd++; }
    }
    for (; i < n; i++) if (a[i] != b[i]) { if (!nd) f = i; l = i; nd++; }
    *first = f; *last = l;
    return nd;
}

// ---- the thread slots -----------------------------------------------------------------------------------------------------------
static inline int32_t n48_sk82_thr_find(const n48_sk82_tab *t, uintptr_t thr) {
    for (uint32_t i = 0; i < N48_SK82_THR; i++) if (thr && t->t[i].thr == thr) return (int32_t)i;
    return -1;
}
// This thread's earlier pending (a copy that returned before its scope opened) is dropped: its entry stays SHADOW.
static inline void n48_sk82_thr_release(n48_sk82_tab *t, uintptr_t thr) {
    const int32_t s = n48_sk82_thr_find(t, thr);
    if (s < 0) return;
    n48_sk82_thr *h = &t->t[s];
    n48_sk82_ent *e = &t->e[h->ent % N48_SK82_ENTRIES];
    if (e->pend && e->pthr == thr && e->pgen == h->gen) { e->pend = 0u; e->pthr = 0u; }
    h->thr = 0u;
}

// ---- THE DECISION (items 5-7) -------------------------------------------------------------------------------------------------
enum { N48_SK82_A_COPY = 0u, N48_SK82_A_SKIP = 1u };
enum { N48_SK82_CMP_NONE = 0u, N48_SK82_CMP_EQUAL = 1u, N48_SK82_CMP_DIFFER = 2u };
typedef struct {
    uint32_t act, would, cmp, cand, pend, cause, ent, logIt;
    uint64_t first, last, ndiff, fromCopy, n;
} n48_sk82_out;

// One eligible-or-not copy about to run (under the caller's lock). `src` is the whole source, just read (null: not eligible or
// the read failed). Returns o->act: SKIP only in SKIP mode, only for a TRUSTED entry of the same key whose image equals the
// source byte for byte, with every check passing; otherwise COPY, with a pending establishment set up when G0 allows it.
static inline void n48_sk82_try(n48_sk82_tab *t, const n48_sk82_world *w, uint32_t mode, const n48_sk82_key *k,
                                const n48_sk82_snap *sn, const uint8_t *src, uintptr_t thr, uint64_t copyNo, void *md,
                                n48_sk82_out *o) {
    o->act = N48_SK82_A_COPY; o->would = 0u; o->cmp = N48_SK82_CMP_NONE; o->cand = 0u; o->pend = 0u; o->cause = N48_SK82_CAUSES;
    o->ent = N48_SK82_ENTRIES; o->logIt = 0u; o->first = o->last = o->ndiff = o->fromCopy = o->n = 0ull;
    t->st.considered++;
    n48_sk82_thr_release(t, thr);
    if (mode != N48_SK82_MEASURE && mode != N48_SK82_SKIP) return;
    if (!src || !k->bytes || k->bytes > N48_SK82_MAX_BYTES || (k->bytes & 3ull)) return;
    t->st.eligible++;
    t->tick++;
    n48_sk82_drain(t, w->ev);
    // the entry: by resource; a changed key is a different mapping of it - never skipped on, replaced
    uint32_t idx = N48_SK82_ENTRIES;
    for (uint32_t i = 0; i < N48_SK82_ENTRIES; i++)
        if (t->e[i].state != N48_SK82_S_FREE && t->e[i].k.res == k->res) { idx = i; break; }
    int have = idx < N48_SK82_ENTRIES;
    if (have && !n48_sk82_key_eq(&t->e[idx].k, k)) {
        n48_sk82_inval(t, idx, N48_SK82_C_KEY);
        if (t->e[idx].pend) t->e[idx].pgen++;
        t->e[idx].pend = 0u;
        have = 0;
    }
    if (!have && idx == N48_SK82_ENTRIES) {
        t->st.noEntry++;
        for (uint32_t i = 0; i < N48_SK82_ENTRIES && idx == N48_SK82_ENTRIES; i++) if (t->e[i].state == N48_SK82_S_FREE) idx = i;
        if (idx == N48_SK82_ENTRIES) {   // LRU, preferring an entry with no copy running
            uint64_t best = ~0ull;
            for (uint32_t pass = 0; pass < 2u && idx == N48_SK82_ENTRIES; pass++)
                for (uint32_t i = 0; i < N48_SK82_ENTRIES; i++)
                    if ((pass || !t->e[i].pend) && t->e[i].use < best) { best = t->e[i].use; idx = i; }
            t->st.evicted++;
            if (t->e[idx].pend) { t->e[idx].pgen++; t->e[idx].pend = 0u; }
        }
    } else if (!have) {
        t->st.noEntry++;
    }
    n48_sk82_ent *e = &t->e[idx];
    o->ent = idx;
    if (have) {
        o->ndiff = n48_sk82_cmp(src, e->img, k->bytes, &o->first, &o->last);
        if (!o->ndiff) { o->cmp = N48_SK82_CMP_EQUAL; t->st.equal++; }
        else { o->cmp = N48_SK82_CMP_DIFFER; t->st.differ++; o->n = ++t->differN;
               o->logIt = (o->n <= N48_SK82_LOG_FIRST || !(o->n % N48_SK82_LOG_EVERY)) ? 1u : 0u; }
    }
    // THE SKIP CHECKS: equal image, TRUSTED, nobody copying it, then state, slots, counters, poison, a final drain.
    if (o->cmp == N48_SK82_CMP_EQUAL && e->state == N48_SK82_S_TRUSTED && !e->pend) {
        uint32_t cause = N48_SK82_CAUSES;
        uint64_t c[2], ev = 0ull;
        if (!n48_sk82_snap_eq(&e->snap, sn)) cause = N48_SK82_C_STATE;
        else if (n48_sk82_open_now(w, k->vram, k->vram + k->bytes)) cause = N48_SK82_C_OPEN;       // T ...
        else {
            n48_sk82_step(w, N48_SK82_AT_SKIP_MID);
            n48_sk82_read_cnt(w, k->vram, e->nb, c, &ev);                                           // ... then C
            if (c[0] != e->cnt[0] || c[1] != e->cnt[1]) cause = N48_SK82_C_COUNTER;
            else if (ev != e->every) cause = N48_SK82_C_EVERY;
            else if (n48_cg_poison_overlaps(w->poison, k->vram, k->vram + k->bytes)) cause = N48_SK82_C_POISON;
            else {
                n48_sk82_drain(t, w->ev);                                                           // the LAST check
                if (e->state != N48_SK82_S_TRUSTED) cause = N48_SK82_CAUSES + 1u;                  // counted by the drain
            }
        }
        if (cause == N48_SK82_CAUSES) {
            o->fromCopy = e->fromCopy;
            e->use = t->tick;
            if (mode == N48_SK82_SKIP) {
                o->act = N48_SK82_A_SKIP;
                t->st.skipped++; t->st.skippedBytes += k->bytes;
                o->n = ++t->skipN;
                o->logIt = (o->n <= N48_SK82_LOG_FIRST || !(o->n % N48_SK82_LOG_EVERY)) ? 1u : 0u;
                return;                                                                             // nothing else (item 8)
            }
            o->would = 1u; t->st.wouldSkip++;
            o->n = ++t->skipN;
            o->logIt = (o->n <= N48_SK82_LOG_FIRST || !(o->n % N48_SK82_LOG_EVERY)) ? 1u : 0u;
        } else {
            if (cause < N48_SK82_CAUSES) n48_sk82_inval(t, idx, cause);
            o->cause = cause;
        }
    }
    // THE COPY: the entry now describes what this copy is about to write (untrusted until its establishment).
    for (uint64_t i = 0; i < k->bytes; i++) e->img[i] = src[i];
    e->k = *k;
    e->state = N48_SK82_S_SHADOW;
    e->nb = n48_sk82_nbk(k->vram, k->bytes);
    e->use = t->tick;
    e->pend = 0u; e->pthr = 0u; e->ptaint = 0u; e->pgen++;
    // G0 (item 6): no committed frame in flight and no recent unknown write; the counters, THEN no overlapping open slot.
    if (n48_sk82_busy(w)) { t->st.inval[N48_SK82_C_FLIGHT]++; return; }
    uint64_t g0[2], g0e = 0ull;
    n48_sk82_read_cnt(w, k->vram, e->nb, g0, &g0e);                                                 // C ...
    n48_sk82_step(w, N48_SK82_AT_G0_MID);
    if (n48_sk82_open_now(w, k->vram, k->vram + k->bytes)) { t->st.inval[N48_SK82_C_OPEN]++; return; }   // ... then T
    int32_t s = -1;
    for (uint32_t i = 0; i < N48_SK82_THR && s < 0; i++) if (!t->t[i].thr) s = (int32_t)i;
    if (s < 0) { t->st.noThr++; return; }
    e->pend = 1u; e->pthr = thr; e->g0[0] = g0[0]; e->g0[1] = g0[1]; e->g0every = g0e; e->snap = *sn;
    n48_sk82_thr *h = &t->t[s];
    h->thr = thr; h->ent = idx; h->gen = e->pgen; h->ok = 0u; h->taint = 0u; h->lo = k->vram; h->hi = k->vram + k->bytes;
    h->copyNo = copyNo; h->md = md; h->boff = k->boff;
    // CANDIDATE (item 6): SKIP mode, the source equals the stored image, and this copy can establish: it runs a FULL verify.
    h->cand = (mode == N48_SK82_SKIP && o->cmp == N48_SK82_CMP_EQUAL) ? 1u : 0u;
    if (h->cand) t->st.candidate++;
    o->pend = 1u; o->cand = h->cand;
}

// The copier's result, before its scope closes (0.0.527: residency_copy_to_vram after its counters). `ok` = returned true with
// 0 mismatches, compared x 4 >= bytes, not re-tiled, no backing-sourced image, no switch-62 bump; `wBytes` what it wrote.
static inline void n48_sk82_result(n48_sk82_tab *t, uintptr_t thr, uint32_t ok, uint64_t wBytes) {
    const int32_t s = n48_sk82_thr_find(t, thr);
    if (s < 0) return;
    n48_sk82_thr *h = &t->t[s];
    h->ok = (ok && wBytes == h->hi - h->lo) ? 1u : 2u;
}
// A write into the copy's range that is not the source (shadercache, kernsub) by this thread inside the copy's scope.
static inline void n48_sk82_taint(n48_sk82_tab *t, uintptr_t thr) {
    const int32_t s = n48_sk82_thr_find(t, thr);
    if (s >= 0) t->t[s].taint = 1u;
}
// The per-thread flag fc_copy_chunk ORs into rpCand (read by the owning thread only).
static inline uint32_t n48_sk82_cand(const n48_sk82_tab *t, uintptr_t thr) {
    const int32_t s = n48_sk82_thr_find(t, thr);
    return s >= 0 ? t->t[s].cand : 0u;
}

// THE ESTABLISHMENT (item 6), after the copy's scope closed. `post` is the source
// re-read now. Returns 1 when the entry became TRUSTED. A scope that is not this thread's pending copy's returns 0 untouched.
static inline uint32_t n48_sk82_establish(n48_sk82_tab *t, const n48_sk82_world *w, uintptr_t thr, uint64_t lo, uint64_t hi,
                                          const n48_sk82_snap *sn, const uint8_t *post, uint32_t *causeOut) {
    *causeOut = N48_SK82_CAUSES;
    const int32_t s = n48_sk82_thr_find(t, thr);
    if (s < 0 || t->t[s].lo != lo || t->t[s].hi != hi) return 0u;
    const n48_sk82_thr h = t->t[s];
    t->t[s].thr = 0u;                                          // consumed, whatever follows
    n48_sk82_ent *e = &t->e[h.ent % N48_SK82_ENTRIES];
    uint32_t cause = N48_SK82_CAUSES;
    if (!e->pend || e->pthr != thr || e->pgen != h.gen) { t->st.inval[N48_SK82_C_RACE]++; *causeOut = N48_SK82_C_RACE; return 0u; }
    if (h.ok != 1u) { t->st.notFull++; e->pend = 0u; e->pthr = 0u; *causeOut = N48_SK82_CAUSES + 1u; return 0u; }
    if (h.taint) cause = N48_SK82_C_TAINT;
    else if (!post) { t->st.readFail++; cause = N48_SK82_C_SRC; }
    else {
        uint64_t f = 0ull, l = 0ull;
        if (n48_sk82_cmp(post, e->img, e->k.bytes, &f, &l)) cause = N48_SK82_C_SRC;
    }
    uint64_t c[2] = { 0ull, 0ull }, ev = 0ull;
    if (cause == N48_SK82_CAUSES) {
        if (!n48_sk82_snap_eq(&e->snap, sn)) cause = N48_SK82_C_STATE;
        else if (n48_sk82_busy(w)) cause = N48_SK82_C_FLIGHT;
        else if (n48_sk82_open_now(w, lo, hi)) cause = N48_SK82_C_OPEN;                              // T ...
        else {
            n48_sk82_step(w, N48_SK82_AT_EST_MID);
            n48_sk82_read_cnt(w, lo, e->nb, c, &ev);                                                // ... then C
            // our own scope's open: +1 on every bucket (item 6); anything else moved them
            if (c[0] != e->g0[0] + 1ull || (e->nb > 1u && c[1] != e->g0[1] + 1ull)) cause = N48_SK82_C_COUNTER;
            else if (ev != e->g0every) cause = N48_SK82_C_EVERY;
            else if (n48_cg_poison_overlaps(w->poison, lo, hi)) cause = N48_SK82_C_POISON;
            else {
                n48_sk82_drain(t, w->ev);                                                           // the LAST check
                if (e->ptaint) cause = e->ptaint - 1u;
            }
        }
    }
    if (cause == N48_SK82_CAUSES && e->ptaint) cause = e->ptaint - 1u;
    e->pend = 0u; e->pthr = 0u;
    if (cause != N48_SK82_CAUSES) { if (cause < N48_SK82_CAUSES) t->st.inval[cause]++; *causeOut = cause; return 0u; }
    e->state = N48_SK82_S_TRUSTED;
    e->cnt[0] = c[0]; e->cnt[1] = c[1]; e->every = ev;
    e->fromCopy = h.copyNo;
    t->st.established++;
    return 1u;
}

// ---- log lines (item 9): each <= 491 bytes at every field's widest (tests/gfx_sk82_test.cpp S2) ------------------------------------
/* build 0.0.528 item 5b: Apple's dirty range as recorded at res+0x138 (START) and res+0x140 (LENGTH),
 * two separate fields, beside the 0.0.527 dirtyEnd (their sum). */
#define N48_SK82_SKIP_FMT "sk82: %s #%llu resource=%p VRAM [%#llx,%#llx) VA %#llx bytes %#llx full-compare from copy #%llu " \
                          "dirtyEnd %#llx dirtyStart %#llx dirtyLen %#llx"
#define N48_SK82_DIFFER_FMT "sk82: DIFFER resource=%p first %#llx last %#llx ndiff %llu dirtyEnd %#llx dirtyStart %#llx dirtyLen " \
                            "%#llx (differ #%llu)"
#define N48_SK82_REPORT1_FMT "sk82: 82 %s%s; considered/eligible/no-entry/equal/differ %llu/%llu/%llu/%llu/%llu; SKIPPED %llu " \
                             "(%llu B) would-skip %llu; established/candidate/full-forced/evicted/not-full %llu/%llu/%llu/%llu/" \
                             "%llu; memory %s"
#define N48_SK82_REPORT2_FMT "sk82: invalid by cause counter/everything/unmap/target/poison/open/state/unknown/ws/wrap/busy/key/" \
                             "taint/src/race %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu; " \
                             "read-fail/no-slot %llu/%llu"
#define N48_SK82_REPORT2_ARGS(s) \
    (unsigned long long)(s)->inval[N48_SK82_C_COUNTER], (unsigned long long)(s)->inval[N48_SK82_C_EVERY], \
    (unsigned long long)(s)->inval[N48_SK82_C_UNMAP], (unsigned long long)(s)->inval[N48_SK82_C_TARGET], \
    (unsigned long long)(s)->inval[N48_SK82_C_POISON], (unsigned long long)(s)->inval[N48_SK82_C_OPEN], \
    (unsigned long long)(s)->inval[N48_SK82_C_STATE], (unsigned long long)(s)->inval[N48_SK82_C_UNKNOWN], \
    (unsigned long long)(s)->inval[N48_SK82_C_WS], (unsigned long long)(s)->inval[N48_SK82_C_WRAP], \
    (unsigned long long)(s)->inval[N48_SK82_C_FLIGHT], (unsigned long long)(s)->inval[N48_SK82_C_KEY], \
    (unsigned long long)(s)->inval[N48_SK82_C_TAINT], (unsigned long long)(s)->inval[N48_SK82_C_SRC], \
    (unsigned long long)(s)->inval[N48_SK82_C_RACE], (unsigned long long)(s)->readFail, (unsigned long long)(s)->noThr

#endif // N48_GFX_SK82_H
