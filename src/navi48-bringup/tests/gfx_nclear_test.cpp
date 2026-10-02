// gfx_nclear_test.cpp — build 0.0.535 ( fix 1 and item 4): SWITCH 91, the CP DMA_DATA zero fill in place of Apple's
// compute clear N (xlat12_ib.h XLAT12_EXTRA_NCLEAR), and the draw's own CB0/scissor for tex531 (xlat12_tex_state, gfx_p87.h).
//
// Over REAL captured IBs (tests/fixture_nclear_run11v.h: run11v F118 IB1 - the study's example, dispatch at IB dword 4487 -, F18 IB0,
// F17 IB0, F18 IB1, F38 IB1, F75 IB1: every colour-target shape N clears in the capture), with the REAL translator:
//   T1  a fill: the translation is OK and the IB length unchanged; the packets decode (AMD's gfx12 DMA_DATA layout: ENGINE 0,
//       DST_SEL 3, SRC_SEL 2 = data, CP_SYNC 1, data 0, BYTE_COUNT only, <= 32736 bytes each) and cover EXACTLY [CB0_BASE, DCC_BASE)
//       of the target the generator derived independently from the captured registers; no DISPATCH_DIRECT remains; the public
//       verify still refuses a DMA_DATA (the backstop stays strict) and the kext's check passes; every packet that is not N's compute
//       run or dispatch is emitted exactly as 57 alone emits it, and every draw stays at its own dword; ORDERING: every fill packet
//       carries CP_SYNC and precedes Apple's own CS_PARTIAL_FLUSH + ACQUIRE_MEM, which precede the next draw.
//   T2  the targets whose fill does not fit N's own dwords (384 KiB x3, 1 MiB): 57's NOP stands, byte-identical to 57 alone,
//       counted `room`.
//   T3  OFF IDENTITY: today's translator with no extra block and with 57 alone answers exactly as the FROZEN 0.0.534 translator
//       (48740edf, the fixture's recorded status / length / elisions / FNV-1a of the whole output); the tex_state observer never
//       changes a dword.
//   T4  refusals: the flag without 57 (ERR_ARG); a mutated extent (size, format, another surface) falls back `extent`; the packet
//       check's mutations; the kext's check over mutated outputs.
//   T5  tex_state: the translation's own last CB0 / window-scissor writes, zeroed at every translation; the tex531 line prints the
//       draw's own CB0 and scissor and fits 491 bytes.
//   T6  the kext's wiring (AppleHardwareHook.cpp, argv[1]): default OFF, the selector and its mid-arm guard, the per-pass latch (57 ON
//       and WindowServer only), the flag, the kext check placed right after the translate and before the copy guard, the page walk,
//       the report lines (<= 491 bytes), and tex_state only under 87.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 -x c++ src/navi48-bringup/tests/gfx_nclear_test.cpp \
//         src/xlat12/xlat12_ib.c src/xlat12/xlat12.c -o /tmp/nc && /tmp/nc src/navi48-bringup/src/apple/AppleHardwareHook.cpp
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <initializer_list>

#include "xlat12.h"
#include "xlat12_ib.h"
#include "gfx_commit.h"
#include "gfx_p87.h"
#include "fixture_nclear_run11v.h"

