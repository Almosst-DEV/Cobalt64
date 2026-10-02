// gfx_p95.h — build 0.0.538: SWITCH 95, REPLAY A HELD PRESENT. Pure, header-only, host-tested by
// tests/gfx_p94_test.cpp. `95 | M << 8`: M 1 ON (= 351), M 2 OFF (= 607, the default and the boot value); bare `95` reads.
//
// WHY: most held presents in RUN AM were held UNKNOWN because their plane's P was spared but its retirement not yet
// observed; nothing re-asks a held present, so its picture never reaches the glass even after the P commits.
//
// WHAT SWITCH 95 DOES (ON only, and only while switch 73 is ON; OFF the present is 0.0.537's):
//   REMEMBER (n48_p95_remember): a present that switch 73 HELD with reason UNKNOWN while its plane X's slot is PENDING with P seq
//     s != 0 is remembered as (X, s) with the present's copy geometry, in a table of N48_P95_ENTS entries (one per plane: a newer
//     hold of X replaces its entry; a full table evicts the entry with the OLDEST seq). No allocation: a file-scope static.
//   SUPERSEDE (n48_p95_on_copy): a present that switch 73 lets copy empties the table - every remembered present is OLDER than it
//     in WindowServer's present order, so none may be shown after it.
//   REPLAY (n48_p95_pick): at the NEXT held present - only the present right after the held one (its perform number + 1; until
//     that next present completes WindowServer still treats X as the displayed buffer and draws nothing into it, SUSPECTED from
//     its buffer discipline; an older entry is dropped, `aged`) - a remembered (X, s) whose slot is NOW COMMITTED WITH THE SAME
//     SEQ s (read state, seq, state), newer than the glass's P (Part E's own n48_p73_seq_newer against lastDelivered) and, under switch 80 ON,
//     judged COMPLETE, is copied INSTEAD of the held present, through the SAME copy path dpg_perform uses (the replay only swaps
//     the source and its geometry; flip mode's n48_fm_present or the console copy, the same locks). The newest such seq wins; it
//     becomes Part E's decision (decided = s), so the copy's n48_p73_delivered makes s the glass's P; the table then drops every
//     entry whose seq is not newer than s. Refused (nothing replayed, the entries kept) while flip mode has a restore pending, a
//     failed restore's A copy pending, or A/B engaged with flip mode OFF (`fm_blocked`, the kext's navi48_fm_replay_blocked); a
//     flip whose latch is still pending is flip mode's own bounded wait, which HOLDS without a copy (gfx_flipmode.h step 2).
// COMMITTED is switch 73's own positive state: the walk SPARED the P and its own fence RETIRED it. So a replay can never show a
// P that was withdrawn (WITHDRAWN), refused (REFUSED), NOT_RUN / NOPED (never spared, never COMMITTED) or superseded by a newer P
// or a non-P writer of X (the slot's seq moved or went UNKNOWN: the entry is dropped).
//
// Pure: no lock, no clock, no register, no log of its own.
#ifndef N48_GFX_P95_H
#define N48_GFX_P95_H

#include <stdint.h>
#include "gfx_present73.h"
#include "gfx_p94.h"            /* build 0.0.539 (M1): n48_p94_times, the slot's gate stamp */

#define N48_P95_ENTS 4u
#ifndef N48_P94_YIELD   /* the host test's interleaving point (gfx_p94.h); the kext never defines it */
#define N48_P94_YIELD(k) ((void)0)
#endif
enum { N48_P95_OFF = 0u, N48_P95_ON = 1u };

/* A present's copy source: the plane's VRAM offset (its key) and dpg_perform's geometry for it. */
typedef struct { uint64_t key, len; uint32_t pw, ph, stride, surfW, surfH, swz; } n48_p95_geom;
typedef struct { n48_p95_geom g; uint64_t presentNo; uint32_t seq, used; } n48_p95_ent;

typedef struct {
    uint32_t on;                                       /* N48_P95_*; the verb is its only writer */
    n48_p95_ent e[N48_P95_ENTS];
    uint64_t remembered, replaced, evicted, notEligible;   /* holds remembered / replacing their plane's entry / evicting / not */
    uint64_t supersededByCopy, dropStale, dropOlder, dropIncomplete, dropAged;
    uint64_t replayed, blockedFm, asked;               /* replays made, replays refused by flip mode, held presents asked */
    uint32_t lines;                                    /* the capped REPLAYED line */
    uint64_t lastPresentNo; uint32_t lastSeq;          /* the last replay (log) */
    /* build 0.0.539: the post-copy re-check - replays re-checked after their copy, of them RACED, replays whose
     * copy was not made, replays whose present never reached the copy (held by flip mode); the capped RACED line. APPENDED. */
    uint64_t rechecked, raced, recheckNoCopy, recheckStale;
    uint32_t racedLines;
} n48_p95;

