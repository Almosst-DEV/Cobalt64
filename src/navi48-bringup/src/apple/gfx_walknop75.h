// gfx_walknop75.h — build 0.0.519: SWITCH 75, A FLIGHT NOPed AT THE RING WALK DOES NOT END THE ARM.
// Switch 75 (`75 | M << 8`: M 1 ON = 331, M 2 OFF = 587, the default and the boot value; bare `75` reads). Mid-arm guarded.
//
// THE CASE (run10w token seq 3, run10r token seq 10). The gate answered COMMIT, the keystone proved the frame and marked its
// flight-ring entry COMMITTED, then the ring walk refused the exemption (heap-gen) and NOPed the IB: `spared 0 of 1 IB(s)`.
// Through 0.0.518 the hook then marked the entry NOT_RUN, which n48_fr_defer_verdict still counts as LIVE: every unmap for two
// seconds was deferred behind a frame that could never run, the entry expired (`flightring: 1 NOT_RUN entry(ies) expiring`),
// the deferred withdrawal fired while it still showed live (withdrawnWhileLive), and the continuous arm stopped (stop_why 3).
//
// THE RULE. With the switch ON, a committed flight whose IB(s) the walk NOPed IN FULL is retired AT ONCE (the flight ring's
// terminal NOPED state, n48_fr_retire_nopped): no deferral hold, no NOT_RUN expiry, no withdrawal-while-live, no fence wait.
// "In full" is a POSITIVE PROOF taken by the walk itself (n48_wn75_proof), under gRenderDrainLock, right after
// gfx_neuter_frame NOPed the ring and BEFORE Apple's writeTail rings the doorbell:
//   - a record is in flight (the hook's TRANSLATE branch is inside Apple's original) and the walk REFUSED it (any reason but
//     none-in-flight and SPARED);
//   - the NOP pass over this walk's list was clean: every listed position NOPed and read back (done == npos), no read-back
//     mismatch, no listed position that was no longer an IB, no IB over the per-frame cap, the walk reached its end, and the
//     CP had NOT been told about this frame before the NOP (the race witness gfx_neuter_frame already reads);
//   - EVERY one of the frame's IBs (nib of them, IB k at the template position expect_pos + 4k) is in this walk's NOP list,
//     now reads as the NOP of an INDIRECT_BUFFER (type 3, opcode 0x10, count 2: the CP skips the three body dwords, so it
//     never fetches the IB at all - there is no partial IB), and its body still names the record's VA, length and VMID.
// The hook then retires only when (n48_wn75_plan): the switch is ON, the final answer was a refusal, spared 0 of N (a frame
// spared in part or whole - `spared k of N`, k > 0 - keeps today's behaviour), no walk during the record ever spared it, and
// the proof above was taken for THIS seq with no walk for the record failing it. Anything else is today's NOT_RUN.
//
// WHAT STAYS EXACTLY AS TODAY, ON OR OFF: the ledger un-feed of the frame's token (queued, drained at the top of the next
// judged frame, gfx_desc_port.h) - the plan always asks for it, so no later frame's proof can stand on this frame's outputs;
// switch 73's final outcome for the P (WITHDRAWN: the walk did not spare it); the token, gate seq and exemption record, all
// cleared on the hook's own path before and after Apple's original.
//
// Pure: no lock, no clock, no register, no log. Host-tested by tests/gfx_walknop75_test.cpp (run10w's real sequence).
#ifndef N48_GFX_WALKNOP75_H
#define N48_GFX_WALKNOP75_H

#include <stdint.h>
#include "gfx_neuter.h"

