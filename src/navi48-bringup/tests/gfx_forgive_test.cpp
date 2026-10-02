// gfx_forgive_test.cpp — X9-F's safety proof, offline (notes/M4-PREARM-FORGIVENESS.md).
//
// THE PROPERTY UNDER TEST. X9-F is the ONE place in this kext where a safety check is deliberately made weaker, so the
// property is not "it forgives when it should" but:
//
//     "a pre-baseline dropped write is forgiven ONLY when every one of eleven clauses is POSITIVELY observed, and a
//      forgiveness once withdrawn can never be re-granted."
//
// Every clause, cleared on its own out of an otherwise complete request, must come out of n48_fg_eval as EXACTLY its own
// named clause and must grant NOTHING; a zero-initialised request must refuse rather than read as a clean world; and the
// amount granted must be the baseline total and never more.
//
// PLANTED-DEFECT CONTROL (rule: a test no mutation can break is not testing anything). Ten mutants are run against the
// SAME assertions. Five are the ones the brief named - forgiving a drop whose context is still alive, forgiving without
// the evidence, a forgiveness surviving a withdrawal, the accounting identity broken, and the switch ignored - and the
// sixth is the reviewer's own proposal as literally stated ("a surface in a dead context has no live mapping"), which
// is unsound because a shared IOSurface outlives the context that drew into it.
//
// THE POSITIVE CONTROL IS FILLED, NEVER RELAXED: `good()` below is a complete, PASSING request modelled on hp9's own
// numbers under the ONE recipe change that would make them forgivable (notes/M4-PREARM-FORGIVENESS.md, change 2).
// Without it every mutant would be "caught" by a rule that refuses everything, which tests nothing.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_forgive_test.cpp -o /tmp/fgtest && /tmp/fgtest
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include "gfx_forgive.h"
#include "gfx_dep.h"

