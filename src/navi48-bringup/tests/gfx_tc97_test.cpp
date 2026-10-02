// gfx_tc97_test.cpp — build 0.0.540 item 5 (switch 97, xlat12_ib.h XLAT12_EXTRA_TBLCACHE): the output verifier's table cache.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I src/navi48-bringup/src/apple \
//         -I src/xlat12 -x c++ src/navi48-bringup/tests/gfx_tc97_test.cpp src/xlat12/xlat12_ib.c src/xlat12/xlat12.c -o /tmp/tc97 && \
//         /tmp/tc97 src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/xlat12/xlat12_ib.c
// Covers:
//   Q1 the FIRST asks race: 8 threads ask the cache before it exists (one builds, the others answer from the walk); every answer is
//      the linear walk's, and afterwards the cache is built (state 2);
//   Q2 EXHAUSTIVE over the verifier's domain: every 4-byte-aligned gfx12 address in [0, 0x80000) (every base + 16-bit offset the
//      SET packets can name) answers the same from the cache as from the walk;
//   Q3 every register-table row, by class: its gfx12 address answers the same; rows of the four allowed classes answer 1; the table
//      fits XLAT12_TC_CAP;
//   Q4 the verifier itself: random SET_CONTEXT / SET_SH / SET_UCONFIG streams (table registers, policy registers, neither) give the same
//      return, bad address and bad op with and without the flag;
//   Q5 the kext glue (source pins): OFF at boot, the verb its only writer, the mid-arm guard, read once per pass, set on every segment
//      (and nowhere else), d_verify's flag from the translation's own `ex`, the OFF call is d_table_g12 itself; the report line fits.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "xlat12.h"
#include "xlat12_ib.h"
#include "gfx_commit.h"
#include "gfx_perf540.h"
#include "tests/fixture_r44_drawinj.h"   // src/xlat12/tests: Apple's real draw-injection IB (xlat12's own self-test input)

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
static uint64_t gRs = 0x9e3779b97f4a7c15ull;
static uint32_t rnd() { gRs ^= gRs << 13; gRs ^= gRs >> 7; gRs ^= gRs << 17; return (uint32_t)(gRs >> 11); }

static void q1()
{
    static const uint32_t probes[] = { 0x28c70u, 0x28000u, 0x2c08u << 2, 0x31128u, 0x12345u << 2, 0x28a6cu, 0xb030u, 0x30998u };
    uint32_t bad[8] = {};
    std::vector<std::thread> th;
    for (uint32_t t = 0; t < 8u; t++)
        th.emplace_back([t, &bad]() {
            for (uint32_t r = 0; r < 200u; r++) {
                int lin = -1, cac = -1;
                (void)xlat12_ib_tblcache_answer(probes[(t + r) % 8u], &lin, &cac);
                if (lin != cac) bad[t]++;
            }
        });
    for (auto &x : th) x.join();
    uint32_t b = 0; for (uint32_t t = 0; t < 8u; t++) b += bad[t];
    expect_u("Q1 8 threads asking while the cache is built: every answer is the walk's", b, 0u);
    int lin = 0, cac = 0;
    expect_u("Q1 ... and the cache is built afterwards (state 2)", xlat12_ib_tblcache_answer(0x28c70u, &lin, &cac), 2u);
}

static void q2()
{
    uint32_t diff = 0, allowed = 0, firstDiff = 0xFFFFFFFFu;
    for (uint32_t a = 0; a < 0x80000u; a += 4u) {
        int lin = 0, cac = 0;
        (void)xlat12_ib_tblcache_answer(a, &lin, &cac);
        if (lin != cac) { if (!diff) firstDiff = a; diff++; }
        allowed += lin ? 1u : 0u;
    }
    std::printf("  exhaustive: %u addresses allowed by the table in [0, 0x80000)\n", allowed);
    if (diff) std::printf("  first difference at %#x\n", firstDiff);
    expect_u("Q2 EXHAUSTIVE: every aligned gfx12 address in [0, 0x80000) answers the same from the cache and the walk", diff, 0u);
    expect_u("Q2 ... and the table allows some (the domain is not vacuous)", allowed > 100u, 1u);
}

static void q3()
{
    uint32_t rows = 0, diff = 0, allowedRows = 0, allowedYes = 0, byCls[16] = {}, outside = 0;
    for (uint32_t i = 0; i < xlat12_table_len(); i++) {
        uint32_t g10 = 0, g12 = 0, cls = 0; const char *nm = nullptr;
        if (!xlat12_table_entry(i, &g10, &g12, &cls, &nm)) continue;
        rows++;
        byCls[cls & 15u]++;
        int lin = 0, cac = 0;
        (void)xlat12_ib_tblcache_answer(g12, &lin, &cac);
        if (lin != cac) diff++;
        if (g12 >= 0x80000u) outside++;
        if (cls == XLAT12_CLS_IDENTICAL || cls == XLAT12_CLS_MOVED || cls == XLAT12_CLS_FIELD_REPACK || cls == XLAT12_CLS_REUSED) {
            allowedRows++;
            if (cac) allowedYes++;
        }
    }
    std::printf("  table rows %u (allowed classes %u); by class:", rows, allowedRows);
    for (uint32_t c = 0; c < 16u; c++) if (byCls[c]) std::printf(" %u:%u", c, byCls[c]);
    std::printf("; g12 outside [0, 0x80000): %u\n", outside);
    expect_u("Q3 every row's gfx12 address answers the same from the cache and the walk (every class)", diff, 0u);
    expect_u("Q3 every row of the four allowed classes answers 1 from the cache", allowedYes, allowedRows);
    expect_u("Q3 the allowed rows fit XLAT12_TC_CAP", allowedRows <= XLAT12_TC_CAP, 1u);
}

