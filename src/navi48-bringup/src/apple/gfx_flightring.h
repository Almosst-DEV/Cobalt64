// gfx_flightring.h — C5 part 1 (0.0.443, an internal design note Q1 "For N flights"): THE FLIGHT RING.
// Pure C, host-tested by tests/gfx_flightring_test.cpp (with planted defects); the kext compiles the SAME header.
// DEFAULT-INERT in the sense every other X-header here is: a zero-initialised ring is sixteen FREE entries, and
// every function below answers the empty/off case first.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHY THIS EXISTS —'s hazard, generalised past ONE flight
// ---------------------------------------------------------------------------------------------------------------------
// Through 0.0.442 there was ONE kstone flight record (`gKsFlight`), stamped by every commit at the gate BEFORE the
// keystone runs. At budget > 1 a second candidate B overwrote the record of a committed frame A that might still be
// executing; 0.0.430 CLEARED it on B's refusal and let hook_unmapVA WITHDRAW root[511] under A (the arm3/arm10 class,
// ); 0.0.431 fixed it by STASHING the outgoing record at the stamp and RESTORING it on a matching refusal
// (`n48_ks_flight_save` / `n48_ks_flight_restore_ok`, still in gfx_keystone.h and still exercised by that header's own
// interleaving model). That fix works for exactly ONE flight at a time being clobbered by exactly ONE later stamp.
//
// This build replaces the single record with a RING of up to N48_FR_CAPACITY flights, each independent. A keystone
// refusal now frees ONLY the entry the refused commit itself pushed (item 2 of the brief) — nothing else in the ring
// is touched, so the stash/restore dance is no longer needed for the kext's own use (the pure save/restore helpers
// stay in gfx_keystone.h, unused by the kext from this build on, because gfx_keystone_test.cpp's own H2 interleaving
// model still drives them independently of anything here).
//
// ---------------------------------------------------------------------------------------------------------------------
// THE FIVE STATES
// ---------------------------------------------------------------------------------------------------------------------
//   FREE       the slot holds no flight. The boot-time zero value.
//   PENDING    pushed at the GATE STAMP — the same instant 0.0.442 wrote `gKsFlight.active = 1`, BEFORE the keystone
//              runs. This preserves KEYSTONE-A-PRIME's ordering (an internal design note, "A′'s order: stamp
//              before the marker read"): an unmap that races in between the stamp and the keystone's decision must
//              still see this flight as live and defer, exactly as it did when the single record's `active` bit was
//              set at the same instant.
//   COMMITTED  the keystone proved the frame will run (n48_fr_mark_committed). In flight; may still retire by its own
//              fence or expire by its own timeout.
//   NOT_RUN    reached from COMMITTED (the ring-walk exemption refused the frame AFTER the keystone permitted it —
//              the IB was NOPed and never reached the CP) OR, since 0.0.444 (an internal review note (B) item 4), from
//              PENDING (a TOKEN MISMATCH at the hook: the entry's own flight preserved A′'s ordering and must stay
//              live rather than being freed, matching 0.0.442's timing — see n48_fr_mark_not_run). Either way it
//              is still deferred to its own bound — nothing will ever write its fence slot, so a poll can never
//              retire it, and it can end only by the timeout (0.0.444: or by EXPIRED, below). It must NEVER be
//              read as retired (item 1's "never counted as retirement"): n48_fr_poll_entry refuses to retire
//              anything but a COMMITTED entry.
//   RETIRED    this entry's OWN owned-slot fence read OURS (n48_fr_poll_entry). Terminal for the DECISION: never
//              re-armed, never polled again, never counted as live. Reclaimable (0.0.444, below).
//   EXPIRED    (0.0.444, item 6) a live entry whose own age passed the withdrawal bound (n48_fr_expire), asked the
//              same question n48_fr_defer_verdict already asks per entry. Terminal for the DECISION, exactly as
//              RETIRED is, but it names "we gave up waiting" apart from "we saw it finish" for a report. Reclaimable.
//
// A keystone REFUSAL frees a PENDING entry back to FREE (n48_fr_free_by_seq) — the entry never ran, so there is
// nothing left for it to protect. 0.0.444 (item 6): RETIRED and EXPIRED entries are RECLAIMED back to FREE
// (n48_fr_reclaim) once their terminal state has been observed, because the ring's capacity (16) is NOT far above a
// future continuous arm's spend rate (switch 41, the NEXT build) the way it is above this build's own one-shot
// ceiling (N48_CM_SHOT_BUDGET_MAX = 4) — without reclamation a long-running arm hits RING-FULL at its 16th commit
// and never recovers. `newestSeq`/`newestRetired` on the ring itself (item 7) are what let a decision still answer
// NOW_EOP for "the newest flight retired" AFTER its slot has been reclaimed and no longer holds that evidence.
//
// ---------------------------------------------------------------------------------------------------------------------
// THE WITHDRAWAL DECISION (item 4) — "ANY entry live and within its bound" DEFERS; EVERY entry ended WITHDRAWS
// ---------------------------------------------------------------------------------------------------------------------
// n48_fr_defer_verdict asks, of every entry still PENDING/COMMITTED/NOT_RUN ("live"), the same clock question
// gfx_keystone.h's n48_ksd_eval asks of its one flight — is `now_us - at_us >= timeout_us`? — and combines them with
// a FOR-ALL: withdrawal is permitted only once every live entry has expired ITS OWN bound (or retired by its own
// fence, which excludes it from "live" entirely). One entry still within its bound defers the whole decision,
// exactly as item 4 asks. At exactly one live entry this reduces field for field to n48_ksd_eval's own answer — the
// budget-1 identity the brief requires — and gfx_flightring_test.cpp proves it by driving both functions on the same
// fixture and comparing n48_ksd_defer() of each.
//
// A torn stamp (`at_us == 0` on a live entry, or `now_us` before it) fails the WHOLE ring closed to N48_KSD_NOW_TORN,
// the same direction n48_ksd_eval takes for its one flight — TORN does not defer, it falls back to 0.0.383's
// unconditional withdraw/re-arm. This is the one place a single entry's anomaly can affect the others, and it is
// deliberate: `at_us` comes from the same monotonic clock every push and every decision reads, so a live entry whose
// own stamp is impossible is a sign the clock or a store is not to be trusted for ANY entry this instant, not just
// its own — exactly the class of doubt n48_ksd_eval already treats as ground for falling back to the pre-deferral
// behaviour rather than defer past it. It cannot make withdrawal happen EARLIER than today for a legitimately
// tracked entry: it degrades the *system*, not one entry's evidence, to the same safe floor 0.0.383 always had.
//
// A RETIRED entry answers "not in flight" wherever it would once have answered "end of pipe observed": both are the
// SAME point in gfx_keystone.h's own enum (n48_ksd_defer is 0 for both N48_KSD_NOW_EOP and N48_KSD_NOW_NOT_IN_FLIGHT),
// so the defer/withdraw DECISION is identical either way; only the reported reason NAME can differ, and it is
// reported honestly (a retired entry really is no longer in flight).
//
// ---------------------------------------------------------------------------------------------------------------------
// OUT-OF-ORDER (item 5) — RECORDED ONLY IN THIS BUILD
// ---------------------------------------------------------------------------------------------------------------------
// n48_fr_out_of_order asks, the instant an entry retires, whether any entry with an EARLIER ordinal is still live
// (not yet retired). It answers a question, nothing more: this build does not stop on it, count it against a safety
// gate, or change any decision because of it. The NEXT build (switch 41, continuous mode) makes it a stop condition.
#ifndef N48_GFX_FLIGHTRING_H
#define N48_GFX_FLIGHTRING_H

