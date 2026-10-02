// gfx_p86.h — build 0.0.531: SWITCH 86, THE PRESENT-TIME RETIREMENT RE-CHECK, plus the read-only glass
// instruments (the UNKNOWN split, the re-check counters, wall-time histograms). Pure, header-only, host-tested by
// tests/gfx_p86_test.cpp. `86 | M << 8`: M 1 ON (= 342), M 2 OFF (= 598, the default and the boot value); bare `86` reads.
//
// WHY: with switch 73 ON a plane's newest P is copied only once its slot is COMMITTED, i.e. once BOTH the hook's final
// outcome SPARED it and its flight RETIRED by its own fence (gfx_present73.h 0.0.519). The retirement is observed only by the
// flight ring's two polls - the judged-frame poll and switch 65's expiry poll - i.e. on WindowServer's NEXT submission. A present
// that arrives after the P's GPU completion but before that next submission reads the slot PENDING and is HELD (UNKNOWN), and
// nothing re-checks it: in RUN AB 80 of 102 mid-arm presents were held this way while 98% of Ps committed.
//
// WHAT SWITCH 86 DOES (ON only; OFF the present is 0.0.530's, byte for byte in its answers): before switch 73's question, for the
// ONE slot the present names, and only when that slot is PENDING and its P was SPARED (spared == seq != 0):
//   1. try-lock the kext's decide lock (the kext passes IOLockTryLock(gXdLock): NEVER a blocking lock - busy = held as today);
//   2. under it, re-ask the slot (the gate runs under the same lock, so it cannot re-key the slot now) and find the P's flight
//      ring entry BY THE P's OWN SEQ (n48_fr_find);
//   3. read that entry's owned fence slot ONCE, and only when the entry is one the judged-frame poll would read (COMMITTED with a
//      real fence: n48_fr_next_poll's filter) - the kext passes the expiry poll's own reader (ks_x_read = navi48_vram_read_mm of
//      one dword; the judged-frame poll's vram_read_sub is the same read under a DECIDE sub-tag, a no-op outside DECIDE);
//   4. judge it with n48_fr_poll_entry's OWN test (n48_p86_fence_ours: COMMITTED, want != 0, read ok, value == want) PLUS the
//      entry's seq == the P's seq (the entry was found by it, re-checked here);
//   5. OURS: mark the P retired through the EXISTING n48_p73_retired (the same marks, the same READY / CAS / RE-CHECK promotion,
//      the same counters) - the present then proceeds through the unchanged n48_p73_present (switch 80's hold, Part E's
//      monotonic NEWER test, the flip-mode path after it). Not OURS: nothing changes (held as today).
// THE FLIGHT RING IS NOT WRITTEN HERE (the decision, with the code: gfx_flightring.h's writer discipline is the commit path's,
// and a ring retirement carries bookkeeping this context must not duplicate - the OUT-OF-ORDER stop count and line, the arm's
// fence-OURS / EOP latency, switch 76's spill sync, the capped RETIRED line, newest/prevNewest for the defer verdict). The entry
// stays COMMITTED - the fail-safe direction for the unmap deferral (it keeps waiting) - and the ring's own judged-frame poll
// retires it at the next submission; its p73_retired then finds the slot already COMMITTED and counts retireNoSlot (a counter).
//
// Pure: no lock, no clock, no register, no log of its own. The kext supplies the lock and the read through n48_p86_io.
#ifndef N48_GFX_P86_H
#define N48_GFX_P86_H

#include <stdint.h>
#include "gfx_present73.h"
#include "gfx_flightring.h"

enum { N48_P86_OFF = 0u, N48_P86_ON = 1u };
/* What one re-check came to (the return of n48_p86_recheck; counted in n48_p86 below). */
enum { N48_P86_R_NONE = 0u,      /* not asked: the slot is not PENDING-spared (every other state goes its existing way) */
       N48_P86_R_BUSY,           /* the try-lock failed: held as today */
       N48_P86_R_RACED,          /* under the lock the slot no longer names the same PENDING-spared P */
       N48_P86_R_NOENTRY,        /* no flight ring entry carries the P's seq */
       N48_P86_R_NOTLIVE,        /* the entry is not COMMITTED with a real fence (RETIRED / EXPIRED / NOPED / NOT_RUN / fence-less) */
       N48_P86_R_NOTYET,         /* read, not OURS (unreadable, unchanged, another value) */
       N48_P86_R_PROMOTED,       /* OURS and promoted by n48_p73_retired */
       N48_P86_R_LOST,           /* OURS but n48_p73_retired did not promote (a racing final / gate) */
       N48_P86_RS };

