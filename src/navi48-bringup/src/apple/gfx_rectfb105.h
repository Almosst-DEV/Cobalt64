// gfx_rectfb105.h — build 0.0.547 item 3 ( ranked fix (3); switch 92): SWITCH 105, "rectfb", THE RECTLIST
// FALLBACK MUST NOT COMMIT AS A TRIANGLE.
//
// THE DEFECT (CONFIRMED in code; the black box over "Mon" in RUN AW): with switch 92 ON a segment is translated FLAGGED
// (XLAT12_EXTRA_RECT2D: VGT_GS_OUT_PRIM_TYPE = RECT_2D before each RECTLIST draw). When that flagged pass is refused after it wrote
// (the end rule: RECT_2D still in force at the end of the output; no room; any later refusal), xlat12_ib_translate_draw_ex runs the
// segment again WITHOUT the flag - today's output - and a RECTLIST draw in it runs under the profile's TRISTRIP: 3 vertices, ONE
// triangle, the upper-left half of the rectangle. run11aq: `RECTLIST draws 8099; RECT_2D written 6857 ... fallback 1239`. The
// kext's backstop (rect92_seg: xlat12_ib_rect2d_check) refuses RECT_2D at a non-RECTLIST draw, never the opposite.
//
// `accel gfxneuter 105 | M << 8`: M 1 ON (= 361), M 2 OFF (= 617, the default and the boot value), M 3 SHADOW (= 873). Bare `105`
// reads; any other M is refused unchanged. Joins the continuous mid-arm guard. Writes no register, no page table, nothing of Apple's.
//   ON:     a fallback segment that TRANSLATED and whose output carries a draw with RECTLIST in force (xlat12_ib_rectlist_draws > 0,
//           or an unwalkable output) is REFUSED: XLAT12_IB_ERR_VERIFY, err_op XLAT12_RFB_REFUSED - the gate's usual path (the
//           frame then refuses at N48_CM_SEG_REFUSED, exactly as a rect92 backstop refusal does).
//   SHADOW: counted as ON would refuse; nothing changes.
//   OFF:    nothing changes; the counters and the capped reason line still run (every mode, with 92 ON; they need 92's flag).
// WHY REFUSE AND NOT RE-EMIT: the fallback runs exactly when the flagged pass could not produce a RECT_2D output that passes the
// backstop, so "emit RECT_2D in the fallback" would be a third translation (e.g. a TRISTRIP restore in the tail pad for the end
// rule) that has never run on hardware or been reviewed. Refusing reuses the existing, reviewed refusal. COST: frames refused whole
// (partial execution,'s dominant mechanism) instead of committed with a half-cleared rectangle; SHADOW measures it first.
//
// PURE: compiled into the kext and into tests/gfx_rect92_test.cpp.
#ifndef N48_GFX_RECTFB105_H
#define N48_GFX_RECTFB105_H

#include <stdint.h>
#include "xlat12_ib.h"

#define N48_RFB_SWITCH     105u
#define N48_RFB_LINES_MAX  8u      /* per-segment reason lines per arm */
enum { N48_RFB_M_ON = 1u, N48_RFB_M_OFF = 2u, N48_RFB_M_SHADOW = 3u };

static inline int n48_rfb_set(uint32_t m, uint32_t *mode)
{
    if (m != N48_RFB_M_ON && m != N48_RFB_M_OFF && m != N48_RFB_M_SHADOW) return 0;
    if (mode) *mode = m;
    return 1;
}
static inline const char *n48_rfb_mode_name(uint32_t m)
{
    return m == N48_RFB_M_ON ? "ON (361: a fallback segment carrying a RECTLIST draw is refused)"
         : m == N48_RFB_M_SHADOW ? "SHADOW (873: counted as ON would refuse)"
         : "OFF (617, default: 0.0.546's fallback)";
}

typedef struct {
    uint64_t fallbacks;      /* segments whose flagged pass fell back (r2d_fallback 1) */
    uint64_t fb_refused;     /* ... and the fallback itself was refused (nothing of it can run) */
    uint64_t fb_translated;  /* ... and the fallback translated */
    uint64_t fb_rect;        /* ... carrying >= 1 draw with RECTLIST in force (or unwalkable) */
    uint64_t fb_rect_draws;  /* those draws */
    uint64_t fb_rect_last;   /* fallbacks whose LAST draw is a RECTLIST (the end rule's signature) */
    uint64_t fb_unwalked;    /* outputs the scan could not walk (counted in fb_rect, fail closed) */
    uint64_t refused;        /* ON: refused here */
    uint64_t would_refuse;   /* SHADOW: ON would have refused */
    uint64_t by_op_backstop, by_op_noroom, by_op_other;   /* the flagged pass's refusal: 0xD3, 0xD2, anything else */
    uint64_t committed_frames, committed_segs;            /* committed frames carrying a RECTLIST fallback (OFF / SHADOW) */
    uint64_t lines;
} n48_rfb_st;

