// gfx_ks81.h — build 0.0.526. PURE (no kernel headers, no hardware): the host suite
// (tests/gfx_ks81_test.cpp) drives the SAME functions the kext calls.
//
// ITEM 1 — THE MARKER WINDOW, TIMED (log-only, UNCONDITIONAL, decision-inert). hook_unmapVA holds the keystone withdrawal
// marker (VmCtxObs::ksWithdrawing) from its fetch_add to its release (the DEFER release or the exit release). found
// the hold-time tail growing (RUN W: 7 switch-64 timeouts at 2001 us) and SUSPECTED switch 37's MM-window priority spin
// under the marker; nothing timed the window. The thread that holds the marker claims one slot of a small table (the
// "holder table", 8 slots) right after the fetch_add; the slot carries the window's timestamps, the per-part durations and
// the MM-window yields/skips THIS thread made while holding the marker. At the release the slot is read and freed and the
// window is recorded in seven histograms: T (the whole window, fetch_add .. release), D (gfxsrc_desc_unmap), E (the entry
// read: the context's root, its entry count, root[511] through the MM window), W (the withdrawal: the guard, the 8-byte
// clear, the read-back, the HDP flush, the TLB invalidate, the fault-status sample and its log line), O (Apple's unmapVA),
// R (the re-arm), Y (the time this thread spent spinning in switch 37's yield while holding the marker, per window that
// spun at all). Buckets (us, upper-inclusive): <=100, <=250, <=500, <=1000, <=2000, <=4000, <=8000, more. Nothing reads
// these to decide anything.
//
// ITEM 2 — SWITCH 81 (default OFF): FLIP MODE IS KEPT THROUGH A KEYSTONE WITHDRAWAL. hook_gfxCommitIB's withdrawal branch
// asked navi48_fm_note_withdrawal() for EVERY withdrawal, and the first one restored flip mode to A and left it OFF (RUN W).
// Under 81 ON a withdrawal that the KEYSTONE made (the token matched, the flight-ring guard passed so the keystone ran, and it
// refused) with switch 73 ON (so p73_final(seq, 0) has already marked the frame's P WITHDRAWN and its present is held) does NOT
// ask for the restore; a token mismatch, a flight-ring guard refusal, 73 OFF, and every other restore trigger are unchanged.
//
// ITEM 3 — M3: the switch-64 wait's bound is 5000 us / 1000 polls instead of 2000 us / 400 polls. The wait decides nothing: it
// only moves the instant the unchanged checks are taken (gfx_keystone.h's switch-64 comment); a marker still held at the bound is
// today's VERDICT 15.
//
// ITEM 5 — M4: a thread that HOLDS the marker does not spin in switch 37's MM-window yield (navi48_vram_read_mm/_write_mm); it goes
// straight to IOLockLock like a 37-OFF caller. Identified by the holder table's `holder` field == current_thread(). A full table
// leaves that thread unregistered: it spins as before (the old behaviour).
//
// `81 | M << 8`: M 1 = item 2 (337), M 2 = OFF (593, the default and the boot value), M 3 = item 2 + item 3 (849), M 4 = item 2 +
// item 5 (1105); bare `81` reads; any other M is refused unchanged. Joins the continuous mid-arm guard.
#ifndef N48_GFX_KS81_H
#define N48_GFX_KS81_H

#include <stdint.h>
#include "gfx_keystone.h"

