// f3_reader.h — WHO WRITES WINDOWSERVER'S F3 TEXTURE (0x400006000)? The full-coverage, read-only reader's decision logic, and the
// per-frame key/time line (0.0.363). Pure C, host-tested by tests/f3_reader_test.cpp; the kext compiles the SAME header.
//
// hp4 asked F3 through WindowServer's own root and could not settle it: 1 of SecurityAgent's 14 frames had its targets
// read, 3 of the 4 frames read in full overflowed the 64-item capture scan (n48_gcap_scan lists colour targets LAST, and only the
// last CB_COLORn_BASE of each slot), 327 frames were skipped because the decision's IB buffer was busy, and the per-frame line was
// cut at the kext's 512-byte log cap right where the reference page was printed. This header replaces the parts that decided:
//   - n48_f3_cb_scan: EVERY colour target a frame binds, not the last per slot: the (CB_COLORn_BASE, _EXT) pair of every slot
//     that holds a base at each DRAW packet and at the end of the walk, plus every DMA_DATA destination in memory. No item cap
//     of 64: the walk is bounded by the IB's length; the only cap is a hard safety cap on DISTINCT targets per IB, which is
//     COUNTED (`capped`) so the caller logs it and the frame's verdict becomes UNSETTLED, never NO.
//   - n48_f3_frame_verdict: YES (a target page equals the reference page), NO (FULL coverage, the reference resolved, no match),
//     UNSETTLED (no match but something was not seen), NOREF (the reference did not resolve), UNREAD (IB 0 not read).
//   - n48_f3_pages_*: a table of every distinct target page seen, so a frame that ran BEFORE the reference resolved (e.g. before
//     WindowServer was bound) is compared RETROSPECTIVELY the moment it does. A retrospective match is SUSPECTED by construction
//     (a VRAM page can be freed and reused between the two moments) and is printed as such.
//   - n48_f3_sdma_scan: the destinations of the SDMA packets that can write memory (COPY, WRITE, CONST_FILL), so the drain can
//     say whether an SDMA packet it services writes the reference page. Opcode/sub-opcode layout: gfx10 SDMA (upstream
//     navi10_sdma_pkt_open.h), SUSPECTED here - no copy of that header is in the repo; every length is used ONLY to step to the
//     next packet, and a packet whose length this table does not know stops the walk AFTER its destination is recorded (the
//     stop is reported, so an unknown layout can only lose coverage, never invent a match).
// Colour-target semantics (SUSPECTED, conservative): a slot that holds a base at a draw is counted as written even if its
// CB_COLORn_INFO format is INVALID or its target mask is 0. That can only ADD targets - a YES from it is "bound at a draw", which
// the reader's per-target line names by slot so it can be checked; it can never turn a writer into a NO.
#ifndef N48_F3_READER_H
#define N48_F3_READER_H

#include <stdint.h>
#include "gfx_xlat_verdict.h"

#define N48_F3_TEX_VA      0x400006000ull   /* wsgc1's F3 window texture */
#define N48_F3_TGT_CAP     128u             /* the hard safety cap on DISTINCT targets per IB (logged when hit) */
#define N48_F3_PAGES_CAP   512u             /* distinct target pages kept for the retrospective comparison */
#define N48_F3_SDMA_CAP    64u              /* SDMA destinations per IB */

enum { N48_F3_T_CB = 1u, N48_F3_T_DMA = 2u, N48_F3_T_SDMA = 3u };
typedef struct { uint64_t va; uint32_t kind, slot, dword; } n48_f3_tgt;
typedef struct {
    uint32_t walked;   /* dwords walked (the well-formed prefix) */
    uint32_t draws;    /* draw packets seen */
    uint32_t nested;   /* nested INDIRECT_BUFFER(_CONST) packets: their targets are NOT seen here (coverage loss, counted) */
    uint32_t total;    /* targets found (distinct while under the cap; past it, per occurrence) */
    uint32_t stored;   /* of which stored (<= max), distinct */
    uint32_t capped;   /* occurrences NOT stored because the safety cap was hit (>= the distinct count; only != 0 matters) */
} n48_f3_scan;

