// gfx_admit112_test.cpp - build 0.0.554 (; an internal design note): SWITCH 112 "admit112", ADMIT STALE, offline.
//
// THE PROPERTY. A tiled, NOT-DCC, table-path texture read of an allow-listed program that every proof ask refused is admitted ONLY when its
// surface was demonstrably written earlier this arm (the EVER ledger: a committed frame's colour target or a RECORDED residency copy, matched
// on context, VA, mode and base page) - and never otherwise. SHADOW counts the same question and changes nothing. Everything after the ask
// (the dependency gate's R1-R5', the copy guard) still runs on an admitted input.
//
// WHAT RUNS. The REAL gfx_admit112.h (the ledger, the decision, n48_ad_ask - the function the kext's callback calls), the REAL translator
// (src/xlat12/xlat12.c + xlat12_ib.c) for the end-to-end draws, and the REAL n48_cp_build_consumer / n48_cp_eval_hz for the dependency gate.
// The records are the spec's own (run11aw frame 862 segment 9's AN T# at 0x4021f8000; run11at frame 29's S T# at 0x402180000; run11aw's
// r3hit f16 pointer 0x4053d0110 on page 0x10030000; GPUPass at 0x403100000). Where the page or the context is not in a log it is a stated
// constant of this test. The translator half against stub callbacks is src/xlat12/tests/test_xlat12_ib.c (section test_admit554).
//   T1  AN once written: ON admits (R1's condition is met), SHADOW returns 0 and counts it under AN (identity 62)
//   T2  S never written: refused, in both feed forms; a committed row on the same page under a DIFFERENT VA refuses; unrecorded copies feed nothing
//   T3  the address class stays refused: with every image admitted the gate still answers R3 (a hazard page) and R2 (an unresolved page)
//   T4  a DCC record is never offered (counted apart)   T5  the callback's reach (source pins over the translator)
//   T6  OFF is inert   T7  SHADOW output equals OFF end to end   T8  an unmap drops the EVER entry (and every other drop)
//   T9  a page mismatch refuses   T10 an off-list program refuses (GPUPass f15, 0x403100000)   T11 the verb exclusion, both directions
//   T12 a two-texture draw whose second texture was never written is refused
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 -I src/xlat12/tests -x c++ src/navi48-bringup/tests/gfx_admit112_test.cpp \
//         src/xlat12/xlat12.c src/xlat12/xlat12_ib.c -o /tmp/admit112 && /tmp/admit112 src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/xlat12/xlat12_ib.c
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include "gfx_admit112.h"
#include "gfx_dep.h"
#include "gfx_cp_build.h"
#include "gfx_commit.h"
#include "xlat12.h"
#include "xlat12_ib.h"
#include "xlat12_desc.h"   // xlat12_format_elem_bytes

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-96s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-96s %#llx\n", what, (unsigned long long)got);
}
static void expect(const char *what, bool ok) { expect_u(what, ok ? 1u : 0u, 1u); }

// ---- the spec's own records ----------------------------------------------------------------------------------------------------------
// T1: run11aw frame 862 segment 9, AN reading VA 0x4021f8000: 64x64, format 71, SW27, one level, not DCC.
static const uint32_t kAn[8] = { 0x04021f80u, 0xc4700000u, 0x800fc00fu, 0x91b00facu, 0x00000000u, 0x00400000u, 0x00000200u, 0x00000000u };
// T2: run11at frame 29 segment 0, S reading VA 0x402180000 (the log prints the first six dwords; the rest are zero).
static const uint32_t kS[8]  = { 0x04021800u, 0x83200000u, 0x80254063u, 0x99b00f2eu, 0x00000000u, 0x00400000u, 0x00000000u, 0x00000000u };
static const uint64_t kAnVa = 0x4021f8000ull, kSVa = 0x402180000ull, kGpuPassVa = 0x403100000ull;
static const uint64_t kIdAN = (122ull << 32) | 0x7b3a6dfeull, kIdX = (118ull << 32) | 0xc5e80d66ull, kIdBD = (1271ull << 32) | 0x3858ea3aull;
static const uint64_t kIdGPUPass = (83ull << 32) | 0xd3d36bb9ull, kIdS = (85ull << 32) | 0xd0a62abeull, kIdP = (46ull << 32) | 0xebaa377cull;
static const uint64_t kCtx = 5ull;                 // WindowServer's context key (the run's create #5), a stated constant here
static const uint64_t kArm = 0x1234567890ull;      // one arm's armed_at_us, a stated constant here

static void rec_facts(const uint32_t *rec, uint32_t *mode, uint32_t *elem, uint32_t *dcc)
{
    uint32_t g[8], d = 0;
    const uint32_t st = xlat12_table_img_desc(rec, g, &d);
    *mode = st == 0u ? ((g[3] >> 20) & 0x1Fu) : 0xFFu;
    *elem = xlat12_format_elem_bytes((rec[1] >> 20) & 0x1FFu);
    *dcc = xlat12_desc_has_dcc(rec) ? 1u : 0u;
}

// A fresh state: one ledger (static, ~30 KB), counters, the frame, the window.
struct St {
    n48_ever ev; n48_ad_ctr ctr; n48_ad_frame fr; n48_ad_win win; uint32_t frameAdm;
    n48_ad_state S() { return n48_ad_state { &ev, &ctr, &fr, &win, &frameAdm }; }
};
static St *newSt(uint64_t arm)
{
    St *s = new St;
    n48_ad_reset(&s->ev); s->ctr = n48_ad_ctr {}; s->fr = n48_ad_frame {}; s->win = n48_ad_win {}; s->frameAdm = 0u;
    n48_ad_scope(&s->ev, arm);
    return s;
}
// build 0.0.555: the test's stand-in for gfxsrc_rp_walk over the asking frame's VM: va -> page0 + (va - va0), with an optional page that
// no longer resolves (unresVa) and an optional page that Apple re-mapped elsewhere (movedVa). One TVm per mkq (a ring: q.vm points into it).
struct TVm { uint64_t va0, page0, movedVa, unresVa; };
static int walk_t(const void *vm, uint64_t va, uint64_t *out)
{
    const TVm *t = static_cast<const TVm *>(vm);
    if (!t) return 0;
    if (t->unresVa && (va & ~0xfffull) == t->unresVa) return 0;
    uint64_t p = t->page0 + (va - t->va0);
    if (t->movedVa && (va & ~0xfffull) == t->movedVa) p += 0x100000ull;
    *out = p;
    return 1;
}
static int gWalkCalls = 0;
static int walk_count(const void *vm, uint64_t va, uint64_t *out) { gWalkCalls++; return walk_t(vm, va, out); }
static n48_ad_q mkq(uint64_t id, uint64_t va, uint64_t page, uint32_t mode, uint32_t elem, const uint32_t *rec)
{
    static TVm ring[64]; static uint32_t nxt = 0;
    TVm *t = &ring[nxt++ % 64u];
    *t = TVm { va, page, 0ull, 0ull };
    n48_ad_q q {};
    q.ps_id = id; q.ctx = kCtx; q.va = va; q.page = page; q.pageOk = page ? 1u : 0u; q.mode = mode; q.elemBytes = elem; q.rec = rec;
    q.walk = &walk_t; q.vm = t;
    return q;
}
static TVm *vmof(n48_ad_q &q) { return const_cast<TVm *>(static_cast<const TVm *>(q.vm)); }
static uint32_t ask(St *s, uint32_t mode, uint64_t arm, const n48_ad_q &q, uint32_t *clamp = nullptr, n48_ad_r *rOut = nullptr)
{
    n48_ad_r r {}; uint32_t lk = 0, ln = 0, cl = 0;
    const n48_ad_state S = s->S();
    const uint32_t y = n48_ad_ask(&S, mode, arm, &q, 0u, &cl, &r, &lk, &ln);
    if (clamp) *clamp = cl;
    if (rOut) *rOut = r;
    return y;
}

// Sequenced combinations (never two side-effecting operands in one expression): the verdict beside the answer, and a src / clamp readout.
static uint32_t askv(St *s, uint32_t mode, uint64_t arm, const n48_ad_q &q, n48_ad_r *rOut = nullptr)
{
    n48_ad_r r {};
    const uint32_t y = ask(s, mode, arm, q, nullptr, &r);
    if (rOut) *rOut = r;
    return y * 10u + r.verdict;
}
static uint32_t askc(St *s, uint32_t mode, uint64_t arm, const n48_ad_q &q, uint32_t *src)
{
    n48_ad_r r {}; uint32_t cl = 9u;
    const uint32_t y = ask(s, mode, arm, q, &cl, &r);
    *src = r.src;
    return y * 10u + cl;
}

// ---- the end-to-end harness: the REAL translator, a BD (two textures, no sampler) draw, the kext's callback logic ------------------------
#define H_TABLE 0x4000c0000ull
#define H_IMG   0x4000b0000ull
#define H_SAMP  0x400038000ull
#define H_IBVA  0x4000d16c8ull
static const uint32_t kHTable[8] = { 0x000b0000u, 4u, 0x00008000u, 0x1104bfacu, 0x00038000u, 4u, 0x00008000u, 0x1104bfacu };
static struct { uint32_t tex[8][8], samp[16][4]; } gH;
static int h_read(void *, uint64_t va, uint32_t ndw, uint32_t *out)
{
    const uint32_t *src = nullptr;
    if (va == H_TABLE && ndw == 2u) src = &kHTable[0];
    else if (va == H_TABLE + 0x10u && ndw == 2u) src = &kHTable[4];
    else if (va >= H_IMG && va < H_IMG + 8u * 32u && !((va - H_IMG) & 31u) && ndw == 8u) src = gH.tex[(va - H_IMG) / 32u];
    else if (va >= H_SAMP && va < H_SAMP + 16u * 16u && !((va - H_SAMP) & 15u) && ndw == 4u) src = gH.samp[(va - H_SAMP) / 16u];
    if (!src) return 0;
    for (uint32_t k = 0; k < ndw; k++) out[k] = src[k];
    return 1;
}
static int h_never(void *, uint64_t, uint32_t, uint32_t) { return 0; }   // every proof ask says no
static uint32_t gRowBD, gRowGP;
static int h_resolver(void *, uint32_t stage, uint64_t va, xlat12_draw_profile *out)
{
    if (stage == 0u && va == 0x400700000ull) out->ps_table_abi1 = gRowBD;
    if (stage == 0u && va == 0x400710000ull) out->ps_table_abi1 = gRowGP;
    if (stage == 1u && va == 0x400070000ull) out->vs_abi_ptr1 = 3u;   // ViewportToNDC's ABI-pointer row (kXlat12AbiPtrs[2]), as the translator tests hold it
    return 1;
}
struct Bld { uint32_t *in; uint32_t k; };
static void b_slack(Bld *b, uint32_t q) { while (q--) { b->in[b->k++] = 0xC0016900u; b->in[b->k++] = (0x28c8cu - 0x28000u) >> 2; b->in[b->k++] = 0u; } }
static void b_pgm(Bld *b, uint32_t lo) { b->in[b->k++] = 0xC0047600u; b->in[b->k++] = (0xb020u >> 2) - 0x2c00u; b->in[b->k++] = lo; b->in[b->k++] = 0u;
                                         b->in[b->k++] = 0x020F0000u; b->in[b->k++] = 0x00000020u; }
static void b_ud(Bld *b, const uint32_t *v, uint32_t nv) { b->in[b->k++] = 0xC0007600u | (nv << 16); b->in[b->k++] = (0xb030u >> 2) - 0x2c00u;
                                                           for (uint32_t q = 0; q < nv; q++) b->in[b->k++] = v[q]; }
static void b_vpgm(Bld *b, uint32_t lo) { b->in[b->k++] = 0xC0047600u; b->in[b->k++] = (0xb120u >> 2) - 0x2c00u; b->in[b->k++] = lo; b->in[b->k++] = 0u;
                                          b->in[b->k++] = 0x020F0000u; b->in[b->k++] = 0u; }
static void b_vud(Bld *b, const uint32_t *v, uint32_t nv) { b->in[b->k++] = 0xC0007600u | (nv << 16); b->in[b->k++] = (0xb130u >> 2) - 0x2c00u;
                                                            for (uint32_t q = 0; q < nv; q++) b->in[b->k++] = v[q]; }
