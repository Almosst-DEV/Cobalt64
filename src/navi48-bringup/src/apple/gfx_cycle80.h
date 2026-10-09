// gfx_cycle80.h — build 0.0.525 (an internal design note): CYCLE COMPLETENESS, SWITCH 80.
// `80 | M << 8`: M 1 ON (= 336), M 2 OFF (= 592, the default and the boot value), M 3 SHADOW (= 848), bare `80` reads.
//
// THE RULE (CYCLE80.md C1-C7). WindowServer double-buffers two LAYERS (run11c: X 0x400800000, X' 0x404800000); each cycle's frames
// write one layer and the cycle's P (the 1040-dword final display pass) samples it into a plane. Switch 73 copies a plane only
// when its P COMMITTED; that says nothing about the frames that painted the layer the P sampled (: the avatar's frame 'a'
// refused while d, b and P committed: a picture WindowServer never produced reached the glass). With 80 ON a COMMITTED P's
// present is held unless its cycle was COMPLETE: every frame that wrote the P's layer since that layer's previous P committed.
//   C1 THE TABLE: N48_C80_LAYERS layers keyed by (ctx, physical base page) - the hazard's identity - never evicted, cleared when
//      80 turns ON; per layer va, extent, state (UNKNOWN / CLEAN / DIRTY) and the counts since its last P (writers, committed
//      writers, held-back writes, unknowns); a N48_C80_RING-entry ring {seq, layer mask} of the frames committed at the gate.
//      Everything here is written under gXdLock; the two things that happen outside it (a writer lost after the gate, a frame
//      the judge never saw) are QUEUED lock-free and drained at the next judged frame's top (n48_c80_drain).
//   C2 A P: a single-IB 1040-dword frame with ONE colour target that is NOT a known layer. Its layer is its D draw's one tiled
//      input (cyc515's in_layer/in_ok), resolved to a physical page through the P's own vm - BY CONTENT, never by the
//      descriptor slot (`dslot` below is an instrument the kext cannot fill today; T10 proves the key never reads it). No
//      input, an input that does not resolve, or a full table: UNDETERMINED (held). A 1040-dword frame whose target IS a
//      layer is a WRITER.
//   C3 WRITERS: a judged frame's colour targets are matched with each layer's base page, or (same ctx) by VA inside [va,
//      va + extent). Committed at the gate: a committed writer (and ring). Held back (the R5′ note site): READABLE - every
//      layer whose base page equals a target page or whose range holds a target or memory-destination VA gets a held-back
//      write; BLIND, targets truncated, no R5′ record (28 OFF), or a frame never judged: EVERY layer gets an unknown;
//      BOUNDED: nothing.
//      0.0.525 BUILD FINDING (deviation from CYCLE80.md C3, in the fail-closed direction): the R5′ record's colour targets are
//      each CB slot's LAST base per IB (gfx_capture_scan.h n48_gcap_scan_ex emits cbBase[] once, after the walk), so a frame
//      that draws a layer and then retargets CB0 never names the layer - run11c F67 ('b', refused) and F70 (refused) wrote X
//      and X' and their R5′ records do not show it, and P69 / P74 read COMPLETE (a fail-OPEN). So every judged frame's
//      colour targets are ALSO cyc515's write set for the same frame (gfx_cycle515.h: EVERY CB/DB base any IB sets, gCy515.ws),
//      matched by VA: a held-back frame's layers are the UNION; a held-back frame whose cyc515 set is not known gives every
//      layer an unknown. It only ever ADDS held-back writes ( MEDIUM-1: the instrument never opens anything): a committed
//      frame is credited from the gate's own target list alone, as the contract says.
//   C4 LOST AFTER THE GATE: the ring's layers of that seq get an unknown and every P slot on those layers whose seq is newer is
//      DEMOTED to INCOMPLETE (a demotion of a slot already COMMITTED - a copy may have been made - is counted). A seq not in the
//      ring: every layer.
//   C5 THE P's JUDGEMENT, at its gate, BEFORE anything resets: DIRTY if any held-back write or unknown since the layer's last P;
//      else CLEAN if at least one committed writer; else the state is UNCHANGED (sticky). Only then are the counts zeroed. A
//      committed writer of L that reads another layer M that is not CLEAN (or whose reads are not known) gets an unknown.
//   C6 (gfx_present73.h n48_p73_c80_hold) the present holds a COMMITTED P unless 80 judged THAT P COMPLETE; before Part E.
//   C7 A layer starts UNKNOWN (pre-arm, pre-discovery): the glass waits for its first complete cycle.
//   X1 SHADOW computes, counts and logs exactly as ON; only the present's hold is not applied (it counts "would hold").
//
// Pure: no lock, no clock, no register, no log. Host-tested by tests/gfx_cycle80_test.cpp over run11c's real sequence.
#ifndef N48_GFX_CYCLE80_H
#define N48_GFX_CYCLE80_H

