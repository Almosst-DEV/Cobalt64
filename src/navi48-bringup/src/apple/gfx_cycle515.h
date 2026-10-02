// gfx_cycle515.h — build 0.0.515: READ-ONLY INSTRUMENTS, NO SWITCH, NOTHING DECIDED.
//
// PART A (A1 + A3 of the 0.0.515 brief, WITHOUT A2): WOULD A PRESENT BE GATED ON A COMPLETE CYCLE? WindowServer double-buffers
// two layers (run10u: X 0x400800000, X' 0x404800000); each 3-frame cycle writes ONE layer and the next present (P, the 1040-dword
// final display pass) samples that layer through its D draw. A refused frame of the cycle leaves the layer missing
// part of the picture. A2 (refuse such a P) was NOT BUILT: refusing P leaves P's PLANE unwritten while the display shim still
// copies that plane to the glass on the present (DisplayPipeGuard.cpp dpg_perform copies plane 0 of every transaction; nothing
// there reads the gate), so the glass would show the plane's OLDER content - and until each plane has been written once by a
// gated P that is pre-arm VRAM, i.e. older AND incomplete (the brief's STOP condition). This header only COUNTS what the gate
// would have done, under two reset rules, so the options can be chosen on hardware data:
//   rule A (the brief's): a layer is DIRTY from a refused writer until a COMMITTED P presents it. With the gate ON a P over a
//          dirty layer is refused, so under this rule a dirty layer can never reset (simulated here: dirtyA is never cleared;
//          the counters show how soon and how often that lock-out would bite);
//   rule B: DIRTY from a refused writer until the next P that samples the layer, committed or not (each cycle's first draw
//          repaints the whole layer, run10u F102/F106 @1009 window (0,0)-(1920,1080): SUSPECTED opaque).
// A layer is identified by ROLE: the one tiled input of a 1040-dword single-IB frame's D draw (the translator's own input list,
// the n48_lut_plane_shape pair: one tiled, one linear). A P whose sampled layer is not determined is counted UNDETERMINED.
// A write set is the colour/depth targets Apple's input names (SET_CONTEXT_REG CB_COLOR0..7_BASE, DB_Z/STENCIL_READ/WRITE_BASE,
// value << 8: n48_mib_defer_wscan's registers) over EVERY IB of the frame; a short read, a walk that stops early, a set that
// does not fit, or a LOAD_CONTEXT_REG naming one of those registers makes it UNKNOWN, and an unknown refused writer dirties
// every tracked layer (fail-closed in the gating direction).
//
// PART B: PER-ROW DRAW-ELIDE COUNTERS FOR COMMITTED FRAMES (the evidence gap: the 16 per-elision lines were spent pre-arm).
// Per segment the policy records its latest attempt's elisions (overwritten by a retry); a frame the COMMIT gate answered yes
// for adds the elisions of its segments whose final status is 0, per row, and the first 8 such frames are logged.
//
// Pure: no lock, no clock, no register. The kext holds gXdLock around every call (gfxsrc_decide_frame / gfxsrc_policy).
// Host-tested by tests/gfx_cycle515_test.cpp over run10u's real stream (tests/fixture_cycle515_run10u.h).
#ifndef N48_GFX_CYCLE515_H
#define N48_GFX_CYCLE515_H

#include <stdint.h>

#define N48_CY_MAX     32u    /* tracked surfaces (layers are never evicted) */
#define N48_CY_WS_MAX  24u    /* write-set VAs per frame */
#define N48_CY_LINES    8u    /* capped log lines of P judgments with a dirty layer */

enum { N48_CY_NOTP = 0u, N48_CY_UNDET = 1u, N48_CY_CLEAN = 2u, N48_CY_DIRTY = 3u };

typedef struct {
    uint64_t va;
    uint32_t used, layer;          /* layer: seen as a P's sampled layer (never evicted) */
    uint32_t dirtyA, dirtyB;       /* rule A / rule B */
    uint64_t byA, byB;             /* the frame that last dirtied it under each rule */
    uint64_t touch;                /* LRU tick */
} n48_cy_ent;

typedef struct {
    n48_cy_ent e[N48_CY_MAX];
    uint64_t tick;
    /* THIS frame (reset by n48_cy_frame_begin) */
    uint64_t ws[N48_CY_WS_MAX];
    uint32_t nws, ws_over, ws_unknown;
    uint64_t in_layer;             /* the policy's D-draw input: the tiled VA of a plane-shaped input list */
    uint32_t in_ok;
    /* the last judged frame, for a post-gate withdrawal (keystone / token) */
    uint64_t last_frame;
    uint32_t last_seq, last_committed;
    /* counters (boot totals) */
    uint64_t frames, refusedWriters, unknownRefused, evictDirty, tableFull;
    uint64_t pSeen, pUndet, pCleanA, pDirtyA, pCleanB, pDirtyB, pDirtyBCommitted, pFirstSight;
    uint64_t withdrawn, withdrawUnmatched;
    uint32_t lines;
} n48_cy;

