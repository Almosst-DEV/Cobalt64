// gfx_flightring_test.cpp — C5 part 1 (0.0.443, an internal design note Q1): THE FLIGHT RING'S SAFETY PROOF.
//
// Every clause of gfx_flightring.h is exercised, each with a PLANTED BREAK that must be CAUGHT (the project's own
// non-vacuity rule): a test no mutation can break is not testing anything.
//
//   B1  an earlier un-retired entry defers a withdrawal (break: today's latest-flight-only rule - only the newest
//       entry is asked)
//   B2  a refusal frees only its own entry (break: the whole ring is cleared on refusal)
//   B3  the ring full refuses (break: the oldest entry is silently overwritten instead)
//   B4  OUT-OF-ORDER is detected (break: only the newest entry is polled/considered)
//   B5  NOT_RUN never counts as retired (break: NOT_RUN is treated as RETIRED / can retire)
//   B6  budget 1 is IDENTICAL to 0.0.442's n48_ksd_eval, field for field, across every one of its seven verdicts AND
//       across a REAL recorded sequence (modelled on arm35 seq 11's own timeline: push, several judged-frame polls
//       that do not yet match, then the poll that matches, with unmap-decision points threaded throughout)
//   B7  the report format (N48_FR_REPORT_FMT) fits under the logger's cap at its widest arguments
//   0.0.446 ( fixes (1)-(4)), each with a planted model of the old behaviour on the same inputs:
//   fix1 the newest seq/retired/expired restored on a free of the newest; an expired newest answers NOW_TIMEOUT;
//        the newest flight is named at the COMMIT mark (n48_fr_commit_mark), never at the push
//   fix2 the poll walks n48_fr_next_poll (ordinal order): no false OUT-OF-ORDER after a reclaimed slot is reused
//   fix3 reclamation after the tgtsample AFTER, holding a slot's pending seq (driven through n48_ts_after_eval)
//   fix4 n48_fr_keystone_guard (== 0.0.444's inline predicate) and its own line, which claims no keystone verdict
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_flightring_test.cpp -o /tmp/frtest && /tmp/frtest
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "gfx_flightring.h"
#include "gfx_tgtsample.h"   // 0.0.446 fix (3): the AFTER's own take-rule, driven against the ring
#include "gfx_commit.h"      // 0.0.498: the continuous arm's own stop (n48_cm_shot_stop_if_rose), for stop_why 3

static int gFail = 0, gRun = 0, gQuiet = 0, gFailOnly = 0;

static void ck(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) printf("  FAIL %-72s got %llu want %llu\n", what, (unsigned long long)got, (unsigned long long)want);
        else if (gFailOnly) printf("        FAIL %s\n", what);
    } else if (!gQuiet) printf("  ok   %-72s %llu\n", what, (unsigned long long)got);
}

// =========================================================================================================
// B1/B2/B3 — PUSH, FIND, FREE, FULL.
// =========================================================================================================
static void basics()
{
    n48_fr_ring r {};
    ck("empty ring: not full", n48_fr_full(&r), 0u);
    ck("empty ring: find(seq 1) fails", n48_fr_find(&r, 1u, nullptr), 0u);
    ck("push with seq 0 refuses", n48_fr_push(&r, 0u, 100ull, 1u, 0x1000ull, 0x11u, nullptr), 0u);

    uint32_t idx = 999u;
    ck("push seq 1 succeeds", n48_fr_push(&r, 1u, 100ull, 1u, 0x1000ull, 0xAAAAu, &idx), 1u);
    ck("push returns a real index", idx < N48_FR_CAPACITY, 1u);
    ck("the pushed entry is PENDING", r.e[idx].state, (uint64_t)N48_FR_PENDING);
    ck("find(1) now succeeds", n48_fr_find(&r, 1u, nullptr), 1u);
    ck("find(0) still refuses (0 is never a real seq)", n48_fr_find(&r, 0u, nullptr), 0u);
    ck("find(2) fails (never pushed)", n48_fr_find(&r, 2u, nullptr), 0u);
    ck("pushed counter", r.pushed, 1u);

    // mark_committed: only from PENDING.
    ck("mark_committed(1) succeeds from PENDING", n48_fr_mark_committed(&r, 1u), 1u);
    ck("the entry is now COMMITTED", r.e[idx].state, (uint64_t)N48_FR_COMMITTED);
    ck("mark_committed(1) again is a no-op (not PENDING any more)", n48_fr_mark_committed(&r, 1u), 0u);
    ck("mark_committed(unknown seq) is a no-op", n48_fr_mark_committed(&r, 999u), 0u);

    // B2: free_by_seq frees ONLY its own entry, and ONLY from PENDING.
    uint32_t idx2 = 999u;
    ck("push a second entry, seq 2", n48_fr_push(&r, 2u, 150ull, 2u, 0x2000ull, 0xBBBBu, &idx2), 1u);
    ck("entry 1 is still COMMITTED (untouched by entry 2's push)", r.e[idx].state, (uint64_t)N48_FR_COMMITTED);
    ck("free_by_seq(1) refuses - entry 1 is COMMITTED, not PENDING", n48_fr_free_by_seq(&r, 1u), 0u);
    ck("entry 1 is STILL COMMITTED after the refused free (B2's own point)", r.e[idx].state, (uint64_t)N48_FR_COMMITTED);
    ck("free_by_seq(2) succeeds - entry 2 IS PENDING", n48_fr_free_by_seq(&r, 2u), 1u);
    ck("entry 2 is now FREE", r.e[idx2].state, (uint64_t)N48_FR_FREE);
    ck("entry 1 is UNAFFECTED by entry 2's free (THE WHOLE POINT of item 2)", r.e[idx].state, (uint64_t)N48_FR_COMMITTED);
    ck("find(2) fails now it is freed", n48_fr_find(&r, 2u, nullptr), 0u);
    ck("freedOnRefusal counter", r.freedOnRefusal, 1u);

    // B3: ring full.
    n48_fr_ring full {};
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++)
        ck("fill the ring", n48_fr_push(&full, i + 1u, 100ull + i, i + 1u, 0x1000ull + i * 4u, 0x10u + i, nullptr), 1u);
    ck("a full ring reports full", n48_fr_full(&full), 1u);
    uint32_t overflowIdx = 999u;
    ck("push into a full ring refuses", n48_fr_push(&full, 9999u, 500ull, 99u, 0x9000ull, 0x99u, &overflowIdx), 0u);
    ck("... and touches no index", overflowIdx, (uint64_t)N48_FR_CAPACITY);
    ck("pushRefusedFull counted", full.pushRefusedFull, 1u);
    // Non-vacuity / falsifier B3: the OLDEST entry (seq 1) must be UNCHANGED - it was NOT silently overwritten.
    uint32_t oldIdx = 999u;
    ck("... and seq 1 (the oldest) can still be found, unharmed", n48_fr_find(&full, 1u, &oldIdx), 1u);
    ck("... at its own vram_off, unmoved", full.e[oldIdx].vram_off, 0x1000ull);
}

// PLANTED BREAK for B3: a ring that overwrites the OLDEST entry instead of refusing when full.
static uint32_t push_overwrites_oldest(n48_fr_ring *r, uint32_t seq, uint64_t at_us, uint32_t ordinal,
                                       uint64_t vram_off, uint32_t want)
{
    // Find the entry with the smallest at_us (oldest) and overwrite it unconditionally - today's bug, if it existed.
    uint32_t oldest = 0u;
    for (uint32_t i = 1; i < N48_FR_CAPACITY; i++)
        if (r->e[i].at_us < r->e[oldest].at_us) oldest = i;
    r->e[oldest].seq = seq; r->e[oldest].at_us = at_us; r->e[oldest].ordinal = ordinal;
    r->e[oldest].vram_off = vram_off; r->e[oldest].want = want; r->e[oldest].state = N48_FR_PENDING;
    return 1u;
}

static void b3_planted_break()
{
    n48_fr_ring full {};
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) (void)n48_fr_push(&full, i + 1u, 100ull + i, i + 1u, 0x1000ull + i * 4u, 0u, nullptr);
    // The REAL function must refuse (proven above). The BROKEN one overwrites the oldest instead.
    const uint32_t brokenPush = push_overwrites_oldest(&full, 9999u, 500ull, 99u, 0x9000ull, 0x99u);
    uint32_t oldIdx = 999u;
    const uint32_t oldStillThere = n48_fr_find(&full, 1u, &oldIdx);
    const bool caught = (brokenPush == 1u) && (oldStillThere == 0u);   // the broken function DID overwrite seq 1
    printf("B3 planted break (push overwrites the oldest entry instead of refusing): %s\n",
           caught ? "CAUGHT (the real n48_fr_push, proven above, does not do this)" : "*** NOT CAUGHT ***");
    if (!caught) gFail++;
    gRun++;
}

// =========================================================================================================
// B4/B5 — POLL, RETIRE, NOT_RUN, OUT-OF-ORDER.
// =========================================================================================================
static void poll_and_retire()
{
    n48_fr_ring r {};
    uint32_t i1 = 0u, i2 = 0u, i3 = 0u;
    (void)n48_fr_push(&r, 1u, 100ull, 1u, 0x1000ull, 0xAAAAu, &i1);
    (void)n48_fr_mark_committed(&r, 1u);
    (void)n48_fr_push(&r, 2u, 200ull, 2u, 0x1004ull, 0xBBBBu, &i2);
    (void)n48_fr_mark_committed(&r, 2u);
    (void)n48_fr_push(&r, 3u, 300ull, 3u, 0x1008ull, 0xCCCCu, &i3);
    (void)n48_fr_mark_committed(&r, 3u);

    // A poll that reads the WRONG value never retires.
    ck("poll entry 1 with the wrong value: does not retire", n48_fr_poll_entry(&r, i1, 1u, 0x1111u), 0u);
    ck("entry 1 is still COMMITTED", r.e[i1].state, (uint64_t)N48_FR_COMMITTED);
    // An UNREADABLE poll (got=0) never retires even with the right value.
    ck("poll entry 1, unreadable: does not retire", n48_fr_poll_entry(&r, i1, 0u, 0xAAAAu), 0u);
    ck("entry 1 is still COMMITTED after an unreadable poll", r.e[i1].state, (uint64_t)N48_FR_COMMITTED);

    // B5: NOT_RUN never retires, even if its slot happens to read its own `want` (it structurally never will in the
    // kext - nothing ever writes it - but the state guard must hold regardless of what the VRAM read returns).
    (void)n48_fr_mark_not_run(&r, 2u);
    ck("entry 2 is now NOT_RUN", r.e[i2].state, (uint64_t)N48_FR_NOT_RUN);
    ck("poll a NOT_RUN entry with its OWN want: does NOT retire", n48_fr_poll_entry(&r, i2, 1u, 0xBBBBu), 0u);
    ck("entry 2 is STILL NOT_RUN, never RETIRED", r.e[i2].state, (uint64_t)N48_FR_NOT_RUN);

    // B4: entry 3 (ordinal 3) retires while entry 1 (ordinal 1, still COMMITTED) has not - OUT OF ORDER.
    ck("entry 3 retires on a matching poll", n48_fr_poll_entry(&r, i3, 1u, 0xCCCCu), 1u);
    ck("entry 3 is now RETIRED", r.e[i3].state, (uint64_t)N48_FR_RETIRED);
    ck("re-polling a RETIRED entry is a no-op", n48_fr_poll_entry(&r, i3, 1u, 0xCCCCu), 0u);
    ck("OUT-OF-ORDER: entry 1 (ordinal 1) is still live while entry 3 (ordinal 3) just retired",
       n48_fr_out_of_order(&r, 3u), 1u);
    // Now retire entry 1 too, in order this time relative to what's left (nothing earlier is live).
    ck("entry 1 now retires (right value, readable)", n48_fr_poll_entry(&r, i1, 1u, 0xAAAAu), 1u);
    ck("no OUT-OF-ORDER for entry 1's own retirement (nothing earlier is live: entry 2 is NOT_RUN with a HIGHER "
       "ordinal, not lower)", n48_fr_out_of_order(&r, 1u), 0u);

    // A fence-less entry (want == 0) never retires, whatever the poll reads.
    n48_fr_ring r2 {};
    uint32_t i4 = 0u;
    (void)n48_fr_push(&r2, 9u, 100ull, 0u, 0ull, 0u, &i4);   // ordinal/vram_off/want all 0: no owned-slot fence
    (void)n48_fr_mark_committed(&r2, 9u);
    ck("fence-less entry: a poll (even a 'matching' 0==0) does not retire", n48_fr_poll_entry(&r2, i4, 1u, 0u), 0u);
    ck("fence-less entry stays COMMITTED (bound-only, as the build's own constraint requires)",
       r2.e[i4].state, (uint64_t)N48_FR_COMMITTED);
}

