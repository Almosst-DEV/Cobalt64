// gfx_perf540_test.cpp — build 0.0.540: switch 96, perf540 (gfx_perf540.h), its MM sub-tags (gfx_mmhold.h),
// the mid-arm guard (gfx_commit.h), the SDMA line fix, and the kext glue.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_perf540_test.cpp -o /tmp/pf540 && /tmp/pf540 \
//         src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/Navi48Bringup.cpp \
//         src/navi48-bringup/src/amd/n48log.cpp src/navi48-bringup/src/apple/Navi48Ttl.hpp
// Covers:
//   P1 OFF: n48_pf_begin never reads the clock and answers 0; ON reads it once; a runtime model of every timed site's
//      begin/note shape over a counting clock: OFF, 10^4 site executions read no clock and move no accumulator;
//   P2 the idle-gap and present-gap buckets at every boundary;
//   P3 the per-second window: the arm opens it (no line), >= 1 s closes it with that second's deltas, < 1 s does not, the cap
//      of 240 lines per arm (the rest suppressed), a new arm resets the budget, not armed closes the window;
//   P4 the START snapshot and the STOP deltas (deltas, never totals; no START = nothing; STOP closes the arm; max passes through);
//   P5 the shadow direct-mapped tags (per-generation / any-time would-hit; the cross-frame memo's tag and validity);
//   P6 the MM sub-tags: JUDGE's pushed only over JUDGE (or a JUDGE sub-tag), DECIDE's only over DECIDE; the mmhold514 line's
//      decide sum excludes JUDGE's sub-tags, its judge sum includes them; the walk classes;
//   P7 every perf540 line <= 491 bytes at maximal fields;
//   P8 the mid-arm guard: 96 is guarded (ON/OFF refused under a standing continuous arm, a bare read allowed);
//   P9 the kext glue (source pins): OFF at boot, the verb the only writer, the guard; every timed site's OFF shape (one load, the
//      clock only behind it) and count; START / STOP placement; the tick's arm source; the MM / log / cg-check sites; the SDMA fix.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "gfx_perf540.h"
#include "gfx_commit.h"

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
    std::string s; FILE *f = p ? std::fopen(p, "rb") : nullptr;
    if (!f) return s;
    char b[65536]; size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}
static uint32_t count(const std::string &s, const std::string &n)
{
    uint32_t c = 0; size_t p = 0;
    if (n.empty()) return 0;
    while ((p = s.find(n, p)) != std::string::npos) { c++; p += n.size(); }
    return c;
}
static std::string body_of(const std::string &s, const std::string &head)
{
    const size_t a = s.find(head);
    if (a == std::string::npos) return std::string();
    const size_t e = s.find("\n}\n", a);
    return s.substr(a, e == std::string::npos ? std::string::npos : e - a);
}
static bool ordered(const std::string &s, std::initializer_list<const char *> parts)
{
    size_t p = 0;
    for (const char *x : parts) { const size_t q = s.find(x, p); if (q == std::string::npos) return false; p = q + 1; }
    return true;
}

/* ---- P1 ---- */
static uint32_t gClk = 0;
static uint64_t clk() { gClk++; return 1000ull * gClk; }
static bool all_zero(const n48_pf_tot &t)
{
    const uint64_t *w = reinterpret_cast<const uint64_t *>(&t);
    for (uint32_t i = 0; i < (uint32_t)N48_PF_TOT_WORDS; i++) if (w[i]) return false;
    return true;
}
/* The kext's site shape: t = n48_pf_begin(on, clock); ...work...; if (t) { add(id, clock() - t) }. */
static void model_site(n48_pf_tot *acc, uint32_t on, uint32_t id)
{
    const uint64_t t = n48_pf_begin(on, &clk);
    if (t) { const uint64_t t1 = clk(); acc->t[id].n++; acc->t[id].ns += t1 - t; }
}
static void p1()
{
    gClk = 0;
    expect_u("P1 OFF: n48_pf_begin answers 0", n48_pf_begin(0u, &clk), 0u);
    expect_u("P1 OFF: ... and read no clock", gClk, 0u);
    expect_u("P1 ON: n48_pf_begin answers the clock (non-zero)", n48_pf_begin(1u, &clk) != 0u, 1u);
    expect_u("P1 ON: ... read once", gClk, 1u);
    n48_pf_tot acc; std::memset(&acc, 0, sizeof acc);
    gClk = 0;
    for (uint32_t i = 0; i < 10000u; i++) model_site(&acc, 0u, i % N48_PF_T_N);
    expect_u("P1 OFF identity (runtime): 10^4 site executions read no clock", gClk, 0u);
    expect_u("P1 OFF identity (runtime): ... and every accumulator stays zero", all_zero(acc) ? 1u : 0u, 1u);
    for (uint32_t i = 0; i < 100u; i++) model_site(&acc, 1u, N48_PF_T_HOOK);
    expect_u("P1 ON: one clock pair per site execution (200 reads for 100)", gClk, 200u);
    expect_u("P1 ON: the site's timer counts 100 calls, 100 us", acc.t[N48_PF_T_HOOK].n * 1000000ull + acc.t[N48_PF_T_HOOK].ns,
             100u * 1000000ull + 100u * 1000u);
    expect_u("P1 the totals are all uint64_t words (the element-wise delta relies on it)",
             sizeof(n48_pf_tot) % 8u == 0u && sizeof(n48_pf_acc) == 16u && sizeof(n48_pf_walk) == 32u &&
             sizeof(n48_pf_ext) == (3u * N48_PF_MM_TAGS + 8u + 2u) * 8u, 1u);
}