static inline void n48_f3_add(n48_f3_tgt *t, uint32_t max, n48_f3_scan *s, uint32_t kind, uint32_t slot, uint32_t dw, uint64_t va)
{
    if (!va) return;
    for (uint32_t i = 0; i < s->stored; i++)
        if (t[i].va == va && t[i].kind == kind) return;
    /* a value past the cap may repeat: it is counted once per occurrence past the cap, which can only OVER-report a cap hit */
    s->total++;
    if (s->stored < max) { t[s->stored].va = va; t[s->stored].kind = kind; t[s->stored].slot = slot; t[s->stored].dword = dw; s->stored++; }
    else s->capped++;
}

/* Draw packets (gfx10 PM4, Mesa sid.h): DRAW_INDIRECT 0x24, DRAW_INDEX_INDIRECT 0x25, DRAW_INDEX_2 0x27, DRAW_INDIRECT_MULTI 0x2c,
 * DRAW_INDEX_AUTO 0x2d, DRAW_INDEX_MULTI_AUTO 0x30, DRAW_INDEX_OFFSET_2 0x35, DRAW_INDEX_INDIRECT_MULTI 0x38. */
static inline int n48_f3_is_draw(uint32_t op)
{
    return op == 0x24u || op == 0x25u || op == 0x27u || op == 0x2cu || op == 0x2du || op == 0x30u || op == 0x35u || op == 0x38u;
}

static inline void n48_f3_bound(n48_f3_tgt *t, uint32_t max, n48_f3_scan *s, const uint32_t *cbBase, const uint32_t *cbExt,
                                const uint32_t *cbAt)
{
    for (uint32_t n = 0; n < 8u; n++)
        if (cbBase[n]) n48_f3_add(t, max, s, N48_F3_T_CB, n, cbAt[n], (((uint64_t)(cbExt[n] & 0xffu)) << 40) | (((uint64_t)cbBase[n]) << 8));
}

/* Returns the dwords walked. The walk is n48_gcap_scan's (type-3 packets, type-2 fillers, stops at anything else or a packet
 * running past n); CB_COLORn_BASE = context dword 0xa318 + 15n, _EXT = 0xa390 + n (gfx_capture_scan.h, Mesa ac_descriptors.c). */
static inline uint32_t n48_f3_cb_scan(const uint32_t *d, uint32_t n, n48_f3_tgt *t, uint32_t max, n48_f3_scan *s)
{
    uint32_t cbBase[8] = { 0 }, cbExt[8] = { 0 }, cbAt[8] = { 0 }, i = 0;
    s->walked = s->draws = s->nested = s->total = s->stored = s->capped = 0;
    while (d && i < n) {
        const uint32_t h = d[i], type = h >> 30;
        if (type == 2u) { i++; continue; }
        if (type != 3u) break;
        const uint32_t op = (h >> 8) & 0xffu, nb = ((h >> 16) & 0x3fffu) + 1u;
        if (nb + 1u > n - i) break;
        const uint32_t *b = &d[i + 1u];
        if ((op == 0x69u || op == 0x6au) && nb >= 2u) {           /* SET_CONTEXT_REG (_INDEX): offset in the low 16 bits */
            const uint32_t reg0 = 0xa000u + (b[0] & 0xffffu);
            for (uint32_t k = 1; k < nb; k++) {
                const uint32_t reg = reg0 + (k - 1u);
                if (reg >= 0xa318u && reg < 0xa318u + 15u * 8u && ((reg - 0xa318u) % 15u) == 0u) {
                    const uint32_t slot = (reg - 0xa318u) / 15u;
                    cbBase[slot] = b[k]; cbAt[slot] = i + 1u + k;
                } else if (reg >= 0xa390u && reg < 0xa398u) cbExt[reg - 0xa390u] = b[k];
            }
        } else if (n48_f3_is_draw(op)) {
            s->draws++;
            n48_f3_bound(t, max, s, cbBase, cbExt, cbAt);
        } else if (op == 0x50u && nb >= 5u) {                     /* DMA_DATA: control, src lo/hi, dst lo/hi, command */
            const uint32_t dstSel = (b[0] >> 20) & 3u;            /* 0 = memory (DAS), 3 = memory no-L2 (SUSPECTED, sid.h) */
            if (dstSel == 0u || dstSel == 3u) n48_f3_add(t, max, s, N48_F3_T_DMA, 0u, i, (((uint64_t)b[4]) << 32) | (b[3] & ~3u));
        } else if (op == 0x3fu || op == 0x33u) s->nested++;
        i += nb + 1u;
    }
    n48_f3_bound(t, max, s, cbBase, cbExt, cbAt);                 /* the end of the walk: what is still bound */
    s->walked = i;
    return i;
}

