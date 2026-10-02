// ctx_latch_test.cpp — the context latch's safety properties, offline (notes/M4-CONTEXT-LATCH.md). The property under test:
//
//     "no client context is created while the latch is closed, and a closed latch is left only by ALL FOUR protections
//      being live in one sample - never by time, never by a partial set, never by who arrived first."
//
// Every check runs against an OPS TABLE, so the same assertions judge the real header and each planted defect. The real
// table must pass every check; each mutant must be CAUGHT by at least one named check (a test no mutation can break is not
// testing anything). The mutants are the plausible ways to write this wrong: open on any bit, open on the drain alone (the
// design's own named hazard), bypass by timing, no timeout, a per-caller deadline, a failure that is not sticky, a timeout
// that fails open, a GFX arm plan that is not idempotent, no kernel bypass, and two wrong refusal ACTIONS (let through; spin).
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/ctx_latch_test.cpp -o /tmp/latchtest && /tmp/latchtest
#include <cstdio>
#include <cstdint>
#include "ctx_latch.h"

static int gFail = 0, gRun = 0, gQuiet = 0;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) std::printf("FAIL  %-86s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
    } else if (!gQuiet) {
        std::printf("ok    %-86s %#llx\n", what, (unsigned long long)got);
    }
}

struct Ops {
    const char *name;
    void     (*init)(n48_latch *, uint32_t, uint32_t, uint64_t);
    void     (*fail)(n48_latch *, uint32_t);
    uint32_t (*chain_end)(n48_latch *, uint32_t);
    uint32_t (*decide)(n48_latch *, uint32_t, uint64_t, uint32_t);
    uint32_t (*plan)(uint32_t, uint32_t, uint32_t);
    uint32_t (*action)(uint32_t);
};

static const uint64_t B = 30000;   // the bound, in test ticks (the kext uses 30 s in mach absolute units)

// ---------------------------------------------------------------------------------------------------------------------
// The mutants. Each is the real function with ONE plausible mistake.
// ---------------------------------------------------------------------------------------------------------------------
static uint32_t m1_chain_end_any_bit(n48_latch *l, uint32_t live)       // opens on a partial set (any protection)
{
    l->live_at_end = live;
    if (l->state != N48_LATCH_CLOSED) return l->state;
    if (live) { l->state = N48_LATCH_OPEN; l->live_at_open = live; } else { l->state = N48_LATCH_FAILED; l->fail_why = N48_LF_PARTIAL; }
    return l->state;
}
static uint32_t m2_chain_end_drain_only(n48_latch *l, uint32_t live)    // "the SDMA drain is the one that killed hp2"
{
    l->live_at_end = live;
    if (l->state != N48_LATCH_CLOSED) return l->state;
    if (live & N48_LP_SDMA_DRAIN) { l->state = N48_LATCH_OPEN; l->live_at_open = live; }
    else { l->state = N48_LATCH_FAILED; l->fail_why = N48_LF_PARTIAL; }
    return l->state;
}
static uint32_t m3_decide_bypass_by_timing(n48_latch *l, uint32_t who, uint64_t now, uint32_t intr)
{
    // "the kernel's context always comes first, so let the first second through" - a bypass decided by WHEN.
    if (l->enforced && who == N48_LC_CLIENT && now < 1000) return N48_LD_BYPASS;
    return n48_latch_decide(l, who, now, intr);
}
static uint32_t m4_decide_no_timeout(n48_latch *l, uint32_t who, uint64_t now, uint32_t intr)
{
    const uint32_t d = n48_latch_decide(l, who, now, intr);
    return d == N48_LD_REFUSE_TIMEOUT ? N48_LD_WAIT : d;
}
static uint32_t m5_decide_per_caller_deadline(n48_latch *l, uint32_t who, uint64_t now, uint32_t intr)
{
    // Every caller gets its own bound, so N queued clients wait N x bound in total (WindowServer's 120 s watchdog).
    if (l->enforced && who == N48_LC_CLIENT && l->state == N48_LATCH_CLOSED && !intr) l->deadline = 0;
    return n48_latch_decide(l, who, now, intr);
}
static void m6_fail_not_sticky(n48_latch *l, uint32_t why)
{
    (void)l; (void)why;   // "a refused step is logged and the chain carries on" - the latch stays CLOSED and can still open
}
static uint32_t m7_decide_timeout_admits(n48_latch *l, uint32_t who, uint64_t now, uint32_t intr)
{
    const uint32_t d = n48_latch_decide(l, who, now, intr);
    return d == N48_LD_REFUSE_TIMEOUT ? N48_LD_ADMIT : d;   // fail OPEN after the wait
}
static uint32_t m8_plan_not_idempotent(uint32_t render_state, uint32_t neuter, uint32_t src_state)
{
    if (render_state != 1u && render_state != 2u) return N48_GP_REFUSE;
    (void)neuter; (void)src_state;
    return N48_GP_SET_NEUTER | N48_GP_INSTALL_SRC;   // "always arm both", re-installing the source hook every time
}
static uint32_t m9_decide_kernel_waits(n48_latch *l, uint32_t who, uint64_t now, uint32_t intr)
{
    // No bypass at all: Apple's own start (kernel_task, before accelerator-started) would wait for a chain that has not
    // been armed yet - the deadlock by construction.
    (void)who;
    return n48_latch_decide(l, N48_LC_CLIENT, now, intr);
}

