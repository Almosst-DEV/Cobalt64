// gfx_r5win104.h — build 0.0.547 items 1-2 ( ranked fix (1),; the latch-fix pattern of switch
// 99): SWITCH 104, "r5win", THE R5′ BLIND WINDOW, and the first-BLIND line.
//
// THE MECHANISM (from RUN AW): R5′'s BLIND bucket (gfx_dep.h n48_r5_ring.blind) is incremented by n48_r5_note and zeroed only
// by n48_r5_scope (arm level or descriptor epoch change), and n48_dep_check refuses at N48_DEP_NEUTER_UNREADABLE while it is > 0.
// So ONE held-back frame whose destination set could not be read refuses every later frame of the arm: RUN AW's ~4 BLIND frames at
// ~+50 s froze the glass for 70 s (`dep reasons 2:26 20:103` per second, reason 0 never returned).
//
// `accel gfxneuter 104 | M << 8 | K << 12`: M 1 ON (= 360), M 2 OFF (= 616, the default and the boot value), M 3 SHADOW (= 872).
// Bare `104` reads. K (bits 12..23) is the window in JUDGED FRAMES, 1..N48_R5W_K_MAX; K 0 with M 1-3 means the default
// N48_R5W_K_DEFAULT (32). Any other M, a K above the cap, or any bit above 23 is refused unchanged. Joins the continuous mid-arm
// guard (gfx_commit.h n48_cm_cont_switch_refused). Writes no register, no page table, nothing of Apple's.
//   ON:     the world's `r5_blind` is the bucket's count only while the CURRENT judged frame is within K frames after the most recent
//           BLIND note (cur - last_blind <= K, i.e. judged - last_blind < K), or AT/BEFORE it; otherwise 0. The count itself is kept.
//   SHADOW: the world carries the latched count (0.0.546's decision); the kext counts the frames the window WOULD have let through.
//   OFF:    the world carries n48_r5_unknown(ring), exactly 0.0.546's value.
//
// WHAT THE WINDOW CAN ADMIT (build 0.0.548 item A, the 0.0.547 review: the old wording "it never protects the GPU" was wrong).
// R5′ is asked about HELD-BACK frames, whose IBs were zeroed before Apple's original emitted them, so a held-back frame WROTE
// NOTHING; a BLIND one is one whose would-be destinations we could not name, and a later consumer may read a page that frame would
// have written and see OLDER bytes. Older bytes are harmless only as texels: a stale DESCRIPTOR or ADDRESS is read by the GPU as a
// pointer (the same reason gfx_dep.h's R3 refuses a held-back write to a raw-pointer page). So the window clears ONLY the BLIND notes
// whose clause can hide nothing but colour-target pixels - targets-truncated, target-unresolved and scan-over. Every note whose clause
// can hide a memory-destination write, a DISPATCH or an unread/unwalked IB (gfx_dep.h n48_r5_frame_latched, 0.0.549: decided from the
// frame's OWN fields - ib-over, ib-unread, walk, memw-truncated, memw-unresolved, dispatch, inherited - whatever clause fired first) is counted in the ring's `latched_blind`, which the window never clears
// (n48_r5w_blind ON refuses while it is > 0; only n48_r5_scope resets it). What the window can admit is therefore stale TEXEL content
// only, with one stated residue: R3 also refuses a consumer pointer page that a held-back frame named as a COLOUR TARGET, and a colour
// target the BLIND frame did not name (truncated / unresolved) is not in the hazard set, so after K frames R3 is not asked about it.
// D4-PRIME-FIXES.md chose to "keep the arm-wide latch"; this switch changes that decision only while ON, and only for those
// three clauses. The window never touches the judged frame's OWN record: a frame is noted only after its own gate ran, and a frame at
// or before `last_blind` (the BLIND frame itself, or a note that ran ahead of this gather) still refuses.
//
// PURE: compiled into the kext and into tests/gfx_wo99_test.cpp.
#ifndef N48_GFX_R5WIN104_H
#define N48_GFX_R5WIN104_H

#include <stdint.h>
#include "gfx_dep.h"

#define N48_R5W_SWITCH     104u
#define N48_R5W_K_DEFAULT  32u
#define N48_R5W_K_MAX      1024u
#define N48_R5W_FIRST_MAX  8u     /* first-BLIND lines per R5′ scope (arm level + descriptor epoch) */
enum { N48_R5W_M_ON = 1u, N48_R5W_M_OFF = 2u, N48_R5W_M_SHADOW = 3u };   /* the switch's M, held as is; OFF at boot */