/* ---- whether a NOT-WindowServer frame is read. TWO inputs, and BOTH must hold:
 *   `enabled`   the runtime switch (`accel gfxneuter 14 | 1 << 8`), N48_F3_READ_DEFAULT at boot;
 *   `allocated` the reader's own lock AND its own buffer exist.
 * Nothing else is an input - not whether another path holds a buffer (hp4: 327 frames skipped "buffer busy"), not a 1-in-N
 * sample (hp4: 270 skipped), not how many were read before. The inputs the retired policy used are parameters here only so the
 * test can prove they still do not matter.
 *
 * 0.0.370 — WHY THE SWITCH EXISTS AND WHY ITS DEFAULT IS OFF. Through 0.0.369 `allocated` was the ONLY input, and
 * the buffer was allocated by `accel gfxneuter 3` - so arming the DECISION armed the reader too, with no way back short of a
 * reboot. The reader runs on Apple's own submit thread and hp5 measured its worst frame at 128 ms; hp9 still paid
 * it. That is a measurement instrument, not something an ARMED run may be made to carry, so it is OFF unless a run asks for it.
 * The switch cannot make the reader do anything but read: every path behind it is read-only, and that is unchanged. ---- */
#define N48_F3_READ_DEFAULT 0u      /* OFF. An armed run never pays for the reader unless it was asked for by name. */
static inline uint32_t n48_f3_should_read(uint32_t enabled, uint32_t allocated, uint32_t otherPathBusy, uint32_t frameIndex)
{
    (void)otherPathBusy; (void)frameIndex;
    return (enabled && allocated) ? 1u : 0u;
}

/* ---- the comparison and the frame's verdict ---- */
typedef struct { uint32_t ok, sys; uint64_t page; } n48_f3_ref;
enum { N48_F3_CMP_NO = 0u, N48_F3_CMP_YES = 1u, N48_F3_CMP_UNRES = 2u };
static inline uint32_t n48_f3_cmp(const n48_f3_ref *r, uint32_t ok, uint32_t sys, uint64_t page)
{
    if (!ok || !r->ok) return N48_F3_CMP_UNRES;
    return (page == r->page && sys == r->sys) ? N48_F3_CMP_YES : N48_F3_CMP_NO;
}

enum { N48_F3_V_YES = 0u, N48_F3_V_NO, N48_F3_V_UNSETTLED, N48_F3_V_NOREF, N48_F3_V_UNREAD, N48_F3_V_COUNT };
static inline const char *n48_f3_verdict_name(uint32_t v)
{
    static const char *const n[N48_F3_V_COUNT] = { "YES", "NO", "UNSETTLED", "NO-REFERENCE", "UNREAD" };
    return v < N48_F3_V_COUNT ? n[v] : "?";
}
typedef struct {
    uint32_t head_ok;      /* IB 0 read through the submitter's own root is a PM4 type-3 header */
    uint32_t ref_ok;       /* the reference page resolved through WindowServer's own root */
    uint32_t ibs, ibs_full;/* IBs, and those read AND walked to exactly their length */
    uint32_t capped, nested;
    uint32_t targets, matches, unresolved;
} n48_f3_frame;
/* A match is a match whatever else was missed; NO needs every IB read and walked in full, no cap hit, no nested IB unfollowed and
 * every target resolved - anything less is UNSETTLED. */
static inline uint32_t n48_f3_frame_verdict(const n48_f3_frame *f)
{
    if (!f->head_ok) return N48_F3_V_UNREAD;
    if (f->matches) return N48_F3_V_YES;
    if (!f->ref_ok) return N48_F3_V_NOREF;
    if (f->ibs == 0u || f->ibs_full != f->ibs || f->capped || f->nested || f->unresolved) return N48_F3_V_UNSETTLED;
    return N48_F3_V_NO;
}
/* Why a frame is UNSETTLED, the first cause in the order above (for the short line). */
static inline const char *n48_f3_unsettled_why(const n48_f3_frame *f)
{
    if (f->ibs == 0u) return "no IB";
    if (f->ibs_full != f->ibs) return "an IB not read/walked in full";
    if (f->capped) return "target CAP HIT";
    if (f->nested) return "nested IB not followed";
    if (f->unresolved) return "a target page unresolved";
    return "-";
}

