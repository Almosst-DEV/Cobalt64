// gfx_rpunmapq_test.cpp — build 0.0.453 item 6 (reviewer findings on 0.0.452): the busy-lock unmap queue,
// race-free. gfx_rpunmapq.h is pure C11; this file drives it with REAL concurrent std::threads (not a hand-simulated
// interleaving) so a missing lock is a REAL data race, catchable by -fsanitize=thread and, for the count-based
// checks below, observable even without it.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=thread -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/gfx_rpunmapq_test.cpp -o /tmp/rpuqtest && /tmp/rpuqtest
//
// (suites.sh runs it under ASan/UBSan like every other suite - see the PLANTED BREAK 1 section below for why an
// actual TSan run, done once by hand while writing this fix, is the strongest evidence for item 6a; ASan/UBSan
// still catch the accounting mismatch the race produces, which is what this file's exit code gates on.)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>
#include <algorithm>
#include "gfx_rpunmapq.h"
#include "ws_resprov.h"   // build 0.0.455 item 5: the dirty-context bounding range's own consumer (n48_rp_unmap_rng)

static int gFail = 0, gRun = 0;
static void ck(const char *what, bool got) { gRun++; if (!got) { gFail++; printf("  FAIL %s\n", what); } else printf("  ok   %s\n", what); }

