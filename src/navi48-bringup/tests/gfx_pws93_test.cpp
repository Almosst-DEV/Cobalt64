// gfx_pws93_test.cpp — build 0.0.537: SWITCH 93, APPLE'S CB/DB BARRIER GETS ITS WAIT BACK ON GFX12. The translator's
// XLAT12_EXTRA_PWS (xlat12_ib.h): Apple's barrier ACQUIRE_MEM (C0065800 86287FC3 FFFFFFFF 000000FF 0 0 0000000A 0001C3F1) becomes our
// RELEASE_MEM(PWS) + ACQUIRE_MEM(PWS) in place where the region before a draw has room, a verbatim copy (counted) where it has not, never
// a refusal; the kext's pairing check (xlat12_ib_pws_check), R1's classifier (gfx_memdst.h n48_md_classify) and the switch's wiring.
// Over REAL captured segments (tests/fixture_pws93.h, run11z: F85 IB0 barrier 7378 -'s capsule barrier; F83 IB1 barriers 4782,
// 4918 and 5021 and its last segment; F17 IB0's compute barriers; F4 IB0's 86007fc0; one segment trimmed from F83 IB1's):
//   T1  POSITIVE CONTROLS: F85 IB0 7378 and F83 IB1 4918 / 5021 are converted, each pair exactly where Apple's barrier was;
//       F83 IB1 4782 (head 4780,'s named control) has NO ROOM - the segment's whole pad is 6 dwords - and falls back to
//       today's output byte for byte, counted (pws_noroom 1, pws_fallback 1, the flagged pass's status TOO_LONG).
//   T2  ORDERING: in every converted output our RELEASE_MEM(PWS) is IMMEDIATELY followed by our ACQUIRE_MEM(PWS), no draw
//       between them, and a draw follows the pair (both precede the next draw); every pair turned back into Apple's barrier (pad
//       NOPs dropped) gives the OFF output's packet sequence; no IB grows.
//   T3  OFF IDENTITY: today's translator, flag OFF, over every fixture segment and four modes (no extra block, an extra block,
//       CS_ELIDE, RECT2D) answers exactly as the FROZEN 0.0.536 translator (12e011c7: status and whole-output FNV-1a).
//   T4  THE TAIL AND NO ROOM AT THE BARRIER: a barrier after the last draw (F83 IB1's last segment) is never converted (the
//       region carries Apple's buried fence828 RELEASE_MEM; R1's fence identity needs its offset), counted pws_tail; the trimmed
//       segment's second barrier (8 dwords before its draw) is copied verbatim, counted, while its first converts, no fallback.
//   T5  UNTOUCHED: the compute barriers (a8c40000 / 80c40000, F17 IB0 with CS_ELIDE) and 86007fc0 (F4 IB0) pass through
//       verbatim, counted pws_other, the outputs equal to OFF's.
//   T6  R1 (gfx_memdst.h n48_md_classify): our release is recognised - not a row, not KIND; EVERY one-bit variant of its words
//       1-7 is refused (KIND or a row), as is Apple's barrier's shape untouched; n48_md_scan over real converted outputs records
//       exactly OFF's rows and no KIND.
//   T7  PAIRING (xlat12_ib_pws_check): every converted output passes; a release without its acquire, an acquire alone, a draw
//       between them, a release at the end, and one-bit variants of either packet are refused; OFF outputs pass.
//   T8  the flag: 0x800000 (RECT2D << 1), accepted, with RECT2D too; the next bit still refused; RECT2D + PWS keeps RECT2D's
//       status and both effects.
//   T9  the kext's wiring (AppleHardwareHook.cpp argv[1], xlat12_ib.c argv[2]): switch 93 OFF at boot, the selector (349 / 605),
//       the mid-arm guard, the flag set only where the translation's first packet runs, pws93_seg after rect92_seg and before the
//       copy guard, the report line (<= 491 bytes), the fallback in translate_draw_ex's own frame.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 -I src/navi48-bringup/tests -x c++ \
//         src/navi48-bringup/tests/gfx_pws93_test.cpp src/xlat12/xlat12_ib.c src/xlat12/xlat12.c \
//         -o /tmp/p93 && /tmp/p93 src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/xlat12/xlat12_ib.c
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
#include "gfx_commit.h"
#include "gfx_memdst.h"
#include "gfx_xlat_verdict.h"
#include "gfx_mib.h"             // fix round item 4: n48_mib_start_runs, the head gate the kext calls
#include "fixture_pws93.h"