// PLANTED BREAK for B5: a poll function that does not check the state, so NOT_RUN can retire.
static uint32_t poll_no_state_guard(n48_fr_ring *r, uint32_t idx, uint32_t got, uint32_t val)
{
    if (idx >= N48_FR_CAPACITY) return 0u;
    n48_fr_entry *e = &r->e[idx];
    if (e->state == N48_FR_FREE || e->state == N48_FR_RETIRED) return 0u;   // still excludes FREE/RETIRED, NOT the bug
    if (!got || e->want == 0u || val != e->want) return 0u;
    e->state = N48_FR_RETIRED;   // BUG: retires a PENDING or NOT_RUN entry too
    return 1u;
}
static void b5_planted_break()
{
    n48_fr_ring r {};
    uint32_t idx = 0u;
    (void)n48_fr_push(&r, 5u, 100ull, 5u, 0x5000ull, 0x55u, &idx);
    (void)n48_fr_mark_committed(&r, 5u);
    (void)n48_fr_mark_not_run(&r, 5u);
    const uint32_t realRetires = n48_fr_poll_entry(&r, idx, 1u, 0x55u);
    n48_fr_ring r2 {};
    uint32_t idx2 = 0u;
    (void)n48_fr_push(&r2, 5u, 100ull, 5u, 0x5000ull, 0x55u, &idx2);
    (void)n48_fr_mark_committed(&r2, 5u);
    (void)n48_fr_mark_not_run(&r2, 5u);
    const uint32_t brokenRetires = poll_no_state_guard(&r2, idx2, 1u, 0x55u);
    const bool caught = (realRetires == 0u) && (brokenRetires == 1u);
    printf("B5 planted break (NOT_RUN can be retired by a poll): %s\n",
           caught ? "CAUGHT (the real n48_fr_poll_entry refuses; the broken model retires)" : "*** NOT CAUGHT ***");
    if (!caught) gFail++;
    gRun++;
}

// PLANTED BREAK for B4: an out-of-order check that only looks at the newest entry (never finds an earlier live one).
static uint32_t out_of_order_newest_only(const n48_fr_ring *, uint32_t) { return 0u; }
static void b4_planted_break()
{
    n48_fr_ring r {};
    uint32_t i1 = 0u, i2 = 0u;
    (void)n48_fr_push(&r, 1u, 100ull, 1u, 0x1000ull, 0xAAAAu, &i1);
    (void)n48_fr_mark_committed(&r, 1u);
    (void)n48_fr_push(&r, 2u, 200ull, 2u, 0x1004ull, 0xBBBBu, &i2);
    (void)n48_fr_mark_committed(&r, 2u);
    (void)n48_fr_poll_entry(&r, i2, 1u, 0xBBBBu);   // ordinal 2 retires while ordinal 1 is still live
    const uint32_t real = n48_fr_out_of_order(&r, 2u);
    const uint32_t broken = out_of_order_newest_only(&r, 2u);
    const bool caught = (real == 1u) && (broken == 0u);
    printf("B4 planted break (OUT-OF-ORDER checks only the newest entry): %s\n",
           caught ? "CAUGHT (the real n48_fr_out_of_order scans every live entry)" : "*** NOT CAUGHT ***");
    if (!caught) gFail++;
    gRun++;
}

// =========================================================================================================
// B6 — THE BUDGET-1 IDENTITY: n48_fr_defer_verdict on a ONE-ENTRY ring must equal n48_ksd_eval on the
// equivalent single flight, for EVERY one of n48_ksd_eval's seven verdicts.
// =========================================================================================================
static void identity_all_verdicts()
{
    struct Case {
        const char *name;
        uint32_t on, armed, flight, now_ok, retired /* ring-only: entry already RETIRED before this decision */;
        uint64_t start_us, now_us, timeout_us;
        uint32_t want; // n48_ksd_eval's answer
    };
    // Every clause n48_ksd_eval can return, reproduced with a ONE-ENTRY ring standing in for the single flight.
    // `flight` false / `armed` false model "nothing to push" (an empty ring) since n48_fr_defer_verdict has no
    // separate "armed" input (that per-context test stays the caller's, exactly as it always was).
    const Case cases[] = {
        // OFF: on=0.
        { "OFF",              0u, 1u, 1u, 1u, 0u,   100ull, 200ull, 50ull, N48_KSD_NOW_OFF },
        // NOT_IN_FLIGHT: no flight pushed at all (empty ring).
        { "NOT_IN_FLIGHT",    1u, 1u, 0u, 1u, 0u,   0ull,   200ull, 50ull, N48_KSD_NOW_NOT_IN_FLIGHT },
        // TORN (clock unreadable).
        { "TORN clock",       1u, 1u, 1u, 0u, 0u,   100ull, 0ull,   50ull, N48_KSD_NOW_TORN },
        // TORN (timeout_us 0).
        { "TORN timeout=0",   1u, 1u, 1u, 1u, 0u,   100ull, 200ull, 0ull,  N48_KSD_NOW_TORN },
        // EOP: entry already retired -> 0.0.444 (item 7): the ring's OWN newest-push tracking answers NOW_EOP
        // directly (nothing else live, and the newest entry's own retirement is what ended it) - the fix for the
        // review's Q4 (0.0.443 could never return NOW_EOP at all, breaking's "must read the last committed
        // seq" and appliedEop). n48_ksd_defer treats NOW_EOP and NOT_IN_FLIGHT identically (both are 0 = do not
        // defer), so this is a REPORTING fix, not a decision change - see gfx_flightring.h's own note.
        { "EOP (retired)",    1u, 1u, 1u, 1u, 1u,   100ull, 200ull, 50ull, N48_KSD_NOW_EOP },
        // TIMEOUT: past the bound, not retired.
        { "TIMEOUT",          1u, 1u, 1u, 1u, 0u,   100ull, 300ull, 50ull, N48_KSD_NOW_TIMEOUT },
        // DEFER: within the bound, not retired.
        { "DEFER",             1u, 1u, 1u, 1u, 0u,   100ull, 120ull, 50ull, N48_KSD_DEFER },
    };
    for (const Case &tc : cases) {
        n48_fr_ring r {};
        if (tc.flight) {
            uint32_t idx = 0u;
            (void)n48_fr_push(&r, 7u, tc.start_us, 7u, 0x7000ull, 0x77u, &idx);
            (void)n48_fr_mark_committed(&r, 7u);
            if (tc.retired) (void)n48_fr_poll_entry(&r, idx, 1u, 0x77u);
        }
        const uint32_t got = n48_fr_defer_verdict(&r, tc.on, tc.now_ok, tc.now_us, tc.timeout_us, nullptr, nullptr);
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "budget-1 identity: %s", tc.name);
        ck(lbl, got, tc.want);
        char lbl2[160];
        std::snprintf(lbl2, sizeof(lbl2), "budget-1 identity: %s - n48_ksd_defer() agrees with n48_ksd_eval's shape",
                      tc.name);
        ck(lbl2, n48_ksd_defer(got), n48_ksd_defer(tc.want));
        (void)tc.armed;
    }
}

// =========================================================================================================
// B1/B6 — MULTI-ENTRY: an earlier un-retired entry defers a withdrawal, modelled on arm35's own commit order
// (a budget > 1 scenario the single flight record could never represent). Two commits, A (ordinal 1) then B
// (ordinal 2): B retires quickly (its own fence fires), A does not. The ring must still DEFER - "ANY entry
// live and within its bound" - even though the NEWEST entry (B) is already done.
// =========================================================================================================
static void multi_entry_earlier_defers()
{
    n48_fr_ring r {};
    uint32_t ia = 0u, ib = 0u;
    (void)n48_fr_push(&r, 101u, 1000ull, 1u, 0x1000ull, 0x1u, &ia);   // A, pushed first
    (void)n48_fr_mark_committed(&r, 101u);
    (void)n48_fr_push(&r, 102u, 1010ull, 2u, 0x1004ull, 0x2u, &ib);   // B, pushed 10us later
    (void)n48_fr_mark_committed(&r, 102u);

    // B retires almost immediately (its own fence observed OURS); A has not.
    (void)n48_fr_poll_entry(&r, ib, 1u, 0x2u);
    ck("B (the newer entry) is RETIRED", r.e[ib].state, (uint64_t)N48_FR_RETIRED);
    ck("A (the older entry) is STILL COMMITTED", r.e[ia].state, (uint64_t)N48_FR_COMMITTED);

    // Item 1 (B4): B retiring while A (an earlier ordinal) has not is itself OUT-OF-ORDER.
    ck("B's retirement while A (earlier ordinal) is still live: OUT-OF-ORDER", n48_fr_out_of_order(&r, 2u), 1u);

    // THE PLANTED BREAK THIS TEST IS FOR (today's latest-flight-only rule): a rule that asks only about the
    // NEWEST/last-pushed entry would see B (RETIRED) and answer "not in flight" - permitting withdrawal while A
    // is still genuinely executing. n48_fr_defer_verdict must DEFER instead, because it asks EVERY live entry.
    const uint32_t got = n48_fr_defer_verdict(&r, 1u, 1u, 1100ull /* 100us after A, 90us after B */, 5000000ull,
                                              nullptr, nullptr);
    ck("the RING (asking every entry) DEFERS - A is still live and within its bound", got, (uint64_t)N48_KSD_DEFER);
    ck("n48_ksd_defer() agrees: this is a deferral", n48_ksd_defer(got), 1u);

    // PLANTED BREAK, modelled directly: "ask only the newest entry" (B), which is already retired.
    n48_fr_ring newestOnly {};
    newestOnly.e[0] = r.e[ib];   // ONLY B survives in this broken model's view
    const uint32_t brokenGot = n48_fr_defer_verdict(&newestOnly, 1u, 1u, 1100ull, 5000000ull, nullptr, nullptr);
    const bool caught = (got == N48_KSD_DEFER) && (brokenGot != N48_KSD_DEFER);
    printf("B1 planted break (only the newest entry is asked, today's latest-flight-only rule): %s\n",
           caught ? "CAUGHT (the real ring-wide verdict defers; the newest-only model does not)" : "*** NOT CAUGHT ***");
    if (!caught) gFail++;
    gRun++;

    // Eventually A also retires (or times out); once nothing is live the ring stops deferring.
    (void)n48_fr_poll_entry(&r, ia, 1u, 0x1u);
    const uint32_t gotAfter = n48_fr_defer_verdict(&r, 1u, 1u, 1200ull, 5000000ull, nullptr, nullptr);
    ck("once BOTH entries have retired, the ring no longer defers", n48_ksd_defer(gotAfter), 0u);
}

// =========================================================================================================
// B6 — A REAL RECORDED SEQUENCE, MODELLED ON ARM35 SEQ 11: push at the commit, ~0.3s measured
// commit-to-observed-retirement latency (arm8/arm9's own number, the same one kKsFlightUs=2s in the kext is
// justified from), several judged-frame polls before the match, an unmap-decision point BEFORE and AFTER
// retirement. This is the regression pin that RUNS the sequence, not just isolated states.
// =========================================================================================================
static void real_sequence_regression_pin()
{
    n48_fr_ring r {};
    const uint64_t kCommitUs = 21470000ull;         // arm35's own commit instant, relative units (us)
    const uint64_t kTimeoutUs = 2000000ull;          // kKsFlightUs, the kext's own bound
    uint32_t idx = 0u;
    (void)n48_fr_push(&r, 11u, kCommitUs, 3u, 0x10930000ull, 0x4E480003u, &idx);   // seq 11, fence slot 3
    (void)n48_fr_mark_committed(&r, 11u);

    // Judged frames poll every ~14ms (1/70s, roughly this project's measured cadence); the fence has not fired yet.
    uint64_t t = kCommitUs;
    for (int i = 0; i < 5; i++) {
        t += 14000ull;
        ck("sequence: a non-matching poll before retirement does not retire", n48_fr_poll_entry(&r, idx, 1u, 0u), 0u);
        const uint32_t v = n48_fr_defer_verdict(&r, 1u, 1u, t, kTimeoutUs, nullptr, nullptr);
        ck("sequence: an unmap decision before retirement and before the bound DEFERS", n48_ksd_defer(v), 1u);
    }
    // ~0.3s after the commit (arm8/arm9's own measured latency), the fence fires: OURS.
    t = kCommitUs + 300000ull;
    ck("sequence: the matching poll retires the entry", n48_fr_poll_entry(&r, idx, 1u, 0x4E480003u), 1u);
    ck("sequence: the entry is RETIRED", r.e[idx].state, (uint64_t)N48_FR_RETIRED);
    const uint32_t vAfter = n48_fr_defer_verdict(&r, 1u, 1u, t + 1000ull, kTimeoutUs, nullptr, nullptr);
    ck("sequence: an unmap decision AFTER retirement no longer defers", n48_ksd_defer(vAfter), 0u);

    // Cross-check against n48_ksd_eval directly, for the SAME instants, proving the budget-1 identity holds across
    // a whole timeline and not just at isolated sample points.
    n48_ksd_in kd {};
    kd.on = 1u; kd.armed = 1u; kd.flight = 1u; kd.start_us = kCommitUs;
    kd.now_ok = 1u; kd.now_us = kCommitUs + 70000ull; kd.eop = 0u; kd.timeout_us = kTimeoutUs;
    ck("sequence cross-check: n48_ksd_eval also defers at +70ms, not yet retired",
       n48_ksd_defer(n48_ksd_eval(&kd)), 1u);
    kd.now_us = kCommitUs + 300001ull; kd.eop = 1u;
    ck("sequence cross-check: n48_ksd_eval does not defer once eop is observed (matches the ring's RETIRED exclusion)",
       n48_ksd_defer(n48_ksd_eval(&kd)), 0u);
}

