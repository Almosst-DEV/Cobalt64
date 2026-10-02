// gfx_wo99_test.cpp — build 0.0.541 item 6 (; switch 99, gfx_wo99.h, gfx_dep.h n48_dep_wo_relaxable): the
// witness-overflow relaxation.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/navi48-bringup/tests src/navi48-bringup/tests/gfx_wo99_test.cpp -o /tmp/wo99 && \
//         /tmp/wo99 src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/apple/gfx_dep.h \
//         src/navi48-bringup/src/apple/gfx_cp_build.h
// Covers:
//   O1 THE TRUTH TABLE: the witness-overflow rung is skipped ONLY with wo_mode ON AND consumer_enumerated 1 AND r5_mode 1 AND
//      cp_rows_free 1 AND r5_blind 0 - and only that rung, in its place: every rung above it (R5′'s blind bucket, R1-R4, the ring
//      neuter, the neuter-other, the target-unknown) still answers first, every rung below it still answers after;
//   O2 OFF identity: over 200 000 random worlds, n48_dep_check with wo_mode OFF or SHADOW (or ON but not relaxable) is the frozen
//      0.0.540 rule (tests/frozen/gfx_dep_check_2c91d368.h), reason AND detail; n48_dep_ok likewise;
//   O3 the fill: wo_mode copied only as OFF / ON / SHADOW, cp_rows_free only beside a positive enumeration; a zero source is OFF;
//   O4 the instrument's classification (n48_wo_gate), the per-second window, the histogram's bound;
//   O5 the safety argument, pinned in the code: the three 30-ON evaluators only null-check `wt` (no n48_cp_in_witness, no
//      wt->row); n48_cp_in_witness is called only by n48_cp_eval; the hazard filter's bits are set before its row cap; R1 refuses
//      a tiled input without a ledger proof; R5′ is BLIND for a truncated or unresolved target list;
//   O6 the kext glue (source pins): OFF at boot, the verb the only writer, the guard; the gather hands the switch and THIS frame's
//      evaluator's row-freedom; row-freedom reset every frame and set only by the three evaluators; the gate's decision is still
//      n48_dep_ok(&dw); the instruments after it;
//   O7 every wo99 line <= 491 bytes at maximal fields.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "gfx_wo99.h"
#include "gfx_r5win104.h"
#include "gfx_commit.h"
#include "frozen/gfx_dep_check_2c91d368.h"
#include "gfx_r3hit.h"   // build 0.0.550: the R3 hazard-hit line and the namer identity (log-only)

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
static uint64_t gRs = 0x853c49e6748fea9bull;
static uint32_t rnd() { gRs ^= gRs << 13; gRs ^= gRs >> 7; gRs ^= gRs << 17; return (uint32_t)(gRs >> 11); }

/* A clean, believable world: sampled, every observer, nothing counted. */
static n48_dep_world clean_world()
{
    n48_dep_world w; std::memset(&w, 0, sizeof w);
    w.sampled = 1u; w.observers = N48_DEP_OBS_REQUIRED;
    return w;
}

/* ---- O1: the truth table ---- */
static void o1()
{
    static const uint32_t modes[] = { N48_DEP_WO_OFF, N48_DEP_WO_ON, N48_DEP_WO_SHADOW, 2u };
    uint32_t bad = 0u, skipped = 0u;
    for (uint32_t mi = 0; mi < 4u; mi++)
        for (uint32_t en = 0; en < 2u; en++)
            for (uint32_t r5 = 0; r5 < 2u; r5++)
                for (uint32_t rf = 0; rf < 2u; rf++)
                    for (uint32_t bl = 0; bl < 2u; bl++) {
                        n48_dep_world w = clean_world();
                        w.wo_mode = modes[mi]; w.consumer_enumerated = en; w.r5_mode = r5; w.cp_rows_free = rf;
                        w.r5_blind = bl ? 3u : 0u; w.witness_over = 5u;
                        uint64_t d = 0;
                        const uint32_t got = n48_dep_check(&w, &d);
                        const uint32_t relax = (modes[mi] == N48_DEP_WO_ON && en && r5 && rf && !bl) ? 1u : 0u;
                        uint32_t want;
                        if (en && r5 && bl) want = N48_DEP_NEUTER_UNREADABLE;          /* R5′'s blind bucket answers FIRST */
                        else if (relax) want = N48_DEP_OK;
                        else want = N48_DEP_WITNESS_OVER;
                        if (got != want) { bad++; std::printf("  mode %u en %u r5 %u rf %u blind %u: got %u want %u\n",
                                                              modes[mi], en, r5, rf, bl, got, want); }
                        if (relax) skipped++;
                    }
    expect_u("O1 64 combinations: the witness rung is skipped ONLY with ON + enumerated + r5_mode 1 + rows-free + blind 0", bad, 0u);
    expect_u("O1 ... exactly one of the 64 relaxes", skipped, 1u);

    /* in its place: the rungs above it still refuse first (ON, relaxable, witness_over set) */
    struct { const char *name; uint32_t want; void (*set)(n48_dep_world &); } above[] = {
        { "consumer-inputs-unproven (R1-R4)", N48_DEP_CONSUMER_UNPROVEN, [](n48_dep_world &w) { w.consumer_inputs_unproven = 2u; } },
        { "ring-neuter", N48_DEP_RING_NEUTER, [](n48_dep_world &w) { w.ring_neuters = 1u; } },
        { "neuter-other", N48_DEP_NEUTER_OTHER, [](n48_dep_world &w) { w.neuter_other = 1u; } },
        { "target-unknown (an unresolved witness target)", N48_DEP_TARGET_UNKNOWN, [](n48_dep_world &w) { w.targets_unknown = 1u; } },
        { "not-observed", N48_DEP_NOT_OBSERVED, [](n48_dep_world &w) { w.observers = 0u; } },
        { "unaccounted", N48_DEP_UNACCOUNTED, [](n48_dep_world &w) { w.unaccounted = 4u; } },
        { "gfx-escaped (BELOW the rung: asked after it)", N48_DEP_GFX_ESCAPED, [](n48_dep_world &w) { w.gfx_escaped = 1u; } },
        { "vm-fault (BELOW)", N48_DEP_VM_FAULT, [](n48_dep_world &w) { w.vm_faults = 1u; } },
    };
    for (auto &a : above) {
        n48_dep_world w = clean_world();
        w.wo_mode = N48_DEP_WO_ON; w.consumer_enumerated = 1u; w.r5_mode = 1u; w.cp_rows_free = 1u; w.witness_over = 9u;
        a.set(w);
        uint64_t d = 0;
        char lbl[200]; std::snprintf(lbl, sizeof lbl, "O1 ON + relaxable + witness_over: %s still answers", a.name);
        expect_u(lbl, n48_dep_check(&w, &d), a.want);
    }
    /* r5_blind with every other relaxing condition: blind refuses (the relaxation is AFTER r5_blind) */
    {
        n48_dep_world w = clean_world();
        w.wo_mode = N48_DEP_WO_ON; w.consumer_enumerated = 1u; w.r5_mode = 1u; w.cp_rows_free = 1u; w.witness_over = 9u; w.r5_blind = 1u;
        uint64_t d = 0;
        expect_u("O1 a BLIND held-back frame (truncated / unresolved targets): NEUTER_UNREADABLE, the relaxation never reached",
                 n48_dep_check(&w, &d), N48_DEP_NEUTER_UNREADABLE);
        expect_u("O1 ... and n48_dep_wo_relaxable says no", n48_dep_wo_relaxable(&w), 0u);
    }
    /* n48_dep_check_m: relax 0 is the old rule whatever the mode; relax 1 needs relaxable */
    {
        n48_dep_world w = clean_world();
        w.wo_mode = N48_DEP_WO_SHADOW; w.consumer_enumerated = 1u; w.r5_mode = 1u; w.cp_rows_free = 1u; w.witness_over = 9u;
        uint64_t d = 0;
        expect_u("O1 SHADOW: n48_dep_check refuses witness-overflow (as today)", n48_dep_check(&w, &d), N48_DEP_WITNESS_OVER);
        expect_u("O1 SHADOW: n48_dep_check_m(relax 1) is the relaxed answer (clean)", n48_dep_check_m(&w, &d, 1u), N48_DEP_OK);
        w.consumer_enumerated = 0u;
        expect_u("O1 n48_dep_check_m(relax 1) with enumerated 0: still refused (source-neuter 0, so witness-overflow)",
                 n48_dep_check_m(&w, &d, 1u), N48_DEP_WITNESS_OVER);
    }
    expect_u("O1 n48_dep_wo_relaxable(null) is 0", n48_dep_wo_relaxable(nullptr), 0u);
}