static int gChecks = 0, gFails = 0;
static void expect(const char *what, bool ok)
{
    gChecks++;
    if (!ok) gFails++;
    if (!ok || std::getenv("P93_VERBOSE")) std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static void expectf(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void expectf(bool ok, const char *fmt, ...)
{
    char b[512];
    va_list ap; va_start(ap, fmt); std::vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    expect(b, ok);
}

static const uint32_t kRel[8] = { XLAT12_PWS_REL_HDR, XLAT12_PWS_REL_W1, 0u, 0u, 0u, 0u, 0u, 0u };
static const uint32_t kAcq[8] = { XLAT12_PWS_ACQ_HDR, XLAT12_PWS_ACQ_W1, 0xFFFFFFFFu, 0x01FFFFFFu, 0u, 0u, XLAT12_PWS_ACQ_W6, XLAT12_PWS_ACQ_W7 };
static const uint32_t kBar[8] = { 0xC0065800u, 0x86287FC3u, 0xFFFFFFFFu, 0x000000FFu, 0u, 0u, 0x0000000Au, 0x0001C3F1u };

static int cs_is_n(void *, uint64_t va) { return va == 0x400017a00ull ? 1 : 0; }
static uint32_t fnv(const uint32_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    const unsigned char *b = reinterpret_cast<const unsigned char *>(p);
    for (uint32_t i = 0; i < n * 4u; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static uint32_t plen(const uint32_t *d, uint32_t i, uint32_t n)
{
    const uint32_t h = d[i];
    if (h == 0xFFFF1000u || (h >> 30) == 2u) return 1u;
    if ((h >> 30) != 3u) return 0u;
    const uint32_t l = 2u + ((h >> 16) & 0x3FFFu);
    return l <= n - i ? l : 0u;
}
static bool is_draw(uint32_t h)
{
    const uint32_t op = (h >> 8) & 0xFFu;
    return (h >> 30) == 3u && h != 0xFFFF1000u && (op == 0x2Du || op == 0x27u || op == 0x35u || op == 0x24u || op == 0x25u);
}

// mode: 0 no extra block, 1 an extra block with no flag, 2 CS_ELIDE, 3 RECT2D; `pws` adds XLAT12_EXTRA_PWS (modes 1-3)
struct Xl { uint32_t st, len; xlat12_draw_stats ds; std::vector<uint32_t> out; };
static Xl run(const n48_fixture_p93 &f, int mode, bool pws)
{
    Xl r; r.out.assign(f.n, 0xDEADBEEFu); r.len = 0;
    xlat12_draw_extra ex; std::memset(&ex, 0, sizeof ex);
    if (mode == 2) { ex.flags = XLAT12_EXTRA_CS_ELIDE; ex.cs_is_n = cs_is_n; }
    if (mode == 3) ex.flags = XLAT12_EXTRA_RECT2D;
    if (pws) ex.flags |= XLAT12_EXTRA_PWS;
    std::memset(&r.ds, 0, sizeof r.ds);
    r.st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), (mode || pws) ? &ex : nullptr, f.dw, f.n, r.out.data(), &r.len, &r.ds);
    return r;
}
static const n48_fixture_p93 &fx(const char *name)
{
    for (uint32_t q = 0; q < N48_FIXTURE_P93_COUNT; q++) if (!std::strcmp(kP93Segs[q].name, name)) return kP93Segs[q];
    std::printf("FAIL  no fixture %s\n", name); std::exit(2);
}
// every packet start of `o`, or empty when unwalkable
static std::vector<uint32_t> starts(const std::vector<uint32_t> &o)
{
    std::vector<uint32_t> s; uint32_t i = 0; const uint32_t n = (uint32_t)o.size();
    while (i < n) { const uint32_t l = plen(o.data(), i, n); if (!l) return std::vector<uint32_t>(); s.push_back(i); i += l; }
    return s;
}
static bool eq8(const uint32_t *a, const uint32_t *b) { return std::memcmp(a, b, 32) == 0; }
// our releases' positions in o (packet starts only)
static std::vector<uint32_t> releases(const std::vector<uint32_t> &o)
{
    std::vector<uint32_t> r;
    for (uint32_t s : starts(o)) if (s + 8u <= o.size() && eq8(&o[s], kRel)) r.push_back(s);
    return r;
}
// build 0.0.539 (xlat12_ib.h XLAT12_PWS_SLOT_*): THE C4 COMPARISON WITH THE SLOT RE-INSERTED. ON against OFF packet for packet
// (1-dword NOPs skipped on both sides): each of our pairs stands where OFF has Apple's barrier; right after such a pair, when OFF
// continues with EVENT_WRITE 0xE and Apple's disabled slot (C0061000 C0051000, 8 dwords) and ON continues with the EVENT_WRITE but
// not that slot, the slot is RE-INSERTED (counted *reins); every other packet must be equal, dword for dword. A slot missing ANYWHERE
// else (not right after one of our pairs) is a difference.
static bool is_ew_e(const std::vector<uint32_t> &o, uint32_t k)
{
    return k + 2u <= o.size() && o[k] == XLAT12_PWS_SLOT_EW0 && o[k + 1u] == XLAT12_PWS_SLOT_EW1;
}
static bool is_slot(const std::vector<uint32_t> &o, uint32_t k)
{
    return k + 8u <= o.size() && o[k] == XLAT12_PWS_SLOT0 && o[k + 1u] == XLAT12_PWS_SLOT1;
}
static bool same_but_pairs(const std::vector<uint32_t> &on, const std::vector<uint32_t> &off, uint32_t *pairs, uint32_t *reins)
{
    const uint32_t n = (uint32_t)on.size(), m = (uint32_t)off.size();
    uint32_t i = 0, j = 0;
    *pairs = 0; *reins = 0;
    for (;;) {
        while (i < n && on[i] == 0xFFFF1000u) i++;
        while (j < m && off[j] == 0xFFFF1000u) j++;
        if (i >= n || j >= m) return i >= n && j >= m;
        const uint32_t li = plen(on.data(), i, n), lj = plen(off.data(), j, m);
        if (!li || !lj) return false;
        if (li == 8u && i + 16u <= n && eq8(&on[i], kRel) && eq8(&on[i + 8u], kAcq)) {
            if (lj != 8u || !eq8(&off[j], kBar)) return false;
            i += 16u; j += 8u; (*pairs)++;
            if (is_ew_e(on, i) && is_ew_e(off, j) && is_slot(off, j + 2u) &&
                !(is_slot(on, i + 2u) && std::memcmp(&on[i + 2u], &off[j + 2u], 32) == 0)) {
                i += 2u; j += 10u; (*reins)++;   // the EVENT_WRITE compared equal; OFF's slot re-inserted
            }
            continue;
        }
        if (li != lj || std::memcmp(&on[i], &off[j], 4u * li) != 0) return false;
        i += li; j += lj;
    }
}
// build 0.0.539: THE SLOT'S BYTE INVARIANT at the Apple barrier of input dword b (flag ON, the caller's extra block). The
// reference R is the SAME pass with that ONE barrier left unconverted: its word 7 GCR bit 6 flipped, every other input dword identical
// (xlat12_ib_pws_apple_ok then refuses it: it stays verbatim, `other`). Answers 0 the barrier did not convert (ON == R, the same
// counts), 1 it converted with its slot dropped and every byte obeys the invariant (out[s..s+16) our pair, out[s+16..s+18) EVENT_WRITE
// 0xE, out[0..s) and out[s+18..n) R's; R holds Apple's barrier, the EVENT_WRITE and the slot at s; pws_slot and pws_conv one more than
// R's), 2 it converted and its input has no slot (pws_slot equal to R's), 9 anything else. *sOut = s.
static uint32_t slot_inv(const uint32_t *dw, uint32_t n, const xlat12_draw_extra &ex0, uint32_t b, uint32_t *sOut)
{
    xlat12_draw_extra ex = ex0; ex.flags |= XLAT12_EXTRA_PWS;
    std::vector<uint32_t> on(n, 0xDEADBEEFu), r(n, 0xDEADBEEFu), mut(dw, dw + n);
    mut[b + 7u] ^= 0x40u;
    xlat12_draw_stats d1, d0; uint32_t l1 = 0, l0 = 0;
    const uint32_t s1 = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, dw, n, on.data(), &l1, &d1);
    const uint32_t s0 = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, mut.data(), n, r.data(), &l0, &d0);
    if (s1 || s0) return 9u;
    uint32_t s = 0;
    while (s < n && on[s] == r[s]) s++;
    if (sOut) *sOut = s;
    if (s == n) return 9u;   // the flipped word must show somewhere
    // not converted: ON holds Apple's barrier verbatim where R holds the flipped one (the first difference is its word 7), and not
    // one other dword differs
    if (s >= 7u && eq8(&on[s - 7u], kBar) && std::memcmp(&r[s - 7u], &mut[b], 32) == 0) {
        uint32_t k = s + 1u;
        while (k < n && on[k] == r[k]) k++;
        return (k == n && d1.pws_conv == d0.pws_conv && d1.pws_slot == d0.pws_slot) ? 0u : 9u;
    }
    if (s + 18u > n || !eq8(&on[s], kRel) || !eq8(&on[s + 8u], kAcq) || d1.pws_conv != d0.pws_conv + 1u) return 9u;
    if (!(b + 18u <= n && xlat12_ib_pws_slot_at(&dw[b], n - b))) return d1.pws_slot == d0.pws_slot ? 2u : 9u;
    if (on[s + 16u] != XLAT12_PWS_SLOT_EW0 || on[s + 17u] != XLAT12_PWS_SLOT_EW1) return 9u;
    if (std::memcmp(&r[s], &mut[b], 4u * 18u) != 0) return 9u;   // R: the (flipped) barrier, the EVENT_WRITE, the slot - Apple's 18
    for (uint32_t k = s + 18u; k < n; k++) if (on[k] != r[k]) return 9u;
    return d1.pws_slot == d0.pws_slot + 1u ? 1u : 9u;
}

// T2's ordering rule over one output: each of our releases is followed at +8 by our acquire, and a draw follows the pair
static bool ordering_ok(const std::vector<uint32_t> &o, uint32_t *pairs)
{
    const std::vector<uint32_t> s = starts(o);
    if (s.empty()) return false;
    *pairs = 0;
    for (size_t k = 0; k < s.size(); k++) {
        if (!(s[k] + 8u <= o.size() && eq8(&o[s[k]], kRel))) continue;
        if (k + 1u >= s.size() || s[k + 1u] != s[k] + 8u || !eq8(&o[s[k + 1u]], kAcq)) return false;   // IMMEDIATELY followed
        bool drawAfter = false;
        for (size_t j = k + 2u; j < s.size(); j++) {
            if (is_draw(o[s[j]])) { drawAfter = true; break; }
            if (eq8(&o[s[j]], kRel)) break;   // another pair before any draw: still fine, but this one needs a draw after it too
        }
        if (!drawAfter) {   // a draw must come after the pair before the output ends
            bool any = false;
            for (size_t j = k + 2u; j < s.size(); j++) if (is_draw(o[s[j]])) { any = true; break; }
            if (!any) return false;
        }
        (*pairs)++;
    }
    return true;
}

// =============================================================================================================================
static void test_positive()
{
    const n48_fixture_p93 &a = fx("z85_0_s2");
    const Xl off = run(a, 1, false), on = run(a, 1, true);
    expectf(on.st == 0u && on.len == a.n && eq8(&on.out[0], kRel) && eq8(&on.out[8], kAcq) && on.ds.pws_seen == 1u &&
            on.ds.pws_conv == 1u && on.ds.pws_noroom == 0u && on.ds.pws_fallback == 0u && off.st == 0u,
            "T1 F85 IB0 7378 (the capsule barrier AI -> AJ,): converted at the barrier's own dword (st %u, conv %u)",
            on.st, on.ds.pws_conv);
    const n48_fixture_p93 &b = fx("z83_1_s5");
    const Xl b1 = run(b, 1, true);
    const std::vector<uint32_t> br = releases(b1.out);
    expectf(b1.st == 0u && b1.ds.pws_conv == 1u && br.size() == 1u && br[0] == 0u && b.bar_at[1] == 103u && eq8(&b1.out[103], kBar) &&
            b1.ds.pws_fence == 1u && b1.out[111] == 0xC0071000u && b1.out[112] == 0xC0064900u,
            "T1 F83 IB1 4918 converts at its own dword; 5021 (a per-draw trailer: its region holds Apple's buried fence828 packet right "
            "after it) stays Apple's, verbatim, so the buried packet keeps Apple's offset (fix round item 3; pws_fence %u)",
            (uint32_t)b1.ds.pws_fence);
    const n48_fixture_p93 &c = fx("z83_1_s4");
    const Xl c0 = run(c, 1, false), c1 = run(c, 1, true);
    expectf(c1.st == 0u && c1.out == c0.out && c1.ds.pws_seen == 1u && c1.ds.pws_conv == 0u && c1.ds.pws_noroom == 1u &&
            c1.ds.pws_fallback == 1u && c1.ds.pws_on_st == XLAT12_IB_ERR_TOO_LONG && c0.ds.pad_dwords == 6u,
            "T1 F83 IB1 4782 (head 4780,'s named control): NO ROOM - whole pad %u dwords - so today's output byte for byte, "
            "counted no-room with the fallback (flagged pass st %u)", c0.ds.pad_dwords, (uint32_t)c1.ds.pws_on_st);
}