static void q4()
{
    // candidate addresses: table registers, policy registers, and random others, as SET packets in the three register spaces
    std::vector<uint32_t> regs;
    for (uint32_t i = 0; i < xlat12_table_len(); i++) {
        uint32_t g10 = 0, g12 = 0, cls = 0; const char *nm = nullptr;
        if (xlat12_table_entry(i, &g10, &g12, &cls, &nm)) regs.push_back(g12);
    }
    uint32_t diff = 0, verified = 0, refused = 0;
    static uint32_t out[4096];
    for (uint32_t t = 0; t < 20000u; t++) {
        uint32_t n = 0;
        const uint32_t pk = 1u + rnd() % 6u;
        for (uint32_t p = 0; p < pk && n + 8u < 4096u; p++) {
            const uint32_t a = (rnd() % 4u) ? regs[rnd() % regs.size()] : ((rnd() % 0x70000u) & ~3u);
            uint32_t op, base;
            if (a >= 0x28000u && a < 0x30000u) { op = 0x69u; base = 0xA000u; }
            else if (a >= 0x30000u && a < 0x40000u) { op = 0x79u; base = 0xC000u; }
            else if (a >= 0xB000u && a < 0xC000u) { op = 0x76u; base = 0x2C00u; }
            else continue;
            const uint32_t cnt = 1u + rnd() % 3u;
            out[n++] = 0xC0000000u | ((cnt) << 16) | (op << 8);
            out[n++] = (a >> 2) - base;
            for (uint32_t k = 0; k < cnt; k++) out[n++] = rnd();
        }
        if (!n) continue;
        uint32_t b0 = 0, o0 = 0, b1 = 0, o1 = 0;
        const uint32_t r0 = xlat12_ib_draw_verify_ex(out, n, 0u, &b0, &o0);
        const uint32_t r1 = xlat12_ib_draw_verify_ex(out, n, XLAT12_EXTRA_TBLCACHE, &b1, &o1);
        const uint32_t r2 = xlat12_ib_draw_verify(out, n, nullptr, nullptr);
        if (r0 != r1 || b0 != b1 || o0 != o1 || r2 != r0) diff++;
        if (r0 == 0u) verified++; else refused++;
    }
    std::printf("  random streams: %u verified, %u refused\n", verified, refused);
    expect_u("Q4 the verifier answers the same (return, bad address, bad op) with and without the flag; the public one is OFF's", diff, 0u);
    expect_u("Q4 ... over streams both verified and refused", verified > 100u && refused > 100u, 1u);
}