/* ---- O2: OFF identity against the frozen 0.0.540 rule ---- */
static uint64_t rv() { const uint32_t r = rnd() % 8u; return r < 5u ? 0ull : (uint64_t)(rnd() % 4u) + 1ull; }
static void random_world(n48_dep_world &w)
{
    std::memset(&w, 0, sizeof w);
    w.sampled = (rnd() % 16u) ? 1u : rnd() % 3u;
    w.observers = (rnd() % 16u) ? N48_DEP_OBS_REQUIRED : (rnd() & N48_DEP_OBS_REQUIRED);
    w.source_neuters = rv(); w.ring_neuters = rv(); w.neuter_other = rv(); w.targets_unknown = rv(); w.witness_over = rv() ? 1u + rnd() % 70u : 0u;
    w.nonmonotone = (rnd() % 12u) ? 0u : 1u; w.unaccounted = (rnd() % 12u) ? 0u : 2u;
    w.gfx_escaped = rv(); w.gfx_native = rv(); w.sdma_untranslated = rv(); w.sdma_waits_removed = rv(); w.ring_resets = rv();
    w.engine_stalls = rv(); w.compute_queues = rv(); w.vm_faults = rv();
    w.consumer_enumerated = rnd() % 2u; w.neuter_unknown_writeset = rv(); w.consumer_inputs_unproven = rv();
    w.r5_mode = rnd() % 2u; w.r5_blind = rv(); w.cp_rows_free = rnd() % 2u;
}
static void o2()
{
    uint64_t diffOff = 0, diffShadow = 0, diffOnNotRelax = 0, diffOk = 0, onRelaxN = 0, onRelaxWrong = 0;
    for (uint32_t i = 0; i < 200000u; i++) {
        n48_dep_world w; random_world(w);
        uint64_t d0 = 0, d1 = 0;
        const uint32_t f = frozen540_dep_check(&w, &d0);
        w.wo_mode = N48_DEP_WO_OFF;
        if (n48_dep_check(&w, &d1) != f || d1 != d0) diffOff++;
        if (n48_dep_ok(&w) != (f == N48_DEP_OK ? 1u : 0u)) diffOk++;
        w.wo_mode = N48_DEP_WO_SHADOW;
        if (n48_dep_check(&w, &d1) != f || d1 != d0) diffShadow++;
        w.wo_mode = 2u;   /* the switch's OFF M, never a world value: must read as OFF */
        if (n48_dep_check(&w, &d1) != f || d1 != d0) diffOff++;
        w.wo_mode = N48_DEP_WO_ON;
        const uint32_t on = n48_dep_check(&w, &d1);
        if (!n48_dep_wo_relaxable(&w)) { if (on != f || d1 != d0) diffOnNotRelax++; }
        else {
            onRelaxN++;
            /* relaxable: identical unless the old rule stopped AT the witness rung, where the new one is the old rule with the
             * witness count zeroed */
            n48_dep_world z = w; z.witness_over = 0u;
            uint64_t dz = 0;
            const uint32_t fz = frozen540_dep_check(&z, &dz);
            const uint32_t want = (f == N48_DEP_WITNESS_OVER) ? fz : f;
            if (on != want) onRelaxWrong++;
        }
    }
    expect_u("O2 200 000 random worlds, wo_mode OFF (0, and the M value 2): reason and detail equal the frozen 0.0.540 rule", diffOff, 0u);
    expect_u("O2 ... wo_mode SHADOW: equal (SHADOW never relaxes)", diffShadow, 0u);
    expect_u("O2 ... wo_mode ON but not relaxable: equal", diffOnNotRelax, 0u);
    expect_u("O2 ... n48_dep_ok is the frozen rule's clean (OFF)", diffOk, 0u);
    expect_u("O2 ... ON and relaxable: exactly the frozen rule with the witness count removed", onRelaxWrong, 0u);
    expect_u("O2 the relaxable population was exercised (non-vacuous)", onRelaxN > 1000u ? 1u : 0u, 1u);
}

/* ---- O3: the fill ---- */
static void o3()
{
    n48_dep_src s; std::memset(&s, 0, sizeof s);
    s.gathered = 1u;
    n48_dep_mono m; std::memset(&m, 0, sizeof m);
    n48_dep_world w;
    n48_dep_fill(&s, &m, &w);
    expect_u("O3 a zero source: wo_mode OFF, rows-free 0", w.wo_mode == N48_DEP_WO_OFF && w.cp_rows_free == 0u ? 1u : 0u, 1u);
    static const uint32_t in[] = { 0u, 1u, 2u, 3u, 4u, 0xffffffffu }, out[] = { 0u, 1u, 0u, 3u, 0u, 0u };
    uint32_t ok = 1u;
    for (uint32_t i = 0; i < 6u; i++) { s.wo_mode = in[i]; n48_dep_fill(&s, &m, &w); if (w.wo_mode != out[i]) ok = 0u; }
    expect_u("O3 wo_mode copied only as OFF / ON / SHADOW (anything else OFF)", ok, 1u);
    s.wo_mode = N48_DEP_WO_ON; s.cp_rows_free = 1u;
    s.cp_enabled = 0u; s.cp_enumerated = 0u;
    n48_dep_fill(&s, &m, &w);
    expect_u("O3 rows-free without a positive enumeration: 0", w.cp_rows_free, 0u);
    s.cp_enabled = 1u; s.cp_enumerated = 1u;
    n48_dep_fill(&s, &m, &w);
    expect_u("O3 rows-free with 28 ON and an enumeration: 1", w.consumer_enumerated == 1u && w.cp_rows_free == 1u ? 1u : 0u, 1u);
    /* the whole chain: a 28+30 source with an arm-scoped overflow */
    s.r5_mode = 1u; s.cp_scoped = 1u; s.cp_wt_over = 3u; s.cp_wt_trunc = 2u; s.r5_blind = 0u;
    n48_dep_fill(&s, &m, &w);
    expect_u("O3 the fill carries witness_over = over + trunc (5), relaxable", w.witness_over == 5u && n48_dep_wo_relaxable(&w) ? 1u : 0u, 1u);
    s.wo_mode = N48_DEP_WO_OFF;
    n48_dep_fill(&s, &m, &w);
    uint64_t d = 0;
    const uint32_t r = n48_dep_check(&w, &d);
    expect_u("O3 ... and with 99 OFF the same source is refused as 0.0.540 refused it", r == frozen540_dep_check(&w, &d) ? 1u : 0u, 1u);
}

/* ---- O4: the instruments ---- */
static void o4()
{
    n48_wo_st st {};
    n48_dep_world w = clean_world();
    w.consumer_enumerated = 1u; w.r5_mode = 1u; w.cp_rows_free = 1u; w.witness_over = 4u;
    w.wo_mode = N48_DEP_WO_SHADOW;
    uint64_t d = 0;
    uint32_t ev = n48_wo_gate(&st, &w, n48_dep_check_m(&w, &d, 0u), n48_dep_check_m(&w, &d, 1u));
    expect_u("O4 SHADOW, relaxable, clean otherwise: WOULD_PASS counted", ev == N48_WO_E_WOULD_PASS && st.would_pass == 1u ? 1u : 0u, 1u);
    w.wo_mode = N48_DEP_WO_ON;
    ev = n48_wo_gate(&st, &w, n48_dep_check_m(&w, &d, 0u), n48_dep_check_m(&w, &d, 1u));
    expect_u("O4 ON: SKIPPED (and then clean) counted", ev == N48_WO_E_SKIPPED && st.skipped == 1u && st.skipped_ok == 1u ? 1u : 0u, 1u);
    w.gfx_escaped = 1u;
    ev = n48_wo_gate(&st, &w, n48_dep_check_m(&w, &d, 0u), n48_dep_check_m(&w, &d, 1u));
    expect_u("O4 ON with a later rung dirty: SKIPPED, not clean", ev == N48_WO_E_SKIPPED && st.skipped == 2u && st.skipped_ok == 1u ? 1u : 0u, 1u);
    w.gfx_escaped = 0u; w.ring_neuters = 1u;
    ev = n48_wo_gate(&st, &w, n48_dep_check_m(&w, &d, 0u), n48_dep_check_m(&w, &d, 1u));
    expect_u("O4 an earlier rung refused: nothing (the witness rung never decided)", ev, (uint64_t)N48_WO_E_NONE);
    w.ring_neuters = 0u; w.wo_mode = N48_DEP_WO_OFF;
    ev = n48_wo_gate(&st, &w, n48_dep_check_m(&w, &d, 0u), n48_dep_check_m(&w, &d, 1u));
    expect_u("O4 OFF: nothing counted but the gate", ev == N48_WO_E_NONE && st.gates == 5u && st.relaxable == 5u && st.skipped == 2u && st.would_pass == 1u ? 1u : 0u, 1u);
    /* the window */
    n48_wo_win win {}; n48_wo_secline L {};
    uint32_t lines = 0;
    for (uint32_t t = 0; t < 5000u; t++) lines += n48_wo_sec_tick(&win, 1u, 1000000ull + (uint64_t)t * 1000ull, N48_DEP_WITNESS_OVER, &L);
    expect_u("O4 the window: 5 s of gates at 1 kHz armed -> 4 lines (the first second opens it)", lines, 4u);
    expect_u("O4 ... each line one second's gates, all witness-overflow", L.gates == 1000u && L.cnt[N48_DEP_WITNESS_OVER] == 1000u ? 1u : 0u, 1u);
    expect_u("O4 not armed: no line, the window closes", n48_wo_sec_tick(&win, 0u, 9000000ull, 0u, &L) == 0u && win.open == 0u ? 1u : 0u, 1u);
    for (uint32_t t = 0; t < 300u; t++) (void)n48_wo_sec_tick(&win, 1u, 20000000ull + (uint64_t)t * 1000000ull, 0u, &L);
    expect_u("O4 at most 240 lines per arm, the rest suppressed", win.lines == N48_WO_SEC_LINES && win.suppressed > 0u ? 1u : 0u, 1u);
    /* the histogram */
    uint64_t c[N48_DEP_REASONS]; for (uint32_t i = 0; i < N48_DEP_REASONS; i++) c[i] = ~0ull;
    char h[N48_WO_HIST_MAX + 8]; std::memset(h, 'Z', sizeof h);
    n48_wo_hist(c, h);
    expect_u("O4 the histogram at maximal counts fits its buffer and is terminated", std::strlen(h) < N48_WO_HIST_MAX ? 1u : 0u, 1u);
    uint64_t c2[N48_DEP_REASONS] = {}; c2[0] = 12; c2[6] = 569; c2[19] = 3;
    n48_wo_hist(c2, h);
    expect_u("O4 the histogram names reasons by index", std::strcmp(h, "0:12 6:569 19:3") == 0 ? 1u : 0u, 1u);
    uint64_t c3[N48_DEP_REASONS] = {};
    n48_wo_hist(c3, h);
    expect_u("O4 an empty histogram prints '-'", std::strcmp(h, "-") == 0 ? 1u : 0u, 1u);
    int set = n48_wo_set(0u, nullptr) + n48_wo_set(4u, nullptr);
    uint32_t m = N48_WO_M_OFF;
    expect_u("O4 the setter: 1/2/3 only", set == 0 && n48_wo_set(3u, &m) && m == 3u && n48_wo_set(1u, &m) && m == 1u ? 1u : 0u, 1u);
    expect_u("O4 the world mode of M: 1 -> ON, 3 -> SHADOW, 2/0/7 -> OFF", n48_wo_world_mode(1u) == N48_DEP_WO_ON &&
             n48_wo_world_mode(3u) == N48_DEP_WO_SHADOW && n48_wo_world_mode(2u) == N48_DEP_WO_OFF && n48_wo_world_mode(0u) == 0u &&
             n48_wo_world_mode(7u) == 0u ? 1u : 0u, 1u);
}