static uint32_t m10_action_refusal_calls_apple(uint32_t d)
{
    // "after the deadline, let it through so WindowServer is not stuck" - a refusal that calls Apple is a fail-OPEN.
    return d == N48_LD_REFUSE_TIMEOUT ? (uint32_t)N48_LA_CALL_APPLE : n48_latch_action(d);
}
static uint32_t m11_action_waits_forever(uint32_t d)
{
    // "a refusal just keeps sleeping to the deadline" - the same as PARK in effect, EXCEPT that the deadline has passed,
    // so the sleep returns at once: a busy spin under the lock. Caught because a refusal must PARK, not SLEEP_TO_DEADLINE.
    return n48_latch_calls_apple(d) ? (uint32_t)N48_LA_CALL_APPLE : (uint32_t)N48_LA_SLEEP_TO_DEADLINE;
}

static const Ops kReal = { "REAL", n48_latch_init, n48_latch_fail, n48_latch_chain_end, n48_latch_decide, n48_gfxprot_plan,
                           n48_latch_action };

// ---------------------------------------------------------------------------------------------------------------------
// The checks.
// ---------------------------------------------------------------------------------------------------------------------
static n48_latch fresh(const Ops &o, uint32_t enforced = 1, uint32_t mode_ok = 1)
{
    n48_latch l {};
    o.init(&l, enforced, mode_ok, B);
    return l;
}

// C1: closed at load; a client waits; kernel_task bypasses.
static void c1_closed_at_load(const Ops &o)
{
    n48_latch l = fresh(o);
    expect_u("C1 state at load is CLOSED", l.state, N48_LATCH_CLOSED);
    expect_u("C1 a client at t=0 WAITS", o.decide(&l, N48_LC_CLIENT, 0, 0), N48_LD_WAIT);
    expect_u("C1 a client at t=500 (early, before any protection) WAITS", o.decide(&l, N48_LC_CLIENT, 500, 0), N48_LD_WAIT);
    expect_u("C1 kernel_task at t=0 BYPASSES", o.decide(&l, N48_LC_KERNEL, 0, 0), N48_LD_BYPASS);
}

// C2: opens on all four, then admits.
static void c2_opens_on_all_four(const Ops &o)
{
    n48_latch l = fresh(o);
    expect_u("C2 chain end with ALL four live -> OPEN", o.chain_end(&l, N48_LP_ALL), N48_LATCH_OPEN);
    expect_u("C2 live_at_open == ALL", l.live_at_open, N48_LP_ALL);
    expect_u("C2 a client after the open is ADMITTED", o.decide(&l, N48_LC_CLIENT, 10, 0), N48_LD_ADMIT);
    expect_u("C2 ... and calls Apple", n48_latch_calls_apple(o.decide(&l, N48_LC_CLIENT, 10, 0)), 1);
}

// C3: never opens on a partial set - every one of the 15 proper subsets, one at a time.
static void c3_never_partial(const Ops &o)
{
    uint32_t opened = 0, admitted = 0;
    for (uint32_t m = 0; m < N48_LP_ALL; m++) {
        n48_latch l = fresh(o);
        if (o.chain_end(&l, m) != N48_LATCH_FAILED) opened++;
        for (uint64_t t = 0; t < 3 * B; t += 997)
            if (n48_latch_calls_apple(o.decide(&l, N48_LC_CLIENT, t, 0))) admitted++;
    }
    expect_u("C3 of 15 partial sets, how many did NOT end FAILED", opened, 0);
    expect_u("C3 client calls that reached Apple after a partial set", admitted, 0);
    n48_latch d = fresh(o);
    expect_u("C3 drain-only (the design's named hazard) -> FAILED", o.chain_end(&d, N48_LP_SDMA_DRAIN), N48_LATCH_FAILED);
    expect_u("C3 drain+render+src without the neuter -> FAILED", o.chain_end(&(d = fresh(o)), 0x7u), N48_LATCH_FAILED);
    n48_latch x = fresh(o);
    expect_u("C3 bits outside the four do not matter (0x1f -> OPEN)", o.chain_end(&x, 0x1Fu), N48_LATCH_OPEN);
}

