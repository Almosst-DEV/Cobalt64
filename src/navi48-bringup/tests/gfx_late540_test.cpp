// gfx_late540_test.cpp — build 0.0.540 item 6: late-phase evidence (gfx_late540.h, gfx_p87.h's late window) and its
// kext glue. Logging and capture only.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_late540_test.cpp -o /tmp/lt540 && /tmp/lt540 \
//         src/navi48-bringup/src/apple/AppleHardwareHook.cpp
// Covers:
//   L1 (a) the capture's late window: the first 160 frames only record signatures; past them a never-seen signature is taken (at
//      most 24 per capture arm), a seen one is not, each of the six named signatures is taken twice whenever it occurs (seen or not);
//      the table is bounded (64 rows; a full table is counted and never grows); a capture arm resets it; the memory bound;
//   L2 (c)/(d) the two-window cap: 32 lines from the arm's start, then 64 more only from judge frame 480 on, the rest counted;
//   L3 (c) THE GATE DECISION IS UNTOUCHED: n48_lt_live_first says "live" exactly when gfx_commit.h's n48_cm_live does, over all 2^13
//      inputs, and the clause it names is the first that fails in the kext's `live` order (every earlier clause holds);
//   L4 (b) tex531's late window: after the 64 early lines, frames < 480 print nothing more, frames >= 480 print 64 more per arm;
//   L5 every new line <= 491 bytes at maximal fields;
//   L6 the glue (source pins): the gate note is one statement right after the gate, values only, `reason` still assigned in exactly
//      its three places, `live` defined once; the decide frame's line call; the capture's 160-frame rule unchanged beside the late
//      window; the table reset at the capture arm under its lock; the report lines printed.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "gfx_commit.h"
#include "gfx_late540.h"
#include "gfx_p87.h"

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
static uint64_t gRs = 0x9e3779b97f4a7c15ull;
static uint32_t rnd() { gRs ^= gRs << 13; gRs ^= gRs >> 7; gRs ^= gRs << 17; return (uint32_t)(gRs >> 11); }
static n48_lt_sig sig2(uint32_t a, uint32_t b) { n48_lt_sig s; std::memset(&s, 0, sizeof s); s.nib = b ? 2u : 1u; s.len[0] = a; s.len[1] = b; return s; }