static void test_ordering_and_identity()
{
    uint32_t totPairs = 0;
    for (uint32_t q = 0; q < N48_FIXTURE_P93_COUNT; q++) {
        const n48_fixture_p93 &f = kP93Segs[q];
        for (int mode = 0; mode < 4; mode++) {
            const Xl off = run(f, mode, false);
            expectf(off.st == f.f_st[mode] && fnv(off.out.data(), f.n) == f.f_fnv[mode],
                    "T3 %s mode %d: OFF is the frozen 0.0.536 answer (st %u/%u, fnv %08x/%08x)", f.name, mode, off.st, f.f_st[mode],
                    fnv(off.out.data(), f.n), f.f_fnv[mode]);
            if (mode == 0) continue;
            const Xl on = run(f, mode, true);
            expectf(on.st == off.st, "T2 %s mode %d: never a refusal (ON %u, OFF %u)", f.name, mode, on.st, off.st);
            if (on.st) continue;
            uint32_t pairs = 0;
            expectf(on.len == f.n && ordering_ok(on.out, &pairs) && pairs == on.ds.pws_conv && releases(on.out).size() == on.ds.pws_conv,
                    "T2 %s mode %d: ORDERING - each release immediately followed by its acquire, a draw after the pair (%u pairs, "
                    "conv %u), the IB does not grow", f.name, mode, pairs, on.ds.pws_conv);
            uint32_t pp = 0, ri = 0;
            expectf(same_but_pairs(on.out, off.out, &pp, &ri) && pp == on.ds.pws_conv && ri == on.ds.pws_slot,
                    "T2 %s mode %d: every pair turned back into Apple's barrier, its dropped slot re-inserted (0.0.539), gives the OFF "
                    "packets (pairs %u, slots re-inserted %u = pws_slot %u)", f.name, mode, pp, ri, (uint32_t)on.ds.pws_slot);
            totPairs += pairs;
        }
    }
    expectf(totPairs >= 9u, "T2 the fixtures carry %u converted pairs across the modes", totPairs);
}

static void test_tail_noroom()
{
    const n48_fixture_p93 &t = fx("z83_1_s11");
    const Xl off = run(t, 1, false), on = run(t, 1, true);
    uint32_t lastDraw = 0;
    for (uint32_t s : starts(on.out)) if (is_draw(on.out[s])) lastDraw = s;
    bool tailSame = on.st == 0u && off.st == 0u && lastDraw > 0u;
    for (uint32_t k = lastDraw; tailSame && k < t.n; k++) tailSame = on.out[k] == off.out[k];
    expectf(t.nbar == 3u && t.bar_draw_after[2] == 0u && on.ds.pws_seen == 3u && on.ds.pws_conv == 2u && on.ds.pws_tail == 1u &&
            on.ds.pws_noroom == 1u && on.ds.pws_fallback == 0u && tailSame,
            "T4 the barrier after the last draw (F83 IB1 last segment, local %u) is never converted: the whole region after the last "
            "draw is OFF's byte for byte (tail %u, conv %u)", t.bar_at[2], (uint32_t)on.ds.pws_tail, (uint32_t)on.ds.pws_conv);
    const n48_fixture_p93 &m = fx("z83_1_trim");
    const Xl m0 = run(m, 1, false), m1 = run(m, 1, true);
    const std::vector<uint32_t> r = releases(m1.out);
    // the second barrier: Apple's 8 dwords verbatim at its own dword (region 1 starts where the input's does)
    const bool verb = m1.st == 0u && m.nbar == 2u && m.bar_at[1] + 8u <= m.n && eq8(&m1.out[m.bar_at[1]], kBar);
    expectf(m0.st == 0u && m1.st == 0u && m1.ds.pws_seen == 2u && m1.ds.pws_conv == 1u && m1.ds.pws_noroom == 1u && m1.ds.pws_tail == 0u &&
            m1.ds.pws_fallback == 0u && r.size() == 1u && r[0] == 0u && verb,
            "T4 NO ROOM AT THE BARRIER (trimmed from F83 IB1): the first barrier converts, the second (8 dwords before its draw) is "
            "Apple's verbatim, counted no-room, no fallback, no refusal");
}

static uint32_t count_acq(const std::vector<uint32_t> &o, uint32_t w1)
{
    uint32_t c = 0;
    for (uint32_t s : starts(o)) if (s + 8u <= o.size() && o[s] == 0xC0065800u && o[s + 1u] == w1) c++;
    return c;
}
static void test_untouched()
{
    const n48_fixture_p93 &c = fx("z17_0_cs");
    const Xl off = run(c, 2, false), on = run(c, 2, true);
    uint32_t pp5 = 0, ri5 = 0;
    expectf(off.st == 0u && on.st == 0u && c.other == 2u && on.ds.pws_other == 2u && count_acq(on.out, 0xa8c40000u) == 1u &&
            count_acq(on.out, 0x80c40000u) == 1u && count_acq(off.out, 0xa8c40000u) == 1u && count_acq(off.out, 0x80c40000u) == 1u &&
            same_but_pairs(on.out, off.out, &pp5, &ri5) && pp5 == on.ds.pws_conv && ri5 == on.ds.pws_slot,
            "T5 the compute barriers a8c40000 / 80c40000 (F17 IB0, CS_ELIDE): verbatim, counted other (%u), nothing else changed",
            (uint32_t)on.ds.pws_other);
    const n48_fixture_p93 &h = fx("z4_0_hl");
    const Xl h0 = run(h, 1, false), h1 = run(h, 1, true);
    expectf(h0.st == 0u && h1.st == 0u && h1.out == h0.out && h1.ds.pws_seen == 0u && h1.ds.pws_other == 6u &&
            count_acq(h1.out, 0x86007fc0u) == 3u,
            "T5 86007fc0 (F4 IB0, three of them): the output is OFF's byte for byte, counted other (%u)", (uint32_t)h1.ds.pws_other);
}

// T5b: THE MATCH IS EXACT. Every one-bit variant of Apple's barrier's words 1-7 in the real F85 segment (and the real compute GCR
// 0x1C3B1 in place of 0x1C3F1) is NOT converted: counted other, the output OFF's of the same mutated input, byte for byte.
static void test_exact_match()
{
    const n48_fixture_p93 &a = fx("z85_0_s2");
    uint32_t ok = 0, tot = 0;
    std::vector<uint32_t> mut(a.dw, a.dw + a.n);
    auto one = [&](uint32_t w, uint32_t v) {
        mut.assign(a.dw, a.dw + a.n); mut[w] = v;
        n48_fixture_p93 m = a; m.dw = mut.data();
        const Xl off = run(m, 1, false), on = run(m, 1, true);
        tot++;
        if (on.st == off.st && on.out == off.out && on.ds.pws_conv == 0u && on.ds.pws_seen == 0u && on.ds.pws_other == 1u) ok++;
    };
    for (uint32_t w = 1u; w < 8u; w++)
        for (uint32_t b = 0u; b < 32u; b++) one(w, a.dw[w] ^ (1u << b));
    expectf(ok == tot && tot == 224u, "T5b every one-bit variant of Apple's barrier (words 1-7) is left as today, counted other (%u of %u)", ok, tot);
    ok = tot = 0; one(7u, 0x0001C3B1u);
    expect("T5b Apple's barrier with the compute GCR (0x1C3B1) in word 7 is left as today (the GCR is part of the match)", ok == 1u);
}

static uint32_t classify(const uint32_t *p, uint32_t *kb)
{
    uint64_t va = 0ull; uint32_t w = 0, f = 0, rf = 0, m = 0;
    return n48_md_classify(p, 0u, 8u, kb, &va, &w, &f, &rf, &m);
}
static void test_md()
{
    uint32_t kb = 9u;
    expect("T6 R1: OUR release is recognised - not a destination row, not KIND", classify(kRel, &kb) == 0u && kb == 0u);
    uint32_t refused = 0, total = 0;
    for (uint32_t w = 1u; w < 8u; w++)
        for (uint32_t b = 0u; b < 32u; b++) {
            uint32_t v[8]; std::memcpy(v, kRel, sizeof v); v[w] ^= 1u << b;
            uint32_t k2 = 0u; const uint32_t isMem = classify(v, &k2);
            total++; if (k2 || isMem) refused++;
        }
    expectf(refused == total && total == 224u, "T6 R1: EVERY one-bit variant of words 1-7 is refused - KIND or a row (%u of %u)", refused, total);
    uint32_t v31[8]; std::memcpy(v31, kRel, sizeof v31); v31[1] ^= 1u << 31;   // PWS_ENABLE dropped
    uint32_t k31 = 0u; const uint32_t m31 = classify(v31, &k31);
    expect("T6 R1: the release with PWS_ENABLE cleared (a plain cache-flush event) is refused KIND", m31 == 0u && k31 == 1u);
    uint32_t k3 = 0u; uint64_t va = 0ull; uint32_t w = 0, f = 0, rf = 0, mm = 0;
    expect("T6 R1: our ACQUIRE_MEM and Apple's barrier are not rows (as today: ACQUIRE_MEM is never one)",
           n48_md_classify(kAcq, 0u, 8u, &k3, &va, &w, &f, &rf, &mm) == 0u && k3 == 0u &&
           n48_md_classify(kBar, 0u, 8u, &k3, &va, &w, &f, &rf, &mm) == 0u && k3 == 0u);
    for (const char *nm : { "z85_0_s2", "z83_1_s5", "z83_1_trim" }) {
        const n48_fixture_p93 &x = fx(nm);
        const Xl off = run(x, 1, false), on = run(x, 1, true);
        static n48_md_scan_result r0, r1;
        (void)n48_md_scan(off.out.data(), x.n, x.dw, x.n, 0ull, 0ull, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r0);
        (void)n48_md_scan(on.out.data(), x.n, x.dw, x.n, 0ull, 0ull, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r1);
        bool same = r0.n == r1.n && r0.nWrites == r1.nWrites && r0.nWaits == r1.nWaits;
        for (uint32_t k = 0; same && k < r0.n; k++) same = r0.d[k].kind == r1.d[k].kind && r0.d[k].va == r1.d[k].va;
        expectf(on.ds.pws_conv >= 1u && r1.walkOk && !r1.kindBad && !r0.kindBad && same,
                "T6 R1's scan over %s's converted output: walks clean, no KIND, exactly OFF's rows (%u) and counts", nm, r1.n);
    }
}