/* ---- P2 ---- */
static void p2()
{
    const uint64_t il[] = { 0, 499, 500, 1999, 2000, 4999, 5000, 9999, 10000, 19999, 20000, 49999, 50000, 99999, 100000, ~0ull };
    const uint32_t ib[] = { 0, 0,   1,   1,    2,    2,    3,    3,    4,     4,     5,     5,     6,     6,     7,      7 };
    uint32_t ok = 1u;
    for (uint32_t i = 0; i < sizeof il / sizeof il[0]; i++) if (n48_pf_idle_bucket(il[i]) != ib[i]) { ok = 0u; std::printf("  idle %llu -> %u\n", (unsigned long long)il[i], n48_pf_idle_bucket(il[i])); }
    expect_u("P2 the idle-gap buckets <0.5 <2 <5 <10 <20 <50 <100 >=100 ms at every boundary", ok, 1u);
    const uint64_t pl[] = { 0, 19999, 20000, 32999, 33000, 49999, 50000, 66999, 67000, 99999, 100000, 199999, 200000, 499999, 500000, ~0ull };
    const uint32_t pb[] = { 0, 0,     1,     1,     2,     2,     3,     3,     4,     4,     5,      5,      6,      6,      7,      7 };
    ok = 1u;
    for (uint32_t i = 0; i < sizeof pl / sizeof pl[0]; i++) if (n48_pf_present_bucket(pl[i]) != pb[i]) { ok = 0u; std::printf("  present %llu -> %u\n", (unsigned long long)pl[i], n48_pf_present_bucket(pl[i])); }
    expect_u("P2 the present-gap buckets <20 <33 <50 <67 <100 <200 <500 >=500 ms at every boundary", ok, 1u);
    uint32_t mono = 1u;
    for (uint64_t us = 1; us < 2000000ull; us += 997ull) {
        if (n48_pf_present_bucket(us) < n48_pf_present_bucket(us - 1)) mono = 0u;
        if (n48_pf_idle_bucket(us) < n48_pf_idle_bucket(us - 1)) mono = 0u;
    }
    expect_u("P2 both bucket functions are monotone and stay in [0, 8)", mono && n48_pf_present_bucket(~0ull) < N48_PF_GAP_B, 1u);
}

/* ---- P3 ---- */
static void secv_set(n48_pf_secv *v, uint64_t x) { for (uint32_t k = 0; k < N48_PF_S_N; k++) v->v[k] = x * (k + 1u); }
static void p3()
{
    n48_pf_win w; std::memset(&w, 0, sizeof w);
    n48_pf_secv v; n48_pf_secline L; std::memset(&L, 0, sizeof L);
    expect_u("P3 not armed: nothing wanted", n48_pf_win_want(&w, 0u, 5000000ull), 0u);
    expect_u("P3 armed, no window: wanted (the open)", n48_pf_win_want(&w, 1u, 5000000ull), 1u);
    secv_set(&v, 10u);
    expect_u("P3 the open prints nothing", n48_pf_win_close(&w, &v, 5000000ull, &L), 0u);
    expect_u("P3 < 1 s later: not wanted", n48_pf_win_want(&w, 1u, 5999999ull), 0u);
    expect_u("P3 1 s later: wanted", n48_pf_win_want(&w, 1u, 6000000ull), 1u);
    secv_set(&v, 13u);
    const uint32_t due = n48_pf_win_close(&w, &v, 6000000ull, &L);
    uint32_t dOk = 1u;
    for (uint32_t k = 0; k < N48_PF_S_N; k++) if (L.v[k] != 3u * (k + 1u)) dOk = 0u;
    expect_u("P3 the first line: that second's deltas (13 - 10 per field), line 1, +0 ms, 1000 ms",
             due && dOk && L.line == 1u && L.at_ms == 0u && L.len_ms == 1000u, 1u);
    // run 300 more windows: exactly 240 lines this arm, 61 suppressed
    uint32_t lines = 1u;
    uint64_t now = 6000000ull;
    for (uint32_t i = 0; i < 300u; i++) {
        now += 1000000ull;
        secv_set(&v, 14u + i);
        if (n48_pf_win_want(&w, 1u, now) && n48_pf_win_close(&w, &v, now, &L)) lines++;
    }
    expect_u("P3 THE CAP: 240 lines per arm", lines, (uint64_t)N48_PF_SEC_LINES);
    expect_u("P3 ... the rest counted suppressed (301 windows - 240)", w.suppressed, 61u);
    expect_u("P3 not armed closes the window", n48_pf_win_want(&w, 0u, now + 5000000ull) == 0u && w.open == 0u, 1u);
    expect_u("P3 a new arm opens a fresh window and budget", n48_pf_win_want(&w, 1u, now + 9000000ull), 1u);
    secv_set(&v, 1000u);
    (void)n48_pf_win_close(&w, &v, now + 9000000ull, &L);
    secv_set(&v, 1001u);
    const uint32_t d2 = n48_pf_win_want(&w, 1u, now + 10000000ull) && n48_pf_win_close(&w, &v, now + 10000000ull, &L);
    expect_u("P3 ... its first line is line 1 again, with the new arm's base", d2 && L.line == 1u && L.v[0] == 1u, 1u);
    // a counter that went backwards reads 0 (never a huge unsigned)
    secv_set(&v, 5u);
    const uint32_t d3 = n48_pf_win_want(&w, 1u, now + 11000000ull) && n48_pf_win_close(&w, &v, now + 11000000ull, &L);
    expect_u("P3 a backwards counter reads 0", d3 && L.v[0] == 0u, 1u);
    // secv_of: the per-second fields come from the right totals
    n48_pf_tot t; std::memset(&t, 0, sizeof t);
    t.t[N48_PF_T_HOOK].n = 7; t.t[N48_PF_T_HOOK].ns = 70; t.t[N48_PF_T_JUDGE].ns = 50;
    t.t[N48_PF_T_XLAT_OK].ns = 1; t.t[N48_PF_T_XLAT_GREF].ns = 2; t.t[N48_PF_T_XLAT_VREF].ns = 4;
    t.x.mm_ns[N48_MMT_JUDGE] = 1; t.x.mm_ns[N48_MMT_J_WALK] = 2; t.x.mm_ns[N48_MMT_DECIDE] = 4; t.x.mm_ns[N48_MMT_COMMIT] = 8;
    t.x.mm_ns[N48_MMT_OTHER] = 100; t.x.mm_ns[N48_MMT_RESIDENCY] = 200; t.x.mm_ns[N48_MMT_FCVERIFY] = 400;
    t.w[N48_PF_W_JUDGE].calls = 3; t.w[N48_PF_W_OTHER].calls = 4;
    t.t[N48_PF_T_HMAP_R].n = 5; t.t[N48_PF_T_HMAP_W].n = 6; t.x.lg_n[0] = 1; t.x.lg_n[1] = 2;
    t.c[N48_PF_C_PRESENTS] = 9; t.c[N48_PF_C_COPIED] = 8; t.c[N48_PF_C_IDLE_NS] = 77;
    n48_pf_secv o; n48_pf_secv_of(&t, &o);
    expect_u("P3 secv_of: calls 7 wall 70 judge 50 xlat 7 mm 15 (hook takers only) walks 7 hmaps 11 log 3 presents 9 copied 8 idle 77",
             o.v[N48_PF_S_CALLS] == 7 && o.v[N48_PF_S_WALL] == 70 && o.v[N48_PF_S_JUDGE] == 50 && o.v[N48_PF_S_XLAT] == 7 &&
             o.v[N48_PF_S_MM] == 15 && o.v[N48_PF_S_WALKS] == 7 && o.v[N48_PF_S_HMAPS] == 11 && o.v[N48_PF_S_LOG] == 3 &&
             o.v[N48_PF_S_PRESENTS] == 9 && o.v[N48_PF_S_COPIED] == 8 && o.v[N48_PF_S_IDLE] == 77, 1u);
}

