// gfx_rect92_test.cpp — build 0.0.536: SWITCH 92, RECTLIST CLEARS THAT CLEAR ON GFX12. Both halves:
//   (1) the translator's XLAT12_EXTRA_RECT2D (xlat12_ib.h): VGT_GS_OUT_PRIM_TYPE (0x30998) = RECT_2D (3) immediately before every draw
//       whose VGT_PRIMITIVE_TYPE (0x30908) is RECTLIST (0x11), the profile's TRISTRIP (2) back before the next other draw, never a
//       grown IB, today's output where there is no room (the fallback), and the backstop xlat12_ib_rect2d_check;
//   (2) the residency copy's image choice (gfx_rv92.h n48_rv92_pick over shadercache.c sc_alt_find): today's RectPosTexFast_VS
//       image OFF, the blob's v3 alternative ON.
// Over REAL captured IBs (tests/fixture_rect92.h: run11y F125 IB0 s0/s2/s5/s6, F127 IB1 s4, F16 IB0 s5; run11v F118 IB1 s0) and two
// blobs rebuilt from re/cache/m4c-r20's own entries:
//   T1  ORDERING: every RECTLIST draw runs with RECT_2D written IMMEDIATELY before it (the last non-NOP packet before the draw is
//       `C0017900 00000266 00000003`, or the previous draw was a RECTLIST with nothing written between), every other draw with 2;
//       nothing else changes: the flagged output with each inserted packet turned back into three pad NOPs IS today's output.
//   T1b ORDERING of the restore, on a real segment with ONE planted mutation (the Apple 0x28a6c write between its first RECTLIST
//       draw and the next draw turned into a same-length NOP): TRISTRIP is written back immediately before that next draw.
//   T2  no IB grows: every flagged translation's length is the input's, nothing written past it, and it verifies.
//   T3  OFF IDENTITY: today's translator, flag OFF, over every segment and three extra-block modes answers exactly as the FROZEN
//       0.0.535 translator (00ff5906, the fixture's recorded status and FNV-1a of the whole output); the flag changes nothing in the
//       control segment (no RECTLIST draw).
//   T4  the verify accepts RECT_2D for RECTLIST draws and REFUSES it for any other draw (mutations of real flagged outputs); a
//       value that is neither 2 nor 3 is refused; the flag is refused with the two other 0x30998 policies.
//   T4b the fallback: a real segment that refuses after its RECTLIST write (F16 IB0 s5, UNLISTED) and a real segment with its
//       room removed by planted NOPs both answer exactly today's output, counted.
//   T5  the v3 image: one dword differs from today's (+0x50, v_cvt_f32_i32 src0 v0 -> v3), both identify to their own rows.
//   T6  the kext's choice: OFF today's image, ON the v3 image, ON without an alternative today's image, another key untouched;
//       the alternative never verifies in a lookup; Apple's bytes followed by the marker are AMBIGUOUS (fail-safe).
//   E1-E3 (fix round item 2) the end rule: RECT_2D in force at the end of an output is refused (0 effect on the real fixtures; a
//       planted segment ending on its RECTLIST draw falls back to today's output).
//   T7  the kext's wiring (AppleHardwareHook.cpp argv[1], Navi48AccelPeer.cpp argv[2]), the report line (<= 491 bytes), and (fix round
//       item 1) the one-frame fallback in xlat12_ib.c (argv[3]).
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 -I src/shadercache -I src/navi48-bringup/tests -x c++ \
//         src/navi48-bringup/tests/gfx_rect92_test.cpp src/xlat12/xlat12_ib.c src/xlat12/xlat12.c src/shadercache/shadercache.c \
//         -o /tmp/r92 && /tmp/r92 src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/apple/Navi48AccelPeer.cpp \
//         src/xlat12/xlat12_ib.c
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <initializer_list>

#include "xlat12.h"
#include "xlat12_ib.h"
#include "shadercache.h"
#include "gfx_commit.h"
#include "gfx_rv92.h"
#include "gfx_rectfb105.h"
#include "fixture_rect92.h"