// C4: the timeout returns NULL, and the deadline is ONE for the boot.
static void c4_timeout(const Ops &o)
{
    n48_latch l = fresh(o);
    expect_u("C4 first waiter at t=100 WAITS (anchors the deadline)", o.decide(&l, N48_LC_CLIENT, 100, 0), N48_LD_WAIT);
    expect_u("C4 deadline anchored at 100 + bound", l.deadline, 100 + B);
    expect_u("C4 a second client at t=100+B/2 WAITS", o.decide(&l, N48_LC_CLIENT, 100 + B / 2, 0), N48_LD_WAIT);
    expect_u("C4 ... and did NOT move the deadline", l.deadline, 100 + B);
    expect_u("C4 at t=100+B-1 still WAITS", o.decide(&l, N48_LC_CLIENT, 100 + B - 1, 0), N48_LD_WAIT);
    const uint32_t at = o.decide(&l, N48_LC_CLIENT, 100 + B, 0);
    expect_u("C4 at the deadline -> REFUSED (timeout)", at, N48_LD_REFUSE_TIMEOUT);
    expect_u("C4 ... which does NOT call Apple (NULL)", n48_latch_calls_apple(at), 0);
    const uint32_t late = o.decide(&l, N48_LC_CLIENT, 100 + 2 * B, 0);
    expect_u("C4 a client arriving after the boot-wide deadline -> REFUSED at once", late, N48_LD_REFUSE_TIMEOUT);
    // Safety is decided by the protections, not by history: if the chain opens later, later clients are admitted.
    expect_u("C4 the chain opens after the timeout", o.chain_end(&l, N48_LP_ALL), N48_LATCH_OPEN);
    expect_u("C4 a client after that open is ADMITTED", o.decide(&l, N48_LC_CLIENT, 100 + 3 * B, 0), N48_LD_ADMIT);
}

// C5: a chain failure latches it closed for the boot.
static void c5_failure_sticky(const Ops &o)
{
    n48_latch l = fresh(o);
    o.fail(&l, N48_LF_CHAIN_STEP);
    expect_u("C5 after a chain failure the state is FAILED", l.state, N48_LATCH_FAILED);
    expect_u("C5 a client is REFUSED at once (no wait)", o.decide(&l, N48_LC_CLIENT, 5, 0), N48_LD_REFUSE_FAILED);
    expect_u("C5 a later ALL-live sample cannot open it", o.chain_end(&l, N48_LP_ALL), N48_LATCH_FAILED);
    uint32_t reached = 0;
    for (uint64_t t = 0; t < 3 * B; t += 1009) reached += n48_latch_calls_apple(o.decide(&l, N48_LC_CLIENT, t, 0));
    expect_u("C5 client calls reaching Apple over 3 bounds after the failure", reached, 0);
    n48_latch m = fresh(o, 1, 0);
    expect_u("C5 latch bit without the chain bits -> FAILED at load", m.state, N48_LATCH_FAILED);
    expect_u("C5 ... and a client is refused at once", o.decide(&m, N48_LC_CLIENT, 0, 0), N48_LD_REFUSE_FAILED);
    n48_latch p = fresh(o);
    expect_u("C5 an interrupted wait -> REFUSED", o.decide(&p, N48_LC_CLIENT, 7, 1), N48_LD_REFUSE_INTR);
}

// C6: the bypass is by identity, never by time: kernel_task in every state and at every time; a client never.
static void c6_bypass_identity(const Ops &o)
{
    uint32_t kernelNot = 0, clientBypass = 0;
    for (uint32_t s = 0; s < 3; s++) {
        n48_latch l = fresh(o);
        if (s == 1) o.chain_end(&l, N48_LP_ALL);
        if (s == 2) o.fail(&l, N48_LF_CHAIN_STEP);
        for (uint64_t t = 0; t < 3 * B; t += 211) {
            if (o.decide(&l, N48_LC_KERNEL, t, 0) != N48_LD_BYPASS) kernelNot++;
            if (o.decide(&l, N48_LC_CLIENT, t, 0) == N48_LD_BYPASS) clientBypass++;
        }
    }
    expect_u("C6 kernel_task decisions that were NOT a bypass (3 states x time sweep)", kernelNot, 0);
    expect_u("C6 client decisions that WERE a bypass (any state, any time)", clientBypass, 0);
}

