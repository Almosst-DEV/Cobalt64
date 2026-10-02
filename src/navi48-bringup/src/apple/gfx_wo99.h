// gfx_wo99.h — build 0.0.541 item 6 (from the xhigh study of RUN AQ2's stopped commits): SWITCH 99, "wo99",
// THE WITNESS-OVERFLOW RELAXATION's switch and its instruments. The RULE itself is gfx_dep.h's (n48_dep_wo_relaxable,
// n48_dep_check_m); this header is the switch's setter, the counters, the per-second window, and every line's format.
//
// THE MECHANISM (CONFIRMED by the study): every refused frame's colour-target pages are noted into the arm-scoped witness
// (gXdDepArm, 64 rows); with switch 28 ON the fill sets `witness_over = cp_wt_over + cp_wt_trunc` and n48_dep_check refuses on
// it at a count of ONE; `over` resets only in n48_dep_arm_scope; every frame refused there is itself neutered and noted, so the
// refusal latches for the rest of the arm. With 30 ON no evaluator reads the rows (n48_cp_eval_hz / _fill_hz / _hz_d4 only
// null-check `wt`; n48_cp_in_witness is called only by the 30-OFF n48_cp_eval).
//
// `accel gfxneuter 99 | M << 8`: M 1 ON (= 355: n48_dep_check skips the witness-overflow rung when n48_dep_wo_relaxable, and
// counts each frame the old rule would have refused there), M 2 OFF (= 611, the default and the boot value: 0.0.540's rule),
// M 3 SHADOW (= 867: refuses as today, and counts / logs each frame the relaxed rule would have passed). Bare `99` reads. Joins
// the continuous mid-arm guard (gfx_commit.h). Writes no register, no page table, nothing of Apple's.
//
// PURE: compiled into the kext and into tests/gfx_wo99_test.cpp.
#ifndef N48_GFX_WO99_H
#define N48_GFX_WO99_H

#include <stdint.h>
#include "gfx_dep.h"

#define N48_WO_SWITCH 99u
enum { N48_WO_M_ON = 1u, N48_WO_M_OFF = 2u, N48_WO_M_SHADOW = 3u };   /* the switch's M, held as is; OFF at boot */

/* The setter: M 1, 2, 3 set it (1 = changed); anything else 0, *m untouched. */
static inline int n48_wo_set(uint32_t m, uint32_t *mode)
{
    if (m != N48_WO_M_ON && m != N48_WO_M_OFF && m != N48_WO_M_SHADOW) return 0;
    if (mode) *mode = m;
    return 1;
}
/* The switch's M -> the world's wo_mode (gfx_dep.h N48_DEP_WO_*): OFF and anything unknown are N48_DEP_WO_OFF. */
static inline uint32_t n48_wo_world_mode(uint32_t m)
{
    return m == N48_WO_M_ON ? (uint32_t)N48_DEP_WO_ON : m == N48_WO_M_SHADOW ? (uint32_t)N48_DEP_WO_SHADOW : (uint32_t)N48_DEP_WO_OFF;
}
static inline const char *n48_wo_mode_name(uint32_t m)
{
    return m == N48_WO_M_ON ? "ON (355: the witness-overflow rung is skipped when 28+30 judged the consumer without the rows)"
         : m == N48_WO_M_SHADOW ? "SHADOW (867: refused as today; frames the relaxed rule would pass are counted)"
         : "OFF (611, default: 0.0.540's rule)";
}

/* ---- per gate: what the two rules answered ---- */
enum { N48_WO_E_NONE = 0u, N48_WO_E_SKIPPED, N48_WO_E_WOULD_PASS };
typedef struct {
    uint64_t gates;        /* gate dependency checks seen */
    uint64_t over_gates;   /* ... whose world carried witness_over > 0 */
    uint64_t relaxable;    /* ... and n48_dep_wo_relaxable */
    uint64_t skipped;      /* ON: the old rule would have refused at the witness rung, the new one went past it */
    uint64_t skipped_ok;   /* ... and the whole check then answered clean */
    uint64_t would_pass;   /* SHADOW: refused at the witness rung, the relaxed rule would have answered clean */
    uint64_t lines;        /* SHADOW / ON per-frame lines printed (capped per arm) */
} n48_wo_st;

/* One gate's classification from the world the gate judged. `today` is n48_dep_check_m(w, 0), `relaxed` n48_dep_check_m(w, 1). */
static inline uint32_t n48_wo_gate(n48_wo_st *st, const n48_dep_world *w, uint32_t today, uint32_t relaxed)
{
    st->gates++;
    if (!w || !w->witness_over) return N48_WO_E_NONE;
    st->over_gates++;
    if (!n48_dep_wo_relaxable(w)) return N48_WO_E_NONE;
    st->relaxable++;
    if (today != N48_DEP_WITNESS_OVER) return N48_WO_E_NONE;   /* refused earlier, or not at all: the rung never decided */
    if (w->wo_mode == N48_DEP_WO_ON) {
        st->skipped++;
        if (relaxed == N48_DEP_OK) st->skipped_ok++;
        return N48_WO_E_SKIPPED;
    }
    if (w->wo_mode == N48_DEP_WO_SHADOW && relaxed == N48_DEP_OK) { st->would_pass++; return N48_WO_E_WOULD_PASS; }
    return N48_WO_E_NONE;
}