/* ---- O5: the safety argument, in the code ---- */
static void o5(const char *depPath, const char *cpbPath, const std::string &ahh)
{
    const std::string dep = slurp(depPath), cpb = slurp(cpbPath);
    expect_u("O5 sources read", !dep.empty() && !cpb.empty() ? 1u : 0u, 1u);
    const std::string hz = body_of(dep, "static inline uint32_t n48_cp_eval_hz(const n48_cp_consumer *c,");
    const std::string hzd4 = body_of(dep, "static inline uint32_t n48_cp_eval_hz_d4(const n48_cp_consumer_d4 *c,");
    const std::string fill = body_of(dep, "static inline uint32_t n48_cp_eval_fill_hz(const n48_cp_consumer *c,");
    const std::string ev = body_of(dep, "static inline uint32_t n48_cp_eval(const n48_cp_consumer *c,");
    expect_u("O5 n48_cp_eval_hz: `wt` only null-checked (no n48_cp_in_witness, no wt->)", !hz.empty() && count(hz, "n48_cp_in_witness") == 0u &&
             count(hz, "wt->") == 0u && count(hz, "!wt") == 1u ? 1u : 0u, 1u);
    expect_u("O5 n48_cp_eval_hz_d4: likewise", !hzd4.empty() && count(hzd4, "n48_cp_in_witness") == 0u && count(hzd4, "wt->") == 0u &&
             count(hzd4, "!wt") == 1u ? 1u : 0u, 1u);
    expect_u("O5 n48_cp_eval_fill_hz: takes no witness at all", !fill.empty() && count(fill, "wt") == 0u ? 1u : 0u, 1u);
    expect_u("O5 n48_cp_d4_judge passes `wt` only to n48_cp_eval_hz_d4", count(body_of(cpb, "static inline uint32_t n48_cp_d4_judge("), "wt") == 2u ? 1u : 0u, 1u);
    expect_u("O5 n48_cp_in_witness has ONE caller: the 30-OFF n48_cp_eval", count(dep, "n48_cp_in_witness(") == 2u &&
             count(ev, "n48_cp_in_witness(wt, c->ptr[i])") == 1u ? 1u : 0u, 1u);
    expect_u("O5 no kext code reads the arm-scoped witness's rows (only its cap `.rows` is written)", count(ahh, "gXdDepArm.row[") +
             count(ahh, "gXdDepArm->row"), 0u);
    expect_u("O5 R3 with 30 ON asks the hazard FILTER (n48_hz_hit), which n48_hz_add sets BEFORE its row cap", count(hz,
             "if (n48_hz_hit(&r5->hz, c->ptr_page[i])) dst++;") == 1u ? 1u : 0u, 1u);
    n48_hazard hzs; std::memset(&hzs, 0, sizeof hzs);
    for (uint64_t p = 1; p <= 4000u; p++) n48_hz_add(&hzs, p << 12, p << 12, 1u);
    uint32_t allHit = 1u;
    for (uint64_t p = 1; p <= 4000u; p++) if (!n48_hz_probe(&hzs, p << 12)) allHit = 0u;
    expect_u("O5 4000 pages added past the hazard table's rows: every one still HITS (the filter cannot overflow)", allHit == 1u &&
             hzs.over > 0u ? 1u : 0u, 1u);
    expect_u("O5 R1 (30 ON): a tiled input without a ledger proof refuses", count(hz,
             "for (uint32_t i = 0; i < c->n; i++) if (c->mode[i] && c->proven[i] != 1u) bad++;\n      if (bad) { if (unproven) *unproven = bad; return N48_CP_R1_TILED; }") == 1u ? 1u : 0u, 1u);
    n48_r5_frame x; std::memset(&x, 0, sizeof x);
    x.ib_ok = 1u; x.walk_ok = 1u; x.targets_ok = 1u; x.memw_ok = 1u; x.tgts_resolved = 1u; x.memw_resolved = 1u; x.ntgt = 1u;
    x.has_draw = 1u;
    const uint32_t rd = n48_r5_bucket(&x);
    x.targets_ok = 0u; const uint32_t tr = n48_r5_bucket(&x); x.targets_ok = 1u;
    x.tgts_resolved = 0u; const uint32_t un = n48_r5_bucket(&x);
    expect_u("O5 R5′: a complete, resolved record is READABLE; a truncated target list or an unresolved target is BLIND",
             rd == N48_R5_READABLE && tr == N48_R5_BLIND && un == N48_R5_BLIND ? 1u : 0u, 1u);
    expect_u("O5 R5′'s target list truncates at N48_CP_TGT_MAX (8, the witness's own hold) and says so", count(dep,
             "if (x->ntgt < N48_CP_TGT_MAX) { x->tgt[x->ntgt].va = items[i].va; x->tgt[x->ntgt].page = 0ull; x->ntgt++; }\n"
             "        else x->targets_ok = 0u;") == 1u ? 1u : 0u, 1u);
    // build 0.0.547 (switch 104): the gather hands n48_r5w_blind, which IS n48_r5_unknown OFF and SHADOW (R5 below proves it).
    expect_u("O5 the gather: blind is R5′'s (only with 28 and 30), through switch 104's n48_r5w_blind", count(ahh,
             "s.r5_blind = (gR5On && gXpOn) ? n48_r5w_blind(&gR5Ring, __atomic_load_n(&gR5wMode, __ATOMIC_RELAXED), gR5wK, gXdC.judged + 1u) : 0ull;"), 1u);
}

/* ---- O6: the kext glue ---- */
static void o6(const std::string &a)
{
    expect_u("O6 source read", a.empty() ? 0u : 1u, 1u);
    expect_u("O6 OFF at boot", count(a, "static volatile uint32_t gWo99Mode { N48_WO_M_OFF };"), 1u);
    expect_u("O6 the verb is gWo99Mode's only writer", count(a, "__atomic_store_n(&gWo99Mode,"), 1u);
    expect_u("O6 the verb asks the mid-arm guard", count(a, "n48_cm_cont_switch_refused(99u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)"), 1u);
    expect_u("O6 the guard refuses 99 under a standing continuous arm", n48_cm_cont_switch_refused(99u, 0u, 1u, (uint32_t)N48_CM_SHOT_ARMED), 1u);
    expect_u("O6 the gather hands the switch's world mode and THIS frame's row-freedom", (count(a,
             "s.wo_mode = n48_wo_world_mode(__atomic_load_n(&gWo99Mode, __ATOMIC_RELAXED));") == 1u &&
             count(a, "s.cp_rows_free = gXp.lastRowsFree;") == 1u) ? 1u : 0u, 1u);
    const std::string cp = body_of(a, "static void gfxsrc_cprov_eval(const GfxcVm &vm)");
    expect_u("O6 row-freedom reset at the top of every frame's evaluation, BEFORE the 28-OFF early return", (cp.find("gXp.lastRowsFree = 0u;") <
             cp.find("if (!gXpOn) return;") && count(cp, "gXp.lastRowsFree = 0u;") == 1u) ? 1u : 0u, 1u);
    expect_u("O6 row-freedom set only in the fill / D4' / hz branches, and from the SAME reading of 30 that picked the rule", (
             count(cp, "rowsFree = 1u;") == 2u && count(cp, "const uint32_t hz = gR5On ? 1u : 0u;") == 1u &&
             count(cp, "clause = hz ? n48_cp_eval_hz(") == 1u && count(cp, "rowsFree = hz;") == 1u &&
             count(cp, "gXp.lastRowsFree = rowsFree;") == 1u) ? 1u : 0u, 1u);
    expect_u("O6 the gate's decision is still n48_dep_ok(&dw), and the instruments come after it", (count(a, "c.dep_ok = n48_dep_ok(&dw);") == 1u &&
             a.find("c.dep_ok = n48_dep_ok(&dw);") < a.find("wo99_gate(dw, gXdGateDep.reason);") &&
             count(a, "wo99_gate(dw, gXdGateDep.reason);") == 1u) ? 1u : 0u, 1u);
    expect_u("O6 the STOP block prints the witness (after wc98's)", count(a, "wo99_arm_stop();                  // build 0.0.541 item 6"), 1u);
    expect_u("O6 the first overflow is recorded at the note site, from the exact note that overflowed", count(a,
             "if (gXdDepArm.over > woOver0 && gWoFirstSeq != gXdDepArm.arm_seq)\n                    wo99_first(gXdC.judged + 1u, hVa[q], hPage[q], hRes[q], verdict, pid);"), 1u);
    const std::string g = body_of(a, "static __attribute__((noinline)) void wo99_gate(const n48_dep_world &w, uint32_t reason)");
    expect_u("O6 wo99_gate writes no world and no decision (instrument only)", !g.empty() && count(g, "w.") == 0u && count(g, "dep_ok") == 0u ? 1u : 0u, 1u);
}

/* ---- O7: the lines ---- */
static void o7()
{
    char buf[2048];
    uint32_t ok = 1u;
    n48_wo_st s; std::memset(&s, 0xff, sizeof s);
    const char *how = " - `gfxneuter 99` REFUSED - a continuous arm stands, unchanged";
    for (uint32_t m = 1u; m <= 3u; m++) {
        const int n = std::snprintf(buf, sizeof buf, N48_WO_FMT, N48_WO_ARGS(m, how, &s));
        if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  WO %d\n", n); }
    }
    n48_dep_witness wt; std::memset(&wt, 0xff, sizeof wt); wt.rows = N48_DEP_ROWS_MAX;
    n48_wo_first f; std::memset(&f, 0xff, sizeof f); f.valid = 1u;
    int n = std::snprintf(buf, sizeof buf, N48_WO_STOP_FMT, N48_WO_STOP_ARGS(&wt, ~0ull, &f));
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  STOP %d\n", n); }
    f.valid = 0u;
    n = std::snprintf(buf, sizeof buf, N48_WO_STOP_FMT, N48_WO_STOP_ARGS(&wt, ~0ull, &f));
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  STOP0 %d\n", n); }
    n = std::snprintf(buf, sizeof buf, N48_WO_FIRST_FMT, N48_WO_FIRST_ARGS(&f, ~0ull));
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  FIRST %d\n", n); }
    n48_wo_secline L; std::memset(&L, 0xff, sizeof L);
    uint64_t c[N48_DEP_REASONS]; for (uint32_t i = 0; i < N48_DEP_REASONS; i++) c[i] = ~0ull;
    char h[N48_WO_HIST_MAX]; n48_wo_hist(c, h);
    n = std::snprintf(buf, sizeof buf, N48_WO_SEC_FMT, N48_WO_SEC_ARGS(L, &wt, ~0ull, h));
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  SEC %d\n", n); }
    n48_dep_world w; std::memset(&w, 0xff, sizeof w);
    for (uint32_t ev = N48_WO_E_SKIPPED; ev <= N48_WO_E_WOULD_PASS; ev++) {
        n = std::snprintf(buf, sizeof buf, N48_WO_FRAME_FMT, N48_WO_FRAME_ARGS(~0ull, ev, &w, ~0ull, ~0ull, N48_DEP_CONSUMER_UNPROVEN));
        if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  FRAME %d\n", n); }
    }
    expect_u("O7 every wo99 line fits 491 bytes at maximal fields", ok, 1u);
}