static void test_pairing()
{
    for (const char *nm : { "z85_0_s2", "z83_1_s5", "z83_1_s11", "z83_1_trim" }) {
        const n48_fixture_p93 &x = fx(nm);
        const Xl off = run(x, 1, false), on = run(x, 1, true);
        expectf(xlat12_ib_pws_check(on.out.data(), x.n) == 0u && xlat12_ib_pws_check(off.out.data(), x.n) == 0u,
                "T7 %s: the pairing check passes the converted output and OFF's", nm);
    }
    const n48_fixture_p93 &a = fx("z85_0_s2");
    const Xl on = run(a, 1, true);
    std::vector<uint32_t> o = on.out;
    for (uint32_t k = 8u; k < 16u; k++) o[k] = 0xFFFF1000u;   // the acquire gone
    expect("T7 a release without its acquire: refused", xlat12_ib_pws_check(o.data(), a.n) == 1u);
    o = on.out;
    for (uint32_t k = 0u; k < 8u; k++) o[k] = 0xFFFF1000u;    // the release gone
    expect("T7 an acquire alone: refused", xlat12_ib_pws_check(o.data(), a.n) == 1u);
    uint32_t bad = 0, tot = 0;
    for (uint32_t w = 1u; w < 8u; w++)
        for (uint32_t b = 0u; b < 32u; b++) {
            for (uint32_t which = 0u; which < 2u; which++) {
                o = on.out; o[8u * which + w] ^= 1u << b;
                // a variant that clears the PWS bit of the release leaves the acquire orphaned; one that clears the acquire's leaves
                // the release unpaired; any other is not ours: every one must be refused
                tot++; if (xlat12_ib_pws_check(o.data(), a.n) == 1u) bad++;
            }
        }
    expectf(bad == tot, "T7 every one-bit variant of either packet's words 1-7 is refused (%u of %u)", bad, tot);
    uint32_t t1[16]; std::memcpy(t1, kRel, 32); for (uint32_t k = 8; k < 16u; k++) t1[k] = 0xFFFF1000u;
    expect("T7 a release at the very end: refused", xlat12_ib_pws_check(t1, 16u) == 1u);
    uint32_t t2[22]; std::memcpy(t2, kRel, 32); t2[8] = 0xC0012D00u; t2[9] = 3u; t2[10] = 2u; t2[11] = 0xFFFF1000u; t2[12] = 0xFFFF1000u;
    t2[13] = 0xFFFF1000u; std::memcpy(t2 + 14, kAcq, 32);
    expect("T7 a draw between the release and the acquire: refused", xlat12_ib_pws_check(t2, 22u) == 1u);
    uint32_t t3[16]; std::memcpy(t3, kRel, 32); std::memcpy(t3 + 8, kAcq, 32);
    expect("T7 the pair alone: passes", xlat12_ib_pws_check(t3, 16u) == 0u);
    expect("T7 an unwalkable stream / null: refused", xlat12_ib_pws_check(t3, 12u) == 1u && xlat12_ib_pws_check(nullptr, 0u) == 1u);
}

static void test_flag()
{
    expect("T8 XLAT12_EXTRA_PWS is 0x800000, the bit after XLAT12_EXTRA_RECT2D", XLAT12_EXTRA_PWS == 0x800000u && XLAT12_EXTRA_PWS == (XLAT12_EXTRA_RECT2D << 1));
    const n48_fixture_p93 &a = fx("z85_0_s2");
    uint32_t out[2048]; uint32_t len = 0; xlat12_draw_stats ds; xlat12_draw_extra ex; std::memset(&ex, 0, sizeof ex);
    ex.flags = XLAT12_EXTRA_PWS << 2;   // build 0.0.540 item 5: 0x1000000 is XLAT12_EXTRA_TBLCACHE now; the next free bit
    expect("T8 the next free bit (0x2000000) is still refused ERR_ARG",
           xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, a.dw, a.n, out, &len, &ds) == (uint32_t)XLAT12_ERR_ARG);
    ex.flags = XLAT12_EXTRA_PWS;
    expect("T8 the flag alone is accepted",
           xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, a.dw, a.n, out, &len, &ds) == 0u && ds.pws_conv == 1u);
    const Xl r2 = run(a, 3, false), both = run(a, 3, true);
    uint32_t pp8 = 0, ri8 = 0;
    expectf(both.st == r2.st && both.st == 0u && both.ds.pws_conv == 1u && both.ds.r2d_written == r2.ds.r2d_written &&
            same_but_pairs(both.out, r2.out, &pp8, &ri8) && pp8 == 1u && ri8 == both.ds.pws_slot,
            "T8 RECT2D + PWS: RECT2D's status and RECT_2D writes (%u), plus the pair", (uint32_t)both.ds.r2d_written);
}

// T10 (fix round item 4): THE HEAD GATE, BY BEHAVIOUR. The CP-visible stream is the segment's head dwords followed by the translation
// of [start, end). Behind Apple's NOP-disguised head (F83 IB1 [0, 1216): C0081000 00000016 C0065800 ...) the translation starts inside
// the NOP's body: flagged, the CP would skip our release with the body and run our acquire alone - xlat12_ib_pws_check over the CP
// view refuses it - so n48_mib_start_runs answers 0 and the kext never flags it. A real head (F85 IB0 7376) and a segment that starts
// at its own head answer 1, and their CP views pass.
static void test_head_gate()
{
    const n48_fixture_p93 &d = fx("z83_1_disg");
    xlat12_ib_segment sd {}; sd.head = 0u; sd.start = 2u; sd.end = d.n;
    expect("T10 the disguised head (F83 IB1 dword 0: C0081000 00000016 C0065800) - n48_mib_start_runs answers 0",
           d.dw[0] == 0xC0081000u && d.dw[1] == 0x16u && d.dw[2] == 0xC0065800u && n48_mib_start_runs(d.dw, &sd, d.n) == 0);
    auto cpview = [](const uint32_t *head2, const std::vector<uint32_t> &out) {
        std::vector<uint32_t> v(head2, head2 + 2); v.insert(v.end(), out.begin(), out.end()); return v;
    };
    n48_fixture_p93 body = d; body.dw = d.dw + 2; body.n = d.n - 2u;
    const Xl on = run(body, 1, true), off = run(body, 1, false);
    const std::vector<uint32_t> vOn = cpview(d.dw, on.out), vOff = cpview(d.dw, off.out);
    expectf(on.st == 0u && on.ds.pws_conv == 1u && eq8(&on.out[0], kRel) && xlat12_ib_pws_check(on.out.data(), body.n) == 0u &&
            xlat12_ib_pws_check(vOn.data(), (uint32_t)vOn.size()) == 1u && xlat12_ib_pws_check(vOff.data(), (uint32_t)vOff.size()) == 0u,
            "T10 WHY: flagged anyway, the translation converts the body's barrier (conv %u) and passes its own check, but the CP view "
            "(the NOP header first) runs an acquire with no release - refused; unflagged, the CP view passes", (uint32_t)on.ds.pws_conv);
    const n48_fixture_p93 &a = fx("z85_0_s2");
    std::vector<uint32_t> ia = { 0xC0004600u, 0x16u }; ia.insert(ia.end(), a.dw, a.dw + a.n);
    xlat12_ib_segment sa {}; sa.head = 0u; sa.start = 2u; sa.end = (uint32_t)ia.size();
    const Xl a1 = run(a, 1, true);
    const std::vector<uint32_t> vA = cpview(ia.data(), a1.out);
    expect("T10 a real head (F85 IB0 7376: C0004600 00000016) answers 1 and its CP view passes with the pair",
           n48_mib_start_runs(ia.data(), &sa, (uint32_t)ia.size()) == 1 && a1.ds.pws_conv == 1u &&
           xlat12_ib_pws_check(vA.data(), (uint32_t)vA.size()) == 0u);
    xlat12_ib_segment sl {}; sl.head = 0u; sl.start = 0u; sl.end = d.n;
    std::vector<uint32_t> hl(d.dw, d.dw + d.n); hl[0] = 0xC0012800u;   // any first dword: a lead / headless segment starts at its head
    expect("T10 a segment starting at its own head (a lead, a headless IB) answers 1; a null segment 0",
           n48_mib_start_runs(hl.data(), &sl, d.n) == 1 && n48_mib_start_runs(hl.data(), nullptr, d.n) == 0);
    xlat12_ib_segment sx {}; sx.head = 0u; sx.start = 2u; sx.end = d.n;
    std::vector<uint32_t> bad(d.dw, d.dw + d.n); bad[1] = 0x17u;   // an EVENT_WRITE head of another event: not positive evidence
    bad[0] = 0xC0004600u;
    expect("T10 a head that is an EVENT_WRITE of another event (0x17) answers 0", n48_mib_start_runs(bad.data(), &sx, d.n) == 0);
}