// C7: THE INVARIANT, exhaustively over the reachable states: a client reaches Apple only when OPEN (or not enforced).
static void c7_invariant(const Ops &o)
{
    uint32_t leaks = 0, total = 0;
    for (uint32_t m = 0; m <= 0x1Fu; m++)
        for (uint32_t failFirst = 0; failFirst < 2; failFirst++)
            for (uint32_t intr = 0; intr < 2; intr++) {
                n48_latch l = fresh(o);
                // Clients arrive both before and after the chain's end.
                for (uint64_t t = 0; t < B / 2; t += 331) {
                    total++;
                    if (n48_latch_calls_apple(o.decide(&l, N48_LC_CLIENT, t, intr)) && l.state != N48_LATCH_OPEN) leaks++;
                }
                if (failFirst) o.fail(&l, N48_LF_CHAIN_STEP);
                o.chain_end(&l, m);
                for (uint64_t t = B / 2; t < 3 * B; t += 337) {
                    total++;
                    if (n48_latch_calls_apple(o.decide(&l, N48_LC_CLIENT, t, intr)) && l.state != N48_LATCH_OPEN) leaks++;
                }
            }
    expect_u("C7 client calls that reached Apple while the latch was not OPEN", leaks, 0);
    expect_u("C7 ... out of this many decisions (the sweep ran)", total > 5000, 1);
    n48_latch off = fresh(o, 0, 0);
    expect_u("C7 not enforced: a client passes (0.0.360 behaviour)", o.decide(&off, N48_LC_CLIENT, 0, 0), N48_LD_PASS_OFF);
}

// C8: the GFX arm plan is idempotent - the chain arms at [10], the recipe's `gfxneuter 1` then has nothing to do.
struct Gfx { uint32_t render, neuter, src, installs; };
static void apply(const Ops &o, Gfx &g)
{
    const uint32_t p = o.plan(g.render, g.neuter, g.src);
    if (p & N48_GP_REFUSE) return;
    if (p & N48_GP_SET_NEUTER) g.neuter = 1;
    if (p & N48_GP_INSTALL_SRC) { g.src = 1; g.installs++; }
}
static void c8_gfx_plan(const Ops &o)
{
    expect_u("C8 render drain off -> REFUSE", o.plan(0, 0, 0), N48_GP_REFUSE);
    expect_u("C8 render drain refused (3) -> REFUSE", o.plan(3, 0, 0), N48_GP_REFUSE);
    expect_u("C8 fresh arm at [10] -> SET_NEUTER | INSTALL_SRC", o.plan(1, 0, 0), N48_GP_SET_NEUTER | N48_GP_INSTALL_SRC);
    Gfx g { 1, 0, 0, 0 };
    apply(o, g);                                  // boot chain [10]
    const Gfx once = g;
    apply(o, g);                                  // the recipe's STEP `gfxneuter-1`
    apply(o, g);                                  // and again, for good measure
    expect_u("C8 after the chain's arm, the plan is EMPTY", o.plan(g.render, g.neuter, g.src), 0);
    expect_u("C8 two more `gfxneuter 1` left the neuter as the chain set it", g.neuter == once.neuter, 1);
    expect_u("C8 ... and installed the source hook exactly ONCE in total", g.installs, 1);
    expect_u("C8 a refused install (2) is retried, as the verb always did", o.plan(2, 1, 2), N48_GP_INSTALL_SRC);
}

// C9: what the kext DOES: a client that is not admitted never calls Apple and is never handed a NULL - every refusal PARKS
// (a NULL context panics in AMDAccelTask::free, M4-CONTEXT-LATCH.md section 3), and only the three admitting decisions call.
static void c9_action(const Ops &o)
{
    uint32_t bad = 0;
    for (uint32_t d = N48_LD_PASS_OFF; d <= N48_LD_REFUSE_INTR; d++) {
        const uint32_t a = o.action(d);
        const uint32_t want = n48_latch_calls_apple(d) ? (uint32_t)N48_LA_CALL_APPLE
                            : d == N48_LD_WAIT ? (uint32_t)N48_LA_SLEEP_TO_DEADLINE : (uint32_t)N48_LA_PARK;
        if (a != want) bad++;
    }
    expect_u("C9 decisions whose action is not CALL / SLEEP / PARK as required", bad, 0);
    expect_u("C9 a timed-out client PARKS (it does not call Apple, it is not handed NULL)", o.action(N48_LD_REFUSE_TIMEOUT),
             N48_LA_PARK);
    expect_u("C9 a client of a FAILED latch PARKS", o.action(N48_LD_REFUSE_FAILED), N48_LA_PARK);
    // A parked client is admitted if the chain opens late - safety by the protections, not by history.
    n48_latch l = fresh(o);
    (void)o.decide(&l, N48_LC_CLIENT, 0, 0);
    const uint32_t first = o.decide(&l, N48_LC_CLIENT, B + 1, 0);
    expect_u("C9 past the deadline: PARK", o.action(first), N48_LA_PARK);
    o.chain_end(&l, N48_LP_ALL);
    expect_u("C9 the chain opens late: the parked client's next decision CALLS Apple", o.action(o.decide(&l, N48_LC_CLIENT, B + 5, 0)),
             N48_LA_CALL_APPLE);
}