/* The kext's side: the try-lock (1 = taken), its unlock, the one-dword VRAM read (1 = read), the flight ring. */
typedef struct {
    uint32_t (*trylock)(void *ctx);
    void (*unlock)(void *ctx);
    uint32_t (*read32)(void *ctx, uint64_t vram_off, uint32_t *val);
    const n48_fr_ring *ring;
    void *ctx;
} n48_p86_io;

typedef struct {
    uint32_t on;                                       /* N48_P86_*; the verb is its only writer */
    uint64_t r[N48_P86_RS];                            /* re-checks by outcome (r[NONE] is not counted) */
    uint64_t asked;                                    /* re-checks asked (PENDING-spared with 86 ON) */
    /* THE UNKNOWN SPLIT (unconditional while 73 is ON, 86 ON or OFF; decision-inert): a present held UNKNOWN, by what its slot
     * read just after the answer - PENDING whose P was spared, PENDING otherwise (not yet spared: final not seen), or a state
     * that is not PENDING (UNKNOWN: an unjudged frame, a non-P writer, an unresolved CB0, a COMMIT with no seq). */
    uint64_t unkPendSpared, unkPendOther, unkState;
    uint64_t decideBusyByUs;                           /* the judge's own try-lock failed while this re-check held the lock */
} n48_p86;

/* n48_fr_poll_entry's OWN retirement test (gfx_flightring.h: COMMITTED, want != 0, read ok, value == want), plus the entry's seq
 * naming THIS P (the flight of another seq never answers for it). Reads only; nothing is written. */
static inline uint32_t n48_p86_fence_ours(const n48_fr_entry *e, uint32_t seq, uint32_t got, uint32_t val)
{
    if (!e || !seq || e->seq != seq) return 0u;
    if (e->state != N48_FR_COMMITTED) return 0u;
    if (e->want == 0u) return 0u;
    if (!got || val != e->want) return 0u;
    return 1u;
}

/* Is slot `i` a re-check candidate - PENDING, and its P (the slot's seq) SPARED by the hook's final? *seq_out = that P's seq. */
static inline uint32_t n48_p86_candidate(const n48_p73 *t, uint32_t i, uint32_t *seq_out)
{
    if (seq_out) *seq_out = 0u;
    if (!t || i >= N48_P73_SLOTS) return 0u;
    const n48_p73_slot *s = &t->s[i];
    if (__atomic_load_n(&s->state, __ATOMIC_SEQ_CST) != N48_P73_ST_PENDING) return 0u;
    const uint32_t seq = __atomic_load_n(&s->seq, __ATOMIC_SEQ_CST);
    if (!seq || __atomic_load_n(&s->spared, __ATOMIC_SEQ_CST) != seq) return 0u;
    if (seq_out) *seq_out = seq;
    return 1u;
}

/* THE RE-CHECK for the plane `key` (switch 86 ON only; the caller asks nothing while OFF). Returns N48_P86_R_*. */
static inline uint32_t n48_p86_recheck(n48_p73 *t, n48_p86 *c, uint64_t key, const n48_p86_io *io)
{
    const uint32_t i = n48_p73_find(t, key);
    uint32_t seq = 0u;
    if (!n48_p86_candidate(t, i, &seq)) return N48_P86_R_NONE;
    c->asked++;
    uint32_t r;
    if (!io || !io->trylock || !io->trylock(io->ctx)) { r = N48_P86_R_BUSY; c->r[r]++; return r; }
    uint32_t seq2 = 0u, idx = N48_FR_CAPACITY;
    if (!n48_p86_candidate(t, i, &seq2) || seq2 != seq) r = N48_P86_R_RACED;
    else if (!io->ring || !n48_fr_find(io->ring, seq, &idx) || idx >= N48_FR_CAPACITY) r = N48_P86_R_NOENTRY;
    else {
        const n48_fr_entry *e = &io->ring->e[idx];
        if (e->state != N48_FR_COMMITTED || e->want == 0u || !io->read32) r = N48_P86_R_NOTLIVE;
        else {
            uint32_t val = 0u;
            const uint32_t got = io->read32(io->ctx, e->vram_off, &val) ? 1u : 0u;   /* ONE read of the owned slot */
            if (!n48_p86_fence_ours(e, seq, got, val)) r = N48_P86_R_NOTYET;
            else r = n48_p73_retired(t, seq) ? N48_P86_R_PROMOTED : N48_P86_R_LOST;
        }
    }
    io->unlock(io->ctx);
    c->r[r]++;
    return r;
}

/* THE UNKNOWN SPLIT (log-only): called for a present that switch 73 held with reason UNKNOWN. */
static inline void n48_p86_split(const n48_p73 *t, n48_p86 *c, uint32_t slot)
{
    uint32_t seq = 0u;
    if (slot < N48_P73_SLOTS && __atomic_load_n(&t->s[slot].state, __ATOMIC_ACQUIRE) == N48_P73_ST_PENDING)
        (n48_p86_candidate(t, slot, &seq) ? c->unkPendSpared : c->unkPendOther)++;
    else c->unkState++;
}