// T12 (fix round item 3): FENCE828's IDENTITY SURVIVES. Over every fixture segment and mode, flagged, the buried fence828 packet
// (C0071000 C0064900) sits at exactly Apple's offset wherever n48_f828_find finds one - no converted pair ever moves it.
static void test_fence_identity()
{
    uint32_t found = 0, same = 0;
    for (uint32_t q = 0; q < N48_FIXTURE_P93_COUNT; q++)
        for (int mode = 1; mode < 4; mode++) {
            const n48_fixture_p93 &f = kP93Segs[q];
            const Xl on = run(f, mode, true);
            if (on.st) continue;
            for (uint32_t i : starts(on.out)) if (on.out[i] == 0xC0071000u && i + 1u < f.n && on.out[i + 1u] == 0xC0064900u) {
                found++; if (f.dw[i] == 0xC0071000u && f.dw[i + 1u] == 0xC0064900u) same++;
            }
        }
    expectf(found > 0u && same == found, "T12 every buried fence828 packet in a flagged output sits at Apple's own offset (%u of %u)", same, found);
}

// T11 (fix round item 2): THE CAPSULE BARRIER UNDER THE RECIPE'S RING AND RASTER DELTA (switches 27 + 50 and the open ring - the parts
// of RUN AM's flag set that need no client memory). F85 IB0 7378's region 0 has 39 dwords of pad bare, 22 with the ring block
// (d_rings), 5 with the raster follow-ons too: the pair needs 8, so the flagged pass runs out of room and falls back - today's output,
// byte for byte, never a refusal. THE CAPSULE BARRIER DOES NOT CONVERT UNDER THE REAL RECIPE (the table step can only take more).
// build 0.0.539 (XLAT12_PWS_SLOT_*,): WITH THE SLOT REUSE IT DOES, at every step: the barrier is slotted (EVENT_WRITE 0xE and
// Apple's disabled slot right after it), so the pair takes the slot's 8 dwords and none of the pad - the pad stays 39 / 22 / 5, no
// fallback, the slot dropped (pws_slot 1), and the byte invariant holds (slot_inv).
static void test_recipe_room()
{
    const n48_fixture_p93 &a = fx("z85_0_s2");
    for (int step = 0; step < 3; step++) {
        std::vector<uint32_t> o0(a.n, 0xDEADBEEFu), o1(a.n, 0xDEADBEEFu);
        xlat12_draw_extra ex; std::memset(&ex, 0, sizeof ex);
        if (step >= 1) { ex.ring_va = 0x23f0000000ull; ex.gs_sgpr0_va = 0x23f0a80000ull; }
        if (step >= 2) { ex.flags |= XLAT12_EXTRA_RASTER | XLAT12_EXTRA_RASTER_PER_DRAW; ex.rsrc3_gs = XLAT12_RSRC3_GS_CU_EN; }
        xlat12_draw_stats d0, d1; uint32_t l0 = 0, l1 = 0;
        const uint32_t s0 = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, a.dw, a.n, o0.data(), &l0, &d0);
        ex.flags |= XLAT12_EXTRA_PWS;
        const uint32_t s1 = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, a.dw, a.n, o1.data(), &l1, &d1);
        uint32_t first = 0; while (first < a.n && !is_draw(o0[first])) first++;
        uint32_t pad = 0; while (first > pad && o0[first - 1u - pad] == 0xFFFF1000u) pad++;
        static const uint32_t want[3] = { 39u, 22u, 5u };
        const bool conv = d1.pws_conv == 1u && eq8(&o1[0], kRel);
        uint32_t pad1 = 0; while (first > pad1 && o1[first - 1u - pad1] == 0xFFFF1000u) pad1++;
        ex.flags &= ~XLAT12_EXTRA_PWS;
        uint32_t at = 99u;
        const uint32_t inv = slot_inv(a.dw, a.n, ex, 0u, &at);
        expectf(s0 == 0u && s1 == 0u && pad == want[step] && conv && d1.pws_slot == 1u && d1.pws_fallback == 0u && pad1 == pad &&
                inv == 1u && at == 0u,
                "T11 F85 IB0 7378 %s: region-0 pad %u dwords -> %s (0.0.539: slot dropped %u, pad after %u, byte invariant %u)",
                step == 0 ? "bare" : step == 1 ? "+ the ring block" : "+ the ring block + the raster delta (RUN AM's 27 + 50)", pad,
                conv ? "CONVERTED" : d1.pws_fallback ? "NOT converted: the fallback" : "?", (uint32_t)d1.pws_slot, pad1, inv);
    }
}