/* The NOP pass gfx_neuter_frame just made over this walk's position list (its own counters, per frame). */
typedef struct {
    uint32_t npos;     /* positions handed to the NOP */
    uint32_t done;     /* NOPed and read back */
    uint32_t bad;      /* read-back mismatches */
    uint32_t not_ib;   /* a listed position that was no longer an INDIRECT_BUFFER */
    uint32_t over;     /* IBs found beyond the list (the per-frame cap) */
    uint32_t stopped;  /* the walk stopped before the end of the new dwords */
    uint32_t raced;    /* CP_RB0_WPTR already included this frame before the NOP */
    uint32_t cp0;      /* build 0.0.520: CP_RB0_WPTR exactly as that pass read it BEFORE the NOP (0xFFFFFFFF = unreadable) */
} n48_wn75_pass;

/* 1 iff `h` is what n48_gfxn_nop_for makes of an INDIRECT_BUFFER header: type 3, opcode NOP (0x10), count 2. */
static inline uint32_t n48_wn75_is_ib_nop(uint32_t h)
{
    return (h >> 30) == 3u && ((h >> 8) & 0xFFu) == 0x10u && ((h >> 16) & 0x3FFFu) == 2u;
}

/* THE WALK'S PROOF (see the file header). `ex` is the exemption record the walk judged (the kext passes gCmx), `why` the answer
 * it got, `pos[0..npos)` the NOP list it handed gfx_neuter_frame, `p` that pass's own counters, `g`/`size` the ring AFTER the
 * NOP. 1 = every IB of the committed frame is proven NOPed before the doorbell; 0 = not proven (the caller keeps today's
 * behaviour). */
static inline uint32_t n48_wn75_proof(const volatile uint32_t *g, uint32_t size, const n48_gfxn_exempt_t *ex, uint32_t why,
                                      const uint32_t *pos, uint32_t npos, const n48_wn75_pass *p)
{
    if (!g || !size || !ex || !pos || !p) return 0u;
    if (ex->inflight != 1u || ex->seq == 0u) return 0u;
    if (why == N48_GFXN_EX_NONE || why == N48_GFXN_EX_SPARED || why >= N48_GFXN_EX_REASONS) return 0u;
    if (ex->pos_known != 1u || ex->vmid == 0u || ex->vmid > 15u) return 0u;
    const uint32_t nib = ex->nib;
    if (nib == 0u || nib > N48_GFXN_EX_MAX_IBS || (nib > 1u && !ex->mib)) return 0u;
    if (p->npos == 0u || p->npos != npos || p->done != npos || p->bad || p->not_ib || p->over || p->stopped || p->raced)
        return 0u;
    for (uint32_t k = 0; k < nib; k++) {
        const uint32_t at = (uint32_t)(((uint64_t)ex->expect_pos + 4ull * (uint64_t)k) % (uint64_t)size);
        uint32_t listed = 0u;
        for (uint32_t j = 0; j < npos; j++) if (pos[j] == at) { listed = 1u; break; }
        if (!listed) return 0u;
        const uint32_t h = g[at % size], lo = g[(at + 1u) % size], hi = g[(at + 2u) % size], ctl = g[(at + 3u) % size];
        const uint64_t va = ((uint64_t)hi << 32) | (lo & ~3u);
        const uint64_t wantVa = k == 0u ? ex->va : ex->va_k[k];
        const uint32_t wantLen = k == 0u ? ex->len : ex->len_k[k];
        if (!n48_wn75_is_ib_nop(h) || ((ctl >> 24) & 0xFu) != ex->vmid || (ctl & 0xFFFFFu) != wantLen || va != wantVa)
            return 0u;
    }
    return 1u;
}

/* build 0.0.520 (MEDIUM-1 of the 0.0.519 review): THE STRICT CP WITNESS. The proof stood on ONE read of
 * CP_RB0_WPTR taken before the NOP loop, judged by gfx_dep.h's n48_cp_told_past - which answers "not told" for an unreadable
 * register (0xFFFFFFFF) and for a register already past the frame's END, and which cannot see a publish that lands between that
 * read and the NOP stores. The proof now needs TWO reads, each positively "not told past the frame's start": the pass's own read
 * before the NOP (`cp0`) and a fresh read the hook takes AFTER the NOP loop and a full fence (`cp1`). Each must be readable and
 * `(int32_t)(cp - (uint32_t)from) <= 0`, in the same unwrapped 32-bit serial space n48_cp_told_past uses. n48_cp_told_past and
 * its callers (the neuter's race counter, the writeTail publish instrument) are unchanged. */