#include <stdint.h>
#include "gfx_present73.h"
#include "gfx_dep.h"

#define N48_C80_LAYERS        4u
#define N48_C80_RING         16u
#define N48_C80_EVICT_P      16u                     /* FIX PASS (SHOULD): a layer no P sampled for this many P is evicted */
#define N48_C80_LOSTQ         8u                     /* lost seqs queued between two judged frames (overflow: every layer) */
#define N48_C80_EXTENT_DEF    (16ull << 20)          /* the ledger has no size for the layer: 16 MiB, counted */
#define N48_C80_LINES_FIRST  16u                     /* per-P lines: the first 16 unconditionally ... */
#define N48_C80_LINES_MAX    64u                     /* ... then INCOMPLETE / state changes only, at most 1 per second, 64 in all */
#define N48_C80_LINE_GAP_US  1000000ull
#define N48_C80_COPIES       64u                     /* the copy ring the bare verb dumps, 8 per line */
#define N48_C80_COPY_FIRST    8u                     /* 73's own first 8 COPIED lines; past them one summary per 2 s */
#define N48_C80_SUM_GAP_US   2000000ull
#define N48_C80_NOSLOT       0xFFFFFFFFu

enum { N48_C80_ST_UNKNOWN = 0u, N48_C80_ST_CLEAN = 1u, N48_C80_ST_DIRTY = 2u };

typedef struct {
    uint64_t ctx, page, va, extent;   /* the key (ctx, page); va/extent for the range match */
    uint32_t used, state;
    uint32_t w, c, h, u;              /* since its last P: writers, committed writers, held-back writes, unknowns */
    uint32_t extentAssumed, pad0;     /* the ledger had no size at discovery: 16 MiB until a later P brings one */
    uint64_t hFirst;                  /* the judged-frame number of the first held-back write since its last P (0 none) */
    uint64_t heldArm, heldLastP;      /* held-back writes since the arm scope; the since-last-P count at its last P */
    uint64_t pSeen, pComplete, pIncomplete, found;
    uint64_t lastP;                   /* FIX PASS (SHOULD): c->pJudged when a P last sampled it (eviction clock) */
} n48_c80_layer;

typedef struct { uint32_t seq, mask; } n48_c80_wr;

typedef struct {
    n48_c80_layer L[N48_C80_LAYERS];
    n48_c80_wr ring[N48_C80_RING];
    uint32_t ringHead, ringN;
    uint32_t armSeq;
    /* THIS frame's reads (n48_c80_note_reads, the policy pass; cleared by n48_c80_frame_begin) */
    /* FIX PASS MF-1: the LAYERS this frame's draws read (a mask, computed at note time - no capped list), and whether the
     * translator said a list was incomplete (the caller's `over`, and only that). FIX PASS MF-3: the plane-shaped input pairs
     * (one tiled + one linear) this frame's draws bound, and the last pair's tiled VA: a P's layer only when exactly ONE. */
    uint32_t rdMask, rdOver;
    uint32_t nPair, pad1;
    uint64_t pairVa;
    /* the per-P line caps */
    uint32_t lines, pad0;
    uint64_t lastLineUs, linesSuppressed;
    /* counters (since boot; n48_c80_reset clears the table, the ring and the line caps, not these) */
    uint64_t pJudged, pComplete, pIncomplete, pUndet, pUndetInput, pUndetFull, pFirstSight, stateChanges;
    uint64_t wrCommitted, wrHeld, heldReadable, heldBounded, heldBlind, heldTrunc, heldNoRecord, unjudged;
    uint64_t lost, lostUnknown, demoted, demotedLate, crossDirty, crossUnknown, extentAssumed, resets;
    uint64_t heldWsUnknown, heldWsOnly;
    uint64_t evicted;                     /* FIX PASS (SHOULD): layers evicted after N48_C80_EVICT_P P without a sample */
    uint32_t evictLogged, pad2;           /* the kext's one eviction line was printed */   /* held-back frames: cyc515's set not known (every layer); layers ONLY cyc515's set named */
} n48_c80;

/* The two things that happen outside gXdLock, queued lock-free (atomics only) and drained under it. */
typedef struct {
    uint32_t q[N48_C80_LOSTQ];
    uint32_t n;            /* claimed slots (fetch-add); > N48_C80_LOSTQ = overflow */
    uint32_t unjudged;     /* frames the judge never saw since the last drain */
    uint32_t reset;        /* the verb turned 80 ON: clear the table before anything else */
} n48_c80_q;

