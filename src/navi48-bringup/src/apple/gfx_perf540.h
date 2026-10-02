// gfx_perf540.h — build 0.0.540 (its instrument list T1-T11;'s mid-arm plateau): SWITCH 96, "perf540".
// A LOG-ONLY INSTRUMENT. It answers WHERE the GFX hook's time goes while armed and why WindowServer presents only ~6/s mid-arm
// (then 13-16/s for the last ~9 s). It decides nothing: no register, no page table, nothing of Apple's is written, no answer of
// any rule changes, and nothing is printed while it is OFF.
//
// `accel gfxneuter 96 | M << 8`: M 1 ON (= 352), M 2 OFF (= 608, the default and the boot value), bare `96` reads (the status line).
// Joins the continuous mid-arm guard (gfx_commit.h n48_cm_cont_switch_refused), so it is constant for the whole of an arm.
//
// COST CONTRACT. OFF, every timed site pays ONE load of the switch (n48_pf_begin's `on`, read by the caller once per site) and
// reads no clock. ON, a timed site adds at most ONE clock pair (clock_get_uptime, the clock the glass531 wrappers use), and every
// accumulator is a file-scope static updated with relaxed atomics (report-only numbers; nothing branches on them).
//
// This header is the PURE half, host-tested by tests/gfx_perf540_test.cpp: the timer / counter layout, the two histograms'
// buckets, the per-second window and its cap, the START snapshot and the STOP deltas, the shadow direct-mapped tag arrays (T8's
// cross-frame memo, T11's walk and host-page caches: would-hit counters only), and every line's format (each <= 491 bytes at
// maximal fields). The kext glue lives in AppleHardwareHook.cpp (the sites), Navi48Bringup.cpp (navi48_vram_read_mm's per-taker
// call time, T5; n48_logf's split, T9) and amd/n48log.cpp (T9's three clock reads).
#ifndef N48_GFX_PERF540_H
#define N48_GFX_PERF540_H

#include <stdint.h>
#include "gfx_mmhold.h"

enum { N48_PF_OFF = 0u, N48_PF_ON = 1u };

/* A timer's clock: the kext hands clock_get_uptime's reader; the host test counts its calls (OFF must read no clock). */
typedef uint64_t (*n48_pf_clock_fn)(void);
/* A timed site's start: 0 (= not timed) when OFF, without calling the clock; the clock's reading (never 0 after boot) when ON. */
static inline uint64_t n48_pf_begin(uint32_t on, n48_pf_clock_fn clk)
{
    return on ? clk() : 0ull;
}

/* ---- the timers: each accumulates calls and nanoseconds ---- */
enum {
    /* T1 / T2: the hook (hook_gfxCommitIB_timed's whole GFX call; ws_classify; gcap_submission; the judge; each orig call) */
    N48_PF_T_HOOK = 0u, N48_PF_T_WSCLASS, N48_PF_T_GCAP, N48_PF_T_JUDGE,
    N48_PF_T_ORIG_PASS, N48_PF_T_ORIG_NOTWS, N48_PF_T_ORIG_REFUSED, N48_PF_T_ORIG_XLAT, N48_PF_T_ORIG_NEUTER,
    /* T3: gfxsrc_decide_frame's phases (the IB gather, the identify loop, the policy call, the ring-poll block, commit_try, the
     * ledger feed) */
    N48_PF_T_GATHER, N48_PF_T_IDENT, N48_PF_T_POLICY, N48_PF_T_RINGPOLL, N48_PF_T_COMMIT, N48_PF_T_LEDGER,
    /* T4: gfxsrc_policy's translate time (its existing phXlat, no new clock) by the frame's final outcome at the gate */
    N48_PF_T_XLAT_OK, N48_PF_T_XLAT_GREF, N48_PF_T_XLAT_VREF,
    /* T4: one clock pair per callback */
    N48_PF_T_CB_TILED, N48_PF_T_CB_DCC, N48_PF_T_CB_TEX, N48_PF_T_CB_CSN, N48_PF_T_CB_DESC, N48_PF_T_CB_PGM,
    N48_PF_T_UNIT_RETRY, N48_PF_T_CG_REDO,   /* navi48_cg_seg_check's pair is n48_pf_ext.cg_* (it lives in Navi48Bringup.cpp) */
    /* T7: a host page's IOMemoryDescriptor create -> release (gfxc_read_core's isSys branch; gfxc_write_sys) */
    N48_PF_T_HMAP_R, N48_PF_T_HMAP_W,
    N48_PF_T_N
};
typedef struct { uint64_t n, ns; } n48_pf_acc;