/* ---- P4 ---- */
static void fill(n48_pf_tot *t, uint64_t base)
{
    uint64_t *w = reinterpret_cast<uint64_t *>(t);
    for (uint32_t i = 0; i < (uint32_t)N48_PF_TOT_WORDS; i++) w[i] = base + i;
}
static void p4()
{
    n48_pf_arm a; std::memset(&a, 0, sizeof a);
    n48_pf_tot cur, out;
    fill(&cur, 1000u);
    std::memset(&out, 0xAB, sizeof out);
    expect_u("P4 a STOP with no START: nothing (0)", n48_pf_arm_stop(&a, &cur, &out), 0u);
    fill(&cur, 1000u);
    n48_pf_arm_start(&a, &cur, 55u);
    expect_u("P4 the START is valid and stamped", a.valid == 1u && a.start_us == 55u, 1u);
    fill(&cur, 1000u);
    uint64_t *cw = reinterpret_cast<uint64_t *>(&cur);
    for (uint32_t i = 0; i < (uint32_t)N48_PF_TOT_WORDS; i++) cw[i] += 3u * i + 1u;   // the arm's own activity
    cur.hook_max_ns = 12345u;
    expect_u("P4 the STOP answers 1", n48_pf_arm_stop(&a, &cur, &out), 1u);
    uint32_t ok = 1u;
    const uint64_t *ow = reinterpret_cast<const uint64_t *>(&out);
    const uint32_t maxWord = (uint32_t)(offsetof(n48_pf_tot, hook_max_ns) / 8u);
    for (uint32_t i = 0; i < (uint32_t)N48_PF_TOT_WORDS; i++) if (i != maxWord && ow[i] != 3u * i + 1u) ok = 0u;
    expect_u("P4 DELTAS, NOT TOTALS: every word is the arm's own activity (3i + 1), never since-boot (>= 1000)", ok, 1u);
    expect_u("P4 hook_max passes through as the arm's max", out.hook_max_ns, 12345u);
    expect_u("P4 the STOP closed the arm: a second STOP prints nothing", n48_pf_arm_stop(&a, &cur, &out), 0u);
    // a counter that went backwards (wrap / reset) reads 0
    n48_pf_tot s2; fill(&s2, 50u); n48_pf_arm_start(&a, &s2, 1u);
    n48_pf_tot c2; fill(&c2, 10u);
    (void)n48_pf_arm_stop(&a, &c2, &out);
    expect_u("P4 a backwards word reads 0", out.t[0].n == 0u && out.c[0] == 0u, 1u);
}

/* ---- P5 ---- */
static void p5()
{
    static n48_pf_dm d; std::memset(&d, 0, sizeof d);
    uint32_t any = 9u;
    expect_u("P5 a cold key: no hit", n48_pf_dm_probe(&d, 0x8000000000001234ull, 1u, &any) == 0u && any == 0u, 1u);
    expect_u("P5 the same key, same generation: per-generation hit and any-time hit",
             n48_pf_dm_probe(&d, 0x8000000000001234ull, 1u, &any) == 1u && any == 1u, 1u);
    expect_u("P5 the same key, a later generation: any-time hit only", n48_pf_dm_probe(&d, 0x8000000000001234ull, 2u, &any) == 0u && any == 1u, 1u);
    // a colliding key evicts
    uint64_t k2 = 0x8000000000001235ull;
    while (n48_pf_dm_ix(k2) != n48_pf_dm_ix(0x8000000000001234ull)) k2++;
    (void)n48_pf_dm_probe(&d, k2, 2u, &any);
    expect_u("P5 a colliding key evicts (direct-mapped)", n48_pf_dm_probe(&d, 0x8000000000001234ull, 2u, &any) == 0u && any == 0u, 1u);
    expect_u("P5 the walk key is never 0 and names root and page",
             n48_pf_walk_key(0, 0) != 0u && n48_pf_walk_key(0x1000, 0x5000) != n48_pf_walk_key(0x2000, 0x5000) &&
             n48_pf_walk_key(0x1000, 0x5000) == n48_pf_walk_key(0x1000, 0x5fff), 1u);
    uint32_t valid = 9u;
    expect_u("P5 cross-frame: an unfilled (stage, va) has no tag", n48_pf_xf_ask(&d, 0u, 0x400020700ull, 7u, &valid) == 0u && valid == 0u, 1u);
    n48_pf_xf_fill(&d, 0u, 0x400020700ull, 7u);
    expect_u("P5 cross-frame: after a fill, the tag, valid at the same mark", n48_pf_xf_ask(&d, 0u, 0x400020700ull, 7u, &valid) == 1u && valid == 1u, 1u);
    expect_u("P5 cross-frame: a moved mark: the tag, not valid", n48_pf_xf_ask(&d, 0u, 0x400020700ull, 8u, &valid) == 1u && valid == 0u, 1u);
    expect_u("P5 cross-frame: another stage at the same VA is another key", n48_pf_xf_ask(&d, 1u, 0x400020700ull, 7u, &valid), 0u);
}