#include <stdint.h>
#include "gfx_keystone.h"   /* reuses N48_KSD_* and n48_ksd_name for the ring-wide defer verdict (see above) */

#ifdef __cplusplus
extern "C" {
#endif

#define N48_FR_CAPACITY 16u

enum {
    N48_FR_FREE = 0,
    N48_FR_PENDING,
    N48_FR_COMMITTED,
    N48_FR_NOT_RUN,
    N48_FR_RETIRED,
    /* 0.0.444 (an internal design note (B) item 6) — EXPIRED, TERMINAL. A live entry (PENDING/COMMITTED/
     * NOT_RUN) whose own age has passed the same bound n48_fr_defer_verdict already judges it against
     * (n48_fr_expire). Distinct from RETIRED (a fence proved the frame ran) so a report can still tell "we gave up
     * waiting" from "we saw it finish" apart, but for every DECISION this header makes (defer/withdraw, blocking,
     * out-of-order) it is excluded from "live" exactly as RETIRED is — it was never going to retire by fence, and
     * a decision that is still waiting on it is waiting on nothing. */
    N48_FR_EXPIRED,
    /* build 0.0.519 - NOPED, TERMINAL. A COMMITTED entry whose IB(s) the ring walk NOPed in full
     * before the doorbell (the walk's own positive proof, gfx_walknop75.h), retired at once by n48_fr_retire_nopped. Excluded
     * from "live" exactly as RETIRED/EXPIRED are (every live test here names PENDING/COMMITTED/NOT_RUN explicitly), never
     * polled (n48_fr_next_poll takes COMMITTED only), reclaimed like them. Reached ONLY with switch 75 ON; OFF no entry is
     * ever NOPED and every function below answers exactly as 0.0.518's. */
    N48_FR_NOPED,
    N48_FR_STATES
};

static inline const char *n48_fr_state_name(uint32_t s)
{
    static const char *const n[N48_FR_STATES] = { "FREE", "PENDING", "COMMITTED", "NOT_RUN", "RETIRED", "EXPIRED", "NOPED" };
    return s < N48_FR_STATES ? n[s] : "?";
}

/* {seq, at_us, ordinal, vram_off, want, state} — exactly C5-CONTINUOUS.md Q1's field list.
 *   seq       the commit's own token seq (gXdCmToken.seq / gXdCmGateSeq): never 0 for a real push.
 *   at_us     the gate stamp (drain_now_us() at the spend) — the SAME instant 0.0.442 wrote gKsFlight.at_us.
 *   ordinal   the fence828 commit ordinal this entry's slot was placed at; 0 = no owned-slot fence for this commit
 *             (a fence-less producer, R1-MEMDST Q5, or the fence828 switch off / the candidate refused) — this
 *             entry can end only by timeout, never by n48_fr_poll_entry, matching the build's own constraint.
 *   vram_off  the fence slot's VRAM offset (gRingMap.base + fence page + slot*4); 0 = no fence, same meaning as above.
 *   want      the value only this commit's fence writes (epoch|ordinal); 0 = no fence.
 *   state     N48_FR_*.
 *
 * CROSS-THREAD ACCESS, replicating 0.0.384's `gKsFlight` discipline (the SAME hazard, generalised): hook_unmapVA can
 * run on a different thread than the commit path (that IS the deferral's whole premise — another client's unmap
 * arriving while WindowServer's committed frame still executes), and it only ever READS an entry, never writes one.
 * `state` and `at_us` are the two fields that cross-thread read touches (`state` as the gate, `at_us` only once
 * `state` says the entry is live) and are declared `volatile` for exactly the reason gKsFlight's three fields were:
 * so the compiler cannot cache a stale value across the loop this header's own functions run. The KEXT is
 * responsible for the fence discipline at the two boundaries, exactly as it always was for gKsFlight: a
 * `__ATOMIC_RELEASE` thread fence after every write that publishes a NEW value of `state` (the push, and every
 * later transition), and ONE `__ATOMIC_ACQUIRE` thread fence in hook_unmapVA BEFORE it calls n48_fr_defer_verdict
 * (covering every entry's `state` and `at_us` that call goes on to read, rather than one fence per entry — simpler,
 * and strictly more conservative). This header's own functions run single-threaded (the writer side is always the
 * one commit-path thread, serialised by the kext's own gXdLock, exactly as gKsFlight's writer always was) and never
 * need the fences themselves; only the kext's cross-thread reader does, at its own call site.
 * `seq`/`ordinal`/`vram_off`/`want` are written ONCE at the push and read afterwards only by the SAME thread (the
 * poll loop) or informationally (a best-effort log field), so they do not need to be volatile. */
typedef struct {
    uint32_t seq;
    volatile uint64_t at_us;
    uint32_t ordinal;
    uint64_t vram_off;
    uint32_t want;
    volatile uint32_t state;
} n48_fr_entry;

typedef struct {
    n48_fr_entry e[N48_FR_CAPACITY];
    /* Read-only reporting, never gates anything. */
    uint64_t pushed, pushRefusedFull, committed, notRun, retired, freedOnRefusal, polls, outOfOrder;
    /* 0.0.444 (item 6) — EXPIRY AND RECLAMATION, read-only counters. */
    uint64_t expired, reclaimed;
    /* 0.0.444 (item 7) — THE NEWEST PUSH'S OWN FATE, so n48_fr_defer_verdict can answer NOW_EOP for "the newest
     * flight retired and nothing else is live" even after RETIRED/EXPIRED slots have been reclaimed and no longer
     * hold the evidence themselves (item 6's reclamation would otherwise erase exactly the fact this needs).
     * Written ONLY by n48_fr_push (a new push is always the newest, and starts unretired) and by n48_fr_poll_entry
     * (a retirement that lands on the CURRENT newest seq sets it). Never read by anything but n48_fr_defer_verdict. */
    uint32_t newestSeq;
    uint32_t newestRetired;
    /* 0.0.446 ( fix (1)) — THE NEWEST PUSH EXPIRED. Set by n48_fr_expire when the entry it expires is the
     * CURRENT newest push, cleared by every push. With nothing live, n48_fr_defer_verdict answers NOW_TIMEOUT for an
     * expired newest flight - the SAME verdict it gave that entry the instant before n48_fr_expire moved it out of
     * "live" (past its bound, nothing blocking) - instead of NOT_IN_FLIGHT, which named no fate at all and let
     * `kstone-defer APPLIED` exceed end-of-pipe + timeout. Reporting only: n48_ksd_defer is 0 for all three names. */
    uint32_t newestExpired;
    /* 0.0.446 ( fix (1)) — THE STASH OF THE PREVIOUS NEWEST, the ring's generalisation of 0.0.431's
     * n48_ks_flight_save / n48_ks_flight_restore_ok (gfx_keystone.h). n48_fr_push saves the outgoing newest
     * {seq, retired, expired} here before it overwrites them; n48_fr_free_by_seq, freeing the CURRENT newest (a
     * keystone refusal of the frame that just pushed), restores them - so a refused push can no longer erase the fate
     * of the flight before it (0.0.444: "the newest seq/retired are not restored on a free"). Kept current while
     * stashed: a retirement or expiry landing on the stashed seq updates the stashed flag too. One level deep, exactly
     * as 0.0.431's was: a push is followed by its own keystone decision on the same call chain, so at most one push
     * is ever outstanding; a second restore finds the stash already consumed and restores zeros (NOT_IN_FLIGHT, the
     * pre-0.0.446 answer). Never read by anything but n48_fr_free_by_seq. */
    uint32_t prevNewestSeq, prevNewestRetired, prevNewestExpired;
    /* build 0.0.519 (switch 75) - entries retired as NOPED by n48_fr_retire_nopped (read-only report counter). */
    uint64_t noped;
} n48_fr_ring;

static inline void n48_fr_reset(n48_fr_ring *r)
{
    if (!r) return;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) {
        r->e[i].seq = 0u; r->e[i].at_us = 0ull; r->e[i].ordinal = 0u;
        r->e[i].vram_off = 0ull; r->e[i].want = 0u; r->e[i].state = N48_FR_FREE;
    }
    r->pushed = 0ull; r->pushRefusedFull = 0ull; r->committed = 0ull; r->notRun = 0ull;
    r->retired = 0ull; r->freedOnRefusal = 0ull; r->polls = 0ull; r->outOfOrder = 0ull;
    r->expired = 0ull; r->reclaimed = 0ull; r->newestSeq = 0u; r->newestRetired = 0u;
    r->newestExpired = 0u; r->prevNewestSeq = 0u; r->prevNewestRetired = 0u; r->prevNewestExpired = 0u;
    r->noped = 0ull;
}