typedef struct { uint32_t ok, m, k; } n48_r5w_arg;
/* The verb's argument. ok 0 = refused (unknown M, K past the cap, a bit above 23, or a K with a bare read). */
static inline n48_r5w_arg n48_r5w_decode(uint64_t arg)
{
    n48_r5w_arg a; a.ok = 0u; a.m = (uint32_t)((arg >> 8) & 0xFu); a.k = (uint32_t)((arg >> 12) & 0xFFFu);
    if ((arg & 0xFFull) != (uint64_t)N48_R5W_SWITCH || (arg >> 24) != 0ull) return a;
    if (a.m == 0u) { a.ok = a.k == 0u ? 1u : 0u; return a; }
    if (a.m != N48_R5W_M_ON && a.m != N48_R5W_M_OFF && a.m != N48_R5W_M_SHADOW) return a;
    if (a.k > N48_R5W_K_MAX) return a;
    if (a.k == 0u) a.k = N48_R5W_K_DEFAULT;
    a.ok = 1u;
    return a;
}
static inline uint32_t n48_r5w_k_clamp(uint32_t k)
{
    return k == 0u ? N48_R5W_K_DEFAULT : (k > N48_R5W_K_MAX ? N48_R5W_K_MAX : k);
}
/* Is frame `cur` inside the window of the BLIND note at frame `last`? A BLIND count with no frame (0) is always inside (fail
 * closed); so is a frame at or before `last`. */
static inline uint32_t n48_r5w_in_window(uint64_t last, uint64_t cur, uint32_t k)
{
    if (last == 0ull) return 1u;
    if (cur <= last) return 1u;
    return (cur - last) <= (uint64_t)n48_r5w_k_clamp(k) ? 1u : 0u;
}
/* THE WORLD'S r5_blind. OFF and SHADOW (and anything unknown): n48_r5_unknown(r), 0.0.546's value exactly. ON: that count while
 * any LATCHED note stands (0.0.548 item A: never cleared by the window) or while `cur` is in the window, else 0. It reads the ring's
 * `blind`, `latched_blind` and `last_blind` and nothing else. */
static inline uint64_t n48_r5w_blind(const n48_r5_ring *r, uint32_t mode, uint32_t k, uint64_t cur)
{
    const uint64_t b = n48_r5_unknown(r);
    if (mode != N48_R5W_M_ON || !r || b == 0ull) return b;
    if (r->latched_blind != 0ull) return b;
    return n48_r5w_in_window(r->last_blind, cur, k) ? b : 0ull;
}
/* The OTHER rule's answer for a world the gate already judged: a copy with r5_blind replaced (only when R5′ is the rule in force
 * for it), checked by the same n48_dep_check. Never writes `w`. */
static inline uint32_t n48_r5w_check_alt(const n48_dep_world *w, uint64_t alt, n48_dep_world *scratch, uint64_t *detail)
{
    if (!w || !scratch) return n48_dep_check(w, detail);
    *scratch = *w;
    if (scratch->consumer_enumerated == 1u && scratch->r5_mode == 1u) scratch->r5_blind = alt;
    return n48_dep_check(scratch, detail);
}

/* ---- the counters (reset by the verb); each gate the kext asks with R5′ in force and a BLIND count > 0 ---- */
typedef struct {
    uint64_t gates;        /* gates whose ring carried blind > 0 with R5′ in force (28 + 30 ON and the consumer enumerated) */
    uint64_t in_window;    /* ... and the current frame inside the window */
    uint64_t rung_clear;   /* ON: the latch would have refused at R5′ and the window did not (SHADOW: would not have) */
    uint64_t pass;         /* ON: ... and the whole check then answered clean (SHADOW: would have answered clean) */
    uint64_t pass_first, pass_last;   /* frame numbers of the first and last `pass` */
    uint64_t first_lines;  /* first-BLIND lines printed (every mode) */
} n48_r5w_st;
enum { N48_R5W_E_NONE = 0u, N48_R5W_E_CLEAR = 1u, N48_R5W_E_PASS = 2u };
/* One gate. `latched` is the ring's count, `windowed` the ON value; `today` is the gate's reason, `other` the other rule's. */
static inline uint32_t n48_r5w_gate(n48_r5w_st *st, uint32_t mode, uint64_t latched, uint64_t windowed, uint32_t today, uint32_t other,
                                    uint64_t frame)
{
    if (!st || latched == 0ull || (mode != N48_R5W_M_ON && mode != N48_R5W_M_SHADOW)) return N48_R5W_E_NONE;
    st->gates++;
    if (windowed != 0ull) { st->in_window++; return N48_R5W_E_NONE; }
    /* ON: the gate answered with the window (`today`), the latch is `other`. SHADOW: the gate answered with the latch. */
    const uint32_t latchR = mode == N48_R5W_M_ON ? other : today, winR = mode == N48_R5W_M_ON ? today : other;
    if (latchR != N48_DEP_NEUTER_UNREADABLE || winR == N48_DEP_NEUTER_UNREADABLE) return N48_R5W_E_NONE;
    st->rung_clear++;
    if (winR != N48_DEP_OK) return N48_R5W_E_CLEAR;
    st->pass++;
    if (!st->pass_first) st->pass_first = frame;
    st->pass_last = frame;
    return N48_R5W_E_PASS;
}