/* ---- plain counters ---- */
enum {
    N48_PF_C_OVERLAP = 0u,        /* T1: a GFX call that entered before the previous one returned (no idle gap) */
    N48_PF_C_IDLE_NS,             /* T1: the idle gaps' sum (ns) */
    N48_PF_C_CALLER_WS,           /* T1: the calling thread's pid is the bound WindowServer's */
    N48_PF_C_CALLER_KERN,         /* T1: pid 0 (a kernel thread) */
    N48_PF_C_CALLER_OTHER,
    N48_PF_C_PRESENTS,            /* T10: hw_p73_present calls, and the copies it let through */
    N48_PF_C_COPIED,
    N48_PF_C_RANGE_ASKED,         /* T8: ring-mark invalidations a RANGE-AWARE memo would have asked, and would have hit */
    N48_PF_C_RANGE_HIT,
    N48_PF_C_XF_ASKED,            /* T8: cold misses a CROSS-FRAME memo would have asked; its tag present; and still valid */
    N48_PF_C_XF_TAG,
    N48_PF_C_XF_VALID,
    N48_PF_C_WALK_ASKED,          /* T11: gfxc_page calls probed; a per-pass cache would hit; any-time cache would hit */
    N48_PF_C_WALK_HIT_PASS,
    N48_PF_C_WALK_HIT_ANY,
    N48_PF_C_HMAP_ASKED,          /* T11: host-page maps probed; a per-call cache would hit; any-time cache would hit */
    N48_PF_C_HMAP_HIT_CALL,
    N48_PF_C_HMAP_HIT_ANY,
    N48_PF_C_ARMS_STARTED,        /* START snapshots taken / STOP deltas printed / STOPs with no START snapshot */
    N48_PF_C_ARMS_STOPPED,
    N48_PF_C_ARMS_NOSTART,
    N48_PF_C_WC_LOOK,             /* build 0.0.541 (switch 98, gfx_wc98.h): walks asked inside an ask's walk-cache scope; hits */
    N48_PF_C_WC_HIT,
    N48_PF_C_WCU_ALL,             /* build 0.0.543 item B (switch 101, gfx_wc98.h n48_wc_attr): unstable walks attributed; by */
    N48_PF_C_WCU_MAP,             /* class (a walk may name several); nothing attributable */
    N48_PF_C_WCU_UNMAP,
    N48_PF_C_WCU_CG,
    N48_PF_C_WCU_OTHER,
    N48_PF_C_WCU_NONE,
    N48_PF_C_N
};

/* ---- T6: gfxc_page by the calling thread's MM sub-tag ---- */
enum { N48_PF_W_JUDGE = 0u, N48_PF_W_PGMID, N48_PF_W_DESC, N48_PF_W_ASK, N48_PF_W_DECIDE, N48_PF_W_COMMIT, N48_PF_W_OTHER,
       N48_PF_W_N };
typedef struct { uint64_t calls, levels, fails, ns; } n48_pf_walk;
static inline uint32_t n48_pf_walk_class(uint32_t tag)
{
    if (tag == N48_MMT_JUDGE || tag == N48_MMT_J_WALK) return N48_PF_W_JUDGE;
    if (tag == N48_MMT_J_PGMID) return N48_PF_W_PGMID;
    if (tag == N48_MMT_J_DESC) return N48_PF_W_DESC;
    if (tag == N48_MMT_J_ASK) return N48_PF_W_ASK;
    if (n48_mmt_is_decide(tag)) return N48_PF_W_DECIDE;
    if (tag == N48_MMT_COMMIT) return N48_PF_W_COMMIT;
    return N48_PF_W_OTHER;
}

/* ---- T1 / T10: the two histograms ---- */
#define N48_PF_GAP_B 8u
/* T1: the hook's idle gap (entry minus the previous exit), buckets <0.5, <2, <5, <10, <20, <50, <100, >=100 ms. */
static inline uint32_t n48_pf_idle_bucket(uint64_t us)
{
    static const uint64_t lim[N48_PF_GAP_B - 1u] = { 500ull, 2000ull, 5000ull, 10000ull, 20000ull, 50000ull, 100000ull };
    for (uint32_t b = 0; b < N48_PF_GAP_B - 1u; b++) if (us < lim[b]) return b;
    return N48_PF_GAP_B - 1u;
}
/* T10: the present gap (a present's entry minus the previous present's), buckets <20, <33, <50, <67, <100, <200, <500, >=500 ms. */
static inline uint32_t n48_pf_present_bucket(uint64_t us)
{
    static const uint64_t lim[N48_PF_GAP_B - 1u] = { 20000ull, 33000ull, 50000ull, 67000ull, 100000ull, 200000ull, 500000ull };
    for (uint32_t b = 0; b < N48_PF_GAP_B - 1u; b++) if (us < lim[b]) return b;
    return N48_PF_GAP_B - 1u;
}