typedef struct {                   /* one P judgment, for the caller's capped line */
    uint32_t kind;                 /* N48_CY_NOTP / UNDET / CLEAN / DIRTY (DIRTY = dirty under A or B) */
    uint32_t dirtyA, dirtyB, log;
    uint64_t layer, byA, byB;
} n48_cy_judge;

static inline void n48_cy_frame_begin(n48_cy *c, uint32_t reader_ok)
{
    c->nws = 0u; c->ws_over = 0u; c->ws_unknown = reader_ok ? 0u : 1u;
    c->in_layer = 0ull; c->in_ok = 0u;
}
static inline void n48_cy_ws_add(n48_cy *c, uint64_t va)
{
    for (uint32_t i = 0; i < c->nws && i < N48_CY_WS_MAX; i++) if (c->ws[i] == va) return;
    if (c->nws >= N48_CY_WS_MAX) { c->ws_over = 1u; return; }
    c->ws[c->nws++] = va;
}
static inline uint32_t n48_cy_is_target(uint32_t r)   /* gfx10 context register dword (0xA000-based) */
{
    return (r >= 0xA318u && r <= 0xA381u && ((r - 0xA318u) % 15u) == 0u) || (r >= 0xA010u && r <= 0xA013u);
}
/* One IB's write set, the WHOLE body [0, n). NOP 0xFFFF1000 and type-2 are one dword; any other non-type-3 header, or a
 * packet running past the body, stops the walk and makes the set UNKNOWN. SET_CONTEXT_REG (0x69) targets are added; a
 * LOAD_CONTEXT_REG (0x61) whose (offset, count) pairs name a target register, a malformed one, or a LOAD_CONTEXT_REG_INDEX
 * (0x9F) makes it UNKNOWN (a value this scan cannot see). */
static inline void n48_cy_ws_scan(n48_cy *c, const uint32_t *in, uint32_t n)
{
    if (!in) { c->ws_unknown = 1u; return; }
    uint32_t i = 0u;
    while (i < n) {
        const uint32_t h = in[i];
        if (h == 0xFFFF1000u || (h >> 30) == 2u) { i++; continue; }
        if ((h >> 30) != 3u) { c->ws_unknown = 1u; return; }
        const uint32_t op = (h >> 8) & 0xFFu, cnt = (h >> 16) & 0x3FFFu;
        if ((uint64_t)i + 2u + cnt > n) { c->ws_unknown = 1u; return; }
        if (op == 0x69u && cnt >= 1u) {
            const uint32_t r0 = 0xA000u + (in[i + 1u] & 0xFFFFu);
            for (uint32_t q = 0; q < cnt; q++) {
                const uint32_t r = r0 + q, v = in[i + 2u + q];
                if (n48_cy_is_target(r) && v) n48_cy_ws_add(c, (uint64_t)v << 8);
            }
        } else if (op == 0x61u) {
            const uint32_t l = cnt + 2u;
            if (l < 4u || ((l - 3u) & 1u) != 0u) { c->ws_unknown = 1u; }
            else {
                for (uint32_t k = i + 3u; k + 1u < i + l; k += 2u) {
                    const uint32_t off = in[k] & 0xFFFFu, cnt2 = in[k + 1u];
                    if (cnt2 > 0x4000u) { c->ws_unknown = 1u; break; }
                    for (uint32_t j = 0; j < cnt2; j++) if (n48_cy_is_target(0xA000u + off + j)) { c->ws_unknown = 1u; break; }
                }
            }
        } else if (op == 0x9Fu) {
            c->ws_unknown = 1u;
        }
        i += cnt + 2u;
    }
}
/* The policy's D-draw input list (xlat12_draw_stats in_va/in_mode/in_n): a plane-shaped pair (one tiled, one linear) names the
 * sampled layer as its tiled entry. Anything else leaves the frame's layer undetermined. */
static inline void n48_cy_note_inputs(n48_cy *c, const uint64_t *va, const uint32_t *mode, uint32_t n, uint32_t over)
{
    c->in_ok = 0u; c->in_layer = 0ull;
    if (!va || !mode || over || n != 2u) return;
    const uint32_t t0 = mode[0] != 0u, t1 = mode[1] != 0u;
    if (t0 == t1) return;
    c->in_layer = t0 ? va[0] : va[1];
    c->in_ok = c->in_layer ? 1u : 0u;
}
static inline n48_cy_ent *n48_cy_find(n48_cy *c, uint64_t va)
{
    for (uint32_t i = 0; i < N48_CY_MAX; i++) if (c->e[i].used && c->e[i].va == va) return &c->e[i];
    return 0;
}
/* Find or insert. A full table evicts the least recently touched NON-layer entry (a dirty one is counted: a later P could have
 * sampled it); a table full of layers answers 0 (counted). */