static void l1()
{
    static n48_lt_cap c; std::memset(&c, 0, sizeof c);
    // the first 160: signatures recorded, nothing taken (the existing rule decides those frames)
    uint32_t took = 0;
    for (uint32_t f = 0; f < 160u; f++) { n48_lt_sig s = sig2(1040u + (f % 5u), 0u); took += n48_lt_cap_note(&c, &s, 0u); }
    n48_lt_sig t0 = sig2(16224u, 1776u);
    took += n48_lt_cap_note(&c, &t0, 0u);   // a named one seen early
    expect_u("L1 inside the first 160 nothing is taken by the late window; 6 signatures recorded", took == 0u && c.nseen == 6u, 1u);
    // past: a seen signature is not taken; a never-seen one is, up to 24
    n48_lt_sig seen = sig2(1041u, 0u);
    expect_u("L1 past 160: a signature seen before is not taken", n48_lt_cap_note(&c, &seen, 1u), 0u);
    uint32_t tn = 0;
    for (uint32_t k = 0; k < 40u; k++) { n48_lt_sig s = sig2(2000u + k, 0u); tn += n48_lt_cap_note(&c, &s, 1u); }
    expect_u("L1 never-seen signatures: 24 taken of 40, the rest counted over budget", tn == N48_LT_NEW_BUDGET && c.takenNew == 24u &&
             c.refusedBudget == 16u, 1u);
    n48_lt_sig again = sig2(2000u, 0u);
    expect_u("L1 ... a signature taken once is now seen (not taken again)", n48_lt_cap_note(&c, &again, 1u), 0u);
    // the six named: twice each, whenever they occur, even when seen before and after the new budget is spent
    const uint32_t names[6][2] = { { 16224u, 1776u }, { 16192u, 1776u }, { 16224u, 1760u }, { 16192u, 1760u }, { 14544u, 0u }, { 14528u, 0u } };
    uint32_t tt = 0;
    for (uint32_t r = 0; r < 5u; r++) for (uint32_t k = 0; k < 6u; k++) { n48_lt_sig s = sig2(names[k][0], names[k][1]); tt += n48_lt_cap_note(&c, &s, 1u); }
    expect_u("L1 each of the six named signatures is taken exactly twice (12 of 30)", tt == 12u && c.takenTgt == 12u, 1u);
    uint32_t each = 1u; for (uint32_t k = 0; k < 6u; k++) if (c.tgtTaken[k] != 2u) each = 0u;
    expect_u("L1 ... two of each", each, 1u);
    n48_lt_sig near = sig2(16224u, 1777u);
    expect_u("L1 a near miss (16224|1777) is not a named signature", n48_lt_target(&near), (uint64_t)N48_LT_TARGETS);
    // the table bound
    static n48_lt_cap d; std::memset(&d, 0, sizeof d);
    for (uint32_t k = 0; k < 100u; k++) { n48_lt_sig s = sig2(5000u + k, 7u); (void)n48_lt_cap_note(&d, &s, 0u); }
    expect_u("L1 the table holds 64 signatures; the rest are counted (never grows)", d.nseen == N48_LT_SIGS && d.tableFull == 36u, 1u);
    expect_u("L1 THE MEMORY BOUND: the table is a 64-row static (<= 1.5 KiB); late bodies <= 24 + 6 x 2 = 36 per capture arm",
             sizeof(n48_lt_cap) <= 1536u && N48_LT_NEW_BUDGET + N48_LT_TARGETS * N48_LT_TARGET_EACH == 36u, 1u);
    n48_lt_cap_reset(&c);
    n48_lt_sig s2 = sig2(2000u, 0u);
    expect_u("L1 a capture arm resets the window (a signature taken last arm is new again)", n48_lt_cap_note(&c, &s2, 1u) == 1u &&
             c.nseen == 1u && c.newTaken == 1u, 1u);
    n48_lt_sig bad; std::memset(&bad, 0, sizeof bad);
    expect_u("L1 a frame with no IBs (or more than 4) is never taken", n48_lt_cap_note(&c, &bad, 1u), 0u);
    char b[64];
    n48_lt_sig w = sig2(16224u, 1776u);
    expect_u("L1 the signature's text", std::strcmp(n48_lt_sig_str(&w, b, sizeof b), "16224|1776") == 0, 1u);
}

static void l2()
{
    n48_lt_lines l; std::memset(&l, 0, sizeof l);
    uint32_t n = 0;
    for (uint64_t f = 1; f < 480u; f++) n += n48_lt_cap_take(&l, f);
    expect_u("L2 32 lines from the arm's start; nothing more before judge frame 480", n == N48_LT_EARLY && l.suppressed == 479u - 32u, 1u);
    uint32_t m = 0;
    for (uint64_t f = 480; f < 700u; f++) m += n48_lt_cap_take(&l, f);
    expect_u("L2 64 more from judge frame 480 on, the rest counted", m == N48_LT_LATE && l.suppressed == (479u - 32u) + (220u - 64u), 1u);
    n48_lt_lines_reset(&l);
    expect_u("L2 a new arm: the early window again", n48_lt_cap_take(&l, 900u) == 1u && l.early == 1u && l.late == 0u, 1u);
}

