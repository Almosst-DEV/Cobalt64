// gfx_desc_port_test.cpp - M4-DESC-KEXT-PORT (notes/M4-DESC-KEXT-PORT.md): the kext's descriptor path, offline.
//
//   A  n48_dp_read (gfx_desc_port.h): the snapshot read adds only refusals to the reader it is handed - a short read is a FAILED
//      read, and it refuses a length, alignment or range the table step never asks for, before calling the reader at all.
//   B  n48_dl, the producer ledger: fed ONLY by a committed frame (gate N48_CM_OK, commit_ok 1, verdict TRANSLATE), keyed by VM
//      context, emptied per context at unmapVA, cleared by an epoch or arm change, CB1-7 clears it, overflow is never proof.
//   E  0.0.380 - LEDGER CORRECTNESS, both halves and both switches: the RANGE-ACCURATE unmap (n48_dl_unmap_rng)
//      and the BASE-PHYSICAL-PAGE key (n48_dl_tiled_ok_pg). Every check is run at the switch's default as well as on, because
//      the whole point is that the default is TODAY'S behaviour and the difference is the measurement; E13 re-proves that the
//      drop's predicate is ws_resprov.h n48_rp_clr_clean's and not a second one, over a grid.
//   K  0.0.394 - KEEP ACROSS A COVERING UNMAP: arm19b's own withdraw-and-re-map sequence (fire #68's
//      `va 0x400800000 size 0x870000`, fire #162's re-map of the same VA). With `n48_dl.keep` on, a covering unmap keeps the
//      entry and marks it, and the key decides at the ask: same page -> proven, different page -> keyMoved. Non-vacuous both
//      ways (K1's off path is today's drop; the planted defect reads `keep` and ignores it).
//   C  THE KEXT'S WIRING, read out of AppleHardwareHook.cpp itself (argv[1]), comments stripped: desc_read is gfxc_read over the
//      frame's own vm and nothing else, reached only through n48_dp_read; the three flags (INLINE | TABLE | DESC_INV) and both
//      callbacks exist ONLY inside the policy's `if (dp)` block; the switch defaults to 0 and only the verb sets it; the ledger is
//      fed once, after the COMMIT gate, from commit_ok; unmapVA drops the context. The guards themselves are gfxc_read's, which is
//      kernel code and cannot run here: C pins that the path GOES THROUGH them, it cannot re-prove them.
//
// PLANTED DEFECTS (a test no mutation can break is not testing anything): every group is re-run against mutants - copies of the
// functions with one defect in B's case, one-line edits of the loaded kext source in C's - and each must be CAUGHT.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_desc_port_test.cpp -o /tmp/dptest && \
//         /tmp/dptest src/navi48-bringup/src/apple/AppleHardwareHook.cpp
#include <cstdio>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>
#include "gfx_desc_port.h"
#include "ws_resprov.h"   // 0.0.380: E13 re-proves that the exact drop reuses THIS file's predicate
#include "gfx_capture_scan.h"   // build 0.0.485 (group L485): the per-draw pass the kext feeds the ledger's page map from

static int gFail = 0, gRun = 0, gQuiet = 0;
static void expect(const char *what, bool ok)
{
    gRun++;
    if (!ok) { gFail++; if (!gQuiet) std::printf("FAIL  %s\n", what); }
    else if (!gQuiet) std::printf("ok    %s\n", what);
}

// ---------------------------------------------------------------------------------------------------------------------------
// A. the snapshot read
// ---------------------------------------------------------------------------------------------------------------------------
struct MockVm { uint32_t mem[64]; uint64_t base; uint32_t mapped; uint32_t calls; const void *seen; };
// A stand-in for gfxc_read's contract: dwords are read from `base` on, and the read STOPS at the first unmapped dword (as gfxc_read
// stops at the first page that does not resolve or fails its RAM-range guard) - it returns how many it read.
static uint32_t mock_reader(const void *vm, uint64_t va, uint32_t *dst, uint32_t n)
{
    MockVm *m = const_cast<MockVm *>(static_cast<const MockVm *>(vm));
    m->calls++; m->seen = vm;
    uint32_t got = 0;
    while (got < n) {
        const uint64_t a = va + 4ull * got;
        if (a < m->base || a >= m->base + 4ull * m->mapped) break;
        dst[got] = m->mem[(a - m->base) / 4u]; got++;
    }
    return got;
}
typedef int (*DpFn)(n48_dp_reader, const void *, uint64_t, uint32_t, uint32_t *);
// mutants of n48_dp_read
static int dp_mut_short_ok(n48_dp_reader rd, const void *vm, uint64_t va, uint32_t ndw, uint32_t *out)
{   // a short read counts: the reader's stop at an unresolvable page no longer refuses
    for (uint32_t k = 0; k < ndw && k < N48_DP_READ_MAX; k++) out[k] = 0u;
    if (!rd || !vm || !ndw || ndw > N48_DP_READ_MAX) return 0;
    return rd(vm, va, out, ndw) > 0u;
}
static int dp_mut_no_range(n48_dp_reader rd, const void *vm, uint64_t va, uint32_t ndw, uint32_t *out)
{   // no length / alignment / range refusal before the reader
    for (uint32_t k = 0; k < ndw && k < N48_DP_READ_MAX; k++) out[k] = 0u;
    if (!rd || !vm) return 0;
    return rd(vm, va, out, ndw) == ndw;
}
static int dp_real(n48_dp_reader rd, const void *vm, uint64_t va, uint32_t ndw, uint32_t *out) { return n48_dp_read(rd, vm, va, ndw, out); }

static void checks_read(DpFn fn)
{
    MockVm m; std::memset(&m, 0, sizeof m); m.base = 0x4000b0000ull; m.mapped = 16u;
    for (uint32_t k = 0; k < 64u; k++) m.mem[k] = 0xA0000000u + k;
    uint32_t out[16];
    std::memset(out, 0x55, sizeof out);
    int ok = fn(&mock_reader, &m, 0x4000b0020ull, 8u, out);
    expect("A1 a whole T# (8 dw) inside the mapping reads, the bytes are the reader's", ok == 1 && out[0] == 0xA0000008u && out[7] == 0xA000000Fu);
    expect("A2 the reader is handed the caller's vm object itself (the frame's own root)", m.seen == &m);
    std::memset(out, 0x55, sizeof out);
    ok = fn(&mock_reader, &m, 0x4000b0030ull, 8u, out);   // dwords 12..19, 16.. unmapped: the reader stops at 4
    expect("A3 a record crossing into an unmapped page is SHORT and REFUSED", ok == 0);
    expect("A3 and the refused output is zero, never a half record", out[0] == 0u && out[3] == 0u);
    ok = fn(&mock_reader, &m, 0x4000c0000ull, 2u, out);
    expect("A4 a record the reader cannot reach at all is refused", ok == 0);
    m.calls = 0;
    expect("A5 zero dwords: refused", fn(&mock_reader, &m, 0x4000b0000ull, 0u, out) == 0);
    expect("A5 nine dwords (more than any record): refused", fn(&mock_reader, &m, 0x4000b0000ull, 9u, out) == 0);
    expect("A6 a VA that is not dword-aligned: refused", fn(&mock_reader, &m, 0x4000b0002ull, 2u, out) == 0);
    expect("A7 a VA at or above 2^48: refused", fn(&mock_reader, &m, 1ull << 48, 2u, out) == 0);
    expect("A7 a read that would cross 2^48: refused", fn(&mock_reader, &m, (1ull << 48) - 4u, 2u, out) == 0);
    expect("A8 all of A5-A7 refused BEFORE the reader was called", m.calls == 0u);
    expect("A9 no vm: refused", fn(&mock_reader, nullptr, 0x4000b0000ull, 2u, out) == 0);
    expect("A9 no reader: refused", fn(nullptr, &m, 0x4000b0000ull, 2u, out) == 0);
}

// ---------------------------------------------------------------------------------------------------------------------------
// B. the ledger
// ---------------------------------------------------------------------------------------------------------------------------
struct LedgerOps {
    uint32_t (*feed)(n48_dl *, const n48_dl_frame *);
    int (*tiled_ok)(n48_dl *, uint64_t, uint64_t, uint32_t);
    void (*unmap)(n48_dl *, uint64_t);
};
// mutants
static uint32_t feed_no_verdict(n48_dl *l, const n48_dl_frame *f)
{   // THE BRIEF'S PLANTED DEFECT: the ledger accepts a frame whatever its verdict
    if (!f || f->committed != 1u || f->gate != N48_CM_OK || !f->out || !f->seg || !f->ctx) return 0u;
    uint32_t a = 0;
    for (uint32_t k = 0; k < f->nseg; k++) if (!f->seg[k].status) a += n48_dl_from_output(l, f->ctx, &f->out[f->seg[k].start], f->seg[k].end - f->seg[k].start, &f->pg);
    return a;
}
static uint32_t feed_at_translate(n48_dl *l, const n48_dl_frame *f)
{   // the pre-review rule: fed at a TRANSLATE verdict, whatever the COMMIT gate answered
    if (!f || f->verdict != N48_XV_TRANSLATE || !f->out || !f->seg || !f->ctx) return 0u;
    uint32_t a = 0;
    for (uint32_t k = 0; k < f->nseg; k++) if (!f->seg[k].status) a += n48_dl_from_output(l, f->ctx, &f->out[f->seg[k].start], f->seg[k].end - f->seg[k].start, &f->pg);
    return a;
}
static uint32_t feed_gate_only(n48_dl *l, const n48_dl_frame *f)
{   // trusts `gate` alone - so a zero-initialised frame (gate 0 == N48_CM_OK) is fed
    if (!f || f->gate != N48_CM_OK || f->verdict != N48_XV_TRANSLATE || !f->out || !f->seg || !f->ctx) return 0u;
    uint32_t a = 0;
    for (uint32_t k = 0; k < f->nseg; k++) if (!f->seg[k].status) a += n48_dl_from_output(l, f->ctx, &f->out[f->seg[k].start], f->seg[k].end - f->seg[k].start, &f->pg);
    return a;
}
static int tiled_any_ctx(n48_dl *l, uint64_t ctx, uint64_t va, uint32_t mode)
{   // keyed by VA alone
    (void)ctx;
    for (uint32_t k = 0; k < l->n; k++) if (l->e[k].va == va && l->e[k].mode == mode) return 1;
    return 0;
}
static int tiled_any_mode(n48_dl *l, uint64_t ctx, uint64_t va, uint32_t mode)
{   // a producer in ANY mode proves the layout
    (void)mode;
    for (uint32_t k = 0; k < l->n; k++) if (l->e[k].ctx == ctx && l->e[k].va == va) return 1;
    return 0;
}
static void unmap_noop(n48_dl *l, uint64_t ctx) { (void)l; (void)ctx; }
static uint32_t feed_real(n48_dl *l, const n48_dl_frame *f) { return n48_dl_feed(l, f); }
static int tiled_real(n48_dl *l, uint64_t c, uint64_t v, uint32_t m) { return n48_dl_tiled_ok(l, c, v, m); }
static void unmap_real(n48_dl *l, uint64_t c) { n48_dl_unmap(l, c); }

// A translated segment that writes CB0 at `va` (gfx12 addresses, xlat12's output shape) in gfx12 mode `mode`, then draws.
static uint32_t seg_cb0(uint32_t *o, uint64_t va, uint32_t mode, int cb1)
{
    uint32_t k = 0;
    o[k++] = 0xFFFF1000u;                                                                 // a one-dword NOP
    o[k++] = 0xC0016900u; o[k++] = (0x28c60u - 0x28000u) >> 2; o[k++] = (uint32_t)(va >> 8);          // CB_COLOR0_BASE
    o[k++] = 0xC0016900u; o[k++] = (0x28e40u - 0x28000u) >> 2; o[k++] = (uint32_t)(va >> 40) & 0xFFu; // CB_COLOR0_BASE_EXT
    o[k++] = 0xC0016900u; o[k++] = (0x28c7cu - 0x28000u) >> 2; o[k++] = mode << 15;                   // CB_COLOR0_ATTRIB3
    o[k++] = 0xC0016900u; o[k++] = (0x28850u - 0x28000u) >> 2; o[k++] = 0xFu;                         // CB_TARGET_MASK
    if (cb1) { o[k++] = 0xC0016900u; o[k++] = (0x28c84u - 0x28000u) >> 2; o[k++] = 0x1234u; }          // CB_COLOR1_BASE
    o[k++] = 0xC0012D00u; o[k++] = 3u; o[k++] = 2u;                                                    // DRAW_INDEX_AUTO
    return k;
}
static n48_dl_frame frame_ok(const uint32_t *o, uint32_t n, n48_cm_seg *seg, uint64_t ctx)
{
    seg[0].head = 0; seg[0].start = 0; seg[0].end = n; seg[0].status = 0; seg[0].out_len = n;
    n48_dl_frame f; std::memset(&f, 0, sizeof f);
    f.committed = 1u; f.gate = N48_CM_OK; f.verdict = N48_XV_TRANSLATE; f.nseg = 1u; f.n = n; f.ctx = ctx; f.seg = seg; f.out = o;
    return f;
}
static n48_dl gL;
static void checks_ledger(const LedgerOps &op)
{
    static uint32_t o[64];
    n48_cm_seg seg[2];
    const uint64_t VA = 0x400800000ull;
    std::memset(&gL, 0, sizeof gL);
    n48_dl_sync(&gL, 2u, 7u);
    uint32_t n = seg_cb0(o, VA, 3u, 0);
    n48_dl_frame f = frame_ok(o, n, seg, 5u);
    expect("B1 a COMMITTED frame (commit_ok 1, gate OK, TRANSLATE) writing CB0 in mode 3 adds its target", op.feed(&gL, &f) == 1u);
    expect("B1 and the ledger proves (ctx 5, VA, mode 3)", op.tiled_ok(&gL, 5u, VA, 3u) == 1);
    expect("B2 the same VA in ANOTHER context is not proven", op.tiled_ok(&gL, 6u, VA, 3u) == 0);
    expect("B3 the same VA in another MODE is not proven", op.tiled_ok(&gL, 5u, VA, 2u) == 0);
    expect("B3 another VA is not proven", op.tiled_ok(&gL, 5u, VA + 0x1000u, 3u) == 0);
    expect("B3 context 0 (unknown) is never proven", op.tiled_ok(&gL, 0u, VA, 3u) == 0);
    // refusals: nothing may be added
    std::memset(&gL, 0, sizeof gL);
    n48_dl_frame g = f; g.verdict = N48_XV_TARGET_VRAM;
    expect("B4 a frame whose verdict is NOT TRANSLATE is refused (the brief's planted defect)", op.feed(&gL, &g) == 0u && gL.n == 0u);
    g = f; g.committed = 0u;
    expect("B5 a TRANSLATE frame the COMMIT path did not commit (DECIDE, or gate refused) is refused", op.feed(&gL, &g) == 0u && gL.n == 0u);
    g = f; g.gate = N48_CM_LEN;
    expect("B5 a frame whose gate answered anything but OK is refused", op.feed(&gL, &g) == 0u && gL.n == 0u);
    { n48_dl_frame z; std::memset(&z, 0, sizeof z); z.seg = seg; z.out = o; z.n = n; z.nseg = 1u; z.ctx = 5u;
      expect("B6 a ZERO-INITIALISED frame (gate 0 == N48_CM_OK) is refused", op.feed(&gL, &z) == 0u && gL.n == 0u); }
    g = f; g.ctx = 0u;
    expect("B7 a frame without a context key is refused", op.feed(&gL, &g) == 0u && gL.n == 0u);
    seg[0].status = 7u;
    expect("B8 a REFUSED segment's output is not read", op.feed(&gL, &f) == 0u && gL.n == 0u);
    seg[0].status = 0u;
    // unmap, per context
    std::memset(&gL, 0, sizeof gL);
    n48_dl_frame f9 = f; f9.ctx = 9u;
    op.feed(&gL, &f); op.feed(&gL, &f9);
    expect("B9 two contexts each hold the same VA", op.tiled_ok(&gL, 5u, VA, 3u) == 1 && op.tiled_ok(&gL, 9u, VA, 3u) == 1);
    op.unmap(&gL, 5u);
    expect("B9 unmapVA of context 5 drops ITS entry", op.tiled_ok(&gL, 5u, VA, 3u) == 0);
    expect("B9 and keeps context 9's", op.tiled_ok(&gL, 9u, VA, 3u) == 1);
    op.unmap(&gL, 0u);
    expect("B9 an unmap of an unknown context drops everything", op.tiled_ok(&gL, 9u, VA, 3u) == 0);
    // epoch / arm
    std::memset(&gL, 0, sizeof gL); n48_dl_sync(&gL, 2u, 7u); op.feed(&gL, &f);
    n48_dl_sync(&gL, 2u, 7u);
    expect("B10 the same arm and epoch keep the entry", op.tiled_ok(&gL, 5u, VA, 3u) == 1);
    n48_dl_sync(&gL, 2u, 8u);
    expect("B10 an untranslated pass (epoch moved) clears it", op.tiled_ok(&gL, 5u, VA, 3u) == 0);
    op.feed(&gL, &f); n48_dl_sync(&gL, 1u, 8u);
    expect("B10 an arm change clears it", op.tiled_ok(&gL, 5u, VA, 3u) == 0);
    // CB1-7
    std::memset(&gL, 0, sizeof gL); op.feed(&gL, &f);
    uint32_t n2 = seg_cb0(o, VA + 0x100000u, 3u, 1);
    n48_dl_frame h = frame_ok(o, n2, seg, 5u);
    op.feed(&gL, &h);
    expect("B11 a translated CB1 write clears the ledger (only CB0 is modelled)", op.tiled_ok(&gL, 5u, VA, 3u) == 0);
    // overflow
    std::memset(&gL, 0, sizeof gL);
    for (uint32_t i = 0; i < N48_DL_MAX + 1u; i++) { n = seg_cb0(o, VA + 0x10000ull * i, 3u, 0); n48_dl_frame q = frame_ok(o, n, seg, 5u); op.feed(&gL, &q); }
    expect("B12 the 64th target is proven", op.tiled_ok(&gL, 5u, VA + 0x10000ull * (N48_DL_MAX - 1u), 3u) == 1);
    expect("B12 the 65th, which did not fit, is NOT proven", op.tiled_ok(&gL, 5u, VA + 0x10000ull * N48_DL_MAX, 3u) == 0);
}

// ---------------------------------------------------------------------------------------------------------------------------
// D-FL. build 0.0.448 item 3 (notes/design/MIB-A1-PROVENANCE.md Q4 step 2, switch 45) — FRAME-LOCAL PROVENANCE.
//
// The kext's own wiring (AppleHardwareHook.cpp: gfxsrc_policy, gfxsrc_desc_tiled_ok) is a second n48_dl instance, cleared
// unconditionally at the top of every pass and fed PER SEGMENT, IN SUBMISSION ORDER, right after that segment's own
// successful translate - never waiting for the frame's commit gate. This drives the EXACT SAME primitives
// (n48_dl_clear / n48_dl_from_output / n48_dl_tiled_ok) in that SAME order, so it proves the mechanism the kext's per-segment
// loop applies, not merely the ledger functions section B already covers in isolation. Source-order pins below (group D-FLW)
// confirm the kext's OWN text actually calls them in this sequence.
// ---------------------------------------------------------------------------------------------------------------------------
static void checks_frame_local()
{
    static uint32_t oA[64], oB[64], oBad[64];
    n48_dl fl; std::memset(&fl, 0, sizeof fl);
    const uint64_t CTX = 5u, VA_A = 0x400900000ull, VA_B = 0x400a00000ull;

    /* "new pass starts empty": a fresh (or freshly re-cleared) list vouches for nothing at all. */
    expect("FL1 a fresh frame-local list proves nothing", n48_dl_tiled_ok(&fl, CTX, VA_A, 3u) == 0);

    /* PLANTED BREAK ("consumer before producer"): ask for the producer's OWN surface BEFORE its segment is fed - this is
     * what a consumer EARLIER in submission order than its producer would see. Must be 0: the real wiring feeds AFTER a
     * segment's translate returns, so nothing later in the SAME segment - let alone an earlier one - can see it yet. */
    expect("FL2 BREAK-check: asking before the producer segment is fed finds nothing (consumer-before-producer)",
           n48_dl_tiled_ok(&fl, CTX, VA_A, 3u) == 0);

    /* Segment A (the producer) translates successfully and writes CB0 at VA_A in mode 3; fed right after, exactly as the
     * kext's `if (dp && gXdFrameLocalOn && st == 0u)` gate does. */
    uint32_t nA = seg_cb0(oA, VA_A, 3u, 0);
    expect("FL3 segment A's own successful output feeds the list", n48_dl_from_output(&fl, CTX, oA, nA, nullptr) == 1u);

    /* Segment B (a LATER consumer, submitted after A): now proven, at the SAME va/mode/ctx. */
    expect("FL4 a later segment's ask now sees the earlier producer", n48_dl_tiled_ok(&fl, CTX, VA_A, 3u) == 1);

    /* PLANTED BREAK ("mode mismatch"): the SAME va, a DIFFERENT gfx12 mode, is not proven. */
    expect("FL5 BREAK-check: the same VA in a DIFFERENT mode is not proven (mode mismatch)",
           n48_dl_tiled_ok(&fl, CTX, VA_A, 2u) == 0);
    /* ... and a different context, matching the real ledger's own (ctx, va, mode) key. */
    expect("FL5 the same VA in a DIFFERENT context is not proven", n48_dl_tiled_ok(&fl, CTX + 1u, VA_A, 3u) == 0);

    /* PLANTED BREAK ("a refused producer segment"): a segment whose translate REFUSED (status != 0) is never handed to
     * n48_dl_from_output at all - the kext's own `st == 0u` gate (`if (dp && gXdFrameLocalOn && st == 0u) { ... }`).
     * build 0.0.449 item 4 (F6): made NON-VACUOUS by mirroring that EXACT condition here, with `st` set BOTH
     * ways over the SAME segment data, rather than simply never calling the feed at all (0.0.448's own shape,
     * which trivially "proves" nothing because nothing ever runs either way). */
    uint32_t nBad = seg_cb0(oBad, VA_B, 3u, 0);
    { volatile uint32_t dp = 1u, gXdFrameLocalOn = 1u;
      volatile uint32_t stRefused = 7u;   // any non-zero status - the segment's own translate refused
      if (dp && gXdFrameLocalOn && stRefused == 0u) n48_dl_from_output(&fl, CTX, oBad, nBad, nullptr);
      expect("FL6 the kext's OWN feed condition (`st == 0u`), mirrored: a refused segment (st != 0) is not fed",
             n48_dl_tiled_ok(&fl, CTX, VA_B, 3u) == 0);
      volatile uint32_t stOk = 0u;   // the SAME segment data, this time as if it had translated successfully
      if (dp && gXdFrameLocalOn && stOk == 0u) n48_dl_from_output(&fl, CTX, oBad, nBad, nullptr);
      expect("FL6 BREAK-check: the SAME condition with st == 0u DOES feed it - the check above was not vacuous",
             n48_dl_tiled_ok(&fl, CTX, VA_B, 3u) == 1); }

    /* PLANTED BREAK ("a nonzero CB1-CB7 base in the producer"): a LATER segment C writes a nonzero CB1 base. This is
     * n48_dl_from_output's OWN existing rule (reused, not reimplemented), so it clears the WHOLE frame-local list -
     * including segment A's entry, fed earlier in this SAME pass. */
    uint32_t nC = seg_cb0(oB, VA_B, 3u, 1 /* cb1 nonzero */);
    n48_dl_from_output(&fl, CTX, oB, nC, nullptr);
    expect("FL7 BREAK-check: a nonzero CB1 base anywhere in the pass clears the WHOLE frame-local list",
           n48_dl_tiled_ok(&fl, CTX, VA_A, 3u) == 0);

    /* "new pass starts empty": the next frame's pass-start clear (n48_dl_clear, exactly what gfxsrc_policy calls
     * unconditionally before its segment loop) empties whatever THIS pass fed, however it fed it. */
    std::memset(&fl, 0, sizeof fl);
    uint32_t nA2 = seg_cb0(oA, VA_A, 3u, 0);
    n48_dl_from_output(&fl, CTX, oA, nA2, nullptr);
    expect("FL8 a surface fed in one pass is proven within it", n48_dl_tiled_ok(&fl, CTX, VA_A, 3u) == 1);
    n48_dl_clear(&fl);
    expect("FL9 BREAK-check: the NEXT pass's clear empties it - nothing crosses a frame", n48_dl_tiled_ok(&fl, CTX, VA_A, 3u) == 0);
}

// ---------------------------------------------------------------------------------------------------------------------------
// D-FLW. THE KEXT'S OWN WIRING for switch 45, read out of AppleHardwareHook.cpp itself (argv[1], comments stripped exactly
// as group C does for the descriptor path) - a source-order pin, never a line number, proving the block above is actually
// REACHED in the real ordering: cleared once before the segment loop, fed inside it gated on a successful translate, and
// consulted in gfxsrc_desc_tiled_ok strictly after the ledger and resprov.
// ---------------------------------------------------------------------------------------------------------------------------
static void checks_frame_local_wiring(const std::string &raw)
{
    if (raw.empty()) { expect("FLW0 SKIPPED: no source given", true); return; }
    const size_t pPolicy = raw.find("gfxsrc_policy(const GfxcVm");
    const size_t pClear = raw.find("n48_dl_clear(&gXdFrameLocal)");
    const size_t pLoop = raw.find("for (uint32_t k = 0; k < ns && k < N48_XV_MAX_SEGS; k++)");
    // build 0.0.480 (switch 55, C2): the per-segment feed now covers `[flFrom, olen)` - 0 for a segment, a unit's LAST
    // constituent for a unit (its earlier constituents are fed at each head by gfxsrc_unit_cons_feed, below).
    const size_t pFeed = raw.find("n48_dl_from_output(&gXdFrameLocal, dctxKey, out + flFrom, olen - flFrom, nullptr)");
    const size_t pTiledFn = raw.find("static int gfxsrc_desc_tiled_ok(void *ctx");
    const size_t pLedAsk = raw.find("n48_dl_tiled_ok_pg(&gXdLed,");
    const size_t pRpAsk = raw.find("n48_rp_ok(&gXdRp,");
    const size_t pFlAsk = raw.find("n48_dl_tiled_ok(&gXdFrameLocal,");
    expect("FLW1 gXdFrameLocal is cleared inside gfxsrc_policy, BEFORE the segment loop",
           pPolicy != std::string::npos && pClear != std::string::npos && pLoop != std::string::npos &&
           pPolicy < pClear && pClear < pLoop);
    expect("FLW2 the frame-local feed is INSIDE the segment loop (after it starts)",
           pFeed != std::string::npos && pLoop < pFeed);
    expect("FLW3 the frame-local feed is gated on a successful translate (`st == 0u`), same line-area",
           raw.find("if (dp && gXdFrameLocalOn && st == 0u) {") != std::string::npos);
    expect("FLW4 gfxsrc_desc_tiled_ok consults the ledger, then resprov, then the frame-local list, IN THAT ORDER",
           pTiledFn != std::string::npos && pLedAsk != std::string::npos && pRpAsk != std::string::npos &&
           pFlAsk != std::string::npos && pTiledFn < pLedAsk && pLedAsk < pRpAsk && pRpAsk < pFlAsk);
    /* PLANTED BREAK ("remove the ordering"): a REAL mutation - physically move the frame-local ask's own
     * statement to BEFORE gfxsrc_desc_tiled_ok's opening brace in a COPY of the source text, then re-run FLW4's
     * OWN check (as a closure, not a re-typed condition) against that mutated text. build 0.0.449 item 4
     * (F6): 0.0.448's own version built `mutSrc` and never mutated it, so it silently re-checked the REAL text
     * twice under two different names - this actually edits the string. */
    { const size_t flStmt = raw.find("if (gXdFrameLocalOn && n48_dl_tiled_ok(&gXdFrameLocal, c->ctx, va, mode)) return 1;");
      expect("FLW5 setup: the frame-local ask's own statement is found verbatim (so the mutation below moves a REAL line)",
             flStmt != std::string::npos && pTiledFn != std::string::npos && pTiledFn < flStmt);
      if (flStmt != std::string::npos && pTiledFn != std::string::npos && pTiledFn < flStmt) {
          const size_t stmtEnd = raw.find(';', flStmt) + 1u;
          std::string mut = raw;
          const std::string moved = mut.substr(flStmt, stmtEnd - flStmt);
          mut.erase(flStmt, stmtEnd - flStmt);
          mut.insert(pTiledFn, moved + " ");   // now sits BEFORE the ledger ask, ahead of the function's own opening
          const auto check_order = [](const std::string &s) {
              const size_t tf = s.find("static int gfxsrc_desc_tiled_ok(void *ctx");
              const size_t la = s.find("n48_dl_tiled_ok_pg(&gXdLed,");
              const size_t rp = s.find("n48_rp_ok(&gXdRp,");
              const size_t fa = s.find("n48_dl_tiled_ok(&gXdFrameLocal,");
              return tf != std::string::npos && la != std::string::npos && rp != std::string::npos &&
                     fa != std::string::npos && tf < la && la < rp && rp < fa;
          };
          expect("FLW5 the SAME check (FLW4's own closure) passes on the real, unmutated source", check_order(raw));
          expect("FLW5 BREAK-check: the SAME check FAILS once the frame-local ask is physically moved earlier - the pin is real, not vacuous",
                 !check_order(mut));
      } }

    /* build 0.0.449 item 4 (F6): "a test of switch 44's kext wiring (the flag set only when the switch is
     * ON, with a carry)". A source-order pin: ex.flags |= XLAT12_EXTRA_UD_REEMIT and ex.ud_carry = &gPolicyUdCarry
     * are the SAME statement pair, both inside `if (gXdUdReemitOn)` - so the flag can never be set without the
     * carry pointer being handed over in the SAME breath (PLANTED BREAK (c), 4b: "UD_REEMIT set without
     * gXdUdReemitOn" would mean the flag line sits OUTSIDE this guard - checked by requiring them on ONE line). */
    expect("FLW6 switch 44: the flag and the carry pointer are set TOGETHER, inside `if (gXdUdReemitOn)`",
           raw.find("if (gXdUdReemitOn) { ex.flags |= XLAT12_EXTRA_UD_REEMIT; ex.ud_carry = &gPolicyUdCarry;") != std::string::npos);
    { std::string mut = raw;
      const size_t p = mut.find("if (gXdUdReemitOn) { ex.flags |= XLAT12_EXTRA_UD_REEMIT; ex.ud_carry = &gPolicyUdCarry;");
      if (p != std::string::npos) mut.replace(p, 18, "if (1)            "); // "if (gXdUdReemitOn)" (18 chars) -> unconditional
      expect("FLW6 BREAK-check: an unconditional flag+carry set (switch ignored) is caught",
             p != std::string::npos &&
             mut.find("if (gXdUdReemitOn) { ex.flags |= XLAT12_EXTRA_UD_REEMIT; ex.ud_carry = &gPolicyUdCarry;") == std::string::npos); }

    /* build 0.0.449 item 4b planted break (b): "the feed moved to just after the translate call, before
     * navi48_cg_seg_check". The copy-guard's own check (`cgRefused`/`navi48_cg_seg_check`) must appear BEFORE the
     * frame-local feed in gfxsrc_policy's text, so a copy-guard-refused segment's `st` is already non-zero by
     * the time the feed's `st == 0u` gate is reached. */
    { const size_t pCg = raw.find("bool cgRefused = false;");
      const size_t pFeedStmt = raw.find("if (dp && gXdFrameLocalOn && st == 0u) {");
      expect("FLW7 the copy-guard check (cgRefused) precedes the frame-local feed in gfxsrc_policy's own text",
             pCg != std::string::npos && pFeedStmt != std::string::npos && pCg < pFeedStmt); }

    /* build 0.0.450 item 0 (P10) — THE FRAME-LOCAL FEED EXISTS EXACTLY ONCE IN THE FILE, and only under
     * `st == 0u`. The 0.0.448/449 tests above (FLW2/FLW3) already prove ONE occurrence is inside the loop and
     * gated correctly; this proves there is no SECOND, ungated feed anywhere else - specifically the shape the
     * brief names: a second `n48_dl_from_output(&gXdFrameLocal, ...)` inside the `if (st && build)` restore
     * branch (`if (st && build) memcpy(&gXdNew[from], &gXdIb[from], ...)` - "a refused segment leaves no
     * half-rewrite"), which would feed a REFUSED segment's (stale Apple) bytes into the frame-local ledger. */
    // count() and has() are defined later in this file (they read gXdLed's ledger-line tests, below); this group
    // runs before them, so it counts inline rather than forward-declaring across an unrelated section.
    auto occurrences = [](const std::string &s, const char *t) -> size_t {
        size_t n = 0, p = 0, tl = std::strlen(t);
        while ((p = s.find(t, p)) != std::string::npos) { n++; p += tl; }
        return n;
    };
    // build 0.0.480 (switch 55, C2, design Q4): EXACTLY TWO - the per-segment feed above, and the per-constituent
    // feed gfxsrc_unit_cons_feed, which the translator calls at each later constituent head of a unit with the previous
    // constituent's finished slice. That second one feeds BEFORE the unit's own final status is known; a unit that then
    // refuses leaves its frame uncommittable (the gate's SEG_REFUSED rung), and the list is cleared at the next pass
    // (FL9), so its entries can vouch only for asks of a frame that never reaches the GPU. It must stay the ONLY other one.
    expect("P10 the frame-local feed exists EXACTLY TWICE in the whole file (per segment, and per unit constituent)",
           occurrences(raw, "n48_dl_from_output(&gXdFrameLocal") == 2u);
    { const size_t pCb = raw.find("static void gfxsrc_unit_cons_feed(void *ctx, const uint32_t *out, uint32_t n)");
      const size_t pCbFeed = raw.find("gXdFrameLocalS.vouched += n48_dl_from_output(&gXdFrameLocal, key, out, n, nullptr);");
      expect("P10 the second one is gfxsrc_unit_cons_feed's own (the translator's per-constituent callback)",
             pCb != std::string::npos && pCbFeed != std::string::npos && pCb < pCbFeed && pCbFeed - pCb < 400u); }
    { const size_t pRestore = raw.find("if (st && build) memcpy(&gXdNew[from], &gXdIb[from], (size_t)(to - from) * 4u);");
      expect("P10 setup: the restore-branch statement is found verbatim (so the mutation below inserts at a REAL site)",
             pRestore != std::string::npos);
      if (pRestore != std::string::npos) {
          const size_t stmtEnd = raw.find(';', pRestore) + 1u;
          std::string mut = raw;
          mut.insert(stmtEnd, " if (st && build) n48_dl_from_output(&gXdFrameLocal, dctxKey, out, olen, nullptr);");
          expect("P10 BREAK-check: another feed inside `if (st && build)` (a REFUSED segment vouching) is caught by the exact count",
                 occurrences(mut, "n48_dl_from_output(&gXdFrameLocal") == 3u);
      } }

    /* The BEHAVIOUR half of P10 ("a behaviour test through n48_dl_from_output that a refused segment's output
     * never vouches") is D-FL's own FL6, above (checks_frame_local(), a few hundred lines up): it mirrors the
     * kext's exact `st == 0u` gate over the SAME segment data both ways and proves the refused (st != 0) call
     * feeds nothing while the successful (st == 0) call does - non-vacuously (0.0.449 item 4/F6). This group
     * (D-FLW) adds the missing KEXT-GLUE half: that the real gate is applied EXACTLY ONCE, nowhere else. */
}

// ---------------------------------------------------------------------------------------------------------------------------
// E. 0.0.380 — THE EXACT DROP AND THE PHYSICAL-PAGE KEY.
//
// arm9 measured the ledger EMPTY on 255 of 256 judged frames with `cl 0` and `unmap-dropped 1`: n48_dl_unmap is the emptier,
// and it drops every entry of a context whatever the unmapVA's range was (1.32 unmaps per judged frame). CONFIRMED that
// Apple's own allocator re-maps a VA mid-run, so dropping less WITHOUT a physical-page key would keep an entry that now names
// somebody else's pages. The two are one unit and are tested as one.
// ---------------------------------------------------------------------------------------------------------------------------
typedef void (*UnmapFn)(n48_dl *, uint64_t, uint64_t, uint64_t, uint32_t);
typedef int (*AskFn)(n48_dl *, uint64_t, uint64_t, uint32_t, uint64_t, uint32_t);
struct LedOps2 { UnmapFn unmap; AskFn ask; };
static void unmap2_real(n48_dl *l, uint64_t c, uint64_t va, uint64_t sz, uint32_t ex) { n48_dl_unmap_rng(l, c, va, sz, ex); }
static int ask2_real(n48_dl *l, uint64_t c, uint64_t va, uint32_t m, uint64_t pg, uint32_t k) { return n48_dl_tiled_ok_pg(l, c, va, m, pg, k); }

/* THE BRIEF'S FIRST PLANTED DEFECT: an unmap that drops an entry it does not cover - i.e. the range is threaded through and
 * then ignored, which is today's wholesale drop wearing the new switch. */
static void unmap_mut_ignores_range(n48_dl *l, uint64_t ctx, uint64_t va, uint64_t size, uint32_t exact)
{ (void)va; (void)size; (void)exact; n48_dl_unmap(l, ctx); }
/* THE BRIEF'S SECOND: an unmap that KEEPS one it does cover. Here by a plausible wrong predicate - "covers" read as "starts at
 * the same VA", so an unmap of a wider range that contains the entry walks past it. */
static void unmap_mut_base_only(n48_dl *l, uint64_t ctx, uint64_t va, uint64_t size, uint32_t exact)
{
    l->unmaps++;
    uint32_t w = 0;
    for (uint32_t k = 0; k < l->n && k < N48_DL_MAX; k++) {
        const int mine = (!ctx || l->e[k].ctx == ctx);
        int drop = mine;
        if (mine && exact) drop = (size != 0u && l->e[k].va == va);
        if (drop) { l->unmapDropped++; continue; }
        if (mine) l->unmapKept++;
        if (w != k) { l->e[w].ctx = l->e[k].ctx; l->e[w].va = l->e[k].va; l->e[w].page = l->e[k].page;
                      l->e[w].mode = l->e[k].mode; l->e[w].flags = l->e[k].flags; l->e[w].size = l->e[k].size; }
        w++;
    }
    l->n = w;
}
/* Fail-OPEN on an unknown scope: a size of 0 (the whole context) is read as "covers nothing" and the entry survives. This is
 * the direction the brief forbids. */
static void unmap_mut_unknown_keeps(n48_dl *l, uint64_t ctx, uint64_t va, uint64_t size, uint32_t exact)
{
    l->unmaps++;
    uint32_t w = 0;
    for (uint32_t k = 0; k < l->n && k < N48_DL_MAX; k++) {
        const int mine = (!ctx || l->e[k].ctx == ctx);
        int drop = mine;
        if (mine && exact) drop = size ? !n48_dl_unmap_keeps(va, size, l->e[k].va, l->e[k].size) : 0;
        if (drop) { l->unmapDropped++; continue; }
        if (mine) l->unmapKept++;
        if (w != k) { l->e[w].ctx = l->e[k].ctx; l->e[w].va = l->e[k].va; l->e[w].page = l->e[k].page;
                      l->e[w].mode = l->e[k].mode; l->e[w].flags = l->e[k].flags; l->e[w].size = l->e[k].size; }
        w++;
    }
    l->n = w;
}
/* The switch is not a switch: the narrowing happens even at the default, so no A/B is possible and a boot that never threw it
 * silently changed behaviour. */
