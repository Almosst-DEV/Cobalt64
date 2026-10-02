// gfx_mib_test.cpp — 0.0.421 (notes/design/MIB-COMMIT.md binding B10). THE MIB-0 CENSUS'S HOST PROOF.
//
// The classifier is the only part of MIB-0 that can be wrong without a hardware run: it turns an IB body's first dwords
// into HEAD / NOP-HEAD / MID / UNREAD, and's whole 50/27/29 boundary split depends on it. So this suite drives the
// REAL classifier (gfx_mib.h, the same header the kext compiles) over crafted bodies that carry each signature, checks
// the counter helpers and the report line, and then re-runs the classifier checks against planted defects — including
// the one warns about: a NOP-headed IB READ AS HEAD, which is exactly what a dword-0 test placed after the head
// test would do, since a NOP-headed body still carries the EVENT_WRITE 0xE at dword 10.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 -x c++ \
//         src/navi48-bringup/tests/gfx_mib_test.cpp src/xlat12/xlat12_ib.c src/xlat12/xlat12.c -o /tmp/mibtest \
//         && /tmp/mibtest [src/navi48-bringup/src/apple/AppleHardwareHook.cpp]
// 0.0.427 ( conditions (2) and (3)) needs xlat12_ib.c (n48_mib_segment calls xlat12_ib_segments) and, for the
// source pins, AppleHardwareHook.cpp as argv[1] (skipped, not failed, when absent).
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "gfx_mib.h"
#include "gfx_mibseg.h"                  // 0.0.446: the `mibseg:` per-segment status counter
#include "gfx_commit.h"                    // 0.0.427: the gate the real segment table must pass
#include "fixture_mib_f48_f20_f21.h"       // 0.0.427: F48/F20/F21's REAL IB bodies, generated from arm32's capture
#include "fixture_mib_nop_decide36b.h"     // 0.0.432: a SECOND, independent NOP-headed frame (decide36b)

static int gFail = 0, gRun = 0, gQuiet = 0, gFailOnly = 0;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet || gFailOnly) std::printf("FAIL  %-72s got %#llx want %#llx\n", what,
                                              (unsigned long long)got, (unsigned long long)want);
    } else if (!gQuiet) {
        std::printf("ok    %-72s %#llx\n", what, (unsigned long long)got);
    }
}

// ---- the bodies. Sized to hold the 12 dwords a head needs; the head's own five signatures are xlat12_ib.h's, so a
//      change to the recogniser's idea of a head changes this fixture too. ----
static void body_head(uint32_t *b)
{
    std::memset(b, 0, 16u * sizeof(uint32_t));
    b[0] = XLAT12_SEG_HEAD0; b[1] = XLAT12_SEG_HEAD1; b[2] = XLAT12_SEG_ACQUIRE;
    b[10] = XLAT12_SEG_HEAD0; b[11] = XLAT12_SEG_EVENT_E;
}
static void body_nop_head(uint32_t *b)
{
    //: dword 0 is the 10-dword NOP, and the EVENT_WRITE 0xE follows it at dword 10/11 — the reason the NOP test
    // must come FIRST in the classifier.
    body_head(b);
    b[0] = N48_MIB_NOP_HEAD; b[1] = 0x00000016u; b[2] = XLAT12_SEG_ACQUIRE;
}
static void body_mid(uint32_t *b)
{
    std::memset(b, 0, 16u * sizeof(uint32_t));
    b[0] = 0xC0012D00u; b[1] = 3u; b[2] = 2u;   // a mid-encoder SET/DRAW opening, not a head
}

// ---- the planted defects. ----
enum { MIB_MUT_NONE = 0, MIB_MUT_NOP_AS_HEAD, MIB_MUT_HEAD_IGNORED, MIB_MUT_UNREAD_AS_MID, MIB_MUT_COUNT };

static const char *mib_mut_name(int m)
{
    switch (m) {
    case MIB_MUT_NONE:           return "(the real classifier)";
    case MIB_MUT_NOP_AS_HEAD:    return "a NOP-headed IB reads as HEAD";
    case MIB_MUT_HEAD_IGNORED:   return "the head signature is never recognised";
    case MIB_MUT_UNREAD_AS_MID:  return "a too-short body is guessed MID";
    default:                     return "?";
    }
}

static uint32_t mib_class_mut(int m, const uint32_t *d, uint32_t n)
{
    if (m == MIB_MUT_NOP_AS_HEAD) {
        if (d && n >= 3u && d[0] == N48_MIB_NOP_HEAD) return N48_MIB_START_HEAD;
        return n48_mib_start_class(d, n);
    }
    if (m == MIB_MUT_HEAD_IGNORED) {
        const uint32_t c = n48_mib_start_class(d, n);
        return c == N48_MIB_START_HEAD ? N48_MIB_START_MID : c;
    }
    if (m == MIB_MUT_UNREAD_AS_MID) {
        const uint32_t c = n48_mib_start_class(d, n);
        return c == N48_MIB_START_UNREAD ? N48_MIB_START_MID : c;
    }
    return n48_mib_start_class(d, n);
}