/* =============================================================================================================================
 * build 0.0.547 items 1-2 ( fix (1); gfx_r5win104.h): SWITCH 104, THE R5′ BLIND WINDOW, and the first-BLIND line.
 *   R1 the latch reproduced (OFF): one BLIND note refuses the next 1000 judged frames at N48_DEP_NEUTER_UNREADABLE;
 *   R2 ON clears after exactly K judged frames (K 1, 7, 32, 1024), the count kept;
 *   R3 the window never lets the BLIND frame itself, a frame at/before the note, or a frame after a NEWER BLIND through;
 *   R4 SHADOW never changes a verdict (random sequences: world value and n48_dep_check equal to OFF's);
 *   R5 OFF identity (and every unknown M): n48_r5w_blind == n48_r5_unknown over 200 000 random states; the note's `blind` is the
 *      bucket's count exactly;
 *   R6 arm/epoch reset: a new scope zeroes blind, last_blind, the clause census and the line budget;
 *   R7 the verb's K and M: 1..1024 accepted, 0 -> 32, 1025 / a stray bit / an unknown M / a K with a bare read refused;
 *   R8 the clause classifier mirrors the bucket (NONE exactly when not BLIND) and the census sums to `blind`;
 *   R9 a run11aq-shaped timeline (wo99 `sec N` gate counts, 4 BLIND notes at sec 49-50, cyc80's `BLIND 4`): OFF refuses to the end,
 *      ON K 32 refuses at most 4 x 32 frames;
 *   R10 content-only and the glue (source pins);
 *   R11 every r5win line <= 491 bytes at its widest. */