/* ---- the other files' accumulators (Navi48Bringup.cpp): T5 per MM tag, T9 by thread class ---- */
#define N48_PF_MM_TAGS 16u
typedef struct {
    uint64_t mm_n[N48_PF_MM_TAGS], mm_ns[N48_PF_MM_TAGS], mm_dw[N48_PF_MM_TAGS];   /* navi48_vram_read_mm: calls, ns, dwords */
    uint64_t lg_n[2], lg_vsn[2], lg_io[2], lg_ring[2];   /* n48_logf lines and ns in vsnprintf / IOLog / the ring: [0] a hook
                                                            thread, [1] any other */
    uint64_t cg_n, cg_ns;                                 /* T4: navi48_cg_seg_check (the per-segment copy-guard check) */
} n48_pf_ext;

/* ---- the totals a START snapshot copies and a STOP subtracts. EVERY FIELD IS A uint64_t (the delta is element-wise). ---- */
typedef struct {
    n48_pf_acc t[N48_PF_T_N];
    uint64_t idle_b[N48_PF_GAP_B], pgap_b[N48_PF_GAP_B];
    uint64_t c[N48_PF_C_N];
    n48_pf_walk w[N48_PF_W_N];
    uint64_t pm[4];                   /* T8: the pgmid memo's own always-on counters: hits, cold misses, ring-mark, poison */
    n48_pf_ext x;
    uint64_t hook_max_ns;             /* the longest GFX call since the START (not a delta: the STOP prints it as is) */
} n48_pf_tot;
#define N48_PF_TOT_WORDS (sizeof(n48_pf_tot) / sizeof(uint64_t))

/* out = cur - snap, element-wise (a counter that went backwards reads 0); hook_max_ns is cur's. */
static inline void n48_pf_sub(n48_pf_tot *out, const n48_pf_tot *cur, const n48_pf_tot *snap)
{
    const uint64_t *a = (const uint64_t *)cur, *b = (const uint64_t *)snap;
    uint64_t *o = (uint64_t *)out;
    for (uint32_t i = 0; i < (uint32_t)N48_PF_TOT_WORDS; i++) o[i] = a[i] >= b[i] ? a[i] - b[i] : 0ull;
    out->hook_max_ns = cur->hook_max_ns;
}

/* ---- the arm: the START snapshot and the STOP deltas ---- */
typedef struct {
    uint32_t valid;                   /* 1 between a START snapshot and its STOP */
    uint64_t start_us;
    n48_pf_tot snap;
} n48_pf_arm;
static inline void n48_pf_arm_start(n48_pf_arm *a, const n48_pf_tot *cur, uint64_t now_us)
{
    a->snap = *cur;
    a->start_us = now_us;
    a->valid = 1u;
}
/* 1 = *out holds this arm's deltas (and the arm is closed); 0 = no START snapshot was taken this arm: nothing to print (the
 * totals since boot are NOT deltas, so they are never printed as such). */
static inline uint32_t n48_pf_arm_stop(n48_pf_arm *a, const n48_pf_tot *cur, n48_pf_tot *out)
{
    if (!a->valid) return 0u;
    n48_pf_sub(out, cur, &a->snap);
    a->valid = 0u;
    return 1u;
}

/* ---- the per-second line while armed (like present94 sec) ---- */
enum { N48_PF_S_CALLS = 0u, N48_PF_S_WALL, N48_PF_S_JUDGE, N48_PF_S_XLAT, N48_PF_S_MM, N48_PF_S_WALKS, N48_PF_S_HMAPS,
       N48_PF_S_LOG, N48_PF_S_PRESENTS, N48_PF_S_COPIED, N48_PF_S_IDLE,
       N48_PF_S_WCHIT, N48_PF_S_WCLOOK,   /* build 0.0.541: switch 98's hits of its lookups (0 of 0 while 98 is OFF) */
       N48_PF_S_WCU_ALL, N48_PF_S_WCU_MAP, N48_PF_S_WCU_UNMAP, N48_PF_S_WCU_CG, N48_PF_S_WCU_OTHER, N48_PF_S_WCU_NONE,   /* 0.0.543 B */
       N48_PF_S_N };