static void unmap_mut_always_exact(n48_dl *l, uint64_t ctx, uint64_t va, uint64_t size, uint32_t exact)
{ (void)exact; n48_dl_unmap_rng(l, ctx, va, size, 1u); }

/* 0.0.382 —'s WITHDRAWN RULE, RESTORED AS A DEFECT: the two-sided disjointness test over an entry extent of
 * "base VA to the end of its base page". arm9's own ctx-5 unmap [0x400240000,+0x40000) is 64 pages, so an entry inside such a
 * surface but past its first page is judged disjoint and KEPT - an entry the unmap really destroyed. */
static void unmap_mut_page_extent(n48_dl *l, uint64_t ctx, uint64_t va, uint64_t size, uint32_t exact)
{
    l->unmaps++;
    uint32_t w = 0;
    for (uint32_t k = 0; k < l->n && k < N48_DL_MAX; k++) {
        const int mine = (!ctx || l->e[k].ctx == ctx);
        int drop = mine;
        if (mine && exact) {
            const uint64_t esz = l->e[k].page ? (N48_DL_PAGE - (l->e[k].va & (N48_DL_PAGE - 1ull))) : 0ull;
            if (!size || !esz) l->unmapNoRange++;
            drop = 1;
            if (size && esz && (va + size <= l->e[k].va || l->e[k].va + esz <= va)) drop = 0;
        }
        if (drop) { l->unmapDropped++; continue; }
        if (mine) l->unmapKept++;
        if (w != k) { l->e[w].ctx = l->e[k].ctx; l->e[w].va = l->e[k].va; l->e[w].page = l->e[k].page;
                      l->e[w].mode = l->e[k].mode; l->e[w].flags = l->e[k].flags; l->e[w].size = l->e[k].size; }
        w++;
    }
    l->n = w;
}

/* 0.0.383 — BREAKING THE REAL OVERLAP TEST, THREE WAYS. The first is the whole point: the entry's extent is carried and then
 * not consulted, which is 0.0.382's one-sided rule wearing the new field, and it must kill arm10's 27. */
static void unmap_mut_extent_ignored(n48_dl *l, uint64_t ctx, uint64_t va, uint64_t size, uint32_t exact)
{
    l->unmaps++;
    uint32_t w = 0;
    for (uint32_t k = 0; k < l->n && k < N48_DL_MAX; k++) {
        const int mine = (!ctx || l->e[k].ctx == ctx);
        int drop = mine;
        if (mine && exact) {
            if (!size) l->unmapNoRange++;
            drop = !n48_dl_unmap_keeps(va, size, l->e[k].va, 0ull);        /* the extent thrown away */
            if (size && va + size > l->e[k].va && va + size >= va) { if (!l->e[k].size) l->unmapNoExtent++; else if (!drop) l->unmapKeptExtent++; }
        }
        if (drop) { l->unmapDropped++; continue; }
        if (mine) l->unmapKept++;
        if (w != k) { l->e[w].ctx = l->e[k].ctx; l->e[w].va = l->e[k].va; l->e[w].page = l->e[k].page;
                      l->e[w].mode = l->e[k].mode; l->e[w].flags = l->e[k].flags; l->e[w].size = l->e[k].size; }
        w++;
    }
    l->n = w;
}
/* FAIL OPEN on a missing extent: an entry with no size is kept unless the unmap starts at its base. The direction the brief
 * forbids, and the one was thrown out for. */
static void unmap_mut_extent_open(n48_dl *l, uint64_t ctx, uint64_t va, uint64_t size, uint32_t exact)
{
    l->unmaps++;
    uint32_t w = 0;
    for (uint32_t k = 0; k < l->n && k < N48_DL_MAX; k++) {
        const int mine = (!ctx || l->e[k].ctx == ctx);
        int drop = mine;
        if (mine && exact) {
            const uint64_t vsz = l->e[k].size ? l->e[k].size : N48_DL_PAGE;   /* "no extent" read as one page */
            if (!size) l->unmapNoRange++;
            drop = !(size && (va + size <= l->e[k].va || l->e[k].va + vsz <= va));
            if (size && va + size > l->e[k].va && va + size >= va) { if (!l->e[k].size) l->unmapNoExtent++; else if (!drop) l->unmapKeptExtent++; }
        }
        if (drop) { l->unmapDropped++; continue; }
        if (mine) l->unmapKept++;
        if (w != k) { l->e[w].ctx = l->e[k].ctx; l->e[w].va = l->e[k].va; l->e[w].page = l->e[k].page;
                      l->e[w].mode = l->e[k].mode; l->e[w].flags = l->e[k].flags; l->e[w].size = l->e[k].size; }
        w++;
    }
    l->n = w;
}
/* An off-by-one at the far boundary: `<` where the predicate says `<=`, so an unmap beginning exactly at the entry's END is
 * judged to overlap. Fail-closed, and still wrong - it is not ws_resprov.h's predicate. */
static void unmap_mut_extent_offbyone(n48_dl *l, uint64_t ctx, uint64_t va, uint64_t size, uint32_t exact)
{
    l->unmaps++;
    uint32_t w = 0;
    for (uint32_t k = 0; k < l->n && k < N48_DL_MAX; k++) {
        const int mine = (!ctx || l->e[k].ctx == ctx);
        int drop = mine;
        if (mine && exact) {
            const uint64_t vsz = l->e[k].size;
            if (!size) l->unmapNoRange++;
            drop = 1;
            if (size && va + size >= va) {
                if (va + size <= l->e[k].va) drop = 0;
                else if (vsz && l->e[k].va + vsz < va) drop = 0;            /* `<` instead of `<=` */
            }
            if (size && va + size > l->e[k].va && va + size >= va) { if (!vsz) l->unmapNoExtent++; else if (!drop) l->unmapKeptExtent++; }
        }
        if (drop) { l->unmapDropped++; continue; }
        if (mine) l->unmapKept++;
        if (w != k) { l->e[w].ctx = l->e[k].ctx; l->e[w].va = l->e[k].va; l->e[w].page = l->e[k].page;
                      l->e[w].mode = l->e[k].mode; l->e[w].flags = l->e[k].flags; l->e[w].size = l->e[k].size; }
        w++;
    }
    l->n = w;
}

/* THE BRIEF'S THIRD PLANTED DEFECT: a re-mapped page answered as if unchanged - the key is carried and never compared. */
static int ask_mut_ignores_page(n48_dl *l, uint64_t c, uint64_t va, uint32_t m, uint64_t pg, uint32_t k)
{ (void)pg; (void)k; return n48_dl_tiled_ok_pg(l, c, va, m, 0ull, 0u); }
/* A missing page on either side counts as a match: "no key" read as "key satisfied". */
static int ask_mut_missing_ok(n48_dl *l, uint64_t ctx, uint64_t va, uint32_t mode, uint64_t page, uint32_t key)
{
    l->asked++;
    if (!ctx) return 0;
    for (uint32_t k = 0; k < l->n && k < N48_DL_MAX; k++)
        if (l->e[k].ctx == ctx && l->e[k].va == va && l->e[k].mode == mode) {
            if (key && page && l->e[k].page && l->e[k].page != page) { l->keyMoved++; return 0; }
            l->proven++; return 1;
        }
    return 0;
}
/* The other vacuity trap: with the key on NOTHING is ever proven, so a run would read as "the key works" while the path is
 * simply dead. */
static int ask_mut_refuses_all(n48_dl *l, uint64_t ctx, uint64_t va, uint32_t mode, uint64_t page, uint32_t key)
{ if (key) { l->asked++; l->keyNoPage++; return 0; } return n48_dl_tiled_ok_pg(l, ctx, va, mode, page, key); }
/* The key acts at the default too: no A/B, and every boot that never threw `gfxneuter 18` loses answers it used to give. */
static int ask_mut_always_key(n48_dl *l, uint64_t c, uint64_t va, uint32_t m, uint64_t pg, uint32_t k)
{ (void)k; return n48_dl_tiled_ok_pg(l, c, va, m, pg, 1u); }

/* One committed frame writing CB0 at `va` in mode 3, with the frame's own walk row (`va` -> `page`, page 0 = did not resolve). */
static void feed_with_page(n48_dl *l, uint64_t ctx, uint64_t va, uint64_t page)
{
    static uint32_t o[64];
    static uint64_t rowVa[1], rowPg[1];
    n48_cm_seg seg[2];
    const uint32_t n = seg_cb0(o, va, 3u, 0);
    rowVa[0] = va; rowPg[0] = page;
    n48_dl_frame f = frame_ok(o, n, seg, ctx);
    f.pg.va = rowVa; f.pg.page = rowPg; f.pg.n = 1u;
    n48_dl_feed(l, &f);
}

static void checks_exact(const LedOps2 &op)
{
    const uint64_t VA = 0x400800000ull, PG = 0x10030000ull;   /*'s own page for this VA */
    n48_dl a, b;

    /* E1 the feed carries the page the frame's own walk found, and only for a VA that walk named. */
    std::memset(&a, 0, sizeof a);
    feed_with_page(&a, 5u, VA, PG);
    expect("E1 a fed entry carries the base physical page its frame resolved", a.n == 1u && a.e[0].page == PG);
    { n48_dl c; std::memset(&c, 0, sizeof c);
      feed_with_page(&c, 5u, VA, 0ull);
      expect("E1 a target whose page did not resolve carries no key (0)", c.n == 1u && c.e[0].page == 0ull); }

    /* E2 THE DEFAULT IS TODAY'S BEHAVIOUR, AND THE SWITCH IS NON-VACUOUS: the same unmap, the same ledger, two answers. */
    std::memset(&a, 0, sizeof a); feed_with_page(&a, 5u, VA, PG);
    std::memset(&b, 0, sizeof b); feed_with_page(&b, 5u, VA, PG);
    op.unmap(&a, 5u, VA - 0x100000ull, 0x1000ull, 0u);          /* a range ENDING BELOW the entry, switch OFF */
    op.unmap(&b, 5u, VA - 0x100000ull, 0x1000ull, 1u);          /* the same unmap, switch ON */
    expect("E2 DEFAULT: an unmap of an unrelated range still drops the entry (today's wholesale drop)",
           a.n == 0u && a.unmapDropped == 1u && a.unmapKept == 0u);
    expect("E2 ON: the same unmap leaves it standing - the two differ, so the switch is not vacuous",
           b.n == 1u && b.unmapDropped == 0u && b.unmapKept == 1u);
    expect("E2 and the entry still answers after it", op.ask(&b, 5u, VA, 3u, PG, 0u) == 1);

    /* E3 an unmap that DOES cover the entry drops it, switch on. Three shapes: exact, containing, and straddling the base. */
    { const struct { const char *what; uint64_t va, size; } hit[] = {
          { "exactly the entry's own page", VA, 0x1000ull },
          { "a wide range containing it",   VA - 0x10000ull, 0x100000ull },
          { "a range straddling its base",  VA - 0x800ull, 0x1000ull },
          { "a range inside its base page", VA + 0x100ull, 0x8ull } };
      for (const auto &h : hit) {
          n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
          op.unmap(&c, 5u, h.va, h.size, 1u);
          expect(std::string("E3 exact ON: an unmap covering the entry drops it - ").append(h.what).c_str(),
                 c.n == 0u && c.unmapDropped == 1u); } }

    /* E4-E5 FAIL CLOSED. An unknown scope on EITHER side drops exactly as today. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      op.unmap(&c, 5u, 0ull, 0ull, 1u);
      expect("E4 exact ON: an unmap with NO size (the whole context) still drops the entry", c.n == 0u && c.unmapDropped == 1u);
      expect("E4 and it is counted as a fallback to the wholesale rule", c.unmapNoRange == 1u); }
    /* E5 0.0.382: the entry's own page is NO LONGER READ by the drop rule -'s extent is withdrawn, so a
     * keyless entry is kept or dropped on the UNMAP's range alone. The page still decides the ANSWER (the key), never the drop. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, 0ull);
      op.unmap(&c, 5u, VA - 0x100000ull, 0x1000ull, 1u);
      expect("E5 exact ON: a keyless entry is judged on the unmap's range alone and survives one ending below it",
             c.n == 1u && c.unmapKept == 1u);
      expect("E5 and no wholesale fallback was taken - the entry's extent is never consulted", c.unmapNoRange == 0u);
      op.unmap(&c, 5u, VA, 0x1000ull, 1u);
      expect("E5 and the same keyless entry is dropped by an unmap that reaches it", c.n == 0u); }
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      op.unmap(&c, 5u, ~0ull - 0x10ull, 0x1000ull, 1u);
      expect("E5 exact ON: an unmap range that wraps 2^64 is no scope at all and drops", c.n == 0u && c.unmapDropped == 1u); }

    /* E6 the context rule is untouched by either switch. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG); feed_with_page(&c, 9u, VA, PG);
      op.unmap(&c, 5u, VA, 0x1000ull, 1u);
      expect("E6 exact ON: another context's entry at the same VA is untouched", c.n == 1u && c.e[0].ctx == 9u);
      op.unmap(&c, 0u, VA - 0x100000ull, 0x1000ull, 1u);
      expect("E6 exact ON: an unmap of an UNKNOWN context (0) reaches every entry, and this one ends below it",
             c.n == 1u);
      op.unmap(&c, 0u, VA, 0x1000ull, 1u);
      expect("E6 and covers this one", c.n == 0u); }

    /* E7 0.0.382 INVERTS's CAVEAT, and this is the change: a partial unmap of a LATER page of the surface
     * now DROPS the entry, because no extent is claimed for it. The gap that is left is the opposite one - an unmap of a LOWER
     * range belonging to the same allocation is indistinguishable from an unrelated one and keeps the entry. Both are pinned. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      op.unmap(&c, 5u, VA + 0x1000ull, 0x1000ull, 1u);
      expect("E7 exact ON: an unmap of a LATER page of the surface DROPS the entry ('s one-page extent is withdrawn)",
             c.n == 0u && c.unmapDropped == 1u); }
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      op.unmap(&c, 5u, VA - 0x1000ull, 0x1000ull, 1u);
      expect("E7 CAVEAT, pinned: an unmap ENDING at the base VA keeps the entry even if it belonged to the same allocation - "
             "nothing on this path knows a surface's size", c.n == 1u); }
}

static void checks_key(const LedOps2 &op)
{
    const uint64_t VA = 0x400800000ull, PG = 0x10030000ull, PG2 = 0x13aa0000ull;
    n48_dl a, b;

    /* E8's re-map, both ways round: the default answers, the key refuses. NON-VACUITY, stated as one check. */
    std::memset(&a, 0, sizeof a); feed_with_page(&a, 5u, VA, PG);
    std::memset(&b, 0, sizeof b); feed_with_page(&b, 5u, VA, PG);
    expect("E8 DEFAULT: the VA was re-mapped (0x10030000 -> 0x13aa0000) and the ledger still answers YES (today's behaviour)",
           op.ask(&a, 5u, VA, 3u, PG2, 0u) == 1);
    expect("E8 KEY ON: the same ask over the same ledger REFUSES - the two differ, so the key is not vacuous",
           op.ask(&b, 5u, VA, 3u, PG2, 1u) == 0);
    expect("E8 and the refusal is counted as a MOVE, not as a missing key", b.keyMoved == 1u && b.keyNoPage == 0u);

    /* E9 the key is not a blanket refusal: the unchanged page still proves. (A key that refuses everything passes E8.) */
    expect("E9 KEY ON: the SAME page still answers yes", op.ask(&b, 5u, VA, 3u, PG, 1u) == 1 && b.proven == 1u);

    /* E10 no key on either side is not a match, and the two refusals are counted apart. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      expect("E10 KEY ON: an asker whose own walk found no page is refused", op.ask(&c, 5u, VA, 3u, 0ull, 1u) == 0);
      expect("E10 and counted as a missing key", c.keyNoPage == 1u && c.keyMoved == 0u); }
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, 0ull);
      expect("E10 KEY ON: an ENTRY recorded without a page is refused", op.ask(&c, 5u, VA, 3u, PG, 1u) == 0);
      expect("E10 and counted as a missing key", c.keyNoPage == 1u && c.keyMoved == 0u);
      expect("E10 DEFAULT: the same entry still answers yes (today's behaviour)", op.ask(&c, 5u, VA, 3u, PG, 0u) == 1); }

    /* E11 the key can only ever REFUSE: it never turns a no into a yes. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      expect("E11 KEY ON: a wrong context is still refused", op.ask(&c, 6u, VA, 3u, PG, 1u) == 0);
      expect("E11 KEY ON: a wrong mode is still refused", op.ask(&c, 5u, VA, 2u, PG, 1u) == 0);
      expect("E11 KEY ON: a wrong VA is still refused", op.ask(&c, 5u, VA + 0x1000ull, 3u, PG, 1u) == 0);
      expect("E11 KEY ON: context 0 is still refused", op.ask(&c, 0u, VA, 3u, PG, 1u) == 0); }

    /* E12 BOTH halves together, over's own sequence: the producer commits, Apple re-maps the VA, an unrelated range is
     * unmapped. The exact drop keeps the entry - and the key is the only thing standing between that entry and a wrong yes. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      op.unmap(&c, 5u, VA - 0x100000ull, 0x1000ull, 1u);
      expect("E12 the exact drop KEEPS the entry across an unmap ending below it", c.n == 1u);
      expect("E12 and WITHOUT the key that kept entry answers yes for the re-mapped page (the whole reason both are one unit)",
             op.ask(&c, 5u, VA, 3u, PG2, 0u) == 1);
      expect("E12 with the key it refuses", op.ask(&c, 5u, VA, 3u, PG2, 1u) == 0); }
}

/* E13 (rewritten again for 0.0.383) THE RULE HAS TWO POPULATIONS AND THE GRID MUST PIN BOTH, against the SAME predicate
 * ws_resprov.h already carries (n48_rp_clr_clean: `eva + esz <= va || va + bytes <= eva`).
 *   - AN ENTRY WITH NO DERIVED EXTENT (vsz 0) keeps 0.0.382's one-sided answer, and over the grid it is still a STRICT SUBSET
 *     of the two-sided predicate's keeps: fail closed, and PROPERLY tighter, so the no-extent path did not silently loosen;
 *   - AN ENTRY WITH A DERIVED EXTENT agrees with n48_rp_clr_clean EXACTLY at every grid point - not a subset, EQUAL - which is
 *     the claim "the two-sided test is ws_resprov.h's predicate verbatim, over a size we now have" reduced to an assertion;
 *   - an unmap of size 0 (the whole context) and a range that wraps both DROP whatever the extent is, as does an entry extent
 *     that wraps 2^64. */
static void checks_predicate()
{
    const uint64_t base = 0x400800000ull;
    int cases = 0, subset = 0, tighter = 0, exact = 0;
    for (uint64_t ev = 0; ev < 6; ev++)
        for (uint64_t es = 0; es < 4; es++)
            for (uint64_t qv = 0; qv < 6; qv++)
                for (uint64_t qs = 1; qs < 4; qs++) {   /* bytes > 0: a zero-byte queried extent is degenerate for the
                                                           * two-sided predicate (it refuses everything), and for the rule
                                                           * it means "no extent", which the checks below cover separately */
                    const uint64_t eva = base + ev * 0x1000ull, esz = es * 0x1000ull;
                    const uint64_t va = base + qv * 0x1000ull, bytes = qs * 0x1000ull;
                    n48_rp_clog g; std::memset(&g, 0, sizeof g);
                    n48_rp_clr_push(&g, N48_RP_CLR_UNMAP, 1u, eva, esz);
                    uint32_t why = 0;
                    const int clean = n48_rp_clr_clean(&g, 0ull, 1u, va, bytes, &why);   /* 1 = the two-sided rule KEEPS */
                    const int none  = n48_dl_unmap_keeps(eva, esz, va, 0ull);            /* NO derived extent */
                    const int with  = n48_dl_unmap_keeps(eva, esz, va, bytes);           /* the SAME extent it is asked about */
                    cases++;
                    if (!none || clean) subset++;               /* none => clean : never keeps what the two-sided one drops */
                    if (clean && !none) tighter++;              /* and the no-extent path really is tighter somewhere */
                    if (with == clean) exact++;                 /* with an extent it IS ws_resprov.h's predicate */
                }
    expect("E13 a NO-EXTENT entry never keeps what ws_resprov.h's two-sided predicate would drop (fail-closed, 432 cases)",
           cases == subset && cases == 432);
    expect("E13 and the no-extent path is STRICTLY tighter - the second disjunct it cannot use keeps entries it drops",
           tighter > 0);
    expect("E13 0.0.383: WITH a derived extent the rule is ws_resprov.h's predicate EXACTLY, at all 432 grid points",
           exact == cases);
    expect("E13 size 0 (the whole context) DROPS, extent or no extent",
           n48_dl_unmap_keeps(base, 0ull, base + 0x100000ull, 0ull) == 0 &&
           n48_dl_unmap_keeps(base, 0ull, base + 0x100000ull, 0x870000ull) == 0);
    expect("E13 a range that wraps 2^64 DROPS, extent or no extent",
           n48_dl_unmap_keeps(~0ull - 0x1000ull, 0x8000ull, base, 0ull) == 0 &&
           n48_dl_unmap_keeps(~0ull - 0x1000ull, 0x8000ull, base, 0x870000ull) == 0);
    /* The entry sits just under 2^64 and its extent runs past it; the unmap is ABOVE its base, so the first disjunct
     * cannot answer and the wrapping extent has to. */
    expect("E13 an ENTRY EXTENT that wraps 2^64 is no extent at all and DROPS",
           n48_dl_unmap_keeps(~0ull - 0x80ull, 0x40ull, ~0ull - 0x100ull, 0x8000ull) == 0);
    expect("E13 an unmap ending exactly AT the entry's base VA keeps it",
           n48_dl_unmap_keeps(base - 0x1000ull, 0x1000ull, base, 0ull) == 1);
    expect("E13 an unmap ending one byte past it drops it",
           n48_dl_unmap_keeps(base - 0x1000ull, 0x1001ull, base, 0ull) == 0);
    expect("E13 an unmap STARTING at the base VA drops a NO-EXTENT entry however small (0.0.382's answer, unchanged)",
           n48_dl_unmap_keeps(base, 1ull, base, 0ull) == 0);
    expect("E13 and the SAME unmap still drops it when it HAS an extent, because it is inside it",
           n48_dl_unmap_keeps(base, 1ull, base, 0x870000ull) == 0);
    expect("E13 an unmap starting exactly AT the entry's END is kept, one byte below it is not",
           n48_dl_unmap_keeps(base + 0x870000ull, 0x1000ull, base, 0x870000ull) == 1 &&
           n48_dl_unmap_keeps(base + 0x870000ull - 1ull, 0x1000ull, base, 0x870000ull) == 0);
}

/* E14 — THE WHOLE POINT, ASSERTED AGAINST arm9's REAL UNMAP RANGES.
 *
 * arm9's own log: the ledger is fed ONCE (`fd 1/1/1 fed 1` at f1, `added 1` for the boot) and the FIRST ask is frame 23
 * (`descriptor PROVENANCE REFUSED: surface VA 0x400800000 ... context 5, frame 23 seg 0`). CONFIRMED, read out of
 * notes/logs/runs/arm9/driverlog-stream.txt: WindowServer's context (create #5) fires 44 unmapVAs in that boot, every one of
 * them with a NON-ZERO size, and the highest end any of them reaches is 0x4007d5000 - BELOW the asked VA 0x400800000.
 * (SUSPECTED, and it is not asserted here: that the entry the f1 feed recorded carried exactly that VA. Nothing in the log
 * prints the fed entry's VA; what is confirmed is that the frames' single colour target and the f23 ask are both 0x400800000.)
 *
 * So: an entry at (ctx 5, 0x400800000) fed before f1's unmaps must SURVIVE all 44 under the one-sided rule, and must be
 * DROPPED by the first of them under today's default. If this ever fails, the run cannot answer the question it is for. */
static const struct { uint64_t va, size; } kArm9Ctx5[] = {
    {0x4000e0000ull,0x10000ull}, {0x4000b8000ull,0x8000ull}, {0x4000f0000ull,0x8000ull},
    {0x4000e0000ull,0x9000ull},  {0x400200000ull,0x10000ull},{0x4000b8000ull,0x8000ull},
    {0x400200000ull,0x40000ull}, {0x400240000ull,0x40000ull},{0x400280000ull,0x9000ull},
    {0x400240000ull,0x10000ull}, {0x4000b8000ull,0x8000ull}, {0x400238000ull,0x8000ull},
    {0x4002a0000ull,0x1a000ull}, {0x4002c0000ull,0x10000ull},{0x4000b8000ull,0x8000ull},
    {0x400238000ull,0x8000ull},  {0x4002a0000ull,0x9000ull}, {0x4000a0000ull,0x10000ull},
    {0x4000b8000ull,0x8000ull},  {0x4000a0000ull,0x10000ull},{0x4000a0000ull,0x10000ull},
    {0x4000b8000ull,0x8000ull},  {0x4000a0000ull,0x10000ull},{0x4000a0000ull,0x10000ull},
    {0x4000a0000ull,0x10000ull}, {0x4000b8000ull,0x8000ull}, {0x4000a0000ull,0x10000ull},
    {0x400380000ull,0x40000ull}, {0x4003c0000ull,0x40000ull},{0x4000a0000ull,0x10000ull},
    {0x400770000ull,0x10000ull}, {0x4002b8000ull,0x8000ull}, {0x4003e8000ull,0x8000ull},
    {0x400780000ull,0x40000ull}, {0x400770000ull,0x9000ull}, {0x4000be000ull,0x2000ull},
    {0x400780000ull,0x20000ull}, {0x4000bb000ull,0x1000ull}, {0x400235000ull,0x1000ull},
    {0x4007a0000ull,0x35000ull}, {0x4000be000ull,0x2000ull}, {0x4000bb000ull,0x1000ull},
    {0x4000bb000ull,0x1000ull},  {0x4000be000ull,0x1000ull} };

static void checks_arm9(const LedOps2 &op)
{
    const uint64_t VA = 0x400800000ull, PG = 0x10030000ull;
    const uint32_t N = (uint32_t)(sizeof kArm9Ctx5 / sizeof kArm9Ctx5[0]);
    expect("E14 arm9's WindowServer context fired 44 unmapVAs in the logged window", N == 44u);

    uint64_t hi = 0; uint32_t zero = 0;
    for (uint32_t i = 0; i < N; i++) { if (kArm9Ctx5[i].va + kArm9Ctx5[i].size > hi) hi = kArm9Ctx5[i].va + kArm9Ctx5[i].size;
                                       if (!kArm9Ctx5[i].size) zero++; }
    expect("E14 none of them has size 0 (so none falls back to the wholesale rule)", zero == 0u);
    expect("E14 the highest end any of them reaches is 0x4007d5000, below the asked VA", hi == 0x4007d5000ull && hi <= VA);

    /* THE ASSERTION: fed at f1, still there at the f23 ask, under the new rule. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      for (uint32_t i = 0; i < N; i++) op.unmap(&c, 5u, kArm9Ctx5[i].va, kArm9Ctx5[i].size, 1u);
      expect("E14 THE ENTRY FED AT f1 SURVIVES ALL 44 OF arm9's REAL ctx-5 UNMAPS", c.n == 1u);
      expect("E14 and it is still the one that was fed", c.e[0].ctx == 5u && c.e[0].va == VA && c.e[0].page == PG);
      expect("E14 so the f23 ask is ANSWERED, keyed on the page the feed resolved", op.ask(&c, 5u, VA, 3u, PG, 1u) == 1);
      expect("E14 the narrowing is what did it - 44 kept, none dropped", c.unmapKept == 44ull && c.unmapDropped == 0ull); }

    /* NON-VACUITY, against the same ranges: today's default kills it on the FIRST unmap. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      op.unmap(&c, 5u, kArm9Ctx5[0].va, kArm9Ctx5[0].size, 0u);
      expect("E14 non-vacuity: at the DEFAULT (exact 0) the very first of those 44 unmaps drops it", c.n == 0u); }

    /* E14b THE CASE THAT KILLED's EXTENT, from arm9's own ranges. 0x400240000 is a colour-target VA the run really
     * unmapped, and it unmapped it as [0x400240000,+0x40000) - 64 PAGES. An unmap of a LATER page of that surface must drop
     * an entry based there; a rule that credits the entry with a ONE-PAGE extent calls it disjoint and keeps it. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, 0x400240000ull, 0x11000000ull);
      op.unmap(&c, 5u, 0x400250000ull, 0x10000ull, 1u);     /* strictly inside arm9's real 0x40000 range, past page 0 */
      expect("E14b an unmap of a LATER page of a 64-page surface DROPS the entry (no extent is claimed)", c.n == 0u); }

    /* And a real covering unmap still drops it: ctx 1 fired [0x400000000,+0x870000) in the same boot, which DOES cover the
     * VA. It is another context, so it cannot reach this entry - but the same range against ctx 5 must drop it. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      op.unmap(&c, 1u, 0x400000000ull, 0x870000ull, 1u);
      expect("E14 arm9's ctx-1 unmap covering that VA does NOT reach a ctx-5 entry", c.n == 1u);
      op.unmap(&c, 5u, 0x400000000ull, 0x870000ull, 1u);
      expect("E14 but the same range fired by ctx 5 DOES drop it", c.n == 0u); }
}

/* G. 0.0.383 — THE ENTRY EXTENT, AND arm10's OWN 27 UNMAPS.
 *
 * WHAT arm10 PROVED, from its own log and its own capture (both on this host Mac, both quoted here):
 *   - the ledger held the entry at (ctx 5, VA 0x400800000, page 0x10030000) across 26 real ctx-5 unmaps and was killed by the
 *     27th, `va 0x4010a0000 size 0x10000` - a range 0xb7000 ABOVE the end of a 1920x1080 BGRA surface, overlapping NOTHING.
 *     The 27 below are ctx-5 fires #5..#31 of notes/logs/runs/arm10/driverlog-stream.txt's `unmapva: ENTRY ... va ... size`
 *     lines, in log order (fires #1..#4 precede the f1 feed; `unmapKept 26` + the one drop is what fixes the window);
 *   - the surface's own size is in the frame's registers: capture.bin IB F1 #0 (VA 0x4000d0000) writes CB_COLOR0_ATTRIB2
 *     0x01dfc437 = MIP0_HEIGHT 1079 / MIP0_WIDTH 1919 -> 1920 x 1080, CB_COLOR0_INFO 0x00008828 -> FORMAT 10 (8_8_8_8),
 *     CB_COLOR0_ATTRIB3 0x4ddec000 -> COLOR_SW_MODE 27 (gfx12 3 = ADDR3_64KB_2D) / MIP0_DEPTH 0 / RESOURCE_TYPE 1,
 *     CB_COLOR0_VIEW 0x00000000 - in the SAME SET_CONTEXT_REG run as CB_COLOR0_BASE 0x04008000. CONFIRMED.
 * So the assertion this group exists for: THE ENTRY MUST SURVIVE ALL 27. */
static const struct { uint64_t va, size; } kArm10Ctx5[] = {
    {0x400200000ull,0x10000ull}, {0x4000b8000ull,0x8000ull}, {0x400200000ull,0x40000ull},
    {0x400240000ull,0x40000ull}, {0x400280000ull,0x9000ull},  {0x400260000ull,0x10000ull},
    {0x4000b8000ull,0x8000ull},  {0x400238000ull,0x8000ull},  {0x4002e0000ull,0x1a000ull},
    {0x4002e0000ull,0x10000ull}, {0x4000b8000ull,0x8000ull},  {0x400238000ull,0x8000ull},
    {0x4002e0000ull,0x9000ull},  {0x4002f0000ull,0x10000ull}, {0x4000b8000ull,0x8000ull},
    {0x400390000ull,0x10000ull}, {0x4003b0000ull,0x10000ull}, {0x4000b8000ull,0x8000ull},
    {0x4003d0000ull,0x10000ull}, {0x4003f0000ull,0x10000ull}, {0x400410000ull,0x10000ull},
    {0x4000b8000ull,0x8000ull},  {0x400430000ull,0x10000ull}, {0x4004c0000ull,0x40000ull},
    {0x400500000ull,0x40000ull}, {0x4004d0000ull,0x10000ull}, {0x4010a0000ull,0x10000ull} };

/* The same translated segment seg_cb0 builds, PLUS the three gfx12 words the extent is derived from. Defaults are arm10's own
 * values; every argument exists so a check can take one of them away and watch the extent refuse. */
static uint32_t seg_cb0_x(uint32_t *o, uint64_t va, uint32_t mode, uint32_t w, uint32_t h,
                          uint32_t fmt, uint32_t view, uint32_t rsrc, uint32_t depth, int noAt2)
{
    uint32_t k = 0;
    o[k++] = 0xFFFF1000u;                                                                              // a one-dword NOP
    o[k++] = 0xC0016900u; o[k++] = (0x28c60u - 0x28000u) >> 2; o[k++] = (uint32_t)(va >> 8);           // CB_COLOR0_BASE
    o[k++] = 0xC0016900u; o[k++] = (0x28e40u - 0x28000u) >> 2; o[k++] = (uint32_t)(va >> 40) & 0xFFu;  // CB_COLOR0_BASE_EXT
    o[k++] = 0xC0016900u; o[k++] = (0x28c7cu - 0x28000u) >> 2;
    o[k++] = (mode << 15) | (rsrc << 24) | (depth & 0x3FFFu);                                          // CB_COLOR0_ATTRIB3
    if (!noAt2) { o[k++] = 0xC0016900u; o[k++] = (0x28c78u - 0x28000u) >> 2;
                  o[k++] = ((w - 1u) << 16) | ((h - 1u) & 0xFFFFu); }                                  // CB_COLOR0_ATTRIB2
    o[k++] = 0xC0016900u; o[k++] = (0x28ec0u - 0x28000u) >> 2; o[k++] = fmt;                           // CB_COLOR0_INFO
    o[k++] = 0xC0016900u; o[k++] = (0x28c64u - 0x28000u) >> 2; o[k++] = view;                          // CB_COLOR0_VIEW
    o[k++] = 0xC0016900u; o[k++] = (0x28850u - 0x28000u) >> 2; o[k++] = 0xFu;                          // CB_TARGET_MASK
    o[k++] = 0xC0012D00u; o[k++] = 3u; o[k++] = 2u;                                                    // DRAW_INDEX_AUTO
    return k;
}
static void feed_x(n48_dl *l, uint64_t ctx, uint64_t va, uint64_t page, uint32_t mode, uint32_t w, uint32_t h,
                   uint32_t fmt, uint32_t view, uint32_t rsrc, uint32_t depth, int noAt2)
{
    static uint32_t o[64];
    static uint64_t rowVa[1], rowPg[1];
    n48_cm_seg seg[2];
    const uint32_t n = seg_cb0_x(o, va, mode, w, h, fmt, view, rsrc, depth, noAt2);
    rowVa[0] = va; rowPg[0] = page;
    n48_dl_frame f = frame_ok(o, n, seg, ctx);
    f.pg.va = rowVa; f.pg.page = rowPg; f.pg.n = 1u;
    n48_dl_feed(l, &f);
}
/* arm10's own frame, exactly. */
static void feed_arm10(n48_dl *l, uint64_t ctx, uint64_t va, uint64_t page)
{ feed_x(l, ctx, va, page, 3u, 1920u, 1080u, 10u, 0u, 1u, 0u, 0); }

static void checks_arm10(const LedOps2 &op)
{
    const uint64_t VA = 0x400800000ull, PG = 0x10030000ull;
    const uint64_t SZ = 0x870000ull, END = VA + SZ;          /* 15 x 9 x 64 KiB = 0x870000 -> ends at 0x401070000 */
    const uint32_t N = (uint32_t)(sizeof kArm10Ctx5 / sizeof kArm10Ctx5[0]);

    /* G1 THE EXTENT ITSELF, and that it is scanout_copy.h's equation and not a second one. */
    expect("G1 the ADDR3_64KB_2D size equation gives 1920x1080x4bpe = 0x870000",
           n48_addr3_64kb_2d_bytes_4bpe(1920u, 1080u) == SZ);
    expect("G1 n48_dl_extent decodes arm10's OWN register words to that size",
           n48_dl_extent(((1920u - 1u) << 16) | (1080u - 1u), (3u << 15) | (1u << 24), 10u, 0u) == SZ);
    expect("G1 and the recorded entry carries it",
           [&]{ n48_dl c; std::memset(&c, 0, sizeof c); feed_arm10(&c, 5u, VA, PG);
                return c.n == 1u && c.e[0].size == SZ && c.entExtent == 1ull && c.entNoExtent == 0ull; }());
    /* G2 FAIL CLOSED: every shape the equation does not cover records NO extent, and each is counted as such. */
    { const struct { const char *what; uint32_t mode, w, h, fmt, view, rsrc, depth; int noAt2; } bad[] = {
          { "a swizzle we have no equation for",  4u, 1920u, 1080u, 10u, 0u, 1u, 0u, 0 },
          { "a linear target (mode 0)",           0u, 1920u, 1080u, 10u, 0u, 1u, 0u, 0 },
          { "a format that is not 4 bytes",       3u, 1920u, 1080u, 12u, 0u, 1u, 0u, 0 },
          { "an array slice (VIEW non-zero)",     3u, 1920u, 1080u, 10u, 1u, 1u, 0u, 0 },
          { "a 3D resource type",                 3u, 1920u, 1080u, 10u, 0u, 3u, 0u, 0 },
          { "MIP0_DEPTH non-zero",                3u, 1920u, 1080u, 10u, 0u, 1u, 4u, 0 },
          { "no ATTRIB2 in the output at all",    3u, 1920u, 1080u, 10u, 0u, 1u, 0u, 1 } };
      for (const auto &b : bad) {
          n48_dl c; std::memset(&c, 0, sizeof c);
          feed_x(&c, 5u, VA, PG, b.mode, b.w, b.h, b.fmt, b.view, b.rsrc, b.depth, b.noAt2);
          expect(std::string("G2 NO extent is derived for ").append(b.what).c_str(),
                 c.n == 1u && c.e[0].size == 0ull && c.entNoExtent == 1ull && c.entExtent == 0ull); } }

    /* G3 THE CORPUS ITSELF, before it is used for anything. */
    expect("G3 arm10's ctx-5 window is 27 unmaps long", N == 27u);
    { uint64_t hi = 0; uint32_t zero = 0;
      for (uint32_t i = 0; i + 1u < N; i++) { if (kArm10Ctx5[i].va + kArm10Ctx5[i].size > hi) hi = kArm10Ctx5[i].va + kArm10Ctx5[i].size;
                                              if (!kArm10Ctx5[i].size) zero++; }
      expect("G3 none of them has size 0 (so none falls back to the wholesale rule)",
             zero == 0u && kArm10Ctx5[N - 1u].size != 0ull);
      expect("G3 the first 26 all end at or below the entry's base VA - the one-sided rule kept them", hi <= VA);
      expect("G3 and the 27th is 0x4010a0000 +0x10000, ABOVE the entry's END, overlapping nothing",
             kArm10Ctx5[N - 1u].va == 0x4010a0000ull && kArm10Ctx5[N - 1u].size == 0x10000ull &&
             kArm10Ctx5[N - 1u].va >= END && kArm10Ctx5[N - 1u].va - END == 0x30000ull); }

    /* G4 ★ THE ASSERTION: the real entry survives ALL 27 of arm10's real unmaps, and the ask is answered. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_arm10(&c, 5u, VA, PG);
      for (uint32_t i = 0; i < N; i++) op.unmap(&c, 5u, kArm10Ctx5[i].va, kArm10Ctx5[i].size, 1u);
      expect("G4 THE ENTRY FED AT f1 SURVIVES ALL 27 OF arm10's REAL ctx-5 UNMAPS", c.n == 1u);
      expect("G4 and it is still the one that was fed, extent intact",
             c.e[0].ctx == 5u && c.e[0].va == VA && c.e[0].page == PG && c.e[0].size == SZ);
      expect("G4 27 kept, none dropped", c.unmapKept == 27ull && c.unmapDropped == 0ull);
      expect("G4 and the 27th was kept BY THE EXTENT, not by the one-sided rule",
             c.unmapKeptExtent == 1ull && c.unmapNoExtent == 0ull);
      expect("G4 so the keyed ask is ANSWERED", op.ask(&c, 5u, VA, 3u, PG, 1u) == 1); }

    /* G5 NON-VACUITY, THREE WAYS, against the same 27 ranges. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);   /* the SAME entry with NO extent */
      for (uint32_t i = 0; i < N; i++) op.unmap(&c, 5u, kArm10Ctx5[i].va, kArm10Ctx5[i].size, 1u);
      expect("G5 non-vacuity: WITHOUT an extent the very same 27 kill it - this is the 0.0.382 defect, reproduced",
             c.n == 0u && c.unmapKept == 26ull && c.unmapDropped == 1ull);
      expect("G5 and the drop is counted as a drop for want of an extent", c.unmapNoExtent == 1ull); }
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_arm10(&c, 5u, VA, PG);
      op.unmap(&c, 5u, kArm10Ctx5[0].va, kArm10Ctx5[0].size, 0u);
      expect("G5 non-vacuity: at the DEFAULT (exact 0) the very first of the 27 drops it, extent or no extent", c.n == 0u); }
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_arm10(&c, 5u, VA, PG);
      op.unmap(&c, 5u, kArm10Ctx5[N - 1u].va, kArm10Ctx5[N - 1u].size, 1u);
      expect("G5 the 27th ALONE leaves the entry standing", c.n == 1u && c.unmapKeptExtent == 1ull); }

    /* G6 A SYNTHETIC UNMAP THAT GENUINELY OVERLAPS THE SURFACE MUST DROP IT - five shapes, and the two boundaries. */
    { const struct { const char *what; uint64_t va, size; int keep; } hit[] = {
          { "the entry's own base page",               VA,                    0x1000ull,  0 },
          { "a page in the MIDDLE of the surface",     VA + 0x400000ull,      0x1000ull,  0 },
          { "the surface's LAST page",                 END - 0x1000ull,       0x1000ull,  0 },
          { "a range containing the whole surface",    VA - 0x10000ull,       SZ + 0x20000ull, 0 },
          { "a range straddling the surface's END",    END - 1ull,            0x10000ull, 0 },
          { "a range ending exactly AT the base",      VA - 0x10000ull,       0x10000ull, 1 },
          { "a range starting exactly AT the end",     END,                   0x10000ull, 1 } };
      for (const auto &h : hit) {
          n48_dl c; std::memset(&c, 0, sizeof c); feed_arm10(&c, 5u, VA, PG);
          op.unmap(&c, 5u, h.va, h.size, 1u);
          expect(std::string(h.keep ? "G6 KEPT: " : "G6 DROPPED: ").append(h.what).c_str(),
                 c.n == (h.keep ? 1u : 0u)); } }

    /* G7 AN ENTRY WITH NO DERIVABLE SIZE KEEPS 0.0.382's ANSWER EXACTLY: it drops on the first higher-address unmap,
     * however far above it that unmap is, and the drop is counted apart from a drop the extent really justified. */
    { n48_dl c; std::memset(&c, 0, sizeof c);
      feed_x(&c, 5u, VA, PG, 0u, 1920u, 1080u, 10u, 0u, 1u, 0u, 0);          /* linear: no equation, no extent */
      expect("G7 the entry carries no extent", c.n == 1u && c.e[0].size == 0ull);
      op.unmap(&c, 5u, VA - 0x10000ull, 0x10000ull, 1u);
      expect("G7 an unmap ending at its base still keeps it", c.n == 1u && c.unmapNoExtent == 0ull);
      op.unmap(&c, 5u, VA + 0x8000000ull, 0x1000ull, 1u);                    /* 128 MiB above: overlaps nothing */
      expect("G7 and the first HIGHER-address unmap drops it - today's behaviour, unchanged",
             c.n == 0u && c.unmapDropped == 1ull && c.unmapNoExtent == 1ull); }

    /* G8 the extent never reaches another context, and the whole-context unmap is untouched by any of this. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_arm10(&c, 5u, VA, PG); feed_arm10(&c, 9u, VA, PG);
      op.unmap(&c, 9u, VA + 0x400000ull, 0x1000ull, 1u);
      expect("G8 an unmap inside the surface reaches only its own context's entry", c.n == 1u && c.e[0].ctx == 5u);
      op.unmap(&c, 5u, 0ull, 0ull, 1u);
      expect("G8 and a size-0 unmap (the whole context) still drops it, extent and all", c.n == 0u); }
}

/* F. 0.0.382 — THE DEFERRED DRAIN. */
typedef uint32_t (*DrainFn)(n48_dl *, const n48_rp_clog *, uint64_t, uint64_t *, uint32_t);
static uint32_t drain_real(n48_dl *l, const n48_rp_clog *g, uint64_t s, uint64_t *m, uint32_t e)
{ return n48_dl_drain(l, g, s, m, e); }