static int gChecks = 0, gFails = 0;
static void expect(const char *what, bool ok)
{
    gChecks++;
    if (!ok) gFails++;
    if (!ok || std::getenv("R92_VERBOSE")) std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static void expectf(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void expectf(bool ok, const char *fmt, ...)
{
    char b[640];
    va_list ap; va_start(ap, fmt); std::vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    expect(b, ok);
}

static const uint32_t kSentinel = 0xDEADBEEFu;   // the generator's own prefill (the frozen FNV covers a refused output too)
static const uint32_t kNop = 0xFFFF1000u;
static uint32_t gIn[8192], gOn[8192 + 8], gOff[8192 + 8], gTmp[8192 + 8];
static int isn(void *, uint64_t va) { return va == 0x400017a00ull; }

static uint32_t tr(const uint32_t *in, uint32_t n, uint32_t flags, uint32_t *out, xlat12_draw_stats *ds, bool noEx = false)
{
    xlat12_draw_extra ex; std::memset(&ex, 0, sizeof ex);
    ex.flags = flags; ex.cs_is_n = (flags & XLAT12_EXTRA_CS_ELIDE) ? &isn : nullptr;
    for (uint32_t k = 0; k < n + 8u; k++) out[k] = kSentinel;
    uint32_t len = 0;
    const uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), noEx ? nullptr : &ex, in, n, out, &len, ds);
    return st | (len == (st ? 0u : n) ? 0u : 0x80000000u);
}
static uint32_t fnv_out(const uint32_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    const unsigned char *b = reinterpret_cast<const unsigned char *>(p);
    for (uint32_t i = 0; i < n * 4u; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static uint32_t plen(const uint32_t *d, uint32_t i, uint32_t n)
{
    const uint32_t h = d[i];
    if (h == kNop || (h >> 30) == 2u) return 1u;
    if ((h >> 30) != 3u) return 0u;
    const uint32_t l = ((h >> 16) & 0x3FFFu) + 2u;
    return i + l <= n ? l : 0u;
}
static uint32_t op_of(uint32_t h) { return (h >> 8) & 0xFFu; }
static bool is_draw(uint32_t h)
{
    const uint32_t o = op_of(h);
    return (h >> 30) == 3u && h != kNop && (o == 0x2Du || o == 0x27u || o == 0x35u || o == 0x24u || o == 0x25u);
}
static bool is_nop_pkt(uint32_t h) { return h == kNop || (h >> 30) == 2u || ((h >> 30) == 3u && op_of(h) == 0x10u); }
static bool is_r2d_pkt(const uint32_t *d, uint32_t i, uint32_t l, uint32_t v)
{
    return l == 3u && d[i] == 0xC0017900u && d[i + 1u] == 0x266u && d[i + 2u] == v;
}

// The GPU's view of an output: at each draw, the VGT_PRIMITIVE_TYPE and VGT_GS_OUT_PRIM_TYPE in force, and the last non-NOP packet.
struct DrawAt { uint32_t at, prim, primSeen, gso, gsoSeen, lastPkt, lastLen; };
static std::vector<DrawAt> draws_of(const uint32_t *o, uint32_t n, bool *walked)
{
    std::vector<DrawAt> v;
    uint32_t i = 0, prim = 0, primSeen = 0, gso = 0, gsoSeen = 0, last = 0xFFFFFFFFu, lastLen = 0u;
    *walked = true;
    while (i < n) {
        const uint32_t l = plen(o, i, n);
        if (!l) { *walked = false; break; }
        const uint32_t h = o[i];
        if (is_draw(h)) {
            v.push_back({ i, prim, primSeen, gso, gsoSeen, last, lastLen });
        } else if (!is_nop_pkt(h) && (h >> 30) == 3u) {
            const uint32_t op = op_of(h);
            if ((op == 0x79u || op == 0x7Au) && l >= 3u) {
                const uint32_t off = o[i + 1u] & 0xFFFFu;
                for (uint32_t k = 0; k + 2u < l; k++) {
                    const uint32_t a = (0xC000u + off + k) << 2;
                    if (a == 0x30908u) { prim = o[i + 2u + k] & 0x3Fu; primSeen = 1u; }
                    if (a == 0x30998u) { gso = o[i + 2u + k]; gsoSeen = 1u; }
                }
            }
        }
        if (!is_nop_pkt(h)) { last = i; lastLen = l; }
        i += l;
    }
    return v;
}
// ORDERING over one output. rect = draws with RECTLIST in force; returns the number of violations.
static uint32_t ordering(const uint32_t *o, uint32_t n, uint32_t *rect, uint32_t *other, uint32_t *restoredImm, const char *name)
{
    bool walked = false;
    const std::vector<DrawAt> v = draws_of(o, n, &walked);
    uint32_t bad = walked ? 0u : 1u;
    *rect = *other = *restoredImm = 0u;
    bool prevRect = false;
    for (size_t k = 0; k < v.size(); k++) {
        const DrawAt &d = v[k];
        const bool r = d.primSeen && d.prim == XLAT12_R2D_PRIM_RECTLIST;
        const bool imm3 = d.lastPkt != 0xFFFFFFFFu && is_r2d_pkt(o, d.lastPkt, d.lastLen, XLAT12_R2D_OUTPRIM);
        const bool imm2 = d.lastPkt != 0xFFFFFFFFu && is_r2d_pkt(o, d.lastPkt, d.lastLen, 2u);
        if (r) {
            (*rect)++;
            if (!(d.gsoSeen && d.gso == XLAT12_R2D_OUTPRIM) || !(imm3 || prevRect)) {
                bad++;
                std::printf("  %s: RECTLIST draw at %u runs with 0x30998 %s%u%s\n", name, d.at, d.gsoSeen ? "" : "unwritten ", d.gso,
                            imm3 ? "" : " (not written immediately before it)");
            }
        } else {
            (*other)++;
            if (d.gsoSeen && d.gso == XLAT12_R2D_OUTPRIM) { bad++; std::printf("  %s: draw at %u (prim %u) runs with RECT_2D\n", name, d.at, d.prim); }
            if (prevRect && imm2) (*restoredImm)++;
        }
        prevRect = r;
    }
    return bad;
}
// NOTHING ELSE CHANGES: `on` equals today's `off` dword for dword except where `on` holds an inserted `C0017900 00000266 v` (v 3 or 2)
// over three of today's pad NOPs. Returns the number of such inserts, or -1 at the first other difference.
static int diff_is_inserts(const uint32_t *on, const uint32_t *off, uint32_t n)
{
    int k = 0;
    for (uint32_t i = 0; i < n; ) {
        if (on[i] == off[i]) { i++; continue; }
        if (i + 3u <= n && (is_r2d_pkt(on, i, 3u, XLAT12_R2D_OUTPRIM) || is_r2d_pkt(on, i, 3u, 2u)) &&
            off[i] == kNop && off[i + 1u] == kNop && off[i + 2u] == kNop) { k++; i += 3u; continue; }
        std::printf("  first other difference at %u: %08x vs today's %08x\n", i, on[i], off[i]);
        return -1;
    }
    return k;
}
static void nop_packet(uint32_t *d, uint32_t at, uint32_t n)
{
    const uint32_t l = plen(d, at, n);
    d[at] = 0xC0001000u | ((l - 2u) << 16);
    for (uint32_t k = 1; k < l; k++) d[at + k] = 0u;
}
static const n48_fixture_r92 *seg_named(const char *nm)
{
    for (uint32_t s = 0; s < N48_FIXTURE_R92_COUNT; s++) if (!std::strcmp(kR92Segs[s].name, nm)) return &kR92Segs[s];
    return nullptr;
}

// =============================================================================================================================
// T1 / T2 / T3
// =============================================================================================================================
static void test_fixtures()
{
    uint32_t totRect = 0, totWritten = 0;
    for (uint32_t s = 0; s < N48_FIXTURE_R92_COUNT; s++) {
        const n48_fixture_r92 *f = &kR92Segs[s];
        const uint32_t n = f->n;
        std::memcpy(gIn, f->dw, n * 4u);
        // T3: OFF identity against the frozen 0.0.535 translator, three modes
        for (uint32_t mode = 0; mode < 3u; mode++) {
            xlat12_draw_stats d {};
            const uint32_t st = tr(gIn, n, mode == 2u ? XLAT12_EXTRA_CS_ELIDE : 0u, gTmp, &d, mode == 0u);
            expectf((st & 0xFFFFu) == f->f_st[mode] && fnv_out(gTmp, n) == f->f_fnv[mode] && d.r2d_rect == 0u && d.r2d_fallback == 0u,
                    "T3 %s OFF identity mode %u: status %u fnv %08x == frozen 00ff5906 %u %08x", f->name, mode, st, fnv_out(gTmp, n),
                    f->f_st[mode], f->f_fnv[mode]);
        }
        xlat12_draw_stats d0 {};
        const uint32_t s0 = tr(gIn, n, 0u, gOff, &d0);
        if (f->f_st[1] != 0u) continue;   // the fallback segment: T4b
        // T1 / T2: ON
        xlat12_draw_stats d {};
        const uint32_t st = tr(gIn, n, XLAT12_EXTRA_RECT2D, gOn, &d);
        bool tail = true;
        for (uint32_t k = n; k < n + 8u; k++) tail = tail && gOn[k] == kSentinel;
        uint32_t ba = 0, bo = 0;
        expectf(st == 0u && tail && xlat12_ib_draw_verify(gOn, n, &ba, &bo) == 0u,
                "T2 %s ON: translated, the IB length unchanged (%u dwords, nothing past it), the output verifies", f->name, n);
        uint32_t nr = 0, no = 0, ri = 0;
        const uint32_t bad = ordering(gOn, n, &nr, &no, &ri, f->name);
        expectf(bad == 0u && nr == f->rect && nr + no == f->draws,
                "T1 %s ORDERING: %u RECTLIST draw(s) each with RECT_2D written immediately before it, %u other draw(s) with 2", f->name,
                nr, no);
        expectf(d.r2d_rect == f->rect && d.r2d_written == f->rect && d.r2d_restored == 0u && d.r2d_noroom == 0u && d.r2d_fallback == 0u,
                "T1 %s counters: rect %u written %u restored %u noroom %u fallback %u", f->name, d.r2d_rect, d.r2d_written,
                d.r2d_restored, d.r2d_noroom, d.r2d_fallback);
        expectf(xlat12_ib_rect2d_check(gOn, n, 2u) == 0u, "T4 %s the backstop accepts the flagged output", f->name);
        const int ins = diff_is_inserts(gOn, gOff, n + 8u);
        expectf(s0 == 0u && ins == (int)d.r2d_written,
                "T1 %s nothing else changes: the flagged output is today's except its %d insert(s) over today's pad", f->name, ins);
        if (f->rect == 0u)
            expectf(std::memcmp(gOn, gOff, (n + 8u) * 4u) == 0, "T3 %s (control, no RECTLIST draw): the flag changes no dword", f->name);
        totRect += nr; totWritten += d.r2d_written;
    }
    expectf(totRect >= 6u && totWritten == totRect, "T1 the corpus: %u RECTLIST draws, %u RECT_2D writes", totRect, totWritten);
    // F16 IB0 s5 with 57's elision translates: RECT2D on top of it
    const n48_fixture_r92 *f = seg_named("y16_0_s5");
    if (f) {
        std::memcpy(gIn, f->dw, f->n * 4u);
        xlat12_draw_stats d {};
        const uint32_t st = tr(gIn, f->n, XLAT12_EXTRA_CS_ELIDE | XLAT12_EXTRA_RECT2D, gOn, &d);
        uint32_t nr = 0, no = 0, ri = 0;
        expectf(st == 0u && ordering(gOn, f->n, &nr, &no, &ri, f->name) == 0u && nr == f->rect && d.r2d_written == f->rect,
                "T1 %s with 57's elision: translated, ORDERING holds (%u RECTLIST)", f->name, nr);
    }
}

// T1b: the restore, on a real segment with the Apple 0x28a6c write after its first RECTLIST draw planted out.
static void test_restore()
{
    const n48_fixture_r92 *f = seg_named("y125_0_s0");
    expect("T1b the fixture carries y125_0_s0 with three Apple 0x28a6c writes and two RECTLIST draws", f && f->ng == 3u && f->rect == 2u);
    if (!f) return;
    const uint32_t n = f->n;
    std::memcpy(gIn, f->dw, n * 4u);
    const uint32_t g = f->g[1];   // the second 0x28a6c write: between RECTLIST #1 and the next (indexed) draw
    expectf(gIn[g] == 0xC0016900u && gIn[g + 1u] == 0x29Bu, "T1b input dword %u is a one-register SET_CONTEXT_REG of 0x28a6c", g);
    nop_packet(gIn, g, n);
    xlat12_draw_stats d0 {}, d {};
    const uint32_t s0 = tr(gIn, n, 0u, gOff, &d0);
    uint32_t nr = 0, no = 0, ri = 0;
    bool walked = false;
    const std::vector<DrawAt> v0 = draws_of(gOff, n, &walked);
    uint32_t leak = 0;
    for (const DrawAt &x : v0) if (!(x.primSeen && x.prim == 0x11u) && x.gsoSeen && x.gso == 3u) leak++;
    const uint32_t st = tr(gIn, n, XLAT12_EXTRA_RECT2D, gOn, &d);
    const uint32_t bad = ordering(gOn, n, &nr, &no, &ri, "y125_0_s0 (0x28a6c #2 planted out)");
    expectf(s0 == 0u && st == 0u && bad == 0u && nr == 2u && ri == 1u && d.r2d_written == 2u && d.r2d_restored == 1u,
            "T1b ORDERING of the restore: TRISTRIP written immediately before the draw after RECTLIST #1 (restored %u, immediate %u, "
            "written %u, violations %u)", d.r2d_restored, ri, d.r2d_written, bad);
    expect("T1b nothing else changes (today's output but for the 3 inserts over its pad); today's own output never runs RECT_2D",
           diff_is_inserts(gOn, gOff, n + 8u) == 3 && leak == 0u);
}

// =============================================================================================================================
// T4 / T4b
// =============================================================================================================================
static int first_write(const uint32_t *o, uint32_t n, uint32_t addr, uint32_t val, uint32_t after)
{
    for (uint32_t i = 0; i < n; ) {
        const uint32_t l = plen(o, i, n);
        if (!l) return -1;
        const uint32_t h = o[i];
        if (i >= after && (h >> 30) == 3u && h != kNop && (op_of(h) == 0x79u || op_of(h) == 0x7Au) && l >= 3u) {
            const uint32_t off = o[i + 1u] & 0xFFFFu;
            for (uint32_t k = 0; k + 2u < l; k++)
                if (((0xC000u + off + k) << 2) == addr && o[i + 2u + k] == val) return (int)(i + 2u + k);
        }
        i += l;
    }
    return -1;
}
static void test_verify()
{
    const n48_fixture_r92 *f = seg_named("y125_0_s2");
    expect("T4 the fixture carries y125_0_s2", f != nullptr);
    if (!f) return;
    const uint32_t n = f->n;
    std::memcpy(gIn, f->dw, n * 4u);
    xlat12_draw_stats d {};
    const uint32_t st = tr(gIn, n, XLAT12_EXTRA_RECT2D, gOn, &d);
    expect("T4 y125_0_s2 flagged translates and the backstop accepts it (RECT_2D before its RECTLIST draw)",
           st == 0u && xlat12_ib_rect2d_check(gOn, n, 2u) == 0u);
    // (a) the RECTLIST draw's own VGT_PRIMITIVE_TYPE made TRILIST: RECT_2D now precedes a non-RECTLIST draw
    std::memcpy(gTmp, gOn, n * 4u);
    const int p = first_write(gTmp, n, 0x30908u, 0x11u, 0u);
    if (p >= 0) gTmp[p] = 4u;
    expect("T4 RECT_2D in force at a TRILIST draw: REFUSED", p >= 0 && xlat12_ib_rect2d_check(gTmp, n, 2u) == 1u);
    {   // build 0.0.548 item B: WHERE - the refusing draw's OUTPUT dword, after the TRILIST write
        uint32_t aA = 0u;
        expect("T4 (0.0.548 B) xlat12_ib_rect2d_check_at names the refusing draw's output dword (after the primitive write, inside out[])",
               p >= 0 && xlat12_ib_rect2d_check_at(gTmp, n, 2u, &aA) == 1u && aA > (uint32_t)p && aA < n);
    }
    // (b) Apple's translated 0x28a6c (2) before the indexed draw made 3
    std::memcpy(gTmp, gOn, n * 4u);
    const int q = first_write(gTmp, n, 0x30998u, 2u, 0u);
    if (q >= 0) gTmp[q] = 3u;
    expect("T4 RECT_2D in force at an indexed TRILIST draw: REFUSED", q >= 0 && xlat12_ib_rect2d_check(gTmp, n, 2u) == 1u);
    // (c) a value that is neither the profile's nor RECT_2D
    std::memcpy(gTmp, gOn, n * 4u);
    const int w = first_write(gTmp, n, 0x30998u, 3u, 0u);
    if (w >= 0) gTmp[w] = 4u;
    expect("T4 VGT_GS_OUT_PRIM_TYPE 4 (RECTLIST, the gfx10 value): REFUSED", w >= 0 && xlat12_ib_rect2d_check(gTmp, n, 2u) == 1u);
    // (d) RECT_2D with no VGT_PRIMITIVE_TYPE in the output at all (every packet writing 0x30908 turned into a same-length NOP)
    std::memcpy(gTmp, gOn, n * 4u);
    uint32_t nopped = 0;
    for (uint32_t i = 0; i < n; ) {
        const uint32_t l = plen(gTmp, i, n);
        if (!l) break;
        const uint32_t h = gTmp[i];
        if ((h >> 30) == 3u && h != kNop && op_of(h) == 0x79u && l >= 3u) {
            const uint32_t off = gTmp[i + 1u] & 0xFFFFu;
            if (off <= 0x242u && off + (l - 2u) > 0x242u) { nop_packet(gTmp, i, n); nopped++; }
        }
        i += l;
    }
    expect("T4 RECT_2D at a draw whose primitive type this output never wrote: REFUSED",
           nopped > 0u && xlat12_ib_rect2d_check(gTmp, n, 2u) == 1u);
    // (e) today's outputs are accepted as they are
    xlat12_draw_stats d0 {};
    const uint32_t s0 = tr(gIn, n, 0u, gOff, &d0);
    expect("T4 today's output (only 2 written) is accepted", s0 == 0u && xlat12_ib_rect2d_check(gOff, n, 2u) == 0u);
    expect("T4 an unwalkable stream is refused", xlat12_ib_rect2d_check(nullptr, 4u, 2u) == 1u);
    // (f) one register, one policy
    xlat12_draw_stats dx {};
    expect("T4 RECT2D with APPLE_OUTPRIM: ERR_ARG", tr(gIn, n, XLAT12_EXTRA_RECT2D | XLAT12_EXTRA_APPLE_OUTPRIM, gTmp, &dx) == XLAT12_ERR_ARG);
    expect("T4 RECT2D with SYNTH_IDXPRIM: ERR_ARG", tr(gIn, n, XLAT12_EXTRA_RECT2D | XLAT12_EXTRA_SYNTH_IDXPRIM, gTmp, &dx) == XLAT12_ERR_ARG);
    expect("T4 the constants: RECT_2D 3 and DI_PT_RECTLIST 17 (gfx12.json), new err_ops unique",
           XLAT12_R2D_OUTPRIM == 3u && XLAT12_R2D_PRIM_RECTLIST == 17u && XLAT12_R2D_NO_ROOM == 0xD2u && XLAT12_R2D_BACKSTOP == 0xD3u &&
           XLAT12_EXTRA_RECT2D == 0x400000u && (XLAT12_EXTRA_RECT2D & XLAT12_EXTRA_NCLEAR) == 0u);
}

static uint32_t pad_before_(const uint32_t *o, uint32_t at)
{
    uint32_t k = 0;
    while (k < at && o[at - 1u - k] == kNop) k++;
    return k;
}
// Fix round item 2: RECT_2D may not be in force at the END of an output.
static void test_end_rule()
{
    // (a) the real flagged outputs: none ends under RECT_2D (0 effect) - T4's per-fixture acceptance covers each; counted here
    uint32_t okN = 0, tot = 0;
    for (uint32_t s = 0; s < N48_FIXTURE_R92_COUNT; s++) {
        const n48_fixture_r92 *f = &kR92Segs[s];
        if (f->f_st[1] != 0u) continue;
        std::memcpy(gIn, f->dw, f->n * 4u);
        xlat12_draw_stats d {};
        tot++;
        if (tr(gIn, f->n, XLAT12_EXTRA_RECT2D, gOn, &d) == 0u && d.r2d_fallback == 0u && xlat12_ib_rect2d_check(gOn, f->n, 2u) == 0u) okN++;
    }
    expectf(tot == 6u && okN == tot, "E1 the end rule changes nothing on the real fixtures (%u of %u translate flagged, no fallback)", okN, tot);
    // (b) a planted segment that ends under RECT_2D: y125_0_s2 with its Apple 0x28a6c after the RECTLIST draw and its indexed
    // draw turned into same-length NOPs - its only draw is the RECTLIST
    const n48_fixture_r92 *f = seg_named("y125_0_s2");
    if (!f) { expect("E1 the fixture carries y125_0_s2", false); return; }
    const uint32_t n = f->n;
    std::memcpy(gIn, f->dw, n * 4u);
    nop_packet(gIn, f->g[0], n);
    uint32_t lastDraw = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; ) { const uint32_t l = plen(gIn, i, n); if (!l) break; if (is_draw(gIn[i])) lastDraw = i; i += l; }
    const bool idx = lastDraw != 0xFFFFFFFFu && op_of(gIn[lastDraw]) == 0x27u;
    if (idx) nop_packet(gIn, lastDraw, n);
    xlat12_draw_stats d0 {}, d {};
    const uint32_t s0 = tr(gIn, n, 0u, gOff, &d0);
    const uint32_t st = tr(gIn, n, XLAT12_EXTRA_RECT2D, gOn, &d);
    expectf(idx && s0 == 0u && d0.draws == 1u && st == 0u && d.r2d_fallback == 1u && d.r2d_on_st == XLAT12_IB_ERR_VERIFY &&
            d.r2d_rect == 1u && std::memcmp(gOn, gOff, (n + 8u) * 4u) == 0,
            "E2 a segment whose last draw is a RECTLIST: the flagged pass is REFUSED by the end rule (on_st %u) and today's output "
            "stands, byte for byte (fallback %u)", d.r2d_on_st, d.r2d_fallback);
    // (c) the check itself, on that output: RECT_2D written into the pad before the draw -> refused; TRISTRIP after the draw -> accepted
    bool walked = false;
    const std::vector<DrawAt> v = draws_of(gOff, n, &walked);
    const uint32_t at = v.empty() ? 0u : v[0].at, pad = at ? pad_before_(gOff, at) : 0u;
    std::memcpy(gTmp, gOff, n * 4u);
    bool placed = false;
    if (pad >= 3u) { gTmp[at - 3u] = 0xC0017900u; gTmp[at - 2u] = 0x266u; gTmp[at - 1u] = 3u; placed = true; }
    expect("E3 an output that ends with RECT_2D in force: REFUSED", placed && xlat12_ib_rect2d_check(gTmp, n, 2u) == 1u);
    uint32_t tail = n;
    while (tail > 0u && gTmp[tail - 1u] == kNop) tail--;
    bool restored = false;
    if (n - tail >= 3u) { gTmp[tail] = 0xC0017900u; gTmp[tail + 1u] = 0x266u; gTmp[tail + 2u] = 2u; restored = true; }
    expect("E3 the same output with TRISTRIP written back after the draw: accepted", restored && xlat12_ib_rect2d_check(gTmp, n, 2u) == 0u);
    {   // build 0.0.548 item B: the accepted output answers 0 at 0; the end rule answers the output length
        uint32_t a0 = 7u, a1 = 7u;
        const uint32_t r0 = xlat12_ib_rect2d_check_at(gTmp, n, 2u, &a0);
        if (restored) { gTmp[tail] = kNop; gTmp[tail + 1u] = kNop; gTmp[tail + 2u] = kNop; }
        const uint32_t r1 = xlat12_ib_rect2d_check_at(gTmp, n, 2u, &a1);
        expect("E3 (0.0.548 B) check_at: accepted -> 0 at 0; the end rule -> 1 at n (the output length); same answer as the old check",
               restored && r0 == 0u && a0 == 0u && r1 == 1u && a1 == n && xlat12_ib_rect2d_check(gTmp, n, 2u) == 1u);
    }
}

static uint32_t pad_before(const uint32_t *o, uint32_t at)
{
    uint32_t k = 0;
    while (k < at && o[at - 1u - k] == kNop) k++;
    return k;
}
static void test_fallback()
{
    // (a) a real segment that refuses after its RECTLIST write: F16 IB0 s5, UNLISTED (its compute dispatch without 57)
    const n48_fixture_r92 *f = seg_named("y16_0_s5");
    expect("T4b the fixture carries y16_0_s5 (frozen: UNLISTED without 57)", f && f->f_st[1] == 20u);
    if (f) {
        std::memcpy(gIn, f->dw, f->n * 4u);
        xlat12_draw_stats d {};
        const uint32_t st = tr(gIn, f->n, XLAT12_EXTRA_RECT2D, gOn, &d);
        expectf(st == f->f_st[1] && fnv_out(gOn, f->n) == f->f_fnv[1] && d.r2d_fallback == 1u && d.r2d_on_st == 20u &&
                d.r2d_rect == 1u && d.r2d_written == 0u,
                "T4b a refusal after the write: today's answer exactly (status %u, fnv %08x), fallback %u, on_st %u, rect %u",
                st, fnv_out(gOn, f->n), d.r2d_fallback, d.r2d_on_st, d.r2d_rect);
    }
    // (b) no room: a real segment whose pad before its RECTLIST draw is planted away (dropped packets -> same-length NOPs)
    const n48_fixture_r92 *g = seg_named("y125_0_s2");
    if (!g) return;
    const uint32_t n = g->n;
    std::memcpy(gIn, g->dw, n * 4u);
    xlat12_draw_stats d0 {};
    uint32_t s0 = tr(gIn, n, 0u, gOff, &d0);
    bool walked = false;
    std::vector<DrawAt> v = draws_of(gOff, n, &walked);
    uint32_t rAt = 0xFFFFFFFFu;
    for (const DrawAt &x : v) if (x.primSeen && x.prim == 0x11u) { rAt = x.at; break; }
    uint32_t pad = rAt != 0xFFFFFFFFu ? pad_before(gOff, rAt) : 0u, planted = 0;
    const uint32_t pad0 = pad;
    uint32_t from = 0;
    for (const DrawAt &x : v) if (x.at < rAt) from = x.at + plen(gIn, x.at, n);
    for (uint32_t i = from; pad >= 3u && i < rAt; ) {
        const uint32_t l = plen(gIn, i, n);
        if (!l) break;
        if (!is_nop_pkt(gIn[i])) {
            std::memcpy(gTmp, gIn, n * 4u);
            nop_packet(gTmp, i, n);
            xlat12_draw_stats dt {};
            static uint32_t o2[8192 + 8];
            if (tr(gTmp, n, 0u, o2, &dt) == 0u && pad_before(o2, rAt) < pad) {
                std::memcpy(gIn, gTmp, n * 4u); pad = pad_before(o2, rAt); planted++;
            }
        }
        i += l;
    }
    s0 = tr(gIn, n, 0u, gOff, &d0);
    xlat12_draw_stats d {};
    const uint32_t st = tr(gIn, n, XLAT12_EXTRA_RECT2D, gOn, &d);
    expectf(s0 == 0u && pad0 >= 3u && pad < 3u && planted > 0u && st == 0u && std::memcmp(gOn, gOff, (n + 8u) * 4u) == 0 &&
            d.r2d_noroom == 1u && d.r2d_fallback == 1u && d.r2d_on_st == XLAT12_IB_ERR_TOO_LONG && d.r2d_written == 0u,
            "T4b no room (pad %u -> %u by %u planted NOPs): today's output byte for byte, noroom %u fallback %u on_st %u", pad0, pad,
            planted, d.r2d_noroom, d.r2d_fallback, d.r2d_on_st);
    // build 0.0.547 item 3: the flagged pass's reason survives the fallback (no room: 0xD2 at 0x30998)
    expectf(d.r2d_on_op == XLAT12_R2D_NO_ROOM && d.r2d_on_reg == 0x30998u && d.r2d_on_dw != 0u,
            "F3 no room: the flagged pass's refusal is kept across the fallback (on_op %#x reg %#x dword %u)", d.r2d_on_op, d.r2d_on_reg,
            d.r2d_on_dw);
}

// =============================================================================================================================
// T5 / T6
// =============================================================================================================================
static void test_image()
{
    uint32_t diff = 0, at = 0;
    for (uint32_t k = 0; k < 64u; k++) if (kR92ImgOld[k] != kR92ImgV3[k]) { diff++; at = k; }
    const uint32_t o = kR92ImgOld[20], v = kR92ImgV3[20];
    expectf(diff == 1u && at == 20u && o == 0x7E000B00u && v == 0x7E000B03u,
            "T5 the v3 image differs from today's in ONE dword, +0x50: %08x -> %08x", o, v);
    // VOP1 (gfx12): [31:25] 0x3F, VDST [24:17], OP [16:9], SRC0 [8:0] (256 + n = vn): V_CVT_F32_I32 is VOP1 opcode 5
    expectf((v >> 25) == 0x3Fu && ((v >> 17) & 0xFFu) == 0u && ((v >> 9) & 0xFFu) == 5u && (v & 0x1FFu) == 0x103u && (o & 0x1FFu) == 0x100u,
            "T5 +0x50 decodes as v_cvt_f32_i32 v0, v3 (today: v0, v0)");
    const int io = xlat12_shader_id_match(1u, kR92ImgOld, 64u), iv = xlat12_shader_id_match(1u, kR92ImgV3, 64u);
    const char *no = io >= 0 ? xlat12_shader_id_name(io) : "", *nv = iv >= 0 ? xlat12_shader_id_name(iv) : "";
    expectf(io >= 0 && iv >= 0 && !std::strcmp(no, "RectPosTexFast_VS_attr_gfx1201") && !std::strcmp(nv, "RectPosTexFast_VS_attr_v3_gfx1201"),
            "T5 each image identifies to its own row (%s / %s)", no, nv);
    xlat12_draw_profile po {}, pv {};
    uint32_t ioo[2] = { 0, 0 }, iov[2] = { 0, 0 };
    expect("T5 the v3 row's profile words and parameter count are the original's (RSRC1/2_GS, io 1, RING)",
           io >= 0 && iv >= 0 && xlat12_ib_profile_stage(io, &po, ioo) == 0u && xlat12_ib_profile_stage(iv, &pv, iov) == 0u &&
           po.vs_rsrc1_gs == pv.vs_rsrc1_gs && po.vs_rsrc2_gs == pv.vs_rsrc2_gs && ioo[0] == 1u && iov[0] == 1u &&
           xlat12_shader_id_desc_class(io) == 2u && xlat12_shader_id_desc_class(iv) == 2u &&   /* 2 = RING (xlat12_shader_desc.h) */
           po.vs_readset1 != 0u && pv.vs_readset1 != 0u && po.vs_readset1 != pv.vs_readset1);
}

static void test_pick()
{
    static uint8_t out[512];
    sc_cache c2 {}, c1 {};
    const int o2 = sc_open(&c2, reinterpret_cast<const uint8_t *>(kR92BlobTwo), sizeof kR92BlobTwo);
    const int o1 = sc_open(&c1, reinterpret_cast<const uint8_t *>(kR92BlobOne), sizeof kR92BlobOne);
    expectf(o2 == SC_OK && o1 == SC_OK && sc_entry_count(&c2) == 2u && sc_entry_count(&c1) == 1u,
            "T6 both blobs open (m4c-r20's two entries; the base alone): %d %d", o2, o1);
    // the resource as the residency copy reads it: Apple's 32 dwords, then the slot's zero padding
    static uint8_t res[256];
    std::memset(res, 0, sizeof res);
    std::memcpy(res, kR92Apple, sizeof kR92Apple);
    sc_match m {};
    const int lk = sc_lookup(&c2, res, sizeof res, &m);
    expectf(lk == SC_OK && m.verified == 1u && m.key == N48_R92_KEY && m.apple_dwords == 32u,
            "T6 the lookup verifies today's entry ALONE over Apple's bytes + padding (st %d, apple %u)", lk, m.apple_dwords);
    sc_match alt {}; uint32_t why = 99u, nb = 0;
    const sc_match *u = n48_rv92_pick(&c2, &m, 0u, &alt, &why);
    const int r0 = sc_subst_render(&c2, u, 1, out, sizeof out, &nb);
    expectf(u == &m && why == N48_RV92_OLD && r0 == SC_OK && nb == 256u && std::memcmp(out, kR92ImgOld, 256) == 0,
            "T6 OFF: today's image, byte for byte (%u B)", nb);
    u = n48_rv92_pick(&c2, &m, 1u, &alt, &why);
    const int r1 = sc_subst_render(&c2, u, 1, out, sizeof out, &nb);
    expectf(u == &alt && why == N48_RV92_NEW && r1 == SC_OK && nb == 256u && std::memcmp(out, kR92ImgV3, 256) == 0 && alt.key == m.key &&
            alt.capacity_bytes == 256u && alt.apple_dwords == 64u,
            "T6 ON: the v3 image, byte for byte (%u B)", nb);
    sc_match m1 {};
    const int lk1 = sc_lookup(&c1, res, sizeof res, &m1);
    u = n48_rv92_pick(&c1, &m1, 1u, &alt, &why);
    const int r2 = sc_subst_render(&c1, u, 1, out, sizeof out, &nb);
    expect("T6 ON with a blob holding no alternative (r19's shape): today's image, counted NOALT",
           lk1 == SC_OK && u == &m1 && why == N48_RV92_NOALT && r2 == SC_OK && std::memcmp(out, kR92ImgOld, 256) == 0);
    sc_match other = m; other.key ^= 1ull;
    u = n48_rv92_pick(&c2, &other, 1u, &alt, &why);
    expect("T6 ON, any other key: untouched (no alternative is even looked for)", u == &other && why == N48_RV92_OLD);
    // the alternative can never be matched by a lookup: its own stored bytes (Apple's + marker) are AMBIGUOUS, not the alt
    static uint8_t resm[256];
    std::memcpy(resm, kR92Apple, sizeof kR92Apple);
    for (uint32_t k = 32; k < 64u; k++) { const uint32_t mk = k == 32u ? 0x4E343852u : (0x4E343800u | (0xA0u + (k - 32u))); std::memcpy(resm + 4u * k, &mk, 4); }
    sc_match ma {};
    expect("T6 Apple's bytes followed by the marker: AMBIGUOUS - nothing substituted (fail-safe)",
           sc_lookup(&c2, resm, sizeof resm, &ma) == SC_E_AMBIGUOUS);
    sc_match unv = m; unv.verified = 0u;
    expect("T6 sc_alt_find refuses an unverified match (SC_E_NOSUB) and zeroes *alt", sc_alt_find(&c2, &unv, &alt) == SC_E_NOSUB && alt.key == 0ull);
    expect("T6 sc_alt_find with no alternative: SC_MISS", sc_alt_find(&c1, &m1, &alt) == SC_MISS);
}

// =============================================================================================================================
// T7: the kext's wiring
// =============================================================================================================================
static bool has(const std::string &t, const char *s) { return t.find(s) != std::string::npos; }
static size_t count_of(const std::string &t, const char *s)
{
    size_t k = 0, at = 0; const size_t m = std::strlen(s);
    while ((at = t.find(s, at)) != std::string::npos) { k++; at += m; }
    return k;
}
static bool order(const std::string &t, std::initializer_list<const char *> steps)
{
    size_t prev = 0; bool first = true;
    for (const char *st : steps) {
        const size_t at = t.find(st, first ? 0 : prev);
        if (at == std::string::npos) { std::printf("  order: missing after %zu: %s\n", prev, st); return false; }
        prev = at + 1; first = false;
    }
    return true;
}
static std::string fn_text(const std::string &src, const char *head)
{
    const size_t a = src.find(head);
    if (a == std::string::npos) return std::string();
    size_t i = src.find('{', a + std::strlen(head) - 1u);
    if (i == std::string::npos) return std::string();
    int depth = 0;
    for (size_t j = i; j < src.size(); j++) {
        if (src[j] == '{') depth++;
        else if (src[j] == '}') { if (--depth == 0) return src.substr(a, j - a + 1u); }
    }
    return std::string();
}
static std::string slurp(const char *p)
{
    std::ifstream in(p ? p : "");
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}
static void test_wiring(const char *ahhPath, const char *peerPath, const char *xlatPath)
{
    const std::string ahh = slurp(ahhPath), peer = slurp(peerPath);
    expect("T7 AppleHardwareHook.cpp (argv[1]) and Navi48AccelPeer.cpp (argv[2]) were read", ahh.size() > 100000u && peer.size() > 50000u);
    expect("T7 switch 92 is OFF at boot and changed by the verb alone",
           has(ahh, "static volatile uint32_t gRect92On { 0u };") && count_of(ahh, "__atomic_store_n(&gRect92On, ") == 1u &&
           count_of(ahh, "gRect92On = ") == 0u);
    expect("T7 the selector: 92 via n48_ra_set (M1 348 ON, M2 604 OFF), mid-arm guarded, an unknown M refused, the report",
           order(ahh, { "} else if ((arg & 0xffull) == 92ull) {",
                        "const bool contRefused92 = n48_cm_cont_switch_refused(92u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;",
                        "if (contRefused92) st = 5;",
                        "else { changed92 = n48_ra_set(m, &f92); if (changed92) __atomic_store_n(&gRect92On, f92, __ATOMIC_RELEASE); }",
                        "if (!contRefused92 && !changed92 && m != 0u) st = 11;", "rect92_report_line(contRefused92 ?" }) &&
           (92u | 1u << 8) == 348u && (92u | 2u << 8) == 604u);
    expect("T7 the mid-arm guard: 92 refused while a continuous arm stands, a read allowed; 114 unclaimed (112: 0.0.554; 111: 0.0.553; 109/110: 0.0.552; 93: 0.0.537, 94/95: 0.0.538; 107/108: 0.0.550)",
           n48_cm_cont_switch_guarded(92u) == 1u && n48_cm_cont_switch_refused(92u, 0u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 1u &&
           n48_cm_cont_switch_refused(92u, 1u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 0u && n48_cm_cont_switch_guarded(114u) == 0u);   /* build 0.0.541: 98 and 99 are claimed; 0.0.543: 100-102; 0.0.544: 103; 0.0.547: 104-105; 0.0.548: 106; 0.0.550: 107-108 */
    const std::string pol = fn_text(ahh, "static void gfxsrc_policy(const GfxcVm &vm, n48_xv_frame *f, uint32_t n, uint64_t ibVa, uint32_t dp,");
    expect("T7 the policy: 92 read once per pass, WindowServer's frames only; the flag beside 91's; the kext's check right after the "
           "translate, before the copy guard",
           order(pol, { "const uint32_t r2On = (gRect92On && wsBound) ? 1u : 0u;",
                        "if (r2On) { ex.flags |= XLAT12_EXTRA_RECT2D; gRect92S.segsOn++; }",
                        "uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from], to - from,",
                        "if (ex.flags & XLAT12_EXTRA_NCLEAR) nclear_seg(vm, k, &st, &ds, out, olen);",
                        "if (ex.flags & XLAT12_EXTRA_RECT2D) rect92_seg(&st, &ds, out, olen);",
                        "const uint32_t cgReason = navi48_cg_seg_check();" }) &&
           count_of(ahh, "ex.flags |= XLAT12_EXTRA_RECT2D") == 1u && count_of(ahh, "rect92_seg(&st, &ds, out, olen)") == 1u);
    const std::string seg = fn_text(ahh, "void rect92_seg(uint32_t *st, xlat12_draw_stats *ds, const uint32_t *out, uint32_t olen)");
    expect("T7 rect92_seg: every flagged segment counted; a TRANSLATED one checked by xlat12_ib_rect2d_check; a failure refuses VERIFY/0xD3",
           order(seg, { "gRect92S.rect += ds->r2d_rect;", "gRect92S.fallback += ds->r2d_fallback;",
                        "if (!*st && xlat12_ib_rect2d_check(out, olen, xlat12_ib_m2tri_profile()->gs_out_prim_type) != 0u) {",
                        "*st = XLAT12_IB_ERR_VERIFY; ds->err_op = XLAT12_R2D_BACKSTOP;" }));
    expect("T7 hw_rv92_on reads the switch; hw_rv92_note counts NEW and NOALT",
           has(ahh, "bool hw_rv92_on() { return __atomic_load_n(&gRect92On, __ATOMIC_ACQUIRE) != 0u; }") &&
           has(ahh, "if (why == N48_RV92_NEW) __atomic_fetch_add(&gRect92SubNew, 1ull, __ATOMIC_RELAXED);"));
    const std::string hit = fn_text(peer, "static int shadercache_hit(void *vctx, size_t off, const sc_match *m)");
    expect("T7 shadercache_hit: the image is n48_rv92_pick's, rendered from it; counted only after the write and its read-back",
           order(hit, { "const sc_match *use92 = n48_rv92_pick(&gSc, m, n48::hw_rv92_on() ? 1u : 0u, &alt92, &why92);",
                        "const int rst = sc_subst_render(&gSc, use92, (int)gScAcceptAdjust, gScOut, sizeof gScOut, &nb);",
                        "ok = navi48_vram_write_mm(at + o, w, nd) && navi48_vram_read_mm(at + o, b, nd);",
                        "if (why92 != N48_RV92_OLD) n48::hw_rv92_note(why92);", "gScSt.substituted++;" }));
    const std::string ic = fn_text(peer, "static int ic_hit(void *vctx, size_t off, const sc_match *m)");
    expect("T7 ic_hit (switch 62's in-copy patch) makes the same choice; its Apple re-check bytes stay the verified match's",
           order(ic, { "const sc_match *use92 = n48_rv92_pick(&gSc, m, n48::hw_rv92_on() ? 1u : 0u, &alt92, &why92);",
                       "const int rst = sc_subst_render(&gSc, use92, (int)gScAcceptAdjust, p->arena + p->arenaUsed, kScMaxSubstBytes, &nb);",
                       "const uint32_t anb = m->apple_dwords * 4u;" }));
    expect("T7 no residency writer renders the verified match directly any more", count_of(peer, "sc_subst_render(&gSc, m,") == 0u &&
           count_of(peer, "sc_subst_render(&gSc, use92,") == 2u);
    // Fix round item 1 (the stack): the fallback is a second pass of xlat12_ib_translate_draw_ex's own block, never a second frame.
    const std::string xl = slurp(xlatPath);
    const std::string tde = fn_text(xl, "uint32_t xlat12_ib_translate_draw_ex(const xlat12_draw_profile *pf, const xlat12_draw_extra *ex, const uint32_t *in,");
    expect("T7 xlat12_ib.c (argv[3]) was read", xl.size() > 100000u && tde.size() > 30000u);
    expect("T7 STACK: the fallback re-runs the SAME block (goto d_r2d_pass, retry state in locals), the body never calls itself, and no "
           "separate translate/fallback function exists",
           order(tde, { "uint32_t r2dCtl = (ex && (ex->flags & XLAT12_EXTRA_RECT2D)) ? 1u : 0u, r2dSt = 0u;",
                        "d_r2d_pass:", "d_r2d_done:", "r2dCtl = 2u | ", "goto d_r2d_pass;", "return r2dSt;" }) &&
           count_of(tde, "xlat12_ib_translate_draw_ex(") == 1u && count_of(tde, "\n    return ") == 1u &&
           !has(xl, "d_translate_draw(") && !has(xl, "d_r2d_fallback(") && !has(tde, "static uint32_t r2dCtl"));
    char b[1024];
    const unsigned long long M = 18446744073709551615ull;
    const int w = std::snprintf(b, sizeof b, N48_CM_RECT92_FMT, "OFF (604, default)", "`gfxneuter 92` REFUSED - a continuous arm stands, unchanged",
                                " - the v3 image reaches only programs paged in while ON", M, M, M, M, M, M, M, M, M);
    expectf(w > 0 && (uint32_t)w <= N48_LOG_CAP_BODY, "T7 the rect92 report fits 491 bytes at its widest (%d)", w);
    expect("T7 N48_LOG_CAP_BODY is 491", N48_LOG_CAP_BODY == 491u);
}

// =============================================================================================================================
// build 0.0.547 item 3 ( fix (3); gfx_rectfb105.h, xlat12_ib_rectlist_draws): SWITCH 105, A RECTLIST FALLBACK NEVER
// COMMITS AS A TRIANGLE.
//   F1 the real planted fixture (E2: y125_0_s2 ending on its RECTLIST draw): the flagged pass is refused by the end rule (0xD3), the
//      fallback translates, and its RECTLIST draw runs with NO RECT_2D in force (TRISTRIP: one triangle) - OFF keeps it (commits),
//      ON refuses, SHADOW counts;
//   F2 non-RECTLIST fallbacks and refused fallbacks are unchanged in every mode; the scanner agrees with the flagged pass's own
//      RECTLIST count on every real fixture; a refused fallback keeps its flagged pass's own error (y16_0_s5);
//   F3 the reason (no room: 0xD2) survives the fallback (in test_fallback);
//   F4 an unwalkable output counts as a RECTLIST fallback (fail closed);
//   F5 the lines fit 491 bytes;  F6 the glue (source pins);  F7 SHADOW and OFF never refuse.
static void test_rectfb(const char *ahhPath, const char *xlatPath)
{
    const n48_fixture_r92 *f = seg_named("y125_0_s2");
    if (!f) { expect("F1 the fixture carries y125_0_s2", false); return; }
    const uint32_t n = f->n;
    std::memcpy(gIn, f->dw, n * 4u);
    nop_packet(gIn, f->g[0], n);
    uint32_t lastDraw = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; ) { const uint32_t l = plen(gIn, i, n); if (!l) break; if (is_draw(gIn[i])) lastDraw = i; i += l; }
    if (lastDraw != 0xFFFFFFFFu && op_of(gIn[lastDraw]) == 0x27u) nop_packet(gIn, lastDraw, n);
    xlat12_draw_stats d {};
    const uint32_t st = tr(gIn, n, XLAT12_EXTRA_RECT2D, gOn, &d);
    uint32_t last = 0u;
    const uint32_t nr = xlat12_ib_rectlist_draws(gOn, n, &last);
    bool walked = false;
    const std::vector<DrawAt> v = draws_of(gOn, n, &walked);
    uint32_t rectAt = 0xFFFFFFFFu, rectGso = 0xFFu, rectGsoSeen = 0u, anyR2d = 0u;
    for (const DrawAt &x : v) if (x.primSeen && x.prim == XLAT12_R2D_PRIM_RECTLIST) { rectAt = x.at; rectGso = x.gso; rectGsoSeen = x.gsoSeen; }
    for (uint32_t i = 0; i + 2u < n; i++) if (is_r2d_pkt(gOn, i, 3u, XLAT12_R2D_OUTPRIM)) anyR2d = 1u;
    expectf(st == 0u && d.r2d_fallback == 1u && d.r2d_on_st == XLAT12_IB_ERR_VERIFY && d.r2d_on_op == XLAT12_R2D_BACKSTOP &&
            d.r2d_on_reg == 0x30998u && nr == 1u && last == 1u && walked && rectAt != 0xFFFFFFFFu &&
            !(rectGsoSeen && rectGso == XLAT12_R2D_OUTPRIM) && anyR2d == 0u,
            "F1 the end-rule fallback: flagged pass refused (status %u, on_op %#x reg %#x), fallback translated (status %u) with %u RECTLIST "
            "draw(s) (last %u) at dword %u running under 0x30998 %s%u - NO RECT_2D anywhere: the GPU draws ONE TRIANGLE", d.r2d_on_st,
            d.r2d_on_op, d.r2d_on_reg, st, nr, last, rectAt, rectGsoSeen ? "" : "unwritten (the profile's TRISTRIP) ", rectGso);
    n48_rfb_st so {}, sn {}, ss {};
    const uint32_t aOff = n48_rfb_seg(&so, N48_RFB_M_OFF, d.r2d_fallback, st, nr, last, d.r2d_on_op);
    const uint32_t aOn = n48_rfb_seg(&sn, N48_RFB_M_ON, d.r2d_fallback, st, nr, last, d.r2d_on_op);
    const uint32_t aSh = n48_rfb_seg(&ss, N48_RFB_M_SHADOW, d.r2d_fallback, st, nr, last, d.r2d_on_op);
    expectf(aOff == N48_RFB_A_KEEP && aOn == N48_RFB_A_REFUSE && aSh == N48_RFB_A_WOULD && so.fb_rect == 1u && so.fb_rect_last == 1u &&
            so.by_op_backstop == 1u && so.refused == 0u && sn.refused == 1u && ss.would_refuse == 1u && ss.refused == 0u,
            "F1 OFF keeps the triangle (commits as 0.0.546), ON refuses the segment, SHADOW counts it (%u/%u/%u)", aOff, aOn, aSh);

    // F2: the scanner against the flagged pass's own count on every real segment that translates flagged without a fallback, and the
    // control (no RECTLIST) segments
    uint32_t agree = 0u, tot = 0u, zeroCtl = 1u;
    for (uint32_t s2 = 0; s2 < N48_FIXTURE_R92_COUNT; s2++) {
        const n48_fixture_r92 *g = &kR92Segs[s2];
        std::memcpy(gIn, g->dw, g->n * 4u);
        xlat12_draw_stats dOn {}, dOff {};
        const uint32_t sOn = tr(gIn, g->n, XLAT12_EXTRA_RECT2D, gOn, &dOn), sOff = tr(gIn, g->n, 0u, gOff, &dOff);
        if (sOn != 0u || sOff != 0u || dOn.r2d_fallback) continue;
        tot++;
        uint32_t l2 = 0u;
        if (xlat12_ib_rectlist_draws(gOff, g->n, &l2) == dOn.r2d_rect && xlat12_ib_rectlist_draws(gOn, g->n, &l2) == dOn.r2d_rect) agree++;
        if (dOn.r2d_rect == 0u && xlat12_ib_rectlist_draws(gOff, g->n, &l2) != 0u) zeroCtl = 0u;
    }
    expectf(tot >= 5u && agree == tot && zeroCtl, "F2 the scanner equals the flagged pass's RECTLIST count on %u of %u real segments "
            "(OFF and ON outputs); a control segment reads 0", agree, tot);
    uint32_t unchanged = 1u;
    for (uint32_t m = 0u; m < 5u; m++) {
        n48_rfb_st z {};
        if (n48_rfb_seg(&z, m, 1u, 0u, 0u, 0u, 0xD3u) != N48_RFB_A_NONE || z.fb_rect || z.refused || z.would_refuse) unchanged = 0u;
        if (n48_rfb_seg(&z, m, 0u, 0u, 3u, 1u, 0xD3u) != N48_RFB_A_NONE) unchanged = 0u;
        if (n48_rfb_seg(&z, m, 1u, 20u, 0u, 0u, 0x15u) != N48_RFB_A_NONE) unchanged = 0u;
    }
    expect("F2 a non-RECTLIST fallback, a segment that did not fall back and a refused fallback: NONE in every mode (nothing refused)", unchanged);
    const n48_fixture_r92 *y = seg_named("y16_0_s5");
    if (y) {
        std::memcpy(gIn, y->dw, y->n * 4u);
        xlat12_draw_stats dy {}, d0 {};
        const uint32_t sy = tr(gIn, y->n, XLAT12_EXTRA_RECT2D, gOn, &dy), s0 = tr(gIn, y->n, 0u, gOff, &d0);
        expectf(sy == 20u && s0 == 20u && dy.r2d_fallback == 1u && dy.r2d_on_st == 20u && dy.r2d_on_op == (d0.err_op & 0xFFu) &&
                dy.r2d_on_dw == d0.err_in_dword, "F2 a refusal after the write (y16_0_s5, UNLISTED): the flagged pass's own err_op %#x at "
                "dword %u is kept (the unflagged pass refuses at the same %#x / %u)", dy.r2d_on_op, dy.r2d_on_dw, d0.err_op, d0.err_in_dword);
    } else expect("F2 the fixture carries y16_0_s5", false);

    // F4
    uint32_t bad[4] = { 0xC0FF7900u, 0x266u, 3u, 0u }, lb = 7u;
    const uint32_t unw = xlat12_ib_rectlist_draws(bad, 4u, &lb);
    n48_rfb_st zu {};
    expect("F4 an unwalkable output answers XLAT12_RL_UNWALKED and counts as a RECTLIST fallback: ON refuses it",
           unw == XLAT12_RL_UNWALKED && lb == 0u && n48_rfb_seg(&zu, N48_RFB_M_ON, 1u, 0u, unw, 0u, 0xD3u) == N48_RFB_A_REFUSE &&
           zu.fb_unwalked == 1u && xlat12_ib_rectlist_draws(nullptr, 4u, nullptr) == XLAT12_RL_UNWALKED);

    // F5
    char b[1024];
    n48_rfb_st mx; std::memset(&mx, 0xff, sizeof mx);
    int w = 0, wmax = 0;
    for (uint32_t m = 1u; m <= 3u; m++) {
        w = std::snprintf(b, sizeof b, N48_RFB_FMT, N48_RFB_ARGS(m, " - `gfxneuter 105` REFUSED - a continuous arm stands, unchanged", &mx));
        if (w > wmax) wmax = w;
    }
    w = std::snprintf(b, sizeof b, N48_RFB_FMT2, N48_RFB_ARGS2(&mx));
    if (w > wmax) wmax = w;
    xlat12_draw_stats dm; std::memset(&dm, 0xff, sizeof dm);
    for (uint32_t a = 0u; a <= 3u; a++) {
        w = std::snprintf(b, sizeof b, N48_RFB_SEG_FMT, N48_RFB_SEG_ARGS(~0ull, 4294967295u, 4294967295u, &dm, 4294967295u, 4294967295u, 4294967295u, a));
        if (w > wmax) wmax = w;
    }
    expectf(wmax > 0 && (uint32_t)wmax <= N48_LOG_CAP_BODY, "F5 every rectfb line fits 491 bytes at its widest (%d)", wmax);

    // F6
    const std::string ahh = slurp(ahhPath), xl = slurp(xlatPath);
    const std::string pol = fn_text(ahh, "static void gfxsrc_policy(const GfxcVm &vm, n48_xv_frame *f, uint32_t n, uint64_t ibVa, uint32_t dp,");
    expect("F6 the policy: rectfb_seg right after rect92's backstop, before the copy guard; the per-pass tally zeroed at the top",
           order(pol, { "for (uint32_t z = 0; z < N48_XV_MAX_SEGS; z++) gRectFbSeg[z] = 0u;",
                        "if (ex.flags & XLAT12_EXTRA_RECT2D) rect92_seg(&st, &ds, out, olen);",
                        "if (ex.flags & XLAT12_EXTRA_RECT2D) rectfb_seg(k, from, &st, &ds, out, olen);",
                        "const uint32_t cgReason = navi48_cg_seg_check();" }) && count_of(ahh, "rectfb_seg(k, from, &st, &ds, out, olen)") == 1u);
    const std::string rs = fn_text(ahh, "void rectfb_seg(uint32_t k, uint32_t from, uint32_t *st, xlat12_draw_stats *ds, const uint32_t *out,");
    expect("F6 rectfb_seg: counts every fallback, scans only a translated one, and writes *st ONLY on ON's REFUSE (VERIFY / 0xD5)",
           order(rs, { "if (!ds->r2d_fallback) return;", "const uint32_t nrect = fst ? 0u : xlat12_ib_rectlist_draws(out, olen, &last);",
                       "n48_rfb_seg(&gRectFbS, __atomic_load_n(&gRectFbMode, __ATOMIC_RELAXED), 1u, fst, nrect, last, ds->r2d_on_op);",
                       "if (act == N48_RFB_A_REFUSE) { *st = XLAT12_IB_ERR_VERIFY; ds->err_op = XLAT12_RFB_REFUSED; ds->err_reg = 0x30908u; }" }) &&
           count_of(rs, "*st =") == 1u);
    expect("F6 switch 105 OFF at boot, the verb the only writer, mid-arm guarded; committed frames tallied after the gate",
           has(ahh, "static volatile uint32_t gRectFbMode { N48_RFB_M_OFF };") && count_of(ahh, "__atomic_store_n(&gRectFbMode, ") == 1u &&
           count_of(ahh, "gRectFbMode = ") == 0u && has(ahh, "const bool contRefused105 = n48_cm_cont_switch_refused(105u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;") &&
           n48_cm_cont_switch_guarded(105u) == 1u && has(ahh, "    if (commitOk && gRect92On) rectfb_commit();") &&
           ahh.find("    if (commitOk && gRect92On) rectfb_commit();") > ahh.find("? gfxsrc_commit_try(vm, info, ib0Va, f.ib[0].got, f.nib, arm, verdict, stampNow, tgtVa) : 0u;"));
    const std::string tde = fn_text(xl, "uint32_t xlat12_ib_translate_draw_ex(const xlat12_draw_profile *pf, const xlat12_draw_extra *ex, const uint32_t *in,");
    expect("F6 xlat12_ib.c: the flagged pass's err_op/err_reg/dword captured on the RECT_2D fallback branch, before its goto, and restored "
           "with r2d_fallback",
           order(tde, { "uint32_t r2dOnA = 0u, r2dOnB = 0u;", "d_r2d_pass:", "d_r2d_done:",
                        "r2dCtl = 2u | ((r2dSt < 0xFFu ? r2dSt : 0xFFu) << 8) | ((uint32_t)ds->r2d_rect << 16) | ((uint32_t)ds->r2d_noroom << 24);",
                        "r2dOnA = ((ds->err_op & 0xFFu) << 24) | (ds->err_reg & 0xFFFFFFu); r2dOnB = ds->err_in_dword;",
                        "goto d_r2d_pass;   /* the fallback: the same block, bit 0 clear - today's translation (never a second frame) */",
                        "ds->r2d_rect = (uint8_t)(r2dCtl >> 16); ds->r2d_noroom = (uint8_t)(r2dCtl >> 24); ds->r2d_fallback = 1u;",
                        "ds->r2d_on_op = r2dOnA >> 24; ds->r2d_on_reg = r2dOnA & 0xFFFFFFu; ds->r2d_on_dw = r2dOnB;", "return r2dSt;" }));
    // build 0.0.548 items B and C
    expect("F6 (0.0.548 B) the flagged pass's backstop records WHERE it refused in err_in_dword (xlat12_ib_rect2d_check_at), and the "
           "reason line labels that position an OUT dword for 0xD3",
           has(xl, "if ((r2dCtl & 1u) && xlat12_ib_rect2d_check_at(out, n, pf->gs_out_prim_type, &r2dAt)) {") &&
           has(xl, "ds->err_op = XLAT12_R2D_BACKSTOP; ds->err_reg = 0x30998u; ds->err_in_dword = r2dAt;") &&
           order(tde, { "ds->err_op = 0xFFFFFFFFu; ds->err_in_dword = 0;", "uint32_t r2dAt = 0u;",
                        "ds->err_op = XLAT12_R2D_BACKSTOP; ds->err_reg = 0x30998u; ds->err_in_dword = r2dAt;", "d_r2d_done:" }));
    {
        xlat12_draw_stats d3 {}; d3.r2d_on_op = XLAT12_R2D_BACKSTOP; d3.r2d_on_dw = 77u;
        xlat12_draw_stats d2 {}; d2.r2d_on_op = XLAT12_R2D_NO_ROOM; d2.r2d_on_dw = 55u;
        char b3[600], b2[600];
        std::snprintf(b3, sizeof b3, N48_RFB_SEG_FMT, N48_RFB_SEG_ARGS(1ull, 0u, 0u, &d3, 0u, 1u, 0u, 1u));
        std::snprintf(b2, sizeof b2, N48_RFB_SEG_FMT, N48_RFB_SEG_ARGS(1ull, 0u, 0u, &d2, 0u, 1u, 0u, 1u));
        expect("F6 (0.0.548 B) the reason line: 0xD3 prints `at out dword 77`, 0xD2 `at in dword 55`",
               std::strstr(b3, "at out dword 77;") != nullptr && std::strstr(b2, "at in dword 55;") != nullptr);
    }
    expect("F6 (0.0.548 C) rectfb_seg spends its 8 reason lines only on fallbacks that CARRY a RECTLIST draw (nrect > 0); the counting "
           "above is unchanged", order(rs, { "const uint32_t nrect = fst ? 0u : xlat12_ib_rectlist_draws(out, olen, &last);",
                                             "if (nrect != 0u && gRectFbArmLines < N48_RFB_LINES_MAX) {",
                                             "HWLOG(N48_RFB_SEG_FMT," }) && count_of(rs, "gRectFbArmLines < N48_RFB_LINES_MAX") == 1u);
    // F7
    uint32_t never = 1u;
    for (uint32_t fb = 0; fb < 2u; fb++) for (uint32_t s3 = 0; s3 < 30u; s3 += 5u) for (uint32_t nr2 = 0; nr2 < 4u; nr2++)
        for (uint32_t l3 = 0; l3 < 2u; l3++) {
            n48_rfb_st z {};
            if (n48_rfb_seg(&z, N48_RFB_M_SHADOW, fb, s3, nr2, l3, 0xD3u) == N48_RFB_A_REFUSE ||
                n48_rfb_seg(&z, N48_RFB_M_OFF, fb, s3, nr2, l3, 0xD3u) == N48_RFB_A_REFUSE || z.refused) never = 0u;
        }
    expect("F7 SHADOW and OFF never answer REFUSE (and never count a refusal)", never);
}

int main(int argc, char **argv)
{
    test_fixtures();
    test_restore();
    test_verify();
    test_fallback();
    test_image();
    test_pick();
    test_end_rule();
    test_rectfb(argc > 1 ? argv[1] : nullptr, argc > 3 ? argv[3] : nullptr);
    test_wiring(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr);
    std::printf("gfx_rect92: %d checks, %d failed\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