#ifdef __cplusplus
extern "C" {
#endif

#define N48_KS81_SWITCH 81u
enum { N48_KS81_OFF = 0u, N48_KS81_KEEP = 1u, N48_KS81_KEEP_WAIT = 3u, N48_KS81_KEEP_NOYIELD = 4u };
#define N48_KS81_M_READ 0xFFu   /* bare 81: read only */
#define N48_KS81_M_BAD  0xFEu   /* an unknown M: refused, unchanged */

/* The verb's M byte -> the mode to store (or READ / BAD). */
static inline uint32_t n48_ks81_mode_of_m(uint32_t m)
{
    switch (m) {
    case 0u: return N48_KS81_M_READ;
    case 1u: return N48_KS81_KEEP;
    case 2u: return N48_KS81_OFF;
    case 3u: return N48_KS81_KEEP_WAIT;
    case 4u: return N48_KS81_KEEP_NOYIELD;
    default: return N48_KS81_M_BAD;
    }
}
static inline const char *n48_ks81_mode_name(uint32_t mode)
{
    return mode == N48_KS81_OFF ? "OFF" : mode == N48_KS81_KEEP ? "M1" : mode == N48_KS81_KEEP_WAIT ? "M3"
         : mode == N48_KS81_KEEP_NOYIELD ? "M4" : "M?";
}
/* Item 2 applies at M1, M3 and M4 (never OFF, never an unknown value). */
static inline uint32_t n48_ks81_keeps_flip(uint32_t mode)
{
    return (mode == N48_KS81_KEEP || mode == N48_KS81_KEEP_WAIT || mode == N48_KS81_KEEP_NOYIELD) ? 1u : 0u;
}
/* Item 5 applies at M4 only. */
static inline uint32_t n48_ks81_skip_yield(uint32_t mode) { return mode == N48_KS81_KEEP_NOYIELD ? 1u : 0u; }

/* Item 3: the switch-64 wait's bound in use. M3 only: 5000 us; every other mode: today's N48_KS_WAIT_BOUND_US (2000). The poll
 * cap follows the bound at N48_KS_WAIT_POLL_US per poll (2000 -> 400 = N48_KS_WAIT_MAX_POLLS, 5000 -> 1000). */
#define N48_KS81_LONG_BOUND_US 5000u
static inline uint32_t n48_ks81_wait_bound_us(uint32_t mode)
{
    return mode == N48_KS81_KEEP_WAIT ? N48_KS81_LONG_BOUND_US : N48_KS_WAIT_BOUND_US;
}
static inline uint32_t n48_ks81_wait_max_polls(uint32_t bound_us) { return bound_us / N48_KS_WAIT_POLL_US; }

/* Item 2: ask navi48_fm_note_withdrawal() for this withdrawal? Asked only for a withdrawal (!(tokMatch && ksOk)); `guardOk` is
 * the flight-ring guard's answer (the keystone RAN), `p73on` switch 73. 1 = note (today's), 0 = flip mode kept. */
static inline uint32_t n48_ks81_note_withdrawal(uint32_t mode, uint32_t tokMatch, uint32_t guardOk, uint32_t ksOk, uint32_t p73on)
{
    if (tokMatch && ksOk) return 0u;   /* not a withdrawal (the commit branch): never noted, as today */
    if (tokMatch && guardOk && p73on && n48_ks81_keeps_flip(mode)) return 0u;   /* a KEYSTONE withdrawal: flip mode kept */
    return 1u;
}

/* ---- the holder table (items 1 and 5) -------------------------------------------------------------------------------------- */
#define N48_MKH_SLOTS      8u
#define N48_MKH_TOK_NESTED 0x100u   /* a token for a nested enter on the same thread: the flag stays with the outer one */
#define N48_MKH_GATE_SKIP  0x100u   /* n48_mkh_gate: the holder skips the yield (M4) */
enum { N48_KW_P_DESC = 0u, N48_KW_P_ENTRY = 1u, N48_KW_P_WD = 2u, N48_KW_P_ORIG = 3u, N48_KW_P_REARM = 4u, N48_KW_PARTS = 5u };

typedef struct {
    uintptr_t busy;     /* 0 free; else the thread that claimed it (CAS 0 -> me); the slot's data is that thread's until the end */
    uintptr_t holder;   /* THE MARKER-HOLDER FLAG: the thread while it holds the marker; 0 from its unhold on */
    uint32_t depth;     /* nested enters on the same thread */
    uint32_t mask;      /* 1 << N48_KW_P_* for every part that ran */
    uint64_t t0, tmark, dur[N48_KW_PARTS];
    uint32_t yn, skips; /* MM-window yields (spins) and skipped yields this thread made while holding the marker */
    uint64_t yus;       /* ... and the spin time */
} n48_mkh_slot;
typedef struct {
    n48_mkh_slot s[N48_MKH_SLOTS];
    uint64_t overflow, nested;
} n48_mkh_table;
typedef struct {
    uint32_t valid, mask, yn, skips;
    uint64_t t0, dur[N48_KW_PARTS], yus;
} n48_mkh_snap;

static inline uint32_t n48_mkh_idx(uint32_t tok) { return (tok & 0xFFu) - 1u; }
static inline uint32_t n48_mkh_live(uint32_t tok)
{
    return (tok != 0u && !(tok & N48_MKH_TOK_NESTED) && (tok & 0xFFu) >= 1u && (tok & 0xFFu) <= N48_MKH_SLOTS) ? 1u : 0u;
}

/* RIGHT AFTER THE FETCH_ADD: claim a slot and SET the flag. 0 = the table is full (counted; this window is untimed and the thread
 * is not a holder for the MM gate - it spins as before). A thread that already holds a slot's flag nests (depth). */
static inline uint32_t n48_mkh_enter(n48_mkh_table *t, uintptr_t me, uint64_t now_us)
{
    if (!me) return 0u;
    for (uint32_t i = 0; i < N48_MKH_SLOTS; i++)
        if (__atomic_load_n(&t->s[i].holder, __ATOMIC_RELAXED) == me) {
            t->s[i].depth++;
            __atomic_fetch_add(&t->nested, 1ull, __ATOMIC_RELAXED);
            return (i + 1u) | N48_MKH_TOK_NESTED;
        }
    for (uint32_t i = 0; i < N48_MKH_SLOTS; i++) {
        uintptr_t z = 0u;
        if (!__atomic_compare_exchange_n(&t->s[i].busy, &z, me, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) continue;
        n48_mkh_slot *s = &t->s[i];
        s->depth = 0u; s->mask = 0u; s->yn = 0u; s->skips = 0u; s->yus = 0ull;
        for (uint32_t p = 0; p < N48_KW_PARTS; p++) s->dur[p] = 0ull;
        s->t0 = now_us; s->tmark = now_us;
        __atomic_store_n(&s->holder, me, __ATOMIC_RELEASE);
        return i + 1u;
    }
    __atomic_fetch_add(&t->overflow, 1ull, __ATOMIC_RELAXED);
    return 0u;
}
/* A part starts now. */
static inline void n48_mkh_mark(n48_mkh_table *t, uint32_t tok, uint64_t now_us)
{
    if (!n48_mkh_live(tok)) return;
    t->s[n48_mkh_idx(tok)].tmark = now_us;
}
/* The part that started at the last mark ends now; recorded only when it ran. */
static inline void n48_mkh_part(n48_mkh_table *t, uint32_t tok, uint32_t part, uint32_t ran, uint64_t now_us)
{
    if (!n48_mkh_live(tok) || part >= N48_KW_PARTS) return;
    n48_mkh_slot *s = &t->s[n48_mkh_idx(tok)];
    if (ran) {
        s->dur[part] += now_us >= s->tmark ? now_us - s->tmark : 0ull;
        s->mask |= 1u << part;
    }
    s->tmark = now_us;
}
/* RIGHT BEFORE THE RELEASE: clear the flag (the slot stays claimed until n48_mkh_end reads it). */
static inline void n48_mkh_unhold(n48_mkh_table *t, uint32_t tok)
{
    if (tok == 0u || (tok & 0xFFu) < 1u || (tok & 0xFFu) > N48_MKH_SLOTS) return;
    n48_mkh_slot *s = &t->s[n48_mkh_idx(tok)];
    if (tok & N48_MKH_TOK_NESTED) { if (s->depth) s->depth--; return; }
    __atomic_store_n(&s->holder, (uintptr_t)0u, __ATOMIC_RELEASE);
}
/* AFTER THE RELEASE: copy the window out and free the slot. */
static inline void n48_mkh_end(n48_mkh_table *t, uint32_t tok, n48_mkh_snap *out)
{
    out->valid = 0u;
    if (!n48_mkh_live(tok)) return;
    n48_mkh_slot *s = &t->s[n48_mkh_idx(tok)];
    out->valid = 1u; out->mask = s->mask; out->yn = s->yn; out->skips = s->skips; out->t0 = s->t0; out->yus = s->yus;
    for (uint32_t p = 0; p < N48_KW_PARTS; p++) out->dur[p] = s->dur[p];
    __atomic_store_n(&s->holder, (uintptr_t)0u, __ATOMIC_RELEASE);   /* already 0 after the unhold; never left set by a free */
    __atomic_store_n(&s->busy, (uintptr_t)0u, __ATOMIC_RELEASE);
}
/* THE MM GATE (asked only inside switch 37's yield branch): 0 = not a marker holder (yield as today); slot+1 = a holder that
 * yields (its spin is accounted by n48_mkh_spin); | N48_MKH_GATE_SKIP = a holder that skips the yield (`skip_on`, M4). */
static inline uint32_t n48_mkh_gate(n48_mkh_table *t, uintptr_t caller, uint32_t skip_on)
{
    if (!caller) return 0u;
    for (uint32_t i = 0; i < N48_MKH_SLOTS; i++) {
        if (__atomic_load_n(&t->s[i].holder, __ATOMIC_RELAXED) != caller) continue;
        if (skip_on) { t->s[i].skips++; return (i + 1u) | N48_MKH_GATE_SKIP; }
        return i + 1u;
    }
    return 0u;
}
static inline uint32_t n48_mkh_gate_skips(uint32_t g) { return (g & N48_MKH_GATE_SKIP) ? 1u : 0u; }
/* build 0.0.528 item 4 (measurement only, decision-inert): does `caller` hold a keystone withdrawal marker right now? slot+1 or
 * 0. NO side effect (unlike n48_mkh_gate, which counts M4's skips): the MM window asks it around its IOLockLock(gVramMmLock) and
 * times that wait only for a holder (n48_kwl_note). */
static inline uint32_t n48_mkh_holder(const n48_mkh_table *t, uintptr_t caller)
{
    if (!caller) return 0u;
    for (uint32_t i = 0; i < N48_MKH_SLOTS; i++)
        if (__atomic_load_n(&t->s[i].holder, __ATOMIC_RELAXED) == caller) return i + 1u;
    return 0u;
}
static inline void n48_mkh_spin(n48_mkh_table *t, uint32_t g, uint64_t us)
{
    if (g == 0u || (g & N48_MKH_GATE_SKIP) || (g & 0xFFu) > N48_MKH_SLOTS) return;
    n48_mkh_slot *s = &t->s[(g & 0xFFu) - 1u];
    s->yn++;
    s->yus += us;
}

/* ---- the histograms (item 1) ---------------------------------------------------------------------------------------------- */
#define N48_KW_BUCKETS 8u
enum { N48_KW_H_T = 0u, N48_KW_H_D = 1u, N48_KW_H_E = 2u, N48_KW_H_W = 3u, N48_KW_H_O = 4u, N48_KW_H_R = 5u, N48_KW_H_Y = 6u,
       N48_KW_HISTS = 7u };
#define N48_KW_EVENT_US    2000ull   /* a window longer than this gets its own line ... */
#define N48_KW_EVENT_LINES 16u       /* ... the first 16 per boot */
static inline uint32_t n48_kw_bucket(uint64_t us)
{
    if (us <= 100u) return 0u;
    if (us <= 250u) return 1u;
    if (us <= 500u) return 2u;
    if (us <= 1000u) return 3u;
    if (us <= 2000u) return 4u;
    if (us <= 4000u) return 5u;
    if (us <= 8000u) return 6u;
    return 7u;
}
typedef struct {
    uint32_t b[N48_KW_HISTS][N48_KW_BUCKETS];
    uint64_t mx[N48_KW_HISTS];
    uint64_t n, untimed, ev, yc, yus, skips;
} n48_kw_stats;
static inline void n48_kw_note(n48_kw_stats *st, uint32_t h, uint64_t us)
{
    if (h >= N48_KW_HISTS) return;
    __atomic_fetch_add(&st->b[h][n48_kw_bucket(us)], 1u, __ATOMIC_RELAXED);
    uint64_t cur = __atomic_load_n(&st->mx[h], __ATOMIC_RELAXED);
    while (us > cur && !__atomic_compare_exchange_n(&st->mx[h], &cur, us, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
/* One window: the total and every part that ran; the MM yield time (only when it spun); the counters. Returns 1 when the window
 * is an event (longer than N48_KW_EVENT_US) that should be logged (the first N48_KW_EVENT_LINES per boot). */
static inline uint32_t n48_kw_record(n48_kw_stats *st, const n48_mkh_snap *sn, uint64_t total_us, uint32_t *ev_no)
{
    if (ev_no) *ev_no = 0u;
    if (!sn->valid) { __atomic_fetch_add(&st->untimed, 1ull, __ATOMIC_RELAXED); return 0u; }
    __atomic_fetch_add(&st->n, 1ull, __ATOMIC_RELAXED);
    n48_kw_note(st, N48_KW_H_T, total_us);
    for (uint32_t p = 0; p < N48_KW_PARTS; p++)
        if (sn->mask & (1u << p)) n48_kw_note(st, N48_KW_H_D + p, sn->dur[p]);
    if (sn->yn) {
        n48_kw_note(st, N48_KW_H_Y, sn->yus);
        __atomic_fetch_add(&st->yc, (uint64_t)sn->yn, __ATOMIC_RELAXED);
        __atomic_fetch_add(&st->yus, sn->yus, __ATOMIC_RELAXED);
    }
    if (sn->skips) __atomic_fetch_add(&st->skips, (uint64_t)sn->skips, __ATOMIC_RELAXED);
    if (total_us <= N48_KW_EVENT_US) return 0u;
    const uint64_t e = __atomic_fetch_add(&st->ev, 1ull, __ATOMIC_RELAXED);
    if (e >= N48_KW_EVENT_LINES) return 0u;
    if (ev_no) *ev_no = (uint32_t)e + 1u;
    return 1u;
}
/* Display caps (the line must fit the 491-byte log body at every counter's widest value): counts at 99999, a histogram's max at
 * 999999 us, the yield total at 9999999 us (a printed cap means "at least"). */
static inline uint32_t n48_kw_c5(uint64_t v) { return v > 99999ull ? 99999u : (uint32_t)v; }
static inline uint32_t n48_kw_c6(uint64_t v) { return v > 999999ull ? 999999u : (uint32_t)v; }
static inline uint32_t n48_kw_c7(uint64_t v) { return v > 9999999ull ? 9999999u : (uint32_t)v; }

/* THE BARE-64 LINE (one line; terse so seven histograms fit the 491-byte body). args: 81's mode name, the switch-64 bound in use
 * (us), windows timed (n), windows untimed because the holder table was full (full), windows > 2000 us (>2ms), MM yields made
 * under the marker and their us, skipped yields (M4), then per histogram T D E W O R Y the 8 buckets (us <= 100, 250, 500, 1000,
 * 2000, 4000, 8000, more) and ^ the max us. T the window, D gfxsrc_desc_unmap, E the entry read, W the withdrawal, O Apple's
 * unmapVA, R the re-arm, Y the yield spin per window that spun. */
#define N48_KW_H_FMT "%u,%u,%u,%u,%u,%u,%u,%u^%u"
#define N48_KW_REPORT_FMT \
    "kswin: 81 %s bound %uus; n %u full %u >2ms %u; yields %u %uus skip %u; T" N48_KW_H_FMT " D" N48_KW_H_FMT " E" N48_KW_H_FMT \
    " W" N48_KW_H_FMT " O" N48_KW_H_FMT " R" N48_KW_H_FMT " Y" N48_KW_H_FMT
#define N48_KW_H_ARGS(st, h) \
    n48_kw_c5((st)->b[h][0]), n48_kw_c5((st)->b[h][1]), n48_kw_c5((st)->b[h][2]), n48_kw_c5((st)->b[h][3]), \
    n48_kw_c5((st)->b[h][4]), n48_kw_c5((st)->b[h][5]), n48_kw_c5((st)->b[h][6]), n48_kw_c5((st)->b[h][7]), \
    n48_kw_c6((st)->mx[h])
#define N48_KW_REPORT_ARGS(st, modeName, boundUs) \
    (modeName), (boundUs), n48_kw_c5((st)->n), n48_kw_c5((st)->untimed), n48_kw_c5((st)->ev), n48_kw_c5((st)->yc), \
    n48_kw_c7((st)->yus), n48_kw_c5((st)->skips), N48_KW_H_ARGS(st, N48_KW_H_T), N48_KW_H_ARGS(st, N48_KW_H_D), \
    N48_KW_H_ARGS(st, N48_KW_H_E), N48_KW_H_ARGS(st, N48_KW_H_W), N48_KW_H_ARGS(st, N48_KW_H_O), N48_KW_H_ARGS(st, N48_KW_H_R), \
    N48_KW_H_ARGS(st, N48_KW_H_Y)

/* build 0.0.528 item 4: THE MARKER HOLDER's gVramMmLock WAIT (navi48_vram_read_mm / navi48_vram_write_mm: from before
 * IOLockLock to after it, for a caller n48_mkh_holder names; the switch-37 yield before it is Y's, not this). Same buckets as the
 * kswin histograms. Nothing reads it to decide anything. */
typedef struct {
    uint32_t b[N48_KW_BUCKETS];
    uint64_t mx, n, sum;
} n48_kwl_stats;
static inline void n48_kwl_note(n48_kwl_stats *st, uint64_t us)
{
    __atomic_fetch_add(&st->b[n48_kw_bucket(us)], 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&st->n, 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&st->sum, us, __ATOMIC_RELAXED);
    uint64_t cur = __atomic_load_n(&st->mx, __ATOMIC_RELAXED);
    while (us > cur && !__atomic_compare_exchange_n(&st->mx, &cur, us, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
/* THE SECOND BARE-64 LINE (0.0.528; the first is full at 485 of 491 bytes). args: L = the marker holder's gVramMmLock wait (waits
 * timed, their us total, the 8 buckets ^ max, as the kswin histograms), then switch 83 (gfx_tlb83.h): its mode word, ON calls,
 * reads to the ack in 6 buckets (1, 2-3, 4-10, 11-100, 101-1000, > 1000), spin us total, fell back to sleep, ON timeouts, ON wait
 * max us, then OFF (poll_reg) calls, timeouts, wait max us. Counts cap at 99999, us at 9999999 (a printed cap means "at least"). */
#define N48_KW2_FMT \
    "kswin2: holder MM-lock wait n %u %uus L" N48_KW_H_FMT "; TLB ack 83 %s: ON calls %u reads %u,%u,%u,%u,%u,%u spin %uus " \
    "fell %u timeouts %u max %uus; OFF calls %u timeouts %u max %uus"

/* THE EVENT LINE (a window > 2000 us, the first 16 per boot). args: total us, event no, create #, fire #, the deferral verdict
 * number, desc, entry, withdrawal, Apple's unmapVA, re-arm (us), the parts that ran (mask), MM yields, their us, skipped, mode. */
#define N48_KW_EVENT_FMT \
    "kswin: WINDOW %llu us > 2000 (event %u of 16) create #%u fire #%u verdict %u: desc %llu entry %llu withdraw %llu " \
    "Apple's unmapVA %llu re-arm %llu us (ran %#x); MM under the marker: yields %u %llu us, skipped %u (81 %s)"

/* THE SWITCH-81 LINE. args: mode name, how, kept (all), kept while flip mode was ON, noted (asked), bound us, poll cap, the
 * marker-holder yield skip state, skipped yields, table-full windows. */
#define N48_KS81_REPORT_FMT \
    "ks81: switch 81 is %s%s. M1/M3/M4: a KEYSTONE withdrawal (token matched, keystone refused) with 73 ON keeps flip mode; " \
    "flip kept through keystone withdrawal %llu (%llu with flip mode ON); noted to flip mode %llu. Switch-64 bound %u us/%u " \
    "polls. Marker-holder MM yield %s (skipped %llu; holder table full %llu)."

#ifdef __cplusplus
}
#endif

#endif /* N48_GFX_KS81_H */