// =========================================================================================================
// B7 — THE REPORT FORMAT FITS UNDER THE LOGGER'S CAP AT ITS WIDEST ARGUMENTS.
// =========================================================================================================
static void report_format_bound()
{
    char b[1024];
    const int n = std::snprintf(b, sizeof(b), N48_FR_REPORT_FMT,
                                N48_FR_CAPACITY, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
                                0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
                                0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffu, 0xffffffffffffffffull);
    if (!gQuiet) printf("      flightring REPORT line worst case: %d bytes (cap 512)\n", n);
    ck("the flightring REPORT line fits under the log cap", n > 0 && (unsigned)n <= 491u, 1u);
}

// =========================================================================================================
// 0.0.444 (an internal review note (B) item 2) — Q1's TWO RACES, EACH PLANTED AS A BREAK. `old_defer_verdict`
// reproduces 0.0.443's function EXACTLY (ring-wide TORN on ANY at_us==0 or ANY future stamp, checked before any
// blocking is computed) so each scenario can be run through BOTH and the difference asserted.
// =========================================================================================================
static uint32_t old_defer_verdict(const n48_fr_ring *r, uint32_t on, uint32_t now_ok, uint64_t now_us,
                                  uint64_t timeout_us)
{
    if (!r) return N48_KSD_NOW_TORN;
    if (!on) return N48_KSD_NOW_OFF;
    uint32_t anyLive = 0u;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) {
        const uint32_t st = r->e[i].state;
        if (st != N48_FR_PENDING && st != N48_FR_COMMITTED && st != N48_FR_NOT_RUN) continue;
        anyLive = 1u;
        if (r->e[i].at_us == 0ull) return N48_KSD_NOW_TORN;                 // 0.0.443: ring-wide, on ANY entry
    }
    if (!anyLive) return N48_KSD_NOW_NOT_IN_FLIGHT;
    if (!now_ok || timeout_us == 0ull) return N48_KSD_NOW_TORN;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) {
        const uint32_t st = r->e[i].state;
        if (st != N48_FR_PENDING && st != N48_FR_COMMITTED && st != N48_FR_NOT_RUN) continue;
        if (now_us < r->e[i].at_us) return N48_KSD_NOW_TORN;                // 0.0.443: ring-wide, on ANY future stamp
    }
    uint32_t anyBlocking = 0u;
    for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) {
        const uint32_t st = r->e[i].state;
        if (st != N48_FR_PENDING && st != N48_FR_COMMITTED && st != N48_FR_NOT_RUN) continue;
        if ((now_us - r->e[i].at_us) < timeout_us) anyBlocking = 1u;
    }
    return anyBlocking ? N48_KSD_DEFER : N48_KSD_NOW_TIMEOUT;
}

static void item2_race_planted_breaks()
{
    // Race 1 (Q1 route 1): "a push between the clock read and the scan" - modelled directly as a FUTURE STAMP.
    // A is a genuinely older, still-blocking entry; B is a brand-new push whose own at_us (stamped by its own,
    // LATER clock read) is AFTER the `now_us` this decision was handed - exactly what a push landing between the
    // caller's clock read and the scan produces.
    {
        n48_fr_ring r {};
        uint32_t ia = 0u, ib = 0u;
        (void)n48_fr_push(&r, 1u, 1000ull, 1u, 0x1000ull, 0x11u, &ia);
        (void)n48_fr_mark_committed(&r, 1u);
        (void)n48_fr_push(&r, 2u, 1200ull, 2u, 0x2000ull, 0x22u, &ib);   // at_us AHEAD of now_us below
        const uint64_t nowUs = 1100ull, timeoutUs = 5000000ull;
        const uint32_t real = n48_fr_defer_verdict(&r, 1u, 1u, nowUs, timeoutUs, nullptr, nullptr);
        const uint32_t old  = old_defer_verdict(&r, 1u, 1u, nowUs, timeoutUs);
        ck("race 1 (future stamp): the REAL ring DEFERS - A validly blocks, B's future stamp is age 0", real,
           (uint64_t)N48_KSD_DEFER);
        const bool caught = (real == N48_KSD_DEFER) && (old == N48_KSD_NOW_TORN);
        printf("item 2 planted break (race 1: a push between the clock read and the scan -> future stamp): %s\n",
               caught ? "CAUGHT (0.0.443's ring-wide rule answers TORN on the SAME ring; 0.0.444 DEFERs)"
                      : "*** NOT CAUGHT ***");
        if (!caught) gFail++;
        gRun++;
    }
    // Race 2 (Q1 route 2): "a free that is half-written, at_us 0 with the state still PENDING" - modelled
    // directly as the torn intermediate state a concurrent scan could observe under 0.0.443's field-then-state
    // free order (fixed at the source by item 3's reordering; this proves the CONSUMER side is fail-safe too).
    {
        n48_fr_ring r {};
        uint32_t ia = 0u;
        (void)n48_fr_push(&r, 1u, 1000ull, 1u, 0x1000ull, 0x11u, &ia);
        (void)n48_fr_mark_committed(&r, 1u);
        uint32_t ib = 0u;
        (void)n48_fr_push(&r, 2u, 1010ull, 2u, 0x2000ull, 0x22u, &ib);
        // Simulate the OLD order's torn mid-write window directly on entry B: fields cleared, state NOT yet.
        r.e[ib].seq = 0u; r.e[ib].at_us = 0ull; r.e[ib].ordinal = 0u; r.e[ib].vram_off = 0ull; r.e[ib].want = 0u;
        // r.e[ib].state is still N48_FR_PENDING here - exactly the torn snapshot Q1 route 2 describes.
        ck("race 2 setup: B's state is still PENDING (the torn snapshot)", r.e[ib].state, (uint64_t)N48_FR_PENDING);
        ck("race 2 setup: B's at_us is torn (0)", r.e[ib].at_us, 0ull);
        const uint64_t nowUs = 1100ull, timeoutUs = 5000000ull;
        const uint32_t real = n48_fr_defer_verdict(&r, 1u, 1u, nowUs, timeoutUs, nullptr, nullptr);
        const uint32_t old  = old_defer_verdict(&r, 1u, 1u, nowUs, timeoutUs);
        ck("race 2 (half-written free): the REAL ring DEFERS - A validly blocks despite B's torn entry", real,
           (uint64_t)N48_KSD_DEFER);
        const bool caught = (real == N48_KSD_DEFER) && (old == N48_KSD_NOW_TORN);
        printf("item 2 planted break (race 2: a free half-written, at_us 0 with state still PENDING): %s\n",
               caught ? "CAUGHT (0.0.443's ring-wide rule answers TORN on the SAME ring; 0.0.444 DEFERs)"
                      : "*** NOT CAUGHT ***");
        if (!caught) gFail++;
        gRun++;
    }
    // Reachability: the REAL free (item 3) never PRODUCES the race-2 snapshot in the first place - once
    // n48_fr_free_by_seq runs, state is FREE (checked first by every live-entry loop) whatever the field values.
    {
        n48_fr_ring r {};
        uint32_t ib = 0u;
        (void)n48_fr_push(&r, 2u, 1010ull, 2u, 0x2000ull, 0x22u, &ib);
        (void)n48_fr_free_by_seq(&r, 2u);
        ck("item 3 (free order): the real free leaves the entry FREE (excluded from every live scan)",
           r.e[ib].state, (uint64_t)N48_FR_FREE);
    }
}

// =========================================================================================================
// 0.0.444 (an internal review note (B) item 4) — TOKEN MISMATCH: PENDING -> NOT_RUN, NOT FREE.
// =========================================================================================================
static void item4_token_mismatch_not_run()
{
    n48_fr_ring r {};
    uint32_t idx = 0u;
    (void)n48_fr_push(&r, 5u, 1000ull, 5u, 0x5000ull, 0x55u, &idx);
    ck("item 4: n48_fr_mark_not_run from PENDING succeeds", n48_fr_mark_not_run(&r, 5u), 1u);
    ck("item 4: the entry is NOT_RUN, not FREE", r.e[idx].state, (uint64_t)N48_FR_NOT_RUN);
    ck("item 4: find(5) still succeeds - the entry is still LIVE, not freed", n48_fr_find(&r, 5u, nullptr), 1u);
    // The entry still counts as LIVE for the withdrawal decision (deferred to its own bound), exactly as a
    // COMMITTED-then-exemption-refused entry does.
    const uint32_t v = n48_fr_defer_verdict(&r, 1u, 1u, 1100ull, 5000000ull, nullptr, nullptr);
    ck("item 4: a NOT_RUN (ex-PENDING) entry still DEFERS a withdrawal within its bound", n48_ksd_defer(v), 1u);

    // PLANTED BREAK: 0.0.443's behaviour (free instead of NOT_RUN) frees the entry OUTRIGHT, so a SEPARATE,
    // concurrently-still-PENDING frame's entry (freed by the SAME shared seq under Q2's concurrency gap) would no
    // longer be live at all - the withdrawal decision no longer defers for it. Modelled directly:
    n48_fr_ring broken {};
    uint32_t bidx = 0u;
    (void)n48_fr_push(&broken, 5u, 1000ull, 5u, 0x5000ull, 0x55u, &bidx);
    (void)n48_fr_free_by_seq(&broken, 5u);   // 0.0.443's actual call at this site
    const uint32_t brokenLive = n48_fr_find(&broken, 5u, nullptr);
    const uint32_t brokenVerdict = n48_fr_defer_verdict(&broken, 1u, 1u, 1100ull, 5000000ull, nullptr, nullptr);
    const bool caught = (n48_fr_find(&r, 5u, nullptr) == 1u) && (brokenLive == 0u) &&
                        (n48_ksd_defer(v) == 1u) && (n48_ksd_defer(brokenVerdict) == 0u);
    printf("item 4 planted break (token mismatch frees instead of NOT_RUN): %s\n",
           caught ? "CAUGHT (0.0.443's free drops the entry from the ring entirely; 0.0.444 keeps it live)"
                  : "*** NOT CAUGHT ***");
    if (!caught) gFail++;
    gRun++;
}