/* 1 iff no entry is FREE — the gate's RING-FULL rung (item 6). */
static inline uint32_t n48_fr_full(const n48_fr_ring *r)
{
    if (!r) return 1u;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++)
        if (r->e[i].state == N48_FR_FREE) return 0u;
    return 1u;
}

/* PENDING at the gate stamp (item 1). Finds the first FREE slot; refuses (0) when the ring is full — the caller must
 * have already asked n48_fr_full() at the gate (this is the SAME test, asked again at the write so a caller can never
 * silently overwrite a live entry by skipping the gate's own rung). `seq` must be non-zero: a push with seq 0 can
 * never be found again (n48_fr_find refuses seq 0 by construction, see below) and is refused outright. */
static inline uint32_t n48_fr_push(n48_fr_ring *r, uint32_t seq, uint64_t at_us, uint32_t ordinal,
                                   uint64_t vram_off, uint32_t want, uint32_t *out_idx)
{
    if (out_idx) *out_idx = N48_FR_CAPACITY;
    if (!r || !seq) return 0u;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) {
        if (r->e[i].state == N48_FR_FREE) {
            r->e[i].seq = seq;
            r->e[i].at_us = at_us;
            r->e[i].ordinal = ordinal;
            r->e[i].vram_off = vram_off;
            r->e[i].want = want;
            r->e[i].state = N48_FR_PENDING;
            r->pushed++;
            /* 0.0.444 (item 7) — a new push is always the newest by construction (pushes are serialised on the
             * one commit-path thread under gXdLock, exactly as the rest of this header's writer side is), and it
             * starts unretired even if an OLDER entry is still live or has already retired.
             * 0.0.446 ( fix (1)) — the outgoing newest's fate is STASHED first, so a keystone refusal of
             * THIS push (n48_fr_free_by_seq) can put it back. */
            r->prevNewestSeq = r->newestSeq;
            r->prevNewestRetired = r->newestRetired;
            r->prevNewestExpired = r->newestExpired;
            r->newestSeq = seq;
            r->newestRetired = 0u;
            r->newestExpired = 0u;
            if (out_idx) *out_idx = i;
            return 1u;
        }
    }
    r->pushRefusedFull++;
    return 0u;
}

/* Find the LIVE (non-FREE) entry with this token seq. seq 0 never matches — 0 is never a real token seq (gXdCmSeq
 * pre-increments from 0, so the first real seq is 1), and treating it as a wildcard would let an uninitialised
 * caller-side seq alias whichever slot happens to hold a stale 0. A slot that has gone back to FREE keeps its old
 * seq field until the NEXT push overwrites it, so excluding FREE is what stops a stale seq from re-matching. */
static inline uint32_t n48_fr_find(const n48_fr_ring *r, uint32_t seq, uint32_t *out_idx)
{
    if (out_idx) *out_idx = N48_FR_CAPACITY;
    if (!r || !seq) return 0u;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++)
        if (r->e[i].state != N48_FR_FREE && r->e[i].seq == seq) {
            if (out_idx) *out_idx = i;
            return 1u;
        }
    return 0u;
}

/* PENDING -> COMMITTED: the keystone proved the frame will run. Refuses (0, no-op) on any other state, so a call
 * this build never repeats twice cannot silently move a COMMITTED/NOT_RUN/RETIRED entry backward. */
static inline uint32_t n48_fr_mark_committed(n48_fr_ring *r, uint32_t seq)
{
    uint32_t idx;
    if (!r || !n48_fr_find(r, seq, &idx)) return 0u;
    if (r->e[idx].state != N48_FR_PENDING) return 0u;
    r->e[idx].state = N48_FR_COMMITTED;
    r->committed++;
    return 1u;
}

/* 0.0.446 ( fix (1)) — THE COMMIT MARK, AND THE ONE PLACE THE KEXT'S "NEWEST FLIGHT" IS NAMED.
 * `*last_flight` is the kext's gKsLastFlightSeq: the token seq every kstone-defer line calls "the newest flight" and
 * the fence828 promotion stamps into its committed record. Through 0.0.445 it was set at the PUSH (PENDING), before
 * the keystone ran, so a keystone-REFUSED frame's seq was named as the newest flight ('s instrument note:'s
 * falsifier would fire falsely). It is now set HERE, only when PENDING -> COMMITTED actually happened, so a refused
 * or mismatched push never becomes "the newest flight". Returns n48_fr_mark_committed's own answer; `last_flight`
 * untouched on 0. Names a flight, decides nothing: no withdrawal or defer path reads gKsLastFlightSeq. */
static inline uint32_t n48_fr_commit_mark(n48_fr_ring *r, uint32_t seq, uint32_t *last_flight)
{
    if (!n48_fr_mark_committed(r, seq)) return 0u;
    if (last_flight) *last_flight = seq;
    return 1u;
}