typedef struct { uint64_t v[N48_PF_S_N]; } n48_pf_secv;
#define N48_PF_SEC_US    1000000ull   /* one line per second while armed */
#define N48_PF_SEC_LINES 240u         /* at most this many per arm (the rest counted suppressed) */
/* The hook's own MM takers (every tag but OTHER, the fast copy's verify and the residency copy). */
static inline uint32_t n48_pf_mm_hook_tag(uint32_t tag)
{
    return tag < N48_PF_MM_TAGS && tag != N48_MMT_OTHER && tag != N48_MMT_FCVERIFY && tag != N48_MMT_RESIDENCY ? 1u : 0u;
}
static inline void n48_pf_secv_of(const n48_pf_tot *t, n48_pf_secv *o)
{
    o->v[N48_PF_S_CALLS] = t->t[N48_PF_T_HOOK].n;
    o->v[N48_PF_S_WALL] = t->t[N48_PF_T_HOOK].ns;
    o->v[N48_PF_S_JUDGE] = t->t[N48_PF_T_JUDGE].ns;
    o->v[N48_PF_S_XLAT] = t->t[N48_PF_T_XLAT_OK].ns + t->t[N48_PF_T_XLAT_GREF].ns + t->t[N48_PF_T_XLAT_VREF].ns;
    uint64_t mm = 0ull, walks = 0ull;
    for (uint32_t k = 0; k < N48_PF_MM_TAGS; k++) if (n48_pf_mm_hook_tag(k)) mm += t->x.mm_ns[k];
    for (uint32_t k = 0; k < N48_PF_W_N; k++) walks += t->w[k].calls;
    o->v[N48_PF_S_MM] = mm;
    o->v[N48_PF_S_WALKS] = walks;
    o->v[N48_PF_S_HMAPS] = t->t[N48_PF_T_HMAP_R].n + t->t[N48_PF_T_HMAP_W].n;
    o->v[N48_PF_S_LOG] = t->x.lg_n[0] + t->x.lg_n[1];
    o->v[N48_PF_S_PRESENTS] = t->c[N48_PF_C_PRESENTS];
    o->v[N48_PF_S_COPIED] = t->c[N48_PF_C_COPIED];
    o->v[N48_PF_S_IDLE] = t->c[N48_PF_C_IDLE_NS];
    o->v[N48_PF_S_WCHIT] = t->c[N48_PF_C_WC_HIT];
    o->v[N48_PF_S_WCLOOK] = t->c[N48_PF_C_WC_LOOK];
    for (uint32_t k = 0; k < 6u; k++) o->v[N48_PF_S_WCU_ALL + k] = t->c[N48_PF_C_WCU_ALL + k];   /* build 0.0.543 item B */
}
typedef struct {
    uint32_t open;                    /* 1 while an armed window is open */
    uint32_t lines;                   /* printed this arm */
    uint64_t t0, a0;                  /* the window's start and the arm's first window (us) */
    uint64_t suppressed, windows;
    n48_pf_secv last;                 /* the totals at the window's start */
} n48_pf_win;
typedef struct { uint64_t v[N48_PF_S_N]; uint64_t at_ms, len_ms; uint32_t line; } n48_pf_secline;
/* Asked by a tick site (a hook call's exit, a present) with the arm state and the time: 1 = the caller must build the totals and
 * call n48_pf_win_close (a window opens, or one of >= 1 s is due). Not armed: the window only closes (nothing to build). */
static inline uint32_t n48_pf_win_want(n48_pf_win *w, uint32_t armed, uint64_t now_us)
{
    if (!armed) { w->open = 0u; return 0u; }
    if (!w->open) return 1u;
    return (now_us >= w->t0 && now_us - w->t0 >= N48_PF_SEC_US) ? 1u : 0u;
}
/* The first call of an arm opens its first window (the totals then are its base) and a fresh line budget; a later one closes the
 * window into *out: 1 = print it; past N48_PF_SEC_LINES per arm the line is counted suppressed and 0 returned. */
static inline uint32_t n48_pf_win_close(n48_pf_win *w, const n48_pf_secv *cur, uint64_t now_us, n48_pf_secline *out)
{
    if (!w->open) {
        w->open = 1u; w->t0 = now_us; w->a0 = now_us; w->lines = 0u; w->last = *cur;
        return 0u;
    }
    uint32_t due = 0u;
    w->windows++;
    if (w->lines < N48_PF_SEC_LINES) {
        w->lines++;
        for (uint32_t k = 0; k < N48_PF_S_N; k++) out->v[k] = cur->v[k] >= w->last.v[k] ? cur->v[k] - w->last.v[k] : 0ull;
        out->at_ms = (w->t0 - w->a0) / 1000ull; out->len_ms = (now_us - w->t0) / 1000ull; out->line = w->lines;
        due = 1u;
    } else {
        w->suppressed++;
    }
    w->last = *cur;
    w->t0 = now_us;
    return due;
}