static uint32_t b_draw(Bld *b) { const uint32_t at = b->k; b->in[b->k++] = 0xC0012D00u; b->in[b->k++] = 3u; b->in[b->k++] = 2u; return at; }
static const uint32_t kSamp[4] = { 0x000080b6u, 0x06fff000u, 0x20500000u, 0u };   // wsgc1's sampler 8 (the translator tests' kT6Samp)
static uint64_t va_of(const uint32_t *r) { return ((uint64_t)(r[1] & 0xFFu) << 40) | ((uint64_t)r[0] << 8); }

// The kext's callback, minus the page walk and the log: it builds the same n48_ad_q and calls the SAME n48_ad_ask.
struct Wrap {
    St *st; uint32_t mode; uint64_t arm; uint64_t (*pageOf)(uint64_t); uint32_t calls, verdictLast; uint64_t idLast;
};
static int wrap_cb(void *ctx, uint64_t ps, uint32_t, const uint32_t *rec, uint64_t va, uint32_t mode, uint32_t elem, uint32_t *clamp)
{
    Wrap *w = static_cast<Wrap *>(ctx);
    w->calls++;
    n48_ad_q q = mkq(ps, va, w->pageOf ? w->pageOf(va) : 0ull, mode, elem, rec);
    n48_ad_r r {};
    const uint32_t y = ask(w->st, w->mode, w->arm, q, clamp, &r);
    w->verdictLast = r.verdict; w->idLast = ps;
    return (int)y;
}
static uint64_t page_of_default(uint64_t va) { return 0x30000000ull + ((va >> 12) & 0xFFFFFull) * 0x1000ull; }   // a stand-in walk: unique, 4 KiB aligned

// A BD draw: source T# (s4, heap index 7) and sdf T# (s6, index 2), the u and edr_scale pages, ViewportToNDC as the vertex program.
struct Run { uint32_t st, len; xlat12_draw_stats ds; uint32_t out[2048]; uint32_t n, d0; };
static void run_bd(Run *r, xlat12_draw_extra ex, const uint32_t *rA, const uint32_t *rB, uint64_t p1)
{
    static uint32_t in[2048];
    memset(&gH, 0, sizeof gH);
    memcpy(gH.tex[7], rA, 32); memcpy(gH.tex[2], rB, 32);
    ex.pgm_profile = &h_resolver; ex.flags = XLAT12_EXTRA_TABLE_DESC; ex.ib_va = H_IBVA; ex.desc_read = &h_read; ex.desc_tiled_ok = &h_never;
    const uint32_t ud12[12] = { (uint32_t)H_TABLE, (uint32_t)(H_TABLE >> 32), 0u, 0u, 7u, 0u, 2u, 0u,
                                (uint32_t)p1, (uint32_t)(p1 >> 32), (uint32_t)0x400052000ull, (uint32_t)(0x400052000ull >> 32) };
    const uint32_t vud10[10] = { 0u, 0u, 0xffffffffu, 0xffffffffu, 0x000c0070u, 4u, 0x000c00a0u, 4u, 0x000c00d0u, 4u };
    Bld b { in, 0u };
    b_slack(&b, 24u); b_vpgm(&b, 0x04000700u); b_vud(&b, vud10, 10u); b_pgm(&b, 0x04007000u); b_ud(&b, ud12, 12u);
    r->d0 = b_draw(&b); r->n = b.k;
    memset(r->out, 0xA5, sizeof r->out);
    r->len = 0u;
    r->st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, in, r->n, r->out, &r->len, &r->ds);
}

// ---- the source pins ------------------------------------------------------------------------------------------------------------------
static std::string slurp(const char *path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
static size_t cnt(const std::string &s, const std::string &needle)
{
    size_t n = 0, at = 0;
    if (needle.empty()) return 0;
    while ((at = s.find(needle, at)) != std::string::npos) { n++; at += needle.size(); }
    return n;
}
static std::string between(const std::string &s, const std::string &from, const std::string &to)
{
    const size_t a = s.find(from);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find(to, a + from.size());
    return b == std::string::npos ? std::string() : s.substr(a, b - a);
}

// =====================================================================================================================================
static void t_widths()
{
    const unsigned long long M = ~0ull; const uint32_t U = 0xFFFFFFFFu;
    char b[4096];
    const char *how = " - `gfxneuter 112` REFUSED - switch 103 is ON (112 ON and 103 ON refuse each other), unchanged";
    const char *wired = "INERT (10, 11, 18, 21, 45 must all be ON)";
    const int w1 = std::snprintf(b, sizeof b, N48_AD_FMT, "OFF (default)", how, "OFF", "OFF", "OFF", "OFF", "OFF", "not ON", wired);
    const int w1b = std::snprintf(b, sizeof b, N48_AD_EVER_FMT, U, U, M, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U);
    const int w2 = std::snprintf(b, sizeof b, N48_AD_ID3_FMT, "-A", "AN", U, U, U, U, U, U, "AN", U, U, U, U, U, U, "AN", U, U, U, U, U, U);
    const int w3 = std::snprintf(b, sizeof b, N48_AD_MISS_FMT, U, U, U, U, U, U, U, U, U, U, U, U, U);
    const int w4 = std::snprintf(b, sizeof b, N48_AD_EV_FMT, "SHADOW", M, "other", U, M, M, U, U, M, "never-written", " via ", "committed-frame", U, U);
    const int w5 = std::snprintf(b, sizeof b, N48_AD_SEC_FMT, "SHADOW", U, U, U, U, U, U, U, U, U, U, U);
    std::printf("      widths: verb %d + ever %d, ids %d, why %d, event %d, sec %d (cap %u)\n", w1, w1b, w2, w3, w4, w5, N48_LOG_CAP_BODY);
    expect("T0 every line fits the log body at worst-case values", w1 > 0 && w1b > 0 && (unsigned)w1b <= N48_LOG_CAP_BODY && w2 > 0 && w3 > 0 && w4 > 0 && w5 > 0 && (unsigned)w1 <= N48_LOG_CAP_BODY &&
           (unsigned)w2 <= N48_LOG_CAP_BODY && (unsigned)w3 <= N48_LOG_CAP_BODY && (unsigned)w4 <= N48_LOG_CAP_BODY && (unsigned)w5 <= N48_LOG_CAP_BODY);
}

static void t_modes_and_allowlist()
{
    expect("T0 the values: ON 368, OFF 624, SHADOW 880; M 0 and M 4 refused; OFF is 0 (the boot value)",
           (112u | 1u << 8) == 368u && (112u | 2u << 8) == 624u && (112u | 3u << 8) == 880u && n48_ad_mode_of_m(1u) == N48_AD_ON &&
           n48_ad_mode_of_m(2u) == N48_AD_OFF && n48_ad_mode_of_m(3u) == N48_AD_SHADOW && n48_ad_mode_of_m(0u) == N48_AD_MODES &&
           n48_ad_mode_of_m(4u) == N48_AD_MODES && N48_AD_OFF == 0u);
    // preconditions: only 10 && 11 && 18 && 21 && 45 (all 32 combinations)
    uint32_t okAll = 1u;
    for (uint32_t m = 0; m < 32u; m++) {
        const uint32_t p = n48_ad_preconds(m & 1u, (m >> 1) & 1u, (m >> 2) & 1u, (m >> 3) & 1u, (m >> 4) & 1u);
        if (p != (m == 31u ? 1u : 0u)) okAll = 0u;
        for (uint32_t mode = 0; mode < 3u; mode++)
            for (uint32_t on103 = 0; on103 < 2u; on103++) {
                const uint32_t wire = n48_ad_wire(mode, p, on103), feed = n48_ad_feeding(mode, p);
                if (wire != ((mode != N48_AD_OFF && p && !on103) ? 1u : 0u)) okAll = 0u;
                if (feed != ((mode != N48_AD_OFF && p) ? 1u : 0u)) okAll = 0u;
            }
    }
    expect("T0 INERT unless 10, 11, 18, 21 and 45 are all ON; wired only when not OFF, preconditions hold and 103 is not ON (all combinations)", okAll);
    // the allow-list: the six programs, each a shipped kDTableAbi row
    const struct { uint32_t ndw, fnv, ident; const char *name; } want[6] = {
        { 118u, 0xc5e80d66u, 93u, "X" }, { 122u, 0x7b3a6dfeu, 62u, "AN" }, { 192u, 0x92c6ae13u, 90u, "U" },
        { 60u, 0xd53dee91u, 77u, "BC" }, { 173u, 0x1051f3f6u, 54u, "AF" }, { 1271u, 0x3858ea3au, 78u, "BD" } };
    uint32_t listOk = 1u;
    for (uint32_t k = 0; k < 6u; k++) {
        const uint64_t id = ((uint64_t)want[k].ndw << 32) | want[k].fnv;
        if (n48_ad_id_index(id) != k || strcmp(n48_ad_id_name(k), want[k].name) != 0 || kN48AdIds[k].ident != want[k].ident) listOk = 0u;
        if (xlat12_table_abi_find(want[k].ndw, want[k].fnv) == 0u) listOk = 0u;   // a shipped table row
    }
    expect("T10 the allow-list is exactly X {118, 0xc5e80d66} AN {122, 0x7b3a6dfe} U {192, 0x92c6ae13} BC {60, 0xd53dee91} AF {173, 0x1051f3f6} BD {1271, 0x3858ea3a}", listOk && N48_AD_IDS == 6u);
    expect("T10 GPUPass, S, P and an unknown identity are NOT on it",
           n48_ad_id_index(kIdGPUPass) == N48_AD_IDS && n48_ad_id_index(kIdS) == N48_AD_IDS && n48_ad_id_index(kIdP) == N48_AD_IDS &&
           n48_ad_id_index(0ull) == N48_AD_IDS && !strcmp(n48_ad_id_name(N48_AD_IDS), "other"));
}

// T1
static void t1()
{
    uint32_t mode = 0, elem = 0, dcc = 0;
    rec_facts(kAn, &mode, &elem, &dcc);
    expect("T1 the record: tiled (gfx12 mode 3), format 71 has a bytes-per-element, not DCC, VA 0x4021f8000", mode == 3u && elem != 0u && !dcc && va_of(kAn) == kAnVa);
    St *s = newSt(kArm);
    // AN committed with this texture at frames 480/484/488: the committed-frame feed, three times (a re-feed refreshes the same key)
    const uint64_t page = 0x2a0000000ull;   // the page is not in the log: a stated constant, 4 KiB aligned
    for (uint32_t k = 0; k < 3u; k++) expect_u("T1 the committed-frame feed stores it (a re-feed refreshes)", n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 480u + 4u * k), 1u);
    expect_u("T1 one entry, two refreshes", s->ev.n * 10u + (uint32_t)s->ev.refreshed, 1u * 10u + 2u);
    uint32_t clamp = 9;
    n48_ad_q q = mkq(kIdAN, kAnVa, page, mode, elem, kAn);
    n48_ad_r r {};
    expect_u("T1 ON admits (n48_ad_ask answers 1)", ask(s, N48_AD_ON, kArm, q, &clamp, &r), 1u);
    expect_u("T1 ... by the committed-frame entry, no clamp, identity AN (index 1, identity number 62)",
             (r.verdict == N48_AD_V_ADMIT ? 1u : 0u) + (r.src == N48_AD_SRC_LEDGER ? 2u : 0u) + (clamp == 0u ? 4u : 0u) + (r.idIx == 1u ? 8u : 0u) + (kN48AdIds[r.idIx].ident == 62u ? 16u : 0u), 31u);
    expect_u("T1 counted under AN would-admit", s->ctr.v[1][N48_AD_V_ADMIT] * 100u + (uint32_t)s->ctr.admitted * 10u + (uint32_t)s->frameAdm, 111u);
    // the R1 condition of the dependency rule, on the export the translator would give (in_mode 3, in_proven 1): met
    n48_cp_consumer c {}; c.enumerated = 1u; c.n = 1u; c.va[0] = kAnVa; c.mode[0] = mode; c.proven[0] = 1u; c.resolved[0] = 1u;
    uint64_t un = 0, st = 0; n48_cp_ring ring {}; n48_dep_witness wt {}; n48_r5_ring r5 {}; n48_r5_scope(&r5, 7u);
    expect_u("T1 R1 passes for the admitted input (the rule reaches the clean answer)", n48_cp_eval_hz(&c, &ring, &wt, &r5, &un, &st), N48_CP_OK);
    // SHADOW: refuses (answer 0), counts under AN, and moves nothing else
    St *h = newSt(kArm);
    n48_ad_feed_ledger(&h->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 480u);
    expect_u("T1 SHADOW answers 0 (the 0xf7 refusal stands)", ask(h, N48_AD_SHADOW, kArm, q, &clamp, &r), 0u);
    expect_u("T1 ... and counts the would-admit under AN, sets no clamp, admits nothing, no frame admission",
             h->ctr.v[1][N48_AD_V_ADMIT] * 1000u + (uint32_t)h->ctr.admitted * 100u + (uint32_t)h->frameAdm * 10u + clamp, 1100u);
    expect_u("T1 the SHADOW would-admit is a would-admit in the verdict, not an admission", r.verdict, N48_AD_V_ADMIT);
    // OFF: the ask does nothing at all
    St *o = newSt(kArm);
    n48_ad_feed_ledger(&o->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 480u);
    { const uint32_t y = ask(o, N48_AD_OFF, kArm, q, &clamp, &r);
      expect_u("T6 OFF answers 0 and counts nothing", y + o->ctr.asks + o->fr.asked + o->frameAdm, 0u); }
    delete s; delete h; delete o;
}