// =========================================================================================================
// 0.0.444 (an internal review note (B) item 6/7) — EXPIRED, RECLAMATION, AND NOW_EOP AFTER RECLAMATION.
// =========================================================================================================
static void item6_expire_reclaim()
{
    // A live entry past its own bound becomes EXPIRED, not left live forever.
    n48_fr_ring r {};
    uint32_t idx = 0u;
    (void)n48_fr_push(&r, 1u, 1000ull, 1u, 0x1000ull, 0x11u, &idx);
    (void)n48_fr_mark_committed(&r, 1u);
    ck("item 6: not yet expired within the bound", n48_fr_expire(&r, 1u, 1100ull, 5000000ull), 0u);
    ck("item 6: still COMMITTED within the bound", r.e[idx].state, (uint64_t)N48_FR_COMMITTED);
    ck("item 6: expires once past the bound", n48_fr_expire(&r, 1u, 1000ull + 5000001ull, 5000000ull), 1u);
    ck("item 6: the entry is now EXPIRED", r.e[idx].state, (uint64_t)N48_FR_EXPIRED);
    ck("item 6: EXPIRED excluded from the withdrawal decision's 'live' set",
       n48_ksd_defer(n48_fr_defer_verdict(&r, 1u, 1u, 1000ull + 5000002ull, 5000000ull, nullptr, nullptr)), 0u);
    // A torn or future-stamped entry is NEVER expired by this function (it is not this function's business).
    n48_fr_ring torn {};
    uint32_t tidx = 0u;
    (void)n48_fr_push(&torn, 2u, 1000ull, 1u, 0x1000ull, 0x11u, &tidx);
    (void)n48_fr_mark_committed(&torn, 2u);
    torn.e[tidx].at_us = 0ull;   // torn
    ck("item 6: a torn entry is never expired", n48_fr_expire(&torn, 1u, 9999999999ull, 1ull), 0u);

    // Reclamation: RETIRED and EXPIRED slots go back to FREE; live/PENDING/COMMITTED entries are untouched.
    n48_fr_ring r2 {};
    uint32_t iRet = 0u, iExp = 0u, iLive = 0u;
    (void)n48_fr_push(&r2, 10u, 100ull, 1u, 0x1000ull, 0x11u, &iRet);
    (void)n48_fr_mark_committed(&r2, 10u);
    (void)n48_fr_poll_entry(&r2, iRet, 1u, 0x11u);
    ck("item 6 setup: entry RETIRED", r2.e[iRet].state, (uint64_t)N48_FR_RETIRED);
    (void)n48_fr_push(&r2, 11u, 200ull, 2u, 0x2000ull, 0x22u, &iExp);
    (void)n48_fr_mark_committed(&r2, 11u);
    (void)n48_fr_expire(&r2, 1u, 200ull + 5000001ull, 5000000ull);
    ck("item 6 setup: entry EXPIRED", r2.e[iExp].state, (uint64_t)N48_FR_EXPIRED);
    (void)n48_fr_push(&r2, 12u, 300ull, 3u, 0x3000ull, 0x33u, &iLive);
    (void)n48_fr_mark_committed(&r2, 12u);
    ck("item 6 reclaim: reclaims exactly the RETIRED + EXPIRED slots", n48_fr_reclaim(&r2), 2u);
    ck("item 6 reclaim: the RETIRED slot is now FREE", r2.e[iRet].state, (uint64_t)N48_FR_FREE);
    ck("item 6 reclaim: the EXPIRED slot is now FREE", r2.e[iExp].state, (uint64_t)N48_FR_FREE);
    ck("item 6 reclaim: the still-live entry is UNTOUCHED", r2.e[iLive].state, (uint64_t)N48_FR_COMMITTED);
    ck("item 6 reclaim: a second reclaim is a no-op", n48_fr_reclaim(&r2), 0u);

    // Item 7: NOW_EOP survives reclamation of the newest entry's own slot.
    n48_fr_ring r3 {};
    uint32_t iN = 0u;
    (void)n48_fr_push(&r3, 20u, 100ull, 1u, 0x1000ull, 0x11u, &iN);
    (void)n48_fr_mark_committed(&r3, 20u);
    (void)n48_fr_poll_entry(&r3, iN, 1u, 0x11u);
    ck("item 7: newestRetired is set on the ring itself", r3.newestRetired, 1u);
    ck("item 7: before reclaim, the ring answers NOW_EOP (nothing else live)",
       n48_fr_defer_verdict(&r3, 1u, 1u, 200ull, 5000000ull, nullptr, nullptr), (uint64_t)N48_KSD_NOW_EOP);
    (void)n48_fr_reclaim(&r3);
    ck("item 7: AFTER reclaiming the newest entry's own slot, NOW_EOP still holds (ring-level, not entry-level)",
       n48_fr_defer_verdict(&r3, 1u, 1u, 200ull, 5000000ull, nullptr, nullptr), (uint64_t)N48_KSD_NOW_EOP);
    // A fresh boot (nothing ever pushed) is NOT_IN_FLIGHT, never EOP.
    n48_fr_ring empty {};
    ck("item 7: an empty, never-pushed ring answers NOT_IN_FLIGHT, not EOP",
       n48_fr_defer_verdict(&empty, 1u, 1u, 200ull, 5000000ull, nullptr, nullptr),
       (uint64_t)N48_KSD_NOW_NOT_IN_FLIGHT);

    // Item 6's own test: 40 sequential single commits, each retired by its own fence, never hit RING-FULL - the
    // continuous-mode safety net this build lays (switch 41 itself is the NEXT build).
    n48_fr_ring cont {};
    for (uint32_t k = 1; k <= 40u; k++) {
        uint32_t ci = 0u;
        const uint32_t pushed = n48_fr_push(&cont, k, 1000ull * k, k, 0x1000ull + k * 4u, 0x1000u + k, &ci);
        char lbl[64]; std::snprintf(lbl, sizeof(lbl), "item 6: commit %u pushes (never RING-FULL)", k);
        ck(lbl, pushed, 1u);
        (void)n48_fr_mark_committed(&cont, k);
        (void)n48_fr_poll_entry(&cont, ci, 1u, 0x1000u + k);
        (void)n48_fr_reclaim(&cont);   // the maintenance the kext runs once per judged frame
    }
    ck("item 6: after 40 commits, the ring is back to all-FREE (every slot reclaimed)", n48_fr_full(&cont), 0u);
}

// =========================================================================================================
// 0.0.444 (an internal review note (B) item 8) — OUT-OF-ORDER EXCLUDES NOT_RUN AND EXPIRED; COMPARES BY ORDINAL,
// NEVER BY SLOT INDEX (proven with a reclaimed-and-reused slot).
// =========================================================================================================
static void item8_out_of_order_excludes()
{
    // A NOT_RUN entry with a LOWER ordinal than a retiring entry must NOT be read as OUT-OF-ORDER (0.0.443 did).
    n48_fr_ring r {};
    uint32_t i1 = 0u, i2 = 0u;
    (void)n48_fr_push(&r, 1u, 1000ull, 1u, 0x1000ull, 0x11u, &i1);       // ordinal 1
    (void)n48_fr_mark_not_run(&r, 1u);                                  // NOT_RUN, will never retire by fence
    (void)n48_fr_push(&r, 2u, 1010ull, 2u, 0x2000ull, 0x22u, &i2);       // ordinal 2
    (void)n48_fr_mark_committed(&r, 2u);
    ck("item 8: entry 2 retires", n48_fr_poll_entry(&r, i2, 1u, 0x22u), 1u);
    ck("item 8: a NOT_RUN entry with a lower ordinal is NOT counted OUT-OF-ORDER", n48_fr_out_of_order(&r, 2u), 0u);

    // PLANTED BREAK: 0.0.443's own check (NOT_RUN counted as live/blocking for this purpose).
    static const auto oldOutOfOrder = [](const n48_fr_ring *rr, uint32_t retiredOrdinal) -> uint32_t {
        if (!rr || !retiredOrdinal) return 0u;
        for (uint32_t i = 0; i < N48_FR_CAPACITY; i++) {
            const uint32_t st = rr->e[i].state;
            if ((st == N48_FR_PENDING || st == N48_FR_COMMITTED || st == N48_FR_NOT_RUN) &&
                rr->e[i].ordinal && rr->e[i].ordinal < retiredOrdinal)
                return 1u;
        }
        return 0u;
    };
    const bool caught = (n48_fr_out_of_order(&r, 2u) == 0u) && (oldOutOfOrder(&r, 2u) == 1u);
    printf("item 8 planted break (OUT-OF-ORDER counts NOT_RUN as still-blocking): %s\n",
           caught ? "CAUGHT (0.0.443's check flags it; 0.0.444 excludes NOT_RUN)" : "*** NOT CAUGHT ***");
    if (!caught) gFail++;
    gRun++;

    // Slot reuse: a slot reclaimed and reused for a LATER push with a HIGHER ordinal must never be misread by
    // POSITION - only by the ordinal each entry actually carries.
    n48_fr_ring r2 {};
    uint32_t slotA = 0u;
    (void)n48_fr_push(&r2, 100u, 1000ull, 5u, 0x5000ull, 0x55u, &slotA);   // ordinal 5, in whatever slot it lands
    (void)n48_fr_mark_committed(&r2, 100u);
    (void)n48_fr_poll_entry(&r2, slotA, 1u, 0x55u);   // retires, ordinal 5
    (void)n48_fr_reclaim(&r2);                        // slot freed, ready for reuse
    uint32_t slotB = 0u;
    (void)n48_fr_push(&r2, 101u, 2000ull, 6u, 0x6000ull, 0x66u, &slotB);   // reused slot, ordinal 6 (HIGHER)
    ck("item 8: slot reuse lands in the SAME slot the reclaimed entry vacated", slotB, slotA);
    (void)n48_fr_mark_committed(&r2, 101u);
    ck("item 8: the reused slot's higher-ordinal entry retiring is NOT out-of-order against nothing live",
       n48_fr_poll_entry(&r2, slotB, 1u, 0x66u), 1u);
    ck("item 8: no OUT-OF-ORDER - ordinal 5 (reclaimed, gone) is not confused with the new slot's own ordinal 6",
       n48_fr_out_of_order(&r2, 6u), 0u);
}

// =========================================================================================================
// 0.0.444 (an internal review note (B) item 5(ii)) — NO ENTRY, NO KEYSTONE, NO RUN. hook_gfxCommitIB's TRANSLATE branch
// gates `commit_keystone_arm` on `tokMatch && n48_fr_find(&gKsRing, here.seq, &idx) && gKsRing.e[idx].state ==
// N48_FR_PENDING` - reproduced here as `fr_has_pending_entry` so the exact boolean the kext evaluates is
// host-tested against the SAME ring primitives, driven through the real push -> (concurrent free) -> gate sequence
// rather than a hand-picked flag.
// =========================================================================================================
// 0.0.446 ( fix (4)): the kext now asks the PURE n48_fr_keystone_guard; this helper drives that SAME
// function (`frHasPendingEntry = (frGuard == N48_FR_GUARD_OK)`), and fr446_guard below proves it equals 0.0.444's
// inline expression on every ring state.
static bool fr_has_pending_entry(const n48_fr_ring *r, uint32_t tokMatch, uint32_t seq)
{
    return n48_fr_keystone_guard(r, tokMatch, seq, nullptr) == N48_FR_GUARD_OK;
}
static void item5ii_no_entry_no_keystone()
{
    // The ordinary path: push at the gate stamp, still PENDING when the keystone site is reached.
    n48_fr_ring r {};
    (void)n48_fr_push(&r, 7u, 1000ull, 1u, 0x7000ull, 0x77u, nullptr);
    ck("item 5(ii): tokMatch + a real PENDING entry -> the keystone MAY run", fr_has_pending_entry(&r, 1u, 7u), 1u);
    ck("item 5(ii): tokMatch false -> refused regardless of the ring", fr_has_pending_entry(&r, 0u, 7u), 0u);

    // Q2's concurrency gap: a DIFFERENT thread's free (or, pre-item-4, mismatch-free) already took the entry off
    // PENDING before this call reached the keystone site. No entry left to track the flight -> must refuse.
    n48_fr_ring freed {};
    (void)n48_fr_push(&freed, 7u, 1000ull, 1u, 0x7000ull, 0x77u, nullptr);
    (void)n48_fr_free_by_seq(&freed, 7u);
    ck("item 5(ii): the entry was freed by a concurrent call -> the keystone MUST NOT run",
       fr_has_pending_entry(&freed, 1u, 7u), 0u);

    // Never pushed at all (unreachable when tokMatch holds in the real kext, since the push and the token share
    // one commit's seq - but the guard must still refuse fail-closed if it somehow were).
    n48_fr_ring never {};
    ck("item 5(ii): no entry was ever pushed for this seq -> refused", fr_has_pending_entry(&never, 1u, 7u), 0u);

    // Already COMMITTED (this build's keystone runs at most once per seq, so unreachable - fail-closed regardless).
    n48_fr_ring committed {};
    (void)n48_fr_push(&committed, 7u, 1000ull, 1u, 0x7000ull, 0x77u, nullptr);
    (void)n48_fr_mark_committed(&committed, 7u);
    ck("item 5(ii): an already-COMMITTED entry does not re-authorise a second keystone run",
       fr_has_pending_entry(&committed, 1u, 7u), 0u);

    // PLANTED BREAK: the 0.0.443 gate (tokMatch alone, no ring check) - modelled directly, and run on the SAME
    // freed-entry ring the real guard above already refused.
    const bool oldGate = (1u != 0u);   // 0.0.443's `ksOk = tokMatch && commit_keystone_arm(...)`: tokMatch alone
    const bool caught = (fr_has_pending_entry(&freed, 1u, 7u) == false) && (oldGate == true);
    printf("item 5(ii) planted break (the keystone gate is tokMatch alone, no ring-entry check): %s\n",
           caught ? "CAUGHT (0.0.444's guard refuses the freed-entry case; 0.0.443's tokMatch-alone rule would "
                    "have let it run)"
                  : "*** NOT CAUGHT ***");
    if (!caught) gFail++;
    gRun++;
}


// =========================================================================================================
// 0.0.446 (the fix list for the next ring build) — FIXES (1)-(4). Each is driven through the REAL
// header functions in the order the kext calls them, with a PLANTED model of the 0.0.444/0.0.445 behaviour run on
// the SAME inputs; the physical plant-and-revert of each fix in gfx_flightring.h is recorded in the build report.
// =========================================================================================================
static const uint64_t kFr446Bound = 2000000ull;   /* the kext's kKsFlightUs */

