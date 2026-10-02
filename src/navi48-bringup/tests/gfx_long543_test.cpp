// gfx_long543_test.cpp — build 0.0.543 item D (a policy decision: "about 4,000 frames / 120 seconds";
// switch 102, gfx_commit.h n48_cm_sw102_decode / n48_cm_shot_arm_cont_ext_us): THE LONGER CONTINUOUS ARM.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_long543_test.cpp -o /tmp/long543 && \
//         /tmp/long543 src/navi48-bringup/src/apple/AppleHardwareHook.cpp
// Covers:
//   L1 switch 102's decode: SET exactly for N 1..4000 and T 1..1200 (x 100 ms); REFUSED at N 0, N 4001..4094, T 0, T 1201..2047
//      and any bit above 30; CLEAR at N 0xFFF whatever T; a bare read at N 0, T 0; exhaustive over N x a T sample;
//   L2 switch 41 UNCHANGED: every (N 0..1023, T 0..1023, unit 0/1) plus high bits decodes exactly as 0.0.542's frozen decoder;
//      N 0x3FF is still CLEAR; N 1023 / T past 60 s are still refused by switch 41 and by its arm; the switch-41 arm writes
//      cont_ext 0 and its budget clamps an N past 1022 to 1 (fail-safe), exactly as 0.0.542;
//   L3 the extended arm's own second guard (N 4001, T 120.0000001 s, 0s refused and the shot untouched) and a SYNTHETIC 4000-COMMIT
//      ARM through the real budget / spend / T / pre-plane logic: pre-plane budget min(4, N); 3999 spends stand ARMED, the 4000th
//      SPENDS with stop_why N; a repeated seq is never charged; T stops at exactly 120 s after the first plane and not before; a
//      one-shot / switch-41 arm after it reads cont_ext 0;
//   L4 EVERY CAP a 4000 / 120 s arm meets (the per-second line caps of perf540, present94 and wo99, driven through their real window
//      functions over a 150 s arm - 120 s of T plus the 30 s pre-plane bound - with nothing suppressed; the count-only log caps;
//      the budget constants that must NOT move);
//   L5 the kext glue (source pins): the 102 verb decodes once with n48_cm_sw102_decode, is mid-arm guarded and is gXdContExt's only
//      setter to 1; switch 41's SET and CLEAR write gXdContExt 0; the arm picks n48_cm_shot_arm_cont_ext_us only on gXdContExt;
//   L6 the verb's line <= 491 bytes at maximal fields.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "gfx_commit.h"
#include "gfx_perf540.h"
#include "gfx_p94.h"
#include "gfx_wo99.h"

#ifndef N48_LOG_CAP_BODY
#define N48_LOG_CAP_BODY 491u
#endif

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-110s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-110s %#llx\n", what, (unsigned long long)got);
}
static std::string slurp(const char *p)
{
    FILE *f = std::fopen(p, "rb");
    if (!f) return std::string();
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}
static uint32_t count(const std::string &s, const char *x)
{
    uint32_t c = 0u;
    for (size_t p = s.find(x); p != std::string::npos; p = s.find(x, p + 1)) c++;
    return c;
}
static std::string body_from(const std::string &s, const char *start, const char *end)
{
    const size_t a = s.find(start);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find(end, a + std::strlen(start));
    return b == std::string::npos ? std::string() : s.substr(a, b - a);
}

/* 0.0.542's switch-41 decoder and its two constants, FROZEN (copied from gfx_commit.h at 1b947c2c). */
#define F542_N_MAX 1022u
#define F542_T_MAX 500u
#define F542_T_MAX_US 60000000ull
static uint64_t f542_t_us(uint32_t t_field, uint32_t unit_100ms)
{
    if (t_field == 0u) return 0ull;
    if (!unit_100ms && t_field > F542_T_MAX) return 0ull;
    const uint64_t us = (uint64_t)t_field * (unit_100ms ? 100000ull : 10000ull);
    if (us > F542_T_MAX_US) return 0ull;
    return us;
}
static n48_cm_sw41 f542_sw41(uint64_t arg)
{
    n48_cm_sw41 d;
    d.n = (uint32_t)((arg >> 8) & 0x3FFull);
    d.t = (uint32_t)((arg >> 18) & 0x3FFull);
    d.unit_100ms = (uint32_t)((arg >> 28) & 1ull);
    d.t_us = 0ull;
    if (d.n == 0x3FFu) { d.action = N48_CM_SW41_CLEAR; return d; }
    if (d.n == 0u && d.t == 0u) { d.action = N48_CM_SW41_READ; return d; }
    d.t_us = f542_t_us(d.t, d.unit_100ms);
    d.action = (d.n == 0u || d.n > F542_N_MAX || d.t_us == 0ull) ? N48_CM_SW41_REFUSED : N48_CM_SW41_SET;
    if (d.action == N48_CM_SW41_REFUSED) d.t_us = 0ull;
    return d;
}
static uint64_t sw41_arg(uint32_t n, uint32_t t, uint32_t unit) { return 41ull | ((uint64_t)n << 8) | ((uint64_t)t << 18) | ((uint64_t)unit << 28); }
static uint64_t sw102_arg(uint32_t n, uint32_t t) { return 102ull | ((uint64_t)n << 8) | ((uint64_t)t << 20); }

