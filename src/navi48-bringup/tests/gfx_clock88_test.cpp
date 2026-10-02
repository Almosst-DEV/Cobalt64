// gfx_clock88_test.cpp — build 0.0.512 Part B ( plan step 2, src/apple/gfx_clock88.h): the clock's-source
// instrumentation. PRINT ONLY. What is proven here:
//   B1  the per-frame stash keeps S's (identity 88) FIRST draw's records only, drops a retry's re-read, never another program's,
//       and restarts on a new frame; the CB0 scan returns the last CB_COLOR0_BASE / _EXT before the draw (a real frame-b unit:
//       tests/fixture_drawelide512.h, BA's clock layer 0x401080000 at input dword 2411); the 8-per-boot cap is the caller's.
//   B2  the watch: at most 2 surfaces, deduplicated, overlap exact at both edges, 16 lines then `unlogged`; READ-ONLY: every
//       hook is handed a buffer whose bytes must be unchanged afterwards, and the glue bodies (Navi48Bringup.cpp,
//       AppleHardwareHook.cpp, Navi48AccelPeer.cpp) hold no write primitive and sit where the copy / write they watch sits.
//   B3  the shape classes (8736|14352, 8736|14544), all and armed.
//   LINES every format fits under N48_LOG_CAP_BODY (the 512-byte n48_logf line less the longest prefix) at worst-case values.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 src/navi48-bringup/tests/gfx_clock88_test.cpp -o /tmp/c88 && \
//         /tmp/c88 src/navi48-bringup/src/Navi48Bringup.cpp src/navi48-bringup/src/apple/AppleHardwareHook.cpp \
//         src/navi48-bringup/src/apple/Navi48AccelPeer.cpp
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdlib>
#include "gfx_clock88.h"
#include "gfx_commit.h"            // N48_LOG_CAP_BODY
#include "fixture_drawelide512.h"  // a real frame-b unit (run10t F107 k11)
#include "fixture_clock88_run10v.h"  // build 0.0.521: run10v F51 (the producer) and F55 (frame b), whole frames

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-96s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-96s %#llx\n", what, (unsigned long long)got);
}
static std::string slurp(const char *p)
{
    std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
static std::string body(const std::string &src, const char *sig)
{
    const size_t a = src.find(sig);
    if (a == std::string::npos) return std::string();
    const size_t b = src.find("\n}\n", a);
    return b == std::string::npos ? std::string() : src.substr(a, b - a);
}
static uint32_t count(const std::string &h, const char *n)
{
    uint32_t c = 0; for (size_t p = h.find(n); p != std::string::npos; p = h.find(n, p + 1)) c++; return c;
}
// the write primitives no read-only hook may hold
static uint32_t writes_in(const std::string &b)
{
    static const char *const kW[] = { "write_mm(", "writeBytes", "WVRAM", "navi48_fc_chunk", "IOMalloc", "memcpy(", "memset(",
                                      "WREG", "wreg(", "navi48_cg_open", "->write", "hw_", "gXdNew[", "gXdOut[" };
    uint32_t c = 0; for (const char *w : kW) c += count(b, w); return c;
}

static void b1_checks()
{
    std::printf("== B1: the per-frame stash ==\n");
    n48_c88_frame s; std::memset(&s, 0, sizeof s);
    uint32_t rec[8] = { 0x04023800u, 0xc4700000u, 0x801fc04fu, 0x91b00facu, 0u, 0x00400000u, 0u, 0u };
    const uint32_t keep[8] = { rec[0], rec[1], rec[2], rec[3], rec[4], rec[5], rec[6], rec[7] };
    expect_u("B1 another program (BA's identity) is never kept", n48_c88_note(&s, 7u, (120ull << 32) | 0x8e1812e4ull, 2411u, rec, 0x402380000ull, 3u), 0u);
    expect_u("B1 S's first record of frame 7 is kept", n48_c88_note(&s, 7u, N48_C88_PS_ID, 13251u, rec, 0x402230000ull, 10u), 1u);
    expect_u("B1   ... the observer's record is untouched (read-only)", std::memcmp(rec, keep, sizeof rec) == 0, 1u);
    expect_u("B1 a retry's re-read of the same draw and texture is not kept twice", n48_c88_note(&s, 7u, N48_C88_PS_ID, 13251u, rec, 0x402230000ull, 10u), 0u);
    expect_u("B1 a SECOND S draw of the same frame is not kept (the first draw only)", n48_c88_note(&s, 7u, N48_C88_PS_ID, 14000u, rec, 0x402230000ull, 10u), 0u);
    expect_u("B1 texture 1 of the same draw is kept (up to N48_C88_TEX)", n48_c88_note(&s, 7u, N48_C88_PS_ID, 13251u | (1u << 24), rec, 0x401380000ull, 10u), 1u);
    expect_u("B1   ... and a third is not", n48_c88_note(&s, 7u, N48_C88_PS_ID, 13251u | (2u << 24), rec, 0x401390000ull, 10u), 0u);
    expect_u("B1 the stash holds 2 records, VA and texture index as observed", s.n * 0x100u + s.tex[1] * 0x10u + (s.va[1] == 0x401380000ull), 0x211u);
    s.printed = 1u;
    expect_u("B1 a new frame restarts the stash (printed cleared)", n48_c88_note(&s, 8u, N48_C88_PS_ID, 13251u, rec, 0x402230000ull, 10u) * 0x10u + s.n + s.printed, 0x11u);
    expect_u("B1 S's identity is kDTableAbi's {85, 0xd0a62abe}", N48_C88_PS_ID, (85ull << 32) | 0xd0a62abeull);
    // CB0 on a real unit: BA's draw at input dword 2411 of run10t F107 k11 draws into the clock layer (: 0x401080000)
    const DeFx &b = kDeFxT107K11;
    expect_u("B1 CB0 at BA's draw (2411) of the real frame-b unit is the clock layer 0x401080000", n48_c88_cb0(b.in, b.n, 2411u), 0x401080000ull);
    expect_u("B1 CB0 at BD's draw (1031) is X 0x404800000", n48_c88_cb0(b.in, b.n, 1031u), 0x404800000ull);
    expect_u("B1 CB0 before any CB0 write is 0", n48_c88_cb0(b.in, b.n, 100u), 0u);
    const uint32_t ext[6] = { 0xC0016900u, 0x318u, 0x01234567u, 0xC0016900u, 0x390u, 0x000000ABu };
    expect_u("B1 CB_COLOR0_BASE_EXT supplies VA [47:40]", n48_c88_cb0(ext, 6u, 6u), 0xAB0123456700ull);
    expect_u("B1 a write AT or after the draw is not in force", n48_c88_cb0(ext, 6u, 3u), 0x0123456700ull);
    expect_u("B1 NULL input: 0", n48_c88_cb0(nullptr, 6u, 6u), 0u);
}

static const uint32_t *big0() { static uint32_t b[8] = { 0u, 3u << 30, 0x3FFFu | (0xFFFFu << 14), 0u, 0u, 0u, 0u, 0u }; return b; }
/* build 0.0.546 ( instrument defect): watches die at unmapVA. RUN AU's case: WindowServer unmaps `va 0x400450000 size
 * 0x30000` (S's 259x111 format-50 image), then residency copy #316 lands at that VA: no HIT. */
static void b2u_checks()
{
    std::printf("== B2U (0.0.546): unmapVA kills a watch ==\n");
    n48_c88_watch w; std::memset(&w, 0, sizeof w);
    uint32_t idx = 99u;
    const uint32_t ws = 7u, other = 9u;
    expect_u("B2U add S's surface in context 7", n48_c88_watch_add_ctx(&w, ws, 0x400450000ull, 0x30000ull, 0x7f000000ull, 1u, &idx) * 0x10u + idx, 0x10u);
    expect_u("B2U before the unmap, the copy's VA hits", (uint64_t)n48_c88_hit_va(&w, 0x400450000ull, 0x10000ull), 0u);
    expect_u("B2U another context's unmap of the same VA does not kill it", n48_c88_unmap(&w, other, 0x400450000ull, 0x30000ull), 0u);
    expect_u("B2U an unmap of the same context just past the range does not kill it", n48_c88_unmap(&w, ws, 0x400480000ull, 0x10000ull), 0u);
    expect_u("B2U the context's own unmap of the range kills watch 0 (mask 1)", n48_c88_unmap(&w, ws, 0x400450000ull, 0x30000ull), 1u);
    expect_u("B2U ... counted once", w.kills, 1u);
    expect_u("B2U ... a second unmap of it kills nothing", n48_c88_unmap(&w, ws, 0x400450000ull, 0x30000ull), 0u);
    expect_u("B2U #316 at the same VA after the unmap: NO hit", (uint64_t)(int64_t)n48_c88_hit_va(&w, 0x400450000ull, 0x10000ull), (uint64_t)(int64_t)-1);
    expect_u("B2U ... nor an MM write onto its old VRAM", (uint64_t)(int64_t)n48_c88_hit_vram(&w, 0x7f000000ull, 0x100ull), (uint64_t)(int64_t)-1);
    expect_u("B2U a NEW surface set at the same VA re-arms the slot (added, index 0)",
             n48_c88_watch_add_ctx(&w, ws, 0x400450000ull, 0x20000ull, 0x7e000000ull, 1u, &idx) * 0x10u + idx, 0x10u);
    expect_u("B2U ... live again with its new VRAM", ((uint64_t)n48_c88_hit_vram(&w, 0x7e000000ull, 0x100ull) == 0u &&
             n48_c88_hit_vram(&w, 0x7f000000ull, 0x100ull) == -1 && w.n == 1u) ? 1u : 0u, 1u);
    expect_u("B2U ... adding it again while live is not an add", n48_c88_watch_add_ctx(&w, ws, 0x400450000ull, 0x20000ull, 0u, 0u, &idx), 0u);
    expect_u("B2U an unmap of size 0 (the whole space) kills it", n48_c88_unmap(&w, ws, 0u, 0u), 1u);
    n48_c88_watch u; std::memset(&u, 0, sizeof u);
    (void)n48_c88_watch_add(&u, 0x401000000ull, 0x10000ull, 0u, 0u, &idx);   /* context unknown (0): any context's unmap kills it */
    expect_u("B2U a watch of unknown context dies at any context's overlapping unmap", n48_c88_unmap(&u, other, 0x401008000ull, 0x1000ull), 1u);
    expect_u("B2U NULL is safe", n48_c88_unmap(nullptr, ws, 0u, 0u), 0u);
}

static void b2_checks()
{
    std::printf("== B2: the watch ==\n");
    n48_c88_watch w; std::memset(&w, 0, sizeof w);
    uint32_t idx = 99u;
    expect_u("B2 add the first surface", n48_c88_watch_add(&w, 0x402230000ull, 0x30000ull, 0x7f000000ull, 1u, &idx) * 0x10u + idx, 0x10u);
    expect_u("B2 the same VA again: not added, its index", n48_c88_watch_add(&w, 0x402230000ull, 0x30000ull, 0u, 0u, &idx) * 0x10u + idx, 0x0u);
    expect_u("B2 a second surface (VRAM unresolved)", n48_c88_watch_add(&w, 0x401380000ull, 0x10000ull, 0u, 0u, &idx) * 0x10u + idx, 0x11u);
    expect_u("B2 a third and a fourth (0.0.521: the cap is 4)", n48_c88_watch_add(&w, 0x401390000ull, 0x10000ull, 0u, 0u, &idx) +
             n48_c88_watch_add(&w, 0x4013a0000ull, 0x10000ull, 0u, 0u, &idx), 2u);
    expect_u("B2 a fifth: refused, the cap is 4", n48_c88_watch_add(&w, 0x4013b0000ull, 0x10000ull, 0u, 0u, &idx) * 0x10u + w.n, 0x4u);
    expect_u("B2 VA: a copy ending exactly at the base does not hit", (uint64_t)(int64_t)n48_c88_hit_va(&w, 0x402220000ull, 0x10000ull), (uint64_t)(int64_t)-1);
    expect_u("B2 VA: a copy's last byte inside the base hits watch 0", (uint64_t)n48_c88_hit_va(&w, 0x402220000ull, 0x10001ull), 0u);
    expect_u("B2 VA: starting at the end does not hit", (uint64_t)(int64_t)n48_c88_hit_va(&w, 0x402260000ull, 0x1000ull), (uint64_t)(int64_t)-1);
    expect_u("B2 VA: inside the second surface hits watch 1", (uint64_t)n48_c88_hit_va(&w, 0x401381000ull, 0x100ull), 1u);
    expect_u("B2 VRAM: inside watch 0's run hits", (uint64_t)n48_c88_hit_vram(&w, 0x7f010000ull, 0x100ull), 0u);
    expect_u("B2 VRAM: watch 1 has no resolved VRAM, never hits", (uint64_t)(int64_t)n48_c88_hit_vram(&w, 0u, 0x100000000ull) == 0u, 1u);
    expect_u("B2 zero-length ranges never hit", (uint64_t)(int64_t)n48_c88_hit_va(&w, 0x402230000ull, 0u), (uint64_t)(int64_t)-1);
    uint32_t ln = 0u, got = 0u;
    for (uint32_t i = 0; i < 20u; i++) { const uint32_t l = n48_c88_take_line(&w); if (l) { got++; ln = l; } }
    expect_u("B2 16 lines of 20 hits, the last numbered 16, 4 past the cap", got * 0x10000u + ln * 0x100u + (uint32_t)w.unlogged, 0x101004u);
    /* 299 x 113 (the input's shape): WIDTH - 1 = 298 = w1 [31:30] 2 | w2 [13:0] 74 << 2; HEIGHT - 1 = 112 = w2 [29:14] */
    uint32_t t2[8] = { 0x04022300u, 0x00d00004u | (2u << 30), 74u | (112u << 14), 0u, 0u, 0u, 0u, 0u };
    expect_u("B2 extent: 299x113 x4 bytes = 0x21004 -> 0x30000 (64 KiB granular)", n48_c88_extent(t2, 4u), 0x30000ull);
    /* RE-BASELINED 0.0.546: an unknown element size was assumed 16 B (at least 64 KiB); now the FIRST PAGE only */
    expect_u("B2 extent: element size unknown (0) -> the first page only, never an estimate", n48_c88_extent(t2, 0u), N48_C88_EXT_UNKNOWN);
    expect_u("B2 extent: ... 0x1000", N48_C88_EXT_UNKNOWN, 0x1000u);
    expect_u("B2 extent: ... even for a huge T# (no 16 B assumption)", n48_c88_extent(big0(), 0u), 0x1000u);
    expect_u("B2 extent: a known size still gets at least 64 KiB", n48_c88_extent(t2, 1u) >= N48_C88_EXT_MIN, 1u);
    uint32_t big[8] = { 0u, 3u << 30, 0x3FFFu | (0xFFFFu << 14), 0u, 0u, 0u, 0u, 0u };
    expect_u("B2 extent: clamped at 32 MiB", n48_c88_extent(big, 16u), N48_C88_EXT_MAX);
    expect_u("B2 extent: NULL is the minimum", n48_c88_extent(nullptr, 4u), N48_C88_EXT_MIN);
    // READ-ONLY: the hooks' inputs are unchanged; the watch state moves only by its own counters
    uint32_t buf[64]; for (uint32_t i = 0; i < 64u; i++) buf[i] = 0xA5000000u + i;
    uint32_t snap[64]; std::memcpy(snap, buf, sizeof buf);
    w.mmRunEnd = 0x5A5A5A5Aull;   /* a sentinel no hook would write */
    n48_c88_watch w2; std::memcpy(&w2, &w, sizeof w);
    (void)n48_c88_hit_vram(&w, 0x7f000000ull, sizeof buf); (void)n48_c88_hit_va(&w, 0x402230000ull, sizeof buf);
    (void)n48_c88_extent(buf, 4u); (void)n48_c88_cb0(buf, 64u, 64u);
    expect_u("B2 READ-ONLY: the hooks leave the written bytes exactly as they were", std::memcmp(buf, snap, sizeof buf) == 0, 1u);
    expect_u("B2 READ-ONLY: a hit test leaves the watch state unchanged", std::memcmp(&w, &w2, sizeof w) == 0, 1u);
}

// build 0.0.513 gated B1/B2 on the shape 8736|14352; build 0.0.521 on CONTENT: only a frame
// n48_c88_ct_judge called the producer spends the cap (GLUE below pins that the frame end asks gfxsrc_c88_producer first).
static void gate_checks()
{
    std::printf("== GATE (0.0.521): only a CONTENT producer spends the cap, whatever its shape ==\n");
    uint32_t lines = 0u, spent = 0u;
    for (uint32_t i = 0; i < 6u; i++) spent += n48_c88_take_frame(&lines, 0u);
    expect_u("GATE six frames the content scan did not call the producer spend nothing", spent * 0x10u + lines, 0u);
    expect_u("GATE a producer frame then gets line 1", n48_c88_take_frame(&lines, 1u), 1u);
    for (uint32_t i = 0; i < 100u; i++) (void)n48_c88_take_frame(&lines, 1u);
    expect_u("GATE the cap holds at N48_C88_LINES (32)", lines * 0x100u + N48_C88_LINES, 32u * 0x100u + 32u);
    expect_u("GATE past the cap: 0", n48_c88_take_frame(&lines, 1u), 0u);
    expect_u("GATE NULL counter: 0", n48_c88_take_frame(nullptr, 1u), 0u);
    uint32_t boot = 0u;
    for (uint32_t i = 0; i < 80u; i++) (void)n48_c88_take_frame(&boot, 0u);
    expect_u("GATE after 80 non-producer frames the producer is still logged (line 1)", n48_c88_take_frame(&boot, 1u), 1u);
}

// build 0.0.521 Part B: THE CONTENT SCAN on real frames. The key lookup is the fixture's table (the kext's memo, as its own
// PROVENANCE lines printed it for run10v); `gKeyMiss` makes S's VA unknown to it (an evicted memo row).
static uint32_t gKeyMiss = 0u, gKeyAsks = 0u;
static uint64_t fx_key(void *ctx, uint64_t va)
{
    (void)ctx; gKeyAsks++;
    if (gKeyMiss && va == 0x40002c100ull) return 0ull;
    for (const C88Key &k : kC88Keys) if (k.va == va) return k.key;
    return 0ull;
}
static void scan_frame(n48_c88_ct *c, const C88Fx &f, n48_c88_keyfn key = &fx_key)
{
    n48_c88_ct_begin(c, f.frame);
    for (uint32_t k = 0; k < f.nib; k++) n48_c88_ct_walk(c, f.ib[k], f.len[k], k, key, nullptr);
}
static void content_checks(const char *adoptJson)
{
    std::printf("== CONTENT (0.0.521): the producer is a frame with S's draw while CB0 is bound ==\n");
    static n48_c88_ct c;
    scan_frame(&c, kC88F51);
    expect_u("CT F51 (8736|14160) walks both IBs to the end", c.ibs * 0x10u + c.stops, 0x20u);
    expect_u("CT F51 is THE PRODUCER", c.producer, 1u);
    expect_u("CT F51: one S draw, IB 1 dword 13059 (the draw names)", c.sDraws * 0x1000000ull + c.sIb * 0x100000ull + c.sAt, 0x1000000ull + 0x100000ull + 13059u);
    expect_u("CT F51: its CB0 is this boot's SDF 0x401560000 (IB1 12197 `c00e6900 00000318 04015600`)", c.sCb0, 0x401560000ull);
    expect_u("CT F51: S's PS VA 0x40002c100", c.sPs, 0x40002c100ull);
    expect_u("CT F51: fewer unkeyed asks than asks (the fixture keys only PS VAs the kext printed: TEST ASSUMPTION)", c.unkeyed < c.asks, 1u);
    std::printf("      F51: %u key asks for its PS changes, %u unkeyed in the fixture's table\n", c.asks, c.unkeyed);
    scan_frame(&c, kC88F55);
    expect_u("CT F55 (8736|14352, frame b) is NOT the producer", c.producer * 0x10u + c.sDraws, 0u);
    expect_u("CT F55 still drew with CB0 bound (its PS VAs were asked, not skipped)", c.asks > 0u && c.ibs == 2u && c.stops == 0u, 1u);
    expect_u("CT F51's shape is 8736|14160 (the 0.0.513 shape gate never took it)", kC88F51.len[0] * 0x10000ull + kC88F51.len[1], 8736ull * 0x10000ull + 14160ull);
    expect_u("CT F55's shape is the old gate's 8736|14352 (and it is frame b)", kC88F55.len[0] * 0x10000ull + kC88F55.len[1], 8736ull * 0x10000ull + 14352ull);
    // an evicted memo row: S's VA unkeyed -> not the producer (under-count only)
    gKeyMiss = 1u; scan_frame(&c, kC88F51);
    expect_u("CT F51 with S's memo row gone: not the producer, counted unkeyed", c.producer * 0x10u + (c.unkeyed >= 1u), 1u);
    gKeyMiss = 0u;
    scan_frame(&c, kC88F51, nullptr);
    expect_u("CT NULL key function: nothing is the producer", c.producer, 0u);
    // the lookup is asked once per PS CHANGE, never per draw
    gKeyAsks = 0u; scan_frame(&c, kC88F51);
    expect_u("CT key asks == the scan's own count (one per change of the PS drawn)", gKeyAsks == c.asks && c.asks > 0u, 1u);
    // READ-ONLY over Apple's bytes
    std::vector<uint32_t> cp(kC88F51Ib1, kC88F51Ib1 + kC88F51.len[1]);
    n48_c88_ct_begin(&c, 1u); n48_c88_ct_walk(&c, cp.data(), (uint32_t)cp.size(), 1u, &fx_key, nullptr);
    expect_u("CT the walk leaves the IB exactly as it was", std::memcmp(cp.data(), kC88F51Ib1, cp.size() * 4u) == 0, 1u);
    // synthetic: S bound, then a draw BEFORE any CB0 write (not counted), then CB0 0 (unbound: not counted), then CB0 set (counted)
    const uint32_t syn[] = {
        0xC0027600u, 0x08u, 0x00000000u, 0x00000000u,                /* SET_SH_REG PGM_LO_PS / _HI_PS (S's VA, set below) */
        0xC0012D00u, 3u, 2u,                                          /* DRAW_INDEX_AUTO: no CB0 written yet */
        0xC0016900u, 0x318u, 0u,                                      /* CB_COLOR0_BASE = 0 */
        0xC0012D00u, 3u, 2u,                                          /* DRAW: CB0 unbound */
        0xFFFF1000u,                                                  /* the one-dword filler */
        0xC0016900u, 0x318u, 0x04015600u, 0xC0016900u, 0x390u, 0x0u,  /* CB0 = 0x401560000 */
        0xC0012D00u, 3u, 2u,                                          /* DRAW: counted */
        0xC0012D00u, 3u, 2u };                                        /* DRAW: the same PS, counted again */
    uint32_t s2[sizeof syn / 4]; std::memcpy(s2, syn, sizeof syn);
    s2[2] = (uint32_t)(0x40002c100ull >> 8); s2[3] = (uint32_t)(0x40002c100ull >> 40);   /* PS = S's VA 0x40002c100 */
    n48_c88_ct_begin(&c, 2u); n48_c88_ct_walk(&c, s2, (uint32_t)(sizeof s2 / 4), 0u, &fx_key, nullptr);
    expect_u("CT synthetic: two S draws counted (not the one before CB0, not the one with CB0 = 0), first at dword 20, one ask",
             c.producer * 0x10000u + c.sDraws * 0x100u + c.sAt + c.asks * 0x100000u, 0x100000u + 0x10000u + 0x200u + 20u);
    // CB0 written in IB0 is in force for an S draw in IB1 (the frame's order)
    const uint32_t i0[] = { 0xC0016900u, 0x318u, 0x04015600u };
    const uint32_t i1[] = { 0xC0027600u, 0x08u, (uint32_t)(0x40002c100ull >> 8), 0u, 0xC0012D00u, 3u, 2u };
    n48_c88_ct_begin(&c, 3u); n48_c88_ct_walk(&c, i0, 3u, 0u, &fx_key, nullptr); n48_c88_ct_walk(&c, i1, 7u, 1u, &fx_key, nullptr);
    expect_u("CT CB0 from IB0 carries into IB1's S draw", c.producer * 0x100u + c.sIb * 0x10u + (c.sCb0 == 0x401560000ull), 0x111u);
    // PGM_HI_PS is part of the VA: S's low bits with HI 1 are another program (0x1040002c100), never S
    const uint32_t hi1[] = { 0xC0016900u, 0x318u, 0x04015600u, 0xC0027600u, 0x08u, (uint32_t)(0x40002c100ull >> 8), 1u, 0xC0012D00u, 3u, 2u };
    n48_c88_ct_begin(&c, 6u); n48_c88_ct_walk(&c, hi1, 10u, 0u, &fx_key, nullptr);
    expect_u("CT S's low VA bits with PGM_HI_PS 1 are not S", c.producer, 0u);
    // a packet running past the IB ends the walk (counted), nothing after it is read
    const uint32_t cut[] = { 0xC0016900u, 0x318u, 0x04015600u, 0xC0027600u, 0x08u };
    n48_c88_ct_begin(&c, 4u); n48_c88_ct_walk(&c, cut, 5u, 0u, &fx_key, nullptr);
    expect_u("CT a truncated packet stops the walk (stops 1)", c.stops, 1u);
    // another program with CB0 bound: asked, not the producer
    const uint32_t o1[] = { 0xC0016900u, 0x318u, 0x04015600u, 0xC0027600u, 0x08u, (uint32_t)(0x401161900ull >> 8), 0u, 0xC0012D00u, 3u, 2u };
    n48_c88_ct_begin(&c, 5u); n48_c88_ct_walk(&c, o1, 10u, 0u, &fx_key, nullptr);
    expect_u("CT BD (identity 78) drawing with CB0 bound: asked once, not the producer", c.asks * 0x10u + c.producer, 0x10u);
    // S's key is the generated rows' key for ws_S_TimgXhu_Idfr (src/xlat12/xlat12_adopt_rows.json)
    const std::string js = slurp(adoptJson);
    const size_t nm = js.find("\"ws_S_TimgXhu_Idfr\""), kk = nm == std::string::npos ? nm : js.find("\"keys\"", nm);
    const size_t q0 = kk == std::string::npos ? kk : js.find("\"0x", kk);
    const uint64_t jkey = q0 == std::string::npos ? 0ull : std::strtoull(js.c_str() + q0 + 1, nullptr, 16);
    expect_u("CT N48_C88_S_KEY is the adopted rows' key for ws_S_TimgXhu_Idfr", jkey, N48_C88_S_KEY);
    uint32_t sInFx = 0u; for (const C88Key &k : kC88Keys) if (k.id == 88) sInFx += (k.key == N48_C88_S_KEY);
    expect_u("CT ... and the key run10v's kext printed for identity 88", sInFx, 1u);
}

// build 0.0.521: THE PER-TARGET BUDGETS over the real producer sequences. Every content-producer frame of the first 160 frames
// of run10t / run10u / run10v, in order, as its first S draw's CB0 (this header's walk over each capture, with the keys the kext
// printed for that boot; the builder's census). The clock's SDF target comes LAST in each (run10t F86 0x402380000, run10u F95
// 0x402540000, run10v F51 0x401560000): with every log spent the way the kext spends it, it must still get a line.
static const uint64_t kSeq_run10t[] = { 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x401100000ull, 0x402380000ull };
static const uint64_t kSeq_run10u[] = { 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x401700000ull, 0x400460000ull, 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x400460000ull, 0x401100000ull, 0x400460000ull, 0x400460000ull, 0x401100000ull, 0x402540000ull };
static const uint64_t kSeq_run10v[] = { 0x400400000ull, 0x400600000ull, 0x400400000ull, 0x400400000ull, 0x400600000ull, 0x400400000ull, 0x400400000ull, 0x400600000ull, 0x400400000ull, 0x400400000ull, 0x400600000ull, 0x400400000ull, 0x400400000ull, 0x400600000ull, 0x400400000ull, 0x400600000ull, 0x401560000ull };
static void boot_checks()
{
    std::printf("== BOOT (0.0.521): the clock's SDF frame is logged after every start-up S frame ==\n");
    struct { const char *nm; const uint64_t *v; uint32_t n; uint64_t sdf; } runs[3] = {
        { "run10t", kSeq_run10t, (uint32_t)(sizeof kSeq_run10t / 8), 0x402380000ull },
        { "run10u", kSeq_run10u, (uint32_t)(sizeof kSeq_run10u / 8), 0x402540000ull },
        { "run10v", kSeq_run10v, (uint32_t)(sizeof kSeq_run10v / 8), 0x401560000ull } };
    for (auto &r : runs) {
        static n48_c88_tgt t; std::memset(&t, 0, sizeof t);
        uint32_t ct = 0u, b1 = 0u; uint64_t gl = 64ull, sl = 0ull;   /* the general mibseg-detail cap already spent (run10v: by F123) */
        uint32_t lastCt = 0u, lastDet = 0u, lastB1 = 0u;
        for (uint32_t i = 0; i < r.n; i++) {
            uint32_t c = 0u;
            if (ct < N48_C88_CLINES && n48_c88_tgt_take(&t, r.v[i], N48_C88_TGT_K_CT, N48_C88_TGT_CT)) { ct++; c = 1u; }
            const uint32_t d = n48_c88_detail_take(&gl, 64ull, &sl, n48_c88_tgt_take(&t, r.v[i], N48_C88_TGT_K_DETAIL, N48_C88_TGT_DETAIL));
            const uint32_t b = n48_c88_take_frame(&b1, b1 < N48_C88_LINES ? n48_c88_tgt_take(&t, r.v[i], N48_C88_TGT_K_B1, N48_C88_TGT_B1) : 0u);
            if (i + 1u == r.n) { lastCt = c; lastDet = d; lastB1 = b ? 1u : 0u; }
        }
        char w[160]; std::snprintf(w, sizeof w, "BOOT %s: %u producer frames; the last (the SDF %#llx) gets clock88c, detail (own cap), B1", r.nm, r.n, (unsigned long long)r.sdf);
        expect_u(w, (r.v[r.n - 1u] == r.sdf) * 0x1000u + lastCt * 0x100u + lastDet * 0x10u + lastB1, 0x1000u + 0x100u + 0x20u + 1u);
        std::printf("      %s: targets %u over %u; clock88c %u, detail(own) %llu, B1 %u\n", r.nm, t.n, t.over, ct, (unsigned long long)sl, b1);
    }
    expect_u("BOOT 0.0.513's 8-frame cap by content alone would be spent before the SDF frame in all three (why per target)",
             (sizeof kSeq_run10t / 8 > 9u) + (sizeof kSeq_run10u / 8 > 9u) + (sizeof kSeq_run10v / 8 > 9u), 3u);
    static n48_c88_tgt t; std::memset(&t, 0, sizeof t);
    uint32_t got = 0u;
    for (uint32_t i = 0; i < 40u; i++) got += n48_c88_tgt_take(&t, 0x400000000ull + 0x10000ull * i, N48_C88_TGT_K_CT, 4u);
    expect_u("TGT 40 distinct targets: 32 taken, 8 over", got * 0x100u + t.over, 32u * 0x100u + 8u);
    uint32_t took = 0u;
    for (uint32_t i = 0; i < 4u; i++) took += n48_c88_tgt_take(&t, 0x400000000ull, N48_C88_TGT_K_DETAIL, 4u);
    const uint32_t det = n48_c88_tgt_take(&t, 0x400000000ull, N48_C88_TGT_K_DETAIL, 4u), ctk = n48_c88_tgt_take(&t, 0x400000000ull, N48_C88_TGT_K_CT, 4u);
    expect_u("TGT a target's budget per kind: exactly 4 detail lines, the 5th refused; its clock88c budget is separate", took * 0x100u + det * 0x10u + ctk, 0x401u);
    expect_u("TGT zero VA, bad kind, NULL table: 0", n48_c88_tgt_take(&t, 0ull, 0u, 4u) + n48_c88_tgt_take(&t, 0x400000000ull, N48_C88_TGT_KINDS, 4u) +
             n48_c88_tgt_take(nullptr, 0x400000000ull, 0u, 4u), 0u);
}

// build 0.0.521 Part C: the mibseg-detail budget - producer frames under their own cap, then the general one.
static void detail_checks()
{
    std::printf("== DETAIL (0.0.521 Part C): producer frames keep a cap of their own ==\n");
    uint64_t lines = 0ull, sl = 0ull;
    for (uint32_t i = 0; i < 70u; i++) (void)n48_c88_detail_take(&lines, 64ull, &sl, 0u);
    expect_u("DETAIL 70 ordinary frames: 64 logged, the general cap spent, the producer cap untouched", lines * 0x100u + sl, 64u * 0x100u);
    expect_u("DETAIL a producer frame after that is still logged (its own cap: answer 2)", n48_c88_detail_take(&lines, 64ull, &sl, 1u), 2u);
    for (uint32_t i = 0; i < 300u; i++) (void)n48_c88_detail_take(&lines, 64ull, &sl, 1u);
    expect_u("DETAIL the producer cap holds at N48_C88_DETAIL_S (128)", sl * 0x1000u + N48_C88_DETAIL_S, 128u * 0x1000u + 128u);
    uint64_t l2 = 0ull, s2 = N48_C88_DETAIL_S;
    expect_u("DETAIL producer cap spent, general cap free: logged under the general cap (answer 1)", n48_c88_detail_take(&l2, 64ull, &s2, 1u) * 0x10u + l2, 0x11u);
    l2 = 0ull; s2 = 0ull;
    const uint32_t ord = n48_c88_detail_take(&l2, 64ull, &s2, 0u);
    expect_u("DETAIL an ordinary frame never spends the producer cap", ord * 0x10u + s2, 0x10u);
    expect_u("DETAIL NULL counters: 0", n48_c88_detail_take(nullptr, 64ull, &s2, 1u) + n48_c88_detail_take(&l2, 64ull, nullptr, 1u), 0u);
    n48_c88_shapes s; std::memset(&s, 0, sizeof s);
    n48_c88_prod_note(&s, 1u, 51u, 1u); n48_c88_prod_note(&s, 0u, 55u, 1u); n48_c88_prod_note(&s, 1u, 220u, 0u);
    expect_u("B3 content: 2 producers (armed 1), last 220; a non-producer is not counted", s.prod * 0x10000u + s.prodArmed * 0x1000u + s.prodLast, 0x20000u + 0x1000u + 220u);
}

static void b3_checks()
{
    std::printf("== B3: the shape classes ==\n");
    n48_c88_shapes s; std::memset(&s, 0, sizeof s);
    const uint32_t p[2] = { 8736u, 14352u }, b[2] = { 8736u, 14544u }, e[2] = { 8768u, 12912u }, a[2] = { 1152u, 14944u };
    n48_c88_shape_note(&s, 2u, p, 1u); n48_c88_shape_note(&s, 2u, p, 0u);
    n48_c88_shape_note(&s, 2u, b, 1u); n48_c88_shape_note(&s, 2u, b, 1u); n48_c88_shape_note(&s, 2u, b, 0u);
    n48_c88_shape_note(&s, 2u, e, 1u); n48_c88_shape_note(&s, 2u, a, 1u); n48_c88_shape_note(&s, 1u, b, 1u);
    const uint32_t b3[3] = { 8736u, 14544u, 40u };
    n48_c88_shape_note(&s, 3u, b3, 1u);
    expect_u("B3 the producer 2 (armed 1), frame b 3 (armed 2); e, a, nib 1 and nib 3 not counted",
             s.p14352 * 0x1000u + s.p14352Armed * 0x100u + s.b14544 * 0x10u + s.b14544Armed, 0x2132u);
}

static void line_checks()
{
    std::printf("== LINES: every format under N48_LOG_CAP_BODY (%u) at worst case ==\n", N48_LOG_CAP_BODY);
    char bf[2048]; const unsigned long long M = 0xffffffffffffffffull; const uint32_t U = 0xffffffffu;
    int n = std::snprintf(bf, sizeof bf, N48_C88_FMT, M, U, U, U, U, M, 999999u, M, U, U, M, U, M, U, U, U, U, U, U, U, U, -1, U, U);
    expect_u("LINE clock88 (B1)", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u); std::printf("      %d bytes\n", n);
    n = std::snprintf(bf, sizeof bf, N48_C88_COPY_FMT, U, M, M, M, M, M, M, U, U, M, 999999u, M, -2147483647, "0123456789abcdefghijklmnopqrstu", U, U);
    expect_u("LINE clock88w copy (B2)", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u); std::printf("      %d bytes\n", n);
    n = std::snprintf(bf, sizeof bf, N48_C88_MM_FMT, U, M, M, M, M, U, U, M, 999999u, M, -2147483647, "0123456789abcdefghijklmnopqrstu", U, U);
    expect_u("LINE clock88w MM (B2)", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u); std::printf("      %d bytes\n", n);
    n = std::snprintf(bf, sizeof bf, N48_C88_DEAD_FMT, U, M, M, U, M, M, M, U, U);
    expect_u("LINE clock88w DEAD (0.0.546)", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u); std::printf("      %d bytes\n", n);
    n = std::snprintf(bf, sizeof bf, N48_C88_ADD_FMT, U, U, M, M, U, U, U, M, " - VRAM UNRESOLVED (MM-window writes not watched)");
    expect_u("LINE clock88w SET (B2)", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u); std::printf("      %d bytes\n", n);
    n = std::snprintf(bf, sizeof bf, N48_C88_REPORT_FMT, M, M, M, M, U, U, M, U, M, M, U, U, M);
    expect_u("LINE clock88 report (B3)", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u); std::printf("      %d bytes\n", n);
    n = std::snprintf(bf, sizeof bf, N48_C88_CREPORT_FMT, M, M, M, M, U, U, M, U, U, U, U);
    expect_u("LINE clock88 content report (0.0.521)", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u); std::printf("      %d bytes\n", n);
    n = std::snprintf(bf, sizeof bf, N48_C88_CT_FMT, M, U, U, U, M, M, U, U, U, U, U, U, U, U, U, M, U, U);
    expect_u("LINE clock88c (0.0.521)", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u); std::printf("      %d bytes\n", n);
}

static void glue_checks(const char *nb, const char *ahh, const char *peer)
{
    std::printf("== GLUE: read-only bodies, wired where they watch ==\n");
    const std::string N = slurp(nb), H = slurp(ahh), P = slurp(peer);
    if (N.empty() || H.empty() || P.empty()) { expect_u("GLUE: the three sources were read", 0u, 1u); return; }
    const std::string mm = body(N, "static __attribute__((noinline)) void c88_mm_note("), cp = body(N, "void navi48_c88_copy_note("),
                      tn = body(H, "static void gfxsrc_tex_note("), fe = body(H, "static __attribute__((noinline)) void gfxsrc_c88_frame_end(");
    expect_u("GLUE: the four hook bodies were found", !mm.empty() && !cp.empty() && !tn.empty() && !fe.empty(), 1u);
    expect_u("GLUE READ-ONLY: c88_mm_note holds no write primitive", writes_in(mm), 0u);
    expect_u("GLUE READ-ONLY: navi48_c88_copy_note holds no write primitive", writes_in(cp), 0u);
    expect_u("GLUE READ-ONLY: gfxsrc_tex_note holds no write primitive", writes_in(tn), 0u);
    expect_u("GLUE READ-ONLY: gfxsrc_c88_frame_end holds no write primitive", writes_in(fe), 0u);
    expect_u("GLUE READ-ONLY: the MM hook takes the written bytes const", count(N, "static __attribute__((noinline)) void c88_mm_note(uint64_t vramOffset, const uint32_t *src, uint32_t dwords)"), 1u);
    // REACHABILITY/ORDER: the MM hook sits in navi48_vram_write_mm BEFORE the lock (outside a copy's scope), the write loop after it
    const std::string wm = body(N, "bool navi48_vram_write_mm(uint64_t vramOffset, const uint32_t *src, uint32_t dwords) {");
    const size_t h1 = wm.find("if (!cgContained && __atomic_load_n(&gC88W.n, __ATOMIC_RELAXED)) c88_mm_note(vramOffset, src, dwords);");
    const size_t h2 = wm.find("IOLockLock(gVramMmLock);"), h3 = wm.find("amdgpu::WVRAM32_via_mm(");
    expect_u("GLUE ORDER: navi48_vram_write_mm: containment -> watch -> lock -> write", h1 != std::string::npos && h2 != std::string::npos &&
             h3 != std::string::npos && wm.find("const bool cgContained") < h1 && h1 < h2 && h2 < h3, 1u);
    const std::string rc = body(P, "static bool residency_copy_to_vram(void *self, void *dstMap, void *srcMap) {");
    const size_t c1 = rc.find("PEERLOG(\"residency-copy: COPIED #%llu"), c2 = rc.find("navi48_fc_copy_report(gCopy.copies);"),
                 c3 = rc.find("navi48_c88_copy_note(dVa, dVaOk, wBytes, firstDst, gCopy.copies);");
    expect_u("GLUE ORDER: the copier: COPIED -> via SDMA -> the watch (after every chunk landed)", c1 != std::string::npos &&
             c2 != std::string::npos && c3 != std::string::npos && c1 < c2 && c2 < c3, 1u);
    expect_u("GLUE: the observer is set on the descriptor path", count(H, "ex.tex_note = &gfxsrc_tex_note;"), 1u);
    expect_u("GLUE: the frame end runs right after the policy pass",
             count(H, "gfxsrc_policy(vm, &f, f.ib[0].got, ib0Va, dp, dkey, arm, boundCtx, wsBound);\n                gfxsrc_c88_frame_end(vm, &f, ctxSeq);"), 1u);   /* RE-BASELINED 0.0.546: + ctxSeq */
    // build 0.0.546: the watch carries the frame's context; hook_unmapVA kills it for every recorded context, after the scope check
    expect_u("GLUE (0.0.546): the watch is set with the frame's context", count(fe, "navi48_c88_watch_add(ctxSeq, gC88F.va[0], ext, vram, vok ? 1u : 0u, &idx);"), 1u);
    const std::string hu = body(H, "static uint64_t hook_unmapVA(void *self, uint64_t va, uint64_t size) {");
    const size_t u1 = hu.find("if (!e || e->state != 1)"), u2 = hu.find("    navi48_c88_unmap(e->seq, va, size);"),
                 u3 = hu.find("reinterpret_cast<Fn>(gOrigUnmapVA)(self, va, size)", u1 == std::string::npos ? 0 : u1 + 60u);
    expect_u("GLUE ORDER (0.0.546): hook_unmapVA: scope check -> the watch dies -> Apple's unmap", u1 != std::string::npos &&
             u2 != std::string::npos && u3 != std::string::npos && u1 < u2 && u2 < u3 && count(H, "navi48_c88_unmap(") == 1u, 1u);
    const std::string nu = body(N, "void navi48_c88_unmap(uint32_t ctx, uint64_t va, uint64_t size) {");
    expect_u("GLUE READ-ONLY (0.0.546): navi48_c88_unmap holds no write primitive, one load unset, capped lines",
             (!nu.empty() && writes_in(nu) == 0u && count(nu, "if (!__atomic_load_n(&gC88W.n, __ATOMIC_ACQUIRE)) return;") == 1u &&
              count(nu, "const uint32_t m = n48_c88_unmap(&gC88W, ctx, va, size);") == 1u && count(nu, "N48LOG(N48_C88_DEAD_FMT") == 1u) ? 1u : 0u, 1u);
    // build 0.0.513: the gate comes BEFORE both the line cap's use and the watch's arming in the frame end
    const size_t g1 = fe.find("if (!n48_c88_take_frame(&gC88Lines, (gC88Ct.frame == gC88F.frame && gC88Lines < N48_C88_LINES) ?\n                                            gfxsrc_c88_producer_take(N48_C88_TGT_K_B1, N48_C88_TGT_B1) : 0u)) return;"),
                 g2 = fe.find("navi48_c88_watch_add("), g3 = fe.find("HWLOG(N48_C88_FMT");
    expect_u("GLUE ORDER (0.0.521): frame end: the CONTENT gate -> the watch -> the lines", g1 != std::string::npos &&
             g2 != std::string::npos && g3 != std::string::npos && g1 < g2 && g2 < g3, 1u);
    expect_u("GLUE (0.0.521): the shape gate is gone from the frame end", count(fe, "8736u") + count(fe, "f->ib[1].len : 0u)) return;"), 0u);
    // build 0.0.521 Part B: the content scan's three calls, in order, inside gfxsrc_decide_frame and before the policy pass
    const std::string df = body(H, "static uint32_t gfxsrc_decide_frame(const uint8_t *info, uint32_t shapeOk, uint32_t n, const WsFrame *wf) {");
    const size_t d0 = df.find("gfxsrc_c88_ct_begin();"), d1 = df.find("for (uint32_t k = 0; k < f.nib && f.reader_ok; k++) {"),
                 d2 = df.find("if (!f.npgm) gXdC.noPgm++;\n            gfxsrc_c88_ct_ib(dst, got, k);"),
                 d3 = df.find("gfxsrc_c88_ct_end(&f);"), d4 = df.find("gfxsrc_policy(vm, &f, f.ib[0].got, ib0Va, dp, dkey, arm, boundCtx, wsBound);");
    expect_u("GLUE ORDER (0.0.521): begin -> IB loop -> walk each fully read IB after its items -> count/log -> policy", d0 != std::string::npos &&
             d1 != std::string::npos && d2 != std::string::npos && d3 != std::string::npos && d4 != std::string::npos &&
             d0 < d1 && d1 < d2 && d2 < d3 && d3 < d4, 1u);
    expect_u("GLUE (0.0.521): each content call appears once in the frame", count(df, "gfxsrc_c88_ct_begin();") + count(df, "gfxsrc_c88_ct_ib(") + count(df, "gfxsrc_c88_ct_end("), 3u);
    // the producer answer is THIS frame's only: a scan left from another frame (a stale `producer`) answers 0
    expect_u("GLUE (0.0.521): gfxsrc_c88_producer requires the scan to be this judged frame's",
             count(H, "static uint32_t gfxsrc_c88_producer(void) { return (gC88Ct.frame == gXdC.judged + 1u && gC88Ct.producer) ? 1u : 0u; }"), 1u);
    const std::string ce = body(H, "static __attribute__((noinline)) void gfxsrc_c88_ct_end(const n48_xv_frame *f) {"),
                      ck = body(H, "static uint64_t gfxsrc_c88_key(void *ctx, uint64_t va) {");
    expect_u("GLUE READ-ONLY (0.0.521): the frame's end and the key lookup hold no write primitive", !ce.empty() && !ck.empty() && writes_in(ce) + writes_in(ck) == 0u, 1u);
    expect_u("GLUE (0.0.521): the key is the memo's read-only peek, never an identification", count(ck, "n48_sd_memo_peek_key(&gXdMemo, navi48_shadercache_epoch(), va, nullptr)") * 0x10u +
             count(ck, "n48_sd_memo_get") + count(ck, "gfxsrc_identify") + count(ck, "gfxc_read"), 0x10u);
    expect_u("GLUE (0.0.521): the frame's end counts a producer and logs clock88c under its cap", count(ce, "if (gC88Ct.frame != gXdC.judged + 1u || !gC88Ct.producer) return;") +
             count(ce, "n48_c88_prod_note(&gC88Sh, 1u, gC88Ct.frame, armed);") + count(ce, "if (gC88CLines >= N48_C88_CLINES) return;") + count(ce, "HWLOG(N48_C88_CT_FMT"), 4u);
    const size_t e1 = ce.find("if (gC88CLines >= N48_C88_CLINES) return;"), e2 = ce.find("n48_c88_tgt_take(&gC88Tgt, gC88Ct.sCb0, N48_C88_TGT_K_CT, N48_C88_TGT_CT)"),
                 e3 = ce.find("gC88CLines++;"), e4 = ce.find("HWLOG(N48_C88_CT_FMT");
    expect_u("GLUE ORDER (0.0.521): clock88c: total cap -> the target's budget -> count -> line", e1 != std::string::npos && e2 != std::string::npos &&
             e3 != std::string::npos && e4 != std::string::npos && e1 < e2 && e2 < e3 && e3 < e4, 1u);
    expect_u("GLUE (0.0.521): the IB walk hands the memo peek as the key", count(H, "n48_c88_ct_walk(&gC88Ct, in, n, k, &gfxsrc_c88_key, nullptr);"), 1u);
    // Part C: the mibseg-detail line asks n48_c88_detail_take with this frame's content answer
    expect_u("GLUE (0.0.521 Part C): the mibseg-detail budget is n48_c88_detail_take with the producer flag",
             count(H, "if (n48_c88_detail_take(&gXdMibsegDetailLines, kXdMibsegDetailLines, &gXdMibsegDetailSLines,\n                                gfxsrc_c88_producer_take(N48_C88_TGT_K_DETAIL, N48_C88_TGT_DETAIL))) {"), 1u);
    expect_u("GLUE (0.0.521 Part C): the old inline cap test is gone", count(H, "if (gXdMibsegDetailLines < kXdMibsegDetailLines) {"), 0u);
    expect_u("GLUE (0.0.521): the content report line is printed with the shape one",
             count(body(H, "static void c88_report_line() {"), "HWLOG(N48_C88_CREPORT_FMT"), 1u);
    expect_u("GLUE (0.0.513): the frame end no longer counts lines by itself (only n48_c88_take_frame does)", count(fe, "gC88Lines++"), 0u);
    expect_u("GLUE: the report line is printed by the drawelide66 verb", count(body(H, "static void drawelide_report_line(const char *how)"), "c88_report_line();"), 1u);
}

int main(int argc, char **argv)
{
    std::printf("gfx_clock88 (0.0.512 Part B, 0.0.521 content): the clock's-source instrumentation, print only\n");
    b1_checks(); b2_checks(); b2u_checks(); gate_checks(); b3_checks(); detail_checks(); boot_checks(); line_checks();
    {   // build 0.0.521: the adopted rows sit at src/xlat12 beside src/navi48-bringup (derived from argv[1], Navi48Bringup.cpp)
        std::string j = argc >= 2 ? std::string(argv[1]) : std::string();
        const size_t at = j.rfind("navi48-bringup/src/Navi48Bringup.cpp");
        j = at == std::string::npos ? std::string("src/xlat12/xlat12_adopt_rows.json") : j.substr(0, at) + "xlat12/xlat12_adopt_rows.json";
        content_checks(j.c_str());
    }
    if (argc >= 4) glue_checks(argv[1], argv[2], argv[3]);
    else expect_u("GLUE: pass Navi48Bringup.cpp AppleHardwareHook.cpp Navi48AccelPeer.cpp", 0u, 1u);
    std::printf("gfx_clock88: %d run, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