/* THE PLANTED DEFECT THE BRIEF NAMES FIRST, in its pure form: a drain that applies every event to EVERY context - i.e. the
 * event's own ctxSeq is thrown away, which is the wholesale wipe the epoch bump already did. */
static uint32_t drain_mut_wrong_ctx(n48_dl *l, const n48_rp_clog *g, uint64_t since, uint64_t *mark, uint32_t exact)
{
    const uint64_t now = n48_rp_clr_mark(g);
    if (mark) *mark = now;
    if (now < since || now - since > (uint64_t)N48_RP_CLR) { n48_dl_clear(l); return N48_DL_DRAIN_WRAP; }
    for (uint64_t n = since; n < now; n++) {
        const n48_rp_clr *e = &g->s[n % N48_RP_CLR];
        if (e->stamp != n + 1ull) { n48_dl_clear(l); return N48_DL_DRAIN_TORN; }
        if (e->kind != N48_RP_CLR_UNMAP) { n48_dl_clear(l); return N48_DL_DRAIN_WS; }
        n48_dl_unmap_rng(l, 0ull, e->va, e->size, exact);   /* ctx 0 reaches EVERY entry: the defect */
    }
    return N48_DL_DRAIN_OK;
}
/* A wrap that does not fail closed: the drain gives up on the events it cannot see and leaves the ledger standing. */
static uint32_t drain_mut_wrap_open(n48_dl *l, const n48_rp_clog *g, uint64_t since, uint64_t *mark, uint32_t exact)
{
    const uint64_t now = n48_rp_clr_mark(g);
    if (now - since > (uint64_t)N48_RP_CLR) { if (mark) *mark = now; return N48_DL_DRAIN_OK; }
    return n48_dl_drain(l, g, since, mark, exact);
}
/* A whole-table WindowServer event drained as if it were an unmap of nothing. */
static uint32_t drain_mut_ws_ignored(n48_dl *l, const n48_rp_clog *g, uint64_t since, uint64_t *mark, uint32_t exact)
{
    const uint64_t now = n48_rp_clr_mark(g);
    if (mark) *mark = now;
    if (now < since || now - since > (uint64_t)N48_RP_CLR) { n48_dl_clear(l); return N48_DL_DRAIN_WRAP; }
    for (uint64_t n = since; n < now; n++) {
        const n48_rp_clr *e = &g->s[n % N48_RP_CLR];
        if (e->stamp != n + 1ull) { n48_dl_clear(l); return N48_DL_DRAIN_TORN; }
        if (e->kind != N48_RP_CLR_UNMAP) continue;          /* the defect: a WS-wide clear is skipped */
        n48_dl_unmap_rng(l, (uint64_t)e->ctxSeq, e->va, e->size, exact);
    }
    return N48_DL_DRAIN_OK;
}

static void checks_drain(DrainFn dr)
{
    const uint64_t VA = 0x400800000ull, PG = 0x10030000ull;

    /* F1 THE CASE THE WHOLE CHANGE IS FOR: a busy unmap of an unrelated range no longer costs this entry anything. Under the
     * old branch it moved the epoch, which n48_dl_sync turns into a wipe of every context at the top of the next frame. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      n48_rp_clog g; std::memset(&g, 0, sizeof g);
      for (uint32_t i = 0; i < 44u; i++) n48_rp_clr_push(&g, N48_RP_CLR_UNMAP, 5u, kArm9Ctx5[i].va, kArm9Ctx5[i].size);
      uint64_t mark = 0;
      expect("F1 a drain of arm9's 44 deferred unmaps succeeds", dr(&c, &g, 0ull, &mark, 1u) == N48_DL_DRAIN_OK);
      expect("F1 and the entry SURVIVES all 44 of them", c.n == 1u && c.e[0].va == VA);
      expect("F1 the mark advanced to the whole ring's contents", mark == 44ull);
      expect("F1 every one of them was applied per entry", c.drainEv == 44ull && c.drainWiped == 0ull); }

    /* F2 the event's own context, and nobody else's. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG); feed_with_page(&c, 6u, VA, PG);
      n48_rp_clog g; std::memset(&g, 0, sizeof g);
      n48_rp_clr_push(&g, N48_RP_CLR_UNMAP, 6u, VA, 0x1000ull);      /* covers the VA, but it is ctx 6's */
      uint64_t mark = 0;
      expect("F2 the drain succeeds", dr(&c, &g, 0ull, &mark, 1u) == N48_DL_DRAIN_OK);
      expect("F2 ONLY the event's own context lost its entry", c.n == 1u && c.e[0].ctx == 5u); }

    /* F3 fail closed on a wrap: more than N48_RP_CLR events since the mark and the ledger is WIPED - today's behaviour. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      n48_rp_clog g; std::memset(&g, 0, sizeof g);
      for (uint32_t i = 0; i < N48_RP_CLR + 1u; i++) n48_rp_clr_push(&g, N48_RP_CLR_UNMAP, 9u, 0x10000ull, 0x1000ull);
      uint64_t mark = 0;
      expect("F3 a wrapped ring refuses", dr(&c, &g, 0ull, &mark, 1u) == N48_DL_DRAIN_WRAP);
      expect("F3 and WIPES - an unrelated context's events must not leave this entry standing", c.n == 0u);
      expect("F3 the mark is still advanced, so the next drain is not a second wrap", mark == (uint64_t)N48_RP_CLR + 1ull); }

    /* F4 fail closed on a whole-table WindowServer event. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      n48_rp_clog g; std::memset(&g, 0, sizeof g);
      n48_rp_clr_push(&g, N48_RP_CLR_WS, 0u, 0u, 0u);
      uint64_t mark = 0;
      expect("F4 a WindowServer-wide clear refuses", dr(&c, &g, 0ull, &mark, 1u) == N48_DL_DRAIN_WS);
      expect("F4 and WIPES", c.n == 0u); }

    /* F5 fail closed on a torn slot (a publisher mid-write: stamp 0). */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      n48_rp_clog g; std::memset(&g, 0, sizeof g);
      n48_rp_clr_push(&g, N48_RP_CLR_UNMAP, 5u, 0x10000ull, 0x1000ull);
      g.s[0].stamp = 0ull;                                  /* being written */
      uint64_t mark = 0;
      expect("F5 a torn slot refuses", dr(&c, &g, 0ull, &mark, 1u) == N48_DL_DRAIN_TORN);
      expect("F5 and WIPES", c.n == 0u); }

    /* F6 an empty drain is a no-op, and the DEFAULT rule still drops wholesale (the switch is a switch). */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      n48_rp_clog g; std::memset(&g, 0, sizeof g);
      uint64_t mark = 7ull;
      expect("F6 nothing to drain succeeds and holds", dr(&c, &g, 0ull, &mark, 1u) == N48_DL_DRAIN_OK && c.n == 1u && mark == 0ull);
      n48_rp_clr_push(&g, N48_RP_CLR_UNMAP, 5u, 0x10000ull, 0x1000ull);
      expect("F6 the same event at exact 0 drops the entry wholesale", dr(&c, &g, 0ull, &mark, 0u) == N48_DL_DRAIN_OK && c.n == 0u); }

    /* F7 a drained unmap with size 0 is the whole context, not a keep. */
    { n48_dl c; std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
      n48_rp_clog g; std::memset(&g, 0, sizeof g);
      n48_rp_clr_push(&g, N48_RP_CLR_UNMAP, 5u, 0ull, 0ull);
      uint64_t mark = 0;
      expect("F7 a size-0 unmap drains cleanly", dr(&c, &g, 0ull, &mark, 1u) == N48_DL_DRAIN_OK);
      expect("F7 and still empties that context", c.n == 0u && c.unmapNoRange == 1ull); }
}

/* G. 0.0.382 — OUR OWN 8-TARGET CAP, COUNTED APART. */
static void checks_cap()
{
    const uint64_t VA = 0x400800000ull;
    n48_dl c; std::memset(&c, 0, sizeof c);
    static uint32_t o[64];
    static uint64_t rowVa[1], rowPg[1];
    n48_cm_seg seg[2];
    const uint32_t n = seg_cb0(o, VA, 3u, 0);
    rowVa[0] = 0x401000000ull; rowPg[0] = 0x20000000ull;    /* a DIFFERENT target: this VA is not in the rows */
    n48_dl_frame f = frame_ok(o, n, seg, 5u);
    f.pg.va = rowVa; f.pg.page = rowPg; f.pg.n = 1u; f.pg.full = 1u;   /* the frame named more targets than the rows hold */
    n48_dl_feed(&c, &f);
    expect("G1 the entry was recorded with no page", c.n == 1u && c.e[0].page == 0ull);
    expect("G1 and stamped as OUR CAP, counted at the feed", (c.e[0].flags & N48_DL_F_CAPPED) != 0u && c.pgCapped == 1ull);
    expect("G2 the keyed ask refuses it", n48_dl_tiled_ok_pg(&c, 5u, VA, 3u, 0x10030000ull, 1u) == 0);
    expect("G2 and the refusal is counted BOTH as keyNoPage and, distinctly, as OUR CAP",
           c.keyNoPage == 1ull && c.keyCapped == 1ull && c.keyMoved == 0ull);
    /* The same entry with a page is not stamped and not counted. */
    n48_dl d; std::memset(&d, 0, sizeof d); feed_with_page(&d, 5u, VA, 0x10030000ull);
    expect("G3 an entry whose row WAS there is not stamped", (d.e[0].flags & N48_DL_F_CAPPED) == 0u && d.pgCapped == 0ull);
    expect("G3 and it answers", n48_dl_tiled_ok_pg(&d, 5u, VA, 3u, 0x10030000ull, 1u) == 1 && d.keyCapped == 0ull);
}

/* K. 0.0.394 — KEEP ACROSS A COVERING UNMAP, `gfxneuter 29`.
 *
 * THE SEQUENCE IS arm19b's OWN, out of notes/logs/runs/arm19b/driverlog-stream.txt, by content:
 *   - `unmapva: ENTRY for ctx 0xffffff903dd7da80 (create #5, fire #68 of this context) va 0x400800000 size 0x870000`
 *     (stream line 8679): WindowServer's OWN covering unmap of our fill's whole surface range, 8 s after the commit. It is
 *     the call that withdrew the ledger's one entry, exactly 13 s before the first consumer that sampled our surface.
 *   - `mapva: ctx 0xffffff903dd7da80 (create #5, fire #162) va 0x400800000` (stream line 8831): the same VA mapped again,
 *     and nothing re-fed the ledger.'s prediction turns on whether the page bound at this re-map is the one the feed
 *     recorded, so the test drives BOTH: SAME page -> proven; DIFFERENT page -> `keyMoved`. The two pages are's own
 *     for this very VA (0x10030000 -> 0x13aa0000); arm19b prints no page, and says as much.
 *
 * `keep` is the switch mirrored into `n48_dl.keep`, so the real unmap below is the ledger's own function, not a copy. */
typedef void (*KeepFn)(n48_dl *, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t);
static void keep_real(n48_dl *l, uint64_t c, uint64_t va, uint64_t sz, uint32_t ex, uint32_t keep)
{ l->keep = keep; n48_dl_unmap_rng(l, c, va, sz, ex); }
/* THE PLANTED DEFECT: the keep switch is READ and then IGNORED - the covering unmap drops as today, so every K check that
 * depends on the keep fails. This is the "break the real code" direction for the pure ledger. */
static void keep_mut_ignores(n48_dl *l, uint64_t c, uint64_t va, uint64_t sz, uint32_t ex, uint32_t keep)
{ (void)keep; l->keep = 0u; n48_dl_unmap_rng(l, c, va, sz, ex); }

static void checks_keep(const KeepFn &unmap)
{
    const uint64_t VA = 0x400800000ull, PG = 0x10030000ull, PG2 = 0x13aa0000ull;
    const uint64_t SZ = 0x870000ull;                 /* fire #68's size, verbatim (stream line 8679) */
    n48_dl c;

    /* K1 THE DEFAULT IS 0.0.393: the covering unmap still withdraws the entry. */
    std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
    unmap(&c, 5u, VA, SZ, 1u, 0u);
    expect("K1 switch OFF: arm19b's covering unmap still DROPS the entry (0.0.393)",
           c.n == 0u && c.unmapDropped == 1u && c.keptAcross == 0u);

    /* K2 ON: the SAME covering unmap KEEPS the entry, marks it, and leaves the recorded page alone. */
    std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
    unmap(&c, 5u, VA, SZ, 1u, 1u);
    expect("K2 fire #68: the covering unmap KEEPS the entry and drops nothing",
           c.n == 1u && c.unmapDropped == 0u && c.keptAcross == 1u);
    expect("K2 and marks it WITHDRAWN-by-unmap with its recorded page untouched",
           (c.e[0].flags & N48_DL_F_WITHDRAWN) != 0u && c.e[0].page == PG && c.e[0].va == VA);

    /* K3 fire #162, SAME page: the key proves it - the re-map put the fill's own pixels back. */
    expect("K3 fire #162 re-maps the SAME page: the key PROVES it", n48_dl_tiled_ok_pg(&c, 5u, VA, 3u, PG, 1u) == 1);
    expect("K3 and it is counted as proven-after-remap, not moved",
           c.proven == 1ull && c.provenRemap == 1ull && c.movedRemap == 0ull);

    /* K4 fire #162, DIFFERENT page: refused as `keyMoved`, and counted apart. This is the re-backed surface. */
    std::memset(&c, 0, sizeof c); feed_with_page(&c, 5u, VA, PG);
    unmap(&c, 5u, VA, SZ, 1u, 1u);
    expect("K4 fire #162 re-maps a DIFFERENT page: the key REFUSES", n48_dl_tiled_ok_pg(&c, 5u, VA, 3u, PG2, 1u) == 0);
    expect("K4 and the refusal is a MOVE after a re-map, counted apart",
           c.keyMoved == 1ull && c.movedRemap == 1ull && c.proven == 0ull && c.provenRemap == 0ull);

    /* K5 THE HONEST HAZARD, PINNED: with the key OFF there is nothing to compare, so the kept entry answers for whatever now
     * lives at the VA. This is why runs 18 and 29 together; the switch line says FAIL-OPEN. */
    { n48_dl d; std::memset(&d, 0, sizeof d); feed_with_page(&d, 5u, VA, PG);
      unmap(&d, 5u, VA, SZ, 1u, 1u);
      expect("K5 CAVEAT, pinned: with the key OFF a kept entry answers yes (FAIL-OPEN)",
             n48_dl_tiled_ok_pg(&d, 5u, VA, 3u, 0ull, 0u) == 1 && d.provenRemap == 1ull); }

    /* K6 THE TWO UNKNOWN SCOPES STILL FAIL CLOSED, even with the switch on. */
    { n48_dl d; std::memset(&d, 0, sizeof d); feed_with_page(&d, 5u, VA, PG);
      unmap(&d, 5u, 0ull, 0ull, 1u, 1u);
      expect("K6 a size-0 unmap (the whole context) still DROPS, keep or no keep",
             d.n == 0u && d.keptAcross == 0u); }
    { n48_dl d; std::memset(&d, 0, sizeof d); feed_with_page(&d, 5u, VA, PG);
      unmap(&d, 5u, ~0ull - 0x10ull, 0x1000ull, 1u, 1u);
      expect("K6 a range that wraps 2^64 is no scope at all and still DROPS", d.n == 0u && d.keptAcross == 0u); }

    /* K7 the context rule and the disjoint case are untouched: a covering unmap of ANOTHER context is not ours, and a
     * disjoint unmap keeps WITHOUT marking (that is switch 17's own answer, not this one). */
    { n48_dl d; std::memset(&d, 0, sizeof d); feed_with_page(&d, 5u, VA, PG); feed_with_page(&d, 9u, VA, PG);
      unmap(&d, 9u, VA, SZ, 1u, 1u);
      expect("K7 another context's covering unmap leaves the ctx-5 entry unmarked",
             d.n == 2u && (d.e[0].flags & N48_DL_F_WITHDRAWN) == 0u && d.keptAcross == 1u); }
    { n48_dl d; std::memset(&d, 0, sizeof d); feed_with_page(&d, 5u, VA, PG);
      unmap(&d, 5u, VA - 0x100000ull, 0x1000ull, 1u, 1u);
      expect("K7 a DISJOINT unmap keeps by switch 17's rule and marks nothing",
             d.n == 1u && (d.e[0].flags & N48_DL_F_WITHDRAWN) == 0u && d.keptAcross == 0u && d.unmapKept == 1u); }

    /* K8 THE DEFERRED DRAIN HONOURS IT TOO: the keep lives in the ledger, so n48_dl_drain's per-entry application (which calls
     * n48_dl_unmap_rng with the same l) gets the same answer. runs 21, so this is the path arm19b would actually take. */
    { n48_dl d; std::memset(&d, 0, sizeof d); feed_with_page(&d, 5u, VA, PG); d.keep = 1u;
      n48_rp_clog g; std::memset(&g, 0, sizeof g);
      n48_rp_clr_push(&g, N48_RP_CLR_UNMAP, 5u, VA, SZ);
      uint64_t mark = 0;
      expect("K8 a DRAINED covering unmap keeps and marks the same way",
             n48_dl_drain(&d, &g, 0ull, &mark, 1u) == N48_DL_DRAIN_OK && d.n == 1u &&
             (d.e[0].flags & N48_DL_F_WITHDRAWN) != 0u && d.keptAcross == 1u && d.drainEv == 1ull); }

    /* K9 THE CLASSIFICATION NAMES IT. A held-and-withdrawn ledger is not an ordinary hold. */
    { n48_dl d; std::memset(&d, 0, sizeof d); feed_with_page(&d, 5u, VA, PG);
      unmap(&d, 5u, VA, SZ, 1u, 1u);
      expect("K9 a ledger holding only a withdrawn entry reads WITHDRAWN, not HOLDING",
             n48_dl_empty_why(&d) == N48_DL_WHY_WITHDRAWN &&
             std::strcmp(n48_dl_why_name(n48_dl_empty_why(&d)), "withdrawn") == 0); }

    /* K10 A RE-FEED IS A FRESH PROOF: the mark is cleared, so the classification goes back to HOLDING. */
    { n48_dl d; std::memset(&d, 0, sizeof d); feed_with_page(&d, 5u, VA, PG);
      unmap(&d, 5u, VA, SZ, 1u, 1u);
      feed_with_page(&d, 5u, VA, PG);
      expect("K10 a re-feed of the same surface clears the withdrawn mark",
             d.n == 1u && (d.e[0].flags & N48_DL_F_WITHDRAWN) == 0u && n48_dl_empty_why(&d) == N48_DL_WHY_HOLDING); }
}

// ---------------------------------------------------------------------------------------------------------------------------
// C. the kext's own wiring, read out of AppleHardwareHook.cpp
// ---------------------------------------------------------------------------------------------------------------------------
static std::string strip_comments(const std::string &s)
{
    std::string o; o.reserve(s.size());
    size_t i = 0; const size_t n = s.size();
    while (i < n) {
        const char c = s[i];
        if (c == '"' || c == '\'') {                          // a literal: copied through, escapes honoured
            const char q = c; o += c; i++;
            while (i < n && s[i] != q) { if (s[i] == '\\' && i + 1 < n) { o += s[i++]; } o += s[i++]; }
            if (i < n) o += s[i++];
        } else if (c == '/' && i + 1 < n && s[i + 1] == '/') { while (i < n && s[i] != '\n') i++; }
        else if (c == '/' && i + 1 < n && s[i + 1] == '*') { i += 2; while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++; i += 2; }
        else o += s[i++];
    }
    return o;
}
// the brace-matched block that starts at the first '{' at or after `from`; "" when unbalanced
static std::string block_at(const std::string &s, size_t from, size_t *end = nullptr)
{
    size_t i = s.find('{', from);
    if (i == std::string::npos) return "";
    const size_t b = i; int depth = 0;
    for (; i < s.size(); i++) {
        const char c = s[i];
        if (c == '"' || c == '\'') { const char q = c; i++; while (i < s.size() && s[i] != q) { if (s[i] == '\\') i++; i++; } continue; }
        if (c == '{') depth++;
        else if (c == '}' && --depth == 0) { if (end) *end = i + 1; return s.substr(b, i + 1 - b); }
    }
    return "";
}
static std::string body_of(const std::string &s, const char *sig)
{
    const size_t p = s.find(sig);
    return p == std::string::npos ? std::string() : block_at(s, p);
}
static size_t count(const std::string &s, const std::string &t)
{
    size_t c = 0, p = 0;
    while (!t.empty() && (p = s.find(t, p)) != std::string::npos) { c++; p += t.size(); }
    return c;
}
// build 0.0.453 item 5: a WORD-BOUNDARY count - `count()` alone treats "XLAT12_EXTRA_DESC_INV" as present
// inside "XLAT12_EXTRA_DESC_INV_APPLE_HEAD" too (a plain substring match), which is wrong for a check whose whole
// point is "this exact flag name appears nowhere else" once a LONGER name sharing the same prefix exists. Counts
// `t` only where neither the character before nor the character after is an identifier character (alnum or `_`).
static size_t count_word(const std::string &s, const std::string &t)
{
    size_t c = 0, p = 0;
    while (!t.empty() && (p = s.find(t, p)) != std::string::npos) {
        const bool leftOk = (p == 0) || !(std::isalnum((unsigned char)s[p - 1]) || s[p - 1] == '_');
        const size_t after = p + t.size();
        const bool rightOk = (after >= s.size()) || !(std::isalnum((unsigned char)s[after]) || s[after] == '_');
        if (leftOk && rightOk) c++;
        p += t.size();
    }
    return c;
}
static bool has(const std::string &s, const char *t) { return s.find(t) != std::string::npos; }

// build 0.0.450 item 3 (switch 47) - THE KEXT'S OWN WIRING, read out of AppleHardwareHook.cpp itself, the
// same source-order-pin discipline group D-FLW already uses: proves gfxsrc_desc_tiled_ok calls n48_rp_ok_carry
// (not the plain n48_rp_ok) ONLY when gXdRpOkCarryOn is set, and the plain n48_rp_ok call - 0.0.449's own, byte
// for byte - is what runs otherwise. ws_resprov_test.cpp's test_hole4_carry already proves n48_rp_ok_carry's OWN
// epoch/superseded correctness at the header level (record at DECIDE, ask at COMMIT, epoch-move and switch-off
// planted breaks); this group proves the KEXT actually reaches that primitive through the real switch, not a
// literal-line pin on a block that might not be reachable at all.
static void checks_resprov_carry_wiring(const std::string &raw)
{
    if (raw.empty()) { expect("RPC0 SKIPPED: no source given", true); return; }
    const std::string tf = body_of(raw, "static int gfxsrc_desc_tiled_ok(void *ctx");
    expect("RPC1 gfxsrc_desc_tiled_ok's body was found", !tf.empty());
    expect("RPC2 the switch-off path calls n48_rp_ok - 0.0.449's exact call, same arguments, same order - exactly once",
           count(tf, "n48_rp_ok(&gXdRp, c->ctx, va, mode, elemBytes, gXdArm, gXdDpEpoch, &gfxsrc_rp_walk, c->vm)") == 1u);
    const char *kGuard = "if (gXdRpOkCarryOn) {";
    const char *kCarryCall = "n48_rp_ok_carry(&gXdRp, c->ctx, va, mode, elemBytes, gXdArm, gXdDpEpoch, &gfxsrc_rp_walk, c->vm, 1u,\n"
                              "                                N48_SD_ARM_DECIDE, N48_SD_ARM_COMMIT)) return 1;";
    // build 0.0.451 item 5 (N5, reviewer's planted break, NOT caught): the arm-pair argument ORDER, not
    // merely that the call exists - a swap (COMMIT, DECIDE) would compile and pass RPC2/the old RPC3 unchanged.
    expect("N5 the carry call passes N48_SD_ARM_DECIDE then N48_SD_ARM_COMMIT, in that order, exactly once",
           count(tf, kCarryCall) == 1u);
    // RPC3, REDONE (the reviewer's fix): a REAL structural check - the call must sit INSIDE the block block_at
    // extracts for `kGuard`, not merely "both strings appear somewhere in this function". Reusable so RPC4 below
    // can apply the SAME check to a genuinely mutated copy, rather than asserting a blanked string is absent.
    auto call_is_inside_guard = [&](const std::string &s) -> bool {
        const size_t g = s.find(kGuard);
        if (g == std::string::npos) return false;
        const std::string blk = block_at(s, g);
        return blk.find(kCarryCall) != std::string::npos;
    };
    expect("RPC3 the real source: n48_rp_ok_carry's call sits INSIDE `if (gXdRpOkCarryOn) { ... }`", call_is_inside_guard(tf));
    /* PLANTED BREAK ("carry applied with the switch OFF") - REDONE (the reviewer's fix): a REAL edit that MOVES
     * the carry call out from inside the guard block to just BEFORE it (FLW5's own technique: erase the real
     * statement from its found position, re-insert it elsewhere), so `call_is_inside_guard` - the SAME closure
     * used on the real source above - must now find it OUTSIDE the block and correctly return false. This is not
     * a check that a blanked string disappears from its own copy (always true); it is the SAME predicate,
     * evaluated on a text that genuinely differs in a way the predicate is supposed to catch. */
    { const size_t pGuard = tf.find(kGuard);
      const size_t pCall = tf.find(kCarryCall);
      expect("RPC4 setup: both the guard and the carry call are found verbatim (so the move below is real)",
             pGuard != std::string::npos && pCall != std::string::npos && pGuard < pCall);
      if (pGuard != std::string::npos && pCall != std::string::npos && pGuard < pCall) {
          std::string mut = tf;
          const std::string stmt = mut.substr(pCall, std::strlen(kCarryCall));
          mut.erase(pCall, stmt.size());
          mut.insert(pGuard, stmt + " ");   // now sits BEFORE the guard's own opening brace: unconditional
          const bool realOk = call_is_inside_guard(tf), mutOk = call_is_inside_guard(mut);
          std::printf("  planted %-66s %s (real inside-guard=%d, moved-out inside-guard=%d)\n",
                      "N5/RPC4: the carry call moved outside its switch guard", (realOk && !mutOk) ? "CAUGHT" : "MISSED", realOk, mutOk);
          expect("RPC4 BREAK-check: the SAME predicate correctly says NO once the call is moved outside the guard",
                 realOk && !mutOk); } }
}

// build 0.0.452 item 1 (F1): gfxsrc_desc_unmap's lock-busy branch (drainOn) must reach gXdRp - queuing
// (ctx, va, size), never merely counting the unmap and leaving gXdRp untouched.
//
// build 0.0.453 item 6: the bound-check, the push and the overflow fallback used to live INLINE in this call
// site (text-anchored below by the old F1 checks); they now live in gfx_rpunmapq.h (n48_rpuq_push/n48_rpuq_take),
// pure and host-tested with REAL concurrent threads in tests/gfx_rpunmapq_test.cpp - see that file for the race
// (item 6a) and single-context-overflow (item 6b) planted breaks. This function now checks only the WIRING: the
// busy branch calls n48_rpuq_push (not gXdRpUnmapQ's old fields directly), and the drain still runs where
// needs it to.
static void checks_resprov_unmap_busy_queue(const std::string &raw)
{
    if (raw.empty()) { expect("F1-0 SKIPPED: no source given", true); return; }
    const std::string s = strip_comments(raw);
    const std::string uf = body_of(s, "static void gfxsrc_desc_unmap(uint32_t ctxSeq, uint64_t va, uint64_t size) {");
    expect("F1-1 gfxsrc_desc_unmap's body was found", !uf.empty());
    const char *kBusyGuard = "} else if (drainOn) {";
    const size_t pBusy = uf.find(kBusyGuard);
    expect("F1-2 the lock-busy (drainOn) branch exists exactly once", count(uf, kBusyGuard) == 1u && pBusy != std::string::npos);
    const std::string busy = block_at(uf, pBusy);
    expect("F1-3 the busy branch's body was extracted", !busy.empty());
    expect("F1-4 (item 6) the busy branch pushes into gXdRpUnmapQ through n48_rpuq_push (the race-free, pure "
           "queue), not the old inline array/count", count(busy, "n48_rpuq_push(&gXdRpUnmapQ, ctxSeq, va, size);") == 1u);
    expect("F1-4b (item 6) the OLD inline fields are gone from this branch (no direct array/count write here "
           "any more - that logic now lives, and is tested, in gfx_rpunmapq.h)",
           busy.find("gXdRpUnmapQN") == std::string::npos && busy.find("gXdRpUnmapDirtyCtx") == std::string::npos);
    expect("F1-5 gfxsrc_rp_unmap_drain_locked() runs FIRST inside gfxsrc_rp_drain_locked, before the copy-queue drain",
           has(raw, "static void gfxsrc_rp_drain_locked() {\n    gfxsrc_rp_unmap_drain_locked();"));
    expect("F1-6 gfxsrc_rp_drain_locked() is called at the top of decide_frame, before anything can ask",
           has(raw, "if (dp && gXdResProv) gfxsrc_rp_drain_locked();   //: queued residency copies are in the "
                    "table before anything asks"));
    expect("F1-7 (item 6) the drain pops from gXdRpUnmapQ through n48_rpuq_pop (small, fixed stack footprint - see "
           "gfx_rpunmapq.h's own comment on the +0x40 stack rule)",
           has(raw, "while (n48_rpuq_pop(&gXdRpUnmapQ, &ent)) {"));
    // build 0.0.455 item 5 (decide45): an overflow no longer ALWAYS wipes the whole ledger - it first asks
    // n48_rpuq_take_dirty for a BOUNDED per-context range and only falls back to n48_rp_unmap(&gXdRp, 0u) (the
    // 0.0.453 item 6b behaviour) when that call itself reports `wholeWipe` (or, defensively, no dirty entries at
    // all). Both branches still run only inside `if (n48_rpuq_take_overflow(&gXdRpUnmapQ)) {`.
    expect("F1-7 (item 5) an overflow calls n48_rpuq_take_dirty, and BOTH the bounded (n48_rp_unmap_rng per "
           "context) and the whole-ledger (n48_rp_unmap(&gXdRp, 0u)) fallback still live inside the SAME "
           "`if (n48_rpuq_take_overflow(...))` guard",
           has(raw, "if (n48_rpuq_take_overflow(&gXdRpUnmapQ)) {") &&
           has(raw, "n48_rpuq_take_dirty(&gXdRpUnmapQ, dirty, &dirtyN, &wholeWipe);") &&
           has(raw, "if (wholeWipe || !dirtyN) {\n            n48_rp_unmap(&gXdRp, 0u);") &&
           has(raw, "n48_rp_unmap_rng(&gXdRp, dirty[i].ctx, dirty[i].vaMin, dirty[i].vaEnd - dirty[i].vaMin);"));
}
// 0.0.399: collect the printf format a HWLOG call passes - every adjacent string literal from `from` up to the
// first comma at the top level (the separator before the first argument). Only `\n`, `\"` and `\\` appear in these
// formats; anything else is copied through. Returns "" when there is no literal there.
static std::string collect_printf_fmt(const std::string &s, size_t from)
{
    if (from == std::string::npos) return "";
    const size_t q = s.find('"', from);
    if (q == std::string::npos) return "";
    std::string out; size_t i = q;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"') {
            i++;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    const char e = s[i + 1];
                    out += (e == 'n') ? '\n' : (e == '"') ? '"' : (e == '\\') ? '\\' : e;
                    i += 2;
                } else out += s[i++];
            }
            if (i < s.size()) i++;
        } else if (c == ',') break;
        else if (c == ' ' || c == '\n' || c == '\t' || c == '\r')
            i++;
        else break;
    }
    return out;
}
// build 0.0.453 item 7 (inv-f84/REPORT.txt): the resprov closing line USED to be ONE HWLOG call whose format
// string alone (614 bytes, measured by 0.0.452's own F2 test below this comment used to be, before ANY value was
// substituted) was already past n48_logf's 512-byte line buffer (n48log.cpp: `char line[512]`, hard vsnprintf
// truncation) - so the tail (elemMismatch and both warning counters) was silently cut on EVERY read of this
// report. Item 7 splits it into FOUR lines, each with the SAME `resprov:` prefix. This test measures each of the
// four (static substitution against the real format text, worst-case = every %llu at UINT64_MAX / every %u at
// UINT32_MAX / every %s at its own real longest literal - see the call site's own ternaries) and checks every one
// is under 512 bytes even at that worst case, and that elemMismatch (item 2, 0.0.452) still appears exactly once.
static void checks_resprov_line_budget(const std::string &raw)
{
    if (raw.empty()) { expect("F7-0 SKIPPED: no source given", true); return; }
    auto measure = [&](const std::string &f, int llu_w, int u_w, const std::vector<int> &sw) -> size_t {
        size_t total = 0, i = 0, si = 0;
        while (i < f.size()) {
            if (f.compare(i, 4, "%llu") == 0) { total += (size_t)llu_w; i += 4; }
            else if (f.compare(i, 2, "%u") == 0) { total += (size_t)u_w; i += 2; }
            else if (f.compare(i, 2, "%s") == 0) { total += (size_t)(si < sw.size() ? sw[si] : 40); si++; i += 2; }
            else { total++; i++; }
        }
        return total;
    };
    const std::string prefix = "AppleHardwareHook: ";   // HWLOG's own prefix, counted the same way n48_logf sees it
    struct Line { const char *name, *anchor; std::vector<int> smax; };
    const Line lines[] = {
        { "L1 (status/copies-handed-in)", "HWLOG(\"resprov: residency provenance is %s%s%s;", { 14, 23, 37 } },
        { "L2 (refused breakdown)",       "HWLOG(\"resprov: refused: not copied %llu,", {} },
        { "L3 (table/asked/proven)",      "HWLOG(\"resprov: table %u entr(y/ies),", {} },
        { "L4 (pending queue/legend/elemMismatch)", "HWLOG(\"resprov: pending queue (", {} },
    };
    size_t elemMismatchCount = 0;
    for (const auto &L : lines) {
        const size_t anchor = raw.find(L.anchor);
        expect((std::string("F7-1 ") + L.name + ": the line's format was found").c_str(), anchor != std::string::npos);
        if (anchor == std::string::npos) continue;
        const std::string fmt = collect_printf_fmt(raw, anchor);
        expect((std::string("F7-2 ") + L.name + ": the format text was collected").c_str(), !fmt.empty());
        expect((std::string("F7-3 ") + L.name + ": starts with the shared \"resprov: \" prefix").c_str(),
               fmt.compare(0, 9, "resprov: ") == 0);
        const size_t worst = prefix.size() + measure(fmt, 20, 10, L.smax) + 1u /* the HWLOG macro's own "\n" */;
        std::printf("  resprov (item 7) %-42s worst-case %zu bytes\n", L.name, worst);
        expect((std::string("F7-4 ") + L.name + ": fits n48_logf's 512-byte line buffer even at UINT64_MAX/UINT32_MAX").c_str(),
               worst < 512u);
        elemMismatchCount += count(fmt, "elem mismatch %llu");
    }
    expect("F7-5 elemMismatch (item 2, 0.0.452) is still printed, exactly once across the four lines",
           elemMismatchCount == 1u);
}
// 0.0.379: how many times `name.<field>` appears on the LEFT of a plain `=` (not `==`, not `!=`, `<=`, `>=`).
// Used to pin that an instrument is written in exactly one function and only read everywhere else.
static size_t writes_to(const std::string &s, const char *name)
{
    size_t c = 0, p = 0; const std::string t(name);
    while ((p = s.find(t, p)) != std::string::npos) {
        size_t i = p + t.size();
        while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.')) i++;
        while (i < s.size() && s[i] == ' ') i++;
        if (i < s.size() && s[i] == '=' && (i + 1 >= s.size() || s[i + 1] != '=') &&
            (i == 0 || (s[i - 1] != '!' && s[i - 1] != '<' && s[i - 1] != '>' && s[i - 1] != '='))) c++;
        p += t.size();
    }
    return c;
}