static void l1()
{
    uint32_t bad = 0u, sets = 0u, cases = 0u;
    for (uint32_t N = 0u; N <= 0xFFFu; N++) {
        const uint32_t ts[] = { 0u, 1u, 2u, 599u, 600u, 601u, 1199u, 1200u, 1201u, 2047u };
        for (uint32_t T : ts) {
            const n48_cm_sw41 d = n48_cm_sw102_decode(sw102_arg(N, T));
            cases++;
            uint32_t want;
            if (N == 0xFFFu) want = N48_CM_SW41_CLEAR;
            else if (N == 0u && T == 0u) want = N48_CM_SW41_READ;
            else if (N >= 1u && N <= 4000u && T >= 1u && T <= 1200u) want = N48_CM_SW41_SET;
            else want = N48_CM_SW41_REFUSED;
            if (d.action != want) bad++;
            if (want == N48_CM_SW41_SET) {
                sets++;
                if (d.n != N || d.t != T || d.t_us != (uint64_t)T * 100000ull || d.unit_100ms != 1u) bad++;
            } else if (d.t_us != 0ull) bad++;
        }
    }
    expect_u("L1 sw102: every N 0..0xFFF x 10 T values decodes as specified, mismatches", bad, 0u);
    expect_u("L1 sw102:   cases run / SET cases", (uint64_t)cases * 100000ull + sets, 40960ull * 100000ull + 4000ull * 7ull);
    const n48_cm_sw41 top = n48_cm_sw102_decode(sw102_arg(4000u, 1200u));
    expect_u("L1 sw102: N 4000, T 1200 is SET at exactly 120 000 000 us", top.action == N48_CM_SW41_SET && top.t_us == 120000000ull, 1u);
    expect_u("L1 sw102: N 4001 REFUSED", n48_cm_sw102_decode(sw102_arg(4001u, 600u)).action, (uint64_t)N48_CM_SW41_REFUSED);
    expect_u("L1 sw102: T 1201 (120.1 s) REFUSED", n48_cm_sw102_decode(sw102_arg(60u, 1201u)).action, (uint64_t)N48_CM_SW41_REFUSED);
    expect_u("L1 sw102: bit 31 set on a legal value REFUSED", n48_cm_sw102_decode(sw102_arg(60u, 600u) | (1ull << 31)).action,
             (uint64_t)N48_CM_SW41_REFUSED);
    expect_u("L1 sw102: bit 40 set on a legal value REFUSED", n48_cm_sw102_decode(sw102_arg(60u, 600u) | (1ull << 40)).action,
             (uint64_t)N48_CM_SW41_REFUSED);
    expect_u("L1 sw102: N 0xFFF with a bit above 30 is REFUSED, not CLEAR (never guessed)",
             n48_cm_sw102_decode(sw102_arg(0xFFFu, 0u) | (1ull << 33)).action, (uint64_t)N48_CM_SW41_REFUSED);
    expect_u("L1 sw102: the operator's 4000 / 120 s value is 0x4b0fa066", sw102_arg(4000u, 1200u), 0x4b0fa066ull);
    expect_u("L1 sw102: the CLEAR value 0xfff66 decodes CLEAR", n48_cm_sw102_decode(0xfff66ull).action, (uint64_t)N48_CM_SW41_CLEAR);
    expect_u("L1 sw102: bare 102 reads", n48_cm_sw102_decode(102ull).action, (uint64_t)N48_CM_SW41_READ);
}