static void l3()
{
    uint32_t same = 1u, order = 1u;
    for (uint32_t m = 0; m < (1u << 13); m++) {
        const uint32_t b[13] = { m & 1u, (m >> 1) & 1u, (m >> 2) & 1u, (m >> 3) & 1u, (m >> 4) & 1u, (m >> 5) & 1u, (m >> 6) & 1u,
                                 (m >> 7) & 1u, (m >> 8) & 1u, (m >> 9) & 1u, (m >> 10) & 1u, (m >> 11) & 1u, (m >> 12) & 1u };
        // b: armed, translate, built, buffers, dep, md_enforce, md_ok, ring_full, cont_on, cont_fence_ok, cs, draw, heap
        const uint32_t first = n48_lt_live_first(b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12]);
        const uint32_t live = n48_cm_live(b[0] ? (uint32_t)N48_SD_ARM_COMMIT : (uint32_t)N48_SD_ARM_DECIDE,
                                          b[1] ? (uint32_t)N48_XV_TRANSLATE : (uint32_t)N48_XV_SEG_POLICY, b[2], b[3], b[4], b[5], b[6], b[7],
                                          b[8], b[9], b[10], b[12], b[11]);
        if ((first == N48_LT_LIVE) != (live == 1u)) same = 0u;
        // the named clause fails, every earlier one holds (the kext's `live` order)
        const uint32_t fails[N48_LT_CLAUSES] = { 0u, !b[0], !b[1], !b[2], !b[3], !b[4], b[5] && !b[6], b[7], b[8] && !b[9],
                                                 b[10] && !(b[5] && b[6]), b[11] && !(b[5] && b[6]), b[12] };
        if (first != N48_LT_LIVE) {
            if (first >= N48_LT_CLAUSES || !fails[first]) order = 0u;
            for (uint32_t k = 1; k < first; k++) if (fails[k]) order = 0u;
        }
    }
    expect_u("L3 over all 2^13 inputs: 'live' exactly when n48_cm_live says live (the decision is the gate's, untouched)", same, 1u);
    expect_u("L3 ... and the named clause is the FIRST that fails in the kext's order", order, 1u);
    (void)rnd;
}

static void l4()
{
    static n48_p87 s; std::memset(&s, 0, sizeof s);
    s.on = N48_P87_ON;
    const uint32_t rec[8] = { 0x04008000u, 0x0421c004u, 0x00efc07fu, 0x0f90fac6u, 0u, 0u, 0x00200000u, 0u };
    const uint64_t P = ((uint64_t)46u << 32) | 0xebaa377cu;
    uint32_t early = 0, mid = 0, late = 0;
    for (uint64_t fr = 2; fr < 200u; fr++) { for (uint32_t d = 0; d < 3u; d++) (void)n48_p87_note(&s, fr, P, d, d, rec); early += n48_p87_frame_end(&s, fr, 1u); }
    for (uint64_t fr = 300; fr < 480u; fr++) { (void)n48_p87_note(&s, fr, P, 1u, 1u, rec); mid += n48_p87_frame_end(&s, fr, 1u); }
    for (uint64_t fr = 480; fr < 600u; fr++) { (void)n48_p87_note(&s, fr, P, 1u, 1u, rec); late += n48_p87_frame_end(&s, fr, 1u); }
    expect_u("L4 tex531: 64 early lines; nothing more before judge frame 480; then 64 more (the late window)",
             early == N48_P87_LINES && mid == 0u && late == N48_P87_LATE_LINES && s.lateLines == 64u && s.latePrinted == 64u, 1u);
    expect_u("L4 ... the rest counted suppressed", s.suppressed, (uint64_t)(198u * 3u - 64u + 180u + (120u - 64u)));
    n48_p87_arm_reset(&s);
    (void)n48_p87_note(&s, 700u, P, 1u, 1u, rec);
    expect_u("L4 a new arm resets both windows", n48_p87_frame_end(&s, 700u, 1u) == 1u && s.lines == 1u && s.lateLines == 0u, 1u);
    // a frame >= 480 while the EARLY budget is not spent uses the early budget first
    n48_p87_arm_reset(&s);
    for (uint32_t d = 0; d < 3u; d++) (void)n48_p87_note(&s, 800u, P, d, d, rec);
    expect_u("L4 the early budget is spent first even late", n48_p87_frame_end(&s, 800u, 1u) == 3u && s.lines == 3u && s.lateLines == 0u, 1u);
}