static n48_dep_world r5_world(uint64_t blind)
{
    n48_dep_world w = clean_world();
    w.consumer_enumerated = 1u; w.r5_mode = 1u; w.r5_blind = blind;
    return w;
}
static n48_r5_frame blind_frame(uint32_t clause)
{
    n48_r5_frame x; std::memset(&x, 0, sizeof x);
    x.ib_ok = 1u; x.walk_ok = 1u; x.targets_ok = 1u; x.memw_ok = 1u; x.tgts_resolved = 1u; x.memw_resolved = 1u; x.ntgt = 1u; x.has_draw = 1u;
    switch (clause) {
    case 0: break;                                    /* READABLE */
    case N48_R5_BC_TGT_UNRESOLVED: x.tgts_resolved = 0u; break;
    case N48_R5_BC_TGT_TRUNC: x.targets_ok = 0u; break;
    default: x.ib_ok = 0u; break;
    }
    return x;
}
/* One judged frame's R5′ verdict under a mode: the gather's value, then the gate. */
static uint32_t r5_verdict(const n48_r5_ring *r, uint32_t mode, uint32_t k, uint64_t cur)
{
    const n48_dep_world w = r5_world(n48_r5w_blind(r, mode, k, cur));
    uint64_t d = 0ull;
    return n48_dep_check(&w, &d);
}
static void r_tests(const std::string &ahh, const char *depPath)
{
    /* R1 */
    {
        static n48_r5_ring r; std::memset(&r, 0, sizeof r); n48_r5_scope(&r, 7u);
        const n48_r5_frame b = blind_frame(N48_R5_BC_TGT_UNRESOLVED), ok = blind_frame(0u);
        n48_r5_note(&r, &b, 10u);
        uint32_t refused = 0u;
        for (uint64_t f = 11u; f <= 1010u; f++) {
            if (r5_verdict(&r, N48_R5W_M_OFF, 32u, f) == N48_DEP_NEUTER_UNREADABLE) refused++;
            n48_r5_note(&r, &ok, f);                     /* readable held-back frames never clear it */
        }
        expect_u("R1 the latch (OFF): one BLIND note at frame 10 refuses all 1000 later frames (11..1010) at neuter-unreadable", refused, 1000u);
    }
    /* R2 */
    {
        static const uint32_t ks[] = { 1u, 7u, 32u, 1024u };
        uint32_t good = 1u;
        for (uint32_t ki = 0; ki < 4u; ki++) {
            static n48_r5_ring r; std::memset(&r, 0, sizeof r); n48_r5_scope(&r, 3u);
            const n48_r5_frame b = blind_frame(N48_R5_BC_TGT_TRUNC);
            n48_r5_note(&r, &b, 100u);
            uint32_t refused = 0u, firstClean = 0u;
            for (uint64_t f = 101u; f <= 100u + ks[ki] + 50u; f++) {
                const uint32_t v = r5_verdict(&r, N48_R5W_M_ON, ks[ki], f);
                if (v == N48_DEP_NEUTER_UNREADABLE) { refused++; if (firstClean) good = 0u; }
                else if (v == N48_DEP_OK && !firstClean) firstClean = (uint32_t)f;
            }
            if (refused != ks[ki] || firstClean != 101u + ks[ki] || r.blind != 1u) { good = 0u; std::printf("  K %u refused %u clean@%u\n", ks[ki], refused, firstClean); }
        }
        expect_u("R2 ON: exactly K later frames refuse (K 1, 7, 32, 1024), frame last+K+1 is clean, the count is kept", good, 1u);
    }
    /* R3 */
    {
        static n48_r5_ring r; std::memset(&r, 0, sizeof r); n48_r5_scope(&r, 5u);
        const n48_r5_frame b = blind_frame(N48_R5_BC_TGT_UNRESOLVED);   /* 0.0.548: a WINDOWED clause (ib-unread now latches: R12) */
        n48_r5_note(&r, &b, 200u);
        const uint32_t self = r5_verdict(&r, N48_R5W_M_ON, 32u, 200u), before = r5_verdict(&r, N48_R5W_M_ON, 32u, 150u);
        const uint32_t after = r5_verdict(&r, N48_R5W_M_ON, 32u, 240u);
        n48_r5_note(&r, &b, 500u);                         /* a NEWER BLIND frame re-opens the window from its own number */
        const uint32_t again = r5_verdict(&r, N48_R5W_M_ON, 32u, 510u), end = r5_verdict(&r, N48_R5W_M_ON, 32u, 533u);
        r.last_blind = 0ull;                              /* a count with no frame number: fail closed */
        const uint32_t nofr = r5_verdict(&r, N48_R5W_M_ON, 32u, 100000u);
        expect_u("R3 ON: the BLIND frame itself (cur == last) and a frame before it refuse; clean past K; a NEWER BLIND refuses again "
                 "for its own K; a count without a frame number always refuses", self == N48_DEP_NEUTER_UNREADABLE &&
                 before == N48_DEP_NEUTER_UNREADABLE && after == N48_DEP_OK && again == N48_DEP_NEUTER_UNREADABLE && end == N48_DEP_OK &&
                 nofr == N48_DEP_NEUTER_UNREADABLE ? 1u : 0u, 1u);
        /* The frame whose OWN record is BLIND: noted only after its own gate (the note site follows commit_try), and every later
         * gate reads the window from that note; the order is pinned in R10. A BLIND record is BLIND in every mode (the bucket). */
        n48_r5_frame own = blind_frame(N48_R5_BC_TGT_UNRESOLVED);
        expect_u("R3 a record whose own destination set is unreadable is BLIND whatever switch 104 says (the bucket reads no mode)",
                 n48_r5_bucket(&own) == N48_R5_BLIND ? 1u : 0u, 1u);
    }
    /* R4 + R5 */
    {
        static n48_r5_ring r;
        uint32_t shadowSame = 1u, offSame = 1u, countSame = 1u;
        for (uint32_t it = 0; it < 200000u; it++) {
            if ((it & 1023u) == 0u) { std::memset(&r, 0, sizeof r); n48_r5_scope(&r, 1u + (it >> 10)); }
            uint64_t ref = r.blind;
            const uint32_t pick = rnd() % 16u;
            const n48_r5_frame x = blind_frame(pick < 10u ? 0u : pick < 12u ? N48_R5_BC_TGT_TRUNC : pick < 14u ? N48_R5_BC_TGT_UNRESOLVED : N48_R5_BC_IB_UNREAD);
            n48_r5_frame y = x; if ((rnd() & 15u) == 0u) y.out_of_scope = 1u;
            const uint64_t fr = 1u + (rnd() % 4000u);
            if (n48_r5_bucket(&y) == N48_R5_BLIND) ref++;
            n48_r5_note(&r, &y, fr);
            if (r.blind != ref) countSame = 0u;
            const uint64_t cur = 1u + (rnd() % 5000u);
            const uint32_t k = rnd() % 1100u;
            const uint64_t u = n48_r5_unknown(&r);
            if (n48_r5w_blind(&r, N48_R5W_M_OFF, k, cur) != u) offSame = 0u;
            for (uint32_t m = 0u; m < 16u; m++) if (m != N48_R5W_M_ON && n48_r5w_blind(&r, m, k, cur) != u) offSame = 0u;
            if (n48_r5w_blind(&r, N48_R5W_M_SHADOW, k, cur) != u ||
                r5_verdict(&r, N48_R5W_M_SHADOW, k, cur) != r5_verdict(&r, N48_R5W_M_OFF, k, cur)) shadowSame = 0u;
        }
        expect_u("R4 SHADOW never changes a verdict: over 200 000 random states its world value and n48_dep_check equal OFF's", shadowSame, 1u);
        expect_u("R5 OFF identity: n48_r5w_blind == n48_r5_unknown for OFF and every M but ON (0, 2..15)", offSame, 1u);
        expect_u("R5 the note's `blind` is the bucket's BLIND count exactly (0.0.546's count; the window's fields are beside it)", countSame, 1u);
        expect_u("R5 a NULL ring answers 1 in every mode (fail closed, as n48_r5_unknown)", n48_r5w_blind(nullptr, N48_R5W_M_ON, 32u, 5u) == 1ull &&
                 n48_r5w_blind(nullptr, N48_R5W_M_OFF, 32u, 5u) == 1ull ? 1u : 0u, 1u);
    }
    /* R6 */
    {
        static n48_r5_ring r; std::memset(&r, 0, sizeof r); n48_r5_scope(&r, 9u);
        const n48_r5_frame b = blind_frame(N48_R5_BC_TGT_TRUNC);
        n48_r5_note(&r, &b, 42u); n48_r5_note(&r, &b, 43u); r.first_lines = 5u; r.latched_blind = 3u;   /* 0.0.548: reset by scope only */
        const uint32_t pre = (r.blind == 2u && r.last_blind == 43u && r.blind_clause[N48_R5_BC_TGT_TRUNC] == 2u) ? 1u : 0u;
        n48_r5_scope(&r, 9u);                               /* same scope: nothing moves */
        const uint32_t same = (r.blind == 2u && r.last_blind == 43u && r.first_lines == 5u && r.latched_blind == 3u) ? 1u : 0u;
        n48_r5_scope(&r, 9u | (2u << 8));                   /* a new descriptor epoch */
        uint64_t cs = 0ull; for (uint32_t c = 0; c < (uint32_t)N48_R5_BC_N; c++) cs += r.blind_clause[c];
        const uint32_t reset = (r.blind == 0u && r.last_blind == 0u && cs == 0u && r.first_lines == 0u && r.latched_blind == 0u) ? 1u : 0u;
        n48_r5_note_counted(&r, 9u | (2u << 8), &b, 77u, 0u, 0u, 0ull);
        expect_u("R6 a new scope (arm level or epoch) zeroes blind, last_blind, the clause census and the line budget; the same scope keeps "
                 "them; the next BLIND sets last_blind", pre && same && reset && r.last_blind == 77u && r.blind == 1u ? 1u : 0u, 1u);
    }
    /* R7 */
    {
        const n48_r5w_arg on = n48_r5w_decode(104u | 1u << 8), off = n48_r5w_decode(104u | 2u << 8), sh = n48_r5w_decode(104u | 3u << 8);
        const n48_r5w_arg k1 = n48_r5w_decode(104u | 1u << 8 | 1u << 12), kmax = n48_r5w_decode(104u | 1u << 8 | 1024ull << 12);
        const n48_r5w_arg kbig = n48_r5w_decode(104u | 1u << 8 | 1025ull << 12), stray = n48_r5w_decode(104u | 1u << 8 | 1ull << 24);
        const n48_r5w_arg m4 = n48_r5w_decode(104u | 4u << 8), rd = n48_r5w_decode(104u), rdk = n48_r5w_decode(104u | 5u << 12);
        const n48_r5w_arg other = n48_r5w_decode(103u | 1u << 8);
        expect_u("R7 the verb: 360 ON / 616 OFF / 872 SHADOW with K 32; K 1 and 1024 accepted; 1025, bit 24, M 4, a K with a bare read and "
                 "another switch refused; a bare read accepted", on.ok && on.m == 1u && on.k == 32u && off.ok && off.m == 2u && sh.ok &&
                 sh.m == 3u && k1.ok && k1.k == 1u && kmax.ok && kmax.k == 1024u && !kbig.ok && !stray.ok && !m4.ok && rd.ok && rd.m == 0u &&
                 !rdk.ok && !other.ok && (104u | 1u << 8) == 360u && (104u | 2u << 8) == 616u && (104u | 3u << 8) == 872u ? 1u : 0u, 1u);
        expect_u("R7 the window's K is clamped (0 -> 32, above 1024 -> 1024) wherever it is read",
                 n48_r5w_in_window(100u, 132u, 0u) == 1u && n48_r5w_in_window(100u, 133u, 0u) == 0u &&
                 n48_r5w_in_window(100u, 1124u, 5000u) == 1u && n48_r5w_in_window(100u, 1125u, 5000u) == 0u ? 1u : 0u, 1u);
    }
    /* R8 */
    {
        uint32_t eq = 1u;
        static n48_r5_ring r; std::memset(&r, 0, sizeof r); n48_r5_scope(&r, 11u);
        for (uint32_t it = 0; it < 100000u; it++) {
            n48_r5_frame x; std::memset(&x, 0, sizeof x);
            const uint32_t v = rnd();
            x.ib_over = (v & 7u) == 0u; x.out_of_scope = (v >> 3 & 7u) == 0u; x.ib_ok = (v >> 6 & 7u) != 0u; x.walk_ok = (v >> 9 & 7u) != 0u;
            x.targets_ok = (v >> 12 & 7u) != 0u; x.memw_ok = (v >> 15 & 7u) != 0u; x.tgts_resolved = (v >> 18 & 7u) != 0u;
            x.memw_resolved = (v >> 21 & 7u) != 0u; x.scan_over = (v >> 24 & 7u) == 0u; x.has_dispatch = (v >> 27 & 1u);
            x.dispatch_ok = (v >> 28 & 1u); x.has_draw = (v >> 29 & 1u); x.ntgt = (v >> 30 & 1u); x.nmemw = (v >> 31 & 1u);
            if ((n48_r5_blind_clause(&x) != N48_R5_BC_NONE) != (n48_r5_bucket(&x) == N48_R5_BLIND)) eq = 0u;
            n48_r5_note(&r, &x, it + 1u);
        }
        uint64_t cs = 0ull; for (uint32_t c = 0; c < (uint32_t)N48_R5_BC_N; c++) cs += r.blind_clause[c];
        expect_u("R8 the clause classifier is NONE exactly when the bucket is not BLIND (100 000 random records); the census sums to "
                 "`blind` with nothing under `none`", eq && cs == r.blind && r.blind_clause[N48_R5_BC_NONE] == 0u && r.blind > 0u ? 1u : 0u, 1u);
    }
    /* R9: run11aq's per-second gate counts (driverlog-stream.txt `wo99 sec N: ... gates G`), 4 BLIND notes at sec 49-50 */
    {
        static const uint32_t gates[] = { 13, 47, 38, 57, 58, 41, 56, 59, 58, 58, 77, 74, 54, 54, 75, 71, 45, 47, 95, 75, 68, 63, 61, 49, 53, 69, 55,
            53, 46, 63, 72, 91, 42, 51, 86, 90, 131, 121, 144, 101, 119, 111, 93, 120, 49, 64, 59, 122, 127, 114, 136, 129, 120, 82, 102, 52,
            48, 49, 49, 56, 49, 48, 49, 48, 49, 48, 49, 48, 49, 49, 48, 49, 48, 49, 49, 39, 112, 122, 85, 49, 49, 48, 49, 48, 49, 49, 48, 48,
            48, 49, 49, 49, 48, 48, 48, 49, 49, 49, 48, 48, 49, 49, 49, 49, 48, 49, 48, 48, 49, 48, 49, 48, 49, 48, 48, 49, 48, 49, 48 };
        const uint32_t nsec = (uint32_t)(sizeof gates / sizeof gates[0]);
        static n48_r5_ring r;
        uint64_t refusedOff = 0ull, refusedOn = 0ull, afterOff = 0ull;
        for (uint32_t mode = N48_R5W_M_ON; mode <= N48_R5W_M_OFF; mode++) {
            std::memset(&r, 0, sizeof r); n48_r5_scope(&r, 1u);
            uint64_t fr = 0ull, firstBlind = 0ull, ref = 0ull, after = 0ull;
            const n48_r5_frame b = blind_frame(N48_R5_BC_TGT_UNRESOLVED);
            for (uint32_t sec = 0; sec < nsec; sec++)
                for (uint32_t g = 0; g < gates[sec]; g++) {
                    fr++;
                    if (r5_verdict(&r, mode, 32u, fr) == N48_DEP_NEUTER_UNREADABLE) { ref++; if (firstBlind && fr > firstBlind) after++; }
                    if ((sec == 48u || sec == 49u) && (g == 20u || g == 60u)) { n48_r5_note(&r, &b, fr); if (!firstBlind) firstBlind = fr; }
                }
            if (mode == N48_R5W_M_OFF) { refusedOff = ref; afterOff = after; } else refusedOn = ref;
            if (mode == N48_R5W_M_OFF) expect_u("R9 run11aq-shaped: OFF refuses EVERY frame after the first BLIND note to the end of the arm",
                                                after == fr - firstBlind ? 1u : 0u, 1u);
        }
        std::printf("  R9 frames refused: OFF %llu (after the first BLIND %llu), ON K 32 %llu\n", (unsigned long long)refusedOff,
                    (unsigned long long)afterOff, (unsigned long long)refusedOn);
        expect_u("R9 run11aq-shaped: ON (K 32) refuses at most 4 x 32 frames for the 4 BLIND notes", refusedOn <= 128u && refusedOn >= 32u &&
                 refusedOff > 4000u ? 1u : 0u, 1u);
    }
    /* R10: content-only and the glue */
    {
        const std::string dep = slurp(depPath);
        const std::string wb = body_of(dep, "static inline void n48_r5_note(n48_r5_ring *r, const n48_r5_frame *x, uint64_t namer)");
        expect_u("R10 the note sets last_blind in the BLIND branch, right after the count (before the branch returns)",
                 !wb.empty() && wb.find("r->blind++;") < wb.find("r->last_blind = namer;") && wb.find("r->last_blind = namer;") <
                 wb.find("if (x->memw_ok != 1u) r->over_cap++;") && count(dep, "r->last_blind = namer;") == 1u ? 1u : 0u, 1u);
        const std::string g = body_of(ahh, "static __attribute__((noinline)) void r5w_gate(const n48_dep_world &w, uint32_t reason)");
        expect_u("R10 r5w_gate writes no world and no decision (the other rule's copy is gR5wAlt; counts only)", !g.empty() &&
                 count(g, "w.r5_blind =") == 0u && count(g, "dep_ok") == 0u && count(g, "gXdGateDep") == 0u && count(g, "&gR5wAlt") == 1u ? 1u : 0u, 1u);
        expect_u("R10 the gate: the decision is n48_dep_ok(&dw), r5w_gate after it and after wo99_gate", count(ahh,
                 "    wo99_gate(dw, gXdGateDep.reason);   // build 0.0.541 item 6 (switch 99): both answers counted, the per-second line; decides nothing\n"
                 "    r5w_gate(dw, gXdGateDep.reason);") == 1u && ahh.find("c.dep_ok = n48_dep_ok(&dw);") < ahh.find("r5w_gate(dw, gXdGateDep.reason);") &&
                 count(ahh, " r5w_gate(") == 2u ? 1u : 0u, 1u);
        const size_t cmt = ahh.find("? gfxsrc_commit_try(vm, info, ib0Va, f.ib[0].got, f.nib, arm, verdict, stampNow, tgtVa) : 0u;");
        const size_t note = ahh.find("                n48_r5_note_counted(&gR5Ring, gXpScopeSeq, &r5f, gXdC.judged + 1u,");
        const size_t fb = ahh.find("                r5w_first_blind(&r5f, &f, n, pid, pname, shapeOk, r5ShortRead);");
        const size_t jp = ahh.find("    gXdC.judged++;\n");
        expect_u("R10 ORDER: the frame's own gate (commit_try) BEFORE its note, the first-BLIND line right after the note, the judged count "
                 "after both (so a window reads only EARLIER notes by frame number)", cmt != std::string::npos && note != std::string::npos &&
                 fb != std::string::npos && jp != std::string::npos && cmt < note && note < fb && fb < jp && count(ahh, "r5w_first_blind(") == 3u ? 1u : 0u, 1u);
        expect_u("R10 OFF at boot, K 32 at boot, the verb the only writer, mid-arm guarded", count(ahh, "static volatile uint32_t gR5wMode { N48_R5W_M_OFF };") == 1u &&
                 count(ahh, "static volatile uint32_t gR5wK { N48_R5W_K_DEFAULT };") == 1u && count(ahh, "__atomic_store_n(&gR5wMode, ") == 1u &&
                 count(ahh, "__atomic_store_n(&gR5wK, ") == 1u && count(ahh, "gR5wMode = ") == 0u && count(ahh, "gR5wK = ") == 0u &&
                 count(ahh, "const bool contRefused104 = n48_cm_cont_switch_refused(104u, a104.m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;") == 1u &&
                 n48_cm_cont_switch_guarded(104u) == 1u && n48_cm_cont_switch_refused(104u, 1u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 0u ? 1u : 0u, 1u);
        /* CONTENT-ONLY: the window's two functions read the ring's blind/last_blind and nothing that protects hardware. */
        std::string whp = depPath ? std::string(depPath) : std::string();
        if (whp.size() >= 9u) whp = whp.substr(0, whp.size() - 9u) + "gfx_r5win104.h";   /* beside gfx_dep.h (argv[2]) */
        const std::string wh = slurp(whp.c_str());
        const std::string f1 = body_of(wh, "static inline uint64_t n48_r5w_blind(const n48_r5_ring *r, uint32_t mode, uint32_t k, uint64_t cur)");
        const std::string f2 = body_of(wh, "static inline uint32_t n48_r5w_in_window(uint64_t last, uint64_t cur, uint32_t k)");
        uint32_t clean = (!f1.empty() && !f2.empty()) ? 1u : 0u;
        static const char *const glob[] = { "gXd", "gKs", "gLed", "gFr", "gR5", "gGn", "gGs", "gLatch", "fault", "keystone", "flight", "hz" };
        for (const char *t : glob) if (count(f1, t) || count(f2, t)) clean = 0u;
        clean = clean && count(f1, "r->") == 2u && count(f1, "r->last_blind") == 1u && count(f1, "r->latched_blind") == 1u &&
                count(f1, "n48_r5_unknown(r)") == 1u && count(f2, "->") == 0u;
        expect_u("R10 CONTENT-ONLY: n48_r5w_blind reads only n48_r5_unknown(r) (the count), r->latched_blind (0.0.548) and r->last_blind; "
                 "n48_r5w_in_window reads no state at all (no global, ledger, keystone, flight ring, fault latch or hazard set)", clean, 1u);
        /* 0.0.548 item A: the latched check comes BEFORE the window, so no window answer can clear a latched note */
        expect_u("R10 (0.0.548 A) n48_r5w_blind asks the latched count BEFORE the window", f1.find("if (r->latched_blind != 0ull) return b;") <
                 f1.find("n48_r5w_in_window(") && f1.find("if (r->latched_blind != 0ull) return b;") != std::string::npos ? 1u : 0u, 1u);
        const std::string nb = body_of(dep, "static inline void n48_r5_note(n48_r5_ring *r, const n48_r5_frame *x, uint64_t namer)");
        const std::string sb = body_of(dep, "static inline void n48_r5_scope(n48_r5_ring *r, uint32_t seq)");
        expect_u("R10 (0.0.548 A) latched_blind: counted only in the note's BLIND branch, reset only by n48_r5_scope, written nowhere else "
                 "(the window never clears it)", count(nb, "r->latched_blind++;") == 1u && count(sb, "r->latched_blind = 0ull;") == 1u &&
                 count(dep, "latched_blind =") == 1u && count(dep, "latched_blind++") == 1u && count(wh, "latched_blind =") == 0u &&
                 count(wh, "latched_blind--") == 0u && count(ahh, "latched_blind =") == 0u && count(ahh, "latched_blind--") == 0u ? 1u : 0u, 1u);
        /* 0.0.548 item D: the switch-77 drain path prints the first-BLIND line too */
        const std::string dr = body_of(ahh, "static __attribute__((noinline)) void gfxsrc_rn_drain(void)");
        expect_u("R10 (0.0.548 D) gfxsrc_rn_drain: after n48_rn_drain, each drained BLIND record gets r5w_first_blind (f null, pid -1)",
                 dr.find("(void)n48_rn_drain(") < dr.find("r5w_first_blind(&gRnStore.r[gRn.b_slot[i]].f, nullptr, gRn.b_nib[i], -1, \"rn77-drain\", 0u, 0u);") &&
                 count(dr, "r5w_first_blind(") == 1u && count(dr, "gRn.b_slot[i] < N48_RN_STORE") == 1u ? 1u : 0u, 1u);
    }
    /* R11 */
    {
        char buf[2048]; uint32_t ok = 1u;
        static n48_r5_ring r; std::memset(&r, 0xff, sizeof r);
        n48_r5w_st st; std::memset(&st, 0xff, sizeof st);
        for (uint32_t m = 1u; m <= 3u; m++) {
            const int n = std::snprintf(buf, sizeof buf, N48_R5W_FMT, N48_R5W_ARGS(m, N48_R5W_K_MAX, " - `gfxneuter 104` REFUSED (unknown M, K out of 1..1024, or a stray bit), unchanged", &r, &st));
            if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  R5W %d\n", n); }
        }
        int n = std::snprintf(buf, sizeof buf, N48_R5W_CLAUSE_FMT, N48_R5W_CLAUSE_ARGS(&r));
        if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  CLAUSE %d\n", n); }
        n48_r5w_first o; std::memset(&o, 0xff, sizeof o);
        o.pid = -2147483647 - 1; o.pname = "SecurityAgentXXXXXXXXXXXXXX"; o.clause = N48_R5_BC_MEMW_UNRESOLVED;
        n = std::snprintf(buf, sizeof buf, N48_R5W_FIRST_FMT, N48_R5W_FIRST_ARGS(4294967295u, &o, ~0ull));
        if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  FIRST %d\n", n); }
        std::printf("  R11 first-BLIND line at its widest: %d bytes\n", n);
        r.blind = 5u; r.latched_blind = ~0ull;   /* the split's windowed half never underflows */
        n = std::snprintf(buf, sizeof buf, N48_R5W_SPLIT_FMT, N48_R5W_SPLIT_ARGS(&r));
        if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY || std::strstr(buf, "windowed 0 ") == nullptr) { ok = 0u; std::printf("  SPLIT %d\n", n); }
        std::memset(&r, 0xff, sizeof r); r.latched_blind = 0u;
        n = std::snprintf(buf, sizeof buf, N48_R5W_SPLIT_FMT, N48_R5W_SPLIT_ARGS(&r));
        if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  SPLIT2 %d\n", n); }
        expect_u("R11 every r5win line (the report, the clause census, the first-BLIND line) fits 491 bytes at its widest", ok, 1u);
    }
}