// T2
static void t2()
{
    uint32_t mode = 0, elem = 0, dcc = 0;
    rec_facts(kS, &mode, &elem, &dcc);
    std::printf("      S's record: gfx12 mode %#x, bytes per element %u (format %u), dcc %u\n", mode, elem, (kS[1] >> 20) & 0x1FFu, dcc);
    expect("T2 S's record is tiled (gfx12 mode 3) and not DCC; its format 50 has NO bytes-per-element in the translator's table (0)",
           mode == 3u && elem == 0u && !dcc && va_of(kS) == kSVa);
    St *s = newSt(kArm);
    const uint64_t page = 0x2b0000000ull;
    // no committed writer; copies #303 and #596 landed at the VA UNRECORDED (reason 2): they feed nothing
    n48_rp_copy c {}; c.copied = 1u; c.ctx = kCtx; c.va = kSVa; c.mode = mode; c.vram = page; c.bytes = 0x4000ull; c.elemBytes = elem;
    { const uint32_t f = n48_ad_feed_copy(&s->ev, kArm, &c, N48_RP_REC_UNVERIFIED);
      expect_u("T2 an UNRECORDED copy (reason 2, N48_RP_REC_UNVERIFIED) feeds nothing", f + s->ev.n, 0u); }
    { const uint32_t f1 = n48_ad_feed_copy(&s->ev, kArm, &c, N48_RP_REC_NOT_WS), f2 = n48_ad_feed_copy(&s->ev, kArm, &c, N48_RP_REC_FULL),
                     f3 = n48_ad_feed_copy(&s->ev, kArm, &c, N48_RP_REC_MODE), f4 = n48_ad_feed_copy(&s->ev, kArm, &c, N48_RP_REC_NO_CTX);
      expect_u("T2 ... nor does any other reason (NOT_WS, FULL, MODE, NO_CTX)", f1 + f2 + f3 + f4 + s->ev.n, 0u); }
    expect_u("T2 ... counted as unrecorded (five)", s->ev.unrecorded, 5u);
    // a committed row on the SAME PAGE under a DIFFERENT VA (the case the 103-row form would have taken for a witness)
    n48_ad_feed_ledger(&s->ev, kArm, kCtx, 0x4053d0000ull, mode, page, 0x4000ull, 2u);
    n48_ad_q q = mkq(kIdX, kSVa, page, mode, elem, kS);
    expect_u("T2 an on-list program reading S's VA: refused NEVER-WRITTEN (no entry for this VA), even with a committed row on its page under another VA",
             askv(s, N48_AD_ON, kArm, q), 0u * 10u + N48_AD_V_NEVER);
    // S itself is off the list
    n48_ad_q qs = mkq(kIdS, kSVa, page, mode, elem, kS);
    expect_u("T2 S's own program is off the list: NOT-ROW", askv(s, N48_AD_ON, kArm, qs), 0u * 10u + N48_AD_V_NOROW);
    // and once a RECORDED copy lands there (the other feed form) the element-size clause still speaks
    expect_u("T2 a RECORDED copy at the VA feeds the ledger", n48_ad_feed_copy(&s->ev, kArm, &c, N48_RP_REC_OK), 1u);
    { n48_ad_r rr {};
      const uint32_t v = askv(s, N48_AD_ON, kArm, q, &rr);
      expect_u("T2 ... and a read of it is STILL refused, as SHAPE: a copy's element size must be non-zero and S's format 50 has none", v, 0u * 10u + N48_AD_V_SHAPE); }
    // the ledger's VA never reached before an unmap: the unmap drops it again (later unmapped, log line 28819)
    { const uint32_t d = n48_ad_unmap_rng(&s->ev, kCtx, 0x402100000ull, 0x100000ull);
      n48_ad_r rr {};
      const uint32_t v = askv(s, N48_AD_ON, kArm, q, &rr);
      expect_u("T2 the VA is later unmapped: the entry drops and the read is refused again (never-written)", d * 100u + v, 1u * 100u + 0u * 10u + N48_AD_V_NEVER); }
    delete s;
}

// T3: the address class stays refused (through the REAL translator, the REAL consumer builder and the REAL rule)
static uint64_t g_unres_va;
static uint64_t t3_page(uint64_t va) { return va == g_unres_va ? 0ull : (((va & ~0xfffull) == 0x4053d0000ull) ? 0x10030000ull : page_of_default(va)); }
static void t3()
{
    uint32_t mode = 0, elem = 0, dcc = 0;
    rec_facts(kAn, &mode, &elem, &dcc);
    uint32_t rA[8], rB[8];
    memcpy(rA, kAn, 32); memcpy(rB, kAn, 32); rB[0] = 0x04022000u;   // two tiled records, two VAs (the run's AN VA and its neighbour)
    St *s = newSt(kArm);
    n48_ad_feed_ledger(&s->ev, kArm, kCtx, va_of(rA), mode, page_of_default(va_of(rA)), 0x4000ull, 480u);
    n48_ad_feed_ledger(&s->ev, kArm, kCtx, va_of(rB), mode, page_of_default(va_of(rB)), 0x4000ull, 481u);
    Wrap w { s, N48_AD_ON, kArm, &t3_page, 0u, 0u, 0ull };
    xlat12_draw_extra ex; memset(&ex, 0, sizeof ex); ex.desc_ctx = &w; ex.desc_stale_ok = &wrap_cb;
    gRowBD = xlat12_table_abi_find(1271u, 0x3858ea3au);
    static Run run;
    run_bd(&run, ex, rA, rB, 0x4053d0110ull);   // BD's `u` pointer = r3hit f16's VA
    expect("T3 the draw translates with BOTH images admitted (BD, two textures), the export says so",
           run.st == 0u && run.ds.in_n == 2u && run.ds.in_proven[0] == 1u && run.ds.in_proven[1] == 1u && run.ds.in_admit == 3u && run.ds.stale_admitted == 2u && w.calls == 2u);
    n48_cp_consumer c {};
    n48_cp_build_consumer(&c, &run.ds);
    expect("T3 the consumer built from the export is COMPLETE and carries both inputs proven (R1's condition met)",
           c.enumerated == 1u && !c.over && c.n == 2u && c.mode[0] == 3u && c.mode[1] == 3u && c.proven[0] == 1u && c.proven[1] == 1u);
    // resolve exactly as the kext does (gfxc_page per input and per pointer), through a stand-in map: r3hit's page
    auto resolve = [&](n48_cp_consumer *cc) {
        for (uint32_t q = 0; q < cc->n; q++) { cc->resolved[q] = t3_page(cc->va[q] & ~0xfffull) ? 1u : 0u; }
        for (uint32_t q = 0; q < cc->nptr; q++) { const uint64_t p = t3_page(cc->ptr[q] & ~0xfffull); cc->ptr_resolved[q] = p ? 1u : 0u; cc->ptr_page[q] = p; }
    };
    n48_cp_ring ring {}; n48_dep_witness wt {};
    n48_r5_ring hr {}; n48_r5_scope(&hr, 0x5A5Bu);
    n48_r5_frame x {}; x.ib_ok = 1u; x.walk_ok = 1u; x.targets_ok = 1u; x.memw_ok = 1u; x.tgts_resolved = 1u; x.memw_resolved = 1u; x.ntgt = 1u;
    x.tgt[0].va = 0x4053d0110ull; x.tgt[0].page = 0x10030000ull;
    n48_r5_note(&hr, &x, 2ull);   // frame 2's CB0, an EXACT hazard row (r3hit f16: VA 0x4053d0110, page 0x10030000)
    uint64_t un = 0, st = 0;
    n48_cp_consumer c1 = c; g_unres_va = 0ull; resolve(&c1);
    expect_u("T3 with every image admitted the gate still answers R3-neutered-write-destination for the hazard page",
             n48_cp_eval_hz(&c1, &ring, &wt, &hr, &un, &st), N48_CP_R3_MEMDST);
    n48_r5_ring empty {}; n48_r5_scope(&empty, 0x5A5Cu);
    expect_u("T3 the control: the same consumer against an empty hazard set is CLEAN (the admitted inputs pass R1-R4)",
             n48_cp_eval_hz(&c1, &ring, &wt, &empty, &un, &st), N48_CP_OK);
    n48_cp_consumer c2 = c; g_unres_va = va_of(rA); resolve(&c2);
    expect_u("T3 an admitted input whose base page does not resolve still returns R2-input-page",
             n48_cp_eval_hz(&c2, &ring, &wt, &empty, &un, &st), N48_CP_R2_PAGE);
    expect_u("T3 ... and R2 is asked BEFORE R3 (the unresolved input wins over the hazard)", n48_cp_eval_hz(&c2, &ring, &wt, &hr, &un, &st), N48_CP_R2_PAGE);
    n48_cp_consumer c3 = c; g_unres_va = 0ull; resolve(&c3); c3.proven[1] = 0u;
    expect_u("T3 an input that is NOT admitted still returns R1-tiled-unproven", n48_cp_eval_hz(&c3, &ring, &wt, &empty, &un, &st), N48_CP_R1_TILED);
    g_unres_va = 0ull;
    delete s;
}