// Q6 THE FLAG IS ACCEPTED: a real IB (R44's draw injection) translates with XLAT12_EXTRA_TBLCACHE exactly as without it (status,
// length, every output dword), and the flag is one the translator's argument check admits (the corpus run found ERR_ARG first).
static void q6()
{
    static uint32_t o0[1022], o1[1022];
    const uint32_t n = 1022u;
    xlat12_draw_extra e0; std::memset(&e0, 0, sizeof e0);
    xlat12_draw_extra e1 = e0; e1.flags = XLAT12_EXTRA_TBLCACHE;
    xlat12_draw_stats d0, d1; uint32_t l0 = 0, l1 = 0;
    for (uint32_t k = 0; k < n; k++) { o0[k] = 0xDEADBEEFu; o1[k] = 0xDEADBEEFu; }
    const uint32_t s0 = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &e0, kR44DrawInjIb, n, o0, &l0, &d0);
    const uint32_t s1 = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &e1, kR44DrawInjIb, n, o1, &l1, &d1);
    expect_u("Q6 R44 translates OFF (status 0)", s0, 0u);
    xlat12_draw_extra e2 = e0; e2.flags = XLAT12_EXTRA_TBLCACHE << 1; uint32_t l2 = 0;
    expect_u("Q6 the flag is 0x1000000 (the bit after PWS); the next bit is still refused ERR_ARG",
             XLAT12_EXTRA_TBLCACHE == (XLAT12_EXTRA_PWS << 1) &&
             xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &e2, kR44DrawInjIb, n, o1, &l2, &d1) == (uint32_t)XLAT12_ERR_ARG, 1u);
    expect_u("Q6 ... and ON with the same status, length and every output dword",
             s1 == s0 && l1 == l0 && std::memcmp(o0, o1, sizeof o0) == 0 && d1.err_op == d0.err_op, 1u);
}
static void q5(const char *ahhp, const char *xcp)
{
    const std::string s = slurp(ahhp), x = slurp(xcp);
    expect_u("Q5 the sources were read", !s.empty() && !x.empty(), 1u);
    expect_u("Q5 switch 97: OFF at boot, only the verb writes it, through the guarded n48_ra_set",
             count(s, "static volatile uint32_t gTc97On { 0u };") == 1u && count(s, "__atomic_store_n(&gTc97On,") == 1u &&
             count(s, "gTc97On =") == 0u &&
             count(s, "const bool contRefused97 = n48_cm_cont_switch_refused(97u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;") == 1u &&
             count(s, "else { changed97 = n48_ra_set(m, &f97); if (changed97) __atomic_store_n(&gTc97On, f97, __ATOMIC_RELEASE); }") == 1u, 1u);
    const std::string pol = body_of(s, "static void gfxsrc_policy(");
    const size_t pRead = pol.find("    const uint32_t tcOn = gTc97On ? 1u : 0u;");
    const size_t pSet = pol.find("        if (tcOn) { ex.flags |= XLAT12_EXTRA_TBLCACHE; gTc97S.segsOn++; }");
    const size_t pX = pol.find("        uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from], to - from,");
    expect_u("Q5 read ONCE per pass, set on the segment's `ex` before its translate, the only XLAT12_EXTRA_TBLCACHE in the kext",
             pRead != std::string::npos && pSet != std::string::npos && pX != std::string::npos && pRead < pSet && pSet < pX &&
             count(pol, "gTc97On") == 1u && count(s, "ex.flags |= XLAT12_EXTRA_TBLCACHE") == 1u && count(s, "|= XLAT12_EXTRA_TBLCACHE") == 1u &&
             count(s, "gTc97On ?") == 2u /* the pass read and the report line */, 1u);
    const std::string tr = body_of(x, "uint32_t xlat12_ib_translate_draw_ex(");
    expect_u("Q5 the translation asks d_verify with its own ex's TBLCACHE bit (once)",
             count(x, "(ex && (ex->flags & XLAT12_EXTRA_TBLCACHE)) ? 1u : 0u,") == 1u, 1u);
    const std::string dv = body_of(x, "static uint32_t d_verify(const uint32_t *out, uint32_t n, uint32_t fill, uint32_t tc, uint32_t *bad_addr, uint32_t *bad_op)\n{");
    expect_u("Q5 d_verify: OFF asks d_table_g12 itself; ON its cache; nothing else changed",
             count(dv, "!(d_policy_g12(a) || (tc ? d_table_g12_tc(a) : d_table_g12(a)))") == 1u && count(dv, "d_table_g12") == 2u, 1u);
    expect_u("Q5 the public xlat12_ib_draw_verify stays OFF",
             count(x, "    return d_verify(out, n, 0u, 0u, bad_addr, bad_op);") == 1u, 1u);
    const std::string bd = body_of(x, "static void d_tc_build(void)\n{");
    expect_u("Q5 the build: one claimant by CAS; published by a release store; the table outgrowing the cap never publishes a partial cache",
             count(bd, "if (!__atomic_compare_exchange_n(&d_tc_state, &want, 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;") == 1u &&
             count(bd, "__atomic_store_n(&d_tc_state, 2u, __ATOMIC_RELEASE);") == 1u &&
             count(bd, "if (nc >= XLAT12_TC_CAP) { __atomic_store_n(&d_tc_state, 3u, __ATOMIC_RELEASE); return; }") == 1u, 1u);
    char b[1024];
    const char *hows[] = { " - `gfxneuter 97` REFUSED - a continuous arm stands, unchanged", " - `gfxneuter 97` REFUSED (unknown M), unchanged" };
    uint32_t ok = 1u;
    for (const char *h : hows) {
        int n = std::snprintf(b, sizeof b, N48_TC97_FMT, N48_TC97_ON_TXT, h, ~0ull, 4294967295u,
                              "NOT BUILT (the table outgrew XLAT12_TC_CAP: every ask walks)", -2147483647 - 1, -2147483647 - 1);
        if (n <= 0 || (unsigned)n > N48_LOG_CAP_BODY) ok = 0u;
        n = std::snprintf(b, sizeof b, N48_TC97_FMT, N48_TC97_OFF_TXT, h, ~0ull, 4294967295u,
                          "NOT BUILT (the table outgrew XLAT12_TC_CAP: every ask walks)", -2147483647 - 1, -2147483647 - 1);
        if (n <= 0 || (unsigned)n > N48_LOG_CAP_BODY) ok = 0u;
    }
    expect_u("Q5 the tblcache97 line fits 491 bytes at maximal fields", ok, 1u);
    (void)tr;
}

int main(int argc, char **argv)
{
    q1();   // FIRST: the cache must not exist yet
    q2(); q3(); q4(); q6();
    if (argc >= 3) q5(argv[1], argv[2]);
    else expect_u("the source files were given (AHH, xlat12_ib.c)", 0u, 1u);
    std::printf("%d run, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
