// gfx_p94.h — build 0.0.538: SWITCH 94, PRESENT-TIME PROMOTION WITHOUT gXdLock, plus the glass
// instruments (the per-second present outcome line, the present-minus-gate and present-minus-retirement histograms). Pure,
// header-only, host-tested by tests/gfx_p94_test.cpp. `94 | M << 8`: M 1 ON (= 350), M 2 OFF (= 606, the default and the boot
// value); bare `94` reads.
//
// WHY: a present copies a new picture only when its plane's P is COMMITTED (spared AND its retirement observed). The
// retirement is observed by the flight ring's judged-frame poll (after the NEXT buffer's policy pass, inside the gXdLock region)
// or by switch 86's present-time re-check, which needs gXdLock by try-lock; in RUN AM the judge held gXdLock almost continuously
// (86 busy 317 of 318) and 323 presents were held UNKNOWN with the P spared and not yet seen retired.
//
// WHAT SWITCH 94 DOES (ON only; OFF nothing here is called and the present is 0.0.537's): for the ONE slot the present names,
// only when that slot is PENDING and its P was SPARED (n48_p86_candidate: spared == seq != 0), BEFORE switch 86's re-check:
//   1. find the P's flight ring entry BY THE P's OWN SEQ, lock-free, and snapshot it TWICE around its fence fields
//      (n48_p94_entry_snap): state COMMITTED and seq == the P's seq on both sides of the vram_off / want loads. A ring entry holds
//      one seq for its whole life (a push writes a NEW, never-reused seq; an entry leaves COMMITTED only to RETIRED / EXPIRED /
//      NOPED / NOT_RUN and is reused only after FREE), so an entry read COMMITTED with seq s both before and after the two loads
//      was the push of s throughout, and vram_off / want are that push's;
//   2. read the owned fence slot ONCE with the kext's reader - the expiry poll's and switch 86's own (ks_x_read:
//      navi48_vram_read_mm of one dword, which takes gVramMmLock, a leaf; with switch 37 (mmprio) ON a non-owner caller first
//      SPINS, yielding to the MM window's owner, up to gfx_mmprio.h's N48_MMPRIO_BOUND_US (4 ms) before taking it - no other lock,
//      but not a bounded-short wait: measured up to ~4.5 ms behind the judge) - and judge it with switch 86's own test
//      (n48_p86_fence_ours = n48_fr_poll_entry's retirement test: COMMITTED, want != 0, read ok, value == want (epoch|ordinal),
//      plus seq equality);
//   3. re-validate the entry after the read (same seq; COMMITTED, or RETIRED by the ring's own poll meanwhile): an entry that
//      went EXPIRED / NOPED / NOT_RUN / FREE is refused (86 under gXdLock could never see those mid-read);
//   4. OURS: mark the P retired and promote through the EXISTING three steps of gfx_present73.h (n48_p73_promote_ready, the CAS
//      PENDING -> COMMITTED, the re-check that reverts when a new P took the slot) - never n48_p73_retired itself, whose counters
//      are the gXdLock writers'. The mark is a compare-and-swap FROM the value read after the candidate check, so a newer P's
//      own mark (its ring poll's store) is never overwritten.
// THE FLIGHT RING IS NEVER WRITTEN (as switch 86): the entry stays COMMITTED and the ring's own poll retires it later (its
// n48_p73_retired then finds the slot COMMITTED and counts retireNoSlot), as in 0.0.531.
//
// build 0.0.539: step 1 also refuses (N48_P94_R_OFFPAGE, its own reason since the fix round; before any read) an entry whose vram_off is not a 4-byte slot
// INSIDE our own fence page (io->fence_page: gfx_fence828.h's N48_F828_FENCE_PAGE_OFF above the ring map's base; 0 = not mapped,
// refuse all): the one-dword read is never pointed anywhere else, whatever the ring entry holds.
//
// Pure: no lock, no clock, no register, no log of its own. The kext supplies the read through n48_p94_io.
#ifndef N48_GFX_P94_H
#define N48_GFX_P94_H

#include <stdint.h>
#include "gfx_present73.h"
#include "gfx_flightring.h"
#include "gfx_p86.h"

enum { N48_P94_OFF = 0u, N48_P94_ON = 1u };
/* The host test's interleaving point (tests/gfx_p94_test.cpp defines it to run other threads' actions between any two shared
 * accesses of the step). The kext never defines it: nothing. */