/* ---- the distinct-page table, for the retrospective comparison ---- */
typedef struct { uint64_t page, va; uint32_t sys, kind, frame, vmid, hits; int32_t pid; } n48_f3_page;
typedef struct { n48_f3_page p[N48_F3_PAGES_CAP]; uint32_t used; uint32_t over; } n48_f3_pages;
/* Returns 1 when the page is new (stored or, if the table is full, counted in `over`), 0 when already held. */
static inline uint32_t n48_f3_pages_note(n48_f3_pages *t, uint64_t page, uint32_t sys, uint32_t kind, uint64_t va, uint32_t frame,
                                         uint32_t vmid, int32_t pid)
{
    for (uint32_t i = 0; i < t->used; i++)
        if (t->p[i].page == page && t->p[i].sys == sys) { t->p[i].hits++; return 0u; }
    if (t->used >= N48_F3_PAGES_CAP) { t->over++; return 1u; }
    n48_f3_page *e = &t->p[t->used++];
    e->page = page; e->sys = sys; e->kind = kind; e->va = va; e->frame = frame; e->vmid = vmid; e->pid = pid; e->hits = 1u;
    return 1u;
}
/* The indices (at most max) of every held page equal to the reference; returns the count of matches (may exceed max). */
static inline uint32_t n48_f3_pages_retro(const n48_f3_pages *t, const n48_f3_ref *r, uint32_t *idx, uint32_t max)
{
    uint32_t m = 0;
    if (!r->ok) return 0u;
    for (uint32_t i = 0; i < t->used; i++)
        if (t->p[i].page == r->page && t->p[i].sys == r->sys) { if (m < max && idx) idx[m] = i; m++; }
    return m;
}
/* Whether the reference changed in a way that warrants a retrospective pass (first resolution, or a different page). */
static inline int n48_f3_ref_changed(const n48_f3_ref *was, const n48_f3_ref *now)
{
    return now->ok && (!was->ok || was->page != now->page || was->sys != now->sys);
}

/* ---- the SDMA watch: destinations of packets that write memory (gfx10 SDMA; SUSPECTED layouts, see the top) ---- */
typedef struct { uint64_t va; uint32_t op, sub, at; } n48_f3_sdst;
typedef struct { uint32_t walked, total, stored, stopHdr, stopped; } n48_f3_sscan;
static inline void n48_f3_sdma_add(n48_f3_sdst *o, uint32_t max, n48_f3_sscan *s, uint32_t op, uint32_t sub, uint32_t at, uint64_t va)
{
    s->total++;
    if (s->stored < max) { o[s->stored].va = va; o[s->stored].op = op; o[s->stored].sub = sub; o[s->stored].at = at; s->stored++; }
}
static inline uint64_t n48_f3_q(const uint32_t *d, uint32_t k) { return (((uint64_t)d[k + 1u]) << 32) | d[k]; }
/* Returns the number of destinations found (total; at most max stored). s->stopped = 1 when the walk did not reach n exactly. */
static inline uint32_t n48_f3_sdma_scan(const uint32_t *d, uint32_t n, n48_f3_sdst *o, uint32_t max, n48_f3_sscan *s)
{
    uint32_t k = 0;
    s->walked = s->total = s->stored = s->stopHdr = s->stopped = 0;
    while (d && k < n) {
        const uint32_t h = d[k], op = h & 0xffu, sub = (h >> 8) & 0xffu;
        uint32_t len = 0;
        int dst = -1;                    /* dword offset of the destination address pair, -1 none */
        switch (op) {
        case 0:  len = 1u + ((h >> 16) & 0x3fffu); break;                              /* NOP */
        case 1:                                                                          /* COPY */
            if (sub == 0u) { len = 7u; dst = 5; }                                       /* LINEAR: count, param, src, dst */
            else if (sub == 1u) { len = 14u; dst = (h >> 31) ? 9 : 1; }                 /* TILED: detile -> the linear side */
            else if (sub == 4u) { len = 13u; dst = 6; }                                 /* LINEAR_SUB_WINDOW */
            else if (sub == 5u) { len = 14u; dst = (h >> 31) ? 9 : 1; }                 /* TILED_SUB_WINDOW */
            else if (sub == 6u) { len = 15u; dst = 7; }                                 /* T2T_SUB_WINDOW */
            break;
        case 2:                                                                          /* WRITE */
            if (sub == 0u) { if (k + 3u < n) len = 4u + (d[k + 3u] & 0xfffffu) + 1u; dst = 1; }   /* UNTILED: count-1 at dw3 */
            else dst = 1;                                                                /* TILED etc: dst known, length not */
            break;
        case 5:  len = 4u; break;  case 6: len = 1u; break;  case 8: len = 6u; break;  case 9: len = 5u; break;
        case 11: len = 5u; dst = 1; break;                                              /* CONST_FILL: dst, data, count */
        case 12: len = 10u; break; case 13: len = 3u; break; case 14: len = 3u; break; case 16: len = 4u; break;
        default: break;
        }
        if (dst >= 0 && k + (uint32_t)dst + 1u < n) n48_f3_sdma_add(o, max, s, op, sub, k, n48_f3_q(d, k + (uint32_t)dst));
        if (len == 0u || len > n - k) { s->stopHdr = h; break; }
        k += len;
    }
    s->walked = k;
    s->stopped = (k != n) ? 1u : 0u;
    return s->total;
}