typedef struct {              /* one P, as the gate sees it */
    uint64_t ctx, frame, plane;
    uint64_t in_va, in_page;  /* cyc515's in_layer and its page through the P's own vm */
    uint64_t extent;          /* the ledger's size for in_va (0 = absent) */
    uint32_t in_ok, page_ok;  /* cyc515's in_ok; the page resolved */
    uint32_t seq;             /* the gate seq when committed, else 0 */
    uint32_t dslot;           /* the descriptor slot, INSTRUMENT ONLY (the kext passes N48_C80_NOSLOT); never a key */
} n48_c80_pin;

typedef struct {
    uint32_t verdict;         /* N48_P73_C80V_COMPLETE / INCOMPLETE / UNDET */
    uint32_t layer;           /* the layer index, N48_C80_LAYERS for UNDET */
    uint32_t state, prev, changed, log;
    uint32_t w, c, h, u;      /* the counts the judgement read (before the reset) */
    uint64_t hFirst, layerVa, page;
} n48_c80_pout;

static inline const char *n48_c80_state_name(uint32_t s)
{
    return s == N48_C80_ST_CLEAN ? "CLEAN" : s == N48_C80_ST_DIRTY ? "DIRTY" : "UNKNOWN";
}
static inline const char *n48_c80_verdict_name(uint32_t v)
{
    return v == N48_P73_C80V_COMPLETE ? "COMPLETE" : v == N48_P73_C80V_INCOMPLETE ? "INCOMPLETE" : v == N48_P73_C80V_UNDET ? "UNDET"
         : "none";
}
static inline const char *n48_c80_mode_name(uint32_t m)
{
    return m == N48_P73_C80_ON ? "ON" : m == N48_P73_C80_SHADOW ? "SHADOW" : "OFF";
}
/* The verb's M -> the mode: 1 ON, 2 OFF, 3 SHADOW; anything else 0xFF (refused, unchanged). */
static inline uint32_t n48_c80_mode_of_m(uint32_t m)
{
    return m == 1u ? N48_P73_C80_ON : m == 2u ? N48_P73_C80_OFF : m == 3u ? N48_P73_C80_SHADOW : 0xFFu;
}

/* Turned ON (or SHADOW from OFF): forget every layer, the ring, the line caps. Counters stay (boot totals). */
static inline void n48_c80_reset(n48_c80 *c)
{
    for (uint32_t i = 0; i < N48_C80_LAYERS; i++) {
        const n48_c80_layer z = { 0ull, 0ull, 0ull, 0ull, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull };
        c->L[i] = z;
    }
    for (uint32_t i = 0; i < N48_C80_RING; i++) { c->ring[i].seq = 0u; c->ring[i].mask = 0u; }
    c->ringHead = 0u; c->ringN = 0u; c->rdMask = 0u; c->rdOver = 0u; c->nPair = 0u; c->pairVa = 0ull;
    c->lines = 0u; c->lastLineUs = 0ull;
    c->resets++;
}
/* The arm scope moved (gXpScopeSeq): the "held-back since the arm" counts start again. `seq` 0 is not a scope. */
static inline void n48_c80_scope(n48_c80 *c, uint32_t seq)
{
    if (!seq || c->armSeq == seq) return;
    c->armSeq = seq;
    for (uint32_t i = 0; i < N48_C80_LAYERS; i++) c->L[i].heldArm = 0ull;
}
/* The layers a surface is: base page equal (whatever the ctx: one physical page is one surface), or - same ctx - its VA inside
 * [va, va + extent) (end exclusive). `page` 0 = not resolved (the VA alone is asked). */
static inline uint32_t n48_c80_layers_of(const n48_c80 *c, uint64_t ctx, uint64_t page, uint64_t va)
{
    uint32_t m = 0u;
    for (uint32_t i = 0; i < N48_C80_LAYERS; i++) {
        const n48_c80_layer *e = &c->L[i];
        if (!e->used) continue;
        if (page && e->page == (page & ~0xFFFull)) { m |= 1u << i; continue; }
        if (va && ctx == e->ctx && va >= e->va && va - e->va < e->extent) m |= 1u << i;
    }
    return m;
}
static inline uint32_t n48_c80_all(const n48_c80 *c)
{
    uint32_t m = 0u;
    for (uint32_t i = 0; i < N48_C80_LAYERS; i++) if (c->L[i].used) m |= 1u << i;
    return m;
}
static inline void n48_c80_unknown(n48_c80 *c, uint32_t mask)
{
    for (uint32_t i = 0; i < N48_C80_LAYERS; i++) if ((mask >> i) & 1u) c->L[i].u++;
}
/* This frame's reads start empty (the frame top). */
static inline void n48_c80_frame_begin(n48_c80 *c) { c->rdMask = 0u; c->rdOver = 0u; c->nPair = 0u; c->pairVa = 0ull; }
/* One draw's input list (the policy pass: xlat12_draw_stats in_va/in_mode/in_n/in_over). FIX PASS MF-1: each VA is folded into
 * the mask of the LAYERS it lies in (VA inside [va, va + extent)) at once - any number of inputs, no list to overflow; only the
 * translator's own `over` makes the reads UNKNOWN. FIX PASS MF-3: a plane-shaped pair (n == 2, one tiled + one linear, not
 * over - n48_cy_note_inputs' test) is counted and its tiled VA kept. */
