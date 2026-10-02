// gfx_tgtsample_test.cpp — the offline proof for gfx_tgtsample.h (0.0.387). The properties under test are the
// project's own specification of the COMMIT-gate target before/after sample:
//
//     1. THE AFTER SAMPLE IS NEVER TAKEN WHILE THE COMMITTED FRAME IS STILL IN FLIGHT.
//     2. AN AFTER IS TAKEN ONLY AGAINST A PUBLISHED BEFORE, IN THE COMMITTED FRAME'S OWN CONTEXT, ONCE PER BOOT.
//     3. THE BEFORE IS TAKEN ONLY ON THE ARMED COMMIT+TRANSLATE PATH, WITHIN A BOUNDED READ BUDGET, AT A SANE VA.
//     4. THE DIGEST NEVER REPORTS A SATURATED DISTINCT COUNT AS AN EXACT ONE, AND NEVER READS PAST THE ROW.
//     5. "UNCHANGED" IS ONLY REACHABLE WHEN SOMETHING WAS ACTUALLY COMPARED.
//     6. NEITHER VERDICT NAMES A WRITER: CHANGED SAYS THE TARGET MOVED INSIDE THE WINDOW AND THAT THE WRITER IS NOT
//        ESTABLISHED (0.0.387); UNCHANGED CARRIES BOTH OF ITS READINGS.
//
// Property 1 is checked EXHAUSTIVELY, not by example: the 2048-case cross product of every perturbation of
// n48_ts_after_eval's inputs (including the boundary dt == timeout and a clock that went backwards) is compared
// against an INDEPENDENTLY WRITTEN predicate, so the test does not restate the implementation's clause order.
//
// PLANTED-DEFECT CONTROL (rule: a test no mutation can break is not testing anything). Nineteen mutants are run
// against the SAME checks, and each must be CAUGHT by at least one of them. D1 is the defect the brief names -
// "AFTER taken before end-of-flight" - and it is the non-vacuity proof for property 1. D18/D19 are the two
// starvation defects 0.0.410's K3 and 0.0.411's F2 exist to prevent.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_tgtsample_test.cpp -o /tmp/tstest && /tmp/tstest
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "gfx_tgtsample.h"
#include "gfx_fillset.h"   // 0.0.411: the replay drives the REAL fill-set step, not a copy of it

static int gFail = 0, gRun = 0, gQuiet = 0;

static void ck(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) std::printf("  FAIL %-66s got %llu want %llu\n", what,
                                 (unsigned long long)got, (unsigned long long)want);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The functions under test, reachable through pointers so a mutant can be swapped in.
// ---------------------------------------------------------------------------------------------------------------------
typedef uint32_t (*AfterFn)(const n48_ts_after_in *);
typedef uint32_t (*BeforeFn)(const n48_ts_before_in *);
typedef void     (*DigestFn)(const uint32_t *, uint32_t, n48_ts_row *);
typedef uint32_t (*ChangedFn)(const uint32_t *, const uint32_t *, uint32_t);
typedef uint32_t (*VerdictFn)(uint32_t, uint32_t);
typedef const char *(*NameFn)(uint32_t);
// 0.0.410: the first-commit skip, as a substitutable function so D18 can replace it.
typedef uint32_t (*SkipFn)(uint32_t, uint32_t);
// 0.0.411: the will-a-later-rung-refuse skip, substitutable so D19 can replace it.
typedef uint32_t (*SkipRefFn)(uint32_t, uint32_t);

static AfterFn   gAfter   = &n48_ts_after_eval;
static BeforeFn  gBefore  = &n48_ts_before_eval;
static DigestFn  gDigest  = &n48_ts_digest;
static ChangedFn gChanged = &n48_ts_changed;
static VerdictFn gVerdict = &n48_ts_verdict;
static NameFn    gName    = &n48_ts_verdict_name;
static SkipFn    gSkip    = &n48_ts_skip_commit;
static SkipRefFn gSkipRef = &n48_ts_skip_refused;

// ---------------------------------------------------------------------------------------------------------------------
// 1. GEOMETRY.
// ---------------------------------------------------------------------------------------------------------------------
static void checks_geometry()
{
    ck("blocks", N48_TS_BLOCKS, 3u);
    ck("rows", N48_TS_ROWS, 4u);
    ck("row dwords", N48_TS_ROW_DW, 256u);
    ck("block dwords = one 4 KiB page", N48_TS_BLOCK_DW, 1024u);
    ck("total dwords = 3 x 4 x 256", N48_TS_TOTAL_DW, 3072u);
    ck("block 0 at the base", n48_ts_block_off(0u), 0ull);
    ck("block 1 at 2 MiB", n48_ts_block_off(1u), 0x200000ull);
    ck("block 2 at 4 MiB", n48_ts_block_off(2u), 0x400000ull);
    ck("block 3 is REFUSED, not aliased onto block 0", n48_ts_block_off(3u), N48_TS_OFF_BAD);
    ck("block 99 is REFUSED", n48_ts_block_off(99u), N48_TS_OFF_BAD);
    ck("the span covers the last block's page", N48_TS_SPAN, 0x401000ull);

    ck("VA 0 refused", n48_ts_va_ok(0ull), 0u);
    ck("a real target VA accepted", n48_ts_va_ok(0x400800000ull), 1u);
    ck("a VA at 2^48 refused", n48_ts_va_ok(1ull << 48), 0u);
    ck("a VA whose base is fine but whose +4 MiB leaves the range is refused",
       n48_ts_va_ok((1ull << 48) - 0x1000ull), 0u);
    ck("a VA that wraps is refused", n48_ts_va_ok(0xFFFFFFFFFFFFF000ull), 0u);
    ck("the last VA whose whole window fits is accepted", n48_ts_va_ok((1ull << 48) - N48_TS_SPAN), 1u);
}