#ifndef N48_P94_YIELD
#define N48_P94_YIELD(k) ((void)0)
#endif
/* What one step came to (the return of n48_p94_step; counted in n48_p94 below). */
enum { N48_P94_R_NONE = 0u,      /* not asked: the slot is not PENDING-spared */
       N48_P94_R_NOENTRY,        /* no ring entry carries the P's seq (or no io) */
       N48_P94_R_NOTLIVE,        /* the entry is not COMMITTED with a real fence (before or after the read) */
       N48_P94_R_TORN,           /* the entry changed between the two snapshots around its fence fields */
       N48_P94_R_UNREAD,         /* the one-dword read failed */
       N48_P94_R_NOTYET,         /* read, not the entry's want */
       N48_P94_R_RACED,          /* the slot no longer names the same PENDING-spared P, or its mark moved, before the mark */
       N48_P94_R_PROMOTED,       /* OURS and promoted (READY, CAS, RE-CHECK) */
       N48_P94_R_LOST,           /* OURS, but READY / the CAS / the RE-CHECK did not promote (a racing gate / final / poll) */
       N48_P94_R_OFFPAGE,        /* build 0.0.539 fix round (S2): the entry's vram_off is not a slot of our fence page (no read) */
       N48_P94_RS };

typedef struct {
    uint32_t (*read32)(void *ctx, uint64_t vram_off, uint32_t *val);   /* the MM-window read of ONE dword; 1 = read */
    const n48_fr_ring *ring;
    void *ctx;
    uint64_t (*fence_page)(void *ctx);   /* build 0.0.539 (S2): our fence page's VRAM offset; 0 (or no function) = refuse all */
} n48_p94_io;
#define N48_P94_FENCE_PAGE_BYTES 0x1000u  /* = gfx_fence828.h's N48_F828_FENCE_PAGE_BYTES (the kext static_asserts it) */
/* 1 when [vo, vo + 4) is a 4-byte slot inside the fence page at `page` (0 = no page: never). Pure. */
static inline uint32_t n48_p94_in_page(uint64_t vo, uint64_t page)
{
    return page && !(vo & 3ull) && vo >= page && vo - page <= (uint64_t)N48_P94_FENCE_PAGE_BYTES - 4ull ? 1u : 0u;
}

/* The per-second present outcome (item 3), a window and its boot totals. Present-thread only (one present at a time). */
enum { N48_P94_OC_PRESENTS = 0u, N48_P94_OC_COPIED, N48_P94_OC_UNKNOWN, N48_P94_OC_OLDER, N48_P94_OC_REFUSED, N48_P94_OC_WITHDRAWN,
       N48_P94_OC_NOMATCH, N48_P94_OC_INCOMPLETE, N48_P94_OC_P94, N48_P94_OC_P95, N48_P94_OC_P86, N48_P94_OCS };
#define N48_P94_SEC_US    1000000ull   /* one line per second while armed */
#define N48_P94_SEC_LINES 240u         /* at most this many lines per arm (the rest counted) */
typedef struct {
    uint32_t armed;                 /* the arm state the last note saw */
    uint64_t t0;                    /* the window's start (us); the arm's start in a0 */
    uint64_t a0;
    uint32_t n[N48_P94_OCS];        /* this window */
    uint64_t tot[N48_P94_OCS];      /* since boot (73 ON presents) */
    uint32_t lines;                 /* printed this arm */
    uint64_t suppressed, windows;
} n48_p94_sec;
/* A due line: the window's counts, its start relative to the arm's start, and its length. */
typedef struct { uint32_t n[N48_P94_OCS]; uint64_t at_ms, len_ms; uint32_t line; } n48_p94_secline;

/* Per p73 slot: the gate time and the retirement-observed time of the P that set them (item 3's histograms). Written by the
 * gate / retirement sites (seq 0 first, the time, then the seq), read by the present (seq, time, seq). Log-only. */
typedef struct { uint32_t gseq, rseq; uint64_t gus, rus; } n48_p94_times;

typedef struct {
    uint32_t on;                                       /* N48_P94_*; the verb is its only writer */
    uint64_t asked, r[N48_P94_RS], reads;              /* steps asked (PENDING-spared with 94 ON), by outcome, fence reads */
    uint64_t atReplay;                                 /* steps asked by switch 95 for a remembered plane */
    n48_p94_sec sec;
    n48_p94_times tm[N48_P73_SLOTS];
    n48_wt_hist hGate, hRet;                           /* present - gate, present - retirement observed (us) */
    uint64_t notRetired, noGate;                       /* presents whose P had no retirement / no gate time recorded */
    uint64_t retNoStamp;                               /* presents whose P is COMMITTED without a stamp (the expiry poll) */
} n48_p94;