/* THE PRESENT's question with switch 86: `on86` 0 (OFF) asks nothing new - switch 73's own n48_p73_should_copy, unchanged. ON: the
 * re-check first, then the unchanged question. The UNKNOWN split is counted either way (73 ON). */
static inline uint32_t n48_p86_should_copy(uint32_t on73, uint32_t on86, n48_p73 *t, n48_p86 *c, uint64_t key,
                                           const n48_p86_io *io, uint32_t *reason, uint32_t *slot)
{
    if (!on73) return 1u;
    if (on86) (void)n48_p86_recheck(t, c, key, io);
    uint32_t why = N48_P73_HOLDS, i = N48_P73_SLOTS;
    const uint32_t copy = n48_p73_should_copy(on73, t, key, &why, &i);
    if (!copy && why == N48_P73_HOLD_UNKNOWN) n48_p86_split(t, c, i);
    if (reason) *reason = why;
    if (slot) *slot = i;
    return copy;
}

/* =============================================================================================================================
 * WALL-TIME HISTOGRAMS (read-only, unconditional): per call, in microseconds. Buckets: < 10, < 50, < 100, < 500 us, < 1, < 5,
 * < 20, < 100 ms, >= 100 ms. Relaxed atomics (the hook and the page-in run on several threads); max by a CAS loop.
 * ============================================================================================================================= */
#define N48_WT_BUCKETS 9u
typedef struct { uint64_t b[N48_WT_BUCKETS]; uint64_t n, sum_us, max_us; } n48_wt_hist;
static inline uint32_t n48_wt_bucket(uint64_t us)
{
    static const uint64_t edge[N48_WT_BUCKETS - 1u] = { 10u, 50u, 100u, 500u, 1000u, 5000u, 20000u, 100000u };
    uint32_t k = 0u;
    while (k < N48_WT_BUCKETS - 1u && us >= edge[k]) k++;
    return k;
}
static inline void n48_wt_note(n48_wt_hist *h, uint64_t us)
{
    __atomic_fetch_add(&h->b[n48_wt_bucket(us)], 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&h->n, 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&h->sum_us, us, __ATOMIC_RELAXED);
    uint64_t m = __atomic_load_n(&h->max_us, __ATOMIC_RELAXED);
    while (us > m && !__atomic_compare_exchange_n(&h->max_us, &m, us, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
}
/* args: name, n, mean us, max us, then the nine buckets */
#define N48_WT_FMT "glass531: %s wall time: %llu call(s), mean %llu us, max %llu us; <10us %llu, <50us %llu, <100us %llu, " \
    "<500us %llu, <1ms %llu, <5ms %llu, <20ms %llu, <100ms %llu, >=100ms %llu"
#define N48_WT_ARGS(name, h) (name), (unsigned long long)(h)->n, \
    (unsigned long long)((h)->n ? (h)->sum_us / (h)->n : 0ull), (unsigned long long)(h)->max_us, \
    (unsigned long long)(h)->b[0], (unsigned long long)(h)->b[1], (unsigned long long)(h)->b[2], (unsigned long long)(h)->b[3], \
    (unsigned long long)(h)->b[4], (unsigned long long)(h)->b[5], (unsigned long long)(h)->b[6], (unsigned long long)(h)->b[7], \
    (unsigned long long)(h)->b[8]

/* build 0.0.531 item 3 ('s instrument bug): switch 73's COPIED line with the LAST gate seq (the kext's gXdCmGateSeqLast,
 * written only where the gate sets gXdCmGateSeq). Through 0.0.530 the line (N48_P73_COPY_FMT, kept unchanged for the frozen copies)
 * printed gXdCmGateSeq, which is cleared once per frame and so always read 0 at a present. Same arguments. */
#define N48_P86_COPY_FMT "present73: COPIED present #%llu (perform counter) plane VRAM %#llx slot %u: the slot's P seq %u, " \
    "last gate seq %u (copy line %u of 8)"

/* The switch-86 report (the bare `gfxneuter 86`, any M): state, the re-check counters, the UNKNOWN split. */
#define N48_P86_FMT "present86: switch 86 %s%s. re-checks asked %llu: try-lock busy %llu, raced %llu, no ring entry %llu, entry " \
    "not live %llu, fence not yet %llu, fence OURS -> promoted %llu (lost to a race %llu); judge busy while held %llu"
#define N48_P86_FMT2 "present86: HELD UNKNOWN split (73 ON, 86 either): PENDING spared (retirement not yet seen) %llu, PENDING not " \
    "spared %llu, state UNKNOWN %llu; switch 73 held unknown %llu, copied %llu; last gate seq %u"

#endif /* N48_GFX_P86_H */
