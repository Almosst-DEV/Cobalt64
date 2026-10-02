// gfx_rnforgive.h — build 0.0.523 (notes/design/RING-NEUTER-FORGIVE.md, REVISION 2 governs; ).
// SWITCH 77: THE RING-NEUTER FORGIVENESS, PURE HALF.
//
// THE HAZARD: `w->ring_neuters = v[N48_DEPC_GN_IBS]` is gGn.ibs, boot-global and written only by
// `gGn.ibs += done` in the ring walk. One committed frame whose ring exemption the walk refused (heap-gen) NOPs its IB(s) and
// latches the rung at 1: every later translated frame refuses DEPENDENCY-STALE (ring-neuter) for the rest of the boot (RUN R,
// RUN S). Switch 75 (gfx_walknop75.h) retires such a flight at once as NOPED when the walk PROVED every IB NOPed before the
// doorbell, but never touched this rung.
//
// WHAT A NOPed COMMITTED FRAME LEAVES BEHIND, AND WHO COVERS IT (the contract's findings):
//   - R1 (the ledger): covered already - the retire site queues the un-feed of the frame's own token (n48_dl_unfeed_queue).
//   - R3/R5′ (the hazard set): NOT covered - a committed frame is never noted in gR5Ring (the note site is `action !=
//     TRANSLATE`). So this header SAVES the committed frame's R5′ record at decide (resolved exactly as the held-back site
//     resolves it), and the DRAIN notes it into the CURRENT arm scope's R5′ ring before it may grant anything.
//   - Only the CONSUMER path (switch 28) asks R1/R3, and only R5′ (switch 30) asks the hazard set: gfx_dep.h's fill subtracts
//     a grant only while both are live (consumer_enumerated 1, r5_mode 1). On the legacy path the rung stays raw.
//
// THE FLOW (the kext, AppleHardwareHook.cpp):
//   SAVE   gfxsrc_decide_frame, right after n48_dl_feed, a COMMITTED frame (commitOk, TRANSLATE, gate seq, 28 + 30): its r5f,
//          `out_of_scope` forced 0, resolved through its own vm, the short-read rule -> n48_rn_save(seq, scope, nib, &r5f).
//   QUEUE  hook_gfxCommitIB, inside `if (wnDone)` (switch 75's PROVEN NOP retired the flight): n48_rn_queue(seq, n). NO grant.
//   DRAIN  gfxsrc_decide_frame, under gXdLock, immediately after `gXpScopeSeq = ...` (after n48_dl_unfeed_drain, before
//          the read loop, the policy and the gather): n48_rn_drain -> each entry noted into R5′, READABLE ones granted.
//   GATHER s.rn_enabled / s.rn_forgive = forgiven / s.rn_clause = n48_rn_verdict(...) -> gfx_dep.h's fill.
// `forgiven` is written ONLY by n48_rn_drain. A grant at the retire site would let a concurrent decide see a forgiven rung
// while the ledger still held the NOPed frame's feed (planted break B3/P3, caught by T8).
//
// Plain C that also compiles as C++; no heap, no libc beyond stdint. Storage lives in the kext as file-scope statics
// (the store is 4 x sizeof(n48_rn_rec), ~4.9 KiB of .bss - the gR5fScratch precedent).
#ifndef N48_GFX_RNFORGIVE_H
#define N48_GFX_RNFORGIVE_H

#include <stdint.h>
#include "gfx_dep.h"

#define N48_RN_STORE   4u   /* saved records (committed frames awaiting their walk's answer) */
#define N48_RN_QUEUE   8u   /* retired-as-NOPED flights awaiting the next drain (the un-feed queue's size) */
#define N48_RN_NIB_MAX 4u   /* the submission shape's IB bound (N48_GFXN_EX_MAX_IBS) */

typedef struct {
    uint32_t used;          /* 1 while the record awaits a drain */
    uint32_t seq;           /* the gate/token seq (gXdCmGateSeq) - the seq space the un-feed relies on (lf.tok) */
    uint32_t arm_seq;       /* gXpScopeSeq at the save: a grant is bound to THIS arm scope */
    uint32_t nib;           /* the submission's DECLARED IB count */
    n48_r5_frame f;         /* the frame's R5′ record, resolved at the save */
} n48_rn_rec;

typedef struct {
    n48_rn_rec r[N48_RN_STORE];
    uint32_t next;          /* the slot the next save overwrites when none is free (oldest first) */
} n48_rn_store;

/* The queue: one writer (hook_gfxCommitIB, Apple's submit thread, no lock - the un-feed queue's shape), drained under gXdLock.
 * A full queue counts `overflow` and marks `lost`: nothing is granted for a lost entry (fail-closed - the rung stays raw). */
typedef struct { uint32_t seq[N48_RN_QUEUE]; uint32_t nib[N48_RN_QUEUE]; uint32_t n, overflow, lost; } n48_rn_q;