/* COMMITTED -> NOT_RUN, or (0.0.444, an internal review note (B) item 4) PENDING -> NOT_RUN.
 *   COMMITTED -> NOT_RUN: the ring-walk exemption refused it (the IB was NOPed and never reached the CP).
 *   PENDING -> NOT_RUN: a TOKEN MISMATCH at the hook. This entry's push preserved KEYSTONE-A-PRIME's ordering
 *     ("stamp before the marker read"), so it must stay counted as a live flight — freeing it to FREE instead
 *     (0.0.443's behaviour, and the review's headline SUSPECTED gap: a concurrent free by the shared
 *     gXdCmToken.seq could drop ANOTHER frame's still-PENDING entry) would let hook_unmapVA withdraw while that
 *     other frame might still be in flight, which is EARLIER than 0.0.442 ever allowed. NOT_RUN keeps it live,
 *     deferred to its own bound, exactly as 0.0.442's single-flight timing did for an exemption refusal.
 * Never RETIRED (a fence that already fired cannot be un-fired), never NOT_RUN again (idempotence is the caller's
 * business, and every call site in this build asks it at most once per seq). */
static inline uint32_t n48_fr_mark_not_run(n48_fr_ring *r, uint32_t seq)
{
    uint32_t idx;
    if (!r || !n48_fr_find(r, seq, &idx)) return 0u;
    if (r->e[idx].state != N48_FR_COMMITTED && r->e[idx].state != N48_FR_PENDING) return 0u;
    r->e[idx].state = N48_FR_NOT_RUN;
    r->notRun++;
    return 1u;
}

/* build 0.0.519 - COMMITTED -> NOPED: THE WALK NOPed THIS FLIGHT'S IB(S) IN FULL BEFORE THE
 * DOORBELL, SO IT CAN NEVER RUN. The caller (hook_gfxCommitIB, after Apple's original returned) calls this INSTEAD of
 * n48_fr_mark_not_run only when gfx_walknop75.h's n48_wn75_plan answered RETIRE: switch 75 ON, spared 0 of N, no walk ever
 * spared the record, and the walk's own positive proof for THIS seq (every IB packet of the frame, at its template position,
 * found by the walk, NOPed with read-back, its body still naming the record's VA/length/VMID, the NOP pass clean and not
 * raced by the CP). Nothing will ever read the flight's resources (the CP skips the NOP packet whole: it never fetches the
 * IB) and nothing will ever write its fence slot, so there is nothing for the unmap deferral to wait for: the entry leaves
 * "live" at once (no 2 s hold, no NOT_RUN expiry, no withdrawal-while-live). Only a COMMITTED entry moves (the state
 * n48_fr_commit_mark set at the keystone; a PENDING, NOT_RUN or terminal entry is a no-op, 0 - the fail-closed direction:
 * the caller then keeps today's n48_fr_mark_not_run). The newest-flight names are restored from the stash exactly as
 * n48_fr_free_by_seq restores them for a keystone refusal (this flight never ran, so the flight before it is the newest one
 * that could have) - names only, n48_ksd_defer is 0 for every verdict they select. `state` is published first. */
static inline uint32_t n48_fr_retire_nopped(n48_fr_ring *r, uint32_t seq)
{
    uint32_t idx;
    if (!r || !n48_fr_find(r, seq, &idx)) return 0u;
    if (r->e[idx].state != N48_FR_COMMITTED) return 0u;
    r->e[idx].state = N48_FR_NOPED;                /* THE PUBLISH: from this store on, the entry is not live */
    r->noped++;
    if (seq == r->newestSeq) {
        r->newestSeq = r->prevNewestSeq;
        r->newestRetired = r->prevNewestRetired;
        r->newestExpired = r->prevNewestExpired;
        r->prevNewestSeq = 0u; r->prevNewestRetired = 0u; r->prevNewestExpired = 0u;   /* consumed: one level deep */
    }
    return 1u;
}

/* Item 2: a keystone refusal frees ONLY its own entry. Only a PENDING entry may be freed this way — by the time an
 * entry is COMMITTED/NOT_RUN/RETIRED the keystone has already permitted it once and this call site never runs for
 * it again, so this is the fail-closed direction (a call on anything else is a no-op, never a stray free of a live
 * entry). Every OTHER entry in the ring is untouched, which is the whole of what replaces 0.0.431's stash/restore:
 * there is no single record left for a refusal to clobber. */
/* 0.0.444 (an internal review note (B) item 2/3, Q1 route 2) — `state` GOES FIRST. Through 0.0.443 this wrote the fields
 * (`at_us` among them, to 0) BEFORE `state = FREE`, so a concurrent scan (hook_unmapVA holds no lock across this)
 * could observe PENDING with `at_us == 0` — a torn stamp — and fail the whole ring closed to TORN even while an
 * EARLIER, still-COMMITTED entry was validly within its bound: an earlier-withdrawal route the review named
 * (headline Q1 route 2). Publishing `state = FREE` FIRST closes that window: the instant a reader can observe the
 * new state, the entry is no longer "live" by any of this header's own checks (every live-entry loop tests `state`
 * before it ever reads `at_us`), so the stale fields it clears afterward are never read by a concurrent decision.
 * The `state` store is `volatile` and this thread's writes retire to memory in program order (x86-64 TSO, no
 * store/store reordering); the fields cleared after it are read again only by the SAME single writer thread's own
 * next push into this slot.
 * 0.0.446 ( fix (1)) — AND IF THE FREED ENTRY IS THE RING'S NEWEST PUSH, THE PREVIOUS NEWEST'S FATE IS
 * RESTORED. 0.0.444 left `newestSeq`/`newestRetired` naming the refused push (unretired), so with nothing live the
 * verdict answered NOT_IN_FLIGHT even when the flight before it had retired (should be NOW_EOP) or expired (should
 * be NOW_TIMEOUT) - `kstone-defer APPLIED` then exceeded end-of-pipe + timeout. The restore is the ring's form of
 * 0.0.431's n48_ks_flight_restore_ok: only on a free whose seq IS the current newest, from the stash n48_fr_push
 * filled, which is then consumed. Names only: n48_ksd_defer is 0 for NOW_EOP, NOW_TIMEOUT and NOT_IN_FLIGHT alike,
 * so no defer/withdraw decision can move. */
static inline uint32_t n48_fr_free_by_seq(n48_fr_ring *r, uint32_t seq)
{
    uint32_t idx;
    if (!r || !n48_fr_find(r, seq, &idx)) return 0u;
    if (r->e[idx].state != N48_FR_PENDING) return 0u;
    r->e[idx].state = N48_FR_FREE;                 /* THE PUBLISH: from this store on, the entry reads as gone */
    r->e[idx].seq = 0u; r->e[idx].at_us = 0ull; r->e[idx].ordinal = 0u;
    r->e[idx].vram_off = 0ull; r->e[idx].want = 0u;
    r->freedOnRefusal++;
    if (seq == r->newestSeq) {
        r->newestSeq = r->prevNewestSeq;
        r->newestRetired = r->prevNewestRetired;
        r->newestExpired = r->prevNewestExpired;
        r->prevNewestSeq = 0u; r->prevNewestRetired = 0u; r->prevNewestExpired = 0u;   /* consumed: one level deep */
    }
    return 1u;
}