// The classifier checks, run against `m` so a planted defect is scored by "was it caught".
static int mib_checks(int m)
{
    const int before = gFail;
    char buf[160];
    uint32_t b[16];

    body_head(b);
    std::snprintf(buf, sizeof(buf), "B10 a full encoder head -> HEAD            %s", mib_mut_name(m));
    expect_u(buf, mib_class_mut(m, b, 16u), N48_MIB_START_HEAD);

    body_nop_head(b);
    std::snprintf(buf, sizeof(buf), "B10 the 10-dword NOP -> NOP-HEAD           %s", mib_mut_name(m));
    expect_u(buf, mib_class_mut(m, b, 16u), N48_MIB_START_NOP_HEAD);

    body_mid(b);
    std::snprintf(buf, sizeof(buf), "B10 a mid-encoder opening -> MID           %s", mib_mut_name(m));
    expect_u(buf, mib_class_mut(m, b, 16u), N48_MIB_START_MID);

    // A head whose first three dwords match but whose tail does not is NOT a boundary the segmenter recognises.
    body_head(b); b[11] = 0u;
    std::snprintf(buf, sizeof(buf), "B10 head-like but wrong tail -> MID        %s", mib_mut_name(m));
    expect_u(buf, mib_class_mut(m, b, 16u), N48_MIB_START_MID);

    // Too short to hold a head: UNREAD, never guessed — and NULL is UNREAD, never a class.
    body_head(b);
    std::snprintf(buf, sizeof(buf), "B10 a head cut at 11 dwords -> UNREAD      %s", mib_mut_name(m));
    expect_u(buf, mib_class_mut(m, b, 11u), N48_MIB_START_UNREAD);
    std::snprintf(buf, sizeof(buf), "B10 a NULL body -> UNREAD                  %s", mib_mut_name(m));
    expect_u(buf, mib_class_mut(m, nullptr, 0u), N48_MIB_START_UNREAD);
    std::snprintf(buf, sizeof(buf), "B10 a two-dword body -> UNREAD             %s", mib_mut_name(m));
    expect_u(buf, mib_class_mut(m, b, 2u), N48_MIB_START_UNREAD);

    // The names are part of the report's contract (one grep must find a class).
    std::snprintf(buf, sizeof(buf), "B10 the class names are the report's       %s", mib_mut_name(m));
    expect_u(buf, (uint64_t)(std::strcmp(n48_mib_start_name(N48_MIB_START_HEAD), "HEAD") == 0 &&
                             std::strcmp(n48_mib_start_name(N48_MIB_START_NOP_HEAD), "NOP-HEAD") == 0 &&
                             std::strcmp(n48_mib_start_name(N48_MIB_START_MID), "MID") == 0 &&
                             std::strcmp(n48_mib_start_name(N48_MIB_START_UNREAD), "UNREAD") == 0) ? 1u : 0u, 1u);

    return gFail - before;
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.426 (MIB-COMMIT B1/B2/B3/B0, tests T4/T8/T9). The MIB commit's PURE arithmetic, over the F44 family's real shape
// (two IBs, 15,520 + 7,616 dwords, 7 + 5 = 12 segments; MIB-COMMIT). These are the three places a wrong answer names
// the WRONG IB or the WRONG segment, which no hardware run can catch because the frame would simply refuse or draw from
// the wrong page: `n48_mib_seg_va` (the segment's client VA), `n48_mib_want` (what the concatenated buffer can hold), and
// `n48_f828_offered` (which segment may carry the end-of-pipe fence). Each has a planted break, run below.
// ---------------------------------------------------------------------------------------------------------------------
static const uint32_t kF44N0 = 15520u, kF44N1 = 7616u, kF44Off1 = 15520u, kF44Nseg = 12u;
static_assert(kF44Off1 == kF44N0, "IB 1 lands exactly after IB 0 (B1's off_1 = len_0)");

// mutants: mu 1 = IB0's VA used for every segment (B2's plant); mu 2 = the cap ignored (B1's plant); mu 3 = every segment
// offered the fence (B3's "fence in IB0" plant); mu 4 = only the FIRST segment offered.
static uint64_t seg_va_mut(int mu, uint64_t ib_va, uint32_t from, uint32_t off)
{
    if (mu == 1) return ib_va + 4ull * (uint64_t)from;   // IB 0's VA used for IB 1's bytes
    return n48_mib_seg_va(ib_va, from, off);
}
static uint32_t want_mut(int mu, uint32_t len, uint32_t off, uint32_t cap)
{
    if (mu == 2) return len;                             // the cap ignored: the buffer is overrun and the short read hidden
    return n48_mib_want(len, off, cap);
}
static uint32_t offered_mut(int mu, uint32_t k, uint32_t nseg, uint32_t mib)
{
    if (mu == 3) return 1u;                              // the fence offered at every segment, including IB 0's
    if (mu == 4) return (k == 0u) ? 1u : 0u;             // only the FIRST segment, so the fence fires at IB 0's first draw
    return n48_f828_offered(k, nseg, mib);
}
static const char *mib_helper_mut_name(int mu)
{
    switch (mu) {
    case 1: return "IB 0's VA used for IB 1's bytes";
    case 2: return "the concatenation cap ignored";
    case 3: return "the fence offered at every segment (fence in IB0)";
    case 4: return "the fence offered only at the first segment";
    default: return "(the real helpers)";
    }
}

static int mib_commit_helper_checks(int mu)
{
    const int before = gFail;
    char b[160];

    // B2 — THE SEGMENT'S CLIENT VA. IB 1's first segment (from == 15,520) must use IB 1's VA; the last must add 4 per dword.
    const uint64_t va0 = 0x4002a0000ull;   // F44 IB0's VA family
    const uint64_t va1 = 0x4005a0000ull;   // F44 IB1's VA family
    std::snprintf(b, sizeof b, "B2 seg VA: IB 0's first segment uses IB 0's VA          %s", mib_helper_mut_name(mu));
    expect_u(b, seg_va_mut(mu, va0, 0u, 0u), va0);
    std::snprintf(b, sizeof b, "B2 seg VA: IB 1's first segment uses IB 1's VA          %s", mib_helper_mut_name(mu));
    expect_u(b, seg_va_mut(mu, va1, kF44Off1, kF44Off1), va1);
    std::snprintf(b, sizeof b, "B2 seg VA: a dword inside IB 1 shifts by 4             %s", mib_helper_mut_name(mu));
    expect_u(b, seg_va_mut(mu, va1, kF44Off1 + 100u, kF44Off1), va1 + 400ull);
    std::snprintf(b, sizeof b, "B2 seg VA: the result is never IB 0's VA for IB 1       %s", mib_helper_mut_name(mu));
    expect_u(b, seg_va_mut(mu, va1, kF44Off1 + 100u, kF44Off1) == va1 + 400ull ? 1u : 0u, 1u);

    // B1 — WHAT THE CONCATENATED BUFFER HOLDS, the T8 test. The F44 family fits (23,136 <= 32,768) at every offset; a
    // frame whose first IB is 20,000 leaves only 12,768 for the second, so that read is SHORT (IB_SHORT, detail 1).
    std::snprintf(b, sizeof b, "B1 want: F44's IB 1 fits at off 15,520                    %s", mib_helper_mut_name(mu));
    expect_u(b, want_mut(mu, kF44N1, kF44Off1, 32768u), kF44N1);
    std::snprintf(b, sizeof b, "T8 want: a 20,000+20,000 frame's IB 1 is SHORT           %s", mib_helper_mut_name(mu));
    expect_u(b, want_mut(mu, 20000u, 20000u, 32768u), 12768u);
    std::snprintf(b, sizeof b, "T8 want: the short read is < the IB's declared length   %s", mib_helper_mut_name(mu));
    expect_u(b, want_mut(mu, 20000u, 20000u, 32768u) < 20000u ? 1u : 0u, 1u);
    std::snprintf(b, sizeof b, "B1 want: off at/beyond the cap holds nothing (0)        %s", mib_helper_mut_name(mu));
    expect_u(b, want_mut(mu, 100u, 32768u, 32768u), 0u);

    // B3 — THE FENCE'S SEGMENT. OFF, every segment is offered (0.0.425's rule). ON, ONLY the frame's FINAL segment; the
    // last segment of IB 0 (k = 6) and every other earlier one is refused, so the fence can never fire early.
    std::snprintf(b, sizeof b, "B3 fence: OFF offers every segment                       %s", mib_helper_mut_name(mu));
    expect_u(b, offered_mut(mu, 0u, kF44Nseg, 0u) & offered_mut(mu, 6u, kF44Nseg, 0u) & offered_mut(mu, 11u, kF44Nseg, 0u), 1u);
    std::snprintf(b, sizeof b, "B3 fence: ON offers the frame's FINAL segment (k=11)     %s", mib_helper_mut_name(mu));
    expect_u(b, offered_mut(mu, kF44Nseg - 1u, kF44Nseg, 1u), 1u);
    std::snprintf(b, sizeof b, "T4 fence: ON refuses IB 0's LAST segment (k=6, a plant)  %s", mib_helper_mut_name(mu));
    expect_u(b, offered_mut(mu, 6u, kF44Nseg, 1u), 0u);
    std::snprintf(b, sizeof b, "T4 fence: ON refuses IB 1's earlier segments             %s", mib_helper_mut_name(mu));
    expect_u(b, offered_mut(mu, 7u, kF44Nseg, 1u) | offered_mut(mu, 8u, kF44Nseg, 1u), 0u);

    // T9 — THE SWITCH'S REPORT LINE IS ONE LINE UNDER THE LOGGER'S BODY CAP at its widest (a 10-digit frame count).
    {
        char line[512];
        const int w = std::snprintf(line, sizeof(line), N48_MIB_REPORT_FMT, "ON", " `gfxneuter 36` CHANGED it", 4294967295ull);
        std::snprintf(b, sizeof b, "T9 report: formats and is ONE line                        %s", mib_helper_mut_name(mu));
        expect_u(b, (w > 0 && std::strchr(line, '\n') == nullptr) ? 1u : 0u, 1u);
        std::snprintf(b, sizeof b, "T9 report: fits the 512 cap at widest numerics            %s", mib_helper_mut_name(mu));
        expect_u(b, (unsigned)w < 480u ? 1u : 0u, 1u);
    }
    return gFail - before;
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.427 ( condition (3)) — THE PURE SEGMENT STAGE (gfx_mib.h n48_mib_segment) OVER REAL CAPTURED BODIES.
// arm32's F48 is Family A: 15,520 + 7,616 dwords, a clean boundary, 7 + 5 segments. F20 is mid-encoder and F21 is
// NOP-headed; both must give 0 segments from the second IB, which refuses the whole frame. The fixture carries the real
// bodies (tests/fixture_mib_f48_f20_f21.h, generated from the capture; sha256 recorded there).
// ---------------------------------------------------------------------------------------------------------------------

// The gate frame the kext's gfxsrc_commit_try would build for a MIB frame whose real segmented table is `segs`. The
// per-IB rewrite evidence is set to the positive values (every dword written and read back host memory); this test is
// about the real table's TILING and the IB boundary, which is what the gate re-proves.
static void mib_frame_from(n48_cm_frame &c, const xlat12_ib_segment *segs, uint32_t ns,
                           const uint32_t *ib_n, uint32_t nib, uint32_t total)
{
    c = n48_cm_frame {};
    c.arm = N48_SD_ARM_COMMIT;
    c.verdict = N48_XV_TRANSLATE;
    c.buffers_ok = 1u;
    c.nib = nib; c.mib = 1u; c.n = total; c.cap = 32768u;
    c.nseg = ns;
    for (uint32_t k = 0; k < ns && k < N48_XV_MAX_SEGS; k++) {
        c.seg[k] = n48_cm_seg {};
        c.seg[k].head = segs[k].head; c.seg[k].start = segs[k].start; c.seg[k].end = segs[k].end;
        c.seg[k].out_len = segs[k].end - segs[k].start;
    }
    c.seg_kind = N48_CM_KIND_ENCODER;
    for (uint32_t k = 0; k < nib; k++) {
        c.ib_n[k] = ib_n[k];
        c.pages_ib[k] = (ib_n[k] * 4u + 0xFFFu) / 0x1000u;
        c.sys_pages_ib[k] = c.pages_ib[k];
        c.wrote_ib[k] = ib_n[k];
        c.got_ib[k] = ib_n[k];
        c.back_sys_pages_ib[k] = c.pages_ib[k];
        c.mismatch_ib[k] = 0u;
    }
    c.pages = c.pages_ib[0]; c.sys_pages = c.sys_pages_ib[0];
    c.wrote = c.wrote_ib[0]; c.got = c.got_ib[0];
    c.back_sys_pages = c.back_sys_pages_ib[0]; c.mismatch = c.mismatch_ib[0];
    c.token_ok = 1u; c.dep_ok = 1u;
}

// 0.0.429: WHICH IB OWNS SEGMENT k. The kext's inline loop (AppleHardwareHook.cpp, under gXdBuild.mib)
// advances the owner while the segment's start is at or past the current IB's end in the concatenated buffer. That loop
// is source-pinned (mib_source_pins, "PIN B2"); this model runs the same arithmetic over the REAL F48 table so the wrong
// owner - which would name the wrong client VA through n48_mib_seg_va and point a descriptor at the wrong page - is
// exercised, and `plant` 1 is the planted "every segment is IB 0's" defect the model must catch.
static uint32_t mib_seg_owner(uint32_t from, uint32_t nib, const uint32_t *ib_off, const uint32_t *ib_n, int plant)
{
    if (plant == 1) return 0u;                    // the plant: IB 0 owns everything
    uint32_t segIb = 0u;
    while (segIb + 1u < nib && from >= ib_off[segIb] + ib_n[segIb]) segIb++;
    return segIb;
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.430 — THE SEGMENT-STAGE REFUSAL REASONS OVER THE REAL BODIES, WITH PLANTED BREAKS.
// decide36's log could not say WHY all 36 two-IB policy runs answered 0: an IB that named no segment and a table that
// overflowed both leave `ns == 0`. The reason now travels with the count (n48_mib_seg_diag), and these checks drive the
// REAL stage over F48 (clean), F20 (mid-encoder IB1) and F21 (NOP-headed IB1), plus a deliberately one-row table for
// F48. Three plants each drop one reason; each must make at least one named check FAIL - the non-vacuity proof.
// ---------------------------------------------------------------------------------------------------------------------
static uint32_t mib_seg_diag_mut(int plant, const uint32_t *ib, uint32_t nib, const uint32_t *off, const uint32_t *len,
                                 xlat12_ib_segment *segs, uint32_t maxSegs, uint32_t *ib_nseg, uint32_t *tot,
                                 n48_mib_seg_diag *d)
{
    const uint32_t r = n48_mib_segment(ib, nib, off, len, segs, maxSegs, ib_nseg, tot, d, 0u, nullptr);
    if (plant == 1) for (uint32_t i = 0u; i < N48_MIB_STARTS; i++) d->zero_by_class[i] = 0u;   // the empty-IB reason dropped
    if (plant == 2) d->overflow = 0u;                                                          // the overflow reason dropped
    if (plant == 3) d->max_segs = 0u;                                                          // the high-water dropped
    return r;
}
static const char *mib_seg_plant_name(int plant)
{
    switch (plant) {
    case 1: return "the zero-segment IB's start class is never recorded";
    case 2: return "the segment-table overflow is never recorded";
    case 3: return "max segments seen is never recorded";
    default: return "(the real stage)";
    }
}

static int mib_seg_reason_checks(int plant)
{
    const int before = gFail;
    xlat12_ib_segment segs[64]; uint32_t ib_nseg[4] = { 0, 0, 0, 0 }; uint32_t tot = 0u;
    const uint32_t off48[2] = { kF48MibIbs[0].off, kF48MibIbs[1].off };
    const uint32_t len48[2] = { kF48MibIbs[0].len, kF48MibIbs[1].len };
    const uint32_t off20[2] = { kF20MibIbs[0].off, kF20MibIbs[1].off };
    const uint32_t len20[2] = { kF20MibIbs[0].len, kF20MibIbs[1].len };
    const uint32_t off21[2] = { kF21MibIbs[0].off, kF21MibIbs[1].off };
    const uint32_t len21[2] = { kF21MibIbs[0].len, kF21MibIbs[1].len };
    n48_mib_seg_diag d {};
    char buf[176];

    // F48, the clean Family-A frame: no reason at all, and the high-water is the real 12.
    const uint32_t ns48 = mib_seg_diag_mut(plant, kF48Mib, kF48MibNib, off48, len48, segs, 64u, ib_nseg, &tot, &d);
    std::snprintf(buf, sizeof buf, "a F48 clean: 12 segments                                  %s", mib_seg_plant_name(plant));
    expect_u(buf, ns48, 12u);
    std::snprintf(buf, sizeof buf, "a F48 clean: NO zero-segment IB                           %s", mib_seg_plant_name(plant));
    expect_u(buf, d.zero_by_class[N48_MIB_START_HEAD] + d.zero_by_class[N48_MIB_START_NOP_HEAD] +
                  d.zero_by_class[N48_MIB_START_MID] + d.zero_by_class[N48_MIB_START_UNREAD], 0u);
    std::snprintf(buf, sizeof buf, "a F48 clean: NO table overflow                           %s", mib_seg_plant_name(plant));
    expect_u(buf, d.overflow, 0u);
    std::snprintf(buf, sizeof buf, "a F48 clean: max segments seen is the real 12            %s", mib_seg_plant_name(plant));
    expect_u(buf, d.max_segs, 12u);

    // F20 (mid-encoder IB1): the whole frame refuses, and IB 1's own class is what is counted.
    d = n48_mib_seg_diag {};
    const uint32_t ns20 = mib_seg_diag_mut(plant, kF20Mib, kF20MibNib, off20, len20, segs, 64u, ib_nseg, &tot, &d);
    std::snprintf(buf, sizeof buf, "a F20 mid-encoder: the frame is refused (0)               %s", mib_seg_plant_name(plant));
    expect_u(buf, ns20, 0u);
    std::snprintf(buf, sizeof buf, "a F20: IB1's empty result is counted MID                 %s", mib_seg_plant_name(plant));
    expect_u(buf, d.zero_by_class[N48_MIB_START_MID], 1u);
    std::snprintf(buf, sizeof buf, "a F20: no other class is blamed                          %s", mib_seg_plant_name(plant));
    expect_u(buf, d.zero_by_class[N48_MIB_START_HEAD] + d.zero_by_class[N48_MIB_START_NOP_HEAD] +
                  d.zero_by_class[N48_MIB_START_UNREAD], 0u);
    std::snprintf(buf, sizeof buf, "a F20: no overflow (the IB was asked and named none)     %s", mib_seg_plant_name(plant));
    expect_u(buf, d.overflow, 0u);

    // 0.0.432: F21 MOVED OUT of this refusal suite. Through 0.0.431 its IB1 was an unexplained zero
    // (counted NOP-HEAD here); n48_mib_seg_disguised_head now explains it, so F21 SEGMENTS (mib_nop_disguise_checks,
    // below) and no longer belongs among "the segment-stage refusal reasons". (void)off21; (void)len21 keep this
    // function's F21 offset/length locals meaningful even though F21 itself moved.
    (void)off21; (void)len21;

    // TABLE OVERFLOW, on purpose: one row cannot hold F48's 12. The frame refuses, the overflow is named, and the IB
    // the table had no room for is NOT blamed as a zero-segment class (it was never asked). This is the distinction the
    // whole instrument exists for, and it is the ONLY case that reaches the second reason.
    d = n48_mib_seg_diag {};
    const uint32_t nsCap = mib_seg_diag_mut(plant, kF48Mib, kF48MibNib, off48, len48, segs, 1u, ib_nseg, &tot, &d);
    std::snprintf(buf, sizeof buf, "a 1-row table: the frame is refused (0)                  %s", mib_seg_plant_name(plant));
    expect_u(buf, nsCap, 0u);
    std::snprintf(buf, sizeof buf, "a 1-row table: the OVERFLOW is recorded                  %s", mib_seg_plant_name(plant));
    expect_u(buf, d.overflow, 1u);
    std::snprintf(buf, sizeof buf, "a 1-row table: ... not misread as a zero-segment IB      %s", mib_seg_plant_name(plant));
    expect_u(buf, d.zero_by_class[N48_MIB_START_MID], 0u);
    std::snprintf(buf, sizeof buf, "a 1-row table: the high-water is what the recogniser COULD count (7)  %s", mib_seg_plant_name(plant));
    expect_u(buf, d.max_segs, 7u);
    return gFail - before;
}

static int mib_real_bodies_checks(void)
{
    const int before = gFail;
    xlat12_ib_segment segs[64]; uint32_t ib_nseg[4] = { 0, 0, 0, 0 }; uint32_t tot = 0u;
    const uint32_t off48[2] = { kF48MibIbs[0].off, kF48MibIbs[1].off };
    const uint32_t len48[2] = { kF48MibIbs[0].len, kF48MibIbs[1].len };

    // ---- F48, the clean Family-A frame. ----
    const uint32_t ns48 = n48_mib_segment(kF48Mib, kF48MibNib, off48, len48, segs, 64u, ib_nseg, &tot, nullptr, 0u, nullptr);
    expect_u("C3 F48: the real 2-IB frame segments", ns48, 12u);
    expect_u("C3 F48: ... 7 segments in IB 0", ib_nseg[0], 7u);
    expect_u("C3 F48: ... 5 segments in IB 1", ib_nseg[1], 5u);
    expect_u("C3 F48: ... the recognisers' total equals the returned count", tot, 12u);
    expect_u("C3 F48: IB0's last segment ends at the boundary 15520", segs[6].end, kF48MibIbs[1].off);
    expect_u("C3 F48: ... and IB1's first segment head is that same dword", segs[7].head, kF48MibIbs[1].off);
    {
        uint32_t at = 0u, bad = 0u;
        for (uint32_t k = 0u; k < ns48; k++) {
            if (segs[k].head != at || segs[k].start != segs[k].head + 2u || segs[k].end <= segs[k].start) bad++;
            at = segs[k].end;
        }
        expect_u("C3 F48: every segment tiles head-to-head, ENCODER start", bad, 0u);
        expect_u("C3 F48: ... and the last segment ends at n", at, kF48Mib_dwords);
    }
    // THE GATE, on the REAL table.
    {
        n48_cm_frame c; mib_frame_from(c, segs, ns48, len48, kF48MibNib, kF48Mib_dwords);
        uint32_t d = 0u;
        expect_u("C3 F48: the real segment table COMMITS at the gate", n48_cm_gate(&c, &d), N48_CM_OK);
        // negative: a segment allowed to cross the IB boundary must refuse (COVER) - not merely COVER's own SEG_LEN -
        // so the gate's per-IB boundary check is what refuses it. out_len is moved with end so the length rung passes.
        n48_cm_frame k = c;
        k.seg[6].end += 4u; k.seg[6].out_len += 4u;
        expect_u("C3 F48 neg: a segment crossing the IB boundary -> COVER", n48_cm_gate(&k, &d), N48_CM_COVER);
        // 0.0.429: ISOLATE THE PER-IB RUNG. The negative above is refused by the GENERAL tiling loop at
        // dword 0 (`s->head != at`: segment 7's head is still 15520 while the running mark is 15524), so it does not
        // prove the per-IB rung at all. This frame TILES every head/end (the general loop and the SEG_KIND/SEG_LEN rungs
        // all pass) but a segment now SPANS the IB boundary at 15520: IB0's last segment is grown by one dword and IB1's
        // first head/start move with it. Only gfx_commit.h's per-IB COVER block can refuse this, and its detail is
        // (IB index << 16) | the offending end - here IB 0, 15524 - not the general loop's (k << 16) | head.
        n48_cm_frame iso = c;
        iso.seg[6].end += 4u; iso.seg[6].out_len = iso.seg[6].end - iso.seg[6].start;
        iso.seg[7].head += 4u; iso.seg[7].start += 4u; iso.seg[7].out_len = iso.seg[7].end - iso.seg[7].start;
        {
            uint32_t at2 = 0u, bad2 = 0u;
            for (uint32_t s2 = 0u; s2 < ns48; s2++) {
                if (iso.seg[s2].head != at2 || iso.seg[s2].start != iso.seg[s2].head + 2u ||
                    iso.seg[s2].out_len != iso.seg[s2].end - iso.seg[s2].start) bad2++;
                at2 = iso.seg[s2].end;
            }
            expect_u("C3 F48 neg-iso: the mutated table STILL TILES (the general check would pass)", bad2, 0u);
            expect_u("C3 F48 neg-iso: ... its last end is still n", at2, kF48Mib_dwords);
        }
        expect_u("C3 F48 neg-iso: a segment spanning the IB boundary -> COVER", n48_cm_gate(&iso, &d), N48_CM_COVER);
        expect_u("C3 F48 neg-iso: ... refused by the PER-IB rung (IB 0, its end 15524)", d, (0u << 16) | 15524u);
    }
    // 0.0.429: THE SEGMENT OWNER over the REAL table (the loop that names each segment's IB). F48's first
    // seven segments are IB0's and the last five IB1's; the owner must advance exactly at the boundary.
    {
        uint32_t owners[64], badOwner = 0u, badPlant = 0u;
        for (uint32_t k = 0u; k < ns48; k++)
            owners[k] = mib_seg_owner(segs[k].start, kF48MibNib, off48, len48, 0);
        for (uint32_t k = 0u; k < ns48; k++) {
            const uint32_t want = (k < 7u) ? 0u : 1u;
            if (owners[k] != want) badOwner++;
            if (mib_seg_owner(segs[k].start, kF48MibNib, off48, len48, 1) != owners[k]) badPlant++;
        }
        uint32_t ownerSum = 0u;
        for (uint32_t k = 0u; k < ns48; k++) ownerSum += owners[k];
        expect_u("B2 owner: every F48 segment is owned by the IB that contains it", badOwner, 0u);
        expect_u("B2 owner: ... five of the twelve segments are IB 1's", ownerSum, 5u);
        expect_u("B2 owner: ... the boundary falls between segment 6 (IB 0) and 7 (IB 1)",
                 owners[6] * 10u + owners[7], 1u);
        expect_u("B2 owner plant (IB 0 owns all): CAUGHT on IB 1's five segments", badPlant, 5u);
    }
    // THE FENCE: the last IB's final segment carries exactly ONE buried RELEASE_MEM, after its last draw; apply un-NOPs it.
    {
        const xlat12_ib_segment &fin = segs[ns48 - 1u];
        const uint32_t s = fin.start, e = fin.end;
        n48_f828 fr {};
        expect_u("C3 F48 fence: IB1's final segment has one buried RELEASE_MEM",
                 n48_f828_find(&kF48Mib[s], e - s, &fr), N48_F828_OK);
        expect_u("C3 F48 fence: ... exactly one candidate", fr.candidates, 1u);
        expect_u("C3 F48 fence: ... and it is the 9-dword NOP + 8-dword RELEASE_MEM",
                 (fr.nop_hdr == N48_F828_NOP9 && fr.rel_at == fr.nop_at + 1u && fr.next_at == fr.nop_at + 9u) ? 1u : 0u, 1u);
        static uint32_t scratch[8192];
        for (uint32_t i = 0u; i < e - s; i++) scratch[i] = kF48Mib[s + i];
        const uint64_t pageVa = 0x5000000ull, slotVa = pageVa;   // slot 0, inside our own fence page
        expect_u("C3 F48 fence: apply succeeds on the real packet",
                 n48_f828_apply(scratch, e - s, &fr, slotVa, pageVa, 0x12345678u), N48_F828_OK);
        uint32_t rel = 0u, relAt = 0u, walk = 1u;
        for (uint32_t i = 0u; i < e - s; ) {
            const uint32_t l = n48_f828_pkt_len(scratch[i]);
            if (!l || i + l > e - s) { walk = 0u; break; }
            if (scratch[i] == N48_F828_RELMEM) { rel++; relAt = i; }
            i += l;
        }
        expect_u("C3 F48 fence: the rewritten segment still walks end to end", walk, 1u);
        expect_u("C3 F48 fence: exactly ONE un-NOPed RELEASE_MEM after apply", rel, 1u);
        // draw_at is LOCAL to IB 1 (the recogniser's own frame), so the last draw's global dword is IB1's off + draw_at.
        const uint32_t lastDraw = kF48MibIbs[1].off + fin.draw_at;
        expect_u("C3 F48 fence: ... and it executes AFTER the segment's last draw",
                 (s + relAt > lastDraw) ? 1u : 0u, 1u);
    }
    // THE TRANSLATED FENCE (0.0.429): the block above runs find/apply on Apple's UNTRANSLATED bytes. The kext
    // translates the segment with xlat12_ib_translate_draw_ex (the frozen m2tri profile; no resolver or descriptor path,
    // exactly MIB-COMMIT's probe (b)) and only then offers and applies the fence, and the translator NOP-fills and
    // shrinks the packets before a draw. Repeat the find and the apply on ITS output: the result must be the same shape,
    // giving exactly one un-NOPed RELEASE_MEM in the last IB's final segment AFTER its last draw.
    {
        const xlat12_ib_segment &fin = segs[ns48 - 1u];
        const uint32_t inlen = fin.end - fin.start;
        static uint32_t xout[8192];
        uint32_t outlen = 0u; xlat12_draw_stats ds {};
        expect_u("C3 F48 xlat: the real final segment translates",
                 xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), nullptr, &kF48Mib[fin.start], inlen,
                                             xout, &outlen, &ds), 0u);
        expect_u("C3 F48 xlat: ... IN PLACE (out length == in length)", outlen, inlen);
        expect_u("C3 F48 xlat: ... and it still carries every draw", ds.draws, fin.draws);
        n48_f828 xf {};
        expect_u("C3 F48 xlat fence: the TRANSLATED final segment has one buried RELEASE_MEM",
                 n48_f828_find(xout, outlen, &xf), N48_F828_OK);
        expect_u("C3 F48 xlat fence: ... exactly one candidate", xf.candidates, 1u);
        static uint32_t xscratch[8192];
        for (uint32_t i = 0u; i < outlen; i++) xscratch[i] = xout[i];
        const uint64_t xpageVa = 0x5000000ull, xslotVa = xpageVa;
        expect_u("C3 F48 xlat fence: apply succeeds on the translated packet",
                 n48_f828_apply(xscratch, outlen, &xf, xslotVa, xpageVa, 0x12345678u), N48_F828_OK);
        uint32_t xrel = 0u, xrelAt = 0u, xwalk = 1u;
        for (uint32_t i = 0u; i < outlen; ) {
            const uint32_t l = n48_f828_pkt_len(xscratch[i]);
            if (!l || i + l > outlen) { xwalk = 0u; break; }
            if (xscratch[i] == N48_F828_RELMEM) { xrel++; xrelAt = i; }
            i += l;
        }
        expect_u("C3 F48 xlat fence: the translated segment still walks end to end", xwalk, 1u);
        expect_u("C3 F48 xlat fence: exactly ONE un-NOPed RELEASE_MEM after apply", xrel, 1u);
        // Translation is IN PLACE, so the draw keeps its index; `draw_at` is local to IB 1, so subtract the segment's
        // own offset inside IB 1 to get the index in the translated output (which starts at the segment's start).
        const uint32_t xlastDraw = fin.draw_at - (fin.start - kF48MibIbs[1].off);
        expect_u("C3 F48 xlat fence: ... and it executes AFTER the translated segment's last draw",
                 (xrelAt > xlastDraw) ? 1u : 0u, 1u);
        // NON-VACUITY: the find on the translated output depends on the real packet. Corrupting either the buried
        // RELEASE_MEM header or its NOP wrapper in a COPY must make find refuse; a find that ignored the bytes would
        // pass both and prove nothing.
        static uint32_t xmut[8192];
        for (uint32_t i = 0u; i < outlen; i++) xmut[i] = xout[i];
        n48_f828 xm {};
        xmut[xf.rel_at] ^= 1u;
        expect_u("C3 F48 xlat fence plant (RELEASE_MEM header corrupted): find REFUSES",
                 n48_f828_find(xmut, outlen, &xm) == N48_F828_OK ? 1u : 0u, 0u);
        for (uint32_t i = 0u; i < outlen; i++) xmut[i] = xout[i];
        xmut[xf.nop_at] ^= 1u;
        n48_f828 xn {};
        expect_u("C3 F48 xlat fence plant (NOP wrapper corrupted): find REFUSES",
                 n48_f828_find(xmut, outlen, &xn) == N48_F828_OK ? 1u : 0u, 0u);
    }
    // ---- F20 (mid-encoder): the second IB yields no segments, so the frame refuses. Still true after 0.0.432 -
    // n48_mib_seg_disguised_head's opcode check (dword 0 must be a type-3 NOP) never fires on a mid-encoder start,
    // whose dword 0 is a SET_CONTEXT_REG/draw opening, not a NOP header at all. ----
    {
        const uint32_t off[2] = { kF20MibIbs[0].off, kF20MibIbs[1].off };
        const uint32_t len[2] = { kF20MibIbs[0].len, kF20MibIbs[1].len };
        const uint32_t ns = n48_mib_segment(kF20Mib, kF20MibNib, off, len, segs, 64u, ib_nseg, &tot, nullptr, 0u, nullptr);
        expect_u("C3 F20 mid-encoder: IB0 segments", ib_nseg[0], 7u);
        expect_u("C3 F20 mid-encoder: IB1 segments (mid-encoder -> none)", ib_nseg[1], 0u);
        expect_u("C3 F20 mid-encoder: the whole frame is refused (0 segments)", ns, 0u);
        expect_u("C3 F20 mid-encoder: ... and total is cleared", tot, 0u);
    }
    // F21 (NOP-headed IB1) moved to mib_nop_disguise_checks (0.0.432): it now SEGMENTS.
    // ---- THE TWO PLANTS condition (3) NAMES, over the REAL boundary. ----
    {
        // (a) IB 0's VA used for a segment that lives in IB 1. seg_va_mut(1,...) is the B2 plant; the real answer names
        //     IB1's page, the mutant names IB0's, and the two cannot be equal for a real segment.
        const uint64_t va0 = 0x400020000ull, va1 = 0x4005a0000ull;
        const uint32_t fromInIb1 = segs[7].start;   // the first segment of IB 1, global dword
        const uint64_t good = n48_mib_seg_va(va1, fromInIb1, off48[1]);
        const uint64_t bad  = seg_va_mut(1, va0, fromInIb1, off48[1]);
        expect_u("C3 plant (a): IB0's VA for IB1's bytes is a different address", good != bad ? 1u : 0u, 1u);
        // The segment body starts two dwords into its head, so the real VA is IB1's base + 4*(start - IB1's off).
        expect_u("C3 plant (a): ... and the real answer IS IB1's base plus its own shift",
                 good, va1 + 4ull * (uint64_t)(fromInIb1 - off48[1]));
    }
    {
        // (b) the fence offered at a non-final segment. The real helper offers only k == nseg-1 for mib; offered_mut(3)
        //     offers every segment, including IB0's last (k=6), which would fire the fence before IB1 ran.
        expect_u("C3 plant (b): the real helper refuses IB0's last segment (k=6)", n48_f828_offered(6u, ns48, 1u), 0u);
        expect_u("C3 plant (b): ... and offers the frame's final segment (k=11)", n48_f828_offered(11u, ns48, 1u), 1u);
        expect_u("C3 plant (b): the mutant offers k=6 (caught)", offered_mut(3, 6u, ns48, 1u), 1u);
    }
    return gFail - before;
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.432 — THE DISGUISED-HEAD FIX, OVER TWO INDEPENDENT REAL CAPTURES, PLUS ITS PLANTED
// BREAKS. Through 0.0.431 an IB k >= 1 whose dword 0 was the 10-dword NOP always answered 0 segments (all 52 two-IB
// segment-stage runs in decide36b,). `n48_mib_seg_disguised_head` (gfx_mib.h) now recognises it; this drives
// n48_mib_segment over F21 (arm32's real capture, already in fixture_mib_f48_f20_f21.h) and F33D36B (decide36b's own
// real capture, fixture_mib_nop_decide36b.h - a DIFFERENT boot, DIFFERENT frame shape, so the fix is not proven on one
// sample), checks the resulting table still tiles and still passes the SAME unmodified gate F48 passes, and then
// drives the three breaks this brief names: a NOP whose length runs past the IB end, a non-NOP header at dword 0, and
// a mid-encoder IB - each must still yield zero segments / a refused frame.
// ---------------------------------------------------------------------------------------------------------------------

// Checks common to both real NOP-headed frames: the frame now segments, tiles head-to-head with ENCODER start, ends
// exactly at the frame's own dword count, and the SAME unmodified gate (gfx_commit.h, n48_cm_gate - F48's own gate,
// untouched by this brief) commits it. Returns the segment count so the caller can also run the fence check.
static uint32_t mib_nop_disguise_frame_checks(const char *tag, const uint32_t *mib, uint32_t nib,
                                              const n48_mib_fixture_ib *ibs, uint32_t totalDwords,
                                              uint32_t wantIb0Segs, uint32_t wantIb1Segs)
{
    xlat12_ib_segment segs[64]; uint32_t ib_nseg[4] = { 0, 0, 0, 0 }; uint32_t tot = 0u;
    const uint32_t off[2] = { ibs[0].off, ibs[1].off };
    const uint32_t len[2] = { ibs[0].len, ibs[1].len };
    char b[192];
    const uint32_t ns = n48_mib_segment(mib, nib, off, len, segs, 64u, ib_nseg, &tot, nullptr, 0u, nullptr);
    std::snprintf(b, sizeof b, "%s: IB0 segments (clean, unaffected by this brief)", tag);
    expect_u(b, ib_nseg[0], wantIb0Segs);
    std::snprintf(b, sizeof b, "%s: IB1 segments (0.0.432: the disguised head now counts)", tag);
    expect_u(b, ib_nseg[1], wantIb1Segs);
    std::snprintf(b, sizeof b, "%s: the frame SEGMENTS (was 0 through 0.0.431)", tag);
    expect_u(b, ns, wantIb0Segs + wantIb1Segs);
    std::snprintf(b, sizeof b, "%s: the recognisers' total equals the returned count", tag);
    expect_u(b, tot, ns);
    uint32_t at = 0u, bad = 0u;
    for (uint32_t k = 0u; k < ns; k++) {
        if (segs[k].head != at || segs[k].start != segs[k].head + 2u || segs[k].end <= segs[k].start) bad++;
        at = segs[k].end;
    }
    std::snprintf(b, sizeof b, "%s: every segment tiles head-to-head, ENCODER start (incl. the disguised one)", tag);
    expect_u(b, bad, 0u);
    std::snprintf(b, sizeof b, "%s: ... and the last segment ends at the frame's own dword count", tag);
    expect_u(b, at, totalDwords);
    // THE GATE: the SAME n48_cm_gate F48 passes (mib_frame_from, above), UNMODIFIED by this brief. If the disguised
    // head's head/start convention were wrong, this - not just the local tiling loop above - would refuse it.
    n48_cm_frame c; mib_frame_from(c, segs, ns, len, nib, totalDwords);
    uint32_t d = 0u;
    std::snprintf(b, sizeof b, "%s: the real segment table COMMITS at the SAME unmodified gate", tag);
    expect_u(b, n48_cm_gate(&c, &d), N48_CM_OK);
    return ns;
}

static int mib_nop_disguise_checks(void)
{
    const int before = gFail;

    // ---- F21 (arm32's real capture): IB0 clean (4 segs), IB1 was NOP-headed -> 0 through 0.0.431, now 9. ----
    mib_nop_disguise_frame_checks("0.0.432 F21 (arm32)", kF21Mib, kF21MibNib, kF21MibIbs, kF21Mib_dwords, 4u, 9u);

    // ---- F33D36B (decide36b's OWN real capture: a different boot, different frame shape): IB0 clean (9 segs), IB1
    //      was NOP-headed -> 0, now 1 (the disguised head) + 9 (what follows it) = 10. ----
    const uint32_t ns33 = mib_nop_disguise_frame_checks("0.0.432 F33D36B (decide36b)", kF33D36BMib, kF33D36BMibNib,
                                                         kF33D36BMibIbs, kF33D36BMib_dwords, 9u, 10u);

    // THE FENCE, on F33D36B's own final segment (n48_mib_segment ran again to get a fresh table for this check, since
    // the helper above did not return `segs`). fence/gate still pass on the NEW real body this brief's fix reaches.
    {
        xlat12_ib_segment segs[64]; uint32_t ib_nseg[4] = { 0, 0, 0, 0 }; uint32_t tot = 0u;
        const uint32_t off[2] = { kF33D36BMibIbs[0].off, kF33D36BMibIbs[1].off };
        const uint32_t len[2] = { kF33D36BMibIbs[0].len, kF33D36BMibIbs[1].len };
        const uint32_t ns = n48_mib_segment(kF33D36BMib, kF33D36BMibNib, off, len, segs, 64u, ib_nseg, &tot, nullptr, 0u, nullptr);
        expect_u("0.0.432 F33D36B fence: segments again, same count", ns, ns33);
        const xlat12_ib_segment &fin = segs[ns - 1u];
        n48_f828 fr {};
        expect_u("0.0.432 F33D36B fence: the final segment has one buried RELEASE_MEM",
                 n48_f828_find(&kF33D36BMib[fin.start], fin.end - fin.start, &fr), N48_F828_OK);
        expect_u("0.0.432 F33D36B fence: ... exactly one candidate", fr.candidates, 1u);
    }

    // ---- THE THREE PLANTED BREAKS this brief names. Each must still yield zero segments. ----
    // A minimal, valid single-segment IB0 (12 dwords: the head, nothing after it - xlat12_ib_segments' own control).
    static const uint32_t kIb0Only[12] = { XLAT12_SEG_HEAD0, XLAT12_SEG_HEAD1, XLAT12_SEG_ACQUIRE, 0u, 0u, 0u, 0u, 0u,
                                           0u, 0u, XLAT12_SEG_HEAD0, XLAT12_SEG_EVENT_E };
    {
        // (a) "skipping a NOP whose length runs past the IB end must FAIL": dword 0 decodes as a type-3 NOP whose
        // declared length is EXACTLY 10 (so it is not merely the length-10 pin that refuses it) but IB1 is only 10
        // dwords long - leaving no room for the trailing EVENT_WRITE this rule must read right after the NOP. n48's
        // own bounds guard must refuse it BEFORE it reads dword 10/11 at all.
        uint32_t mib[22];
        for (uint32_t i = 0; i < 12u; i++) mib[i] = kIb0Only[i];
        mib[12] = N48_MIB_NOP_HEAD;                          /* type-3 NOP, declared length 10 (0xC0081000) */
        for (uint32_t i = 13; i < 22; i++) mib[i] = 0u;       /* IB1 is only 10 dwords: [12, 22) — no room for a tail */
        const uint32_t off[2] = { 0u, 12u }, len[2] = { 12u, 10u };
        xlat12_ib_segment segs[16]; uint32_t ib_nseg[4] = { 9, 9, 9, 9 }; uint32_t tot = 9u;
        expect_u("0.0.432 plant (a): n48_mib_seg_disguised_head refuses the oversized NOP directly",
                 (uint32_t)n48_mib_seg_disguised_head(mib, 12u, 10u), 0u);
        const uint32_t ns = n48_mib_segment(mib, 2u, off, len, segs, 16u, ib_nseg, &tot, nullptr, 0u, nullptr);
        expect_u("0.0.432 plant (a): a NOP whose length runs past the IB end -> IB1 still 0 segments", ib_nseg[1], 0u);
        expect_u("0.0.432 plant (a): ... and the whole frame is refused", ns, 0u);
        expect_u("0.0.432 plant (a): ... and total is cleared", tot, 0u);
    }
    {
        // (b) "skipping a non-NOP header must FAIL": dword 0 is type-3 with the SAME declared length (10) and the
        // SAME tail (dwords 1/2/10/11 all match), but its OPCODE is 0x11 (SET_BASE), not 0x10 (NOP). Only the opcode
        // differs from a genuine disguise.
        uint32_t mib[24];
        for (uint32_t i = 0; i < 12u; i++) mib[i] = kIb0Only[i];
        mib[12] = 0xC0081100u;                                /* type-3, count 8 (len 10), opcode 0x11 — NOT NOP */
        mib[13] = XLAT12_SEG_HEAD1; mib[14] = XLAT12_SEG_ACQUIRE;
        for (uint32_t i = 15; i < 22; i++) mib[i] = 0u;
        mib[22] = XLAT12_SEG_HEAD0; mib[23] = XLAT12_SEG_EVENT_E;   /* the disguise's own tail, present and correct */
        const uint32_t off[2] = { 0u, 12u }, len[2] = { 12u, 12u };
        xlat12_ib_segment segs[16]; uint32_t ib_nseg[4] = { 9, 9, 9, 9 }; uint32_t tot = 9u;
        expect_u("0.0.432 plant (b): n48_mib_seg_disguised_head refuses the non-NOP header directly",
                 (uint32_t)n48_mib_seg_disguised_head(mib, 12u, 12u), 0u);
        const uint32_t ns = n48_mib_segment(mib, 2u, off, len, segs, 16u, ib_nseg, &tot, nullptr, 0u, nullptr);
        expect_u("0.0.432 plant (b): a non-NOP header at dword 0 -> IB1 still 0 segments", ib_nseg[1], 0u);
        expect_u("0.0.432 plant (b): ... and the whole frame is refused", ns, 0u);
        expect_u("0.0.432 plant (b): ... and total is cleared", tot, 0u);
    }
    {
        // (c) "a mid-encoder IB must still yield zero segments": dword 0 is not type-3 NOP at all (a mid-encoder
        // opening, the same shape body_mid() uses elsewhere in this file) - the opcode check refuses it before any
        // of the disguise's other guards run, exactly as F20's real body already proves end to end.
        uint32_t mib[24];
        for (uint32_t i = 0; i < 12u; i++) mib[i] = kIb0Only[i];
        mib[12] = 0xC0012D00u; mib[13] = 3u; mib[14] = 2u;    /* body_mid()'s own opening, not a head or a NOP */
        for (uint32_t i = 15; i < 24; i++) mib[i] = 0u;
        const uint32_t off[2] = { 0u, 12u }, len[2] = { 12u, 12u };
        xlat12_ib_segment segs[16]; uint32_t ib_nseg[4] = { 9, 9, 9, 9 }; uint32_t tot = 9u;
        expect_u("0.0.432 plant (c): n48_mib_seg_disguised_head refuses a mid-encoder opening directly",
                 (uint32_t)n48_mib_seg_disguised_head(mib, 12u, 12u), 0u);
        const uint32_t ns = n48_mib_segment(mib, 2u, off, len, segs, 16u, ib_nseg, &tot, nullptr, 0u, nullptr);
        expect_u("0.0.432 plant (c): a mid-encoder IB1 -> still 0 segments", ib_nseg[1], 0u);
        expect_u("0.0.432 plant (c): ... and the whole frame is refused", ns, 0u);
    }
    return gFail - before;
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.427 ( condition (2)) — THE WRITE/READ ORDER.
// The kext now writes EVERY IB before reading any back; through 0.0.426 it wrote, read and compared one IB at a time.
// The difference is invisible for disjoint IB ranges and decisive for overlapping ones: an interleaved compare of IB k
// can pass before IB k+1's write lands on the same bytes, so the frame could commit with memory that no longer holds
// IB k's stream. The model below is the property; the source pins tie it to AppleHardwareHook.cpp.
// ---------------------------------------------------------------------------------------------------------------------
static uint32_t mib_order_model(uint32_t interleaved, uint32_t secondOff)
{
    uint32_t cand[200], mem[200], back[200];
    for (uint32_t i = 0u; i < 200u; i++) { cand[i] = 0xA000u + i; mem[i] = 0u; back[i] = 0u; }
    const uint32_t off[2] = { 0u, secondOff }, len[2] = { 100u, 100u };   // two 100-dword IBs; overlap when secondOff < 100
    // The candidate slices are SEPARATE (cand[k*100 + j] is IB k's j-th dword), as they are in gXdNew at off_k; the
    // client memory is one address space, as it is in the client's pages. So an overlap writes DIFFERENT bytes over IB 0.
    uint32_t mismatch = 0u;
    const auto candv = [&](uint32_t k, uint32_t j) -> uint32_t { return cand[k * 100u + j]; };
    if (!interleaved) {
        for (uint32_t k = 0u; k < 2u; k++) for (uint32_t j = 0u; j < len[k]; j++) mem[off[k] + j] = candv(k, j);
        for (uint32_t k = 0u; k < 2u; k++) {
            for (uint32_t j = 0u; j < len[k]; j++) back[off[k] + j] = mem[off[k] + j];
            for (uint32_t j = 0u; j < len[k]; j++) if (back[off[k] + j] != candv(k, j)) mismatch++;
        }
    } else {
        for (uint32_t k = 0u; k < 2u; k++) {
            for (uint32_t j = 0u; j < len[k]; j++) mem[off[k] + j] = candv(k, j);
            for (uint32_t j = 0u; j < len[k]; j++) back[off[k] + j] = mem[off[k] + j];
            for (uint32_t j = 0u; j < len[k]; j++) if (back[off[k] + j] != candv(k, j)) mismatch++;
        }
    }
    return mismatch == 0u ? 1u : 0u;   // 1 = the model would hand the frame COMMIT
}

// The source pins read AppleHardwareHook.cpp (argv[1]) for the two-pass ORDER the kext must have. Deleting either PASS
// marker (reverting to the interleaved 0.0.426 loop) or moving the read before the write makes these FAIL.
static char *mib_pin_slurp(const char *path, long *len)
{
    FILE *f = std::fopen(path, "rb");
    if (!f) return nullptr;
    std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    char *b = static_cast<char *>(std::malloc(static_cast<size_t>(n) + 1u));
    if (!b) { std::fclose(f); return nullptr; }
    if (std::fread(b, 1u, static_cast<size_t>(n), f) != static_cast<size_t>(n)) { std::free(b); std::fclose(f); return nullptr; }
    b[n] = 0; std::fclose(f); if (len) *len = n;
    return b;
}
static uint32_t mib_pin_count(const char *hay, const char *needle)
{
    uint32_t c = 0u;
    for (const char *p = hay; (p = std::strstr(p, needle)) != nullptr; p += std::strlen(needle)) c++;
    return c;
}
// The first occurrence of `needle` in [begin, end), or null. Used to prove PASS 1 contains the write and NO IB read-back.
static const char *mib_pin_in(const char *begin, const char *end, const char *needle)
{
    if (!begin || !end || begin >= end) return nullptr;
    const size_t hl = static_cast<size_t>(end - begin), nl = std::strlen(needle);
    if (nl > hl) return nullptr;
    for (const char *p = begin; p + nl <= end; p++) if (std::memcmp(p, needle, nl) == 0) return p;
    return nullptr;
}
static void mib_source_pins(const char *path)
{
    long n = 0;
    char *src = mib_pin_slurp(path, &n);
    if (!src) { std::printf("  SKIP MIB source pins: cannot read %s\n", path); gFail++; gRun++; return; }
    const char *p1 = "PASS 1 - WRITE EVERY IB BEFORE ANY READ.";
    const char *p2 = "PASS 2 - READ BACK AND COMPARE EVERY IB, AFTER EVERY WRITE IS IN.";
    const char *wr = "c.wrote_ib[k] = gfxc_write_sys(vm, vak, &gXdNew[offk], nk, &c.pages_ib[k], &c.sys_pages_ib[k]);";
    const char *rd = "c.got_ib[k] = gfxc_read(vm, vak, &gXdBack[offk], nk, &bsys);";
    expect_u("PIN C2: the MIB commit has PASS 1 (write every IB)", mib_pin_count(src, p1), 1u);
    expect_u("PIN C2: ... and PASS 2 (read back and compare every IB)", mib_pin_count(src, p2), 1u);
    const char *s1 = std::strstr(src, p1), *s2 = std::strstr(src, p2);
    expect_u("PIN C2: PASS 1 precedes PASS 2", (s1 && s2 && s1 < s2) ? 1u : 0u, 1u);
    // THE INTERLEAVE GUARD: PASS 1 (everything up to PASS 2) must carry the write and must NOT read an IB back. A
    // write+read in one loop - the 0.0.426 shape this build fixes - leaves the read inside PASS 1 and FAILS.
    expect_u("PIN C2: PASS 1 carries the IB write", mib_pin_in(s1, s2, wr) ? 1u : 0u, 1u);
    expect_u("PIN C2: ... and PASS 1 does NOT read the IB back (no interleave)",
             mib_pin_in(s1, s2, rd) ? 1u : 0u, 0u);
    const char *w = std::strstr(src, wr), *r = std::strstr(src, rd);
    expect_u("PIN C2: the IB write statement appears exactly once", mib_pin_count(src, wr), 1u);
    expect_u("PIN C2: the write pass precedes the read-back pass", (w && r && w < r) ? 1u : 0u, 1u);
    // 0.0.429 ( condition (1),'s "pin AHH:24315 in the source pins"). The writeTail's ring accounting
    // must count the WHOLE spared frame's IBs through the same helper the host model uses (gfx_neuter.h's
    // n48_gfxn_spared_n). The 0.0.426 defect was the flat `? 1 : 0`; requiring the exact call means reverting it (the
    // helper call replaced by a literal) makes this count 0 and FAILS. Nothing else in AppleHardwareHook.cpp carries
    // this statement, so the count is exact.
    const char *sparedN = "sparedN = n48_gfxn_spared_n(exWhy, ex.mib, w.ibs);";
    expect_u("PIN C1: the writeTail spared count is n48_gfxn_spared_n(exWhy, ex.mib, w.ibs)",
             mib_pin_count(src, sparedN), 1u);
    // 0.0.429: THE SEGMENT-OWNER LOOP. Every segment must resolve to the IB whose concatenated range
    // contains it; deleting the loop leaves every segment owned by IB 0 and points n48_mib_seg_va at the wrong client
    // pages. The anchor is the exact loop from AppleHardwareHook.cpp (the mib_seg_owner model above runs its arithmetic
    // over F48's real table).
    const char *ownerLoop = "while (segIb + 1u < f->nib && from >= gXdBuild.ib_off[segIb] + gXdBuild.ib_n[segIb]) segIb++;";
    expect_u("PIN B2: the segment-owner loop is present exactly once", mib_pin_count(src, ownerLoop), 1u);
    // 0.0.430: THE REFUSAL DIAG IS BUILT, PASSED AND NOTED. Each anchor is the exact statement from
    // AppleHardwareHook.cpp; deleting the note call silences the whole instrument, and deleting the diag argument leaves
    // the census with no reason to print. The census call itself is not otherwise constrained, so this is what ties the
    // host proof above to the kext.
    const char *segDecl = "n48_mib_seg_diag mibseg {};";
    const char *segCall = "gXdBuild.ib_nseg, &tot, &mibseg, gXdBuild.xib, &leadMask);";   // build 0.0.505: + switch 69
    const char *segNote = "n48_mib0_note_seg(&gMib0, &mibseg);";
    expect_u("PINa: the diag is declared in gfxsrc_policy", mib_pin_count(src, segDecl), 1u);
    expect_u("PINa: ... passed to n48_mib_segment", mib_pin_count(src, segCall), 1u);
    expect_u("PINa: ... and noted into the MIB-0 census", mib_pin_count(src, segNote), 1u);
    expect_u("PINa: ... in that order",
             (strstr(src, segDecl) && strstr(src, segCall) && strstr(src, segNote) &&
              strstr(src, segDecl) < strstr(src, segNote)) ? 1u : 0u, 1u);
    // 0.0.430: THE PHASE TIMERS ARE WIRED. One place enters the phase timing, one folds the phases and
    // counts the run, and both report lines are called from the gfxneuter report beside the census.
    // 0.0.434 (notes/design/PGMID-COPYGUARD.md Part 1): re-baselined for gfxsrc_policy's edit that lets the
    // SAME timing also feed nib >= 2 passes into a second n48_mib_pol - `polSingle`'s literal on/off flag became
    // `polTarget`, a pointer at whichever accumulator (or neither) this pass belongs to, and the fold now writes
    // through that pointer rather than naming `gMibPol` directly, so it serves gMibPol2 identically.
    const char *polEnter = "if (polTarget) { clock_get_uptime(&polT0); gPolDescTarget = polTarget; }";
    const char *polFold  = "polTarget->runs++;";
    const char *polSegLn = "mib0_seg_report_line();";
    const char *polPolLn = "mib0_pol_report_line();";
    const char *polPol2Ln = "mib0_pol2_report_line();";
    expect_u("PINc: the phase timing is entered exactly once", mib_pin_count(src, polEnter), 1u);
    expect_u("PINc: ... and the phases are folded exactly once", mib_pin_count(src, polFold), 1u);
    expect_u("PINa: the segment-refusal report is called", mib_pin_count(src, polSegLn), 1u);
    expect_u("PINc: the policy-phase report is called", mib_pin_count(src, polPolLn), 1u);
    expect_u("PIN: the nib>=2 policy-phase report is called", mib_pin_count(src, polPol2Ln), 1u);
    std::free(src);
}


// =====================================================================================================================
// 0.0.446 — THE `mibseg:` COUNTER (gfx_mibseg.h). build 0.0.447 (reviewer review of 0.0.446,
// item 6): the dataset now covers EVERY one of the thirteen named DESC codes (F1-FA + the three inline-path codes)
// individually, the DEFENSIVE unknown-desc bucket, PAIR, COPY-OVERLAP, and the non-DESC/non-PAIR histogram (two
// DIFFERENT "other" codes, so the histogram's own per-code split is exercised, not just its total). `plant` 1 is
// the brief's named break, updated for the new design: PROVENANCE miscounted into DESC_UNKNOWN (the one remaining
// "lump"-shaped bucket; every OTHER code now has its own named slot, so this is the only miscount left to plant).
// =====================================================================================================================
static uint32_t mibseg_bucket_mut(int plant, uint32_t st, uint32_t op)
{
    const uint32_t b = n48_mibseg_bucket(st, op);
    if (plant == 1 && b == N48_MIBSEG_DESC_PROVENANCE) return N48_MIBSEG_DESC_UNKNOWN;
    return b;
}
static int mibseg_checks(int plant)
{
    const int f0 = gFail;
    // IB 0 (9 segments): ok, PROV, UNSEEN, SHARED, SAMP-OVERRIDE, READ, UNSTABLE, NOT-APPLE, other (MEMLOADED).
    // IB 1 (11 segments): NO-ROOM, REDIRECTED, TOO-MANY, inline-mixed, inline-inherited, inline-range, unknown-desc
    // (a bogus op), PAIR, COPY-OVERLAP, other (DRAW-SHAPE), other (VERIFY, so lastOther[1] ends on VERIFY - the
    // LAST one processed, proving the per-IB field is not just "the first" or "whichever fires").
    static const uint32_t st[20] = {
        0u, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC,
        XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC,
        XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC,
        XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_PAIR, (uint32_t)N48_SEG_COPY_OVERLAP,
        (uint32_t)XLAT12_IB_ERR_DRAW_SHAPE, XLAT12_ERR_MEMLOADED, (uint32_t)XLAT12_IB_ERR_VERIFY };
    static const uint32_t op[20] = {
        0xFFFFFFFFu, XLAT12_TDESC_PROVENANCE, XLAT12_TDESC_SLOT_UNSEEN, XLAT12_TDESC_SLOT_SHARED, XLAT12_TDESC_SAMP_OVERRIDE,
        XLAT12_TDESC_READ, XLAT12_TDESC_UNSTABLE, XLAT12_TDESC_NOT_APPLE,
        XLAT12_TDESC_NO_ROOM, XLAT12_TDESC_REDIRECTED, XLAT12_TDESC_TOO_MANY, 0xFDu, 0xFEu,
        0xFFu, 0x1234u, 0x76u, 0u,
        0u, 0u, 0u };
    static const uint8_t ib[20] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1 };
    n48_mibseg m {};
    if (plant == 0) {
        n48_mibseg_note_frame(&m, 77u, 2u, 20u, st, op, ib);
    } else {
        // The planted path: the SAME note logic with the mutated bucket (n48_mibseg_note_frame is small and pure;
        // this is its body, byte for byte, with the one call swapped).
        m.frames++;
        for (uint32_t k = 0; k < 20u; k++) {
            const uint32_t b = mibseg_bucket_mut(plant, st[k], op[k]);
            const uint32_t side = ib[k] ? 1u : 0u;
            m.seg[side][b]++;
            if (b == N48_MIBSEG_OTHER) m.lastOther[side] = st[k];
            if (b == N48_MIBSEG_DESC_UNKNOWN) m.lastDescUnknownOp[side] = op[k];
        }
    }
    expect_u("mibseg: one frame with segments", m.frames, 1u);
    expect_u("mibseg: it is NOT every-segment-translated", m.framesAllOk, 0u);
    expect_u("mibseg IB0: ok 1", m.seg[0][N48_MIBSEG_OK], 1u);
    expect_u("mibseg IB0: DESC provenance 1", m.seg[0][N48_MIBSEG_DESC_PROVENANCE], 1u);
    expect_u("mibseg IB0: DESC slot-unseen 1", m.seg[0][N48_MIBSEG_DESC_SLOT_UNSEEN], 1u);
    expect_u("mibseg IB0: DESC slot-shared 1", m.seg[0][N48_MIBSEG_DESC_SLOT_SHARED], 1u);
    expect_u("mibseg IB0: DESC samp-override 1", m.seg[0][N48_MIBSEG_DESC_SAMP_OVERRIDE], 1u);
    expect_u("mibseg IB0: DESC read 1", m.seg[0][N48_MIBSEG_DESC_READ], 1u);
    expect_u("mibseg IB0: DESC unstable 1", m.seg[0][N48_MIBSEG_DESC_UNSTABLE], 1u);
    expect_u("mibseg IB0: DESC not-apple 1", m.seg[0][N48_MIBSEG_DESC_NOT_APPLE], 1u);
    expect_u("mibseg IB0: other 1 (MEMLOADED)", m.seg[0][N48_MIBSEG_OTHER], 1u);
    expect_u("mibseg IB0: lastOther is THIS ib's own (MEMLOADED, not IB1's)", m.lastOther[0], (uint64_t)XLAT12_ERR_MEMLOADED);
    { uint64_t sum = 0; for (uint32_t b = 0; b < N48_MIBSEG_BUCKETS; b++) sum += m.seg[0][b];
      expect_u("mibseg IB0: every one of its 9 segments landed in exactly one bucket", sum, 9u); }
    expect_u("mibseg IB1: DESC no-room 1", m.seg[1][N48_MIBSEG_DESC_NO_ROOM], 1u);
    expect_u("mibseg IB1: DESC redirected 1", m.seg[1][N48_MIBSEG_DESC_REDIRECTED], 1u);
    expect_u("mibseg IB1: DESC too-many 1", m.seg[1][N48_MIBSEG_DESC_TOO_MANY], 1u);
    expect_u("mibseg IB1: DESC inline-mixed 1 (0xFD)", m.seg[1][N48_MIBSEG_DESC_INLINE_MIXED], 1u);
    expect_u("mibseg IB1: DESC inline-inherited 1 (0xFE)", m.seg[1][N48_MIBSEG_DESC_INLINE_INHERITED], 1u);
    expect_u("mibseg IB1: DESC inline-range 1 (0xFF)", m.seg[1][N48_MIBSEG_DESC_INLINE_RANGE], 1u);
    expect_u("mibseg IB1: DESC unknown-op 1 (a bogus 0x1234)", m.seg[1][N48_MIBSEG_DESC_UNKNOWN], 1u);
    expect_u("mibseg IB1: lastDescUnknownOp is THIS ib's own (0x1234)", m.lastDescUnknownOp[1], 0x1234u);
    expect_u("mibseg IB0: lastDescUnknownOp untouched (0, no unknown-desc segment on IB0)", m.lastDescUnknownOp[0], 0u);
    expect_u("mibseg IB1: PAIR 1", m.seg[1][N48_MIBSEG_PAIR], 1u);
    expect_u("mibseg IB1: COPY-OVERLAP 1", m.seg[1][N48_MIBSEG_COPY_OVERLAP], 1u);
    expect_u("mibseg IB1: other 2 (DRAW-SHAPE, then VERIFY)", m.seg[1][N48_MIBSEG_OTHER], 2u);
    expect_u("mibseg IB1: lastOther is the LAST of the two other segments (VERIFY)", m.lastOther[1], (uint64_t)XLAT12_IB_ERR_VERIFY);
    expect_u("mibseg IB1: DESC provenance 0 (kept apart from IB 0's)", m.seg[1][N48_MIBSEG_DESC_PROVENANCE], 0u);
    expect_u("mibseg IB1: ok 0", m.seg[1][N48_MIBSEG_OK], 0u);
    { uint64_t sum = 0; for (uint32_t b = 0; b < N48_MIBSEG_BUCKETS; b++) sum += m.seg[1][b];
      expect_u("mibseg IB1: every one of its 11 segments landed in exactly one bucket", sum, 11u); }
    // build 0.0.447 — the histogram: combined across both IBs, one slot per named non-DESC/non-PAIR code.
    expect_u("mibseg-other: draw-shape 1", m.otherHist[N48_MIBSEG_OHIST_DRAW_SHAPE], 1u);
    expect_u("mibseg-other: memloaded 1", m.otherHist[N48_MIBSEG_OHIST_MEMLOADED], 1u);
    expect_u("mibseg-other: verify 1", m.otherHist[N48_MIBSEG_OHIST_VERIFY], 1u);
    { uint64_t sum = 0; for (uint32_t h = 0; h <= N48_MIBSEG_OHIST_N; h++) sum += m.otherHist[h];
      expect_u("mibseg-other: exactly the 3 OTHER segments are histogrammed, nothing else", sum, 3u); }
    if (plant) return gFail - f0;
    // The refused frame's compact record: 20 segments, only the first 16 shown (N48_MIBSEG_LAST_MAX) - the
    // SEPARATE 20-uniform-PAIR-segment test below proves the cap and its separators precisely; here only the
    // shape is checked (16 of 20 shown, still starting with this frame's own first bucket letters).
    char last[N48_MIBSEG_LAST_STR + 1u];
    n48_mibseg_last_str(&m, last, (uint32_t)sizeof(last));
    expect_u("mibseg-last: the frame is recorded (judged 77, nib 2, 20 segments, 16 shown)",
             (m.lastFrame == 77u && m.lastNib == 2u && m.lastNseg == 20u && m.lastShown == 16u) ? 1u : 0u, 1u);
    expect_u("mibseg-last: starts \".PUSVRBN|MET\" - one letter per bucket, '|' at the IB change",
             std::strncmp(last, ".PUSVRBN|MET", 12) == 0 ? 1u : 0u, 1u);
    if (!gQuiet) std::printf("      mibseg-last string: \"%s\"\n", last);
    // An all-translated two-IB frame counts as such and does NOT replace the last REFUSED record.
    static const uint32_t okst[3] = { 0u, 0u, 0u }; static const uint8_t okib[3] = { 0, 1, 1 };
    n48_mibseg_note_frame(&m, 78u, 2u, 3u, okst, nullptr, okib);
    expect_u("mibseg: an all-translated frame is counted EVERY-segment-translated", m.framesAllOk, 1u);
    expect_u("mibseg: ... and the last REFUSED record still names frame 77", m.lastFrame, 77u);
    // A one-IB frame is not this counter's population; a two-IB frame with no segment is counted apart.
    n48_mibseg_note_frame(&m, 79u, 1u, 3u, okst, nullptr, okib);
    expect_u("mibseg: a nib 1 frame is ignored", m.frames, 2u);
    n48_mibseg_note_frame(&m, 80u, 2u, 0u, okst, nullptr, okib);
    expect_u("mibseg: a nib 2 frame with NO segment is counted apart", m.framesNoSeg, 1u);
    expect_u("mibseg: ... and not as a frame with segments", m.frames, 2u);
    // More than 16 segments: 16 shown, separators still placed, the string bounded.
    uint32_t st20[20]; uint32_t op20[20]; uint8_t ib20[20];
    for (uint32_t k = 0; k < 20u; k++) { st20[k] = XLAT12_IB_ERR_PAIR; op20[k] = 0u; ib20[k] = (uint8_t)(k < 10u ? 0u : 1u); }
    n48_mibseg_note_frame(&m, 81u, 2u, 20u, st20, op20, ib20);
    n48_mibseg_last_str(&m, last, (uint32_t)sizeof(last));
    expect_u("mibseg-last: 20 segments -> 16 shown", m.lastShown, 16u);
    expect_u("mibseg-last: ... \"AAAAAAAAAA|AAAAAA\"", std::strcmp(last, "AAAAAAAAAA|AAAAAA") == 0 ? 1u : 0u, 1u);
    // The lines: at real counts and at WIDEST numerics, every one under the MIB-0 lines' own 480-byte bound.
    // build 0.0.447 (item 6): six lines now (A0/A1/A2/B1/B2/C), not two, so none of them risks the cap.
    char line[1024];
    // build 0.0.482 (item 3): the marker is a SEPARATE vararg, before *_ARGS_* (mibseg_report_lines' own call
    // shape); n48_mib_units_mark(0u) is "" - proves the line is BYTE-IDENTICAL to 0.0.474/0.0.481 when 55 never
    // formed units this boot, exactly as the brief requires.
    int w = std::snprintf(line, sizeof(line), N48_MIBSEG_FMT_A0, n48_mib_units_mark(0u), N48_MIBSEG_ARGS_A0(&m));
    expect_u("mibseg line A0: names itself mibseg:", std::strncmp(line, "mibseg:", 7) == 0 ? 1u : 0u, 1u);
    w = std::snprintf(line, sizeof(line), N48_MIBSEG_FMT_A0, n48_mib_units_mark(1u), N48_MIBSEG_ARGS_A0(&m));
    expect_u("mibseg line A0 with units formed: names itself \"mibseg units:\"",
             std::strncmp(line, "mibseg units:", 13) == 0 ? 1u : 0u, 1u);
    n48_mibseg wide {};
    for (uint32_t side = 0; side < 2u; side++) for (uint32_t b = 0; b < N48_MIBSEG_BUCKETS; b++) wide.seg[side][b] = ~0ull;
    for (uint32_t h = 0; h <= N48_MIBSEG_OHIST_N; h++) wide.otherHist[h] = ~0ull;
    wide.frames = wide.framesAllOk = wide.framesNoSeg = wide.lastFrame = ~0ull;
    wide.lastOther[0] = wide.lastOther[1] = 0xFFFFFFFFu;
    wide.lastDescUnknownOp[0] = wide.lastDescUnknownOp[1] = 0xFFFFFFFFu;
    wide.lastNib = wide.lastNseg = 0xFFFFFFFFu; wide.lastShown = 16u;
    for (uint32_t k = 0; k < 16u; k++) { wide.lastBucket[k] = N48_MIBSEG_OTHER; wide.lastIb[k] = (uint8_t)(k & 1u); }
    w = std::snprintf(line, sizeof(line), N48_MIBSEG_FMT_A0, n48_mib_units_mark(0u), N48_MIBSEG_ARGS_A0(&wide));
    if (!gQuiet) std::printf("      mibseg line A0 at widest numerics: %d bytes\n", w);
    expect_u("mibseg line A0 fits the 480-byte bound at widest numerics", (w > 0 && (unsigned)w < 480u) ? 1u : 0u, 1u);
    w = std::snprintf(line, sizeof(line), N48_MIBSEG_FMT_A1, n48_mib_units_mark(0u), N48_MIBSEG_ARGS_A1(&wide));
    if (!gQuiet) std::printf("      mibseg line A1 at widest numerics: %d bytes\n", w);
    expect_u("mibseg line A1 fits the 480-byte bound at widest numerics", (w > 0 && (unsigned)w < 480u) ? 1u : 0u, 1u);
    w = std::snprintf(line, sizeof(line), N48_MIBSEG_FMT_A2, n48_mib_units_mark(0u), N48_MIBSEG_ARGS_A2(&wide));
    if (!gQuiet) std::printf("      mibseg line A2 at widest numerics: %d bytes\n", w);
    expect_u("mibseg line A2 fits the 480-byte bound at widest numerics", (w > 0 && (unsigned)w < 480u) ? 1u : 0u, 1u);
    w = std::snprintf(line, sizeof(line), N48_MIBSEG_FMT_B1, n48_mib_units_mark(0u), N48_MIBSEG_ARGS_B1(&wide));
    if (!gQuiet) std::printf("      mibseg line B1 at widest numerics: %d bytes\n", w);
    expect_u("mibseg line B1 fits the 480-byte bound at widest numerics", (w > 0 && (unsigned)w < 480u) ? 1u : 0u, 1u);
    expect_u("mibseg line B1 names itself mibseg:", std::strncmp(line, "mibseg:", 7) == 0 ? 1u : 0u, 1u);
    w = std::snprintf(line, sizeof(line), N48_MIBSEG_FMT_B2, n48_mib_units_mark(0u), N48_MIBSEG_ARGS_B2(&wide));
    if (!gQuiet) std::printf("      mibseg line B2 at widest numerics: %d bytes\n", w);
    expect_u("mibseg line B2 fits the 480-byte bound at widest numerics", (w > 0 && (unsigned)w < 480u) ? 1u : 0u, 1u);
    // build 0.0.482 (item 3): the TRUE worst case - widest numerics AND the units marker both present -
    // against the real kext-wide N48_LOG_CAP_BODY (f3_reader.h, 491), the same bound the brief names.
    w = std::snprintf(line, sizeof(line), N48_MIBSEG_FMT_A0, n48_mib_units_mark(1u), N48_MIBSEG_ARGS_A0(&wide));
    if (!gQuiet) std::printf("      mibseg line A0 at widest numerics + units marker: %d bytes (cap 491)\n", w);
    expect_u("mibseg line A0 with units formed fits the 491-byte log cap at widest numerics",
             w > 0 && (unsigned)w <= 491u ? 1u : 0u, 1u);
    w = std::snprintf(line, sizeof(line), N48_MIBSEG_FMT_A1, n48_mib_units_mark(1u), N48_MIBSEG_ARGS_A1(&wide));
    if (!gQuiet) std::printf("      mibseg line A1 at widest numerics + units marker: %d bytes (cap 491)\n", w);
    expect_u("mibseg line A1 with units formed fits the 491-byte log cap at widest numerics",
             w > 0 && (unsigned)w <= 491u ? 1u : 0u, 1u);
    w = std::snprintf(line, sizeof(line), N48_MIBSEG_FMT_A2, n48_mib_units_mark(1u), N48_MIBSEG_ARGS_A2(&wide));
    if (!gQuiet) std::printf("      mibseg line A2 at widest numerics + units marker: %d bytes (cap 491)\n", w);
    expect_u("mibseg line A2 with units formed fits the 491-byte log cap at widest numerics",
             w > 0 && (unsigned)w <= 491u ? 1u : 0u, 1u);
    w = std::snprintf(line, sizeof(line), N48_MIBSEG_FMT_B1, n48_mib_units_mark(1u), N48_MIBSEG_ARGS_B1(&wide));
    if (!gQuiet) std::printf("      mibseg line B1 at widest numerics + units marker: %d bytes (cap 491)\n", w);
    expect_u("mibseg line B1 with units formed fits the 491-byte log cap at widest numerics",
             w > 0 && (unsigned)w <= 491u ? 1u : 0u, 1u);
    expect_u("mibseg line B1 with units formed: names itself \"mibseg units:\"",
             std::strncmp(line, "mibseg units:", 13) == 0 ? 1u : 0u, 1u);
    w = std::snprintf(line, sizeof(line), N48_MIBSEG_FMT_B2, n48_mib_units_mark(1u), N48_MIBSEG_ARGS_B2(&wide));
    if (!gQuiet) std::printf("      mibseg line B2 at widest numerics + units marker: %d bytes (cap 491)\n", w);
    expect_u("mibseg line B2 with units formed fits the 491-byte log cap at widest numerics",
             w > 0 && (unsigned)w <= 491u ? 1u : 0u, 1u);
    // build 0.0.447 (item 6) — the new histogram line, also at widest numerics.
    w = std::snprintf(line, sizeof(line), N48_MIBSEG_FMT_C, N48_MIBSEG_ARGS_C(&wide));
    if (!gQuiet) std::printf("      mibseg line C (histogram) at widest numerics: %d bytes\n", w);
    expect_u("mibseg line C fits the 480-byte bound at widest numerics", (w > 0 && (unsigned)w < 480u) ? 1u : 0u, 1u);
    expect_u("mibseg line C names itself mibseg-other:", std::strncmp(line, "mibseg-other:", 13) == 0 ? 1u : 0u, 1u);
    n48_mibseg_last_str(&wide, last, (uint32_t)sizeof(last));
    expect_u("mibseg-last: the worst string is 16 letters + 15 separators", (uint64_t)std::strlen(last), 31u);
    w = std::snprintf(line, sizeof(line), N48_MIBSEG_LAST_FMT, n48_mibseg_cap32(wide.lastFrame), wide.lastNib,
                      wide.lastNseg, wide.lastShown, last);
    if (!gQuiet) std::printf("      mibseg-last line at widest: %d bytes\n", w);
    expect_u("mibseg-last line fits the 480-byte bound at widest", (w > 0 && (unsigned)w < 480u) ? 1u : 0u, 1u);
    expect_u("mibseg-last line names itself mibseg-last:", std::strncmp(line, "mibseg-last:", 12) == 0 ? 1u : 0u, 1u);
    return gFail - f0;
}

// build 0.0.474 item 2 (10B-COVERAGE.md Q4 item 4 contract (2)) — the PER-JUDGED-FRAME line's two pure
// helpers (gfx_mibseg.h): n48_mibseg_frame_str (the SAME letter string as n48_mibseg_last_str, but for ANY one
// frame's own arrays, not only the boot's last refused frame) and n48_mibseg_refused_list (each refused segment's
// own index/err_op/err_in_dword, capped). REACHABILITY: this reuses mibseg_checks' own 20-segment fixture
// (9 IB0 + 11 IB1, the SAME real bucket function n48_mibseg_note_frame already classified above) end to end:
// the SAME st[]/op[]/ib[] arrays a real gfxsrc_policy pass would have left in gMibSegSt/gMibSegOp/gMibSegIb, fed
// straight into these two functions exactly as AppleHardwareHook.cpp's new per-frame site does.
static int mibseg_detail_checks()
{
    const int f0 = gFail;
    static const uint32_t st[20] = {
        0u, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC,
        XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC,
        XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC,
        XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_DESC, XLAT12_IB_ERR_PAIR, (uint32_t)N48_SEG_COPY_OVERLAP,
        (uint32_t)XLAT12_IB_ERR_DRAW_SHAPE, XLAT12_ERR_MEMLOADED, (uint32_t)XLAT12_IB_ERR_VERIFY };
    static const uint32_t op[20] = {
        0xFFFFFFFFu, XLAT12_TDESC_PROVENANCE, XLAT12_TDESC_SLOT_UNSEEN, XLAT12_TDESC_SLOT_SHARED, XLAT12_TDESC_SAMP_OVERRIDE,
        XLAT12_TDESC_READ, XLAT12_TDESC_UNSTABLE, XLAT12_TDESC_NOT_APPLE,
        XLAT12_TDESC_NO_ROOM, XLAT12_TDESC_REDIRECTED, XLAT12_TDESC_TOO_MANY, 0xFDu, 0xFEu,
        0xFFu, 0x1234u, 0x76u, 0u,
        0u, 0u, 0u };
    static const uint8_t ib[20] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1 };
    static uint32_t dw[20];
    for (uint32_t k = 0; k < 20u; k++) dw[k] = 100u + k;   // distinct err_in_dword per segment, arbitrary

    // n48_mibseg_frame_str: the SAME string mibseg_checks() above already proved n48_mibseg_last_str produces for
    // this exact fixture via the aggregate struct - here straight from the raw arrays, no aggregate involved.
    char mstr[N48_MIBSEG_LAST_STR + 1u];
    n48_mibseg_frame_str(st, op, ib, 20u, mstr, (uint32_t)sizeof(mstr));
    expect_u("D1 n48_mibseg_frame_str: SAME 16-of-20 truncation and letters as n48_mibseg_last_str",
             std::strncmp(mstr, ".PUSVRBN|MET", 12) == 0 ? 1u : 0u, 1u);
    expect_u("D2 ... length 16 letters + 1 separator (this fixture's one IB change lands inside the first 16)",
             (uint64_t)std::strlen(mstr), 17u);

    // n48_mibseg_refused_list: 19 of the 20 segments are refused (only index 0 is OK); capped at
    // N48_MIBSEG_DETAIL_CAP (8), in segment order, each with its OWN err_op and err_in_dword.
    uint32_t idx[N48_MIBSEG_DETAIL_CAP], rop[N48_MIBSEG_DETAIL_CAP], rdw[N48_MIBSEG_DETAIL_CAP], total = 0;
    const uint32_t shown = n48_mibseg_refused_list(st, op, dw, 20u, idx, rop, rdw, N48_MIBSEG_DETAIL_CAP, &total);
    expect_u("D3 refused-list: total refused is 19 of 20 (never silent)", total, 19u);
    expect_u("D4 refused-list: shown is capped at N48_MIBSEG_DETAIL_CAP (8), never more", shown, N48_MIBSEG_DETAIL_CAP);
    expect_u("D5 refused-list: first shown is segment 1 (segment 0 is OK, skipped)", idx[0], 1u);
    expect_u("D6 ... its err_op is TDESC_PROVENANCE", rop[0], (uint32_t)XLAT12_TDESC_PROVENANCE);
    expect_u("D7 ... its err_in_dword is 101 (100 + segment index 1)", rdw[0], 101u);
    expect_u("D8 refused-list: 8th shown is segment 8 (the 8th refused segment in order)", idx[7], 8u);
    expect_u("D9 ... its err_op is TDESC_NO_ROOM", rop[7], (uint32_t)XLAT12_TDESC_NO_ROOM);
    // TOO_LONG "site": err_op IS the site (0xFF S10 vs 0xFC S11/S12) - no separate field. Segment 13 here carries
    // 0xFFu under ERR_DESC (INLINE_RANGE, a different bucket) - proving err_op is read verbatim regardless of
    // bucket, exactly as the real per-segment record does (n48_mibseg_bucket is a SEPARATE classification of the
    // very same (st,op) pair this list also reports raw).
    uint32_t idx2[N48_MIBSEG_DETAIL_CAP], rop2[N48_MIBSEG_DETAIL_CAP], rdw2[N48_MIBSEG_DETAIL_CAP], total2 = 0;
    static const uint32_t stTL[2] = { (uint32_t)XLAT12_IB_ERR_TOO_LONG, (uint32_t)XLAT12_IB_ERR_TOO_LONG };
    static const uint32_t opTL[2] = { 0xFFu, 0xFCu };
    static const uint32_t dwTL[2] = { 54u, 42u };
    const uint32_t shownTL = n48_mibseg_refused_list(stTL, opTL, dwTL, 2u, idx2, rop2, rdw2, N48_MIBSEG_DETAIL_CAP, &total2);
    expect_u("D10 TOO_LONG site: seg 0's err_op 0xFF names the S10 extra-block site", (shownTL >= 1u && rop2[0] == 0xFFu) ? 1u : 0u, 1u);
    expect_u("D11 TOO_LONG site: seg 1's err_op 0xFC names the S11/S12 re-emission site", (shownTL >= 2u && rop2[1] == 0xFCu) ? 1u : 0u, 1u);

    // An all-OK frame: 0 refused, nothing crashes, the string still builds.
    static const uint32_t okst[3] = { 0u, 0u, 0u };
    uint32_t idx3[N48_MIBSEG_DETAIL_CAP], rop3[N48_MIBSEG_DETAIL_CAP], rdw3[N48_MIBSEG_DETAIL_CAP], total3 = 9;
    const uint32_t shown3 = n48_mibseg_refused_list(okst, nullptr, nullptr, 3u, idx3, rop3, rdw3, N48_MIBSEG_DETAIL_CAP, &total3);
    expect_u("D12 an all-OK frame: 0 refused shown", shown3, 0u);
    expect_u("D13 ... 0 refused total (overwritten, not left poisoned)", total3, 0u);

    // The line, at widest numerics, under the SAME 491-byte cap f3_reader.h's kext-wide bound uses.
    char line[1024];
    // build 0.0.481: with the switch-55 units marker (n48_mib_units_mark's only non-empty value) after nseg.
    int w = std::snprintf(line, sizeof(line), N48_MIBSEG_DETAIL_FMT,
                          0xffffffffffffffffull, 0xffffffffu, 0xffffffffu, n48_mib_units_mark(1u), ".PUSVRBN|METGIH?A",
                          0xffffffffu, 0xffffffffu,
                          0u, 0xffffffffu, 0xffffffffu, 1u, 0xffffffffu, 0xffffffffu, 2u, 0xffffffffu, 0xffffffffu,
                          3u, 0xffffffffu, 0xffffffffu, 4u, 0xffffffffu, 0xffffffffu, 5u, 0xffffffffu, 0xffffffffu,
                          6u, 0xffffffffu, 0xffffffffu, 7u, 0xffffffffu, 0xffffffffu);
    if (!gQuiet) std::printf("      mibseg-detail line worst case: %d bytes (cap 491)\n", w);
    expect_u("D14 the mibseg-detail line fits under the 491-byte log cap", w > 0 && (unsigned)w <= 491 ? 1u : 0u, 1u);
    return gFail - f0;
}

// The kext wiring, pinned (supplementary to the behaviour above): noted once per pass, printed only by the report.
static void mibseg_source_pins(const char *path)
{
    long n = 0;
    char *src = mib_pin_slurp(path, &n);
    if (!src) { std::printf("  SKIP mibseg source pins: cannot read %s\n", path); gFail++; gRun++; return; }
    expect_u("PIN mibseg: the frame is noted exactly once (gfxsrc_policy, after the segment loop)",
             mib_pin_count(src, "n48_mibseg_note_frame(&gMibSeg, gXdC.judged + 1u, f->nib"), 1u);
    // build 0.0.474 item 2: the `if (f->nib >= 2u)` line now opens a BLOCK (the new per-frame detail line
    // shares its scope), so the pin follows the note call by one line (the clamp) instead of directly.
    expect_u("PIN mibseg: ... only for nib >= 2", mib_pin_count(src, "if (f->nib >= 2u) {\n        const uint32_t nsClamped ="), 1u);
    expect_u("PIN mibseg: every segment's FINAL status and the translator's own err_op are recorded",
             mib_pin_count(src, "gMibSegSt[k] = st; gMibSegOp[k] = ds.err_op;"), 1u);
    const char *copyGuard = "st = N48_SEG_COPY_OVERLAP;";
    const char *record = "gMibSegSt[k] = st; gMibSegOp[k] = ds.err_op;";
    const char *a = std::strstr(src, copyGuard), *b = std::strstr(src, record);
    expect_u("PIN mibseg: ... AFTER the copy-guard check can rewrite `st`", (a && b && a < b) ? 1u : 0u, 1u);
    // build 0.0.447 (item 6): now SIX report lines (A0/A1/A2/B1/B2/C), still printed by the ONE function.
    expect_u("PIN mibseg: the six lines are printed by ONE report function", mib_pin_count(src, "HWLOG(N48_MIBSEG_FMT_A0"), 1u);
    expect_u("PIN mibseg: ... which is called exactly once (the gfxneuter report)", mib_pin_count(src, "    mibseg_report_lines();\n"), 1u);
    // build 0.0.482 (item 3): the five aggregate lines' own units marker is latched, never cleared mid-boot,
    // and only where unitMap itself is computed (gfxsrc_units_form) - never at gXdBuild.units' per-frame reset.
    expect_u("PIN item3: the boot flag is latched only where unitMap is computed",
             mib_pin_count(src, "gUnitsFormedBoot = 1u;   // build 0.0.482"), 1u);
    expect_u("PIN item3: the aggregate marker is read once and reused for all five lines",
             mib_pin_count(src, "const char *unitsBoot = n48_mib_units_mark(gUnitsFormedBoot);"), 1u);
    expect_u("PIN item3: A0 is called with the marker before its own ARGS", mib_pin_count(src, "HWLOG(N48_MIBSEG_FMT_A0, unitsBoot, N48_MIBSEG_ARGS_A0(&gMibSeg));"), 1u);
    expect_u("PIN item3: A1 is called with the marker before its own ARGS", mib_pin_count(src, "HWLOG(N48_MIBSEG_FMT_A1, unitsBoot, N48_MIBSEG_ARGS_A1(&gMibSeg));"), 1u);
    expect_u("PIN item3: A2 is called with the marker before its own ARGS", mib_pin_count(src, "HWLOG(N48_MIBSEG_FMT_A2, unitsBoot, N48_MIBSEG_ARGS_A2(&gMibSeg));"), 1u);
    expect_u("PIN item3: B1 is called with the marker before its own ARGS", mib_pin_count(src, "HWLOG(N48_MIBSEG_FMT_B1, unitsBoot, N48_MIBSEG_ARGS_B1(&gMibSeg));"), 1u);
    expect_u("PIN item3: B2 is called with the marker before its own ARGS", mib_pin_count(src, "HWLOG(N48_MIBSEG_FMT_B2, unitsBoot, N48_MIBSEG_ARGS_B2(&gMibSeg));"), 1u);
    // C (mibseg-other:) never took the marker - it has no "mibseg:" prefix of its own to label.
    expect_u("PIN item3: the histogram line C is UNCHANGED (no marker)", mib_pin_count(src, "HWLOG(N48_MIBSEG_FMT_C, N48_MIBSEG_ARGS_C(&gMibSeg));"), 1u);
    // build 0.0.474 item 2 — the per-frame mibseg-detail line's own wiring: err_in_dword recorded alongside
    // err_op (same lifetime as gMibSegSt/Op), and the cap gate REACHED via `<` (never `<=`, which would let one
    // extra line through after the cap).
    expect_u("PIN item2: err_in_dword is recorded beside err_op, same segment loop", mib_pin_count(src, "gMibSegDw[k] = ds.err_in_dword;"), 1u);
    // build 0.0.521 Part C: the gate and the count moved INTO gfx_clock88.h's n48_c88_detail_take (its
    // strict `*lines < cap` and its increments are proven in tests/gfx_clock88_test.cpp DETAIL/BOOT); a producer frame has a cap
    // of its own. These pins now hold the call, its place right before the line is built, and the unconditional frame count.
    expect_u("PIN item2 (0.0.521): the boot cap is n48_c88_detail_take over the general counter and cap",
             mib_pin_count(src, "if (n48_c88_detail_take(&gXdMibsegDetailLines, kXdMibsegDetailLines, &gXdMibsegDetailSLines,"), 1u);
    expect_u("PIN item2 (0.0.521): nothing else counts the general cap (the helper does, only when it answers non-zero)",
             mib_pin_count(src, "gXdMibsegDetailLines++;") + mib_pin_count(src, "if (gXdMibsegDetailLines < kXdMibsegDetailLines) {"), 0u);
    expect_u("PIN item2 (0.0.521): the gate sits right before the line is built",
             mib_pin_count(src, "gfxsrc_c88_producer_take(N48_C88_TGT_K_DETAIL, N48_C88_TGT_DETAIL))) {\n            n48_mibseg_frame_str("), 1u);
    // 0.0.521: the counter is its own statement at the start of its line (a plant making it conditional kept the old substring)
    expect_u("PIN item2: the boot-total frame counter moves UNCONDITIONALLY (its own statement, right before the cap gate)",
             mib_pin_count(src, "no new state, no new decision.\n        gXdMibsegDetailFrames++;\n        // build 0.0.521 Part C: a content-producer frame is logged under its own cap first (n48_c88_detail_take counts it)\n        if (n48_c88_detail_take("), 1u);
    expect_u("PIN item2: the new boot-total report line is called exactly once", mib_pin_count(src, "    mibseg_detail_report_line();\n"), 1u);
    std::free(src);
}

// build 0.0.480 (notes/design/CONTINUATION-UNITS.md Q10/Q11): the continuation-unit checks, over decide44's real bytes
// in the kext's order (segment, unit, translate, fence, provenance, gate). `N48_U480_PRINT=1` prints each measured value.
#include "gfx_mib_units_checks.h"
#include "gfx_mib_xib_checks.h"   // build 0.0.505 (CROSS-IB.md T1-T8)
#include "gfx_mib_defer511_checks.h"   // build 0.0.511 (switch 70's ordering hole, run10t F107)

int main(int argc, char **argv)
{
    std::printf("== MIB-0: the classifier over the real header ==\n");
    (void)mib_checks(MIB_MUT_NONE);

    std::printf("\n== MIB-0: the counters and the report line ==\n");
    {
        n48_mib0 m {};
        n48_mib0_note_ib(&m, N48_MIB_START_HEAD);
        n48_mib0_note_ib(&m, N48_MIB_START_HEAD);
        n48_mib0_note_ib(&m, N48_MIB_START_NOP_HEAD);
        n48_mib0_note_ib(&m, N48_MIB_START_MID);
        n48_mib0_note_ib(&m, N48_MIB_START_UNREAD);
        expect_u("counters: four IB classes are kept apart", m.ib_start[N48_MIB_START_HEAD], 2u);
        expect_u("counters: NOP-HEAD is its own class", m.ib_start[N48_MIB_START_NOP_HEAD], 1u);
        expect_u("counters: MID is its own class", m.ib_start[N48_MIB_START_MID], 1u);
        expect_u("counters: UNREAD is its own class (never folded)", m.ib_start[N48_MIB_START_UNREAD], 1u);

        n48_mib0_note_frame(&m, 3u);
        expect_u("counters: the frame is counted by nib", m.frames_by_nib[3], 1u);
        n48_mib0_note_policy(&m, 2u, 1500ull);
        n48_mib0_note_policy(&m, 2u, 500ull);
        expect_u("counters: policy ns accumulate by nib", m.policy_ns[2], 2000u);
        expect_u("counters: policy runs by nib", m.policy_runs[2], 2u);
        n48_mib0_note_commit(&m, 2u);
        expect_u("counters: commits by nib", m.commits[2], 1u);

        n48_mib0_note_f828(&m, N48_F828_OK);
        n48_mib0_note_f828(&m, N48_F828_WALK);
        n48_mib0_note_f828(&m, N48_F828_NO_NOP);
        expect_u("counters: the final-segment find answer is tallied", m.f828_answer[N48_F828_OK], 1u);
        expect_u("counters: ... and so is a refusal", m.f828_answer[N48_F828_WALK], 1u);
        expect_u("counters: offered counts every look", m.f828_offered, 3u);
        expect_u("counters: refused = offered - OK", n48_mib0_f828_refused(&m), 2u);

        char line[512];
        const int w = std::snprintf(line, sizeof(line), N48_MIB0_FMT, N48_MIB0_ARGS(&m));
        if (!gQuiet) std::printf("      report at real counts: %d bytes\n", w);
        expect_u("report: formats", w > 0 ? 1u : 0u, 1u);
        expect_u("report: names itself gfx-mib", std::strncmp(line, "gfx-mib:", 8) == 0 ? 1u : 0u, 1u);
        expect_u("report: fits the logger body cap at real counts", (unsigned)w < 480u ? 1u : 0u, 1u);
    }
    {
        // WIDEST NUMERICS. Every field at its maximum, with f828 OK 1 and offered max so `refused` is 10 digits too:
        // 19 numbers of up to 10 digits plus about 210 bytes of text. The line must still fit.
        n48_mib0 m {};
        for (uint32_t i = 0; i < N48_MIB_STARTS; i++) m.ib_start[i] = 0xFFFFFFFFFFFFFFFFull;
        for (uint32_t i = 0; i < N48_MIB_NIB; i++) { m.policy_ns[i] = 0xFFFFFFFFFFFFFFFFull; m.policy_runs[i] = 0xFFFFFFFFFFFFFFFFull; m.commits[i] = 0xFFFFFFFFFFFFFFFFull; }
        for (uint32_t i = 0; i < N48_F828_REASONS; i++) m.f828_answer[i] = 0u;
        m.f828_answer[N48_F828_OK] = 1ull;
        m.f828_offered = 0xFFFFFFFFFFFFFFFFull;
        char line[512];
        const int w = std::snprintf(line, sizeof(line), N48_MIB0_FMT, N48_MIB0_ARGS(&m));
        if (!gQuiet) std::printf("      report at widest numerics: %d bytes (cap 512, body cap 491)\n", w);
        expect_u("report: fits 512 even at widest numerics", (w > 0 && (unsigned)w < 480u) ? 1u : 0u, 1u);
        expect_u("report: is ONE line (no newline in it)", std::strchr(line, '\n') == nullptr ? 1u : 0u, 1u);
    }
    // 0.0.430: THE SEGMENT-STAGE REFUSAL COUNTERS AND THEIR OWN LINE. The fields are fed by
    // n48_mib0_note_seg from one n48_mib_seg_diag, and the line is bounded at its widest like the census line above.
    {
        n48_mib0 m {};
        n48_mib_seg_diag d {};
        d.zero_by_class[N48_MIB_START_MID] = 1u;
        d.zero_by_class[N48_MIB_START_NOP_HEAD] = 2u;
        d.overflow = 1u;
        d.max_segs = 31u;
        n48_mib0_note_seg(&m, &d);
        n48_mib_seg_diag d2 {};
        d2.max_segs = 32u;                                     // a later, larger frame moves the high-water
        n48_mib0_note_seg(&m, &d2);
        expect_u("a counters: the zero-segment classes are kept apart", m.seg_zero[N48_MIB_START_MID], 1u);
        expect_u("a counters: ... and NOP-HEAD is its own", m.seg_zero[N48_MIB_START_NOP_HEAD], 2u);
        expect_u("a counters: ... and HEAD is untouched", m.seg_zero[N48_MIB_START_HEAD], 0u);
        expect_u("a counters: the overflow is counted per frame", m.seg_overflow, 1u);
        expect_u("a counters: max segments seen keeps the HIGH-water", m.seg_max, 32u);
        expect_u("a counters: the runs are the denominator", m.seg_frames, 2u);

        char line[512];
        const int w = std::snprintf(line, sizeof(line), N48_MIB0_SEG_FMT, N48_MIB0_SEG_ARGS(&m));
        if (!gQuiet) std::printf("      segment report at real counts: %d bytes\n", w);
        expect_u("a report: names itself gfx-mib", std::strncmp(line, "gfx-mib:", 8) == 0 ? 1u : 0u, 1u);
        expect_u("a report: fits the body cap at real counts", (w > 0 && (unsigned)w < 480u) ? 1u : 0u, 1u);
        expect_u("a report: is ONE line", std::strchr(line, '\n') == nullptr ? 1u : 0u, 1u);
    }
    {
        // WIDEST: every refusal field at its 10-digit maximum.
        n48_mib0 m {};
        for (uint32_t i = 0; i < N48_MIB_STARTS; i++) m.seg_zero[i] = 0xFFFFFFFFFFFFFFFFull;
        m.seg_overflow = 0xFFFFFFFFFFFFFFFFull;
        m.seg_max      = 0xFFFFFFFFFFFFFFFFull;
        m.seg_frames   = 0xFFFFFFFFFFFFFFFFull;
        char line[512];
        const int w = std::snprintf(line, sizeof(line), N48_MIB0_SEG_FMT, N48_MIB0_SEG_ARGS(&m));
        if (!gQuiet) std::printf("      segment report at widest numerics: %d bytes (cap 512, body cap 491)\n", w);
        expect_u("a report: fits 512 at widest numerics", (w > 0 && (unsigned)w < 480u) ? 1u : 0u, 1u);
        expect_u("a report: is ONE line at widest", std::strchr(line, '\n') == nullptr ? 1u : 0u, 1u);
    }
    // 0.0.430: THE SINGLE-IB POLICY PHASE LINE. No behaviour is asserted here - only the format and the
    // bound, because that is the one part of the instrument a host can prove; the values are uptime deltas on hardware.
    {
        n48_mib_pol p {};
        p.setup_ns = 1234ull; p.xlat_ns = 9876543ull; p.desc_ns = 56789ull;
        p.cons_ns = 42ull; p.fence_ns = 7ull; p.log_ns = 999999999ull; p.runs = 3ull;
        char line[512];
        const int w = std::snprintf(line, sizeof(line), N48_MIB0_POL_FMT, N48_MIB0_POL_ARGS(&p));
        if (!gQuiet) std::printf("      policy phase report at real counts: %d bytes\n", w);
        expect_u("c report: names itself gfx-mib", std::strncmp(line, "gfx-mib:", 8) == 0 ? 1u : 0u, 1u);
        expect_u("c report: fits the body cap", (w > 0 && (unsigned)w < 480u) ? 1u : 0u, 1u);
        expect_u("c report: is ONE line", std::strchr(line, '\n') == nullptr ? 1u : 0u, 1u);
        expect_u("c report: names the phases and their unit",
                 (std::strstr(line, "setup") && std::strstr(line, "translate") && std::strstr(line, "consumer") &&
                  std::strstr(line, "fence") && std::strstr(line, "log") && std::strstr(line, "(us accumulated)")) ? 1u : 0u, 1u);
        // The fold: two contributions add, and `runs` counts passes, never frames with no pass.
        n48_mib_pol acc {}, one {};
        one.setup_ns = 5ull; one.xlat_ns = 50ull; one.desc_ns = 1ull; one.cons_ns = 2ull;
        one.fence_ns = 3ull; one.log_ns = 4ull; one.runs = 1ull;
        n48_mib0_note_pol(&acc, &one);
        n48_mib0_note_pol(&acc, &one);
        expect_u("c fold: setup accumulates", acc.setup_ns, 10u);
        expect_u("c fold: translate accumulates", acc.xlat_ns, 100u);
        expect_u("c fold: descriptor reads accumulate", acc.desc_ns, 2u);
        expect_u("c fold: runs count passes", acc.runs, 2u);
        n48_mib_pol widest {};
        widest.setup_ns = widest.xlat_ns = widest.desc_ns = widest.cons_ns = widest.fence_ns = widest.log_ns = 0xFFFFFFFFFFFFFFFFull;
        widest.runs = 0xFFFFFFFFFFFFFFFFull;
        const int w2 = std::snprintf(line, sizeof(line), N48_MIB0_POL_FMT, N48_MIB0_POL_ARGS(&widest));
        if (!gQuiet) std::printf("      policy phase report at widest numerics: %d bytes\n", w2);
        expect_u("c report: fits 512 at widest numerics", (w2 > 0 && (unsigned)w2 < 480u) ? 1u : 0u, 1u);

        // 0.0.434: the SAME check for the nib >= 2 line (N48_MIB0_POL2_FMT), which reuses
        // N48_MIB0_POL_ARGS - only its own static text differs, and it is slightly LONGER ("multi-IB (nib>=2)"
        // vs "single-IB"), so this is the one that actually bounds the pair.
        const int w3 = std::snprintf(line, sizeof(line), N48_MIB0_POL2_FMT, N48_MIB0_POL_ARGS(&widest));
        if (!gQuiet) std::printf("      nib>=2 policy phase report at widest numerics: %d bytes\n", w3);
        expect_u(" report: names itself gfx-mib and nib>=2",
                 (std::strncmp(line, "gfx-mib:", 8) == 0 && std::strstr(line, "nib>=2")) ? 1u : 0u, 1u);
        expect_u(" report: fits the body cap at widest numerics", (w3 > 0 && (unsigned)w3 < 480u) ? 1u : 0u, 1u);
        expect_u(" report: is ONE line", std::strchr(line, '\n') == nullptr ? 1u : 0u, 1u);
    }

    std::printf("\n== MIB-COMMIT: the pure arithmetic (B1/B2/B3/B0; T4/T8/T9) ==\n");
    (void)mib_commit_helper_checks(0);

    std::printf("\n== MIB-COMMIT: the pure segment stage over REAL captured bodies (C3) ==\n");
    (void)mib_real_bodies_checks();

    std::printf("\n== 0.0.432: the disguised-head fix, over TWO independent real captures + its planted breaks ==\n");
    (void)mib_nop_disguise_checks();

    std::printf("\n== MIB-0: the segment-stage refusal reasons over the REAL bodies ==\n");
    (void)mib_seg_reason_checks(0);

    std::printf("\n== MIB-COMMIT: the write/read/compare order (C2) ==\n");
    expect_u("C2 order: disjoint IBs COMMIT under the two-pass order", mib_order_model(0u, 100u), 1u);
    expect_u("C2 order: overlapping IBs are REFUSED by the two-pass order", mib_order_model(0u, 50u), 0u);
    expect_u("C2 order: the interleaved 0.0.426 order COMMITS them (planted defect)", mib_order_model(1u, 50u), 1u);

    std::printf("\n== 0.0.446: the `mibseg:` per-segment status counter ==\n");
    (void)mibseg_checks(0);

    std::printf("\n== build 0.0.474 item 2: the per-judged-frame mibseg-detail line's two pure helpers ==\n");
    (void)mibseg_detail_checks();
    u480::gPrint = std::getenv("N48_U480_PRINT") ? 1 : 0;
    u480::checks(argc > 1 ? argv[1] : nullptr);   // build 0.0.480
    u480::checks481(argc > 1 ? argv[1] : nullptr);   // build 0.0.481 (F2, F3, the census label, switch 56)
    u480::checks491();   // build 0.0.491: the Z/AO/AN/AF rows live, and their attribution
    u480::checks501(argc > 1 ? argv[1] : nullptr);   // build 0.0.501 (switch 67, PACK)
    u480::checks506(argc > 1 ? argv[1] : nullptr);   // build 0.0.506 (switch 70, the deferred room retry)
    u480::checks508(argc > 1 ? argv[1] : nullptr);   // build 0.0.508 (LOW-1, switch 71's wiring)
    xib505::checks(argc > 1 ? argv[1] : nullptr);   // build 0.0.505 (switch 69, CROSS-IB)
    d511::checks(argc > 1 ? argv[1] : nullptr);     // build 0.0.511 (switch 70's ordering hole)

    if (argc > 1) { mib_source_pins(argv[1]); mibseg_source_pins(argv[1]); }
    else std::printf("  SKIP MIB source pins (no AppleHardwareHook.cpp argument)\n");

    std::printf("\n== MIB-0 planted defects: each must be CAUGHT ==\n");
    const int realFail = gFail;   // the mutants deliberately fail checks; only failures BEFORE the sweep count
    int caught = 0, mutants = 0;
    for (int m = MIB_MUT_NOP_AS_HEAD; m < MIB_MUT_COUNT; m++) {
        mutants++;
        gQuiet = 1;
        const int f = mib_checks(m);
        gQuiet = 0;
        if (f > 0) caught++;
        std::printf("  %-44s %s (%d check(s) failed)\n", mib_mut_name(m), f > 0 ? "CAUGHT" : "*** NOT CAUGHT ***", f);
    }
    std::printf("\n== MIB-COMMIT planted defects: each must be CAUGHT ==\n");
    for (int mu = 1; mu <= 4; mu++) {
        mutants++;
        gQuiet = 1;
        const int f = mib_commit_helper_checks(mu);
        gQuiet = 0;
        if (f > 0) caught++;
        std::printf("  %-52s %s (%d check(s) failed)\n", mib_helper_mut_name(mu), f > 0 ? "CAUGHT" : "*** NOT CAUGHT ***", f);
    }
    std::printf("\n== MIB-0a planted defects: each must be CAUGHT ==\n");
    for (int plant = 1; plant <= 3; plant++) {
        mutants++;
        gQuiet = 1; gFailOnly = 1;
        const int f = mib_seg_reason_checks(plant);
        gQuiet = 0; gFailOnly = 0;
        if (f > 0) caught++;
        std::printf("  %-60s %s (%d check(s) failed)\n", mib_seg_plant_name(plant), f > 0 ? "CAUGHT" : "*** NOT CAUGHT ***", f);
    }

    std::printf("\n== 0.0.446 mibseg planted defect: must be CAUGHT ==\n");
    {
        mutants++;
        gQuiet = 1; gFailOnly = 1;
        const int f = mibseg_checks(1);
        gQuiet = 0; gFailOnly = 0;
        if (f > 0) caught++;
        std::printf("  %-60s %s (%d check(s) failed)\n", "PROVENANCE miscounted into other-desc",
                    f > 0 ? "CAUGHT" : "*** NOT CAUGHT ***", f);
    }
    std::printf("gfx_mib: %d check(s), %d failed; planted defects %d of %d caught\n", gRun, realFail, caught, mutants);
    const bool pass = (realFail == 0 && caught == mutants);
    std::printf("%s\n", pass ? "N48-MIB-TEST-PASS" : "N48-MIB-TEST-FAIL");
    return pass ? 0 : 1;
}