static int gFail = 0, gRun = 0, gQuiet = 0;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) std::printf("FAIL  %-76s got %llu want %llu\n", what, (unsigned long long)got,
                                 (unsigned long long)want);
    } else if (!gQuiet) {
        std::printf("ok    %-76s %llu\n", what, (unsigned long long)got);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// hp9's OWN NUMBERS, under the one recipe change that would make them forgivable. 583 pre-baseline drops, ALL of them
// create #5 (`drawclient`), released BEFORE the committing context create #6 was created. This is the ONLY shape that
// may pass, and every check below is a single-field departure from it.
// ---------------------------------------------------------------------------------------------------------------------
static n48_fg_in good()
{
    n48_fg_in f {};
    f.gathered = 1u;
    f.enabled = 1u;
    f.base_valid = 1u;
    f.base_ctx_seq = 6u;            // WindowServer pid 1227, hp9's create #6
    f.base_total = 583u;            // X9's own detail 0x247 at ws#1
    f.base_own = 0u;                // the committing context had never had a frame dropped
    f.base_binds = 1u; f.base_rebinds = 0u; f.base_unbinds = 0u; f.base_gone = 0u;
    f.now_total = 583u;             // nothing dropped since the baseline
    f.other_classes = 0u;           // ring 0, walk/raced 0, unresolved 0, witness overflow 0 (hp9 at ws#1)
    f.ws_bound = 1u; f.ws_seq = 6u;
    f.binds = 1u; f.rebinds = 0u; f.unbinds = 0u; f.gone = 0u;
    f.census_complete = 1u; f.ctx_table_complete = 1u;
    f.rows = 1u; f.census_over = 0u;
    f.attributed = 583u; f.unattributed = 0u;
    f.row[0].seq = 5u;              // drawclient pid 506
    f.row[0].drops = 583u;
    f.row[0].dead = 1u;
    f.row[0].dead_evidence = N48_FG_DEAD_RELEASED | N48_FG_DEAD_ROOT_FREED;
    f.row[0].creates_at_release = 5u;   // released when 5 creates had happened, i.e. before create #6 existed
    return f;
}

// ---------------------------------------------------------------------------------------------------------------------
// THE MUTANTS. Each is a plausible way to write this wrong; each must be CAUGHT by at least one named check.
// ---------------------------------------------------------------------------------------------------------------------
enum {
    M_NONE = 0,
    M_ALIVE_OK,          /* the brief's #1: a drop whose context is STILL ALIVE is forgiven */
    M_NO_EVIDENCE_OK,    /* the brief's #2: `dead` believed with no positive teardown evidence behind it */
    M_WITHDRAW_FORGOTTEN,/* the brief's #3: a withdrawn forgiveness comes back */
    M_NO_IDENTITY,       /* the brief's #4: the accounting identity is not checked */
    M_SWITCH_IGNORED,    /* the brief's #5: the switch is ignored - the rule acts although it is OFF */
    M_NO_COEXIST,        /* the reviewer's proposal AS STATED: dead is enough, ordering not checked */
    M_OWN_DROPS_OK,      /* the committing context's own dropped frames are forgiven too */
    M_POST_BASELINE_OK,  /* `<=` instead of `==`: drops since the baseline tolerated */
    M_UNATTRIBUTED_OK,   /* a drop with no owning context is forgiven by an owner-keyed rule */
    M_ZERO_IS_CLEAN,     /* a request nobody filled reads as a request that passes */
    M_MUTANTS
};

static const char *m_name(int m)
{
    static const char *const n[M_MUTANTS] = {
        "(the real rule)",
        "an ALIVE owner is forgiven",
        "`dead` with no evidence is forgiven",
        "a WITHDRAWN forgiveness comes back",
        "the accounting identity is not checked",
        "the switch is IGNORED",
        "dead is enough - COEXISTED not checked",
        "the committing context's own drops forgiven",
        "drops since the baseline tolerated",
        "an unattributed drop is forgiven",
        "a zero request reads as PASS" };
    return (m >= 0 && m < M_MUTANTS) ? n[m] : "?";
}

/* The mutated rule. Each mutant reproduces n48_fg_eval with exactly one clause weakened; anything it does not weaken is
 * delegated to the real rule, so a mutant can only ever be MORE permissive than the real one. */
static uint32_t m_eval(int m, const n48_fg_in *f, uint64_t *forgive)
{
    if (forgive) *forgive = 0u;
    if (m == M_ZERO_IS_CLEAN && (!f || f->gathered != 1u)) { if (forgive && f) *forgive = f->base_total; return N48_FG_PASS; }
    if (m == M_SWITCH_IGNORED && f && f->enabled != 1u) {
        n48_fg_in g = *f; g.enabled = 1u; return n48_fg_eval(&g, forgive);
    }
    const uint32_t real = n48_fg_eval(f, forgive);
    if (!f) return real;
    switch (m) {
    case M_ALIVE_OK:
        if (real == N48_FG_CTX_ALIVE) {                       /* "it is only a client, it is gone anyway" */
            n48_fg_in g = *f;
            for (uint32_t i = 0; i < g.rows && i < N48_FG_ROWS; i++)
                if (g.row[i].dead != 1u) {
                    g.row[i].dead = 1u;
                    if (!g.row[i].dead_evidence) g.row[i].dead_evidence = N48_FG_DEAD_RELEASED;
                    if (!g.row[i].creates_at_release) g.row[i].creates_at_release = 1u;
                }
            return n48_fg_eval(&g, forgive);
        }
        break;
    case M_NO_EVIDENCE_OK:
        if (real == N48_FG_CTX_NO_EVIDENCE) {                  /* `dead` taken on trust */
            n48_fg_in g = *f;
            for (uint32_t i = 0; i < g.rows && i < N48_FG_ROWS; i++) {
                if (!g.row[i].dead_evidence) g.row[i].dead_evidence = N48_FG_DEAD_RELEASED;
                if (!g.row[i].creates_at_release) g.row[i].creates_at_release = 1u;
            }
            return n48_fg_eval(&g, forgive);
        }
        break;
    case M_NO_IDENTITY:
        if (real == N48_FG_ACCOUNT || real == N48_FG_ROW_EMPTY) {
            n48_fg_in g = *f;                                 /* "the rows are what they are" */
            uint64_t sum = 0u;
            for (uint32_t i = 0; i < g.rows && i < N48_FG_ROWS; i++) {
                if (!g.row[i].seq) g.row[i].seq = 5u;
                if (!g.row[i].drops) g.row[i].drops = 1u;
                sum += g.row[i].drops;
            }
            g.attributed = sum;
            g.base_total = sum + g.unattributed;
            g.now_total = g.base_total;
            return n48_fg_eval(&g, forgive);
        }
        break;
    case M_NO_COEXIST:
        if (real == N48_FG_COEXISTED) {
            n48_fg_in g = *f;                                 /* clause 9 deleted: dead is enough */
            for (uint32_t i = 0; i < g.rows && i < N48_FG_ROWS; i++) g.row[i].creates_at_release = 1u;
            return n48_fg_eval(&g, forgive);
        }
        break;
    case M_OWN_DROPS_OK:
        if (real == N48_FG_OWN_DROPS) {
            n48_fg_in g = *f;                                 /* clause 4 deleted */
            g.base_own = 0u;
            for (uint32_t i = 0; i < g.rows && i < N48_FG_ROWS; i++)
                if (g.row[i].seq == g.base_ctx_seq) g.row[i].seq = g.base_ctx_seq + 100u;
            return n48_fg_eval(&g, forgive);
        }
        break;
    case M_POST_BASELINE_OK:
        if (real == N48_FG_POST_BASELINE && f->now_total > f->base_total) {
            n48_fg_in g = *f; g.now_total = g.base_total;     /* `<=`: "a few more cannot matter" */
            return n48_fg_eval(&g, forgive);
        }
        break;
    case M_UNATTRIBUTED_OK:
        if (real == N48_FG_UNATTRIBUTED) {
            n48_fg_in g = *f;                                 /* clause 6's unattributed half deleted */
            g.base_total -= g.unattributed;
            g.now_total = g.base_total;
            g.unattributed = 0u;
            return n48_fg_eval(&g, forgive);
        }
        break;
    default: break;
    }
    return real;
}

/* The mutated latch. M_WITHDRAW_FORGOTTEN is the one that matters here: `withdrawn` cleared, so the grant comes back. */
static uint32_t m_step(int m, n48_fg_latch *l, const n48_fg_in *f, uint64_t *forgive)
{
    if (m == M_WITHDRAW_FORGOTTEN && l) l->withdrawn = 0u;
    if (m == M_NONE || m == M_WITHDRAW_FORGOTTEN) return n48_fg_step(l, f, forgive);
    /* every other mutant is in the rule, so run the real latch over the mutated rule */
    uint64_t want = 0u;
    if (forgive) *forgive = 0u;
    if (!l) return N48_FG_NOT_GATHERED;
    l->calls++;
    if (l->withdrawn) { l->clause = N48_FG_WITHDRAWN; return N48_FG_WITHDRAWN; }
    const uint32_t c = m_eval(m, f, &want);
    if (c == N48_FG_PASS) {
        if (l->granted && want != l->amount) { l->withdrawn = 1u; l->granted = 0u; return N48_FG_AMOUNT_MOVED; }
        if (!l->granted) { l->granted = 1u; l->amount = want; l->grants++; }
        if (forgive) *forgive = l->amount;
        return N48_FG_PASS;
    }
    if (l->granted) { l->withdrawn = 1u; l->granted = 0u; l->clause = c; return N48_FG_WITHDRAWN; }
    l->clause = c;
    return c;
}

// ---------------------------------------------------------------------------------------------------------------------
static int checks(int m)
{
    const int before = gFail;
    char buf[192];

    // ---- THE POSITIVE CONTROL, FILLED. Without this every mutant is "caught" by a rule that refuses everything. ----
    {
        uint64_t g = 0u;
        const n48_fg_in f = good();
        std::snprintf(buf, sizeof(buf), "complete request PASSES                          %s", m_name(m));
        expect_u(buf, m_eval(m, &f, &g), N48_FG_PASS);
        std::snprintf(buf, sizeof(buf), "... and grants EXACTLY the baseline total (583)   %s", m_name(m));
        expect_u(buf, g, 583u);
    }

    // ---- A ZERO REQUEST REFUSES. A request nobody filled is not a world in which nothing was dropped. ----
    {
        uint64_t g = 0u;
        n48_fg_in f {};
        std::snprintf(buf, sizeof(buf), "a zero request -> not-gathered                    %s", m_name(m));
        expect_u(buf, m_eval(m, &f, &g), N48_FG_NOT_GATHERED);
        std::snprintf(buf, sizeof(buf), "... and grants NOTHING                            %s", m_name(m));
        expect_u(buf, g, 0u);
        std::snprintf(buf, sizeof(buf), "a NULL request -> not-gathered                    %s", m_name(m));
        expect_u(buf, m_eval(m, nullptr, &g), N48_FG_NOT_GATHERED);
    }

    // ---- EVERY CLAUSE, CLEARED ALONE, NAMES ITSELF AND GRANTS NOTHING. ----
    {
        struct Case { const char *name; uint32_t clause; void (*bend)(n48_fg_in &); };
        static const Case cases[] = {
            { "switch OFF",                 N48_FG_DISABLED,          [](n48_fg_in &f) { f.enabled = 0u; } },
            { "no baseline",                N48_FG_NO_BASELINE,       [](n48_fg_in &f) { f.base_valid = 0u; } },
            { "baseline names no context",  N48_FG_BASE_CTX,          [](n48_fg_in &f) { f.base_ctx_seq = 0u; } },
            { "not bound now",              N48_FG_NOT_BOUND,         [](n48_fg_in &f) { f.ws_bound = 0u; } },
            { "bound to another context",   N48_FG_BASE_CTX,          [](n48_fg_in &f) { f.ws_seq = 9u; } },
            { "a rebind since",             N48_FG_BINDING_MOVED,     [](n48_fg_in &f) { f.rebinds = 1u; } },
            { "an unbind since",            N48_FG_BINDING_MOVED,     [](n48_fg_in &f) { f.unbinds = 1u; } },
            { "a `gone` since",             N48_FG_BINDING_MOVED,     [](n48_fg_in &f) { f.gone = 1u; } },
            { "a second bind since",        N48_FG_BINDING_MOVED,     [](n48_fg_in &f) { f.binds = 2u; } },
            { "nothing to forgive",         N48_FG_NOTHING_TO_FORGIVE,[](n48_fg_in &f) { f.base_total = 0u; f.now_total = 0u;
                                                                                        f.attributed = 0u; f.rows = 0u; } },
            { "a drop SINCE the baseline",  N48_FG_POST_BASELINE,     [](n48_fg_in &f) { f.now_total = 584u; } },
            { "the count went DOWN",        N48_FG_POST_BASELINE,     [](n48_fg_in &f) { f.now_total = 582u; } },
            { "the committing ctx's own",   N48_FG_OWN_DROPS,         [](n48_fg_in &f) { f.base_own = 1u; } },
            { "the committing ctx in a row",N48_FG_OWN_DROPS,         [](n48_fg_in &f) { f.row[0].seq = 6u; } },
            { "ring-neuter already dirty",  N48_FG_OTHER_CLASSES,     [](n48_fg_in &f) { f.other_classes = 1u; } },
            { "census not complete",        N48_FG_CENSUS_INCOMPLETE, [](n48_fg_in &f) { f.census_complete = 0u; } },
            { "context table overflowed",   N48_FG_CENSUS_INCOMPLETE, [](n48_fg_in &f) { f.ctx_table_complete = 0u; } },
            { "census overflowed",          N48_FG_CENSUS_INCOMPLETE, [](n48_fg_in &f) { f.census_over = 1u; } },
            { "no census rows at all",      N48_FG_CENSUS_INCOMPLETE, [](n48_fg_in &f) { f.rows = 0u; } },
            { "more rows than the table",   N48_FG_CENSUS_INCOMPLETE, [](n48_fg_in &f) { f.rows = N48_FG_ROWS + 1u; } },
            { "an unattributed drop",       N48_FG_UNATTRIBUTED,      [](n48_fg_in &f) { f.unattributed = 1u;
                                                                                        f.attributed = 582u;
                                                                                        f.row[0].drops = 582u; } },
            { "rows do not sum",            N48_FG_ACCOUNT,           [](n48_fg_in &f) { f.row[0].drops = 500u; } },
            { "census below the total",     N48_FG_ACCOUNT,           [](n48_fg_in &f) { f.attributed = 500u;
                                                                                        f.row[0].drops = 500u; } },
            { "an empty census row",        N48_FG_ROW_EMPTY,         [](n48_fg_in &f) { f.row[0].seq = 0u; } },
            { "a row with no drops",        N48_FG_ROW_EMPTY,         [](n48_fg_in &f) { f.row[0].drops = 0u;
                                                                                        f.attributed = 0u; } },
            { "the owner is STILL ALIVE",   N48_FG_CTX_ALIVE,         [](n48_fg_in &f) { f.row[0].dead = 0u; } },
            { "dead with NO evidence",      N48_FG_CTX_NO_EVIDENCE,   [](n48_fg_in &f) { f.row[0].dead_evidence = 0u; } },
            { "dead with no release order", N48_FG_CTX_NO_EVIDENCE,   [](n48_fg_in &f) { f.row[0].creates_at_release = 0u; } },
            { "the contexts COEXISTED",     N48_FG_COEXISTED,         [](n48_fg_in &f) { f.row[0].creates_at_release = 6u; } },
            { "hp9 as run: released later", N48_FG_COEXISTED,         [](n48_fg_in &f) { f.row[0].creates_at_release = 7u; } },
        };
        for (const Case &c : cases) {
            n48_fg_in f = good();
            c.bend(f);
            uint64_t g = 0u;
            const uint32_t got = m_eval(m, &f, &g);
            std::snprintf(buf, sizeof(buf), "%-30s -> %-26s %s", c.name, n48_fg_clause_name(c.clause), m_name(m));
            expect_u(buf, got, c.clause);
            std::snprintf(buf, sizeof(buf), "%-30s grants NOTHING             %s", c.name, m_name(m));
            expect_u(buf, g, 0u);
        }
    }

    // ---- hp9 AS RUN, END TO END: the drawclient is ALIVE at ws#1, so the answer is CTX_ALIVE and 0 forgiven. ----
    {
        n48_fg_in f = good();
        f.row[0].dead = 0u; f.row[0].dead_evidence = 0u; f.row[0].creates_at_release = 0u;
        uint64_t g = 0u;
        std::snprintf(buf, sizeof(buf), "hp9 AS RUN -> an-owning-context-is-ALIVE          %s", m_name(m));
        expect_u(buf, m_eval(m, &f, &g), N48_FG_CTX_ALIVE);
        std::snprintf(buf, sizeof(buf), "hp9 AS RUN forgives 0 of 583                      %s", m_name(m));
        expect_u(buf, g, 0u);
    }

    // ---- THE MONOTONE LATCH (clause 11). ----
    {
        n48_fg_latch l {};
        const n48_fg_in f = good();
        uint64_t g = 0u;
        std::snprintf(buf, sizeof(buf), "latch: first call GRANTS                          %s", m_name(m));
        expect_u(buf, m_step(m, &l, &f, &g), N48_FG_PASS);
        std::snprintf(buf, sizeof(buf), "latch: ... 583                                    %s", m_name(m));
        expect_u(buf, g, 583u);
        std::snprintf(buf, sizeof(buf), "latch: a second identical call is the SAME grant   %s", m_name(m));
        expect_u(buf, m_step(m, &l, &f, &g), N48_FG_PASS);
        std::snprintf(buf, sizeof(buf), "latch: grants counted ONCE                        %s", m_name(m));
        expect_u(buf, l.grants, 1u);

        // the evidence fails: the owner comes back (a recycled record), so the grant is WITHDRAWN
        n48_fg_in bad = good();
        bad.row[0].dead = 0u;
        std::snprintf(buf, sizeof(buf), "latch: the owner is alive now -> WITHDRAWN         %s", m_name(m));
        expect_u(buf, m_step(m, &l, &bad, &g), N48_FG_WITHDRAWN);
        std::snprintf(buf, sizeof(buf), "latch: ... and grants NOTHING                     %s", m_name(m));
        expect_u(buf, g, 0u);
        std::snprintf(buf, sizeof(buf), "latch: the withdrawal names its cause              %s", m_name(m));
        expect_u(buf, l.clause, N48_FG_CTX_ALIVE);
        // THE DEFECT THIS EXISTS FOR: the complete request must NEVER be granted again.
        std::snprintf(buf, sizeof(buf), "latch: a COMPLETE request after a withdrawal REFUSES %s", m_name(m));
        expect_u(buf, m_step(m, &l, &f, &g), N48_FG_WITHDRAWN);
        std::snprintf(buf, sizeof(buf), "latch: ... and STILL grants nothing               %s", m_name(m));
        expect_u(buf, g, 0u);
        std::snprintf(buf, sizeof(buf), "latch: withdrawn is never cleared                 %s", m_name(m));
        expect_u(buf, l.withdrawn, 1u);
        std::snprintf(buf, sizeof(buf), "latch: still exactly ONE grant this boot          %s", m_name(m));
        expect_u(buf, l.grants, 1u);
    }
    // Turning the switch OFF after a grant withdraws, and turning it back ON does not re-grant.
    {
        n48_fg_latch l {};
        n48_fg_in f = good();
        uint64_t g = 0u;
        (void)m_step(m, &l, &f, &g);
        f.enabled = 0u;
        std::snprintf(buf, sizeof(buf), "latch: switch OFF after a grant -> WITHDRAWN       %s", m_name(m));
        expect_u(buf, m_step(m, &l, &f, &g), N48_FG_WITHDRAWN);
        f.enabled = 1u;
        std::snprintf(buf, sizeof(buf), "latch: switching it back ON does NOT re-grant      %s", m_name(m));
        expect_u(buf, m_step(m, &l, &f, &g), N48_FG_WITHDRAWN);
        std::snprintf(buf, sizeof(buf), "latch: ... 0 forgiven                             %s", m_name(m));
        expect_u(buf, g, 0u);
    }
    // A baseline that MOVED under a standing grant: the amount changes, which withdraws.
    {
        n48_fg_latch l {};
        n48_fg_in f = good();
        uint64_t g = 0u;
        (void)m_step(m, &l, &f, &g);
        f.base_total = 600u; f.now_total = 600u; f.attributed = 600u; f.row[0].drops = 600u;
        const uint32_t got = m_step(m, &l, &f, &g);
        std::snprintf(buf, sizeof(buf), "latch: the baseline MOVED -> amount-moved          %s", m_name(m));
        expect_u(buf, got == N48_FG_AMOUNT_MOVED || got == N48_FG_WITHDRAWN, 1u);
        std::snprintf(buf, sizeof(buf), "latch: ... and nothing is forgiven                 %s", m_name(m));
        expect_u(buf, g, 0u);
        std::snprintf(buf, sizeof(buf), "latch: a later complete request still REFUSES      %s", m_name(m));
        expect_u(buf, m_step(m, &l, &f, &g), N48_FG_WITHDRAWN);
    }
    // A null latch is not a latch.
    {
        uint64_t g = 0u;
        const n48_fg_in f = good();
        expect_u("latch: a NULL latch refuses", n48_fg_step(nullptr, &f, &g), N48_FG_NOT_GATHERED);
        expect_u("latch: ... and grants nothing", g, 0u);
    }
    // Clause names exist, are distinct from PASS, and every index is named.
    if (m == M_NONE) {
        expect_u("clause 0 is named PASS", std::strcmp(n48_fg_clause_name(N48_FG_PASS), "PASS") == 0, 1u);
        for (uint32_t c = 1; c < N48_FG_CLAUSES; c++) {
            std::snprintf(buf, sizeof(buf), "clause %u has a name that is not PASS and not '?'", c);
            expect_u(buf, std::strcmp(n48_fg_clause_name(c), "PASS") != 0 &&
                          std::strcmp(n48_fg_clause_name(c), "?") != 0, 1u);
        }
        expect_u("an index past the list is '?'", std::strcmp(n48_fg_clause_name(N48_FG_CLAUSES), "?") == 0, 1u);
    }
    return gFail - before;
}

// ---------------------------------------------------------------------------------------------------------------------
// THE SECOND HALF: what gfx_dep.h's FILL does with a verdict and an amount. The rule is only half the safety; the other
// half is that the fill refuses to act on a malformed grant, and that with the switch OFF it is 0.0.371 exactly.
// ---------------------------------------------------------------------------------------------------------------------
static n48_dep_src clean_src()
{
    n48_dep_src s {};
    s.gathered = 1u; s.snap_ok = 1u;
    s.src_install = 1u; s.ring_state = 1u; s.ring_hooked = 1u; s.sdma_state = 2u;
    s.stall_armed = 1u; s.stall_read_ok = 1u;
    s.pre_walked = 1u; s.pre_ibs = 0u;
    s.q_hook_live = 1u; s.q_tally_ok = 1u; s.fault_read_ok = 1u;
    s.v[N48_DEPC_Q_STARTS] = 4u; s.v[N48_DEPC_Q_KNOWN_TYPE] = 4u;
    return s;
}

/* A world with `n` source-neuters and nothing else wrong. */
static n48_dep_src dirty_src(uint64_t n)
{
    n48_dep_src s = clean_src();
    s.v[N48_DEPC_SRC_CALLS] = n;
    s.v[N48_DEPC_SRC_NEUTERED_OTHER] = n;
    return s;
}

enum { F_NONE = 0, F_NO_SWITCH, F_ANY_CLAUSE, F_SATURATES, F_NO_IDENTITY_BIT, F_MUTANTS };
static const char *f_name(int m)
{
    static const char *const n[F_MUTANTS] = {
        "(the real fill)", "the fill subtracts with the switch OFF", "the fill subtracts on ANY verdict",
        "the fill SATURATES instead of refusing", "a malformed grant breaks no identity" };
    return (m >= 0 && m < F_MUTANTS) ? n[m] : "?";
}

static void f_fill(int m, const n48_dep_src *s0, n48_dep_mono *mono, n48_dep_world *w)
{
    n48_dep_src s = *s0;
    if (m == F_NO_SWITCH && s.fg_forgive) s.fg_enabled = 1u;
    if (m == F_ANY_CLAUSE && s.fg_forgive) s.fg_clause = 0u;
    n48_dep_fill(&s, mono, w);
    const uint64_t sn = s.v[N48_DEPC_SRC_NEUTERED] + s.v[N48_DEPC_SRC_NEUTERED_OTHER];
    if (m == F_SATURATES && s.fg_enabled == 1u && s.fg_clause == 0u && s.fg_forgive > sn) {
        w->source_neuters = 0u; w->forgiven = sn;             /* "clamp it, a bigger grant can only mean cleaner" */
        w->unaccounted &= ~(uint64_t)N48_DEP_ID_FORGIVE;
    }
    if (m == F_NO_IDENTITY_BIT) w->unaccounted &= ~(uint64_t)N48_DEP_ID_FORGIVE;
}

static int fill_checks(int m)
{
    const int before = gFail;
    char buf[192];
    uint64_t d = 0u;

    // THE INERT DIRECTION: switch off, any verdict, any amount -> 0.0.371's arithmetic, byte for byte.
    for (uint32_t clause = 0; clause < N48_FG_CLAUSES; clause++)
        for (uint64_t amt : { (uint64_t)0u, (uint64_t)1u, (uint64_t)583u, (uint64_t)~0ull }) {
            n48_dep_src s = dirty_src(583u);
            s.fg_enabled = 0u; s.fg_clause = clause; s.fg_forgive = amt;
            n48_dep_mono mono {}; n48_dep_world w {};
            f_fill(m, &s, &mono, &w);
            std::snprintf(buf, sizeof(buf), "switch OFF (clause %u, amount %llu): class UNCHANGED at 583   %s", clause,
                          (unsigned long long)amt, f_name(m));
            expect_u(buf, w.source_neuters, 583u);
            std::snprintf(buf, sizeof(buf), "switch OFF (clause %u, amount %llu): forgiven 0               %s", clause,
                          (unsigned long long)amt, f_name(m));
            expect_u(buf, w.forgiven, 0u);
        }

    // A GRANT WITH THE SWITCH ON AND A PASS VERDICT, that fits: the class is reduced and the world goes clean.
    {
        n48_dep_src s = dirty_src(583u);
        s.fg_enabled = 1u; s.fg_clause = N48_FG_PASS; s.fg_forgive = 583u;
        n48_dep_mono mono {}; n48_dep_world w {};
        f_fill(m, &s, &mono, &w);
        std::snprintf(buf, sizeof(buf), "a PASS grant of 583 -> class 0                              %s", f_name(m));
        expect_u(buf, w.source_neuters, 0u);
        std::snprintf(buf, sizeof(buf), "... forgiven is recorded as 583                             %s", f_name(m));
        expect_u(buf, w.forgiven, 583u);
        std::snprintf(buf, sizeof(buf), "... and the world is CLEAN                                  %s", f_name(m));
        expect_u(buf, n48_dep_check(&w, &d), N48_DEP_OK);
    }
    // A PARTIAL grant leaves the rest refusing.
    {
        n48_dep_src s = dirty_src(583u);
        s.fg_enabled = 1u; s.fg_clause = N48_FG_PASS; s.fg_forgive = 582u;
        n48_dep_mono mono {}; n48_dep_world w {};
        f_fill(m, &s, &mono, &w);
        std::snprintf(buf, sizeof(buf), "a grant of 582 of 583 leaves 1 -> source-neuter             %s", f_name(m));
        expect_u(buf, n48_dep_check(&w, &d), N48_DEP_SOURCE_NEUTER);
        std::snprintf(buf, sizeof(buf), "... and the detail is the ONE that remains                  %s", f_name(m));
        expect_u(buf, d, 1u);
    }
    // EVERY NON-PASS VERDICT WITH AN AMOUNT IS MALFORMED: the identity refuses BEFORE the class is ever read.
    for (uint32_t clause = 1; clause < N48_FG_CLAUSES; clause++) {
        n48_dep_src s = dirty_src(583u);
        s.fg_enabled = 1u; s.fg_clause = clause; s.fg_forgive = 583u;
        n48_dep_mono mono {}; n48_dep_world w {};
        f_fill(m, &s, &mono, &w);
        std::snprintf(buf, sizeof(buf), "an amount with verdict %-2u -> UNACCOUNTED                    %s", clause, f_name(m));
        expect_u(buf, n48_dep_check(&w, &d), N48_DEP_UNACCOUNTED);
        std::snprintf(buf, sizeof(buf), "... naming X9-F's identity bit                              %s", f_name(m));
        expect_u(buf, (d & N48_DEP_ID_FORGIVE) != 0u, 1u);
    }
    // A GRANT WITH THE SWITCH OFF is malformed the same way.
    {
        n48_dep_src s = dirty_src(583u);
        s.fg_enabled = 0u; s.fg_clause = N48_FG_PASS; s.fg_forgive = 583u;
        n48_dep_mono mono {}; n48_dep_world w {};
        f_fill(m, &s, &mono, &w);
        std::snprintf(buf, sizeof(buf), "an amount with the switch OFF -> UNACCOUNTED                %s", f_name(m));
        expect_u(buf, n48_dep_check(&w, &d), N48_DEP_UNACCOUNTED);
        std::snprintf(buf, sizeof(buf), "... naming X9-F's identity bit                              %s", f_name(m));
        expect_u(buf, (d & N48_DEP_ID_FORGIVE) != 0u, 1u);
    }
    // A GRANT BIGGER THAN THE CLASS must never produce a clean world - not by wrapping, and not by clamping.
    for (uint64_t amt : { (uint64_t)584u, (uint64_t)1000u, (uint64_t)~0ull }) {
        n48_dep_src s = dirty_src(583u);
        s.fg_enabled = 1u; s.fg_clause = N48_FG_PASS; s.fg_forgive = amt;
        n48_dep_mono mono {}; n48_dep_world w {};
        f_fill(m, &s, &mono, &w);
        std::snprintf(buf, sizeof(buf), "a grant of %llu against 583 -> NOT clean                      %s",
                      (unsigned long long)amt, f_name(m));
        expect_u(buf, n48_dep_check(&w, &d) != N48_DEP_OK, 1u);
        std::snprintf(buf, sizeof(buf), "a grant of %llu against 583 -> UNACCOUNTED                    %s",
                      (unsigned long long)amt, f_name(m));
        expect_u(buf, n48_dep_check(&w, &d), N48_DEP_UNACCOUNTED);
    }
    // FORGIVENESS TOUCHES EXACTLY ONE CLASS. A ring neuter (or any other) still refuses under a full grant.
    {
        struct C { const char *name; uint32_t idx; uint32_t reason; };
        static const C cs[] = {
            { "ring neuter",    N48_DEPC_GN_IBS,         N48_DEP_RING_NEUTER },
            { "walk-stopped",   N48_DEPC_GN_WALK_STOP,   N48_DEP_NEUTER_OTHER },
            { "raced",          N48_DEPC_GN_RACED,       N48_DEP_NEUTER_OTHER },
            { "unresolved tgt", N48_DEPC_WT_UNRESOLVED,  N48_DEP_TARGET_UNKNOWN },
            { "witness over",   N48_DEPC_WT_OVER,        N48_DEP_WITNESS_OVER },
            { "SDMA refused",   N48_DEPC_DX_REFUSED,     N48_DEP_SDMA_UNTRANSLATED },
            { "ring backwards", N48_DEPC_RING_BACKWARDS, N48_DEP_RING_RESET },
            { "a latched fault",N48_DEPC_VM_FAULTS,      N48_DEP_VM_FAULT },
        };
        for (const C &c : cs) {
            n48_dep_src s = dirty_src(583u);
            s.v[c.idx] = 1u;
            // FILL THE BUILDER, NEVER RELAX THE RUNG (this project has done that four times). A lone `GN_IBS = 1`
            // breaks the ring-NOP identity (`IBS_ARMED == GN_IBS + mismatch + not-IB + over + exempt`) and the world
            // then refuses at UNACCOUNTED, which would have "passed" the wrong assertion. A real ring neuter comes
            // with the IB that was found, the frame it was found in, and the frame the NOP handled.
            if (c.idx == N48_DEPC_GN_IBS) {
                s.v[N48_DEPC_RING_IBS_FOUND] = 1u; s.v[N48_DEPC_RING_IBS_ARMED] = 1u;
                s.v[N48_DEPC_RING_FRAMES_NEUTERED] = 1u; s.v[N48_DEPC_GN_FRAMES] = 1u;
            }
            if (c.idx == N48_DEPC_DX_REFUSED) s.v[N48_DEPC_DX_IBS] = 1u;   // keep the drain identity intact
            s.fg_enabled = 1u; s.fg_clause = N48_FG_PASS; s.fg_forgive = 583u;
            n48_dep_mono mono {}; n48_dep_world w {};
            f_fill(m, &s, &mono, &w);
            std::snprintf(buf, sizeof(buf), "a FULL grant does not forgive %-15s -> %-18s %s", c.name,
                          n48_dep_reason_name(c.reason), f_name(m));
            expect_u(buf, n48_dep_check(&w, &d), c.reason);
        }
    }
    // AND IT CANNOT SUBSTITUTE FOR AN OBSERVER. A grant over an unobserved world still refuses at NOT_OBSERVED.
    {
        n48_dep_src s = dirty_src(583u);
        s.q_hook_live = 0u;
        s.fg_enabled = 1u; s.fg_clause = N48_FG_PASS; s.fg_forgive = 583u;
        n48_dep_mono mono {}; n48_dep_world w {};
        f_fill(m, &s, &mono, &w);
        std::snprintf(buf, sizeof(buf), "a FULL grant over an UNOBSERVED world -> not-observed        %s", f_name(m));
        expect_u(buf, n48_dep_check(&w, &d), N48_DEP_NOT_OBSERVED);
    }
    // A world nobody gathered still refuses, grant or no grant.
    {
        n48_dep_src s {};
        s.fg_enabled = 1u; s.fg_clause = N48_FG_PASS; s.fg_forgive = 583u;
        n48_dep_mono mono {}; n48_dep_world w {};
        f_fill(m, &s, &mono, &w);
        std::snprintf(buf, sizeof(buf), "a FULL grant over an UNSAMPLED world -> not-sampled          %s", f_name(m));
        expect_u(buf, n48_dep_check(&w, &d), N48_DEP_NOT_SAMPLED);
        std::snprintf(buf, sizeof(buf), "... and nothing was forgiven                                %s", f_name(m));
        expect_u(buf, w.forgiven, 0u);
    }
    return gFail - before;
}

// =====================================================================================================================
// 0.0.381 — EXIT C: THE RUNNING BUDGET, THE SECOND GRANT SOURCE. A SEPARATE RULE, A SEPARATE CLAUSE ENUM,
// THE SAME CLAUSE-11 LATCH. It is WEAKER than everything above by design - it has neither clause 5 nor clause 9 - so the
// checks here are about the two things that are all it has left: THE BOUND, and THE LATCH.
//
// THE POSITIVE CONTROL IS arm9's OWN f15, as `dpled841` printed it: `ns 11 sn 13` at the frame Exit C exists to buy,
// against the briefed N = 16. Without a filled control every mutant would be "caught" by a rule that refuses
// everything, which tests nothing.
// =====================================================================================================================
static n48_rb_in rb_good()
{
    n48_rb_in b {};
    b.gathered = 1u;
    b.budget = 16u;              // the briefed N.: <= 12 buys nothing, 32 buys only f29 for 2.1x the exposure
    b.base_valid = 1u;
    b.base_ctx_seq = 6u;
    b.base_total = 0u;           // arm9's baseline is the FIRST bind, before f1: every drop it has is post-baseline
    b.base_binds = 1u; b.base_rebinds = 0u; b.base_unbinds = 0u; b.base_gone = 0u;
    b.ws_bound = 1u; b.ws_seq = 6u;
    b.binds = 1u; b.rebinds = 0u; b.unbinds = 0u; b.gone = 0u;
    b.now_total = 13u;           // arm9's `sn 13` at f15 - what the gate reads and what the grant must cover
    b.other_classes = 0u;
    b.own_known = 1u;
    b.own_ctx = 11u;             // THE PRICE: 11 of the 13 are the committing compositor's own judged drops
    b.own_proc = 11u;            // arm9's `ns 11`, WindowServer at process granularity
    return b;
}

enum {
    R_NONE = 0,
    R_BOUND_IGNORED,     /* A GRANT THAT SURVIVES ITS BOUND: more outstanding than N, granted anyway */
    R_ACTIVE_AT_DEFAULT, /* A GRANT ACTIVE AT ITS DEFAULT: N = 0 is the boot value and it grants */
    R_CEILING_IGNORED,   /* N above N48_RB_MAX accepted: the rule trusts its caller's bound, so there is no bound */
    R_PARTIAL_GRANT,     /* min(now_total, N) instead of refusing outright - and clause 11 then withdraws for the boot */
    R_NOT_SIGHTED_OK,    /* the price is not measured and it grants anyway: the one thing that must never be silent */
    R_IDENTITY_DROPPED,  /* the baseline's context need not be the one bound now: a grant for another compositor */
    R_BINDING_MOVED_OK,  /* the binding may move under a standing grant */
    R_WITHDRAW_FORGOTTEN,/* a withdrawn running-budget grant comes back */
    R_RUNG_NO_GRANT,     /* THE RUNG FORGIVES WITHOUT A GRANT: the socket subtracts on a REFUSED running-budget verdict */
    R_MUTANTS
};

static const char *r_name(int m)
{
    static const char *const n[R_MUTANTS] = {
        "(the real running-budget rule)", "a grant SURVIVES ITS BOUND", "a grant is ACTIVE AT ITS DEFAULT (N = 0)",
        "N above the ceiling is accepted", "an OVER-BUDGET world gets a PARTIAL grant", "the PRICE need not be measured",
        "the baseline's context need not be bound", "the binding may move under the grant",
        "a WITHDRAWN running budget comes back", "the RUNG FORGIVES WITHOUT A GRANT" };
    return (m >= 0 && m < R_MUTANTS) ? n[m] : "?";
}

/* The mutated rule. Each reproduces n48_rb_eval with exactly one thing weakened and delegates everything else to the
 * real rule, so a mutant can only ever be MORE permissive. */
static uint32_t r_eval(int m, const n48_rb_in *b, uint64_t *forgive)
{
    if (forgive) *forgive = 0u;
    if (!b) return n48_rb_eval(b, forgive);
    if (m == R_ACTIVE_AT_DEFAULT && b->budget == 0u) {
        n48_rb_in g = *b; g.budget = N48_RB_MAX;              /* "0 must mean no limit" */
        return n48_rb_eval(&g, forgive);
    }
    if (m == R_CEILING_IGNORED && b->budget > N48_RB_MAX) {
        n48_rb_in g = *b; g.budget = N48_RB_MAX;              /* the ceiling delegated to the verb and dropped here */
        return n48_rb_eval(&g, forgive);
    }
    const uint32_t real = n48_rb_eval(b, forgive);
    switch (m) {
    case R_BOUND_IGNORED:
        if (real == N48_RB_OVER_BUDGET) {                     /* "a few more cannot matter" - the bound deleted */
            n48_rb_in g = *b; g.budget = g.now_total;
            return n48_rb_eval(&g, forgive);
        }
        break;
    case R_PARTIAL_GRANT:
        if (real == N48_RB_OVER_BUDGET) {                     /* grant N of a larger number instead of refusing */
            n48_rb_in g = *b; g.now_total = g.budget;
            return n48_rb_eval(&g, forgive);
        }
        break;
    case R_NOT_SIGHTED_OK:
        if (real == N48_RB_NOT_SIGHTED) {
            n48_rb_in g = *b; g.own_known = 1u;
            return n48_rb_eval(&g, forgive);
        }
        break;
    case R_IDENTITY_DROPPED:
        if (real == N48_RB_BASE_CTX || real == N48_RB_NOT_BOUND) {
            n48_rb_in g = *b;
            if (!g.base_ctx_seq) g.base_ctx_seq = 6u;
            g.ws_bound = 1u; g.ws_seq = g.base_ctx_seq;
            return n48_rb_eval(&g, forgive);
        }
        break;
    case R_BINDING_MOVED_OK:
        if (real == N48_RB_BINDING_MOVED) {
            n48_rb_in g = *b;
            g.binds = g.base_binds; g.rebinds = g.base_rebinds;
            g.unbinds = g.base_unbinds; g.gone = g.base_gone;
            return n48_rb_eval(&g, forgive);
        }
        break;
    default: break;
    }
    return real;
}

static uint32_t r_step(int m, n48_fg_latch *l, const n48_rb_in *b, uint64_t *forgive)
{
    if (m == R_WITHDRAW_FORGOTTEN && l) l->withdrawn = 0u;
    if (m == R_NONE || m == R_WITHDRAW_FORGOTTEN) return n48_rb_step(l, b, forgive);
    uint64_t want = 0u;
    if (forgive) *forgive = 0u;
    if (!l) return N48_RB_NOT_GATHERED;
    if (n48_fg_latch_begin(l, N48_RB_WITHDRAWN)) return N48_RB_WITHDRAWN;
    const uint32_t c = r_eval(m, b, &want);
    return n48_fg_latch_apply(l, c, want, N48_RB_WITHDRAWN, N48_RB_AMOUNT_MOVED, forgive);
}

/* The mutated FILL, for the one defect that lives in the socket rather than in either rule: the rung subtracting an
 * amount no verdict authorised. gfx_dep.h needs ZERO change for Exit C, so this pins that its EXISTING guard is what
 * protects the second source too - it is not a new guard and it was not re-derived. */
static void r_fill(int m, const n48_dep_src *s0, n48_dep_mono *mono, n48_dep_world *w)
{
    n48_dep_src s = *s0;
    if (m == R_RUNG_NO_GRANT && s.fg_forgive) { s.fg_enabled = 1u; s.fg_clause = N48_FG_PASS; }
    n48_dep_fill(&s, mono, w);
}

static int rb_checks(int m)
{
    const int before = gFail;
    char buf[192];

    // ---- THE POSITIVE CONTROL, FILLED: arm9's f15 against the briefed N = 16. ----
    {
        uint64_t g = 0u;
        const n48_rb_in b = rb_good();
        std::snprintf(buf, sizeof(buf), "arm9's f15 (sn 13) under N = 16 PASSES            %s", r_name(m));
        expect_u(buf, r_eval(m, &b, &g), N48_RB_PASS);
        std::snprintf(buf, sizeof(buf), "... and the amount is the WHOLE outstanding count %s", r_name(m));
        expect_u(buf, g, 13u);
    }

    // ---- N = 0 IS THE DEFAULT, THE BOOT VALUE AND OFF. A zero-initialised request must never grant. ----
    {
        uint64_t g = 7u;
        n48_rb_in b {};
        std::snprintf(buf, sizeof(buf), "a request nobody filled REFUSES                   %s", r_name(m));
        expect_u(buf, r_eval(m, &b, &g), N48_RB_NOT_GATHERED);
        std::snprintf(buf, sizeof(buf), "... and grants 0                                  %s", r_name(m));
        expect_u(buf, g, 0u);
        b = rb_good();
        b.budget = 0u;
        g = 7u;
        std::snprintf(buf, sizeof(buf), "N = 0 REFUSES (the default and the boot value)    %s", r_name(m));
        expect_u(buf, r_eval(m, &b, &g), N48_RB_DISABLED);
        std::snprintf(buf, sizeof(buf), "... and grants 0 at the default                   %s", r_name(m));
        expect_u(buf, g, 0u);
    }

    // ---- THE BOUND. This is all that stands in for clause 5, so it is checked exhaustively around the edge. ----
    for (uint64_t sn = 1u; sn <= 24u; sn++) {
        n48_rb_in b = rb_good();
        b.now_total = sn;
        uint64_t g = 0u;
        const uint32_t c = r_eval(m, &b, &g);
        std::snprintf(buf, sizeof(buf), "sn %-2llu under N 16 -> %-12s               %s", (unsigned long long)sn,
                      sn <= 16u ? "PASS" : "OVER-BUDGET", r_name(m));
        expect_u(buf, c, sn <= 16u ? (uint64_t)N48_RB_PASS : (uint64_t)N48_RB_OVER_BUDGET);
        std::snprintf(buf, sizeof(buf), "... sn %-2llu grants %-2llu                            %s",
                      (unsigned long long)sn, (unsigned long long)(sn <= 16u ? sn : 0u), r_name(m));
        expect_u(buf, g, sn <= 16u ? sn : 0u);
    }
    // ... and at every legal N, the edge is exactly N.
    for (uint64_t n = 1u; n <= N48_RB_MAX; n++) {
        n48_rb_in b = rb_good();
        b.budget = n;
        uint64_t g = 0u;
        b.now_total = n;
        const uint32_t at = r_eval(m, &b, &g);
        b.now_total = n + 1u;
        uint64_t g2 = 0u;
        const uint32_t past = r_eval(m, &b, &g2);
        std::snprintf(buf, sizeof(buf), "N %-2llu: exactly N outstanding PASSES               %s",
                      (unsigned long long)n, r_name(m));
        expect_u(buf, at, N48_RB_PASS);
        std::snprintf(buf, sizeof(buf), "N %-2llu: N+1 outstanding is OVER-BUDGET             %s",
                      (unsigned long long)n, r_name(m));
        expect_u(buf, past, N48_RB_OVER_BUDGET);
        std::snprintf(buf, sizeof(buf), "N %-2llu: ... and grants NOTHING, not a partial      %s",
                      (unsigned long long)n, r_name(m));
        expect_u(buf, g2, 0u);
    }
    // ---- THE CEILING IS THE RULE'S OWN, not the verb's. ----
    for (uint64_t n : { (uint64_t)(N48_RB_MAX + 1u), (uint64_t)100u, (uint64_t)254u, (uint64_t)255u,
                        (uint64_t)0x100000000ull }) {
        n48_rb_in b = rb_good();
        b.budget = n;
        uint64_t g = 0u;
        std::snprintf(buf, sizeof(buf), "N %-12llu is above the ceiling -> REFUSED    %s", (unsigned long long)n,
                      r_name(m));
        expect_u(buf, r_eval(m, &b, &g), N48_RB_BUDGET_INSANE);
        std::snprintf(buf, sizeof(buf), "... N %-12llu grants 0                       %s", (unsigned long long)n,
                      r_name(m));
        expect_u(buf, g, 0u);
    }

    // ---- THE PRICE MUST HAVE BEEN MEASURED. The VALUE never refuses; its ABSENCE always does. ----
    {
        n48_rb_in b = rb_good();
        b.own_known = 0u;
        uint64_t g = 0u;
        std::snprintf(buf, sizeof(buf), "the price UNMEASURED -> REFUSED                   %s", r_name(m));
        expect_u(buf, r_eval(m, &b, &g), N48_RB_NOT_SIGHTED);
        std::snprintf(buf, sizeof(buf), "... and grants 0                                  %s", r_name(m));
        expect_u(buf, g, 0u);
        /* AND THE VALUE NEVER REFUSES - not even when EVERY forgiven drop is the compositor's own. That is the price,
         * and a rule that quietly refused on it would be's dead end wearing this rule's name. */
        b = rb_good();
        b.own_ctx = 13u; b.own_proc = 13u;
        g = 0u;
        std::snprintf(buf, sizeof(buf), "ALL 13 being the compositor's OWN still PASSES    %s", r_name(m));
        expect_u(buf, r_eval(m, &b, &g), N48_RB_PASS);
        std::snprintf(buf, sizeof(buf), "... and grants all 13 (THIS IS THE PRICE)         %s", r_name(m));
        expect_u(buf, g, 13u);
    }

    // ---- CLAUSE 2 AND CLAUSE 3 SURVIVE: the amount is no longer the baseline's, but the IDENTITY still is. ----
    {
        struct Case { const char *what; uint32_t want; n48_rb_in (*mk)(); };
        const Case cs[] = {
            { "no baseline",            N48_RB_NO_BASELINE,  []{ n48_rb_in b = rb_good(); b.base_valid = 0u; return b; } },
            { "baseline names no ctx",  N48_RB_BASE_CTX,     []{ n48_rb_in b = rb_good(); b.base_ctx_seq = 0u; return b; } },
            { "not bound now",          N48_RB_NOT_BOUND,    []{ n48_rb_in b = rb_good(); b.ws_bound = 0u; return b; } },
            { "bound to ANOTHER ctx",   N48_RB_BASE_CTX,     []{ n48_rb_in b = rb_good(); b.ws_seq = 9u; return b; } },
            { "a bind since",           N48_RB_BINDING_MOVED,[]{ n48_rb_in b = rb_good(); b.binds = 2u; return b; } },
            { "a rebind since",         N48_RB_BINDING_MOVED,[]{ n48_rb_in b = rb_good(); b.rebinds = 1u; return b; } },
            { "an unbind since",        N48_RB_BINDING_MOVED,[]{ n48_rb_in b = rb_good(); b.unbinds = 1u; return b; } },
            { "the root gone since",    N48_RB_BINDING_MOVED,[]{ n48_rb_in b = rb_good(); b.gone = 1u; return b; } },
            { "nothing outstanding",    N48_RB_NOTHING_TO_FORGIVE, []{ n48_rb_in b = rb_good(); b.now_total = 0u; return b; } },
            { "another class dirty",    N48_RB_OTHER_CLASSES,[]{ n48_rb_in b = rb_good(); b.other_classes = 1u; return b; } },
        };
        for (const Case &c : cs) {
            const n48_rb_in b = c.mk();
            uint64_t g = 0u;
            std::snprintf(buf, sizeof(buf), "%-24s -> %-22s %s", c.what, n48_rb_clause_name(c.want), r_name(m));
            expect_u(buf, r_eval(m, &b, &g), c.want);
            std::snprintf(buf, sizeof(buf), "... %-24s grants 0                 %s", c.what, r_name(m));
            expect_u(buf, g, 0u);
        }
    }

    // ---- WHAT IS DELIBERATELY *NOT* A CLAUSE, PINNED SO THAT A READER CANNOT MISTAKE IT FOR ONE. ----
    {
        /* CLAUSE 5 IS GONE. A world 13 drops past its baseline is exactly what X9-F refuses at POST_BASELINE, and this
         * rule grants it. If this check ever starts failing, someone put clause 5 back and Exit C grants nothing. */
        n48_rb_in b = rb_good();
        b.base_total = 0u; b.now_total = 13u;
        uint64_t g = 0u;
        std::snprintf(buf, sizeof(buf), "13 drops PAST THE BASELINE are granted (no cl.5)  %s", r_name(m));
        expect_u(buf, r_eval(m, &b, &g), N48_RB_PASS);
        /* ... and X9-F, asked the same world, refuses it. The two rules are disjoint by construction. */
        n48_fg_in f = good();
        f.now_total = f.base_total + 13u;
        uint64_t gf = 0u;
        std::snprintf(buf, sizeof(buf), "... X9-F refuses that SAME world at POST_BASELINE %s", r_name(m));
        expect_u(buf, n48_fg_eval(&f, &gf), N48_FG_POST_BASELINE);
        std::snprintf(buf, sizeof(buf), "... so exactly one of the two can ever fill it    %s", r_name(m));
        expect_u(buf, gf, 0u);
    }

    // ---- CLAUSE 11 ON THE RUNNING BUDGET'S OWN LATCH: one grant per boot, the amount frozen, withdrawal final. ----
    {
        n48_fg_latch l {};
        const n48_rb_in b = rb_good();
        uint64_t g = 0u;
        std::snprintf(buf, sizeof(buf), "latch: the first grant PASSES                     %s", r_name(m));
        expect_u(buf, r_step(m, &l, &b, &g), N48_RB_PASS);
        std::snprintf(buf, sizeof(buf), "... amount 13                                     %s", r_name(m));
        expect_u(buf, g, 13u);
        std::snprintf(buf, sizeof(buf), "... granted                                       %s", r_name(m));
        expect_u(buf, l.granted, 1u);
        std::snprintf(buf, sizeof(buf), "... and what is FROZEN is the BOUND, not 13       %s", r_name(m));
        expect_u(buf, l.amount, 16u);
        /* THE FROZEN QUANTITY IS N. `sn` only grows, and the gather runs on nearly every judged frame, so a latch that
         * froze the AMOUNT would fire AMOUNT_MOVED on the very next frame and spend the boot's one grant on whatever
         * frame was judged first. A LARGER amount UNDER THE SAME BOUND is therefore granted, and the bound re-checked. */
        n48_rb_in b2 = b;
        b2.now_total = 14u;
        g = 0u;
        std::snprintf(buf, sizeof(buf), "latch: a larger amount UNDER the bound still PASSES %s", r_name(m));
        expect_u(buf, r_step(m, &l, &b2, &g), N48_RB_PASS);
        std::snprintf(buf, sizeof(buf), "... and grants the larger amount, 14               %s", r_name(m));
        expect_u(buf, g, 14u);
        std::snprintf(buf, sizeof(buf), "... and the grant was still made only ONCE         %s", r_name(m));
        expect_u(buf, l.grants, 1u);
        /* THE WORLD DRIFTING PAST N CLOSES THE BUDGET FOR THE BOOT. This is what ends it: once `sn` passes N nothing can
         * be bought again, and the latch makes that permanent rather than waiting for `sn` to come back down. */
        n48_rb_in b3 = b;
        b3.now_total = 17u;
        g = 0u;
        std::snprintf(buf, sizeof(buf), "latch: drifting PAST the bound -> WITHDRAWN       %s", r_name(m));
        expect_u(buf, r_step(m, &l, &b3, &g), N48_RB_WITHDRAWN);
        std::snprintf(buf, sizeof(buf), "... withdrawn                                     %s", r_name(m));
        expect_u(buf, l.withdrawn, 1u);
        std::snprintf(buf, sizeof(buf), "... and grants 0                                  %s", r_name(m));
        expect_u(buf, g, 0u);
        /* AND IT NEVER COMES BACK, not even for the exact world that was granted. */
        g = 0u;
        std::snprintf(buf, sizeof(buf), "latch: the ORIGINAL world -> still WITHDRAWN      %s", r_name(m));
        expect_u(buf, r_step(m, &l, &b, &g), N48_RB_WITHDRAWN);
        std::snprintf(buf, sizeof(buf), "... and grants 0 forever                          %s", r_name(m));
        expect_u(buf, g, 0u);
    }
    {
        /* THE BOUND MOVING UNDER A STANDING GRANT IS AMOUNT_MOVED, in EITHER direction: the operator changing N while
         * frames are being forgiven means the grant is no longer the one that was judged. */
        for (uint64_t n2 : { (uint64_t)14u, (uint64_t)32u }) {   /* both still >= the 13 outstanding */
            n48_fg_latch l {};
            n48_rb_in b = rb_good();
            uint64_t g = 0u;
            r_step(m, &l, &b, &g);
            b.budget = n2;
            g = 0u;
            std::snprintf(buf, sizeof(buf), "latch: N moved to %-2llu under a grant -> MOVED     %s",
                          (unsigned long long)n2, r_name(m));
            expect_u(buf, r_step(m, &l, &b, &g), N48_RB_AMOUNT_MOVED);
            std::snprintf(buf, sizeof(buf), "... withdrawn after N moved to %-2llu               %s",
                          (unsigned long long)n2, r_name(m));
            expect_u(buf, l.withdrawn, 1u);
            std::snprintf(buf, sizeof(buf), "... and grants 0 after N moved to %-2llu            %s",
                          (unsigned long long)n2, r_name(m));
            expect_u(buf, g, 0u);
        }
        /* LOWERING N BELOW WHAT IS OUTSTANDING withdraws too, but by the BOUND rather than by the freeze: the rule
         * refuses OVER-BUDGET first and the latch makes that refusal permanent. Different clause, same finality. */
        {
            n48_fg_latch l {};
            n48_rb_in b = rb_good();
            uint64_t g = 0u;
            r_step(m, &l, &b, &g);
            b.budget = 8u;                       /* below the 13 outstanding */
            g = 0u;
            std::snprintf(buf, sizeof(buf), "latch: N lowered BELOW the count -> WITHDRAWN     %s", r_name(m));
            expect_u(buf, r_step(m, &l, &b, &g), N48_RB_WITHDRAWN);
            std::snprintf(buf, sizeof(buf), "... by the BOUND, and it is final                 %s", r_name(m));
            expect_u(buf, l.withdrawn, 1u);
            std::snprintf(buf, sizeof(buf), "... the clause kept is OVER-BUDGET                %s", r_name(m));
            expect_u(buf, l.first_clause, (uint64_t)N48_RB_OVER_BUDGET);
        }
    }
    {
        /* TURNING THE LEVER OFF WITHDRAWS A STANDING GRANT. Going back is always allowed; coming back is not. */
        n48_fg_latch l {};
        n48_rb_in b = rb_good();
        uint64_t g = 0u;
        r_step(m, &l, &b, &g);
        b.budget = 0u;
        g = 0u;
        std::snprintf(buf, sizeof(buf), "latch: N back to 0 WITHDRAWS the standing grant   %s", r_name(m));
        expect_u(buf, r_step(m, &l, &b, &g), N48_RB_WITHDRAWN);
        b = rb_good();
        g = 0u;
        std::snprintf(buf, sizeof(buf), "latch: turning it back ON does NOT re-grant       %s", r_name(m));
        expect_u(buf, r_step(m, &l, &b, &g), N48_RB_WITHDRAWN);
        std::snprintf(buf, sizeof(buf), "... and grants 0                                  %s", r_name(m));
        expect_u(buf, g, 0u);
    }
    {
        /* A REFUSAL BEFORE ANY GRANT is not a withdrawal: the boot may still earn its one grant. */
        n48_fg_latch l {};
        n48_rb_in b = rb_good();
        b.now_total = 99u;
        uint64_t g = 0u;
        std::snprintf(buf, sizeof(buf), "latch: OVER-BUDGET before a grant is not final    %s", r_name(m));
        expect_u(buf, r_step(m, &l, &b, &g), N48_RB_OVER_BUDGET);
        std::snprintf(buf, sizeof(buf), "... not withdrawn                                 %s", r_name(m));
        expect_u(buf, l.withdrawn, 0u);
        b = rb_good();
        g = 0u;
        std::snprintf(buf, sizeof(buf), "... and a later good world still PASSES           %s", r_name(m));
        expect_u(buf, r_step(m, &l, &b, &g), N48_RB_PASS);
    }

    // ---- EVERY CLAUSE HAS A NAME. A verdict an operator cannot read is a verdict nobody checks. ----
    for (uint32_t c = 0; c < N48_RB_CLAUSES; c++) {
        std::snprintf(buf, sizeof(buf), "running-budget clause %-2u has a name               %s", c, r_name(m));
        expect_u(buf, std::strcmp(n48_rb_clause_name(c), "?") != 0, 1u);
    }
    std::snprintf(buf, sizeof(buf), "an unknown running-budget clause reads ?          %s", r_name(m));
    expect_u(buf, std::strcmp(n48_rb_clause_name(N48_RB_CLAUSES), "?") == 0, 1u);

    // ---- AND THE SOCKET ACCEPTS IT UNCHANGED: gfx_dep.h needs ZERO change for the second source. ----
    {
        n48_dep_src s = clean_src();
        s.v[N48_DEPC_SRC_CALLS] = 13u;           /* the source identity: every call lands in exactly one bucket */
        s.v[N48_DEPC_SRC_NEUTERED] = 11u;        /* arm9's `ns 11` - WindowServer's OWN judged drops */
        s.v[N48_DEPC_SRC_NEUTERED_OTHER] = 2u;   /* ... and the 2 that are not, for `sn 13` */
        s.fg_enabled = 1u;
        s.fg_clause = N48_FG_PASS;
        s.fg_forgive = 13u;                     /* what the RUNNING budget granted, in X9-F's own socket */
        n48_dep_mono mono {};
        n48_dep_world w {};
        r_fill(m, &s, &mono, &w);
        uint64_t d = 0u;
        std::snprintf(buf, sizeof(buf), "the socket takes the running budget's 13          %s", r_name(m));
        expect_u(buf, w.source_neuters, 0u);
        std::snprintf(buf, sizeof(buf), "... records `forgiven` 13                         %s", r_name(m));
        expect_u(buf, w.forgiven, 13u);
        std::snprintf(buf, sizeof(buf), "... breaks NO identity                            %s", r_name(m));
        expect_u(buf, w.unaccounted, 0u);
        std::snprintf(buf, sizeof(buf), "... and the world is CLEAN                        %s", r_name(m));
        expect_u(buf, n48_dep_check(&w, &d), N48_DEP_OK);

        /* AND THE RUNG MUST NOT FORGIVE WITHOUT A GRANT. Every non-PASS verdict of the RUNNING budget's own enum, and
         * the switch off, carrying the same amount: each must break the identity and refuse BEFORE source-neuter. */
        for (uint32_t c = 1u; c < N48_RB_CLAUSES; c++) {
            n48_dep_src bad = s;
            bad.fg_clause = c;
            n48_dep_mono m2 {};
            n48_dep_world w2 {};
            r_fill(m, &bad, &m2, &w2);
            uint64_t d2 = 0u;
            std::snprintf(buf, sizeof(buf), "rung: running-budget clause %-2u forgives NOTHING   %s", c, r_name(m));
            expect_u(buf, w2.forgiven, 0u);
            std::snprintf(buf, sizeof(buf), "rung: ... clause %-2u breaks X9-F's identity bit    %s", c, r_name(m));
            expect_u(buf, w2.unaccounted & N48_DEP_ID_FORGIVE, (uint64_t)N48_DEP_ID_FORGIVE);
            std::snprintf(buf, sizeof(buf), "rung: ... clause %-2u refuses UNACCOUNTED           %s", c, r_name(m));
            expect_u(buf, n48_dep_check(&w2, &d2), N48_DEP_UNACCOUNTED);
        }
        {
            n48_dep_src off = s;
            off.fg_enabled = 0u;                      /* N = 0: the default, the boot value, and OFF */
            n48_dep_mono m2 {};
            n48_dep_world w2 {};
            r_fill(m, &off, &m2, &w2);
            uint64_t d2 = 0u;
            std::snprintf(buf, sizeof(buf), "rung: the switch OFF forgives NOTHING             %s", r_name(m));
            expect_u(buf, w2.forgiven, 0u);
            std::snprintf(buf, sizeof(buf), "rung: ... and refuses UNACCOUNTED                 %s", r_name(m));
            expect_u(buf, n48_dep_check(&w2, &d2), N48_DEP_UNACCOUNTED);
        }
    }
    return gFail - before;
}

int main()
{
    std::printf("== X9-F: the rule (, notes/M4-PREARM-FORGIVENESS.md) ==\n");
    const int ruleFails = checks(M_NONE);
    const int ruleRun = gRun;
    std::printf("\n== X9-F: what gfx_dep.h's fill does with a verdict and an amount ==\n");
    const int fillFails = fill_checks(F_NONE);
    const int fillRun = gRun - ruleRun;

    std::printf("\n== EXIT C: the RUNNING budget, the second grant source (notes \u00a7853) ==\n");
    const int rbFails = rb_checks(R_NONE);
    const int rbRun = gRun - ruleRun - fillRun;

    std::printf("\n== planted defects in the RULE: each must be CAUGHT ==\n");
    int caught = 0, mutants = 0;
    for (int m = M_ALIVE_OK; m < M_MUTANTS; m++) {
        mutants++;
        gQuiet = 1;
        const int f = checks(m);
        gQuiet = 0;
        if (f > 0) caught++;
        std::printf("  %-46s %s (%d check(s) failed)\n", m_name(m), f > 0 ? "CAUGHT" : "*** NOT CAUGHT ***", f);
    }
    std::printf("\n== planted defects in the FILL: each must be CAUGHT ==\n");
    for (int m = F_NO_SWITCH; m < F_MUTANTS; m++) {
        mutants++;
        gQuiet = 1;
        const int f = fill_checks(m);
        gQuiet = 0;
        if (f > 0) caught++;
        std::printf("  %-46s %s (%d check(s) failed)\n", f_name(m), f > 0 ? "CAUGHT" : "*** NOT CAUGHT ***", f);
    }

    std::printf("\n== planted defects in the RUNNING BUDGET: each must be CAUGHT ==\n");
    for (int m = R_BOUND_IGNORED; m < R_MUTANTS; m++) {
        mutants++;
        gQuiet = 1;
        const int f = rb_checks(m);
        gQuiet = 0;
        if (f > 0) caught++;
        std::printf("  %-46s %s (%d check(s) failed)\n", r_name(m), f > 0 ? "CAUGHT" : "*** NOT CAUGHT ***", f);
    }

    const int bad = ruleFails + fillFails + rbFails;
    std::printf("\n%d check(s) on the real rules and the real fill (%d X9-F rule + %d fill + %d running budget), %d "
                "failed. %d of %d planted defects caught.\n", ruleRun + fillRun + rbRun, ruleRun, fillRun, rbRun, bad,
                caught, mutants);
    if (bad == 0 && caught == mutants) {
        std::printf("N48-FORGIVE-TEST-PASS: a pre-baseline drop is forgiven only on all eleven clauses, a RUNNING "
                    "budget only inside its bound and only once, a withdrawal is final either way, and %d of %d defects "
                    "are caught.\n", caught, mutants);
        return 0;
    }
    std::printf("N48-FORGIVE-TEST-FAIL: %d real check(s) failed, %d of %d defects caught.\n", bad, caught, mutants);
    return 1;
}