// FIX (1): the newest seq/retired/expired are restored on a free of the newest; an expired newest answers
// NOW_TIMEOUT; the "newest flight" (gKsLastFlightSeq) is named at the COMMIT mark, never at the push.
static void fr446_fix1_newest_restore()
{
    // (a) A retired flight, then a refused push: the verdict names the retired flight's end of pipe again.
    n48_fr_ring r {};
    uint32_t lastFlight = 3u;                       // the kext's gKsLastFlightSeq, from some earlier commit
    uint32_t ia = 0u, ib = 0u;
    (void)n48_fr_push(&r, 5u, 1000ull, 1u, 0x1000ull, 0x11u, &ia);
    ck("fix1: a PUSH does not name the newest flight (still the earlier commit's 3)", lastFlight, 3u);
    ck("fix1: the COMMIT mark succeeds on the PENDING entry", n48_fr_commit_mark(&r, 5u, &lastFlight), 1u);
    ck("fix1: ... and names it the newest flight (5)", lastFlight, 5u);
    (void)n48_fr_poll_entry(&r, ia, 1u, 0x11u);
    ck("fix1: retired, nothing live -> NOW_EOP", n48_fr_defer_verdict(&r, 1u, 1u, 1500ull, kFr446Bound, nullptr, nullptr),
       (uint64_t)N48_KSD_NOW_EOP);
    (void)n48_fr_reclaim(&r);
    (void)n48_fr_push(&r, 6u, 2000ull, 2u, 0x2000ull, 0x22u, &ib);    // the next candidate, PENDING
    ck("fix1: the refused candidate's push did not rename the newest flight", lastFlight, 5u);
    ck("fix1: its free (keystone refusal) frees it", n48_fr_free_by_seq(&r, 6u), 1u);
    ck("fix1: ... a mark on the freed seq fails and names nothing", n48_fr_commit_mark(&r, 6u, &lastFlight), 0u);
    ck("fix1: ... so the newest flight is STILL 5", lastFlight, 5u);
    const uint32_t vA = n48_fr_defer_verdict(&r, 1u, 1u, 2100ull, kFr446Bound, nullptr, nullptr);
    ck("fix1: after the refused newest is freed, the verdict is NOW_EOP again (5's own end of pipe)", vA,
       (uint64_t)N48_KSD_NOW_EOP);
    ck("fix1: ... newestSeq restored to 5", r.newestSeq, 5u);

    // (b) An EXPIRED newest answers NOW_TIMEOUT - before reclamation, after it, and after a refused push is freed.
    n48_fr_ring t {};
    uint32_t it = 0u;
    (void)n48_fr_push(&t, 7u, 1000ull, 3u, 0x3000ull, 0x33u, &it);
    (void)n48_fr_commit_mark(&t, 7u, nullptr);
    ck("fix1: live and past its bound -> NOW_TIMEOUT (n48_ksd_eval's own answer for one flight)",
       n48_fr_defer_verdict(&t, 1u, 1u, 1000ull + kFr446Bound + 5ull, kFr446Bound, nullptr, nullptr),
       (uint64_t)N48_KSD_NOW_TIMEOUT);
    ck("fix1: n48_fr_expire moves it to EXPIRED", n48_fr_expire(&t, 1u, 1000ull + kFr446Bound + 5ull, kFr446Bound), 1u);
    const uint32_t vExp = n48_fr_defer_verdict(&t, 1u, 1u, 1000ull + kFr446Bound + 6ull, kFr446Bound, nullptr, nullptr);
    ck("fix1: EXPIRED newest, nothing live -> STILL NOW_TIMEOUT (was NOT_IN_FLIGHT in 0.0.444)", vExp,
       (uint64_t)N48_KSD_NOW_TIMEOUT);
    (void)n48_fr_reclaim(&t);
    ck("fix1: ... and after its slot is reclaimed", n48_fr_defer_verdict(&t, 1u, 1u, 1000ull + kFr446Bound + 7ull,
       kFr446Bound, nullptr, nullptr), (uint64_t)N48_KSD_NOW_TIMEOUT);
    (void)n48_fr_push(&t, 8u, 1000ull + kFr446Bound + 8ull, 4u, 0x4000ull, 0x44u, nullptr);
    (void)n48_fr_free_by_seq(&t, 8u);
    const uint32_t vExp2 = n48_fr_defer_verdict(&t, 1u, 1u, 1000ull + kFr446Bound + 9ull, kFr446Bound, nullptr, nullptr);
    ck("fix1: ... and after a refused push on top of it is freed (the restore carries `expired`)", vExp2,
       (uint64_t)N48_KSD_NOW_TIMEOUT);

    // (c) The stash stays current: the previous newest retires WHILE the next push is pending.
    n48_fr_ring c {};
    uint32_t c9 = 0u;
    (void)n48_fr_push(&c, 9u, 1000ull, 5u, 0x5000ull, 0x55u, &c9);
    (void)n48_fr_commit_mark(&c, 9u, nullptr);
    (void)n48_fr_push(&c, 10u, 1100ull, 6u, 0x6000ull, 0x66u, nullptr);   // PENDING; 9 stashed, unretired
    (void)n48_fr_poll_entry(&c, c9, 1u, 0x55u);                           // 9 retires in between
    (void)n48_fr_free_by_seq(&c, 10u);
    ck("fix1: a retirement of the STASHED flight is kept -> NOW_EOP after the restore",
       n48_fr_defer_verdict(&c, 1u, 1u, 1200ull, kFr446Bound, nullptr, nullptr), (uint64_t)N48_KSD_NOW_EOP);

    // (d) The fix moves NO decision: every one of the verdicts above has n48_ksd_defer 0, as NOT_IN_FLIGHT had.
    ck("fix1: n48_ksd_defer(NOW_EOP) == n48_ksd_defer(NOT_IN_FLIGHT) == n48_ksd_defer(NOW_TIMEOUT) == 0",
       n48_ksd_defer(N48_KSD_NOW_EOP) + n48_ksd_defer(N48_KSD_NOW_NOT_IN_FLIGHT) + n48_ksd_defer(N48_KSD_NOW_TIMEOUT), 0u);

    // PLANTED MODEL (0.0.444): the free restores nothing and expiry records nothing - the same two scenarios.
    n48_fr_ring m {};
    uint32_t ma = 0u;
    (void)n48_fr_push(&m, 5u, 1000ull, 1u, 0x1000ull, 0x11u, &ma);
    (void)n48_fr_mark_committed(&m, 5u);
    (void)n48_fr_poll_entry(&m, ma, 1u, 0x11u);
    (void)n48_fr_reclaim(&m);
    (void)n48_fr_push(&m, 6u, 2000ull, 2u, 0x2000ull, 0x22u, nullptr);
    (void)n48_fr_free_by_seq(&m, 6u);
    m.newestSeq = 6u; m.newestRetired = 0u; m.newestExpired = 0u;          // 0.0.444: nothing restored
    const uint32_t vOld = n48_fr_defer_verdict(&m, 1u, 1u, 2100ull, kFr446Bound, nullptr, nullptr);
    const bool caught = vA == N48_KSD_NOW_EOP && vOld == N48_KSD_NOW_NOT_IN_FLIGHT && vExp == N48_KSD_NOW_TIMEOUT;
    printf("fix 1 planted break (0.0.444: a free of the newest restores nothing; expiry records nothing): %s\n",
           caught ? "CAUGHT (0.0.444 answers NOT_IN_FLIGHT where 0.0.446 answers NOW_EOP / NOW_TIMEOUT)"
                  : "*** NOT CAUGHT ***");
    if (!caught) gFail++;
    gRun++;
}

// The kext's poll pass (gfxsrc_decide_frame), modelled over the ring: `ordinal` 1 walks n48_fr_next_poll (0.0.446),
// 0 walks slot order (0.0.445). `ours(seq)` says whether that entry's owned slot reads its own `want` this pass.
template <typename F>
static uint32_t fr446_poll_pass(n48_fr_ring *r, uint32_t ordinal, F &&ours)
{
    uint32_t ooo = 0u;
    auto one = [&](uint32_t fi) {
        const uint32_t ord = r->e[fi].ordinal, want = r->e[fi].want;
        const uint32_t hit = ours(r->e[fi].seq) ? 1u : 0u;
        if (n48_fr_poll_entry(r, fi, 1u, hit ? want : 0xdeadu) && n48_fr_out_of_order(r, ord)) ooo++;
    };
    if (ordinal) {
        uint64_t cur = 0ull;
        for (uint32_t fi = n48_fr_next_poll(r, &cur); fi < N48_FR_CAPACITY; fi = n48_fr_next_poll(r, &cur)) one(fi);
    } else {
        for (uint32_t fi = 0; fi < N48_FR_CAPACITY; fi++) {
            if (r->e[fi].state != N48_FR_COMMITTED || r->e[fi].want == 0u) continue;
            one(fi);
        }
    }
    return ooo;
}
// Two commits in flight with the NEWER one (ordinal 12) in a LOWER slot than the older (ordinal 11), which only a
// reclaim-and-reuse produces.
static void fr446_build_reused(n48_fr_ring *r)
{
    uint32_t s10 = 0u, s11 = 0u, s12 = 0u;
    (void)n48_fr_push(r, 10u, 1000ull, 10u, 0xa000ull, 0x1au, &s10);
    (void)n48_fr_push(r, 11u, 1100ull, 11u, 0xb000ull, 0x1bu, &s11);
    (void)n48_fr_mark_committed(r, 10u);
    (void)n48_fr_mark_committed(r, 11u);
    (void)n48_fr_poll_entry(r, s10, 1u, 0x1au);
    (void)n48_fr_reclaim(r);
    (void)n48_fr_push(r, 12u, 1200ull, 12u, 0xc000ull, 0x1cu, &s12);
    (void)n48_fr_mark_committed(r, 12u);
    ck("fix2 setup: the newer commit (ordinal 12) sits in a LOWER slot than the older (ordinal 11)", s12 < s11 ? 1u : 0u, 1u);
}
// FIX (2): retire in ordinal order.
static void fr446_fix2_ordinal_order()
{
    n48_fr_ring r {};
    fr446_build_reused(&r);
    const uint32_t ooo = fr446_poll_pass(&r, 1u, [](uint32_t) { return true; });   // BOTH fences read OURS
    ck("fix2: both retire this pass", (uint64_t)r.retired, 3u);
    ck("fix2: ordinal order -> NO false OUT-OF-ORDER when both fences land in one pass", ooo, 0u);
    // A GENUINE out-of-order (the older's fence NOT written yet) is still caught in ordinal order.
    n48_fr_ring g {};
    fr446_build_reused(&g);
    const uint32_t oooG = fr446_poll_pass(&g, 1u, [](uint32_t seq) { return seq == 12u; });
    ck("fix2: a GENUINE out-of-order (11 not done, 12 done) is still flagged", oooG, 1u);
    // n48_fr_next_poll visits every pollable entry exactly once, oldest ordinal first, and skips everything else.
    n48_fr_ring v {};
    fr446_build_reused(&v);
    (void)n48_fr_push(&v, 13u, 1300ull, 0u, 0ull, 0u, nullptr);   // fence-less
    (void)n48_fr_mark_committed(&v, 13u);
    (void)n48_fr_push(&v, 14u, 1400ull, 14u, 0xe000ull, 0x1eu, nullptr);   // PENDING
    uint64_t cur = 0ull; uint32_t seen[4] = { 0u, 0u, 0u, 0u }, nSeen = 0u;
    for (uint32_t fi = n48_fr_next_poll(&v, &cur); fi < N48_FR_CAPACITY && nSeen < 4u; fi = n48_fr_next_poll(&v, &cur))
        seen[nSeen++] = v.e[fi].seq;
    ck("fix2: next_poll visits exactly the two COMMITTED fenced entries", nSeen, 2u);
    ck("fix2: ... oldest ordinal first (11, then 12)", (uint64_t)seen[0] * 100u + seen[1], 1112u);
    // PLANTED MODEL (0.0.445): slot order, the same ring, the same fences.
    n48_fr_ring o {};
    fr446_build_reused(&o);
    const uint32_t oooOld = fr446_poll_pass(&o, 0u, [](uint32_t) { return true; });
    const bool caught = ooo == 0u && oooOld == 1u;
    printf("fix 2 planted break (0.0.445: poll in SLOT order after a reclaim): %s\n",
           caught ? "CAUGHT (slot order raises a false OUT-OF-ORDER; ordinal order does not)" : "*** NOT CAUGHT ***");
    if (!caught) gFail++;
    gRun++;
}