/* ---- P6 ---- */
static void p6()
{
    n48_mmt_table t; std::memset(&t, 0, sizeof t);
    const uintptr_t A = 0x1000u;
    expect_u("P6 the tag space: 16 (12 + JUDGE's four)", N48_MMT_N == 16u && N48_MMT_J_WALK == 12u && N48_MMT_J_ASK == 15u &&
             N48_PF_MM_TAGS >= N48_MMT_N, 1u);
    const uint32_t tj = n48_mmt_push(&t, A, N48_MMT_JUDGE);
    const uint32_t k1 = n48_mmt_push_sub(&t, A, N48_MMT_J_DESC);
    expect_u("P6 a JUDGE sub-tag over JUDGE is pushed", k1 != 0u && n48_mmt_lookup(&t, A) == N48_MMT_J_DESC, 1u);
    const uint32_t k2 = n48_mmt_push_sub(&t, A, N48_MMT_J_WALK);
    expect_u("P6 ... and nests (WALK inside DESC)", k2 != 0u && n48_mmt_lookup(&t, A) == N48_MMT_J_WALK, 1u);
    n48_mmt_pop(&t, A, k2);
    expect_u("P6 ... and pops back to DESC", n48_mmt_lookup(&t, A) == N48_MMT_J_DESC, 1u);
    n48_mmt_pop(&t, A, k1);
    expect_u("P6 ... and to JUDGE", n48_mmt_lookup(&t, A) == N48_MMT_JUDGE, 1u);
    expect_u("P6 a DECIDE sub-tag over JUDGE is refused (unchanged 0.0.515 rule)", n48_mmt_push_sub(&t, A, N48_MMT_D_TGT), 0u);
    n48_mmt_pop(&t, A, tj);
    const uint32_t td = n48_mmt_push(&t, A, N48_MMT_DECIDE);
    expect_u("P6 a JUDGE sub-tag over DECIDE is refused", n48_mmt_push_sub(&t, A, N48_MMT_J_WALK) == 0u && n48_mmt_lookup(&t, A) == N48_MMT_DECIDE, 1u);
    const uint32_t k3 = n48_mmt_push_sub(&t, A, N48_MMT_D_IB);
    expect_u("P6 a DECIDE sub-tag over DECIDE is still pushed", k3 != 0u && n48_mmt_lookup(&t, A) == N48_MMT_D_IB, 1u);
    n48_mmt_pop(&t, A, k3); n48_mmt_pop(&t, A, td);
    const uint32_t tc = n48_mmt_push(&t, A, N48_MMT_COMMIT);
    expect_u("P6 no sub-tag over COMMIT", n48_mmt_push_sub(&t, A, N48_MMT_J_WALK) == 0u && n48_mmt_push_sub(&t, A, N48_MMT_D_FENCE) == 0u, 1u);
    n48_mmt_pop(&t, A, tc);
    expect_u("P6 no slot: nothing", n48_mmt_push_sub(&t, A, N48_MMT_J_WALK) == 0u && t.live == 0u, 1u);
    n48_mmhold_stats s; std::memset(&s, 0, sizeof s);
    s.count[N48_MMT_DECIDE] = 1; s.count[N48_MMT_D_IB] = 2; s.count[N48_MMT_J_WALK] = 100; s.count[N48_MMT_JUDGE] = 10;
    s.count[N48_MMT_J_ASK] = 1000;
    expect_u("P6 the decide sum is DECIDE + D sub-tags only (3, never JUDGE's 1100)", n48_mmhold_dsum(&s, 0u), 3u);
    expect_u("P6 the judge sum is JUDGE + its sub-tags (1110)", n48_mmhold_jsum(&s, 0u), 1110u);
    expect_u("P6 walk classes", n48_pf_walk_class(N48_MMT_JUDGE) == N48_PF_W_JUDGE && n48_pf_walk_class(N48_MMT_J_WALK) == N48_PF_W_JUDGE &&
             n48_pf_walk_class(N48_MMT_J_PGMID) == N48_PF_W_PGMID && n48_pf_walk_class(N48_MMT_J_DESC) == N48_PF_W_DESC &&
             n48_pf_walk_class(N48_MMT_J_ASK) == N48_PF_W_ASK && n48_pf_walk_class(N48_MMT_D_TGT) == N48_PF_W_DECIDE &&
             n48_pf_walk_class(N48_MMT_DECIDE) == N48_PF_W_DECIDE && n48_pf_walk_class(N48_MMT_COMMIT) == N48_PF_W_COMMIT &&
             n48_pf_walk_class(N48_MMT_CAPTURE) == N48_PF_W_OTHER && n48_pf_walk_class(N48_MMT_OTHER) == N48_PF_W_OTHER, 1u);
    n48_pf_ext x; std::memset(&x, 0, sizeof x);
    for (uint32_t k = 0; k < N48_PF_MM_TAGS; k++) { x.mm_n[k] = 1u << k; x.mm_ns[k] = 1u << k; x.mm_dw[k] = 1u << k; }
    uint64_t dec[3], oth[3];
    n48_pf_mm_fold(&x, 0u, dec); n48_pf_mm_fold(&x, 1u, oth);
    expect_u("P6 the MM fold: decide = DECIDE + D sub-tags (bits 2, 7-11); other = OTHER, CAPTURE, FCVERIFY, RESIDENCY (0, 4, 5, 6)",
             dec[0] == ((1u << 2) | 0xF80u) && oth[0] == ((1u << 0) | (1u << 4) | (1u << 5) | (1u << 6)), 1u);
    expect_u("P6 the per-second MM excludes OTHER, FCVERIFY, RESIDENCY",
             n48_pf_mm_hook_tag(N48_MMT_OTHER) == 0u && n48_pf_mm_hook_tag(N48_MMT_FCVERIFY) == 0u &&
             n48_pf_mm_hook_tag(N48_MMT_RESIDENCY) == 0u && n48_pf_mm_hook_tag(N48_MMT_JUDGE) == 1u &&
             n48_pf_mm_hook_tag(N48_MMT_J_ASK) == 1u && n48_pf_mm_hook_tag(N48_MMT_CAPTURE) == 1u, 1u);
}