// T4 / T5 / T6 / T7 / T12 end to end, and the rest of the ledger
static void t_e2e()
{
    uint32_t mode = 0, elem = 0, dcc = 0;
    rec_facts(kAn, &mode, &elem, &dcc);
    uint32_t rA[8], rB[8];
    memcpy(rA, kAn, 32); memcpy(rB, kAn, 32); rB[0] = 0x04022000u;
    const uint64_t vA = va_of(rA), vB = va_of(rB);
    gRowBD = xlat12_table_abi_find(1271u, 0x3858ea3au);
    gRowGP = xlat12_table_abi_find(83u, 0xd3d36bb9u);
    static Run none, shadow, both, only1;
    // ---- T7: SHADOW's output equals OFF's, end to end ----
    { St *s = newSt(kArm);
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, vA, mode, page_of_default(vA), 0x4000ull, 1u);
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, vB, mode, page_of_default(vB), 0x4000ull, 2u);
      Wrap w { s, N48_AD_SHADOW, kArm, &page_of_default, 0u, 0u, 0ull };
      xlat12_draw_extra ex; memset(&ex, 0, sizeof ex);
      run_bd(&none, ex, rA, rB, 0x400051000ull);
      ex.desc_ctx = &w; ex.desc_stale_ok = &wrap_cb;
      run_bd(&shadow, ex, rA, rB, 0x400051000ull);
      expect("T7 no callback: BD refuses PROVENANCE at its first texture (the OFF baseline)", none.st == XLAT12_IB_ERR_DESC && none.ds.err_op == XLAT12_TDESC_PROVENANCE && none.ds.prov_va == vA);
      expect("T7 SHADOW (both surfaces WERE written, so both would-admit): the same status, error, VA, length and OUTPUT BYTES as OFF",
             shadow.st == none.st && shadow.ds.err_op == none.ds.err_op && shadow.ds.prov_va == none.ds.prov_va && shadow.len == none.len &&
             !memcmp(shadow.out, none.out, sizeof none.out) && shadow.ds.in_proven[0] == 0u && shadow.ds.in_admit == 0u && shadow.ds.stale_admitted == 0u);
      expect_u("T7 ... while the SHADOW counters saw it: one ask, one would-admit under BD (index 5), nothing admitted, no frame admission",
               w.calls * 1000u + s->ctr.v[5][N48_AD_V_ADMIT] * 100u + (uint32_t)s->ctr.admitted * 10u + s->frameAdm, 1u * 1000u + 1u * 100u + 1u * 10u + 0u);
      delete s; }
    // ---- T1/T12: ON, one and two textures ----
    { St *s = newSt(kArm);
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, vA, mode, page_of_default(vA), 0x4000ull, 1u);
      Wrap w { s, N48_AD_ON, kArm, &page_of_default, 0u, 0u, 0ull };
      xlat12_draw_extra ex; memset(&ex, 0, sizeof ex); ex.desc_ctx = &w; ex.desc_stale_ok = &wrap_cb;
      run_bd(&only1, ex, rA, rB, 0x400051000ull);
      expect("T12 the FIRST texture written, the SECOND never: PROVENANCE names the SECOND surface; input 0 admitted, input 1 not",
             only1.st == XLAT12_IB_ERR_DESC && only1.ds.err_op == XLAT12_TDESC_PROVENANCE && only1.ds.prov_va == vB && only1.ds.in_n == 2u &&
             only1.ds.in_proven[0] == 1u && only1.ds.in_proven[1] == 0u && only1.ds.in_admit == 1u && only1.ds.stale_asked == 2u && only1.ds.stale_admitted == 1u);
      expect_u("T12 ... counted: would-admit once and never-written once under BD", s->ctr.v[5][N48_AD_V_ADMIT] * 10u + s->ctr.v[5][N48_AD_V_NEVER], 11u);
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, vB, mode, page_of_default(vB), 0x4000ull, 2u);
      w.calls = 0;
      run_bd(&both, ex, rA, rB, 0x400051000ull);
      expect("T12 once BOTH were written the draw translates, both inputs exported admitted", both.st == 0u && both.ds.in_admit == 3u && both.ds.in_proven[0] == 1u && both.ds.in_proven[1] == 1u);
      delete s; }
    // ---- T9: a page mismatch refuses (the surface was re-mapped to another page) ----
    { St *s = newSt(kArm);
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, vA, mode, page_of_default(vA), 0x4000ull, 1u);
      n48_ad_r r {};
      n48_ad_q q = mkq(kIdBD, vA, page_of_default(vA) + 0x1000ull, mode, elem, rA);
      expect_u("T9 the VA is written but its base page MOVED (+0x1000): refused never-written", askv(s, N48_AD_ON, kArm, q), 0u * 10u + N48_AD_V_NEVER);
      q.page = page_of_default(vA);
      expect_u("T9 ... the same page admits (the control)", askv(s, N48_AD_ON, kArm, q), 1u * 10u + N48_AD_V_ADMIT);
      q.page = 0ull; q.pageOk = 0u;
      expect_u("T9 no resolved page at all: NO-PAGE, refused", askv(s, N48_AD_ON, kArm, q), 0u * 10u + N48_AD_V_NOPAGE);
      q.page = page_of_default(vA); q.pageOk = 1u; q.mode = mode + 1u;
      expect_u("T9 a different mode: refused", ask(s, N48_AD_ON, kArm, q, nullptr, &r), 0u);
      q.mode = mode; q.ctx = kCtx + 1u;
      expect_u("T9 a different context: refused", ask(s, N48_AD_ON, kArm, q, nullptr, &r), 0u);
      q.ctx = kCtx; q.va = vA + 0x100ull;
      expect_u("T9 a different VA on the same page: refused", ask(s, N48_AD_ON, kArm, q, nullptr, &r), 0u);
      q.va = vA; q.page = 0x1234ull;
      expect_u("T9 an unaligned page is no key", ask(s, N48_AD_ON, kArm, q, nullptr, &r), 0u);
      delete s; }
    // ---- T10: an off-list program refuses even for a written surface (GPUPass f15, 0x403100000), end to end ----
    { St *s = newSt(kArm);
      const uint64_t vG = kGpuPassVa;
      uint32_t rG[8]; memcpy(rG, kAn, 32); rG[0] = (uint32_t)(vG >> 8);
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, vG, mode, page_of_default(vG), 0x4000ull, 15u);
      n48_ad_q q = mkq(kIdGPUPass, vG, page_of_default(vG), mode, elem, rG);
      expect_u("T10 GPUPass reading 0x403100000, WRITTEN by a committed frame: NOT-ROW, refused", askv(s, N48_AD_ON, kArm, q), 0u * 10u + N48_AD_V_NOROW);
      expect_u("T10 ... counted under the off-list row", s->ctr.v[N48_AD_IDS][N48_AD_V_NOROW], 1u);
      // and through the real translator: GPUPass's table draw (F6's user data) asks the callback once and stays refused
      static uint32_t in[2048]; static Run gp;
      memset(&gH, 0, sizeof gH); memcpy(gH.tex[1], rG, 32); memcpy(gH.tex[4], kAn, 32); memcpy(gH.samp[8], kSamp, 16);
      Wrap w { s, N48_AD_ON, kArm, &page_of_default, 0u, 0u, 0ull };
      xlat12_draw_extra ex; memset(&ex, 0, sizeof ex);
      ex.pgm_profile = &h_resolver; ex.flags = XLAT12_EXTRA_TABLE_DESC; ex.ib_va = H_IBVA; ex.desc_read = &h_read; ex.desc_tiled_ok = &h_never;
      ex.desc_ctx = &w; ex.desc_stale_ok = &wrap_cb;
      const uint32_t ud6[10] = { (uint32_t)H_TABLE, (uint32_t)(H_TABLE >> 32), 0x000c0110u, 4u, 1u, 0u, 4u, 0u, 8u, 0u };
      Bld b { in, 0u }; b_slack(&b, 14u); b_pgm(&b, 0x04007100u); b_ud(&b, ud6, 10u); (void)b_draw(&b);
      memset(gp.out, 0, sizeof gp.out); gp.len = 0;
      gp.st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, in, b.k, gp.out, &gp.len, &gp.ds);
      expect("T10 end to end: GPUPass's table draw refuses PROVENANCE, the callback asked once and answered 0, nothing admitted",
             gp.st == XLAT12_IB_ERR_DESC && gp.ds.err_op == XLAT12_TDESC_PROVENANCE && w.calls == 1u && gp.ds.stale_admitted == 0u && gp.ds.in_admit == 0u && w.verdictLast == N48_AD_V_NOROW);
      delete s; }
    // ---- T4: a DCC record is never offered; the counters keep it apart ----
    { n48_ad_ctr c {};
      n48_ad_count_dcc(&c, 1u, 3u); n48_ad_count_dcc(&c, N48_AD_IDS + 5u, 2u);
      expect_u("T4 DCC refusals are counted per identity and off-list, never as an ask", c.v[1][N48_AD_V_DCC] * 100u + c.v[N48_AD_IDS][N48_AD_V_DCC] * 10u + c.asks, 3u * 100u + 2u * 10u + 0u); }
}

// T8 and every other drop, the feeds, eviction, scope
static void t_ledger()
{
    uint32_t mode = 0, elem = 0, dcc = 0;
    rec_facts(kAn, &mode, &elem, &dcc);
    const uint64_t page = 0x2a0000000ull;
    n48_ad_r r {};
    { St *s = newSt(kArm);
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 1u);
      n48_ad_q q = mkq(kIdAN, kAnVa, page, mode, elem, kAn);
      expect_u("T8 before the unmap: admitted", ask(s, N48_AD_ON, kArm, q, nullptr, &r), 1u);
      { const uint32_t d = n48_ad_unmap_rng(&s->ev, kCtx + 1u, 0x402000000ull, 0x400000ull); const uint32_t y = ask(s, N48_AD_ON, kArm, q, nullptr, &r);
        expect_u("T8 an unmap of ANOTHER context's identical range leaves it", d * 10u + y, 1u); }
      { const uint32_t d = n48_ad_unmap_rng(&s->ev, kCtx, 0x403000000ull, 0x10000ull); const uint32_t y = ask(s, N48_AD_ON, kArm, q, nullptr, &r);
        expect_u("T8 an unmap of a DISJOINT range (the entry has an extent) leaves it", d * 10u + y, 1u); }
      { const uint32_t d = n48_ad_unmap_rng(&s->ev, kCtx, 0x402100000ull, 0x200000ull); const uint32_t y = ask(s, N48_AD_ON, kArm, q, nullptr, &r);
        expect_u("T8 an unmap COVERING the VA drops it, and the read is refused", d * 10u + y, 10u + 0u); }
      expect_u("T8 ... counted (dropUnmap 1, unmaps 3)", s->ev.dropUnmap * 10u + s->ev.unmaps, 13u);
      delete s; }
    { St *s = newSt(kArm);   // fail closed: no extent + an unmap ABOVE the base drops; scope-unknown (size 0) and a wrapping range drop
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, page, 0ull, 1u);
      expect_u("T8 an entry with NO extent: an unmap that ends at or below its base keeps it", n48_ad_unmap_rng(&s->ev, kCtx, 0x400000000ull, 0x1000ull), 0u);
      expect_u("T8 an entry with NO extent: an unmap above its base drops it (fail closed)", n48_ad_unmap_rng(&s->ev, kCtx, 0x402300000ull, 0x1000ull), 1u);
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 2u);
      expect_u("T8 an unmap of unknown scope (size 0) drops the context's entries", n48_ad_unmap_rng(&s->ev, kCtx, 0ull, 0ull), 1u);
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 3u);
      expect_u("T8 a wrapping range drops", n48_ad_unmap_rng(&s->ev, kCtx, 0xFFFFFFFFFFFFF000ull, 0x2000ull), 1u);
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 4u);
      expect_u("T8 ctx 0 (unknown context) reaches every entry", n48_ad_unmap_rng(&s->ev, 0ull, 0x402100000ull, 0x200000ull), 1u);
      delete s; }
    { St *s = newSt(kArm);   // the WindowServer drop, the rebind sweep, the lost-unmap wipe, the arm scope
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 1u);
      n48_ad_feed_ledger(&s->ev, kArm, kCtx + 1u, kAnVa, mode, page, 0x4000ull, 2u);
      { const uint32_t d = n48_ad_rebind(&s->ev, kCtx); expect_u("rebind: only entries of the bound context stay", d * 10u + s->ev.n, 10u + 1u); }
      { const uint32_t d = n48_ad_rebind(&s->ev, 0ull); expect_u("rebind to none drops everything", d * 10u + s->ev.n, 10u + 0u); }
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 1u);
      { const uint32_t d = n48_ad_ws_gone(&s->ev); expect_u("a WindowServer drop empties it", d * 10u + s->ev.n + s->ev.dropWs, 10u + 0u + 1u); }
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 1u);
      { const uint32_t d = n48_ad_wipe_lost(&s->ev); expect_u("a LOST unmap event wipes the whole ledger", d * 10u + s->ev.n + s->ev.wipeLost, 10u + 0u + 1u); }
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 1u);
      { const uint32_t d = n48_ad_scope(&s->ev, kArm + 1u); expect_u("an arm change empties it (a new armed_at_us)", d * 10u + s->ev.n, 10u + 0u); }
      expect_u("... an unchanged arm empties nothing", n48_ad_scope(&s->ev, kArm + 1u), 0u);
      { const uint32_t f = n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 1u);
        expect_u("... a feed under the OLD arm stores nothing (no-arm counted)", f + s->ev.n + s->ev.noArm, 0u + 0u + 1u); }
      expect_u("... a feed under arm 0 (no arm standing) stores nothing", n48_ad_feed_ledger(&s->ev, 0ull, kCtx, kAnVa, mode, page, 0x4000ull, 1u), 0u);
      n48_ad_q q = mkq(kIdAN, kAnVa, page, mode, elem, kAn);
      expect_u("... and a read under arm 0 is refused", ask(s, N48_AD_ON, 0ull, q, nullptr, &r), 0u);
      delete s; }
    { St *s = newSt(kArm);   // keys: no page, unaligned page, zero VA/mode/ctx are never stored
      { const uint32_t f1 = n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, 0ull, 0x4000ull, 1u), f2 = n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, 0x2a0000010ull, 0x4000ull, 1u),
                       f3 = n48_ad_feed_ledger(&s->ev, kArm, kCtx, 0ull, mode, page, 0x4000ull, 1u), f4 = n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, 0u, page, 0x4000ull, 1u),
                       f5 = n48_ad_feed_ledger(&s->ev, kArm, 0ull, kAnVa, mode, page, 0x4000ull, 1u);
        expect_u("no key is stored: page 0, an unaligned page, VA 0, mode 0, ctx 0", f1 + f2 + f3 + f4 + f5 + s->ev.n, 0u); }
      // eviction: 257 distinct entries -> 256, the oldest gone, the newest present, evicted counted
      for (uint32_t k = 0; k < N48_AD_EVER_MAX + 1u; k++) n48_ad_feed_ledger(&s->ev, kArm, kCtx, 0x400000000ull + 0x10000ull * k, mode, 0x100000000ull + 0x1000ull * k, 0x4000ull, k + 1u);
      n48_ad_q q0 = mkq(kIdAN, 0x400000000ull, 0x100000000ull, mode, elem, kAn);
      n48_ad_q qN = mkq(kIdAN, 0x400000000ull + 0x10000ull * N48_AD_EVER_MAX, 0x100000000ull + 0x1000ull * N48_AD_EVER_MAX, mode, elem, kAn);
      expect_u("eviction: 257 feeds hold 256 entries, one evicted", s->ev.n * 10u + (uint32_t)s->ev.evicted - 2559u * 0u, 2560u + 1u);
      { const uint32_t a = ask(s, N48_AD_ON, kArm, q0, nullptr, &r), b = ask(s, N48_AD_ON, kArm, qN, nullptr, &r);
        expect_u("eviction: the OLDEST entry is gone, the newest is present", a * 10u + b, 0u * 10u + 1u); }
      // a refresh keeps the entry young: re-feed entry #1 (index 1) then overflow again: entry #1 survives, entry #2 goes
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, 0x400000000ull + 0x10000ull * 1u, mode, 0x100000000ull + 0x1000ull * 1u, 0x4000ull, 999u);
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, 0x500000000ull, mode, 0x200000000ull, 0x4000ull, 1000u);
      n48_ad_q q1 = mkq(kIdAN, 0x400000000ull + 0x10000ull * 1u, 0x100000000ull + 0x1000ull * 1u, mode, elem, kAn);
      n48_ad_q q2 = mkq(kIdAN, 0x400000000ull + 0x10000ull * 2u, 0x100000000ull + 0x1000ull * 2u, mode, elem, kAn);
      { const uint32_t a = ask(s, N48_AD_ON, kArm, q1, nullptr, &r), b = ask(s, N48_AD_ON, kArm, q2, nullptr, &r);
        expect_u("eviction: a refreshed entry outlives an older unrefreshed one", a * 10u + b, 1u * 10u + 0u); }
      delete s; }
    { // residency copies: the element size must match and be non-zero; a backing-sourced entry needs its T# and asks for the clamp
      St *s = newSt(kArm);
      n48_rp_copy c {}; c.copied = 1u; c.ctx = kCtx; c.va = kAnVa; c.mode = mode; c.vram = page; c.bytes = 0x4000ull; c.elemBytes = elem;
      expect_u("copy: a RECORDED copy feeds (source COPY)", n48_ad_feed_copy(&s->ev, kArm, &c, N48_RP_REC_OK), 1u);
      n48_ad_q q = mkq(kIdAN, kAnVa, page, mode, elem, kAn);
      uint32_t cl = 9;
      { uint32_t src = 0; const uint32_t v = askc(s, N48_AD_ON, kArm, q, &src);
        expect_u("copy: admitted with the matching element size, no clamp", v * 10u + src, 1u * 100u + 0u * 10u + N48_AD_SRC_COPY); }
      q.elemBytes = elem + 1u;
      expect_u("copy: a different element size is refused as SHAPE", askv(s, N48_AD_ON, kArm, q), 0u * 10u + N48_AD_V_SHAPE);
      q.elemBytes = 0u;
      expect_u("copy: a zero element size is refused as SHAPE", askv(s, N48_AD_ON, kArm, q), 0u * 10u + N48_AD_V_SHAPE);
      // backing-sourced: the T# must match (64x64, type 9, SW27 = g12 mode 3) and the answer asks for the mip-0 clamp
      St *l = newSt(kArm);
      n48_rp_copy lc = c; lc.lin = 1u; lc.w = 64u; lc.h = 64u; lc.retiled = 1u;
      expect_u("lin: a recorded backing-sourced copy feeds (source LIN)", n48_ad_feed_copy(&l->ev, kArm, &lc, N48_RP_REC_OK) + l->ev.fedLin * 10u, 1u + 10u);
      q.elemBytes = elem;
      { uint32_t src = 0; const uint32_t v = askc(l, N48_AD_ON, kArm, q, &src);
        expect_u("lin: the matching T# is admitted and the answer asks for the mip-0 clamp", v * 10u + src, 1u * 100u + 1u * 10u + N48_AD_SRC_LIN); }
      uint32_t bad[8]; memcpy(bad, kAn, 32); bad[2] = (bad[2] & ~0xFFFu) | 0x00Eu;   // width 64 -> 60
      q.rec = bad;
      expect_u("lin: a T# that does not match the entry (a different width) is refused as SHAPE", askv(l, N48_AD_ON, kArm, q), 0u * 10u + N48_AD_V_SHAPE);
      q.rec = nullptr;
      expect_u("lin: no record at all is refused as SHAPE", askv(l, N48_AD_ON, kArm, q), 0u * 10u + N48_AD_V_SHAPE);
      // a committed-frame entry beside a mismatching copy entry still admits (the ledger source needs no element size)
      n48_ad_feed_ledger(&l->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 5u);
      q.rec = bad;
      { n48_ad_r rr {}; const uint32_t y = ask(l, N48_AD_ON, kArm, q, &cl, &rr);
        expect_u("a committed-frame entry for the same key admits even when the backing entry's T# disagrees", y * 10u + (rr.src == N48_AD_SRC_LEDGER ? 1u : 0u), 1u * 10u + 1u); }
      delete s; delete l; }
}