static void checks_source(const std::string &raw, const std::string &hdrRaw)
{
    const std::string s = strip_comments(raw);
    // 0.0.393: the consumer builder's loops live in gfx_cp_build.h; pin THE HEADER (comments
    // stripped) for those, and pin the kext's CALL to it here. Passing it separately keeps body_of(s, ...) reading only the
    // kext.
    const std::string hd = strip_comments(hdrRaw);
    // C1 the switch
    expect("C1 the switch is defined once, default 0", count(s, "static volatile uint32_t gXdDescPort { 0u };") == 1u);
    { const size_t vb = s.find("(arg & 0xffull) == 10ull");
      const std::string verb = vb == std::string::npos ? "" : block_at(s, vb);
      expect("C1 only the `gfxneuter 10` verb assigns it", count(s, "gXdDescPort =") == 1u && count(verb, "gXdDescPort =") == 1u); }
    // C2 the reader
    // 0.0.435 (notes/design/PGMID-COPYGUARD.md Part 2): the adapter is now routed through gfxc_read_rs
    // (gfxc_read's byte-identical recording twin - see gfxc_read_core in AppleHardwareHook.cpp), not gfxc_read
    // directly, so the copy-overlap reader check sees every VRAM page a descriptor read touches. Re-baselined
    // deliberately, the same way 0.0.433 re-baselined gfx_mmprio_test.cpp's T6: the review diffs old against new.
    const std::string gfxc = body_of(s, "static uint32_t gfxsrc_desc_gfxc(");
    expect("C2 the adapter exists and calls gfxc_read_rs exactly once", !gfxc.empty() && count(gfxc, "gfxc_read_rs(") == 1u);
    { bool clean = !gfxc.empty();
      for (const char *t : { "navi48_vram_read_mm", "gfxc_page(", "withPhysicalAddress", "vmib_to_vram_off", "IOMemoryDescriptor",
                             "gfxc_write_sys", "->map(" })
          if (has(gfxc, t)) clean = false;
      expect("C2 and reads memory NO other way (no MM window, page walk, physical mapping)", clean); }
    const std::string rd = body_of(s, "static int gfxsrc_desc_read(");
    expect("C3 desc_read goes through n48_dp_read over the adapter", !rd.empty() && count(rd, "n48_dp_read(&gfxsrc_desc_gfxc, c->vm,") == 1u);
    { bool clean = !rd.empty();
      for (const char *t : { "gfxc_read(", "navi48_vram_read_mm", "gfxc_page(", "withPhysicalAddress", "vmib_to_vram_off" })
          if (has(rd, t)) clean = false;
      expect("C3 and never around it (no direct gfxc_read, so the short-read refusal cannot be skipped)", clean); }
    // C4 the policy
    const std::string pol = body_of(s, "static void gfxsrc_policy(");
    std::string rest = pol, flagsBlock;
    for (;;) {
        const size_t p = rest.find("if (dp) {");
        if (p == std::string::npos) break;
        size_t e = 0; const std::string b = block_at(rest, p, &e);
        if (b.empty()) break;
        if (has(b, "ex.desc_read")) flagsBlock += b;
        rest.erase(p, e - p);
    }
    expect("C4 the policy's `if (dp)` block passes INLINE | TABLE | DESC_INV",
           has(flagsBlock, "XLAT12_EXTRA_INLINE_DESC") && has(flagsBlock, "XLAT12_EXTRA_TABLE_DESC") && has(flagsBlock, "XLAT12_EXTRA_DESC_INV"));
    expect("C4 and wires desc_read, desc_tiled_ok, desc_ctx and ib_va there",
           has(flagsBlock, "ex.desc_read = &gfxsrc_desc_read;") && has(flagsBlock, "ex.desc_tiled_ok = &gfxsrc_desc_tiled_ok;") &&
           has(flagsBlock, "ex.desc_ctx = &dctx;") && has(flagsBlock, "ex.ib_va = ibVa + 4ull * from;"));
    { bool clean = !pol.empty();
      for (const char *t : { "desc_read", "desc_tiled_ok", "desc_ctx", "ib_va", "XLAT12_EXTRA_INLINE_DESC", "XLAT12_EXTRA_TABLE_DESC",
                             "XLAT12_EXTRA_DESC_INV" })
          if (has(rest, t)) clean = false;
      expect("C4 and NOTHING of it outside `if (dp)` (the default policy is the old one)", clean); }
    // build 0.0.453 item 5: word-boundary counts for the two flag names - XLAT12_EXTRA_DESC_INV is now also a
    // PREFIX of XLAT12_EXTRA_DESC_INV_APPLE_HEAD (a legitimate, separate flag this item adds; see its own comment
    // and gfxsrc_policy's own new line for switch 49), so a plain substring count would over-count it there.
    expect("C4 the file names the flags nowhere else",
           count_word(s, "XLAT12_EXTRA_TABLE_DESC") == 1u && count_word(s, "XLAT12_EXTRA_DESC_INV") == 1u && count(s, "ex.desc_read =") == 1u);
    { const std::string dc = body_of(s, "static void gfxsrc_policy(");
      expect("C4 dctx is the policy's OWN vm (the frame's root)", has(dc, "dctx.vm = &vm;")); }
    // C5 the feed
    const std::string dec = body_of(s, "static uint32_t gfxsrc_decide_frame(");
    const size_t pFeedBlk = dec.find("if (dp && ranPolicy) {");
    const std::string feed = pFeedBlk == std::string::npos ? "" : block_at(dec, pFeedBlk);
    expect("C5 the ledger is fed exactly once in the file, inside decide_frame's `if (dp && ranPolicy)`",
           count(s, "n48_dl_feed(") == 1u && count(feed, "n48_dl_feed(") == 1u);
    expect("C5 from commit_ok and the gate's own reason", has(feed, "lf.committed = commitOk;") && has(feed, "lf.gate = (shapeOk && vm.ok && f.reader_ok) ? gXdCm.lastReason"));
    { const size_t pCommit = dec.find("gfxsrc_commit_try(");
      expect("C5 and only AFTER the COMMIT gate ran", pCommit != std::string::npos && pFeedBlk != std::string::npos && pFeedBlk > pCommit); }
    // C6 0.0.379: the sync is unchanged, but `n` either side of it and the epoch VALUE it was handed are kept for
    // the readout. The exact statement is pinned so the brackets cannot drift off the call they are supposed to bracket.
    { const char *kSync = "if (dp) { dpEp = gXdDpEpoch; ledBefore = gXdLed.n; n48_dl_sync(&gXdLed, arm, dpEp); ledAfter = gXdLed.n; }";
      const size_t pSync = dec.find(kSync), pPol = dec.find("gfxsrc_policy(");
      expect("C6 the ledger is synced (arm, epoch) before the policy can ask it, with `n` captured either side",
             pSync != std::string::npos && pPol != std::string::npos && pSync < pPol && count(s, "n48_dl_sync(") == 1u); }
    // C10-C13 0.0.379: THE READOUT. It must exist, be gated and capped, write nothing, and leave the one
    // producer of `dep_ok` alone.
    // build 0.0.525 (switch 80, CYCLE80.md X4): the cap is 256, or 1024 only while armed and switch 80 is ON or SHADOW; the
    // pins below name the extended condition (every check and mutant is otherwise unchanged).
    { const size_t pLine = dec.find("HWLOG(N48_DL_LINE_FMT");
      expect("C10 the per-frame readout uses N48_DL_LINE_FMT, once, inside decide_frame",
             pLine != std::string::npos && count(s, "N48_DL_LINE_FMT") == 1u);
      expect("C10 and is gated on the descriptor path AND capped for the boot",
             has(dec, "if (dp && dpLines < ((c80_mode() && hw_cm_armed()) ? kXdDpLines80 : kXdDpLines)) {") && has(dec, "static constexpr uint32_t kXdDpLines"));
      const size_t pGate = dec.find("if (dp && dpLines < ((c80_mode() && hw_cm_armed()) ? kXdDpLines80 : kXdDpLines)) {");
      const std::string ro = pGate == std::string::npos ? "" : block_at(dec, pGate);
      bool clean = !ro.empty();
      for (const char *t : { "n48_dl_feed(", "n48_dl_sync(", "n48_dl_set(", "n48_dl_clear(", "n48_dl_unmap(", "n48_dl_tiled_ok(",
                             "gXdLed.n =", "gXdLed.e[", "n48_rd32(", "n48_wr32(", "gfxc_write" })
          if (has(ro, t)) clean = false;
      expect("C11 the readout READS: it calls no mutator of the ledger and writes no memory", clean);
      expect("C11 and it says GATE-NOT-RUN rather than repeating a stale gate reading",
             has(ro, "depFresh") && has(dec, "gXdGateDep.frame == gXdC.judged + 1u") && has(ro, "GATE-NOT-RUN"));
      // build 0.0.450 item 0 (P12): the `rf %llu` FIELD'S OWN ARGUMENT, not only N48_DL_LINE_FMT's format
      // string (a reviewer mutant can print `rf` as a literal 0 while the format string above is untouched).
      // `ro` is the readout block body_of/block_at already isolated (the `if (dp && dpLines < ((c80_mode() && hw_cm_armed()) ? kXdDpLines80 : kXdDpLines)) { ... }`
      // scope), so this also proves the real argument sits inside the SAME gated call C10/C11 already located -
      // not a stray, unreachable copy elsewhere in the file (count(s, ...) below is over the WHOLE file).
      expect("P12 the ledger line's LAST argument is the real counter, not a literal (`rf` printed as 0 is caught)",
             has(ro, "(unsigned long long)gXdLed.reFeedDropped)") &&
             count(s, "(unsigned long long)gXdLed.reFeedDropped)") == 1u); }
    { const std::string ct = body_of(s, "static uint32_t gfxsrc_commit_try(");
      std::string rest = s;
      { const size_t pc = rest.find(ct); if (!ct.empty() && pc != std::string::npos) rest.erase(pc, ct.size()); }
      expect("C12 the gate's dependency reading is taken from the gate's OWN world, in gfxsrc_commit_try",
             !ct.empty() && has(ct, "gXdGateDep.reason") && has(ct, "= n48_dep_check(&dw, &gXdGateDep.detail);") &&
             has(ct, "= gGs.neuteredSubs;") && has(ct, "= dw.observers;") && has(ct, "= dw.source_neuters;"));
      expect("C12 and nothing outside that function WRITES it (the readout only reads)",
             writes_to(rest, "gXdGateDep") == 0u && writes_to(ct, "gXdGateDep") >= 7u);
      expect("C13 and `dep_ok` is still n48_dep_ok's, the one value gfx_dep.h allows",
             count(s, "c.dep_ok = n48_dep_ok(&dw);") == 1u && count(s, "c.dep_ok =") == 1u); }
    // C7 unmapVA and the untranslated passes
    const std::string um = body_of(s, "static uint64_t hook_unmapVA(");
    //: the unmap now passes its OWN range (hook_unmapVA(self, va, size)), so the residency race check can be scoped per VA
    // instead of table-wide. Passing the context alone is what made hp7 refuse 391 of 424 queued copies.
    expect("C7 unmapVA drops the context's entries (switch on)", count(um, "if (gXdDescPort) gfxsrc_desc_unmap(e->seq, va, size);") == 1u);
    expect("C7 and it hands over the unmap's own VA and size", count(s, "gfxsrc_desc_unmap(uint32_t ctxSeq, uint64_t va, uint64_t size)") >= 1u);
    const std::string hk = body_of(s, "static uint64_t hook_gfxCommitIB(");
    expect("C8 both untranslated passes of the source hook move the epoch",
           count(hk, "if (gXdDescPort) __atomic_fetch_add(&gXdDpEpoch, 1u, __ATOMIC_RELAXED);") == 2u);
    const std::string key = body_of(s, "static uint64_t gfxsrc_desc_ctx(");
    expect("C9 a context key only for a LIVE record whose unmapVA we hook", has(key, "gVmCtx[i].state == 1 && gVmCtx[i].vtPatched"));
    // C14-C17 0.0.380: the two ledger-correctness switches. Both OFF by default, each assigned only by its own
    // verb, the unmap's range reaching the ledger, the feed's pages taken from the frame's OWN walk and the ask's from the
    // frame's OWN vm - and neither switch paying anything while it is off.
    expect("C14 both switches are defined once, default 0",
           count(s, "static volatile uint32_t gXdDlExact { 0u };") == 1u &&
           count(s, "static volatile uint32_t gXdDlPgKey { 0u };") == 1u);
    { const size_t b17 = s.find("(arg & 0xffull) == 17ull"), b18 = s.find("(arg & 0xffull) == 18ull");
      const std::string v17 = b17 == std::string::npos ? "" : block_at(s, b17);
      const std::string v18 = b18 == std::string::npos ? "" : block_at(s, b18);
      expect("C14 and each is assigned ONLY by its own `gfxneuter` verb",
             count(s, "gXdDlExact =") == 1u && count(v17, "gXdDlExact =") == 1u &&
             count(s, "gXdDlPgKey =") == 1u && count(v18, "gXdDlPgKey =") == 1u);
      expect("C14 and throwing either moves the ledger's epoch, so no entry outlives the rule it was recorded under",
             count(v17, "__atomic_fetch_add(&gXdDpEpoch, 1u, __ATOMIC_RELAXED);") == 1u &&
             count(v18, "__atomic_fetch_add(&gXdDpEpoch, 1u, __ATOMIC_RELAXED);") == 1u); }
    { const std::string du = body_of(s, "static void gfxsrc_desc_unmap(uint32_t ctxSeq, uint64_t va, uint64_t size) {");
      expect("C15 the unmap hands the ledger its OWN range and the switch, once, and nowhere else in the file",
             count(du, "n48_dl_unmap_rng(&gXdLed, ctxSeq, va, size, gXdDlExact);") == 1u &&
             count(s, "n48_dl_unmap_rng(") == 1u && count(s, "n48_dl_unmap(") == 0u);
      // 0.0.382 — THE CAVEAT THAT DECIDES THE RUN, PINNED IN THE SOURCE.
      expect("C18 with the drain on the locked branch drains instead of applying its own range - one or the other, never both",
             count(du, "if (drainOn) xd_led_drain_locked();") == 1u &&
             count(du, "else n48_dl_unmap_rng(&gXdLed, ctxSeq, va, size, gXdDlExact);") == 1u);
      expect("C18 the deferred branch exists and the epoch is bumped EXACTLY ONCE in the whole function",
             count(du, "} else if (drainOn) {") == 1u &&
             count(du, "__atomic_fetch_add(&gXdDpEpoch, 1u, __ATOMIC_RELAXED);") == 1u);
      const size_t bb = du.find("} else if (drainOn) {");
      const std::string busy = bb == std::string::npos ? "" : block_at(du, bb);
      expect("C18 ★ AND THE DEFERRED BRANCH BUMPS NOTHING - the drain REPLACES the bump, it does not sit beside it",
             !busy.empty() && count(busy, "gXdDpEpoch") == 0u); }
    { const size_t b21 = s.find("(arg & 0xffull) == 21ull");
      const std::string v21 = b21 == std::string::npos ? "" : block_at(s, b21);
      expect("C19 the drain is a switch, defined once and 0 AT BOOT (today's behaviour)",
             count(s, "static volatile uint32_t gXdDlDrain { 0u };") == 1u);
      expect("C19 assigned only by its own verb, which moves the epoch and RE-SEATS the ring mark",
             count(s, "gXdDlDrain =") == 1u && count(v21, "gXdDlDrain =") == 1u &&
             count(v21, "__atomic_fetch_add(&gXdDpEpoch, 1u, __ATOMIC_RELAXED);") == 1u &&
             count(v21, "gXdLedClr = n48_rp_clr_mark(&gXdRpClr);") == 1u); }
    { const std::string dcf = body_of(s, "static uint32_t gfxsrc_decide_frame(");
      const size_t dpos = dcf.find("if (dp && gXdDlDrain) xd_led_drain_locked();");
      const size_t spos = dcf.find("n48_dl_sync(&gXdLed, arm, dpEp);");
      expect("C20 the judged frame drains under gXdLock BEFORE the sync and before anything can ask",
             dpos != std::string::npos && spos != std::string::npos && dpos < spos);
      expect("C20 and the drain helper is the ONLY thing that moves the ledger's ring mark",
             count(s, "n48_dl_drain(") == 1u && count(s, "xd_led_drain_locked();") == 2u); }
    { const std::string dec2 = body_of(s, "static uint32_t gfxsrc_decide_frame(");
      const size_t pf = dec2.find("if (dp && ranPolicy) {");
      const std::string fd = pf == std::string::npos ? "" : block_at(dec2, pf);
      expect("C16 the feed hands the ledger the frame's OWN colour-target walk (hVa/hPage/hHeld), once",
             count(fd, "lf.pg.va = hVa; lf.pg.page = hPage; lf.pg.n = hHeld;") == 1u && count(s, "lf.pg.") == 4u);
      expect("C16 and the frame says whether OUR 8-target cap truncated them, from the frame's own two counts",
             count(fd, "lf.pg.full = (tgtN > hHeld) ? 1u : 0u;") == 1u);
      expect("C16 and those rows are gfxc_page's own answers over this frame's vm",
             has(dec2, "const bool ok = gfxc_page_sub(N48_MMT_D_TGT, vm, it.va & ~0xfffull, page, isSys);") &&
             has(dec2, "hVa[hHeld] = it.va; hPage[hHeld] = ok ? page : 0u;")); }
    { const std::string tk = body_of(s, "static int gfxsrc_desc_tiled_ok(");
      expect("C17 the ask keys the ledger on a page resolved through the ASKING frame's own vm",
             count(tk, "if (gXdDlPgKey && c->vm) {") == 1u &&
             count(tk, "if (!gfxc_page(*c->vm, va & ~0xfffull, askPage, askSys)) askPage = 0;") == 1u &&
             count(tk, "n48_dl_tiled_ok_pg(&gXdLed, c->ctx, va, mode, askPage, gXdDlPgKey)") == 1u);
      // build 0.0.448 item 3: this used to require the un-keyed ask GONE from the whole kext (`count == 0`),
      // when gXdLed was the only n48_dl instance in the file. It is narrowed here to what the property actually
      // protects - gXdLed itself never bypasses the key - because switch 45 adds a SECOND, throwaway n48_dl
      // instance (gXdFrameLocal) that legitimately never uses the physical-page key at all (it is cleared every
      // pass and never crosses a frame, so a re-mapped VA mid-run is not the hazard it is for gXdLed). The
      // un-keyed ask is still gone for gXdLed specifically - the ONE ledger this key was built to protect.
      expect("C17 and the un-keyed ask of gXdLed specifically is gone from the kext, so ITS key cannot be bypassed",
             count(s, "n48_dl_tiled_ok(&gXdLed,") == 0u);
      // PLANTED BREAK ("the narrowing quietly waves through a NEW un-keyed ask of gXdLed"): a mutant source
      // string with a bare `n48_dl_tiled_ok(&gXdLed,` call added elsewhere must still be caught.
      { const std::string mut = s + "\nint x() { return n48_dl_tiled_ok(&gXdLed, 1, 2, 3); }\n";
        expect("C17 BREAK-check: a NEW un-keyed ask of gXdLed anywhere in the file is still caught",
               count(mut, "n48_dl_tiled_ok(&gXdLed,") != 0u); }
      // build 0.0.448 item 3: the ONE place an un-keyed ask of a DIFFERENT ledger is expected - the
      // frame-local list's own consult, pinned again here (group D-FLW already pins its ORDER against gXdLed).
      expect("C17 the frame-local list IS asked un-keyed (by design: it never uses the physical-page key)",
             count(s, "n48_dl_tiled_ok(&gXdFrameLocal,") == 1u); }
    // C21-C24 0.0.391 ( conditions 2, 3, 4 and 6). Four facts that live in the kext's own wiring rather than in
    // any header, so a host test can pin them only here. Each has a planted one-line edit below it in `sm[]`.
    // 0.0.393: C22/C23's loops MOVED to gfx_cp_build.h, so what is pinned here is the kext's CALL
    // to the shared builder; the loops themselves are pinned against the header in `hd` above.
    { const std::string dcf = body_of(s, "static uint32_t gfxsrc_decide_frame(");
      // C21 (condition 3): the 64-item gcap scan cap reaches R5's `complete` PER FRAME. gXdC.scanOverflow is a boot
      // total and per IB, so it cannot answer the question, and asks "scan <= 64 items" of each neutered frame.
      expect("C21 the gcap scan's per-frame overflow is recorded where the scan itself overflows",
             count(dcf, "scanOverFrame = 1u;") == 1u && count(s, "scanOverFrame") == 3u);
      expect("C21 and `complete` asks it, beside the walk, the programs, the dispatch and our own target cap",
             count(dcf, "cf.complete = (walkOk && pgmOk && !scanOverFrame && !cf.has_dispatch && "
                        "cf.ntgt <= N48_CP_TGT_MAX) ? 1u : 0u;") == 1u);
      // C25 (0.0.393,  condition 1): the note site computes the out-of-scope reading from the frame's OWN
      // classified root/pid against WindowServer's, and nothing else writes `out_of_scope`. 0.0.395 adds ONE reader:
      // the R5′ record CARRIES the same reading (`rf.out_of_scope = cf.out_of_scope;`) so the two rules are scoped by
      // the one positive reading. Both are in the decide path; no other writer exists.
      expect("C25 the note records a POSITIVE out-of-scope reading from the frame's own root/pid",
             count(dcf, "cf.out_of_scope = n48_cp_scope_out(") == 1u &&
             count(dcf, ".out_of_scope = ") == 2u);
    }
    { // C22 (condition 4): the consumer list is built by ONE shared function, called by the kext and compiled into the
      // host suite. Through 0.0.392 a mirror lived in gfx_dep_test.cpp and drifted blind outside four pinned strings.
      expect("C22 the kext CALLS the shared consumer builder once",
             count(s, "n48_cp_build_consumer(&gXpIn, &ds);") == 1u);
      expect("C22 the consumer carries the VERTEX ABI pointer pages as well as the fragment's",
             count(hd, "c->ptr[c->nptr++] = ds->in_vptr[q];") == 1u &&
             count(hd, "c->ptr[c->nptr++] = ds->in_ptr[q];") == 1u &&
             count(hd, "c->ptr[c->nptr++] = fixed[q];") == 1u);
      expect("C22 and an UNKNOWN vertex ABI row refuses exactly as an unknown fragment one does",
             count(hd, "!ds->in_ptr_known || !ds->in_vptr_known") == 1u);
      // C23 (condition 2): the inherited count is carried SEPARATELY, so it cannot answer as `list-overflow`.
      // 0.0.438 (FINDING 4 INTERIM REFUSAL): the `over` expression pin is updated - it gained a
      // `|| ds->draws > 1u` clause (a multi-draw segment's consumer is also incomplete, same reasoning).
      expect("C23 an inherited declared pointer is carried under its own name, not folded into `over`",
             count(hd, "c->ptr_inherit = ds->in_ptr_inherit;") == 1u &&
             count(hd, "c->over = (ds->in_over || ds->in_n > N48_CP_IN_MAX || !ds->in_ptr_known || "
                       "!ds->in_vptr_known ||\n               ds->draws > 1u) ? 1u : 0u;") == 1u); }
    // C24 (condition 6): the ONE-SHOT SPENT line no longer asserts "NO OTHER FRAME can be handed COMMIT" - a sentence
    // that is true at budget 1 and false at any budget above it (n48_cm_shot_spend leaves the state ARMED while
    // spent < budget). It READS the budget and says which of the two worlds this boot is in.
    expect("C24 the ONE-SHOT SPENT line reads the budget instead of asserting one value of it",
           count(s, "bud = n48_cm_shot_budget_of(&gXdShot), left = n48_cm_shot_left(&gXdShot);") == 1u &&
           count(s, "HWLOG(N48_CM_SPENT_FMT,") == 1u &&
           count(s, "left ? N48_CM_SPENT_MORE : N48_CM_SPENT_DONE);") == 1u);
    expect("C24 and the unconditional claim is GONE from the file",
           count(s, "NO OTHER FRAME can be handed COMMIT") == 0u);
    // C26 0.0.394: KEEP ACROSS A COVERING UNMAP. Off by default, assigned only by its own `gfxneuter`
    // verb, which mirrors it into the ledger (`n48_dl.keep`) and moves the epoch so no entry crosses the rule change. The
    // report line names the three new counters. OFF, the ledger never reads `keep` and its answers are 0.0.393's.
    { const size_t b29 = s.find("(arg & 0xffull) == 29ull");
      const std::string v29 = b29 == std::string::npos ? "" : block_at(s, b29);
      expect("C26 the keep-across-unmap switch is defined once, default 0",
             count(s, "static volatile uint32_t gXdDlKeep { 0u };") == 1u);
      expect("C26 and assigned ONLY by its own verb, which mirrors it into the ledger and moves the epoch",
             count(s, "gXdDlKeep =") == 1u && count(v29, "gXdDlKeep =") == 1u &&
             count(v29, "gXdLed.keep = gXdDlKeep;") == 1u &&
             count(v29, "__atomic_fetch_add(&gXdDpEpoch, 1u, __ATOMIC_RELAXED);") == 1u);
      expect("C26 and the report names the three new counters",
             count(v29, "keptAcross") == 1u && count(v29, "provenRemap") == 1u && count(v29, "movedRemap") == 1u);
      expect("C26 and the switch line says the kept entry's own name",
             count(v29, "keep-across-unmap") == 1u); }
    // C27 0.0.398/0.0.399/0.0.400: THE FILL-COLOUR RETARGET. Off by default and off at boot,
    // assigned only by its own `gfxneuter 31` verb; the 16-byte float4 is placed in the relocation arena and NOTHING
    // else is written; the translator flag is set ONLY under the switch. 0.0.399 added a THREE-clause gate.
    // 0.0.400 DROPS its root clause: the keystone writes root[511] AFTER translation, for the frame it
    // commits, so the clause refused on every fresh boot and would have refused the fill itself - the gate keeps
    // ring-built and vaBase only. G2 keeps a root READ on this path and requires `!e->withdrawn`, but as a reading
    // (`rootLive`), not a refusal. F3 splits the report into two lines, each measured under the logger's cap. Every
    // clause, the root read, and both gate call sites have a planted one-line edit in `sm[]` below.
    { const size_t b31 = s.find("(arg & 0xffull) == 31ull");
      const std::string v31 = b31 == std::string::npos ? "" : block_at(s, b31);
      expect("C27 the fill-colour switch is defined once, default 0",
             count(s, "static volatile uint32_t gXdFillColorOn { 0u };") == 1u);
      expect("C27 and assigned ONLY by its own verb",
             count(s, "gXdFillColorOn =") == 5u && count(v31, "gXdFillColorOn =") == 5u);   // build 0.0.522 (M3b): + the spill-busy refusal (= 0u)
      expect("C27 the buffer is placed in the relocation arena and its VA is the ring region's own",
             has(v31, "gfxsrc_fillcolor_place()") &&
             has(s, "n48_reloc_place(&gReloc, gRingMap.vaBase") && has(s, "n48_reloc_va(gRingMap.vaBase, off)") &&
             count(s, "kN48FillColorKey") >= 2u);
      { const std::string ph = body_of(s, "gfxsrc_fillcolor_place");
        expect("C27 the placement writes ONLY our own bytes, through the arena writer",
               !ph.empty() && count(ph, "gfxsrc_reloc_write(") == 1u &&
               !has(ph, "navi48_reg_write") && !has(ph, "navi48_vram_write_mm") && !has(ph, "navi48_vram_read_mm")); }
      // G1: the gate has the TWO surviving clauses, in order, and NO_ROOT is gone from the gate and the whole file.
      const std::string gb = body_of(s, "static uint32_t gfxsrc_fillcolor_gate(");
      expect("C27 G1 the gate exists with the TWO surviving clauses, ring-built then vaBase",
             has(s, "gfxsrc_fillcolor_gate(uint64_t boundCtx)") &&
             has(s, "if (!gRingMap.built || !gRingMap.vaBase) return N48_FILL_GATE_NO_RING;") &&
             has(s, "if (gReloc.vaBase && gReloc.vaBase != gRingMap.vaBase) return N48_FILL_GATE_VA_MOVED;"));
      expect("C27 G1 the gate has exactly two refusal returns and no third clause",
             !gb.empty() && count(gb, "return N48_FILL_GATE_NO_RING;") == 1u &&
             count(gb, "return N48_FILL_GATE_VA_MOVED;") == 1u &&
             count(gb, "return N48_FILL_GATE_OK;") == 1u && count(gb, "return N48_FILL_GATE_") == 3u);
      expect("C27 G1 NO_ROOT is dropped from the gate and named NOWHERE in the file",
             count(s, "N48_FILL_GATE_NO_ROOT") == 0u &&
             count(s, "gfxsrc_fillcolor_root_written(boundCtx)) return") == 0u);
      { const std::string pol1 = body_of(s, "static void gfxsrc_policy(");
        expect("C27 G1 the VERB gate and the POLICY gate both have no NO_ROOT clause",
               !has(v31, "NO_ROOT") && !pol1.empty() && !has(pol1, "NO_ROOT")); }
      // G2: the one root-record read on this path requires the record be un-withdrawn, and it is a reading.
      expect("C27 G2 the root read on this path requires the record NOT be withdrawn",
             has(s, "e->state == 1u && e->rootWritten != 0 && !e->withdrawn"));
      expect("C27 G2 and it is a READING the gate counts, refusing nothing on it",
             has(gb, "gXdFillColorS.rootLive += gfxsrc_fillcolor_root_written(boundCtx);"));
      expect("C27 the VERB consults the gate for the bound context and keeps the switch OFF on refusal",
             has(v31, "gfxsrc_fillcolor_gate(gfxsrc_fillcolor_bound_ctx())") &&
             has(v31, "gXdFillColorS.gateRefused[gate]++") && has(v31, "gXdFillColorOn = 0u; st = 5;"));
      { const std::string pol2 = body_of(s, "static void gfxsrc_policy(");
        expect("C27 the POLICY re-asks the gate against THIS frame's own binding and counts its clause",
               has(pol2, "gfxsrc_fillcolor_gate(boundCtx)") && has(pol2, "gXdFillColorS.gateRefused[fgate]++")); }
      expect("C27 the policy is handed the frame's own WindowServer binding (and, 0.0.402, its BOUND flag)",
             has(s, "wf->wsState == N48_WS_BOUND) ? wf->wsCtx : 0ull;") &&
             has(s, "gfxsrc_policy(vm, &f, f.ib[0].got, ib0Va, dp, dkey, arm, boundCtx, wsBound);"));
      expect("C27 the translator flag is set only under the switch, and handed the arena's own VA",
             count(s, "XLAT12_EXTRA_FILL_COLOR") == 1u && count(s, "ex.fill_color_va =") == 1u &&
             has(s, "if (gXdFillColorOn) {") &&
             has(s, "ex.flags |= XLAT12_EXTRA_FILL_COLOR;") &&
             has(s, "ex.fill_color_va = n48_reloc_va(gRingMap.vaBase, foff);"));
      // F3: the report is TWO lines, and both are measured with the widest possible numerics. The formats are read out of
      // the kext source itself, so a lengthened line fails here rather than silently tail-truncating in the logger.
      const std::string l1 = collect_printf_fmt(s, s.find("HWLOG(\"fillcolor:"));
      const std::string l2 = collect_printf_fmt(s, s.find("HWLOG(\"fillcolor counts:"));
      expect("C27 F3 the report is TWO lines and both are present", !l1.empty() && !l2.empty());
      char b1[2048], b2[2048];
      const int w1 = l1.empty() ? -1 : std::snprintf(b1, sizeof b1, l1.c_str(), "OFF (default)",
                            " - CHANGED BY THIS VERB", 0xffffffffffffffffull, 0xffffffffu, 0xffffffffu,
                            0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, "key-placed-at-another-size");
      const int w2 = l2.empty() ? -1 : std::snprintf(b2, sizeof b2, l2.c_str(), 0xffffffffffffffffull,
                            0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffu, 0xffffffffu, 0xffffffffu,
                            0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull);
      if (!gQuiet) std::printf("      fillcolor400 line 1 worst case: %d bytes; line 2: %d bytes (cap %u)\n",
                               w1, w2, N48_LOG_CAP_BODY);
      expect("C27 F3 both fillcolor lines fit under 480 body bytes at widest numerics",
             w1 > 0 && w2 > 0 && w1 < 480 && w2 < 480 && (unsigned)w1 <= N48_LOG_CAP_BODY && (unsigned)w2 <= N48_LOG_CAP_BODY);
      expect("C27 the report names fillcolor, retargeted, old/new, the gate's two clauses, the root reading, the colour",
             has(v31, "fillcolor:") && count(v31, "retargeted %llu") == 1u &&
             has(v31, "old %#llx new %#llx") && has(v31, "colour (%u %u %u %u)") &&
             has(v31, "gate refused ring %llu va-moved %llu") && has(v31, "root live %llu")); }
    // C28 0.0.402 (a/b and's BINDING FIXES H1-H7): THE IDENTITY LUT. Off by default and off at
    // boot, assigned only by its own `gfxneuter 32` verb; the learn is gated on the COMMIT arm, the BOUND WindowServer
    // context and the two-surface plane shape (H2) and reads the slot-4 record ONCE per arm scope; every page of the
    // extent is resolved UNDER gXdLock into a fixed table (H3) and the deferred kernel_thread_start thread writes VRAM
    // ONLY from that table (H3) — never walking a page table itself — generating the ramp into a 64-dword batch buffer
    // inside the batch loop (H1), with a read-back and an HDP flush before `ready`. Every clause has a planted edit.
    { const size_t b32 = s.find("(arg & 0xffull) == 32ull");
      const std::string v32 = b32 == std::string::npos ? "" : block_at(s, b32);
      const std::string thr = body_of(s, "static void lutfill_thread(");
      const std::string lrn = body_of(s, "static void gfxsrc_lut_learn(");
      const std::string npb = body_of(s, "static void gfxsrc_lut_note_producer(");
      const std::string pol3 = body_of(s, "static void gfxsrc_policy(");
      expect("C28 the LUT switch is defined once, default 0 (OFF AT BOOT)",
             count(s, "gLutOn     { 0u };") == 1u &&
             count(s, "gLutReady  { 0u };") == 1u &&
             count(s, "gLutKicked { 0u };") == 1u);
      expect("C28 and gLutOn is assigned ONLY by its own verb (M 1 on, M 0xFF off)",
             count(s, "gLutOn =") == 2u && count(v32, "gLutOn =") == 2u);
      expect("C28 the switch verb reads M 1 / M 0xFF and refuses anything else",
             has(v32, "m == 0xFFu") && has(v32, "m == 1u") && has(v32, "st = 5;"));
      expect("C28 the ONE report line is `lutfill:` and lives in the verb",
             has(v32, "lutfill_report_line(") && count(s, "lutfill_report_line(") == 2u);
      expect("C28 no per-frame line: the learn site logs NOTHING",
             !lrn.empty() && !has(lrn, "HWLOG(") && !has(lrn, "N48LOG("));
      expect("C28 the held-back HEADLESS producer's CB0 is recorded at the decide path's note site, and logs NOTHING",
             count(s, "if (gLutOn) gfxsrc_lut_note_producer(gXdBuild.kind, hVa, hHeld);") == 1u &&
             !npb.empty() && !has(npb, "HWLOG(") && !has(npb, "N48LOG("));
      expect("C28 and the record's base is CROSS-CHECKED against it - a disagreement writes nothing",
             has(lrn, "if (gLutProdSeen && gLutProdVa != d.va) { gLutD.decodeRefused++; gLutD.lastWhy = N48_LUT_MISMATCH; return; }"));
      // build 0.0.553: the entry is li111_learn_off() - N48_LUT_SLOT4_OFF (entry 4, the old read)
      // unless 111 ON selected the draw's own entry (gfx_lutidx111.h n48_li_learn_off; tests/gfx_lutfill_test.cpp section 111).
      expect("C28 the LUT VA + extent come from the image-table record (entry 4 unless 111 ON), read through the frame's own page table",
             !lrn.empty() &&
             has(lrn, "gfxc_read(vm, imgVa + li111_learn_off(), rec, N48_LUT_RECORD_DWORDS, nullptr)") &&
             has(s, "static inline uint64_t li111_learn_off() { return n48_li_learn_off(&gLiSel); }") &&
             has(lrn, "n48_lut_decode_record(rec, &d)"));
      // H2 (0.0.402): the learn gate — the COMMIT arm, the BOUND WindowServer binding, and the translator's OWN plane
      // shape — and one record read per arm scope (`gLutAttempted`).
      expect("C28 H2 the learn is gated on the COMMIT arm AND the BOUND WindowServer context AND the plane shape",
             has(pol3, "if (arm == N48_SD_ARM_COMMIT && wsBound && n48_lut_plane_shape(ds.in_mode, ds.in_n) &&") &&
             has(s, "const uint32_t wsBound = (wf && wf->asked && wf->wsState == N48_WS_BOUND) ? 1u : 0u;") &&
             has(s, "gfxsrc_policy(vm, &f, f.ib[0].got, ib0Va, dp, dkey, arm, boundCtx, wsBound);"));
      expect("C28 H2 and the learn reads the record at most ONCE per arm scope (a per-scope attempted flag)",
             !lrn.empty() && has(lrn, "if (!gLutOn || gLutHave || gLutAttempted || !imgVa || !vm.ok) return;") &&
             has(lrn, "gLutAttempted = 1u;"));
      // 0.0.435: `&& !cgRefused` added deliberately - a segment the copy-overlap check refused must
      // never learn the LUT from bytes the restore is about to discard (Part 2's own binding: "the LUT learn
      // skipped" for that segment). Re-baselined the same way as C2 above.
      expect("C28 the record is learned only while the switch is on, and the frame handed the learn is the policy's own",
             has(pol3, "if (gLutOn && ds.in_abi && !cgRefused) {") && has(pol3, "gfxsrc_lut_learn(vm, ds.in_img_va);"));
      expect("C28 a frame that samples the learned VA is marked a plane candidate, from the translator's own list",
             has(pol3, "if (ds.in_va[q] == gLutLearnedVa) { gXdBuild.lutPlane = 1u; break; }"));
      // H4 (0.0.402): the instrument's ONE shape is applied after the decode, before anything is resolved or written.
      expect("C28 H4 the decoded record must pass the instrument's OWN shape gate before it is used",
             has(lrn, "if (why == N48_LUT_OK) why = n48_lut_instrument_ok(&d);"));
      // H3 (0.0.402): the LEARN resolves every page under gXdLock into the fixed table; the THREAD only reads the table.
      expect("C28 H3 the learn resolves EVERY page under gXdLock with the file-local gfxc_page into the fixed table",
             !lrn.empty() && has(lrn, "gfxc_page(vm, va, page, isSys)") &&
             has(lrn, "vmib_to_vram_off(page, vm.fbStart") &&
             has(lrn, "navi48_vram_apple_dest_check(voff, pbytes)") &&
             has(lrn, "for (uint32_t p = 0u; p < np; p++) gLutTable[p] = tbl[p];"));
      expect("C28 H3 the table is fixed at 48 offsets and the learn refuses an extent that does not fit it",
             has(s, "static uint64_t     gLutTable[N48_LUT_TABLE_PAGES]") &&
             has(lrn, "if (np == 0u || np > N48_LUT_TABLE_PAGES)"));
      expect("C28 H3 the thread writes ONLY from the table and NEVER walks a page table itself",
             !thr.empty() && has(thr, "const uint64_t voff = gLutTable[p];") &&
             !has(thr, "gfxc_page") && !has(thr, "vmib_to_vram_off") && !has(thr, "navi48_vram_apple_dest_check") &&
             !has(thr, "gfxc_read"));
      expect("C28 H3 and a table whose arm scope moved is refused as stale (checked before the write AND before ready)",
             !thr.empty() &&
             count(thr, "if (scope != gLutScope) { gLutD.stale++; gLutD.lastWhy = N48_LUT_STALE; return; }") == 2u);
      // H1 (0.0.402): the ramp is generated into the 64-dword batch buffer INSIDE the batch loop, never a page at a time.
      expect("C28 H1 the ramp is generated into a 64-dword batch buffer inside the batch loop (no page on the stack)",
             !thr.empty() && has(thr, "uint32_t src[N48_LUT_BATCH_DWORDS], back[N48_LUT_BATCH_DWORDS];") &&
             has(thr, "n48_lut_batch_page(&d, p, off, take, src)") &&
             !has(thr, "src[1024]") && !has(thr, "uint32_t src[64]"));
      expect("C28 the frame carries the rung's three flags from the same globals",
             has(s, "c.lut_switch = gLutOn ? 1u : 0u;") && has(s, "c.lut_plane  = gXdBuild.lutPlane;") &&
             has(s, "c.lut_ready  = gLutReady ? 1u : 0u;"));
      expect("C28 the thread is a plain kernel_thread_start, started at most once per arm scope by the CAS",
             has(lrn, "kernel_thread_start(&lutfill_thread, nullptr, &th)") &&
             has(lrn, "__sync_bool_compare_and_swap(&gLutKicked, 0u, 1u)"));
      expect("C28 and a new arm scope re-arms exactly one attempt",
             count(s, "gLutKicked =") == 3u);
      // H5 (0.0.402): gLutHave is PER ARM SCOPE — cleared at the arm and at a fresh ON — and a failed start clears the kick.
      expect("C28 H5 the per-scope learn state is cleared at BOTH the arm and a fresh ON (gLutHave is live, not sticky)",
             // build 0.0.550: a THIRD `gLutAttempted = 0u;` is switch 107's retry, in lr107_learn only (tests/gfx_commit_test.cpp 107 R7)
             count(s, "gLutHave = 0u;") == 2u && count(s, "gLutAttempted = 0u;") == 3u &&
             count(s, "gLutTableN = 0u;") == 2u && count(s, "gLutScope++;") == 2u);
      expect("C28 H5 a failed kernel_thread_start clears the kick so the next scope may retry",
             has(lrn, "if (kr != KERN_SUCCESS) { gLutD.kickRefused++; gLutKicked = 0u; return; }"));
      expect("C28 the thread takes NO lock a gXdLock holder takes (b's lock order)",
             !thr.empty() && !has(thr, "gXdLock") && !has(thr, "IOLock"));
      expect("C28 and it writes VRAM only, through the writer/reader, and nothing else",
             !thr.empty() && count(thr, "navi48_vram_write_mm(") == 1u && count(thr, "navi48_vram_read_mm(") == 1u &&
             !has(thr, "gfxc_write_sys") && !has(thr, "navi48_reg_write"));
      expect("C28 FAIL-CLOSED: `ready` is set exactly once, only after the HDP flush, and nowhere else in the file",
             count(s, "gLutReady =") == 1u && !thr.empty() &&
             thr.find("navi48_hdp_flush_now()") != std::string::npos &&
             thr.find("gLutReady = 1u;") != std::string::npos &&
             thr.find("navi48_hdp_flush_now()") < thr.find("gLutReady = 1u;"));
      expect("C28 the learn site is called UNDER gXdLock but writes nothing (the write is the thread's)",
             !lrn.empty() && !has(lrn, "navi48_vram_write_mm") && !has(lrn, "navi48_hdp_flush_now"));
      // 0.0.402 : the three display-pipe offsets must appear NOWHERE in the thread or the verb.
      // The check itself assembles the forbidden tokens from parts, so that the literals it forbids appear nowhere in
      // THIS file either — the rule is the absence, and the assertion must not be the one place that breaks it.
      { const std::string pfx = "0x2" "8";        // the three tokens are pfx+"0", pfx+"2", and the "0x2" "9" family
        const std::string f0 = pfx + "0", f2 = pfx + "2", f9 = std::string("0x2" "9") + "9";
        expect("C28 the thread and the verb NEVER touch the display pipe's forbidden offsets",
               !thr.empty() && !has(thr, f0.c_str()) && !has(thr, f2.c_str()) && !has(thr, f9.c_str()) &&
               !has(v32, f0.c_str()) && !has(v32, f2.c_str()) && !has(v32, f9.c_str())); }
      expect("C28 and it never touches the `gfxneuter 15` (forgiveness) or `19` (running budget) switches",
             !has(thr, "gXdForgive") && !has(thr, "gXdRunBudget") && !has(lrn, "gXdForgive") && !has(lrn, "gXdRunBudget")); }
    // C29 0.0.404/0.0.405 (a items 1-7,b,) — THE FIRST-SHOT FILL-SET RESERVATION. Off by default
    // and off at boot, assigned only by its own `gfxneuter 33` verb; the ARM opens the window and the disarm / shot-finish
    // close it; the COMMIT path steps the pure rule once per judged frame from the translator's own fragment identity and the
    // frame's own CB0, made STRICT in 0.0.405 (: one segment AND the ColorFill PS AND a member CB0); the policy
    // divisor is bypassed only for a frame IDENTIFIED AS A MEMBER FILL, whose flag is computed in the gather
    // before the divisor; a reserved fill is recorded only when the gate answered OK AND the shot was spent. The appended
    // gate rung itself is gfx_commit.h's, pinned in gfx_commit_test.cpp. Every clause has a planted edit in `sm[]` below.
    { const size_t b33 = s.find("(arg & 0xffull) == 33ull");
      const std::string v33 = b33 == std::string::npos ? "" : block_at(s, b33);
      const std::string rep = body_of(s, "static void fillset_report_line(");
      const std::string idf = body_of(s, "static int gfxsrc_id_is_fill(");
      const std::string ct  = body_of(s, "static uint32_t gfxsrc_commit_try(");
      const std::string res = body_of(s, "static int gfxsrc_pgm_profile(");
      const std::string idp = body_of(s, "static void gfxsrc_identify_pgm(");
      const std::string dec = body_of(s, "static uint32_t gfxsrc_decide_frame(");
      expect("C29 the fill-set switch is defined once, default 0 (OFF AT BOOT)",
             count(s, "static n48_fs gFs {};") == 1u);
      expect("C29 and gFs.on is assigned ONLY by its own verb (M 1 on, M 0xFF off)",
             count(s, "gFs.on =") == 2u && count(v33, "gFs.on =") == 2u);
      expect("C29 the switch verb reads M 1 / M 0xFF and refuses anything else",
             has(v33, "m == 0xFFu") && has(v33, "m == 1u") && has(v33, "st = 5;"));
      // 0.0.420 (P1): the arm opens BOTH windows through the ONE n48_fs_open call now (its condition names 35 too), and
      // the second window's own verb (35) prints the SAME report line - so the count below is the definition + 33 + 35.
      expect("C29 the window is OPENED by the arm and CLOSED by the disarm, the cancellation and the shot's finish",
             count(s, "n48_fs_open(&gFs)") == 1u &&
             has(s, "if (gFs.on || gFs.plane_on) n48_fs_open(&gFs);") &&
             count(s, "n48_fs_clear(&gFs);") == 4u);
      expect("C29 the ONE report line is `fillset:` and lives in the fill/window verbs (NO per-frame line)",
             has(v33, "fillset_report_line(") && count(s, "fillset_report_line(") == 3u &&
             count(s, "\"fillset:") == 0u);   // the format itself lives in gfx_fillset.h, measured by its own suite
      expect("C29 the report is the ONLY fillset logger: the step site and the commit log NOTHING",
             has(rep, "HWLOG(") && !has(ct, "fillset_report_line(") && !has(rep, "n48_fs_step("));
      expect("C29 the identity is the translator's OWN in-force fragment id, matched by NAME (copied from 0.0.398)",
             has(res, "gfxsrc_id_is_fill((int)out->ps_id)") && has(idf, "ws_B_ColorFill") &&
             !has(idf, "kXlat12ShaderIds"));
      expect("C29 and the identity is read ONLY while the switch is on",
             has(res, "if (gFs.on && stage == 0u && gfxsrc_id_is_fill((int)out->ps_id)) gXdBuild.fill = 1u;") &&
             count(s, "gXdBuild.fill = 1u;") == 1u);
      expect("C29 the per-frame flag is cleared on EVERY frame (the policy's reset and decide_frame's)",
             count(s, "gXdBuild.fill = 0u;") == 2u);
      expect("C29 the COMMIT path steps the pure rule once per judged frame from its own verdict, identity and CB0",
             // build 0.0.530 (SRCFILL85.md item 8): through the pure dispatcher n48_fs85_frame, which calls
             // n48_fs_identify_fill / n48_fs_step / n48_fs_plane_step verbatim with switch 85 not in force (gfx_fs85_test).
             has(ct, "n48_fs85_frame(&gFs, &gFs85, fs85On, fsActive, fpActive, fsEligible, gXdBuild.nsegPre, gXdBuild.fill, gXdBuild.plane, tgtVa,") &&
             has(ct, "const uint32_t fsStep = fs85F.fs_step;") && count(s, "n48_fs85_frame(") == 1u &&
             has(ct, "const uint32_t fsEligible = (verdict == N48_XV_TRANSLATE) ? 1u : 0u;"));
      expect("C29 K2 (0.0.405): the identity is the STRICT one - one segment AND the ColorFill PS AND an uncommitted member",
             has(ct, "n48_fs85_frame(&gFs, &gFs85, fs85On, fsActive, fpActive, fsEligible, gXdBuild.nsegPre, gXdBuild.fill, gXdBuild.plane, tgtVa,") &&
             count(s, "n48_fs_identify_fill(") == 0u);   // 0.0.530: only the dispatcher calls it
      expect("C29 and it runs only while the arm is AT COMMIT (a's `while ON and the arm is at COMMIT`)",
             has(ct, "const uint32_t fsActive = (gFs.on && arm == N48_SD_ARM_COMMIT) ? 1u : 0u;"));
      expect("C29 and hands the gate the rung's three flags from the SAME globals",
             has(ct, "c.fs_switch  = fsActive;") &&
             has(ct, "c.fs_open    = fsActive ? (n48_fs_win_open(&gFs) ? 1u : 0u) : 0u;") &&
             has(ct, "c.fs_reserve = (fsStep == N48_FS_RESERVE) ? 1u : 0u;"));
      expect("C29 a reserved fill is RECORDED at the ONE instant the gate answered OK AND the shot was spent",
             has(ct, "if (c.fs_reserve) { gFsPend.active = 1u; gFsPend.cb0 = tgtVa; }"));
      expect("C29 K2 (0.0.410,): the GATE does not COMMIT - the member is not counted here",
             !has(ct, "n48_fs_commit(") && !has(ct, "n48_fs85_commit("));
      expect("C29 K1 (0.0.405): a reserved fill BYPASSES THE POLICY DIVISOR, and the condition NAMES the identified-fill flag",
             has(dec, "!(arm == N48_SD_ARM_COMMIT && n48_fs_bypass_divisor(&gFs, gXdFrameFill, tgtVa))") &&
             count(s, "n48_fs_bypass_divisor(") == 1u);
      expect("C29 K1: that flag is computed in the GATHER, switch-gated, from THIS frame's own fragment program",
             // build 0.0.515 D1: switch-gated AND divisor-gated (n48_fs_fill_read_wanted: gFs.on, stage 0,
             // flag not yet set, gXdPolicyEvery > 1 - the only setting under which the flag's one reader can act)
             has(idp, "if (n48_fs_fill_read_wanted(gFs.on, stage, gXdFrameFill, gXdPolicyEvery)) {") &&
             has(idp, "const int fid = xlat12_shader_id_match(0u, gXdPgm, gotf);") &&
             has(idp, "if (fid >= 0 && gfxsrc_id_is_fill(fid)) gXdFrameFill = 1u;") &&
             count(s, "gXdFrameFill = 1u;") == 1u);
      expect("C29 K1: and cleared once per frame, BEFORE the gather, so a stale flag cannot describe a new frame",
             count(s, "gXdFrameFill = 0u;") == 1u && has(dec, "gXdFrameFill = 0u;"));
      { const std::string pfx = "0x2" "8";        // the three forbidden display-pipe offsets, assembled from parts
        const std::string f0 = pfx + "0", f2 = pfx + "2", f9 = std::string("0x2" "9") + "9";
        expect("C29 the reservation NEVER touches the display pipe's forbidden offsets",
               !v33.empty() && !has(v33, f0.c_str()) && !has(v33, f2.c_str()) && !has(v33, f9.c_str()) &&
               !has(rep, f0.c_str()) && !has(rep, f2.c_str()) && !has(rep, f9.c_str())); }
      expect("C29 and it never touches the `gfxneuter 15` (forgiveness) or `19` (running budget) switches",
             !has(v33, "gXdForgive") && !has(v33, "gXdRunBudget") && !has(rep, "gXdForgive") && !has(rep, "gXdRunBudget")); }
    // C30 0.0.406 — THE INPUT-FREE FILL AND THE RETIREMENT, pinned WHERE THEY ARE WIRED. L1: the policy
    // builds the input-free consumer from THIS frame's own translator export (no descriptor table, no image read, nseg 1,
    // the ColorFill PS, and the PS_2/3 pointer the retarget NAMED), and gfxsrc_cprov_eval judges it with R3's hazard
    // question and nothing else. N1 (0.0.408,): a RESERVE frame that did NOT go live because the DEPENDENCY failed
    // (`!c.dep_ok`) is RETIRED, and the dependency's own reason is recorded for the report line. Every clause has a planted
    // edit in `sm[]` below; the ones the strings here name are exactly the ones those edits break.
    { const std::string cprov = body_of(s, "static void gfxsrc_cprov_eval(");
      const std::string pol   = body_of(s, "static void gfxsrc_policy(");
      const std::string ct30  = body_of(s, "static uint32_t gfxsrc_commit_try(");
      expect("C30 L1 the kext USES the shared input-free builder (no second copy)",
             count(s, "n48_cp_build_input_free(") == 1u && !cprov.empty() && !pol.empty());
      expect("C30 L1 the builder is called in the POLICY, with this frame's own facts, after the segment loop",
             has(pol, "n48_cp_build_input_free(&fi, gXdBuild.nsegPre, gXdBuild.fill, gXdFillHasTable, gXdFillNImg, gXdFillPtrVa,") &&
             count(pol, "gfxsrc_fillcolor_root_written(boundCtx)") == 1u);
      // 0.0.438 (FINDING 4 INTERIM REFUSAL): the call site pin is updated - a `, gXdBuild.segDraws`
      // 8th argument now sits between the root-written read and the closing parens (see gfx_cp_build.h's own
      // comment on why: `draws != 1u` refuses exactly as the sixth condition does).
      expect("C30 M1 (0.0.407,): the builder's SIXTH condition names the root-written read (its page cannot resolve yet)",
             has(pol, "gfxsrc_fillcolor_root_written(boundCtx), gXdBuild.segDraws))"));
      expect("C30 L1 the pointer VA is recorded ONLY from the retarget (our arena), never Apple's original",
             has(pol, "if (ds.fill_color_retargeted) gXdFillPtrVa = ds.fill_color_new;"));
      expect("C30 L1 no descriptor table and no image read are read from the translator's own export",
             has(pol, "gXdFillHasTable = (ds.in_tbl_va != 0ull) ? 1u : 0u;") && has(pol, "gXdFillNImg = ds.in_n;"));
      expect("C30 L1 the build needs switch 28's machinery AND switch 33 (OFF stays 0.0.403's path)",
             has(pol, "if (gXpOn && gFs.on && gXdBuild.fill) {"));
      expect("C30 L1 cprov judges an input_free consumer with R3's hazard question and nothing else",
             has(cprov, "if (cc.input_free) {") &&
             has(cprov, "clause = n48_cp_eval_fill_hz(&cc, &gR5Ring, &unproven, &stale);"));
      expect("C30 L1 the fill evaluator is a SEPARATE function (the two reviewed evaluators are untouched)",
             count(s, "n48_cp_eval_fill_hz(") == 1u);
      expect("C30 N1 (0.0.408,) the kext RETIRES a reserved fill that did NOT go live because the DEPENDENCY failed",
             has(ct30, "if (c.fs_reserve && !live) (void)n48_fs85_retire(&gFs, &gFs85, tgtVa, live ? 1u : 0u, !c.dep_ok, "
                       "(uint32_t)gXdGateDep.reason, reason);") &&
             count(s, "n48_fs85_retire(") == 1u && count(s, "n48_fs_retire(") == 0u);
      { const std::string rep30 = body_of(s, "static void fillset_report_line(");
        expect("C30 N1 (0.0.408,): the report line prints the retiring DEPENDENCY reason",
               has(rep30, "n48_dep_reason_name(gFs.last_retire_reason)")); }
      expect("C30 L2 the RETIRE path is still the complement of the commit (a committed fill is not retired)",
             count(s, "n48_fs85_retire(") == 1u);
      expect("C30 L1/L2 neither path touches the `gfxneuter 15` (forgiveness) or `19` (running budget) switches",
             !has(cprov, "gXdForgive") && !has(cprov, "gXdRunBudget") && !has(ct30, "gXdForgive") &&
             !has(ct30, "gXdRunBudget")); }
    // C31 0.0.410 — THE FILL-SET COMMIT MOVED TO THE TRANSLATED POINT, AND THE TGTSAMPLE SLOTS START AT
    // THE SECOND COMMIT UNDER SWITCH 33. K2: the gate RECORDS a reserved member (`gFsPend`) and the keystone's own
    // `if (tokMatch && ksOk)` branch calls n48_fs_commit - so a member whose commit the keystone WITHDRAWS stays
    // uncommitted, and the string `n48_fs_commit(` appears exactly once in the whole file. K3: the second slot is offered
    // while switch 33 is on and the BEFORE is skipped until at least one commit has reached the ring.
    { const std::string hook = body_of(s, "static uint64_t hook_gfxCommitIB(");
      const size_t pKsOk = hook.find("if (tokMatch && ksOk) {");
      const std::string ksOk = pKsOk == std::string::npos ? "" : block_at(hook, pKsOk);
      expect("C31 K2 n48_fs85_commit (n48_fs_commit with 85 not in force) is called exactly once in the kext",
             count(s, "n48_fs85_commit(") == 1u && count(s, "n48_fs_commit(") == 0u);
      expect("C31 K2 ...and it is called in the KEYSTONE's committed branch, not at the gate",
             !ksOk.empty() && has(ksOk, "n48_fs85_commit(") &&
             has(ksOk, "if (gFsPend.active) { (void)n48_fs85_commit(&gFs, &gFs85, gFsPend.cb0, seq); gFsPend.active = 0u; }"));
      expect("C31 K2 the keystone's WITHDRAWN branch clears the intent (the member is not committed)",
             has(hook, "if (tokMatch && !ksOk) {") && has(hook, "gFsPend.active = 0u;"));
      expect("C31 K2 and the intent is cleared on EVERY exit of the hook (no stale carry)",
             count(hook, "gFsPend.active = 0u;") == 4u);
      expect("C31 K2 the gate RECORDS the member rather than committing it",
             has(s, "gFsPend.active = 1u; gFsPend.cb0 = tgtVa;"));
      expect("C31 K3 gfxsrc_ts_slots offers the second slot while 33 is on too",
             has(s, "static inline uint32_t gfxsrc_ts_slots() { return (gXpOn || gFs.on) ? N48_TS_SLOTS : 1u; }"));
      expect("C31 K3 the BEFORE is skipped until a commit reaches the ring",
             has(s, "if (gTsOn && !n48_ts_skip_commit(gFs.on, (uint32_t)gXdCm.commits) &&") &&
             count(s, "n48_ts_skip_commit(") == 1u);
      // 0.0.411: and NO BEFORE for a frame the fill-set reservation or the LUT rung will refuse.
      // 0.0.420 (P1 /): the SAME predicate now carries the second window's refusal and the region-moved neuter,
      // one call each, so there are TWO call sites and the second names `fpWillRefuse` and `f828RegionWillNeuter`.
      expect("C31 F2 the BEFORE is also skipped for a frame a later rung will refuse",
             has(s, "!n48_ts_skip_refused(fsWillRefuse, lutWillRefuse) &&") &&
             has(s, "!n48_ts_skip_refused(fpWillRefuse, f828RegionWillNeuter))") &&
             has(s, "const uint32_t fsWillRefuse  = (fsStep == N48_FS_REFUSE) ? 1u : 0u;") &&
             has(s, "const uint32_t lutWillRefuse = (gFs.on && c.lut_switch && c.lut_plane && !c.lut_ready) ? 1u : 0u;") &&
             has(s, "const uint32_t fpWillRefuse  = (fpStep == N48_FS_REFUSE) ? 1u : 0u;") &&
             count(s, "n48_ts_skip_refused(") == 2u); }
    // C31 0.0.409 — THE PAIR PRE-RESOLVE. Off by default and off at boot, assigned only by its own
    // `gfxneuter 34`; the translator bit is set ONLY under the switch, at every segment, and the counters the translator
    // returns are accumulated for the verb's ONE report line. No per-frame line. Every clause has a planted edit in `sm[]`.
    { const size_t b34 = s.find("(arg & 0xffull) == 34ull");
      const std::string v34 = b34 == std::string::npos ? "" : block_at(s, b34);
      const std::string pol34 = body_of(s, "static void gfxsrc_policy(");
      expect("C31 the pair-pre switch is defined once, default 0 (OFF AT BOOT)",
             count(s, "static volatile uint32_t gXdPairPre  { 0u };") == 1u);
      expect("C31 and assigned ONLY by its own verb (M 1 on, M 0xFF off)",
             count(s, "gXdPairPre =") == 2u && count(v34, "gXdPairPre =") == 2u);
      expect("C31 the switch verb reads M 1 / M 0xFF and refuses anything else",
             has(v34, "m == 0xFFu") && has(v34, "m == 1u") && has(v34, "st = 5;"));
      expect("C31 the ONE report line is `pairpre936:` and lives in the verb (NO per-frame line)",
             has(v34, "pairpre_report_line(") && count(s, "pairpre_report_line(") == 2u &&
             count(s, "\"pairpre936:") == 1u);
      expect("C31 the translator bit is set ONLY under the switch, at every segment",
             count(s, "XLAT12_EXTRA_PAIR_PRE") == 1u &&
             has(pol34, "if (gXdPairPre) { ex.flags |= XLAT12_EXTRA_PAIR_PRE; gXdPairPreS.segsOn++; }"));
      { const std::string prl = body_of(s, "static void pairpre_report_line(");
        expect("C31 and the translator's own counters are the report's reading",
               has(pol34, "gXdPairPreS.resolved += ds.pair_pre_resolved;") &&
               has(pol34, "gXdPairPreS.unresolved += ds.pair_pre_unresolved;") &&
               has(prl, "gXdPairPreS.resolved") && has(prl, "gXdPairPreS.unresolved")); }
      { const std::string lf = collect_printf_fmt(s, s.find("HWLOG(\"pairpre936:"));
        char b[2048];
        // the second %s is the verb's own `how`; its widest value is the REFUSED form, so measure with that.
        const int w = lf.empty() ? -1 : std::snprintf(b, sizeof b, lf.c_str(), "OFF (default)",
                                                      "`gfxneuter 34` REFUSED it, unchanged",
                                                      0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull);
        if (!gQuiet) std::printf("      pairpre936 line worst case: %d bytes (cap %u)\n", w, N48_LOG_CAP_BODY);
        expect("C31 the pairpre936 report fits under 480 body bytes at widest numerics",
               w > 0 && w < 480 && (unsigned)w <= N48_LOG_CAP_BODY); }
      { const std::string pfx = "0x2" "8";        // the three forbidden display-pipe offsets, assembled from parts
        const std::string f0 = pfx + "0", f2 = pfx + "2", f9 = std::string("0x2" "9") + "9";
        expect("C31 the pair-pre switch NEVER touches the display pipe's forbidden offsets",
               !v34.empty() && !has(v34, f0.c_str()) && !has(v34, f2.c_str()) && !has(v34, f9.c_str())); }
      expect("C31 and it never touches the `gfxneuter 15` (forgiveness) or `19` (running budget) switches",
             !has(v34, "gXdForgive") && !has(v34, "gXdRunBudget"));
    }
    // C5 part 1 (hygiene, notes/design/C5-CONTINUOUS.md Q4) — THE LEDGER UN-FEED IS WIRED AT ALL THREE SITES the
    // brief names: keystone-withdrawn, token-mismatched, never-run (the ring-walk exemption's NOT_RUN). Each site
    // is also gated on `gXdDescPort`, exactly as every other ledger touch in this file already is.
    // 0.0.444 (C5-RING-REVIEW.md (B) item 9a, Q7a) — THE THREE SITES NOW QUEUE (n48_dl_unfeed_queue), not apply
    // directly: none of the three runs under gXdLock (they are all inside hook_gfxCommitIB, on Apple's submit
    // thread), so calling n48_dl_unfeed_tok from there could race gfxsrc_desc_unmap's LOCKED compaction. The queue
    // is drained (n48_dl_unfeed_drain) under gXdLock at the top of the NEXT judged frame instead - pinned below.
    expect("C32 the ledger UN-FEED is QUEUED at exactly the three named sites (keystone-withdrawn, "
           "token-mismatched, never-run)", count(s, "n48_dl_unfeed_queue(&gXdLedUnfeedQ") == 3u);
    expect("C32 ... every call is gated on the descriptor-port switch, like every other ledger touch",
           count(s, "if (gXdDescPort) n48_dl_unfeed_queue(&gXdLedUnfeedQ") == 3u);
    // build 0.0.523 fix pass (MUST-FIX 1): the ONE direct n48_dl_unfeed_tok is switch 77's rn_unfeed_cb, which only
    // gfxsrc_rn_drain passes (under gXdLock, at the top of a judged frame) - never an unlocked hook_gfxCommitIB site.
    expect("C32 ... and NOTHING un-feeds the ledger directly from an unlocked hook_gfxCommitIB site any more "
           "(the one direct call is 77's rn_unfeed_cb, used only by the locked drain)",
           count(s, "(void)n48_dl_unfeed_tok(&gXdLed") == 1u &&
           count(s, "static void rn_unfeed_cb(void *, uint32_t seq) { if (gXdDescPort) (void)n48_dl_unfeed_tok(&gXdLed, seq); }") == 1u &&
           count(s, "rn_unfeed_cb") == 2u &&
           count(s, "(void)n48_rn_drain(&gRnQ, &gRnStore, &gR5Ring, gXpScopeSeq, gXdC.judged + 1u, &gRn, &rn_unfeed_cb, nullptr);") == 1u);
    expect("C32 ... the queue is DRAINED under gXdLock, at the top of the next judged frame (gfxsrc_decide_frame), "
           "before the sync and before anything can ask the ledger",
           count(s, "(void)n48_dl_unfeed_drain(&gXdLed, &gXdLedUnfeedQ);") == 1u);
}