static inline void n48_c80_note_reads(n48_c80 *c, const uint64_t *va, const uint32_t *mode, uint32_t n, uint32_t over)
{
    if (over) c->rdOver = 1u;
    if (!va) return;
    for (uint32_t k = 0; k < n; k++) {
        if (!va[k]) continue;
        for (uint32_t i = 0; i < N48_C80_LAYERS; i++) {
            const n48_c80_layer *e = &c->L[i];
            if (e->used && va[k] >= e->va && va[k] - e->va < e->extent) c->rdMask |= 1u << i;
        }
    }
    if (mode && !over && n == 2u && ((mode[0] != 0u) != (mode[1] != 0u))) {
        const uint64_t t = mode[0] != 0u ? va[0] : va[1];
        if (t) { c->nPair++; c->pairVa = t; }
    }
}
/* FIX PASS MF-3: the P's input BY ITS OWN COUNT: 1 and *va only when exactly one plane-shaped pair was bound this frame. */
static inline uint32_t n48_c80_p_input(const n48_c80 *c, uint64_t *va)
{
    if (va) *va = c->nPair == 1u ? c->pairVa : 0ull;
    return c->nPair == 1u && c->pairVa ? 1u : 0u;
}
/* The frame is a P (C2): a P shape (one IB of 1040 dwords) with ONE colour target that is not a known layer. */
static inline uint32_t n48_c80_is_p(const n48_c80 *c, uint32_t pShape, uint32_t tgtN, uint64_t ctx, uint64_t tgtPage,
                                    uint64_t tgtVa)
{
    return (pShape && tgtN == 1u && !n48_c80_layers_of(c, ctx, tgtPage, tgtVa)) ? 1u : 0u;
}
static inline void n48_c80_ring_push(n48_c80 *c, uint32_t seq, uint32_t mask)
{
    if (!seq) return;
    c->ring[c->ringHead].seq = seq; c->ring[c->ringHead].mask = mask;
    c->ringHead = (c->ringHead + 1u) % N48_C80_RING;
    if (c->ringN < N48_C80_RING) c->ringN++;
}
/* The ring entry for `seq`: 1 and *mask when found. */
static inline uint32_t n48_c80_ring_find(const n48_c80 *c, uint32_t seq, uint32_t *mask)
{
    if (!seq) return 0u;
    for (uint32_t i = 0; i < N48_C80_RING; i++)
        if (c->ring[i].seq == seq) { if (mask) *mask = c->ring[i].mask; return 1u; }
    return 0u;
}

/* C5 + C2: THE P's JUDGEMENT at its gate. Writes the verdict into its p73 slot (`t`, `slot`; N48_P73_SLOTS or a null `t` = no
 * slot: switch 73 is OFF). Returns the verdict. */