static int gChecks = 0, gFails = 0;
static void expect(const char *what, bool ok)
{
    gChecks++;
    if (!ok) gFails++;
    if (!ok || std::getenv("NC_VERBOSE")) std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static void expectf(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void expectf(bool ok, const char *fmt, ...)
{
    char b[512];
    va_list ap; va_start(ap, fmt); std::vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    expect(b, ok);
}

static int isn(void *, uint64_t va) { return va == 0x400017a00ull; }
static const uint32_t kSentinel = 0xDEADBEEFu;   // the generator's own prefill (the frozen FNV covers a refused output too)
static uint32_t gIn[8192], gOn[8192 + 8], g57[8192 + 8], gTmp[8192 + 8];

static uint32_t tr(const uint32_t *in, uint32_t n, uint32_t flags, uint32_t *out, xlat12_draw_stats *ds, xlat12_tex_state *ts = nullptr,
                   bool noEx = false)
{
    xlat12_draw_extra ex; std::memset(&ex, 0, sizeof ex);
    ex.flags = flags; ex.cs_is_n = (flags & XLAT12_EXTRA_CS_ELIDE) ? &isn : nullptr; ex.tex_state = ts;
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
    if (h == XLAT12_IB_NOP || (h >> 30) == 2u) return 1u;
    if ((h >> 30) != 3u) return 0u;
    const uint32_t l = ((h >> 16) & 0x3FFFu) + 2u;
    return i + l <= n ? l : 0u;
}
static uint32_t op_of(uint32_t h) { return (h >> 8) & 0xFFu; }
static bool is_draw(uint32_t h) { const uint32_t o = op_of(h); return (h >> 30) == 3u && h != XLAT12_IB_NOP && (o == 0x2Du || o == 0x27u || o == 0x35u || o == 0x24u || o == 0x25u); }
static bool is_cs_set(const uint32_t *p, uint32_t l)
{
    const uint32_t o = op_of(p[0]), off = p[1] & 0xFFFFu;
    return (p[0] >> 30) == 3u && (o == 0x76u || o == 0x9Bu) && l >= 3u && off >= 0x200u && off + (l - 2u) <= 0x280u;
}
// The packets of out[from, n) as (offset, length), skipping the one-dword pad NOPs.
struct Pk { uint32_t at, l; };
static std::vector<Pk> packets(const uint32_t *d, uint32_t from, uint32_t n)
{
    std::vector<Pk> v;
    for (uint32_t i = from; i < n; ) {
        const uint32_t l = plen(d, i, n);
        if (!l) { v.push_back({ i, 0u }); break; }
        if (d[i] != XLAT12_IB_NOP) v.push_back({ i, l });
        i += l;
    }
    return v;
}

// =============================================================================================================================
// T1 / T2 / T3
// =============================================================================================================================
static void test_fixtures()
{
    for (uint32_t s = 0; s < N48_FIXTURE_NC_COUNT; s++) {
        const n48_fixture_nc *f = &kNcSegs[s];
        const uint32_t n = f->n;
        std::memcpy(gIn, f->dw, n * 4u);
        const bool fits = (f->size + XLAT12_NCLEAR_MAX_BYTES - 1u) / XLAT12_NCLEAR_MAX_BYTES * 7u <= 73u;
        // T3: OFF identity against the frozen 0.0.534 translator
        xlat12_draw_stats d0 {}, d1 {}, dts {};
        const uint32_t s0 = tr(gIn, n, 0u, gTmp, &d0, nullptr, true);
        expectf((s0 & 0xFFFFu) == f->f_st0 && fnv_out(gTmp, n) == f->f_fnv0,
                "T3 %s OFF identity (no extra block): status %u fnv %08x == frozen 48740edf %u %08x", f->name, s0, fnv_out(gTmp, n),
                f->f_st0, f->f_fnv0);
        const uint32_t s1 = tr(gIn, n, XLAT12_EXTRA_CS_ELIDE, g57, &d1);
        expectf(s1 == f->f_st1 && d1.cs_elided == f->f_el1 && fnv_out(g57, n) == f->f_fnv1 && f->f_len1 == n,
                "T3 %s OFF identity (57 alone, 91 OFF): status %u elided %u fnv %08x == frozen %u %u %08x", f->name, s1, d1.cs_elided,
                fnv_out(g57, n), f->f_st1, f->f_el1, f->f_fnv1);
        xlat12_tex_state ts {};
        const uint32_t sts = tr(gIn, n, XLAT12_EXTRA_CS_ELIDE, gTmp, &dts, &ts);
        expectf(sts == s1 && std::memcmp(gTmp, g57, (n + 8u) * 4u) == 0, "T3 %s the tex_state observer changes no dword", f->name);
        // ON
        xlat12_draw_stats d {};
        const uint32_t st = tr(gIn, n, XLAT12_EXTRA_CS_ELIDE | XLAT12_EXTRA_NCLEAR, gOn, &d);
        expectf(st == 0u && gOn[n] == kSentinel && gOn[n + 7u] == kSentinel && d.cs_elided == 1u,
                "T1 %s ON: translated, the IB length unchanged (%u dwords, nothing written past it), still one elision (%u)", f->name,
                n, d.cs_elided);
        if (!fits) {
            expectf(d.nc_filled == 0u && d.nc_ref_room == 1u && d.nc_ref_extent == 0u && d.nc_ref_run == 0u &&
                    std::memcmp(gOn, g57, (n + 8u) * 4u) == 0,
                    "T2 %s (%#x bytes = %u packets > 73 dwords): 57's NOP stands, byte-identical to 57 alone, counted room", f->name,
                    f->size, (f->size + XLAT12_NCLEAR_MAX_BYTES - 1u) / XLAT12_NCLEAR_MAX_BYTES);
            continue;
        }
        const uint32_t k = (f->size + XLAT12_NCLEAR_MAX_BYTES - 1u) / XLAT12_NCLEAR_MAX_BYTES;
        expectf(d.nc_filled == 1u && d.nc_pkts == k && d.nc_bytes == f->size && ((uint64_t)d.nc_va8[0] << 8) == f->cb0 &&
                d.nc_len[0] == f->size && !d.nc_ref_extent && !d.nc_ref_room && !d.nc_ref_run,
                "T1 %s fills %#llx + %#x in %u packets (recorded %#llx + %#x, %u packets)", f->name, (unsigned long long)f->cb0, f->size,
                k, (unsigned long long)d.nc_va8[0] << 8, d.nc_len[0], d.nc_pkts);
        // decode every DMA_DATA; coverage; no dispatch
        const std::vector<Pk> on = packets(gOn, 0u, n);
        uint64_t next = f->cb0; uint32_t nfill = 0u, ndisp = 0u, firstFill = 0u, lastFill = 0u, fieldsOk = 1u, syncOk = 1u;
        for (const Pk &p : on) {
            if (!p.l) { fieldsOk = 0u; break; }
            if (op_of(gOn[p.at]) == 0x15u) ndisp++;
            if (op_of(gOn[p.at]) != 0x50u) continue;
            const uint32_t *q = &gOn[p.at];
            const uint32_t ctl = q[1];
            if (!nfill) firstFill = p.at;
            lastFill = p.at; nfill++;
            if (p.l != 7u || (ctl & 1u) != 0u || ((ctl >> 20) & 3u) != 3u || ((ctl >> 29) & 3u) != 2u || q[2] || q[3] ||
                (q[6] & ~0x3FFFFFFu) || !q[6] || q[6] > 32736u || !xlat12_ib_nclear_pkt_ok(q, p.l)) fieldsOk = 0u;
            if (!((ctl >> 31) & 1u)) syncOk = 0u;
            const uint64_t dst = (uint64_t)q[4] | ((uint64_t)q[5] << 32);
            if (dst != next) fieldsOk = 0u;
            next = dst + q[6];
        }
        expectf(fieldsOk && nfill == k && next == f->dcc && next == f->cb0 + f->size,
                "T1 %s the fill decodes (ENGINE 0, DST_SEL 3, SRC_SEL data, data 0, BYTE_COUNT only) and covers EXACTLY [%#llx, %#llx) "
                "(ends %#llx)", f->name, (unsigned long long)f->cb0, (unsigned long long)f->dcc, (unsigned long long)next);
        expectf(ndisp == 0u, "T1 %s no DISPATCH_DIRECT in the output", f->name);
        uint32_t ba = 0, bo = 0;
        expectf(xlat12_ib_draw_verify(gOn, n, &ba, &bo) == XLAT12_IB_ERR_VERIFY && bo == 0x50u && xlat12_ib_nclear_check(gOn, n, &d) == 0u,
                "T1 %s the public verify still refuses a DMA_DATA (op %#x); the kext's check passes", f->name, bo);
        // ORDERING
        uint32_t draw = n, ew = n, acq = n;
        for (const Pk &p : on) if (p.at > lastFill && is_draw(gOn[p.at])) { draw = p.at; break; }
        for (const Pk &p : on) if (p.at > lastFill && p.at < draw && op_of(gOn[p.at]) == 0x46u && gOn[p.at + 1u] == 0x407u) { ew = p.at; break; }
        for (const Pk &p : on) if (p.at > ew && p.at < draw && op_of(gOn[p.at]) == 0x58u) { acq = p.at; break; }
        expectf(syncOk && firstFill < lastFill + 1u && lastFill < ew && ew < acq && acq < draw && draw == f->next_draw,
                "T1 %s ORDERING: every fill carries CP_SYNC and precedes CS_PARTIAL_FLUSH (@%u) and ACQUIRE_MEM (@%u), which precede "
                "the next draw (@%u, captured %u)", f->name, ew, acq, draw, f->next_draw);
        // every non-elided packet untouched: [0, run start) identical; after, the packet sequences agree once the fill (ON) and N's
        // compute run + the NOP (57) are skipped; every draw at its own dword in both
        const uint32_t p0 = firstFill;
        bool same = std::memcmp(gOn, g57, p0 * 4u) == 0;
        const std::vector<Pk> a = packets(gOn, p0, n), b = packets(g57, p0, n);
        size_t ia = 0, ib = 0;
        while (ia < a.size() && op_of(gOn[a[ia].at]) == 0x50u) ia++;
        uint32_t skipped = 0u;
        while (ib < b.size() && b[ib].l && is_cs_set(&g57[b[ib].at], b[ib].l)) { ib++; skipped++; }
        const bool nopOk = ib < b.size() && g57[b[ib].at] == XLAT12_CS_ELIDE_NOP && b[ib].l == 5u;
        if (nopOk) ib++;
        same = same && nopOk && skipped > 0u && (a.size() - ia) == (b.size() - ib);
        for (; same && ia < a.size() && ib < b.size(); ia++, ib++) {
            if (a[ia].l != b[ib].l || std::memcmp(&gOn[a[ia].at], &g57[b[ib].at], a[ia].l * 4u) != 0) same = false;
            if (is_draw(gOn[a[ia].at]) && a[ia].at != b[ib].at) same = false;
        }
        expectf(same, "T1 %s every packet outside N's compute run (%u packets) and dispatch is emitted as 57 alone emits it, every draw at "
                "its own dword", f->name, skipped);
    }
}

// =============================================================================================================================
// T4: refusals and the checks' mutations
// =============================================================================================================================
static uint32_t find_ctx(const uint32_t *d, uint32_t from, uint32_t to, uint32_t g10)
{
    for (uint32_t i = from; i < to; ) {
        const uint32_t l = plen(d, i, to);
        if (!l) break;
        if ((d[i] >> 30) == 3u && d[i] != XLAT12_IB_NOP && op_of(d[i]) == 0x69u) {
            const uint32_t off = d[i + 1u] & 0xFFFFu;
            for (uint32_t k = 0; k + 2u < l; k++) if (((0xa000u + off + k) << 2) == g10) return i + 2u + k;
        }
        i += l;
    }
    return 0xFFFFFFFFu;
}
static void test_refusals()
{
    const n48_fixture_nc *f = &kNcSegs[0];   // F118 IB1
    const uint32_t n = f->n, disp = f->disp;
    xlat12_draw_stats d {};
    std::memcpy(gIn, f->dw, n * 4u);
    expect("T4 the flag without 57's is refused ERR_ARG", tr(gIn, n, XLAT12_EXTRA_NCLEAR, gOn, &d) == XLAT12_ERR_ARG);
    struct Mut { const char *what; uint32_t reg, xorv; } muts[] = {
        { "ATTRIB2 width + 128 (the tiled size no longer equals DCC_BASE - BASE)", 0x28ec0u, 0x80u << 14 },
        { "INFO FORMAT 12 -> 13 (not a 4- or 8-byte colour format)", 0x28c70u, 1u << 2 },
        { "CB_COLOR0_BASE names another surface (+64 KiB)", 0x28c60u, 0x100u },
        { "ATTRIB3 swizzle 27 -> 26", 0x28ee0u, 1u << 14 },
        { "VIEW slice 1", 0x28c6cu, 1u },
    };
    for (const Mut &m : muts) {
        std::memcpy(gIn, f->dw, n * 4u);
        const uint32_t at = find_ctx(gIn, disp + 5u, f->next_draw, m.reg);
        if (at == 0xFFFFFFFFu) { expectf(false, "T4 %s: register %#x not found in the window", m.what, m.reg); continue; }
        gIn[at] ^= m.xorv;
        xlat12_draw_stats d57 {}, dn {};
        const uint32_t s57 = tr(gIn, n, XLAT12_EXTRA_CS_ELIDE, g57, &d57);
        const uint32_t sn = tr(gIn, n, XLAT12_EXTRA_CS_ELIDE | XLAT12_EXTRA_NCLEAR, gOn, &dn);
        expectf(sn == s57 && dn.nc_filled == 0u && dn.nc_ref_extent == 1u && std::memcmp(gOn, g57, (n + 8u) * 4u) == 0,
                "T4 %s: no fill, counted extent, output == 57 alone", m.what);
    }
    // the packet check
    const uint32_t good[7] = { XLAT12_NCLEAR_HDR, XLAT12_NCLEAR_CTRL, 0u, 0u, 0x01240000u, 4u, 32736u };
    uint32_t p[7];
    expect("T4 pkt_ok: the fill packet", xlat12_ib_nclear_pkt_ok(good, 7u) == 1);
    struct PM { const char *what; uint32_t at, v; } pm[] = {
        { "no CP_SYNC", 1u, XLAT12_NCLEAR_CTRL & ~0x80000000u }, { "DST_SEL 0 (DAS)", 1u, XLAT12_NCLEAR_CTRL & ~0x00300000u },
        { "SRC_SEL memory", 1u, XLAT12_NCLEAR_CTRL & ~0x60000000u }, { "data 1", 2u, 1u }, { "src_hi 1", 3u, 1u },
        { "destination 16-byte aligned", 4u, 0x01240010u }, { "destination >= 2^48", 5u, 0x10000u }, { "0 bytes", 6u, 0u },
        { "32737 bytes", 6u, 32737u }, { "32740 bytes (over the cap)", 6u, 32740u }, { "DAS register (bit 27)", 6u, 32u | (1u << 27) },
        { "a DMA_DATA of another count", 0u, 0xC0045000u },
    };
    for (const PM &m : pm) { std::memcpy(p, good, sizeof p); p[m.at] = m.v; expectf(xlat12_ib_nclear_pkt_ok(p, 7u) == 0, "T4 pkt_ok refuses: %s", m.what); }
    expect("T4 pkt_ok refuses a 6-dword packet", xlat12_ib_nclear_pkt_ok(good, 6u) == 0);
    // the kext's check over mutated outputs
    std::memcpy(gIn, f->dw, n * 4u);
    xlat12_draw_stats dd {};
    (void)tr(gIn, n, XLAT12_EXTRA_CS_ELIDE | XLAT12_EXTRA_NCLEAR, gOn, &dd);
    uint32_t first = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; ) { const uint32_t l = plen(gOn, i, n); if (!l) break; if (op_of(gOn[i]) == 0x50u) { first = i; break; } i += l; }
    expect("T4 nclear_check passes the translation's own output", first != 0xFFFFFFFFu && xlat12_ib_nclear_check(gOn, n, &dd) == 0u);
    if (first == 0xFFFFFFFFu) return;
    std::memcpy(gTmp, gOn, n * 4u); gTmp[first + 4u] += 0x1000u;
    expect("T4 nclear_check refuses a fill that starts a page late", xlat12_ib_nclear_check(gTmp, n, &dd) == 1u);
    std::memcpy(gTmp, gOn, n * 4u); gTmp[first] = 0xC0051000u;
    expect("T4 nclear_check refuses a fill with a packet missing", xlat12_ib_nclear_check(gTmp, n, &dd) == 1u);
    xlat12_draw_stats d2 = dd; d2.nc_va8[0] += 0x10u;
    expect("T4 nclear_check refuses a recorded fill the output does not carry", xlat12_ib_nclear_check(gOn, n, &d2) == 1u);
    d2 = dd; d2.nc_pkts++;
    expect("T4 nclear_check refuses a packet count that differs", xlat12_ib_nclear_check(gOn, n, &d2) == 1u);
}

// =============================================================================================================================
// T5: tex_state and the tex531 line
// =============================================================================================================================
static void test_tex_state()
{
    const n48_fixture_nc *f = &kNcSegs[0];
    const uint32_t n = f->n;
    std::memcpy(gIn, f->dw, n * 4u);
    uint32_t cb0 = 0u, wtl = 0u, wbr = 0u, seen = 0u;
    for (uint32_t i = 0; i < n; ) {
        const uint32_t l = plen(gIn, i, n);
        if (!l) break;
        if ((gIn[i] >> 30) == 3u && gIn[i] != XLAT12_IB_NOP && op_of(gIn[i]) == 0x69u) {
            const uint32_t off = gIn[i + 1u] & 0xFFFFu;
            for (uint32_t k = 0; k + 2u < l; k++) {
                const uint32_t g10 = (0xa000u + off + k) << 2, v = gIn[i + 2u + k];
                if (g10 == 0x28c60u) { cb0 = v; seen |= 1u; } else if (g10 == 0x28204u) { wtl = v; seen |= 4u; } else if (g10 == 0x28208u) { wbr = v; seen |= 8u; }
            }
        }
        i += l;
    }
    xlat12_tex_state ts {}; ts.seen = 0xFFu; ts.cb0 = 0xDEADu;
    xlat12_draw_stats d {};
    (void)tr(gIn, n, XLAT12_EXTRA_CS_ELIDE, gOn, &d, &ts);
    expectf((ts.seen & 0xDu) == seen && ts.cb0 == cb0 && ts.win_tl == wtl && ts.win_br == wbr,
            "T5 tex_state holds the translation's own last CB0 %#x and window scissor %08x/%08x (seen %#x)", ts.cb0, ts.win_tl, ts.win_br, ts.seen);
    xlat12_tex_state t2 {}; t2.seen = 0xFFu; t2.cb0 = 7u;
    const uint32_t nop[2] = { XLAT12_IB_NOP, XLAT12_IB_NOP };
    (void)tr(nop, 2u, 0u, gOn, &d, &t2);
    expect("T5 tex_state is zeroed at every translation (nothing inherited)", t2.seen == 0u && t2.cb0 == 0u);
    n48_p87 s {}; s.on = N48_P87_ON;
    uint32_t rec[8] = { 0u };
    const uint64_t P = ((uint64_t)46u << 32) | 0xebaa377cu;
    expect("T5 p87: a kept record takes the draw's state", n48_p87_note(&s, 3u, P, 5u, 38u, rec) == 1u);
    n48_p87_note_state(&s, 0xDu, 0x04012400u, 0u, 0x83c70370u, 0x03e30410u);
    const n48_p87_rec &e = s.r[0];
    expect("T5 p87: the draw's CB0 VA and window scissor (880,967)-(1040,995) decode",
           n48_p87_cb0_va(&e) == 0x401240000ull && n48_p87_sc_x(e.st_win_tl) == 880u && n48_p87_sc_y(e.st_win_tl) == 967u &&
           n48_p87_sc_x(e.st_win_br) == 1040u && n48_p87_sc_y(e.st_win_br) == 995u);
    char b[1024];
    const unsigned long long M = 18446744073709551615ull;
    const int w = std::snprintf(b, sizeof b, N48_P87_FMT, M, M, " inh", 32767u, 32767u, 32767u, 32767u, " inh", M, "UCF", 4294967295u,
                                4294967295u, 4294967295u, 4294967295u, M, 4294967295u, 4294967295u, 4294967295u, 4294967295u,
                                4294967295u, 4294967295u, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
                                0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu);
    expectf(w > 0 && (uint32_t)w <= N48_LOG_CAP_BODY, "T5 the tex531 line fits 491 bytes at its widest (%d)", w);
}

// =============================================================================================================================
// T6: the kext's wiring
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
        if (at == std::string::npos) return false;
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
static void test_wiring(const char *ahhPath)
{
    std::ifstream in(ahhPath ? ahhPath : "");
    std::stringstream ss; ss << in.rdbuf();
    const std::string ahh = ss.str();
    expect("T6 AppleHardwareHook.cpp was read (argv[1])", ahh.size() > 100000u);
    expect("T6 switch 91 is OFF at boot", has(ahh, "static volatile uint32_t gNclearOn { 0u };") && count_of(ahh, "gNclearOn = ") == 1u &&
           count_of(ahh, "if (changed91) gNclearOn = f91;") == 1u);
    expect("T6 the selector: 91 via n48_ra_set (M1 347 ON, M2 603 OFF), mid-arm guarded, an unknown M refused, the report",
           order(ahh, { "} else if ((arg & 0xffull) == 91ull) {",
                        "const bool contRefused91 = n48_cm_cont_switch_refused(91u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;",
                        "if (contRefused91) st = 5;", "else { changed91 = n48_ra_set(m, &f91); if (changed91) gNclearOn = f91; }",
                        "if (!contRefused91 && !changed91 && m != 0u) st = 11;", "nclear_report_line(contRefused91 ?" }) &&
           (91u | 1u << 8) == 347u && (91u | 2u << 8) == 603u);
    expect("T6 the mid-arm guard: 91 refused while a continuous arm stands, a read allowed; 114 unclaimed (112: 0.0.554; 111: 0.0.553; 109/110: 0.0.552; 92: 0.0.536, 93: 0.0.537, 94/95: 0.0.538; 107/108: 0.0.550)",
           n48_cm_cont_switch_guarded(91u) == 1u && n48_cm_cont_switch_refused(91u, 0u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 1u &&
           n48_cm_cont_switch_refused(91u, 1u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 0u && n48_cm_cont_switch_guarded(114u) == 0u);   /* build 0.0.541: 98 and 99 are claimed; 0.0.543: 100-102; 0.0.544: 103; 0.0.547: 104-105; 0.0.550: 107-108 */
    const std::string pol = fn_text(ahh, "static void gfxsrc_policy(const GfxcVm &vm, n48_xv_frame *f, uint32_t n, uint64_t ibVa, uint32_t dp,");
    expect("T6 the policy: 91 read once per pass, only with 57 ON and for WindowServer's own frame; the flag beside 57's; the kext's check "
           "right after the translate (and 56's retry), before the copy guard",
           order(pol, { "const uint32_t csOn = gCsElideOn ? 1u : 0u;", "const uint32_t ncOn = (gNclearOn && csOn && wsBound) ? 1u : 0u;",
                        "if (csOn) { ex.flags |= XLAT12_EXTRA_CS_ELIDE;", "if (ncOn) { ex.flags |= XLAT12_EXTRA_NCLEAR; gNclearS.segsOn++; }",
                        "uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from], to - from,",
                        "gXdBuild.csElided += ds.cs_elided;", "if (ex.flags & XLAT12_EXTRA_NCLEAR) nclear_seg(vm, k, &st, &ds, out, olen);",
                        "const uint32_t cgReason = navi48_cg_seg_check();" }) &&
           count_of(ahh, "ex.flags |= XLAT12_EXTRA_NCLEAR") == 1u && count_of(ahh, "nclear_seg(vm, k, &st, &ds, out, olen)") == 1u);
    const std::string seg = fn_text(ahh, "void nclear_seg(const GfxcVm &vm, uint32_t k, uint32_t *st,");
    const std::string pg = fn_text(ahh, "uint32_t nclear_pages_ok(const GfxcVm &vm, const xlat12_draw_stats *ds)");
    expect("T6 nclear_seg: only a translated segment that filled is checked; the packets THEN the pages; a failure refuses VERIFY/0xDC",
           order(seg, { "if (!ds->nc_filled) return;", "if (!*st && (xlat12_ib_nclear_check(out, olen, ds) != 0u || !nclear_pages_ok(vm, ds))) {",
                        "*st = XLAT12_IB_ERR_VERIFY; ds->err_op = XLAT12_NCLEAR_BACKSTOP;" }) && XLAT12_NCLEAR_BACKSTOP == 0xDCu);
    const std::string pr = fn_text(ahh, "static uint32_t nclear_probe(void *ctx, uint64_t va, uint32_t *big)");
    expect("T6 nclear_pages_ok: every byte of every recorded fill walked by n48_nc_pages_walk over the frame's own VM; an unrecorded "
           "fill refuses",
           order(pg, { "if (ds->nc_filled > XLAT12_NCLEAR_MAX) return 0u;",
                       "if (!n48_nc_pages_walk(&nclear_probe, const_cast<GfxcVm *>(&vm), va, ds->nc_len[q], &gNclearS.pages)) { gNclearS.pagesBad++; return 0u; }" }));
    expect("(e) nclear_probe: gfxc_page's root / L1 (va >> 16) / sub-table (va >> 12) walk; big only when the L1 entry is the leaf "
           "(bit 63); system memory refused",
           order(pr, { "const uint32_t rootIdx = (uint32_t)((va - vm.startVa) >> 28);", "l1Base + ((va >> 16) & 0xfffull) * 8u",
                       "if (!((l1Ent >> 63) & 1ull)) {", "subBase + ((va >> 12) & 0xfull) * 8u", "*big = 1u;",
                       "return ((leaf >> 1) & 1ull) ? 0u : 1u;" }));
    // build 0.0.544 DELIBERATE re-baseline: switch 103 (ON / SHADOW) reads the same state (its self-read exemption, its boxes).
    expect("T6 tex_state is handed to the translation only while 87 is ON or 103 is ON / SHADOW, and read at each kept T# record",
           has(ahh, "if (__atomic_load_n(&gP87.on, __ATOMIC_ACQUIRE) == N48_P87_ON || n48_st_active(st103_mode())) ex.tex_state = &gP87Ts;") &&
           has(ahh, "n48_p87_note_state(&gP87, gP87Ts.seen, gP87Ts.cb0, gP87Ts.cb0_ext, gP87Ts.win_tl, gP87Ts.win_br);") &&
           count_of(ahh, "ex.tex_state = ") == 1u);
    char b[1024];
    const unsigned long long M = 18446744073709551615ull;
    int w = std::snprintf(b, sizeof b, N48_CM_NCLEAR_FMT, "OFF (603, default)", "`gfxneuter 91` REFUSED - a continuous arm stands, unchanged",
                          " - 57 is OFF: nothing is filled", M, M, M, M, M, M, M, M, M, M, M, M);
    expectf(w > 0 && (uint32_t)w <= N48_LOG_CAP_BODY, "T6 the nclear91 report fits 491 bytes at its widest (%d)", w);
    w = std::snprintf(b, sizeof b, N48_CM_NCLEAR_ONE_FMT, M, 4294967295u, M, M, 4294967295u, 4294967295u,
                      "REFUSED by the kext's check (packets or pages)");
    expectf(w > 0 && (uint32_t)w <= N48_LOG_CAP_BODY, "T6 the nclear91 per-fill line fits (%d)", w);
    expect("T6 the per-fill lines are capped (16 per boot)", N48_CM_NCLEAR_LINES == 16u && has(ahh, "gNclearS.lines < N48_CM_NCLEAR_LINES"));
}

// =============================================================================================================================
// FIX ROUND (a): Apple's barriers around N, each one missing on the real fixtures; (b): the third fill; (e): the page walk.
// =============================================================================================================================
static void nop_out(uint32_t *d, uint32_t at, uint32_t n)
{
    const uint32_t l = plen(d, at, n);
    d[at] = 0xC0001000u | ((l - 2u) << 16);
    for (uint32_t k = 1; k < l; k++) d[at + k] = 0u;
}
static void test_barriers()
{
    for (uint32_t s = 0; s < 2u; s++) {                 // the two targets that fill: f118_ib1, f18_ib0
        const n48_fixture_nc *f = &kNcSegs[s];
        const uint32_t n = f->n, disp = f->disp;
        uint32_t rel = ~0u, wait = ~0u, acq = ~0u, ew2 = ~0u, acq2 = ~0u;
        for (uint32_t i = 0; i < disp; ) {
            const uint32_t l = plen(f->dw, i, n); if (!l) break;
            const uint32_t o = op_of(f->dw[i]);
            if ((f->dw[i] >> 30) == 3u && f->dw[i] != XLAT12_IB_NOP) {
                if (o == 0x49u && f->dw[i + 1u] == 0x514u) rel = i;
                if (o == 0x3Cu) wait = i;
                if (o == 0x58u) acq = i;
            }
            i += l;
        }
        ew2 = disp + 5u; acq2 = ew2 + plen(f->dw, ew2, n);
        expectf(rel < wait && wait < acq && acq < disp && op_of(f->dw[ew2]) == 0x46u && f->dw[ew2 + 1u] == 0x407u &&
                op_of(f->dw[acq2]) == 0x58u && (f->dw[acq + 7u] & (1u << 15)) && (f->dw[acq2 + 7u] & (1u << 14)),
                "(a) %s positive control: RELEASE_MEM 0x514 @%u, WAIT @%u, ACQUIRE_MEM GL2_WB @%u, then CS_PARTIAL_FLUSH @%u and "
                "ACQUIRE_MEM GL2_INV @%u around the dispatch @%u", f->name, rel, wait, acq, ew2, acq2, disp);
        if (rel == ~0u || wait == ~0u || acq == ~0u) continue;
        struct M { const char *what; int kind; uint32_t at, xorv; } m[] = {
            { "the end-of-pipe RELEASE_MEM NOPed", 0, rel, 0u },
            { "the RELEASE_MEM not end of pipe (0x514 -> 0x507)", 1, rel + 1u, 0x514u ^ 0x507u },
            { "the WAIT_REG_MEM NOPed", 0, wait, 0u },
            { "the WAIT on another value", 1, wait + 4u, 1u },
            { "the WAIT on another address", 1, wait + 2u, 0x100u },
            { "the ACQUIRE_MEM before N NOPed", 0, acq, 0u },
            { "the ACQUIRE_MEM before N without GL2_WB", 1, acq + 7u, 1u << 15 },
            { "the CS_PARTIAL_FLUSH after N NOPed", 0, ew2, 0u },
            { "the ACQUIRE_MEM after N NOPed", 0, acq2, 0u },
            { "the ACQUIRE_MEM after N without GL2_INV", 1, acq2 + 7u, 1u << 14 },
        };
        for (const M &x : m) {
            std::memcpy(gIn, f->dw, n * 4u);
            if (x.kind == 0) nop_out(gIn, x.at, n); else gIn[x.at] ^= x.xorv;
            xlat12_draw_stats d57 {}, dn {};
            const uint32_t s57 = tr(gIn, n, XLAT12_EXTRA_CS_ELIDE, g57, &d57);
            const uint32_t sn = tr(gIn, n, XLAT12_EXTRA_CS_ELIDE | XLAT12_EXTRA_NCLEAR, gOn, &dn);
            expectf(sn == s57 && dn.nc_filled == 0u && dn.nc_ref_barrier == 1u && dn.cs_elided == d57.cs_elided &&
                    std::memcmp(gOn, g57, (n + 8u) * 4u) == 0,
                    "(a) %s, %s: NO fill (barrier counted), output == 57 alone", f->name, x.what);
        }
    }
}
static uint32_t gTri[3u * 1470u + 8u], gTriO[3u * 1470u + 8u], gTri57[3u * 1470u + 8u];
static void test_max_fills()
{
    const n48_fixture_nc *f = &kNcSegs[1];              // f18_ib0: 128 KiB, 5 packets
    const uint32_t n = f->n, N = 3u * n;
    for (uint32_t c = 0; c < 3u; c++) std::memcpy(&gTri[c * n], f->dw, n * 4u);
    xlat12_draw_stats d {}, d57 {};
    const uint32_t s57 = tr(gTri, N, XLAT12_EXTRA_CS_ELIDE, gTri57, &d57);
    const uint32_t st = tr(gTri, N, XLAT12_EXTRA_CS_ELIDE | XLAT12_EXTRA_NCLEAR, gTriO, &d);
    expectf(s57 == 0u && d57.cs_elided == 3u, "(b) positive control: three real N segments back to back, 57 elides 3 (st %u, %u)", s57,
            d57.cs_elided);
    expectf(st == 0u && d.cs_elided == 3u && d.nc_filled == XLAT12_NCLEAR_MAX && d.nc_ref_max == 1u && d.nc_pkts == 10u &&
            xlat12_ib_nclear_check(gTriO, N, &d) == 0u,
            "(b) the third proven N falls back to 57's NOP: filled %u, max %u, packets %u; the segment translates and the kext's check "
            "passes", d.nc_filled, d.nc_ref_max, d.nc_pkts);
    uint32_t nops = 0u;
    for (uint32_t i = 0; i < N; ) { const uint32_t l = plen(gTriO, i, N); if (!l) break; if (gTriO[i] == XLAT12_CS_ELIDE_NOP && l == 5u) nops++; i += l; }
    expectf(nops >= 1u, "(b) the third one's NOP is in the output (%u)", nops);
    // an output carrying a fill no recorded fill accounts for: the kext's check refuses it
    xlat12_draw_stats d2 = d; d2.nc_filled = 1u; d2.nc_pkts = 5u;
    expect("(b) nclear_check refuses fill packets beyond the recorded fills", xlat12_ib_nclear_check(gTriO, N, &d2) == 1u);
}
// (e): a mock page table. 64 KiB regions: `big` (one mapping) or small (sixteen 4 KiB leaves, each ok or not).
struct PT { uint64_t base; uint32_t nreg; uint8_t big[16]; uint16_t bad4k[16]; uint32_t bigBad[16]; };
static uint32_t pt_probe(void *c, uint64_t va, uint32_t *big)
{
    const PT *t = static_cast<const PT *>(c);
    *big = 0u;
    if (va < t->base || va >= t->base + (uint64_t)t->nreg * 0x10000ull) return 0u;
    const uint32_t r = (uint32_t)((va - t->base) >> 16), p = (uint32_t)((va >> 12) & 0xFu);
    if (t->big[r]) { *big = 1u; return t->bigBad[r] ? 0u : 1u; }
    return ((t->bad4k[r] >> p) & 1u) ? 0u : 1u;
}
static void test_walk()
{
    PT t {}; t.base = 0x401240000ull; t.nreg = 4u;
    t.big[0] = 1u; t.big[1] = 0u; t.big[2] = 1u; t.big[3] = 0u;
    uint64_t probes = 0u;
    expect("(e) all mapped: 1", n48_nc_pages_walk(&pt_probe, &t, 0x401240000ull, 0x40000u, &probes) == 1u);
    expectf(probes == 2u + 32u, "(e) one probe per 64 KiB page, sixteen per 4 KiB-mapped 64 KiB (%llu, want 34)", (unsigned long long)probes);
    t.bad4k[1] = 1u << 10;                              // 0x40125a000: inside a 4 KiB-mapped region
    expect("(e) a bad 4 KiB page inside a 4 KiB-mapped region is found", n48_nc_pages_walk(&pt_probe, &t, 0x401240000ull, 0x40000u, nullptr) == 0u);
    t.bad4k[1] = 0u; t.bad4k[3] = 1u << 15;            // the last 4 KiB page of the range
    expect("(e) the last 4 KiB page of the range is walked", n48_nc_pages_walk(&pt_probe, &t, 0x401240000ull, 0x40000u, nullptr) == 0u);
    t.bad4k[3] = 0u; t.bigBad[2] = 1u;
    expect("(e) a bad 64 KiB page is found", n48_nc_pages_walk(&pt_probe, &t, 0x401240000ull, 0x40000u, nullptr) == 0u);
    t.bigBad[2] = 0u;
    probes = 0u;
    expect("(e) a range starting mid-64 KiB in a big page steps to the next 64 KiB boundary",
           n48_nc_pages_walk(&pt_probe, &t, 0x401248000ull, 0x10000u, &probes) == 1u && probes == 1u + 8u);
    expect("(e) a range running past the mapped table fails", n48_nc_pages_walk(&pt_probe, &t, 0x401270000ull, 0x20000u, nullptr) == 0u);
    expect("(e) zero length or no probe fails", n48_nc_pages_walk(&pt_probe, &t, 0x401240000ull, 0u, nullptr) == 0u &&
           n48_nc_pages_walk(nullptr, &t, 0x401240000ull, 0x1000u, nullptr) == 0u);
}

int main(int argc, char **argv)
{
    test_barriers();
    test_max_fills();
    test_walk();
    test_fixtures();
    test_refusals();
    test_tex_state();
    test_wiring(argc > 1 ? argv[1] : nullptr);
    std::printf("gfx_nclear: %d check(s), %d failed\n", gChecks, gFails);
    std::printf("gfx_nclear: %s\n", gFails ? "N48-NCLEAR-TEST-FAIL" : "N48-NCLEAR-TEST-PASS");
    return gFails ? 1 : 0;
}