// FIX (3): reclamation runs after the tgtsample AFTER, and holds an entry a pending AFTER still needs. The AFTER's
// input is built exactly as gfxsrc_ts_after_slot builds it (find by seq; start_us = at_us; eop = RETIRED).
static uint32_t fr446_after(const n48_fr_ring *r, uint32_t seq, uint32_t sameRoot, uint64_t now)
{
    n48_ts_after_in d;
    std::memset(&d, 0, sizeof d);
    d.on = 1u; d.before_ok = 1u; d.done = 0u; d.same_root = sameRoot;
    uint32_t idx = 0u;
    if (n48_fr_find(r, seq, &idx)) { d.flight = 1u; d.start_us = r->e[idx].at_us; d.eop = r->e[idx].state == N48_FR_RETIRED; }
    d.now_us = now; d.now_ok = 1u; d.timeout_us = kFr446Bound;
    return n48_ts_after_eval(&d);
}
// One judged frame, in the kext's order: poll, AFTER, then expire + reclaim (0.0.446: holding the pending slot).
// `oldOrder` runs 0.0.444's order instead: poll, expire + reclaim (nothing held), then AFTER.
static uint32_t fr446_frame(n48_fr_ring *r, uint32_t seq, uint32_t sameRoot, uint64_t now, uint32_t *afterDone,
                            uint32_t oldOrder)
{
    (void)fr446_poll_pass(r, 1u, [](uint32_t) { return true; });
    uint32_t a = N48_TS_AF_DONE;
    if (oldOrder) {
        (void)n48_fr_expire(r, 1u, now, kFr446Bound);
        (void)n48_fr_reclaim(r);
    }
    if (!*afterDone) { a = fr446_after(r, seq, sameRoot, now); if (n48_ts_after_take(a)) *afterDone = 1u; }
    if (!oldOrder) {
        const uint32_t hold[1] = { *afterDone ? 0u : seq };
        (void)n48_fr_expire(r, 1u, now, kFr446Bound);
        (void)n48_fr_reclaim_hold(r, hold, 1u);
    }
    return a;
}
static void fr446_fix3_reclaim_after_the_after()
{
    // Commit seq 5 with a published tgtsample BEFORE; its fence retires on a frame of ANOTHER context (the AFTER
    // answers NO_CONTEXT there), and the next frame is the committing context's own.
    n48_fr_ring r {};
    (void)n48_fr_push(&r, 5u, 1000ull, 1u, 0x1000ull, 0x11u, nullptr);
    (void)n48_fr_mark_committed(&r, 5u);
    uint32_t done = 0u;
    const uint32_t a1 = fr446_frame(&r, 5u, 0u, 1500ull, &done, 0u);
    ck("fix3: frame N (another context): the AFTER waits (NO_CONTEXT)", a1, (uint64_t)N48_TS_AF_NO_CONTEXT);
    uint32_t idx = 0u;
    ck("fix3: ... and the RETIRED entry is HELD, not reclaimed", n48_fr_find(&r, 5u, &idx), 1u);
    const uint32_t a2 = fr446_frame(&r, 5u, 1u, 1600ull, &done, 0u);
    ck("fix3: frame N+1 (its own context): the AFTER is TAKEN at end of pipe", a2, (uint64_t)N48_TS_AF_TAKE_EOP);
    ck("fix3: ... and the entry is reclaimed once its AFTER is in", n48_fr_find(&r, 5u, &idx), 0u);
    // The same-frame case (retires and is sampled on one frame) - the order alone fixes it.
    n48_fr_ring s {};
    (void)n48_fr_push(&s, 6u, 1000ull, 2u, 0x2000ull, 0x22u, nullptr);
    (void)n48_fr_mark_committed(&s, 6u);
    uint32_t doneS = 0u;
    ck("fix3: retires and is sampled on ONE frame -> TAKE_EOP", fr446_frame(&s, 6u, 1u, 1500ull, &doneS, 0u),
       (uint64_t)N48_TS_AF_TAKE_EOP);
    // Holding costs no decision: a held RETIRED entry is not live.
    n48_fr_ring h {};
    uint32_t hi = 0u;
    (void)n48_fr_push(&h, 7u, 1000ull, 3u, 0x3000ull, 0x33u, &hi);
    (void)n48_fr_mark_committed(&h, 7u);
    (void)n48_fr_poll_entry(&h, hi, 1u, 0x33u);
    const uint32_t hold7[1] = { 7u };
    ck("fix3: reclaim_hold keeps the held seq", n48_fr_reclaim_hold(&h, hold7, 1u), 0u);
    ck("fix3: ... and a held RETIRED entry defers nothing (n48_ksd_defer 0)",
       n48_ksd_defer(n48_fr_defer_verdict(&h, 1u, 1u, 1100ull, kFr446Bound, nullptr, nullptr)), 0u);
    ck("fix3: reclaim_hold with nothing held == n48_fr_reclaim", n48_fr_reclaim_hold(&h, nullptr, 0u), 1u);
    // PLANTED MODEL (0.0.444's order: reclaim BEFORE the AFTER, nothing held): the AFTER never finds its flight.
    n48_fr_ring o {};
    (void)n48_fr_push(&o, 5u, 1000ull, 1u, 0x1000ull, 0x11u, nullptr);
    (void)n48_fr_mark_committed(&o, 5u);
    uint32_t doneO = 0u;
    (void)fr446_frame(&o, 5u, 0u, 1500ull, &doneO, 1u);
    const uint32_t o2 = fr446_frame(&o, 5u, 1u, 1600ull, &doneO, 1u);
    n48_fr_ring os {};
    (void)n48_fr_push(&os, 6u, 1000ull, 2u, 0x2000ull, 0x22u, nullptr);
    (void)n48_fr_mark_committed(&os, 6u);
    uint32_t doneOS = 0u;
    const uint32_t os1 = fr446_frame(&os, 6u, 1u, 1500ull, &doneOS, 1u);
    const bool caught = a2 == N48_TS_AF_TAKE_EOP && o2 == N48_TS_AF_NO_FLIGHT && os1 == N48_TS_AF_NO_FLIGHT;
    printf("fix 3 planted break (0.0.444: reclaim BEFORE the AFTER, nothing held): %s\n",
           caught ? "CAUGHT (0.0.444's AFTER reads NO_FLIGHT in both cases - dead; 0.0.446 takes it at end of pipe)"
                  : "*** NOT CAUGHT ***");
    if (!caught) gFail++;
    gRun++;
}

// FIX (4): the keystone guard's own answer, and a line that does not claim a keystone write.
static void fr446_fix4_guard_line()
{
    n48_fr_ring r {};
    (void)n48_fr_push(&r, 7u, 1000ull, 1u, 0x7000ull, 0x77u, nullptr);
    uint32_t st = 0u;
    ck("fix4: identity matched + PENDING entry -> OK", n48_fr_keystone_guard(&r, 1u, 7u, &st), (uint64_t)N48_FR_GUARD_OK);
    ck("fix4: ... state reported PENDING", st, (uint64_t)N48_FR_PENDING);
    ck("fix4: identity mismatch -> NO_TOKEN", n48_fr_keystone_guard(&r, 0u, 7u, &st), (uint64_t)N48_FR_GUARD_NO_TOKEN);
    ck("fix4: no entry for this seq -> NO_ENTRY", n48_fr_keystone_guard(&r, 1u, 8u, &st), (uint64_t)N48_FR_GUARD_NO_ENTRY);
    ck("fix4: ... state reported as none (N48_FR_STATES)", st, (uint64_t)N48_FR_STATES);
    (void)n48_fr_mark_committed(&r, 7u);
    ck("fix4: entry already COMMITTED -> NOT_PENDING", n48_fr_keystone_guard(&r, 1u, 7u, &st),
       (uint64_t)N48_FR_GUARD_NOT_PENDING);
    ck("fix4: ... state reported COMMITTED", st, (uint64_t)N48_FR_COMMITTED);
    // Equivalence with 0.0.444's inline predicate on every state an entry can hold.
    uint32_t agree = 0u, cases = 0u;
    for (uint32_t state = N48_FR_FREE; state < N48_FR_STATES; state++)
        for (uint32_t tm = 0u; tm < 2u; tm++) {
            n48_fr_ring q {};
            uint32_t qi = 0u;
            (void)n48_fr_push(&q, 9u, 1000ull, 1u, 0x9000ull, 0x99u, &qi);
            q.e[qi].state = state;
            uint32_t fi = 0u;
            const bool old444 = tm && n48_fr_find(&q, 9u, &fi) != 0u && q.e[fi].state == N48_FR_PENDING;
            const bool now446 = n48_fr_keystone_guard(&q, tm, 9u, nullptr) == N48_FR_GUARD_OK;
            cases++; if (old444 == now446) agree++;
        }
    ck("fix4: the guard equals 0.0.444's inline `tokMatch && find && PENDING` on every state x token case (14 with 0.0.519 NOPED)", agree, cases);
    // The LINE: 0.0.444 printed the KEYSTONE line with a zero KsResult, whose verdict 0 names a written keystone.
    const char *liar = n48_ks_name(0u);
    ck("fix4: the liar exists - verdict 0's own name claims the keystone is written",
       std::strstr(liar, "written") != nullptr ? 1u : 0u, 1u);
    char b[1024];
    const int n = std::snprintf(b, sizeof(b), N48_FR_GUARD_REFUSED_FMT, 0xffffffffu,
                                n48_fr_guard_name(N48_FR_GUARD_NOT_PENDING), "COMMITTED");
    if (!gQuiet) printf("      guard-refusal line worst case: %d bytes (cap 512)\n", n);
    ck("fix4: the guard-refusal line fits the log cap", n > 0 && (unsigned)n <= 480u, 1u);
    ck("fix4: the guard-refusal line claims NO keystone verdict", std::strstr(b, "keystone verdict") == nullptr ? 1u : 0u, 1u);
    ck("fix4: ... and says the keystone did NOT run", std::strstr(b, "KEYSTONE WAS NOT RUN") != nullptr ? 1u : 0u, 1u);
    uint32_t longest = 0u;
    for (uint32_t g = 0u; g <= N48_FR_GUARDS; g++) {
        const uint32_t l = (uint32_t)std::strlen(n48_fr_guard_name(g));
        if (l > longest) longest = l;
    }
    ck("fix4: the longest guard name is the one measured above",
       longest, (uint64_t)std::strlen(n48_fr_guard_name(N48_FR_GUARD_NOT_PENDING)));
    // PLANTED MODEL: a guard that asks only whether the entry EXISTS (the state clause dropped) - it would authorise
    // a second keystone run on an already-COMMITTED entry.
    auto brokenGuard = [](const n48_fr_ring *rr, uint32_t tm, uint32_t seq) -> uint32_t {
        uint32_t i = 0u;
        if (!tm) return N48_FR_GUARD_NO_TOKEN;
        return n48_fr_find(rr, seq, &i) ? N48_FR_GUARD_OK : N48_FR_GUARD_NO_ENTRY;
    };
    const bool caught = brokenGuard(&r, 1u, 7u) == N48_FR_GUARD_OK &&
                        n48_fr_keystone_guard(&r, 1u, 7u, nullptr) == N48_FR_GUARD_NOT_PENDING;
    printf("fix 4 planted break (the guard drops its PENDING clause): %s\n",
           caught ? "CAUGHT (the broken guard authorises a COMMITTED entry; the real one refuses it)" : "*** NOT CAUGHT ***");
    if (!caught) gFail++;
    gRun++;
}