static inline uint32_t n48_c80_p(n48_c80 *c, const n48_c80_pin *in, n48_p73 *t, uint32_t slot, n48_c80_pout *o)
{
    n48_c80_pout z = { N48_P73_C80V_UNDET, N48_C80_LAYERS, N48_C80_ST_UNKNOWN, N48_C80_ST_UNKNOWN, 0u, 0u, 0u, 0u, 0u, 0u,
                       0ull, 0ull, 0ull };
    c->pJudged++;
    uint32_t li = N48_C80_LAYERS;
    if (!in->in_ok || !in->in_va || !in->page_ok || !in->in_page) { c->pUndet++; c->pUndetInput++; }
    else {
        const uint64_t pg = in->in_page & ~0xFFFull;
        for (uint32_t i = 0; i < N48_C80_LAYERS; i++)
            if (c->L[i].used && c->L[i].ctx == in->ctx && c->L[i].page == pg) { li = i; break; }   /* THE KEY: (ctx, page) */
        if (li >= N48_C80_LAYERS) {
            for (uint32_t i = 0; i < N48_C80_LAYERS; i++) if (!c->L[i].used) { li = i; break; }
            if (li >= N48_C80_LAYERS) { c->pUndet++; c->pUndetFull++; }
            else {
                n48_c80_layer *e = &c->L[li];
                e->used = 1u; e->ctx = in->ctx; e->page = pg; e->va = in->in_va;
                e->extent = in->extent ? in->extent : N48_C80_EXTENT_DEF;
                e->extentAssumed = in->extent ? 0u : 1u;
                if (!in->extent) c->extentAssumed++;
                e->state = N48_C80_ST_UNKNOWN;   /* C7: discovery starts UNKNOWN */
                e->w = e->c = e->h = e->u = 0u; e->hFirst = 0ull; e->found = in->frame; e->lastP = c->pJudged;
                c->pFirstSight++;
            }
        }
    }
    if (li < N48_C80_LAYERS) {
        n48_c80_layer *e = &c->L[li];
        if (e->extentAssumed && in->extent) { e->extent = in->extent; e->extentAssumed = 0u; }   /* the ledger has it now */
        z.layer = li; z.layerVa = e->va; z.page = e->page;
        z.w = e->w; z.c = e->c; z.h = e->h; z.u = e->u; z.hFirst = e->hFirst;
        z.prev = e->state;
        /* C5, IN THIS ORDER: judge, THEN reset */
        if (e->h || e->u) e->state = N48_C80_ST_DIRTY;
        else if (e->c) e->state = N48_C80_ST_CLEAN;
        /* else: unchanged (sticky) */
        z.state = e->state;
        z.changed = (z.state != z.prev) ? 1u : 0u;
        if (z.changed) c->stateChanges++;
        z.verdict = e->state == N48_C80_ST_CLEAN ? N48_P73_C80V_COMPLETE : N48_P73_C80V_INCOMPLETE;
        e->heldLastP = e->h;
        e->w = e->c = e->h = e->u = 0u; e->hFirst = 0ull;
        e->pSeen++;
        if (z.verdict == N48_P73_C80V_COMPLETE) { e->pComplete++; c->pComplete++; } else { e->pIncomplete++; c->pIncomplete++; }
    }
    /* FIX PASS (SHOULD): the eviction clock. The layer this P sampled is seen now; a layer no P sampled for N48_C80_EVICT_P P is
     * forgotten (counted): rediscovered, it starts UNKNOWN again (C7), so an eviction can only hold more. */
    if (li < N48_C80_LAYERS) c->L[li].lastP = c->pJudged;
    for (uint32_t i = 0; i < N48_C80_LAYERS; i++) {
        n48_c80_layer *e = &c->L[i];
        if (!e->used || i == li || c->pJudged - e->lastP < N48_C80_EVICT_P) continue;
        const n48_c80_layer zl = { 0ull, 0ull, 0ull, 0ull, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull };
        *e = zl;
        c->evicted++;
    }
    if (t && slot < N48_P73_SLOTS) {
        /* FIX PASS MF-2: c80seq 0 FIRST (no present can pair an old seq with the new verdict), then the verdict and the layer,
         * then the seq (release). n48_p73_c80_hold reads seq, verdict, seq and needs both seqs equal. */
        n48_p73_st32(&t->s[slot].c80seq, 0u);
        n48_p73_st32(&t->s[slot].c80verdict, z.verdict);
        n48_p73_st32(&t->s[slot].c80layer, li < N48_C80_LAYERS ? li + 1u : 0u);
        n48_p73_st32(&t->s[slot].c80seq, in->seq);
    }
    if (in->seq) n48_c80_ring_push(c, in->seq, 0u);   /* a committed P writes no layer */
    if (o) *o = z;
    return z.verdict;
}

/* The layers a cyc515 write set names (VA only, same ctx). */
static inline uint32_t n48_c80_ws_layers(const n48_c80 *c, uint64_t ctx, const uint64_t *ws, uint32_t nws)
{
    uint32_t m = 0u;
    for (uint32_t k = 0; ws && k < nws; k++) m |= n48_c80_layers_of(c, ctx, 0ull, ws[k]);
    return m;
}
/* C3 + C5's cross clause: A NON-P FRAME AT THE GATE. Only a COMMITTED one counts here (a held-back one is n48_c80_held's, at the
 * note site). `pg[k]`/`va[k]`/`res[k]` are the frame's held colour targets (the list p73_judge receives). Every committed frame
 * is recorded in the ring with its layer mask (0 = it wrote no layer). Returns the mask. */