// ---------------------------------------------------------------------------------------------------------------------
// 2. THE DIGEST. Planted rows with known answers.
// ---------------------------------------------------------------------------------------------------------------------
static void checks_digest()
{
    static uint32_t row[N48_TS_ROW_DW];
    n48_ts_row d;

    // (a) all zero
    std::memset(row, 0, sizeof(row));
    gDigest(row, N48_TS_ROW_DW, &d);
    ck("all-zero row: got", d.got, N48_TS_ROW_DW);
    ck("all-zero row: nonzero", d.nonzero, 0u);
    ck("all-zero row: distinct", d.distinct, 1u);
    ck("all-zero row: not capped", d.capped, 0u);
    ck("all-zero row: first[0]", d.first[0], 0u);

    // (b) one repeated non-zero value
    for (uint32_t i = 0; i < N48_TS_ROW_DW; i++) row[i] = 0xDEADBEEFu;
    gDigest(row, N48_TS_ROW_DW, &d);
    ck("uniform row: nonzero", d.nonzero, N48_TS_ROW_DW);
    ck("uniform row: distinct", d.distinct, 1u);
    ck("uniform row: not capped", d.capped, 0u);
    ck("uniform row: first[3]", d.first[3], 0xDEADBEEFu);

    // (c) 256 distinct values - the cap must SATURATE and SAY SO
    for (uint32_t i = 0; i < N48_TS_ROW_DW; i++) row[i] = 0x1000u + i;
    gDigest(row, N48_TS_ROW_DW, &d);
    ck("all-distinct row: nonzero", d.nonzero, N48_TS_ROW_DW);
    ck("all-distinct row: distinct saturates at the cap", d.distinct, N48_TS_DISTINCT_CAP);
    ck("all-distinct row: the saturation is DECLARED", d.capped, 1u);
    ck("all-distinct row: first[1]", d.first[1], 0x1001u);

    // (d) exactly the cap's worth of distinct values, repeated - saturated but exact
    for (uint32_t i = 0; i < N48_TS_ROW_DW; i++) row[i] = 0x2000u + (i % N48_TS_DISTINCT_CAP);
    gDigest(row, N48_TS_ROW_DW, &d);
    ck("cap-many distinct: distinct", d.distinct, N48_TS_DISTINCT_CAP);
    ck("cap-many distinct: capped flag set (a LOWER BOUND, and the line must say so)", d.capped, 1u);

    // (e) one under the cap - exact, and NOT declared capped
    for (uint32_t i = 0; i < N48_TS_ROW_DW; i++) row[i] = 0x3000u + (i % (N48_TS_DISTINCT_CAP - 1u));
    gDigest(row, N48_TS_ROW_DW, &d);
    ck("cap-1 distinct: distinct", d.distinct, N48_TS_DISTINCT_CAP - 1u);
    ck("cap-1 distinct: not capped", d.capped, 0u);

    // (f) a SHORT read is never filled
    for (uint32_t i = 0; i < N48_TS_ROW_DW; i++) row[i] = 0xAAAAAAAAu;
    gDigest(row, 3u, &d);
    ck("short row: got", d.got, 3u);
    ck("short row: nonzero counts only what was read", d.nonzero, 3u);
    ck("short row: first[2] read", d.first[2], 0xAAAAAAAAu);
    ck("short row: first[3] NOT invented", d.first[3], 0u);

    // (g) got 0 and a null pointer both produce an empty digest, not a stale one
    d.got = 7u; d.nonzero = 7u; d.distinct = 7u; d.capped = 1u; d.first[0] = 7u;
    gDigest(row, 0u, &d);
    ck("got 0: digest cleared", d.got + d.nonzero + d.distinct + d.capped + d.first[0], 0u);
    d.got = 7u; d.nonzero = 7u;
    gDigest(nullptr, N48_TS_ROW_DW, &d);
    ck("null row: digest cleared", d.got + d.nonzero, 0u);

    // (h) a row length above the contract is CLAMPED, never read past (ASan would catch the alternative)
    gDigest(row, N48_TS_ROW_DW + 64u, &d);
    ck("over-long row is clamped to the row length", d.got, N48_TS_ROW_DW);
}

// ---------------------------------------------------------------------------------------------------------------------
// 3. THE DIFF AND THE VERDICT.
// ---------------------------------------------------------------------------------------------------------------------
static void checks_diff()
{
    static uint32_t a[N48_TS_ROW_DW], b[N48_TS_ROW_DW];
    for (uint32_t i = 0; i < N48_TS_ROW_DW; i++) { a[i] = i; b[i] = i; }
    ck("identical rows: 0 changed", gChanged(a, b, N48_TS_ROW_DW), 0u);
    b[0] = 0xFFFFFFFFu;
    ck("one dword differs: 1 changed", gChanged(a, b, N48_TS_ROW_DW), 1u);
    b[255] = 0xFFFFFFFFu;
    ck("two dwords differ: 2 changed", gChanged(a, b, N48_TS_ROW_DW), 2u);
    // A value that DECREASED still counts: a blit that writes smaller values is still a write.
    for (uint32_t i = 0; i < N48_TS_ROW_DW; i++) { a[i] = 0x1000u + i; b[i] = 0x10u + i; }
    ck("every dword decreased: all changed", gChanged(a, b, N48_TS_ROW_DW), N48_TS_ROW_DW);
    ck("null buffers change nothing", gChanged(nullptr, b, N48_TS_ROW_DW), 0u);
    ck("n = 0 changes nothing", gChanged(a, b, 0u), 0u);

    ck("cmp_n takes the shorter read", n48_ts_cmp_n(1024u, 7u), 7u);
    ck("cmp_n takes the shorter read (other way)", n48_ts_cmp_n(7u, 1024u), 7u);

    ck("nothing compared -> NO SAMPLE", gVerdict(0u, 0u), (uint64_t)N48_TS_V_NO_SAMPLE);
    ck("nothing compared but a changed count -> NO SAMPLE", gVerdict(0u, 5u), (uint64_t)N48_TS_V_NO_SAMPLE);
    ck("compared, none changed -> UNCHANGED", gVerdict(3072u, 0u), (uint64_t)N48_TS_V_UNCHANGED);
    ck("compared, one changed -> CHANGED", gVerdict(3072u, 1u), (uint64_t)N48_TS_V_CHANGED);
    ck("changed above sampled is impossible -> NO SAMPLE", gVerdict(10u, 11u), (uint64_t)N48_TS_V_NO_SAMPLE);
    // The UNCHANGED name must carry BOTH readings, because collapsing them is how the arm10 readback was misread.
    const char *u = gName(N48_TS_V_UNCHANGED);
    ck("UNCHANGED names the 'did not write' reading", std::strstr(u, "did not write") != nullptr, 1u);
    ck("UNCHANGED names the 'wrote Apple's bytes back' reading", std::strstr(u, "Apple's own bytes") != nullptr, 1u);

    // 0.0.387: THE CHANGED NAME MUST NOT NAME A WRITER. The AFTER is taken in a LATER decide-frame
    // pass, so the instrument cannot tell our rewrite's effect from Apple's own frames'. The old string said "OUR
    // COMMITTED FRAME WROTE ITS COLOUR TARGET" and arm13's author had to retract that in prose; these four checks are
    // what stop it coming back. This is the same property the UNCHANGED pair above enforces, on the other verdict.
    const char *c = gName(N48_TS_V_CHANGED);
    ck("CHANGED scopes the finding to the window", std::strstr(c, "INSIDE THE WINDOW") != nullptr, 1u);
    ck("CHANGED says the writer is NOT established", std::strstr(c, "NOT ESTABLISHED BY THIS INSTRUMENT") != nullptr, 1u);
    ck("CHANGED says the AFTER follows whatever ran in between",
       std::strstr(c, "whatever ran between the rewrite and the read") != nullptr, 1u);
    ck("CHANGED keeps the 'not evidence the pixels are right' reading",
       std::strstr(c, "NOT evidence that the pixels are RIGHT") != nullptr, 1u);
    ck("CHANGED never claims OUR frame wrote the target",
       std::strstr(c, "OUR COMMITTED FRAME WROTE") == nullptr, 1u);
}