/* ---- P7 ---- */
static uint32_t fits(const char *what, int n)
{
    std::printf("  %-12s %d bytes\n", what, n);
    return n > 0 && (unsigned)n <= N48_LOG_CAP_BODY ? 1u : 0u;
}
static void p7()
{
    n48_pf_tot d; std::memset(&d, 0xFF, sizeof d);   // every counter at its widest (20 digits)
    for (uint32_t k = 0; k < N48_PF_T_N; k++) d.t[k].ns = ~0ull;
    char b[2048];
    uint32_t ok = 1u;
    uint64_t dec[3] = { ~0ull, ~0ull, ~0ull }, oth[3] = { ~0ull, ~0ull, ~0ull };
    ok &= fits("L1", std::snprintf(b, sizeof b, N48_PF_L1_FMT, N48_PF_L1_ARGS(&d)));
    ok &= fits("L2", std::snprintf(b, sizeof b, N48_PF_L2_FMT, N48_PF_L2_ARGS(&d)));
    ok &= fits("L3", std::snprintf(b, sizeof b, N48_PF_L3_FMT, N48_PF_L3_ARGS(&d)));
    ok &= fits("L4", std::snprintf(b, sizeof b, N48_PF_L4_FMT, N48_PF_L4_ARGS(&d)));
    ok &= fits("L5", std::snprintf(b, sizeof b, N48_PF_L5_FMT, N48_PF_L5_ARGS(&d)));
    ok &= fits("L6", std::snprintf(b, sizeof b, N48_PF_L6_FMT, N48_PF_L6_ARGS(&d)));
    ok &= fits("L7", std::snprintf(b, sizeof b, N48_PF_L7_FMT, N48_PF_L7_ARGS(&d)));
    ok &= fits("L8", std::snprintf(b, sizeof b, N48_PF_L8_FMT, N48_PF_L8_ARGS(&d, dec, oth)));
    ok &= fits("L9", std::snprintf(b, sizeof b, N48_PF_L9_FMT, N48_PF_L9_ARGS(&d)));
    ok &= fits("L10", std::snprintf(b, sizeof b, N48_PF_L10_FMT, N48_PF_L10_ARGS(&d)));
    ok &= fits("L11", std::snprintf(b, sizeof b, N48_PF_L11_FMT, N48_PF_L11_ARGS(&d)));
    ok &= fits("L12", std::snprintf(b, sizeof b, N48_PF_L12_FMT, N48_PF_L12_ARGS(&d)));
    ok &= fits("L13", std::snprintf(b, sizeof b, N48_PF_L13_FMT, N48_PF_L13_ARGS(&d)));
    ok &= fits("L14", std::snprintf(b, sizeof b, N48_PF_L14_FMT, N48_PF_L14_ARGS(&d)));
    ok &= fits("STOP", std::snprintf(b, sizeof b, N48_PF_STOP_FMT, ~0ull, 4294967295u, ~0ull));
    ok &= fits("NOSTART", std::snprintf(b, sizeof b, N48_PF_NOSTART_FMT));
    const char *hows[] = { " - `gfxneuter 96` read only, unchanged", " - `gfxneuter 96` REFUSED - a continuous arm stands, unchanged",
                           " - `gfxneuter 96` CHANGED BY THIS VERB", " - `gfxneuter 96` REFUSED (unknown M), unchanged" };
    for (const char *h : hows) {
        ok &= fits("STATUS ON", std::snprintf(b, sizeof b, N48_PF_STATUS_FMT, N48_PF_ON_TXT, h, ~0ull, ~0ull, ~0ull, 4294967295u, ~0ull));
        ok &= fits("STATUS OFF", std::snprintf(b, sizeof b, N48_PF_STATUS_FMT, N48_PF_OFF_TXT, h, ~0ull, ~0ull, ~0ull, 4294967295u, ~0ull));
    }
    n48_pf_secline L; for (uint32_t k = 0; k < N48_PF_S_N; k++) L.v[k] = ~0ull;
    L.at_ms = ~0ull; L.len_ms = ~0ull; L.line = 4294967295u;
    ok &= fits("SEC", std::snprintf(b, sizeof b, N48_PF_SEC_FMT, N48_PF_SEC_ARGS(L)));
    expect_u("P7 every perf540 line fits 491 bytes at maximal fields", ok, 1u);
}