static uint32_t fits(const char *what, int n)
{
    std::printf("  %-10s %d bytes\n", what, n);
    return n > 0 && (unsigned)n <= N48_LOG_CAP_BODY ? 1u : 0u;
}
static void l5()
{
    char b[2048], sig[64];
    n48_lt_sig w; w.nib = 4; for (uint32_t k = 0; k < 4u; k++) w.len[k] = 0xFFFFFu;
    (void)n48_lt_sig_str(&w, sig, sizeof sig);
    const unsigned long long M = ~0ull; const uint32_t U = 4294967295u;
    uint32_t ok = 1u;
    ok &= fits("GATE", std::snprintf(b, sizeof b, N48_LT_GATE_FMT, M, sig, "MEMORY-DESTINATION", U, "RESERVED-FOR-PLANE", U, U, U));
    for (uint32_t r = 0; r < N48_CM_REASONS; r++)
        ok &= (unsigned)std::snprintf(b, sizeof b, N48_LT_GATE_FMT, M, sig, "CONTINUOUS-NO-FENCE", U, n48_cm_reason_name(r), U, U, U) <= N48_LOG_CAP_BODY;
    ok &= fits("SEG", std::snprintf(b, sizeof b, N48_LT_SEG_FMT, M, sig, U, U, U, U, U, U, U, U, U, U));
    ok &= fits("CAP", std::snprintf(b, sizeof b, N48_LT_CAP_FMT, M, M, M, M, U, M, U, U, U, U, U, U));
    ok &= fits("COUNT", std::snprintf(b, sizeof b, N48_LT_COUNT_FMT, M, M, M, M, M, M, M, M, M, M, M, M, M, M));
    ok &= fits("P87-3", std::snprintf(b, sizeof b, N48_P87_REPORT3_FMT, M, U, U, M));
    expect_u("L5 every item-6 line fits 491 bytes at maximal fields (the 4-IB signature at 20-bit lengths)", ok, 1u);
}