/* Turned ON: forget every entry. Only `used` is cleared: an entry's content is written by the present thread alone, so a reset
 * racing a present can lose an entry but never tear one. */
static inline void n48_p95_reset_table(n48_p95 *p)
{
    for (uint32_t k = 0; k < N48_P95_ENTS; k++) __atomic_store_n(&p->e[k].used, 0u, __ATOMIC_RELEASE);
}

/* A present switch 73 HELD: remember (key, the slot's PENDING seq) when the hold is UNKNOWN on a PENDING slot. 1 = remembered. */
static inline uint32_t n48_p95_remember(n48_p95 *p, const n48_p73 *t, uint32_t why, uint32_t slot, const n48_p95_geom *g,
                                        uint64_t presentNo)
{
    if (why != N48_P73_HOLD_UNKNOWN || slot >= N48_P73_SLOTS || !g || !g->key ||
        n48_p73_ld64(&t->s[slot].key) != g->key) { p->notEligible++; return 0u; }
    const uint32_t st = __atomic_load_n(&t->s[slot].state, __ATOMIC_ACQUIRE);
    const uint32_t seq = __atomic_load_n(&t->s[slot].seq, __ATOMIC_ACQUIRE);
    if (st != N48_P73_ST_PENDING || !seq) { p->notEligible++; return 0u; }
    uint32_t k = N48_P95_ENTS;
    for (uint32_t j = 0; j < N48_P95_ENTS; j++) if (p->e[j].used && p->e[j].g.key == g->key) { k = j; p->replaced++; break; }
    if (k == N48_P95_ENTS) for (uint32_t j = 0; j < N48_P95_ENTS; j++) if (!p->e[j].used) { k = j; break; }
    if (k == N48_P95_ENTS) {                           /* full: evict the OLDEST seq */
        k = 0u;
        for (uint32_t j = 1; j < N48_P95_ENTS; j++) if (n48_p73_seq_newer(p->e[k].seq, p->e[j].seq)) k = j;
        p->evicted++;
    }
    p->e[k].g = *g; p->e[k].seq = seq; p->e[k].presentNo = presentNo; p->e[k].used = 1u;
    p->remembered++;
    return 1u;
}
/* A present switch 73 lets copy: every remembered present is older in present order. */
static inline void n48_p95_on_copy(n48_p95 *p)
{
    for (uint32_t k = 0; k < N48_P95_ENTS; k++) if (p->e[k].used) { p->e[k].used = 0u; p->supersededByCopy++; }
}
/* The remembered plane whose slot is still PENDING-spared with the newest seq (switch 94's replay-time step asks it). 0 = none. */
static inline uint64_t n48_p95_newest_pending(const n48_p95 *p, const n48_p73 *t)
{
    uint64_t key = 0ull; uint32_t best = 0u;
    for (uint32_t k = 0; k < N48_P95_ENTS; k++) {
        if (!p->e[k].used) continue;
        const uint32_t i = n48_p73_find(t, p->e[k].g.key);
        if (i >= N48_P73_SLOTS) continue;
        const n48_p73_slot *s = &t->s[i];
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != N48_P73_ST_PENDING) continue;
        const uint32_t seq = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (seq != p->e[k].seq || __atomic_load_n(&s->spared, __ATOMIC_ACQUIRE) != seq) continue;
        if (!key || n48_p73_seq_newer(seq, best)) { key = p->e[k].g.key; best = seq; }
    }
    return key;
}
/* Is entry k's P COMMITTED at its seq now (state, seq, state)? 1 = COMMITTED, 2 = still PENDING at its seq, 0 = stale. */
static inline uint32_t n48_p95_slot_is(const n48_p73 *t, const n48_p95_ent *e)
{
    const uint32_t i = n48_p73_find(t, e->g.key);
    if (i >= N48_P73_SLOTS) return 0u;
    const n48_p73_slot *s = &t->s[i];
    const uint32_t st1 = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
    N48_P94_YIELD(12);
    const uint32_t seq = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    N48_P94_YIELD(13);
    const uint32_t st2 = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
    if (seq != e->seq || st1 != st2) return 0u;
    return st1 == N48_P73_ST_COMMITTED ? 1u : st1 == N48_P73_ST_PENDING ? 2u : 0u;
}
/* Switch 80's C6 for a replay (read-only: no SHADOW count): 1 = hold (80 ON and this P was not judged COMPLETE). */
static inline uint32_t n48_p95_c80_holds(const n48_p73 *t, const n48_p95_ent *e)
{
    if (n48_p73_ld32(&t->c80on) != N48_P73_C80_ON) return 0u;
    const uint32_t i = n48_p73_find(t, e->g.key);
    if (i >= N48_P73_SLOTS) return 1u;
    const n48_p73_slot *s = &t->s[i];
    const uint32_t s1 = n48_p73_ld32(&s->c80seq), v = n48_p73_ld32(&s->c80verdict), s2 = n48_p73_ld32(&s->c80seq);
    return n48_p73_c80_complete(s1, v, s2, e->seq) ? 0u : 1u;
}
/* THE REPLAY DECISION at held present `presentNo`. 1 = copy *out (its P seq *seq_out; Part E's decision is set); 0 = none. */
static inline uint32_t n48_p95_pick(n48_p95 *p, n48_p73 *t, uint32_t fm_blocked, uint64_t presentNo, n48_p95_geom *out,
                                    uint32_t *seq_out)
{
    p->asked++;
    uint32_t best = N48_P95_ENTS;
    for (uint32_t k = 0; k < N48_P95_ENTS; k++) {
        n48_p95_ent *e = &p->e[k];
        if (!e->used || e->presentNo == presentNo) continue;          /* this present's own hold waits for the next present */
        if (e->presentNo + 1u != presentNo) { e->used = 0u; p->dropAged++; continue; }   /* only the NEXT present replays */
        const uint32_t is = n48_p95_slot_is(t, e);
        if (is == 0u) { e->used = 0u; p->dropStale++; continue; }      /* a newer P, a non-P writer, a withdrawal, ... */
        if (is == 2u) continue;                                         /* still PENDING at its seq: keep */
        if (t->deliveredOk && !n48_p73_seq_newer(e->seq, t->lastDelivered)) { e->used = 0u; p->dropOlder++; continue; }
        if (n48_p95_c80_holds(t, e)) { e->used = 0u; p->dropIncomplete++; continue; }
        if (best == N48_P95_ENTS || n48_p73_seq_newer(e->seq, p->e[best].seq)) best = k;
    }
    if (best == N48_P95_ENTS) return 0u;
    if (fm_blocked) { p->blockedFm++; return 0u; }
    const uint32_t s = p->e[best].seq;
    *out = p->e[best].g;
    if (seq_out) *seq_out = s;
    t->decided = s; t->decidedOk = 1u;               /* Part E: the copy's n48_p73_delivered makes s the glass's P */
    p->replayed++; p->lastSeq = s; p->lastPresentNo = p->e[best].presentNo;
    for (uint32_t k = 0; k < N48_P95_ENTS; k++)       /* the replayed entry and every entry not newer than it */
        if (p->e[k].used && (k == best || !n48_p73_seq_newer(p->e[k].seq, s))) { p->e[k].used = 0u; if (k != best) p->dropOlder++; }
    return 1u;
}