#define N48_RN_GRANT_LOG 8u
typedef struct {
    uint64_t saved, saveMiss, queued, qOverflow, drained, granted, grantedIbs, blind, noRecord, wrongScope, nibBad;
    uint64_t bounded;       /* a record R5′ bucketed BOUNDED (never granted; `out_of_scope` is forced 0 at the save) */
    uint64_t lostDrains;    /* drains that found the queue `lost` */
    uint64_t retiredIbs;    /* IBs of flights switch 75 retired as NOPED and queued here (the verdict's ceiling) */
    uint64_t forgiven;      /* THE AMOUNT: boot-monotone, written ONLY by n48_rn_drain */
    /* this drain's grants, for the kext's capped per-grant line (reset at each drain) */
    uint32_t g_n;
    uint32_t g_seq[N48_RN_GRANT_LOG], g_nib[N48_RN_GRANT_LOG], g_pages[N48_RN_GRANT_LOG];
    /* build 0.0.548 item D: this drain's BLIND notes (store slot, gate seq, IB count), so the kext can print switch 104's
     * first-BLIND line for a drained record too (reset at each drain; the record stays in its slot until the next save). APPENDED. */
    uint32_t b_n;
    uint32_t b_slot[N48_RN_GRANT_LOG], b_seq[N48_RN_GRANT_LOG], b_nib[N48_RN_GRANT_LOG];
} n48_rn_stats;

/* SAVE: a committed frame's resolved R5′ record, keyed by its gate seq. seq 0, scope 0 or a null record is not saved (counted).
 * A record already held for the same seq is replaced; otherwise the first free slot, else the oldest (`next`, round robin):
 * an overwritten record is simply gone, and its flight then drains as "no record" - no grant (fail-closed). */
static inline uint32_t n48_rn_save(n48_rn_store *st, n48_rn_stats *s, uint32_t seq, uint32_t arm_seq, uint32_t nib,
                                   const n48_r5_frame *f)
{
    if (!st || !s) return 0u;
    if (!seq || !arm_seq || !f) { s->saveMiss++; return 0u; }
    uint32_t slot = N48_RN_STORE;
    for (uint32_t i = 0; i < N48_RN_STORE; i++) if (st->r[i].used && st->r[i].seq == seq) { slot = i; break; }
    if (slot == N48_RN_STORE) for (uint32_t i = 0; i < N48_RN_STORE; i++) if (!st->r[i].used) { slot = i; break; }
    if (slot == N48_RN_STORE) { slot = st->next % N48_RN_STORE; st->next = (slot + 1u) % N48_RN_STORE; }
    n48_rn_rec *r = &st->r[slot];
    r->seq = seq; r->arm_seq = arm_seq; r->nib = nib; r->f = *f;
    r->used = 1u;
    s->saved++;
    return 1u;
}

/* QUEUE: the retire site. `seq` 0 is refused (never a real token). NO GRANT HAPPENS HERE (B3/P3). */
static inline void n48_rn_queue(n48_rn_q *q, n48_rn_stats *s, uint32_t seq, uint32_t nib)
{
    if (!q || !seq) return;
    const uint32_t n = __atomic_load_n(&q->n, __ATOMIC_RELAXED);   /* fix pass SHOULD 2: the one writer's own count */
    if (n >= N48_RN_QUEUE) { q->overflow++; q->lost = 1u; if (s) s->qOverflow++; return; }
    q->seq[n] = seq; q->nib[n] = nib;                               /* the entry first ... */
    __atomic_store_n(&q->n, n + 1u, __ATOMIC_RELEASE);              /* ... then publish it */
    if (s) s->queued++;
}

/* 0.0.523 fix pass MUST-FIX 1 (HIGH review): THE GRANT CARRIES ITS OWN UN-FEED. The retire site's separate un-feed queue
 * (gfx_desc_port.h) is lock-free; if two commitIB calls ever overlap, a push can be lost between its drain's read and reset of
 * `n`, and the ledger would still vouch (R1) for a NOPed frame whose ring count this drain forgives. So the drain, under
 * gXdLock, takes the frame's entries back itself (the kext: n48_dl_unfeed_tok, idempotent) BEFORE it notes and grants. */
typedef void (*n48_rn_unfeed_fn)(void *ud, uint32_t seq);

/* DRAIN: under gXdLock, one queued entry at a time, in the CURRENT arm scope `scopeSeq`:
 *   - no saved record with that seq                      -> no note, no grant (noRecord);
 *   - the record's arm scope is not the current one       -> no note, no grant (wrongScope); scope 0 is never current;
 *   - nib 0, above N48_RN_NIB_MAX, or not the queued nib  -> no note, no grant (nibBad);
 *   - otherwise the record is NOTED into R5′ (n48_r5_note_counted, scope first) - its destination pages enter the hazard
 *     set, a BLIND record is charged to R5′'s blind bucket - and ONLY a READABLE record is granted: forgiven += nib.
 * Every record that was found is consumed (used 0), so a duplicated queue entry can never grant twice. A lost queue grants
 * nothing for what it lost. Returns the IBs granted by this drain. */