static void t_counters()
{
    // the proof-miss classification
    n48_ad_missf f {};
    expect_u("miss: nothing anywhere -> ledger-none", n48_ad_miss_why(&f), N48_AD_M_LEDGER_NONE);
    f.ledgerHas = 1u; f.ledgerMoved = 1u; expect_u("miss: the ledger holds (ctx, VA, mode) on another page -> ledger-moved", n48_ad_miss_why(&f), N48_AD_M_LEDGER_MOVED);
    f.ledgerMoved = 0u; expect_u("miss: the ledger holds it on the same page (no reason found) -> none", n48_ad_miss_why(&f), N48_AD_M_NONE);
    f = n48_ad_missf {}; f.rpHas = 1u; f.rpEpoch = 1u; expect_u("miss: only the resprov epoch stale -> resprov-epoch", n48_ad_miss_why(&f), N48_AD_M_RP_EPOCH);
    f.rpShape = 1u; expect_u("miss: shape wins over epoch -> resprov-shape", n48_ad_miss_why(&f), N48_AD_M_RP_SHAPE);
    f = n48_ad_missf {}; f.rpHas = 1u; f.rpWalk = 1u; expect_u("miss: the base page no longer walks -> resprov-walk", n48_ad_miss_why(&f), N48_AD_M_RP_WALK);
    f = n48_ad_missf {}; f.rpHas = 1u; expect_u("miss: an entry with no nameable reason -> none", n48_ad_miss_why(&f), N48_AD_M_NONE);
    expect_u("miss: a null facts pointer -> none", n48_ad_miss_why(nullptr), N48_AD_M_NONE);
    // per identity x verdict, per reason, only in SHADOW and only for on-list programs
    St *s = newSt(kArm);
    uint32_t mode = 0, elem = 0, dcc = 0; rec_facts(kAn, &mode, &elem, &dcc);
    n48_ad_r r {}; uint32_t lk = 0, ln = 0, cl = 0;
    n48_ad_state S = s->S();
    n48_ad_q q = mkq(kIdX, 0x400900000ull, 0x30000000ull, mode, elem, kAn);
    (void)n48_ad_ask(&S, N48_AD_SHADOW, kArm, &q, N48_AD_M_RP_EPOCH, &cl, &r, &lk, &ln);
    (void)n48_ad_ask(&S, N48_AD_SHADOW, kArm, &q, N48_AD_M_LEDGER_NONE, &cl, &r, &lk, &ln);
    n48_ad_q qo = mkq(kIdP, 0x400900000ull, 0x30000000ull, mode, elem, kAn);
    (void)n48_ad_ask(&S, N48_AD_SHADOW, kArm, &qo, N48_AD_M_RP_EPOCH, &cl, &r, &lk, &ln);
    expect_u("counters: SHADOW misses counted for on-list programs only (X twice, P not)", s->ctr.miss[N48_AD_M_RP_EPOCH] * 10u + s->ctr.miss[N48_AD_M_LEDGER_NONE], 1u * 10u + 1u);
    (void)n48_ad_ask(&S, N48_AD_ON, kArm, &q, N48_AD_M_RP_EPOCH, &cl, &r, &lk, &ln);
    expect_u("counters: ON counts no misses", s->ctr.miss[N48_AD_M_RP_EPOCH], 1u);
    expect_u("counters: X never-written x3, off-list not-row x1", s->ctr.v[0][N48_AD_V_NEVER] * 10u + s->ctr.v[N48_AD_IDS][N48_AD_V_NOROW], 3u * 10u + 1u);
    // the log budgets: 24 refusal lines then none; an off-list program never logs; admit lines have their own cap
    St *g = newSt(kArm);
    n48_ad_state SG = g->S();
    uint32_t lines = 0, offLines = 0, admLines = 0;
    for (uint32_t k = 0; k < 100u; k++) { (void)n48_ad_ask(&SG, N48_AD_SHADOW, kArm, &q, 0u, &cl, &r, &lk, &ln); if (lk == 2u) lines++; }
    for (uint32_t k = 0; k < 100u; k++) { (void)n48_ad_ask(&SG, N48_AD_SHADOW, kArm, &qo, 0u, &cl, &r, &lk, &ln); if (lk) offLines++; }
    n48_ad_feed_ledger(&g->ev, kArm, kCtx, 0x400900000ull, mode, 0x30000000ull, 0x4000ull, 1u);
    for (uint32_t k = 0; k < 100u; k++) { (void)n48_ad_ask(&SG, N48_AD_SHADOW, kArm, &q, 0u, &cl, &r, &lk, &ln); if (lk == 1u) admLines++; }
    expect_u("lines: at most 24 refusal lines, 24 admit lines, none for an off-list program (and 64 in all)", lines * 1000000u + admLines * 1000u + offLines, 24u * 1000000u + 24u * 1000u + 0u);
    expect_u("lines: the arm's line total never exceeds 64", (g->ctr.lines <= N48_AD_LINES ? 1u : 0u) + (N48_AD_LINES_ADMIT + N48_AD_LINES_REFUSE + N48_AD_LINES_SEC == N48_AD_LINES ? 2u : 0u), 3u);
    // the per-frame upper bound
    n48_ad_ctr c {}; n48_ad_frame fr {};
    n48_ad_frame_begin(&fr); n48_ad_frame_note(&fr, N48_AD_V_ADMIT); n48_ad_frame_note(&fr, N48_AD_V_ADMIT); n48_ad_frame_end(&c, &fr, 1u);
    n48_ad_frame_begin(&fr); n48_ad_frame_note(&fr, N48_AD_V_ADMIT); n48_ad_frame_note(&fr, N48_AD_V_NEVER); n48_ad_frame_end(&c, &fr, 0u);
    n48_ad_frame_begin(&fr); n48_ad_frame_end(&c, &fr, 0u);
    expect_u("frames: 2 asked, one had EVERY refused record would-admit, one committed with an admission; an empty frame counts nothing",
             c.frames * 100u + c.framesAllWould * 10u + c.framesOnAdmit, 2u * 100u + 1u * 10u + 1u);
    // the per-second window
    n48_ad_win w {}, snap {};
    n48_ad_win_add(&w, 1u, 3u); n48_ad_win_add(&w, N48_AD_IDS + 9u, 1u);
    expect_u("window: nothing flushes inside its second", n48_ad_win_step(&w, 1000000ull, 1u, 1u, &snap), 0u);
    expect_u("window: at the second it flushes once, with the counts", n48_ad_win_step(&w, 2000000ull, 1u, 0u, &snap) * 10000u + snap.adm[1] * 1000u + snap.adm[N48_AD_IDS] * 100u + snap.frames * 10u + snap.framesAdm,
             10000u + 3000u + 100u + 2u * 10u + 1u);
    expect_u("window: an empty second prints nothing", n48_ad_win_step(&w, 3100000ull, 1u, 0u, &snap), 0u);
    delete s; delete g;
}