/* =============================================================================================================================
 * build 0.0.539 (MUST for any run with 95 ON) — THE REPLAY'S POST-COPY RE-CHECK. n48_p95_pick chose (X, s) from a
 * slot it read COMMITTED at seq s; the copy that follows (dpg_perform: flip mode's n48_fm_present or the console copy) takes time, and
 * meanwhile the hook may gate a NEW P on X's slot (its seq and switch 94's gate stamp move), or a non-P writer / invalidation may take
 * it (its state moves). n48_p95_rid_take records - right after the pick, on the present thread - the slot, the picked seq s, and the
 * slot's gate stamp (switch 94's n48_p94_times: gseq, gus, read seq-stamp-seq); n48_p95_rid_raced re-reads them after the copy has
 * returned: RACED when the slot's seq is not s, its state is not COMMITTED, or its gate stamp moved. The copy has happened either way:
 * this only COUNTS (and the kext logs, capped) a replay whose picture may not be the P that was COMMITTED when it was chosen. Pure.
 * ============================================================================================================================= */
typedef struct { uint32_t used, slot, seq, gseq, stampOk; uint64_t gus, presentNo, key; } n48_p95_rid;
/* A gate stamp snapshot of slot i: seq, time, seq (n48_p94_stamp's writer order). 1 = consistent (*gs, *gu). */
static inline uint32_t n48_p95_gate_of(const n48_p94_times *tm, uint32_t i, uint32_t *gs, uint64_t *gu)
{
    const uint32_t a = __atomic_load_n(&tm[i].gseq, __ATOMIC_ACQUIRE);
    const uint64_t u = __atomic_load_n(&tm[i].gus, __ATOMIC_ACQUIRE);
    const uint32_t b = __atomic_load_n(&tm[i].gseq, __ATOMIC_ACQUIRE);
    *gs = b; *gu = u;
    return a == b ? 1u : 0u;
}
/* Right after a pick of (key, seq) at present `presentNo`: 1 = recorded. */
static inline uint32_t n48_p95_rid_take(n48_p95_rid *r, const n48_p73 *t, const n48_p94_times *tm, uint64_t key, uint32_t seq,
                                        uint64_t presentNo)
{
    r->used = 0u;
    const uint32_t i = n48_p73_find(t, key);
    if (!tm || i >= N48_P73_SLOTS || !seq) return 0u;
    r->slot = i; r->seq = seq; r->key = key; r->presentNo = presentNo;
    r->stampOk = n48_p95_gate_of(tm, i, &r->gseq, &r->gus);
    r->used = 1u;
    return 1u;
}
/* After the copy returned: 1 = RACED. *seqNow, *gseqNow, *stNow: what the slot reads now (the log).
 * AN UPPER BOUND (fix round, xhigh review): the re-read happens after the copy RETURNED, so a new gate / invalidation / non-P writer
 * that lands on the slot after the copy had finished - harmless to the picture already on the glass - is counted RACED too. The count
 * never misses a real race (any change during the copy is still visible at the re-read); it can only over-count. */