static inline uint64_t n48_rn_drain(n48_rn_q *q, n48_rn_store *st, n48_r5_ring *r5, uint32_t scopeSeq, uint64_t namer,
                                    n48_rn_stats *s, n48_rn_unfeed_fn unfeed, void *ud)
{
    if (!q || !st || !s) return 0ull;
    uint64_t got = 0ull;
    s->g_n = 0u;
    s->b_n = 0u;   /* build 0.0.548 item D */
    const uint32_t qn = __atomic_load_n(&q->n, __ATOMIC_ACQUIRE);   /* fix pass SHOULD 2: read once, acquire */
    const uint32_t n = qn <= N48_RN_QUEUE ? qn : N48_RN_QUEUE;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t seq = q->seq[i], qnib = q->nib[i];
        s->drained++;
        n48_rn_rec *r = 0;
        for (uint32_t k = 0; k < N48_RN_STORE; k++) if (st->r[k].used && st->r[k].seq == seq) { r = &st->r[k]; break; }
        if (!r) { s->noRecord++; continue; }
        r->used = 0u;                                     /* consumed, whatever the answer */
        if (!scopeSeq || r->arm_seq != scopeSeq) { s->wrongScope++; continue; }
        if (r->nib == 0u || r->nib > N48_RN_NIB_MAX || r->nib != qnib) { s->nibBad++; continue; }
        if (!r5) { s->noRecord++; continue; }
        if (!unfeed) { s->noRecord++; continue; }                 /* no un-feed, no grant (fail-closed) */
        unfeed(ud, seq);                                          /* MUST-FIX 1: the ledger forgets this frame FIRST */
        n48_r5_note_counted(r5, scopeSeq, &r->f, namer, 0u, 0u, 0ull);   /* the note FIRST: the grant needs it */
        const uint32_t b = n48_r5_bucket(&r->f);
        if (b == N48_R5_READABLE) {
            s->forgiven += (uint64_t)r->nib;
            s->granted++; s->grantedIbs += (uint64_t)r->nib;
            got += (uint64_t)r->nib;
            if (s->g_n < N48_RN_GRANT_LOG) {
                s->g_seq[s->g_n] = seq; s->g_nib[s->g_n] = r->nib; s->g_pages[s->g_n] = r->f.ntgt + r->f.nmemw; s->g_n++;
            }
        } else if (b == N48_R5_BLIND) {
            s->blind++;
            if (s->b_n < N48_RN_GRANT_LOG) {   /* build 0.0.548 item D: which record, for the first-BLIND line */
                s->b_slot[s->b_n] = (uint32_t)(r - st->r); s->b_seq[s->b_n] = seq; s->b_nib[s->b_n] = r->nib; s->b_n++;
            }
        } else {
            s->bounded++;
        }
    }
    q->n = 0u;
    if (q->lost) { q->lost = 0u; s->lostDrains++; }
    return got;
}

/* THE VERDICT the gather hands the fill (rn_clause): 0 only when the forgiven total equals the sum of the grants AND that sum
 * does not exceed the IBs switch 75 retired as NOPED into this queue. Anything else is a wiring defect: non-zero refuses at
 * N48_DEP_UNACCOUNTED (gfx_dep.h N48_DEP_ID_RNFORGIVE) whenever a grant is standing. */
enum { N48_RN_V_OK = 0u, N48_RN_V_SUM = 1u, N48_RN_V_OVER = 2u };
static inline uint32_t n48_rn_verdict(uint64_t forgiven, uint64_t grantedIbs, uint64_t retiredIbs)
{
    if (forgiven != grantedIbs) return N48_RN_V_SUM;
    if (grantedIbs > retiredIbs) return N48_RN_V_OVER;
    return N48_RN_V_OK;
}

/* ---- the lines (each bounded under 511 bytes at its widest by tests/gfx_rnforgive_test.cpp) ---------------------------- */
#define N48_RN_REPORT_FMT \
    "rnforgive: switch 77 %s. saved %llu (miss %llu), queued %llu (overflow %llu), drained %llu; FORGIVEN %llu flight(s) / " \
    "%llu IB(s); not forgiven: blind %llu, no record %llu, wrong scope %llu, nib %llu; ring-NOPed raw %llu."
#define N48_RN_REPORT2_FMT \
    "rnforgive: switch 77%s; not forgiven: bounded %llu; lost drains %llu; walk-retired IBs %llu; verdict %u."
#define N48_RN_GRANT_FMT \
    "rnforgive: token seq %u (%u IB) forgiven: 75 proof held, R5' READABLE, %u page(s) into scope %#x hazard set; " \
    "raw %llu forgiven %llu."
#define N48_RN_X9_FMT \
    "gfx-dep: RING-NEUTER FORGIVENESS (switch 77, ) is %s; ring neuter %llu (raw %llu, forgiven %llu); verdict %u; " \
    "applies only with the consumer path (28) and R5' (30) live: consumer %u r5_mode %u."

#endif // N48_GFX_RNFORGIVE_H