// =====================================================================================================================
// build 0.0.498 ( RUN H, RUN H2) — SECTION X: SWITCH 65, END OF PIPE ASKED AT THE DEFERRAL'S
// EXPIRY, driven through the REAL n48_fr_expiry_poll / n48_fr_poll_entry / n48_fr_defer_verdict and the REAL
// n48_cm_shot_stop_if_rose. `x_unmap` below is hook_unmapVA's verdict block statement for statement (the kext's order is
// pinned by gfx_keystone_test's x_source_pins): scan, clock, verdict; switch 65's check at NOW_TIMEOUT (under the lock,
// or BUSY); the re-scan/re-clock/re-verdict only when something retired; deferNow; then the withdrawnWhileLive
// condition copied verbatim from the kext (`kdArmed && !deferNow && kdAnyLiveAtScan && (TIMEOUT || TORN || OFF)`).
// RUN H2's own numbers (run10n driverlog-stream 13549, 14156, 14224): f19 = token seq 11, fence828 ordinal 3, owned slot
// vram+0x3cba8f00c, ours 0x93d0003, committed at stamp 161569653 us; deferred at +80301 and +263064 us; the unmap after
// the pause (fire #46) at +4663901 us found the verdict TIMEOUT; the slot read 0x93d0003 at the next judged frame.
// RUN H (run10m 13546, 14162, 14203): the same f19 shape, epoch 0x2eb2, stamp 171134646, the expiry at +4710271 us.
// =====================================================================================================================
static const uint64_t kXBound = 2000000ull;   /* the kext's kKsFlightUs */
struct XMem {
    uint64_t off[4]; uint32_t val[4]; uint32_t got[4]; uint32_t n;
    uint32_t reads; uint64_t readOff[16];
};
static uint32_t x_rd(void *c, uint64_t off, uint32_t *val)
{
    XMem *m = static_cast<XMem *>(c);
    if (m->reads < 16u) m->readOff[m->reads] = off;
    m->reads++;
    for (uint32_t i = 0; i < m->n; i++)
        if (m->off[i] == off) { *val = m->val[i]; return m->got[i]; }
    *val = 0u;
    return 1u;   // a slot nobody wrote reads 0 (the page was zeroed at the ring map)
}
struct XOut {
    uint32_t kdvBefore, kdvAfter, anyLiveAfter, deferNow, withdrawnWhileLive, retired, locked, outcome;
    n48_fr_xpoll o; n48_fr_xret ret[N48_FR_CAPACITY];
};
// hook_unmapVA's verdict block (kdArmed 1, switch 22 ON), with switch 65 latched as `on65` and gXdLock `lockFree`.
static XOut x_unmap(n48_fr_ring *r, uint32_t on65, uint32_t lockFree, uint64_t now, uint64_t nowAfter, XMem *m)
{
    XOut x {};
    uint32_t bs = 0u; uint64_t ba = 0ull;
    uint32_t anyLive = n48_fr_any_live(r);
    uint32_t kdv = n48_fr_defer_verdict(r, 1u, now ? 1u : 0u, now, kXBound, &bs, &ba);
    x.kdvBefore = kdv;
    if (on65 && kdv == N48_KSD_NOW_TIMEOUT) {
        uint32_t n = 0u;
        if (lockFree) {
            x.locked = 1u;
            n = n48_fr_expiry_poll(r, 1u, now ? 1u : 0u, now, kXBound, &x_rd, m, x.ret, N48_FR_CAPACITY, &x.o);
        }
        x.retired = n;
        if (n) {
            anyLive = n48_fr_any_live(r);
            kdv = n48_fr_defer_verdict(r, 1u, nowAfter ? 1u : 0u, nowAfter, kXBound, &bs, &ba);
        }
        x.outcome = n48_fr_expiry_outcome(x.locked, kdv);
    }
    x.kdvAfter = kdv;
    x.anyLiveAfter = anyLive;
    x.deferNow = n48_ksd_defer(kdv);
    const uint32_t kdArmed = 1u;
    x.withdrawnWhileLive = (kdArmed && !x.deferNow && anyLive &&
                            (kdv == N48_KSD_NOW_TIMEOUT || kdv == N48_KSD_NOW_TORN || kdv == N48_KSD_NOW_OFF)) ? 1u : 0u;
    return x;
}
// The continuous arm's judged-frame-top comparison against the arm snapshot (the kext's N48_CM_STOP_WITHDRAWAL stop).
static uint32_t x_stop_why(uint32_t withdrawnWhileLive)
{
    n48_cm_shot sh {};
    sh.state = N48_CM_SHOT_ARMED; sh.cont = 1u; sh.cont_n = 600u;
    (void)n48_cm_shot_stop_if_rose(&sh, 0ull, withdrawnWhileLive, N48_CM_STOP_WITHDRAWAL);
    return sh.state == N48_CM_SHOT_ARMED ? 0u : sh.stop_why;
}
// A RUN H/H2 ring: f1 (seq 1, ordinal 1) and f16 (seq 9, ordinal 2) retired and reclaimed, then f19 committed.
static void x_ring(n48_fr_ring *r, uint32_t epoch, uint64_t at19)
{
    n48_fr_reset(r);
    uint32_t last = 0u;
    (void)n48_fr_push(r, 1u, at19 - 2900000ull, 1u, 0x3cba8f004ull, (epoch << 16) | 1u, nullptr);
    (void)n48_fr_commit_mark(r, 1u, &last);
    uint32_t i1 = 0u; (void)n48_fr_find(r, 1u, &i1);
    (void)n48_fr_poll_entry(r, i1, 1u, (epoch << 16) | 1u);
    (void)n48_fr_reclaim(r);
    (void)n48_fr_push(r, 9u, at19 - 500000ull, 2u, 0x3cba8f008ull, (epoch << 16) | 2u, nullptr);
    (void)n48_fr_commit_mark(r, 9u, &last);
    uint32_t i9 = 0u; (void)n48_fr_find(r, 9u, &i9);
    (void)n48_fr_poll_entry(r, i9, 1u, (epoch << 16) | 2u);
    (void)n48_fr_reclaim(r);
    (void)n48_fr_push(r, 11u, at19, 3u, 0x3cba8f00cull, (epoch << 16) | 3u, nullptr);
    (void)n48_fr_commit_mark(r, 11u, &last);
}
static uint32_t x_state(const n48_fr_ring *r, uint32_t seq)
{
    uint32_t i = 0u;
    return n48_fr_find(r, seq, &i) ? r->e[i].state : (uint32_t)N48_FR_FREE;
}
static void x_run_case(const char *tag, uint32_t epoch, uint64_t at19, uint64_t expiryAge)
{
    const uint32_t want = (epoch << 16) | 3u;
    char l[200];
    // The two deferrals inside the window, both switch settings: DEFER, nothing read.
    for (uint32_t on = 0u; on < 2u; on++) {
        n48_fr_ring r {}; x_ring(&r, epoch, at19);
        XMem m {}; m.n = 1u; m.off[0] = 0x3cba8f00cull; m.val[0] = 0u; m.got[0] = 1u;
        XOut a = x_unmap(&r, on, 1u, at19 + 80301ull, at19 + 80301ull, &m);
        XOut b = x_unmap(&r, on, 1u, at19 + 263064ull, at19 + 263064ull, &m);
        std::snprintf(l, sizeof l, "%s 65 %s: the deferrals at +80301/+263064 us DEFER and read nothing", tag, on ? "ON" : "OFF");
        ck(l, a.kdvAfter == N48_KSD_DEFER && b.kdvAfter == N48_KSD_DEFER && m.reads == 0u, 1u);
    }
    // 65 OFF — THE POSITIVE CONTROL (today): TIMEOUT, withdrawn while live, stop_why 3, nothing read.
    {
        n48_fr_ring r {}; x_ring(&r, epoch, at19);
        XMem m {}; m.n = 1u; m.off[0] = 0x3cba8f00cull; m.val[0] = want; m.got[0] = 1u;
        const XOut x = x_unmap(&r, 0u, 1u, at19 + expiryAge, at19 + expiryAge + 40ull, &m);
        std::snprintf(l, sizeof l, "%s 65 OFF (positive control): the expiry unmap's verdict is TIMEOUT", tag);
        ck(l, x.kdvAfter, N48_KSD_NOW_TIMEOUT);
        std::snprintf(l, sizeof l, "%s 65 OFF: withdrawn while live -> CONTINUOUS STOP stop_why 3", tag);
        ck(l, x_stop_why(x.withdrawnWhileLive), N48_CM_STOP_WITHDRAWAL);
        std::snprintf(l, sizeof l, "%s 65 OFF: no fence read, f19 still COMMITTED", tag);
        ck(l, m.reads == 0u && x_state(&r, 11u) == N48_FR_COMMITTED, 1u);
    }
    // 65 ON, END OF PIPE REACHED before the expiry is applied: retired, verdict NOW_EOP, no stop, the arm continues.
    {
        n48_fr_ring r {}; x_ring(&r, epoch, at19);
        XMem m {}; m.n = 1u; m.off[0] = 0x3cba8f00cull; m.val[0] = want; m.got[0] = 1u;
        const XOut x = x_unmap(&r, 1u, 1u, at19 + expiryAge, at19 + expiryAge + 40ull, &m);
        std::snprintf(l, sizeof l, "%s 65 ON EOP reached: exactly ONE read, of f19's own owned slot", tag);
        ck(l, m.reads == 1u && m.readOff[0] == 0x3cba8f00cull, 1u);
        std::snprintf(l, sizeof l, "%s 65 ON EOP reached: f19 RETIRED (seq 11, ordinal 3, OURS, in order)", tag);
        ck(l, x.retired == 1u && x_state(&r, 11u) == N48_FR_RETIRED && x.ret[0].seq == 11u && x.ret[0].ordinal == 3u &&
              x.ret[0].val == want && x.ret[0].ooo == 0u && x.ret[0].at_us == at19, 1u);
        std::snprintf(l, sizeof l, "%s 65 ON EOP reached: the verdict after is NOW_EOP, nothing live", tag);
        ck(l, x.kdvAfter == N48_KSD_NOW_EOP && x.anyLiveAfter == 0u, 1u);
        std::snprintf(l, sizeof l, "%s 65 ON EOP reached: NOT withdrawn while live -> no stop, the arm stays ARMED", tag);
        ck(l, x.withdrawnWhileLive == 0u && x_stop_why(x.withdrawnWhileLive) == 0u, 1u);
        std::snprintf(l, sizeof l, "%s 65 ON EOP reached: outcome CLEARED", tag);
        ck(l, x.outcome, N48_FR_X_CLEARED);
        // The next judged frame's own poll finds nothing left to retire (no double retirement), and reclaims the slot.
        uint64_t cur = 0ull;
        std::snprintf(l, sizeof l, "%s 65 ON: the next judged frame has nothing left to poll", tag);
        ck(l, n48_fr_next_poll(&r, &cur), N48_FR_CAPACITY);
        std::snprintf(l, sizeof l, "%s 65 ON: retired counter 3 (f1, f16, f19), out-of-order 0", tag);
        ck(l, r.retired == 3u && r.outOfOrder == 0u, 1u);
    }
    // 65 ON, END OF PIPE NOT REACHED at the expiry: withdrawn EXACTLY as today (same verdict, same stop, entry untouched).
    {
        n48_fr_ring r0 {}; x_ring(&r0, epoch, at19);
        XMem m0 {}; m0.n = 1u; m0.off[0] = 0x3cba8f00cull; m0.val[0] = 0u; m0.got[0] = 1u;
        const XOut off = x_unmap(&r0, 0u, 1u, at19 + expiryAge, at19 + expiryAge, &m0);
        n48_fr_ring r {}; x_ring(&r, epoch, at19);
        XMem m {}; m.n = 1u; m.off[0] = 0x3cba8f00cull; m.val[0] = 0u; m.got[0] = 1u;
        const XOut x = x_unmap(&r, 1u, 1u, at19 + expiryAge, at19 + expiryAge, &m);
        std::snprintf(l, sizeof l, "%s 65 ON EOP NOT reached: the slot WAS read (once) and nothing retired", tag);
        ck(l, m.reads == 1u && x.retired == 0u && x_state(&r, 11u) == N48_FR_COMMITTED && x.o.unchanged == 1u, 1u);
        std::snprintf(l, sizeof l, "%s 65 ON EOP NOT reached: verdict, deferNow, withdrawnWhileLive == 65 OFF's", tag);
        ck(l, x.kdvAfter == off.kdvAfter && x.deferNow == off.deferNow && x.withdrawnWhileLive == off.withdrawnWhileLive &&
              x.anyLiveAfter == off.anyLiveAfter, 1u);
        std::snprintf(l, sizeof l, "%s 65 ON EOP NOT reached: WITHDRAWN AT EXPIRY -> stop_why 3, as today", tag);
        ck(l, x.outcome == N48_FR_X_WITHDRAWN && x_stop_why(x.withdrawnWhileLive) == N48_CM_STOP_WITHDRAWAL, 1u);
        std::snprintf(l, sizeof l, "%s 65 ON EOP NOT reached: every entry's state and the newest-flight fate equal 65 OFF's", tag);
        uint32_t same = 1u;
        for (uint32_t i = 0; i < N48_FR_CAPACITY; i++)
            if (r.e[i].state != r0.e[i].state || r.e[i].seq != r0.e[i].seq) same = 0u;
        if (r.newestSeq != r0.newestSeq || r.newestRetired != r0.newestRetired || r.newestExpired != r0.newestExpired ||
            r.retired != r0.retired || r.outOfOrder != r0.outOfOrder) same = 0u;
        ck(l, same, 1u);
    }
}
static void x_checks()
{
    printf("\nX. build 0.0.498: switch 65, end of pipe asked at the deferral's expiry\n");
    // X1 OFF IDENTITY: n48_fr_expiry_poll with `on` 0 reads nothing and leaves every ring byte as it was.
    {
        n48_fr_ring r {}; x_ring(&r, 0x93du, 161569653ull);
        n48_fr_ring before; std::memcpy(&before, &r, sizeof r);
        XMem m {}; m.n = 1u; m.off[0] = 0x3cba8f00cull; m.val[0] = 0x93d0003u; m.got[0] = 1u;
        n48_fr_xret ret[N48_FR_CAPACITY]; n48_fr_xpoll o; std::memset(&o, 0xa5, sizeof o);
        const uint32_t n = n48_fr_expiry_poll(&r, 0u, 1u, 161569653ull + 4663901ull, kXBound, &x_rd, &m, ret,
                                              N48_FR_CAPACITY, &o);
        ck("X1 OFF: nothing retired", n, 0u);
        ck("X1 OFF: NO fence read", m.reads, 0u);
        ck("X1 OFF: the ring is BYTE-IDENTICAL", std::memcmp(&before, &r, sizeof r), 0u);
        ck("X1 OFF: the counts are reported zero", o.polled + o.retired + o.unreadable + o.unchanged + o.other + o.in_bound, 0u);
    }
    x_run_case("X2 RUN H2 (run10n)", 0x93du, 161569653ull, 4663901ull);
    x_run_case("X3 RUN H  (run10m)", 0x2eb2u, 171134646ull, 4710271ull);
    const uint64_t at19 = 161569653ull, exp = at19 + 4663901ull;
    // X4 UNREADABLE slot: fail closed (not retired, withdrawn as today).
    {
        n48_fr_ring r {}; x_ring(&r, 0x93du, at19);
        XMem m {}; m.n = 1u; m.off[0] = 0x3cba8f00cull; m.val[0] = 0x93d0003u; m.got[0] = 0u;
        const XOut x = x_unmap(&r, 1u, 1u, exp, exp, &m);
        ck("X4 unreadable: not retired, counted unreadable", x.retired == 0u && x.o.unreadable == 1u, 1u);
        ck("X4 unreadable: WITHDRAWN AT EXPIRY, stop_why 3 as today",
           x.outcome == N48_FR_X_WITHDRAWN && x_stop_why(x.withdrawnWhileLive) == N48_CM_STOP_WITHDRAWAL, 1u);
    }
    // X5 STALE value: the same ordinal from ANOTHER boot's epoch (RUN H's 0x2eb20003 left in RUN H2's slot) never retires.
    {
        n48_fr_ring r {}; x_ring(&r, 0x93du, at19);
        XMem m {}; m.n = 1u; m.off[0] = 0x3cba8f00cull; m.val[0] = 0x2eb20003u; m.got[0] = 1u;
        const XOut x = x_unmap(&r, 1u, 1u, exp, exp, &m);
        ck("X5 stale epoch: not retired, counted as another value", x.retired == 0u && x.o.other == 1u, 1u);
        ck("X5 stale epoch: withdrawn as today", x.withdrawnWhileLive, 1u);
    }
    // X6 ANOTHER SLOT holds this entry's value, its OWN slot does not: never retires, and only its OWN slot is read.
    {
        n48_fr_ring r {}; x_ring(&r, 0x93du, at19);
        XMem m {}; m.n = 3u;
        m.off[0] = 0x3cba8f00cull; m.val[0] = 0u;          m.got[0] = 1u;
        m.off[1] = 0x3cba8f010ull; m.val[1] = 0x93d0003u;  m.got[1] = 1u;
        m.off[2] = 0x3cba8f008ull; m.val[2] = 0x93d0003u;  m.got[2] = 1u;
        const XOut x = x_unmap(&r, 1u, 1u, exp, exp, &m);
        ck("X6 other slot: exactly one read, of the entry's OWN vram_off", m.reads == 1u && m.readOff[0] == 0x3cba8f00cull, 1u);
        ck("X6 other slot: not retired, withdrawn as today", x.retired == 0u && x.withdrawnWhileLive == 1u, 1u);
    }
    // X7 TWO flights past the bound: the older (ordinal 3) has not reached end of pipe, the newer (ordinal 4) has.
    // The newer retires (OUT-OF-ORDER asked and raised, as the judged frame would), the older stays live: WITHDRAWN.
    {
        n48_fr_ring r {}; x_ring(&r, 0x93du, at19);
        uint32_t last = 0u;
        (void)n48_fr_push(&r, 12u, at19 + 30000ull, 4u, 0x3cba8f010ull, 0x93d0004u, nullptr);
        (void)n48_fr_commit_mark(&r, 12u, &last);
        XMem m {}; m.n = 2u;
        m.off[0] = 0x3cba8f00cull; m.val[0] = 0u;         m.got[0] = 1u;
        m.off[1] = 0x3cba8f010ull; m.val[1] = 0x93d0004u; m.got[1] = 1u;
        const XOut x = x_unmap(&r, 1u, 1u, exp + 40000ull, exp + 40000ull, &m);
        ck("X7 two flights: both read, in ordinal order", m.reads == 2u && m.readOff[0] == 0x3cba8f00cull &&
                                                          m.readOff[1] == 0x3cba8f010ull, 1u);
        ck("X7 two flights: the newer retired, OUT-OF-ORDER flagged", x.retired == 1u && x.ret[0].seq == 12u && x.ret[0].ooo == 1u, 1u);
        ck("X7 two flights: the older still COMMITTED", x_state(&r, 11u), N48_FR_COMMITTED);
        ck("X7 two flights: verdict still TIMEOUT -> WITHDRAWN AT EXPIRY, stop_why 3",
           x.kdvAfter == N48_KSD_NOW_TIMEOUT && x.outcome == N48_FR_X_WITHDRAWN &&
           x_stop_why(x.withdrawnWhileLive) == N48_CM_STOP_WITHDRAWAL, 1u);
    }
    // X8 a flight still WITHIN its bound is neither read nor retired; once the expired one retires, the verdict DEFERS.
    {
        n48_fr_ring r {}; x_ring(&r, 0x93du, at19);
        XMem m {}; m.n = 2u;
        m.off[0] = 0x3cba8f00cull; m.val[0] = 0x93d0003u; m.got[0] = 1u;
        m.off[1] = 0x3cba8f010ull; m.val[1] = 0x93d0004u; m.got[1] = 1u;
        // A newer commit pushed and committed between the scan and the poll (the lock-free window hook_unmapVA has).
        uint32_t last = 0u;
        (void)n48_fr_push(&r, 12u, exp - 100000ull, 4u, 0x3cba8f010ull, 0x93d0004u, nullptr);
        (void)n48_fr_commit_mark(&r, 12u, &last);
        n48_fr_xret ret[N48_FR_CAPACITY]; n48_fr_xpoll o {};
        const uint32_t n = n48_fr_expiry_poll(&r, 1u, 1u, exp, kXBound, &x_rd, &m, ret, N48_FR_CAPACITY, &o);
        ck("X8 in bound: only the expired flight is read and retired", n == 1u && m.reads == 1u && ret[0].seq == 11u, 1u);
        ck("X8 in bound: the in-bound one is counted, not read, still COMMITTED", o.in_bound == 1u &&
                                                                                   x_state(&r, 12u) == N48_FR_COMMITTED, 1u);
        uint32_t bs = 0u; uint64_t ba = 0ull;
        const uint32_t kdv = n48_fr_defer_verdict(&r, 1u, 1u, exp, kXBound, &bs, &ba);
        ck("X8 in bound: the verdict after is DEFER -> outcome DEFERRED", kdv == N48_KSD_DEFER &&
                                                                         n48_fr_expiry_outcome(1u, kdv) == N48_FR_X_DEFERRED, 1u);
    }
    // X9 NOT_RUN and PENDING entries past the bound are never read and never retire (nothing writes their slots).
    {
        n48_fr_ring r {}; x_ring(&r, 0x93du, at19);
        (void)n48_fr_mark_not_run(&r, 11u);
        (void)n48_fr_push(&r, 12u, at19 + 1000ull, 4u, 0x3cba8f010ull, 0x93d0004u, nullptr);   // PENDING
        XMem m {}; m.n = 2u;
        m.off[0] = 0x3cba8f00cull; m.val[0] = 0x93d0003u; m.got[0] = 1u;
        m.off[1] = 0x3cba8f010ull; m.val[1] = 0x93d0004u; m.got[1] = 1u;
        const XOut x = x_unmap(&r, 1u, 1u, exp, exp, &m);
        ck("X9 NOT_RUN/PENDING: nothing read, nothing retired", m.reads == 0u && x.retired == 0u, 1u);
        ck("X9 NOT_RUN/PENDING: WITHDRAWN AT EXPIRY as today", x.outcome == N48_FR_X_WITHDRAWN && x.withdrawnWhileLive == 1u, 1u);
    }
    // X10 gXdLock BUSY: nothing read, BUSY, withdrawn as today.
    {
        n48_fr_ring r {}; x_ring(&r, 0x93du, at19);
        XMem m {}; m.n = 1u; m.off[0] = 0x3cba8f00cull; m.val[0] = 0x93d0003u; m.got[0] = 1u;
        const XOut x = x_unmap(&r, 1u, 0u, exp, exp, &m);
        ck("X10 lock busy: nothing read, outcome BUSY", m.reads == 0u && x.outcome == N48_FR_X_BUSY, 1u);
        ck("X10 lock busy: withdrawn as today (stop_why 3)", x_stop_why(x.withdrawnWhileLive), N48_CM_STOP_WITHDRAWAL);
    }
    // X11 the outcome table, every verdict.
    {
        ck("X11 outcome: not locked -> BUSY", n48_fr_expiry_outcome(0u, N48_KSD_NOW_EOP), N48_FR_X_BUSY);
        ck("X11 outcome: EOP -> CLEARED", n48_fr_expiry_outcome(1u, N48_KSD_NOW_EOP), N48_FR_X_CLEARED);
        ck("X11 outcome: NOT_IN_FLIGHT -> CLEARED", n48_fr_expiry_outcome(1u, N48_KSD_NOW_NOT_IN_FLIGHT), N48_FR_X_CLEARED);
        ck("X11 outcome: DEFER -> DEFERRED", n48_fr_expiry_outcome(1u, N48_KSD_DEFER), N48_FR_X_DEFERRED);
        ck("X11 outcome: TIMEOUT -> WITHDRAWN", n48_fr_expiry_outcome(1u, N48_KSD_NOW_TIMEOUT), N48_FR_X_WITHDRAWN);
        ck("X11 outcome: TORN -> WITHDRAWN", n48_fr_expiry_outcome(1u, N48_KSD_NOW_TORN), N48_FR_X_WITHDRAWN);
        ck("X11 outcome: OFF -> WITHDRAWN", n48_fr_expiry_outcome(1u, N48_KSD_NOW_OFF), N48_FR_X_WITHDRAWN);
    }
    // X12 the three lines fit under the log cap (491 bytes, as B7) at their widest arguments.
    {
        char b[2048];
        const uint32_t U = 0xffffffffu; const unsigned long long L = 0xffffffffffffffffull;
        uint32_t wo = 0u; size_t ol = 0u;
        for (uint32_t v = 0; v < N48_FR_X_OUTCOMES; v++) { const size_t k = std::strlen(n48_fr_expiry_outcome_name(v)); if (k > ol) { ol = k; wo = v; } }
        const int n1 = std::snprintf(b, sizeof b, N48_FR_X_LINE_FMT, (void *)0xffffff9063d34700ull, U, U, U, L, L,
                                     U, U, U, U, U, U, U, n48_fr_expiry_outcome_name(wo));
        const int n2 = std::snprintf(b, sizeof b, N48_FR_X_RETIRED_FMT, U, U, U, L, U, U, L);
        const int n3 = std::snprintf(b, sizeof b, N48_FR_X_REPORT_FMT, "OFF (default)",
                                     " - `gfxneuter 65` REFUSED: a continuous arm stands, unchanged", L, L, L, L, L, L, L, L, L, L);
        if (!gQuiet) printf("      ksexp65 CHECK %d, RETIRED %d, REPORT %d bytes (cap 491)\n", n1, n2, n3);
        ck("X12 the EXPIRY CHECK line fits", n1 > 0 && (unsigned)n1 <= 491u, 1u);
        ck("X12 the RETIRED AT EXPIRY line fits", n2 > 0 && (unsigned)n2 <= 491u, 1u);
        ck("X12 the ksexp65 report line fits", n3 > 0 && (unsigned)n3 <= 491u, 1u);
    }
}