/* Item 3/item 1's RETIRED: feed one poll for entry `idx`. `got`/`val` are the caller's OWN VRAM read of THIS entry's
 * OWN vram_off (never Apple's page table — the same owned-slot read gfx_fence828.h's poll already performs).
 * Retires (1) only a COMMITTED entry whose OWN want matches; a fence-less entry (vram_off/want both 0 — "want" alone
 * is enough, since a real fence's want is never 0: n48_f828_owned_gate refuses a zero epoch) never retires here,
 * matching the build's constraint that a commit without an owned-slot fence keeps today's behaviour (bound-only).
 * A NOT_RUN entry never retires (item 1's "NOT_RUN never counts as retirement") because it is not COMMITTED any
 * more — this is the ONE guard that makes that constraint hold even if its slot were, by some coincidence, ever
 * read as equal to `want` (it structurally cannot be, because nothing ever writes it, but the state check is what a
 * planted-break test can prove rather than an argument about hardware). Idempotent: an already-RETIRED entry is
 * simply not COMMITTED any more and this returns 0 without touching anything. */
static inline uint32_t n48_fr_poll_entry(n48_fr_ring *r, uint32_t idx, uint32_t got, uint32_t val)
{
    if (!r || idx >= N48_FR_CAPACITY) return 0u;
    r->polls++;
    n48_fr_entry *e = &r->e[idx];
    if (e->state != N48_FR_COMMITTED) return 0u;
    if (e->want == 0u) return 0u;               /* fence-less: never retired by a poll, only by its own timeout */
    if (!got || val != e->want) return 0u;
    /* 0.0.444 (item 7) — if THIS entry is the ring's own newest push, record that it retired, before the state
     * write so a defensive reader between the two stores never sees "newest" and "retired" disagree (irrelevant on
     * one thread, kept for the same reason the free above orders its own publish first). */
    if (e->seq == r->newestSeq) r->newestRetired = 1u;
    if (e->seq == r->prevNewestSeq) r->prevNewestRetired = 1u;   /* 0.0.446: keep the stash current ( fix (1)) */
    e->state = N48_FR_RETIRED;
    r->retired++;
    return 1u;
}

/* 0.0.444 (an internal review note (B) item 6) — A LIVE ENTRY PAST ITS OWN BOUND BECOMES EXPIRED. Asks exactly the same
 * per-entry age question n48_fr_defer_verdict's timeout pass does (a torn `at_us == 0` or a future stamp never
 * expires here — the defer verdict's own TORN/age-0 handling covers those, and expiry is a STATE change, so it
 * must never fire on evidence the defer verdict itself would not trust). Idempotent: EXPIRED is terminal, and this
 * only ever reads PENDING/COMMITTED/NOT_RUN. Returns the number of entries newly expired this call (never gates
 * anything; for the report line only). */
static inline uint32_t n48_fr_expire(n48_fr_ring *r, uint32_t now_ok, uint64_t now_us, uint64_t timeout_us)
{
    if (!r || !now_ok || timeout_us == 0ull) return 0u;
    uint32_t n = 0u;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) {
        n48_fr_entry *e = &r->e[i];
        if (e->state != N48_FR_PENDING && e->state != N48_FR_COMMITTED && e->state != N48_FR_NOT_RUN) continue;
        if (e->at_us == 0ull || now_us < e->at_us) continue;         /* torn or future: not this function's business */
        if (now_us - e->at_us < timeout_us) continue;                /* still within bound */
        /* 0.0.446 ( fix (1)): the newest push's own fate, and the stash's, recorded before the state
         * write for the same reason n48_fr_poll_entry records newestRetired first. */
        if (e->seq == r->newestSeq) r->newestExpired = 1u;
        if (e->seq == r->prevNewestSeq) r->prevNewestExpired = 1u;
        e->state = N48_FR_EXPIRED;
        r->expired++;
        n++;
    }
    return n;
}

/* 0.0.444 (item 6) — RECLAIM RETIRED AND EXPIRED SLOTS BACK TO FREE. Without this, a ring that never has an unmap
 * arrive to withdraw it (continuous mode, the NEXT build) fills at N48_FR_CAPACITY commits and every later push
 * refuses at RING-FULL forever — the terminal states were designed to be terminal for the DECISION (never live
 * again), not to be reserved forever. Safe to call after any retirement/expiry: nothing downstream of this build
 * reads a RETIRED or EXPIRED entry's fields once its state has been observed (the report line reads the COUNTS,
 * gathered before reclamation frees the slot; `newestRetired`, item 7, is read from the RING, not the entry, for
 * exactly this reason — reclaiming the newest entry's slot must never erase whether it retired). Returns the
 * number of slots reclaimed this call. */
/* 0.0.446 ( fix (3)) — RECLAMATION HOLDS BACK AN ENTRY A PENDING tgtsample AFTER STILL NEEDS.
 * `hold[0..nhold)` are token seqs the caller still has to look up by n48_fr_find (the kext: every tgtsample slot that
 * published a BEFORE and has not yet taken its AFTER - gfxsrc_ts_after_slot finds its flight BY SEQ, and a reclaimed
 * slot answers "no flight", so the AFTER could never be taken:'s "the tgtsample AFTER is DEAD"). A RETIRED or
 * EXPIRED entry whose seq is held stays in its terminal state (never live again: every decision this header makes
 * already excludes both) until a later call no longer holds it. Seq 0 in `hold` never matches (0 is not a token seq).
 * What a held entry costs is one slot of capacity (the kext holds at most N48_TS_SLOTS = 2); it can never defer or
 * withdraw anything. n48_fr_reclaim below is this with nothing held - 0.0.444's behaviour exactly. */
static inline uint32_t n48_fr_reclaim_hold(n48_fr_ring *r, const uint32_t *hold, uint32_t nhold)
{
    if (!r) return 0u;
    uint32_t n = 0u;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) {
        n48_fr_entry *e = &r->e[i];
        if (e->state != N48_FR_RETIRED && e->state != N48_FR_EXPIRED && e->state != N48_FR_NOPED) continue;   /* 0.0.519: NOPED too */
        uint32_t held = 0u;
        for (uint32_t h = 0; hold && h < nhold; h++) if (hold[h] && hold[h] == e->seq) { held = 1u; break; }
        if (held) continue;
        e->state = N48_FR_FREE;                 /* publish first, exactly as the keystone-refusal free does */
        e->seq = 0u; e->at_us = 0ull; e->ordinal = 0u; e->vram_off = 0ull; e->want = 0u;
        r->reclaimed++;
        n++;
    }
    return n;
}

static inline uint32_t n48_fr_reclaim(n48_fr_ring *r)
{
    return n48_fr_reclaim_hold(r, 0, 0u);
}

/* 0.0.446 ( fix (2)) — THE POLL ORDER IS THE ORDINAL ORDER, NOT THE SLOT ORDER. Returns the index of the
 * next COMMITTED, fenced (`want != 0`) entry whose key (ordinal << 32 | seq) is STRICTLY greater than `*cursor`,
 * and advances `*cursor` to that key; N48_FR_CAPACITY when there is none. Starting from `*cursor == 0` and calling
 * until N48_FR_CAPACITY visits every pollable entry exactly once, oldest ordinal first. WHY: after reclamation a newer
 * commit can occupy a LOWER slot index than an older one still in flight; polling by slot index then retires the
 * newer one first on a pass where BOTH fences read OURS, and n48_fr_out_of_order - asked the instant it retires -
 * sees the older one still COMMITTED and raises a false OUT-OF-ORDER from slot position alone. In ordinal order the
 * older retires first on that pass, and a GENUINE out-of-order (the older's fence not yet written) is still caught.
 * The seq in the low half only breaks a tie no real ring holds (the fence ordinal advances per promoted commit). */