/* ---- the per-frame key: the first unknown/refused program's, whatever rung the verdict stopped at ---- */
enum { N48_F3_K_NONE = 0u, N48_F3_K_UNKNOWN, N48_F3_K_NO_XLAT, N48_F3_K_NOT_OURS, N48_F3_K_NOPGM,
       N48_F3_K_TABLE_OVERFLOW /* 0.0.387: our table ran out; no program of this frame was examined */ };
static inline const char *n48_f3_key_class_name(uint32_t c)
{
    static const char *const n[6] = { "all-programs-ours", "unknown", "no-translation", "not-substituted", "no-program-read",
                                      "program-table-overflow" };
    return c < 6u ? n[c] : "?";
}
/* The verdict's own key when it names one; otherwise the first program in ladder order (unknown, then no-translation, then not
 * ours and not relocated). A key of 0 with class `unknown` is a program the kext could not key (the Xghc bucket, / SHADER
 * ROUND 2) - printed as such, never hidden. */
static inline uint64_t n48_f3_frame_key(const n48_xv_frame *f, uint32_t verdict, uint64_t vkey, uint32_t *cls)
{
    const uint32_t np = f->npgm < N48_XV_MAX_PGMS ? f->npgm : N48_XV_MAX_PGMS;
    /* 0.0.387: a frame the program table could not hold has an answer of its own. Without this clause it fell through
     * to the ladder below, which would have named the first UNKNOWN among the entries the gather DID store and printed
     * class `unknown` - the same conflation found in the tally, re-made one line lower. */
    if (verdict == N48_XV_PGM_TABLE_OVERFLOW) { *cls = N48_F3_K_TABLE_OVERFLOW; return 0u; }
    if (verdict == N48_XV_PGM_UNKNOWN) { *cls = N48_F3_K_UNKNOWN; return vkey; }
    if (verdict == N48_XV_PGM_NO_XLAT) { *cls = N48_F3_K_NO_XLAT; return vkey; }
    if (verdict == N48_XV_PGM_NOT_OURS) { *cls = N48_F3_K_NOT_OURS; return vkey; }
    if (np == 0u) { *cls = N48_F3_K_NOPGM; return 0u; }
    for (uint32_t k = 0; k < np; k++) if (f->pgm[k].key_class == N48_XV_PGM_KEY_UNKNOWN) { *cls = N48_F3_K_UNKNOWN; return f->pgm[k].key; }
    for (uint32_t k = 0; k < np; k++) if (f->pgm[k].key_class == N48_XV_PGM_KEY_NO_XLAT) { *cls = N48_F3_K_NO_XLAT; return f->pgm[k].key; }
    for (uint32_t k = 0; k < np; k++)
        if (!f->pgm[k].bytes_are_ours && !f->pgm[k].relocated) { *cls = N48_F3_K_NOT_OURS; return f->pgm[k].key; }
    *cls = N48_F3_K_NONE;
    return 0u;
}

/* ---- the short lines' formats, here so the test can bound their worst-case length under the kext's 512-byte cap
 * (amd/n48log.cpp: `char line[512]`, and HWLOG adds "AppleHardwareHook: " and "\n": 20 bytes) ---- */
#define N48_LOG_CAP_BODY 491u
/* args: frame#, pid, name, vmid, targets, cover, shown kind, tgt va, tgt page-kind, tgt page, ref va, ref page-kind, ref page, verdict,
 * why */