static inline uint32_t n48_p95_rid_raced(const n48_p95_rid *r, const n48_p73 *t, const n48_p94_times *tm, uint32_t *seqNow,
                                         uint32_t *gseqNow, uint32_t *stNow)
{
    const n48_p73_slot *s = &t->s[r->slot];
    const uint32_t st1 = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
    N48_P94_YIELD(14);
    const uint32_t sq = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    uint32_t gs = 0u; uint64_t gu = 0ull;
    const uint32_t ok = n48_p95_gate_of(tm, r->slot, &gs, &gu);
    const uint32_t st2 = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
    *seqNow = sq; *gseqNow = gs; *stNow = st2;
    return (sq != r->seq || st1 != N48_P73_ST_COMMITTED || st2 != N48_P73_ST_COMMITTED || n48_p73_ld64(&s->key) != r->key ||
            !ok || !r->stampOk || gs != r->gseq || gu != r->gus) ? 1u : 0u;
}

#define N48_P95_ON_TXT "ON (a held present replays once its P COMMITTED)"
#define N48_P95_OFF_TXT "OFF (default)"
/* The switch-95 report (the bare `gfxneuter 95`, any M), two lines. FMT args: state, how, asked, remembered, replaced, evicted, not
 * eligible, replayed, blocked by flip mode. */
#define N48_P95_FMT "present95: switch 95 %s%s. held presents asked %llu; remembered %llu (replacing %llu, evicting %llu), not " \
    "eligible %llu; REPLAYED %llu, refused by flip mode %llu"
/* args: superseded by a copy, stale, older, incomplete, aged, entries in use. */
#define N48_P95_FMT2 "present95: dropped: superseded by a copy %llu, stale %llu, older than the glass %llu, incomplete %llu, not " \
    "the next present %llu; entries in use %u of 4"
/* The capped REPLAYED line. args: this present, the remembered present, plane VRAM, P seq, glass P seq before, line. */
/* build 0.0.539 (M1). The report's third line. args: re-checked, RACED, copy not made, present never reached its copy. */
#define N48_P95_FMT3 "present95: post-copy re-check (M1): replays re-checked %llu, RACED %llu; copy not made %llu, never reached the " \
    "copy %llu"
/* The capped RACED line. args: present, plane VRAM, picked seq, slot, slot seq now, state now, gate stamp seq then, now, line. */
#define N48_P95_RACED_FMT "present95: REPLAY RACED at present #%llu (plane VRAM %#llx, picked P seq %u, slot %u): after the copy the " \
    "slot reads seq %u state %s, gate stamp seq %u -> %u - line %u of 8"
#define N48_P95_LINE_FMT "present95: REPLAYED at present #%llu the held present #%llu (plane VRAM %#llx, P seq %u, now COMMITTED; " \
    "glass P seq was %u) - line %u of 8"

#endif /* N48_GFX_P95_H */