// ---------------------------------------------------------------------------------------------------------------------
// 4. THE BEFORE RULE. Every clause falsified on its own must refuse; a TAKE must imply all of them.
// ---------------------------------------------------------------------------------------------------------------------
static n48_ts_before_in before_ok_in()
{
    n48_ts_before_in d;
    std::memset(&d, 0, sizeof(d));
    d.on = 1u; d.live = 1u; d.published = 0u; d.reads = 0u; d.cap = 4u; d.tgt_va = 0x400800000ull;
    return d;
}

static void checks_before()
{
    n48_ts_before_in d;

    d = before_ok_in();
    ck("clean: TAKE", gBefore(&d), (uint64_t)N48_TS_BF_TAKE);

    d = before_ok_in(); d.on = 0u;
    ck("switch off: refused", n48_ts_before_take(gBefore(&d)), 0u);
    d = before_ok_in(); d.live = 0u;
    ck("not live: refused", n48_ts_before_take(gBefore(&d)), 0u);
    d = before_ok_in(); d.published = 1u;
    ck("already published: refused", n48_ts_before_take(gBefore(&d)), 0u);
    d = before_ok_in(); d.reads = 4u;
    ck("budget spent: refused", n48_ts_before_take(gBefore(&d)), 0u);
    d = before_ok_in(); d.cap = 0u;
    ck("a zero budget is refused, not treated as unbounded", n48_ts_before_take(gBefore(&d)), 0u);
    d = before_ok_in(); d.tgt_va = 0ull;
    ck("no target VA: refused", n48_ts_before_take(gBefore(&d)), 0u);
    d = before_ok_in(); d.tgt_va = (1ull << 48) - 0x1000ull;
    ck("a target whose +4 MiB leaves the range: refused", n48_ts_before_take(gBefore(&d)), 0u);
    ck("a null input is refused", n48_ts_before_take(gBefore(nullptr)), 0u);

    // The other direction, over the 2^5 x 3 cross product: a TAKE implies every clause.
    for (uint32_t on = 0; on < 2u; on++)
    for (uint32_t live = 0; live < 2u; live++)
    for (uint32_t pub = 0; pub < 2u; pub++)
    for (uint32_t spent = 0; spent < 2u; spent++)
    for (uint32_t zerocap = 0; zerocap < 2u; zerocap++)
    for (uint32_t va = 0; va < 3u; va++) {
        d = before_ok_in();
        d.on = on; d.live = live; d.published = pub;
        d.reads = spent ? 4u : 0u;
        d.cap = zerocap ? 0u : 4u;
        d.tgt_va = (va == 0u) ? 0x400800000ull : (va == 1u ? 0ull : ((1ull << 48) - 0x1000ull));
        const uint32_t want = (on && live && !pub && !spent && !zerocap && va == 0u) ? 1u : 0u;
        ck("BEFORE cross product", n48_ts_before_take(gBefore(&d)), want);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 5. THE AFTER RULE — PROPERTY 1, EXHAUSTIVELY.
//
// The predicate below is written from the SPECIFICATION ("take it only when a published BEFORE exists, in the
// committed frame's own context, with a usable flight record and clock, and either an observed end of pipe or an
// expired bound"), NOT from n48_ts_after_eval's clause order. That is what makes the cross product a test rather
// than a restatement.
// ---------------------------------------------------------------------------------------------------------------------
static const uint64_t kTimeout = 2000000ull;   /* kKsFlightUs, the deferral's own bound */

static void checks_after()
{
    n48_ts_after_in d;
    uint32_t cases = 0;

    for (uint32_t on = 0; on < 2u; on++)
    for (uint32_t bef = 0; bef < 2u; bef++)
    for (uint32_t done = 0; done < 2u; done++)
    for (uint32_t root = 0; root < 2u; root++)
    for (uint32_t flight = 0; flight < 2u; flight++)
    for (uint32_t nowok = 0; nowok < 2u; nowok++)
    for (uint32_t startz = 0; startz < 2u; startz++)
    for (uint32_t eop = 0; eop < 2u; eop++)
    for (uint32_t toz = 0; toz < 2u; toz++)
    for (uint32_t dt = 0; dt < 4u; dt++) {
        std::memset(&d, 0, sizeof(d));
        d.on = on; d.before_ok = bef; d.done = done; d.same_root = root;
        d.flight = flight; d.now_ok = nowok; d.eop = eop;
        d.timeout_us = toz ? 0ull : kTimeout;
        d.start_us = startz ? 0ull : 1000000ull;
        // dt: 0 = the clock went BACKWARDS, 1 = same instant, 2 = one microsecond under the bound, 3 = exactly the bound
        const uint64_t base = d.start_us;
        d.now_us = (dt == 0u) ? (base ? base - 1ull : 0ull)
                 : (dt == 1u) ? base
                 : (dt == 2u) ? base + kTimeout - 1ull
                              : base + kTimeout;
        if (!nowok) d.now_us = 0ull;

        const bool clockUsable = nowok && d.start_us != 0ull && d.now_us >= d.start_us;
        const bool expired = clockUsable && d.timeout_us != 0ull && (d.now_us - d.start_us) >= d.timeout_us;
        const bool endOfFlight = (eop != 0u) || expired;
        const uint32_t want = (on && bef && !done && root && flight && clockUsable && d.timeout_us != 0ull &&
                               endOfFlight) ? 1u : 0u;
        ck("AFTER cross product", n48_ts_after_take(gAfter(&d)), want);
        cases++;
    }
    ck("AFTER cross product covered 2048 cases", cases, 2048u);

    // The named reasons, spot-checked so the log line cannot silently become the wrong sentence.
    std::memset(&d, 0, sizeof(d));
    d.on = 1u; d.before_ok = 1u; d.same_root = 1u; d.flight = 1u; d.now_ok = 1u;
    d.start_us = 1000000ull; d.now_us = 1000000ull; d.timeout_us = kTimeout;
    ck("still in flight -> IN_FLIGHT", gAfter(&d), (uint64_t)N48_TS_AF_IN_FLIGHT);
    d.eop = 1u;
    ck("end of pipe observed -> TAKE_EOP", gAfter(&d), (uint64_t)N48_TS_AF_TAKE_EOP);
    d.eop = 0u; d.now_us = 1000000ull + kTimeout;
    ck("bound expired -> TAKE_TIMEOUT", gAfter(&d), (uint64_t)N48_TS_AF_TAKE_TIMEOUT);
    d.eop = 1u;
    ck("both hold -> EOP is the truer reason", gAfter(&d), (uint64_t)N48_TS_AF_TAKE_EOP);
    d.eop = 0u; d.start_us = 0ull;
    ck("flight with no stamp -> TORN", gAfter(&d), (uint64_t)N48_TS_AF_TORN);
    ck("a null input -> TORN, never a take", n48_ts_after_take(gAfter(nullptr)), 0u);

    // The one line an operator reads when nothing retired must SAY no end-of-flight.
    ck("IN_FLIGHT's name says 'no end-of-flight'",
       std::strstr(n48_ts_after_name(N48_TS_AF_IN_FLIGHT), "no end-of-flight") != nullptr, 1u);
    ck("NO_FLIGHT's name says 'no end-of-flight'",
       std::strstr(n48_ts_after_name(N48_TS_AF_NO_FLIGHT), "no end-of-flight") != nullptr, 1u);
    ck("TAKE_TIMEOUT's name admits no end-of-pipe was seen",
       std::strstr(n48_ts_after_name(N48_TS_AF_TAKE_TIMEOUT), "NO end-of-pipe") != nullptr, 1u);
}

// 0.0.390 ( part 5 (v)) — THE SECOND SLOT, KEYED BY COMMIT TOKEN SEQ. named the defect: both one-shots
// latch at the FIRST N48_CM_OK, so on the two-frame chain the rule is for, the SECOND committed frame — the one the run
// is about — got no pair at all. These are not in the mutant sweep (the sweep swaps the five rule functions); the
// non-vacuity proof for them is a break-and-restore of the header itself.
static void checks_slots()
{
    uint32_t held[N48_TS_SLOTS] = { 0u, 0u }, seq[N48_TS_SLOTS] = { 0u, 0u };
    // ONE SLOT IS 0.0.389: slot 0 or nothing, ever.
    ck("slots: one slot answers slot 0", n48_ts_slot_for(held, seq, 1u, 7u), 0u);
    held[0] = 1u; seq[0] = 7u;
    ck("slots: the seq it already holds answers ITS slot", n48_ts_slot_for(held, seq, 1u, 7u), 0u);
    ck("slots: at one slot a second seq gets NOTHING", n48_ts_slot_for(held, seq, 1u, 9u), N48_TS_SLOT_NONE);
    // TWO SLOTS: the second seq gets the second slot, and the first is never reused.
    ck("slots: two slots give the second seq slot 1", n48_ts_slot_for(held, seq, 2u, 9u), 1u);
    ck("slots: and the first seq still answers slot 0", n48_ts_slot_for(held, seq, 2u, 7u), 0u);
    held[1] = 1u; seq[1] = 9u;
    ck("slots: a third seq gets NOTHING", n48_ts_slot_for(held, seq, 2u, 11u), N48_TS_SLOT_NONE);
    ck("slots: a held seq still answers, never a second copy", n48_ts_slot_for(held, seq, 2u, 9u), 1u);
    // Seq 0 is not an identity, and neither is a bad slot count or a null table.
    ck("slots: seq 0 is not a key", n48_ts_slot_for(held, seq, 2u, 0u), N48_TS_SLOT_NONE);
    ck("slots: zero slots is not room", n48_ts_slot_for(held, seq, 0u, 7u), N48_TS_SLOT_NONE);
    ck("slots: more slots than exist refuses", n48_ts_slot_for(held, seq, N48_TS_SLOTS + 1u, 7u), N48_TS_SLOT_NONE);
    ck("slots: a null table refuses", n48_ts_slot_for(nullptr, seq, 2u, 7u), N48_TS_SLOT_NONE);
    ck("slots: a null seq table refuses", n48_ts_slot_for(held, nullptr, 2u, 7u), N48_TS_SLOT_NONE);
    // `published` for the BEFORE rule: every OFFERED slot must have published before the rule says DONE.
    uint32_t pub[N48_TS_SLOTS] = { 0u, 0u };
    ck("slots: nothing published is not all published", n48_ts_all_published(pub, 2u), 0u);
    pub[0] = 1u;
    ck("slots: at ONE slot, slot 0's flag IS the answer", n48_ts_all_published(pub, 1u), 1u);
    ck("slots: at two slots one published is not all", n48_ts_all_published(pub, 2u), 0u);
    pub[1] = 1u;
    ck("slots: both published is all", n48_ts_all_published(pub, 2u), 1u);
    ck("slots: no slots offered refuses (not 'room')", n48_ts_all_published(pub, 0u), 1u);
    ck("slots: a null table refuses", n48_ts_all_published(nullptr, 2u), 1u);
    // And the BEFORE rule reads that answer: `published` 1 is DONE, 0 is TAKE, exactly as at 0.0.389.
    n48_ts_before_in d {};
    d.on = 1u; d.live = 1u; d.cap = 4u; d.tgt_va = 0x400800000ull;
    d.published = n48_ts_all_published(pub, 2u);
    ck("slots: with every slot published the BEFORE is DONE", gBefore(&d), N48_TS_BF_DONE);
    pub[1] = 0u;
    d.published = n48_ts_all_published(pub, 2u);
    ck("slots: with a slot free the BEFORE is TAKEN", gBefore(&d), N48_TS_BF_TAKE);
}

// 0.0.410 — WITH THE FILL-SET SWITCH (33) ON, THE SAMPLE STARTS AT THE SECOND COMMIT. The first commit
// is fill A, already proven; its slot would be spent on a surface the plane does not sample, and the plane - the frame the
// run is about - would get no pair. Under 33, commit 1 gets NO slot and commits 2 and 3 get slots 0 and 1, so the plane is
// read at end of flight before a present overwrites it. OFF, nothing is skipped and the order is 0.0.409's.
static void checks_k3()
{
    // OFF: never skipped, whatever the commit ordinal.
    ck("K3 OFF: commit 1 is not skipped", gSkip(0u, 0u), 0u);
    ck("K3 OFF: commit 2 is not skipped", gSkip(0u, 1u), 0u);
    ck("K3 OFF: commit 3 is not skipped", gSkip(0u, 2u), 0u);
    // ON: the FIRST commit (no prior commit) is skipped; the 2nd and 3rd are not.
    ck("K3 ON: commit 1 (0 prior commits) is SKIPPED", gSkip(1u, 0u), 1u);
    ck("K3 ON: commit 2 (1 prior commit) is TAKEN", gSkip(1u, 1u), 0u);
    ck("K3 ON: commit 3 (2 prior commits) is TAKEN", gSkip(1u, 2u), 0u);
    // THE SEQUENCE THE BRIEF NAMES, driven through the REAL slot picker: under 33, commit 1 gets no slot and commits 2
    // and 3 get slots 0 and 1. `prior` is the gate's own commit count brought forward, exactly as gXdCm.commits is.
    {
        uint32_t held[N48_TS_SLOTS] = { 0u, 0u }, seqOf[N48_TS_SLOTS] = { 0u, 0u };
        uint32_t prior = 0u, s1 = N48_TS_SLOT_NONE, s2 = N48_TS_SLOT_NONE, s3 = N48_TS_SLOT_NONE;
        if (!gSkip(1u, prior)) s1 = n48_ts_slot_for(held, seqOf, N48_TS_SLOTS, 1u);
        prior = 1u;
        if (!gSkip(1u, prior)) { s2 = n48_ts_slot_for(held, seqOf, N48_TS_SLOTS, 2u);
                                 if (s2 != N48_TS_SLOT_NONE) { held[s2] = 1u; seqOf[s2] = 2u; } }
        prior = 2u;
        if (!gSkip(1u, prior)) { s3 = n48_ts_slot_for(held, seqOf, N48_TS_SLOTS, 3u);
                                 if (s3 != N48_TS_SLOT_NONE) { held[s3] = 1u; seqOf[s3] = 3u; } }
        ck("K3 sequence: commit 1 gets NO slot", s1, (uint64_t)N48_TS_SLOT_NONE);
        ck("K3 sequence: commit 2 (fill B) gets slot 0", s2, 0u);
        ck("K3 sequence: commit 3 (the plane) gets slot 1", s3, 1u);
        ck("K3 sequence: both slots are then held", (uint64_t)(held[0] + held[1]), 2u);
        ck("K3 sequence: a 4th commit gets NOTHING", n48_ts_slot_for(held, seqOf, N48_TS_SLOTS, 4u),
           (uint64_t)N48_TS_SLOT_NONE);
    }
}

// 0.0.411 — NO BEFORE FOR A FRAME THE RESERVATION OR THE LUT RUNG WILL REFUSE, so the plane's slot is
// not starved. arm26's own frame order is replayed through the REAL n48_ts_skip_commit, n48_ts_skip_refused,
// n48_ts_before_eval, n48_ts_all_published, and the REAL fill-set step (n48_fs_identify_fill / n48_fs_step /
// n48_fs_commit) - not copies. f6 and f8 are reservation refusals (their CB0 is not a member), f13 is LUT-not-ready,
// f16 is fill B (slot 0) and f18 is the plane (slot 1). Under 0.0.410's K3 alone the four reads went to f6, f8, f13 and
// f16 and the plane got NOTHING; that is exactly the mutant D19 reproduces. The BEFORE read is bounded at
// kTsBeforeCap, spelled 4u here (the kext's constant lives in AppleHardwareHook.cpp, which this pure test cannot see).
static void checks_f2()
{
    struct Frame { const char *name; uint64_t cb0; uint32_t fillPs; uint32_t lutPlane; uint32_t lutReady; uint32_t gateOk; uint32_t seq; };
    const Frame F[] = {
        { "f1  fill A (member 0x400800000)",      0x400800000ull, 1u, 0u, 1u, 1u, 1u },
        { "f6  fill 0x402800000 (not a member)",  0x402800000ull, 1u, 0u, 1u, 0u, 2u },
        { "f8  fill 0x403800000 (not a member)",  0x403800000ull, 1u, 0u, 1u, 0u, 3u },
        { "f13 fill B (member) but LUT NOT READY", 0x404800000ull, 1u, 1u, 0u, 0u, 4u },
        { "f16 fill B (member), LUT ready",        0x404800000ull, 1u, 1u, 1u, 1u, 5u },
        { "f18 the PLANE",                         0x402800000ull, 0u, 1u, 1u, 1u, 6u },
    };
    const uint32_t NF = (uint32_t)(sizeof(F) / sizeof(F[0]));
    n48_fs fs; n48_fs_open(&fs); fs.on = 1u;
    uint32_t taken[N48_TS_SLOTS] = { 0u, 0u }, pub[N48_TS_SLOTS] = { 0u, 0u }, seqOf[N48_TS_SLOTS] = { 0u, 0u };
    uint32_t reads = 0u, committed = 0u, skipped = 0u;
    for (uint32_t i = 0; i < NF; i++) {
        const uint32_t isFill = n48_fs_identify_fill(&fs, 1u, F[i].fillPs, F[i].cb0);
        const uint32_t fsStep = n48_fs_step(&fs, 1u, isFill, F[i].cb0);   // once per judged frame, like the caller
        const uint32_t lutRefuse = (F[i].lutPlane && !F[i].lutReady) ? 1u : 0u;   // switch 32 assumed ON
        uint32_t slot = N48_TS_SLOT_NONE;
        // THE BEFORE READ: skipped by K3's first-commit rule, or by F2's will-a-later-rung-refuse rule. A skipped
        // frame still reaches the gate below - the skip is a decision about the SAMPLE, not about the commit.
        if (gSkip(fs.on, committed) || gSkipRef(fsStep == N48_FS_REFUSE, lutRefuse)) {
            skipped++;
        } else {
            n48_ts_before_in d {};
            d.on = 1u; d.live = 1u; d.reads = reads; d.cap = 4u;   /* kTsBeforeCap */
            d.tgt_va = F[i].cb0; d.published = n48_ts_all_published(pub, N48_TS_SLOTS);
            if (n48_ts_before_eval(&d) == N48_TS_BF_TAKE) {
                reads++;
                for (uint32_t k = 0; k < N48_TS_SLOTS; k++) if (!pub[k]) { slot = k; break; }   // the kext's picker
                if (slot != N48_TS_SLOT_NONE) { taken[slot] = 1u; seqOf[slot] = F[i].seq; }
            }
        }
        // THE GATE. Independent of the sample decision; a commit is what a member needs, and what publishes a BEFORE.
        if (F[i].gateOk) {
            committed++;
            if (slot != N48_TS_SLOT_NONE) pub[slot] = 1u;
            if (fsStep == N48_FS_RESERVE) (void)n48_fs_commit(&fs, F[i].cb0, F[i].seq);
        }
    }
    ck("F2: exactly 2 BEFORE reads for the whole arm26 order", reads, 2u);
    ck("F2: the reservation/LUT refusals are skipped (f1 f6 f8 f13)", skipped, 4u);
    ck("F2: fill B (f16) publishes slot 0", pub[0], 1u);
    ck("F2: ...and its token seq is f16's", seqOf[0], 5u);
    ck("F2: the PLANE (f18) publishes slot 1 - it is NOT starved", pub[1], 1u);
    ck("F2: ...and its token seq is the plane's", seqOf[1], 6u);
    ck("F2: both slots are held and published, so both pairs are complete",
       (uint64_t)(taken[0] + taken[1] + pub[0] + pub[1]), 4u);
    // OFF-IDENTITY: with both underlying rungs off, the new clause skips nothing.
    ck("F2 OFF: neither rung refusing -> no skip", gSkipRef(0u, 0u), 0u);
    ck("F2 ON: a reservation-refused frame -> skip", gSkipRef(1u, 0u), 1u);
    ck("F2 ON: a LUT-rung-refused frame -> skip", gSkipRef(0u, 1u), 1u);
    ck("F2 ON: both refusing -> skip", gSkipRef(1u, 1u), 1u);
}

// ---------------------------------------------------------------------------------------------------------------------
// L. 0.0.391 — THE READ-OUT'S LINES, MEASURED AT THEIR LONGEST POSSIBLE ARGUMENTS.
//
// split this instrument's AFTER verdict and its report twin and then wrote "worst case now 496 of 512".
// REFUTED it: ts_report_line's FIRST line still carried n48_ts_before_name AND n48_ts_after_name on top of its fixed
// text - 535 bytes at its shortest plausible arguments, 632 at worst - and every instance of it in arm18's own stream
// is exactly 511 bytes, cut mid-word. The lesson of and of this is the same: a length claim written in prose is
// not a check. Every format the read-out uses is now a macro in gfx_tgtsample.h and every one of them is measured here
// against the SAME cap the kext's logger applies, at the longest string each argument can be.
// ---------------------------------------------------------------------------------------------------------------------
static uint32_t longest_of(const char *(*f)(uint32_t), uint32_t n)
{
    uint32_t m = 0u;
    for (uint32_t i = 0; i <= n; i++) { const uint32_t l = (uint32_t)std::strlen(f(i)); if (l > m) m = l; }
    return m;
}
static void checks_lines()
{
    char b[4096];
    const uint32_t lb = longest_of(&n48_ts_before_name, N48_TS_BF_REASONS);
    const uint32_t la = longest_of(&n48_ts_after_name, N48_TS_AF_REASONS);
    const uint32_t lv = longest_of(&n48_ts_verdict_name, 8u);
    if (!gQuiet)
        std::printf("      longest BEFORE name %u B, AFTER name %u B, verdict name %u B; cap %u\n", lb, la, lv,
                    N48_LOG_CAP_BODY);
    // Each format at its worst: every %u at its widest, every %s at the longest string it can ever be handed.
    const char *worstBefore = "", *worstAfter = "", *worstVerdict = "";
    for (uint32_t i = 0; i <= N48_TS_BF_REASONS; i++)
        if (std::strlen(n48_ts_before_name(i)) == lb) worstBefore = n48_ts_before_name(i);
    for (uint32_t i = 0; i <= N48_TS_AF_REASONS; i++)
        if (std::strlen(n48_ts_after_name(i)) == la) worstAfter = n48_ts_after_name(i);
    for (uint32_t i = 0; i <= 8u; i++)
        if (std::strlen(n48_ts_verdict_name(i)) == lv) worstVerdict = n48_ts_verdict_name(i);
    const uint32_t U = 4294967295u;
    struct { const char *what; int n; } L[] = {
        { "L1 A (switch + geometry)",
          std::snprintf(b, sizeof b, N48_TS_RPT_A_FMT, "OFF (default)", "`gfxneuter 24` read it only, unchanged",
                        U, U, U, U, U) },
        { "L2 B (the counts)", std::snprintf(b, sizeof b, N48_TS_RPT_B_FMT, U, U, U, U, U) },
        { "L3 C (the BEFORE reason, on its own line)", std::snprintf(b, sizeof b, N48_TS_RPT_C_FMT, worstBefore) },
        { "L4 D (the AFTER reason, on its own line)", std::snprintf(b, sizeof b, N48_TS_RPT_D_FMT, worstAfter) },
        { "L5 the slots line", std::snprintf(b, sizeof b, N48_TS_RPT_SLOTS_FMT, U, U) },
        { "L6 a slot's verdict counts", std::snprintf(b, sizeof b, N48_TS_RPT_V_FMT, U, "UNCHANGED -", U, U, U) },
        { "L7 a slot with no AFTER", std::snprintf(b, sizeof b, N48_TS_RPT_NONE_FMT, U) },
        { "L8 a slot's long NAME line", std::snprintf(b, sizeof b, N48_TS_RPT_NAME_FMT, U, worstVerdict) },
        { "L9 the same line carrying the longest AFTER name instead",
          std::snprintf(b, sizeof b, N48_TS_RPT_NAME_FMT, U, worstAfter) },
    };
    for (const auto &l : L) {
        if (!gQuiet) std::printf("      %-44s %4d bytes\n", l.what, l.n);
        ck(l.what, (l.n > 0 && (unsigned)l.n <= N48_LOG_CAP_BODY) ? 1u : 0u, 1u);
    }
    // NON-VACUITY OF THE BOUND ITSELF: the 0.0.390 one-line form - A's fixed text plus BOTH long names - must NOT fit.
    // If this ever passes, the cap or the names have changed and the whole measurement needs redoing.
    { const int n = std::snprintf(b, sizeof b,
        "tgtsample: the COMMIT-gate target before/after sample (`gfxneuter 24 | M << 8`) is %s (%s). Geometry: "
        "%u block(s) at +0/+2 MiB/+4 MiB, %u rows of %u dwords each = %u dwords per sample; distinct counts "
        "saturate at %u and a saturated one is printed with a trailing '+'. BEFORE reads %u of %u, taken %u, "
        "PUBLISHED %u (last reason: %s). AFTER taken %u (last reason: %s).",
        "OFF (default)", "`gfxneuter 24` read it only, unchanged", 3u, 4u, 256u, 3072u, 32u, 0u, 4u, 0u, 0u,
        worstBefore, 0u, worstAfter);
      if (!gQuiet) std::printf("      %-44s %4d bytes (0.0.390's, the one measured cut at 511)\n",
                               "L10 the UNSPLIT form", n);
      ck("L10 0.0.390's unsplit first line does NOT fit - the split was necessary, not cosmetic",
         (n > (int)N48_LOG_CAP_BODY) ? 1u : 0u, 1u); }
}

// 0.0.420 — A FRAME THE REGION-MOVED CHECK WILL NEUTER SPENDS NO BEFORE SLOT. The check runs before
// the rewrite now, so the caller can skip the BEFORE read for a fence candidate whose ring region has moved (it will be
// neutered, so its BEFORE could never be compared). This is the SAME pure predicate as F2 - its SECOND argument now also
// carries the region-moved flag - so the only new property is that a region-moved candidate does not starve the plane.
static void checks_region_skip()
{
    ck("R OFF: nothing refusing -> no skip", gSkipRef(0u, 0u), 0u);
    ck("R a region-moved candidate -> skip (the same predicate)", gSkipRef(0u, 1u), 1u);
    // ONE slot's worth of budget, as the shape of kTsBeforeCap. Candidate A is region-moved: it must NOT take. Candidate B
    // is the plane: it must. The D20 mutant (the predicate ignores its second argument) takes for A, so the plane is
    // starved and BOTH checks below fail - the starvation named.
    uint32_t taken = 0u, region_took = 0u, plane_took = 0u;
    if (!gSkipRef(0u, 1u)) { region_took = 1u; taken++; }        // a region-moved fence candidate
    if (taken < 1u && !gSkipRef(0u, 0u)) { plane_took = 1u; taken++; }   // the plane, one slot left
    ck("R the region-moved candidate spends NO slot", region_took, 0u);
    ck("R the PLANE still gets the slot", plane_took, 1u);
}

static void all_checks()
{
    checks_geometry();
    checks_digest();
    checks_diff();
    checks_before();
    checks_after();
    checks_slots();
    checks_k3();
    checks_f2();
    checks_region_skip();
    checks_lines();
}

// ---------------------------------------------------------------------------------------------------------------------
// PLANTED DEFECTS. Each must be CAUGHT by at least one check above.
// ---------------------------------------------------------------------------------------------------------------------

// D1 — THE DEFECT THE BRIEF NAMES: the AFTER is taken before end-of-flight. The IN_FLIGHT clause is deleted and the
//      sample is taken as soon as a BEFORE exists. This is the non-vacuity proof for property 1.
static uint32_t d1_after_before_eof(const n48_ts_after_in *d)
{
    if (!d) return N48_TS_AF_TORN;
    if (!d->on) return N48_TS_AF_OFF;
    if (!d->before_ok) return N48_TS_AF_NO_BEFORE;
    if (d->done) return N48_TS_AF_DONE;
    if (!d->same_root) return N48_TS_AF_NO_CONTEXT;
    if (!d->flight) return N48_TS_AF_NO_FLIGHT;
    if (!d->now_ok || d->start_us == 0ull || d->now_us < d->start_us) return N48_TS_AF_TORN;
    if (d->timeout_us == 0ull) return N48_TS_AF_TORN;
    if (d->eop) return N48_TS_AF_TAKE_EOP;
    return N48_TS_AF_TAKE_TIMEOUT;                       /* *** taken while still in flight *** */
}
// D2 — the bound is tested with `>` instead of `>=`: the sample is not taken at exactly the bound.
static uint32_t d2_after_strict_bound(const n48_ts_after_in *d)
{
    if (!d) return N48_TS_AF_TORN;
    if (!d->on) return N48_TS_AF_OFF;
    if (!d->before_ok) return N48_TS_AF_NO_BEFORE;
    if (d->done) return N48_TS_AF_DONE;
    if (!d->same_root) return N48_TS_AF_NO_CONTEXT;
    if (!d->flight) return N48_TS_AF_NO_FLIGHT;
    if (!d->now_ok || d->start_us == 0ull || d->now_us < d->start_us) return N48_TS_AF_TORN;
    if (d->timeout_us == 0ull) return N48_TS_AF_TORN;
    if (d->eop) return N48_TS_AF_TAKE_EOP;
    if (d->now_us - d->start_us > d->timeout_us) return N48_TS_AF_TAKE_TIMEOUT;
    return N48_TS_AF_IN_FLIGHT;
}
// D3 — the TORN clause is dropped: a flight record with no stamp is trusted.
static uint32_t d3_after_no_torn(const n48_ts_after_in *d)
{
    if (!d) return N48_TS_AF_TORN;
    if (!d->on) return N48_TS_AF_OFF;
    if (!d->before_ok) return N48_TS_AF_NO_BEFORE;
    if (d->done) return N48_TS_AF_DONE;
    if (!d->same_root) return N48_TS_AF_NO_CONTEXT;
    if (!d->flight) return N48_TS_AF_NO_FLIGHT;
    if (d->timeout_us == 0ull) return N48_TS_AF_TORN;
    if (d->eop) return N48_TS_AF_TAKE_EOP;
    if (d->now_us - d->start_us >= d->timeout_us) return N48_TS_AF_TAKE_TIMEOUT;
    return N48_TS_AF_IN_FLIGHT;
}
// D4 — the context check is dropped: an AFTER is taken through some other client's page table.
static uint32_t d4_after_any_root(const n48_ts_after_in *d)
{
    if (!d) return N48_TS_AF_TORN;
    n48_ts_after_in c = *d; c.same_root = 1u;
    return n48_ts_after_eval(&c);
}
// D5 — the once-per-boot latch is dropped: a second AFTER overwrites the first.
static uint32_t d5_after_repeat(const n48_ts_after_in *d)
{
    if (!d) return N48_TS_AF_TORN;
    n48_ts_after_in c = *d; c.done = 0u;
    return n48_ts_after_eval(&c);
}
// D6 — the switch is honoured last: the instrument runs on a boot that never armed it.
static uint32_t d6_after_switch_last(const n48_ts_after_in *d)
{
    if (!d) return N48_TS_AF_TORN;
    n48_ts_after_in c = *d; c.on = 1u;
    return n48_ts_after_eval(&c);
}
// D7 — a zero bound is accepted as "expired", making the deferral window unbounded in the other direction.
static uint32_t d7_after_zero_bound(const n48_ts_after_in *d)
{
    if (!d) return N48_TS_AF_TORN;
    n48_ts_after_in c = *d; if (c.timeout_us == 0ull) c.timeout_us = 1ull;
    return n48_ts_after_eval(&c);
}
// D8 — an AFTER without a BEFORE: a sample with nothing to compare it to is reported as a result.
static uint32_t d8_after_no_before(const n48_ts_after_in *d)
{
    if (!d) return N48_TS_AF_TORN;
    n48_ts_after_in c = *d; c.before_ok = 1u;
    return n48_ts_after_eval(&c);
}
// D9 — the BEFORE runs off the armed COMMIT+TRANSLATE path.
static uint32_t d9_before_any_frame(const n48_ts_before_in *d)
{
    if (!d) return N48_TS_BF_OFF;
    n48_ts_before_in c = *d; c.live = 1u;
    return n48_ts_before_eval(&c);
}
// D10 — the VA guard is dropped: a +4 MiB read that leaves the addressable range is allowed.
static uint32_t d10_before_no_va(const n48_ts_before_in *d)
{
    if (!d) return N48_TS_BF_OFF;
    if (!d->on) return N48_TS_BF_OFF;
    if (!d->live) return N48_TS_BF_NOT_LIVE;
    if (d->published) return N48_TS_BF_DONE;
    if (d->cap == 0u || d->reads >= d->cap) return N48_TS_BF_CAP;
    return N48_TS_BF_TAKE;
}
// D11 — the per-boot read budget is dropped: an unbounded number of 12 KiB reads on the submit path.
static uint32_t d11_before_no_cap(const n48_ts_before_in *d)
{
    if (!d) return N48_TS_BF_OFF;
    n48_ts_before_in c = *d; c.reads = 0u; if (c.cap == 0u) c.cap = 4u;
    return n48_ts_before_eval(&c);
}
// D12 — a published BEFORE is overwritten by a later live frame.
static uint32_t d12_before_overwrite(const n48_ts_before_in *d)
{
    if (!d) return N48_TS_BF_OFF;
    n48_ts_before_in c = *d; c.published = 0u;
    return n48_ts_before_eval(&c);
}
// D13 — the digest saturates but does not DECLARE it: 32 is printed as an exact distinct count.
static void d13_digest_silent_cap(const uint32_t *p, uint32_t got, n48_ts_row *out)
{
    n48_ts_digest(p, got, out);
    if (out) out->capped = 0u;
}
// D14 — "nothing compared" is reported as UNCHANGED, which is exactly the error in miniature.
static uint32_t d14_verdict_empty_unchanged(uint32_t sampled, uint32_t changed)
{
    if (changed) return N48_TS_V_CHANGED;
    (void)sampled;
    return N48_TS_V_UNCHANGED;
}
// D15 — the diff counts only INCREASES, so a frame that wrote smaller values reads as UNCHANGED.
static uint32_t d15_changed_only_greater(const uint32_t *a, const uint32_t *b, uint32_t n)
{
    uint32_t i, c = 0u;
    if (!a || !b) return 0u;
    for (i = 0u; i < n; i++) if (b[i] > a[i]) c++;
    return c;
}

// D16 — 0.0.386's own CHANGED string, verbatim: it names OUR frame as the writer of the target. The AFTER is taken in
// a later decide-frame pass, so that is an attribution the instrument cannot make - this is the liar arm13 caught.
static const char *d16_changed_name_attributes(uint32_t v)
{
    if (v == N48_TS_V_CHANGED)
        return "TARGET CHANGED - OUR COMMITTED FRAME WROTE ITS COLOUR TARGET. This is evidence "
               "that step 7 happens at all; it is NOT evidence that the pixels are RIGHT";
    return n48_ts_verdict_name(v);
}
// D17 — the attribution is dropped but so is the reason: CHANGED with no statement that the writer is unestablished
// reads, to anyone scanning a log, exactly like the claim it replaced.
static const char *d17_changed_name_bare(uint32_t v)
{
    if (v == N48_TS_V_CHANGED) return "TARGET CHANGED INSIDE THE WINDOW";
    return n48_ts_verdict_name(v);
}

// D18 — 0.0.410: THE FIRST COMMIT IS NOT SKIPPED. With the fill-set switch ON the sample starts at commit
//   1 again, so fill A spends a slot and the plane gets none - the defect measured. This is 0.0.409's behaviour.
static uint32_t d18_skip_never(uint32_t, uint32_t) { return 0u; }

// D19 — 0.0.411: A FRAME A LATER RUNG WILL REFUSE IS STILL SAMPLED. This is 0.0.410's behaviour: the
//   BEFORE read is taken for f6/f8 (reservation) and f13 (LUT), spending the kTsBeforeCap budget so the plane (f18)
//   gets no slot -'s starvation. If the F2 checks cannot tell this apart, they are not testing F2.
static uint32_t d19_skip_refused_never(uint32_t, uint32_t) { return 0u; }

// D20 — 0.0.420: THE PREDICATE IGNORES ITS SECOND ARGUMENT. A region-moved fence candidate (the flag
//   now rides that argument) still spends a BEFORE slot, so a later frame is starved - the exact shape described.
static uint32_t d20_skip_second_ignored(uint32_t a, uint32_t) { return a ? 1u : 0u; }

struct Mut {
    const char *what;
    AfterFn af; BeforeFn bf; DigestFn df; ChangedFn cf; VerdictFn vf; NameFn nf; SkipFn sf;
    // 0.0.411: default nullptr keeps every entry above valid; D19 sets it.
    SkipRefFn srf = nullptr;
};

int main()
{
    std::printf("gfx_tgtsample_test - the COMMIT-gate target before/after sample (0.0.387)\n");
    all_checks();
    const int realFail = gFail, realRun = gRun;
    std::printf("real rule: %d checks, %d failures\n", realRun, realFail);

    const Mut mut[] = {
        { "D1 AFTER taken before end-of-flight (the brief's defect)", &d1_after_before_eof, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "D2 the bound is > instead of >=",                         &d2_after_strict_bound, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "D3 a flight record with no stamp is trusted",             &d3_after_no_torn, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "D4 an AFTER through another context's page table",        &d4_after_any_root, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "D5 a second AFTER overwrites the first",                  &d5_after_repeat, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "D6 the switch is honoured last (runs when OFF)",          &d6_after_switch_last, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "D7 a zero bound counts as expired",                       &d7_after_zero_bound, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "D8 an AFTER with no published BEFORE",                    &d8_after_no_before, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "D9 the BEFORE runs off the armed path",                   nullptr, &d9_before_any_frame, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "D10 the BEFORE's VA guard is dropped",                    nullptr, &d10_before_no_va, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "D11 the BEFORE read budget is dropped",                   nullptr, &d11_before_no_cap, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "D12 a published BEFORE is overwritten",                   nullptr, &d12_before_overwrite, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "D13 the digest saturates without declaring it",           nullptr, nullptr, &d13_digest_silent_cap, nullptr, nullptr, nullptr, nullptr },
        { "D14 nothing compared is reported as UNCHANGED",           nullptr, nullptr, nullptr, nullptr, &d14_verdict_empty_unchanged, nullptr, nullptr },
        { "D15 the diff counts only increases",                      nullptr, nullptr, nullptr, &d15_changed_only_greater, nullptr, nullptr, nullptr },
        { "D16 CHANGED names OUR frame as the writer (0.0.386's own string)",
                                                                     nullptr, nullptr, nullptr, nullptr, nullptr, &d16_changed_name_attributes, nullptr },
        { "D17 CHANGED drops the attribution but gives no reason",    nullptr, nullptr, nullptr, nullptr, nullptr, &d17_changed_name_bare, nullptr },
        { "D18 (K3) the first commit is NOT skipped (33 ON)",         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &d18_skip_never },
        { "D19 (F2) a frame a later rung will refuse IS still sampled", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &d19_skip_refused_never },
        { "D20 (E3) the skip predicate IGNORES the region-moved flag", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &d20_skip_second_ignored },
    };

    int caught = 0;
    for (const Mut &m : mut) {
        gQuiet = 1; gFail = 0; gRun = 0;
        gAfter   = m.af ? m.af : &n48_ts_after_eval;
        gBefore  = m.bf ? m.bf : &n48_ts_before_eval;
        gDigest  = m.df ? m.df : &n48_ts_digest;
        gChanged = m.cf ? m.cf : &n48_ts_changed;
        gVerdict = m.vf ? m.vf : &n48_ts_verdict;
        gName    = m.nf ? m.nf : &n48_ts_verdict_name;
        gSkip    = m.sf ? m.sf : &n48_ts_skip_commit;
        gSkipRef = m.srf ? m.srf : &n48_ts_skip_refused;
        all_checks();
        const int f = gFail, r = gRun;
        gAfter = &n48_ts_after_eval; gBefore = &n48_ts_before_eval; gDigest = &n48_ts_digest;
        gChanged = &n48_ts_changed;  gVerdict = &n48_ts_verdict;  gName = &n48_ts_verdict_name;
        gSkip = &n48_ts_skip_commit;  gSkipRef = &n48_ts_skip_refused;
        gQuiet = 0;
        std::printf("mutant %-58s %s (%d of %d checks fail)\n", m.what, f ? "CAUGHT" : "NOT CAUGHT", f, r);
        if (f) caught++;
    }
    const int nmut = (int)(sizeof(mut) / sizeof(mut[0]));
    std::printf("mutants caught %d/%d\n", caught, nmut);
    std::printf("%s\n", (realFail == 0 && caught == nmut) ? "gfx_tgtsample: PASS" : "gfx_tgtsample: FAIL");
    return (realFail || caught != nmut) ? 1 : 0;
}