static void l6(const char *ahhp)
{
    const std::string s = slurp(ahhp);
    expect_u("L6 the source was read", !s.empty(), 1u);
    const std::string ct = body_of(s, "static uint32_t gfxsrc_commit_try(");
    const char *note = "    if (!live && pf_on()) lt_gate_note(n48_lt_live_first(arm == N48_SD_ARM_COMMIT, verdict == N48_XV_TRANSLATE, gXdBuild.ok, c.buffers_ok,\n"
                       "                                                         c.dep_ok, gMdMode == N48_MD_MODE_ENFORCE, gXdBuild.md_ok, c.ring_full,\n"
                       "                                                         c.cont_on, c.cont_fence_ok, c.cs_elided, c.draw_elided, c.heap_refuse),\n"
                       "                                       verdict, reason, detail);\n";
    expect_u("L6 (c) the note: ONE statement right after the gate, switch 96 ON only, values only",
             count(ct, note) == 1u && count(s, "lt_gate_note(") == 2u &&
             ordered(ct, { "    uint32_t reason = n48_cm_gate(&c, &detail);\n", note, "    const uint32_t f828Neutered =" }), 1u);
    expect_u("L6 (c) THE GATE DECISION IS UNTOUCHED: `reason` is assigned in exactly its three places, `live` and `detail` once",
             count(ct, "reason = ") == 3u && count(ct, "uint32_t reason = n48_cm_gate(&c, &detail);") == 1u &&
             count(ct, "if (f828Neutered) reason = N48_CM_FENCE_REGION_MOVED;") == 1u && count(ct, " reason = N48_CM_RING_FULL;") == 1u &&
             count(ct, "const bool live = ") == 1u && count(ct, "live = ") == 1u && count(ct, "detail = ") == 1u &&
             count(ct, "    uint32_t detail = 0;") == 1u, 1u);
    const std::string gn = body_of(s, "static __attribute__((noinline)) void lt_gate_note(uint32_t clause, uint32_t verdict, uint32_t reason, uint32_t detail)\n{");
    expect_u("L6 (c) lt_gate_note writes only its own record and counts", !gn.empty() && count(gn, "gXd") == 0u && count(gn, "gLtGate.") >= 5u, 1u);
    expect_u("L6 (c)/(d) the decide frame prints them after the gate, switch 96 ON only",
             count(s, "    if (pf_on()) lt_frame_lines(&f, gXdC.judged + 1u, verdict);") == 1u &&
             ordered(s, { "? gfxsrc_commit_try(vm, info, ib0Va, f.ib[0].got, f.nib, arm, verdict, stampNow, tgtVa) : 0u;",
                          "if (pf_on()) lt_frame_lines(&f, gXdC.judged + 1u, verdict);" }), 1u);
    const std::string fl = body_of(s, "static __attribute__((noinline)) void lt_frame_lines(const n48_xv_frame *f, uint64_t frame, uint32_t verdict)\n{");
    expect_u("L6 (c)/(d) the lines are capped (n48_lt_cap_take) and read-only (no store but the caps and its own record)",
             count(fl, "n48_lt_cap_take(&gLtGateLines, frame)") == 1u && count(fl, "n48_lt_cap_take(&gLtSegLines, frame)") == 1u &&
             count(fl, "gXdBuild.") >= 1u && count(fl, "gXdBuild.seg[s] =") == 0u && count(fl, "gXdIb[g.head] =") == 0u, 1u);
    expect_u("L6 the caps reset at the START (pf_arm_start)",
             ordered(body_of(s, "static __attribute__((noinline)) void pf_arm_start()\n{"),
                     { "if (!pf_on()) return;", "n48_lt_lines_reset(&gLtGateLines);", "n48_lt_lines_reset(&gLtSegLines);" }), 1u);
    // (a) the capture
    const std::string cap = body_of(s, "static void gcap_submission(void *self, const uint8_t *info, uint32_t srcCheck) {");
    expect_u("L6 (a) the 160-frame rule is unchanged, the late window only OR'd beside it",
             count(cap, "    const bool full = (readVm.ok && gGc.full < kGcapFullFrames && srcCheck == N48_SRC_OK) || ltTake;") == 1u &&
             count(cap, "const bool ltTake = readVm.ok && srcCheck == N48_SRC_OK && n48_lt_cap_note(&gGcLate, &ltSig, ltPast ? 1u : 0u) && ltPast;") == 1u &&
             count(cap, "const bool ltPast = gGc.full >= kGcapFullFrames;") == 1u && count(cap, "if (full) gGc.full++;") == 1u, 1u);
    expect_u("L6 (a) the table is a file-scope static, reset when the capture is armed, under its lock",
             count(s, "static n48_lt_cap gGcLate {};") == 1u &&
             count(s, "            IOLockLock(gGcapLock);                 // build 0.0.540 item 6(a): a capture arm starts its late window afresh\n"
                      "            n48_lt_cap_reset(&gGcLate);\n            IOLockUnlock(gGcapLock);") == 1u, 1u);
    expect_u("L6 the report lines are printed (capture verb, perf540 verb, tex531 report)",
             count(s, "HWLOG(N48_LT_CAP_FMT,") == 1u && count(s, "HWLOG(N48_LT_COUNT_FMT,") == 1u && count(s, "HWLOG(N48_P87_REPORT3_FMT,") == 1u &&
             count(s, "HWLOG(N48_LT_GATE_FMT,") == 1u && count(s, "HWLOG(N48_LT_SEG_FMT,") == 1u, 1u);
}

int main(int argc, char **argv)
{
    l1(); l2(); l3(); l4(); l5();
    if (argc >= 2) l6(argv[1]);
    else expect_u("the source file was given (AppleHardwareHook.cpp)", 0u, 1u);
    std::printf("%d run, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