static inline const char *n48_r5w_mode_name(uint32_t m)
{
    return m == N48_R5W_M_ON ? "ON (360: BLIND refuses only within K frames of the last BLIND note)"
         : m == N48_R5W_M_SHADOW ? "SHADOW (872: latched as today; the window's passes counted)"
         : "OFF (616, default: 0.0.546's arm-wide latch)";
}

/* ---- the lines (each <= 491 bytes at maximal fields: tests/gfx_wo99_test.cpp R7) ---- */
#define N48_R5W_FMT "r5win: 104 %s, K %u%s; blind %llu (last BLIND frame %llu, scope %u); gates blind>0 %llu, in window %llu, " \
    "R5' cleared %llu, then clean %llu (frames %llu..%llu); first-BLIND lines %llu"
#define N48_R5W_ARGS(m, k, how, r, s) n48_r5w_mode_name(m), (k), (how), (unsigned long long)(r)->blind, \
    (unsigned long long)(r)->last_blind, (r)->arm_seq, (unsigned long long)(s)->gates, (unsigned long long)(s)->in_window, \
    (unsigned long long)(s)->rung_clear, (unsigned long long)(s)->pass, (unsigned long long)(s)->pass_first, \
    (unsigned long long)(s)->pass_last, (unsigned long long)(s)->first_lines
/* BLIND by clause (ring scope): the eleven counters in n48_r5_blind_clause order. */
#define N48_R5W_CLAUSE_FMT "r5win: BLIND by clause (this scope): ib-over %llu, ib-unread %llu, walk %llu, targets-truncated %llu, " \
    "memw-truncated %llu, target-unresolved %llu, memw-unresolved %llu, scan-over %llu, dispatch %llu, inherited %llu, none %llu"
#define N48_R5W_CLAUSE_ARGS(r) (unsigned long long)(r)->blind_clause[N48_R5_BC_IB_OVER], \
    (unsigned long long)(r)->blind_clause[N48_R5_BC_IB_UNREAD], (unsigned long long)(r)->blind_clause[N48_R5_BC_WALK], \
    (unsigned long long)(r)->blind_clause[N48_R5_BC_TGT_TRUNC], (unsigned long long)(r)->blind_clause[N48_R5_BC_MEMW_TRUNC], \
    (unsigned long long)(r)->blind_clause[N48_R5_BC_TGT_UNRESOLVED], (unsigned long long)(r)->blind_clause[N48_R5_BC_MEMW_UNRESOLVED], \
    (unsigned long long)(r)->blind_clause[N48_R5_BC_SCAN_OVER], (unsigned long long)(r)->blind_clause[N48_R5_BC_DISPATCH], \
    (unsigned long long)(r)->blind_clause[N48_R5_BC_INHERITED], (unsigned long long)(r)->blind_clause[N48_R5_BC_NONE]
/* build 0.0.548 item A: the latched-vs-windowed split of this scope's BLIND notes (the verb's report and the STOP). */
#define N48_R5W_SPLIT_FMT "r5win: BLIND split (this scope): latched %llu (ib-over, ib-unread, walk, memw-truncated, memw-unresolved, " \
    "dispatch, inherited: never cleared by the window), windowed %llu (targets-truncated, target-unresolved, scan-over: ON clears " \
    "them K frames after the last BLIND note while nothing is latched)"