/* ---- T8 / T11: the shadow direct-mapped tag arrays (would-hit counters only: nothing is ever answered from them) ---- */
#define N48_PF_DM 256u
typedef struct { uint64_t tag[N48_PF_DM]; uint64_t aux[N48_PF_DM]; uint32_t gen[N48_PF_DM]; } n48_pf_dm;
static inline uint32_t n48_pf_dm_ix(uint64_t key) { return (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 56) & (N48_PF_DM - 1u); }
/* Probe `key` (never 0) at generation `gen`, then fill it. *hit_any: the tag was there; return: and from the same generation. */
static inline uint32_t n48_pf_dm_probe(n48_pf_dm *d, uint64_t key, uint32_t gen, uint32_t *hit_any)
{
    const uint32_t i = n48_pf_dm_ix(key);
    const uint32_t any = d->tag[i] == key ? 1u : 0u;
    const uint32_t same = any && d->gen[i] == gen ? 1u : 0u;
    d->tag[i] = key; d->gen[i] = gen;
    if (hit_any) *hit_any = any;
    return same;
}
/* T8's cross-frame memo: the fill stores (stage, va) with its fill mark; a cold miss asks. *valid: the stored mark is still the
 * ring's (no copy-guard event since the fill). */
static inline uint64_t n48_pf_pm_key(uint32_t stage, uint64_t va) { return ((va << 4) | (uint64_t)(stage & 0xfu)) | 1ull << 63; }
static inline void n48_pf_xf_fill(n48_pf_dm *d, uint32_t stage, uint64_t va, uint64_t mark)
{
    const uint64_t k = n48_pf_pm_key(stage, va);
    const uint32_t i = n48_pf_dm_ix(k);
    d->tag[i] = k; d->aux[i] = mark;
}
static inline uint32_t n48_pf_xf_ask(const n48_pf_dm *d, uint32_t stage, uint64_t va, uint64_t mark_now, uint32_t *valid)
{
    const uint64_t k = n48_pf_pm_key(stage, va);
    const uint32_t i = n48_pf_dm_ix(k);
    const uint32_t tag = d->tag[i] == k ? 1u : 0u;
    *valid = tag && d->aux[i] == mark_now ? 1u : 0u;
    return tag;
}
/* T11's walk key: the page-table root and the 4 KiB page (never 0). */
static inline uint64_t n48_pf_walk_key(uint64_t root, uint64_t va) { return ((va >> 12) ^ (root * 0x100000001b3ull)) | 1ull << 63; }

/* ============================================================================================================================
 * THE LINES. Every body <= 491 bytes at maximal fields (tests/gfx_perf540_test.cpp). Times in us (the STOP lines) or ms (the
 * per-second line). `perf540` names every line in a driver log.
 * ============================================================================================================================ */
#define N48_PF_US(ns) (unsigned long long)((ns) / 1000ull)
#define N48_PF_A2(d, k) (unsigned long long)(d)->t[k].n, N48_PF_US((d)->t[k].ns)

/* The verb's status line. args: state, how, arms started, stopped, stopped with no START, sec lines this arm, suppressed. */
#define N48_PF_STATUS_FMT "perf540: switch 96 is %s%s; arms: START snapshots %llu, STOP deltas printed %llu, STOP with no START " \
    "%llu; per-second lines this arm %u (cap 240), suppressed %llu"
#define N48_PF_ON_TXT  "ON (352: the hook, judge, walk, MM, host-map, log and present timers run; lines at START/STOP and 1/s armed)"
#define N48_PF_OFF_TXT "OFF (608, default: one load per timed site, no clock, nothing printed)"

/* STOP: the header, then L1-L14. args: us since the START snapshot, sec lines this arm, suppressed. */
#define N48_PF_STOP_FMT "perf540: CONTINUOUS STOP - the lines below are this arm's deltas since the CONTINUOUS START snapshot (%llu " \
    "us ago); per-second lines %u, suppressed %llu"
#define N48_PF_NOSTART_FMT "perf540: CONTINUOUS STOP with no START snapshot this arm (the arm never reached CONTINUOUS START) - " \
    "no deltas printed: totals since boot are not an arm's deltas"

#define N48_PF_L1_FMT "perf540 arm: hook calls %llu wall %llu us (max %llu us); judge %llu/%llu us; ws_classify %llu/%llu us; " \
    "gcap %llu/%llu us"
#define N48_PF_L1_ARGS(d) N48_PF_A2(d, N48_PF_T_HOOK), N48_PF_US((d)->hook_max_ns), N48_PF_A2(d, N48_PF_T_JUDGE), \
    N48_PF_A2(d, N48_PF_T_WSCLASS), N48_PF_A2(d, N48_PF_T_GCAP)

#define N48_PF_L2_FMT "perf540 arm: orig (calls/us) pass %llu/%llu not-ws %llu/%llu refused %llu/%llu translated %llu/%llu " \
    "neutered %llu/%llu"