/* =============================================================================================================================
 * build 0.0.548 item A (the 0.0.547 review: 104 ON NO-GO as built): THE LATCHED CLAUSES.
 *   R12 every clause that can hide a descriptor / address write (ib-over, ib-unread, walk, memw-truncated, memw-unresolved, dispatch,
 *       inherited) LATCHES: ON keeps refusing past K, to the end of the scope; the windowed ones (targets-truncated, target-unresolved,
 *       scan-over) clear after K; a windowed note followed by a latched one keeps refusing; a new scope clears the latch; OFF and
 *       SHADOW are unchanged (the latch never touches `blind`); the census splits into latched + windowed. */
static n48_r5_frame clause_frame(uint32_t c)
{
    n48_r5_frame x = blind_frame(0u);
    switch (c) {
    case N48_R5_BC_IB_OVER: x.ib_over = 1u; break;
    case N48_R5_BC_IB_UNREAD: x.ib_ok = 0u; break;
    case N48_R5_BC_WALK: x.walk_ok = 0u; break;
    case N48_R5_BC_TGT_TRUNC: x.targets_ok = 0u; break;
    case N48_R5_BC_MEMW_TRUNC: x.memw_ok = 0u; break;
    case N48_R5_BC_TGT_UNRESOLVED: x.tgts_resolved = 0u; break;
    case N48_R5_BC_MEMW_UNRESOLVED: x.memw_resolved = 0u; break;
    case N48_R5_BC_SCAN_OVER: x.scan_over = 1u; break;
    case N48_R5_BC_DISPATCH: x.has_dispatch = 1u; x.dispatch_ok = 0u; break;
    case N48_R5_BC_INHERITED: x.ntgt = 0u; x.nmemw = 0u; x.has_draw = 1u; break;
    default: break;
    }
    return x;
}
static void r12_latched()
{
    uint32_t good = 1u, nl = 0u, nw = 0u;
    for (uint32_t c = 1u; c < (uint32_t)N48_R5_BC_N; c++) {
        const n48_r5_frame x = clause_frame(c);
        if (n48_r5_blind_clause(&x) != c) { good = 0u; std::printf("  R12 clause %u built as %u\n", c, n48_r5_blind_clause(&x)); continue; }
        static n48_r5_ring r; std::memset(&r, 0, sizeof r); n48_r5_scope(&r, 4u);
        n48_r5_note(&r, &x, 100u);
        uint32_t refusedOn = 0u, refusedOff = 0u;
        for (uint64_t f = 101u; f <= 100u + 32u + 500u; f++) {
            refusedOn += r5_verdict(&r, N48_R5W_M_ON, 32u, f) == N48_DEP_NEUTER_UNREADABLE;
            refusedOff += r5_verdict(&r, N48_R5W_M_OFF, 32u, f) == N48_DEP_NEUTER_UNREADABLE;
        }
        const uint32_t latched = n48_r5_bc_latched(c);
        (latched ? nl : nw)++;
        if (refusedOff != 532u) good = 0u;
        if (latched && (refusedOn != 532u || r.latched_blind != 1u)) { good = 0u; std::printf("  R12 latched clause %s: ON refused %u\n", n48_r5_blind_clause_name(c), refusedOn); }
        if (!latched && (refusedOn != 32u || r.latched_blind != 0u)) { good = 0u; std::printf("  R12 windowed clause %s: ON refused %u\n", n48_r5_blind_clause_name(c), refusedOn); }
    }
    expect_u("R12 the latched set is exactly ib-over, ib-unread, walk, memw-truncated, memw-unresolved, dispatch, inherited (7); the "
             "windowed set targets-truncated, target-unresolved, scan-over (3)", nl == 7u && nw == 3u &&
             n48_r5_bc_latched(N48_R5_BC_DISPATCH) && n48_r5_bc_latched(N48_R5_BC_IB_OVER) && n48_r5_bc_latched(N48_R5_BC_IB_UNREAD) &&
             n48_r5_bc_latched(N48_R5_BC_WALK) && n48_r5_bc_latched(N48_R5_BC_INHERITED) && n48_r5_bc_latched(N48_R5_BC_MEMW_TRUNC) &&
             !n48_r5_bc_latched(N48_R5_BC_TGT_TRUNC) && !n48_r5_bc_latched(N48_R5_BC_TGT_UNRESOLVED) &&
             !n48_r5_bc_latched(N48_R5_BC_SCAN_OVER) && !n48_r5_bc_latched(N48_R5_BC_NONE) ? 1u : 0u, 1u);
    expect_u("R12 ON: a latched-clause BLIND (a DISPATCH one among them) refuses every frame past K to the end of the scope; a "
             "windowed one (target-count-only among them) clears after exactly K; OFF refuses every frame for both", good, 1u);
    {
        static n48_r5_ring r; std::memset(&r, 0, sizeof r); n48_r5_scope(&r, 6u);
        const n48_r5_frame w = clause_frame(N48_R5_BC_TGT_TRUNC), d = clause_frame(N48_R5_BC_DISPATCH);
        n48_r5_note(&r, &w, 100u);
        const uint32_t clearedFirst = r5_verdict(&r, N48_R5W_M_ON, 32u, 140u) == N48_DEP_OK;
        n48_r5_note(&r, &d, 150u);
        const uint32_t stuck = r5_verdict(&r, N48_R5W_M_ON, 32u, 5000u) == N48_DEP_NEUTER_UNREADABLE;
        n48_r5_note(&r, &w, 6000u);                         /* a later windowed note does not release the latch */
        const uint32_t still = r5_verdict(&r, N48_R5W_M_ON, 32u, 9000u) == N48_DEP_NEUTER_UNREADABLE;
        n48_r5_scope(&r, 6u | (3u << 8));                   /* a new scope (arm level / epoch) */
        const uint32_t fresh = r.latched_blind == 0u && r5_verdict(&r, N48_R5W_M_ON, 32u, 9001u) == N48_DEP_OK;
        expect_u("R12 a windowed note clears after K; a later DISPATCH note latches for the rest of the scope, whatever is noted after; "
                 "only a new scope clears it", clearedFirst && stuck && still && fresh ? 1u : 0u, 1u);
    }
    {
        static n48_r5_ring r; std::memset(&r, 0, sizeof r); n48_r5_scope(&r, 8u);
        uint64_t lat = 0ull;
        for (uint32_t it = 0; it < 20000u; it++) {
            const uint32_t c = rnd() % (uint32_t)N48_R5_BC_N;
            const n48_r5_frame x = clause_frame(c);
            if (c != N48_R5_BC_NONE && n48_r5_bc_latched(c)) lat++;
            n48_r5_note(&r, &x, it + 1u);
        }
        uint64_t win = 0ull;
        for (uint32_t c = 1u; c < (uint32_t)N48_R5_BC_N; c++) if (!n48_r5_bc_latched(c)) win += r.blind_clause[c];
        expect_u("R12 the census splits: latched_blind == the latched clauses' notes, blind - latched_blind == the windowed clauses' notes",
                 r.latched_blind == lat && r.blind - r.latched_blind == win && r.blind > r.latched_blind && r.latched_blind > 0u ? 1u : 0u, 1u);
    }
}