static void l2()
{
    uint32_t bad = 0u, n = 0u;
    for (uint32_t N = 0u; N <= 1023u; N++)
        for (uint32_t T = 0u; T <= 1023u; T += (T < 16u || T > 1000u) ? 1u : 7u)
            for (uint32_t U = 0u; U < 2u; U++)
                for (uint64_t hi : { 0ull, 1ull << 29, 1ull << 31, 0xF00000000ull }) {
                    const uint64_t a = sw41_arg(N, T, U) | hi;
                    const n48_cm_sw41 x = n48_cm_sw41_decode(a), r = f542_sw41(a);
                    n++;
                    if (x.action != r.action || x.n != r.n || x.t != r.t || x.unit_100ms != r.unit_100ms || x.t_us != r.t_us) bad++;
                }
    expect_u("L2 sw41: every sampled (N, T, unit, high bits) decodes EXACTLY as 0.0.542's frozen decoder, mismatches", bad, 0u);
    expect_u("L2 sw41:   cases run > 300 000", n > 300000u ? 1u : 0u, 1u);
    expect_u("L2 sw41: N 0x3FF still CLEAR", n48_cm_sw41_decode(sw41_arg(0x3FFu, 600u, 1u)).action, (uint64_t)N48_CM_SW41_CLEAR);
    expect_u("L2 sw41: N 1023 cannot SET (it is CLEAR); N 1022 is its ceiling",
             n48_cm_sw41_decode(sw41_arg(1022u, 600u, 1u)).action == N48_CM_SW41_SET &&
             N48_CM_CONT_N_MAX == 1022u && N48_CM_CONT_T_MAX_US == 60000000ull, 1u);
    n48_cm_ws_mark m0 {};
    n48_cm_shot sh {};
    expect_u("L2 the switch-41 arm refuses N 1023 (its own ceiling, unchanged)", n48_cm_shot_arm_cont_us(&sh, &m0, 5ull, 1023u, 1000000ull), 0u);
    expect_u("L2 the switch-41 arm refuses T 60.000001 s (unchanged)", n48_cm_shot_arm_cont_us(&sh, &m0, 5ull, 60u, 60000001ull), 0u);
    expect_u("L2   and the shot is untouched (OFF)", sh.state, (uint64_t)N48_CM_SHOT_OFF);
    sh.cont_ext = 1u;   /* a stale ext flag from a previous arm */
    expect_u("L2 the switch-41 arm at N 1022 / 60 s stands and writes cont_ext 0",
             n48_cm_shot_arm_cont_us(&sh, &m0, 5ull, 1022u, 60000000ull) == 1u && sh.cont_ext == 0u && sh.cont_n == 1022u, 1u);
    sh.cont_start_us = 10ull;
    expect_u("L2   its budget is 1022 after the plane", n48_cm_shot_budget_of(&sh), 1022u);
    sh.cont_n = 4000u;   /* a corrupted N on a switch-41 arm: 0.0.542's fail-safe clamp still applies */
    expect_u("L2   an N past 1022 on a switch-41 arm reads 1 (fail-safe, as 0.0.542)", n48_cm_shot_budget_of(&sh), 1u);
    sh.cont_ext = 1u;
    sh.cont_n = 4001u;
    expect_u("L2   an N past 4000 on an extended arm reads 1 (fail-safe)", n48_cm_shot_budget_of(&sh), 1u);
    n48_cm_shot one {};
    one.cont_ext = 1u;
    expect_u("L2 a one-shot arm writes cont_ext 0 and budget 1", n48_cm_shot_arm(&one, &m0, 5ull, 0u) == 1u && one.cont_ext == 0u &&
             n48_cm_shot_budget_of(&one) == 1u, 1u);
}