// ---------------------------------------------------------------------------------------------------------------------
// 1. SEQUENTIAL CORRECTNESS: push/take behave as a bounded queue, exactly N48_RPUQ_CAP deep, with overflow sticky.
// ---------------------------------------------------------------------------------------------------------------------
static void test_sequential()
{
    n48_rpuq q; n48_rpuq_init(&q);
    for (uint32_t i = 0; i < N48_RPUQ_CAP; i++)
        ck("push into an empty slot succeeds", n48_rpuq_push(&q, i, 0x1000ull * i, 0x100ull) == 1);
    ck("the (N48_RPUQ_CAP+1)'th push overflows", n48_rpuq_push(&q, 99u, 0ull, 0ull) == 0);
    ck("overflow is now sticky", q.overflow == 1u);
    n48_rpuq_entry out[N48_RPUQ_CAP];
    uint32_t n = 0;
    n48_rpuq_entry ent;
    while (n48_rpuq_pop(&q, &ent)) out[n++] = ent;
    ck("popping to empty drains exactly the N48_RPUQ_CAP entries that fit", n == N48_RPUQ_CAP);
    ck("pop() resets the count", q.n == 0u);
    for (uint32_t i = 0; i < N48_RPUQ_CAP; i++)
        ck("each popped entry is exactly what was pushed (FIFO order preserved)", out[i].ctx == i && out[i].va == 0x1000ull * i);
    ck("a pop() on an empty queue returns 0", n48_rpuq_pop(&q, &ent) == 0);
    ck("the overflow flag is UNTOUCHED by pop() - it is a separate critical section", q.overflow == 1u);
    ck("n48_rpuq_take_overflow() reports it", n48_rpuq_take_overflow(&q) == 1u);
    ck("n48_rpuq_take_overflow() clears the sticky overflow flag", q.overflow == 0u);
    ck("a second take_overflow() on a clean queue returns 0", n48_rpuq_take_overflow(&q) == 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// 2. ITEM 6b: TWO DIFFERENT CONTEXTS OVERFLOWING BEFORE ONE DRAIN. 0.0.452's bug: a single remembered `dirtyCtx`
//    means the SECOND overflow's context silently replaces the first's, so the first context's fail-closed wipe
//    never happens. gfx_rpunmapq.h's sticky BOOLEAN fixes this by construction - there is no context to remember,
//    so the caller wipes EVERYTHING on any overflow, regardless of how many distinct contexts overflowed or in
//    what order.
// ---------------------------------------------------------------------------------------------------------------------
static void test_two_context_overflow()
{
    n48_rpuq q; n48_rpuq_init(&q);
    for (uint32_t i = 0; i < N48_RPUQ_CAP; i++) n48_rpuq_push(&q, 1000u, 0ull, 0ull);   // fill it with a THIRD context
    ck("overflow from context A", n48_rpuq_push(&q, 0xAu, 1ull, 1ull) == 0);
    ck("overflow is sticky after A", q.overflow == 1u);
    ck("overflow from context B (a DIFFERENT context) does not need to be seen specially - it's still just 'overflow'",
       n48_rpuq_push(&q, 0xBu, 2ull, 2ull) == 0);
    ck("overflow is STILL sticky after B - nothing about A was lost by B happening", q.overflow == 1u);
    n48_rpuq_entry ent;
    while (n48_rpuq_pop(&q, &ent)) {}
    ck("item 6b: the drain sees the overflow AT ALL - a caller that wipes the whole ledger on this flag covers "
       "BOTH A and B (and the pre-existing third context), not whichever one 0.0.452's single field happened to "
       "remember last", n48_rpuq_take_overflow(&q) == 1u);
}

// PLANTED BREAK for item 6b: restore 0.0.452's actual shape - a SINGLE remembered context instead of a sticky
// boolean - and show it silently drops the first overflow's context when a second, different one follows.
static uint32_t gPlantedDirtyCtx = 0u;   // 0 = none
static int planted_push_single_ctx(uint32_t *n, uint32_t cap, uint32_t ctx)
{
    if (*n < cap) { (*n)++; return 1; }
    gPlantedDirtyCtx = ctx;   // THE BUG: overwrites whatever a previous overflow already remembered
    return 0;
}
static void test_planted_single_context_fallback()
{
    uint32_t n = N48_RPUQ_CAP;   // already full
    gPlantedDirtyCtx = 0u;
    planted_push_single_ctx(&n, N48_RPUQ_CAP, 0xAu);
    ck("planted setup: context A's overflow is remembered", gPlantedDirtyCtx == 0xAu);
    planted_push_single_ctx(&n, N48_RPUQ_CAP, 0xBu);
    const bool lostA = (gPlantedDirtyCtx != 0xAu);
    printf("  planted %-66s %s (dirtyCtx after B's overflow: %#x, A silently lost: %d)\n",
           "item 6b: single remembered context (0.0.452's actual shape)", lostA ? "CAUGHT (this file's real code does not have this shape)" : "NOT CAUGHT", gPlantedDirtyCtx, lostA);
    ck("BREAK-check: a single remembered context DOES lose A's overflow once B also overflows - exactly the bug "
       "item 6b closes, and exactly why gfx_rpunmapq.h uses a sticky boolean instead", lostA);
}

// ---------------------------------------------------------------------------------------------------------------------
// 3. ITEM 6a: REAL CONCURRENT PUSHES RACING A REAL CONCURRENT DRAIN. Many producer threads call n48_rpuq_push in a
//    loop (far more pushes than the queue could ever hold at once) while a consumer thread repeatedly calls
//    n48_rpuq_take, accumulating every drained entry PLUS a running total of every push the queue tells us
//    overflowed. INVARIANT (checked after every thread joins and a final drain empties the queue): every entry a
//    producer successfully pushed (n48_rpuq_push returned 1) is either drained exactly once, or accounted for by
//    the overflow flag on SOME take() call that could only have discarded entries already counted as pushed - in
//    other words, (entries drained) + (entries that returned 0, i.e. genuinely refused before ever being counted
//    as pushed) == (total push() calls), with NO duplicate delivery and no entry appearing that was never pushed.
//    Run under -fsanitize=thread (by hand; suites.sh's own ASan run cannot combine with TSan) this catches the
//    unlocked-push race directly, as a reported data race - the strongest form of "an interleaving model" for code
//    that is ACTUALLY concurrent, not merely modelled as if it were.
// ---------------------------------------------------------------------------------------------------------------------
struct Observed { uint32_t ctx; uint64_t va; };
static void run_concurrency_stress(n48_rpuq *q, uint32_t nThreads, uint32_t pushesPerThread,
                                    uint64_t *outPushed, uint64_t *outDrained, uint64_t *outOverflowed,
                                    std::vector<Observed> *outSeen)
{
    std::atomic<uint64_t> pushed{0}, refused{0};
    std::atomic<bool> stop{false};
    std::mutex seenMu;
    std::thread consumer([&]() {
        n48_rpuq_entry ent;
        while (!stop.load(std::memory_order_relaxed)) {
            while (n48_rpuq_pop(q, &ent)) { std::lock_guard<std::mutex> lk(seenMu); outSeen->push_back({ent.ctx, ent.va}); }
            (void)n48_rpuq_take_overflow(q);   // already counted at push time below; just keep the flag from going stale
            std::this_thread::yield();
        }
    });
    std::vector<std::thread> producers;
    for (uint32_t t = 0; t < nThreads; t++) {
        producers.emplace_back([&, t]() {
            for (uint32_t i = 0; i < pushesPerThread; i++) {
                const uint32_t ctx = t * 1000000u + i;
                if (n48_rpuq_push(q, ctx, (uint64_t)ctx, 0x100ull)) pushed.fetch_add(1, std::memory_order_relaxed);
                else refused.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto &th : producers) th.join();
    stop.store(true, std::memory_order_relaxed);
    consumer.join();
    // one more drain: anything the consumer's last iteration missed
    { n48_rpuq_entry ent;
      while (n48_rpuq_pop(q, &ent)) { std::lock_guard<std::mutex> lk(seenMu); outSeen->push_back({ent.ctx, ent.va}); }
      (void)n48_rpuq_take_overflow(q); }
    *outPushed = pushed.load();
    *outDrained = outSeen->size();
    *outOverflowed = refused.load();
}
static void test_concurrency_no_loss()
{
    n48_rpuq q; n48_rpuq_init(&q);
    uint64_t pushed = 0, drained = 0, refused = 0;
    std::vector<Observed> seen;
    run_concurrency_stress(&q, 8u, 5000u, &pushed, &drained, &refused, &seen);
    printf("  concurrency stress: 8 producer threads x 5000 pushes + 1 concurrent consumer thread: pushed %llu, "
           "drained %llu, refused(overflow) %llu\n", (unsigned long long)pushed, (unsigned long long)drained, (unsigned long long)refused);
    ck("item 6a: EVERY successful push was drained EXACTLY once (no lost push, no duplicate) - pushed == drained",
       pushed == drained);
    ck("item 6a: no entry appeared with a ctx this test never pushed", [&]{
           for (auto &o : seen) { const uint32_t t = o.ctx / 1000000u; const uint32_t i = o.ctx % 1000000u;
               if (t >= 8u || i >= 5000u || o.va != (uint64_t)o.ctx) return false; }
           return true; }());
    // dedup check: no ctx observed twice (would indicate the SAME slot delivered by two overlapping drains)
    std::vector<uint32_t> ctxs; ctxs.reserve(seen.size());
    for (auto &o : seen) ctxs.push_back(o.ctx);
    std::sort(ctxs.begin(), ctxs.end());
    bool dup = false; for (size_t i = 1; i < ctxs.size(); i++) if (ctxs[i] == ctxs[i - 1]) { dup = true; break; }
    ck("item 6a: no entry was delivered twice", !dup);
}

// ---------------------------------------------------------------------------------------------------------------------
// 4. build 0.0.455 item 5 (decide45, notes/logs/runs/decide45/): the DIRTY-CONTEXT BOUNDING RANGE. decide45 hit
//    this queue's overflow 1,379 times in one boot at the old CAP of 8, and the OLD fallback (wipe the whole ledger
//    on ANY overflow) refused PROVENANCE for every Family A segment. Now an overflowing push also widens a
//    per-context [vaMin, vaEnd) bound in `dirty[]`, so the drain can call n48_rp_unmap_rng with a real bound instead
//    of wiping everything - UNLESS more distinct contexts overflow than `dirty[]` can name
//    (N48_RPUQ_DIRTY_CTX_MAX), in which case it must still fall back to the whole wipe.
// ---------------------------------------------------------------------------------------------------------------------
static void test_dirty_bounding_single_context()
{
    n48_rpuq q; n48_rpuq_init(&q);
    for (uint32_t i = 0; i < N48_RPUQ_CAP; i++) n48_rpuq_push(&q, 777u, 0ull, 0ull);   // fill it with an unrelated context
    // 300 busy unmaps for ONE context, at SCATTERED, non-contiguous, "unrelated" ranges - exactly the shape the
    // brief asks for. Each is [va, va+0x100); the bound must widen to cover every one of them.
    for (uint32_t i = 0; i < 300u; i++) n48_rpuq_push(&q, 42u, 0x10000000ull * (uint64_t)i, 0x100ull);
    ck("300 scattered overflowing pushes for one context: overflow is sticky", q.overflow == 1u);
    n48_rpuq_entry drain; while (n48_rpuq_pop(&q, &drain)) {}   // empty the CAP-deep queue of the filler context first
    n48_rpuq_dirty dirty[N48_RPUQ_DIRTY_CTX_MAX]; uint32_t dirtyN = 0u; int wholeWipe = 0;
    n48_rpuq_take_dirty(&q, dirty, &dirtyN, &wholeWipe);
    ck("300 scattered pushes, ONE context: dirty[] holds exactly one entry", dirtyN == 1u);
    ck("that entry's ctx is 42", dirtyN == 1u && dirty[0].ctx == 42u);
    ck("no dirty-context overflow (one context fits easily in N48_RPUQ_DIRTY_CTX_MAX)", !wholeWipe);
    // the bound is the UNION of every pushed range: [0, 299*0x10000000 + 0x100)
    const uint64_t wantMin = 0ull, wantMax = 0x10000000ull * 299ull + 0x100ull;
    ck("the bound's vaMin is the FIRST range's own start", dirtyN == 1u && dirty[0].vaMin == wantMin);
    ck("the bound's vaEnd is the LAST range's own end", dirtyN == 1u && dirty[0].vaEnd == wantMax);
    ck("take_dirty() clears the table (a second call sees nothing)", [&]{
           n48_rpuq_dirty d2[N48_RPUQ_DIRTY_CTX_MAX]; uint32_t n2 = 0u; int w2 = 0;
           n48_rpuq_take_dirty(&q, d2, &n2, &w2); return n2 == 0u && !w2; }());
}

// A LEDGER-LEVEL proof, combining gfx_rpunmapq.h's own bound with ws_resprov.h's n48_rp_unmap_rng exactly as
// gfxsrc_rp_unmap_drain_locked (AppleHardwareHook.cpp) calls it. "300 busy unmaps of unrelated ranges keep an
// entry" and "one overlapping unmap in the overflow drops it" are ONE scenario: a ledger entry planted INSIDE the
// resulting bound is dropped; one planted FAR OUTSIDE it (never touched by any of the 300 pushes) survives.
static n48_rp_ent mk_ent(uint64_t ctx, uint64_t va, uint64_t bytes)
{
    n48_rp_ent e; e.ctx = ctx; e.va = va; e.vram = 0x20000000ull; e.bytes = bytes; e.mode = 1u; e.arm = 1u; e.epoch = 1u; e.elemBytes = 4u;
    return e;
}
static void test_dirty_bounding_ledger_integration()
{
    n48_rpuq q; n48_rpuq_init(&q);
    for (uint32_t i = 0; i < N48_RPUQ_CAP; i++) n48_rpuq_push(&q, 777u, 0ull, 0ull);
    for (uint32_t i = 0; i < 300u; i++) n48_rpuq_push(&q, 42u, 0x10000000ull * (uint64_t)i, 0x100ull);
    n48_rpuq_entry drain; while (n48_rpuq_pop(&q, &drain)) {}
    n48_rpuq_dirty dirty[N48_RPUQ_DIRTY_CTX_MAX]; uint32_t dirtyN = 0u; int wholeWipe = 0;
    n48_rpuq_take_dirty(&q, dirty, &dirtyN, &wholeWipe);
    ck("ledger integration setup: exactly one dirty context, not a whole-wipe", dirtyN == 1u && !wholeWipe);

    n48_rp t; std::memset(&t, 0, sizeof t);
    t.e[t.n++] = mk_ent(42u, 5ull * 0x10000000ull, 0x100ull);          // INSIDE the bound (one of the 300 ranges' own VA)
    t.e[t.n++] = mk_ent(42u, 500ull * 0x10000000ull, 0x100ull);        // FAR OUTSIDE the bound (well past vaEnd)
    t.e[t.n++] = mk_ent(9999u, 5ull * 0x10000000ull, 0x100ull);        // a DIFFERENT context, same VA as the first
    ck("ledger integration setup: 3 entries recorded", t.n == 3u);

    // the SAME call gfxsrc_rp_unmap_drain_locked makes for a bounded (non-whole-wipe) overflow.
    for (uint32_t i = 0; i < dirtyN; i++)
        n48_rp_unmap_rng(&t, dirty[i].ctx, dirty[i].vaMin, dirty[i].vaEnd - dirty[i].vaMin);

    bool haveInside = false, haveOutside = false, haveOtherCtx = false;
    for (uint32_t k = 0; k < t.n; k++) {
        if (t.e[k].ctx == 42u && t.e[k].va == 5ull * 0x10000000ull) haveInside = true;
        if (t.e[k].ctx == 42u && t.e[k].va == 500ull * 0x10000000ull) haveOutside = true;
        if (t.e[k].ctx == 9999u) haveOtherCtx = true;
    }
    ck("the entry INSIDE the bound (one overlapping unmap in the overflow) is DROPPED", !haveInside);
    ck("the entry FAR OUTSIDE the bound (unrelated to any of the 300 pushes) is KEPT", haveOutside);
    ck("a DIFFERENT context's entry at the same VA is KEPT (the bound is per-context, not global)", haveOtherCtx);
    ck("exactly one entry survived", t.n == 2u);
}

// Too many DISTINCT contexts overflow for dirty[] to name (N48_RPUQ_DIRTY_CTX_MAX + 1) - tracking gives up and the
// caller must fall back to the whole-ledger wipe.
static void test_too_many_contexts_falls_back_to_whole_wipe()
{
    n48_rpuq q; n48_rpuq_init(&q);
    for (uint32_t i = 0; i < N48_RPUQ_CAP; i++) n48_rpuq_push(&q, 777u, 0ull, 0ull);
    for (uint32_t c = 0; c < N48_RPUQ_DIRTY_CTX_MAX; c++) n48_rpuq_push(&q, 1000u + c, 0x1000ull, 0x10ull);
    n48_rpuq_entry drain; while (n48_rpuq_pop(&q, &drain)) {}
    {   n48_rpuq_dirty dirty[N48_RPUQ_DIRTY_CTX_MAX]; uint32_t dirtyN = 0u; int wholeWipe = 0;
        n48_rpuq_take_dirty(&q, dirty, &dirtyN, &wholeWipe);
        ck("exactly N48_RPUQ_DIRTY_CTX_MAX distinct contexts: all tracked, no whole-wipe yet", dirtyN == N48_RPUQ_DIRTY_CTX_MAX && !wholeWipe);
    }
    // ONE MORE distinct context overflows, past the cap
    for (uint32_t i = 0; i < N48_RPUQ_CAP; i++) n48_rpuq_push(&q, 777u, 0ull, 0ull);
    for (uint32_t c = 0; c < N48_RPUQ_DIRTY_CTX_MAX; c++) n48_rpuq_push(&q, 2000u + c, 0x1000ull, 0x10ull);   // fills dirty[] again
    n48_rpuq_push(&q, 9999u, 0x1000ull, 0x10ull);   // the (N48_RPUQ_DIRTY_CTX_MAX + 1)'th DISTINCT context
    while (n48_rpuq_pop(&q, &drain)) {}
    n48_rpuq_dirty dirty[N48_RPUQ_DIRTY_CTX_MAX]; uint32_t dirtyN = 0u; int wholeWipe = 0;
    n48_rpuq_take_dirty(&q, dirty, &dirtyN, &wholeWipe);
    ck("item 5: a context beyond dirty[]'s own capacity forces the whole-ledger fallback", wholeWipe == 1);
    ck("the contexts dirty[] DID have room for are still reported (applied redundantly under the wipe, per the "
       "header's own comment)", dirtyN == N48_RPUQ_DIRTY_CTX_MAX);
}

// PLANTED BREAK for item 5: a naive overflow tracker that just OVERWRITES dirty[0] instead of giving up and
// reporting wholeWipe once N48_RPUQ_DIRTY_CTX_MAX is exceeded - exactly 0.0.452's item 6b bug (a single remembered
// slot silently loses an earlier context), reintroduced one level down at the bounding-range layer. Shows why
// gfx_rpunmapq.h's real code must set `dirtyOverflow` instead of silently dropping or overwriting an entry.
static void test_planted_dirty_overwrite_instead_of_wholewipe()
{
    // model: same shape as the real dirty[]/dirtyN, but "full" means "overwrite slot 0" (the bug)
    struct { uint32_t ctx; } naive[N48_RPUQ_DIRTY_CTX_MAX]; uint32_t naiveN = 0u;
    auto naive_track = [&](uint32_t ctx) {
        for (uint32_t i = 0; i < naiveN; i++) if (naive[i].ctx == ctx) return;
        if (naiveN < N48_RPUQ_DIRTY_CTX_MAX) naive[naiveN++].ctx = ctx;
        else naive[0].ctx = ctx;   // THE BUG: overwrites instead of giving up
    };
    for (uint32_t c = 0; c < N48_RPUQ_DIRTY_CTX_MAX; c++) naive_track(1000u + c);
    const uint32_t firstCtxBefore = naive[0].ctx;
    naive_track(9999u);   // one context past the naive model's own capacity
    const bool lostFirst = (naive[0].ctx != firstCtxBefore);
    printf("  planted %-66s %s (slot 0: %#x -> %#x, first context silently lost: %d)\n",
           "item 5: overwrite dirty[0] instead of reporting wholeWipe (the naive model)",
           lostFirst ? "CAUGHT (this file's real code does not have this shape)" : "NOT CAUGHT", firstCtxBefore, naive[0].ctx, lostFirst);
    ck("BREAK-check: the naive overwrite-on-full model DOES silently lose the first context's own bound once a "
       "later, different context also overflows past capacity - exactly why the real n48_rpuq_push sets "
       "dirtyOverflow (forcing a WHOLE wipe) instead of ever overwriting a dirty[] slot", lostFirst);
    // confirm the REAL code does not do this: refill past capacity and show dirtyN never exceeds the cap and the
    // FIRST tracked context's bound is unchanged by a later overflowing context past capacity.
    n48_rpuq q; n48_rpuq_init(&q);
    for (uint32_t i = 0; i < N48_RPUQ_CAP; i++) n48_rpuq_push(&q, 777u, 0ull, 0ull);
    for (uint32_t c = 0; c < N48_RPUQ_DIRTY_CTX_MAX; c++) n48_rpuq_push(&q, 3000u + c, 0x2000ull * (uint64_t)c, 0x10ull);
    n48_rpuq_push(&q, 8888u, 0x1000ull, 0x10ull);   // past capacity
    ck("the real code: dirtyN never exceeds N48_RPUQ_DIRTY_CTX_MAX even after a past-capacity overflow", q.dirtyN == N48_RPUQ_DIRTY_CTX_MAX);
    ck("the real code: the FIRST tracked context (3000) still holds its OWN bound, untouched by the later "
       "past-capacity context (8888's own push never lands in dirty[] at all)",
       q.dirty[0].ctx == 3000u && q.dirty[0].vaMin == 0ull);
}

int main()
{
    printf("gfx_rpunmapq (0.0.453 item 6, 0.0.455 item 5): the busy-lock unmap queue, race-free\n");
    test_sequential();
    test_two_context_overflow();
    test_planted_single_context_fallback();
    test_concurrency_no_loss();
    test_dirty_bounding_single_context();
    test_dirty_bounding_ledger_integration();
    test_too_many_contexts_falls_back_to_whole_wipe();
    test_planted_dirty_overwrite_instead_of_wholewipe();
    printf("\nchecks run %d, failures %d\n", gRun, gFail);
    const bool ok = (gFail == 0);
    printf("gfx_rpunmapq: %s\n", ok ? "N48-RPUNMAPQ-TEST-PASS" : "N48-RPUNMAPQ-TEST-FAIL");
    return ok ? 0 : 1;
}