// ---------------------------------------------------------------------------------------------------------------------------
// D. 0.0.379 — THE READOUT THAT MAKES AN N=2 RUN SCOREABLE.
//
// item 7 is the whole reason this group exists. A run scored on `gXdDpEpoch` + `gXdLed.clears` alone would have printed
// "the lifetime was fine" over a ledger that was NEVER FED - in every carried replay `clears` is 0 and the epoch constant while
// the ledger is empty, because the binding term is the FEED, not the lifetime - and the negative would have been read as "the
// carry failed". D1-D3 are that discrimination, stated as a property and re-run against five mutants of the classification:
// ANY classifier that gives an unfed ledger and a cleared ledger the same answer must FAIL this group.
// ---------------------------------------------------------------------------------------------------------------------------
typedef uint32_t (*WhyFn)(const n48_dl *);
static uint32_t why_real(const n48_dl *l) { return n48_dl_empty_why(l); }
/* M1: every empty ledger is one answer - the defect warns about, in its purest form. */
static uint32_t why_one_answer(const n48_dl *l)
{ if (!l) return N48_DL_WHY_UNKNOWN; return l->n ? N48_DL_WHY_HOLDING : N48_DL_WHY_UNKNOWN; }
/* M2: empty reads as CLEARED - a never-fed run would be reported as a ledger that HAD entries and lost them. */
static uint32_t why_empty_is_cleared(const n48_dl *l)
{ if (!l) return N48_DL_WHY_UNKNOWN; return l->n ? N48_DL_WHY_HOLDING : N48_DL_WHY_CLEARED; }
/* M3: NEVER-FED decided from `added` rather than `feeds` - a run whose every feed REFUSED claims nothing ever fed it. */
static uint32_t why_neverfed_from_added(const n48_dl *l)
{
    if (!l) return N48_DL_WHY_UNKNOWN;
    if (l->n) return N48_DL_WHY_HOLDING;
    if (!l->added) return N48_DL_WHY_NEVER_FED;
    if (l->clears) return N48_DL_WHY_CLEARED;
    if (l->unmapDropped) return N48_DL_WHY_UNMAPPED;
    return N48_DL_WHY_UNKNOWN;
}
/* M4: the unmap is tested before the clear - a ledger that was cleared reports `unmapped` whenever an unmap also happened. */
static uint32_t why_unmap_first(const n48_dl *l)
{
    if (!l) return N48_DL_WHY_UNKNOWN;
    if (l->n) return N48_DL_WHY_HOLDING;
    if (!l->feeds) return N48_DL_WHY_NEVER_FED;
    if (!l->fedOk) return N48_DL_WHY_ALL_REFUSED;
    if (!l->added) return N48_DL_WHY_NO_TARGET;
    if (l->unmapDropped) return N48_DL_WHY_UNMAPPED;
    if (l->clears) return N48_DL_WHY_CLEARED;
    return N48_DL_WHY_UNKNOWN;
}
/* M5: HOLDING is not decided first - a ledger that HAS entries reports CLEARED because it was cleared once, earlier. */
static uint32_t why_holding_last(const n48_dl *l)
{
    if (!l) return N48_DL_WHY_UNKNOWN;
    if (!l->feeds) return N48_DL_WHY_NEVER_FED;
    if (!l->fedOk) return N48_DL_WHY_ALL_REFUSED;
    if (!l->added) return N48_DL_WHY_NO_TARGET;
    if (l->clears) return N48_DL_WHY_CLEARED;
    if (l->unmapDropped) return N48_DL_WHY_UNMAPPED;
    return l->n ? N48_DL_WHY_HOLDING : N48_DL_WHY_UNKNOWN;
}

static void checks_readout(WhyFn why)
{
    static uint32_t o[64];
    n48_cm_seg seg[2];
    const uint64_t VA = 0x400800000ull;
    n48_dl a, b;   /* a = never fed, b = fed then cleared. The two the run has to tell apart. */

    // D1 NEVER FED. Exactly what a carried replay shows: epoch constant, clears 0, ledger empty.
    std::memset(&a, 0, sizeof a);
    n48_dl_sync(&a, 2u, 7u);
    n48_dl_sync(&a, 2u, 7u);
    expect("D1 an unfed ledger is empty with clears 0 and the epoch it was synced to",
           a.n == 0u && a.clears == 0u && a.feeds == 0u && a.epoch == 7u);
    expect("D1 and the readout calls it NEVER-FED", why(&a) == N48_DL_WHY_NEVER_FED);

    // D2 FED, THEN CLEARED. Same three numbers a-style readout would have printed: n 0, clears... non-zero, epoch moved.
    std::memset(&b, 0, sizeof b);
    n48_dl_sync(&b, 2u, 7u);
    const uint32_t n = seg_cb0(o, VA, 3u, 0);
    n48_dl_frame f = frame_ok(o, n, seg, 5u);
    expect("D2 a committed frame puts an entry in", n48_dl_feed(&b, &f) == 1u && b.n == 1u);
    n48_dl_sync(&b, 2u, 8u);
    expect("D2 an epoch change empties it and counts a clear", b.n == 0u && b.clears == 1u && b.added == 1u);
    expect("D2 and the readout calls it CLEARED", why(&b) == N48_DL_WHY_CLEARED);

    // D3 THE DISCRIMINATION ITSELF. This is the check the instrument exists for: both ledgers are EMPTY, and an instrument that
    // reported only `n`, the epoch and `clears` could still not tell them apart from the frame's own line - the classification
    // must, and its NAME must differ too, because the name is what a human reads out of the log.
    expect("D3 'empty because nothing fed it' and 'empty because it was cleared' are DIFFERENT answers",
           a.n == 0u && b.n == 0u && why(&a) != why(&b));
    expect("D3 and different STRINGS in the log", std::strcmp(n48_dl_why_name(why(&a)), n48_dl_why_name(why(&b))) != 0);
    expect("D3 neither is ever reported as HOLDING", why(&a) != N48_DL_WHY_HOLDING && why(&b) != N48_DL_WHY_HOLDING);
    expect("D3 and an unfed ledger is NEVER reported as cleared", why(&a) != N48_DL_WHY_CLEARED);

    // D4 FED AND EVERY FEED REFUSED - the third way to be empty, and it must not read as either of the first two.
    { n48_dl c; std::memset(&c, 0, sizeof c);
      n48_dl_frame g = f; g.gate = N48_CM_LEN;
      n48_dl_feed(&c, &g); n48_dl_feed(&c, &g);
      expect("D4 a ledger every feed refused is empty with feeds > 0, fedOk 0",
             c.n == 0u && c.feeds == 2u && c.fedOk == 0u && c.refusedGate == 2u);
      expect("D4 and reads ALL-REFUSED, not NEVER-FED and not CLEARED",
             why(&c) == N48_DL_WHY_ALL_REFUSED && why(&c) != why(&a) && why(&c) != why(&b)); }

    // D5 FED, PASSED THE GATE, PRODUCED NOTHING - the fourth way, and the one a `fedFrames` counter alone would hide.
    { n48_dl c; std::memset(&c, 0, sizeof c);
      static uint32_t bare[8]; bare[0] = 0xFFFF1000u;
      n48_cm_seg s2[2]; n48_dl_frame g = frame_ok(bare, 1u, s2, 5u);
      expect("D5 a committed frame with no CB0 draw adds nothing", n48_dl_feed(&c, &g) == 0u && c.fedOk == 1u && c.added == 0u);
      expect("D5 and reads NO-TARGET", why(&c) == N48_DL_WHY_NO_TARGET); }

    // D6 UNMAPPED - entries existed and an unmapVA took them, with nothing cleared.
    { n48_dl c; std::memset(&c, 0, sizeof c);
      n48_dl_feed(&c, &f);
      n48_dl_unmap(&c, 5u);
      expect("D6 an unmapped ledger is empty with clears 0 and unmapDropped 1",
             c.n == 0u && c.clears == 0u && c.unmapDropped == 1u);
      expect("D6 and reads UNMAPPED, not CLEARED", why(&c) == N48_DL_WHY_UNMAPPED); }

    // D7 BOTH an unmap and a clear happened. The ledger keeps no ordering, so CLEARED is the reported answer and the raw
    // `unmaps` counter on the same line is what says an unmap also occurred. Pinned so the order cannot silently swap.
    { n48_dl c; std::memset(&c, 0, sizeof c);
      n48_dl_sync(&c, 2u, 7u);
      n48_dl_feed(&c, &f); n48_dl_unmap(&c, 5u);
      n48_dl_feed(&c, &f); n48_dl_sync(&c, 2u, 9u);
      expect("D7 clears and unmapDropped both non-zero", c.n == 0u && c.clears == 1u && c.unmapDropped == 1u);
      expect("D7 and CLEARED wins (the ledger keeps no ordering)", why(&c) == N48_DL_WHY_CLEARED); }

    // D8 HOLDING is decided first: a ledger that HAS entries is never explained by something that happened earlier.
    { n48_dl c; std::memset(&c, 0, sizeof c);
      n48_dl_sync(&c, 2u, 7u);
      n48_dl_feed(&c, &f); n48_dl_sync(&c, 2u, 8u); n48_dl_feed(&c, &f);
      expect("D8 a ledger holding an entry after an earlier clear", c.n == 1u && c.clears == 1u);
      expect("D8 reads HOLDING", why(&c) == N48_DL_WHY_HOLDING); }

    // D9 a null ledger is not a crash and not a claim.
    expect("D9 a null ledger reads UNKNOWN", why(nullptr) == N48_DL_WHY_UNKNOWN);
}