static void l3()
{
    n48_cm_ws_mark m0 {};
    {
        n48_cm_shot t {};
        expect_u("L3 ext arm: N 4001 refused", n48_cm_shot_arm_cont_ext_us(&t, &m0, 5ull, 4001u, 1000000ull), 0u);
        expect_u("L3 ext arm: T 120 000 001 us refused", n48_cm_shot_arm_cont_ext_us(&t, &m0, 5ull, 60u, 120000001ull), 0u);
        expect_u("L3 ext arm: N 0 refused", n48_cm_shot_arm_cont_ext_us(&t, &m0, 5ull, 0u, 1000000ull), 0u);
        expect_u("L3 ext arm: T 0 refused", n48_cm_shot_arm_cont_ext_us(&t, &m0, 5ull, 60u, 0ull), 0u);
        expect_u("L3   and the shot is untouched (OFF)", t.state, (uint64_t)N48_CM_SHOT_OFF);
    }
    n48_cm_shot sh {};
    const uint64_t armAt = 1000000ull;
    expect_u("L3 ext arm: N 4000 / T 120 s stands", n48_cm_shot_arm_cont_ext_us(&sh, &m0, armAt, 4000u, 120000000ull), 1u);
    expect_u("L3   cont, cont_ext, cont_n, cont_t_us, state",
             sh.cont == 1u && sh.cont_ext == 1u && sh.cont_n == 4000u && sh.cont_t_us == 120000000ull && sh.state == N48_CM_SHOT_ARMED &&
             sh.cont_start_us == 0ull && sh.stop_why == N48_CM_STOP_NONE && sh.spent == 0u, 1u);
    expect_u("L3   pre-plane the budget is min(4, N) = 4", n48_cm_shot_budget_of(&sh), 4u);
    expect_u("L3   pre-plane: T never fires, however late", n48_cm_shot_stop_if_t(&sh, armAt + 500000000ull), 0u);
    /* the pre-plane bound still fires at 30 s for a plane that never comes (checked on a copy) */
    {
        n48_cm_shot c = sh;
        expect_u("L3   the pre-plane bound is unchanged (30 s): not at 29.999999 s",
                 n48_cm_shot_stop_if_preplane(&c, armAt + N48_CM_PREPLANE_BOUND_US - 1ull), 0u);
        expect_u("L3   ... and at 30 s", n48_cm_shot_stop_if_preplane(&c, armAt + N48_CM_PREPLANE_BOUND_US) == 1u &&
                 c.stop_why == N48_CM_STOP_PREPLANE_TIMEOUT, 1u);
    }
    /* two pre-plane spends, then the plane starts T */
    expect_u("L3   pre-plane spend seq 1", n48_cm_shot_spend(&sh, 1u), 1u);
    expect_u("L3   pre-plane spend seq 2", n48_cm_shot_spend(&sh, 2u), 1u);
    const uint64_t t0 = armAt + 7000000ull;
    expect_u("L3   the first plane commit starts T", n48_cm_shot_cont_start(&sh, t0, 1u), 1u);
    expect_u("L3   post-plane the budget is N = 4000", n48_cm_shot_budget_of(&sh), 4000u);
    uint32_t armedAfter = 0u, repeatCharged = 0u, tEarly = 0u;
    for (uint32_t seq = 3u; seq <= 3999u; seq++) {
        n48_cm_shot_spend(&sh, seq);
        if (n48_cm_shot_spend(&sh, seq)) repeatCharged++;   /* the same frame again: never charged */
        const uint64_t now = t0 + (uint64_t)(seq - 3u) * 29000ull;   /* ~34.5 commits/s: 3997 commits in ~115.9 s */
        if (n48_cm_shot_stop_if_t(&sh, now)) tEarly++;
        if (sh.state == N48_CM_SHOT_ARMED) armedAfter++;
    }
    expect_u("L3   3999 spends: still ARMED after every one (no early SPENT)", armedAfter, 3997u);
    expect_u("L3   a repeated seq is never charged", repeatCharged, 0u);
    expect_u("L3   T (120 s) never fired inside 116 s", tEarly, 0u);
    expect_u("L3   spent 3999, left 1", sh.spent == 3999u && n48_cm_shot_left(&sh) == 1u, 1u);
    expect_u("L3   the 4000th spend moves ARMED -> SPENT with stop_why N", n48_cm_shot_spend(&sh, 4000u) == 1u &&
             sh.state == N48_CM_SHOT_SPENT && sh.stop_why == N48_CM_STOP_N && sh.spent == 4000u && sh.spent_seq == 4000u, 1u);
    expect_u("L3   nothing more is charged once SPENT", n48_cm_shot_spend(&sh, 4001u), 0u);
    /* T: an arm that commits slowly stops at exactly 120 s */
    n48_cm_shot s2 {};
    (void)n48_cm_shot_arm_cont_ext_us(&s2, &m0, armAt, 4000u, 120000000ull);
    (void)n48_cm_shot_cont_start(&s2, t0, 1u);
    expect_u("L3 T: not at 119.999999 s", n48_cm_shot_stop_if_t(&s2, t0 + 119999999ull), 0u);
    expect_u("L3 T: at 120 s exactly, stop_why T", n48_cm_shot_stop_if_t(&s2, t0 + 120000000ull) == 1u &&
             s2.stop_why == N48_CM_STOP_T && s2.state == N48_CM_SHOT_SPENT, 1u);
    /* an ext arm with a small N behaves as a switch-41 arm with that N */
    n48_cm_shot a {}, b {};
    (void)n48_cm_shot_arm_cont_ext_us(&a, &m0, armAt, 60u, 5000000ull);
    (void)n48_cm_shot_arm_cont_us(&b, &m0, armAt, 60u, 5000000ull);
    a.cont_ext = 0u;
    expect_u("L3 an ext arm at N 60 / 5 s equals the switch-41 arm but for cont_ext", std::memcmp(&a, &b, sizeof a), 0u);
    /* a finished ext arm, then a one-shot and a switch-41 arm on the same struct */
    n48_cm_shot r = sh;
    (void)n48_cm_shot_finish(&r);
    (void)n48_cm_shot_arm(&r, &m0, armAt, 1u);
    expect_u("L3 a one-shot arm after an ext arm reads cont_ext 0, budget 1", r.cont_ext == 0u && n48_cm_shot_budget_of(&r) == 1u, 1u);
}