enum { N48_RFB_A_NONE = 0u, N48_RFB_A_REFUSE = 1u, N48_RFB_A_WOULD = 2u, N48_RFB_A_KEEP = 3u };
/* One segment after the translate (and after rect92's own backstop). `fallback` ds->r2d_fallback; `status` the segment's status so
 * far; `nrect` xlat12_ib_rectlist_draws over the output (only asked when status is 0); `last` its last-draw flag; `on_op` the flagged
 * pass's err_op. Returns what the caller does: REFUSE (ON), WOULD (SHADOW), KEEP (OFF: a RECTLIST fallback commits as today), NONE. */
static inline uint32_t n48_rfb_seg(n48_rfb_st *st, uint32_t mode, uint32_t fallback, uint32_t status, uint32_t nrect, uint32_t last,
                                   uint32_t on_op)
{
    if (!st || !fallback) return N48_RFB_A_NONE;
    st->fallbacks++;
    if (on_op == 0xD3u) st->by_op_backstop++; else if (on_op == 0xD2u) st->by_op_noroom++; else st->by_op_other++;
    if (status != 0u) { st->fb_refused++; return N48_RFB_A_NONE; }
    st->fb_translated++;
    if (nrect == 0u) return N48_RFB_A_NONE;                    /* a non-RECTLIST fallback: unchanged in every mode */
    st->fb_rect++;
    if (nrect == XLAT12_RL_UNWALKED) st->fb_unwalked++; else st->fb_rect_draws += nrect;
    if (last) st->fb_rect_last++;
    if (mode == N48_RFB_M_ON) { st->refused++; return N48_RFB_A_REFUSE; }
    if (mode == N48_RFB_M_SHADOW) { st->would_refuse++; return N48_RFB_A_WOULD; }
    return N48_RFB_A_KEEP;
}
static inline const char *n48_rfb_action_name(uint32_t a)
{
    return a == N48_RFB_A_REFUSE ? "REFUSED (ON)" : a == N48_RFB_A_WOULD ? "would refuse (SHADOW)"
         : a == N48_RFB_A_KEEP ? "kept as today (OFF)" : "-";
}

/* ---- the lines (each <= 491 bytes at maximal fields: tests/gfx_rect92_test.cpp F5) ---- */
#define N48_RFB_FMT "rectfb: 105 %s%s; flagged-pass fallbacks %llu (backstop %llu, no-room %llu, other %llu); fallback refused %llu, " \
    "translated %llu; lines %llu"
#define N48_RFB_ARGS(m, how, s) n48_rfb_mode_name(m), (how), (unsigned long long)(s)->fallbacks, (unsigned long long)(s)->by_op_backstop, \
    (unsigned long long)(s)->by_op_noroom, (unsigned long long)(s)->by_op_other, (unsigned long long)(s)->fb_refused, \
    (unsigned long long)(s)->fb_translated, (unsigned long long)(s)->lines
#define N48_RFB_FMT2 "rectfb: with RECTLIST draws %llu (draws %llu, last draw %llu, unwalked %llu); ON refused %llu; SHADOW would refuse " \
    "%llu; COMMITTED frames %llu (segments %llu)"
#define N48_RFB_ARGS2(s) (unsigned long long)(s)->fb_rect, (unsigned long long)(s)->fb_rect_draws, \
    (unsigned long long)(s)->fb_rect_last, (unsigned long long)(s)->fb_unwalked, (unsigned long long)(s)->refused, \
    (unsigned long long)(s)->would_refuse, (unsigned long long)(s)->committed_frames, (unsigned long long)(s)->committed_segs
/* The reason line: the frame and segment (its first input dword), the flagged pass's status, err_op, err_reg and input dword (which
 * locate the refusing packet or draw), and what the fallback carried. */
/* build 0.0.548 item B: for err_op 0xD3 (the backstop) the position is the flagged candidate's OUTPUT dword of the refusing
 * packet (its length for the end rule: xlat12_ib_rect2d_check_at), printed as "out dword"; every other err_op's is an input dword. */
#define N48_RFB_SEG_FMT "rectfb: frame %llu seg %u (in dword %u): flagged pass refused status %u err_op %#x reg %#x at %s dword %u; " \
    "fallback status %u, RECTLIST draws %u (last draw %u) -> %s"
#define N48_RFB_SEG_ARGS(frame, k, from, ds, fst, nrect, last, act) (unsigned long long)(frame), (k), (from), (uint32_t)(ds)->r2d_on_st, \
    (ds)->r2d_on_op, (ds)->r2d_on_reg, (ds)->r2d_on_op == XLAT12_R2D_BACKSTOP ? "out" : "in", (ds)->r2d_on_dw, (fst), (nrect), (last), \
    n48_rfb_action_name(act)

#endif