// The line itself: every field item 7 asks for is in it, and its worst case fits under n48log's body cap.
static void checks_line()
{
    char lb[2048];
    const char *longestCm = n48_cm_reason_name(0);
    for (uint32_t r = 0; r < N48_CM_REASONS; r++)
        if (std::strlen(n48_cm_reason_name(r)) > std::strlen(longestCm)) longestCm = n48_cm_reason_name(r);
    const char *longestXv = n48_xv_reason_name(0);
    for (uint32_t r = 0; r < N48_XV_REASONS; r++)
        if (std::strlen(n48_xv_reason_name(r)) > std::strlen(longestXv)) longestXv = n48_xv_reason_name(r);
    const char *longestWhy = n48_dl_why_name(0);
    for (uint32_t w = 0; w < N48_DL_WHY_N; w++)
        if (std::strlen(n48_dl_why_name(w)) > std::strlen(longestWhy)) longestWhy = n48_dl_why_name(w);
    /* gfx_dep.h is not included here, so a 24-character filler stands in for its longest reason name ("counter-went-down"
       and "sdma-wait-removed", both 17) and for "GATE-NOT-RUN" (12): a longer one added later still fits. */
    const char *longestDep = "012345678901234567890123";
    const int n = std::snprintf(lb, sizeof lb, N48_DL_LINE_FMT, 0xffffffffffffffffull, 0xffffffffu, 0xffffffffu, 0xffffffffu,
                                0xffffffffu, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
                                0xffffffffffffffffull, 0xffffffffu, longestCm, longestXv, 0xffffffffu,
                                0xffffffffffffffffull, 0xffffffffffffffffull, longestWhy, longestDep,
                                0xffffffffffffffffull, 0xffffffffu, 0xffffffffffffffffull, 0xffffffffffffffffull,
                                0xffffffffffffffffull, 0xffffffffffffffffull);
    if (!gQuiet) std::printf("      dpled841 line worst case: %d bytes (cap %u)\n", n, N48_LOG_CAP_BODY);
    expect("D10 the readout line fits under n48log's body cap", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY);
    /* A line shortened until it fits by dropping a field would pass the bound above, so every field asks for is named. */
    const char *fmt = N48_DL_LINE_FMT;
    bool all = true;
    for (const char *t : { "dpled841:", " arm ", " ep ", " led ", " cl ", " fd ", " fed ", " gate ", " vrd ", " add ",
                           " rg ", " rc ", " why ", " dep ", " d ", " obs ", " ns ", " sn ", " rf " })
        if (!std::strstr(fmt, t)) all = false;
    expect("D10 and still carries the sync half, the feed half, the classification and the gate's dependency reading", all);
    expect("D10 (build 0.0.448 item 7) reFeedDropped (A6's drop count) is now printed as `rf`",
           std::strstr(fmt, " rf %llu") != nullptr);
    expect("D10 the feed's two refusal classes are distinguishable per frame", std::strstr(fmt, " rg %llu rc %llu") != nullptr);
    expect("D10 the gate's own reason AND detail AND observer set are on it",
           std::strstr(fmt, " dep %s d %#llx obs %#x") != nullptr);
}

static int run_quiet(void (*fn)());
// ---------------------------------------------------------------------------------------------------------------------------
// L485. build 0.0.485 (switch 58, notes/design/LOGIN-SCREEN-PATH.md "The ledger wall (L1, L2)") — THE LEDGER FIX.
//
//   L1  every colour target a COMMITTED frame wrote is fed with ITS OWN base page (n48_dl_pgx_extend over the per-draw pass
//       gfx_capture_scan.h n48_gcap_cbt_ib), not only each IB's final target. Fixture: run10c F48 (token seq 9, 15520|7616, 40
//       draws, the per-draw CB0 list read from run10c's own capture by the memo's graph.py; both IBs END on 0x400034000) and the
//       plane F51 that read the composite 0x406800000 and was refused `PROVENANCE ... surface VA 0x406800000`. The pages are
//       the run's own `gfx-dep: VA ... -> page ...` lines (0x406800000 -> 0x13d90000, 0x401088000 -> 0x13b12000,
//       0x400034000 -> 0x12f11000, 0x400800000 -> 0x10030000, 0x401800000 -> 0x10930000). The log names no page for
//       0x4010e0000, 0x401148000 or 0x4000ac000, so the fixture's resolver FAILS those three: that is the unresolved branch.
//   L2  a committed re-feed in a different shape REPLACES the entry (new tok, mode, page, size). Fixture: run10d F1/F17 fills
//       (CB_COLOR0_INFO 0x00008828 = 8_8_8_8), planes F22/F23, then fills F26/F35 re-writing the same composites as 0x00008824
//       (2_10_10_10), and the plane F34 reading 0x400800000. OFF reproduces the hardware counters (`led` 4 -> 3 -> 2, `rf`
//       0 -> 1 -> 2, F34 refused); ON keeps 4 entries and proves F34.
//   The un-feed, the neutered frame, the epoch and the unmap against the NEW entries; OFF identity; the kext's own ordering.
// ---------------------------------------------------------------------------------------------------------------------------
static const uint64_t kL485Ib0[24] = {
    0x406800000ull, 0x401088000ull, 0x401088000ull, 0x401088000ull, 0x4010e0000ull, 0x4010e0000ull, 0x4010e0000ull,
    0x401148000ull, 0x401148000ull, 0x4010e0000ull, 0x401148000ull, 0x401148000ull, 0x401148000ull, 0x4010e0000ull,
    0x4010e0000ull, 0x400034000ull, 0x400034000ull, 0x400034000ull, 0x4000ac000ull, 0x4000ac000ull, 0x4000ac000ull,
    0x400034000ull, 0x400034000ull, 0x400034000ull };
static const uint64_t kL485Ib1[16] = {
    0x401088000ull, 0x401088000ull, 0x401088000ull, 0x4010e0000ull, 0x4010e0000ull, 0x4010e0000ull, 0x4010e0000ull,
    0x400034000ull, 0x400034000ull, 0x400034000ull, 0x4000ac000ull, 0x4000ac000ull, 0x4000ac000ull, 0x400034000ull,
    0x400034000ull, 0x400034000ull };
static const uint64_t kL485X = 0x406800000ull, kL485XPg = 0x13d90000ull;   /* run10c's composite and its page */

/* A gfx10.3 source draw into CB0 at `va` (what n48_gcap_cbt_ib walks): CB_COLOR0_BASE (context 0xa318), CB_COLOR0_BASE_EXT
 * (0xa390), CB_TARGET_MASK (0xa08e, CB0 only), DRAW_INDEX_AUTO. */
static uint32_t l485_src_draw(uint32_t *d, uint64_t va)
{
    uint32_t k = 0;
    d[k++] = 0xC0016900u; d[k++] = 0x318u; d[k++] = (uint32_t)(va >> 8);
    d[k++] = 0xC0016900u; d[k++] = 0x390u; d[k++] = (uint32_t)(va >> 40) & 0xFFu;
    d[k++] = 0xC0016900u; d[k++] = 0x08eu; d[k++] = 0xFu;
    d[k++] = 0xC0012D00u; d[k++] = 3u; d[k++] = 2u;
    return k;
}
struct L485Map { uint64_t va[8], pg[8]; uint32_t n, calls; };
static uint32_t l485_resolve(void *ud, uint64_t va, uint64_t *page)
{
    L485Map *m = static_cast<L485Map *>(ud);
    m->calls++;
    for (uint32_t k = 0; k < m->n; k++) if ((m->va[k] & ~0xfffull) == (va & ~0xfffull)) { *page = m->pg[k]; return 1u; }
    return 0u;
}
static L485Map l485_map()
{
    L485Map m; std::memset(&m, 0, sizeof m);
    const uint64_t v[] = { 0x406800000ull, 0x401088000ull, 0x400034000ull, 0x400800000ull, 0x401800000ull };
    const uint64_t p[] = { 0x13d90000ull, 0x13b12000ull, 0x12f11000ull, 0x10030000ull, 0x10930000ull };
    for (uint32_t k = 0; k < 5u; k++) { m.va[k] = v[k]; m.pg[k] = p[k]; }
    m.n = 5u;
    return m;
}
static uint32_t l485_find(const n48_dl *l, uint64_t va)
{
    for (uint32_t k = 0; k < l->n; k++) if (l->e[k].ctx == 5ull && l->e[k].va == va) return k;
    return N48_DL_MAX;
}
/* run10c's ledger before F48 (`led 2>2`): the fill (tok 1) and the plane (tok 8), each an IB-end target with its page. */
static void l485_pre(n48_dl *l)
{
    std::memset(l, 0, sizeof *l);
    n48_dl_sync(l, 2u, 6u);
    l->feedTok = 1u; n48_dl_set(l, 5ull, 0x400800000ull, 3u, 0x10030000ull, 0u, 0ull);
    l->feedTok = 8u; n48_dl_set(l, 5ull, 0x401800000ull, 3u, 0x10930000ull, 0u, 0ull);
    l->feedTok = 0u;
}
/* THE KEXT'S OWN ORDER for one judged frame (gfxsrc_decide_frame; pinned in the source by group L485-W below): the switch is
 * read once (`ledMid = dp && 58`), the per-draw pass state is reset, every IB is walked by the per-draw pass after its read,
 * the COMMIT gate answers, the IB-end rows go into the map, the map is extended ONLY for a committed frame with the switch
 * on, the REPLACE is mirrored, and the ledger is fed. hVa/hPage here are what the kext's hold for F48: one IB-end item per
 * IB, both 0x400034000 (n48_gcap_scan pushes each slot's LAST base after its walk). */
static uint32_t l485_frame48(n48_dl *l, uint32_t dp, uint32_t on, uint32_t committed, uint32_t tok, L485Map *map)
{
    static uint32_t src0[24u * 12u], src1[16u * 12u], out[40u * 16u];
    static n48_gcap_cbt cbt;
    static n48_dl_pgx pgx;
    static uint64_t hVa[8], hPage[8];
    n48_cm_seg seg[2];
    uint32_t n0 = 0, n1 = 0, o = 0;
    for (uint32_t k = 0; k < 24u; k++) n0 += l485_src_draw(&src0[n0], kL485Ib0[k]);
    for (uint32_t k = 0; k < 16u; k++) n1 += l485_src_draw(&src1[n1], kL485Ib1[k]);
    seg[0].head = 0u; seg[0].start = 0u; seg[0].status = 0u;
    for (uint32_t k = 0; k < 24u; k++) o += seg_cb0(&out[o], kL485Ib0[k], 3u, 0);
    seg[0].end = o; seg[0].out_len = o;
    seg[1].head = o; seg[1].start = o; seg[1].status = 0u;
    for (uint32_t k = 0; k < 16u; k++) o += seg_cb0(&out[o], kL485Ib1[k], 3u, 0);
    seg[1].end = o; seg[1].out_len = o - seg[1].start;
    /* gfxsrc_decide_frame, in order */
    const uint32_t ledMid = (dp && on) ? 1u : 0u;
    if (ledMid) n48_gcap_cbt_reset(&cbt);
    if (ledMid) (void)n48_gcap_cbt_ib(src0, n0, &cbt);
    if (ledMid) (void)n48_gcap_cbt_ib(src1, n1, &cbt);
    hVa[0] = hVa[1] = 0x400034000ull; hPage[0] = hPage[1] = 0x12f11000ull;
    const uint32_t commitOk = committed;
    uint32_t added = 0u;
    if (dp) {
        n48_dl_frame lf; std::memset(&lf, 0, sizeof lf);
        lf.committed = commitOk; lf.gate = N48_CM_OK; lf.verdict = N48_XV_TRANSLATE; lf.nseg = 2u; lf.n = o;
        lf.ctx = 5ull; lf.seg = seg; lf.out = out;
        lf.pg.va = hVa; lf.pg.page = hPage; lf.pg.n = 2u; lf.pg.full = 0u;
        if (ledMid && commitOk) {
            const uint32_t held = cbt.n < N48_GCAP_CBT_MAX ? cbt.n : N48_GCAP_CBT_MAX;
            (void)n48_dl_pgx_extend(l, &pgx, &lf.pg, cbt.va, held, cbt.n, &l485_resolve, map);
        }
        l->replace = ledMid;
        lf.tok = tok; lf.arm_ep = 6u;
        added = n48_dl_feed(l, &lf);
    }
    return added;
}
/* The NEXT judged frame's top, in the kext's order (un-feed drain, then the sync), then the plane's provenance ask. */
static int l485_ask(n48_dl *l, n48_dl_unfeed_q *q, uint32_t ep, uint64_t va, uint64_t page)
{
    if (q) (void)n48_dl_unfeed_drain(l, q);
    n48_dl_sync(l, 2u, ep);
    return n48_dl_tiled_ok_pg(l, 5ull, va, 3u, page, 1u);
}

/* run10d: one committed 1040-dword frame writing CB0 at `va` (gfx12 ADDR3_64KB_2D, 1920x1080) in the gfx12 FORMAT the
 * translator makes of the source CB_COLOR0_INFO: xlat12_repack.h repack_CB_COLOR0_INFO moves FORMAT [6:2] -> [4:0], COMP_SWAP
 * [12:11] and BLEND_CLAMP [15] across, so 0x00008828 -> 0x0000880a (FORMAT 10, 8_8_8_8: an extent is derived, 0x870000)
 * and 0x00008824 -> 0x00008809 (FORMAT 9, 2_10_10_10: n48_dl_extent derives none - size 0, the mismatched field). */
static const uint32_t kL485Info8888 = 0x0000880au, kL485Info2101010 = 0x00008809u;
static uint32_t l485_fill(n48_dl *l, uint32_t on, uint32_t tok, uint64_t va, uint64_t page, uint32_t info)
{
    static uint32_t o[64];
    static uint64_t rowVa[1], rowPg[1];
    n48_cm_seg seg[2];
    const uint32_t n = seg_cb0_x(o, va, 3u, 1920u, 1080u, info, 0u, 1u, 0u, 0);
    rowVa[0] = va; rowPg[0] = page;
    n48_dl_frame f = frame_ok(o, n, seg, 5ull);
    f.pg.va = rowVa; f.pg.page = rowPg; f.pg.n = 1u;
    f.tok = tok; f.arm_ep = 6u;
    l->replace = on;
    return n48_dl_feed(l, &f);
}
static void l485_run10d_pre(n48_dl *l, uint32_t on)
{
    std::memset(l, 0, sizeof *l);
    n48_dl_sync(l, 2u, 6u);
    (void)l485_fill(l, on, 1u, 0x400800000ull, 0x10030000ull, kL485Info8888);   /* F1  fill  */
    (void)l485_fill(l, on, 2u, 0x404800000ull, 0x125e0000ull, kL485Info8888);   /* F17 fill  */
    (void)l485_fill(l, on, 3u, 0x402800000ull, 0x11200000ull, kL485Info8888);   /* F22 plane */
    (void)l485_fill(l, on, 4u, 0x403800000ull, 0x11a70000ull, kL485Info8888);   /* F23 plane */
}

static void checks_l485_l1()
{
    /* ---- L1 ON: the fix ---------------------------------------------------------------------------------------------- */
    {
        n48_dl l; l485_pre(&l);
        L485Map map = l485_map();
        const uint32_t added = l485_frame48(&l, 1u, 1u, 1u, 9u, &map);
        expect("L485-1 ON: F48 feeds all 40 draws (dpled841 `add 40`)", added == 40u);
        expect("L485-1 ON: the ledger holds 8 (the 2 before + 6 distinct targets, `led 2` -> 8)", l.n == 8u);
        const uint32_t kx = l485_find(&l, kL485X);
        expect("L485-1 ON: the composite 0x406800000 (written MID-IB 0, first draw) carries ITS OWN page 0x13d90000",
               kx < N48_DL_MAX && l.e[kx].page == kL485XPg);
        expect("L485-1 ON: ... under F48's token (9), flagged as a per-draw row",
               kx < N48_DL_MAX && l.e[kx].tok == 9u && (l.e[kx].flags & N48_DL_F_MID));
        const uint32_t ke = l485_find(&l, 0x400034000ull);
        expect("L485-1 ON: the IB-END target keeps the IB-end row's answer (page 0x12f11000, NOT flagged)",
               ke < N48_DL_MAX && l.e[ke].page == 0x12f11000ull && !(l.e[ke].flags & N48_DL_F_MID));
        const uint32_t ku = l485_find(&l, 0x4010e0000ull);
        expect("L485-1 ON: a target whose page did NOT resolve is fed with page 0 (never guessed)",
               ku < N48_DL_MAX && l.e[ku].page == 0ull && (l.e[ku].flags & N48_DL_F_MID));
        expect("L485-1 ON: the resolver was asked once per per-draw target the IB-end rows did not name (5), no more",
               map.calls == 5u);
        expect("L485-1 ON: counters - rows 5 (resolved 2, unresolved 3), none dropped",
               l.midRows == 5u && l.midPgRes == 2u && l.midPgUnres == 3u && l.midOver == 0u);
        expect("L485-1 ON: counters - 28 draws answered by a per-draw row, 7 of them with a page",
               l.midFed == 28u && l.midFedKeyed == 7u);
        /* THE PLANE F51, next judged frame: it reads the composite with its own page. */
        expect("L485-1 ON: REACHABILITY - F51's provenance ask for 0x406800000 (page 0x13d90000) is PROVEN",
               l485_ask(&l, nullptr, 6u, kL485X, kL485XPg) == 1);
        expect("L485-1 ON: ... but not for a DIFFERENT page (a re-map is still refused, keyMoved)",
               l485_ask(&l, nullptr, 6u, kL485X, 0x13aa0000ull) == 0 && l.keyMoved == 1u);
        const uint64_t np0 = l.keyNoPage;
        expect("L485-1 ON: a surface whose page is UNKNOWN never proves, whatever the asking frame maps",
               l485_ask(&l, nullptr, 6u, 0x4010e0000ull, 0x13b50000ull) == 0 && l.keyNoPage == np0 + 1u);
    }
    /* ---- L1 OFF: today's behaviour, byte for byte (the hardware's own refusal) --------------------------------------- */
    {
        n48_dl l; l485_pre(&l);
        L485Map map = l485_map();
        const uint32_t added = l485_frame48(&l, 1u, 0u, 1u, 9u, &map);
        const uint32_t kx = l485_find(&l, kL485X);
        expect("L485-2 OFF: F48 still feeds 40 draws and 8 entries", added == 40u && l.n == 8u);
        expect("L485-2 OFF: the composite enters with page 0, unflagged (L1, as run10c measured)",
               kx < N48_DL_MAX && l.e[kx].page == 0ull && l.e[kx].flags == 0u);
        expect("L485-2 OFF: nothing is resolved and no new counter moves",
               map.calls == 0u && l.midRows == 0u && l.midFed == 0u && l.reFeedReplaced == 0u);
        expect("L485-2 OFF: F51 is REFUSED for no page (run10c `refused for no page`)",
               l485_ask(&l, nullptr, 6u, kL485X, kL485XPg) == 0 && l.keyNoPage == 1u);
    }
    /* ---- the switch needs the descriptor path: 58 ON with 10 OFF does nothing ---------------------------------------- */
    {
        n48_dl l; l485_pre(&l);
        L485Map map = l485_map();
        expect("L485-3 58 ON with the descriptor path OFF: no feed, no resolve",
               l485_frame48(&l, 0u, 1u, 1u, 9u, &map) == 0u && map.calls == 0u && l.n == 2u && l.replace == 0u);
    }
    /* ---- a frame the COMMIT gate did not answer yes for resolves nothing and feeds nothing --------------------------- */
    {
        n48_dl l; l485_pre(&l);
        L485Map map = l485_map();
        expect("L485-4 ON, NOT committed: the feed refuses and no page is resolved",
               l485_frame48(&l, 1u, 1u, 0u, 9u, &map) == 0u && map.calls == 0u && l.n == 2u && l.midRows == 0u);
    }
    /* ---- the extension changes the MAP only: it adds no ledger entry by itself --------------------------------------- */
    {
        n48_dl l; l485_pre(&l);
        L485Map map = l485_map();
        n48_dl_pgx x; std::memset(&x, 0, sizeof x);
        uint64_t hv[1] = { 0x400034000ull }, hp[1] = { 0x12f11000ull };
        n48_dl_pgmap m; std::memset(&m, 0, sizeof m); m.va = hv; m.page = hp; m.n = 1u;
        const uint64_t dv[3] = { kL485X, 0x400034000ull, 0ull };
        const uint32_t rows = n48_dl_pgx_extend(&l, &x, &m, dv, 3u, 3u, &l485_resolve, &map);
        expect("L485-5 the extension appends one row (the IB-end VA and the zero base are skipped)", rows == 1u && m.n == 2u);
        expect("L485-5 ... IB-end row first and unchanged, then the per-draw row", m.va[0] == 0x400034000ull &&
               m.page[0] == 0x12f11000ull && m.va[1] == kL485X && m.page[1] == kL485XPg && m.mid0 == 1u && m.midN == 1u);
        expect("L485-5 ... and adds NO ledger entry (only a committed feed does)", l.n == 2u);
        /* the per-draw pass's own 32-target table overflowed (dtotal > dn): no row for the rest, `full` set -> CAPPED */
        n48_dl_pgmap m2; std::memset(&m2, 0, sizeof m2);
        (void)n48_dl_pgx_extend(&l, &x, &m2, dv, 1u, 34u, &l485_resolve, &map);
        expect("L485-5 targets the per-draw pass could not hold get no row and mark the map full (OUR cap)",
               m2.full == 1u && l.midOver == 33u);
        /* the map's own room: 8 IB-end rows + 32 per-draw rows fit; the 33rd distinct target does not */
        static uint64_t h8v[8], h8p[8], d33[33];
        for (uint32_t k = 0; k < 8u; k++) { h8v[k] = 0x500000000ull + 0x100000ull * k; h8p[k] = 0x20000000ull + 0x1000ull * k; }
        for (uint32_t k = 0; k < 33u; k++) d33[k] = 0x600000000ull + 0x100000ull * k;
        n48_dl l2; std::memset(&l2, 0, sizeof l2);
        n48_dl_pgmap m3; std::memset(&m3, 0, sizeof m3); m3.va = h8v; m3.page = h8p; m3.n = 8u;
        const uint32_t r3 = n48_dl_pgx_extend(&l2, &x, &m3, d33, 33u, 33u, &l485_resolve, &map);
        expect("L485-5 the map holds 8 + 32 rows and counts the 33rd as not held", r3 == 32u && m3.n == N48_DL_PGX_MAX &&
               l2.midOver == 1u && m3.full == 1u && n48_dl_pg_of(&m3, d33[32]) == 0ull);
    }
    /* ---- OFF identity of the lookup: n48_dl_pg_of_ix answers exactly n48_dl_pg_of on a grid ------------------------- */
    {
        static uint64_t gv[6] = { 0x400800000ull, 0x400800800ull, 0x401000000ull, 0x400800000ull, 0ull, 0x7ffffffff000ull };
        static uint64_t gp[6] = { 0x10030000ull, 0x11110000ull, 0ull, 0x22220000ull, 0x33330000ull, 0x44440000ull };
        bool same = true;
        for (uint32_t n = 0; n <= 6u; n++) {
            n48_dl_pgmap m; std::memset(&m, 0, sizeof m); m.va = gv; m.page = gp; m.n = n;
            const uint64_t probe[] = { 0x400800000ull, 0x400800fffull, 0x400801000ull, 0x401000000ull, 0ull, 0x7ffffffff123ull };
            for (uint64_t v : probe) { uint32_t ix = 99u; if (n48_dl_pg_of_ix(&m, v, &ix) != n48_dl_pg_of(&m, v)) same = false; }
        }
        uint32_t ix = 7u;
        expect("L485-6 OFF identity: the indexed lookup gives n48_dl_pg_of's answer on every probe of the grid",
               same && n48_dl_pg_of_ix(nullptr, 0x400800000ull, &ix) == 0ull && ix == 0u);
    }
}

static void checks_l485_l2()
{
    /* ---- L2 OFF: run10d's own counters, exactly --------------------------------------------------------------------- */
    {
        n48_dl l; l485_run10d_pre(&l, 0u);
        expect("L485-7 OFF: after F1/F17/F22/F23 the ledger holds 4 (`led 4`), the 8_8_8_8 ones WITH an extent",
               l.n == 4u && l.entExtent == 4u && l.e[0].size == 0x870000ull);
        (void)l485_fill(&l, 0u, 5u, 0x400800000ull, 0x10030000ull, kL485Info2101010);           /* F26 */
        expect("L485-7 OFF: F26's 2_10_10_10 re-feed DROPS the composite (`rf 1`, `led 3`)",
               l.n == 3u && l.reFeedDropped == 1u && l.reFeedReplaced == 0u);
        expect("L485-7 OFF: F34's plane ask for 0x400800000 is REFUSED (run10d: every later plane)",
               l485_ask(&l, nullptr, 6u, 0x400800000ull, 0x10030000ull) == 0);
        (void)l485_fill(&l, 0u, 6u, 0x404800000ull, 0x125e0000ull, kL485Info2101010);           /* F35 */
        expect("L485-7 OFF: F35 drops the second composite (`rf 2`, `led 2`)", l.n == 2u && l.reFeedDropped == 2u);
    }
    /* ---- L2 ON: the replace ------------------------------------------------------------------------------------------ */
    {
        n48_dl l; l485_run10d_pre(&l, 1u);
        (void)l485_fill(&l, 1u, 5u, 0x400800000ull, 0x10030000ull, kL485Info2101010);           /* F26 */
        const uint32_t k = l485_find(&l, 0x400800000ull);
        expect("L485-8 ON: F26 REPLACES the composite's entry (4 kept, 1 replaced, 0 dropped)",
               l.n == 4u && l.reFeedReplaced == 1u && l.reFeedDropped == 0u);
        expect("L485-8 ON: ... with the NEW producer's token and shape (tok 5, size 0: 2_10_10_10 has no extent)",
               k < N48_DL_MAX && l.e[k].tok == 5u && l.e[k].size == 0ull && l.e[k].mode == 3u && l.e[k].page == 0x10030000ull);
        expect("L485-8 ON: REACHABILITY - F34's plane ask for 0x400800000 at its own page is PROVEN",
               l485_ask(&l, nullptr, 6u, 0x400800000ull, 0x10030000ull) == 1);
        (void)l485_fill(&l, 1u, 6u, 0x404800000ull, 0x125e0000ull, kL485Info2101010);           /* F35 */
        expect("L485-8 ON: F35 replaces the second composite too (4 kept, 2 replaced)", l.n == 4u && l.reFeedReplaced == 2u);
    }
    /* ---- the replace proves only the NEW shape --------------------------------------------------------------------- */
    {
        n48_dl l; l485_run10d_pre(&l, 1u);
        (void)l485_fill(&l, 1u, 5u, 0x400800000ull, 0x13aa0000ull, kL485Info8888);   /*'s re-map: same VA, new page */
        expect("L485-9 ON: a re-feed on a NEW page replaces; the OLD page no longer proves (keyMoved)",
               l485_ask(&l, nullptr, 6u, 0x400800000ull, 0x10030000ull) == 0 && l.keyMoved == 1u);
        expect("L485-9 ON: ... the new page does", l485_ask(&l, nullptr, 6u, 0x400800000ull, 0x13aa0000ull) == 1);
        l.feedTok = 7u; l.replace = 1u;
        n48_dl_set(&l, 5ull, 0x400800000ull, 2u, 0x13aa0000ull, 0u, 0ull);          /* a mode change */
        l.feedTok = 0u;
        expect("L485-9 ON: a MODE change replaces; the old mode no longer proves, the new one does",
               n48_dl_tiled_ok_pg(&l, 5ull, 0x400800000ull, 3u, 0x13aa0000ull, 1u) == 0 &&
               n48_dl_tiled_ok_pg(&l, 5ull, 0x400800000ull, 2u, 0x13aa0000ull, 1u) == 1);
    }
    /* ---- a re-shape arriving OUTSIDE a feed (no producer identity) is dropped, never replaced ------------------------ */
    {
        n48_dl l; l485_run10d_pre(&l, 1u);
        l.replace = 1u; l.feedTok = 0u;
        n48_dl_set(&l, 5ull, 0x400800000ull, 3u, 0x10030000ull, 0u, 0ull);
        expect("L485-10 ON, feedTok 0: DROPPED (nothing could ever un-feed it), not replaced",
               l.n == 3u && l.reFeedDropped == 1u && l.reFeedReplaced == 0u);
    }
    /* ---- a SAME-shape re-feed with the switch on is unchanged (0.0.444's keep-the-first-producer) -------------------- */
    {
        n48_dl l; l485_run10d_pre(&l, 1u);
        (void)l485_fill(&l, 1u, 9u, 0x400800000ull, 0x10030000ull, kL485Info8888);
        const uint32_t k = l485_find(&l, 0x400800000ull);
        expect("L485-11 ON: a same-shape re-feed keeps the FIRST producer's token (tok 1), replaces nothing",
               k < N48_DL_MAX && l.e[k].tok == 1u && l.reFeedReplaced == 0u && l.reFeedDropped == 0u);
    }
}

/* THE SAFETY ARGUMENT, tested: the ledger may only say "this surface's current content was written by a frame we committed". */
static void checks_l485_safety()
{
    /* S1 the UN-FEED reaches the new entries: F48 fed its mid-IB targets, then the keystone withdrew it (never ran). */
    {
        n48_dl l; l485_pre(&l);
        L485Map map = l485_map();
        (void)l485_frame48(&l, 1u, 1u, 1u, 9u, &map);
        n48_dl_unfeed_q q; std::memset(&q, 0, sizeof q);
        n48_dl_unfeed_queue(&q, 9u);                                  /* hook_gfxCommitIB: withdrawn / mismatched / NOPed */
        expect("L485-S1 a WITHDRAWN F48: its per-draw-paged entries are un-fed with the rest - F51 REFUSED",
               l485_ask(&l, &q, 6u, kL485X, kL485XPg) == 0);
        expect("L485-S1 ... exactly its 6 entries went; the fill (tok 1) and plane (tok 8) stay", l.n == 2u &&
               l.unfeedRemoved == 6u && l485_find(&l, 0x400800000ull) < N48_DL_MAX && l485_find(&l, 0x401800000ull) < N48_DL_MAX);
    }
    /* S2 un-feeding the REPLACING producer removes the entry; it never reverts to the old producer's proof. */
    {
        n48_dl l; l485_run10d_pre(&l, 1u);
        (void)l485_fill(&l, 1u, 5u, 0x400800000ull, 0x10030000ull, kL485Info2101010);           /* F26 replaces tok 1 */
        n48_dl_unfeed_q q; std::memset(&q, 0, sizeof q);
        n48_dl_unfeed_queue(&q, 5u);                                                            /* F26 withdrawn */
        expect("L485-S2 the replacing frame withdrawn: the composite is NOT proven any more (not reverted to F1)",
               l485_ask(&l, &q, 6u, 0x400800000ull, 0x10030000ull) == 0 && l485_find(&l, 0x400800000ull) == N48_DL_MAX);
        expect("L485-S2 ... and the other three entries stand", l.n == 3u);
    }
    /* S3 un-feeding the OLD producer after a replace finds nothing of its own: the entry is the new producer's. */
    {
        n48_dl l; l485_run10d_pre(&l, 1u);
        (void)l485_fill(&l, 1u, 5u, 0x400800000ull, 0x10030000ull, kL485Info2101010);
        n48_dl_unfeed_q q; std::memset(&q, 0, sizeof q);
        n48_dl_unfeed_queue(&q, 1u);                                                            /* F1 withdrawn, late */
        expect("L485-S3 the OLD producer's un-feed leaves the replacing producer's proof (it wrote the surface since)",
               l485_ask(&l, &q, 6u, 0x400800000ull, 0x10030000ull) == 1 && l.n == 4u && l.unfeedRemoved == 0u);
    }
    /* S4 a NEUTERED frame (the COMMIT gate did not answer yes: its IB is zeroed at the source, it writes nothing) cannot
     *    touch an entry - not add, not replace, not re-page. A's proof of the composite stands, because A's pixels do. */
    {
        n48_dl l; l485_pre(&l);
        L485Map map = l485_map();
        (void)l485_frame48(&l, 1u, 1u, 1u, 9u, &map);
        (void)l485_fill(&l, 1u, 12u, kL485X, 0x10930000ull, kL485Info2101010);   /* committed: control, below */
        n48_dl l2; l485_pre(&l2);
        L485Map map2 = l485_map();
        (void)l485_frame48(&l2, 1u, 1u, 1u, 9u, &map2);
        static uint32_t o[64]; static uint64_t rv[1], rp[1];
        n48_cm_seg seg[2];
        const uint32_t n = seg_cb0_x(o, kL485X, 3u, 1920u, 1080u, kL485Info2101010, 0u, 1u, 0u, 0);
        rv[0] = kL485X; rp[0] = 0x10930000ull;
        n48_dl_frame f = frame_ok(o, n, seg, 5ull); f.pg.va = rv; f.pg.page = rp; f.pg.n = 1u; f.tok = 12u;
        f.committed = 0u;                                                          /* NEUTERED */
        l2.replace = 1u;
        const uint32_t k2 = l485_find(&l2, kL485X);
        expect("L485-S4 a NEUTERED re-writer is refused at the feed and the entry is untouched (tok 9, page 0x13d90000)",
               n48_dl_feed(&l2, &f) == 0u && k2 < N48_DL_MAX && l2.e[k2].tok == 9u && l2.e[k2].page == kL485XPg &&
               l2.reFeedReplaced == 0u);
        const uint32_t k1 = l485_find(&l, kL485X);
        expect("L485-S4 control: the same re-writer COMMITTED replaces it (tok 12, its page)",
               k1 < N48_DL_MAX && l.e[k1].tok == 12u && l.e[k1].page == 0x10930000ull && l.reFeedReplaced == 1u);
    }
    /* S5 Apple's OWN un-committed work: any frame handed to Apple untranslated moves the epoch; the next sync clears the
     *    whole ledger, per-draw-paged and replaced entries alike. */
    {
        n48_dl l; l485_pre(&l);
        L485Map map = l485_map();
        (void)l485_frame48(&l, 1u, 1u, 1u, 9u, &map);
        (void)l485_fill(&l, 1u, 13u, 0x400800000ull, 0x10030000ull, kL485Info2101010);   /* a replace too */
        expect("L485-S5 an untranslated pass (epoch 6 -> 7): the composite and the replaced fill both refuse",
               l485_ask(&l, nullptr, 7u, kL485X, kL485XPg) == 0 &&
               n48_dl_tiled_ok_pg(&l, 5ull, 0x400800000ull, 3u, 0x10030000ull, 1u) == 0 && l.n == 0u);
    }
    /* S6 an unmapVA covering the composite drops a per-draw-paged entry exactly as any other (exact drop on, keep off). */
    {
        n48_dl l; l485_pre(&l);
        L485Map map = l485_map();
        (void)l485_frame48(&l, 1u, 1u, 1u, 9u, &map);
        n48_dl_unmap_rng(&l, 5ull, kL485X, 0x10000ull, 1u);
        expect("L485-S6 a covering unmap drops the per-draw-paged composite (no extent: the one-sided rule)",
               l485_find(&l, kL485X) == N48_DL_MAX && l485_ask(&l, nullptr, 6u, kL485X, kL485XPg) == 0);
    }
    /* the report line's bound: n48log's 491-byte body cap, at the longest strings and the widest numerics */
    {
        char line[1024];
        const int w = std::snprintf(line, sizeof line, N48_DL_MID_REPORT_FMT, "OFF (default)",
                                    "`gfxneuter 58` REFUSED - a continuous arm stands",
                                    " - INERT: the descriptor path (10) is off", ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull,
                                    ~0ull, ~0ull);
        std::printf("      ledmid485 report line at widest numerics and longest strings: %d bytes\n", w);
        expect("L485-R the ledmid485 report line is under 491 bytes at its maximum", w > 0 && w < 491);
    }
}

/* THE KEXT'S OWN WIRING (AppleHardwareHook.cpp, comments stripped). The behaviour above drives the real functions in the
 * kext's order; these prove the kext HAS that order, inside the blocks that run - not a literal line alone. */
static void checks_l485_wiring(const std::string &raw)
{
    const std::string s = strip_comments(raw);
    expect("L485-W1 the switch is defined once, default 0", count(s, "static volatile uint32_t gLedMidOn { 0u };") == 1u);
    { const size_t vb = s.find("(arg & 0xffull) == 58ull");
      const std::string v = vb == std::string::npos ? "" : block_at(s, vb);
      expect("L485-W1 only the `gfxneuter 58` verb assigns it, behind the mid-arm guard, and a change moves the epoch",
             count(s, "gLedMidOn =") == 1u && count(v, "gLedMidOn = flm;") == 1u &&
             has(v, "n48_cm_cont_switch_refused(58u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") &&
             has(v, "__atomic_fetch_add(&gXdDpEpoch, 1u, __ATOMIC_RELAXED);") && has(v, "ledmid_report_line(")); }
    expect("L485-W1 the report line is the bounded N48_DL_MID_REPORT_FMT, once", count(s, "HWLOG(N48_DL_MID_REPORT_FMT,") == 1u);
    const std::string dec = body_of(s, "static uint32_t gfxsrc_decide_frame(");
    const char *seq[] = {
        "const uint32_t ledMid = (dp && gLedMidOn) ? 1u : 0u;",
        "if (ledMid) n48_gcap_cbt_reset(&gLedCbt);",
        "if (got != len || f.ib[k].walk != len) continue;",
        "if (ledMid) (void)n48_gcap_cbt_ib(dst, got, &gLedCbt);",
        "const uint32_t commitOk =",
        "if (dp && ranPolicy) {",
        "lf.pg.va = hVa; lf.pg.page = hPage; lf.pg.n = hHeld;",
        "if (ledMid && commitOk) {",
        "(void)n48_dl_pgx_extend(&gXdLed, &gLedPgx, &lf.pg, gLedCbt.va, lmHeld, gLedCbt.n, &r5_resolve_cb, &lmc);",
        "gXdLed.replace = ledMid;",
        "const uint32_t added = n48_dl_feed(&gXdLed, &lf);" };
    size_t prev = 0; bool ordered = !dec.empty();
    for (const char *t : seq) { const size_t p = dec.find(t, prev); if (p == std::string::npos) { ordered = false; break; } prev = p + 1u; }
    expect("L485-W2 REACHABILITY: in gfxsrc_decide_frame the switch is read, the pass reset, every read IB walked, the gate "
           "answered, the map extended, the REPLACE mirrored and the ledger fed - in that order", ordered);
    { const size_t pl = dec.find("for (uint32_t k = 0; k < f.nib && f.reader_ok; k++) {");
      const std::string loop = pl == std::string::npos ? "" : block_at(dec, pl);
      const size_t pc = loop.find("if (got != len || f.ib[k].walk != len) continue;");
      const size_t pw = loop.find("if (ledMid) (void)n48_gcap_cbt_ib(dst, got, &gLedCbt);");
      expect("L485-W3 the per-draw pass runs INSIDE the IB read loop, after the frame-trust `continue`, on the body just read",
             !loop.empty() && pc != std::string::npos && pw != std::string::npos && pc < pw);
      expect("L485-W3 ... and NOT under switch 54 (it runs whatever `tvOn` says)",
             pw != std::string::npos && loop.substr(pw, 60).find("tvOn") == std::string::npos); }
    { const size_t pf = dec.find("if (dp && ranPolicy) {");
      const std::string fd = pf == std::string::npos ? "" : block_at(dec, pf);
      const size_t pe = fd.find("if (ledMid && commitOk) {");
      const std::string ext = pe == std::string::npos ? "" : block_at(fd, pe);
      expect("L485-W4 the extension sits in the ledger-feed block, gated only on the switch and THIS frame's commit",
             !ext.empty() && has(ext, "R5Ctx lmc { &vm };") && has(ext, "n48_dl_pgx_extend(&gXdLed, &gLedPgx, &lf.pg,") &&
             has(fd, "lf.committed = commitOk;") && has(fd, "gXdLed.replace = ledMid;"));
      expect("L485-W4 the pass state is reset once per frame and written nowhere else",
             count(s, "n48_gcap_cbt_reset(&gLedCbt)") == 1u && count(s, "n48_gcap_cbt_ib(dst, got, &gLedCbt)") == 1u &&
             count(s, "n48_dl_pgx_extend(") == 1u && count(s, "gXdLed.replace =") == 1u &&
             count(dec, "n48_dl_feed(&gXdLed, &lf)") == 1u); }
    { const std::string rc = body_of(s, "static uint32_t r5_resolve_cb(");
      expect("L485-W5 the resolver is gfxc_page over the frame's own vm at the page base - the IB-end rows' walker",
             has(rc, "const bool ok = gfxc_page(*cx->vm, va & ~0xfffull, p, sys);") && has(rc, "if (ok) *page = p;") &&
             has(dec, "hVa[hHeld] = it.va; hPage[hHeld] = ok ? page : 0u;")); }
}