static inline uint32_t n48_p94_ld32(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline uint32_t n48_p94_ld32v(const volatile uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline uint64_t n48_p94_ld64(const uint64_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

/* STEP 1: the COMMITTED entry carrying `seq`, snapshotted twice around its fence fields. 1 = *voff / *want are the push of
 * `seq`; 0 = *why (NOENTRY / NOTLIVE / TORN). Reads only. */
static inline uint32_t n48_p94_entry_snap(const n48_fr_ring *r, uint32_t seq, uint64_t *voff, uint32_t *want, uint32_t *why,
                                          uint64_t page)
{
    *why = N48_P94_R_NOENTRY;
    if (!r || !seq) return 0u;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) {
        const n48_fr_entry *e = &r->e[i];
        const uint32_t st1 = n48_p94_ld32v(&e->state);
        if (st1 == N48_FR_FREE) continue;                  /* n48_fr_find's filter: a FREE entry keeps a stale seq */
        N48_P94_YIELD(1);
        if (n48_p94_ld32(&e->seq) != seq) continue;        /* SEQ EQUALITY: only the P's own flight answers for it */
        if (st1 != N48_FR_COMMITTED) { *why = N48_P94_R_NOTLIVE; return 0u; }
        N48_P94_YIELD(2);
        const uint64_t vo = n48_p94_ld64(&e->vram_off);
        const uint32_t w = n48_p94_ld32(&e->want);
        N48_P94_YIELD(3);
        if (n48_p94_ld32(&e->seq) != seq || n48_p94_ld32v(&e->state) != N48_FR_COMMITTED) { *why = N48_P94_R_TORN; return 0u; }
        if (w == 0u) { *why = N48_P94_R_NOTLIVE; return 0u; }   /* fence-less: only its timeout ends it */
        if (!n48_p94_in_page(vo, page)) { *why = N48_P94_R_OFFPAGE; return 0u; }   /* 0.0.539 S2: never read outside our fence page */
        *voff = vo; *want = w;
        return 1u;
    }
    return 0u;
}
/* STEP 3: after the read, the entry of `seq` is still the same push and did not end without its fence (EXPIRED / NOPED /
 * NOT_RUN / FREE refuse); RETIRED (the ring's own poll saw the same fence meanwhile) is accepted. */
static inline uint32_t n48_p94_entry_still(const n48_fr_ring *r, uint32_t seq)
{
    for (uint32_t i = 0; r && seq && i < N48_FR_CAPACITY; i++) {
        const n48_fr_entry *e = &r->e[i];
        const uint32_t st = n48_p94_ld32v(&e->state);
        if (st == N48_FR_FREE || n48_p94_ld32(&e->seq) != seq) continue;
        return (st == N48_FR_COMMITTED || st == N48_FR_RETIRED) ? 1u : 0u;
    }
    return 0u;
}
/* STEP 4's re-check: gfx_present73.h's n48_p73_promote_recheck, with its two counters incremented atomically (this runs on the
 * present thread, the others on the submit thread). */
static inline uint32_t n48_p94_recheck(n48_p73 *t, n48_p73_slot *s, uint32_t seq)
{
    if (__atomic_load_n(&s->seq, __ATOMIC_SEQ_CST) != seq) {   /* a new P took the slot: its PENDING is not ours to commit */
        uint32_t want = N48_P73_ST_COMMITTED;
        (void)__atomic_compare_exchange_n(&s->state, &want, N48_P73_ST_PENDING, false, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE);
        __atomic_fetch_add(&t->promoteReverted, 1ull, __ATOMIC_RELAXED);
        return 0u;
    }
    __atomic_fetch_add(&t->pCommitted, 1ull, __ATOMIC_RELAXED);
    return 1u;
}
/* THE GUARD, asked at the step's top and again right before the mark: switch 86's own candidate test - the slot is PENDING and
 * its P (the slot's seq) was SPARED by the hook's final (spared == seq != 0). A REFUSED, WITHDRAWN, UNKNOWN, COMMITTED or not
 * yet spared slot is never asked, never read for, never marked. */
static inline uint32_t n48_p94_candidate(const n48_p73 *t, uint32_t i, uint32_t *seq_out)
{
    return n48_p86_candidate(t, i, seq_out);
}
/* STEP 4: the mark (a CAS from the value read after the guard) and the existing READY / CAS / RE-CHECK. */
static inline uint32_t n48_p94_mark_promote(n48_p73 *t, uint32_t i, uint32_t seq)
{
    uint32_t seq2 = 0u;
    if (!n48_p94_candidate(t, i, &seq2) || seq2 != seq) return N48_P94_R_RACED;
    N48_P94_YIELD(7);
    n48_p73_slot *s = &t->s[i];
    uint32_t r0 = __atomic_load_n(&s->retired, __ATOMIC_SEQ_CST);
    N48_P94_YIELD(8);
    if (r0 != seq &&
        !__atomic_compare_exchange_n(&s->retired, &r0, seq, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) && r0 != seq)
        return N48_P94_R_RACED;   /* a newer P's reset or mark landed first: never overwrite it */
    N48_P94_YIELD(9);
    if (!n48_p73_promote_ready(s, seq)) return N48_P94_R_LOST;
    N48_P94_YIELD(10);
    if (!n48_p73_promote_cas(s)) return N48_P94_R_LOST;
    N48_P94_YIELD(11);
    return n48_p94_recheck(t, s, seq) ? N48_P94_R_PROMOTED : N48_P94_R_LOST;
}

/* THE STEP for the plane `key` (switch 94 ON only; the caller asks nothing while OFF). No lock of any kind is taken here; the
 * io's read takes the MM window's own leaf lock (gVramMmLock) - after, with switch 37 ON, a bounded mmprio spin of up to 4 ms
 * yielding to the window's owner. Returns N48_P94_R_*. */
static inline uint32_t n48_p94_step(n48_p73 *t, n48_p94 *c, uint64_t key, const n48_p94_io *io)
{
    const uint32_t i = n48_p73_find(t, key);
    uint32_t seq = 0u;
    if (!n48_p94_candidate(t, i, &seq)) return N48_P94_R_NONE;
    c->asked++;
    uint32_t r = N48_P94_R_NOENTRY, why = N48_P94_R_NOENTRY, want = 0u;
    uint64_t voff = 0ull;
    if (!io || !io->ring || !io->read32) r = N48_P94_R_NOENTRY;
    else if (!n48_p94_entry_snap(io->ring, seq, &voff, &want, &why, io->fence_page ? io->fence_page(io->ctx) : 0ull)) r = why;
    else {
        uint32_t val = 0u;
        c->reads++;
        N48_P94_YIELD(4);
        const uint32_t got = io->read32(io->ctx, voff, &val) ? 1u : 0u;   /* ONE read of the owned slot */
        N48_P94_YIELD(5);
        n48_fr_entry e;                                                    /* the snapshot, judged by 86's own test */
        e.seq = seq; e.at_us = 0ull; e.ordinal = 0u; e.vram_off = voff; e.want = want; e.state = N48_FR_COMMITTED;
        if (!n48_p86_fence_ours(&e, seq, got, val)) r = got ? N48_P94_R_NOTYET : N48_P94_R_UNREAD;   /* == want: epoch|ordinal */
        else if (!n48_p94_entry_still(io->ring, seq)) r = N48_P94_R_NOTLIVE;
        else { N48_P94_YIELD(6); r = n48_p94_mark_promote(t, i, seq); }
    }
    c->r[r]++;
    return r;
}

/* THE PRESENT's question with switches 94 and 86 (the kext's hw_p73_present, in this order): `on94` 0 asks nothing new - 0.0.537's
 * n48_p86_should_copy, unchanged. ON: the lock-free step first, then the unchanged question (86's try-lock re-check, if 86 is ON,
 * finds the slot already COMMITTED when 94 promoted). *r94 = the step's outcome. */
static inline uint32_t n48_p94_should_copy(uint32_t on73, uint32_t on86, uint32_t on94, n48_p73 *t, n48_p86 *c86, n48_p94 *c94,
                                           uint64_t key, const n48_p86_io *io86, const n48_p94_io *io94, uint32_t *reason,
                                           uint32_t *slot, uint32_t *r94)
{
    const uint32_t r = (on73 && on94) ? n48_p94_step(t, c94, key, io94) : N48_P94_R_NONE;
    if (r94) *r94 = r;
    return n48_p86_should_copy(on73, on86, t, c86, key, io86, reason, slot);
}

/* =============================================================================================================================
 * INSTRUMENTS (item 3; log-only, unswitched while 73 is ON): the per-second outcome line and the two histograms.
 * ============================================================================================================================= */
/* The outcome class of a present: N48_P73_HOLDS = copied, else the hold reason. */
static inline uint32_t n48_p94_oc_of(uint32_t why)
{
    return why == N48_P73_HOLDS ? N48_P94_OC_COPIED : why == N48_P73_HOLD_UNKNOWN ? N48_P94_OC_UNKNOWN
         : why == N48_P73_HOLD_OLDER ? N48_P94_OC_OLDER : why == N48_P73_HOLD_REFUSED ? N48_P94_OC_REFUSED
         : why == N48_P73_HOLD_WITHDRAWN ? N48_P94_OC_WITHDRAWN : why == N48_P73_HOLD_NOMATCH ? N48_P94_OC_NOMATCH
         : N48_P94_OC_INCOMPLETE;
}
/* Count one event of class `oc` (a present's outcome or a flag) in the window and the totals. */
static inline void n48_p94_sec_count(n48_p94_sec *w, uint32_t oc)
{
    if (oc >= N48_P94_OCS) return;
    w->n[oc]++; w->tot[oc]++;
}
/* The window's clock, asked once per present AFTER its counts: while armed, a window of >= 1 s closes into *out (1 = print it;
 * past N48_P94_SEC_LINES per arm the line is counted suppressed and 0 returned). An arm's start opens a fresh window and a fresh
 * line budget; while not armed the window is only reset (the totals keep counting). */
static inline uint32_t n48_p94_sec_tick(n48_p94_sec *w, uint32_t armed, uint64_t now_us, n48_p94_secline *out)
{
    if (!armed) {
        w->armed = 0u;
        for (uint32_t k = 0; k < N48_P94_OCS; k++) w->n[k] = 0u;
        return 0u;
    }
    if (!w->armed) {                                   /* the arm started: this present opens the first window */
        w->armed = 1u; w->t0 = now_us; w->a0 = now_us; w->lines = 0u;
        return 0u;
    }
    if (now_us < w->t0 || now_us - w->t0 < N48_P94_SEC_US) return 0u;
    uint32_t due = 0u;
    w->windows++;
    if (w->lines < N48_P94_SEC_LINES) {
        w->lines++;
        for (uint32_t k = 0; k < N48_P94_OCS; k++) out->n[k] = w->n[k];
        out->at_ms = (w->t0 - w->a0) / 1000ull; out->len_ms = (now_us - w->t0) / 1000ull; out->line = w->lines;
        due = 1u;
    } else {
        w->suppressed++;
    }
    for (uint32_t k = 0; k < N48_P94_OCS; k++) w->n[k] = 0u;
    w->t0 = now_us;
    return due;
}
/* The gate / retirement-observed stamps for slot `i` (seq 0 first, then the time, then the seq). */
static inline void n48_p94_stamp(uint32_t *sp, uint64_t *up, uint32_t seq, uint64_t now_us)
{
    __atomic_store_n(sp, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(up, now_us, __ATOMIC_RELEASE);
    __atomic_store_n(sp, seq, __ATOMIC_RELEASE);
}
static inline void n48_p94_note_gate(n48_p94 *c, uint32_t i, uint32_t seq, uint64_t now_us)
{
    if (i < N48_P73_SLOTS && seq) n48_p94_stamp(&c->tm[i].gseq, &c->tm[i].gus, seq, now_us);
}
/* A retirement observed for token seq `seq` (the ring's two polls, 94's or 86's promotion): the slot whose P it is. */
static inline void n48_p94_note_ret(n48_p94 *c, const n48_p73 *t, uint32_t seq, uint64_t now_us)
{
    for (uint32_t i = 0; seq && i < N48_P73_SLOTS; i++)
        if (n48_p73_ld64(&t->s[i].key) && __atomic_load_n(&t->s[i].seq, __ATOMIC_ACQUIRE) == seq) {
            n48_p94_stamp(&c->tm[i].rseq, &c->tm[i].rus, seq, now_us);
            return;
        }
}
/* The stamp of `seq` in (sp, up): 1 = *us is it. */
static inline uint32_t n48_p94_stamp_of(const uint32_t *sp, const uint64_t *up, uint32_t seq, uint64_t *us)
{
    if (!seq || __atomic_load_n(sp, __ATOMIC_ACQUIRE) != seq) return 0u;
    const uint64_t u = __atomic_load_n(up, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(sp, __ATOMIC_ACQUIRE) != seq) return 0u;
    *us = u;
    return 1u;
}
/* The present's histogram notes for slot `i` whose P is `seq`: present - gate, present - retirement observed. A P COMMITTED
 * without a stamp (retired by the expiry poll, whose site is not stamped) is counted apart, not in the histogram. */
static inline void n48_p94_hist_present(n48_p94 *c, const n48_p73 *t, uint32_t i, uint64_t now_us)
{
    if (i >= N48_P73_SLOTS) return;
    const uint32_t seq = __atomic_load_n(&t->s[i].seq, __ATOMIC_ACQUIRE);
    if (!seq) return;
    uint64_t us = 0ull;
    if (n48_p94_stamp_of(&c->tm[i].gseq, &c->tm[i].gus, seq, &us)) n48_wt_note(&c->hGate, now_us >= us ? now_us - us : 0ull);
    else c->noGate++;
    if (n48_p94_stamp_of(&c->tm[i].rseq, &c->tm[i].rus, seq, &us)) n48_wt_note(&c->hRet, now_us >= us ? now_us - us : 0ull);
    else if (__atomic_load_n(&t->s[i].state, __ATOMIC_ACQUIRE) == N48_P73_ST_COMMITTED) c->retNoStamp++;
    else c->notRetired++;
}

static inline const char *n48_p94_r_name(uint32_t r)
{
    static const char *const n[N48_P94_RS] = { "none", "no ring entry", "entry not live", "entry torn", "read failed",
                                               "fence not yet", "raced", "PROMOTED", "lost to a race", "outside the fence page" };
    return r < N48_P94_RS ? n[r] : "?";
}

#define N48_P94_ON_TXT "ON (a PENDING spared P's fence is read at the present without gXdLock)"
#define N48_P94_OFF_TXT "OFF (default)"
/* The switch-94 report (the bare `gfxneuter 94`, any M), five lines. FMT args: state, how, steps asked, promoted, fence reads, steps
 * asked at a 95 replay. */
#define N48_P94_FMT "present94: switch 94 %s%s. lock-free steps %llu, fence OURS -> PROMOTED %llu, fence reads %llu, asked at a 95 " \
    "replay %llu"
/* args: no entry, not live, torn, unread, not yet, raced, lost, (0.0.539 fix round, S2) outside the fence page. */
#define N48_P94_FMT1B "present94: step outcomes: no ring entry %llu, entry not live %llu, torn %llu, read failed %llu, fence not " \
    "yet %llu, raced %llu, lost to a race %llu, outside the fence page %llu"
/* args: presents, copied, unknown, older, refused, withdrawn, no-match, incomplete. */
#define N48_P94_FMT2 "present94: presents since boot (73 ON) %llu: copied %llu, held unknown %llu older %llu refused %llu " \
    "withdrawn %llu no-match %llu incomplete %llu"
/* args: 94, 95, 86, windows, lines this arm, suppressed, no gate stamp, not retired, retired unstamped. */
#define N48_P94_FMT3 "present94: promoted by 94 %llu, replayed by 95 %llu, promoted by 86 %llu; windows %llu, lines this arm %u, " \
    "suppressed %llu; P without a gate stamp %llu, not yet retired %llu, retired unstamped (expiry poll) %llu"
/* The per-second line (armed only). args: line, window start ms after the arm's first present, window ms, then the counts. */
#define N48_P94_SEC_FMT "present94 sec %u: +%llu ms (%llu ms): presents %u copied %u; held unknown %u older %u refused %u " \
    "withdrawn %u no-match %u incomplete %u; promoted 94 %u 86 %u; replayed 95 %u"
#define N48_P94_SEC_ARGS(L) (L).line, (unsigned long long)(L).at_ms, (unsigned long long)(L).len_ms, \
    (L).n[N48_P94_OC_PRESENTS], (L).n[N48_P94_OC_COPIED], (L).n[N48_P94_OC_UNKNOWN], (L).n[N48_P94_OC_OLDER], \
    (L).n[N48_P94_OC_REFUSED], (L).n[N48_P94_OC_WITHDRAWN], (L).n[N48_P94_OC_NOMATCH], (L).n[N48_P94_OC_INCOMPLETE], \
    (L).n[N48_P94_OC_P94], (L).n[N48_P94_OC_P86], (L).n[N48_P94_OC_P95]
/* The capped PROMOTED line (the first 8). args: present number, plane VRAM, slot, P seq, line. */
#define N48_P94_LINE_FMT "present94: PROMOTED present #%llu plane VRAM %#llx slot %u P seq %u without gXdLock (fence OURS, " \
    "line %u of 8)"

#endif /* N48_GFX_P94_H */