static void l4()
{
    /* The longest armed window a 102 arm can hold: the 30 s pre-plane bound, then T. */
    const uint64_t armUs = N48_CM_PREPLANE_BOUND_US + N48_CM_CONT_T_MAX_US_EXT;
    const uint32_t secs = (uint32_t)(armUs / 1000000ull);   /* 150 */
    expect_u("L4 the longest armed window is 150 s", secs, 150u);
    /* perf540: drive its real window over 150 s + 1 at one window per second */
    {
        n48_pf_win w {};
        n48_pf_secv v {};
        n48_pf_secline L {};
        uint32_t printed = 0u;
        for (uint32_t s = 0u; s <= secs + 1u; s++) {
            const uint64_t now = 1000000ull + (uint64_t)s * N48_PF_SEC_US;
            if (n48_pf_win_want(&w, 1u, now) && n48_pf_win_close(&w, &v, now, &L)) printed++;
        }
        expect_u("L4 perf540 sec: a 150 s arm prints every second's line (151), suppressed 0", printed == secs + 1u && w.suppressed == 0u, 1u);
        expect_u("L4 perf540 sec: its cap covers >= 150 s + 60 s of margin", N48_PF_SEC_LINES * (N48_PF_SEC_US / 1000000ull) >= 210u, 1u);
    }
    /* present94 */
    {
        n48_p94_sec w {};
        n48_p94_secline L {};
        uint32_t printed = 0u;
        for (uint32_t s = 0u; s <= secs + 1u; s++) {
            const uint64_t now = 1000000ull + (uint64_t)s * N48_P94_SEC_US;
            n48_p94_sec_count(&w, N48_P94_OC_PRESENTS);
            if (n48_p94_sec_tick(&w, 1u, now, &L)) printed++;
        }
        expect_u("L4 present94 sec: a 150 s arm prints every second's line (151), suppressed 0", printed == secs + 1u && w.suppressed == 0u, 1u);
        expect_u("L4 present94 sec: its cap covers >= 210 s", N48_P94_SEC_LINES * (N48_P94_SEC_US / 1000000ull) >= 210u, 1u);
    }
    /* wo99's per-second line (same shape) */
    expect_u("L4 wo99 sec: its cap covers >= 210 s", N48_WO_SEC_LINES * (N48_WO_SEC_US / 1000000ull) >= 210u, 1u);
    /* the count-only log caps: they cap LINES, never commits or frames - the counts past them are kept */
    {
        uint32_t printed = 0u; uint64_t sup = 0ull;
        for (uint32_t i = 0; i < 4000u; i++) if (n48_cm_cont_log_take(1u, &printed, N48_CM_CONT_LOG_REFUSAL_LINES, &sup)) {}
        expect_u("L4 a refusal-line cap over 4000 events: 64 printed, 3936 counted, none lost", printed == 64u && sup == 3936ull, 1u);
    }
    /* the budget constants that must NOT move with this item */
    expect_u("L4 N48_CM_SHOT_BUDGET_MAX unchanged (4)", N48_CM_SHOT_BUDGET_MAX, 4u);
    expect_u("L4 N48_CM_PREPLANE_BOUND_US unchanged (30 s)", N48_CM_PREPLANE_BOUND_US, 30000000ull);
    expect_u("L4 the extended ceilings are exactly the policy's 4000 / 120 s", (uint64_t)N48_CM_CONT_N_MAX_EXT * 1000000000ull +
             N48_CM_CONT_T_MAX_US_EXT, 4000ull * 1000000000ull + 120000000ull);
    /* the shot's counters are wide enough for 4000 */
    n48_cm_shot sh {};
    sh.spent = 4000u; sh.cont_n = 4000u;
    expect_u("L4 the shot's spent / cont_n hold 4000", (uint64_t)sh.spent + sh.cont_n, 8000u);
}