static inline uint32_t n48_fr_next_poll(const n48_fr_ring *r, uint64_t *cursor)
{
    if (!r || !cursor) return N48_FR_CAPACITY;
    uint32_t best = N48_FR_CAPACITY;
    uint64_t bestKey = 0ull;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) {
        if (r->e[i].state != N48_FR_COMMITTED || r->e[i].want == 0u) continue;
        const uint64_t key = ((uint64_t)r->e[i].ordinal << 32) | (uint64_t)r->e[i].seq;
        if (key <= *cursor) continue;
        if (best == N48_FR_CAPACITY || key < bestKey) { best = i; bestKey = key; }
    }
    if (best != N48_FR_CAPACITY) *cursor = bestKey;
    return best;
}

/* 0.0.446 ( fix (4)) — THE KEYSTONE GUARD'S OWN ANSWER (0.0.444 item 5(ii): no entry, no keystone, no run),
 * as a value the hook can name in its OWN line. 0.0.444 folded this into `ksOk` and printed the keystone's refusal
 * line with a zero KsResult - "keystone verdict 0 (OK - the keystone is written)" - which asserts a keystone write
 * that never happened. OK only when the identity matched AND this seq has a PENDING entry; `*state_out` (optional)
 * carries the found entry's state for the line (N48_FR_STATES = no entry). */
enum { N48_FR_GUARD_OK = 0, N48_FR_GUARD_NO_TOKEN, N48_FR_GUARD_NO_ENTRY, N48_FR_GUARD_NOT_PENDING, N48_FR_GUARDS };
static inline const char *n48_fr_guard_name(uint32_t g)
{
    static const char *const n[N48_FR_GUARDS] = { "a PENDING ring entry (OK)", "no identity match", "no ring entry",
                                                  "a ring entry that is not PENDING" };
    return g < N48_FR_GUARDS ? n[g] : "?";
}
static inline uint32_t n48_fr_keystone_guard(const n48_fr_ring *r, uint32_t tok_match, uint32_t seq, uint32_t *state_out)
{
    if (state_out) *state_out = N48_FR_STATES;
    if (!tok_match) return N48_FR_GUARD_NO_TOKEN;
    uint32_t idx = N48_FR_CAPACITY;
    if (!r || !n48_fr_find(r, seq, &idx)) return N48_FR_GUARD_NO_ENTRY;
    if (state_out) *state_out = r->e[idx].state;
    return r->e[idx].state == N48_FR_PENDING ? N48_FR_GUARD_OK : N48_FR_GUARD_NOT_PENDING;
}
/* The guard-refusal line (args: token seq, guard name, entry state name). It says what happened and nothing about a
 * keystone verdict, because the keystone did not run. Bounded by gfx_flightring_test.cpp like N48_FR_REPORT_FMT. */
#define N48_FR_GUARD_REFUSED_FMT \
    "gfx-commit: COMMIT WITHDRAWN AT THE FLIGHT-RING GUARD - the frame's identity matched, but token seq %u has %s " \
    "(state %s), so the KEYSTONE WAS NOT RUN for it and wrote nothing. NEUTERING: a commit whose flight the ring " \
    "cannot track must not run (0.0.444 item 5(ii))."

/* Item 5/item 8 (0.0.444, an internal review note Q5): does any entry with an ordinal EARLIER than `retiredOrdinal`
 * remain un-retired and STILL ABLE TO RETIRE BY FENCE (PENDING or COMMITTED)? 0.0.443 also counted NOT_RUN here,
 * which the review named a false-positive source once retirement is wired up: a NOT_RUN entry's own slot is never
 * written by anything (gfx_fence828.h's owned slot dies with the IB that never ran), so it can NEVER retire by
 * fence and comparing OTHER entries' retirement order against it answers a question about a fence that does not
 * exist. EXPIRED is excluded the same way, by construction (neither is in the OR below). Ordinal 0 (fence-less)
 * entries are excluded on both sides of the comparison — an ordinal-less commit was never part of the fence
 * ordering either. Compares by ORDINAL (the fence828 commit ordinal, assigned once at the push and never touched
 * by which SLOT an entry occupies), never by slot index, so a slot reclaimed and reused for a later push with a
 * fresh ordinal can never be misread as "the same flight, out of order" — this is why the ordinal lives in the
 * entry rather than being inferred from position. RECORDED ONLY in this build (see the file header); the caller
 * decides whether to log and count it, this function only answers the question. */
static inline uint32_t n48_fr_out_of_order(const n48_fr_ring *r, uint32_t retiredOrdinal)
{
    if (!r || !retiredOrdinal) return 0u;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) {
        const uint32_t st = r->e[i].state;
        if ((st == N48_FR_PENDING || st == N48_FR_COMMITTED) &&
            r->e[i].ordinal && r->e[i].ordinal < retiredOrdinal)
            return 1u;
    }
    return 0u;
}

/* 0.0.444 (an internal review note (B) item 2) — "THE RING SCAN", ASKED BEFORE THE CLOCK IS EVER READ. A cheap presence
 * check over `state` alone (no `at_us`, no timing), so the caller can decide whether reading the clock is even
 * worth doing WITHOUT yet comparing anything against a `now_us` snapshot. This is the fix for Q1 route 1: through
 * 0.0.443 the caller (hook_unmapVA) read the clock, THEN scanned the ring, so a push landing in between could hand
 * n48_fr_defer_verdict a `now_us` that is OLDER than a brand-new entry's own `at_us` (a future stamp) purely
 * because of the reading order. Reading the clock only AFTER this presence scan removes that window for every
 * entry the scan itself observed (its `at_us` was fixed at ITS OWN, earlier push, so any later clock read is >= it
 * by construction); n48_fr_defer_verdict's own future-stamp handling (age 0, never TORN) covers the residual case
 * of a push landing between THIS scan and the clock read the caller takes next. */
static inline uint32_t n48_fr_any_live(const n48_fr_ring *r)
{
    if (!r) return 0u;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) {
        const uint32_t st = r->e[i].state;
        if (st == N48_FR_PENDING || st == N48_FR_COMMITTED || st == N48_FR_NOT_RUN) return 1u;
    }
    return 0u;
}