#define N48_PF_L2_ARGS(d) N48_PF_A2(d, N48_PF_T_ORIG_PASS), N48_PF_A2(d, N48_PF_T_ORIG_NOTWS), N48_PF_A2(d, N48_PF_T_ORIG_REFUSED), \
    N48_PF_A2(d, N48_PF_T_ORIG_XLAT), N48_PF_A2(d, N48_PF_T_ORIG_NEUTER)

#define N48_PF_L3_FMT "perf540 arm: idle gap (ms) <0.5 %llu <2 %llu <5 %llu <10 %llu <20 %llu <50 %llu <100 %llu >=100 %llu; " \
    "overlap %llu; idle %llu us; callers ws %llu kernel %llu other %llu"
#define N48_PF_L3_ARGS(d) (unsigned long long)(d)->idle_b[0], (unsigned long long)(d)->idle_b[1], \
    (unsigned long long)(d)->idle_b[2], (unsigned long long)(d)->idle_b[3], (unsigned long long)(d)->idle_b[4], \
    (unsigned long long)(d)->idle_b[5], (unsigned long long)(d)->idle_b[6], (unsigned long long)(d)->idle_b[7], \
    (unsigned long long)(d)->c[N48_PF_C_OVERLAP], N48_PF_US((d)->c[N48_PF_C_IDLE_NS]), \
    (unsigned long long)(d)->c[N48_PF_C_CALLER_WS], (unsigned long long)(d)->c[N48_PF_C_CALLER_KERN], \
    (unsigned long long)(d)->c[N48_PF_C_CALLER_OTHER]

#define N48_PF_L4_FMT "perf540 arm: judge phases (n/us) gather %llu/%llu ident %llu/%llu policy %llu/%llu ring-poll %llu/%llu " \
    "commit_try %llu/%llu ledger %llu/%llu"
#define N48_PF_L4_ARGS(d) N48_PF_A2(d, N48_PF_T_GATHER), N48_PF_A2(d, N48_PF_T_IDENT), N48_PF_A2(d, N48_PF_T_POLICY), \
    N48_PF_A2(d, N48_PF_T_RINGPOLL), N48_PF_A2(d, N48_PF_T_COMMIT), N48_PF_A2(d, N48_PF_T_LEDGER)

#define N48_PF_L5_FMT "perf540 arm: translate by the frame's outcome (frames/us) gate OK %llu/%llu gate refused %llu/%llu " \
    "verdict refused %llu/%llu; asks (n/us) tiled %llu/%llu dcc %llu/%llu"
#define N48_PF_L5_ARGS(d) N48_PF_A2(d, N48_PF_T_XLAT_OK), N48_PF_A2(d, N48_PF_T_XLAT_GREF), N48_PF_A2(d, N48_PF_T_XLAT_VREF), \
    N48_PF_A2(d, N48_PF_T_CB_TILED), N48_PF_A2(d, N48_PF_T_CB_DCC)

#define N48_PF_L6_FMT "perf540 arm: callbacks (n/us) tex %llu/%llu cs-is-n %llu/%llu desc-read %llu/%llu pgm %llu/%llu " \
    "unit-retry %llu/%llu cg-redo %llu/%llu cg-seg-check %llu/%llu"
#define N48_PF_L6_ARGS(d) N48_PF_A2(d, N48_PF_T_CB_TEX), N48_PF_A2(d, N48_PF_T_CB_CSN), N48_PF_A2(d, N48_PF_T_CB_DESC), \
    N48_PF_A2(d, N48_PF_T_CB_PGM), N48_PF_A2(d, N48_PF_T_UNIT_RETRY), N48_PF_A2(d, N48_PF_T_CG_REDO), \
    (unsigned long long)(d)->x.cg_n, N48_PF_US((d)->x.cg_ns)

/* T5: navi48_vram_read_mm's WHOLE call (the mmprio spin, the lock wait, the reads) by the caller's MM tag. */
#define N48_PF_MM3(x, k) (unsigned long long)(x)->mm_n[k], N48_PF_US((x)->mm_ns[k]), (unsigned long long)(x)->mm_dw[k]
static inline void n48_pf_mm_fold(const n48_pf_ext *x, uint32_t which, uint64_t o[3])
{
    /* which 0: DECIDE and its sub-tags; 1: every tag the lines do not name (OTHER, CAPTURE, FCVERIFY, RESIDENCY) */
    o[0] = o[1] = o[2] = 0ull;
    for (uint32_t k = 0; k < N48_PF_MM_TAGS; k++) {
        const uint32_t in = which == 0u ? (n48_mmt_is_decide(k) ? 1u : 0u)
                          : (k == N48_MMT_OTHER || k == N48_MMT_CAPTURE || k == N48_MMT_FCVERIFY || k == N48_MMT_RESIDENCY) ? 1u : 0u;
        if (!in) continue;
        o[0] += x->mm_n[k]; o[1] += x->mm_ns[k]; o[2] += x->mm_dw[k];
    }
}
#define N48_PF_L7_FMT "perf540 arm: mm reads (calls/us/dw) judge %llu/%llu/%llu walk %llu/%llu/%llu pgmid %llu/%llu/%llu " \
    "desc %llu/%llu/%llu"