// T13 (build 0.0.539, xlat12_ib.h XLAT12_PWS_SLOT_*; ): THE SLOT REUSE.
//   a. over every fixture segment, modes 1-3, every Apple barrier: the byte invariant (slot_inv) never breaks; the slot is dropped
//      exactly at the converted SLOTTED barriers (pws_slot summed over ON == the count of slot_inv answers 1), never at an unslotted
//      one (answers 2: F83 IB1 4918), never at an unconverted one (answers 0: 4782's fallback, 5021's fence, the tails);
//   b. F85 IB0 7378 (the capsule barrier) is slotted and answers 1 in every mode;
//   c. an ENABLED slot (FFFF1000 C0053C00 ...: a 1-dword NOP and a memory WAIT_REG_MEM) is kept, byte for byte, after the pair;
//   d. a STALE-BODIED disabled slot (C0061000 C0051000 and six non-zero dwords) is dropped, and the invariant holds;
//   e. no EVENT_WRITE 0xE between the barrier and the slot (two 1-dword NOPs in its place): the slot is kept, nothing dropped;
//   f. the EVENT_WRITE 0xE of another shape (event 0x16) there: kept;
//   g. the fallback pass never drops a slot (a second, unslotted barrier forces the fallback); OFF output stays the frozen answer (T3).
static void test_slot()
{
    uint32_t inv1 = 0, inv2 = 0, inv0 = 0, inv9 = 0, slotSum = 0;
    for (uint32_t q = 0; q < N48_FIXTURE_P93_COUNT; q++) {
        const n48_fixture_p93 &f = kP93Segs[q];
        if (!std::strcmp(f.name, "z83_1_disg")) continue;   // never flagged (T10)
        for (int mode = 1; mode < 4; mode++) {
            xlat12_draw_extra ex; std::memset(&ex, 0, sizeof ex);
            if (mode == 2) { ex.flags = XLAT12_EXTRA_CS_ELIDE; ex.cs_is_n = cs_is_n; }
            if (mode == 3) ex.flags = XLAT12_EXTRA_RECT2D;
            const Xl on = run(f, mode, true);
            if (on.st) continue;
            slotSum += on.ds.pws_slot;
            for (uint32_t k = 0; k < f.nbar && k < 8u; k++) {
                uint32_t at = 0;
                const uint32_t v = slot_inv(f.dw, f.n, ex, f.bar_at[k], &at);
                if (v == 1u) inv1++; else if (v == 2u) inv2++; else if (v == 0u) inv0++; else inv9++;
                if (v == 9u) std::printf("  slot_inv 9: %s mode %d barrier %u\n", f.name, mode, f.bar_at[k]);
            }
        }
    }
    expectf(inv9 == 0u && inv1 == slotSum && inv1 >= 3u && inv2 >= 3u && inv0 >= 3u,
            "T13a every fixture barrier, modes 1-3: the byte invariant never breaks (%u); slots dropped %u == converted slotted barriers "
            "%u; converted unslotted %u (slot kept: none to drop), unconverted %u", inv9, slotSum, inv1, inv2, inv0);
    const n48_fixture_p93 &a = fx("z85_0_s2");
    uint32_t caps = 0;
    for (int mode = 1; mode < 4; mode++) {
        xlat12_draw_extra ex; std::memset(&ex, 0, sizeof ex);
        if (mode == 2) { ex.flags = XLAT12_EXTRA_CS_ELIDE; ex.cs_is_n = cs_is_n; }
        if (mode == 3) ex.flags = XLAT12_EXTRA_RECT2D;
        uint32_t at = 99u;
        if (slot_inv(a.dw, a.n, ex, 0u, &at) == 1u && at == 0u) caps++;
    }
    expectf(xlat12_ib_pws_slot_at(a.dw, a.n) == 1 && caps == 3u,
            "T13b F85 IB0 7378 (the capsule barrier) is slotted and converts with its slot dropped in every mode (%u of 3)", caps);
    std::vector<uint32_t> m(a.dw, a.dw + a.n);
    n48_fixture_p93 mf = a;
    auto runm = [&](bool pws) { mf.dw = m.data(); return run(mf, 1, pws); };
    // c. an ENABLED slot: FFFF1000, then WAIT_REG_MEM (count 5) on memory (control 0x13: mem space, function ==)
    const uint32_t en[8] = { 0xFFFF1000u, 0xC0053C00u, 0x00000013u, 0x00001000u, 0u, 1u, 0xFFFFFFFFu, 4u };
    for (uint32_t k = 0; k < 8u; k++) m[10u + k] = en[k];
    {
        const Xl off = runm(false), on = runm(true);
        uint32_t pp = 0, ri = 0;
        expectf(off.st == 0u && on.st == 0u && on.ds.pws_conv == 1u && on.ds.pws_slot == 0u && eq8(&on.out[0], kRel) &&
                on.out[16] == XLAT12_PWS_SLOT_EW0 && on.out[17] == XLAT12_PWS_SLOT_EW1 && std::memcmp(&on.out[18], en, 32) == 0 &&
                same_but_pairs(on.out, off.out, &pp, &ri) && pp == 1u && ri == 0u,
                "T13c an ENABLED slot (FFFF1000 C0053C00 ...) never matches: kept after the pair byte for byte, nothing dropped (slot %u)",
                (uint32_t)on.ds.pws_slot);
    }
    // d. a stale-bodied disabled slot
    m.assign(a.dw, a.dw + a.n);
    for (uint32_t k = 12u; k < 18u; k++) m[k] = 0x5A5A0000u + k;
    {
        const Xl off = runm(false), on = runm(true);
        xlat12_draw_extra ex; std::memset(&ex, 0, sizeof ex); ex.flags = 0u;
        uint32_t at = 99u, pp = 0, ri = 0;
        const uint32_t v = slot_inv(m.data(), a.n, ex, 0u, &at);
        expectf(off.st == 0u && on.st == 0u && on.ds.pws_slot == 1u && v == 1u && at == 0u && same_but_pairs(on.out, off.out, &pp, &ri) &&
                ri == 1u && off.out[12] == 0x5A5A000Cu,
                "T13d a STALE-BODIED disabled slot (C0061000 C0051000 + six non-zero dwords) is dropped: its body is never executed "
                "(invariant %u, slot %u)", v, (uint32_t)on.ds.pws_slot);
    }
    // e. no EVENT_WRITE 0xE between the barrier and the slot
    m.assign(a.dw, a.dw + a.n);
    m[8] = 0xFFFF1000u; m[9] = 0xFFFF1000u;
    {
        const Xl off = runm(false), on = runm(true);
        uint32_t k = 16u; while (k < a.n && on.out[k] == 0xFFFF1000u) k++;
        uint32_t pp = 0, ri = 0;
        expectf(off.st == 0u && on.st == 0u && on.ds.pws_conv == 1u && on.ds.pws_slot == 0u && k + 8u <= a.n &&
                std::memcmp(&on.out[k], &a.dw[10], 32) == 0 && same_but_pairs(on.out, off.out, &pp, &ri) && ri == 0u,
                "T13e no EVENT_WRITE 0xE between the barrier and the slot: the slot is kept, nothing dropped (slot %u)",
                (uint32_t)on.ds.pws_slot);
    }
    // f. an EVENT_WRITE of another event (0x16) in its place
    m.assign(a.dw, a.dw + a.n);
    m[9] = 0x16u;
    {
        const Xl on = runm(true);
        expectf(on.st == 0u && on.ds.pws_conv == 1u && on.ds.pws_slot == 0u && on.out[16] == 0xC0004600u && on.out[17] == 0x16u &&
                std::memcmp(&on.out[18], &a.dw[10], 32) == 0,
                "T13f an EVENT_WRITE of event 0x16 between the barrier and the slot: kept (slot %u)", (uint32_t)on.ds.pws_slot);
    }
    // g. the fallback pass never drops a slot. F85 7378 under the ring + raster delta (pad 5), with a SECOND, unslotted Apple barrier
    //    later in region 0: the first (slotted) converts at zero growth, the second's pair needs 8 dwords the pad does not hold, the
    //    converting pass is refused and the fallback pass runs - and must keep the FIRST barrier's slot (today's output byte for byte).
    {
        xlat12_draw_extra ex; std::memset(&ex, 0, sizeof ex);
        ex.ring_va = 0x23f0000000ull; ex.gs_sgpr0_va = 0x23f0a80000ull;
        ex.flags |= XLAT12_EXTRA_RASTER | XLAT12_EXTRA_RASTER_PER_DRAW; ex.rsrc3_gs = XLAT12_RSRC3_GS_CU_EN;
        // the second barrier replaces the first 8-dword packet of region 0 after the slot (a register write, packet-aligned: every other
        // input dword stays where it was) for which OFF still translates and the flagged pass runs out of room
        uint32_t first = 0; while (first < a.n && !is_draw(a.dw[first])) first++;
        uint32_t at2 = 0u;
        for (uint32_t k = 18u; !at2 && k < first; ) {
            const uint32_t l = plen(a.dw, k, a.n);
            if (!l) break;
            if (l == 8u) {
                std::vector<uint32_t> t(a.dw, a.dw + a.n);
                for (uint32_t j = 0; j < 8u; j++) t[k + j] = kBar[j];
                std::vector<uint32_t> q0(a.n), q1(a.n); xlat12_draw_stats e0, e1; uint32_t m0 = 0, m1 = 0;
                xlat12_draw_extra ey = ex;
                const uint32_t r0 = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ey, t.data(), a.n, q0.data(), &m0, &e0);
                ey.flags |= XLAT12_EXTRA_PWS;
                const uint32_t r1 = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ey, t.data(), a.n, q1.data(), &m1, &e1);
                if (r0 == 0u && r1 == 0u && e1.pws_fallback == 1u) at2 = k;
            }
            k += l;
        }
        m.assign(a.dw, a.dw + a.n);
        if (at2) for (uint32_t j = 0; j < 8u; j++) m[at2 + j] = kBar[j];
        std::vector<uint32_t> o0(a.n, 0xDEADBEEFu), o1(a.n, 0xDEADBEEFu);
        xlat12_draw_stats d0, d1; uint32_t l0 = 0, l1 = 0;
        const uint32_t s0 = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, m.data(), a.n, o0.data(), &l0, &d0);
        ex.flags |= XLAT12_EXTRA_PWS;
        const uint32_t s1 = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, m.data(), a.n, o1.data(), &l1, &d1);
        expectf(at2 != 0u && s0 == 0u && s1 == 0u && d1.pws_fallback == 1u && d1.pws_slot == 0u && d1.pws_conv == 0u && o1 == o0 &&
                is_slot(o1, 10u),
                "T13g THE FALLBACK PASS KEEPS THE SLOT: F85 7378 + the ring + raster delta with a second (unslotted) Apple barrier in "
                "place of region 0's 8-dword packet at local %u: the converting pass runs out of room, the fallback is OFF's output byte "
                "for byte, the first barrier's slot kept (fallback %u, slot %u, st %u/%u)", at2, (uint32_t)d1.pws_fallback, (uint32_t)d1.pws_slot, s0, s1);
    }
}

// T13h (0.0.539): a SLOTTED barrier the converting pass does NOT convert keeps its slot. F85's capsule (barrier + EVENT_WRITE 0xE +
// slot, 18 dwords) moved from the segment's front to its very end - after the last draw, the tail region, never converted -
// with every other dword in order: the flagged output is OFF's byte for byte, counted tail, no slot dropped. (No real captured input
// has a slotted barrier left unconverted by a converting pass: the corpus cannot see this case, so it is built here.)
static void test_slot_unconverted()
{
    const n48_fixture_p93 &a = fx("z85_0_s2");
    std::vector<uint32_t> m(a.dw + 18, a.dw + a.n);
    m.insert(m.end(), a.dw, a.dw + 18);
    n48_fixture_p93 mf = a; mf.dw = m.data();
    const Xl off = run(mf, 1, false), on = run(mf, 1, true);
    expectf(off.st == 0u && on.st == 0u && on.out == off.out && on.ds.pws_seen == 1u && on.ds.pws_tail == 1u && on.ds.pws_conv == 0u &&
            on.ds.pws_slot == 0u && is_slot(on.out, a.n - 8u),
            "T13h a slotted barrier left unconverted (the capsule moved after the last draw: the tail) keeps its slot - OFF's output byte "
            "for byte (st %u/%u, tail %u, slot %u)", off.st, on.st, (uint32_t)on.ds.pws_tail, (uint32_t)on.ds.pws_slot);
}