static void l5(const std::string &s)
{
    const std::string v = body_from(s, "    } else if ((arg & 0xffull) == 102ull) {", "    } else if ((arg & 0xffull) == 98ull) {");
    expect_u("L5 the 102 verb exists once", count(s, "} else if ((arg & 0xffull) == 102ull) {"), 1u);
    expect_u("L5   it decodes once with n48_cm_sw102_decode(arg)", count(v, "n48_cm_sw102_decode(arg)"), 1u);
    expect_u("L5   it asks the mid-arm guard with 102 (reads allowed)",
             count(v, "n48_cm_cont_switch_refused(102u, isRead102 ? 1u : 0u, gXdShot.cont, gXdShot.state)"), 1u);
    expect_u("L5   102 is in the guarded list", n48_cm_cont_switch_guarded(102u), 1u);
    expect_u("L5   the verb sets gXdContExt = 1 only on SET, and nothing else anywhere does",
             count(v, "gXdContTUs = d102.t_us; gXdContExt = 1u;") == 1u && count(s, "gXdContExt = 1u") == 1u, 1u);
    const std::string v41 = body_from(s, "    } else if ((arg & 0xffull) == 41ull) {", "    } else if ((arg & 0xffull) == 42ull) {");
    expect_u("L5 switch 41's SET and CLEAR both write gXdContExt 0", count(v41, "gXdContExt = 0u;"), 2u);
    expect_u("L5 switch 41 still decodes with n48_cm_sw41_decode(arg) once", count(v41, "const n48_cm_sw41 d41 = n48_cm_sw41_decode(arg);"), 1u);
    expect_u("L5 the arm: ext only on gXdContExt, else 0.0.542's call",
             count(s, "if (gXdContExt) n48_cm_shot_arm_cont_ext_us(&gXdShot, &wsAt, latch_now_us(), gXdContN, gXdContTUs);\n"
                      "                    else n48_cm_shot_arm_cont_us(&gXdShot, &wsAt, latch_now_us(), gXdContN, gXdContTUs);"), 1u);
    expect_u("L5 n48_cm_shot_arm_cont_ext_us is called only there", count(s, "n48_cm_shot_arm_cont_ext_us("), 1u);
    expect_u("L5 gXdContExt is boot 0", count(s, "static volatile uint32_t gXdContExt { 0u };"), 1u);
}

static void l6()
{
    char b[2048];
    const char *hows[] = { " *** REFUSED: a continuous arm stands, UNCHANGED ***",
                           " *** REFUSED: N or T is out of range (0, past its ceiling, or a bit above 30 set), UNCHANGED ***",
                           " - CHANGED BY THIS VERB", " (read only, unchanged)" };
    uint32_t ok = 1u;
    for (const char *h : hows) {
        const int n = std::snprintf(b, sizeof b, N48_CM_SW102_FMT, 4294967295u, 4294967295u, "100", ~0ull, 999ull, ~0ull, 4294967295u, h,
                                    " - a continuous arm is NOT requested", 4294967295u, 4294967295u, ~0ull, 4294967295u, ~0ull);
        if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  too long (%d): %s\n", n, b); }
    }
    expect_u("L6 the switch-102 line fits 491 bytes at maximal fields", ok, 1u);
}

int main(int argc, char **argv)
{
    l1();
    l2();
    l3();
    l4();
    if (argc > 1) {
        const std::string s = slurp(argv[1]);
        expect_u("L5 AppleHardwareHook.cpp read", s.empty() ? 0u : 1u, 1u);
        if (!s.empty()) l5(s);
    } else {
        expect_u("L5 needs AppleHardwareHook.cpp as argv[1]", 0u, 1u);
    }
    l6();
    std::printf("%s: %d run, %d failed\n", gFail ? "FAIL" : "PASS", gRun, gFail);
    return gFail ? 1 : 0;
}