/* =============================================================================================================================
 * build 0.0.549 MUST-FIX 1 (the 0.0.548 review): LATCHED FROM THE FRAME'S OWN FIELDS.
 *   R13 a MULTI-CLAUSE record whose FIRST failing clause is windowed but which also fails a latched field stays latched past K:
 *       targets-truncated + memw-truncated; target-unresolved + dispatch-unreadable (and the other latched fields behind each windowed
 *       clause); a targets-only record (and a target-unresolved-only, a scan-over-only one) clears after exactly K; n48_r5_frame_latched
 *       agrees with "any latched field fails" over random records; the census still counts the FIRST clause. */
static uint32_t r13_on_refused(const n48_r5_frame &x, uint64_t *lat)
{
    static n48_r5_ring r; std::memset(&r, 0, sizeof r); n48_r5_scope(&r, 4u);
    n48_r5_note(&r, &x, 100u);
    uint32_t refusedOn = 0u;
    for (uint64_t f = 101u; f <= 100u + 32u + 500u; f++) refusedOn += r5_verdict(&r, N48_R5W_M_ON, 32u, f) == N48_DEP_NEUTER_UNREADABLE;
    if (lat) *lat = r.latched_blind;
    return refusedOn;
}
static void r13_multi_clause()
{
    n48_r5_frame a = clause_frame(N48_R5_BC_TGT_TRUNC); a.memw_ok = 0u;                          /* targets-truncated + memw-truncated */
    n48_r5_frame b = clause_frame(N48_R5_BC_TGT_UNRESOLVED); b.has_dispatch = 1u; b.dispatch_ok = 0u; /* target-unresolved + dispatch */
    uint64_t la = 9u, lb = 9u;
    const uint32_t ra = r13_on_refused(a, &la), rb = r13_on_refused(b, &lb);
    expect_u("R13 targets-truncated + memw-truncated: first clause targets-truncated (census), yet LATCHED - ON refuses all 532 frames",
             n48_r5_blind_clause(&a) == N48_R5_BC_TGT_TRUNC && ra == 532u && la == 1u ? 1u : 0u, 1u);
    expect_u("R13 target-unresolved + dispatch-unreadable: first clause target-unresolved (census), yet LATCHED - ON refuses all 532",
             n48_r5_blind_clause(&b) == N48_R5_BC_TGT_UNRESOLVED && rb == 532u && lb == 1u ? 1u : 0u, 1u);
    /* every latched field behind every windowed first clause */
    uint32_t good = 1u;
    const uint32_t wins[] = { N48_R5_BC_TGT_TRUNC, N48_R5_BC_TGT_UNRESOLVED, N48_R5_BC_SCAN_OVER };
    for (uint32_t wi = 0u; wi < 3u; wi++)
        for (uint32_t k = 0u; k < 6u; k++) {
            n48_r5_frame x = clause_frame(wins[wi]);
            switch (k) {
            case 0: x.memw_ok = 0u; break;
            case 1: x.memw_resolved = 0u; break;
            case 2: x.has_dispatch = 1u; x.dispatch_ok = 0u; break;
            case 3: x.ntgt = 0u; x.nmemw = 0u; x.has_draw = 1u; break;
            case 4: x.walk_ok = 0u; break;
            default: x.ib_ok = 0u; break;
            }
            uint64_t l = 0u;
            const uint32_t rr = r13_on_refused(x, &l);
            if (rr != 532u || l != 1u) { good = 0u; std::printf("  R13 windowed %u + latched field %u: ON refused %u\n", wins[wi], k, rr); }
        }
    expect_u("R13 behind targets-truncated / target-unresolved / scan-over, each of memw-truncated, memw-unresolved, dispatch, inherited, "
             "walk, ib-unread latches", good, 1u);
    const n48_r5_frame t = clause_frame(N48_R5_BC_TGT_TRUNC), u = clause_frame(N48_R5_BC_TGT_UNRESOLVED), s = clause_frame(N48_R5_BC_SCAN_OVER);
    uint64_t lt = 9u, lu = 9u, ls = 9u;
    const uint32_t rt = r13_on_refused(t, &lt), ru = r13_on_refused(u, &lu), rs = r13_on_refused(s, &ls);
    expect_u("R13 a targets-only record (and target-unresolved-only, scan-over-only) clears after exactly K = 32",
             rt == 32u && ru == 32u && rs == 32u && lt == 0u && lu == 0u && ls == 0u ? 1u : 0u, 1u);
    /* the decision equals "any latched field fails", over random records (every flag random) */
    uint32_t agree = 1u, multi = 0u;
    for (uint32_t it = 0u; it < 200000u; it++) {
        n48_r5_frame x; std::memset(&x, 0, sizeof x);
        const uint32_t v = rnd();
        x.ib_over = (v & 0x1u) && !(v & 0x800u) ? 1u : 0u;   /* mostly 0 so the later fields decide */
        x.ib_ok = (v & 0x2u) || (v & 0x1000u) ? 1u : 0u;   x.walk_ok = (v & 0x4u) || (v & 0x2000u) ? 1u : 0u;
        x.targets_ok = (v >> 3) & 1u; x.memw_ok = (v & 0x10u) || (v & 0x4000u) ? 1u : 0u;
        x.tgts_resolved = (v >> 5) & 1u; x.memw_resolved = (v & 0x40u) || (v & 0x8000u) ? 1u : 0u;
        x.scan_over = (v >> 7) & 1u; x.has_dispatch = (v >> 8) & 1u; x.dispatch_ok = (v >> 9) & 1u; x.has_draw = (v >> 10) & 1u;
        x.ntgt = (v >> 16) & 1u; x.nmemw = (v >> 17) & 1u;
        const uint32_t want = (x.ib_over || !x.ib_ok || !x.walk_ok || !x.memw_ok || !x.memw_resolved ||
                               (x.has_dispatch && !x.dispatch_ok) || (x.has_draw && !x.ntgt && !x.nmemw)) ? 1u : 0u;
        if (n48_r5_frame_latched(&x) != want) agree = 0u;
        const uint32_t c = n48_r5_blind_clause(&x);
        if (want && c != N48_R5_BC_NONE && !n48_r5_bc_latched(c)) multi++;
    }
    expect_u("R13 n48_r5_frame_latched == (any latched field fails) over 200000 random records; null latches", agree && n48_r5_frame_latched(nullptr) ? 1u : 0u, 1u);
    std::printf("  R13 random records whose first clause is windowed but which latch by their fields: %u\n", multi);
    expect_u("R13 the random sweep reaches the masked case (first clause windowed, a latched field failing)", multi > 0u ? 1u : 0u, 1u);
}