// T14 (0.0.539 fix round item 1, xhigh evidence MUST-FIX): THE LOGGED FRAMES REACH THE CAPSULE CHAIN. A synthetic arm of 200 committed
// frames: frames 1-15 are small (one IB, 2 rows converted), every other frame has 2 IBs and 7 converted rows, and every 4th frame from
// 140 to 164 is a chain frame (the run11ac pattern). The selection (n48_pws93_seg_pick) logs no early small frame, logs chain frames,
// never more than 64 lines, and its lines are not all among the first 64 committed frames.
static void test_seg_pick()
{
    n48_pws93_sel sel {};
    uint32_t smallLogged = 0, chainLogged = 0, lines = 0, late = 0;
    for (uint32_t fr = 1; fr <= 200u; fr++) {
        const bool small = fr < 16u;
        const uint32_t got = n48_pws93_seg_pick(&sel, small ? 1u : 2u, small ? 2u : 7u);
        lines += got;
        if (got && small) smallLogged++;
        if (got && fr >= 140u && fr <= 164u && fr % 4u == 0u) chainLogged++;
        if (got && fr > 64u) late++;
    }
    expectf(smallLogged == 0u && chainLogged >= 2u && lines == N48_PWS93_SEG_FRAMES && late > 0u && sel.lines == lines,
            "T14 the selection: early small frames logged %u (want 0); chain frames 140-164 logged %u (want >= 2); lines %u (cap 64); "
            "lines after frame 64 %u", smallLogged, chainLogged, lines, late);
    n48_pws93_sel s2 {};
    expect("T14 a one-IB frame or one with 5 converted rows is never admitted",
           n48_pws93_seg_pick(&s2, 1u, 30u) == 0u && n48_pws93_seg_pick(&s2, 3u, 5u) == 0u && s2.admitted == 0u);
}