/* ---- the per-second window while armed: the witness state and the dependency refusal histogram ---- */
#define N48_WO_SEC_US    1000000ull
#define N48_WO_SEC_LINES 240u
#define N48_WO_PER_ARM_LINES 8u   /* per-frame SHADOW / ON lines per arm */
typedef struct {
    uint32_t open, lines;
    uint64_t t0, a0, suppressed;
    uint64_t cnt[N48_DEP_REASONS];
    uint64_t gates;
    /* build 0.0.550: this second's COMMITTED frames, split composite (not P-shaped) / P, so a
     * composite freeze shows on the line even while P copies keep committing. Counted by n48_wo_sec_commit; log-only. */
    uint64_t comp, pcm;
} n48_wo_win;
typedef struct { uint64_t cnt[N48_DEP_REASONS]; uint64_t gates, at_ms, len_ms; uint32_t line; uint64_t comp, pcm; } n48_wo_secline;
/* Count one gate's reason; 1 = a line is due (filled into *out). Not armed: the window closes, nothing is counted. */
static inline uint32_t n48_wo_sec_tick(n48_wo_win *w, uint32_t armed, uint64_t now_us, uint32_t reason, n48_wo_secline *out)
{
    if (!armed) { w->open = 0u; return 0u; }
    if (!w->open) {
        w->open = 1u; w->t0 = now_us; w->a0 = now_us; w->lines = 0u; w->gates = 0ull;
        w->comp = 0ull; w->pcm = 0ull;   /* build 0.0.550 */
        for (uint32_t i = 0; i < N48_DEP_REASONS; i++) w->cnt[i] = 0ull;
    }
    w->gates++;
    if (reason < N48_DEP_REASONS) w->cnt[reason]++;
    if (now_us < w->t0 || now_us - w->t0 < N48_WO_SEC_US) return 0u;
    uint32_t due = 0u;
    if (w->lines < N48_WO_SEC_LINES) {
        w->lines++;
        for (uint32_t i = 0; i < N48_DEP_REASONS; i++) out->cnt[i] = w->cnt[i];
        out->gates = w->gates; out->at_ms = (w->t0 - w->a0) / 1000ull; out->len_ms = (now_us - w->t0) / 1000ull;
        out->line = w->lines;
        out->comp = w->comp; out->pcm = w->pcm;   /* build 0.0.550 */
        due = 1u;
    } else {
        w->suppressed++;
    }
    w->t0 = now_us; w->gates = 0ull;
    w->comp = 0ull; w->pcm = 0ull;   /* build 0.0.550 */
    for (uint32_t i = 0; i < N48_DEP_REASONS; i++) w->cnt[i] = 0ull;
    return due;
}
/* build 0.0.550: ONE COMMITTED FRAME into the open window - `is_p` a P-shaped frame
 * (gfx_present73.h n48_p73_is_p_shape), else a composite. A closed window (not armed, or 99 OFF with perf540 OFF) counts nothing.
 * Log-only: nothing reads these but the per-second line. */
static inline void n48_wo_sec_commit(n48_wo_win *w, uint32_t is_p)
{
    if (!w || !w->open) return;
    if (is_p) w->pcm++; else w->comp++;
}
/* The line prints each count clamped to 32 bits (a per-second count; the clamp only bounds the line's width). */
static inline uint32_t n48_wo_u32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v; }
/* The histogram as text: `reason:count` for every non-zero reason, in index order, each count shown at most 9999999 ("+" past
 * it). Always NUL-terminated; at most N48_WO_HIST_MAX bytes (21 reasons x "20:9999999+ " = 252). */
#define N48_WO_HIST_MAX 256u
static inline void n48_wo_hist(const uint64_t *cnt, char *buf)
{
    uint32_t o = 0u;
    for (uint32_t i = 0; i < N48_DEP_REASONS; i++) {
        if (!cnt[i]) continue;
        uint64_t v = cnt[i] > 9999999ull ? 9999999ull : cnt[i];
        char tmp[24]; uint32_t t = 0u;
        if (cnt[i] > 9999999ull) tmp[t++] = '+';
        do { tmp[t++] = (char)('0' + (v % 10ull)); v /= 10ull; } while (v && t < 20u);
        char head[4]; uint32_t h = 0u;
        if (i >= 10u) head[h++] = (char)('0' + i / 10u);
        head[h++] = (char)('0' + i % 10u);
        if (o + h + 1u + t + 1u >= N48_WO_HIST_MAX) break;
        for (uint32_t k = 0; k < h; k++) buf[o++] = head[k];
        buf[o++] = ':';
        while (t) buf[o++] = tmp[--t];
        buf[o++] = ' ';
    }
    if (o && buf[o - 1u] == ' ') o--;
    if (!o) { buf[o++] = '-'; }
    buf[o] = '\0';
}