#define N48_PF_L7_ARGS(d) N48_PF_MM3(&(d)->x, N48_MMT_JUDGE), N48_PF_MM3(&(d)->x, N48_MMT_J_WALK), \
    N48_PF_MM3(&(d)->x, N48_MMT_J_PGMID), N48_PF_MM3(&(d)->x, N48_MMT_J_DESC)
#define N48_PF_L8_FMT "perf540 arm: mm reads ask %llu/%llu/%llu decide %llu/%llu/%llu commit %llu/%llu/%llu other %llu/%llu/%llu"
#define N48_PF_L8_ARGS(d, dec, oth) N48_PF_MM3(&(d)->x, N48_MMT_J_ASK), (unsigned long long)(dec)[0], N48_PF_US((dec)[1]), \
    (unsigned long long)(dec)[2], N48_PF_MM3(&(d)->x, N48_MMT_COMMIT), (unsigned long long)(oth)[0], N48_PF_US((oth)[1]), \
    (unsigned long long)(oth)[2]

/* T6: gfxc_page by the caller's sub-tag. */
#define N48_PF_W4(d, k) (unsigned long long)(d)->w[k].calls, (unsigned long long)(d)->w[k].levels, \
    (unsigned long long)(d)->w[k].fails, N48_PF_US((d)->w[k].ns)
#define N48_PF_L9_FMT "perf540 arm: walks (calls/levels/fails/us) judge %llu/%llu/%llu/%llu pgmid %llu/%llu/%llu/%llu " \
    "desc %llu/%llu/%llu/%llu"
#define N48_PF_L9_ARGS(d) N48_PF_W4(d, N48_PF_W_JUDGE), N48_PF_W4(d, N48_PF_W_PGMID), N48_PF_W4(d, N48_PF_W_DESC)
#define N48_PF_L10_FMT "perf540 arm: walks ask %llu/%llu/%llu/%llu decide %llu/%llu/%llu/%llu commit %llu/%llu/%llu/%llu " \
    "other %llu/%llu/%llu/%llu"
#define N48_PF_L10_ARGS(d) N48_PF_W4(d, N48_PF_W_ASK), N48_PF_W4(d, N48_PF_W_DECIDE), N48_PF_W4(d, N48_PF_W_COMMIT), \
    N48_PF_W4(d, N48_PF_W_OTHER)

/* T7 + T11 */
#define N48_PF_L11_FMT "perf540 arm: host maps (n/us) read %llu/%llu write %llu/%llu; shadow walk cache would-hit per pass %llu " \
    "any %llu of %llu; shadow host-page cache would-hit per call %llu any %llu of %llu"
#define N48_PF_L11_ARGS(d) N48_PF_A2(d, N48_PF_T_HMAP_R), N48_PF_A2(d, N48_PF_T_HMAP_W), \
    (unsigned long long)(d)->c[N48_PF_C_WALK_HIT_PASS], (unsigned long long)(d)->c[N48_PF_C_WALK_HIT_ANY], \
    (unsigned long long)(d)->c[N48_PF_C_WALK_ASKED], (unsigned long long)(d)->c[N48_PF_C_HMAP_HIT_CALL], \
    (unsigned long long)(d)->c[N48_PF_C_HMAP_HIT_ANY], (unsigned long long)(d)->c[N48_PF_C_HMAP_ASKED]

/* T8 */
#define N48_PF_L12_FMT "perf540 arm: pgmid memo hits %llu, misses cold %llu ring-mark %llu poison %llu; shadow range-aware " \
    "would-hit %llu of %llu; shadow cross-frame tag %llu valid %llu of %llu"
#define N48_PF_L12_ARGS(d) (unsigned long long)(d)->pm[0], (unsigned long long)(d)->pm[1], (unsigned long long)(d)->pm[2], \
    (unsigned long long)(d)->pm[3], (unsigned long long)(d)->c[N48_PF_C_RANGE_HIT], (unsigned long long)(d)->c[N48_PF_C_RANGE_ASKED], \
    (unsigned long long)(d)->c[N48_PF_C_XF_TAG], (unsigned long long)(d)->c[N48_PF_C_XF_VALID], \
    (unsigned long long)(d)->c[N48_PF_C_XF_ASKED]

/* T9 */
#define N48_PF_L13_FMT "perf540 arm: n48_logf hook threads: lines %llu vsnprintf %llu us IOLog %llu us ring %llu us; other threads: " \
    "lines %llu vsnprintf %llu us IOLog %llu us ring %llu us"