static std::string gL485Src;
static void l485_wiring_mut() { checks_l485_wiring(gL485Src); }

// ---------------------------------------------------------------------------------------------------------------------------
// DCC60. build 0.0.488 (switch 60, notes/design/DCC-DESC.md Q2 option (A)) — THE ONLY PROOF A STRIPPED DCC T# MAY HAVE.
// gfx_desc_port.h n48_dl_dcc_ok, driven directly: the ledger (keyed as the caller keys it), then the frame-local list only
// while switch 45 is on, and nothing else - it takes no residency table at all. Then the report line's bound, then the
// kext's own wiring in source order (every block the design names, in the order the policy pass runs them).
// ---------------------------------------------------------------------------------------------------------------------------
static void checks_dcc60()
{
    static uint32_t oA[64], oB[64];
    n48_dl led, fl; std::memset(&led, 0, sizeof led); std::memset(&fl, 0, sizeof fl);
    const uint64_t CTX = 5u, VA_L = 0x401080000ull, VA_F = 0x400460000ull;
    expect("DCC60-1 an empty ledger and an empty frame-local list prove nothing (switch 45 on)",
           n48_dl_dcc_ok(&led, &fl, 1u, CTX, VA_L, 3u, 0ull, 0u) == N48_DL_DCC_NONE);
    const uint32_t nA = seg_cb0(oA, VA_L, 3u, 0), nB = seg_cb0(oB, VA_F, 3u, 0);
    expect("DCC60-2 the ledger holds the clock layer (0x401080000, mode 3) and the frame-local list the fp16 layer",
           n48_dl_from_output(&led, CTX, oA, nA, nullptr) == 1u && n48_dl_from_output(&fl, CTX, oB, nB, nullptr) == 1u);
    expect("DCC60-3 the ledger proves its entry (LEDGER), with switch 45 off as well",
           n48_dl_dcc_ok(&led, &fl, 1u, CTX, VA_L, 3u, 0ull, 0u) == N48_DL_DCC_LEDGER &&
           n48_dl_dcc_ok(&led, &fl, 0u, CTX, VA_L, 3u, 0ull, 0u) == N48_DL_DCC_LEDGER);
    expect("DCC60-4 the frame-local list proves its entry only while switch 45 is on (FRAMELOCAL / NONE)",
           n48_dl_dcc_ok(&led, &fl, 1u, CTX, VA_F, 3u, 0ull, 0u) == N48_DL_DCC_FRAMELOCAL &&
           n48_dl_dcc_ok(&led, &fl, 0u, CTX, VA_F, 3u, 0ull, 0u) == N48_DL_DCC_NONE);
    expect("DCC60-5 another mode, another context, another VA: not proven",
           n48_dl_dcc_ok(&led, &fl, 1u, CTX, VA_L, 2u, 0ull, 0u) == N48_DL_DCC_NONE &&
           n48_dl_dcc_ok(&led, &fl, 1u, CTX + 1u, VA_L, 3u, 0ull, 0u) == N48_DL_DCC_NONE &&
           n48_dl_dcc_ok(&led, &fl, 1u, CTX, VA_L + 0x10000u, 3u, 0ull, 0u) == N48_DL_DCC_NONE);
    expect("DCC60-6 the physical-page key applies at the ledger exactly as gfxsrc_desc_tiled_ok's ask (an entry without a "
           "page proves nothing under the key)",
           n48_dl_dcc_ok(&led, &fl, 0u, CTX, VA_L, 3u, 0x13be3000ull, 1u) == N48_DL_DCC_NONE && led.keyNoPage >= 1u);
    { n48_dl both; std::memset(&both, 0, sizeof both); (void)n48_dl_from_output(&both, CTX, oB, nB, nullptr);
      expect("DCC60-7 the ledger is asked FIRST: a surface both lists hold answers LEDGER",
             n48_dl_dcc_ok(&both, &fl, 1u, CTX, VA_F, 3u, 0ull, 0u) == N48_DL_DCC_LEDGER); }
    expect("DCC60-8 no ledger and switch 45 off: nothing; context 0: nothing",
           n48_dl_dcc_ok(nullptr, &fl, 0u, CTX, VA_F, 3u, 0ull, 0u) == N48_DL_DCC_NONE &&
           n48_dl_dcc_ok(&led, &fl, 1u, 0u, VA_L, 3u, 0ull, 0u) == N48_DL_DCC_NONE);
    {
        char line[1024];
        const int w = std::snprintf(line, sizeof line, N48_DL_DCC60_REPORT_FMT, "OFF (default)",
                                    "`gfxneuter 60` REFUSED - a continuous arm stands",
                                    " - INERT: the descriptor path (10) is off", ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull);
        std::printf("      dccstrip60 report line at widest numerics and longest strings: %d bytes\n", w);
        expect("DCC60-R the dccstrip60 report line is under 491 bytes at its maximum", w > 0 && w < 491);
    }
}
static void checks_dcc60_wiring(const std::string &raw)
{
    const std::string s = strip_comments(raw);
    expect("DCC60-W1 the switch is defined once, default 0", count(s, "static volatile uint32_t gDccStripOn { 0u };") == 1u);
    { const size_t vb = s.find("(arg & 0xffull) == 60ull");
      const std::string v = vb == std::string::npos ? "" : block_at(s, vb);
      expect("DCC60-W1 only the `gfxneuter 60` verb assigns it, behind the mid-arm guard, and prints the report line",
             count(s, "gDccStripOn =") == 1u && count(v, "gDccStripOn = fdc;") == 1u &&
             has(v, "n48_cm_cont_switch_refused(60u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") &&
             has(v, "if (contRefused60) st = 5;") && has(v, "dccstrip_report_line(")); }
    expect("DCC60-W1 the report line is the bounded N48_DL_DCC60_REPORT_FMT, once", count(s, "HWLOG(N48_DL_DCC60_REPORT_FMT,") == 1u);
    /* the callback: the ledger and the frame-local list through n48_dl_dcc_ok, never resprov */
    const std::string cb = body_of(s, "static int gfxsrc_desc_dcc_ok(");
    { const size_t pw = cb.find("if (c->vm && gXdDlPgKey) {");
      const size_t pa = cb.find("n48_dl_dcc_ok(&gXdLed, &gXdFrameLocal, gXdFrameLocalOn, c->ctx, va, mode, dccPage, gXdDlPgKey);");
      expect("DCC60-W2 gfxsrc_desc_dcc_ok computes the page key (only with 18 on) and THEN asks n48_dl_dcc_ok with the ledger, "
             "the frame-local list, switch 45 and the key",
             !cb.empty() && pw != std::string::npos && pa != std::string::npos && pw < pa); }
    expect("DCC60-W2 ... and nothing in it can reach the residency table (no n48_rp_ok*, no gXdRp, no gXdResProv, no tiled ask)",
           !cb.empty() && !has(cb, "n48_rp_ok") && !has(cb, "gXdRp") && !has(cb, "gXdResProv") && !has(cb, "gfxsrc_desc_tiled_ok") &&
           count(cb, "return") == 2u && has(cb, "return via != N48_DL_DCC_NONE ? 1 : 0;"));
    /* REACHABILITY, in gfxsrc_policy's own order: inside the descriptor path's block, TABLE_DESC is set, the callbacks are
     * handed over, the flag is set from the switch; the translate runs; the scoreboard reads the flag the translate saw. */
    const std::string pol = body_of(s, "static void gfxsrc_policy(");
    { const char *seq[] = {
          "if (dp) {",
          "ex.flags |= XLAT12_EXTRA_INLINE_DESC | XLAT12_EXTRA_TABLE_DESC | XLAT12_EXTRA_DESC_INV;",
          "ex.desc_tiled_ok = &gfxsrc_desc_tiled_ok;",
          "ex.desc_dcc_ok = &gfxsrc_desc_dcc_ok;",
          "if (gDccStripOn) { ex.flags |= XLAT12_EXTRA_DCC_STRIP; gDccStripS.segsOn++; }",
          "uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from], to - from,",
          "if (ex.flags & XLAT12_EXTRA_DCC_STRIP) {",
          "gDccStripS.stripped += ds.dcc_stripped; gDccStripS.unproven += ds.dcc_unproven; gDccStripS.refused += ds.dcc_refused;",
          "if (dp && gXdFrameLocalOn && st == 0u) {" };
      size_t prev = 0; bool ordered = !pol.empty();
      for (const char *t : seq) { const size_t p = pol.find(t, prev); if (p == std::string::npos) { ordered = false; break; } prev = p + 1u; }
      expect("DCC60-W3 REACHABILITY: in gfxsrc_policy the descriptor block sets TABLE_DESC, hands over both proofs, sets the "
             "flag from switch 60; the translate runs; the scoreboard reads it; then the frame-local feed - in that order", ordered); }
    { const size_t pd = pol.find("if (dp) {\n            // M4-DESC-KEXT-PORT (switch on only): the descriptor path.");
      const size_t pd2 = pd == std::string::npos ? pol.find("if (dp) {") : pd;
      const std::string blk = pd2 == std::string::npos ? "" : block_at(pol, pd2);
      expect("DCC60-W4 the flag and the callback are set INSIDE the descriptor path's own `if (dp)` block (with TABLE_DESC)",
             has(blk, "XLAT12_EXTRA_TABLE_DESC") && has(blk, "ex.desc_dcc_ok = &gfxsrc_desc_dcc_ok;") &&
             has(blk, "if (gDccStripOn) { ex.flags |= XLAT12_EXTRA_DCC_STRIP; gDccStripS.segsOn++; }"));
      expect("DCC60-W4 ... and nowhere else in the file", count(s, "XLAT12_EXTRA_DCC_STRIP; gDccStripS.segsOn++;") == 1u &&
             count(s, "ex.desc_dcc_ok = &gfxsrc_desc_dcc_ok;") == 1u && count(s, "ex.desc_dcc_ok =") == 1u); }
}
static std::string gDcc60Src;
static void dcc60_wiring_mut() { checks_dcc60_wiring(gDcc60Src); }

// ---------------------------------------------------------------------------------------------------------------------------
static int run_quiet(void (*fn)())
{
    const int f0 = gFail, r0 = gRun; gQuiet = 1; fn(); gQuiet = 0; const int d = gFail - f0; gFail = f0; gRun = r0; return d;   // mutant runs are not counted as checks
}
static DpFn gDp; static LedgerOps gOps; static std::string gSrc; static WhyFn gWhy; static LedOps2 gOps2;
static std::string gHdr;   // 0.0.393: gfx_cp_build.h's text, read beside the kext source
static DrainFn gDrn;
static KeepFn gKeep;
static void ex_mut() { checks_exact(gOps2); checks_key(gOps2); checks_arm9(gOps2); checks_arm10(gOps2); }
static void drain_mut() { checks_drain(gDrn); }
static void keep_mut() { checks_keep(gKeep); }
static void why_mut() { checks_readout(gWhy); }
static void rd_mut() { checks_read(gDp); }
static void led_mut() { checks_ledger(gOps); }
static void src_mut() { checks_source(gSrc, gHdr); }