static inline uint32_t n48_wn75_cp_not_told(uint32_t cp_wptr, uint64_t from)
{
    if (cp_wptr == 0xFFFFFFFFu) return 0u;
    return (int32_t)(cp_wptr - (uint32_t)from) <= 0 ? 1u : 0u;
}
/* Why a proof failed (counted per reason). N48_WN75_PFS = no failure. */
enum { N48_WN75_PF_PASS = 0u, N48_WN75_PF_CP0_UNREADABLE, N48_WN75_PF_CP0_PAST, N48_WN75_PF_CP1_UNREADABLE, N48_WN75_PF_CP1_PAST,
       N48_WN75_PFS };
static inline uint32_t n48_wn75_cp_why(uint32_t cp0, uint32_t cp1, uint64_t from)
{
    if (cp0 == 0xFFFFFFFFu) return N48_WN75_PF_CP0_UNREADABLE;
    if (!n48_wn75_cp_not_told(cp0, from)) return N48_WN75_PF_CP0_PAST;
    if (cp1 == 0xFFFFFFFFu) return N48_WN75_PF_CP1_UNREADABLE;
    if (!n48_wn75_cp_not_told(cp1, from)) return N48_WN75_PF_CP1_PAST;
    return N48_WN75_PFS;
}
/* THE PROOF THE HOOK TAKES: both CP reads strict (above), then n48_wn75_proof. `from` is the frame's first ring dword (unwrapped),
 * `cp1` the post-NOP read. *fail = the reason (N48_WN75_PFS when proven). 1 = proven. */
static inline uint32_t n48_wn75_proof_strict(const volatile uint32_t *g, uint32_t size, const n48_gfxn_exempt_t *ex, uint32_t why,
                                             const uint32_t *pos, uint32_t npos, const n48_wn75_pass *p, uint64_t from,
                                             uint32_t cp1, uint32_t *fail)
{
    uint32_t w = p ? n48_wn75_cp_why(p->cp0, cp1, from) : N48_WN75_PF_PASS;
    if (w == N48_WN75_PFS && !n48_wn75_proof(g, size, ex, why, pos, npos, p)) w = N48_WN75_PF_PASS;
    if (fail) *fail = w;
    return w == N48_WN75_PFS ? 1u : 0u;
}

/* What the walk leaves for the hook, per exemption record (the kext's gCmxWn, cleared where the record is set up). */
typedef struct {
    uint32_t seq;      /* the record's seq a proof was taken for (0 = none) */
    uint32_t proven;   /* walks that proved it */
    uint32_t failed;   /* walks that refused the record but could NOT prove it */
} n48_wn75_rec;

/* The walk's bookkeeping for one refusal of an in-flight record (switch ON only: the kext asks only then). */
static inline void n48_wn75_note(n48_wn75_rec *r, uint32_t seq, uint32_t proven)
{
    if (!r) return;
    if (proven && seq) { r->seq = seq; r->proven++; }
    else r->failed++;
}

/* THE HOOK'S PLAN, after Apple's original returned (see the file header). */
enum { N48_WN75_KEEP = 0u, N48_WN75_RETIRE = 1u };
typedef struct {
    uint32_t action;   /* N48_WN75_KEEP: today's n48_fr_mark_not_run; N48_WN75_RETIRE: n48_fr_retire_nopped */
    uint32_t unfeed;   /* queue the ledger un-feed of the frame's token (always 1 when the frame was not spared) */
    uint32_t why;      /* N48_WN75_WHY_*: why KEEP (report only) */
} n48_wn75_plan_t;
enum { N48_WN75_WHY_RETIRED = 0u, N48_WN75_WHY_OFF, N48_WN75_WHY_NOT_REFUSED, N48_WN75_WHY_SPARED_SOME, N48_WN75_WHY_SPARED_ONCE,
       N48_WN75_WHY_NO_PROOF, N48_WN75_WHYS };