/* =============================================================================================================================
 * build 0.0.550 - LOG-ONLY INSTRUMENTS, EVERY MODE, NO SWITCH:
 *   H1 the namer identity table follows the hazard set's exact rows: appended rows take the noting frame's identity, a scope move
 *      starts again at row 0, a duplicate adds no row; the row lookup finds EXACT rows, and a page the filter holds beyond the 64
 *      rows is FILTER-ONLY (no row, no identity)
 *   H2 the probe the line uses never moves the set's own queries/hits (n48_hz_probe), and n48_hz_hit answers identically
 *   H3 the caps: 4 lines per frame, 64 per arm scope, a new scope starts again; the suppressed count
 *   H4 wo99's per-second commit split: counted only while the window is open, P vs composite, copied into the due line and reset
 *   H5 the r3hit line <= 491 bytes at maximal fields (the new wo99 SEC line is O7's)
 *   H6 source pins: r3_id_pre/post bracket the one n48_r5_note_counted; the hit line only for R3-memdst on a hazard-set evaluator,
 *      with n48_hz_probe (never n48_hz_hit); n550_frame_end counts only a committed frame, after the gate
 * ============================================================================================================================= */
static void h_tests(const std::string &ahh)
{
    static n48_hazard hz; std::memset(&hz, 0, sizeof hz);
    static n48_r3_id tab[N48_HZ_ROWS]; std::memset(tab, 0, sizeof tab);
    n48_hz_scope(&hz, 5u);
    auto note = [&](uint64_t frame, const uint64_t *pages, uint32_t np, uint32_t seq) {
        const uint32_t u0 = hz.used, s0 = hz.arm_seq;
        n48_hz_scope(&hz, seq);
        for (uint32_t i = 0; i < np; i++) n48_hz_add(&hz, pages[i], pages[i] | 0x400000000ull, frame);
        n48_r3_id id {}; id.frame = frame; id.key = 0x1000u + frame; id.cb0 = 0x401800000ull; id.pid = 77; id.len0 = 1040u; id.nib = 1u;
        id.verdict = 3u;
        n48_r3_id_note(tab, u0, s0, hz.used, hz.arm_seq, &id);
    };
    const uint64_t p1[] = { 0x10010000ull, 0x10011000ull }, p2[] = { 0x10011000ull, 0x10020000ull };
    note(11u, p1, 2u, 5u);
    note(12u, p2, 2u, 5u);
    uint32_t r = n48_r3_row_of(&hz, 0x10011000ull);
    expect_u("H1 a page named by frame 11 then again by frame 12 is ONE row, its namer frame 11, identity frame 11",
             r < N48_HZ_ROWS && hz.row[r].namer == 11u && tab[r].valid && tab[r].frame == 11u && tab[r].key == 0x1000u + 11u ? 1u : 0u, 1u);
    r = n48_r3_row_of(&hz, 0x10020000ull);
    expect_u("H1 frame 12's new page takes frame 12's identity", r == 2u && tab[r].valid && tab[r].frame == 12u ? 1u : 0u, 1u);
    const uint64_t p3[] = { 0x10030000ull };
    note(13u, p3, 1u, 6u);
    expect_u("H1 a scope move: row 0 is the new scope's (frame 13) and the old rows are invalid",
             hz.used == 1u && tab[0].valid && tab[0].frame == 13u && !tab[1].valid && !tab[2].valid ? 1u : 0u, 1u);
    for (uint32_t i = 0; i < N48_HZ_ROWS + 3u; i++) { const uint64_t pg = 0x20000000ull + (uint64_t)i * 0x1000ull; note(100u + i, &pg, 1u, 6u); }
    const uint64_t last = 0x20000000ull + (uint64_t)(N48_HZ_ROWS + 2u) * 0x1000ull;
    uint32_t exact = 9u;
    (void)n48_hz_count(&hz, last, &exact);
    expect_u("H1 a page past the 64 rows: the filter holds it, no exact row, no row index (FILTER-ONLY)",
             n48_hz_probe(&hz, last) == 1u && exact == 0u && n48_r3_row_of(&hz, last) == N48_HZ_ROWS && hz.over == 4u ? 1u : 0u, 1u);
    expect_u("H1 every exact row has its identity, and the identity frame equals the row's namer", [&]() {
        for (uint32_t i = 0; i < hz.used; i++) if (!tab[i].valid || tab[i].frame != hz.row[i].namer) return 0u;
        return 1u; }(), 1u);
    const uint64_t q0 = hz.queries, h0 = hz.hits;
    uint32_t same = 1u;
    for (uint32_t i = 0; i < 200u; i++) {
        const uint64_t pg = 0x10000000ull + (uint64_t)i * 0x1000ull;
        const uint32_t a = n48_hz_probe(&hz, pg);
        if (a != n48_hz_probe(&hz, pg)) same = 0u;
    }
    expect_u("H2 n48_hz_probe never moves the set's queries/hits", hz.queries == q0 && hz.hits == h0 && same ? 1u : 0u, 1u);
    // H3
    n48_r3h_cap c {};
    uint32_t took = 0u;
    for (uint32_t k = 0; k < 10u; k++) took += n48_r3h_take(&c, 6u, 1u);
    expect_u("H3 4 lines per frame (10 hits in frame 1 -> 4 lines, 6 suppressed)", took * 100u + (uint32_t)c.suppressed, 406u);
    for (uint64_t f = 2u; f < 40u; f++) for (uint32_t k = 0; k < 4u; k++) took += n48_r3h_take(&c, 6u, f);
    expect_u("H3 64 lines per arm scope", took, N48_R3H_LINES_SCOPE);
    expect_u("H3 a new scope starts again", n48_r3h_take(&c, 7u, 40u), 1u);
    // H4
    n48_wo_win w; std::memset(&w, 0, sizeof w);
    n48_wo_secline L; std::memset(&L, 0, sizeof L);
    n48_wo_sec_commit(&w, 0u);
    expect_u("H4 a closed window counts nothing", w.comp + w.pcm, 0u);
    (void)n48_wo_sec_tick(&w, 1u, 1000u, 0u, &L);
    n48_wo_sec_commit(&w, 1u); n48_wo_sec_commit(&w, 1u); n48_wo_sec_commit(&w, 0u);
    const uint32_t due = n48_wo_sec_tick(&w, 1u, 1000u + N48_WO_SEC_US, 19u, &L);
    expect_u("H4 the due line carries this second's split: 1 composite, 2 P", due && L.comp == 1u && L.pcm == 2u ? 1u : 0u, 1u);
    expect_u("H4 ... and the window starts the next second at 0", w.comp + w.pcm, 0u);
    n48_wo_sec_commit(&w, 0u);
    (void)n48_wo_sec_tick(&w, 0u, 5000000u, 0u, &L);
    expect_u("H4 not armed: the window closes; a later open starts at 0", w.open == 0u ? 1u : 0u, 1u);
    (void)n48_wo_sec_tick(&w, 1u, 9000000u, 0u, &L);
    expect_u("H4 ... reopened at 0", w.comp + w.pcm, 0u);
    expect_u("H4 the 32-bit clamp", n48_wo_u32(~0ull) == 0xFFFFFFFFu && n48_wo_u32(7u) == 7u ? 1u : 0u, 1u);
    // H5
    {
        char buf[2048];
        const int n = std::snprintf(buf, sizeof buf, N48_R3H_FMT, ~0ull, 0xFFFFFFFFu, 0xFFFFFFFFu, ~0ull, ~0ull,
                                    "FILTER-ONLY (both filter bits set, no exact row)", ~0ull, ~0ull, ~0ull, "no identity", -2147483647 - 1,
                                    ~0ull, ~0ull, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, ~0ull, ~0ull, ~0ull, ~0ull);
        std::printf("      widths: r3hit %d\n", n);
        expect_u("H5 the r3hit line is UNDER 491 bytes at maximal fields", n > 0 && (uint32_t)n < N48_LOG_CAP_BODY ? 1u : 0u, 1u);
    }
    // H6
    if (ahh.size() < 100000u) { expect_u("H6 AppleHardwareHook.cpp read", 0u, 1u); return; }
    const size_t np = std::string::npos;
    const size_t a = ahh.find("                r3_id_pre();   // build 0.0.550");
    const size_t b = ahh.find("                n48_r5_note_counted(&gR5Ring, gXpScopeSeq, &r5f, gXdC.judged + 1u,");
    const size_t c2 = ahh.find("                r3_id_post(pid, key, tgtN ? tgtVa : 0ull, f.ib[0].len, f.nib, verdict);");
    expect_u("H6 PIN r3_id_pre, THEN the one n48_r5_note_counted, THEN r3_id_post", a != np && b != np && c2 != np && a < b && b < c2 &&
             count(ahh, "n48_r5_note_counted(&gR5Ring") == 1u && count(ahh, "r3_id_pre()") == 1u && count(ahh, "r3_id_post(") == 2u ? 1u : 0u, 1u);
    const std::string hn = body_of(ahh, "static __attribute__((noinline)) void r3_hit_note(");
    expect_u("H6 PIN the hit line asks n48_hz_probe (never the counting n48_hz_hit) and moves nothing of the set",
             count(hn, "n48_hz_probe(&gR5Ring.hz, pg[i])") == 1u && count(hn, "n48_hz_hit") == 0u && count(hn, "n48_hz_add") == 0u &&
             count(hn, "gR5Ring.hz.") == 2u ? 1u : 0u, 1u);
    const std::string ce = body_of(ahh, "static void gfxsrc_cprov_eval(const GfxcVm &vm)");
    const size_t e1 = ce.find("        r3hz = hz;       // build 0.0.550");
    const size_t e2 = ce.find("    if (clause == N48_CP_R3_MEMDST && r3hz) r3_hit_note(r3p, r3pg, r3n);");
    const size_t e3 = ce.find("    gXp.lastRowsFree = rowsFree;");
    expect_u("H6 PIN the hit line: only for R3-memdst on a hazard-set evaluator, after the clause, before the tallies; once",
             e1 != np && e2 != np && e3 != np && e1 < e2 && e2 < e3 && count(ahh, "r3_hit_note(") == 2u ? 1u : 0u, 1u);
    const std::string fe = body_of(ahh, "static __attribute__((noinline)) void n550_frame_end(");
    expect_u("H6 PIN n550_frame_end counts a frame into wo99's window only when the gate committed it",
             count(fe, "    if (commitOk) n48_wo_sec_commit(&gWoWin, pShape);") == 1u && count(ahh, "n48_wo_sec_commit(") == 1u ? 1u : 0u, 1u);
}

int main(int argc, char **argv)
{
    const std::string ahh = slurp(argc > 1 ? argv[1] : nullptr);
    o1(); o2(); o3(); o4();
    o5(argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr, ahh);
    o6(ahh);
    o7();
    r_tests(ahh, argc > 2 ? argv[2] : nullptr);
    r12_latched();
    r13_multi_clause();
    h_tests(ahh);   // build 0.0.550 (log-only instruments)
    std::printf("%s: %d run, %d failed\n", gFail ? "FAIL" : "PASS", gRun, gFail);
    return gFail ? 1 : 0;
}