int main(int argc, char **argv)
{
    int caught = 0, planted = 0;
    std::printf("== A: the snapshot read\n");
    checks_read(&dp_real);
    const struct { const char *name; DpFn fn; } rm[] = { { "a short read counts as a read", &dp_mut_short_ok },
                                                        { "no length/alignment/range refusal", &dp_mut_no_range } };
    for (const auto &m : rm) {
        gDp = m.fn; const int d = run_quiet(&rd_mut); planted++; if (d) caught++;
        std::printf("  planted: %-58s %s (%d check(s) fail)\n", m.name, d ? "CAUGHT" : "MISSED", d);
    }
    std::printf("== B: the producer ledger\n");
    checks_ledger(LedgerOps { &feed_real, &tiled_real, &unmap_real });
    const struct { const char *name; LedgerOps ops; } lm[] = {
        { "the ledger accepts a non-TRANSLATE frame", { &feed_no_verdict, &tiled_real, &unmap_real } },
        { "fed at TRANSLATE whatever the COMMIT gate said", { &feed_at_translate, &tiled_real, &unmap_real } },
        { "trusts gate == 0 without commit_ok", { &feed_gate_only, &tiled_real, &unmap_real } },
        { "keyed by VA alone (context ignored)", { &feed_real, &tiled_any_ctx, &unmap_real } },
        { "any mode proves the layout", { &feed_real, &tiled_any_mode, &unmap_real } },
        { "unmapVA does not clear", { &feed_real, &tiled_real, &unmap_noop } } };
    for (const auto &m : lm) {
        gOps = m.ops; const int d = run_quiet(&led_mut); planted++; if (d) caught++;
        std::printf("  planted: %-58s %s (%d check(s) fail)\n", m.name, d ? "CAUGHT" : "MISSED", d);
    }
    std::printf("== D-FL: build 0.0.448 item 3 - frame-local provenance (switch 45)\n");
    checks_frame_local();
    std::printf("== D: the per-judged-frame readout (notes 841)\n");
    checks_readout(&why_real);
    checks_line();
    const struct { const char *name; WhyFn fn; } wm[] = {
        { "every empty ledger gets ONE answer", &why_one_answer },
        { "an empty ledger reads as CLEARED (never-fed is invisible)", &why_empty_is_cleared },
        { "NEVER-FED decided from `added`, not `feeds`", &why_neverfed_from_added },
        { "the unmap is tested before the clear", &why_unmap_first },
        { "HOLDING is not decided first", &why_holding_last } };
    for (const auto &m : wm) {
        gWhy = m.fn; const int d = run_quiet(&why_mut); planted++; if (d) caught++;
        std::printf("  planted: %-58s %s (%d check(s) fail)\n", m.name, d ? "CAUGHT" : "MISSED", d);
    }
    std::printf("== E: ledger correctness - the exact drop and the physical-page key (notes 851)\n");
    checks_exact(LedOps2 { &unmap2_real, &ask2_real });
    checks_key(LedOps2 { &unmap2_real, &ask2_real });
    checks_predicate();
    checks_arm9(LedOps2 { &unmap2_real, &ask2_real });
    checks_arm10(LedOps2 { &unmap2_real, &ask2_real });
    const struct { const char *name; LedOps2 ops; } em[] = {
        { "an unmap drops an entry it does not cover (the range ignored)", { &unmap_mut_ignores_range, &ask2_real } },
        { "an unmap KEEPS one it does cover (covers == same base VA)", { &unmap_mut_base_only, &ask2_real } },
        { "an unknown scope keeps the entry (fail OPEN)", { &unmap_mut_unknown_keeps, &ask2_real } },
        { "the exact drop acts at the DEFAULT too (no A/B)", { &unmap_mut_always_exact, &ask2_real } },
        { "a re-mapped page is answered as if unchanged", { &unmap2_real, &ask_mut_ignores_page } },
        { "a missing page counts as a matching key", { &unmap2_real, &ask_mut_missing_ok } },
        { "the key refuses EVERYTHING (vacuous the other way)", { &unmap2_real, &ask_mut_refuses_all } },
        { "the key acts at the DEFAULT too (no A/B)", { &unmap2_real, &ask_mut_always_key } },
        /* 0.0.382: the one-sided rule keeping an entry it must drop - the withdrawn second disjunct, restored
         * over the one-page extent invented. arm9's [0x400240000,+0x40000) is 64 pages, so this KEEPS what it must drop. */
        { "the one-sided rule keeps an entry it must drop (the withdrawn page extent)",
          { &unmap_mut_page_extent, &ask2_real } },
        /* 0.0.383 - breaking the REAL overlap test. The first is the defect arm10 measured, restored deliberately. */
        { "the entry's extent is carried and then NOT consulted (0.0.382's rule)", { &unmap_mut_extent_ignored, &ask2_real } },
        { "a missing extent read as one page (fail OPEN)", { &unmap_mut_extent_open, &ask2_real } },
        { "an off-by-one at the entry's far boundary", { &unmap_mut_extent_offbyone, &ask2_real } } };
    for (const auto &m : em) {
        gOps2 = m.ops; const int d = run_quiet(&ex_mut); planted++; if (d) caught++;
        std::printf("  planted: %-58s %s (%d check(s) fail)\n", m.name, d ? "CAUGHT" : "MISSED", d);
    }
    std::printf("== F: the deferred drain (notes 854)\n");
    checks_drain(&drain_real);
    checks_cap();
    const struct { const char *name; DrainFn fn; } dm[] = {
        { "a drained event applied to the WRONG context (every one)", &drain_mut_wrong_ctx },
        { "a ring wrap does not fail closed", &drain_mut_wrap_open },
        { "a WindowServer-wide clear is drained as a no-op", &drain_mut_ws_ignored } };
    for (const auto &m : dm) {
        gDrn = m.fn; const int d = run_quiet(&drain_mut); planted++; if (d) caught++;
        std::printf("  planted: %-58s %s (%d check(s) fail)\n", m.name, d ? "CAUGHT" : "MISSED", d);
    }
    std::printf("== K: keep across a covering unmap (notes 904)\n");
    checks_keep(&keep_real);
    { gKeep = &keep_mut_ignores; const int d = run_quiet(&keep_mut); planted++; if (d) caught++;
      std::printf("  planted: %-58s %s (%d check(s) fail)\n", "the keep switch is read and then IGNORED", d ? "CAUGHT" : "MISSED", d); }
    std::printf("== L: the ledger UN-FEED (C5 part 1, notes/design/C5-CONTINUOUS.md Q4)\n");
    {
        n48_dl l {};
        n48_dl_ent &e0 = l.e[0]; e0.ctx = 1ull; e0.va = 0x1000ull; e0.page = 0x2000ull; e0.mode = 1u; e0.tok = 10u;
        n48_dl_ent &e1 = l.e[1]; e1.ctx = 1ull; e1.va = 0x1100ull; e1.page = 0x2100ull; e1.mode = 1u; e1.tok = 10u;
        n48_dl_ent &e2 = l.e[2]; e2.ctx = 2ull; e2.va = 0x1200ull; e2.page = 0x2200ull; e2.mode = 1u; e2.tok = 11u;
        l.n = 3u;
        expect("L1 unfeed(tok 0) refuses - 0 is never a producer identity", n48_dl_unfeed_tok(&l, 0u) == 0u);
        expect("L2 ... and removes nothing", l.n == 3u);
        expect("L3 unfeed(tok 10) removes exactly the two entries that tok fed", n48_dl_unfeed_tok(&l, 10u) == 2u);
        expect("L4 the ledger now holds one entry", l.n == 1u);
        expect("L5 ... and it is the SURVIVING entry (tok 11, untouched)", l.e[0].tok == 11u && l.e[0].va == 0x1200ull);
        expect("L6 unfeedAsks counted", l.unfeedAsks == 1u);
        expect("L7 unfeedRemoved counted (summed)", l.unfeedRemoved == 2u);
        expect("L8 a second unfeed for a tok no longer present removes nothing (not an error)",
               n48_dl_unfeed_tok(&l, 10u) == 0u);
        expect("L9 ... the survivor is still there", l.n == 1u && l.e[0].tok == 11u);
        expect("L10 unfeedAsks counts every call, even one that removes nothing", l.unfeedAsks == 2u);
        expect("L11 a null ledger is refused, not a crash", n48_dl_unfeed_tok(nullptr, 10u) == 0u);
    }
    // PLANTED BREAK: an "unfeed" that does nothing (the mechanism removed). Proves the checks above are non-vacuous.
    {
        n48_dl l {};
        n48_dl_ent &e0 = l.e[0]; e0.ctx = 1ull; e0.va = 0x1000ull; e0.tok = 10u; l.n = 1u;
        auto broken_unfeed = [](n48_dl *, uint32_t) -> uint32_t { return 0u; };   // the un-feed removed
        const uint32_t removed = broken_unfeed(&l, 10u);
        const bool thisCaught = (removed == 0u) && (l.n == 1u);   // the entry SURVIVES - exactly the bug
        std::printf("  planted: %-58s %s\n", "the ledger un-feed is REMOVED (entries from a withdrawn/mismatched/"
                    "never-run commit stay in the ledger)", thisCaught ? "CAUGHT (the real n48_dl_unfeed_tok, proven "
                    "above, removes them)" : "MISSED");
        planted++; if (thisCaught) caught++;
    }
    // 0.0.444 (C5-RING-REVIEW.md (B) item 9b, Q7b, CONFIRMED) — n48_dl_unmap_rng's COMPACTION MUST CARRY tok/arm_ep
    // WITH THE SURVIVING ENTRY. Two entries, ctx 1 (unmapped, dropped) and ctx 2 (survives, tok 11) - the survivor
    // compacts DOWN from slot 1 to slot 0, so this exercises the exact copy the review found missing.
    {
        n48_dl l {};
        n48_dl_ent &e0 = l.e[0]; e0.ctx = 1ull; e0.va = 0x1000ull; e0.page = 0x2000ull; e0.mode = 1u; e0.tok = 10u;
        e0.arm_ep = 5u;
        n48_dl_ent &e1 = l.e[1]; e1.ctx = 2ull; e1.va = 0x1200ull; e1.page = 0x2200ull; e1.mode = 1u; e1.tok = 11u;
        e1.arm_ep = 6u;
        l.n = 2u;
        n48_dl_unmap_rng(&l, 1ull, 0ull, 0ull, 0u);   // whole-context unmap of ctx 1 (exact=0): drops e0, compacts e1
        expect("item 9b: the survivor compacted down to slot 0", l.n == 1u && l.e[0].va == 0x1200ull);
        expect("item 9b: ... and CARRIED ITS OWN tok through the compaction", l.e[0].tok == 11u);
        expect("item 9b: ... and its arm_ep too", l.e[0].arm_ep == 6u);
        // PLANTED BREAK: 0.0.443's own compaction (ctx/va/page/mode/flags/size only - tok/arm_ep left at
        // whatever slot 0 held BEFORE this call, here 0/0 on a zero-initialised ledger).
        n48_dl broken {};
        n48_dl_ent &b0 = broken.e[0]; b0.ctx = 1ull; b0.va = 0x1000ull; b0.tok = 10u; b0.arm_ep = 5u;
        n48_dl_ent &b1 = broken.e[1]; b1.ctx = 2ull; b1.va = 0x1200ull; b1.tok = 11u; b1.arm_ep = 6u;
        broken.n = 2u;
        uint32_t w = 0u;
        for (uint32_t k = 0; k < broken.n; k++) {
            if (broken.e[k].ctx == 1ull) continue;   // dropped, matching the unmap above
            if (w != k) {
                broken.e[w].ctx = broken.e[k].ctx; broken.e[w].va = broken.e[k].va;
                // tok/arm_ep NOT copied - 0.0.443's actual omission.
            }
            w++;
        }
        broken.n = w;
        const bool thisCaught2 = (l.e[0].tok == 11u) && (broken.e[0].tok != 11u);
        std::printf("  planted: %-58s %s\n", "n48_dl_unmap_rng's compaction drops tok/arm_ep (0.0.443's omission)",
                    thisCaught2 ? "CAUGHT (0.0.444 carries tok through; the 0.0.443 model loses it)" : "MISSED");
        planted++; if (thisCaught2) caught++;
    }
    std::printf("== C: the kext's wiring (%s)\n", argc > 1 ? argv[1] : "NO SOURCE GIVEN");
    std::string raw;
    if (argc > 1) {
        if (FILE *fp = std::fopen(argv[1], "rb")) {
            char buf[65536]; size_t k;
            while ((k = std::fread(buf, 1, sizeof buf, fp)) > 0) raw.append(buf, k);
            std::fclose(fp);
        }
    }
    expect("C0 the kext source was read", raw.size() > 100000u);
    // 0.0.393: read the shared consumer builder beside the kext, so C22/C23 can pin the loops
    // where they now live and the planted edits below can reach them.
    {
        std::string hp = argv[1];
        const size_t sl = hp.find_last_of("/\\");
        hp = (sl == std::string::npos ? std::string() : hp.substr(0, sl + 1u)) + "gfx_cp_build.h";
        if (FILE *fp = std::fopen(hp.c_str(), "rb")) {
            char buf[65536]; size_t k;
            while ((k = std::fread(buf, 1, sizeof buf, fp)) > 0) gHdr.append(buf, k);
            std::fclose(fp);
        }
    }
    expect("C0b the shared consumer builder was read", gHdr.size() > 500u);
    checks_source(raw, gHdr);
    std::printf("== D-FLW: the frame-local mechanism's own wiring (source-order pins)\n");
    checks_frame_local_wiring(raw);
    std::printf("== D-RPC: build 0.0.450 item 3 (switch 47) - gfxsrc_desc_tiled_ok's carry wiring\n");
    checks_resprov_carry_wiring(raw);
    std::printf("== D-F2: build 0.0.452 item 2 (F2) - the resprov closing line's 512-byte budget\n");
    checks_resprov_line_budget(raw);
    std::printf("== D-F1: build 0.0.452 item 1 (F1) - the busy-lock unmap's bounded queue into gXdRp\n");
    checks_resprov_unmap_busy_queue(raw);
    std::printf("== L485: build 0.0.485 (switch 58) - the ledger fix: per-draw target pages (L1), re-shape REPLACE (L2)\n");
    checks_l485_l1();
    checks_l485_l2();
    checks_l485_safety();
    checks_l485_wiring(raw);
    {
        /* Non-vacuity of the wiring group: each mutant is a one-place edit of the loaded kext source, and must be CAUGHT. */
        const struct { const char *name, *from, *to; } lm485[] = {
            { "58: the switch ignored (ledMid reads the descriptor path alone)",
              "const uint32_t ledMid = (dp && gLedMidOn) ? 1u : 0u;", "const uint32_t ledMid = dp ? 1u : 0u;" },
            { "58: the switch defaults ON", "static volatile uint32_t gLedMidOn { 0u };",
              "static volatile uint32_t gLedMidOn { 1u };" },
            { "58: pages resolved for every judged frame, committed or not", "if (ledMid && commitOk) {", "if (ledMid) {" },
            { "58: the per-draw pass runs only under switch 54",
              "if (ledMid) (void)n48_gcap_cbt_ib(dst, got, &gLedCbt);",
              "if (ledMid && tvOn) (void)n48_gcap_cbt_ib(dst, got, &gLedCbt);" },
            { "58: the REPLACE mirrored ON whatever the switch says", "gXdLed.replace = ledMid;", "gXdLed.replace = 1u;" },
            { "58: the ledger fed BEFORE the map is extended", "        if (ledMid && commitOk) {",
              "        (void)n48_dl_feed(&gXdLed, &lf);\n        if (ledMid && commitOk) {" },
            { "58: a change of the switch does not move the ledger's epoch",
              "if (changed58) { gLedMidOn = flm; __atomic_fetch_add(&gXdDpEpoch, 1u, __ATOMIC_RELAXED); }",
              "if (changed58) { gLedMidOn = flm; }" },
            { "58: the mid-arm guard skipped",
              "const bool contRefused58 = n48_cm_cont_switch_refused(58u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;",
              "const bool contRefused58 = false;" },
            { "58: the per-draw pass placed before the frame-trust `continue`",
              "            if (got != len || f.ib[k].walk != len) continue;   // the verdict will stop here; do not scan a frame we cannot trust\n",
              "            if (ledMid) (void)n48_gcap_cbt_ib(dst, got, &gLedCbt);\n            if (got != len || f.ib[k].walk != len) continue;\n" } };
        for (const auto &m : lm485) {
            std::string mut = raw;
            const size_t p = mut.find(m.from);
            if (p == std::string::npos || mut.find(m.from, p + 1u) != std::string::npos) {
                expect("L485-M the mutant's anchor occurs exactly once in the kext source", false);
                continue;
            }
            mut.replace(p, std::strlen(m.from), m.to);
            gL485Src = mut;
            const int d = run_quiet(&l485_wiring_mut); planted++; if (d) caught++;
            std::printf("  planted: %-58s %s (%d check(s) fail)\n", m.name, d ? "CAUGHT" : "MISSED", d);
        }
    }
    std::printf("== DCC60: build 0.0.488 (switch 60) - the DCC T# strip's only proof, its report line and its wiring\n");
    checks_dcc60();
    checks_dcc60_wiring(raw);
    {
        const struct { const char *name, *from, *to; } md[] = {
            { "60: the stripped T#'s proof also asks resprov (gfxsrc_desc_tiled_ok)",
              "const uint32_t via = n48_dl_dcc_ok(&gXdLed, &gXdFrameLocal, gXdFrameLocalOn, c->ctx, va, mode, dccPage, gXdDlPgKey);",
              "const uint32_t via = gfxsrc_desc_tiled_ok(ctx, va, mode, elemBytes) ? 1u : n48_dl_dcc_ok(&gXdLed, &gXdFrameLocal, gXdFrameLocalOn, c->ctx, va, mode, dccPage, gXdDlPgKey);" },
            { "60: the frame-local list asked whatever switch 45 says",
              "n48_dl_dcc_ok(&gXdLed, &gXdFrameLocal, gXdFrameLocalOn, c->ctx, va, mode, dccPage, gXdDlPgKey);",
              "n48_dl_dcc_ok(&gXdLed, &gXdFrameLocal, 1u, c->ctx, va, mode, dccPage, gXdDlPgKey);" },
            { "60: the switch defaults ON", "static volatile uint32_t gDccStripOn { 0u };", "static volatile uint32_t gDccStripOn { 1u };" },
            { "60: the flag set whatever the switch says",
              "if (gDccStripOn) { ex.flags |= XLAT12_EXTRA_DCC_STRIP; gDccStripS.segsOn++; }",
              "{ ex.flags |= XLAT12_EXTRA_DCC_STRIP; gDccStripS.segsOn++; }" },
            { "60: the callback never handed over", "            ex.desc_dcc_ok = &gfxsrc_desc_dcc_ok;\n", "" },
            { "60: the mid-arm guard skipped",
              "const bool contRefused60 = n48_cm_cont_switch_refused(60u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;",
              "const bool contRefused60 = false;" },
            { "60: the page walk paid with the key switch OFF", "if (c->vm && gXdDlPgKey) {", "if (c->vm) {" } };
        for (const auto &m : md) {
            std::string mut = raw;
            const size_t p = mut.find(m.from);
            if (p == std::string::npos || mut.find(m.from, p + 1u) != std::string::npos) {
                expect("DCC60-M the mutant's anchor occurs exactly once in the kext source", false);
                continue;
            }
            mut.replace(p, std::strlen(m.from), m.to);
            gDcc60Src = mut;
            const int d = run_quiet(&dcc60_wiring_mut); planted++; if (d) caught++;
            std::printf("  planted: %-58s %s (%d check(s) fail)\n", m.name, d ? "CAUGHT" : "MISSED", d);
        }
    }
    const struct { const char *name, *from, *to; } sm[] = {
        { "desc_read bypasses gfxc_read_rs (reads the MM window directly)",
          "return gfxc_read_rs(*static_cast<const GfxcVm *>(vm), va, dst, n, nullptr, navi48_cg_active_recorder());",
          "return navi48_vram_read_mm(va, dst, n) ? n : 0u;" },
        { "desc_read calls gfxc_read directly, skipping n48_dp_read",
          "n48_dp_read(&gfxsrc_desc_gfxc, c->vm, va, ndw, out)",
          "(gfxc_read(*c->vm, va, out, ndw, nullptr) != 0u)" },
        { "the invalidate missing (policy without XLAT12_EXTRA_DESC_INV)",
          "XLAT12_EXTRA_INLINE_DESC | XLAT12_EXTRA_TABLE_DESC | XLAT12_EXTRA_DESC_INV;",
          "XLAT12_EXTRA_INLINE_DESC | XLAT12_EXTRA_TABLE_DESC;" },
        { "the switch defaults ON", "static volatile uint32_t gXdDescPort { 0u };", "static volatile uint32_t gXdDescPort { 1u };" },
        { "the callbacks wired outside `if (dp)`",
          "        if (dp) {\n            // M4-DESC-KEXT-PORT (switch on only): the descriptor path.",
          "        ex.desc_read = &gfxsrc_desc_read;\n        if (dp) {\n            // M4-DESC-KEXT-PORT (switch on only): the descriptor path." },
        { "the ledger fed without commit_ok", "lf.committed = commitOk;", "lf.committed = 1u;" },
        { "unmapVA no longer clears", "if (gXdDescPort) gfxsrc_desc_unmap(e->seq, va, size);", "(void)0;" },
        { "unmapVA clears by context only, not by range", "gfxsrc_desc_unmap(e->seq, va, size);", "gfxsrc_desc_unmap(e->seq, 0, 0);" },
        { "a context key for a record whose unmaps we do not hook",
          "(gVmCtx[i].state == 1 && gVmCtx[i].vtPatched)", "(gVmCtx[i].state == 1)" },
        // 0.0.379 - the readout itself.
        { "the readout is not capped (a line per judged frame, forever)",
          "if (dp && dpLines < ((c80_mode() && hw_cm_armed()) ? kXdDpLines80 : kXdDpLines)) {", "if (dp) {" },
        { "the readout runs with the descriptor path OFF",
          "if (dp && dpLines < ((c80_mode() && hw_cm_armed()) ? kXdDpLines80 : kXdDpLines)) {", "if (dpLines < ((c80_mode() && hw_cm_armed()) ? kXdDpLines80 : kXdDpLines)) {" },
        { "the readout repeats a stale gate reading",
          "const uint32_t depFresh = (gXdGateDep.frame == gXdC.judged + 1u) ? 1u : 0u;",
          "const uint32_t depFresh = 1u;" },
        { "the readout WRITES the ledger it is supposed to read",
          "            dpLines++;", "            dpLines++;\n            n48_dl_sync(&gXdLed, arm, dpEp);" },
        // 0.0.380 - the two ledger-correctness switches.
        { "the exact-range unmap defaults ON", "static volatile uint32_t gXdDlExact { 0u };",
          "static volatile uint32_t gXdDlExact { 1u };" },
        { "the physical-page key defaults ON", "static volatile uint32_t gXdDlPgKey { 0u };",
          "static volatile uint32_t gXdDlPgKey { 1u };" },
        { "the exact drop is hardwired on, whatever the switch says",
          "n48_dl_unmap_rng(&gXdLed, ctxSeq, va, size, gXdDlExact);",
          "n48_dl_unmap_rng(&gXdLed, ctxSeq, va, size, 1u);" },
        { "the unmap's range is threaded through and then thrown away",
          "n48_dl_unmap_rng(&gXdLed, ctxSeq, va, size, gXdDlExact);",
          "n48_dl_unmap_rng(&gXdLed, ctxSeq, 0, 0, gXdDlExact);" },
        { "the ask never keys (the un-keyed answer, whatever the switch says)",
          "n48_dl_tiled_ok_pg(&gXdLed, c->ctx, va, mode, askPage, gXdDlPgKey)",
          "n48_dl_tiled_ok(&gXdLed, c->ctx, va, mode)" },
        { "the ask pays the page walk with the key switch OFF",
          "if (gXdDlPgKey && c->vm) {", "if (c->vm) {" },
        { "the feed hands the ledger no pages (every entry keyless)",
          "lf.pg.va = hVa; lf.pg.page = hPage; lf.pg.n = hHeld;", "(void)0;" },
        { "`dep_ok` computed by the readout's own check, not n48_dep_ok",
          "c.dep_ok = n48_dep_ok(&dw);",
          "c.dep_ok = (n48_dep_check(&dw, &gXdGateDep.detail) == N48_DEP_OK) ? 1u : 0u;" },
        { "the sync no longer brackets `n` (the pre- statement)",
          "if (dp) { dpEp = gXdDpEpoch; ledBefore = gXdLed.n; n48_dl_sync(&gXdLed, arm, dpEp); ledAfter = gXdLed.n; }",
          "if (dp) n48_dl_sync(&gXdLed, arm, gXdDpEpoch);" },
        { "the observer set is not the gate's world's",
          "gXdGateDep.observers  = dw.observers;", "gXdGateDep.observers  = 0u;" },
        /* 0.0.382 — THE ONE MISTAKE THAT MAKES THE WHOLE CHANGE WORTHLESS: the deferred branch keeps the
         * wholesale epoch bump, so every context is still wiped at the next frame's sync and the drain has nothing to do. */
        { "a drain that leaves the busy branch's EPOCH BUMP in place",
          "gXdDp.unmapBusy++;\n        gXdDrn.deferred++;",
          "gXdDp.unmapBusy++;\n        __atomic_fetch_add(&gXdDpEpoch, 1u, __ATOMIC_RELAXED);\n        gXdDrn.deferred++;" },
        { "the judged frame never drains, so an ask crosses an unabsorbed unmap",
          "if (dp && gXdDlDrain) xd_led_drain_locked();", "(void)0;" },
        /* 0.0.391 ( conditions 2, 3, 4, 6) — one plausible way to get each of the four wrong. */
        { "R5's `complete` ignores the 64-item scan cap ( gap A, the fail-open)",
          "cf.complete = (walkOk && pgmOk && !scanOverFrame && !cf.has_dispatch && cf.ntgt <= N48_CP_TGT_MAX) ? 1u : 0u;",
          "cf.complete = (walkOk && pgmOk && !cf.has_dispatch && cf.ntgt <= N48_CP_TGT_MAX) ? 1u : 0u;" },
        { "the scan cap is counted for the boot but never for the frame",
          "                scanOverFrame = 1u;                           // 0.0.391: and per FRAME, for R5\n",
          "" },
        { "the vertex ABI pointer pages are gathered and then dropped (0.0.390's dead rows)",
          "c->ptr[c->nptr++] = ds->in_vptr[q];", "(void)q;" },
        { "an unknown VERTEX ABI row is waved through while an unknown fragment one refuses",
          "!ds->in_ptr_known || !ds->in_vptr_known", "!ds->in_ptr_known" },
        { "an inherited declared pointer is folded back into `list-overflow`",
          "c->ptr_inherit = ds->in_ptr_inherit;",
          "c->over = (c->over || ds->in_ptr_inherit) ? 1u : 0u;" },
        /* 0.0.393 ( conditions 1 and 4) — one way to get each wrong. */
        { "the kext stops calling the shared consumer builder (the list is empty)",
          "n48_cp_build_consumer(&gXpIn, &ds);", "(void)0;" },
        { "the note never computes the out-of-scope reading (every frame stays in scope)",
          "cf.out_of_scope = n48_cp_scope_out(", "(void)0; // n48_cp_scope_out(" },
        { "the ONE-SHOT SPENT line says `budget 1, exhausted` whatever the arm's budget is",
          "{ const uint32_t bud = n48_cm_shot_budget_of(&gXdShot), left = n48_cm_shot_left(&gXdShot);",
          "{ const uint32_t bud = 1u, left = 0u;" },
        { "the feed does not say whether OUR 8-target cap truncated the rows",
          "lf.pg.full = (tgtN > hHeld) ? 1u : 0u;", "lf.pg.full = 0u;" },
        /* 0.0.394 — the keep-across-unmap switch, got wrong two ways in the kext wiring. */
        { "the keep-across-unmap switch defaults ON",
          "static volatile uint32_t gXdDlKeep { 0u };", "static volatile uint32_t gXdDlKeep { 1u };" },
        { "the keep switch is thrown but the ledger is NEVER told (the switch is inert)",
          "gXdLed.keep = gXdDlKeep;", "(void)0;" },
        /* 0.0.400 — THE FILL-COLOUR GATE, its two surviving clauses and the root reading, got
         * wrong in the kext wiring. The 0.0.399 bound-root clause is GONE, so its mutant is gone with it; the two
         * that replace it are G1's re-added refusal and G2's dropped `!e->withdrawn`. */
        { "the switch turns ON without asking the gate (the arena clauses are dropped)",
          "gfxsrc_fillcolor_gate(gfxsrc_fillcolor_bound_ctx())", "N48_FILL_GATE_OK" },
        { "the gate's ring-built clause is dropped",
          "if (!gRingMap.built || !gRingMap.vaBase) return N48_FILL_GATE_NO_RING;", "(void)0;" },
        { "the gate's vaBase clause is dropped (a moved arena is dereferenced)",
          "if (gReloc.vaBase && gReloc.vaBase != gRingMap.vaBase) return N48_FILL_GATE_VA_MOVED;", "(void)0;" },
        { "the root read becomes a NO_ROOT refusal again (G1's dropped clause, re-added)",
          "gXdFillColorS.rootLive += gfxsrc_fillcolor_root_written(boundCtx);",
          "if (!gfxsrc_fillcolor_root_written(boundCtx)) return 0xFEu;" },
        { "the root read drops `!e->withdrawn` (a withdrawn entry counts as ours)",
          "e->state == 1u && e->rootWritten != 0 && !e->withdrawn", "e->state == 1u && e->rootWritten != 0" },
        { "the policy is handed no binding (boundCtx is always none)",
          "wf->wsState == N48_WS_BOUND) ? wf->wsCtx : 0ull;", "0ull;" },
        { "the POLICY sets the flag without the gate (the switch's check is not re-asked)",
          "const uint32_t fgate = gfxsrc_fillcolor_gate(boundCtx);", "const uint32_t fgate = N48_FILL_GATE_OK;" },
        { "the F3 report is merged back into one HWLOG line",
          "HWLOG(\"fillcolor counts:", "// HWLOG(\"fillcolor counts:" },
        { "the gate refusals are not named in the report",
          "gate refused ring %llu va-moved %llu", "" },
        /* 0.0.401 — THE IDENTITY LUT, got wrong in the kext wiring. */
        { "the LUT switch defaults ON",
          "static volatile uint32_t gLutOn     { 0u };", "static volatile uint32_t gLutOn     { 1u };" },
        { "the rung's plane flag is always 1 (the fill is refused too)",
          "c.lut_plane  = gXdBuild.lutPlane;", "c.lut_plane  = 1u;" },
        { "the deferred work is run inline instead of on a kernel thread",
          "kernel_thread_start(&lutfill_thread, nullptr, &th)",
          "(th = nullptr, KERN_SUCCESS)" },
        { "the LUT write skips the destination guard (writes any VRAM)",
          "navi48_vram_apple_dest_check(voff, pbytes)", "0u" },
        { "the slot-4 record is read from the image heap's base, not slot 4",   /* build 0.0.553: the entry is li111_learn_off() */
          "gfxc_read(vm, imgVa + li111_learn_off(), rec, N48_LUT_RECORD_DWORDS, nullptr)",
          "gfxc_read(vm, imgVa, rec, N48_LUT_RECORD_DWORDS, nullptr)" },
        { "`ready` is set before the flush proves the write",
          /* build 0.0.551: the anchor carries the scope tag stored right before the flag */
          "    if (!navi48_hdp_flush_now()) return;\n    gLutD.hdp++;\n    gLutReadyScope = scope;   // build 0.0.551 (SHOULD 3): the tag, stored BEFORE the flag (both volatile: program order kept)\n    gLutReady = 1u;",
          "    gLutReady = 1u;\n    if (!navi48_hdp_flush_now()) return;\n    gLutD.hdp++;" },
        { "the record/producer cross-check is dropped",
          "if (gLutProdSeen && gLutProdVa != d.va) { gLutD.decodeRefused++; gLutD.lastWhy = N48_LUT_MISMATCH; return; }",
          "(void)0;" },
        { "the producer note runs regardless of the switch",
          "if (gLutOn) gfxsrc_lut_note_producer(gXdBuild.kind, hVa, hHeld);",
          "gfxsrc_lut_note_producer(gXdBuild.kind, hVa, hHeld);" },
        /* 0.0.402 — THE FIXES, each got wrong in one line. */
        { "H2 the learn gate drops the COMMIT arm / BOUND / plane-shape clauses (any frame teaches the LUT)",
          "arm == N48_SD_ARM_COMMIT && wsBound && n48_lut_plane_shape(ds.in_mode, ds.in_n) &&", "1u &&" },
        { "H2 the learn is not held to ONE record read per arm scope (retried per segment)",
          "gLutAttempted = 1u;", "(void)0;" },
        { "H4 the instrument's shape gate is dropped (any decodable record is written)",
          "n48_lut_instrument_ok(&d)", "N48_LUT_OK" },
        { "H3 the learn stops publishing the whole table (a partial table is written from)",
          "for (uint32_t p = 0u; p < np; p++) gLutTable[p] = tbl[p];", "gLutTable[0] = tbl[0];" },
        { "H3 the thread resolves a page table itself instead of writing from the table",
          "const uint64_t voff = gLutTable[p];",
          "uint64_t voff = 0ull; { uint64_t pg = 0ull; bool isSys = false; if (gfxc_page(gLutVm, d.va, pg, isSys)) voff = pg; }" },
        { "H3 the stale-table check before the write is dropped (a moved scope is written anyway)",
          "if (scope != gLutScope) { gLutD.stale++; gLutD.lastWhy = N48_LUT_STALE; return; }", "(void)0;" },
        { "H1 the ramp is no longer generated by the batch generator (a page-at-a-time shape returns)",
          "n48_lut_batch_page(&d, p, off, take, src)", "(src[0] = 0u, 1u)" },
        { "H5 gLutHave is made sticky again at the arm (the once-per-scope resets go dead)",
          "gLutHave = 0u; gLutAttempted = 0u; gLutKicked = 0u; gLutTableN = 0u;", "gLutKicked = 0u;" },
        /* 0.0.404 (a item 2,b) — THE FILL-SET RESERVATION, got wrong in the kext wiring. */
        { "the fill-set switch defaults the state ON at boot",
          "static n48_fs gFs {};", "static n48_fs gFs { 1u };" },
        { "the arm never OPENS the fill-set window (the reservation is inert)",
          "if (gFs.on || gFs.plane_on) n48_fs_open(&gFs);", "(void)0;" },
        { "the reserved fill is never recorded at the gate (the report's seqs stay 0)",
          "if (c.fs_reserve) { gFsPend.active = 1u; gFsPend.cb0 = tgtVa; }", "(void)0;" },
        /* 0.0.410 — the commit point and the first-commit skip, got wrong in the kext wiring. */
        { "K2 the reserved fill's commit is taken at the GATE again (a keystone-withdrawn commit counts)",
          "if (c.fs_reserve) { gFsPend.active = 1u; gFsPend.cb0 = tgtVa; }",
          "if (c.fs_reserve) (void)n48_fs85_commit(&gFs, &gFs85, tgtVa, gXdCmToken.seq);" },
        { "K2 the keystone's WITHDRAWN branch does not clear the intent (a withdrawn member commits)",
          "gFsPend.active = 0u;\n            gFsPlanePend = 0u;", "gFsPlanePend = 0u;" },
        { "K3 the first commit is not skipped under switch 33",
          "!n48_ts_skip_commit(gFs.on, (uint32_t)gXdCm.commits)", "1u" },
        /* 0.0.411 — the before-new-rung skip, got wrong in the kext wiring. 0.0.420 (P1/) adds a
         * second call for the second window's refusal and the region-moved neuter; the planted edit removes THAT one, so
         * the count drops to 1 and the C31 F2 assertion above fails. */
        { "F2 a frame a later rung will refuse is still sampled",
          "!n48_ts_skip_refused(fpWillRefuse, f828RegionWillNeuter)", "1u" },
        { "K3 the second slot is not offered by switch 33",
          "(gXpOn || gFs.on) ? N48_TS_SLOTS : 1u", "gXpOn ? N48_TS_SLOTS : 1u" },
        { "the gate is handed `fs_reserve` without the rule (every frame is a fill)",
          "c.fs_reserve = (fsStep == N48_FS_RESERVE) ? 1u : 0u;", "c.fs_reserve = 1u;" },
        { "the reservation step is replaced by a constant PASS (the rule never speaks)",
          "const uint32_t fsStep = fs85F.fs_step;",
          "const uint32_t fsStep = (uint32_t)N48_FS_PASS;" },
        { "the strict identification is never applied (the sticky identity is passed as a fill)",
          "fsEligible, gXdBuild.nsegPre, gXdBuild.fill, gXdBuild.plane, tgtVa,",
          "fsEligible, 1u, gXdBuild.fill, gXdBuild.plane, tgtVa," },
        { "the in-force fragment identity is never read (no frame is ever a reserved fill)",
          "if (gFs.on && stage == 0u && gfxsrc_id_is_fill((int)out->ps_id)) gXdBuild.fill = 1u;", "(void)0;" },
        { "the pre-divisor identified-fill flag is never set (the divisor has nothing to name)",
          "if (fid >= 0 && gfxsrc_id_is_fill(fid)) gXdFrameFill = 1u;", "(void)0;" },
        { "a reserved fill no longer bypasses the policy divisor (the twin fill is sampled away)",
          "!(arm == N48_SD_ARM_COMMIT && n48_fs_bypass_divisor(&gFs, gXdFrameFill, tgtVa)) &&", "" },
        { "the divisor bypass IGNORES the identified-fill flag (a non-fill bypasses again)",
          "n48_fs_bypass_divisor(&gFs, gXdFrameFill, tgtVa)", "n48_fs_bypass_divisor(&gFs, 1u, tgtVa)" },
        { "turning the switch OFF leaves `on` set (the default path is not 0.0.403's)",
          "gFs.on = 0u; n48_fs_clear(&gFs); changed = 1;", "changed = 1;" },
        /* 0.0.406 — THE INPUT-FREE FILL AND THE RETIREMENT, got wrong in the kext wiring. */
        { "L1 the kext stops building the input-free fill (every fill falls to source-neuter)",
          "n48_cp_build_input_free(&fi, gXdBuild.nsegPre, gXdBuild.fill, gXdFillHasTable, gXdFillNImg, gXdFillPtrVa,",
          "(void)0;" },
        /* 0.0.407 — THE SIXTH CONDITION AND THE RETIREMENT'S RUNG, got wrong in the kext wiring. */
        { "M1 the SIXTH condition is dropped (root[511] not yet written still enumerates input-free)",
          "gfxsrc_fillcolor_root_written(boundCtx), gXdBuild.segDraws)) {", "1u, gXdBuild.segDraws)) {" },
        /* 0.0.438 (FINDING 4 INTERIM REFUSAL) — the wiring, not gfx_cp_build.h's own pure function
         * (that one is host-tested directly in gfx_dep_test.cpp): forcing the draws argument to 1 at THIS call
         * site would let a multi-draw fill segment through regardless of what gXdBuild.segDraws actually holds. */
        { "FINDING4 the call site stops passing the frame's own draw count (always claims 1 draw)",
          "gfxsrc_fillcolor_root_written(boundCtx), gXdBuild.segDraws)) {", "gfxsrc_fillcolor_root_written(boundCtx), 1u)) {" },
        { "L1 cprov judges the fill with the WRONG rule (R1/R2/R4 asked of an input-free consumer)",
          "clause = n48_cp_eval_fill_hz(&cc, &gR5Ring, &unproven, &stale);",
          "clause = gR5On ? n48_cp_eval_hz(&cc, &gXpRing, &gXdDepArm, &gR5Ring, &unproven, &stale) : "
          "n48_cp_eval(&cc, &gXpRing, &gXdDepArm, &unproven, &stale);" },
        { "L1 the pointer VA is taken from Apple's original instead of the retarget (an unnamed page)",
          "if (ds.fill_color_retargeted) gXdFillPtrVa = ds.fill_color_new;", "gXdFillPtrVa = 0ull;" },
        { "L1 the build is not gated on the fill identity (a non-fill frame can enumerate)",
          "if (gXpOn && gFs.on && gXdBuild.fill) {", "if (gXpOn && gFs.on) {" },
        { "N1 (0.0.408) the retirement is dropped (a member that did not go live never gives up)",
          "if (c.fs_reserve && !live) (void)n48_fs85_retire(&gFs, &gFs85, tgtVa, live ? 1u : 0u, !c.dep_ok, (uint32_t)gXdGateDep.reason, reason);", "(void)0;" },
        { "N1 (0.0.408) the retirement keys on the GATE reason again (the 0.0.407 defect: it never fires)",
          "if (c.fs_reserve && !live) (void)n48_fs85_retire(&gFs, &gFs85, tgtVa, live ? 1u : 0u, !c.dep_ok, (uint32_t)gXdGateDep.reason, reason);",
          "if (c.fs_reserve) (void)n48_fs85_retire(&gFs, &gFs85, tgtVa, live ? 1u : 0u, (reason == (uint32_t)N48_CM_DEP_STALE), "
          "(uint32_t)gXdGateDep.reason, reason);" },
        /* 0.0.409 — THE PAIR PRE-RESOLVE, got wrong in the kext wiring. */
        { "the pair-pre switch defaults ON", "static volatile uint32_t gXdPairPre  { 0u };",
          "static volatile uint32_t gXdPairPre  { 1u };" },
        { "the translator bit is handed in unconditionally (outside the switch)",
          "if (gXdPairPre) { ex.flags |= XLAT12_EXTRA_PAIR_PRE; gXdPairPreS.segsOn++; }",
          "{ ex.flags |= XLAT12_EXTRA_PAIR_PRE; if (gXdPairPre) gXdPairPreS.segsOn++; }" },
        { "the translator's pair counters are never accumulated",
          "gXdPairPreS.resolved += ds.pair_pre_resolved;", "(void)0;" },
        { "turning the switch OFF leaves the intent set (the default path is not 0.0.408's)",
          "gXdPairPre = 0u; changed = 1;", "changed = 1;" },
        { "the pairpre report drops the unresolved count (the fail-closed reading)",
          "(unsigned long long)gXdPairPreS.unresolved);", "0ull);" } };
    const std::string hdrRaw = gHdr;
    for (const auto &m : sm) {
        gSrc = raw; gHdr = hdrRaw;   // 0.0.393: a moved-loop mutant edits the header, the rest edit the kext
        std::string *dst = &gSrc;
        size_t p = gSrc.find(m.from);
        if (p == std::string::npos) { dst = &gHdr; p = gHdr.find(m.from); }
        if (p == std::string::npos) { std::printf("  planted: %-58s NOT APPLIED (anchor missing)\n", m.name); planted++; continue; }
        dst->replace(p, std::strlen(m.from), m.to);
        const int d = run_quiet(&src_mut); planted++; if (d) caught++;
        std::printf("  planted: %-58s %s (%d check(s) fail)\n", m.name, d ? "CAUGHT" : "MISSED", d);
    }
    // 0.0.390 ("Ledger additions: producer token seq + arm epoch per entry"). RECORDED, never read by
    // the ask: an entry that is present is already of this arm and this epoch, because n48_dl_sync clears the whole
    // ledger when either moves. What the pair buys is that a report can NAME the producer -'s item (b) was
    // "no log line prints the committing frame's ctxSeq" - and that a two-frame chain's two producers are told apart.
    std::printf("== J: the producer identity on each entry (notes 889)\n");
    {
        n48_dl l {};
        n48_dl_frame f {};
        uint32_t seg_ok[1] = { 0u };
        (void)seg_ok;
        // Feed one entry through the real feed, with a producer identity on the frame.
        static const uint32_t out[] = {
            0xC00A6900u, 0x00000C18u,                                     /* SET_CONTEXT_REG at 0x28c60 (CB_COLOR0_BASE) */
            0x00400800u, 0u, 0u, 0u, 0u, 0u, 0u,                          /* BASE ... */
            0x00003C00u, 0u, 0u,
            0xC0002D00u, 0x00000000u                                      /* DRAW_INDEX_AUTO */
        };
        (void)out;
        // The direct path: n48_dl_set records whatever the feed in progress declared.
        l.feedTok = 7u; l.feedArmEp = 5u;
        n48_dl_set(&l, 5ull, 0x400800000ull, 3u, 0x10030000ull, 0u, 0x800000ull);
        expect("J the entry carries its producer's token seq", l.n == 1u && l.e[0].tok == 7u);
        expect("J   and the arm epoch it was recorded under", l.e[0].arm_ep == 5u);
        uint32_t tok = 0u, ep = 0u;
        expect("J the producer is readable back by (ctx, va)", n48_dl_producer_of(&l, 5ull, 0x400800000ull, &tok, &ep) == 1);
        expect("J   token", tok == 7u);
        expect("J   epoch", ep == 5u);
        expect("J a surface nothing produced answers NO", n48_dl_producer_of(&l, 5ull, 0x404800000ull, &tok, &ep) == 0);
        expect("J   and hands back no identity", tok == 0u && ep == 0u);
        expect("J a null ledger answers NO", n48_dl_producer_of(nullptr, 5ull, 0x400800000ull, &tok, &ep) == 0);
        // 0.0.444 (C5-RING-REVIEW.md (B) item 9c, Q7c) — A SECOND producer of the SAME surface does NOT replace the
        // identity any more: the PREVIOUS producer's proof is kept. Through 0.0.443 the last writer replaced it,
        // which meant un-feeding a LATER, withdrawn frame (by its own tok) could delete an EARLIER, genuinely
        // committed fill's only proof from the ledger.
        // CONDUCTOR REVIEW OF 0.0.446 (item 5), build 0.0.447 — A6 FAIL-CLOSED: this "keep the first producer"
        // rule ONLY applies to a re-feed that describes the SAME shape (mode, page, size) - a re-feed whose SHAPE
        // DIFFERS is not merely ignored, it DROPS the entry (section J447 below is the planted break for that).
        // A re-feed with an UNCHANGED shape (the common case: the ledger is fed on every committed frame, whether
        // or not the surface actually changed) still only updates `flags`.
        l.feedTok = 9u; l.feedArmEp = 5u;
        n48_dl_set(&l, 5ull, 0x400800000ull, 3u, 0x10030000ull, 0u, 0x800000ull);   // SAME mode/page/size as the first feed
        expect("J a SAME-shape re-feed still holds ONE entry", l.n == 1u);
        expect("J   the FIRST producer's proof survives a same-shape re-feed", l.e[0].tok == 7u);
        expect("J   ... and its arm epoch too", l.e[0].arm_ep == 5u);
        expect("J   ... and its page/size, because the re-feed's page/size MATCH", l.e[0].page == 0x10030000ull && l.e[0].size == 0x800000ull);
        // PLANTED BREAK: 0.0.443's own rule (the last producer overwrites). Un-feeding tok 9 (a later, withdrawn
        // frame that merely re-touched this surface) would then delete tok 7's real, still-valid proof with it.
        {
            n48_dl broken {};
            broken.feedTok = 7u; broken.feedArmEp = 5u;
            n48_dl_set(&broken, 5ull, 0x400800000ull, 3u, 0x10030000ull, 0u, 0x800000ull);
            broken.e[0].tok = broken.feedTok = 9u;   // 0.0.443's overwrite, modelled directly
            const uint32_t removed = n48_dl_unfeed_tok(&broken, 9u);   // un-feeding the LATER (withdrawn) frame
            const bool thisCaught = (l.e[0].tok == 7u) && (removed == 1u) && (broken.n == 0u);
            std::printf("  planted: %-58s %s\n", "n48_dl_set overwrites tok on a re-feed (0.0.443's rule)",
                        thisCaught ? "CAUGHT (0.0.444 keeps tok 7; the 0.0.443 model loses the entry to tok 9's un-feed)"
                                   : "MISSED");
            planted++; if (thisCaught) caught++;
        }
        // THE PROPERTY THAT MATTERS: the ask does not read either field. Same entry, a producer identity that is
        // nonsense, same answer.
        l.e[0].tok = 0xFFFFFFFFu; l.e[0].arm_ep = 0xFFFFFFFFu;
        expect("J the ask is UNCHANGED by the producer identity",
               n48_dl_tiled_ok_pg(&l, 5ull, 0x400800000ull, 3u, 0x10030000ull, 1u) == 1);
        // A feed the gate REFUSES never touches the in-progress identity (it returns above the assignment)...
        f.committed = 0u;
        (void)n48_dl_feed(&l, &f);
        expect("J a refused feed sets no producer identity", l.feedTok == 9u && l.feedArmEp == 5u);
        // ... and a feed that runs sets it from the FRAME and clears it again, so nothing written outside a feed can
        // ever inherit another frame's producer.
        static const n48_cm_seg segs[1] = { { 0u, 0u, 0u, 0u, 0u } };
        static const uint32_t obody[4] = { 0u, 0u, 0u, 0u };
        f.committed = 1u; f.gate = N48_CM_OK; f.verdict = N48_XV_TRANSLATE;
        f.ctx = 5ull; f.nseg = 0u; f.n = 4u; f.seg = segs; f.out = obody;
        f.tok = 11u; f.arm_ep = 6u;
        (void)n48_dl_feed(&l, &f);
        expect("J after a feed the in-progress identity is cleared", l.feedTok == 0u && l.feedArmEp == 0u);
    }
    // =================================================================================================================
    // J446 — 0.0.446 ( fix (6)): A SAME-SHAPE RE-FEED KEEPS THE FIRST PRODUCER'S page, mode AND size.
    // Fill A (tok 7) commits a surface at mode 3 / page P1 / size S1; a later frame B (tok 9) re-feeds the SAME
    // (ctx, va) at the SAME mode/page/size and is then WITHDRAWN (keystone refusal, token mismatch or NOPed), so
    // its un-feed runs by tok 9. The entry's tok is still 7 (0.0.444's own fix), so the un-feed removes nothing,
    // and the entry correctly still describes A's real, committed surface.
    // =================================================================================================================
    {
        n48_dl l446 {};
        l446.feedTok = 7u; l446.feedArmEp = 5u;
        n48_dl_set(&l446, 5ull, 0x400800000ull, 3u, 0x10030000ull, 0u, 0x800000ull);   // A: mode 3, P1, S1
        l446.feedTok = 9u; l446.feedArmEp = 5u;
        n48_dl_set(&l446, 5ull, 0x400800000ull, 3u, 0x10030000ull, 0u, 0x800000ull);   // B: the SAME shape
        (void)n48_dl_unfeed_tok(&l446, 9u);                                             // B withdrawn
        expect("J446 one entry, still A's token", l446.n == 1u && l446.e[0].tok == 7u);
        expect("J446 A's mode, page and size stand (3 / 0x10030000 / 0x800000)",
               l446.e[0].mode == 3u && l446.e[0].page == 0x10030000ull && l446.e[0].size == 0x800000ull);
        expect("J446 an ask at A's own (mode 3, page P1) is PROVEN - A really drew it",
               n48_dl_tiled_ok_pg(&l446, 5ull, 0x400800000ull, 3u, 0x10030000ull, 1u) == 1);
    }
    // =================================================================================================================
    // J447 — CONDUCTOR REVIEW OF 0.0.446 (item 5), build 0.0.447 — A6 FAIL-CLOSED: A DIFFERING-SHAPE RE-FEED
    // DROPS THE ENTRY, IMMEDIATELY, AT THE RE-FEED ITSELF (not deferred to some later un-feed). Fill A (tok 7)
    // commits mode 3 / P1 / S1; frame B (tok 9) re-feeds the SAME (ctx, va) at a DIFFERENT mode 5 / P2 / S2 (a
    // genuine reallocation or format change) - keeping A's fields under A's token while now claiming B's shape,
    // 0.0.446's own bug, would let an ask at (mode 5, P2) read PROVEN under A's proof for pixels A never wrote
    // there. The fix drops the entry outright: reFeedDropped counts it, and asks at EITHER shape refuse until a
    // fresh, single producer feeds this (ctx, va) again.
    // =================================================================================================================
    {
        n48_dl l447 {};
        l447.feedTok = 7u; l447.feedArmEp = 5u;
        n48_dl_set(&l447, 5ull, 0x400800000ull, 3u, 0x10030000ull, 0u, 0x800000ull);   // A: mode 3, P1, S1
        expect("J447 A's own feed holds one entry", l447.n == 1u && l447.reFeedDropped == 0u);
        l447.feedTok = 9u; l447.feedArmEp = 5u;
        n48_dl_set(&l447, 5ull, 0x400800000ull, 5u, 0x10070000ull, 0u, 0x900000ull);   // B: a DIFFERENT shape
        expect("J447 the DIFFERING-shape re-feed DROPS the entry immediately", l447.n == 0u && l447.reFeedDropped == 1u);
        expect("J447 an ask at A's own (mode 3, page P1) now REFUSES - the ledger holds nothing",
               n48_dl_tiled_ok_pg(&l447, 5ull, 0x400800000ull, 3u, 0x10030000ull, 1u) == 0);
        expect("J447 an ask at B's (mode 5, page P2) ALSO refuses - B's own feed was never independently proven",
               n48_dl_tiled_ok_pg(&l447, 5ull, 0x400800000ull, 5u, 0x10070000ull, 1u) == 0);
        // a mode-only difference (page/size unchanged) and a page-only difference (mode/size unchanged) each drop too
        n48_dl l447m {}; l447m.feedTok = 7u; n48_dl_set(&l447m, 5ull, 0x401000000ull, 3u, 0x11000000ull, 0u, 0x1000ull);
        l447m.feedTok = 9u; n48_dl_set(&l447m, 5ull, 0x401000000ull, 4u, 0x11000000ull, 0u, 0x1000ull);   // mode differs alone
        expect("J447 a MODE-ONLY difference drops too", l447m.n == 0u && l447m.reFeedDropped == 1u);
        n48_dl l447p {}; l447p.feedTok = 7u; n48_dl_set(&l447p, 5ull, 0x401000000ull, 3u, 0x11000000ull, 0u, 0x1000ull);
        l447p.feedTok = 9u; n48_dl_set(&l447p, 5ull, 0x401000000ull, 3u, 0x12000000ull, 0u, 0x1000ull);   // page differs alone
        expect("J447 a PAGE-ONLY difference drops too", l447p.n == 0u && l447p.reFeedDropped == 1u);
        // PLANTED BREAK (reviewer's own, item 5): the OLD 0.0.446 model - KEEP the entry (mode/page/size held at
        // A's, only flags follow) instead of DROPPING it - must FAIL this property: it would still PROVE an ask at
        // B's own differing shape is refused (correct, by accident: B's shape was never recorded) but would
        // WRONGLY continue to prove A's original shape as well AFTER a shape conflict was seen - the real fix
        // refuses BOTH once a conflict is seen, because the entry is gone; the broken "keep" model refuses only
        // B's and wrongly keeps proving A's. Modelled directly (mirrors 0.0.446's own code before this item).
        {
            n48_dl keep {};
            keep.feedTok = 7u; keep.feedArmEp = 5u;
            n48_dl_set(&keep, 5ull, 0x400800000ull, 3u, 0x10030000ull, 0u, 0x800000ull);
            // 0.0.446's own rule, modelled: only flags follow a re-feed; mode/page/size never change again.
            keep.e[0].flags = 0u;
            const bool oldModelStillProvesA = n48_dl_tiled_ok_pg(&keep, 5ull, 0x400800000ull, 3u, 0x10030000ull, 1u) == 1;
            // The REAL fix (l447, above) no longer proves A's shape after B's conflicting re-feed was seen.
            const bool realFixRefusesA = n48_dl_tiled_ok_pg(&l447, 5ull, 0x400800000ull, 3u, 0x10030000ull, 1u) == 0;
            const bool thisCaught = oldModelStillProvesA && realFixRefusesA;
            std::printf("  planted: %-58s %s\n", "keep (0.0.446) instead of drop on a differing re-feed",
                        thisCaught ? "CAUGHT (kept model still proves A's shape after a conflict was seen; the real fix does not)"
                                   : "MISSED");
            planted++; if (thisCaught) caught++;
        }
        // CONDUCTOR FOLLOW-UP (0.0.447): TWO TEST GAPS FOUND AGAINST THE BUILT KEXT, both in n48_dl_set's A6 fix.
        // J447s — A6s: a SIZE-ONLY mismatch (mode and page UNCHANGED, size alone differs) must drop too - the
        // comparison is `mode != mode || page != page || size != size`, and a mutant that drops the size clause
        // would miss exactly this case while every mode/page-differing test above still passes.
        n48_dl l447s {}; l447s.feedTok = 7u;
        n48_dl_set(&l447s, 5ull, 0x402000000ull, 3u, 0x13000000ull, 0u, 0x1000ull);          // A: mode 3, P, size 0x1000
        l447s.feedTok = 9u;
        n48_dl_set(&l447s, 5ull, 0x402000000ull, 3u, 0x13000000ull, 0u, 0x2000ull);          // B: SAME mode/page, size 0x2000
        expect("J447s a SIZE-ONLY difference drops too", l447s.n == 0u && l447s.reFeedDropped == 1u);
        expect("J447s an ask at the (now dropped) shape refuses", n48_dl_tiled_ok_pg(&l447s, 5ull, 0x402000000ull, 3u, 0x13000000ull, 1u) == 0);
        // J447c — A6c: the entry must be REMOVED BY COMPACTION (shifting later entries down), not just by
        // decrementing `l->n` in place. Three entries, X/Y/Z; Y (the MIDDLE one) gets a differing-shape re-feed.
        // A missing-compaction mutant leaves Y's STALE data standing at its own slot (still answering PROVEN for
        // Y's OLD shape - fails open) while `l->n--` alone silently drops Z (the LAST entry, unrelated and
        // correct) off the end - the exact "drops the LAST entry" shape the reviewer named.
        n48_dl l447c {};
        l447c.feedTok = 101u; n48_dl_set(&l447c, 5ull, 0x410000000ull, 3u, 0x20000000ull, 0u, 0x1000ull);   // X
        l447c.feedTok = 102u; n48_dl_set(&l447c, 5ull, 0x411000000ull, 3u, 0x21000000ull, 0u, 0x1000ull);   // Y (index 1, middle)
        l447c.feedTok = 103u; n48_dl_set(&l447c, 5ull, 0x412000000ull, 3u, 0x22000000ull, 0u, 0x1000ull);   // Z
        expect("J447c three entries recorded", l447c.n == 3u);
        l447c.feedTok = 109u;
        n48_dl_set(&l447c, 5ull, 0x411000000ull, 4u, 0x21000000ull, 0u, 0x1000ull);   // Y's re-feed: mode differs alone
        expect("J447c only the MIDDLE (Y) entry drops", l447c.n == 2u && l447c.reFeedDropped == 1u);
        expect("J447c an ask at Y's OWN (dropped) shape refuses - not left standing stale",
               n48_dl_tiled_ok_pg(&l447c, 5ull, 0x411000000ull, 3u, 0x21000000ull, 1u) == 0);
        uint32_t tokX = 0u, epX = 0u, tokZ = 0u, epZ = 0u;
        expect("J447c X (before Y) survives with its OWN producer (tok 101)",
               n48_dl_producer_of(&l447c, 5ull, 0x410000000ull, &tokX, &epX) == 1 && tokX == 101u);
        expect("J447c Z (after Y) survives with its OWN producer (tok 103) - not silently dropped off the end",
               n48_dl_producer_of(&l447c, 5ull, 0x412000000ull, &tokZ, &epZ) == 1 && tokZ == 103u);
        expect("J447c X's own (mode, page, size) still proves", n48_dl_tiled_ok_pg(&l447c, 5ull, 0x410000000ull, 3u, 0x20000000ull, 1u) == 1);
        expect("J447c Z's own (mode, page, size) still proves", n48_dl_tiled_ok_pg(&l447c, 5ull, 0x412000000ull, 3u, 0x22000000ull, 1u) == 1);
    }
    // =================================================================================================================
    // U446 — 0.0.446 ( fix (5)): AN UN-FEED QUEUE OVERFLOW CLEARS THE LEDGER (fail closed) AND IS COUNTED.
    // Nine withdrawn frames in one interval against an 8-deep queue: the ninth token is LOST. Its frame fed an entry;
    // under 0.0.444 that entry stayed standing as proof of pixels the withdrawn frame never drew.
    // =================================================================================================================
    {
        n48_dl lq {};
        n48_dl_unfeed_q q {};
        for (uint32_t t = 1u; t <= 9u; t++) {                 // nine producers, one surface each
            lq.feedTok = 100u + t; lq.feedArmEp = 1u;
            n48_dl_set(&lq, 5ull, 0x400000000ull + 0x1000000ull * t, 3u, 0x20000000ull + 0x10000ull * t, 0u, 0x1000ull);
        }
        lq.feedTok = 200u; lq.feedArmEp = 1u;                 // and one real, NEVER-withdrawn fill
        n48_dl_set(&lq, 5ull, 0x40a000000ull, 3u, 0x2a000000ull, 0u, 0x1000ull);
        for (uint32_t t = 1u; t <= 9u; t++) n48_dl_unfeed_queue(&q, 100u + t);   // all nine withdrawn
        expect("U446 the ninth token overflowed the 8-deep queue", q.overflow == 1u && q.lost == 1u && q.n == 8u);
        const uint32_t did = n48_dl_unfeed_drain(&lq, &q);
        expect("U446 the eight queued un-feeds were applied", did == 8u);
        expect("U446 FAIL CLOSED: the ledger is EMPTY after a drain that lost a token", lq.n == 0u);
        expect("U446 ... and the clear is counted (unfeedOverflowClears 1)", lq.unfeedOverflowClears == 1ull);
        expect("U446 ... and the queue's lost mark is consumed", q.lost == 0u && q.n == 0u);
        expect("U446 the lost frame's surface is NOT proven any more",
               n48_dl_tiled_ok_pg(&lq, 5ull, 0x400000000ull + 0x1000000ull * 9u, 3u, 0x20000000ull + 0x10000ull * 9u, 1u) == 0);
        // A drain with NOTHING lost does not clear: the real fill stays proven.
        n48_dl lk {};
        n48_dl_unfeed_q qk {};
        lk.feedTok = 200u; lk.feedArmEp = 1u;
        n48_dl_set(&lk, 5ull, 0x40a000000ull, 3u, 0x2a000000ull, 0u, 0x1000ull);
        lk.feedTok = 101u;
        n48_dl_set(&lk, 5ull, 0x401000000ull, 3u, 0x20010000ull, 0u, 0x1000ull);
        n48_dl_unfeed_queue(&qk, 101u);
        (void)n48_dl_unfeed_drain(&lk, &qk);
        expect("U446 control: no overflow -> only the withdrawn frame's entry goes, the real fill stays",
               lk.n == 1u && lk.e[0].tok == 200u && lk.unfeedOverflowClears == 0ull);
        // PLANTED MODEL: 0.0.444's drain (applies the queued eight, ignores the loss) - the lost frame stays proven.
        n48_dl lb {};
        n48_dl_unfeed_q qb {};
        for (uint32_t t = 1u; t <= 9u; t++) {
            lb.feedTok = 100u + t; lb.feedArmEp = 1u;
            n48_dl_set(&lb, 5ull, 0x400000000ull + 0x1000000ull * t, 3u, 0x20000000ull + 0x10000ull * t, 0u, 0x1000ull);
        }
        for (uint32_t t = 1u; t <= 9u; t++) n48_dl_unfeed_queue(&qb, 100u + t);
        for (uint32_t i = 0; i < qb.n; i++) (void)n48_dl_unfeed_tok(&lb, qb.tok[i]);   // 0.0.444's drain, modelled
        qb.n = 0u;
        const bool caughtU = n48_dl_tiled_ok_pg(&lb, 5ull, 0x400000000ull + 0x1000000ull * 9u, 3u,
                                                0x20000000ull + 0x10000ull * 9u, 1u) == 1 && lq.n == 0u;
        std::printf("  planted: %-58s %s\n", "0.0.444's drain ignores a lost un-feed token",
                    caughtU ? "CAUGHT (0.0.444 keeps the lost frame's proof; 0.0.446 clears the ledger)" : "MISSED");
        planted++; if (caughtU) caught++;
        char line[1024];
        const int w = std::snprintf(line, sizeof(line), N48_DL_UNFEED_REPORT_FMT, ~0ull, ~0ull, 0xFFFFFFFFu, 0xFFFFFFFFu, ~0ull);
        std::printf("      UN-FEED report line at widest numerics: %d bytes\n", w);
        expect("U446 the UN-FEED report line fits the 480-byte bound at widest numerics", w > 0 && w < 480);
    }
    std::printf("gfx_desc_port: %d check(s), %d failed; planted defects %d of %d caught\n", gRun, gFail, caught, planted);
    const bool pass = gFail == 0 && caught == planted;
    std::printf("%s\n", pass ? "N48-DESC-PORT-TEST-PASS" : "N48-DESC-PORT-TEST-FAIL");
    return pass ? 0 : 1;
}