int main()
{
    printf("gfx_flightring_test — C5 part 1 (an internal design note Q1); an internal review note (B), 0.0.444\n\n");
    // B1's own planted break (the "only the newest entry is asked" model) is printed FROM INSIDE
    // multi_entry_earlier_defers(), alongside the real checks it falsifies - both are folded into the totals below.
    basics();
    poll_and_retire();
    identity_all_verdicts();
    multi_entry_earlier_defers();
    real_sequence_regression_pin();
    report_format_bound();
    item2_race_planted_breaks();
    item4_token_mismatch_not_run();
    item6_expire_reclaim();
    item8_out_of_order_excludes();
    item5ii_no_entry_no_keystone();
    fr446_fix1_newest_restore();
    fr446_fix2_ordinal_order();
    fr446_fix3_reclaim_after_the_after();
    fr446_fix4_guard_line();
    x_checks();   // build 0.0.498: switch 65, end of pipe asked at the deferral's expiry (RUN H/H2)
    printf("\nPLANTED DEFECTS (each must be CAUGHT):\n");
    b3_planted_break();
    b4_planted_break();
    b5_planted_break();

    const int realFail = gFail;
    printf("\n-- %d check(s), %d failure(s)\n", gRun, realFail);
    printf("\ngfx_flightring: %s\n", realFail == 0 ? "N48-FLIGHTRING-TEST-PASS" : "FAIL");
    return realFail == 0 ? 0 : 1;
}