#define N48_F3_LINE_FMT "f3-writer: frame #%u pid %d '%s' VMID %u: %u target(s), coverage %s; %s VA %#llx -> %s page %#llx == WS %#llx " \
                        "page %s %#llx? %s%s%s"

/* build 0.0.474 item 1 (10B-COVERAGE.md Q4 item 4 contract (1)) — WHY nseg IS 0, LOG-ONLY. Through 0.0.473 a
 * frame whose verdict was `segment-policy` with nseg 0 (gfx_xlat_verdict.h: the divisor skipped it, the policy ran
 * and genuinely found nothing, or the policy was never eligible to run at all) printed the SAME line as every
 * other segment-policy frame - a 10b run could not tell which of the three it was without re-deriving it from the
 * policy's own counters. Pure and host-tested (tests/f3_reader_test.cpp) so the classification is exactly what a
 * test can drive, not an inline if/else only the kext runs; the kext calls this ONE function with the two booleans
 * its own if/else-if/else already computed (mutually exclusive by construction) rather than repeating the ladder.
 * Meaningful ONLY when the caller's own verdict is N48_XV_SEG_POLICY and f->nseg == 0 (checked at the print site,
 * not here - this function does not see `f`). */
enum { N48_WSF_NSEG_NOT_ELIGIBLE = 0u, N48_WSF_NSEG_SAMPLED, N48_WSF_NSEG_ZERO_SEG, N48_WSF_NSEG_REASONS };

static inline const char *n48_wsf_nseg_cause_name(uint32_t c)
{
    switch (c) {
    case N48_WSF_NSEG_SAMPLED:  return "SAMPLED";
    case N48_WSF_NSEG_ZERO_SEG: return "ZERO-SEG";
    default:                    return "NOT-ELIGIBLE";
    }
}

/* `sampled_skip` is 1 exactly on the frame the policy divisor skipped this pass; `ran_policy` is 1 exactly when
 * gfxsrc_policy was actually called for this frame. If a caller ever manages to set both, SAMPLED wins - the
 * earlier-checked condition in the kext's own stride order is the truer cause, never a silent double count. */
static inline uint32_t n48_wsf_nseg_cause(uint32_t sampled_skip, uint32_t ran_policy)
{
    if (sampled_skip) return N48_WSF_NSEG_SAMPLED;
    if (ran_policy) return N48_WSF_NSEG_ZERO_SEG;
    return N48_WSF_NSEG_NOT_ELIGIBLE;
}

/* args: global frame#, ws frame since bind, bind#, up s, up us, pid, stamp, nib, verdict, key, class, seg_kind, action,
 * nseg0 cause (or "-"), nseg0 IB0 start class name (or "-", meaningful only when the cause is ZERO-SEG). The class
 * name is the CALLER's own string (AppleHardwareHook.cpp already includes gfx_mib.h for n48_mib_start_class/
 * n48_mib_start_name, used elsewhere in the same file) - this header does not include gfx_mib.h itself, so the
 * f3_reader host suite (tests/f3_reader_test.cpp, compiled with -I src/apple only, no xlat12 on its include path)
 * keeps building without it.
 * 0.0.370: `seg_kind` is n48_cm_kind_name of the kind THIS frame's policy pass declared - UNSET when the policy
 * did not run for it, which is the honest answer and never "ENCODER". hp9 could only SUSPECT which frames the headless
 * recogniser carried; this names it per frame. */
/* build 0.0.481: one more trailing %s - N48_UNITS_MARK when THIS frame's policy pass formed
 * switch-55 units (its segment table held UNITS, not Apple's segments), else "" (the line's text is 0.0.474's, byte for
 * byte). The kext passes gfx_mib.h's n48_mib_units_mark; tests/f3_reader_test.cpp bounds the line WITH the marker. */
#ifndef N48_UNITS_MARK
#define N48_UNITS_MARK " units"   /* the SAME text gfx_mib.h defines for n48_mib_units_mark (either header may come first) */
#endif
#define N48_WSF_LINE_FMT "gfx-xlat: frame %llu ws#%u (bind %u) [up %llu.%06llu s] pid %d stamp %#x %u IB(s) VERDICT %s key %#018llx " \
                         "(%s) seg_kind %s -> %s nseg0 %s class %s%s"
#define N48_WSF_LINES 200u     /* key/time lines per WindowServer binding */

#endif