static inline const char *n48_wn75_why_name(uint32_t w)
{
    static const char *const n[N48_WN75_WHYS] = { "RETIRED (the walk proved every IB NOPed before the doorbell)",
        "switch 75 OFF", "not a walk refusal (none in flight, or SPARED)", "spared some of the frame's IBs",
        "a walk spared the record", "no proof for this seq (or a walk for the record could not prove it)" };
    return w < N48_WN75_WHYS ? n[w] : "?";
}
/* `sparedN`/`nib`: the hook's own "spared k of N". `sparedAt`: gCmxSparedAt (0xFFFFFFFF = no walk spared the record). */
static inline n48_wn75_plan_t n48_wn75_plan(uint32_t on, uint32_t seq, uint32_t exWhy, uint32_t sparedN, uint32_t nib,
                                            uint32_t sparedAt, const n48_wn75_rec *r)
{
    n48_wn75_plan_t o;
    o.action = N48_WN75_KEEP;
    o.unfeed = sparedN == 0u ? 1u : 0u;   /* exactly today's condition: a frame that was not spared is un-fed */
    o.why = N48_WN75_WHY_OFF;
    if (!on) return o;
    (void)nib;
    if (exWhy == N48_GFXN_EX_NONE || exWhy == N48_GFXN_EX_SPARED || exWhy >= N48_GFXN_EX_REASONS) {
        o.why = N48_WN75_WHY_NOT_REFUSED; return o;
    }
    if (sparedN != 0u) { o.why = N48_WN75_WHY_SPARED_SOME; return o; }
    if (sparedAt != 0xFFFFFFFFu) { o.why = N48_WN75_WHY_SPARED_ONCE; return o; }
    if (!r || !seq || r->seq != seq || !r->proven || r->failed) { o.why = N48_WN75_WHY_NO_PROOF; return o; }
    o.action = N48_WN75_RETIRE;
    o.why = N48_WN75_WHY_RETIRED;
    return o;
}

/* Counters (since boot) and the lines. */
typedef struct { uint64_t retired, kept, proofs, proofFails, notRetiredInRing; uint64_t proofFailWhy[N48_WN75_PFS]; } n48_wn75_stats;
/* args: ON/OFF, how, retired, kept (refused but not retired), proofs taken, proof failures, ring refusals (entry not COMMITTED),
 * the ring's own NOPED count. */
#define N48_WN75_FMT "walknop75: switch 75 %s%s. walk-NOPed flights RETIRED AT ONCE %llu, kept NOT_RUN (not proven) %llu; " \
    "walk proofs %llu, proof failures %llu; ring refused the retire %llu; ring NOPED %llu."
/* build 0.0.520: args: proofFailWhy[PASS], [CP0_UNREADABLE], [CP0_PAST], [CP1_UNREADABLE], [CP1_PAST]. */
#define N48_WN75_PF_FMT "walknop75: (0.0.520) proof failures by reason: NOP pass or IB check %llu; CP_RB0_WPTR before the NOP " \
    "unreadable %llu, past the frame start %llu; after the NOP and a fence unreadable %llu, past the frame start %llu."
/* args: seq, nib, why name, the exemption reason name. */
#define N48_WN75_LINE_FMT "flightring: token seq %u (%u IB(s)) RETIRED AT ONCE AS NOPED (switch 75) - %s; the walk's answer " \
    "was %s. Nothing can run from this flight: no deferral hold, no withdrawal, no fence wait; the ledger un-feed stands."

#endif /* N48_GFX_WALKNOP75_H */