static inline n48_cy_ent *n48_cy_get(n48_cy *c, uint64_t va, uint32_t as_layer)
{
    n48_cy_ent *e = n48_cy_find(c, va);
    if (!e) {
        for (uint32_t i = 0; i < N48_CY_MAX && !e; i++) if (!c->e[i].used) e = &c->e[i];
        if (!e) {
            for (uint32_t i = 0; i < N48_CY_MAX; i++)
                if (!c->e[i].layer && (!e || c->e[i].touch < e->touch)) e = &c->e[i];
            if (!e) { c->tableFull++; return 0; }
            if (e->dirtyA || e->dirtyB) c->evictDirty++;
        }
        const n48_cy_ent z = { 0ull, 0u, 0u, 0u, 0u, 0ull, 0ull, 0ull };
        *e = z;
        e->va = va; e->used = 1u;
    }
    if (as_layer) e->layer = 1u;
    e->touch = ++c->tick;
    return e;
}
static inline void n48_cy_dirty(n48_cy_ent *e, uint64_t frame)
{
    if (!e) return;
    e->dirtyA = 1u; e->byA = frame;
    e->dirtyB = 1u; e->byB = frame;
}
/* A refused writer: every VA of its write set is dirty; an UNKNOWN (or overflowed) set dirties every tracked layer. */
static inline void n48_cy_mark_refused(n48_cy *c, uint64_t frame)
{
    c->refusedWriters++;
    for (uint32_t i = 0; i < c->nws && i < N48_CY_WS_MAX; i++) n48_cy_dirty(n48_cy_get(c, c->ws[i], 0u), frame);
    if (c->ws_unknown || c->ws_over) {
        c->unknownRefused++;
        for (uint32_t i = 0; i < N48_CY_MAX; i++) if (c->e[i].used && c->e[i].layer) n48_cy_dirty(&c->e[i], frame);
    }
}
/* THE PER-FRAME STEP, after the COMMIT gate answered (`committed` = the gate said COMMIT). `is_p`: a single-IB 1040-dword frame.
 * `judged`: the arm was COMMIT (only then would a gate act; P verdicts are counted only then - the dirt is tracked always,
 * because a frame refused at any level wrote nothing). The P's own sample is judged BEFORE its own write set is applied. */
static inline n48_cy_judge n48_cy_frame(n48_cy *c, uint64_t frame, uint32_t committed, uint32_t is_p, uint32_t judged,
                                        uint32_t seq)
{
    n48_cy_judge j = { N48_CY_NOTP, 0u, 0u, 0u, 0ull, 0ull, 0ull };
    c->frames++;
    if (is_p) {
        if (!c->in_ok) {
            j.kind = N48_CY_UNDET;
            if (judged) { c->pSeen++; c->pUndet++; }
        } else {
            const uint32_t known = n48_cy_find(c, c->in_layer) != 0;
            n48_cy_ent *e = n48_cy_get(c, c->in_layer, 1u);
            j.layer = c->in_layer;
            if (!e) { j.kind = N48_CY_UNDET; if (judged) { c->pSeen++; c->pUndet++; } }
            else {
                j.dirtyA = e->dirtyA; j.dirtyB = e->dirtyB; j.byA = e->byA; j.byB = e->byB;
                j.kind = (e->dirtyA || e->dirtyB) ? N48_CY_DIRTY : N48_CY_CLEAN;
                if (judged) {
                    c->pSeen++;
                    if (!known) c->pFirstSight++;
                    if (e->dirtyA) c->pDirtyA++; else c->pCleanA++;
                    if (e->dirtyB) { c->pDirtyB++; if (committed) c->pDirtyBCommitted++; } else c->pCleanB++;
                    if (j.kind == N48_CY_DIRTY && c->lines < N48_CY_LINES) { c->lines++; j.log = 1u; }
                }
                /* rule A: only a COMMITTED P over a CLEAN (under A) layer presents it - which leaves a clean layer clean;
                 * a dirty one stays dirty (a gated P would be refused and could not reset it). Nothing to do.
                 * rule B: every P that samples the layer ends its cycle. */
                e->dirtyB = 0u;
            }
        }
    }
    if (!committed) n48_cy_mark_refused(c, frame);
    c->last_frame = frame; c->last_seq = seq; c->last_committed = committed;
    return j;
}
/* A frame the gate answered COMMIT for and the hook then withdrew (the keystone / the token): its write set never ran. Only the
 * LAST judged frame's scratch is still held, so a withdrawal naming another gate seq is counted, not applied. */