static void t_verb()
{
    expect_u("T11 112 ON is refused while 103 is ON", n48_ad_verb_refuses_112(N48_AD_ON, 1u), 1u);
    expect_u("T11 103 ON is refused while 112 is ON", n48_ad_verb_refuses_103(1u, N48_AD_ON), 1u);
    uint32_t others = 0;
    for (uint32_t m = 0; m < 3u; m++) for (uint32_t o = 0; o < 2u; o++) if (!(m == N48_AD_ON && o)) others += n48_ad_verb_refuses_112(m, o);
    for (uint32_t m = 0; m < 3u; m++) for (uint32_t o = 0; o < 2u; o++) if (!(m == N48_AD_ON && o)) others += n48_ad_verb_refuses_103(o, m);
    expect_u("T11 every other combination is allowed (112 SHADOW/OFF with 103 ON; 103 SHADOW/OFF with 112 ON; 103 ON with 112 OFF/SHADOW)", others, 0u);
    // 0.0.555: the 110 exclusion, both directions ("ON-pending" is ON: the predicate reads the STORED mode, not the preconditions)
    expect_u("T13 112 ON is refused while 110 is ON", n48_ad_verb_refuses_112_for_110(N48_AD_ON, 1u), 1u);
    expect_u("T13 110 ON is refused while 112 is ON (stored mode, whatever the preconditions)", n48_ad_verb_refuses_110(1u, N48_AD_ON), 1u);
    uint32_t oth2 = 0;
    for (uint32_t m = 0; m < 3u; m++) for (uint32_t o = 0; o < 2u; o++) if (!(m == N48_AD_ON && o)) oth2 += n48_ad_verb_refuses_112_for_110(m, o);
    for (uint32_t m = 0; m < 3u; m++) for (uint32_t o = 0; o < 2u; o++) if (!(m == N48_AD_ON && o)) oth2 += n48_ad_verb_refuses_110(o, m);
    expect_u("T13 every other 110/112 combination is allowed (SHADOW/OFF on either side, 110 ON with 112 OFF/SHADOW, 110 SHADOW/OFF with 112 ON)", oth2, 0u);
}

// ---- 0.0.555 MUST-FIX 1: a copy / lin entry's WHOLE extent is walked --------------------------------------------------------------------
static void t_extent()
{
    uint32_t mode = 0, elem = 0, dcc = 0; rec_facts(kAn, &mode, &elem, &dcc);
    const uint64_t page = 0x2a0000000ull;
    for (uint32_t form = 0; form < 2u; form++) {   // 0 = a residency copy (COPY), 1 = a backing-sourced one (LIN)
        const char *nm = form ? "lin" : "copy";
        char w[160];
        St *s = newSt(kArm);
        n48_rp_copy c {}; c.copied = 1u; c.ctx = kCtx; c.va = kAnVa; c.mode = mode; c.vram = page; c.bytes = 0x4000ull; c.elemBytes = elem;
        if (form) { c.lin = 1u; c.w = 64u; c.h = 64u; c.retiled = 1u; }
        expect_u("extent: feed", n48_ad_feed_copy(&s->ev, kArm, &c, N48_RP_REC_OK), 1u);
        n48_ad_q q = mkq(kIdAN, kAnVa, page, mode, elem, kAn);
        std::snprintf(w, sizeof w, "T14 %s: an intact 4-page extent is ADMITTED", nm);
        expect_u(w, askv(s, N48_AD_ON, kArm, q), 10u + N48_AD_V_ADMIT);
        for (uint32_t pg = 0; pg < 4u; pg++) {   // every page in turn moved: refused (NOPAGE), counted as moved
            n48_ad_q qm = mkq(kIdAN, kAnVa, page, mode, elem, kAn);
            vmof(qm)->movedVa = kAnVa + 0x1000ull * pg;
            n48_ad_r r {};
            const uint64_t before = s->ctr.extMoved;
            std::snprintf(w, sizeof w, "T14 %s: page %u of the extent re-mapped -> REFUSED (no-page), extMoved +1, extWhy moved", nm, pg);
            expect_u(w, askv(s, N48_AD_ON, kArm, qm, &r) * 100u + (uint32_t)(s->ctr.extMoved - before) * 10u + r.extWhy, 0u * 1000u + N48_AD_V_NOPAGE * 100u + 10u + N48_AD_X_MOVED);
        }
        for (uint32_t pg = 1; pg < 4u; pg += 2u) {   // a later page that no longer resolves: refused, counted as unresolved
            n48_ad_q qu = mkq(kIdAN, kAnVa, page, mode, elem, kAn);
            vmof(qu)->unresVa = kAnVa + 0x1000ull * pg;
            n48_ad_r r {};
            const uint64_t before = s->ctr.extUnresolved;
            std::snprintf(w, sizeof w, "T14 %s: page %u unresolved -> REFUSED (never admits on an unresolved page)", nm, pg);
            expect_u(w, askv(s, N48_AD_ON, kArm, qu, &r) * 100u + (uint32_t)(s->ctr.extUnresolved - before) * 10u + r.extWhy, N48_AD_V_NOPAGE * 100u + 10u + N48_AD_X_UNRESOLVED);
        }
        { n48_ad_q qn = mkq(kIdAN, kAnVa, page, mode, elem, kAn); qn.walk = nullptr;
          n48_ad_r r {};
          expect_u("T14: no walk callback -> refused", askv(s, N48_AD_ON, kArm, qn, &r) * 100u + r.extWhy, N48_AD_V_NOPAGE * 100u + N48_AD_X_NOWALK); }
        { n48_ad_q qn = mkq(kIdAN, kAnVa, page, mode, elem, kAn); qn.vm = nullptr;
          expect_u("T14: no VM -> refused", askv(s, N48_AD_ON, kArm, qn), N48_AD_V_NOPAGE); }
        { n48_ad_q qm = mkq(kIdAN, kAnVa, page, mode, elem, kAn); vmof(qm)->movedVa = kAnVa + 0x3000ull;
          n48_ad_r r {}; uint32_t lk = 0, ln = 0, cl = 0; const n48_ad_state S = s->S();
          const uint32_t y = n48_ad_ask(&S, N48_AD_SHADOW, kArm, &qm, 0u, &cl, &r, &lk, &ln);
          expect_u("T14: SHADOW sees the same refusal and returns 0", y * 10u + r.verdict, N48_AD_V_NOPAGE); }
        expect_u("T14: the reasons are counted apart (moved 5, unresolved 2, no-walk 2: no callback, no VM)", s->ctr.extMoved * 100u + s->ctr.extUnresolved * 10u + s->ctr.extNoWalk, 5u * 100u + 2u * 10u + 2u);
        // the walk touches every page of the extent, once each (4 pages), and stops at the first bad one
        { n48_ad_q qc = mkq(kIdAN, kAnVa, page, mode, elem, kAn); qc.walk = &walk_count; gWalkCalls = 0;
          (void)askv(s, N48_AD_ON, kArm, qc);
          expect_u("T14: an intact extent is walked page by page (4 walks for 0x4000 bytes)", (uint64_t)gWalkCalls, 4u);
          gWalkCalls = 0; vmof(qc)->movedVa = kAnVa + 0x1000ull;
          (void)askv(s, N48_AD_ON, kArm, qc);
          expect_u("T14: the walk stops at the first moved page (2 walks)", (uint64_t)gWalkCalls, 2u); }
        delete s;
    }
    // an extent of 0 bytes still walks the base page; an absurd extent is refused unresolved
    { St *s = newSt(kArm);
      n48_rp_copy c {}; c.copied = 1u; c.ctx = kCtx; c.va = kAnVa; c.mode = mode; c.vram = page; c.bytes = 0ull; c.elemBytes = elem;
      n48_ad_feed_copy(&s->ev, kArm, &c, N48_RP_REC_OK);
      n48_ad_q q = mkq(kIdAN, kAnVa, page, mode, elem, kAn); q.walk = &walk_count; gWalkCalls = 0;
      expect_u("extent 0 bytes: admitted, and the base page was walked once", askv(s, N48_AD_ON, kArm, q) * 10u + (uint32_t)gWalkCalls, 100u + 1u);
      vmof(q)->movedVa = kAnVa;
      expect_u("extent 0 bytes: a moved base page refuses", askv(s, N48_AD_ON, kArm, q), N48_AD_V_NOPAGE);
      delete s; }
    { St *s = newSt(kArm);
      n48_rp_copy c {}; c.copied = 1u; c.ctx = kCtx; c.va = kAnVa; c.mode = mode; c.vram = page; c.bytes = 0x200000000ull; c.elemBytes = elem;
      n48_ad_feed_copy(&s->ev, kArm, &c, N48_RP_REC_OK);
      n48_ad_q q = mkq(kIdAN, kAnVa, page, mode, elem, kAn); q.walk = &walk_count; gWalkCalls = 0;
      expect_u("an absurd extent (8 GiB) is refused unresolved without a walk", askv(s, N48_AD_ON, kArm, q) * 10u + (uint32_t)gWalkCalls, N48_AD_V_NOPAGE * 10u);
      delete s; }
    // LEDGER-sourced entries keep today's rule: the base page only, no extent walk
    { St *s = newSt(kArm);
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 1u);
      n48_ad_q q = mkq(kIdAN, kAnVa, page, mode, elem, kAn); q.walk = &walk_count; gWalkCalls = 0; vmof(q)->movedVa = kAnVa + 0x2000ull;
      expect_u("a committed-frame (LEDGER) entry with a moved later page still admits, with NO walk (today's rule)", askv(s, N48_AD_ON, kArm, q) * 10u + (uint32_t)gWalkCalls, 100u + 0u);
      delete s; }
    // a failing copy entry beside an intact ledger entry: the ledger entry admits; beside a second intact copy entry (other src): admits too
    { St *s = newSt(kArm);
      n48_rp_copy c {}; c.copied = 1u; c.ctx = kCtx; c.va = kAnVa; c.mode = mode; c.vram = page; c.bytes = 0x4000ull; c.elemBytes = elem;
      n48_ad_feed_copy(&s->ev, kArm, &c, N48_RP_REC_OK);
      n48_ad_q q = mkq(kIdAN, kAnVa, page, mode, elem, kAn); vmof(q)->movedVa = kAnVa + 0x1000ull;
      expect_u("moved copy entry alone: refused", askv(s, N48_AD_ON, kArm, q), N48_AD_V_NOPAGE);
      n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 2u);
      { n48_ad_r r {}; const uint32_t y = ask(s, N48_AD_ON, kArm, q, nullptr, &r);
        expect_u("... a committed-frame entry for the same key then admits and clears the extent reason", y * 100u + r.src * 10u + r.extWhy, 100u + N48_AD_SRC_LEDGER * 10u); }
      delete s; }
}

// ---- 0.0.555 MUST-FIX 2: a lapsed precondition wipes the ledger ---------------------------------------------------------------------------
static void t_precond()
{
    uint32_t mode = 0, elem = 0, dcc = 0; rec_facts(kAn, &mode, &elem, &dcc);
    const uint64_t page = 0x2a0000000ull;
    St *s = newSt(kArm);
    n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 1u);
    n48_rp_copy c {}; c.copied = 1u; c.ctx = kCtx; c.va = kSVa; c.mode = mode; c.vram = page + 0x100000ull; c.bytes = 0x1000ull; c.elemBytes = elem;
    n48_ad_feed_copy(&s->ev, kArm, &c, N48_RP_REC_OK);
    n48_ad_q q = mkq(kIdAN, kAnVa, page, mode, elem, kAn);
    n48_ad_q qs = mkq(kIdAN, kSVa, page + 0x100000ull, mode, elem, kAn);
    expect_u("T15 before the lapse both entries admit", ask(s, N48_AD_ON, kArm, q) * 10u + ask(s, N48_AD_ON, kArm, qs), 11u);
    expect_u("T15 the gate with the preconditions holding wipes nothing", n48_ad_precond_gate(&s->ev, 1u) * 10u + s->ev.n, 1u * 10u + 2u);
    expect_u("T15 a lapse: the gate answers 0 and WIPES (counted: 1 wipe, 2 entries)", n48_ad_precond_gate(&s->ev, 0u) * 1000u + s->ev.n * 100u + (uint32_t)s->ev.wipePre * 10u + (uint32_t)s->ev.dropPre, 0u * 1000u + 0u + 10u + 2u);
    expect_u("T15 a second lapse check with nothing to wipe counts nothing", n48_ad_precond_gate(&s->ev, 0u) * 100u + (uint32_t)s->ev.wipePre * 10u + (uint32_t)s->ev.dropPre, 12u);
    { const uint32_t g = n48_ad_precond_gate(&s->ev, 1u); const uint32_t v1 = askv(s, N48_AD_ON, kArm, q), v2 = askv(s, N48_AD_ON, kArm, qs);
      expect_u("T15 the preconditions RETURN: the gate answers 1 and the pre-lapse entries never admit (both NEVER)", g * 100u + v1 * 10u + v2, 100u + 10u * N48_AD_V_NEVER + N48_AD_V_NEVER); }
    expect_u("T15 ... the ledger learns again after the return (a post-lapse feed admits)", n48_ad_feed_ledger(&s->ev, kArm, kCtx, kAnVa, mode, page, 0x4000ull, 3u) * 10u + ask(s, N48_AD_ON, kArm, q), 11u);
    // an unmap during the lapse is exactly what the wipe covers: it never reaches the ledger, yet the entry cannot come back
    n48_ad_precond_gate(&s->ev, 0u);
    n48_ad_precond_gate(&s->ev, 1u);
    expect_u("T15 a surface fed, lapsed (its unmap unseen), returned: refused", ask(s, N48_AD_ON, kArm, q), 0u);
    delete s;
}