#define N48_PF_L13_ARGS(d) (unsigned long long)(d)->x.lg_n[0], N48_PF_US((d)->x.lg_vsn[0]), N48_PF_US((d)->x.lg_io[0]), \
    N48_PF_US((d)->x.lg_ring[0]), (unsigned long long)(d)->x.lg_n[1], N48_PF_US((d)->x.lg_vsn[1]), N48_PF_US((d)->x.lg_io[1]), \
    N48_PF_US((d)->x.lg_ring[1])

/* T10 */
#define N48_PF_L14_FMT "perf540 arm: presents %llu copied %llu; present gap (ms) <20 %llu <33 %llu <50 %llu <67 %llu <100 %llu " \
    "<200 %llu <500 %llu >=500 %llu"
#define N48_PF_L14_ARGS(d) (unsigned long long)(d)->c[N48_PF_C_PRESENTS], (unsigned long long)(d)->c[N48_PF_C_COPIED], \
    (unsigned long long)(d)->pgap_b[0], (unsigned long long)(d)->pgap_b[1], (unsigned long long)(d)->pgap_b[2], \
    (unsigned long long)(d)->pgap_b[3], (unsigned long long)(d)->pgap_b[4], (unsigned long long)(d)->pgap_b[5], \
    (unsigned long long)(d)->pgap_b[6], (unsigned long long)(d)->pgap_b[7]

/* The per-second line (armed only). args: line, window start ms after the arm's first window, window ms, then the window's
 * calls, wall / judge / translate / MM ms, walks, host maps, log lines, presents, copies, idle-gap ms. */
#define N48_PF_SEC_FMT "perf540 sec %u: +%llu ms (%llu ms): calls %llu wall %llu judge %llu xlat %llu mm %llu ms; walks %llu " \
    "hmaps %llu log %llu; presents %llu copied %llu; idle %llu ms; wc98 hit %llu of %llu"
#define N48_PF_MS(ns) (unsigned long long)((ns) / 1000000ull)
#define N48_PF_SEC_ARGS(L) (L).line, (unsigned long long)(L).at_ms, (unsigned long long)(L).len_ms, \
    (unsigned long long)(L).v[N48_PF_S_CALLS], N48_PF_MS((L).v[N48_PF_S_WALL]), N48_PF_MS((L).v[N48_PF_S_JUDGE]), \
    N48_PF_MS((L).v[N48_PF_S_XLAT]), N48_PF_MS((L).v[N48_PF_S_MM]), (unsigned long long)(L).v[N48_PF_S_WALKS], \
    (unsigned long long)(L).v[N48_PF_S_HMAPS], (unsigned long long)(L).v[N48_PF_S_LOG], \
    (unsigned long long)(L).v[N48_PF_S_PRESENTS], (unsigned long long)(L).v[N48_PF_S_COPIED], N48_PF_MS((L).v[N48_PF_S_IDLE]), \
    (unsigned long long)(L).v[N48_PF_S_WCHIT], (unsigned long long)(L).v[N48_PF_S_WCLOOK]

/* build 0.0.543 item B (switch 101): the per-second census line, printed right after the same window's sec line while the census
 * is ON (args: the sec line's number, then the window's unstable walks attributed, per class, and nothing attributable). */
#define N48_PF_WCU_SEC_FMT "perf540 sec %u wc101: unstable walks %llu; mapVA %llu unmapVA %llu copy guard %llu other %llu none %llu"
#define N48_PF_WCU_SEC_ARGS(L) (L).line, (unsigned long long)(L).v[N48_PF_S_WCU_ALL], (unsigned long long)(L).v[N48_PF_S_WCU_MAP], \
    (unsigned long long)(L).v[N48_PF_S_WCU_UNMAP], (unsigned long long)(L).v[N48_PF_S_WCU_CG], \
    (unsigned long long)(L).v[N48_PF_S_WCU_OTHER], (unsigned long long)(L).v[N48_PF_S_WCU_NONE]

/* build 0.0.540 item 5 — switch 97's report (the verb). args: state, how, segments flagged, the cache's state number and name,
 * the linear walk's and the cache's answers for one table register (0x28c70: both 1 once built). */
#define N48_TC97_FMT "tblcache97: switch 97 is %s%s; segments flagged %llu; the cache: state %u (%s); probe 0x28c70 walk %d cache %d"
#define N48_TC97_ON_TXT  "ON (353: xlat12's output verifier answers from the sorted table cache)"
#define N48_TC97_OFF_TXT "OFF (609, default: the verifier walks the whole register table per register)"

#endif /* N48_GFX_PERF540_H */