static inline void n48_cy_withdraw(n48_cy *c, uint32_t seq)
{
    if (!seq) return;
    if (c->last_committed && seq == c->last_seq) { c->withdrawn++; c->last_committed = 0u; n48_cy_mark_refused(c, c->last_frame); }
    else c->withdrawUnmatched++;
}

/* The report lines (bare `gfxneuter 73`), each bounded under the 491-byte body by the test at 20-digit counters. */
#define N48_CY_FMT "cyc515: A2 NOT BUILT (a refused P leaves its plane's OLDER content on the glass); read-only%s. P %llu " \
    "undetermined %llu first-sight %llu; rule A clean %llu DIRTY %llu; rule B clean %llu DIRTY %llu (committed today %llu)."
#define N48_CY2_FMT "cyc515: frames %llu, refused writers %llu (unknown write set %llu); withdrawn after the gate %llu " \
    "(unmatched %llu); evicted dirty %llu, table full %llu; P lines %u of 8."
/* One capped line per P judged DIRTY (the first 8): frame, layer, plane (P's own colour target), rule A/B and who dirtied it. */
#define N48_CY_LINE_FMT "cyc515: P frame %llu samples layer %#llx -> plane %#llx: rule A %s (by f%llu) rule B %s (by f%llu); " \
    "gate today %s."

/* ---------------------------------------------------------------- PART B ---------------------------------------------- */
#define N48_DE515_ROWS 7u    /* xlat12_ib.h XLAT12_DE_ROW_* 1..5 (U, Y, AO, BD, BA), 6 AN (build 0.0.552, switch 110; not on N48_DE515_FMT); 0 = other */
#define N48_DE515_PER   2u   /* XLAT12_DRAW_ELIDE_MAX */
typedef struct {
    uint32_t n;                         /* elisions of this segment's LATEST attempt (0..2) */
    uint32_t at[N48_DE515_PER];         /* input dword of the draw */
    uint32_t va8[N48_DE515_PER];        /* the unproven surface >> 8 */
    uint8_t row[N48_DE515_PER];
    uint8_t pad[6];
} n48_de515_seg;
typedef struct {
    uint64_t byRow[N48_DE515_ROWS];
    uint64_t frames, elisions;
    uint32_t lines;
} n48_de515;
typedef struct { uint32_t seg, at, row; uint64_t va; } n48_de515_one;

/* The segment's latest attempt (a retry overwrites the first attempt's record). */
static inline void n48_de515_seg_set(n48_de515_seg *s, uint32_t n, const uint32_t *at, const uint32_t *va8, const uint8_t *row)
{
    if (!s) return;
    s->n = n <= N48_DE515_PER ? n : N48_DE515_PER;
    for (uint32_t q = 0; q < N48_DE515_PER; q++) {
        s->at[q] = q < s->n && at ? at[q] : 0u;
        s->va8[q] = q < s->n && va8 ? va8[q] : 0u;
        s->row[q] = (uint8_t)(q < s->n && row ? row[q] : 0u);
    }
}
/* A COMMITTED frame: add every segment k < nseg whose final status (`st[k]`) is 0; the first four elisions are handed back.
 * Returns the frame's elisions. The caller calls it only when the COMMIT gate answered yes. */
static inline uint32_t n48_de515_commit(n48_de515 *d, const n48_de515_seg *segs, const uint32_t *st, uint32_t nseg,
                                        n48_de515_one *first, uint32_t nfirst, uint32_t *nout)
{
    uint32_t tot = 0u, o = 0u;
    for (uint32_t k = 0; k < nseg; k++) {
        if (st[k] != 0u) continue;
        for (uint32_t q = 0; q < segs[k].n && q < N48_DE515_PER; q++) {
            const uint32_t r = segs[k].row[q] < N48_DE515_ROWS ? segs[k].row[q] : 0u;
            d->byRow[r]++; tot++;
            if (first && o < nfirst) { first[o].seg = k; first[o].at = segs[k].at[q]; first[o].row = r; first[o].va = (uint64_t)segs[k].va8[q] << 8; o++; }
        }
    }
    if (tot) { d->frames++; d->elisions += tot; }
    if (nout) *nout = o;
    return tot;
}
/* The report line (on every `gfxneuter 66` verb, after the drawelide66 lines) and the capped per-frame line (4 elisions). */
#define N48_DE515_FMT "drawelide515: COMMITTED frames only - elided U %llu Y %llu AO %llu BD %llu BA %llu (other %llu); " \
    "committed frames with an elision %llu; lines %u of 8."
#define N48_DE515_LINE_FMT "drawelide515: committed frame %llu shape %s: %u elided; seg %u@%u %s %#llx; seg %u@%u %s %#llx; " \
    "seg %u@%u %s %#llx; seg %u@%u %s %#llx"

#endif /* N48_GFX_CYCLE515_H */