// ---- 0.0.555 SHOULD: the drops compact in one pass and are behaviour-identical to 0.0.554's per-entry shifting -----------------------------------
static void old_drop_at(n48_ever *ev, uint32_t k)
{
    for (uint32_t j = k; j + 1u < ev->n && j + 1u < N48_AD_EVER_MAX; j++) ev->e[j] = ev->e[j + 1u];
    if (ev->n) ev->n--;
}
static uint32_t old_unmap_rng(n48_ever *ev, uint64_t ctx, uint64_t va, uint64_t size)
{
    ev->unmaps++;
    uint32_t d = 0u;
    for (uint32_t k = 0; k < ev->n && k < N48_AD_EVER_MAX; ) {
        const n48_ad_ent *e = &ev->e[k];
        const int mine = (!ctx || e->ctx == ctx);
        if (mine && !n48_dl_unmap_keeps(va, size, e->va, e->bytes)) { old_drop_at(ev, k); ev->dropUnmap++; d++; }
        else k++;
    }
    return d;
}
static uint32_t old_rebind(n48_ever *ev, uint64_t bound)
{
    uint32_t d = 0u;
    for (uint32_t k = 0; k < ev->n && k < N48_AD_EVER_MAX; ) {
        if (!bound || ev->e[k].ctx != bound) { old_drop_at(ev, k); ev->dropRebind++; d++; } else k++;
    }
    return d;
}
static uint32_t old_put(n48_ever *ev, uint64_t arm, const n48_ad_ent *in)
{
    if (!arm || ev->arm != arm) { ev->noArm++; return 0u; }
    if (!in->ctx || !in->va || !in->mode) return 0u;
    if (!in->page || (in->page & 0xfffull)) { ev->noPage++; return 0u; }
    for (uint32_t k = 0; k < ev->n && k < N48_AD_EVER_MAX; k++)
        if (n48_ad_is_key(&ev->e[k], in->ctx, in->va, in->mode, in->page, in->src)) {
            ev->e[k] = *in; ev->e[k].seq = ++ev->seq;
            ev->refreshed++;
            return 1u;
        }
    if (ev->n >= N48_AD_EVER_MAX) {
        uint32_t old = 0u;
        for (uint32_t k = 1; k < ev->n; k++) if (ev->e[k].seq < ev->e[old].seq) old = k;
        old_drop_at(ev, old);
        ev->evicted++;
    }
    ev->e[ev->n] = *in; ev->e[ev->n].seq = ++ev->seq;
    ev->n++;
    return 1u;
}
static bool same_ledger(const n48_ever *a, const n48_ever *b)
{
    if (a->n != b->n || a->seq != b->seq || a->evicted != b->evicted || a->refreshed != b->refreshed || a->dropUnmap != b->dropUnmap ||
        a->dropRebind != b->dropRebind || a->unmaps != b->unmaps || a->noPage != b->noPage || a->noArm != b->noArm) return false;
    for (uint32_t k = 0; k < a->n; k++) {
        const n48_ad_ent &x = a->e[k], &y = b->e[k];
        if (x.ctx != y.ctx || x.va != y.va || x.page != y.page || x.bytes != y.bytes || x.seq != y.seq || x.mode != y.mode || x.src != y.src || x.tok != y.tok) return false;
    }
    return true;
}
static void t_drop_equiv()
{
    static n48_ever A, B;
    n48_ad_reset(&A); n48_ad_reset(&B);
    n48_ad_scope(&A, kArm); n48_ad_scope(&B, kArm);
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    auto rnd = [&rng](uint32_t m) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng % m); };
    uint32_t bad = 0, maxN = 0, wholeDrops = 0;
    for (uint32_t op = 0; op < 60000u && !bad; op++) {
        const uint32_t kind = rnd(1000);
        if (kind < 960) {   // a put: 1-3 contexts, 420 pages / VAs (so the 256-entry eviction runs), extents 0 / 0x4000 / 0x10000, three sources
            n48_ad_ent in {};
            const uint32_t k = rnd(420);
            in.ctx = 1u + rnd(3u); in.va = 0x400000000ull + 0x10000ull * k; in.mode = 1u + rnd(2u); in.page = 0x100000000ull + 0x1000ull * k;
            in.bytes = rnd(3u) == 0u ? 0ull : rnd(2u) ? 0x4000ull : 0x10000ull; in.src = 1u + rnd(3u); in.tok = op;
            const uint32_t ra = n48_ad_put(&A, kArm, &in), rb = old_put(&B, kArm, &in);
            if (ra != rb) bad = 1;
        } else if (kind < 998) {   // an unmap: a range (sometimes all contexts, sometimes size 0, sometimes wrapping)
            const uint64_t ctx = rnd(5) == 0 ? 0ull : 1u + rnd(3u);
            uint64_t va = 0x400000000ull + 0x10000ull * rnd(420), size = 0x10000ull * (1u + rnd(6));
            const uint32_t sp = rnd(20);
            if (sp == 0) size = 0; else if (sp == 1) { va = 0xFFFFFFFFFFFFF000ull; size = 0x2000ull; }
            const uint32_t da = n48_ad_unmap_rng(&A, ctx, va, size), db = old_unmap_rng(&B, ctx, va, size);
            if (da != db) bad = 2;
            if (da >= 3u) wholeDrops++;
        } else {   // a rebind sweep
            const uint64_t bound = rnd(6) == 0 ? 0ull : 1u + rnd(3u);
            const uint32_t da = n48_ad_rebind(&A, bound), db = old_rebind(&B, bound);
            if (da != db) bad = 3;
        }
        if (!same_ledger(&A, &B)) bad = bad ? bad : 4;
        if (A.n > maxN) maxN = A.n;
    }
    expect_u("T16 60000 random put / unmap / rebind ops: the compacting drops equal 0.0.554's per-entry shifting after EVERY op (entries, order, counters)", bad, 0u);
    std::printf("      T16 stats: maxN %u evicted %llu multi-drops %u unmaps %llu\n", maxN, (unsigned long long)B.evicted, wholeDrops, (unsigned long long)B.unmaps);
    expect("T16 the run reached capacity (eviction ran) and dropped many entries at once", maxN == N48_AD_EVER_MAX && B.evicted > 0u && wholeDrops > 0u);
    // the single drop_at: order preserved, any index, the last, out of range
    for (uint32_t n = 0; n <= 5u; n++) for (uint32_t k = 0; k <= n; k++) {
        static n48_ever P, Q; n48_ad_reset(&P); n48_ad_reset(&Q); P.arm = Q.arm = kArm;
        for (uint32_t j = 0; j < n; j++) { P.e[j] = n48_ad_ent {}; P.e[j].va = 100u + j; P.e[j].seq = j + 1u; Q.e[j] = P.e[j]; }
        P.n = Q.n = n;
        n48_ad_drop_at(&P, k); old_drop_at(&Q, k);
        if (!same_ledger(&P, &Q)) bad = 5;
    }
    expect_u("T16 drop_at equals the old shift for every (n, k) in 0..5 (including k == n, an out-of-range index, and n 0)", bad, 0u);
}