static void run_all(const Ops &o)
{
    c1_closed_at_load(o); c2_opens_on_all_four(o); c3_never_partial(o); c4_timeout(o);
    c5_failure_sticky(o); c6_bypass_identity(o); c7_invariant(o); c8_gfx_plan(o); c9_action(o);
}

int main()
{
    std::printf("== the REAL header\n");
    run_all(kReal);
    const int realFail = gFail, realRun = gRun;
    std::printf("%d check(s) on the real header, %d failed\n\n", realRun, realFail);

    const Ops mutants[] = {
        { "M1 opens on ANY live bit (a partial set)",            n48_latch_init, n48_latch_fail, m1_chain_end_any_bit, n48_latch_decide, n48_gfxprot_plan, n48_latch_action },
        { "M2 opens on the SDMA drain alone",                    n48_latch_init, n48_latch_fail, m2_chain_end_drain_only, n48_latch_decide, n48_gfxprot_plan, n48_latch_action },
        { "M3 bypass by TIMING (clients in the first 1000 ticks)", n48_latch_init, n48_latch_fail, n48_latch_chain_end, m3_decide_bypass_by_timing, n48_gfxprot_plan, n48_latch_action },
        { "M4 no timeout (a closed latch waits forever)",        n48_latch_init, n48_latch_fail, n48_latch_chain_end, m4_decide_no_timeout, n48_gfxprot_plan, n48_latch_action },
        { "M5 a per-caller deadline (N clients wait N bounds)",  n48_latch_init, n48_latch_fail, n48_latch_chain_end, m5_decide_per_caller_deadline, n48_gfxprot_plan, n48_latch_action },
        { "M6 a chain failure is not sticky",                    n48_latch_init, m6_fail_not_sticky, n48_latch_chain_end, n48_latch_decide, n48_gfxprot_plan, n48_latch_action },
        { "M7 the timeout fails OPEN (admits)",                  n48_latch_init, n48_latch_fail, n48_latch_chain_end, m7_decide_timeout_admits, n48_gfxprot_plan, n48_latch_action },
        { "M8 the GFX arm plan re-installs every time",          n48_latch_init, n48_latch_fail, n48_latch_chain_end, n48_latch_decide, m8_plan_not_idempotent, n48_latch_action },
        { "M9 no kernel_task bypass (Apple's start would wait)", n48_latch_init, n48_latch_fail, n48_latch_chain_end, m9_decide_kernel_waits, n48_gfxprot_plan, n48_latch_action },
        { "M10 a timed-out client is let through (fail open)",   n48_latch_init, n48_latch_fail, n48_latch_chain_end, n48_latch_decide, n48_gfxprot_plan, m10_action_refusal_calls_apple },
        { "M11 a refusal sleeps to a passed deadline (spin)",    n48_latch_init, n48_latch_fail, n48_latch_chain_end, n48_latch_decide, n48_gfxprot_plan, m11_action_waits_forever },
    };
    const unsigned nm = sizeof(mutants) / sizeof(mutants[0]);
    unsigned caught = 0;
    gQuiet = 1;
    for (unsigned i = 0; i < nm; i++) {
        gFail = 0; gRun = 0;
        run_all(mutants[i]);
        std::printf("mutant %-58s %s (%d of %d checks fail)\n", mutants[i].name, gFail ? "CAUGHT" : "*** SURVIVED ***", gFail, gRun);
        if (gFail) caught++;
    }
    std::printf("\nctx_latch: %d check(s) on the real header, %d failed; %u of %u planted defects caught. %s\n", realRun, realFail,
                caught, nm, (realFail == 0 && caught == nm) ? "N48-LATCH-TEST-PASS" : "N48-LATCH-TEST-FAIL");
    return (realFail == 0 && caught == nm) ? 0 : 1;
}