// =============================================================================================================================
// T9: the kext's wiring
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
static void test_wiring(const char *ahhPath, const char *xlatPath)
{
    const std::string ahh = slurp(ahhPath), xl = slurp(xlatPath);
    expect("T9 AppleHardwareHook.cpp (argv[1]) and xlat12_ib.c (argv[2]) were read", ahh.size() > 100000u && xl.size() > 100000u);
    expect("T9 switch 93 is OFF at boot and changed by the verb alone",
           has(ahh, "static volatile uint32_t gPws93On { 0u };") && count_of(ahh, "__atomic_store_n(&gPws93On, ") == 1u &&
           count_of(ahh, "gPws93On = ") == 0u);
    expect("T9 the selector: 93 via n48_ra_set (M1 349 ON, M2 605 OFF), mid-arm guarded, an unknown M refused, the report",
           order(ahh, { "} else if ((arg & 0xffull) == 93ull) {",
                        "const bool contRefused93 = n48_cm_cont_switch_refused(93u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;",
                        "if (contRefused93) st = 5;",
                        "else { changed93 = n48_ra_set(m, &f93); if (changed93) __atomic_store_n(&gPws93On, f93, __ATOMIC_RELEASE); }",
                        "if (!contRefused93 && !changed93 && m != 0u) st = 11;", "pws93_report_line(contRefused93 ?" }) &&
           (93u | 1u << 8) == 349u && (93u | 2u << 8) == 605u && count_of(ahh, "== 93ull) {") == 1u);
    expect("T9 the mid-arm guard: 93 refused while a continuous arm stands, a read allowed; 114 unclaimed (112: 0.0.554; 111: 0.0.553; 109/110: 0.0.552; 94/95: 0.0.538; 107/108: 0.0.550)",
           n48_cm_cont_switch_guarded(93u) == 1u && n48_cm_cont_switch_refused(93u, 0u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 1u &&
           n48_cm_cont_switch_refused(93u, 1u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 0u &&
           n48_cm_cont_switch_refused(93u, 0u, 0u, (uint32_t)N48_CM_SHOT_ARMED) == 0u && n48_cm_cont_switch_guarded(114u) == 0u);   /* build 0.0.541: 98 and 99 are claimed; 0.0.543: 100-102; 0.0.544: 103; 0.0.547: 104-105; 0.0.550: 107-108 */
    const std::string pol = fn_text(ahh, "static void gfxsrc_policy(const GfxcVm &vm, n48_xv_frame *f, uint32_t n, uint64_t ibVa, uint32_t dp,");
    expect("T9 the policy: 93 read once per pass, WindowServer's frames only; the flag only where the translation's first packet runs "
           "(a real head, or a segment starting at its own head); the kext's check right after rect92's, before the copy guard",
           order(pol, { "const uint32_t pwOn = (gPws93On && wsBound) ? 1u : 0u;",
                        "if (r2On) { ex.flags |= XLAT12_EXTRA_RECT2D; gRect92S.segsOn++; }",
                        "if (pwOn) {",
                        "if (n48_mib_start_runs(gXdIb, &segs[k], n)) { ex.flags |= XLAT12_EXTRA_PWS; gPws93S.segsOn++; }",
                        "else gPws93S.headNotRun++;",
                        "uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from], to - from,",
                        "if (ex.flags & XLAT12_EXTRA_RECT2D) rect92_seg(&st, &ds, out, olen);",
                        "if (ex.flags & XLAT12_EXTRA_PWS) pws93_seg(&st, &ds, out, olen);",
                        "const uint32_t cgReason = navi48_cg_seg_check();" }) &&
           count_of(ahh, "ex.flags |= XLAT12_EXTRA_PWS") == 1u && count_of(ahh, "pws93_seg(&st, &ds, out, olen)") == 1u);
    const std::string seg = fn_text(ahh, "void pws93_seg(uint32_t *st, xlat12_draw_stats *ds, const uint32_t *out, uint32_t olen)");
    expect("T9 pws93_seg: every flagged segment counted; a TRANSLATED one checked by xlat12_ib_pws_check; a failure refuses VERIFY/0xD4",
           order(seg, { "gPws93S.seen += ds->pws_seen;", "gPws93S.fallback += ds->pws_fallback;", "gPws93S.slot += ds->pws_slot;",
                        "if (!*st && xlat12_ib_pws_check(out, olen) != 0u) {",
                        "*st = XLAT12_IB_ERR_VERIFY; ds->err_op = XLAT12_PWS_BACKSTOP;" }));
    const std::string cm = fn_text(ahh, "void pws93_commit(uint32_t nib)");
    expect("T9 (fix round item 1) THE COMMITTED TALLY: per-segment conversions set only where the segment's FINAL status is set, 0 unless "
           "it translated with the flag; zeroed each pass; summed only when the COMMIT gate answered yes",
           order(pol, { "for (uint32_t z = 0; z < N48_XV_MAX_SEGS; z++) gPws93SegConv[z] = 0u;",
                        "gXdBuild.seg[k].status = st;",
                        "if (k < N48_XV_MAX_SEGS) gPws93SegConv[k] = (!st && (ex.flags & XLAT12_EXTRA_PWS)) ? ds.pws_conv : 0u;" }) &&
           count_of(ahh, "gPws93SegConv[k] = ") == 1u && count_of(ahh, "pws93_commit(") == 2u &&
           has(ahh, "    if (commitOk && gPws93On) pws93_commit(f.nib);") &&
           order(cm, { "for (uint32_t k = 0; k < N48_XV_MAX_SEGS; k++) c += gPws93SegConv[k];", "gPws93S.commFrames++; gPws93S.commConv += c;",
                       "if (c) gPws93S.commWith++;" }));
    const std::string cm2 = fn_text(ahh, "void pws93_commit(uint32_t nib)");
    expect("T9 (0.0.539 item 2, fix round item 1) THE PER-SEGMENT LINE: in pws93_commit (committed frames with 93 ON only, the frame's IB "
           "count passed in), after the tally; the selection state reset per arm (gXdShot.armed_at_us); each row's head from "
           "gXdBuild.seg[k].head; the frame chosen by n48_pws93_seg_pick; one HWLOG saying segment or unit row",
           order(cm2, { "if (c) gPws93S.commWith++;",
                        "if (gPws93SegArm != gXdShot.armed_at_us) {",
                        "gPws93SegHead[k] = gPws93SegConv[k] ? gXdBuild.seg[k].head : 0u;",
                        "if (!n48_pws93_seg_pick(&gPws93Sel, nib, rows)) return;",
                        "(void)n48_pws93_segtext(gPws93SegTxt, (uint32_t)sizeof gPws93SegTxt, gPws93SegConv, gPws93SegHead, N48_XV_MAX_SEGS, &omitted);",
                        "HWLOG(N48_PWS93_SEG_FMT, (unsigned long long)(gXdC.judged + 1u), gPws93Sel.lines, nib, gXdBuild.units ? \"unit\" : \"segment\"," }) &&
           count_of(ahh, "HWLOG(N48_PWS93_SEG_FMT,") == 1u && has(ahh, "static char gPws93SegTxt[N48_PWS93_SEG_TEXT];") &&
           has(ahh, "    if (commitOk && gPws93On) pws93_commit(f.nib);") && count_of(ahh, "pws93_commit(") == 2u);
    const std::string rl = fn_text(ahh, "static void pws93_report_line(const char *how)");
    expect("T9 (0.0.539) the report: 0.0.538's first line unchanged, then the second line with the summed slot count and the "
           "per-segment cap",
           order(rl, { "HWLOG(N48_CM_PWS93_FMT,", "(unsigned long long)gPws93S.conv, (unsigned long long)gPws93S.noroom, (unsigned long long)gPws93S.tail,",
                       "HWLOG(N48_PWS93_SEG_REPORT_FMT, (unsigned long long)gPws93S.slot, gPws93Sel.lines, gPws93Sel.admitted,",
                       "(unsigned long long)(gPws93S.segLinesOver + gPws93Sel.notPrinted));" }) && count_of(ahh, "gPws93S.slot") == 2u);
    const std::string tde = fn_text(xl, "uint32_t xlat12_ib_translate_draw_ex(const xlat12_draw_profile *pf, const xlat12_draw_extra *ex, const uint32_t *in,");
    expect("T9 the PWS fallback re-runs the SAME block (goto d_r2d_pass, its state one local), first, the body never calls itself",
           order(tde, { "uint32_t pwsCtl = (ex && (ex->flags & XLAT12_EXTRA_PWS)) ? 1u : 0u;", "d_r2d_pass:",
                        "pair.pws = (pwsCtl & 1u) | (pwsCtl ? 2u : 0u);", "d_r2d_done:", "if ((pwsCtl & 1u) && r2dSt && ds && ds->pws_conv) {",
                        "pwsCtl = 2u | ", "goto d_r2d_pass;", "if ((r2dCtl & 1u) && r2dSt && ds &&", "return r2dSt;" }) &&
           count_of(tde, "xlat12_ib_translate_draw_ex(") == 1u && count_of(tde, "\n    return ") == 1u && !has(tde, "static uint32_t pwsCtl"));
    const std::string reg = fn_text(xl, "static uint32_t d_region(const xlat12_draw_profile *pf, const xlat12_draw_extra *ex, xlat12_draw_stats *ds,");
    expect("T9 the conversion lives in d_region's pass-through branch, before its verbatim copy, and only there; 0.0.539: the slot is "
           "marked only inside that conversion's branch, dropped by the NOP branch (before its copy), cleared at the region's end",
           order(reg, { "if (h == XLAT12_IB_NOP || PT(h) == 2u || PO(h) == OP_NOP) {",
                        "if (d_pws_slot_drop(ds, pr, in, i, l)) { i += l; continue; }",
                        "if (e->cap - e->n < l) { ds->err_op = OP_NOP; return XLAT12_IB_ERR_TOO_LONG; }",
                        "if (is_passthrough(op) && !is_draw(op) && op != OP_DISPATCH_DIRECT) {",
                        "if (op == OP_ACQUIRE_MEM && pr->pws && d_pws(ds, pr, e, &in[i], l, to < n ? to - i : 0u)) {",
                        "if (xlat12_ib_pws_slot_at(&in[i], to - i)) pr->pws_slot = i + 10u;",
                        "i += l; continue;",
                        "for (uint32_t k = 0; k < l; k++) out[e->n++] = in[i + k];",
                        "return XLAT12_IB_ERR_UNLISTED;",
                        "pr->pws_slot = 0u;",
                        "return 0;" }) &&
           count_of(xl, "d_pws(ds, pr, e,") == 1u && count_of(xl, "pr->pws_slot = i + 10u;") == 1u && count_of(xl, "pws_slot = ") == 2u &&
           count_of(xl, "d_pws_slot_drop(") == 2u &&
           // the room at the barrier stays the pair's own 16 dwords: no slot credit is taken there (it is taken only at the drop)
           count_of(xl, "if (!(pr->pws & 1u) || !predraw || e->cap - e->n < 16u || d_pws_fence_after(p, l, rest)) {") == 1u);
    const std::string drop = fn_text(xl, "static int d_pws_slot_drop(xlat12_draw_stats *ds, const DPair *pr, const uint32_t *in, uint32_t i, uint32_t l)");
    expect("T9 (0.0.539) d_pws_slot_drop: exactly the marked dword, re-matched (SLOT0 SLOT1, 8 dwords) before the credit, then counted",
           order(drop, { "if (!pr->pws_slot || i != pr->pws_slot) return 0;",
                         "if (!xlat12_ib_pws_slot_pkt(&in[i], l)) return 0;",
                         "if (ds->pws_slot < 0xFFFFu) ds->pws_slot++;", "return 1;" }));
    char b[1024];
    const unsigned long long M = 18446744073709551615ull;
    const int w = std::snprintf(b, sizeof b, N48_CM_PWS93_FMT, "OFF (605, default)", "`gfxneuter 93` REFUSED - a continuous arm stands, unchanged",
                                M, M, M, M, M, M, M, M, M, M, M, M, M);
    expectf(w > 0 && (uint32_t)w <= N48_LOG_CAP_BODY, "T9 the pws93 report fits 491 bytes at its widest (%d)", w);
    // build 0.0.539 item 2 / fix round item 1: the per-segment line at its widest - a full text, 20-digit frame and conv
    std::string fill(N48_PWS93_SEG_TEXT - 1u, 'x');
    const int w3 = std::snprintf(b, sizeof b, N48_PWS93_SEG_FMT, M, 4294967295u, 4294967295u, "segment", M, 4294967295u, fill.c_str(), 4294967295u);
    expectf(w3 > 0 && (uint32_t)w3 <= N48_LOG_CAP_BODY, "T9 (0.0.539) the per-segment line with a full %u-byte text fits 491 (%d)",
            N48_PWS93_SEG_TEXT - 1u, w3);
    const int w4 = std::snprintf(b, sizeof b, N48_PWS93_SEG_REPORT_FMT, M, 4294967295u, 4294967295u, M);
    expectf(w4 > 0 && (uint32_t)w4 <= N48_LOG_CAP_BODY, "T9 (0.0.539) the per-segment report line fits (%d)", w4);
    uint16_t c2[N48_XV_MAX_SEGS] = {}; uint32_t h2[N48_XV_MAX_SEGS] = {};
    c2[0] = 1u; h2[0] = 2u; c2[5] = 3u; h2[5] = 7376u; c2[31] = 12u; h2[31] = 12496u;
    char t2[64]; uint32_t o2 = 9u;
    expect("T9 (0.0.539 fix round) the text is ` k@head:conv` - the row, its head's dword in Apple's IB, its conversions - in order",
           n48_pws93_segtext(t2, sizeof t2, c2, h2, N48_XV_MAX_SEGS, &o2) == 3u && o2 == 0u && !std::strcmp(t2, " 0@2:1 5@7376:3 31@12496:12"));
    char t3[18]; uint32_t o3 = 0u;
    expect("T9 (0.0.539) a text that does not fit ends with \" +\", counts what it left out, never writes past its buffer",
           n48_pws93_segtext(t3, sizeof t3, c2, h2, N48_XV_MAX_SEGS, &o3) == 2u && o3 == 1u && !std::strcmp(t3, " 0@2:1 5@7376:3 +"));
    // a realistic worst case: 32 rows, 5-digit heads, 2-digit conversions - every entry fits
    uint16_t c4[N48_XV_MAX_SEGS]; uint32_t h4[N48_XV_MAX_SEGS];
    for (uint32_t k = 0; k < N48_XV_MAX_SEGS; k++) { c4[k] = 99u; h4[k] = 40000u + k; }
    char t4[N48_PWS93_SEG_TEXT]; uint32_t o4 = 9u;
    const uint32_t wr4 = n48_pws93_segtext(t4, sizeof t4, c4, h4, N48_XV_MAX_SEGS, &o4);
    expectf(wr4 >= 22u && std::strlen(t4) < N48_PWS93_SEG_TEXT, "T9 (0.0.539) 32 rows at 5-digit heads: %u written, %u left out, %zu bytes",
            wr4, o4, std::strlen(t4));
    expect("T9 (0.0.539) no conversion at all: an empty text", n48_pws93_segtext(t2, sizeof t2, nullptr, h2, 0u, &o4) == 0u && t2[0] == '\0' && o4 == 0u);
    expect("T9 N48_LOG_CAP_BODY is 491", N48_LOG_CAP_BODY == 491u);
}

int main(int argc, char **argv)
{
    test_positive();
    test_ordering_and_identity();
    test_tail_noroom();
    test_untouched();
    test_exact_match();
    test_md();
    test_pairing();
    test_flag();
    test_head_gate();
    test_recipe_room();
    test_fence_identity();
    test_slot();
    test_slot_unconverted();
    test_seg_pick();
    test_wiring(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr);
    std::printf("gfx_pws93: %d checks, %d failed\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