/* Item 4 (and item 2's TORN/future-stamp fix, and item 7's NOW_EOP): the ring-wide withdrawal verdict. See the file
 * header for the base reasoning; this is the mechanism. `on`/`now_ok`/`now_us`/`timeout_us` are exactly
 * n48_ksd_eval's inputs of the same names — the caller is expected to have read the clock AFTER n48_fr_any_live
 * answered 1 (see that function's comment), never before. `armed` (gfx_keystone.h's clause 2, "our root[511] entry
 * is standing for this context") is NOT asked here — it is per-CONTEXT, not per-flight, and the caller asks it
 * exactly as it always did, before ever calling this function.
 * `blocking_seq`/`blocking_at_us`, when non-null, are filled with the OLDEST live entry (by at_us) whether or not it
 * is the one still blocking — 0/0 when the ring holds no live entry at all. This names the entry item 8 (0.0.444)
 * asks a log line to carry as the SECOND field, beside the newest flight (gKsLastFlightSeq, kept by the kext).
 * Returns one of gfx_keystone.h's N48_KSD_* so every existing n48_ksd_name()/n48_ksd_defer() caller keeps working
 * unchanged.
 *
 * 0.0.444 (item 2) — TORN IS NOW PER ENTRY, AND DEFER FROM ANY VALID BLOCKER BEATS IT. Through 0.0.443 a single
 * entry's own torn stamp (`at_us == 0`, the shape a half-written free could leave — Q1 route 2, fixed at the
 * source by n48_fr_free_by_seq's reordering above, and kept here as defense in depth) or a future stamp
 * (`now_us < at_us`, Q1 route 1) failed the WHOLE RING closed to TORN, even while another entry was validly
 * blocking. Now: a future stamp counts that ONE entry's age as 0 (still-live, not-yet-expired — the conservative
 * reading, since a stamp from an instant we have not reached yet cannot be OLDER than the timeout) rather than
 * poisoning the ring; a torn (`at_us == 0`) entry is excluded from the blocking computation entirely (it proves
 * nothing about age, positive OR negative) rather than failing everything; and the ring only answers TORN when NO
 * entry validly blocks AND at least one entry's own evidence could not be trusted — DEFER from any valid blocker
 * always wins, because leaving root[511] standing a little longer is the safe direction this whole mechanism
 * exists to protect, and one entry's bad clock read is never grounds to make that decision EARLIER. */
static inline uint32_t n48_fr_defer_verdict(const n48_fr_ring *r, uint32_t on, uint32_t now_ok, uint64_t now_us,
                                            uint64_t timeout_us, uint32_t *blocking_seq, uint64_t *blocking_at_us)
{
    if (blocking_seq) *blocking_seq = 0u;
    if (blocking_at_us) *blocking_at_us = 0ull;
    if (!r) return N48_KSD_NOW_TORN;
    if (!on) return N48_KSD_NOW_OFF;

    uint32_t anyLive = 0u, anyTorn = 0u;
    uint64_t oldestAt = 0ull;
    uint32_t oldestSeq = 0u;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) {
        const uint32_t st = r->e[i].state;
        if (st != N48_FR_PENDING && st != N48_FR_COMMITTED && st != N48_FR_NOT_RUN) continue;
        anyLive = 1u;
        if (r->e[i].at_us == 0ull) { anyTorn = 1u; continue; }   /* this entry's own stamp proves nothing; not the ring's */
        if (!oldestAt || r->e[i].at_us < oldestAt) { oldestAt = r->e[i].at_us; oldestSeq = r->e[i].seq; }
    }
    if (!anyLive)
        /* Item 7 (0.0.444): nothing is live NOW. If the ring's own newest push retired (n48_fr_poll_entry saw its
         * fence), that is END OF PIPE OBSERVED for the current flight, exactly as 0.0.442's single-flight
         * `n48_ksd_eval` answered it — tracked here, not by re-reading the (possibly already-reclaimed, item 6)
         * entry itself, so reclamation can never erase the fact. A ring that never pushed anything, or whose
         * newest push ended by refusal/mismatch/timeout/expiry rather than a fence, still answers
         * NOT_IN_FLIGHT — unchanged from 0.0.443.
         * 0.0.446 ( fix (1)): EXCEPT a newest push that ended by EXPIRY, which answers NOW_TIMEOUT - what
         * this function answered for that same entry while it was still live and past its bound. Names only: every
         * one of these three verdicts has n48_ksd_defer 0. */
        return r->newestRetired ? N48_KSD_NOW_EOP
             : r->newestExpired ? N48_KSD_NOW_TIMEOUT : N48_KSD_NOW_NOT_IN_FLIGHT;
    if (!now_ok || timeout_us == 0ull) return N48_KSD_NOW_TORN;   /* world-level: no age is computable for ANYTHING */

    uint32_t anyBlocking = 0u;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) {
        const uint32_t st = r->e[i].state;
        if (st != N48_FR_PENDING && st != N48_FR_COMMITTED && st != N48_FR_NOT_RUN) continue;
        if (r->e[i].at_us == 0ull) continue;                     /* torn: never blocks, never disproves a blocker */
        const uint64_t age = (now_us < r->e[i].at_us) ? 0ull : (now_us - r->e[i].at_us);   /* future stamp -> age 0 */
        if (age < timeout_us) anyBlocking = 1u;
    }
    if (blocking_seq) *blocking_seq = oldestSeq;
    if (blocking_at_us) *blocking_at_us = oldestAt;
    if (anyBlocking) return N48_KSD_DEFER;             /* wins over anyTorn unconditionally — see the comment above */
    if (anyTorn) return N48_KSD_NOW_TORN;               /* no valid blocker, and at least one entry cannot be trusted */
    return N48_KSD_NOW_TIMEOUT;
}

/* The per-state census, for the report line. Never gates anything. */
typedef struct { uint32_t free, pending, committed, notRun, retired; } n48_fr_counts;

static inline n48_fr_counts n48_fr_count(const n48_fr_ring *r)
{
    n48_fr_counts c; c.free = 0u; c.pending = 0u; c.committed = 0u; c.notRun = 0u; c.retired = 0u;
    if (!r) return c;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) {
        switch (r->e[i].state) {
        case N48_FR_FREE:      c.free++; break;
        case N48_FR_PENDING:   c.pending++; break;
        case N48_FR_COMMITTED: c.committed++; break;
        case N48_FR_NOT_RUN:   c.notRun++; break;
        case N48_FR_RETIRED:   c.retired++; break;
        default: break;
        }
    }
    return c;
}

/* =====================================================================================================================
 * build 0.0.498 ( RUN H, RUN H2) — SWITCH 65: END OF PIPE, ASKED AT THE DEFERRAL'S EXPIRY.
 * The ring's retirement test (n48_fr_poll_entry against the entry's OWN owned-slot fence) runs only at a JUDGED FRAME,
 * so a commit landing just before WindowServer's ~5 s pause was still COMMITTED when the next unmap arrived after the
 * pause: n48_fr_defer_verdict answered NOW_TIMEOUT, hook_unmapVA withdrew while the ring still showed the flight live,
 * and the continuous arm stopped (stop_why 3) although the GPU had finished (the retirement came at the next judged
 * frame, ~40 log lines later). With switch 65 ON the kext asks THIS function at that NOW_TIMEOUT, before anything is
 * decided from it: every COMMITTED, fenced entry that is past its own bound (the entries that made the verdict
 * TIMEOUT) has its owned slot read ONCE through the caller's `rd` (the same MM-window read the judged-frame poll
 * performs) and is fed to the SAME n48_fr_poll_entry - so it retires ONLY on its own exact epoch|ordinal value, in
 * ordinal order (n48_fr_next_poll), exactly as a judged frame would retire it. The caller then re-asks
 * n48_fr_defer_verdict; if nothing live remains the answer is NOW_EOP and the withdrawal that follows is the ordinary
 * end-of-pipe one (withdrawnWhileLive does not move). Anything that did not read OURS stays COMMITTED, the verdict
 * stays TIMEOUT and the withdrawal is exactly today's (fail-closed).
 *   OFF (`on` 0): returns 0 before reading anything - no `rd` call, no ring byte touched (the OFF identity).
 *   Entries still within their bound are NOT read (they cannot have made the verdict TIMEOUT; a judged frame polls them).
 *   PENDING / NOT_RUN / fence-less entries are never read and never retire (n48_fr_next_poll's own filter).
 *   `ret[0..cap)` receives each retirement (seq, ordinal, value, the entry's own at_us, and n48_fr_out_of_order asked the
 *   instant it retired); the return value is the number retired. `o` (optional) gets the per-call counts.
 * Writes only the ring's own bookkeeping (n48_fr_poll_entry's state/newest/polls/retired). The caller must serialise it
 * against the judged-frame poll (the kext holds gXdLock, try-locked, around it). */