/* ---- the first overflow of the arm-scoped witness this arm ---- */
typedef struct { uint64_t frame, va, page, over_at; uint32_t valid, resolved, used, cap, verdict; int32_t pid; } n48_wo_first;

/* ---- the lines (each <= 491 bytes at maximal fields: tests/gfx_wo99_test.cpp) ---- */
/* The verb's status and the STOP block's counters: mode, how, then n48_wo_st. */
#define N48_WO_FMT "wo99: switch 99 is %s%s; gates %llu, with witness_over %llu, relaxable %llu; ON skipped %llu (then clean %llu); " \
    "SHADOW would pass %llu; frame lines %llu"
#define N48_WO_ARGS(m, how, s) n48_wo_mode_name(m), (how), (unsigned long long)(s)->gates, (unsigned long long)(s)->over_gates, \
    (unsigned long long)(s)->relaxable, (unsigned long long)(s)->skipped, (unsigned long long)(s)->skipped_ok, \
    (unsigned long long)(s)->would_pass, (unsigned long long)(s)->lines
/* The witness at the CONTINUOUS STOP (the arm's state, before the next scope re-bases it): rows used / cap, over, unresolved,
 * truncated targets, scope rebases, then the first overflow (frame, VA, page, resolved, rows at the time, verdict, pid). */
#define N48_WO_STOP_FMT "wo99: CONTINUOUS STOP - the arm-scoped witness: rows %u of %u, over %llu, unresolved %llu, truncated %llu, " \
    "scope seq %u (rebases %llu); first overflow: %s frame %llu VA %#llx page %#llx resolved %u rows %u/%u verdict %u pid %d"
#define N48_WO_STOP_ARGS(wt, trunc, f) (wt)->used, n48_dep_rows_cap(wt), (unsigned long long)(wt)->over, \
    (unsigned long long)(wt)->unresolved, (unsigned long long)(trunc), (wt)->arm_seq, (unsigned long long)(wt)->rebases, \
    (f)->valid ? "at" : "NONE this arm -", (unsigned long long)(f)->frame, (unsigned long long)(f)->va, \
    (unsigned long long)(f)->page, (f)->resolved, (f)->used, (f)->cap, (f)->verdict, (f)->pid
/* The first overflow, when it happens (once per arm scope). */
#define N48_WO_FIRST_FMT "wo99: the arm-scoped witness OVERFLOWED FIRST at frame %llu: VA %#llx page %#llx resolved %u (rows %u/%u " \
    "full; over now %llu); verdict %u pid %d - with 28 ON every later gate refuses witness-overflow unless 99 relaxes it"
#define N48_WO_FIRST_ARGS(f, over) (unsigned long long)(f)->frame, (unsigned long long)(f)->va, (unsigned long long)(f)->page, \
    (f)->resolved, (f)->used, (f)->cap, (unsigned long long)(over), (f)->verdict, (f)->pid
/* The per-second line while armed: line, window start, window length, gates, the witness (rows used / cap, over, truncated),
 * and the histogram of this second's dependency answers (reason index:count; gfx_dep.h n48_dep_reason_name order). */
/* build 0.0.550: + this second's COMMITTED composites (non-P frames) and P frames, before the
 * histogram (so the histogram stays the line's tail). */
#define N48_WO_SEC_FMT "wo99 sec %u: +%llu ms (%llu ms): gates %llu; witness rows %u/%u over %llu trunc %llu; " \
    "commits composite %u P %u; dep reasons %s"
#define N48_WO_SEC_ARGS(L, wt, trunc, hist) (L).line, (unsigned long long)(L).at_ms, (unsigned long long)(L).len_ms, \
    (unsigned long long)(L).gates, (wt)->used, n48_dep_rows_cap(wt), (unsigned long long)(wt)->over, (unsigned long long)(trunc), \
    n48_wo_u32((L).comp), n48_wo_u32((L).pcm), (hist)
/* One SHADOW frame the relaxed rule would pass / one ON frame it let past (the first N48_WO_PER_ARM_LINES per arm). */
#define N48_WO_FRAME_FMT "wo99: frame %llu %s: witness_over %llu (over %llu + trunc %llu), r5_blind %llu, enumerated %u r5_mode %u " \
    "rows-free %u; relaxed answer %s"
#define N48_WO_FRAME_ARGS(fr, ev, w, over, trunc, relaxed) (unsigned long long)(fr), \
    (ev) == N48_WO_E_SKIPPED ? "ON - the witness-overflow rung SKIPPED" : "SHADOW - REFUSED witness-overflow; the relaxed rule would PASS", \
    (unsigned long long)(w)->witness_over, (unsigned long long)(over), (unsigned long long)(trunc), \
    (unsigned long long)(w)->r5_blind, (w)->consumer_enumerated, (w)->r5_mode, (w)->cp_rows_free, n48_dep_reason_name(relaxed)

#endif /* N48_GFX_WO99_H */