static inline uint32_t n48_c80_writer(n48_c80 *c, uint64_t ctx, const uint64_t *va, const uint64_t *pg, const uint32_t *res,
                                      uint32_t n, const uint64_t *ws, uint32_t nws, uint32_t committed, uint32_t seq)
{
    if (!committed) return 0u;
    uint32_t mask = n48_c80_ws_layers(c, ctx, ws, nws);   /* FIX PASS (SHOULD): credit from the union, cyc515's set too */
    for (uint32_t k = 0; va && k < n; k++) mask |= n48_c80_layers_of(c, ctx, (res && res[k] && pg) ? pg[k] : 0ull, va[k]);
    if (mask) c->wrCommitted++;
    for (uint32_t i = 0; i < N48_C80_LAYERS; i++) {
        if (!((mask >> i) & 1u)) continue;
        n48_c80_layer *e = &c->L[i];
        e->w++; e->c++;
        /* C5's cross clause: this writer of L reads a DIFFERENT layer that is DIRTY (FIX PASS MF-1: not "not CLEAN" - two
         * layers that start UNKNOWN and read each other would never reach CLEAN), or its reads are not known */
        const uint32_t rm = c->rdMask & ~(1u << i);
        uint32_t bad = 0u;
        for (uint32_t j = 0; j < N48_C80_LAYERS; j++) if (((rm >> j) & 1u) && c->L[j].state == N48_C80_ST_DIRTY) bad = 1u;
        if (bad) { e->u++; c->crossDirty++; }
        else if (c->rdOver) { e->u++; c->crossUnknown++; }
    }
    n48_c80_ring_push(c, seq, mask);
    return mask;
}

/* C3: A HELD-BACK FRAME at the R5′ note site (after n48_r5_resolve). `r` null = no R5′ record (switch 28 OFF); `trunc` = our
 * 8-target hold truncated its colour targets (tgtN > hHeld); `ws`/`nws`/`wsUnknown` cyc515's write set for the same frame (see
 * C3's 0.0.525 finding above). `frame` for the "first held" field. Returns the mask it touched. */
static inline uint32_t n48_c80_held(n48_c80 *c, const n48_r5_frame *r, uint32_t trunc, const uint64_t *ws, uint32_t nws,
                                    uint32_t wsUnknown, uint64_t ctx, uint64_t frame)
{
    const uint32_t all = n48_c80_all(c);
    if (!r) { c->heldNoRecord++; n48_c80_unknown(c, all); return all; }
    const uint32_t b = n48_r5_bucket(r);
    if (b == N48_R5_BOUNDED) { c->heldBounded++; return 0u; }
    if (b != N48_R5_READABLE) { c->heldBlind++; n48_c80_unknown(c, all); return all; }
    if (trunc) { c->heldTrunc++; n48_c80_unknown(c, all); return all; }
    if (wsUnknown) { c->heldWsUnknown++; n48_c80_unknown(c, all); return all; }
    c->heldReadable++;
    const uint32_t wsMask = n48_c80_ws_layers(c, ctx, ws, nws);
    uint32_t r5Mask = 0u;
    for (uint32_t k = 0; k < r->ntgt && k < N48_CP_TGT_MAX; k++) r5Mask |= n48_c80_layers_of(c, ctx, r->tgt[k].page, r->tgt[k].va);
    for (uint32_t k = 0; k < r->nmemw && k < N48_CP_MEMW_MAX; k++)
        r5Mask |= n48_c80_layers_of(c, ctx, r->memw[k].page, r->memw[k].va);
    const uint32_t mask = wsMask | r5Mask;
    if (mask) c->wrHeld++;
    if (wsMask & ~r5Mask) c->heldWsOnly++;   /* the R5' record alone would have missed a layer (C3's 0.0.525 finding) */
    for (uint32_t i = 0; i < N48_C80_LAYERS; i++) {
        if (!((mask >> i) & 1u)) continue;
        n48_c80_layer *e = &c->L[i];
        e->w++; e->h++; e->heldArm++;
        if (!e->hFirst) e->hFirst = frame;
    }
    return mask;
}

/* C4: THE WRITER `seq` STOPPED AFTER THE GATE (drained under gXdLock). Its layers get an unknown; a seq not in the ring, every
 * layer. Every p73 slot on those layers whose P seq is NEWER is demoted to INCOMPLETE. Returns the mask. */
static inline uint32_t n48_c80_lost(n48_c80 *c, n48_p73 *t, uint32_t seq)
{
    if (!seq) return 0u;
    uint32_t mask = 0u;
    c->lost++;
    if (!n48_c80_ring_find(c, seq, &mask)) { c->lostUnknown++; mask = n48_c80_all(c); }
    n48_c80_unknown(c, mask);
    for (uint32_t i = 0; t && i < N48_P73_SLOTS; i++) {
        n48_p73_slot *s = &t->s[i];
        const uint32_t ly = n48_p73_ld32(&s->c80layer), ps = n48_p73_ld32(&s->c80seq);
        if (!ly || ly > N48_C80_LAYERS || !((mask >> (ly - 1u)) & 1u) || !ps || !n48_p73_seq_newer(ps, seq)) continue;
        if (n48_p73_ld32(&s->c80verdict) == N48_P73_C80V_INCOMPLETE) continue;
        n48_p73_st32(&s->c80verdict, N48_P73_C80V_INCOMPLETE);
        c->demoted++;
        if (n48_p73_ld32(&s->state) == N48_P73_ST_COMMITTED) c->demotedLate++;   /* a copy may already have been made */
    }
    return mask;
}
/* A frame the judge never saw (drained): every layer gets an unknown. */
static inline void n48_c80_unjudged(n48_c80 *c)
{
    c->unjudged++;
    n48_c80_unknown(c, n48_c80_all(c));
}