#define N48_R5W_SPLIT_ARGS(r) (unsigned long long)(r)->latched_blind, \
    (unsigned long long)((r)->blind >= (r)->latched_blind ? (r)->blind - (r)->latched_blind : 0ull)
/* THE FIRST-BLIND LINE (every mode, <= N48_R5W_FIRST_MAX per scope): the frame, its owner, the clause, declared vs read IBs, the
 * reader, each read IB's len/got/walk (4 slots; "-" past the read count), the target and destination lists against their caps,
 * the unresolved count, and the remaining bucket flags. */
typedef struct {
    uint64_t frame;
    int32_t pid;
    const char *pname;
    uint32_t clause, declared, nib, reader_ok, shape_ok, short_read;
    uint32_t len[4], got[4], walk[4];
    uint32_t ntgt, nmemw, targets_ok, memw_ok, tgts_resolved, memw_resolved, n_unresolved, scan_over, has_dispatch, dispatch_ok,
             has_draw, out_of_scope;
} n48_r5w_first;
#define N48_R5W_FIRST_FMT "r5win: FIRST-BLIND %u/%u frame %llu pid %d (%.19s) clause %s; IBs declared %u read %u (reader %u shape %u " \
    "short %u); IB len/got/walk %u/%u/%u %u/%u/%u %u/%u/%u %u/%u/%u; targets %u of %u (ok %u resolved %u), memw %u of %u (ok %u " \
    "resolved %u), unresolved %u; scan-over %u dispatch %u/%u draw %u oos %u; blind %llu"
/* Counts are masked to their real widths (an IB length is 20 bits; IB counts, list lengths and the line number are small) and the
 * flags are printed as 0/1, so the line's widest form is bounded (tests/gfx_wo99_test.cpp R11). */
#define N48_R5W_B(v) ((uint32_t)((v) != 0u))
#define N48_R5W_FIRST_ARGS(n, x, blind) ((n) & 0xFFu), N48_R5W_FIRST_MAX, (unsigned long long)(x)->frame, (x)->pid, \
    ((x)->pname && (x)->pname[0]) ? (x)->pname : "unknown", n48_r5_blind_clause_name((x)->clause), (x)->declared & 0xFFFFu, \
    (x)->nib & 0xFFFFu, N48_R5W_B((x)->reader_ok), N48_R5W_B((x)->shape_ok), N48_R5W_B((x)->short_read), \
    (x)->len[0] & 0xFFFFFu, (x)->got[0] & 0xFFFFFu, (x)->walk[0] & 0xFFFFFu, (x)->len[1] & 0xFFFFFu, (x)->got[1] & 0xFFFFFu, \
    (x)->walk[1] & 0xFFFFFu, (x)->len[2] & 0xFFFFFu, (x)->got[2] & 0xFFFFFu, (x)->walk[2] & 0xFFFFFu, (x)->len[3] & 0xFFFFFu, \
    (x)->got[3] & 0xFFFFFu, (x)->walk[3] & 0xFFFFFu, (x)->ntgt & 0xFFFFu, N48_CP_TGT_MAX, N48_R5W_B((x)->targets_ok), \
    N48_R5W_B((x)->tgts_resolved), (x)->nmemw & 0xFFFFu, N48_CP_MEMW_MAX, N48_R5W_B((x)->memw_ok), N48_R5W_B((x)->memw_resolved), \
    (x)->n_unresolved & 0xFFFFu, N48_R5W_B((x)->scan_over), N48_R5W_B((x)->has_dispatch), N48_R5W_B((x)->dispatch_ok), \
    N48_R5W_B((x)->has_draw), N48_R5W_B((x)->out_of_scope), (unsigned long long)(blind)
/* Fill the line's record from the R5′ record (the IB triples are the caller's). */
static inline void n48_r5w_first_fill(n48_r5w_first *o, const n48_r5_frame *x)
{
    o->clause = n48_r5_blind_clause(x);
    o->ntgt = x->ntgt; o->nmemw = x->nmemw; o->targets_ok = x->targets_ok; o->memw_ok = x->memw_ok;
    o->tgts_resolved = x->tgts_resolved; o->memw_resolved = x->memw_resolved; o->n_unresolved = x->n_unresolved;
    o->scan_over = x->scan_over; o->has_dispatch = x->has_dispatch; o->dispatch_ok = x->dispatch_ok; o->has_draw = x->has_draw;
    o->out_of_scope = x->out_of_scope;
}

#endif