/* ---- P8 ---- */
static void p8()
{
    expect_u("P8 96 is in the mid-arm guarded list", n48_cm_cont_switch_guarded(96u), 1u);
    expect_u("P8 ON/OFF refused while a continuous arm stands", n48_cm_cont_switch_refused(96u, 0u, 1u, (uint32_t)N48_CM_SHOT_ARMED), 1u);
    expect_u("P8 a bare read is allowed while armed", n48_cm_cont_switch_refused(96u, 1u, 1u, (uint32_t)N48_CM_SHOT_ARMED), 0u);
    expect_u("P8 not refused with no arm, under a one-shot, once SPENT",
             n48_cm_cont_switch_refused(96u, 0u, 0u, (uint32_t)N48_CM_SHOT_OFF) == 0u &&
             n48_cm_cont_switch_refused(96u, 0u, 0u, (uint32_t)N48_CM_SHOT_ARMED) == 0u &&
             n48_cm_cont_switch_refused(96u, 0u, 1u, (uint32_t)N48_CM_SHOT_SPENT) == 0u, 1u);
    // build 0.0.543: 100, 101 and 102 are claimed (switches 100 / 101 / 102); 103 is the first unclaimed number.
    // build 0.0.544: 103 is claimed (switch 103, the stale-input gate). build 0.0.547: 104-105 too; 0.0.548: 106; 0.0.550: 107-108; 0.0.552: 109-110; 0.0.553: 111; 0.0.554: 112; 113 is the first unclaimed number (114 the one these tests probe).
    expect_u("P8 97 (item 5, the table cache) is guarded too; 114 (unclaimed; 112: 0.0.554, 111: 0.0.553, 109-110: 0.0.552, 107-108: 0.0.550, 106: 0.0.548, 98/99: 0.0.541, 100-102: 0.0.543, 103: 0.0.544, 104-105: 0.0.547) is not",
             n48_cm_cont_switch_guarded(97u) == 1u && n48_cm_cont_switch_refused(97u, 0u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 1u &&
             n48_cm_cont_switch_refused(97u, 1u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 0u && n48_cm_cont_switch_guarded(114u) == 0u, 1u);
    expect_u("P8 the values: ON 352, OFF 608", (96u | 1u << 8) == 352u && (96u | 2u << 8) == 608u && N48_PF_OFF == 0u, 1u);
}

/* ---- P9: the glue ---- */
static void p9(const char *ahhp, const char *brp, const char *logp, const char *ttlp)
{
    const std::string s = slurp(ahhp), b = slurp(brp), l = slurp(logp), t = slurp(ttlp);
    expect_u("P9 the four sources were read", !s.empty() && !b.empty() && !l.empty() && !t.empty(), 1u);
    // OFF at boot, the verb the only writer
    expect_u("P9 gN48Pf540On is defined once, 0 at boot (OFF), in Navi48Bringup.cpp",
             count(b, "volatile uint32_t gN48Pf540On { 0u };") == 1u && count(b, "gN48Pf540On {") == 1u &&
             count(s, "gN48Pf540On {") == 0u, 1u);
    expect_u("P9 the switch's only writers are the verb's two stores (M 1 ON, M 2 OFF)",
             count(s, "__atomic_store_n(&gN48Pf540On, (uint32_t)N48_PF_ON, __ATOMIC_RELEASE); changed96 = 1;") == 1u &&
             count(s, "__atomic_store_n(&gN48Pf540On, (uint32_t)N48_PF_OFF, __ATOMIC_RELEASE); changed96 = 1;") == 1u &&
             count(s, "__atomic_store_n(&gN48Pf540On") == 2u && count(s, "gN48Pf540On =") == 0u &&
             count(b, "__atomic_store_n(&gN48Pf540On") == 0u && count(b, "gN48Pf540On =") == 0u &&
             count(l, "gN48Pf540On =") == 0u && count(l, "__atomic_store_n(&gN48Pf540On") == 0u, 1u);
    const std::string verb = body_of(s, "    } else if ((arg & 0xffull) == 96ull) {");
    expect_u("P9 the verb: selector once, the guard asked with its own number, refusal before either store",
             count(s, "} else if ((arg & 0xffull) == 96ull) {") == 1u &&
             ordered(s, { "} else if ((arg & 0xffull) == 96ull) {",
                          "const bool contRefused96 = n48_cm_cont_switch_refused(96u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;",
                          "} else if (contRefused96) {\n            st = 5;",
                          "} else if (m == 1u) {\n            __atomic_store_n(&gN48Pf540On, (uint32_t)N48_PF_ON",
                          "} else if (m == 2u) {\n            __atomic_store_n(&gN48Pf540On, (uint32_t)N48_PF_OFF",
                          "st = 11;", "pf_report_line(" }), 1u);
    // the timed sites' OFF shape
    expect_u("P9 pf_on / pf_t0 / pf_note: one load; the clock only through n48_pf_begin; the end a test of the start",
             count(s, "static inline uint32_t pf_on() { return __atomic_load_n(&gN48Pf540On, __ATOMIC_RELAXED); }") == 1u &&
             count(s, "static inline uint64_t pf_t0() { return n48_pf_begin(pf_on(), &pf_ticks); }") == 1u &&
             count(s, "static inline void pf_note(uint32_t id, uint64_t t0) { if (t0) pf_note_slow(id, t0); }") == 1u &&
             count(s, "explicit Pf540Scope(uint32_t id, uint32_t sub = 0u) : id_(id), t_(pf_t0()), tok_(t_ && sub ? navi48_mm_taker_push_sub(sub) : 0u) {}") == 1u &&
             count(s, "~Pf540Scope() { if (tok_) navi48_mm_taker_pop(tok_); pf_note(id_, t_); }") == 1u, 1u);
    // every pf_ticks() read lives in an ON-only function: count its uses and where
    expect_u("P9 pf_ticks: its definition, and reads only in the ON-only helpers below (6 in all; pf_t0 passes its address)",
             count(s, "pf_ticks()") == 6u && count(s, "&pf_ticks") == 1u, 1u);
    const char *onOnly[] = {
        "static __attribute__((noinline)) void pf_note_slow(uint32_t id, uint64_t t0) { pf_add(id, pf_ns(t0, pf_ticks())); }",
        "static __attribute__((noinline)) void pf_hmap(uint32_t id, uint64_t t0, uint64_t page)\n{\n    pf_add(id, pf_ns(t0, pf_ticks()));",
        // build 0.0.541 (switch 98): gfxc_page_pf gained a defaulted `lvOut` (the walk cache's level count); the first read is unchanged
        ("static __attribute__((noinline)) bool gfxc_page_pf(const GfxcVm &vm, uint64_t va, uint64_t &pageBase, bool &isSys, uint64_t *leafOut,\n"
         "                                                   uint32_t *lvOut = nullptr) {\n    const uint64_t t0 = pf_ticks();"),
        "static __attribute__((noinline)) void pf_hook_exit(uint64_t t0, uint32_t tok)\n{\n    const uint64_t t1 = pf_ticks();" };
    uint32_t oo = 0u; for (const char *x : onOnly) oo += count(s, x);
    expect_u("P9 ... pf_note_slow, pf_hmap, gfxc_page_pf, pf_hook_exit (each reached only past a load of the switch)", oo, 4u);
    // the timed sites, each exactly once, each gated on one load
    const char *sites[] = {
        "    const uint32_t pfTok = pf_on() ? pf_hook_enter(t0) : 0u;",
        "    if (pfTok) pf_hook_exit(t0, pfTok);",
        "            const uint64_t pfW = pf_t0();", "            pf_note(N48_PF_T_WSCLASS, pfW);",
        "    const uint64_t pfG = pf_t0();", "    pf_note(N48_PF_T_GCAP, pfG);",
        "    if (pf_on()) pf_note_slow(N48_PF_T_JUDGE, wtDecide0);",
        "        pf_note(N48_PF_T_ORIG_PASS, pfO);", "        pf_note(N48_PF_T_ORIG_NOTWS, pfO);",
        "        pf_note(N48_PF_T_ORIG_REFUSED, pfO);", "            pf_note(N48_PF_T_ORIG_XLAT, pfO);",
        "    pf_note(N48_PF_T_ORIG_NEUTER, pfO);",
        "    Pf540Scope pfs(N48_PF_T_GATHER);",
        "            const uint64_t pfI = pf_t0();", "            pf_note(N48_PF_T_IDENT, pfI);",
        "                  if (pf_on()) pf_add(N48_PF_T_POLICY, mibNs); }",
        "        const uint64_t pfR = pf_t0();", "        pf_note(N48_PF_T_RINGPOLL, pfR);",
        "    const uint64_t pfC = pf_t0();", "    pf_note(N48_PF_T_COMMIT, pfC);",
        "    if (ranPolicy && pf_on()) pf_xlat_outcome(commitOk, verdict == N48_XV_TRANSLATE ? 1u : 0u);",
        "    const uint64_t pfL = pf_t0();", "    pf_note(N48_PF_T_LEDGER, pfL);",
        "        if (pf_on()) gPfXlatNs = phXlat;",
        "    Pf540Scope pfs(N48_PF_T_CB_PGM, N48_MMT_J_PGMID);", "    Pf540Scope pfs(N48_PF_T_CB_DESC, N48_MMT_J_DESC);",
        "    Pf540Scope pfs(N48_PF_T_CB_TILED, N48_MMT_J_ASK);", "    Pf540Scope pfs(N48_PF_T_CB_DCC, N48_MMT_J_ASK);",
        "    Pf540Scope pfs(N48_PF_T_CB_CSN);", "    Pf540Scope pfs(N48_PF_T_UNIT_RETRY);", "    Pf540Scope pfs(N48_PF_T_CG_REDO);",
        "    if (pf_on()) return gfxc_page_pf(vm, va, pageBase, isSys, leafOut);",
        "    return gfxc_page_walk(vm, va, pageBase, isSys, leafOut, nullptr);",
        "            if (pfM) pf_hmap(N48_PF_T_HMAP_R, pfM, page);", "        if (pfM) pf_hmap(N48_PF_T_HMAP_W, pfM, page);",
        "    if (pf_on()) pf_present(wt0, copy);",
        "    if (m == &gPgmMemo && pf_on()) pf_pm_shadow(row, stage, va);",
        "    if (m == &gPgmMemo && pf_on()) n48_pf_xf_fill(&gPfXfDm, stage, va, fillMark);" };
    uint32_t once = 0u;
    for (const char *x : sites) { if (count(s, x) == 1u) once++; else std::printf("  site not found once: %s (%u)\n", x, count(s, x)); }
    expect_u("P9 every AppleHardwareHook.cpp timed site is present exactly once", once, (uint64_t)(sizeof sites / sizeof sites[0]));
    expect_u("P9 the two tex observers and the two host-map starts (2 each)",
             count(s, "    Pf540Scope pfs(N48_PF_T_CB_TEX);") == 2u && count(s, "const uint64_t pfM = pf_t0();") == 4u &&
             count(s, "const uint64_t pfO = pf_t0();") == 5u, 1u);
    // pf_t0(): its definition, Pf540Scope's ctor, the 2 host maps, the 11 listed begin sites (5 orig, ws_classify, gcap, ident,
    // ring-poll, commit_try, ledger). pf_on(): its definition, pf_t0's, the 9 listed gated sites (gfxc_page, present, 2 pgmid,
    // policy's phXlat, POLICY, the outcome, the judge, the wrapper), pf_arm_start / pf_arm_stop's guards and the status line;
    // item 6's two (the gate note in gfxsrc_commit_try, the decide frame's late lines; tests/gfx_late540_test.cpp pins them).
    // build 0.0.541 (switch 98): + 2 pf_on() - the walk cache's walk (gfxc_page_pf while ON, else gfxc_page_walk) and its two
    // perf540 counters (tests/gfx_wc98_test.cpp W7 pins gfxc_page_wc).
    // build 0.0.543 item A (switch 100): + 2 pf_t0() and 2 more host-map starts - hm100_host_read / hm100_host_write time a map
    // they made (none on an ON hit); item B (switch 101): + 1 pf_on() - wc101_pf's guard (tests/gfx_wc98_test.cpp W9 pins it).
    expect_u("P9 every pf_t0() / pf_on() use is one of the listed sites (pf_t0 17, pf_on 19)",
             count(s, "pf_t0()") * 100u + count(s, "pf_on()"), 17u * 100u + 19u);
    // START / STOP placement
    expect_u("P9 START: pf_arm_start() once, right after the START line's mmhold snapshot; STOP: pf_arm_stop() once, after STOP's",
             count(s, "pf_arm_start();") == 1u && count(s, "        pf_arm_stop();   ") == 1u && count(s, "pf_arm_stop();") == 2u &&
             ordered(s, { "HWLOG(N48_CM_CONT_START_FMT,", "navi48_mmhold_snapshot(\"START\");", "pf_arm_start();" }) &&
             ordered(s, { "HWLOG(N48_CM_CONT_STOP_FMT,", "navi48_mmhold_snapshot(\"STOP\");", "pf_arm_stop();" }) &&
             count(s, "navi48_mmhold_snapshot(\"START\");   // build 0.0.531 item 4 (log-only)\n"
                      "                  pf_arm_start();") == 1u, 1u);
    const std::string st = body_of(s, "static __attribute__((noinline)) void pf_arm_start()");
    const std::string sp = body_of(s, "static void pf_arm_stop()\n{");
    expect_u("P9 START takes the snapshot (ON only); STOP subtracts it (ON only) and prints nothing without it",
             ordered(st, { "if (!pf_on()) return;", "pf_build(&gPfCurArm);", "n48_pf_arm_start(&gPfArm, &gPfCurArm, latch_now_us());" }) &&
             ordered(sp, { "if (!pf_on()) return;", "pf_build(&gPfCurArm);", "if (!n48_pf_arm_stop(&gPfArm, &gPfCurArm, &gPfDelta)) {",
                           "HWLOG(N48_PF_NOSTART_FMT);", "return;", "HWLOG(N48_PF_STOP_FMT,", "HWLOG(N48_PF_L1_FMT, N48_PF_L1_ARGS(d));",
                           "HWLOG(N48_PF_L14_FMT, N48_PF_L14_ARGS(d));" }) && count(sp, "HWLOG(N48_PF_L") == 14u, 1u);
    const std::string tk = body_of(s, "static __attribute__((noinline)) void pf_tick(uint64_t now)");
    expect_u("P9 the tick: one owner (try flag), armed = hw_cm_armed(), the window's own want/close, the line only when due",
             ordered(tk, { "if (__atomic_exchange_n(&gPfWinBusy, 1u, __ATOMIC_ACQUIRE)) return;",
                           "if (n48_pf_win_want(&gPfWin, hw_cm_armed(), ns / 1000ull)) {", "pf_build(&gPfCurWin);",
                           "if (n48_pf_win_close(&gPfWin, &v, ns / 1000ull, &L)) HWLOG(N48_PF_SEC_FMT, N48_PF_SEC_ARGS(L));",
                           "__atomic_store_n(&gPfWinBusy, 0u, __ATOMIC_RELEASE);" }) &&
             count(s, "pf_tick(") == 3u /* the definition, the hook exit, the present */, 1u);
    const std::string wr = body_of(s, "static uint64_t hook_gfxCommitIB_timed(void *self, void *info) {");
    expect_u("P9 the wrapper: t0, then the enter (ON), the unchanged call, the glass531 note, then the exit (ON)",
             ordered(wr, { "const uint64_t t0 = wt_ticks();", "const uint32_t pfTok = pf_on() ? pf_hook_enter(t0) : 0u;",
                           "const uint64_t rv = hook_gfxCommitIB(self, info);", "if (self == gPm4GfxChan) wt_note_since(&gWtCommitIB, t0);",
                           "if (pfTok) pf_hook_exit(t0, pfTok);", "return rv;" }), 1u);
    const std::string pr = body_of(s, "uint32_t hw_p73_present(uint64_t phys, uint64_t presentNo)");
    expect_u("P9 the present: the gap note after 94's note, before the COPIED line and the unchanged glass531 note",
             ordered(pr, { "const uint64_t wt0 = wt_ticks();", "p94_present_note(", "if (pf_on()) pf_present(wt0, copy);",
                           "wt_note_since(&gWtPresent, wt0);\n    return copy;" }), 1u);
    // Navi48Bringup.cpp: the MM call and the cg check
    const std::string rd = body_of(b, "bool navi48_vram_read_mm(uint64_t vramOffset, uint32_t *dst, uint32_t dwords) {");
    expect_u("P9 navi48_vram_read_mm: the RAII after the argument checks (one load OFF), noted at its return (after the hold's note)",
             ordered(rd, { "if (vramOffset > size || bytes > size - vramOffset) return false;", "Pf540MmCall pfCall(dwords);",
                           "IOLockLock(gVramMmLock);", "if (gVramMmLock) mm_hold_note(h0, N48_MMT_LOOKUP, dwords);\n\treturn true;" }) &&
             rd.find("Pf540MmCall pfCall(") != std::string::npos && count(rd.substr(rd.find("Pf540MmCall pfCall(")), "return true;") == 1u &&
             count(rd.substr(rd.find("Pf540MmCall pfCall(")), "return false") == 0u &&
             count(b, "explicit Pf540MmCall(uint32_t dw) : t0_(0ull), dw_(dw) { if (__atomic_load_n(&gN48Pf540On, __ATOMIC_RELAXED)) clock_get_uptime(&t0_); }") == 1u &&
             count(b, "~Pf540MmCall() { if (t0_) pf540_mm_note(t0_, dw_); }") == 1u && count(b, "Pf540MmCall pfCall(") == 1u, 1u);
    const std::string cg = body_of(b, "uint32_t navi48_cg_seg_check(void) {");
    expect_u("P9 navi48_cg_seg_check: one load, the note before its one return",
             ordered(cg, { "if (__atomic_load_n(&gN48Pf540On, __ATOMIC_RELAXED)) clock_get_uptime(&pfC);", "const uint32_t r = n48_cg_check_ex(",
                           "if (pfC) pf540_cg_note(pfC);", "return r;" }) && count(cg, "return") == 1u, 1u);
    // n48log.cpp: one load, every clock read behind it
    const std::string lf = body_of(l, "void n48_logf(const char *fmt, ...)");
    expect_u("P9 n48_logf: the switch read once, every clock read behind `pf`, the note last",
             count(lf, "const uint32_t pf = __atomic_load_n(&gN48Pf540On, __ATOMIC_RELAXED);") == 1u &&
             count(lf, "clock_get_uptime(") == 4u && count(lf, "if (pf) clock_get_uptime(") == 3u &&
             count(lf, "if (pf) { clock_get_uptime(&pt3); navi48_pf540_log_note(pt0, pt1, pt2, pt3); }") == 1u &&
             ordered(lf, { "if (pf) clock_get_uptime(&pt0);", "vsnprintf(", "if (pf) clock_get_uptime(&pt1);", "IOLog(\"%s\", line);",
                           "if (pf) clock_get_uptime(&pt2);", "IOSimpleLockUnlockEnableInterrupt(l, s);", "navi48_pf540_log_note(" }), 1u);
    // Navi48Ttl.hpp: the gated JUDGE sub-scope
    expect_u("P9 Navi48PfSubScope: one load OFF, Navi48MmSubScope's push ON; Navi48MmSubScope itself unchanged",
             count(t, "explicit Navi48PfSubScope(uint32_t sub) : tok_(__atomic_load_n(&gN48Pf540On, __ATOMIC_RELAXED) ? navi48_mm_taker_push_sub(sub) : 0u) {}") == 1u &&
             count(t, "explicit Navi48MmSubScope(uint32_t sub) : tok_(navi48_mm_taker_push_sub(sub)) {}") == 1u &&
             count(t, "~Navi48MmSubScope() { navi48_mm_taker_pop(tok_); }") == 1u, 1u);
    // the SDMA line: logs only on a real change
    expect_u("P9 SDMA: the `enabled %u ring(s)` line is printed for the first 4 calls and then only when the count changes",
             count(s, "if (gSubmitCalls <= 4 || newly != gSubmitRingsLogged) {") == 1u &&
             count(s, "gSubmitRingsLogged = newly;") == 1u && count(s, "if (gSubmitCalls <= 4 || newly)\n") == 0u &&
             count(s, "static uint32_t gSubmitRingsLogged = 0xFFFFFFFFu;") == 1u, 1u);
    // the lines exist and are printed from this header's formats only
    expect_u("P9 every perf540 format is printed by the glue exactly once (STATUS, STOP, NOSTART, SEC, L1-L14)",
             count(s, "HWLOG(N48_PF_STATUS_FMT,") == 1u && count(s, "HWLOG(N48_PF_STOP_FMT,") == 1u &&
             count(s, "HWLOG(N48_PF_NOSTART_FMT)") == 1u && count(s, "HWLOG(N48_PF_SEC_FMT,") == 1u &&
             count(s, "HWLOG(N48_PF_L") == 14u && count(s, "\"perf540") == 0u, 1u);
}

int main(int argc, char **argv)
{
    p1(); p2(); p3(); p4(); p5(); p6(); p7(); p8();
    if (argc >= 5) p9(argv[1], argv[2], argv[3], argv[4]);
    else expect_u("the source files were given (AHH, Navi48Bringup.cpp, n48log.cpp, Navi48Ttl.hpp)", 0u, 1u);
    std::printf("%d run, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