/* ---- the lock-free queue (the hook's two sites outside gXdLock, and the verb) ---- */
static inline void n48_c80_q_lost(n48_c80_q *q, uint32_t seq)
{
    if (!seq) return;
    const uint32_t k = __atomic_fetch_add(&q->n, 1u, __ATOMIC_ACQ_REL);
    if (k < N48_C80_LOSTQ) __atomic_store_n(&q->q[k], seq, __ATOMIC_RELEASE);
}
static inline void n48_c80_q_unjudged(n48_c80_q *q) { (void)__atomic_fetch_add(&q->unjudged, 1u, __ATOMIC_ACQ_REL); }
static inline void n48_c80_q_reset(n48_c80_q *q) { __atomic_store_n(&q->reset, 1u, __ATOMIC_RELEASE); }
/* THE DRAIN, at the top of every judged frame's c80 step (under gXdLock), BEFORE anything is judged: a reset first (the table
 * the verb cleared), then the frames the judge never saw, then the losses. A queue that overflowed (more losses than slots)
 * gives every layer an unknown. Returns the losses drained. */
static inline uint32_t n48_c80_drain(n48_c80 *c, n48_c80_q *q, n48_p73 *t)
{
    if (__atomic_exchange_n(&q->reset, 0u, __ATOMIC_ACQ_REL)) n48_c80_reset(c);
    const uint32_t uj = __atomic_exchange_n(&q->unjudged, 0u, __ATOMIC_ACQ_REL);
    if (uj) {   /* O(1) however many: each layer's unknown count grows by the frames (saturating) */
        c->unjudged += uj;
        for (uint32_t i = 0; i < N48_C80_LAYERS; i++)
            if (c->L[i].used) c->L[i].u = (c->L[i].u > 0xFFFFFFFFu - uj) ? 0xFFFFFFFFu : c->L[i].u + uj;
    }
    const uint32_t n = __atomic_exchange_n(&q->n, 0u, __ATOMIC_ACQ_REL);   /* FIX PASS (SHOULD): claim the count at the top */
    if (!n) return 0u;
    uint32_t done = 0u;
    for (uint32_t k = 0; k < n && k < N48_C80_LOSTQ; k++) {
        const uint32_t s = __atomic_exchange_n(&q->q[k], 0u, __ATOMIC_ACQ_REL);
        if (s) { (void)n48_c80_lost(c, t, s); done++; }
        else { c->lost++; c->lostUnknown++; n48_c80_unknown(c, n48_c80_all(c)); }   /* claimed, not yet stored: unknown */
    }
    if (n > N48_C80_LOSTQ) { c->lost += n - N48_C80_LOSTQ; c->lostUnknown += n - N48_C80_LOSTQ; n48_c80_unknown(c, n48_c80_all(c)); }
    return done;
}

/* THE PER-P LINE's cap: the first N48_C80_LINES_FIRST unconditionally; then only an INCOMPLETE / UNDET verdict or a state change,
 * at most one per N48_C80_LINE_GAP_US (a clock of 0 refuses), and N48_C80_LINES_MAX in all. Counts what it refuses. */
static inline uint32_t n48_c80_line_due(n48_c80 *c, uint32_t verdict, uint32_t changed, uint64_t now_us)
{
    if (c->lines >= N48_C80_LINES_MAX) { c->linesSuppressed++; return 0u; }
    if (c->lines >= N48_C80_LINES_FIRST) {
        if (verdict == N48_P73_C80V_COMPLETE && !changed) return 0u;
        if (!now_us || (c->lastLineUs && now_us - c->lastLineUs < N48_C80_LINE_GAP_US)) { c->linesSuppressed++; return 0u; }
    }
    c->lines++;
    c->lastLineUs = now_us;
    return 1u;
}