typedef uint32_t (*n48_fr_read32_fn)(void *ctx, uint64_t vram_off, uint32_t *val);
typedef struct { uint32_t seq, ordinal, val, ooo; uint64_t at_us, vram_off; uint32_t want; } n48_fr_xret;
typedef struct { uint32_t polled, unreadable, unchanged, other, retired, in_bound; } n48_fr_xpoll;

static inline uint32_t n48_fr_expiry_poll(n48_fr_ring *r, uint32_t on, uint32_t now_ok, uint64_t now_us,
                                          uint64_t timeout_us, n48_fr_read32_fn rd, void *ctx,
                                          n48_fr_xret *ret, uint32_t cap, n48_fr_xpoll *o)
{
    if (o) { o->polled = 0u; o->unreadable = 0u; o->unchanged = 0u; o->other = 0u; o->retired = 0u; o->in_bound = 0u; }
    if (!on) return 0u;
    if (!r || !rd || !now_ok || timeout_us == 0ull) return 0u;
    uint32_t n = 0u;
    uint64_t cursor = 0ull;
    for (uint32_t i = n48_fr_next_poll(r, &cursor); i < N48_FR_CAPACITY; i = n48_fr_next_poll(r, &cursor)) {
        n48_fr_entry *e = &r->e[i];
        const uint64_t at = e->at_us;
        if (at == 0ull || now_us < at || now_us - at < timeout_us) { if (o) o->in_bound++; continue; }
        uint32_t val = 0u;
        const uint32_t got = rd(ctx, e->vram_off, &val) ? 1u : 0u;
        if (o) { o->polled++; if (!got) o->unreadable++; }
        const uint32_t seq = e->seq, ord = e->ordinal, want = e->want;
        const uint64_t off = e->vram_off;
        if (n48_fr_poll_entry(r, i, got, val)) {
            const uint32_t ooo = n48_fr_out_of_order(r, ord);
            if (ret && n < cap) {
                ret[n].seq = seq; ret[n].ordinal = ord; ret[n].val = val; ret[n].ooo = ooo;
                ret[n].at_us = at; ret[n].vram_off = off; ret[n].want = want;
            }
            n++;
        } else if (got && o) {
            if (val == 0u) o->unchanged++; else o->other++;
        }
    }
    if (o) o->retired = n;
    return n;
}

/* What the expiry check came to, from the verdict n48_fr_defer_verdict gives AFTER it (the caller re-asks only when
 * something retired; otherwise the verdict is the TIMEOUT it started from). `locked` 0 = the caller could not take its
 * lock, so nothing was read: today's withdrawal. */
enum { N48_FR_X_BUSY = 0, N48_FR_X_WITHDRAWN, N48_FR_X_CLEARED, N48_FR_X_DEFERRED, N48_FR_X_OUTCOMES };
static inline uint32_t n48_fr_expiry_outcome(uint32_t locked, uint32_t kdv_after)
{
    if (!locked) return N48_FR_X_BUSY;
    if (kdv_after == N48_KSD_DEFER) return N48_FR_X_DEFERRED;
    if (kdv_after == N48_KSD_NOW_EOP || kdv_after == N48_KSD_NOW_NOT_IN_FLIGHT || kdv_after == N48_KSD_NOW_NOTHING)
        return N48_FR_X_CLEARED;
    return N48_FR_X_WITHDRAWN;   /* TIMEOUT / TORN / OFF: fail-closed, exactly today's withdrawal */
}
static inline const char *n48_fr_expiry_outcome_name(uint32_t x)
{
    static const char *const n[N48_FR_X_OUTCOMES] = {
        "gXdLock BUSY, nothing read: WITHDRAWN AT EXPIRY as 0.0.497",
        "WITHDRAWN AT EXPIRY as 0.0.497 (not all OURS; fail-closed)",
        "RETIRED AT EXPIRY: the withdrawal is the end-of-pipe one, no stop",
        "a newer flight is in bound: DEFERRED" };
    return x < N48_FR_X_OUTCOMES ? n[x] : "?";
}
/* The per-check line (args: ctx, create #, fire #, oldest live seq, its age us, bound us; polled, retired, unchanged,
 * other value, unreadable, in bound (not read); the verdict after (N48_KSD_*, as a number), the outcome's name). */
#define N48_FR_X_LINE_FMT \
    "ksexp65: EXPIRY CHECK ctx %p (create #%u, fire #%u): verdict TIMEOUT, oldest live seq %u at %llu of %llu us. " \
    "Owned slots read %u: retired %u, unchanged %u, other %u, unreadable %u (in bound, not read %u). Verdict now %u: %s."
/* The retirement line (args: seq, ordinal, fire #, vram_off, value, want, us since the commit). */
#define N48_FR_X_RETIRED_FMT \
    "flightring: RETIRED token seq %u (ordinal %u) AT THE DEFERRAL'S EXPIRY (unmap fire #%u, switch 65) - owned slot " \
    "vram+%#llx read %#x, ours is %#x: OURS, end of pipe observed for this entry's own flight, %llu us after its commit."
/* The bare-read report line (args: ON/OFF, how, checks, busy, polled, unreadable, other, retired, cleared, withdrawn,
 * deferred, out-of-order). */
#define N48_FR_X_REPORT_FMT \
    "ksexp65: EOP at the deferral's expiry (`gfxneuter 65`) is %s%s. Checks %llu (busy %llu); read %llu (unreadable " \
    "%llu, other %llu); RETIRED AT EXPIRY %llu; cleared %llu, WITHDRAWN AT EXPIRY %llu, deferred %llu; OOO %llu."

/* =====================================================================================================================
 * THE REPORT LINE, HERE SO THE HOST TEST CAN BOUND IT (the project's own rule —'s lesson).
 * args: capacity, free, pending, committed, notRun, retired, pushed, pushRefusedFull, freedOnRefusal, polls,
 *       outOfOrder, blocking seq, blocking age (us) */
#define N48_FR_REPORT_FMT \
    "flightring: %u slot(s) - %u free, %u pending, %u committed, %u not-run, %u retired. pushed %llu, refused (ring " \
    "full) %llu, freed on keystone refusal %llu, polls %llu, OUT-OF-ORDER %llu. Oldest live: token seq %u, %llu us ago."

#ifdef __cplusplus
}
#endif
#endif /* N48_GFX_FLIGHTRING_H */