// The source pins: the kext's composition of these pure calls, and the translator's reach
static void t_pins(const char *ahhPath, const char *ibPath)
{
    const std::string hh = slurp(ahhPath), ib = slurp(ibPath);
    if (hh.empty() || ib.empty()) { expect("PIN: the sources were read", false); return; }
    const std::string verb = between(hh, "    } else if ((arg & 0xffull) == 112ull) {", "    } else if ((arg & 0xffull) == 111ull) {");
    const std::string v103 = between(hh, "    } else if ((arg & 0xffull) == 103ull) {", "    } else if ((arg & 0xffull) == 98ull) {");
    expect("T6 the switch: OFF at boot, ONE writer, both mid-arm guards, SWITCH-GUARD:112 (the continuous guard lists it), the selector once",
           cnt(hh, "static volatile uint32_t gAd112Mode { N48_AD_OFF };") == 1u && cnt(hh, "__atomic_store_n(&gAd112Mode, want, __ATOMIC_RELEASE);") == 1u &&
           cnt(hh, "__atomic_store_n(&gAd112Mode") == 1u && cnt(hh, "gAd112Mode = ") == 0u && cnt(hh, "} else if ((arg & 0xffull) == 112ull) {") == 1u && !verb.empty() &&
           cnt(hh, "n48_cm_cont_switch_refused(112u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") == 1u &&
           cnt(hh, "const bool armRefused112 = m != 0u && hw_cm_armed() != 0u;") == 1u && n48_cm_cont_switch_guarded(112u) == 1u &&
           n48_cm_cont_switch_refused(112u, 0u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 1u && n48_cm_cont_switch_refused(112u, 1u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 0u);
    expect("T11 the verb: 112 ON refused while 103 is ON (status 9) and 103 ON refused while 112 is ON (status 9), each by the pure predicate, once",
           cnt(verb, "} else if (n48_ad_verb_refuses_112(want, st103_mode() == N48_ST_M_ON ? 1u : 0u)) {") == 1u && cnt(verb, "st = 9; how112 = ") == 2u &&
           cnt(v103, "} else if (n48_ad_verb_refuses_103(m == N48_ST_M_ON ? 1u : 0u, ad112_mode())) {") == 1u && cnt(v103, "st = 9; how103 = ") == 1u &&
           cnt(hh, "n48_ad_verb_refuses_112(") == 1u && cnt(hh, "n48_ad_verb_refuses_103(") == 1u);
    expect("the verb prints its lines through ad112_report_lines only (FIVE lines there), and resets counters, frame, window and asks the ledger reset",
           cnt(verb, "ad112_report_lines(how112);") == 1u && cnt(verb, "HWLOG(") == 0u && cnt(hh, "ad112_report_lines(") == 2u &&
           cnt(between(hh, "static void ad112_report_lines(const char *how) {", "\n}\n"), "HWLOG(") == 4u &&   /* state, ledger, the identity matrix (in a loop: two lines), the reasons: FIVE lines */
           cnt(verb, "gAdC = n48_ad_ctr {};") == 1u && cnt(verb, "__atomic_store_n(&gAdEverReset, 1u, __ATOMIC_RELEASE);") == 1u);
    // 0.0.555 pins. MUST-FIX 1: the ask hands the decision the frame's own walk and VM. MUST-FIX 2: the precondition gate runs at the frame top, at the ask
    // (BEFORE the scope and the decision) and at both feeds and the frame end, all through ONE definition. SHOULD: the 110 exclusion in the 110 verb.
    const std::string askS = between(hh, "static __attribute__((noinline)) int ad112_ask(", "static int ad112_stale_ok(");
    const std::string topS = between(hh, "static void ad112_frame_top() {", "static __attribute__((noinline)) void ad112_frame_end(");
    const std::string ledS = between(hh, "static __attribute__((noinline)) void ad112_note_ledger(", "// FEED 2 (");
    const std::string cpyS = between(hh, "static void ad112_note_copy(", "// DROP: an unmapVA by range");
    const std::string endS = between(hh, "static __attribute__((noinline)) void ad112_frame_end(", "static void ad112_report_lines(");
    const std::string v110 = between(hh, "    } else if ((arg & 0xffull) == 110ull) {", "    } else if ((arg & 0xffull) == 112ull) {");
    expect("T14 pin: the ask passes the asking frame's own walk and VM into the decision (the whole extent is walked there)",
           cnt(askS, "q.walk = &gfxsrc_rp_walk; q.vm = c->vm;") == 1u && cnt(hh, "q.walk = ") == 1u && cnt(hh, "n48_ad_extent_walk(") == 0u);
    expect("T15 pin: ONE gate definition (n48_ad_precond_gate over gEver and the live preconditions) and every locked entry goes through it",
           cnt(hh, "n48_ad_precond_gate(&gEver, ad112_preconds())") == 1u && cnt(hh, "ad112_pre_locked()") == 3u &&   /* the definition, feeding_locked, the ask */
           cnt(hh, "ad112_feeding_locked()") == 5u && cnt(topS, "ad112_feeding_locked()") == 1u && cnt(ledS, "ad112_feeding_locked()") == 1u &&
           cnt(cpyS, "ad112_feeding_locked()") == 1u && cnt(endS, "ad112_feeding_locked()") == 1u &&
           cnt(topS, "ad112_feeding()") == 0u && cnt(ledS, "ad112_feeding()") == 0u && cnt(cpyS, "ad112_feeding()") == 0u && cnt(endS, "ad112_feeding()") == 0u);
    { const size_t g = askS.find("if (!ad112_pre_locked()) return 0;"), sc = askS.find("ad112_scope();"), dec = askS.find("n48_ad_ask(&S");
      expect("T15 pin (ORDERING): the ASK path itself checks the preconditions, before the scope and before the decision (a wipe at the frame top alone is not enough)",
             g != std::string::npos && sc != std::string::npos && dec != std::string::npos && g < sc && sc < dec); }
    expect("T13 pin: the 110 verb refuses 110 ON while 112 is ON (status 9) by the pure predicate over the STORED 112 mode, once; the 112 verb refuses 112 ON while 110 is ON, once",
           cnt(v110, "} else if (n48_ad_verb_refuses_110(want == N48_BB_ON ? 1u : 0u, ad112_mode())) {") == 1u && cnt(v110, "st = 9; how110 = ") == 1u &&
           cnt(verb, "} else if (n48_ad_verb_refuses_112_for_110(want, an110_mode() == N48_BB_ON ? 1u : 0u)) {") == 1u &&
           cnt(hh, "n48_ad_verb_refuses_110(") == 1u && cnt(hh, "n48_ad_verb_refuses_112_for_110(") == 1u);
    expect("the stale comment is gone (the frame top runs BEFORE the drains: it is the frame's first 112 step)",
           cnt(hh, "(under gXdLock, after the drains)") == 0u && cnt(hh, "BEFORE the drains: it is called ahead of the frame's own unmap work") == 1u);
    // wiring: ONE assignment, under the pure wiring predicate; OFF leaves it null
    expect("T6 the callback is wired in exactly one place, only through n48_ad_wire (mode not OFF, the five preconditions, 103 not ON)",
           cnt(hh, "desc_stale_ok = ") == 1u && cnt(hh, "if (n48_ad_wire(ad112_mode(), ad112_preconds(), st103_mode() == N48_ST_M_ON ? 1u : 0u)) ex.desc_stale_ok = &ad112_stale_ok;") == 1u);
    // the callback: the ONE decision function, no other decision in the kext
    const std::string ask = between(hh, "static __attribute__((noinline)) int ad112_ask(", "static int ad112_stale_ok(");
    expect("the callback composes the pure ask: one n48_ad_ask call, the page from THIS ask's walk consumed once and from the frame's own VM, VRAM pages only",
           cnt(ask, "n48_ad_ask(&S, m, gXdShot.armed_at_us, &q, why, clamp, &r, &logKind, &logNo)") == 1u && cnt(ask, "gXdAskPg.ok = 0u;") == 1u &&
           cnt(ask, "if (have && !sys) {") == 1u && cnt(ask, "vmib_to_vram_off(raw, c->vm->fbStart, c->vm->vramSize, ok)") == 1u &&
           cnt(ask, "gfxc_page(*c->vm, va & ~0xfffull, raw, sys)") == 1u && cnt(hh, "n48_ad_decide(") == 0u && cnt(hh, "n48_ad_count(") == 0u);
    expect("the callback returns the pure answer unchanged (SHADOW and every refusal are 0 by n48_ad_ask), and answers 0 for a null context or a mode that is not ON / SHADOW",
           cnt(between(hh, "static int ad112_stale_ok(", "// A translation's DCC records"), "if (!c || (m != N48_AD_ON && m != N48_AD_SHADOW)) return 0;") == 1u &&
           cnt(ask, "return (int)yes;") == 1u);
    // the feeds: only where the spec says, only for a copy that RECORDED, never from the epoch / 58 / 109
    const std::string sec = between(hh, "static volatile uint32_t gAd112Mode { N48_AD_OFF };", "static void ad112_report_lines(const char *how) {");
    expect("the EVER ledger is fed at exactly two kinds of site: after the committed-frame ledger feed (under `if (added)`), and at BOTH resprov RECORD sites with the record's own answer",
           cnt(hh, "if (added) ad112_note_ledger(vm, lf.ctx, lf.tok);") == 1u && cnt(hh, "ad112_note_copy(&it[i].c, why);") == 1u && cnt(hh, "ad112_note_copy(c, why);") == 1u &&
           cnt(hh, "ad112_note_ledger(") == 2u && cnt(hh, "ad112_note_copy(") == 3u && cnt(hh, "n48_ad_feed_ledger(") == 1u && cnt(hh, "n48_ad_feed_copy(") == 1u && cnt(hh, "n48_ad_put(") == 0u);
    expect("both copy feeds sit beside the RECORD call they mirror (after n48_rp_record_x, in its own block)",
           cnt(hh, "if (why == N48_RP_REC_OK) st103_copy_note(&it[i].c);   // build 0.0.544 (switch 103): the copy wrote this page\n        ad112_note_copy(&it[i].c, why);") == 1u &&
           cnt(hh, "    ad112_note_copy(c, why);   // build 0.0.554 (switch 112): only a copy that RECORDED (why 0) feeds the EVER ledger\n    if (why == N48_RP_REC_OK) st103_copy_note(c);") == 1u);
    expect("the ledger feed keeps only what its own frame wrote: this ctx and this token, a page that walks to VRAM and agrees with the entry's",
           cnt(sec, "if (e.ctx != ctx || e.tok != tok) continue;") == 1u && cnt(sec, "!sys && page && page == e.page") == 1u &&
           cnt(sec, "vmib_to_vram_off(page, vm.fbStart, vm.vramSize, ok)") == 1u);
    expect("the EVER ledger is NOT touched by the descriptor epoch, switch 58's un-feed or switch 109's eviction (no mention in the section but the miss report's read)",
           cnt(sec, "gXdDpEpoch") == 1u && cnt(sec, "n48_dl_unfeed") == 0u && cnt(sec, "gXdLedUnfeedQ") == 0u && cnt(sec, "n48_rp_evict") == 0u && cnt(sec, "rp109") == 0u &&
           cnt(sec, "ledMid") == 0u && cnt(hh, "n48_ad_reset(&gEver)") == 1u);
    // the drops
    expect("T8 the unmap drop: the locked branch, BOTH drain paths (each queued event, the whole-scope wipe, the per-context bounded wipe), the WindowServer drop, the rebind sweep, and a lost event",
           cnt(hh, "ad112_unmap(ctxSeq, va, size);") == 1u && cnt(hh, "ad112_unmap(ent.ctx, ent.va, ent.size);") == 1u && cnt(hh, "ad112_unmap(0ull, 0ull, 0ull);") == 1u &&
           cnt(hh, "ad112_unmap(dirty[i].ctx, dirty[i].vaMin, dirty[i].vaEnd - dirty[i].vaMin);") == 1u && cnt(hh, "(void)n48_ad_ws_gone(&gEver);") == 1u &&
           cnt(hh, "(void)n48_ad_rebind(&gEver, wsKey);") == 1u && cnt(hh, "__atomic_fetch_add(&gAdLost, 1u, __ATOMIC_RELAXED);") == 2u &&
           cnt(hh, "(void)n48_ad_unmap_rng(&gEver, ctx, va, size);") == 1u && cnt(hh, "(void)n48_ad_wipe_lost(&gEver);") == 1u && cnt(hh, "(void)n48_ad_scope(&gEver, arm);") == 1u);
    expect("the frame's hooks: the top (scope and accounting), the end (after the ledger feed, with the commit answer), and the DCC census after the primary translate",
           cnt(hh, "    ad112_frame_top();   // build 0.0.554") == 1u && cnt(hh, "ad112_frame_end(commitOk ? 1u : 0u);") == 1u && cnt(hh, "if (ds.stale_dcc) ad112_note_ds(&ds);") == 1u);
    // T3 pin: the R2 input's `resolved` is the page walk's answer alone, never `proven`
    expect("T3 the kext's R2 input: `resolved` is gfxc_page's answer and nothing else (an admitted input is not resolved by being admitted)",
           cnt(hh, "cc.resolved[q] = gfxc_page(vm, cc.va[q] & ~0xfffull, page, sys) ? 1u : 0u;") == 1u);
    // T5: the translator's reach
    const std::string tdesc = between(ib, "static __attribute__((noinline)) uint32_t d_table_desc(", "/* 0.0.394: in_samp_va was already set above");
    const std::string inl = between(ib, "static __attribute__((noinline)) uint32_t d_inline_desc(", "\nstatic ");
    const std::string probe = between(ib, "static D_NOINLINE uint32_t d_de_probe_rest(", "static D_NOINLINE void d_de_nl_note(");
    expect("T5 the callback is called in ONE place (d_stale_admit) and that helper is called ONLY from d_table_desc's image loop; the inline path and the probe never name it",
           cnt(ib, "ex->desc_stale_ok(") == 1u && cnt(ib, "d_stale_admit(") == 2u && cnt(tdesc, "d_stale_admit(") == 1u && !tdesc.empty() &&
           cnt(inl, "desc_stale_ok") == 0u && cnt(inl, "d_stale_admit") == 0u && cnt(probe, "desc_stale_ok") == 0u && cnt(probe, "d_stale_admit") == 0u && !probe.empty());
    expect("T4/T5 the offer: tiled (mode), unproven, callback present; a stripped record is only counted (stale_dcc), never offered; a yes makes the input proven, marks it "
           "in in_admit and clamps on request; the export and the refusal read `proven` (which now includes an admission)",
           cnt(tdesc, "int proven = dcc ? (ex->desc_dcc_ok") == 1u && cnt(tdesc, "if (!proven && mode && ex->desc_stale_ok) {") == 1u &&
           cnt(tdesc, "            if (dcc) ds->stale_dcc++;\n            else {") == 1u &&
           cnt(tdesc, "if (r) { proven = 1; ds->in_admit |= 1u << i; if (r == 2u) xlat12_tdesc_clamp_mip0(g[i]); }") == 1u &&
           cnt(tdesc, "ds->in_proven[i] = proven ? 1u : 0u;") == 1u && cnt(tdesc, "if ((mode || dcc) && !proven) {") == 1u &&
           cnt(tdesc, "if (proven && mode && clamp == 1u) xlat12_tdesc_clamp_mip0(g[i]);") == 1u && cnt(tdesc, "d_stale_admit(") == 1u);
    const size_t lp0 = tdesc.find("for (uint32_t i = 0; i < a->ntex; i++) {\n        if (!d_tbl_read(ex, ibase + d_tbl_off(aidx[i], 5u)");
    const std::string loop = lp0 != std::string::npos ? tdesc.substr(lp0) : std::string();   // the image loop runs to the end of the slice (the S# loop follows the marker)
    expect("T12 the loop asks every texture in order and never stops early on an admission (no break in the image loop after the offer)",
           !loop.empty() && cnt(loop, "break;") == 0u && cnt(loop, "d_stale_admit(") == 1u && cnt(loop, "return d_tbl_fail(ds, draw_at, XLAT12_TDESC_PROVENANCE);") == 1u);
}

int main(int argc, char **argv)
{
    std::printf("== 0.0.555: switch 112 admit stale (0.0.554 + the review's must-fixes) ==\n");
    t_widths();
    t_modes_and_allowlist();
    t1();
    t2();
    t3();
    t_e2e();
    t_ledger();
    t_counters();
    t_verb();
    t_extent();
    t_precond();
    t_drop_equiv();
    if (argc >= 3) t_pins(argv[1], argv[2]);
    else expect("PIN: the hook and translator sources were passed", false);
    std::printf("gfx_admit112: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