/* ---- the copies (present side, one present at a time; not under gXdLock) ---- */
typedef struct { uint64_t present, key; uint32_t seq; uint8_t buf, verdict, pad[2]; } n48_c80_copy;
typedef struct {
    n48_c80_copy r[N48_C80_COPIES];
    uint64_t n;                    /* copies recorded */
    uint64_t lastSumUs, lastSumN, sumLines;
} n48_c80_copies;
/* `buf`: 'A' / 'B' (flip mode's target), 'C' (the console copy: flip mode OFF), '?' unknown. `verdict` N48_P73_C80V_*. */
static inline void n48_c80_copy_note(n48_c80_copies *cp, uint64_t present, uint64_t key, uint32_t seq, uint8_t buf, uint32_t verdict)
{
    n48_c80_copy *e = &cp->r[cp->n % N48_C80_COPIES];
    e->present = present; e->key = key; e->seq = seq; e->buf = buf; e->verdict = (uint8_t)verdict;
    cp->n++;
}
/* One summary line past the first N48_C80_COPY_FIRST copies, at most one per N48_C80_SUM_GAP_US (a clock of 0 refuses). */
static inline uint32_t n48_c80_copy_sum_due(n48_c80_copies *cp, uint64_t now_us)
{
    if (cp->n <= N48_C80_COPY_FIRST || !now_us) return 0u;
    if (cp->lastSumUs && now_us - cp->lastSumUs < N48_C80_SUM_GAP_US) return 0u;
    cp->lastSumUs = now_us; cp->sumLines++;
    return 1u;
}
static inline char n48_c80_verdict_char(uint32_t v)
{
    return v == N48_P73_C80V_COMPLETE ? 'C' : v == N48_P73_C80V_INCOMPLETE ? 'I' : v == N48_P73_C80V_UNDET ? 'U' : '-';
}

/* ---- the lines (bounded under the 491-byte log body at maximal fields by tests/gfx_cycle80_test.cpp) ---- */
/* The bare verb's three report lines. FMT args: mode, how, the 73 note, the 28 note, P judged, COMPLETE, INCOMPLETE, UNDET (no
 * input, table full). FMT2: first sight, state changes, presents HELD incomplete (73's held[5]), would hold (SHADOW), P lines,
 * suppressed, committed writer frames, unjudged, extent assumed, resets, copies recorded, cross-layer (reads unknown). FMT3: the
 * held-back buckets and the losses. */
#define N48_C80_FMT "cyc80: switch 80 %s%s%s%s. P judged %llu: COMPLETE %llu INCOMPLETE %llu UNDET %llu (no input %llu, table " \
    "full %llu)."
#define N48_C80_FMT2 "cyc80: first sight %llu, state changes %llu; presents HELD incomplete %llu, would hold (SHADOW) %llu; P lines " \
    "%u of 64 (suppressed %llu); committed writer frames %llu; unjudged %llu; extent assumed %llu; resets %llu; copies %llu; " \
    "cross-layer %llu (reads unknown %llu)."
#define N48_C80_FMT3 "cyc80: held-back frames readable %llu (wrote a layer %llu, named only by cyc515's set %llu) bounded %llu " \
    "BLIND %llu truncated %llu cyc515-unknown %llu no-record %llu; lost after the gate %llu (not in the ring %llu); demoted " \
    "%llu (slot already COMMITTED %llu); layers evicted %llu."
/* One per used layer: index, ctx, VA, page, extent, state, since-last-P counts, held-back since the arm / at its last P, P tallies. */
#define N48_C80_LAYER_FMT "cyc80: layer %u ctx %#llx VA %#llx page %#llx extent %#llx state %s; since its last P writers %u " \
    "committed %u held %u unknown %u; held-back frames since the arm %llu, at its last P %llu; P %llu COMPLETE %llu INCOMPLETE %llu"
/* FIX PASS (SHOULD): the one eviction line: P judged, evicted so far, and THIS P's layer (index, VA, page). */
#define N48_C80_EVICT_FMT "cyc80: a layer no P sampled for 16 P was EVICTED (once-only line) at P %llu; evicted %llu; this P's " \
    "layer %u VA %#llx page %#llx; a rediscovered layer starts UNKNOWN"
/* The per-P line (CYCLE80.md "Instruments"): mode, frame, seq, plane, layer index, layer VA, page, verdict, writers,
 * committed, held (first held frame), unknown, state. */
#define N48_C80_LINE_FMT "cyc80: %s P f%llu seq %u plane %#llx layer %u %#llx page %#llx -> %s; writers %u committed %u " \
    "held %u (first f%llu) unknown %u; state %s"
/* The bare verb's copy dump, 8 entries per line: first index, last index, total, the entries (" #present:key:Pseq:buf verdict"). */
#define N48_C80_COPY_FMT "cyc80: copies %llu-%llu:%s"   /* entries " #present:plane:P seq:buffer verdict" (C I U -) */
#define N48_C80_COPY_ENT " #%llu:%llx:%u:%c%c"
/* The present side's summary past the first 8 copies, at most one per 2 s: total, since the last, the last copy's fields,
 * held incomplete, would hold. */
#define N48_C80_SUM_FMT "cyc80: %s copies %llu (+%llu since the last summary); last present #%llu plane %#llx P seq %u buffer %c " \
    "verdict %s; presents HELD incomplete %llu, would hold %llu"

#endif /* N48_GFX_CYCLE80_H */
