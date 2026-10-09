// gfx_commit_test.cpp — X1's safety proof, offline. The property under test is the project's own
// specification of the COMMIT step:
//
//     "commit_ok with a rewrite that did not read back clean" MUST NEUTER.
//
// It is checked EXHAUSTIVELY, not by example: over the two IB lengths the live compositor actually submits (1040 and 1472
// dwords, run sub1's own frame lines), every single-dword corruption of the read-back is fed through the real comparator, the
// real gate and the real n48_sd_action, and every one of them must come out N48_SD_ACT_NEUTER. Then the other ways a rewrite
// can be not-clean — a short read, a read that crossed a page that was not host memory, a page that did not resolve, a segment
// that did not translate, a translation that changed length, segments that do not tile the IB, a second IB, a frame identity
// that does not match — each on its own, each of which must also neuter.
//
// PLANTED-DEFECT CONTROL (rule: a test no mutation can break is not testing anything). Seven mutant gates are run against the
// SAME checks; each is a plausible way to write this wrong, including the two fail-open shapes the review found in the
// ladder next door, and each must be CAUGHT by at least one named check.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_commit_test.cpp -o /tmp/cmtest && /tmp/cmtest
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "gfx_commit.h"
#include "gfx_fillset.h"   // 0.0.408: the retirement, and the reserved-fill frame the caller builds
#include "gfx_dep.h"       // 0.0.408: the dependency world's OWN reason codes (the retirement's real input)
#include "gfx_capture_scan.h"      // 0.0.457: switch 54's scan step, driven in the kext's order below
#include "fixture_f84_decide44.h"  // 0.0.457: decide44 F84's two real IB bodies (a two-head 7|5 frame)
#include "gfx_lutfill.h"   // build 0.0.550: switch 107's record decode (the model's learn)
#include "gfx_lut107.h"    // build 0.0.550: switch 107, the LUT learn retry and the plane's fail-closed gate inputs
#include "gfx_bb552.h"     // build 0.0.552: switches 109 (the resprov table's capacity) and 110 (the AN row)
#include "gfx_lutidx111.h"  // build 0.0.553: switch 111, the LUT learn reads the heap entry the draw names
#include "ws_resprov.h"    // build 0.0.552: N48_RP_CAP_ON / _OFF / N48_RP_MAX for switch 109's line
#include <string>

static int gFail = 0, gRun = 0, gQuiet = 0;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) std::printf("FAIL  %-72s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
    } else if (!gQuiet) {
        std::printf("ok    %-72s %#llx\n", what, (unsigned long long)got);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// A frame that passes every rung: one IB of `n` dwords, `nseg` segments tiling it head to head, every segment translated
// clean and to its own length, every page host memory, the write and the read-back complete, nothing mismatched, the token
// matched. Anything a test wants to break, it breaks on a copy of this.
// ---------------------------------------------------------------------------------------------------------------------
static void good_frame(n48_cm_frame &c, uint32_t n, uint32_t nseg)
{
    std::memset(&c, 0, sizeof(c));
    c.arm = N48_SD_ARM_COMMIT;
    c.verdict = N48_XV_TRANSLATE;
    c.buffers_ok = 1u;
    c.nib = 1u;
    c.n = n;
    c.cap = 32768u;
    c.nseg = nseg;
    uint32_t at = 0;
    for (uint32_t k = 0; k < nseg; k++) {
        const uint32_t end = (k + 1u == nseg) ? n : at + (n / nseg);
        c.seg[k].head = at;
        c.seg[k].start = at + 2u;
        c.seg[k].end = end;
        c.seg[k].status = 0u;
        c.seg[k].out_len = end - (at + 2u);
        at = end;
    }
    c.pages = (n * 4u + 0xfffu) / 0x1000u;
    c.sys_pages = c.pages;
    c.wrote = n;
    c.got = n;
    c.back_sys_pages = c.pages;
    c.mismatch = 0u;
    c.token_ok = 1u;
    // 0.0.353: X9's rung. When gfx_commit.h gained `dep_ok`, THIS LINE WAS MISSING and eight checks below
    // failed at once — the positive control stopped committing. That failure is the property, not an accident: a field
    // nobody sets is 0 and 0 refuses, so a new requirement cannot be added to the gate and then quietly satisfied by
    // default. It is left recorded here because the next field added to n48_cm_frame will do exactly the same thing, and
    // the right response is to fill it, never to relax the rung. gfx_dep_test.cpp owns what makes it 1.
    c.dep_ok = 1u;
    // 0.0.369: the SAME thing happened again, exactly as the paragraph above predicted. When gfx_commit.h gained
    // `seg_kind`, every check here failed at once because a frame that does not say which recogniser found its segments is
    // not a frame this gate can judge. Filled, never relaxed.
    c.seg_kind = N48_CM_KIND_ENCODER;
}

// ---------------------------------------------------------------------------------------------------------------------
// The other legitimate shape (0.0.369): the compositor's setup halves as xlat12_ib_headless_passes returns them
// — one segment per render pass, `start == head` (the pass's own CONTEXT_CONTROL, which the draw policy TRANSLATES; no dword
// of Apple's survives), and the three observations the caller must have made before it may claim the kind.
// ---------------------------------------------------------------------------------------------------------------------
static void good_headless_frame(n48_cm_frame &c, uint32_t n, uint32_t nseg)
{
    good_frame(c, n, nseg);
    c.seg_kind = N48_CM_KIND_HEADLESS;
    uint32_t at = 0;
    for (uint32_t k = 0; k < nseg; k++) {
        const uint32_t end = (k + 1u == nseg) ? n : at + (n / nseg);
        c.seg[k].head = at;
        c.seg[k].start = at;            // the pass opens at its own CONTEXT_CONTROL
        c.seg[k].end = end;
        c.seg[k].out_len = end - at;
        at = end;
    }
    c.hl_ib_segments = 0u;              // xlat12_ib_segments found NOTHING on this same buffer
    c.hl_ok = 1u;                       // the recogniser's report was XLAT12_HL_OK
    c.hl_total = nseg;                  // and it found exactly as many passes as the table holds
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.426 (MIB-COMMIT B5) — A MULTI-IB FRAME THAT PASSES EVERY RUNG. Two IBs (the F44-family sizes 15,520 + 7,616), each
// translated on its own (two segments apiece), the per-IB evidence clean, the frame identity over both. The IB boundary is
// 15,520, so it must fall exactly on a segment end (seg[1].end == 15,520) — that is the COVER-per-IB property B5 adds.
// ---------------------------------------------------------------------------------------------------------------------
static const uint32_t kMibN0 = 15520u, kMibN1 = 7616u, kMibOff1 = 15520u, kMibTotal = 23136u;

static void good_mib_frame(n48_cm_frame &c)
{
    good_frame(c, kMibTotal, 4u);   // one IB's shape first, then overwritten into the multi-IB one
    c.nib = 2u; c.mib = 1u;
    c.ib_n[0] = kMibN0; c.ib_n[1] = kMibN1;
    c.ib_nseg[0] = 2u; c.ib_nseg[1] = 2u;
    // segments tile [0, kMibTotal) with the IB boundary at kMibOff1 on a segment end
    c.seg[0].head = 0u;          c.seg[0].start = 2u;      c.seg[0].end = 8000u;
    c.seg[1].head = 8000u;       c.seg[1].start = 8002u;   c.seg[1].end = kMibOff1;
    c.seg[2].head = kMibOff1;    c.seg[2].start = kMibOff1 + 2u; c.seg[2].end = 20000u;
    c.seg[3].head = 20000u;      c.seg[3].start = 20002u;  c.seg[3].end = kMibTotal;
    for (uint32_t k = 0; k < 4u; k++) { c.seg[k].status = 0u; c.seg[k].out_len = c.seg[k].end - c.seg[k].start; }
    c.nseg = 4u;
    for (uint32_t k = 0; k < 2u; k++) {
        c.pages_ib[k] = (c.ib_n[k] * 4u + 0xfffu) / 0x1000u;
        c.sys_pages_ib[k] = c.pages_ib[k];
        c.wrote_ib[k] = c.ib_n[k];
        c.got_ib[k] = c.ib_n[k];
        c.back_sys_pages_ib[k] = c.pages_ib[k];
        c.mismatch_ib[k] = 0u;
    }
    // index 0 is today's scalars (B5)
    c.pages = c.pages_ib[0]; c.sys_pages = c.sys_pages_ib[0];
    c.wrote = c.wrote_ib[0]; c.got = c.got_ib[0];
    c.back_sys_pages = c.back_sys_pages_ib[0]; c.mismatch = c.mismatch_ib[0];
}

// The gate under test, swappable so a planted defect can be run against the same checks.
typedef uint32_t (*GateFn)(const n48_cm_frame *, uint32_t *);
static GateFn gGate = &n48_cm_gate;
/* 0.0.370 (notes 807): the kind's NAME is now printed on the COMMIT-rehearsal line and on the per-frame ws# verdict line, so
 * a run can READ which recogniser carried a frame. A wrong name there is not a gate defect - the gate still refuses - it is a
 * RECORD defect, and 806 shows what that costs: hp9 had to leave "the recogniser carried those three frames" SUSPECTED. */
typedef const char *(*KindFn)(uint32_t);
static KindFn gKindName = &n48_cm_kind_name;

static uint32_t action_for(const n48_cm_frame &c)
{
    uint32_t d = 0;
    const uint32_t ok = gGate(&c, &d) == N48_CM_OK ? 1u : 0u;
    return n48_sd_action(c.arm, 1u /* shape_ok */, c.verdict, ok);
}

// ---------------------------------------------------------------------------------------------------------------------
// 1. THE MUTANT, EXHAUSTIVELY: every single-dword corruption of the read-back must neuter.
// ---------------------------------------------------------------------------------------------------------------------
static void mutant_every_dword(uint32_t n)
{
    std::vector<uint32_t> want(n), back(n);
    for (uint32_t i = 0; i < n; i++) want[i] = 0xC0021000u ^ (i * 2654435761u);
    back = want;

    // Control 1 (POSITIVE): the uncorrupted read-back commits. Without this the sweep below could pass on a gate that
    // refuses everything, which is the blind-method failure this project has already paid for once.
    n48_cm_frame c;
    good_frame(c, n, 3u);
    uint32_t first = 0;
    c.mismatch = n48_cm_compare(want.data(), back.data(), n, &first);
    expect_u("clean read-back: mismatch count", c.mismatch, 0u);
    expect_u("clean read-back: gate", gGate(&c, &first), N48_CM_OK);
    expect_u("clean read-back: action is TRANSLATE", action_for(c), N48_SD_ACT_TRANSLATE);

    // The sweep. Every dword, every corruption: flip the low bit, so the mutation is minimal and cannot be spotted by a
    // magnitude test.
    uint32_t swept = 0, neutered = 0, named = 0, byMismatch = 0;
    for (uint32_t i = 0; i < n; i++) {
        back[i] = want[i] ^ 1u;
        n48_cm_frame m = c;
        m.mismatch = n48_cm_compare(want.data(), back.data(), n, &first);
        uint32_t d = 0;
        if (m.mismatch == 1u && first == i) named++;
        if (gGate(&m, &d) == N48_CM_MISMATCH) byMismatch++;
        if (action_for(m) == N48_SD_ACT_NEUTER) neutered++;
        back[i] = want[i];
        swept++;
    }
    char lbl[128];
    std::snprintf(lbl, sizeof(lbl), "IB %u dw: every single-dword corruption NEUTERS", n);
    expect_u(lbl, neutered, swept);
    std::snprintf(lbl, sizeof(lbl), "IB %u dw: comparator names the corrupted dword", n);
    expect_u(lbl, named, swept);
    std::snprintf(lbl, sizeof(lbl), "IB %u dw: and the gate's reason is READ-BACK-MISMATCH", n);
    expect_u(lbl, byMismatch, swept);

    // Control 2 (NEGATIVE, on the method): a corruption the comparator is not shown must NOT be reported. If the frame's
    // mismatch field were filled from anything other than the comparison, this would still read 0.
    back[0] = want[0] ^ 0xFFFFFFFFu;
    n48_cm_frame blind = c;
    blind.mismatch = n48_cm_compare(want.data(), want.data(), n, &first);   // compared against ITSELF
    expect_u("method control: comparing a buffer with itself reports 0", blind.mismatch, 0u);
    expect_u("method control: and that frame still commits", (uint64_t)action_for(blind), N48_SD_ACT_TRANSLATE);
    back[0] = want[0];

    // Every whole-buffer corruption count is reported, not clamped to a bool.
    for (uint32_t i = 0; i < n; i++) back[i] = ~want[i];
    expect_u("all dwords corrupted: the COUNT is n", n48_cm_compare(want.data(), back.data(), n, &first), n);
    n48_cm_frame all = c;
    all.mismatch = n;
    expect_u("all dwords corrupted: action NEUTER", action_for(all), N48_SD_ACT_NEUTER);
}

// ---------------------------------------------------------------------------------------------------------------------
// 2. Every OTHER way a rewrite can fail to be a verified-clean rewrite. Each must neuter, and each must name its own rung.
// ---------------------------------------------------------------------------------------------------------------------
static void every_other_refusal()
{
    n48_cm_frame base;
    good_frame(base, 1040u, 3u);
    uint32_t d = 0;

    expect_u("baseline commits", gGate(&base, &d), N48_CM_OK);

    struct Case { const char *what; uint32_t reason; void (*break_it)(n48_cm_frame &); };
    static const Case cases[] = {
        { "arm OFF",                    N48_CM_NOT_ARMED,     [](n48_cm_frame &c){ c.arm = N48_SD_ARM_OFF; } },
        { "arm DECIDE",                 N48_CM_NOT_ARMED,     [](n48_cm_frame &c){ c.arm = N48_SD_ARM_DECIDE; } },
        { "no scratch buffers",         N48_CM_NO_BUFFERS,    [](n48_cm_frame &c){ c.buffers_ok = 0u; } },
        { "verdict target-in-vram",     N48_CM_NOT_TRANSLATE, [](n48_cm_frame &c){ c.verdict = N48_XV_TARGET_VRAM; } },
        { "verdict segment-policy",     N48_CM_NOT_TRANSLATE, [](n48_cm_frame &c){ c.verdict = N48_XV_SEG_POLICY; } },
        { "two IBs",                    N48_CM_MULTI_IB,      [](n48_cm_frame &c){ c.nib = 2u; } },
        { "zero IBs",                   N48_CM_MULTI_IB,      [](n48_cm_frame &c){ c.nib = 0u; } },
        { "empty IB",                   N48_CM_LEN,           [](n48_cm_frame &c){ c.n = 0u; } },
        { "IB longer than the scratch", N48_CM_LEN,           [](n48_cm_frame &c){ c.cap = c.n - 1u; } },
        { "no segments",                N48_CM_NO_SEGMENTS,   [](n48_cm_frame &c){ c.nseg = 0u; } },
        { "more segments than rows",    N48_CM_NO_SEGMENTS,   [](n48_cm_frame &c){ c.nseg = N48_XV_MAX_SEGS + 1u; } },
        { "a segment refused",          N48_CM_SEG_REFUSED,   [](n48_cm_frame &c){ c.seg[1].status = 21u; } },
        { "a translation grew",         N48_CM_SEG_LEN,       [](n48_cm_frame &c){ c.seg[1].out_len++; } },
        { "a translation shrank",       N48_CM_SEG_LEN,       [](n48_cm_frame &c){ c.seg[1].out_len--; } },
        { "a gap between segments",     N48_CM_COVER,         [](n48_cm_frame &c){ c.seg[1].head += 4u; } },
        { "the last segment stops short",N48_CM_COVER,        [](n48_cm_frame &c){ c.seg[2].end -= 8u; c.seg[2].out_len -= 8u; } },
        { "the first head is not 0",    N48_CM_COVER,         [](n48_cm_frame &c){ c.seg[0].head = 2u; } },
        // 0.0.369 (notes 804): the start rule is the KIND's rule now, so it refuses on its own rung and names itself. The
        // second of these is a MIXED frame - segment 1 has the headless shape in a frame that declared ENCODER.
        { "a body not 2 after its head",N48_CM_SEG_KIND,      [](n48_cm_frame &c){ c.seg[1].start++; } },
        { "MIXED: one headless segment",N48_CM_SEG_KIND,      [](n48_cm_frame &c){ c.seg[1].start = c.seg[1].head; c.seg[1].out_len = c.seg[1].end - c.seg[1].start; } },
        { "no kind was declared",       N48_CM_SEG_KIND,      [](n48_cm_frame &c){ c.seg_kind = N48_CM_KIND_UNSET; } },
        { "an unknown kind",            N48_CM_SEG_KIND,      [](n48_cm_frame &c){ c.seg_kind = 77u; } },
        { "a segment past the IB",      N48_CM_COVER,         [](n48_cm_frame &c){ c.seg[2].end = c.n + 4u; c.seg[2].out_len += 4u; } },
        { "no page was walked",         N48_CM_PAGE,          [](n48_cm_frame &c){ c.pages = 0u; c.sys_pages = 0u; } },
        { "one page did not resolve",   N48_CM_PAGE,          [](n48_cm_frame &c){ c.sys_pages--; } },
        { "one page was VRAM",          N48_CM_PAGE,          [](n48_cm_frame &c){ c.sys_pages = c.pages - 1u; } },
        { "the write was short",        N48_CM_WRITE_SHORT,   [](n48_cm_frame &c){ c.wrote = c.n - 1u; } },
        { "nothing was written",        N48_CM_WRITE_SHORT,   [](n48_cm_frame &c){ c.wrote = 0u; } },
        { "the read-back was short",    N48_CM_READ_SHORT,    [](n48_cm_frame &c){ c.got = c.n - 1u; } },
        { "nothing was read back",      N48_CM_READ_SHORT,    [](n48_cm_frame &c){ c.got = 0u; } },
        { "the read-back crossed VRAM", N48_CM_READ_VRAM,     [](n48_cm_frame &c){ c.back_sys_pages--; } },
        { "one dword did not read back",N48_CM_MISMATCH,      [](n48_cm_frame &c){ c.mismatch = 1u; } },
        { "the frame identity differs", N48_CM_TOKEN,         [](n48_cm_frame &c){ c.token_ok = 0u; } },
    };
    for (const Case &k : cases) {
        n48_cm_frame c = base;
        k.break_it(c);
        char lbl[160];
        std::snprintf(lbl, sizeof(lbl), "%-32s -> %s", k.what, n48_cm_reason_name(k.reason));
        expect_u(lbl, gGate(&c, &d), k.reason);
        std::snprintf(lbl, sizeof(lbl), "%-32s -> NEUTER", k.what);
        expect_u(lbl, action_for(c), N48_SD_ACT_NEUTER);
    }

    // A zero-initialised frame — the shape a caller that forgot every step leaves behind — must refuse at the FIRST rung.
    n48_cm_frame zero;
    std::memset(&zero, 0, sizeof(zero));
    expect_u("a frame nobody filled refuses", gGate(&zero, &d), N48_CM_NOT_ARMED);
    expect_u("a frame nobody filled neuters", action_for(zero), N48_SD_ACT_NEUTER);
    expect_u("a null frame refuses", gGate(nullptr, &d), N48_CM_NOT_ARMED);
    expect_u("n48_cm_commit_ok on a zero frame", n48_cm_commit_ok(&zero), 0u);
    expect_u("n48_cm_commit_ok on a good frame", n48_cm_commit_ok(&base), 1u);
}

// ---------------------------------------------------------------------------------------------------------------------
// 3. The frame identity: "we rewrote SOME frame" must never be read as "we rewrote THIS frame".
// ---------------------------------------------------------------------------------------------------------------------
static void token_checks()
{
    static const char block[8] = { 0 };
    n48_cm_token a {};
    a.info = &block[0]; a.va = 0x400004000ull; a.stamp = 0x10u; a.n = 1040u; a.nib = 1u; a.seq = 7u;
    expect_u("token matches itself", n48_cm_token_match(&a, &a), 1u);
    struct { const char *what; void (*brk)(n48_cm_token &); } cases[] = {
        { "a different submission block", [](n48_cm_token &b){ b.info = &block[4]; } },
        { "a different IB VA",            [](n48_cm_token &b){ b.va = 0x400005000ull; } },
        { "a different stamp",            [](n48_cm_token &b){ b.stamp = 0x11u; } },
        { "a different length",           [](n48_cm_token &b){ b.n = 1456u; } },
        { "a different IB count",         [](n48_cm_token &b){ b.nib = 2u; } },
        { "a stale sequence",             [](n48_cm_token &b){ b.seq = 6u; } },
        { "no rewrite has happened",      [](n48_cm_token &b){ b.seq = 0u; } },
    };
    for (auto &k : cases) {
        n48_cm_token b = a; k.brk(b);
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "token: %-34s does NOT match", k.what);
        expect_u(lbl, n48_cm_token_match(&a, &b), 0u);
    }
    // 0.0.426 (MIB-COMMIT B7) — T3: THE FRAME IDENTITY SPANS EVERY IB. The rewritten frame's token now names IB 0 AND IB 1;
    // a later submission whose IB 1 differs (its VA or its length) must NOT match, and the OFF token (mib 0) ignores the
    // arrays entirely. Planted breaks: change IB 1's VA, change IB 1's length, clear `mib` (which must REFUSE a mib token
    // against a non-mib one), and change IB 0's entry (which the old five-field match already caught).
    n48_cm_token m {};
    m.info = &block[0]; m.va = 0x400004000ull; m.stamp = 0x10u; m.n = 1040u; m.nib = 2u; m.seq = 9u; m.mib = 1u;
    m.ib_va[0] = 0x400004000ull; m.ib_va[1] = 0x4005a0000ull; m.ib_n[0] = 1040u; m.ib_n[1] = 7616u;
    expect_u("B7 token: a mib token matches itself", n48_cm_token_match(&m, &m), 1u);
    expect_u("B7 token: IB 0's fields mirror va/n", m.ib_va[0] == m.va && m.ib_n[0] == m.n ? 1u : 0u, 1u);
    struct { const char *what; void (*brk)(n48_cm_token &); } mc[] = {
        { "IB 1's VA (planted: IB0 VA used for IB1)", [](n48_cm_token &b){ b.ib_va[1] = b.ib_va[0]; } },
        { "IB 1's length (planted: IB0 len used for IB1)", [](n48_cm_token &b){ b.ib_n[1] = b.ib_n[0]; } },
        { "IB 1's VA by one page",     [](n48_cm_token &b){ b.ib_va[1] ^= 0x1000ull; } },
        { "the mib bit (a mib token vs a non-mib one)", [](n48_cm_token &b){ b.mib = 0u; } },
        { "IB 0's VA",                 [](n48_cm_token &b){ b.ib_va[0] = 0x400006000ull; } },
    };
    for (auto &k : mc) {
        n48_cm_token b = m; k.brk(b);
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "B7 token: %-44s does NOT match", k.what);
        // a mib token must not match a non-mib token in EITHER direction
        expect_u(lbl, n48_cm_token_match(&m, &b) | n48_cm_token_match(&b, &m), 0u);
    }
    const n48_cm_token unset {};
    expect_u("token: two unset tokens do not match", n48_cm_token_match(&unset, &unset), 0u);
    // OFF identity: a non-mib token ignores the (zeroed) arrays and matches its five-field self exactly.
    n48_cm_token off {};
    off.info = &block[0]; off.va = 0x400004000ull; off.stamp = 0x10u; off.n = 1040u; off.nib = 1u; off.seq = 11u;
    off.ib_va[0] = 0xdeadbeefull; off.ib_n[0] = 7u;   // garbage in the arrays must not matter while mib is 0
    expect_u("B7 token: OFF ignores the arrays (byte-identical to 0.0.425)", n48_cm_token_match(&off, &off), 1u);
}

// ---------------------------------------------------------------------------------------------------------------------
// 4. THE HEADLESS KIND (0.0.369, notes 804). The compositor's setup halves (F5/F10, 1456 dwords) have no encoder head at
// all; xlat12_ib_headless_passes returns one segment per render pass with start == head. That shape is TIGHTER than the
// encoder one - the pass's CONTEXT_CONTROL is translated, so no dword of Apple's survives - but it may only be claimed by a
// caller that OBSERVED all three of the recogniser's preconditions. Each one is checked on its own here, and a frame that
// mixes the two shapes refuses whichever kind it claims.
// ---------------------------------------------------------------------------------------------------------------------
static void headless_checks()
{
    n48_cm_frame base;
    good_headless_frame(base, 1456u, 3u);
    uint32_t d = 0;

    // POSITIVE control: a complete headless frame commits. Without it every check below would pass on a gate that refuses
    // every headless frame, which would make the whole change inert and the suite blind to it.
    expect_u("headless: a complete setup half commits", gGate(&base, &d), N48_CM_OK);
    expect_u("headless: and its action is TRANSLATE", action_for(base), N48_SD_ACT_TRANSLATE);

    struct Case { const char *what; uint32_t reason; void (*break_it)(n48_cm_frame &); };
    const Case cases[] = {
        { "start is head + 2 (encoder shape)", N48_CM_SEG_KIND,
          [](n48_cm_frame &c){ c.seg[1].start = c.seg[1].head + 2u; c.seg[1].out_len = c.seg[1].end - c.seg[1].start; } },
        { "a segment starts past its head",    N48_CM_SEG_KIND,
          [](n48_cm_frame &c){ c.seg[2].start = c.seg[2].head + 9u; c.seg[2].out_len = c.seg[2].end - c.seg[2].start; } },
        { "xlat12_ib_segments found 1 too",    N48_CM_SEG_KIND,   [](n48_cm_frame &c){ c.hl_ib_segments = 1u; } },
        { "the recogniser did not say OK",     N48_CM_SEG_KIND,   [](n48_cm_frame &c){ c.hl_ok = 0u; } },
        { "it found MORE passes than we hold", N48_CM_SEG_KIND,   [](n48_cm_frame &c){ c.hl_total = c.nseg + 1u; } },
        { "it found FEWER passes than we hold",N48_CM_SEG_KIND,   [](n48_cm_frame &c){ c.hl_total = c.nseg - 1u; } },
        { "nobody asked the recogniser",       N48_CM_SEG_KIND,   [](n48_cm_frame &c){ c.hl_ok = 0u; c.hl_total = 0u; } },
        // Everything the encoder kind is judged on is judged here too: the kind widens WHERE a body starts and nothing else.
        { "a headless segment refused",        N48_CM_SEG_REFUSED,[](n48_cm_frame &c){ c.seg[1].status = 21u; } },
        { "a gap between headless passes",     N48_CM_COVER,      [](n48_cm_frame &c){ c.seg[1].head += 4u; c.seg[1].start += 4u; } },
        { "a headless translation grew",       N48_CM_SEG_LEN,    [](n48_cm_frame &c){ c.seg[1].out_len++; } },
        { "a headless read-back mismatch",     N48_CM_MISMATCH,   [](n48_cm_frame &c){ c.mismatch = 1u; } },
        { "a headless page was VRAM",          N48_CM_PAGE,       [](n48_cm_frame &c){ c.sys_pages--; } },
        { "a headless frame identity differs", N48_CM_TOKEN,      [](n48_cm_frame &c){ c.token_ok = 0u; } },
    };
    for (const Case &k : cases) {
        n48_cm_frame c = base;
        k.break_it(c);
        char lbl[160];
        std::snprintf(lbl, sizeof(lbl), "headless: %-38s -> %s", k.what, n48_cm_reason_name(k.reason));
        expect_u(lbl, gGate(&c, &d), k.reason);
        std::snprintf(lbl, sizeof(lbl), "headless: %-38s -> NEUTER", k.what);
        expect_u(lbl, action_for(c), N48_SD_ACT_NEUTER);
    }

    // The three headless-only observations are NOT read on an encoder frame: a kind must not be able to smuggle another
    // kind's preconditions into the rung it is judged on.
    n48_cm_frame enc;
    good_frame(enc, 1040u, 3u);
    enc.hl_ib_segments = 3u; enc.hl_ok = 0u; enc.hl_total = 99u;
    expect_u("encoder frame ignores the headless observations", gGate(&enc, &d), N48_CM_OK);

    // And the refusals name themselves in `detail`, so a run can tell the four headless refusals apart.
    n48_cm_frame t = base; t.hl_ib_segments = 2u; d = 0; (void)gGate(&t, &d);
    expect_u("detail: xlat12_ib_segments non-zero", d, 0x00010002u);
    t = base; t.hl_ok = 0u; d = 0; (void)gGate(&t, &d);
    expect_u("detail: the recogniser did not say OK", d, 0x00020000u);
    t = base; t.hl_total = 5u; d = 0; (void)gGate(&t, &d);
    expect_u("detail: passes found is not nseg", d, 0x00030005u);
    t = base; t.seg[2].start = t.seg[2].head + 2u; t.seg[2].out_len = t.seg[2].end - t.seg[2].start; d = 0; (void)gGate(&t, &d);
    expect_u("detail: segment index and its start", d, 0x80020000u | (base.seg[2].head + 2u));
    t = base; t.seg_kind = N48_CM_KIND_UNSET; d = 0; (void)gGate(&t, &d);
    expect_u("detail: no kind was declared", d, 0u);
}

/* 0.0.370 (notes 807) - THE KIND'S NAME. Three values, three distinct names, and UNSET is NEVER spelled ENCODER: the log has
 * to keep "nobody chose" apart from "encoder" for the same reason the gate does. Anything outside the enum reads UNSET, which
 * is the fail-closed direction for a label. */
static void expect_s(const char *what, const char *got, const char *want)
{
    gRun++;
    if (!got || !want || std::strcmp(got, want) != 0) {
        gFail++;
        if (!gQuiet) std::printf("FAIL  %-88s got \"%s\" want \"%s\"\n", what, got ? got : "(null)", want);
    } else if (!gQuiet) {
        std::printf("ok    %-88s \"%s\"\n", what, got);
    }
}

static void kind_name_checks()
{
    expect_s("kind name: ENCODER", gKindName(N48_CM_KIND_ENCODER), "ENCODER");
    expect_s("kind name: HEADLESS", gKindName(N48_CM_KIND_HEADLESS), "HEADLESS");
    expect_s("kind name: UNSET (0 is never read as ENCODER)", gKindName(N48_CM_KIND_UNSET), "UNSET");
    expect_s("kind name: an unknown kind reads UNSET", gKindName(77u), "UNSET");
    expect_s("kind name: 0xffffffff reads UNSET", gKindName(0xffffffffu), "UNSET");
    /* The three names must be pairwise distinct - a label that collapses two kinds answers the hp9 question wrongly while
     * looking like it answered it. */
    expect_u("kind names are distinct: ENCODER != HEADLESS",
             std::strcmp(gKindName(N48_CM_KIND_ENCODER), gKindName(N48_CM_KIND_HEADLESS)) != 0, 1);
    expect_u("kind names are distinct: ENCODER != UNSET",
             std::strcmp(gKindName(N48_CM_KIND_ENCODER), gKindName(N48_CM_KIND_UNSET)) != 0, 1);
    expect_u("kind names are distinct: HEADLESS != UNSET",
             std::strcmp(gKindName(N48_CM_KIND_HEADLESS), gKindName(N48_CM_KIND_UNSET)) != 0, 1);
    /* And the name must agree with what the GATE does with that same value, over a frame BUILT FOR IT: exactly the two kinds
     * the gate can admit are the two with a name of their own, and every other value both refuses and reads UNSET. Each kind
     * is given its own correctly-shaped frame, because a HEADLESS label on an encoder-shaped frame refuses for the SHAPE and
     * would make this check pass for the wrong reason. */
    for (uint32_t k = 0; k < 4u; k++) {
        n48_cm_frame c;
        if (k == N48_CM_KIND_HEADLESS) good_headless_frame(c, 1456u, 3u);
        else { good_frame(c, 1040u, 3u); c.seg_kind = k; }
        uint32_t d = 0;
        const uint32_t admitted = (gGate(&c, &d) != N48_CM_SEG_KIND) ? 1u : 0u;
        const uint32_t named = (std::strcmp(gKindName(k), "UNSET") != 0) ? 1u : 0u;
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "kind %u: named iff the gate admits it", k);
        expect_u(lbl, named, admitted);
    }
}

/* 0.0.370 (notes 807) - THE REHEARSAL LINE FITS UNDER n48log's CAP. 778 lost an X9 verdict to the 512-byte cut, and the X9
 * verdict plus `ARM IS` sit at the END of this line, which is what truncation eats first. Adding `seg_kind` took the worst
 * case TO the cap; this check is why that was noticed and why it stays noticed. Every numeric field is given its widest
 * value and every string field its longest. */
static void rehearsal_line_bound()
{
    char b[4096];
    const char *longestReason = "";
    for (uint32_t r = 0; r < N48_CM_REASONS; r++)
        if (std::strlen(n48_cm_reason_name(r)) > std::strlen(longestReason)) longestReason = n48_cm_reason_name(r);
    /* gfx_dep.h is not included here, so its longest reason name is written out: "sdma-untranslated" (17), the longest of
     * n48_dep_reason_name's 18 strings. A filler of 24 is used instead, so a longer one added later still fits. */
    const char *longestDep = "012345678901234567890123";
    const char *longestKind = n48_cm_kind_name(N48_CM_KIND_HEADLESS);   /* longer than ENCODER and UNSET */
    const int n = std::snprintf(b, sizeof(b), N48_CM_REHEARSAL_FMT, 0xffffffffffffffffull, 0xffffffffu, 0xffffffffu,
                                longestKind, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
                                longestReason, 0xffffffffu, longestDep, 0xffffffffffffffffull, 0xffffffffu, 0xffffffffu);
    if (!gQuiet) std::printf("      gfx-commit REHEARSAL line worst case: %d bytes (cap %u)\n", n, N48_LOG_CAP_BODY);
    expect_u("the REHEARSAL line fits under the log cap", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1);
    /* And it must still SAY seg_kind - a line shortened until it fits by dropping the new field would pass the check above. */
    expect_u("the REHEARSAL line still carries seg_kind", std::strstr(b, "seg_kind ") != nullptr, 1);
    expect_u("... naming this frame's kind", std::strstr(b, longestKind) != nullptr, 1);
    /* 0.0.391 ( condition 6,) — THE SPEND LINE, REWORDED AND THEREFORE MEASURED. Its old ending
     * ("NO OTHER FRAME can be handed COMMIT from this moment") is true at budget 1 and false above it; reading the
     * budget made the line longer, and the first draft measured 508 bytes - 17 OVER the cap - which is exactly how
     *'s lost X9 verdict and's 511-byte truncation happened. Both endings are measured, not one. */
    for (int k = 0; k < 2; k++) {
        const char *tail = k ? N48_CM_SPENT_MORE : N48_CM_SPENT_DONE;
        const int m = std::snprintf(b, sizeof(b), N48_CM_SPENT_FMT, longestReason, 0xffffffffu,
                                    0xffffffffffffffffull, 0xffffffffu, 0xffffffffu, 0xffffffffu, longestKind,
                                    0xffffffffu, 0xffffffffu, 0xffffffffu, tail);
        if (!gQuiet) std::printf("      gfx-commit ONE-SHOT SPENT line (%s ending) worst case: %d bytes (cap %u)\n",
                                 k ? "budget left" : "exhausted", m, N48_LOG_CAP_BODY);
        expect_u("the ONE-SHOT SPENT line fits under the log cap, at BOTH endings",
                 m > 0 && (unsigned)m <= N48_LOG_CAP_BODY, 1);
        /* A line shortened until it fits by dropping the budget would pass the bound and lie again. */
        expect_u("... and still carries the budget it read", std::strstr(b, "BUDGET: this arm ") != nullptr, 1);
    }
    /* The two endings must be DIFFERENT sentences, or the line says the same thing in both worlds - which is the defect. */
    expect_u("the two endings are not the same sentence", std::strcmp(N48_CM_SPENT_MORE, N48_CM_SPENT_DONE) != 0, 1);
    expect_u("and only the budget-left one warns that a further frame may commit",
             (std::strstr(N48_CM_SPENT_MORE, "MAY STILL BE HANDED COMMIT") != nullptr &&
              std::strstr(N48_CM_SPENT_DONE, "MAY STILL BE HANDED COMMIT") == nullptr) ? 1u : 0u, 1);

    /* C5 part 2 (an internal design note Q3, build 0.0.460) — THE THREE CONTINUOUS-MODE LINES, same rule. */
    {
        const int n2 = std::snprintf(b, sizeof(b), N48_CM_CONT_START_FMT, 0xffffffffu, 0xffffffffffffffffull,
                                     0xffffffffffffffffull, 0xffffffffu, 0xffffffffu);
        if (!gQuiet) std::printf("      gfx-commit CONTINUOUS START line worst case: %d bytes (cap %u)\n", n2, N48_LOG_CAP_BODY);
        expect_u("the CONTINUOUS START line fits under the log cap", n2 > 0 && (unsigned)n2 <= N48_LOG_CAP_BODY, 1);
    }
    {
        const char *longestStopName = "";
        for (uint32_t w = 0; w < N48_CM_STOP_REASONS; w++)
            if (std::strlen(n48_cm_stop_name(w)) > std::strlen(longestStopName)) longestStopName = n48_cm_stop_name(w);
        const int n3 = std::snprintf(b, sizeof(b), N48_CM_CONT_STOP_FMT, longestStopName, 0xffffffffu, 0xffffffffu,
                                     0xffffffffu, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull);
        if (!gQuiet) std::printf("      gfx-commit CONTINUOUS STOP line worst case: %d bytes (cap %u)\n", n3, N48_LOG_CAP_BODY);
        expect_u("the CONTINUOUS STOP line fits under the log cap", n3 > 0 && (unsigned)n3 <= N48_LOG_CAP_BODY, 1);
        expect_u("... naming the longest stop reason", std::strstr(b, longestStopName) != nullptr, 1);
    }
    {
        const int n4 = std::snprintf(b, sizeof(b), N48_CM_CONT_SUMMARY_FMT, 0xffffffffu,
                                     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
                                     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
                                     0xffffffffffffffffull);
        if (!gQuiet) std::printf("      gfx-commit CONTINUOUS SUMMARY line 1 worst case: %d bytes (cap %u)\n", n4, N48_LOG_CAP_BODY);
        expect_u("the CONTINUOUS SUMMARY line 1 fits under the log cap", n4 > 0 && (unsigned)n4 <= N48_LOG_CAP_BODY, 1);
    }
    {
        // build 0.0.473 item 2: one more field (the refusal lines the per-arm cap left out).
        const int n5 = std::snprintf(b, sizeof(b), N48_CM_CONT_SUMMARY2_FMT,
                                     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
                                     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffu,
                                     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
                                     0xffffffffffffffffull);
        if (!gQuiet) std::printf("      gfx-commit CONTINUOUS SUMMARY line 2 worst case: %d bytes (cap %u)\n", n5, N48_LOG_CAP_BODY);
        expect_u("the CONTINUOUS SUMMARY line 2 fits under the log cap", n5 > 0 && (unsigned)n5 <= N48_LOG_CAP_BODY, 1);
        expect_u("... and names the refusal lines the cap left out", std::strstr(b, "refusal lines") != nullptr, 1);
    }
    /* build 0.0.473 item 1 — THE SWITCH-41 LINE, now carrying T's unit and T in seconds: worst case every field
     * at its widest (10-digit N/T, the longer unit, a 20-digit second count, the longest status/request strings). */
    {
        const int n6 = std::snprintf(b, sizeof(b), N48_CM_SW41_FMT, 0xffffffffu, 0xffffffffu, "100",
                                     0xffffffffffffffffull, 999ull, 0xffffffffffffffffull,
                                     " *** REFUSED: N or T is out of range (0, or past its ceiling), UNCHANGED ***",
                                     " - a continuous arm is NOT requested", 0xffffffffu, 0xffffffffu,
                                     0xffffffffffffffffull, 0xffffffffu);
        if (!gQuiet) std::printf("      gfx-commit SWITCH 41 line worst case: %d bytes (cap %u)\n", n6, N48_LOG_CAP_BODY);
        expect_u("473: the SWITCH 41 line fits under the log cap", n6 > 0 && (unsigned)n6 <= N48_LOG_CAP_BODY, 1);
        const int n7 = std::snprintf(b, sizeof(b), N48_CM_SW41_FMT, 600u, 600u, "100", 60ull, 0ull, 60000000ull,
                                     " - CHANGED BY THIS VERB", "", N48_CM_CONT_N_MAX, N48_CM_CONT_T_MAX,
                                     (unsigned long long)(N48_CM_CONT_T_MAX_US / 100000ull), N48_CM_CONT_T_UNIT_BIT);
        expect_u("473: ... and the 10b request reads 'T 600 x 100 ms = 60.000 s'",
                 (n7 > 0 && std::strstr(b, "T 600 x 100 ms = 60.000 s (60000000 us)") != nullptr) ? 1u : 0u, 1u);
        expect_u("473: ... naming bit 28 as the unit bit", std::strstr(b, "with bit 28 set") != nullptr, 1);
    }
    /* build 0.0.473 item 4 — tvscan457 and would457 with EVERY counter at its maximum (UINT64_MAX, 20 digits),
     * the longest `how` the verb passes, and the longer on/off names. 0.0.472's tvscan457 wording measured 588 here. */
    {
        const uint64_t M = 0xffffffffffffffffull;
        const char *how54 = "`gfxneuter 54` REFUSED - a continuous arm stands";   /* the longest of the verb's three */
        const int n8 = std::snprintf(b, sizeof(b), N48_TVSCAN457_FMT, "OFF (default)", how54,
                                     (unsigned long long)M, (unsigned long long)M, (unsigned long long)M,
                                     (unsigned long long)M, (unsigned long long)M, (unsigned long long)M,
                                     (unsigned long long)M, (unsigned long long)M, "OFF", (unsigned long long)M,
                                     (unsigned long long)M);
        if (!gQuiet) std::printf("      tvscan457 line worst case: %d bytes (cap %u)\n", n8, N48_LOG_CAP_BODY);
        expect_u("473: the tvscan457 line fits under the log cap with every counter at 20 digits",
                 n8 > 0 && (unsigned)n8 <= N48_LOG_CAP_BODY, 1);
        /* EVERY NUMBER KEPT: 10 counters, each rendered in full. */
        uint32_t k20 = 0u; for (const char *q = b; (q = std::strstr(q, "18446744073709551615")) != nullptr; q += 20) k20++;
        expect_u("473: ... and all ten tvscan457 counters are on it", k20, 10u);
        const int n9 = std::snprintf(b, sizeof(b), N48_CM_WOULD457_FMT,
                                     (unsigned long long)M, (unsigned long long)M, (unsigned long long)M,
                                     (unsigned long long)M, (unsigned long long)M, (unsigned long long)M,
                                     (unsigned long long)M, (unsigned long long)M, (unsigned long long)M,
                                     (unsigned long long)M, (unsigned long long)M,
                                     (unsigned long long)M,    /* build 0.0.487: CS-ELIDE-R1 */
                                     (unsigned long long)M,    /* build 0.0.495: HEAP-GEN */
                                     (unsigned long long)M);   /* build 0.0.500: DE-R1 */
        if (!gQuiet) std::printf("      would457 line worst case: %d bytes (cap %u)\n", n9, N48_LOG_CAP_BODY);
        expect_u("473: the would457 line fits under the log cap with every counter at 20 digits",
                 n9 > 0 && (unsigned)n9 <= N48_LOG_CAP_BODY, 1);
        k20 = 0u; for (const char *q = b; (q = std::strstr(q, "18446744073709551615")) != nullptr; q += 20) k20++;
        expect_u("473: ... all fourteen would457 counters on it (0.0.472's ten + CONTINUOUS-NO-FENCE + 0.0.487's CS-ELIDE-R1 + 0.0.495's HEAP-GEN + 0.0.500's DE-R1)", k20, 14u);
        /* One per rehearsal answer but NOT-ARMED (unreachable in a rehearsal: its probe forces arm = COMMIT, and
         * 0.0.472 never printed it either), plus the total. */
        expect_u("473: ... one per reachable rehearsal answer (N48_CM_RH_REASONS - NOT-ARMED) plus the total", k20,
                 (uint32_t)N48_CM_RH_REASONS - 1u + 1u);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 6. 0.0.371 (notes 809) — THE ARM GATE AND THE ONE-SHOT. 808 found a PHANTOM LEVER: `accel gfxneuter 4` refused, `gXdArm`
//    had exactly two assignments in the tree (DECIDE and OFF), and N48_CM_OK was therefore unreachable for every frame
//    however clean the world — while the project's prose had promoted the planned lever to an existing one. The lever is
//    built now, and it is built HERE, as data, so that "the arm refuses unless X" is a host-tested property rather than an
//    `if` in a 19,000-line file that is reviewed once.
// ---------------------------------------------------------------------------------------------------------------------
typedef uint32_t (*ArmFn)(const n48_cm_arm_req *);
typedef uint32_t (*LevelFn)(const n48_cm_shot *, uint32_t, const n48_cm_ws_mark *);
typedef uint32_t (*FinishFn)(n48_cm_shot *);
typedef uint32_t (*DisarmFn)(n48_cm_shot *);
typedef uint32_t (*SpendFn)(n48_cm_shot *, uint32_t);       /* 0.0.381 (notes 853): the budget's own mutation point */
static ArmFn    gArmMissing = &n48_cm_arm_missing;
static LevelFn  gShotLevel  = &n48_cm_shot_level;
static FinishFn gShotFinish = &n48_cm_shot_finish;
static DisarmFn gShotDisarm = &n48_cm_shot_disarm;
static SpendFn  gShotSpend  = &n48_cm_shot_spend;

/* Everything the 802 checklist asks for, all present: the one request that may arm. */
static void good_arm_req(n48_cm_arm_req &a)
{
    a = n48_cm_arm_req {};
    a.decide_armed = 1u;
    a.buffers_ok = 1u;
    a.sampled = 1u;
    a.observers = 0x7Fu;            /* N48_DEP_OBS_REQUIRED, as gfx_dep.h defines it */
    a.observers_required = 0x7Fu;
    a.early_bit = 0x10u;            /* N48_DEP_OBS_EARLY */
    a.e1_pass = 1u;
    a.n1_on = 1u;
    a.latch_enforced = 1u;
    a.descport_on = 1u;
    a.resprov_on = 1u;
    a.headless_on = 1u;
}

static void arm_gate_checks()
{
    /* THE POSITIVE CONTROL, FILLED: the complete request arms. Without this every mutant below is caught by a gate that
     * refuses everything, which is not a test of anything (the project's own rule). */
    n48_cm_arm_req a;
    good_arm_req(a);
    expect_u("arm: the complete pre-arm checklist arms", gArmMissing(&a), 0);

    /* Zero-initialised means NOBODY CHECKED, and nobody-checked refuses on every item at once - the same fail-closed
     * construction as n48_cm_frame. */
    const n48_cm_arm_req z {};
    expect_u("arm: a request nobody filled refuses on EVERY item", gArmMissing(&z), N48_CM_ARM_ALL);
    expect_u("arm: a null request refuses on EVERY item", gArmMissing(nullptr), N48_CM_ARM_ALL);

    /* ONE ITEM AT A TIME: clearing exactly one field must set exactly that field's bit and no other. The two observer
     * rungs are asked of the SAMPLED world only, so `sampled` is the one field that sets three bits, deliberately - an
     * unsampled world's observer mask is not evidence of anything and must not read as "one thing to fix". */
    struct One { const char *what; uint32_t bit; uint32_t n48_cm_arm_req::*field; };
    const One one[] = {
        { "the DECISION is not armed",     N48_CM_ARM_DECIDE_FIRST, &n48_cm_arm_req::decide_armed },
        { "the buffers are not allocated", N48_CM_ARM_BUFFERS,      &n48_cm_arm_req::buffers_ok },
        { "RULE E1 did not PASS",          N48_CM_ARM_E1,           &n48_cm_arm_req::e1_pass },
        { "N1 is OFF",                     N48_CM_ARM_N1,           &n48_cm_arm_req::n1_on },
        { "the latch is not ENFORCED",     N48_CM_ARM_LATCH,        &n48_cm_arm_req::latch_enforced },
        { "the descriptor port is OFF",    N48_CM_ARM_DESCPORT,     &n48_cm_arm_req::descport_on },
        { "resprov is OFF",                N48_CM_ARM_RESPROV,      &n48_cm_arm_req::resprov_on },
        { "the headless recogniser is OFF",N48_CM_ARM_HEADLESS,     &n48_cm_arm_req::headless_on },
    };
    for (const One &o : one) {
        n48_cm_arm_req b;
        good_arm_req(b);
        b.*(o.field) = 0u;
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "arm REFUSES, naming exactly it: %s", o.what);
        expect_u(lbl, gArmMissing(&b), o.bit);
        /* and a value that is neither 0 nor 1 is not a yes: these are positive assertions, not truthiness */
        b.*(o.field) = 2u;
        std::snprintf(lbl, sizeof(lbl), "arm REFUSES on a field that is not exactly 1: %s", o.what);
        expect_u(lbl, gArmMissing(&b), o.bit);
    }
    {   /* an observer short of the seven */
        n48_cm_arm_req b; good_arm_req(b); b.observers = 0x7Fu & ~0x20u;   /* C13 (engine queues) gone */
        expect_u("arm REFUSES with C13's observer missing", gArmMissing(&b), N48_CM_ARM_OBSERVERS);
        b.observers = 0x7Fu & ~0x10u;                                      /* EARLY gone: BOTH rungs name it */
        expect_u("arm REFUSES with EARLY missing, on both its rungs", gArmMissing(&b),
                 N48_CM_ARM_OBSERVERS | N48_CM_ARM_EARLY);
        b.observers = 0u;
        expect_u("arm REFUSES with no observer at all", gArmMissing(&b), N48_CM_ARM_OBSERVERS | N48_CM_ARM_EARLY);
    }
    {   /* an unsampled world: the mask is not evidence, so all three refuse together */
        n48_cm_arm_req b; good_arm_req(b); b.sampled = 0u;
        expect_u("arm REFUSES on an unsampled world, and does not believe its observer mask",
                 gArmMissing(&b), N48_CM_ARM_SAMPLED | N48_CM_ARM_OBSERVERS | N48_CM_ARM_EARLY);
    }
    {   /* a caller that forgot to say what `required` and `early` ARE cannot get an arm by passing 0 */
        n48_cm_arm_req b; good_arm_req(b); b.observers_required = 0u;
        expect_u("arm REFUSES when nobody said what the observers must be", gArmMissing(&b), N48_CM_ARM_OBSERVERS);
        good_arm_req(b); b.early_bit = 0u;
        expect_u("arm REFUSES when nobody said which bit EARLY is", gArmMissing(&b), N48_CM_ARM_EARLY);
    }
    /* Every item has a name of its own, and no item is unnamed. */
    for (uint32_t bit = 1u; bit <= N48_CM_ARM_ALL; bit <<= 1) {
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "arm item %#x has a name", bit);
        expect_u(lbl, std::strcmp(n48_cm_arm_item_name(bit), "?") != 0, 1);
    }
    {
        int named = 0;
        for (uint32_t bit = 1u; bit <= N48_CM_ARM_ALL; bit <<= 1) named++;
        expect_u("arm: the mask holds exactly N48_CM_ARM_ITEMS items", (uint32_t)named, N48_CM_ARM_ITEMS);
    }
    /* THE DROPPED-WRITE COUNTS ARE NOT AN ARMING CONDITION, and that is deliberate (806: `source-neuter` cannot be 0 in
     * judge-only mode, because at DECIDE every frame is neutered and each neutered submission IS a dropped write). What
     * holds instead is that the FRAME still refuses: an armed boot with a dirty world commits nothing. */
    {
        n48_cm_frame c; good_frame(c, 1040u, 3u);
        c.dep_ok = 0u;                                   /* X9's world is dirty, exactly as it is at DECIDE */
        uint32_t d = 0;
        expect_u("ARMED + a dirty X9 world: the FRAME still refuses at DEPENDENCY-STALE", gGate(&c, &d),
                 N48_CM_DEP_STALE);
        expect_u("... and its action is NEUTER", action_for(c), N48_SD_ACT_NEUTER);
    }
    /* And the converse, which is the whole point of the arm: a frame that clears every rung commits ONLY at COMMIT. */
    for (uint32_t arm = 0; arm < 3u; arm++) {
        n48_cm_frame c; good_frame(c, 1040u, 3u);
        c.arm = arm;
        uint32_t d = 0;
        char lbl[112];
        std::snprintf(lbl, sizeof(lbl), "a frame that clears every rung at arm %u", arm);
        expect_u(lbl, gGate(&c, &d), arm == N48_SD_ARM_COMMIT ? (uint32_t)N48_CM_OK : (uint32_t)N48_CM_NOT_ARMED);
        std::snprintf(lbl, sizeof(lbl), "... its action at arm %u", arm);
        expect_u(lbl, action_for(c), arm == N48_SD_ARM_COMMIT ? (uint32_t)N48_SD_ACT_TRANSLATE
                                                              : (uint32_t)N48_SD_ACT_NEUTER);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 7. THE ONE-SHOT: the blast radius is exactly one frame, and the arm does not survive WindowServer's binding moving.
// ---------------------------------------------------------------------------------------------------------------------
static const n48_cm_ws_mark kWsPreKill { 0, 0, 0, 0 };   /* hp9's own reading before `wskill` (806): nothing is bound yet */
static const n48_cm_ws_mark kWsAfterRestart { 1, 0, 0, 0 };  /* the compositor the committed frame belongs to */

static void one_shot_checks()
{
    n48_cm_shot sh {};
    /* Zero-initialised is OFF, and OFF hands out no COMMIT: a one-shot nobody armed cannot commit anything. */
    expect_u("one-shot: zero-initialised is OFF", sh.state, N48_CM_SHOT_OFF);
    expect_u("one-shot OFF: a COMMIT level is handed out as DECIDE", gShotLevel(&sh, N48_SD_ARM_COMMIT, &kWsPreKill),
             N48_SD_ARM_DECIDE);
    expect_u("one-shot: a null shot hands out DECIDE", gShotLevel(nullptr, N48_SD_ARM_COMMIT, &kWsPreKill),
             N48_SD_ARM_DECIDE);

    /* INERT WHEN NOTHING IS ARMED: for every state and every binding history, an OFF or DECIDE level passes through
     * unchanged. This is the property that makes the whole change invisible on an unarmed boot. */
    {
        uint32_t same = 0, tried = 0;
        const n48_cm_ws_mark marks[] = { { 0,0,0,0 }, { 1,0,0,0 }, { 2,1,1,1 }, { 9,3,4,2 } };
        for (uint32_t st = 0; st < N48_CM_SHOT_STATES; st++)
            for (const n48_cm_ws_mark &m : marks) {
                n48_cm_shot t {}; t.state = st; t.ws = kWsPreKill;
                tried += 2;
                same += (gShotLevel(&t, N48_SD_ARM_OFF, &m) == N48_SD_ARM_OFF) ? 1u : 0u;
                same += (gShotLevel(&t, N48_SD_ARM_DECIDE, &m) == N48_SD_ARM_DECIDE) ? 1u : 0u;
            }
        expect_u("one-shot is INERT at OFF and DECIDE, in every state and every binding history", same, tried);
    }

    /* ARM -> the next frame may be judged at COMMIT. */
    expect_u("one-shot: arming answers 1", n48_cm_shot_arm(&sh, &kWsPreKill, 1234u, 1u), 1);
    expect_u("one-shot: armed", sh.state, N48_CM_SHOT_ARMED);
    expect_u("one-shot ARMED: a frame is handed COMMIT", gShotLevel(&sh, N48_SD_ARM_COMMIT, &kWsPreKill),
             N48_SD_ARM_COMMIT);
    expect_u("one-shot ARMED: DECIDE is still DECIDE", gShotLevel(&sh, N48_SD_ARM_DECIDE, &kWsPreKill),
             N48_SD_ARM_DECIDE);
    expect_u("one-shot ARMED: OFF is still OFF", gShotLevel(&sh, N48_SD_ARM_OFF, &kWsPreKill), N48_SD_ARM_OFF);

    /* THE RECIPE'S OWN SHAPE (806): the arm is set BEFORE `wskill`, when ws_ident reads `state NONE ... binds 0
     * (rebinds 0), unbinds 0`. The compositor that draws the committed frame does not exist yet, so ONE new binding must
     * NOT cancel the arm - a rule that cancelled on any change would cancel on the very bind the run exists to catch. */
    expect_u("one-shot survives the ONE WindowServer bind the run itself causes",
             gShotLevel(&sh, N48_SD_ARM_COMMIT, &kWsAfterRestart), N48_SD_ARM_COMMIT);

    /* ...and nothing else. A REPLACED or LOST binding is a world the arm was not granted against. */
    struct WsCase { const char *what; n48_cm_ws_mark at_arm, now; };
    const WsCase bad[] = {
        { "a REBIND",                        { 1, 0, 0, 0 }, { 1, 1, 0, 0 } },
        { "a REBIND after the run's own bind",{ 0, 0, 0, 0 }, { 1, 1, 0, 0 } },
        { "an UNBIND",                       { 1, 0, 0, 0 }, { 1, 0, 1, 0 } },
        { "the bound root going away",       { 1, 0, 0, 0 }, { 1, 0, 0, 1 } },
        { "a SECOND new binding",            { 0, 0, 0, 0 }, { 2, 0, 0, 0 } },
        { "a bind counter that went DOWN",   { 1, 0, 0, 0 }, { 0, 0, 0, 0 } },
    };
    for (const WsCase &w : bad) {
        n48_cm_shot t {};
        n48_cm_shot_arm(&t, &w.at_arm, 0u, 1u);
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "one-shot does NOT survive %s", w.what);
        expect_u(lbl, gShotLevel(&t, N48_SD_ARM_COMMIT, &w.now), N48_SD_ARM_DECIDE);
        std::snprintf(lbl, sizeof(lbl), "... and ws_ok says so: %s", w.what);
        expect_u(lbl, n48_cm_shot_ws_ok(&t.ws, &w.now), 0);
    }
    /* The one thing that IS allowed, stated as its own check so the boundary is not an accident: exactly one new binding
     * and nothing else. Two is not one. */
    {
        n48_cm_shot t {};
        n48_cm_shot_arm(&t, &kWsPreKill, 0u, 1u);
        const n48_cm_ws_mark oneMore { 1, 0, 0, 0 }, twoMore { 2, 0, 0, 0 };
        expect_u("one-shot: ONE new binding is allowed", n48_cm_shot_ws_ok(&t.ws, &oneMore), 1);
        expect_u("one-shot: TWO new bindings are not", n48_cm_shot_ws_ok(&t.ws, &twoMore), 0);
        expect_u("one-shot: a null mark is not ok", n48_cm_shot_ws_ok(&t.ws, nullptr), 0);
    }

    /* SPEND: the gate answered OK for ONE frame. From that instant no other frame is handed COMMIT - and the arm LEVEL is
     * deliberately still COMMIT, because the ring walk's exemption reads it inside Apple's own call, after the gate. */
    expect_u("one-shot: the spend answers 1", gShotSpend(&sh, 7u), 1);
    expect_u("one-shot: spent", sh.state, N48_CM_SHOT_SPENT);
    expect_u("one-shot: the spend records the token seq the gate answered for", sh.spent_seq, 7);
    expect_u("one-shot SPENT: NO SECOND FRAME is handed COMMIT", gShotLevel(&sh, N48_SD_ARM_COMMIT, &kWsAfterRestart),
             N48_SD_ARM_DECIDE);
    expect_u("one-shot: a second spend answers 0", gShotSpend(&sh, 8u), 0);
    expect_u("one-shot: ... and does not overwrite the first seq", sh.spent_seq, 7);
    expect_u("one-shot SPENT cannot be CANCELLED (its frame is in flight and still needs the level)",
             n48_cm_shot_cancel(&sh), 0);

    /* FINISH: the committed frame's submission returned, so the level goes back to DECIDE. Exactly once. */
    expect_u("one-shot: the finish answers 1", gShotFinish(&sh), 1);
    expect_u("one-shot: done", sh.state, N48_CM_SHOT_DONE);
    expect_u("one-shot: a second finish answers 0 (the disarm is logged once)", gShotFinish(&sh), 0);
    expect_u("one-shot DONE: no frame is handed COMMIT", gShotLevel(&sh, N48_SD_ARM_COMMIT, &kWsAfterRestart),
             N48_SD_ARM_DECIDE);
    expect_u("one-shot: finishing something that was never spent answers 0", []{ n48_cm_shot t {}; return gShotFinish(&t); }(), 0);
    expect_u("one-shot: finishing an ARMED one answers 0 (only a SPENT one finishes)",
             []{ n48_cm_shot t {}; n48_cm_shot_arm(&t, &kWsPreKill, 0u, 1u); return gShotFinish(&t); }(), 0);

    /* CANCEL from ARMED. */
    {
        n48_cm_shot t {};
        n48_cm_shot_arm(&t, &kWsPreKill, 0u, 1u);
        expect_u("one-shot: cancelling an ARMED one answers 1", n48_cm_shot_cancel(&t), 1);
        expect_u("one-shot: cancelled", t.state, N48_CM_SHOT_CANCELLED);
        expect_u("one-shot CANCELLED: no frame is handed COMMIT", gShotLevel(&t, N48_SD_ARM_COMMIT, &kWsPreKill),
                 N48_SD_ARM_DECIDE);
        expect_u("one-shot: a second cancel answers 0", n48_cm_shot_cancel(&t), 0);
    }

    /* DISARM, the verb's own `4 | 2 << 8`. Always allowed, from any state: going back to DECIDE can never need a
     * precondition, exactly as N1's disable never does. */
    {
        n48_cm_shot t {};
        n48_cm_shot_arm(&t, &kWsPreKill, 0u, 1u);
        expect_u("disarm: an ARMED one-shot disarms, and says it changed something", gShotDisarm(&t), 1);
        expect_u("disarm: ... and is OFF", t.state, N48_CM_SHOT_OFF);
        expect_u("disarm: OFF hands out no COMMIT", gShotLevel(&t, N48_SD_ARM_COMMIT, &kWsPreKill), N48_SD_ARM_DECIDE);
        expect_u("disarm: disarming an already-OFF one answers 0", gShotDisarm(&t), 0);
        n48_cm_shot_arm(&t, &kWsPreKill, 0u, 1u);
        gShotSpend(&t, 3u);
        expect_u("disarm: a SPENT one-shot disarms too", gShotDisarm(&t), 1);
        expect_u("disarm: ... and is OFF", t.state, N48_CM_SHOT_OFF);
        expect_u("disarm: ... and its recorded seq is cleared", t.spent_seq, 0);
        expect_u("disarm: OFF hands out no COMMIT after a spend either",
                 gShotLevel(&t, N48_SD_ARM_COMMIT, &kWsAfterRestart), N48_SD_ARM_DECIDE);
    }

    /* THE PROPERTY THE WHOLE THING EXISTS FOR: over a run of frames, AT MOST ONE is ever handed COMMIT. The loop is the
     * kext's own shape - ask the level, and if it is COMMIT and the frame clears the gate, spend - and it is run both for
     * a frame stream that commits immediately and for one where the first thirty frames refuse at the gate. */
    for (uint32_t firstOk = 0; firstOk < 40u; firstOk += 13u) {
        n48_cm_shot t {};
        n48_cm_shot_arm(&t, &kWsPreKill, 0u, 1u);
        uint32_t handed = 0, committed = 0;
        for (uint32_t i = 0; i < 256u; i++) {
            const n48_cm_ws_mark now = i < 4u ? kWsPreKill : kWsAfterRestart;   /* WindowServer restarts at frame 4 */
            if (gShotLevel(&t, N48_SD_ARM_COMMIT, &now) != N48_SD_ARM_COMMIT) continue;
            handed++;
            n48_cm_frame c;
            good_frame(c, 1040u, 3u);
            if (i < firstOk) c.mismatch = 1u;             /* the gate refuses: the one-shot must NOT be spent */
            uint32_t d = 0;
            if (gGate(&c, &d) != N48_CM_OK) continue;
            committed++;
            gShotSpend(&t, i + 1u);
            gShotFinish(&t);                              /* the frame's submission returns */
        }
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "one-shot over 256 frames (first clean at %u): frames handed COMMIT", firstOk);
        expect_u(lbl, handed, firstOk + 1u);
        std::snprintf(lbl, sizeof(lbl), "one-shot over 256 frames (first clean at %u): frames COMMITTED", firstOk);
        expect_u(lbl, committed, 1);
    }

    /* -----------------------------------------------------------------------------------------------------------------
     * 6b. 0.0.381 (notes 853) — THE N-FRAME BUDGET. Everything above is re-run at budget 1 by construction (every arm in
     *     this file passes 1u), which IS the proof that the default is 0.0.380: 257 checks and 19 mutants unchanged.
     *     What is checked here is the budget itself, and the two properties a reviewer must be able to see fail.
     * -------------------------------------------------------------------------------------------------------------- */
    {
        /* THE DEFAULT. `budget` 0 - a caller that forgot the field - IS one, on every path. */
        n48_cm_shot t {};
        n48_cm_shot_arm(&t, &kWsPreKill, 0u, 0u);
        expect_u("budget: arming with 0 stores 1 (a caller that forgot the field gets the one-shot)", t.budget, 1);
        expect_u("budget: 0 in the struct reads as 1", []{ n48_cm_shot z {}; return n48_cm_shot_budget_of(&z); }(), 1);
        expect_u("budget: a budget ABOVE THE CEILING is refused at the arm and stored as 1",
                 []{ n48_cm_shot z {}; n48_cm_shot_arm(&z, &kWsPreKill, 0u, N48_CM_SHOT_BUDGET_MAX + 1u);
                     return z.budget; }(), 1);
        expect_u("budget: a struct carrying an out-of-range budget still reads as 1",
                 []{ n48_cm_shot z {}; z.budget = 250u; return n48_cm_shot_budget_of(&z); }(), 1);
        expect_u("budget: at 1, ONE spend exhausts it (this is 0.0.380 exactly)", gShotSpend(&t, 7u), 1);
        expect_u("budget: ... and the state moved on that same call", t.state, N48_CM_SHOT_SPENT);
        expect_u("budget: ... and the exhausting seq is recorded", t.spent_seq, 7);
    }
    /* A ZERO-INITIALISED SHOT IS A ONE-SHOT, at the SPEND and not only at the arm. Every caller before 0.0.381 filled no
     * budget field at all, so a `budget` of 0 reaching the spend must behave as 1 there too - reading it as "unlimited"
     * is a silent arm that never puts itself away, and the arm is checked at the arm, not here. */
    {
        n48_cm_shot t {};
        t.state = N48_CM_SHOT_ARMED;
        t.ws = kWsPreKill;                       /* budget deliberately left 0: the 0.0.380 struct, byte for byte */
        expect_u("budget: a shot carrying budget 0 spends once", gShotSpend(&t, 5u), 1);
        expect_u("budget: ... and is SPENT on that call (0 is NOT unlimited)", t.state, N48_CM_SHOT_SPENT);
        expect_u("budget: ... and hands out no further COMMIT",
                 gShotLevel(&t, N48_SD_ARM_COMMIT, &kWsPreKill), N48_SD_ARM_DECIDE);
        expect_u("budget: ... and a second spend answers 0", gShotSpend(&t, 6u), 0);
    }
    /* IDEMPOTENT PER TOKEN SEQ, NOT PER CALL. This is the defect the field exists for: above budget 1 the state is still
     * ARMED after a spend, so a second call for the SAME frame would charge the budget twice and halve it silently. */
    for (uint32_t b = 1u; b <= N48_CM_SHOT_BUDGET_MAX; b++) {
        n48_cm_shot t {};
        n48_cm_shot_arm(&t, &kWsPreKill, 0u, b);
        char lbl[160];
        std::snprintf(lbl, sizeof(lbl), "budget %u: the first spend is charged", b);
        expect_u(lbl, gShotSpend(&t, 11u), 1);
        std::snprintf(lbl, sizeof(lbl), "budget %u: the SAME token seq again answers 0", b);
        expect_u(lbl, gShotSpend(&t, 11u), 0);
        std::snprintf(lbl, sizeof(lbl), "budget %u: ... and did NOT charge the budget twice", b);
        expect_u(lbl, t.spent, 1);
        std::snprintf(lbl, sizeof(lbl), "budget %u: a THIRD call for that seq still answers 0", b);
        expect_u(lbl, gShotSpend(&t, 11u), 0);
        std::snprintf(lbl, sizeof(lbl), "budget %u: ... and the budget is still charged exactly once", b);
        expect_u(lbl, t.spent, 1);
    }
    /* TOKEN SEQ 0 IS A REAL SEQ, not the initial value of the dedupe key. */
    {
        n48_cm_shot t {};
        n48_cm_shot_arm(&t, &kWsPreKill, 0u, 3u);
        expect_u("budget: token seq 0 is charged normally (not mistaken for 'nothing spent yet')", gShotSpend(&t, 0u), 1);
        expect_u("budget: ... and is then deduped like any other", gShotSpend(&t, 0u), 0);
        expect_u("budget: ... charged exactly once", t.spent, 1);
    }
    /* THE BOUND HOLDS: exactly `budget` frames are ever handed COMMIT, never one more, at every legal budget, and the
     * state moves to SPENT on the frame that exhausts it and not before. */
    for (uint32_t b = 1u; b <= N48_CM_SHOT_BUDGET_MAX; b++) {
        n48_cm_shot t {};
        n48_cm_shot_arm(&t, &kWsPreKill, 0u, b);
        uint32_t handed = 0, charged = 0, armedWhileUnspent = 1u;
        for (uint32_t i = 0; i < 256u; i++) {
            const n48_cm_ws_mark now = i < 4u ? kWsPreKill : kWsAfterRestart;
            if (gShotLevel(&t, N48_SD_ARM_COMMIT, &now) != N48_SD_ARM_COMMIT) continue;
            handed++;
            n48_cm_frame c;
            good_frame(c, 1040u, 3u);
            uint32_t d = 0;
            if (gGate(&c, &d) != N48_CM_OK) continue;
            charged += gShotSpend(&t, i + 1u);
            if (t.spent < b && t.state != N48_CM_SHOT_ARMED) armedWhileUnspent = 0u;
            gShotFinish(&t);   /* answers 0 while the budget stands, so the arm level is NOT put back */
        }
        char lbl[160];
        std::snprintf(lbl, sizeof(lbl), "budget %u: frames handed COMMIT over 256 frames", b);
        expect_u(lbl, handed, b);
        std::snprintf(lbl, sizeof(lbl), "budget %u: frames charged", b);
        expect_u(lbl, charged, b);
        std::snprintf(lbl, sizeof(lbl), "budget %u: the shot stayed ARMED while the budget stood", b);
        expect_u(lbl, armedWhileUnspent, 1);
        std::snprintf(lbl, sizeof(lbl), "budget %u: the shot is DONE at the end", b);
        expect_u(lbl, t.state, N48_CM_SHOT_DONE);
        std::snprintf(lbl, sizeof(lbl), "budget %u: nothing is left", b);
        expect_u(lbl, n48_cm_shot_left(&t), 0);
    }
    /* THE BUDGET IS FROZEN AT THE ARM and a disarm takes it with it: it cannot be left standing for an arm that did not
     * ask for one, and no path raises it under a live arm. */
    {
        n48_cm_shot t {};
        n48_cm_shot_arm(&t, &kWsPreKill, 0u, 3u);
        expect_u("budget: 3 left before any spend", n48_cm_shot_left(&t), 3);
        gShotSpend(&t, 1u);
        expect_u("budget: 2 left after one spend", n48_cm_shot_left(&t), 2);
        gShotDisarm(&t);
        expect_u("budget: a disarm clears the budget", t.budget, 0);
        expect_u("budget: a disarm clears what was spent", t.spent, 0);
        expect_u("budget: a disarm clears the dedupe key", t.last_seq, 0);
        expect_u("budget: nothing is left once disarmed", n48_cm_shot_left(&t), 0);
        expect_u("budget: a DISARMED shot is handed no COMMIT",
                 gShotLevel(&t, N48_SD_ARM_COMMIT, &kWsPreKill), N48_SD_ARM_DECIDE);
    }
    /* `left` is only ever about a LIVE arm. */
    for (uint32_t s = 0; s < N48_CM_SHOT_STATES; s++) {
        if (s == N48_CM_SHOT_ARMED) continue;
        n48_cm_shot t {}; t.state = s; t.budget = 4u; t.spent = 0u;
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "budget: nothing is left in state %u", s);
        expect_u(lbl, n48_cm_shot_left(&t), 0);
    }
    /* A CANCELLED arm spends nothing, at every budget: the binding moved, so no frame of it may be charged. */
    for (uint32_t b = 1u; b <= N48_CM_SHOT_BUDGET_MAX; b++) {
        n48_cm_shot t {};
        n48_cm_shot_arm(&t, &kWsPreKill, 0u, b);
        n48_cm_shot_cancel(&t);
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "budget %u: a CANCELLED arm cannot be spent", b);
        expect_u(lbl, gShotSpend(&t, 1u), 0);
    }

    /* Every state has a name of its own. */
    for (uint32_t s = 0; s < N48_CM_SHOT_STATES; s++) {
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "one-shot state %u has a name", s);
        expect_u(lbl, std::strcmp(n48_cm_shot_name(s), "?") != 0, 1);
    }
    expect_u("one-shot: an unknown state reads ?", std::strcmp(n48_cm_shot_name(99u), "?") == 0, 1);
}

/* 0.0.401 (a item 3,b) — THE IDENTITY LUT'S APPENDED RUNG. The rung is present ONLY while switch 32 is on;
 * with it off the gate must be 0.0.400's exactly, and the fill (a frame that does not sample the LUT) must be unaffected
 * even with the switch on. Each check below is the falsifier of one way to write the rung wrong. */
static void lut_checks()
{
    /* The reason is APPENDED: 0..17 are 0.0.400's and may never move. Pin the index and the neighbours. */
    expect_u("lut: the reason is APPENDED at index 18", N48_CM_LUT_NOT_READY, 18u);
    expect_u("lut: the reason table is long enough to hold it", N48_CM_REASONS > N48_CM_LUT_NOT_READY, 1u);
    expect_u("lut: DEPENDENCY-STALE did not move", N48_CM_DEP_STALE, 16u);
    expect_u("lut: SEGMENT-KIND did not move", N48_CM_SEG_KIND, 17u);
    expect_s("lut: the reason has its own name", n48_cm_reason_name(N48_CM_LUT_NOT_READY), "LUT-NOT-READY");

    n48_cm_frame c; uint32_t d = 0;
    /* 1. THE FILL, AND EVERY NON-PLANE FRAME, COMMITS with the switch on and the LUT not ready. This is the clause that
     *    keeps the one-shot's first commit alive; the rung must not turn into "refuse everything". */
    good_frame(c, 1040u, 3u);
    c.lut_switch = 1u; c.lut_plane = 0u; c.lut_ready = 0u;
    expect_u("lut: switch ON, a non-plane frame (the fill) still commits", gGate(&c, &d), N48_CM_OK);
    /* 2. A PLANE CANDIDATE IS REFUSED UNTIL THE IDENTITY WRITE HAS LANDED. */
    good_frame(c, 1040u, 3u);
    c.lut_switch = 1u; c.lut_plane = 1u; c.lut_ready = 0u; d = 0;
    expect_u("lut: switch ON, a plane candidate is refused", gGate(&c, &d), N48_CM_LUT_NOT_READY);
    expect_u("lut: the refusal's detail is 1", d, 1u);
    /* 3. ONCE READY, THE SAME CANDIDATE COMMITS. */
    c.lut_ready = 1u;
    expect_u("lut: switch ON, the plane candidate commits once ready", gGate(&c, &d), N48_CM_OK);
    /* 4. WITH THE SWITCH OFF THE RUNG IS ABSENT: every flag combination is accepted. This is the byte-identity clause —
     *    a boot that never threw 32 must judge frames exactly as 0.0.400 did. */
    good_frame(c, 1040u, 3u);
    c.lut_switch = 0u; c.lut_plane = 1u; c.lut_ready = 0u;
    expect_u("lut: switch OFF, a plane candidate commits (the rung is absent)", gGate(&c, &d), N48_CM_OK);
    c.lut_switch = 0u; c.lut_plane = 0u; c.lut_ready = 0u;
    expect_u("lut: switch OFF, a normal frame commits", gGate(&c, &d), N48_CM_OK);
    /* 5. THE RUNG IS LAST. A frame an EARLIER rung refuses is refused THERE, not as LUT-NOT-READY: the appended rung must
     *    never hide the rung above it. */
    good_frame(c, 1040u, 3u);
    c.verdict = N48_XV_SHAPE; c.lut_switch = 1u; c.lut_plane = 1u; c.lut_ready = 0u; d = 0;
    expect_u("lut: NOT-TRANSLATE still answers before LUT-NOT-READY", gGate(&c, &d), N48_CM_NOT_TRANSLATE);
    good_frame(c, 1040u, 3u);
    c.dep_ok = 0u; c.lut_switch = 1u; c.lut_plane = 1u; c.lut_ready = 0u;
    expect_u("lut: DEPENDENCY-STALE still answers before LUT-NOT-READY", gGate(&c, &d), N48_CM_DEP_STALE);
    /* 6. THE DEFAULT (the three fields nobody set, all 0) IS 0.0.400's GATE. The whole suite proves this frame by frame;
     *    this is the one explicit reading of it. */
    good_frame(c, 1040u, 3u);
    expect_u("lut: the all-zero default is the old gate", gGate(&c, &d), N48_CM_OK);
    expect_u("lut: and LUT-NOT-READY keeps its appended index", N48_CM_LUT_NOT_READY, 18u);
}

/* 0.0.404 (a item 2,b) — THE FIRST-SHOT FILL-SET RESERVATION'S APPENDED RUNG. The rung is present ONLY
 * while switch 33 is on AND the reservation window is open; with it off the gate must be 0.0.403's exactly, the reserved
 * fill must pass, and once the window closes (both members committed, or expired) the plane must pass too. Each check is
 * the falsifier of one way to write the rung wrong. */
static void fillset_checks()
{
    /* The reason is APPENDED: 0..18 are 0.0.403's and may never move. Pin the index and the neighbours. */
    expect_u("fillset: the reason is APPENDED at index 19", N48_CM_RESERVED_FOR_FILL, 19u);
    expect_u("fillset: N48_CM_REASONS was 21 at 0.0.419, 22 after P1's append, 23 after R1's (an internal design note), "
             "24 after C5 part 1's RING-FULL (an internal design note), 25 after 0.0.456's MIB-REQUIRED, 26 after C5 part 2's CONTINUOUS-NO-FENCE (merged after 0.0.456), 27 after 0.0.487's COMPUTE-ELIDE-R1, 28 after 0.0.500's DRAW-ELIDE-R1",
             N48_CM_REASONS, 28u);
    expect_u("fillset: LUT-NOT-READY did not move", N48_CM_LUT_NOT_READY, 18u);
    expect_u("fillset: DEPENDENCY-STALE did not move", N48_CM_DEP_STALE, 16u);
    expect_s("fillset: the reason has its own name", n48_cm_reason_name(N48_CM_RESERVED_FOR_FILL), "RESERVED-FOR-FILL");
    /* 0.0.418 (E3): the REGION-MOVED reason is APPENDED after it, at index 20, and named. It is set by the kext's
     * commit-try, never by n48_cm_gate, but it lives in the same enum so the refusal is named and counted. */
    expect_u("E3: FENCE-REGION-MOVED is APPENDED at index 20", N48_CM_FENCE_REGION_MOVED, 20u);
    expect_s("E3: it has its own name", n48_cm_reason_name(N48_CM_FENCE_REGION_MOVED), "FENCE-REGION-MOVED");
    expect_u("P1: RESERVED-FOR-PLANE is APPENDED at index 21", N48_CM_RESERVED_FOR_PLANE, 21u);
    // R1 (an internal design note Q2) — MEMORY-DESTINATION is APPENDED after it, at index 22, and named.
    expect_u("R1: MEMORY-DESTINATION is APPENDED at index 22", N48_CM_MEMDST, 22u);
    expect_s("R1: it has its own name", n48_cm_reason_name(N48_CM_MEMDST), "MEMORY-DESTINATION");
    // R1's own "N48_CM_REASONS is 23 after the append" is superseded by C5's own count check just below (24) - kept
    // as ONE check per append, on the CURRENT total, rather than a stale count that every future append must edit.
    // C5 part 1 (an internal design note Q1, gfx_flightring.h) — RING-FULL is APPENDED after it, at index 23.
    expect_u("C5: RING-FULL is APPENDED at index 23", N48_CM_RING_FULL, 23u);
    expect_s("C5: it has its own name", n48_cm_reason_name(N48_CM_RING_FULL), "RING-FULL");
    // C5's own "N48_CM_REASONS is 24 after the append" and 0.0.456's "25" are superseded by the merged count:
    // 0.0.456's MIB-REQUIRED is index 24 and C5 part 2's CONTINUOUS-NO-FENCE is APPENDED after it at index 25
    // (track 2 was built on 0.0.451, where it was 24; the merge onto 0.0.456 moves it to 25).
    expect_u("C5-2: CONTINUOUS-NO-FENCE is APPENDED at index 25 (after MIB-REQUIRED)", N48_CM_CONT_NO_FENCE, 25u);
    expect_s("C5-2: it has its own name", n48_cm_reason_name(N48_CM_CONT_NO_FENCE), "CONTINUOUS-NO-FENCE");
    expect_u("C5-2: N48_CM_REASONS is 28 after both appends, 0.0.487's and 0.0.500's", N48_CM_REASONS, 28u);
    // build 0.0.487 — COMPUTE-ELIDE-R1 is APPENDED after CONTINUOUS-NO-FENCE, at index 26, and named.
    expect_u("0.0.487: COMPUTE-ELIDE-R1 is APPENDED at index 26", N48_CM_CS_ELIDE_R1, 26u);
    expect_s("0.0.487: it has its own name", n48_cm_reason_name(N48_CM_CS_ELIDE_R1), "COMPUTE-ELIDE-R1");
    // build 0.0.500 — DRAW-ELIDE-R1 is APPENDED after COMPUTE-ELIDE-R1, at index 27, and named.
    expect_u("0.0.500: DRAW-ELIDE-R1 is APPENDED at index 27", N48_CM_DRAW_ELIDE_R1, 27u);
    expect_s("0.0.500: it has its own name", n48_cm_reason_name(N48_CM_DRAW_ELIDE_R1), "DRAW-ELIDE-R1");

    n48_cm_frame c; uint32_t d = 0;
    /* 1. WITH THE SWITCH OFF THE RUNG IS ABSENT: every flag combination is accepted. This is the byte-identity clause —
     *    a boot that never threw 33 must judge frames exactly as 0.0.403 did. */
    good_frame(c, 1040u, 3u);
    c.fs_switch = 0u; c.fs_open = 1u; c.fs_reserve = 0u;
    expect_u("fillset: switch OFF, a non-fill commits (the rung is absent)", gGate(&c, &d), N48_CM_OK);
    good_frame(c, 1040u, 3u);
    c.fs_switch = 0u; c.fs_open = 1u; c.fs_reserve = 1u;
    expect_u("fillset: switch OFF, a reserved fill commits (the rung is absent)", gGate(&c, &d), N48_CM_OK);
    /* 2. ON WITH THE WINDOW OPEN: a NON-fill is refused; the reserved fill passes. This is the whole reservation: the
     *    first shots go to the fills, and an ordinary frame is held back until they do. */
    good_frame(c, 1040u, 3u);
    c.fs_switch = 1u; c.fs_open = 1u; c.fs_reserve = 0u;
    expect_u("fillset: switch ON, open, a non-fill is refused", gGate(&c, &d), N48_CM_RESERVED_FOR_FILL);
    good_frame(c, 1040u, 3u);
    c.fs_switch = 1u; c.fs_open = 1u; c.fs_reserve = 1u;
    expect_u("fillset: switch ON, open, the reserved fill commits", gGate(&c, &d), N48_CM_OK);
    /* 3. ON WITH THE WINDOW CLOSED (both committed, or expired): today's rule. If this refused, the reservation would
     *    turn into "refuse everything" once the fills were done, and the plane would never commit. */
    good_frame(c, 1040u, 3u);
    c.fs_switch = 1u; c.fs_open = 0u; c.fs_reserve = 0u;
    expect_u("fillset: switch ON, window closed, the plane commits", gGate(&c, &d), N48_CM_OK);
    /* 4. THE RUNG IS LAST. A frame an EARLIER rung refuses is refused THERE, not as RESERVED-FOR-FILL: the appended rung
     *    must never hide the rung above it. Non-TRANSLATE first, then DEPENDENCY-STALE, then the LUT rung above it. */
    good_frame(c, 1040u, 3u);
    c.verdict = N48_XV_SHAPE; c.fs_switch = 1u; c.fs_open = 1u; c.fs_reserve = 0u;
    expect_u("fillset: NOT-TRANSLATE still answers before RESERVED-FOR-FILL", gGate(&c, &d), N48_CM_NOT_TRANSLATE);
    good_frame(c, 1040u, 3u);
    c.dep_ok = 0u; c.fs_switch = 1u; c.fs_open = 1u; c.fs_reserve = 0u;
    expect_u("fillset: DEPENDENCY-STALE still answers before RESERVED-FOR-FILL", gGate(&c, &d), N48_CM_DEP_STALE);
    good_frame(c, 1040u, 3u);
    c.lut_switch = 1u; c.lut_plane = 1u; c.lut_ready = 0u; c.fs_switch = 1u; c.fs_open = 1u; c.fs_reserve = 0u;
    expect_u("fillset: LUT-NOT-READY still answers before RESERVED-FOR-FILL", gGate(&c, &d), N48_CM_LUT_NOT_READY);
    /* 5. The rung is a STATE test, not a count: `detail` stays 0. */
    good_frame(c, 1040u, 3u);
    c.fs_switch = 1u; c.fs_open = 1u; c.fs_reserve = 0u; d = 0xDEADBEEFu;
    (void)gGate(&c, &d);
    expect_u("fillset: the refusal's detail is 0", d, 0u);
    /* 6. THE DEFAULT (the three fields nobody set, all 0) IS 0.0.403's GATE. */
    good_frame(c, 1040u, 3u);
    expect_u("fillset: the all-zero default is the old gate", gGate(&c, &d), N48_CM_OK);
    // P1/R1/C5-1 appended RESERVED-FOR-PLANE, FENCE-REGION-MOVED, MEMORY-DESTINATION and RING-FULL after RESERVED-FOR-FILL,
    // then 0.0.456 appended MIB-REQUIRED and C5-2 CONTINUOUS-NO-FENCE, and 0.0.487 COMPUTE-ELIDE-R1, so the fill reason is
    // EIGHT below the current count; pin it by index so a future insert (not append) is caught here too. build 0.0.500:
    // DRAW-ELIDE-R1 appended too - NINE.
    expect_u("fillset: judged at the OLD reason count", N48_CM_RESERVED_FOR_FILL, 19u);
    expect_u("fillset:   and nine reasons follow it", N48_CM_REASONS - 9u, N48_CM_RESERVED_FOR_FILL);
}

/* C5 part 1 (an internal design note Q1, gfx_flightring.h) — THE RING-FULL RUNG. Unconditional (no switch): the
 * ring exists whenever this gate does, so unlike LUT/fillset/plane there is no "switch off" byte-identity case to
 * prove here — the identity claim for THIS rung is the budget-1 identity proved in gfx_flightring_test.cpp instead
 * (a one-entry ring is never full). What this suite proves is the GATE's own wiring: the rung fires exactly on
 * `ring_full`, is placed after TOKEN and before MEMDST/DEP_STALE, and never hides an earlier refusal. */
static void ring_full_checks()
{
    expect_u("C5: RING-FULL is APPENDED at index 23", N48_CM_RING_FULL, 23u);
    expect_s("C5: it has its own name", n48_cm_reason_name(N48_CM_RING_FULL), "RING-FULL");

    n48_cm_frame c; uint32_t d = 0;
    /* 1. ring_full 0: unaffected, the gate commits exactly as before this field existed. */
    good_frame(c, 1040u, 3u);
    c.ring_full = 0u;
    expect_u("C5: ring_full 0 - the frame commits", gGate(&c, &d), N48_CM_OK);
    /* 2. ring_full 1: refused, named, counted. THE PLANTED BREAK for this rung (a caller that forgets to set it) is
     *    exactly the all-zero default below, which proves the OFF direction; there is no "switch" to break here — the
     *    reachability proof is that setting the field alone, with nothing else changed, flips the verdict. */
    good_frame(c, 1040u, 3u);
    c.ring_full = 1u; d = 0xDEADBEEFu;
    expect_u("C5: ring_full 1 - RING-FULL", gGate(&c, &d), N48_CM_RING_FULL);
    /* 3. THE RUNG IS AFTER TOKEN, BEFORE MEMDST/DEP_STALE: an earlier refusal is not hidden by it. */
    good_frame(c, 1040u, 3u);
    c.verdict = N48_XV_SHAPE; c.ring_full = 1u;
    expect_u("C5: NOT-TRANSLATE still answers before RING-FULL", gGate(&c, &d), N48_CM_NOT_TRANSLATE);
    good_frame(c, 1040u, 3u);
    c.token_ok = 0u; c.ring_full = 1u;
    expect_u("C5: TOKEN still answers before RING-FULL", gGate(&c, &d), N48_CM_TOKEN);
    /* 4. AND RING-FULL ANSWERS BEFORE MEMDST/DEP_STALE: it is a "the world" rung placed ahead of the older ones. */
    good_frame(c, 1040u, 3u);
    c.ring_full = 1u; c.dep_ok = 0u;
    expect_u("C5: RING-FULL answers before DEPENDENCY-STALE", gGate(&c, &d), N48_CM_RING_FULL);
    good_frame(c, 1040u, 3u);
    c.ring_full = 1u; c.md_switch = 1u; c.md_ok = 0u;
    expect_u("C5: RING-FULL answers before MEMORY-DESTINATION", gGate(&c, &d), N48_CM_RING_FULL);
    /* 5. THE DEFAULT (nobody set it) IS THE OLD GATE. */
    good_frame(c, 1040u, 3u);
    expect_u("C5: the all-zero default (ring_full unset) is the old gate", gGate(&c, &d), N48_CM_OK);
}

/* =====================================================================================================================
 * build 0.0.456 item 1 ('s fallback: "a small rung refusing COMMIT when !gXdBuild.mib") — THE
 * MIB-REQUIRED RUNG. Checked immediately beside MULTI_IB (same two fields, `mib`/`nib`, answer both), so the one
 * armed shot switch 53 guards can only ever land on a genuine two-head frame: `mib` 1 AND `nib` >= 2.
 * ===================================================================================================================== */
static void mib_required_checks()
{
    expect_u("MIB-REQUIRED is APPENDED at index 24", N48_CM_MIB_REQUIRED, 24u);
    expect_u("MIB-REQUIRED: N48_CM_REASONS is 28 after 0.0.456's append, C5-2's, 0.0.487's and 0.0.500's", N48_CM_REASONS, 28u);
    expect_s("MIB-REQUIRED: it has its own name", n48_cm_reason_name(N48_CM_MIB_REQUIRED), "MIB-REQUIRED");
    expect_u("MIB-REQUIRED: RING-FULL did not move", N48_CM_RING_FULL, 23u);

    n48_cm_frame c; uint32_t d = 0;
    /* 1. WITH THE SWITCH OFF THE RUNG IS ABSENT: a single-IB frame and a genuine two-IB frame both commit exactly as
     *    0.0.455 judged them. This is the byte-identity clause — a boot that never threw 53 must be unaffected. */
    good_frame(c, 1040u, 3u);
    c.mib_req_switch = 0u;
    expect_u("mibreq: switch OFF, a single-IB frame commits (the rung is absent)", gGate(&c, &d), N48_CM_OK);
    n48_cm_frame g; good_mib_frame(g);
    g.mib_req_switch = 0u;
    expect_u("mibreq: switch OFF, a two-IB frame commits (the rung is absent)", gGate(&g, &d), N48_CM_OK);
    /* 2. ON, A SINGLE-IB FRAME (mib 0) IS REFUSED. This is the whole point: the one armed shot cannot land here. */
    good_frame(c, 1040u, 3u);
    c.mib_req_switch = 1u; d = 0xDEADBEEFu;
    expect_u("mibreq: switch ON, a single-IB frame (mib 0) is refused", gGate(&c, &d), N48_CM_MIB_REQUIRED);
    expect_u("mibreq: ... detail is nib", d, 1u);
    /* 3. ON, `mib` 1 WITH `nib` 1 IS ALSO REFUSED — a frame that took the MIB path but never grew a second head is
     *    not a two-head frame either. `ib_n[0]` is deliberately left 0 (poisoned) to prove this rung answers BEFORE
     *    the LEN-sum rung would ever read it. */
    good_frame(c, 1040u, 3u);
    c.mib_req_switch = 1u; c.mib = 1u; c.nib = 1u; c.ib_n[0] = 0u; d = 0xDEADBEEFu;
    expect_u("mibreq: switch ON, mib 1 with nib 1 is refused, before LEN-sum ever reads ib_n[0]", gGate(&c, &d), N48_CM_MIB_REQUIRED);
    expect_u("mibreq: ... detail is nib", d, 1u);
    /* 4. ON, A GENUINE TWO-IB FRAME COMMITS. */
    good_mib_frame(g);
    g.mib_req_switch = 1u;
    expect_u("mibreq: switch ON, a genuine two-IB frame commits", gGate(&g, nullptr), N48_CM_OK);
    /* 5. MULTI_IB STILL ANSWERS FIRST for a shape both rungs could otherwise claim (nib 0): the two rungs share their
     *    input but MULTI_IB is checked first and must not be hidden by the new rung placed beside it. */
    good_frame(c, 1040u, 3u);
    c.mib_req_switch = 1u; c.nib = 0u;
    expect_u("mibreq: MULTI_IB still answers before MIB-REQUIRED (nib 0)", gGate(&c, &d), N48_CM_MULTI_IB);
    /* 6. THE RUNG IS AFTER VERDICT, BUFFERS AND ARM: an earlier refusal is not hidden by it. */
    good_frame(c, 1040u, 3u);
    c.arm = N48_SD_ARM_DECIDE; c.mib_req_switch = 1u;
    expect_u("mibreq: NOT-ARMED still answers before MIB-REQUIRED", gGate(&c, &d), N48_CM_NOT_ARMED);
    good_frame(c, 1040u, 3u);
    c.buffers_ok = 0u; c.mib_req_switch = 1u;
    expect_u("mibreq: NO-BUFFERS still answers before MIB-REQUIRED", gGate(&c, &d), N48_CM_NO_BUFFERS);
    good_frame(c, 1040u, 3u);
    c.verdict = N48_XV_SHAPE; c.mib_req_switch = 1u;
    expect_u("mibreq: NOT-TRANSLATE still answers before MIB-REQUIRED", gGate(&c, &d), N48_CM_NOT_TRANSLATE);
    /* 7. THE DEFAULT (nobody set it) IS THE OLD GATE. */
    good_frame(c, 1040u, 3u);
    expect_u("mibreq: the all-zero default (mib_req_switch unset) is the old gate", gGate(&c, &d), N48_CM_OK);
}

/* =====================================================================================================================
 * C5 part 1 (hardening, an internal design note Q2) — gfxsrc_commit_try's RETURN MUST NEED THE SPEND.
 *
 * This is a REACHABILITY test on the ORDERING (the brief's own words), not a literal-line source pin: it drives the
 * REAL gate (n48_cm_gate) and the REAL one-shot state machine (n48_cm_shot_spend) in the EXACT sequence the kext's
 * gfxsrc_commit_try uses - gate first, spend second - and shows a scenario where the gate answers N48_CM_OK while
 * the spend itself refuses. The gate has NO field that reads the shot's own state (`c.arm` is the CALLER's reading
 * of `n48_cm_shot_level`, a plain uint32_t the test is free to set independently of the shot fixture, exactly as a
 * stale or wrongly-threaded `arm` value would reach the real gate too) - so nothing in n48_cm_gate can prevent a
 * caller in this state from reaching N48_CM_OK. Only tying the RETURN to the spend closes it.
 *
 * Two models of "what commitOk reports", both driven through the SAME two real functions in the SAME order:
 *   commit_try_old(): reason == N48_CM_OK ? 1u : 0u                          (0.0.442's rule - the bug)
 *   commit_try_new(): live && reason == N48_CM_OK && n48_cm_shot_spend(...)  (this build's rule, mirroring the kext)
 * The FIXTURE: a shot already SPENT (budget exhausted by an earlier, real spend on THIS SAME shot - not a mock), so
 * n48_cm_shot_spend refuses (state != ARMED), while the frame's OWN `arm` field still reads COMMIT. old() reports 1
 * (a commit that did not happen); new() reports 0. The kext's own return line is source-pinned separately (it must
 * read `spent ? 1u : 0u`, not `reason == N48_CM_OK`); this proves WHY that pin matters. */
static uint32_t commit_try_old(const n48_cm_frame &frame, n48_cm_shot *sh, uint32_t seq)
{
    n48_cm_frame c = frame;
    uint32_t d = 0u;
    const uint32_t reason = n48_cm_gate(&c, &d);
    const bool live = (c.arm == N48_SD_ARM_COMMIT) && (c.verdict == N48_XV_TRANSLATE);
    if (live && reason == N48_CM_OK) (void)n48_cm_shot_spend(sh, seq);   // called, but NOT what the return depends on
    return reason == N48_CM_OK ? 1u : 0u;
}
static uint32_t commit_try_new(const n48_cm_frame &frame, n48_cm_shot *sh, uint32_t seq)
{
    n48_cm_frame c = frame;
    uint32_t d = 0u;
    const uint32_t reason = n48_cm_gate(&c, &d);
    const bool live = (c.arm == N48_SD_ARM_COMMIT) && (c.verdict == N48_XV_TRANSLATE);
    const bool spent = live && reason == N48_CM_OK && n48_cm_shot_spend(sh, seq);
    return spent ? 1u : 0u;
}
static void commit_try_spend_reachability()
{
    n48_cm_ws_mark m0 {};
    n48_cm_shot sh {};
    // ARM at budget 1 (0.0.380's shape) and spend it once, for real, on seq 1 - exactly what an earlier, already
    // committed frame this boot would have done. The shot is now genuinely SPENT: n48_cm_shot_spend refuses for
    // ANY seq from here on, because the state is no longer ARMED (gfx_commit.h's own rule, unchanged).
    expect_u("reachability: the shot arms", n48_cm_shot_arm(&sh, &m0, 100ull, 1u), 1u);
    expect_u("reachability: the FIRST spend (seq 1) really succeeds", n48_cm_shot_spend(&sh, 1u), 1u);
    expect_u("reachability: the shot is now SPENT (budget exhausted)", sh.state, (uint32_t)N48_CM_SHOT_SPENT);

    // A SECOND frame's gate is asked to judge, with `c.arm` still reading COMMIT (a caller whose own `arm` field is
    // stale, or - the real-world shape this models - two frames racing the same one-shot before the level catches
    // up). The gate has no way to see `sh.state`, so it answers N48_CM_OK on its own merits.
    n48_cm_frame c; good_frame(c, 1040u, 3u);
    uint32_t probe_d = 0u;
    expect_u("reachability: the SECOND frame's gate, in isolation, answers OK", gGate(&c, &probe_d), N48_CM_OK);

    // OLD rule: reports a commit. NEW rule: does not - because n48_cm_shot_spend(sh, 2) refuses (state != ARMED).
    expect_u("reachability: OLD rule reports commitOk=1 for a spend that did NOT happen (the bug)",
             commit_try_old(c, &sh, 2u), 1u);
    // Re-arm fresh so the NEW-rule call below observes the SAME starting condition (SPENT), not one the old call's
    // own (harmless) spend attempt already consumed - n48_cm_shot_spend on an already-SPENT shot is a no-op, so
    // `sh` is unchanged by the OLD call above and this is not strictly needed, but it is asserted so the fixture's
    // own assumption is checked rather than relied on silently.
    expect_u("reachability: the shot is STILL spent (the old call's spend attempt was a refused no-op)",
             sh.state, (uint32_t)N48_CM_SHOT_SPENT);
    expect_u("reachability: NEW rule reports commitOk=0 for the SAME frame (tied to the spend)",
             commit_try_new(c, &sh, 2u), 0u);

    // Non-vacuity, the other direction: with a FRESH arm the spend genuinely succeeds and the NEW rule reports 1 -
    // proving the fix does not simply always answer 0.
    n48_cm_shot sh2 {};
    expect_u("reachability: a fresh arm", n48_cm_shot_arm(&sh2, &m0, 200ull, 1u), 1u);
    n48_cm_frame c2; good_frame(c2, 1040u, 3u);
    expect_u("reachability: NEW rule reports commitOk=1 when the spend genuinely succeeds",
             commit_try_new(c2, &sh2, 3u), 1u);
}

/* =====================================================================================================================
 * 0.0.408 — THE RETIREMENT, ON THE REAL GATE AND THE FRAME THE CALLER ACTUALLY BUILDS.
 *
 * 0.0.407 keyed the retirement on the GATE's answer being DEPENDENCY-STALE. But a reserved fill that did not go live never
 * entered the write branch, so every field that branch fills is still 0 - `pages` included - and the gate refuses it at
 * PAGE-NOT-HOST, ABOVE the dependency rung. For exactly the frame the retirement exists for, the gate's answer can NEVER be
 * DEPENDENCY-STALE, so the member was never given up and the window held for the whole reservation. These checks drive the
 * REAL n48_cm_gate on that frame and retire through the REAL n48_fs_retire, with the real reason codes. NO sentinels, NO
 * hand-set rung: the frame is good_frame with the write branch's fields zeroed, exactly as gfxsrc_commit_try leaves them.
 * ===================================================================================================================== */
static void retire_checks()
{
    const uint64_t kA = kN48FsMemberVa[0];   //: f1's fill
    const uint64_t kB = kN48FsMemberVa[1];   //: the f13-18 twin

    n48_cm_frame c; good_frame(c, 1040u, 1u);
    // THE NON-LIVE SHAPE: not one field the write branch fills. This is what gfxsrc_commit_try hands the gate when `live`
    // is false for a reserved fill (arm == COMMIT, verdict == TRANSLATE, buffers ok, dep_ok 0).
    c.pages = 0u; c.sys_pages = 0u; c.wrote = 0u; c.got = 0u; c.back_sys_pages = 0u; c.mismatch = 0u; c.token_ok = 0u;
    c.fs_switch = 1u; c.fs_open = 1u; c.fs_reserve = 1u;
    c.dep_ok = 0u;
    uint32_t d = 0u;
    const uint32_t reason = n48_cm_gate(&c, &d);
    expect_u("N2 the non-live reserved fill refuses at page-not-host, ABOVE the dependency rung", reason, N48_CM_PAGE);
    expect_u("N2   so its gate reason can never be DEPENDENCY-STALE (the 0.0.407 key is unreachable here)",
             reason == N48_CM_DEP_STALE ? 1u : 0u, 0u);

    // THE CALLER'S RETIREMENT, from the SAME frame: a failed dependency (`!c.dep_ok`) gives the member up, and the
    // dependency world's own reason is recorded verbatim for the `fillset:` line.
    n48_fs f; std::memset(&f, 0, sizeof f); f.on = 1u; n48_fs_open(&f);
    expect_u("N2 the f1 fill on member A commits", n48_fs_commit(&f, kA, 1u), 1u);
    expect_u("N2 the twin fill on member B RESERVES", n48_fs_step(&f, 1u, 1u, kB), N48_FS_RESERVE);
    expect_u("N2 THE CALLER RETIRES IT (dep_ok 0)",
             n48_fs_retire(&f, kB, (uint32_t)(!c.dep_ok), (uint32_t)N48_DEP_SOURCE_NEUTER), 1u);
    expect_u("N2   recording the DEPENDENCY reason for the fillset line", f.last_retire_reason,
             (uint32_t)N48_DEP_SOURCE_NEUTER);
    expect_u("N2   the window CLOSES (A committed, B retired)", n48_fs_win_open(&f), 0u);
    expect_u("N2   so the plane is no longer refused", n48_fs_step(&f, 1u, 0u, kA), N48_FS_PASS);

    // NON-VACUITY THE OTHER WAY: a CLEAN dependency must NOT retire - the member is left seated for a retry.
    n48_fs g; std::memset(&g, 0, sizeof g); g.on = 1u; n48_fs_open(&g);
    expect_u("N2 a clean dependency does NOT retire (the member is left for a retry)",
             n48_fs_retire(&g, kB, 0u, (uint32_t)N48_DEP_OK), 0u);
    expect_u("N2   and the window stays OPEN", n48_fs_win_open(&g), 1u);
}

/* 0.0.420 (an internal design note P1) — THE SECOND WINDOW'S (hold a shot for the plane) APPENDED RUNG.
 * Present ONLY while switch 35 is on AND the second window is open (the fill window has closed, no plane frame has
 * committed, its own expiry has not fired); with it off the gate must be 0.0.419's exactly, the plane must pass, and once
 * the window closes a non-plane must pass too. Each check is the falsifier of one way to write the rung wrong. */
static void planeset_checks()
{
    /* The reason is APPENDED after RESERVED-FOR-FILL (19) and FENCE-REGION-MOVED (20): pin the index and the name. */
    expect_u("P1 the reason is APPENDED at index 21", N48_CM_RESERVED_FOR_PLANE, 21u);
    expect_u("P1 RESERVED-FOR-FILL did not move", N48_CM_RESERVED_FOR_FILL, 19u);
    expect_u("P1 FENCE-REGION-MOVED did not move", N48_CM_FENCE_REGION_MOVED, 20u);
    expect_s("P1 the reason has its own name", n48_cm_reason_name(N48_CM_RESERVED_FOR_PLANE), "RESERVED-FOR-PLANE");

    n48_cm_frame c; uint32_t d = 0;
    /* 1. WITH THE SWITCH OFF THE RUNG IS ABSENT: every flag combination is accepted. The byte-identity clause. */
    good_frame(c, 1040u, 3u);
    c.fp_switch = 0u; c.fp_open = 1u; c.fp_reserve = 0u;
    expect_u("P1 switch OFF, a non-plane commits (the rung is absent)", gGate(&c, &d), N48_CM_OK);
    good_frame(c, 1040u, 3u);
    c.fp_switch = 0u; c.fp_open = 1u; c.fp_reserve = 1u;
    expect_u("P1 switch OFF, a plane commits (the rung is absent)", gGate(&c, &d), N48_CM_OK);
    /* 2. ON WITH THE WINDOW OPEN: a NON-plane is refused; the plane passes. This is the whole second window: the shot the
     *    fills left is held for the plane, and arm29's "second pair" is held back until it commits or expires. */
    good_frame(c, 1040u, 3u);
    c.fp_switch = 1u; c.fp_open = 1u; c.fp_reserve = 0u;
    expect_u("P1 switch ON, open, a non-plane is refused", gGate(&c, &d), N48_CM_RESERVED_FOR_PLANE);
    good_frame(c, 1040u, 3u);
    c.fp_switch = 1u; c.fp_open = 1u; c.fp_reserve = 1u;
    expect_u("P1 switch ON, open, the plane commits", gGate(&c, &d), N48_CM_OK);
    /* 3. ON WITH THE WINDOW CLOSED (a plane committed, or expired): today's rule. If this refused, the window would turn
     *    into "refuse everything" once the plane was done. */
    good_frame(c, 1040u, 3u);
    c.fp_switch = 1u; c.fp_open = 0u; c.fp_reserve = 0u;
    expect_u("P1 switch ON, window closed, a non-plane commits", gGate(&c, &d), N48_CM_OK);
    /* 4. THE RUNG IS LAST, AFTER THE FILL RUNG. A frame an EARLIER rung refuses is refused THERE, not here; and the fill
     *    rung, which precedes it in the chain, still answers first for its own window. */
    good_frame(c, 1040u, 3u);
    c.verdict = N48_XV_SHAPE; c.fp_switch = 1u; c.fp_open = 1u; c.fp_reserve = 0u;
    expect_u("P1 NOT-TRANSLATE still answers before RESERVED-FOR-PLANE", gGate(&c, &d), N48_CM_NOT_TRANSLATE);
    good_frame(c, 1040u, 3u);
    c.dep_ok = 0u; c.fp_switch = 1u; c.fp_open = 1u; c.fp_reserve = 0u;
    expect_u("P1 DEPENDENCY-STALE still answers before RESERVED-FOR-PLANE", gGate(&c, &d), N48_CM_DEP_STALE);
    good_frame(c, 1040u, 3u);
    c.fs_switch = 1u; c.fs_open = 1u; c.fs_reserve = 0u; c.fp_switch = 1u; c.fp_open = 1u; c.fp_reserve = 0u;
    expect_u("P1 the FILL rung still answers before RESERVED-FOR-PLANE", gGate(&c, &d), N48_CM_RESERVED_FOR_FILL);
    /* 5. The rung is a STATE test, not a count: `detail` stays 0. */
    good_frame(c, 1040u, 3u);
    c.fp_switch = 1u; c.fp_open = 1u; c.fp_reserve = 0u; d = 0xDEADBEEFu;
    (void)gGate(&c, &d);
    expect_u("P1 the refusal's detail is 0", d, 0u);
    /* 6. THE DEFAULT (the three fields nobody set, all 0) IS 0.0.419's GATE. */
    good_frame(c, 1040u, 3u);
    expect_u("P1 the all-zero default is the old gate", gGate(&c, &d), N48_CM_OK);
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.426 (MIB-COMMIT B5/B6, tests T1/T5/T6/T9) — THE PER-IB GATE. A multi-IB frame is judged on EVERY IB; a fault in IB 1
// must refuse the WHOLE frame (the action is NEUTER, T5's all-or-nothing), a segment spanning the IB boundary must refuse
// COVER (B5/H2), a HEADLESS claim over nib > 1 must refuse SEG_KIND (B2), and with `mib` 0 the SAME frame refuses MULTI_IB
// exactly as 0.0.425 did (T9's OFF identity, with the new arrays poisoned to prove they are never read).
// ---------------------------------------------------------------------------------------------------------------------
static void mib_gate_checks()
{
    n48_cm_frame g; good_mib_frame(g);
    expect_u("B5 mib: a clean 2-IB frame COMMITS", gGate(&g, nullptr), N48_CM_OK);
    expect_u("B5 mib: ... and its action is TRANSLATE", action_for(g), N48_SD_ACT_TRANSLATE);
    expect_u("B5 mib: LEN == sum(ib_n)", g.n, kMibN0 + kMibN1);

    // T9 — OFF IDENTITY. `mib 0` with the same nib must refuse MULTI_IB, and the arrays must be ignored. Poison them.
    {
        n48_cm_frame k = g; k.mib = 0u;
        for (uint32_t j = 0; j < N48_XV_MAX_IBS; j++) {
            k.pages_ib[j] = 0xdeadbeefu; k.wrote_ib[j] = 0xdeadbeefu; k.got_ib[j] = 0xdeadbeefu;
            k.mismatch_ib[j] = 0xdeadbeefu; k.back_sys_pages_ib[j] = 0xdeadbeefu; k.sys_pages_ib[j] = 0xdeadbeefu;
        }
        uint32_t d = 0;
        expect_u("T9 mib 0: a 2-IB frame refuses MULTI_IB (0.0.425)", gGate(&k, &d), N48_CM_MULTI_IB);
        expect_u("T9 mib 0: ... and the arrays were never read", d, 2u);
    }
    // T1 — EVERY IB1 FAULT REFUSES THE WHOLE FRAME.
    struct { const char *what; uint32_t want; void (*brk)(n48_cm_frame &); } f[] = {
        { "IB1's segment status",        N48_CM_SEG_REFUSED, [](n48_cm_frame &c){ c.seg[2].status = 1u; } },
        { "IB1's write was short",       N48_CM_WRITE_SHORT, [](n48_cm_frame &c){ c.wrote_ib[1] = c.ib_n[1] - 1u; } },
        { "IB1 read back one dword short", N48_CM_READ_SHORT, [](n48_cm_frame &c){ c.got_ib[1] = c.ib_n[1] - 1u; } },
        { "IB1 has a bad dword",         N48_CM_MISMATCH,    [](n48_cm_frame &c){ c.mismatch_ib[1] = 1u; } },
        { "IB1 crossed a non-host page", N48_CM_PAGE,        [](n48_cm_frame &c){ c.sys_pages_ib[1]--; } },
        { "IB1's read-back left VRAM",   N48_CM_READ_VRAM,   [](n48_cm_frame &c){ c.back_sys_pages_ib[1]--; } },
        { "a segment spans the IB boundary", N48_CM_COVER,   [](n48_cm_frame &c){ c.seg[1].end = kMibOff1 + 4u; c.seg[1].out_len = c.seg[1].end - c.seg[1].start; c.seg[2].head = c.seg[1].end; c.seg[2].start = c.seg[2].head + 2u; c.seg[2].out_len = c.seg[2].end - c.seg[2].start; } },
        { "HEADLESS claimed over nib 2", N48_CM_SEG_KIND,    [](n48_cm_frame &c){ c.seg_kind = N48_CM_KIND_HEADLESS; c.hl_ok = 1u; c.hl_total = c.nseg; } },
        { "IB1 has no segments (a hole)",N48_CM_COVER,       [](n48_cm_frame &c){ c.nseg = 2u; } },
    };
    for (auto &x : f) {
        n48_cm_frame k = g; x.brk(k);
        char lbl[160];
        std::snprintf(lbl, sizeof(lbl), "T1 mib: %-38s -> NEUTER", x.what);
        expect_u(lbl, gGate(&k, nullptr), x.want);
        std::snprintf(lbl, sizeof(lbl), "T5 mib: %-38s -> action NEUTER", x.what);
        expect_u(lbl, action_for(k), N48_SD_ACT_NEUTER);
    }
    // T5 all-or-nothing, the positive control: fixing the single fault restores a COMMIT (the sweep above is not vacuous).
    {
        n48_cm_frame k = g; k.mismatch_ib[1] = 1u;
        expect_u("T5 mib: a faulted frame refuses", gGate(&k, nullptr), N48_CM_MISMATCH);
        k.mismatch_ib[1] = 0u;
        expect_u("T5 mib: clearing it restores COMMIT", gGate(&k, nullptr), N48_CM_OK);
    }
}

/* C5 part 2 (an internal design note Q2, build 0.0.460) — THE CONTINUOUS-NO-FENCE RUNG. Present ONLY while
 * `cont_on` is 1 (this shot is a continuous arm); a one-shot's fence-less commit is unaffected. */
static void cont_no_fence_checks()
{
    // build 0.0.463 MERGE FIX (item 1): track 2 alone (built on 0.0.451) pinned CONTINUOUS-NO-FENCE at index
    // 24; merged onto 0.0.456, whose own MIB-REQUIRED already took 24, it is APPENDED after it at index 25 - the
    // SAME move fillset_checks() above already pins (N48_CM_REASONS 26). This literal was left at the stale value
    // by the reviewer's hand-resolution and failed here before the fix (got 0x19 (25) want 0x18 (24)).
    expect_u("C5-2: CONTINUOUS-NO-FENCE is APPENDED at index 25 (after MIB-REQUIRED, merged onto 0.0.456)",
             N48_CM_CONT_NO_FENCE, 25u);
    expect_s("C5-2: it has its own name", n48_cm_reason_name(N48_CM_CONT_NO_FENCE), "CONTINUOUS-NO-FENCE");

    n48_cm_frame c; uint32_t d = 0;
    /* 1. ONE-SHOT (cont_on 0): a fence-less commit is UNAFFECTED, whatever cont_fence_ok reads. The byte-identity
     *    clause: 0.0.451 never set either field, so this is what every prior boot's frame reads. */
    good_frame(c, 1040u, 3u);
    c.cont_on = 0u; c.cont_fence_ok = 0u;
    expect_u("C5-2: one-shot, no fence - commits (the rung is absent)", gGate(&c, &d), N48_CM_OK);
    good_frame(c, 1040u, 3u);
    c.cont_on = 0u; c.cont_fence_ok = 1u;
    expect_u("C5-2: one-shot, WITH a fence - also commits", gGate(&c, &d), N48_CM_OK);
    /* 2. CONTINUOUS (cont_on 1): a fence-less commit is REFUSED; a fenced one passes. */
    good_frame(c, 1040u, 3u);
    c.cont_on = 1u; c.cont_fence_ok = 0u; d = 0xDEADBEEFu;
    expect_u("C5-2: continuous, no fence - CONTINUOUS-NO-FENCE", gGate(&c, &d), N48_CM_CONT_NO_FENCE);
    expect_u("C5-2:   detail is 0 (a state test, not a count)", d, 0u);
    good_frame(c, 1040u, 3u);
    c.cont_on = 1u; c.cont_fence_ok = 1u;
    expect_u("C5-2: continuous, WITH a fence - commits", gGate(&c, &d), N48_CM_OK);
    /* 3. THE RUNG IS AFTER TOKEN AND RING_FULL, BEFORE MEMDST/DEP_STALE: an earlier refusal is not hidden by it,
     *    and it does not hide a later one either. */
    good_frame(c, 1040u, 3u);
    c.verdict = N48_XV_SHAPE; c.cont_on = 1u; c.cont_fence_ok = 0u;
    expect_u("C5-2: NOT-TRANSLATE still answers before CONTINUOUS-NO-FENCE", gGate(&c, &d), N48_CM_NOT_TRANSLATE);
    good_frame(c, 1040u, 3u);
    c.token_ok = 0u; c.cont_on = 1u; c.cont_fence_ok = 0u;
    expect_u("C5-2: TOKEN still answers before CONTINUOUS-NO-FENCE", gGate(&c, &d), N48_CM_TOKEN);
    good_frame(c, 1040u, 3u);
    c.ring_full = 1u; c.cont_on = 1u; c.cont_fence_ok = 0u;
    expect_u("C5-2: RING-FULL still answers before CONTINUOUS-NO-FENCE", gGate(&c, &d), N48_CM_RING_FULL);
    good_frame(c, 1040u, 3u);
    c.cont_on = 1u; c.cont_fence_ok = 0u; c.dep_ok = 0u;
    expect_u("C5-2: CONTINUOUS-NO-FENCE answers before DEPENDENCY-STALE", gGate(&c, &d), N48_CM_CONT_NO_FENCE);
    good_frame(c, 1040u, 3u);
    c.cont_on = 1u; c.cont_fence_ok = 0u; c.md_switch = 1u; c.md_ok = 0u;
    expect_u("C5-2: CONTINUOUS-NO-FENCE answers before MEMORY-DESTINATION", gGate(&c, &d), N48_CM_CONT_NO_FENCE);
    /* 4. THE DEFAULT (nobody set either field) IS THE OLD GATE. */
    good_frame(c, 1040u, 3u);
    expect_u("C5-2: the all-zero default (cont_on unset) is the old gate", gGate(&c, &d), N48_CM_OK);
}

/* =====================================================================================================================
 * C5 part 2 (an internal design note Q2, build 0.0.460) — THE CONTINUOUS ARM'S OWN STATE MACHINE.
 *
 * OFF IDENTITY FIRST: with switch 41 never thrown, n48_cm_shot_arm_cont is never called, so `cont` stays 0 for
 * every arm this boot makes and n48_cm_shot_budget_of/n48_cm_shot_level/n48_cm_shot_spend all read exactly as
 * 0.0.451's one-shot tests above already proved. Everything below is therefore ADDITIVE: it exercises the NEW
 * function and the NEW fields, never the old ones, and one_shot_checks() above is the OFF-identity proof by
 * construction (it never touches n48_cm_shot_arm_cont at all).
 * ===================================================================================================================== */
/* =====================================================================================================================
 * C3 (0.0.461 review, "MY PLANTED BREAKS ON 0.0.460 ... NOT CAUGHT") — THE CONTINUOUS ARM REFUSAL CHECKLIST,
 * DRIVEN DIRECTLY, EACH BIT ALONE AND ALL-CLEAR TOGETHER. THE PLANTED BREAK: `if (!gKsDeferOn) contMissing |=
 * 0x1u;` deleted from the kext's own `if` chain - through 0.0.460 that line was the ONLY thing that could ever be
 * wrong, and nothing drove it. Now it is gfx_commit.h's own n48_cm_cont_arm_missing, called by the kext AND by
 * this test - a deleted bit-set is provably unreachable here, not merely un-reviewed.
 * ===================================================================================================================== */
static void cont_arm_missing_checks()
{
    // ALL CLEAR: every input satisfied - 0 missing.
    expect_u("C3: all seven satisfied - 0 missing", n48_cm_cont_arm_missing(1u, 1u, 0u, 0u, 1u, 1u, 1u), 0u);
    // EACH BIT, ALONE: flip exactly one input away from its satisfied value and check ONLY that bit is set.
    struct { const char *what; uint32_t deferOn, fenceOn, forgiveOn, runBudgetOn, capRoom, planeOn, noFault; uint32_t want; } bits[] = {
        { "switch 22 OFF (deferOn 0)",       0u,1u,0u,0u,1u,1u,1u, 0x1u },
        { "switch 16 OFF (fenceOn 0)",       1u,0u,0u,0u,1u,1u,1u, 0x2u },
        { "switch 15 ON (forgiveOn 1)",      1u,1u,1u,0u,1u,1u,1u, 0x4u },
        { "switch 19 ON (runBudgetOn 1)",    1u,1u,0u,1u,1u,1u,1u, 0x8u },
        { "no frame-cap room (capRoom 0)",   1u,1u,0u,0u,0u,1u,1u, 0x10u },
        { "switch 35 OFF (planeOn 0)",       1u,1u,0u,0u,1u,0u,1u, 0x20u },
        { "M5: a VM fault already latched (noFault 0)", 1u,1u,0u,0u,1u,1u,0u, 0x40u },
    };
    for (auto &b : bits) {
        char lbl[96]; std::snprintf(lbl, sizeof(lbl), "C3: %s sets ONLY its own bit", b.what);
        expect_u(lbl, n48_cm_cont_arm_missing(b.deferOn, b.fenceOn, b.forgiveOn, b.runBudgetOn, b.capRoom, b.planeOn,
                                              b.noFault), b.want);
    }
    // ALL SEVEN WRONG AT ONCE: every bit set, proving none masks another.
    expect_u("C3: all seven wrong - every bit set", n48_cm_cont_arm_missing(0u, 0u, 1u, 1u, 0u, 0u, 0u),
             0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x40u);
}

/* F5b (0.0.462 review) — FENCE-LATE, EACH PATH ALONE. THE PLANTED BREAK NOT CAUGHT: "byPolls = false" (only the
 * time half of the OR ever fires). Driving n48_cm_cont_fence_late with polls at its threshold and time NOT
 * elapsed proves the polls path ALONE is sufficient - a `byPolls = false`-shaped break must fail this. */
static void cont_fence_late_checks()
{
    expect_u("F5b: polls at threshold, time NOT elapsed - late (polls path ALONE)",
             n48_cm_cont_fence_late(4u, 4u, 1000ull, 250000ull), 1u);
    expect_u("F5b: polls one short, time NOT elapsed - not late", n48_cm_cont_fence_late(3u, 4u, 1000ull, 250000ull), 0u);
    expect_u("F5b: polls NOT at threshold, time elapsed - late (time path ALONE)",
             n48_cm_cont_fence_late(1u, 4u, 250000ull, 250000ull), 1u);
    expect_u("F5b: time one short, polls NOT at threshold - not late", n48_cm_cont_fence_late(1u, 4u, 249999ull, 250000ull), 0u);
    expect_u("F5b: NEITHER - not late", n48_cm_cont_fence_late(0u, 4u, 0ull, 250000ull), 0u);
    expect_u("F5b: BOTH - late", n48_cm_cont_fence_late(5u, 4u, 300000ull, 250000ull), 1u);
}

/* F6 (0.0.462 review) — THE STOP LINE'S THREE NUMBERS, DRIVEN DIRECTLY. */
static void cont_stop_report_checks()
{
    n48_cm_ws_mark m0 {};
    n48_cm_shot t {};
    n48_cm_shot_arm_cont(&t, &m0, 1000ull, 60u, 500u);
    n48_cm_shot_cont_start(&t, 5000ull, 1u);
    t.spent = 7u;
    uint64_t sinceStart = 0xdeadull, lastBeforeStop = 0xdeadull; uint32_t spentOut = 0xdeadu;
    n48_cm_stop_report(&t, 9000ull, 8000ull, &sinceStart, &lastBeforeStop, &spentOut);
    expect_u("F6: since-start is stopNow - cont_start_us (9000-5000=4000)", sinceStart, 4000ull);
    expect_u("F6: last-before-stop is stopNow - lastCommitUs (9000-8000=1000)", lastBeforeStop, 1000ull);
    expect_u("F6: spent reads sh->spent (7), not a caller-side tracker", spentOut, 7u);
    // NON-VACUITY: a stopNow BEFORE cont_start_us/lastCommitUs (a torn clock) answers 0, not underflow.
    uint64_t s2 = 0xdeadull, l2 = 0xdeadull;
    n48_cm_stop_report(&t, 100ull, 8000ull, &s2, &l2, nullptr);
    expect_u("F6: a stopNow before cont_start_us answers 0 (no underflow)", s2, 0ull);
    expect_u("F6: a stopNow before lastCommitUs answers 0 (no underflow)", l2, 0ull);
}

/* F7 (0.0.462 review) — THE SUMMARY BASELINE, DRIVEN DIRECTLY. */
static void cont_summary_baseline_checks()
{
    expect_u("F7: before any summary, baseline is cont_start_us", n48_cm_cont_summary_baseline(0ull, 5000ull), 5000ull);
    expect_u("F7: after a summary, baseline is lastSummaryUs (not cont_start_us)",
             n48_cm_cont_summary_baseline(9000ull, 5000ull), 9000ull);
    expect_u("F7: neither yet - 0", n48_cm_cont_summary_baseline(0ull, 0ull), 0ull);
}

/* build 0.0.463 item 2 — THE MID-ARM SWITCH GUARD, DRIVEN DIRECTLY. Every switch in the guarded list (43, 44,
 * 45, 46, 47, 48, 49, 50, 52, 53) must refuse ON and OFF while a continuous arm stands ARMED, must allow a bare
 * read at every state, and must NOT refuse for a one-shot arm, no arm, or a SPENT continuous arm - the OFF
 * identity for every switch NOT in the list. */
static void cont_switch_refused_checks()
{
    // build 0.0.487: 57 (the compute-N elide) joins. build 0.0.488: 60 (the DCC T# strip) joins.
    // build 0.0.486 - switch 59 joins the list. MERGE 0.0.489: all three.
    // build 0.0.490 item 2: 42 (R1's mode) joins, so it leaves the unguarded list below.
    // build 0.0.495: 62 (the shader-heap copy / substitution race) joins.
    // build 0.0.496: 63 (the residency copy through SDMA) joins.
    // build 0.0.497: 64 (the keystone's bounded wait + fill-member retirement) joins.
    // build 0.0.498: 65 (end of pipe asked at the deferral's expiry) joins.
    // build 0.0.500: 66 (the draw elide) joins.
    // build 0.0.501: 67 (PACK) joins.
    // build 0.0.503: 68 (the hybrid newUserClient refusal) joins; its verb ALSO refuses under a one-shot arm
    // (hybrid_policy.h n48_hy_switch_refused, tested in gfx_copyguard_test.cpp T22).
    // build 0.0.505: 69 (the cross-IB rules) joins.
    // build 0.0.506: 70 (the deferred room retry) joins.
    // build 0.0.508: 71 (the last-candidate fence rule) joins.
    // build 0.0.510: 72 (the copy waits for a committed frame's walk) joins.
    // build 0.0.516: 73 (hold a present whose plane's last P did not commit) joins.
    // build 0.0.518: 74 (flip mode) joins.
    // build 0.0.519: 75 (a walk-NOPed flight retires at once) joins.
    // build 0.0.522: 76 (the kext spill tier) joins.
    // build 0.0.523: 77 (the ring-neuter forgiveness) and 78 (the copy-guard redo) join.
    // build 0.0.524: 79 (t0src, S's texture-0 source; read-only) joins.
    // build 0.0.526: 81 (flip kept through a keystone withdrawal, M3, M4) joins.
    // build 0.0.527: 82 (the byte-identical re-copy skip, MEASURE / SKIP) joins.
    // build 0.0.528: 83 (the TLB ack spin poll and its leaf lock) joins.
    // build 0.0.529: 84 (the granule copy guard + delta write, SHADOW / ON) joins.
    // build 0.0.530: 85 (source fills: S1 twin, S2, S3) joins.
    // build 0.0.531: 86 (the present-time retirement re-check) and 87 (the text-element T# log) join.
    // build 0.0.533: 88 (the range-precise heap-generation judge) joins.
    // build 0.0.534: 89 (the SDMA read-back verify) and 90 (the expiry defer on a busy gXdLock) join.
    // build 0.0.535: 91 (N's zero fill) joins.
    // build 0.0.536: 92 (RECTLIST clears: RECT_2D and the v3 VS image) joins.
    // build 0.0.537: 93 (Apple's CB/DB barrier waits on gfx12: PWS) joins.
    // build 0.0.538: 94 (present-time promotion without gXdLock) and 95 (replay a held present) join.
    // build 0.0.540: 96 (perf540, the log-only timers) and 97 (the verifier's table cache) join.
    const uint32_t guarded[] = { 42u, 43u, 44u, 45u, 46u, 47u, 48u, 49u, 50u, 51u, 52u, 53u, 54u, 55u, 56u, 57u, 58u, 59u, 60u, 61u, 62u, 63u, 64u, 65u, 66u, 67u, 68u, 69u, 70u, 71u, 72u, 73u, 74u, 75u, 76u, 77u, 78u, 79u, 80u, 81u, 82u, 83u, 84u, 85u, 86u, 87u, 88u, 89u, 90u, 91u, 92u, 93u, 94u, 95u, 96u, 97u, 103u, 104u, 105u, 106u, 107u, 108u, 109u, 110u };   // build 0.0.544: 103 joins; 0.0.547: 104 and 105; 0.0.548: 106; 0.0.550: 107, 108; 0.0.552: 109, 110
    for (uint32_t sw : guarded) {
        char lbl[128];
        // ON or OFF (is_read 0), cont armed: REFUSED.
        std::snprintf(lbl, sizeof(lbl), "switch guard: %u ON/OFF refused while a continuous arm stands", sw);
        expect_u(lbl, n48_cm_cont_switch_refused(sw, 0u, 1u, (uint32_t)N48_CM_SHOT_ARMED), 1u);
        // A bare read (is_read 1) is ALWAYS allowed, even while armed.
        std::snprintf(lbl, sizeof(lbl), "switch guard: %u a bare read is allowed while armed", sw);
        expect_u(lbl, n48_cm_cont_switch_refused(sw, 1u, 1u, (uint32_t)N48_CM_SHOT_ARMED), 0u);
        // No arm at all (cont 0, state OFF): not refused.
        std::snprintf(lbl, sizeof(lbl), "switch guard: %u not refused with no arm", sw);
        expect_u(lbl, n48_cm_cont_switch_refused(sw, 0u, 0u, (uint32_t)N48_CM_SHOT_OFF), 0u);
        // A ONE-SHOT arm (cont 0, state ARMED): not refused - only a CONTINUOUS arm guards these switches.
        std::snprintf(lbl, sizeof(lbl), "switch guard: %u not refused under a one-shot arm", sw);
        expect_u(lbl, n48_cm_cont_switch_refused(sw, 0u, 0u, (uint32_t)N48_CM_SHOT_ARMED), 0u);
        // A SPENT continuous arm (cont 1, state SPENT): not refused - the arm no longer stands.
        std::snprintf(lbl, sizeof(lbl), "switch guard: %u not refused once a continuous arm is SPENT", sw);
        expect_u(lbl, n48_cm_cont_switch_refused(sw, 0u, 1u, (uint32_t)N48_CM_SHOT_SPENT), 0u);
    }
    // A switch NOT in the guarded list is never refused by this function, whatever the arm state - it is simply
    // not this function's business (its own mid-arm guard, if any, lives elsewhere).
    // build 0.0.485: 58 joins the guarded list above. build 0.0.487: so does 57 (the compute-N elide).
    // build 0.0.488: 60 joins the guarded list; 61 is NOT in it. MERGE 0.0.489: 59 (0.0.486) is now guarded too, so it
    // leaves this list; 62 (unclaimed) takes its place.
    // build 0.0.494: 61 (the D4' image-union fold) joins the guarded list, so it leaves this one; 63 (unclaimed) is added.
    // build 0.0.495: 62 joins the guarded list, so it leaves this one; 64 (unclaimed) is added.
    // build 0.0.496: 63 joins the guarded list, so it leaves this one; 65 (unclaimed) is added.
    // build 0.0.497: 64 joins the guarded list, so it leaves this one; 66 (unclaimed) is added.
    // build 0.0.498: 65 joins the guarded list, so it leaves this one; 67 (unclaimed) is added.
    // build 0.0.500: 66 joins the guarded list, so it leaves this one; 68 (unclaimed) is added.
    // build 0.0.501: 67 joins the guarded list, so it leaves this one; 69 (unclaimed) is added.
    // build 0.0.503: 68 joins the guarded list, so it leaves this one; 70 (unclaimed) is added.
    // build 0.0.505: 69 joins the guarded list, so it leaves this one; 71 (unclaimed) is added.
    // build 0.0.506: 70 joins the guarded list, so it leaves this one; 72 (unclaimed) is added.
    // build 0.0.508: 71 joins the guarded list, so it leaves this one; 73 (unclaimed) is added.
    // build 0.0.510: 72 joins the guarded list, so it leaves this one; 74 (unclaimed) is added.
    // build 0.0.516: 73 joins the guarded list, so it leaves this one; 75 (unclaimed) is added.
    // build 0.0.518: 74 joins the guarded list, so it leaves this one; 76 (unclaimed) is added.
    // build 0.0.519: 75 joins the guarded list, so it leaves this one; 77 (unclaimed) is added.
    // build 0.0.522: 76 joins the guarded list, so it leaves this one; 78 (unclaimed) is added.
    // build 0.0.523: 77 and 78 join the guarded list, so they leave this one; 79 and 80 (unclaimed) are added.
    // build 0.0.524: 79 joins the guarded list, so it leaves this one; 81 (unclaimed) is added.
    // build 0.0.525: 80 joins the guarded list, so it leaves this one; 82 (unclaimed) is added.
    // build 0.0.526: 81 joins the guarded list, so it leaves this one; 83 (unclaimed) is added.
    // build 0.0.527: 82 joins the guarded list, so it leaves this one; 84 (unclaimed) is added.
    // build 0.0.528: 83 joins the guarded list, so it leaves this one; 85 (unclaimed) is added.
    // build 0.0.529: 84 joins the guarded list, so it leaves this one; 86 (unclaimed) is added.
    // build 0.0.530: 85 joins the guarded list, so it leaves this one; 87 (unclaimed) is added.
    // build 0.0.531: 86 and 87 join the guarded list, so they leave this one; 88 and 89 (unclaimed) are added.
    // build 0.0.533: 88 joins the guarded list, so it leaves this one; 90 (unclaimed) is added.
    // build 0.0.534: 89 and 90 join the guarded list, so they leave this one; 91 and 92 (unclaimed) are added.
    // build 0.0.535: 91 joins the guarded list, so it leaves this one; 93 (unclaimed) is added (92 stays unclaimed).
    // build 0.0.536: 92 joins the guarded list, so it leaves this one; 94 (unclaimed) is added.
    // build 0.0.537: 93 joins the guarded list, so it leaves this one; 95 (unclaimed) is added.
    // build 0.0.538: 94 and 95 join the guarded list, so they leave this one; 96 and 97 (unclaimed) are added.
    // build 0.0.540: 96 and 97 join the guarded list, so they leave this one; 98 and 99 (unclaimed) are added.
    // build 0.0.541: 98 and 99 join the guarded list, so they leave this one; 100 and 101 (unclaimed) are added.
    // build 0.0.543: 100, 101 and 102 join the guarded list, so they leave this one; 103 and 104 (unclaimed) are added.
    // build 0.0.544: 103 joins the guarded list, so it leaves this one; 105 (unclaimed) is added.
    // build 0.0.547: 104 and 105 join the guarded list, so they leave this one; 106 and 107 (unclaimed) are added.
    // build 0.0.550: 107 and 108 join the guarded list, so they leave this one; 109 and 110 (unclaimed) are added.
    // build 0.0.552: 109 and 110 join the guarded list, so they leave this one; 111 and 112 (unclaimed) are added.
    // build 0.0.553: 111 joins the guarded list, so it leaves this one; 113 (unclaimed) is added.
    // build 0.0.554: 112 joins the guarded list, so it leaves this one; 114 (unclaimed) is added.
    const uint32_t unguarded[] = { 1u, 3u, 15u, 16u, 19u, 22u, 35u, 37u, 38u, 40u, 41u, 113u, 114u };   // 0.0.548: 106 joins the guarded list
    for (uint32_t sw : unguarded) {
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "switch guard: %u is NOT in this function's list (armed, not-read)", sw);
        expect_u(lbl, n48_cm_cont_switch_refused(sw, 0u, 1u, (uint32_t)N48_CM_SHOT_ARMED), 0u);
    }
}

/* build 0.0.490 item 1 (; the reviewer's correction) — THE ARM-VERB REFUSAL FOR R1 ENFORCE, DRIVEN DIRECTLY
 * over every combination: only a ONE-SHOT arm with 42 ENFORCE and 13 ON is refused; a continuous arm never is (its fence
 * rung keeps the headless producers out); 42 SHADOW/OFF never is (OFF identity); 13 OFF never is. The pre-arm checklist
 * REQUIRES 13 ON (n48_cm_arm_missing's HEADLESS item), which is exactly why the continuous exemption must hold: with it,
 * an ENFORCE arm stays possible. */
static void arm_enforce_hl_checks()
{
    for (uint32_t cont = 0u; cont < 2u; cont++)
        for (uint32_t enf = 0u; enf < 2u; enf++)
            for (uint32_t hl = 0u; hl < 2u; hl++) {
                char lbl[200];
                const uint32_t want = (!cont && enf && hl) ? 1u : 0u;
                std::snprintf(lbl, sizeof(lbl), "ARM-ENFORCE-HL: %s arm, 42 %s, 13 %s -> %s", cont ? "continuous" : "one-shot",
                              enf ? "ENFORCE" : "SHADOW/OFF", hl ? "ON" : "OFF", want ? "REFUSED" : "allowed");
                expect_u(lbl, n48_cm_arm_enforce_hl_refused(cont, enf, hl), want);
            }
    // With 13 ON (the checklist's own requirement) ENFORCE is armable: continuous yes, one-shot no.
    n48_cm_arm_req r {};
    r.decide_armed = 1u; r.buffers_ok = 1u; r.sampled = 1u; r.observers = N48_DEP_OBS_REQUIRED;
    r.observers_required = N48_DEP_OBS_REQUIRED; r.early_bit = N48_DEP_OBS_EARLY; r.e1_pass = 1u; r.n1_on = 1u;
    r.latch_enforced = 1u; r.descport_on = 1u; r.resprov_on = 1u; r.headless_on = 1u;
    expect_u("ARM-ENFORCE-HL: a clean checklist (13 ON) + 42 ENFORCE: the CONTINUOUS arm stands (checklist 0, refusal 0)",
             (n48_cm_arm_missing(&r) == 0u && n48_cm_arm_enforce_hl_refused(1u, 1u, r.headless_on) == 0u) ? 1u : 0u, 1u);
    expect_u("ARM-ENFORCE-HL: ... the ONE-SHOT arm is refused by this item alone",
             (n48_cm_arm_missing(&r) == 0u && n48_cm_arm_enforce_hl_refused(0u, 1u, r.headless_on) == 1u) ? 1u : 0u, 1u);
    r.headless_on = 0u;
    expect_u("ARM-ENFORCE-HL: 13 OFF is refused by the checklist (HEADLESS), not by this item",
             (n48_cm_arm_missing(&r) == N48_CM_ARM_HEADLESS && n48_cm_arm_enforce_hl_refused(0u, 1u, 0u) == 0u) ? 1u : 0u, 1u);
}

static void cont_shot_checks()
{
    n48_cm_ws_mark m0 {};

    /* 1. THE TWO CEILINGS, EACH REFUSED ON ITS OWN (the verb's ceiling-moved test: 601 / 5.01 s / 0). Refusing
     *    leaves the shot UNTOUCHED (still OFF), never half-armed. */
    {
        n48_cm_shot t {};
        // build 0.0.530 item 9: the ceiling is 1022 (was 600); 1023 = 0x3FF is switch 41's CLEAR, never a set N.
        expect_u("cont: N = 1023 (over N48_CM_CONT_N_MAX) is refused", n48_cm_shot_arm_cont(&t, &m0, 0ull, 1023u, 500u), 0u);
        expect_u("cont:   and the shot is untouched (still OFF)", t.state, (uint32_t)N48_CM_SHOT_OFF);
    }
    {
        n48_cm_shot t {};
        expect_u("cont: T = 501 (5.01 s, over N48_CM_CONT_T_MAX) is refused", n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 501u), 0u);
        expect_u("cont:   and the shot is untouched (still OFF)", t.state, (uint32_t)N48_CM_SHOT_OFF);
    }
    {
        n48_cm_shot t {};
        expect_u("cont: N = 0 is refused (not a sentinel here - the verb owns OFF)", n48_cm_shot_arm_cont(&t, &m0, 0ull, 0u, 500u), 0u);
        expect_u("cont: T = 0 is refused", n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 0u), 0u);
    }
    /* the exact ceilings PASS (1023/501 refused does not mean 1022/500 also refuse - the off-by-one both ways). */
    {
        n48_cm_shot t {};
        expect_u("cont: N48_CM_CONT_N_MAX is 1022 (: the largest settable 10-bit N; 0x3FF is CLEAR)", N48_CM_CONT_N_MAX, 1022u);
        expect_u("cont: N = 1022 (the ceiling) is accepted", n48_cm_shot_arm_cont(&t, &m0, 0ull, 1022u, 500u), 1u);
        expect_u("cont:   cont_n == 1022", t.cont_n, 1022u);
    }
    {
        n48_cm_shot t {};
        expect_u("cont: N = 600 (the old ceiling) is still accepted", n48_cm_shot_arm_cont(&t, &m0, 0ull, 600u, 500u), 1u);
        expect_u("cont:   armed", t.state, (uint32_t)N48_CM_SHOT_ARMED);
        expect_u("cont:   cont_n == 600", t.cont_n, 600u);
        expect_u("cont:   cont_t_us == 5,000,000 (500 * 10 ms)", (uint64_t)t.cont_t_us, (uint64_t)5000000ull);
        expect_u("cont:   cont_start_us == 0 (T has not started)", (uint64_t)t.cont_start_us, 0ull);
        // F1 (0.0.461 review): PRE-PLANE the ceiling is 4 (today's budget-4 one-shot), never cont_n directly.
        expect_u("cont:   budget_of is 4 PRE-PLANE, not cont_n (F1)", n48_cm_shot_budget_of(&t), (uint32_t)N48_CM_SHOT_BUDGET_MAX);
        n48_cm_shot_cont_start(&t, 10ull, 1u);
        expect_u("cont:   budget_of honours cont_n once T has started (post-plane)", n48_cm_shot_budget_of(&t), 600u);
    }
    {
        // M2 (0.0.462 review) — PRE-PLANE CEILING IS min(4, N), NOT A BARE 4: with N = 2 (< 4), the pre-plane
        // ceiling must be 2, never 4 - a bare-4 break would let this arm pre-plane-spend DOUBLE its own N.
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 0ull, 2u, 500u);
        expect_u("M2: pre-plane budget_of is min(4,N)=2 when N=2", n48_cm_shot_budget_of(&t), 2u);
        expect_u("M2: spend 1/2 - still ARMED", n48_cm_shot_spend(&t, 1u), 1u);
        expect_u("M2:   ", t.state, (uint32_t)N48_CM_SHOT_ARMED);
        expect_u("M2: spend 2/2 - SPENT at 2, never reaching 4", n48_cm_shot_spend(&t, 2u), 1u);
        expect_u("M2:   SPENT", t.state, (uint32_t)N48_CM_SHOT_SPENT);
        expect_u("M2:   spent == 2 (never let through to 4)", t.spent, 2u);
    }
    {
        // NON-VACUITY the other way: N = 4 exactly still reads 4 (min(4,4)=4), and N > 4 still reads 4 pre-plane
        // (min(4,600)=4) - both already covered above; this pins the EQUAL case specifically.
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 0ull, 4u, 500u);
        expect_u("M2: pre-plane budget_of is min(4,4)=4", n48_cm_shot_budget_of(&t), (uint32_t)N48_CM_SHOT_BUDGET_MAX);
    }

    /* 2. N REACHED — n48_cm_shot_spend is the SAME function a one-shot uses; this proves it honours cont_n (not
     *    N48_CM_SHOT_BUDGET_MAX) and tags stop_why POST-PLANE, and the F1 pre-plane cap of 4 SEPARATELY.
     *    THE PLANTED BREAK THIS CATCHES: ">=" written as ">" would leave the shot ARMED after the Nth spend
     *    instead of SPENT, on EITHER ceiling. */
    {
        // POST-PLANE: N=3, ceiling is cont_n once T has started.
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 100ull, 3u, 500u);
        n48_cm_shot_cont_start(&t, 100ull, 1u);   // the plane committed at the very first spend's instant
        expect_u("cont: N=3 (post-plane), spend 1/3 - still ARMED", n48_cm_shot_spend(&t, 1u), 1u);
        expect_u("cont:   ", t.state, (uint32_t)N48_CM_SHOT_ARMED);
        expect_u("cont: N=3 (post-plane), spend 2/3 - still ARMED", n48_cm_shot_spend(&t, 2u), 1u);
        expect_u("cont:   ", t.state, (uint32_t)N48_CM_SHOT_ARMED);
        expect_u("cont: N=3 (post-plane), spend 3/3 - N REACHED, SPENT", n48_cm_shot_spend(&t, 3u), 1u);
        expect_u("cont:   SPENT", t.state, (uint32_t)N48_CM_SHOT_SPENT);
        expect_u("cont:   stop_why is N48_CM_STOP_N", t.stop_why, (uint32_t)N48_CM_STOP_N);
        expect_u("cont:   n48_cm_shot_level now answers DECIDE", n48_cm_shot_level(&t, N48_SD_ARM_COMMIT, &m0),
                 (uint32_t)N48_SD_ARM_DECIDE);
        // NON-VACUITY: a shot that reaches spent == cont_n - 1 must NOT have stopped yet (the ">" mutant's own
        // failure mode is "never stops"; this direction catches "stops one early", the same rung's other misuse).
    }
    {
        // The OFF-BY-ONE the other way, POST-PLANE: N=1 that has not spent yet must still be ARMED.
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 100ull, 1u, 500u);
        n48_cm_shot_cont_start(&t, 100ull, 1u);
        expect_u("cont: N=1 (post-plane), before any spend - ARMED", t.state, (uint32_t)N48_CM_SHOT_ARMED);
        expect_u("cont: N=1 (post-plane), first spend - N REACHED immediately", n48_cm_shot_spend(&t, 1u), 1u);
        expect_u("cont:   SPENT", t.state, (uint32_t)N48_CM_SHOT_SPENT);
    }
    {
        // F1 NON-VACUITY: PRE-PLANE with a LARGE N (600), the shot still stops at exactly 4 spends (today's
        // budget-4 one-shot), never at N - proving the pre-plane ceiling is genuinely 4 and not merely "whatever
        // is smaller". Then, since no plane ever committed, `spent` (4) is already counted against N per the
        // design ("N counts every spend, the pre-plane ones included").
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 100ull, 600u, 500u);
        for (uint32_t i = 1u; i <= 3u; i++) {
            char lbl[64]; std::snprintf(lbl, sizeof(lbl), "cont: pre-plane spend %u/4 - still ARMED", i);
            expect_u(lbl, n48_cm_shot_spend(&t, i), 1u);
            expect_u("cont:   ", t.state, (uint32_t)N48_CM_SHOT_ARMED);
        }
        expect_u("cont: pre-plane spend 4/4 - N48_CM_SHOT_BUDGET_MAX reached, SPENT (never cont_n=600)",
                 n48_cm_shot_spend(&t, 4u), 1u);
        expect_u("cont:   SPENT after exactly 4, with N=600 and no plane ever committing", t.state,
                 (uint32_t)N48_CM_SHOT_SPENT);
        expect_u("cont:   spent == 4 (every pre-plane spend counted against N)", t.spent, 4u);
    }

    /* 2a. F2(b) (0.0.461 review) — THE HARD PRE-PLANE BOUND, independent of switch 35. Mirrors T's own boundary
     *     proof: fires at exactly N48_CM_PREPLANE_BOUND_US, never a moment before; is permanently inert once a
     *     plane has committed (cont_start_us != 0), so it and T can never both apply to the same arm. */
    {
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 1000ull, 60u, 500u);
        expect_u("cont: pre-plane bound: one microsecond short - no stop",
                 n48_cm_shot_stop_if_preplane(&t, 1000ull + N48_CM_PREPLANE_BOUND_US - 1ull), 0u);
        expect_u("cont:   still ARMED", t.state, (uint32_t)N48_CM_SHOT_ARMED);
        expect_u("cont: pre-plane bound: reached exactly - STOPS",
                 n48_cm_shot_stop_if_preplane(&t, 1000ull + N48_CM_PREPLANE_BOUND_US), 1u);
        expect_u("cont:   SPENT", t.state, (uint32_t)N48_CM_SHOT_SPENT);
        expect_u("cont:   stop_why is N48_CM_STOP_PREPLANE_TIMEOUT", t.stop_why, (uint32_t)N48_CM_STOP_PREPLANE_TIMEOUT);
    }
    {
        // Once a plane has committed, the pre-plane bound is PERMANENTLY inert, however late 'now' reads -
        // n48_cm_shot_stop_if_t (T) takes over exclusively from that instant on.
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 1000ull, 60u, 500u);
        n48_cm_shot_cont_start(&t, 2000ull, 1u);
        expect_u("cont: pre-plane bound never fires once T has started",
                 n48_cm_shot_stop_if_preplane(&t, 1000ull + N48_CM_PREPLANE_BOUND_US + 999999ull), 0u);
        expect_u("cont:   still ARMED (T governs from here)", t.state, (uint32_t)N48_CM_SHOT_ARMED);
    }
    {
        // OFF GUARD: never fires on a one-shot.
        n48_cm_shot t {};
        n48_cm_shot_arm(&t, &m0, 0ull, 1u);
        expect_u("cont: pre-plane bound is a no-op on a one-shot", n48_cm_shot_stop_if_preplane(&t, 999999999ull), 0u);
    }

    /* 3. THE FIRST PLANE COMMIT STARTS T. Before it, whatever `now_us` reads, T never fires (the design's OWN
     *    correction over "from armed_at_us"). THE PLANTED BREAK THIS CATCHES: "deadline from
     *    armed_at_us instead of the first plane commit" — modelled by comparing against `armed_at_us` directly
     *    instead of through n48_cm_shot_cont_start/n48_cm_shot_stop_if_t, which would fire EARLY here. */
    {
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 1000ull /* armed_at_us */, 60u, 500u /* T = 5s = 5,000,000 us */);
        // A non-plane commit does NOT start T.
        expect_u("cont: a non-plane commit does not start T", n48_cm_shot_cont_start(&t, 2000ull, 0u), 0u);
        expect_u("cont:   cont_start_us is still 0", (uint64_t)t.cont_start_us, 0ull);
        // 9,000,000 us after armed_at_us (1000): the ARMED-AT-US mutant would have fired by now (9s > 5s bound from
        // 1000), but T has not started (no plane commit yet), so the real function must answer 0.
        expect_u("cont: T never fires before the first plane commit, however late 'now' is",
                 n48_cm_shot_stop_if_t(&t, 9000000ull), 0u);
        expect_u("cont:   still ARMED", t.state, (uint32_t)N48_CM_SHOT_ARMED);
        // The first PLANE commit, at us = 4000, starts T (and the "continuous start" line fires exactly once).
        expect_u("cont: the first plane commit starts T", n48_cm_shot_cont_start(&t, 4000ull, 1u), 1u);
        expect_u("cont:   cont_start_us == 4000 (NOT armed_at_us == 1000)", (uint64_t)t.cont_start_us, 4000ull);
        // A LATER plane commit must not move it (idempotent - T starts once).
        expect_u("cont: a second plane commit does not move T's origin", n48_cm_shot_cont_start(&t, 9999ull, 1u), 0u);
        expect_u("cont:   cont_start_us is still 4000", (uint64_t)t.cont_start_us, 4000ull);
        // Just under 5,000,000 us after cont_start_us (4000): must NOT stop yet.
        expect_u("cont: T not yet elapsed (just under 5s since the first plane commit) - no stop",
                 n48_cm_shot_stop_if_t(&t, 4000ull + 4999999ull), 0u);
        expect_u("cont:   still ARMED", t.state, (uint32_t)N48_CM_SHOT_ARMED);
        // Exactly 5,000,000 us after cont_start_us: STOPS (the boundary, ">=" not ">").
        expect_u("cont: T reached (exactly 5s since the first plane commit) - STOPS",
                 n48_cm_shot_stop_if_t(&t, 4000ull + 5000000ull), 1u);
        expect_u("cont:   SPENT", t.state, (uint32_t)N48_CM_SHOT_SPENT);
        expect_u("cont:   stop_why is N48_CM_STOP_T", t.stop_why, (uint32_t)N48_CM_STOP_T);
        expect_u("cont:   n48_cm_shot_level now answers DECIDE", n48_cm_shot_level(&t, N48_SD_ARM_COMMIT, &m0),
                 (uint32_t)N48_SD_ARM_DECIDE);
    }
    {
        // The other direction of the same boundary: one microsecond short must NOT stop (non-vacuity for ">=").
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 500u);
        n48_cm_shot_cont_start(&t, 0ull, 1u);
        expect_u("cont: T one microsecond short of 5s - no stop", n48_cm_shot_stop_if_t(&t, 4999999ull), 0u);
        expect_u("cont:   still ARMED", t.state, (uint32_t)N48_CM_SHOT_ARMED);
    }
    {
        // THE SENTINEL HAZARD: a first plane commit at genuine uptime 0 must still START T, not be read as "not
        // started yet" - 0 is n48_cm_shot_stop_if_t's OWN sentinel for that. Caught once already while writing
        // this suite (n48_cm_shot_cont_start coerced 0 -> 1, mirroring gfx_flightring.h's n48_fr_push).
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 500u);
        expect_u("cont: a plane commit at genuine uptime 0 still starts T", n48_cm_shot_cont_start(&t, 0ull, 1u), 1u);
        expect_u("cont:   cont_start_us is NOT 0 (the sentinel would erase it)", (uint64_t)t.cont_start_us != 0ull, 1u);
        expect_u("cont:   T still fires once its bound elapses", n48_cm_shot_stop_if_t(&t, 6000000ull), 1u);
        expect_u("cont:   SPENT", t.state, (uint32_t)N48_CM_SHOT_SPENT);
    }

    /* 4. THE SIX SNAPSHOT-DIFF STOPS (item 3's Q2 list minus N and T): n48_cm_shot_stop_if_rose is ONE function
     *    shared by every one of them, so this proves the comparison itself (strictly rising, from the right pair,
     *    from ARMED only) rather than re-deriving six copies. Each of the SIX real reasons is driven through it by
     *    name, so a reason enum ever silently renumbered would be caught by the expect_u below it. */
    const uint32_t sixReasons[] = { N48_CM_STOP_WITHDRAWAL, N48_CM_STOP_DEFER_HAZARD, N48_CM_STOP_VM_FAULT,
                                    N48_CM_STOP_FENCE_LATE, N48_CM_STOP_FENCE_CORRUPT, N48_CM_STOP_OUT_OF_ORDER };
    for (uint32_t why : sixReasons) {
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 500u);
        // NOT rising (now == snapshot): no stop. THE PLANTED BREAK THIS CATCHES: a ">=" or "!=" instead of ">"
        // would fire on a counter that never moved.
        expect_u("cont: a counter that did not rise does not stop", n48_cm_shot_stop_if_rose(&t, 5ull, 5ull, why), 0u);
        expect_u("cont:   still ARMED", t.state, (uint32_t)N48_CM_SHOT_ARMED);
        // FALLING (now < snapshot, e.g. the pair swapped): no stop either - the conservative direction.
        expect_u("cont: a counter that fell (a swapped pair) does not stop", n48_cm_shot_stop_if_rose(&t, 5ull, 4ull, why), 0u);
        expect_u("cont:   still ARMED", t.state, (uint32_t)N48_CM_SHOT_ARMED);
        // RISING: stops, and names the reason.
        expect_u("cont: a counter that rose stops", n48_cm_shot_stop_if_rose(&t, 5ull, 6ull, why), 1u);
        expect_u("cont:   SPENT", t.state, (uint32_t)N48_CM_SHOT_SPENT);
        expect_u("cont:   stop_why matches", t.stop_why, why);
        expect_u("cont:   n48_cm_shot_level now answers DECIDE", n48_cm_shot_level(&t, N48_SD_ARM_COMMIT, &m0),
                 (uint32_t)N48_SD_ARM_DECIDE);
    }
    /* THE OFF GUARD: n48_cm_shot_stop_if_rose is a no-op on a ONE-SHOT (cont 0), whatever the counters say - a
     * one-shot never has a "snapshot at arm" to compare against, and this is what keeps a stray call site from
     * ever stopping a one-shot early. */
    {
        n48_cm_shot t {};
        n48_cm_shot_arm(&t, &m0, 0ull, 1u);
        expect_u("cont: stop_if_rose is a no-op on a ONE-SHOT", n48_cm_shot_stop_if_rose(&t, 5ull, 6ull, N48_CM_STOP_VM_FAULT), 0u);
        expect_u("cont:   still ARMED (one-shot unaffected)", t.state, (uint32_t)N48_CM_SHOT_ARMED);
    }

    /* 5. EVERY STOP REASON, ONE AT A TIME, THROUGH n48_cm_shot_stop DIRECTLY (item 3's TEST: "each stop flag gives
     *    DECIDE and SPENT (a flag dropped from the mask)"). Looping over the WHOLE enum (not just the six above)
     *    proves n48_cm_shot_stop treats every reason uniformly - nothing in it can special-case, and so silently
     *    drop, any one reason the way a hand-written switch in the kext's own call sites could. */
    for (uint32_t why = 1u; why < N48_CM_STOP_REASONS; why++) {
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 500u);
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "cont: stop reason %u (%s) gives SPENT", why, n48_cm_stop_name(why));
        expect_u(lbl, n48_cm_shot_stop(&t, why), 1u);
        expect_u("cont:   SPENT", t.state, (uint32_t)N48_CM_SHOT_SPENT);
        expect_u("cont:   the level is DECIDE", n48_cm_shot_level(&t, N48_SD_ARM_COMMIT, &m0), (uint32_t)N48_SD_ARM_DECIDE);
        expect_u("cont:   stop_why recorded", t.stop_why, why);
        expect_s("cont:   the reason has a real name", n48_cm_stop_name(why), n48_cm_stop_name(why));
        expect_u("cont:   the name is not '?'", std::strcmp(n48_cm_stop_name(why), "?") != 0, 1u);
        // NEVER OFF, always SPENT: an in-flight frame keeps its exemption (item 3's own rule).
        expect_u("cont:   never OFF", t.state == (uint32_t)N48_CM_SHOT_OFF ? 1u : 0u, 0u);
    }
    expect_u("cont: an out-of-range stop name reads ?", std::strcmp(n48_cm_stop_name(99u), "?") == 0, 1u);
    expect_u("cont: N48_CM_STOP_NONE reads its own name, not '?'", std::strcmp(n48_cm_stop_name(N48_CM_STOP_NONE), "?") != 0, 1u);

    /* 6. n48_cm_shot_stop IS IDEMPOTENT AND NEVER FROM A NON-ARMED STATE (mirrors n48_cm_shot_cancel's own proof
     *    above): a stop on an already-SPENT/DONE/CANCELLED/OFF shot answers 0 and changes nothing, so two stop
     *    conditions racing on the same judged frame can never overwrite the first cause recorded. */
    {
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 500u);
        expect_u("cont: first stop wins", n48_cm_shot_stop(&t, N48_CM_STOP_VM_FAULT), 1u);
        expect_u("cont:   stop_why is VM_FAULT", t.stop_why, (uint32_t)N48_CM_STOP_VM_FAULT);
        expect_u("cont: a second stop (racing cause) is refused", n48_cm_shot_stop(&t, N48_CM_STOP_OUT_OF_ORDER), 0u);
        expect_u("cont:   stop_why is UNCHANGED (still VM_FAULT, not overwritten)", t.stop_why, (uint32_t)N48_CM_STOP_VM_FAULT);
    }
    for (uint32_t s = 0; s < N48_CM_SHOT_STATES; s++) {
        if (s == N48_CM_SHOT_ARMED) continue;
        n48_cm_shot t {}; t.state = s;
        char lbl[80];
        std::snprintf(lbl, sizeof(lbl), "cont: n48_cm_shot_stop from state %u answers 0", s);
        expect_u(lbl, n48_cm_shot_stop(&t, N48_CM_STOP_VM_FAULT), 0u);
    }

    /* 7. ARM/DISARM CLEAR THE CONTINUOUS FIELDS (a struct REUSED across arms must never leak a PRIOR continuous
     *    arm's N/T/start/reason into a later one-shot, or a later continuous arm's own numbers). */
    {
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 500u);
        n48_cm_shot_cont_start(&t, 10ull, 1u);
        n48_cm_shot_stop(&t, N48_CM_STOP_VM_FAULT);
        // Re-arm as a PLAIN ONE-SHOT: every continuous field must go back to 0.
        expect_u("cont: a one-shot re-arm clears cont", n48_cm_shot_arm(&t, &m0, 0ull, 1u), 1u);
        expect_u("cont:   cont == 0", t.cont, 0u);
        expect_u("cont:   cont_n == 0", t.cont_n, 0u);
        expect_u("cont:   cont_t_us == 0", (uint64_t)t.cont_t_us, 0ull);
        expect_u("cont:   cont_start_us == 0", (uint64_t)t.cont_start_us, 0ull);
        expect_u("cont:   stop_why == NONE", t.stop_why, (uint32_t)N48_CM_STOP_NONE);
        expect_u("cont:   budget_of is back to the one-shot's own (1)", n48_cm_shot_budget_of(&t), 1u);
    }
    {
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 500u);
        n48_cm_shot_cont_start(&t, 10ull, 1u);
        n48_cm_shot_stop(&t, N48_CM_STOP_T);
        expect_u("cont: disarm clears cont too", n48_cm_shot_disarm(&t), 1u);
        expect_u("cont:   cont == 0", t.cont, 0u);
        expect_u("cont:   cont_n == 0", t.cont_n, 0u);
        expect_u("cont:   cont_t_us == 0", (uint64_t)t.cont_t_us, 0ull);
        expect_u("cont:   cont_start_us == 0", (uint64_t)t.cont_start_us, 0ull);
        expect_u("cont:   stop_why == NONE", t.stop_why, (uint32_t)N48_CM_STOP_NONE);
    }

    /* 8. OFF IDENTITY, DIRECTLY: a zero-initialised shot (the boot value, switch 41 never thrown) reads `cont` 0
     *    and every continuous accessor is a no-op on it. */
    {
        n48_cm_shot z {};
        expect_u("cont: OFF identity - a fresh shot has cont == 0", z.cont, 0u);
        expect_u("cont:   cont_start does nothing", n48_cm_shot_cont_start(&z, 5ull, 1u), 0u);
        expect_u("cont:   stop_if_t does nothing", n48_cm_shot_stop_if_t(&z, 999999999ull), 0u);
        expect_u("cont:   budget_of is the one-shot's default (1)", n48_cm_shot_budget_of(&z), 1u);
    }
}

/* =====================================================================================================================
 * C5 part 2 — REACHABILITY OF THE JUDGED-FRAME-TOP SEQUENCE, ON THE REAL FUNCTIONS, IN THE KEXT'S OWN ORDER.
 *
 * The brief's own rule: "tests drive the real order of state ... a literal-line pin alone is not accepted." This
 * models gfxsrc_decide_frame's new top-of-frame continuous-stop sequence by calling the SAME pure functions
 * (n48_cm_shot_stop_if_t, n48_cm_shot_stop_if_rose) in the SAME order the kext wiring uses - T first (Q2: "the top
 * of gfxsrc_decide_frame"), then the six snapshot-diff stops in the fixed priority this build gives them
 * (withdrawal, keystone-defer hazard, VM fault, fence-late, fence-corrupt, out-of-order). It is not a stand-in for
 * the gate mutant harness above (there is no separate "gate" to swap here) - it is a genuine second driver of the
 * real state machine, in the real call sequence, and an ORDERING break (a swapped priority, an `else` where an
 * independent `if` belongs, a wrong pair) changes WHICH stop_why comes back when two conditions are true at once,
 * which only a multi-condition drive like the one below can catch.
 * ===================================================================================================================== */
/* (0.0.462 review, "tests") — UPDATED TO THE CURRENT ORDER: T, then the pre-plane bound (mutually exclusive with
 * T by construction - n48_cm_shot_stop_if_preplane only fires while cont_start_us is still 0, exactly as T only
 * fires once it is not), then VM-fault as a RAW-STATUS TRANSITION (armFaultLo == 0 && nowFaultLo != 0 - M5/0.0.461's
 * fix over the old rising-COUNT comparison, which re-fired every frame a pre-existing latch was re-read), then
 * withdrawal, the two defer hazards, fence-late, fence-corrupt, out-of-order - all still n48_cm_shot_stop_if_rose,
 * unchanged in shape. */
static uint32_t decide_frame_top_stops_sim(n48_cm_shot *sh, uint64_t nowUs,
                                            uint32_t armFaultLo, uint32_t nowFaultLo,
                                            uint64_t snapWithdrawal, uint64_t nowWithdrawal,
                                            uint64_t snapDefer, uint64_t nowDefer,
                                            uint64_t snapFenceLate, uint64_t nowFenceLate,
                                            uint64_t snapFenceCorrupt, uint64_t nowFenceCorrupt,
                                            uint64_t snapOOO, uint64_t nowOOO)
{
    if (n48_cm_shot_stop_if_t(sh, nowUs)) return sh->stop_why;
    if (n48_cm_shot_stop_if_preplane(sh, nowUs)) return sh->stop_why;
    if (armFaultLo == 0u && nowFaultLo != 0u && n48_cm_shot_stop(sh, N48_CM_STOP_VM_FAULT)) return sh->stop_why;
    if (n48_cm_shot_stop_if_rose(sh, snapWithdrawal, nowWithdrawal, N48_CM_STOP_WITHDRAWAL)) return sh->stop_why;
    if (n48_cm_shot_stop_if_rose(sh, snapDefer, nowDefer, N48_CM_STOP_DEFER_HAZARD)) return sh->stop_why;
    if (n48_cm_shot_stop_if_rose(sh, snapFenceLate, nowFenceLate, N48_CM_STOP_FENCE_LATE)) return sh->stop_why;
    if (n48_cm_shot_stop_if_rose(sh, snapFenceCorrupt, nowFenceCorrupt, N48_CM_STOP_FENCE_CORRUPT)) return sh->stop_why;
    if (n48_cm_shot_stop_if_rose(sh, snapOOO, nowOOO, N48_CM_STOP_OUT_OF_ORDER)) return sh->stop_why;
    return N48_CM_STOP_NONE;
}
/* =====================================================================================================================
 * M1 (0.0.462 review) — A PLANE THAT IS THE 4th SPEND MUST NOT END THE ARM. Models gfxsrc_commit_try's OWN
 * corrected sequence: for a frame that reaches spend (`live && reason == OK`), n48_cm_shot_cont_start is now
 * called BEFORE n48_cm_shot_spend - so a plane-shaped frame lifts the ceiling to N (or leaves it at min(4,N) if
 * this frame is not plane-shaped) BEFORE ITS OWN spend is judged against whichever ceiling applies. THE BUG THIS
 * FIXES: 0.0.461 called spend first, so a plane that happened to be the 4th pre-plane spend was judged against
 * the OLD ceiling (4) and went SPENT with stop_why N, and cont_start only ran (and printed START) AFTER the arm
 * had already ended - "the arm continues" was the one thing that sequence could never produce.
 * ===================================================================================================================== */
static void commit_sequence_sim(n48_cm_shot *sh, uint64_t at_us, uint32_t is_plane, uint32_t seq)
{
    // The exact order gfxsrc_commit_try now uses (M1): cont_start BEFORE spend, gated the same way spend itself
    // is (this models `live && reason == N48_CM_OK`, which every simulated call here satisfies by construction -
    // a frame that would not reach spend is simply never fed into this function).
    if (sh->cont) (void)n48_cm_shot_cont_start(sh, at_us, is_plane);
    (void)n48_cm_shot_spend(sh, seq);
}
static void commit_order_m1_checks()
{
    n48_cm_ws_mark m0 {};
    n48_cm_shot t {};
    n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 500u);   // N = 60, well above 4
    // fill, fill, fill: three non-plane commits, pre-plane ceiling stays min(4,60) = 4.
    commit_sequence_sim(&t, 1000ull, 0u, 1u);
    expect_u("M1: fill 1/3 - still ARMED", t.state, (uint32_t)N48_CM_SHOT_ARMED);
    expect_u("M1:   cont_start_us still 0 (no plane yet)", (uint64_t)t.cont_start_us, 0ull);
    commit_sequence_sim(&t, 1100ull, 0u, 2u);
    expect_u("M1: fill 2/3 - still ARMED", t.state, (uint32_t)N48_CM_SHOT_ARMED);
    commit_sequence_sim(&t, 1200ull, 0u, 3u);
    expect_u("M1: fill 3/3 - still ARMED (3 of the pre-plane ceiling's 4)", t.state, (uint32_t)N48_CM_SHOT_ARMED);
    expect_u("M1:   spent == 3", t.spent, 3u);
    // plane: the 4th spend, AND plane-shaped. THE FIX: cont_start runs first, lifting the ceiling to 60 BEFORE
    // this spend is judged - so spend 4 is judged against 60, not 4, and the arm CONTINUES.
    commit_sequence_sim(&t, 1300ull, 1u, 4u);
    expect_u("M1: plane (spend 4) - cont_start_us IS SET (T started)", (uint64_t)t.cont_start_us, 1300ull);
    expect_u("M1: THE ARM CONTINUES - still ARMED, not SPENT", t.state, (uint32_t)N48_CM_SHOT_ARMED);
    expect_u("M1:   spent == 4, judged against N = 60, not the old ceiling of 4", t.spent, 4u);
    expect_u("M1:   stop_why is still NONE (no stop fired)", t.stop_why, (uint32_t)N48_CM_STOP_NONE);
    // NON-VACUITY: without the fix (spend-before-cont_start), the SAME sequence would go SPENT at spend 4 - proven
    // directly by calling spend BEFORE cont_start on a fresh, identically-armed shot.
    {
        n48_cm_shot old {};
        n48_cm_shot_arm_cont(&old, &m0, 0ull, 60u, 500u);
        n48_cm_shot_spend(&old, 1u); n48_cm_shot_spend(&old, 2u); n48_cm_shot_spend(&old, 3u);
        n48_cm_shot_spend(&old, 4u);   // spend BEFORE cont_start - the 0.0.461 order
        (void)n48_cm_shot_cont_start(&old, 1300ull, 1u);
        expect_u("M1 non-vacuity: the OLD order (spend before cont_start) DOES end the arm at spend 4",
                 old.state, (uint32_t)N48_CM_SHOT_SPENT);
        expect_u("M1 non-vacuity:   ...while cont_start_us still gets set (the START line still prints) - the bug",
                 (uint64_t)old.cont_start_us, 1300ull);
    }
    // Cosmetic: the plane commit's own SPENT-line budget now reads N (60), not the stale pre-plane 4, because
    // cont_start already ran before this spend - n48_cm_shot_budget_of, asked AFTER, sees cont_start_us set.
    expect_u("cosmetic: budget_of after the plane spend reads N (60), not the stale pre-plane 4",
             n48_cm_shot_budget_of(&t), 60u);
}

static void decide_frame_top_reachability()
{
    n48_cm_ws_mark m0 {};
    // Baseline: NOTHING has risen and T has not started (and the pre-plane bound has not elapsed) - NONE, ARMED.
    {
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 500u);
        expect_u("top-sim: nothing fires - NONE",
                 decide_frame_top_stops_sim(&t, 1000ull, 0u,0u, 5,5, 5,5, 5,5, 5,5, 5,5), (uint32_t)N48_CM_STOP_NONE);
        expect_u("top-sim:   still ARMED", t.state, (uint32_t)N48_CM_SHOT_ARMED);
    }
    // Each of the EIGHT reachable ALONE (T, pre-plane, VM-fault-transition, withdrawal, defer-hazard, fence-late,
    // fence-corrupt, out-of-order), fresh shot each time - proves every branch in the chain is REACHED.
    struct { const char *what; uint32_t want; void (*drive)(n48_cm_shot *); } lanes[] = {
        { "T alone", N48_CM_STOP_T, [](n48_cm_shot *t){ n48_cm_shot_cont_start(t, 1000ull, 1u);
            (void)decide_frame_top_stops_sim(t, 6000000ull, 0u,0u, 5,5, 5,5, 5,5, 5,5, 5,5); } },
        { "pre-plane bound alone", N48_CM_STOP_PREPLANE_TIMEOUT, [](n48_cm_shot *t){
            (void)decide_frame_top_stops_sim(t, N48_CM_PREPLANE_BOUND_US, 0u,0u, 5,5, 5,5, 5,5, 5,5, 5,5); } },
        { "vm-fault TRANSITION alone (armFaultLo 0 -> nowFaultLo != 0)", N48_CM_STOP_VM_FAULT, [](n48_cm_shot *t){
            (void)decide_frame_top_stops_sim(t, 0ull, 0u,1u, 5,5, 5,5, 5,5, 5,5, 5,5); } },
        { "withdrawal alone", N48_CM_STOP_WITHDRAWAL, [](n48_cm_shot *t){
            (void)decide_frame_top_stops_sim(t, 0ull, 0u,0u, 5,6, 5,5, 5,5, 5,5, 5,5); } },
        { "defer-hazard alone", N48_CM_STOP_DEFER_HAZARD, [](n48_cm_shot *t){
            (void)decide_frame_top_stops_sim(t, 0ull, 0u,0u, 5,5, 5,6, 5,5, 5,5, 5,5); } },
        { "fence-late alone", N48_CM_STOP_FENCE_LATE, [](n48_cm_shot *t){
            (void)decide_frame_top_stops_sim(t, 0ull, 0u,0u, 5,5, 5,5, 5,6, 5,5, 5,5); } },
        { "fence-corrupt alone", N48_CM_STOP_FENCE_CORRUPT, [](n48_cm_shot *t){
            (void)decide_frame_top_stops_sim(t, 0ull, 0u,0u, 5,5, 5,5, 5,5, 5,6, 5,5); } },
        { "out-of-order alone", N48_CM_STOP_OUT_OF_ORDER, [](n48_cm_shot *t){
            (void)decide_frame_top_stops_sim(t, 0ull, 0u,0u, 5,5, 5,5, 5,5, 5,5, 5,6); } },
    };
    for (auto &lane : lanes) {
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 500u);
        lane.drive(&t);
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "top-sim: %s reaches its own stop_why", lane.what);
        expect_u(lbl, t.stop_why, lane.want);
        expect_u("top-sim:   SPENT", t.state, (uint32_t)N48_CM_SHOT_SPENT);
    }
    // T vs the six snapshot stops, EVERYTHING true at once: T is checked FIRST in this build's own order.
    {
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 500u);
        n48_cm_shot_cont_start(&t, 1000ull, 1u);
        const uint32_t why = decide_frame_top_stops_sim(&t, 6000000ull, 0u,1u, 5,6, 5,6, 5,6, 5,6, 5,6);
        expect_u("top-sim: with EVERYTHING true at once (T started), T wins (checked first)", why, (uint32_t)N48_CM_STOP_T);
    }
    // PRE-PLANE vs the six snapshot stops (T NOT started): pre-plane is checked before the VM-fault/withdrawal/
    // hazard/fence/OOO group, so it wins over all of them too.
    {
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 500u);
        const uint32_t why = decide_frame_top_stops_sim(&t, N48_CM_PREPLANE_BOUND_US, 0u,1u, 5,6, 5,6, 5,6, 5,6, 5,6);
        expect_u("top-sim: pre-plane bound wins over the six snapshot stops (T not started)", why,
                 (uint32_t)N48_CM_STOP_PREPLANE_TIMEOUT);
    }
    // WITHOUT T or the pre-plane bound (elapsed time 0), the six snapshot stops all true at once: VM-fault wins
    // (checked first among them in this build's own order).
    {
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 500u);
        const uint32_t why = decide_frame_top_stops_sim(&t, 0ull, 0u,1u, 5,6, 5,6, 5,6, 5,6, 5,6);
        expect_u("top-sim: VM-fault wins among the six snapshot stops", why, (uint32_t)N48_CM_STOP_VM_FAULT);
    }
    // ...and withdrawal wins when VM-fault is absent.
    {
        n48_cm_shot t {};
        n48_cm_shot_arm_cont(&t, &m0, 0ull, 60u, 500u);
        const uint32_t why = decide_frame_top_stops_sim(&t, 0ull, 0u,0u, 5,6, 5,6, 5,6, 5,6, 5,6);
        expect_u("top-sim: withdrawal wins with VM-fault absent", why, (uint32_t)N48_CM_STOP_WITHDRAWAL);
    }
}


// =====================================================================================================================
// build 0.0.457 item 3 — THE HONEST REHEARSAL.'s GO was taken from rehearsal lines whose probe
// forced verdict = TRANSLATE and wrote = c.n, so it skipped the verdict rung (every two-head frame was target-in-vram) and,
// on a two-head frame, could only answer write-short. These checks prove: (R1) n48_cm_live, the rehearsal's copy of the
// armed path's `live` predicate, is clause for clause the armed path's own inline expression (which R5 pins as still the
// kext's); (R2) n48_cm_rehearse answers WOULD-COMMIT exactly when
// the armed path's own predicate and gate over the frame AS THE LIVE WRITE WOULD LEAVE IT both pass; (R3) in the kext's
// order over decide44 F84's real bytes - switch 54's scan, then n48_xv_decide, then the rehearsal - the line names
// target-in-vram with 54 OFF and says WOULD-COMMIT with 54 ON (N1 ON, as the runs had it); (R4) the line fits the cap;
// (R5) the kext calls them in that order (reachability pins over the real AppleHardwareHook.cpp, argv[1]).
//
// build 0.0.472 item 1 (MERGE FIX, notes: this build's cherry-pick of 0.0.457 onto 0.0.463/0.0.471) — the armed
// `live` expression grew an eighth clause at 0.0.460 (C5 part 2: `&& (!c.cont_on || c.cont_fence_ok)`), which
// n48_cm_live's 0.0.457 body did not follow (see its own comment in gfx_commit.h). R1 and R2 below are extended to
// drive that clause too, and R5's pin now matches the armed line's CURRENT text rather than 0.0.456's.
// =====================================================================================================================
// The armed path's inline `live` expression, verbatim from THIS build's gfxsrc_commit_try (with its globals as
// parameters). Renamed from 0.0.457's `live456`: it is no longer 0.0.456's text (0.0.460 added the cont clause).
// build 0.0.487: renamed `live487` - the armed line gained a ninth clause, COMPUTE-ELIDE-R1's
// `(!c.cs_elided || (gMdMode == N48_MD_MODE_ENFORCE && gXdBuild.md_ok))`.
static uint32_t live487(uint32_t arm, uint32_t verdict, uint32_t buildOk, uint32_t buffers_ok, uint32_t dep_ok,
                        uint32_t mdMode, uint32_t md_ok, uint32_t ring_full, uint32_t contOn, uint32_t contFenceOk,
                        uint32_t csElided, uint32_t heapRefuse, uint32_t drawElided)
{
    const uint32_t kEnforce = 1u;   // N48_MD_MODE_ENFORCE (AppleHardwareHook.cpp: 0 OFF, 1 ENFORCE, 2 SHADOW)
    // build 0.0.495: + `!heapRefuse` (switch 62), the armed line's tenth clause.
    // build 0.0.500: + DRAW-ELIDE-R1 (switch 66), the eleventh.
    const bool live = (arm == N48_SD_ARM_COMMIT) && (verdict == N48_XV_TRANSLATE) && buildOk && buffers_ok && dep_ok &&
                      (mdMode != kEnforce || md_ok) && !ring_full &&
                      (!contOn || contFenceOk) &&
                      (!csElided || (mdMode == kEnforce && md_ok)) &&
                      (!drawElided || (mdMode == kEnforce && md_ok)) &&
                      !heapRefuse;
    return live ? 1u : 0u;
}
static uint32_t rh_tv_cb(void *ud, uint64_t va, uint32_t *isSys)
{
    (void)ud; *isSys = 0u;
    return (va >= 0x400000000ull && va < 0x500000000ull) ? 1u : 0u;   // every F84 target resolves, into VRAM (as on the runs)
}
static n48_gcap_item gRhItems[N48_GCAP_TV_ITEMS];
static n48_gcap_cbt gRhCbt;
// decide44 F84 through the kext's order: the scan step per IB, the per-draw resolve, then n48_xv_decide over a frame
// whose every other rung passes (programs, 12 segments, descriptors), so the verdict is the target rung's answer.
static uint32_t rh_f84_verdict(uint32_t on, uint32_t n1)
{
    const uint32_t *ib[2] = { kF84Ib0, kF84Ib1 };
    const uint32_t len[2] = { 15520u, 7616u };
    static n48_xv_frame f;
    f = n48_xv_frame {};
    f.armed = 1u; f.shape_ok = 1u; f.reader_ok = 1u; f.budget_left = 1u; f.nib = 2u;
    if (on) n48_gcap_cbt_reset(&gRhCbt);
    uint32_t why = 0u;
    for (uint32_t k = 0; k < 2u; k++) {
        f.ib[k].len = f.ib[k].got = f.ib[k].walk = len[k];
        uint32_t total = 0u, ref = 0u;
        (void)n48_gcap_tv_ib(on, ib[k], len[k], 0x400000000ull, gRhItems, &total, &ref, &why, &gRhCbt);
        if (ref) f.target_vram = 1u;
    }
    if (on && n48_gcap_tv_resolve(&gRhCbt, rh_tv_cb, nullptr, n1, 1u, &why, nullptr)) f.target_vram = 1u;
    f.nseg = 12u;
    uint64_t key = 0u; uint32_t det = 0u;
    return n48_xv_decide(&f, &key, &det);
}
// The dry branch's probe, exactly as gfxsrc_commit_try builds it: a copy of the frame, arm forced to COMMIT, the verdict
// the frame's own, the writes from n48_cm_rehearse_wrote (the dry branch made none, so every wrote field starts 0).
static void rh_probe_of(const n48_cm_frame &c, n48_cm_frame &p)
{
    p = c;
    p.arm = N48_SD_ARM_COMMIT;
    n48_cm_rehearse_wrote(&p);
}
static std::string rh_slurp(const char *path)
{
    std::string out;
    FILE *fp = path ? std::fopen(path, "rb") : nullptr;
    if (!fp) return out;
    char buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    std::fclose(fp);
    return out;
}
static std::string rh_body(const std::string &src, const char *sig)
{
    const size_t a = src.find(sig);
    if (a == std::string::npos) return std::string();
    const size_t b = src.find("\n}\n", a);
    return b == std::string::npos ? std::string() : src.substr(a, b - a);
}

static void rehearsal457_checks(const char *ahh)
{
    std::printf("== 0.0.457: the honest rehearsal ==\n");
    // R1. n48_cm_live == the armed path's expression, over every combination of its ELEVEN inputs (build 0.0.472
    // item 1 widened this from 0.0.457's eight to include cont_on/cont_fence_ok, bits 8/9 of m; build 0.0.487 adds
    // cs_elided, bit 10).
    {
        uint32_t bad = 0u, n = 0u;
        for (uint32_t m = 0; m < 8192u; m++) {   /* build 0.0.495: bit 11 is heap_refuse; 0.0.500: bit 12 draw_elided */
            const uint32_t arm = (m & 1u) ? N48_SD_ARM_COMMIT : N48_SD_ARM_DECIDE;
            const uint32_t v = (m & 2u) ? N48_XV_TRANSLATE : N48_XV_TARGET_VRAM;
            const uint32_t md = (m & 32u) ? 1u : 2u;   // ENFORCE, or SHADOW
            const uint32_t contOn = (m >> 8) & 1u, contFenceOk = (m >> 9) & 1u, csEl = (m >> 10) & 1u, hg = (m >> 11) & 1u;
            const uint32_t de = (m >> 12) & 1u;   // build 0.0.500
            const uint32_t a = live487(arm, v, (m >> 2) & 1u, (m >> 3) & 1u, (m >> 4) & 1u, md, (m >> 6) & 1u, (m >> 7) & 1u,
                                       contOn, contFenceOk, csEl, hg, de);
            const uint32_t b = n48_cm_live(arm, v, (m >> 2) & 1u, (m >> 3) & 1u, (m >> 4) & 1u, md == 1u ? 1u : 0u,
                                           (m >> 6) & 1u, (m >> 7) & 1u, contOn, contFenceOk, csEl, hg, de);
            n++; if (a != b) bad++;
        }
        expect_u("R1 n48_cm_live equals the armed path's inline `live` over all 8192 input combinations (mismatches)", bad, 0u);
        expect_u("R1 ... combinations tried", n, 8192u);
    }
    // R2. WOULD-COMMIT iff the armed path's own predicate AND its own gate over the frame as the LIVE write leaves it pass.
    // build 0.0.472 item 1 widens the loop from 0.0.457's 512 (9 bits) to 2048 (11 bits: bits 9/10 are
    // cont_on/cont_fence_ok).
    {
        uint32_t bad = 0u, would = 0u;
        for (uint32_t m = 0; m < 16384u; m++) {   /* build 0.0.487: bit 11 is cs_elided; 0.0.495: bit 12 heap_refuse; 0.0.500: bit 13 draw_elided */
            n48_cm_frame c; good_mib_frame(c);
            c.arm = N48_SD_ARM_DECIDE;                     // the rehearsal runs unarmed
            c.verdict = (m & 1u) ? N48_XV_TRANSLATE : N48_XV_TARGET_VRAM;
            const uint32_t built = (m >> 1) & 1u;
            c.buffers_ok = (m >> 2) & 1u;
            c.dep_ok = (m >> 3) & 1u;
            c.md_switch = (m >> 4) & 1u; c.md_ok = (m >> 5) & 1u;
            c.ring_full = (m >> 6) & 1u;
            const uint32_t win = (m >> 7) & 1u;
            if ((m >> 8) & 1u) c.mismatch_ib[1] = 3u;      // a per-frame gate rung (the read-back) refuses
            // build 0.0.472 item 1 — the cont clause, driven the same way as every other bit above.
            c.cont_on = (m >> 9) & 1u; c.cont_fence_ok = (m >> 10) & 1u;
            c.cs_elided = (m >> 11) & 1u;                  // build 0.0.487 — COMPUTE-ELIDE-R1's clause
            c.heap_refuse = (m >> 12) & 1u;                // build 0.0.495 — switch 62's clause
            c.draw_elided = (m >> 13) & 1u;                // build 0.0.500 — DRAW-ELIDE-R1's clause
            c.mib_req_switch = 1u;                         // switch 53 ON, as the MIB recipe runs it
            for (uint32_t k = 0; k < 2u; k++) c.wrote_ib[k] = 0u;
            c.wrote = 0u;                                  // the dry branch wrote nothing
            n48_cm_frame p; rh_probe_of(c, p);
            uint32_t g = 0u, d = 0u;
            const uint32_t rh = n48_cm_rehearse(&p, built, c.md_switch, c.md_ok, win, &g, &d);
            // what the ARMED path would do with this frame: its predicate, then its write (every IB written whole), then
            // its gate over the result
            const uint32_t live = n48_cm_live(N48_SD_ARM_COMMIT, c.verdict, built, c.buffers_ok, c.dep_ok, c.md_switch,
                                              c.md_ok, c.ring_full, c.cont_on, c.cont_fence_ok, c.cs_elided, c.heap_refuse,
                                              c.draw_elided);
            n48_cm_frame a = c; a.arm = N48_SD_ARM_COMMIT;
            for (uint32_t k = 0; k < 2u; k++) a.wrote_ib[k] = live ? a.ib_n[k] : 0u;
            a.wrote = a.wrote_ib[0];
            const uint32_t armed = (live && n48_cm_gate(&a, nullptr) == N48_CM_OK && !win) ? 1u : 0u;
            if ((rh == N48_CM_RH_WOULD) != (armed == 1u)) bad++;
            if (rh == N48_CM_RH_WOULD) would++;
        }
        expect_u("R2 WOULD-COMMIT iff the armed predicate + write + gate would commit (disagreements of 16384)", bad, 0u);
        // every other input clean: md and cont are each independently OFF (either its own -ok bit) or ON-with-ok -
        // three valid (switch, ok) pairs each out of four, exactly the SAME shape for both clauses
        // (`!x_switch || x_ok`) - so 3 (md) x 3 (cont) = 9 commit with cs_elided 0; build 0.0.487: with cs_elided 1
        // only md ENFORCE-and-ok remains (1 x 3 = 3), so 12 of the 4096 commit. build 0.0.495: heap_refuse 1 commits
        // none, so still 12 of the 8192. build 0.0.500: draw_elided 1 is the same rule as cs_elided: with it, only md
        // ENFORCE-and-ok remains, whatever cs_elided says (3 x 3 = 9 at cs 0 would become 3, and 3 at cs 1 stays 3), so
        // 12 (draw_elided 0) + 6 (draw_elided 1) = 18 of the 16384.
        expect_u("R2 ... and exactly the eighteen all-clean combinations commit", would, 18u);
        // and the rung it names is the first failing clause, in the armed predicate's own order
        n48_cm_frame c; good_mib_frame(c); c.arm = N48_SD_ARM_DECIDE; c.mib_req_switch = 1u;
        c.verdict = N48_XV_TARGET_VRAM; c.dep_ok = 0u;
        n48_cm_frame p; rh_probe_of(c, p); uint32_t g = 0u, d = 0u;
        expect_u("R2 verdict AND dependency both bad: names the verdict", n48_cm_rehearse(&p, 1u, 0u, 0u, 0u, &g, &d), N48_CM_RH_VERDICT);
        expect_u("R2 ... with detail = the verdict (target-in-vram)", d, N48_XV_TARGET_VRAM);
        c.verdict = N48_XV_TRANSLATE; rh_probe_of(c, p);
        expect_u("R2 only the dependency bad: DEPENDENCY-STALE", n48_cm_rehearse(&p, 1u, 0u, 0u, 0u, &g, &d), N48_CM_RH_DEP);
        c.dep_ok = 1u; c.nib = 1u; c.mib = 0u; rh_probe_of(c, p);
        expect_u("R2 switch 53 ON and a single-IB frame: the gate", n48_cm_rehearse(&p, 1u, 0u, 0u, 0u, &g, &d), N48_CM_RH_GATE);
        expect_u("R2 ... whose reason is MIB-REQUIRED", g, N48_CM_MIB_REQUIRED);
        // build 0.0.472 item 1 — THE NEW CLAUSE, NAMED DIRECTLY: continuous, no fence, otherwise clean falls
        // through the whole ladder and is named CONT_NO_FENCE, not mis-attributed to RING-FULL.
        c.mib_req_switch = 0u; c.nib = 1u; c.mib = 0u;
        c.cont_on = 1u; c.cont_fence_ok = 0u; rh_probe_of(c, p);
        expect_u("R2 continuous with no fence, otherwise clean: CONTINUOUS-NO-FENCE",
                 n48_cm_rehearse(&p, 1u, 0u, 0u, 0u, &g, &d), N48_CM_RH_CONT_NO_FENCE);
        // build 0.0.487 — the ninth clause, named directly: an elided frame with 42 SHADOW (md_enforce 0), otherwise
        // clean, is COMPUTE-ELIDE-R1 - not mis-attributed to CONTINUOUS-NO-FENCE (the ladder's old fall-through).
        good_frame(c, 1040u, 3u); c.arm = N48_SD_ARM_DECIDE;   // a clean single-IB frame: every gate rung passes
        c.cont_on = 0u; c.cont_fence_ok = 0u; c.cs_elided = 1u; rh_probe_of(c, p);
        expect_u("R2 an elided frame under SHADOW, otherwise clean: COMPUTE-ELIDE-R1",
                 n48_cm_rehearse(&p, 1u, 0u, 0u, 0u, &g, &d), N48_CM_RH_CS_ELIDE);
        // build 0.0.495 — the tenth clause, named directly: heap_refuse on an otherwise clean frame is SHADER-HEAP-GEN,
        // and an elided frame under SHADOW that ALSO has heap_refuse is still COMPUTE-ELIDE-R1 (the earlier clause first).
        {
            n48_cm_frame h; good_frame(h, 1040u, 3u); h.arm = N48_SD_ARM_DECIDE; h.heap_refuse = 1u;
            n48_cm_frame hp; rh_probe_of(h, hp);
            expect_u("R2 heap_refuse on an otherwise clean frame: SHADER-HEAP-GEN",
                     n48_cm_rehearse(&hp, 1u, 0u, 0u, 0u, &g, &d), N48_CM_RH_HEAPGEN);
            h.cs_elided = 1u; rh_probe_of(h, hp);
            expect_u("R2 ... and with an elision under SHADOW too: COMPUTE-ELIDE-R1 (the earlier clause)",
                     n48_cm_rehearse(&hp, 1u, 0u, 0u, 0u, &g, &d), N48_CM_RH_CS_ELIDE);
        }
        // build 0.0.500 — the eleventh clause, named directly: an elided draw under SHADOW, otherwise clean, is
        // DRAW-ELIDE-R1; with heap_refuse too it is SHADER-HEAP-GEN (the earlier clause first); under ENFORCE with R1
        // clean it is WOULD-COMMIT.
        {
            n48_cm_frame h; good_frame(h, 1040u, 3u); h.arm = N48_SD_ARM_DECIDE; h.draw_elided = 2u;
            n48_cm_frame hp; rh_probe_of(h, hp);
            expect_u("R2 0.0.500: an elided draw under SHADOW, otherwise clean: DRAW-ELIDE-R1",
                     n48_cm_rehearse(&hp, 1u, 0u, 0u, 0u, &g, &d), N48_CM_RH_DRAW_ELIDE);
            expect_u("R2 0.0.500: ... its gate reason is DRAW-ELIDE-R1 with the count as detail",
                     g * 100u + d, (uint32_t)N48_CM_DRAW_ELIDE_R1 * 100u + 2u);
            h.heap_refuse = 1u; rh_probe_of(h, hp);
            expect_u("R2 0.0.500: ... with heap_refuse too: SHADER-HEAP-GEN (the earlier clause)",
                     n48_cm_rehearse(&hp, 1u, 0u, 0u, 0u, &g, &d), N48_CM_RH_HEAPGEN);
            h.heap_refuse = 0u; h.md_switch = 1u; h.md_ok = 1u; rh_probe_of(h, hp);
            expect_u("R2 0.0.500: ... under ENFORCE with R1 clean: WOULD-COMMIT",
                     n48_cm_rehearse(&hp, 1u, 1u, 1u, 0u, &g, &d), N48_CM_RH_WOULD);
        }
        expect_u("R2 ... under ENFORCE with R1 not clean: MEMORY-DESTINATION first",
                 n48_cm_rehearse(&p, 1u, 1u, 0u, 0u, &g, &d), N48_CM_RH_MEMDST);
        c.md_switch = 1u; c.md_ok = 1u; rh_probe_of(c, p);
        expect_u("R2 ... under ENFORCE with R1 clean: WOULD-COMMIT", n48_cm_rehearse(&p, 1u, 1u, 1u, 0u, &g, &d), N48_CM_RH_WOULD);
        c.cont_on = 1u; c.cs_elided = 1u; c.md_switch = 0u; c.md_ok = 0u; rh_probe_of(c, p);
        expect_u("R2 continuous without a fence AND elided under SHADOW: CONTINUOUS-NO-FENCE (the earlier clause)",
                 n48_cm_rehearse(&p, 1u, 0u, 0u, 0u, &g, &d), N48_CM_RH_CONT_NO_FENCE);
    }
    // R3. THE KEXT'S ORDER OVER REAL BYTES: scan (54) -> n48_xv_decide -> the rehearsal. N1 ON, as mibA1/decide46 ran.
    {
        const uint32_t vOff = rh_f84_verdict(0u, 1u), vOn = rh_f84_verdict(1u, 1u), vOnN1Off = rh_f84_verdict(1u, 0u);
        expect_u("R3 F84, 54 OFF: the verdict is target-in-vram", vOff, N48_XV_TARGET_VRAM);
        expect_u("R3 F84, 54 ON, N1 ON: TRANSLATE", vOn, N48_XV_TRANSLATE);
        expect_u("R3 F84, 54 ON, N1 OFF: target-in-vram (its targets ARE in VRAM; the rule stands)", vOnN1Off, N48_XV_TARGET_VRAM);
        const uint32_t vs[3] = { vOff, vOn, vOnN1Off };
        const uint32_t want[3] = { N48_CM_RH_VERDICT, N48_CM_RH_WOULD, N48_CM_RH_VERDICT };
        const char *lbl[3] = { "R3 rehearsal, 54 OFF: REFUSED at verdict", "R3 rehearsal, 54 ON: WOULD-COMMIT",
                               "R3 rehearsal, 54 ON, N1 OFF: REFUSED at verdict" };
        for (int i = 0; i < 3; i++) {
            n48_cm_frame c; good_mib_frame(c);
            c.arm = N48_SD_ARM_DECIDE; c.verdict = vs[i]; c.mib_req_switch = 1u;
            for (uint32_t k = 0; k < 2u; k++) c.wrote_ib[k] = 0u;
            c.wrote = 0u;
            n48_cm_frame p; rh_probe_of(c, p); uint32_t g = 0u, d = 0u;
            const uint32_t rh = n48_cm_rehearse(&p, 1u, 0u, 0u, 0u, &g, &d);
            expect_u(lbl[i], rh, want[i]);
            if (i == 0) {
                char b[1024];
                std::snprintf(b, sizeof(b), N48_CM_WOULD_FMT, 1ull, 50ull, c.nib, c.nseg, n48_cm_kind_name(c.seg_kind),
                              n48_xv_reason_name(p.verdict), rh == N48_CM_RH_WOULD ? "WOULD-COMMIT" : "REFUSED at ",
                              n48_xv_reason_name(p.verdict), d, 1u, 1u, "clean", 0u, 0u, 0u, n48_cm_reason_name(g), 1u, 1u,
                              0u, 0u, 0u);
                expect_u("R3 ... and its line says `REFUSED at target-in-vram`", std::strstr(b, "REFUSED at target-in-vram") != nullptr, 1u);
                expect_u("R3 ... and not WOULD-COMMIT", std::strstr(b, "WOULD-COMMIT") == nullptr, 1u);
                // 0.0.456's probe over the SAME frame: verdict forced, wrote = c.n, wrote_ib left 0 -> write-short, which is
                // what decide46's two-head rehearsal lines read; it never looked at the verdict.
                n48_cm_frame o = c; o.arm = N48_SD_ARM_COMMIT; o.verdict = N48_XV_TRANSLATE; o.wrote = c.n;
                expect_u("R3 ... 0.0.456's forced probe answered write-short over the refused frame", n48_cm_gate(&o, nullptr), N48_CM_WRITE_SHORT);
            }
        }
    }
    // R4. THE LINE FITS UNDER n48log's CAP with every field at its widest.
    {
        char b[4096];
        const char *lr = "", *lx = "", *lh = "";
        for (uint32_t r = 0; r < N48_CM_REASONS; r++) if (std::strlen(n48_cm_reason_name(r)) > std::strlen(lr)) lr = n48_cm_reason_name(r);
        for (uint32_t r = 0; r < N48_XV_REASONS; r++) if (std::strlen(n48_xv_reason_name(r)) > std::strlen(lx)) lx = n48_xv_reason_name(r);
        for (uint32_t r = 0; r < N48_CM_RH_REASONS; r++) if (std::strlen(n48_cm_rh_name(r)) > std::strlen(lh)) lh = n48_cm_rh_name(r);
        const char *what = std::strlen(lr) > std::strlen(lx) ? lr : lx;
        if (std::strlen(lh) > std::strlen(what)) what = lh;
        const int n = std::snprintf(b, sizeof(b), N48_CM_WOULD_FMT, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffu,
                                    0xffffffffu, n48_cm_kind_name(N48_CM_KIND_HEADLESS), lx, "WOULD-COMMIT", what, 0xffffffffu,
                                    0xffffffffu, 0xffffffffu, "012345678901234567890123", 0xffffffffu, 0xffffffffu, 0xffffffffu,
                                    lr, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu);
        if (!gQuiet) std::printf("      gfx-commit WOULD line worst case: %d bytes (cap %u)\n", n, N48_LOG_CAP_BODY);
        expect_u("R4 the WOULD line fits under the log cap", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u);
        expect_u("R4 ... and still ends with what it does not model", std::strstr(b, "NOT modelled: keystone") != nullptr, 1u);
    }
    // R5. REACHABILITY IN THE REAL SOURCE: the kext runs them in this order, the switch is read, and the probe is honest.
    {
        const std::string src = rh_slurp(ahh);
        expect_u("R5 the hook source was read (argv[1])", src.empty() ? 0u : 1u, 1u);
        const std::string df = rh_body(src, "static uint32_t gfxsrc_decide_frame(");
        const std::string ct = rh_body(src, "static uint32_t gfxsrc_commit_try(");
        const size_t pRead = df.find("const uint32_t tvOn = gTvScanOn ? 1u : 0u;");
        const size_t pReset = df.find("if (tvOn) n48_gcap_cbt_reset(&gTvCbt);");
        const size_t pLoop = df.find("for (uint32_t k = 0; k < f.nib && f.reader_ok; k++) {");
        const size_t pScan = df.find("n48_gcap_tv_ib(tvOn, dst, got, vm.startVa, items, &total, &tvRef, &tvWhy, &gTvCbt);");
        const size_t pRes = df.find("if (tvOn && f.reader_ok) {");
        const size_t pRes2 = df.find("n48_gcap_tv_resolve(&gTvCbt, tv_page_cb, &tvc,");
        const size_t pDecide = df.find("verdict = n48_xv_decide(&f, &key, &detail);");
        const size_t pTry = df.find("gfxsrc_commit_try(vm, info, ib0Va,");
        const size_t np = std::string::npos;
        expect_u("R5 decide_frame: switch 54 read once, from gTvScanOn",
                 (pRead != np && df.find("gTvScanOn", pRead + std::strlen("const uint32_t tvOn = gTvScanOn")) == np) ? 1u : 0u, 1u);
        expect_u("R5 decide_frame: reset -> IB loop -> scan step -> resolve -> verdict -> gate, in that order",
                 (pRead < pReset && pReset < pLoop && pLoop < pScan && pScan < pRes && pRes < pRes2 && pRes2 < pDecide &&
                  pDecide < pTry && pTry != np) ? 1u : 0u, 1u);
        expect_u("R5 the scan step is the ONLY gcap scan in the verdict's item loop (no 0.0.456 call left beside it)",
                 df.find("(void)n48_gcap_scan(dst, got, vm.startVa, items, 64u, &total);") == np ? 1u : 0u, 1u);
        expect_u("R5 the switch is declared OFF", src.find("static volatile uint32_t gTvScanOn { 0u };") != np ? 1u : 0u, 1u);
        size_t w = 0u, writes = 0u;
        while ((w = src.find("gTvScanOn =", w)) != np) { writes++; w++; }
        expect_u("R5 ... and written by the verb alone (one assignment)", writes, 1u);
        // build 0.0.472 item 1 (MERGE FIX) — the armed `live` line is CURRENT (0.0.460's cont clause included),
        // verbatim; R1 proves n48_cm_live equals it, clause for clause, over every input.
        const size_t pLive = ct.find("const bool live = (arm == N48_SD_ARM_COMMIT) && (verdict == N48_XV_TRANSLATE) && "
                                     "gXdBuild.ok && c.buffers_ok && c.dep_ok &&\n                      (gMdMode != "
                                     "N48_MD_MODE_ENFORCE || gXdBuild.md_ok) && !c.ring_full &&\n                      "
                                     "(!c.cont_on || c.cont_fence_ok) &&\n                      "
                                     "(!c.cs_elided || (gMdMode == N48_MD_MODE_ENFORCE && gXdBuild.md_ok)) &&\n                      "
                                     "(!c.draw_elided || (gMdMode == N48_MD_MODE_ENFORCE && gXdBuild.md_ok)) &&\n                      "   /* 0.0.500 */
                                     "!c.heap_refuse;");   /* build 0.0.495: the tenth clause */
        const size_t pDry = ct.find("} else if (dry) {");
        const size_t pArm = ct.find("probe.arm = N48_SD_ARM_COMMIT;");
        const size_t pWrote = ct.find("n48_cm_rehearse_wrote(&probe);");
        const size_t pRh = ct.find("n48_cm_rehearse(&probe,");
        expect_u("R5 commit_try: the armed `live` line is the CURRENT text, verbatim (the expression n48_cm_live mirrors)", pLive != np ? 1u : 0u, 1u);
        expect_u("R5 commit_try: the dry branch forces arm, fills the writes, then asks n48_cm_rehearse",
                 (pLive < pDry && pDry < pArm && pArm < pWrote && pWrote < pRh && pRh != np) ? 1u : 0u, 1u);
        expect_u("R5 commit_try: nothing forces the verdict any more", ct.find("probe.verdict = N48_XV_TRANSLATE") == np ? 1u : 0u, 1u);
        expect_u("R5 commit_try: nor sets wrote to the whole length", ct.find("probe.wrote = c.n") == np ? 1u : 0u, 1u);
    }
}

// =====================================================================================================================
// build 0.0.473 — 10b: A LONGER CONTINUOUS WINDOW.
//   item 1: bit 28 of switch 41 selects T in 100-ms units (1..600 = 60 s); clear, 10-ms units exactly as 0.0.472.
//   item 2: every per-commit / per-frame line under a continuous arm is capped per arm; the summary prints on the clock.
//   item 3: `cont` is cleared at SPENT -> DONE, so no later rehearsal reads a stale CONTINUOUS-NO-FENCE.
// =====================================================================================================================
/* 0.0.472's switch-41 handler, verbatim in logic (AppleHardwareHook.cpp at 5001a40): the OFF-identity reference. */
static n48_cm_sw41 sw41_0472(uint64_t arg)
{
    n48_cm_sw41 d {};
    d.n = (uint32_t)((arg >> 8) & 0x3FFull);
    d.t = (uint32_t)((arg >> 18) & 0x3FFull);
    d.unit_100ms = 0u;
    if (d.n == 0x3FFu) { d.action = N48_CM_SW41_CLEAR; return d; }
    if (d.n == 0u && d.t == 0u) { d.action = N48_CM_SW41_READ; return d; }
    // build 0.0.530 item 9: the ONE deliberate change from 0.0.472 - N's ceiling 600 -> 1022.
    if (d.n == 0u || d.n > 1022u || d.t == 0u || d.t > 500u) { d.action = N48_CM_SW41_REFUSED; return d; }
    d.action = N48_CM_SW41_SET; d.t_us = (uint64_t)d.t * 10000ull;
    return d;
}
static uint64_t sw41_arg(uint32_t n, uint32_t t, uint32_t unit) { return 41ull | ((uint64_t)n << 8) | ((uint64_t)t << 18) | ((uint64_t)unit << 28); }

static void sw41_decode_checks()
{
    /* OFF IDENTITY, EXHAUSTIVE OVER T: with bit 28 clear, every (N, T) the 10-bit fields can carry decodes to 0.0.472's
     * action and 0.0.472's microseconds, byte for byte. */
    {
        const uint32_t ns[] = { 0u, 1u, 2u, 60u, 599u, 600u, 601u, 1000u, 1022u, 1023u };
        uint32_t bad = 0u, n = 0u;
        for (uint32_t N : ns)
            for (uint32_t T = 0u; T <= 1023u; T++) {
                const n48_cm_sw41 a = n48_cm_sw41_decode(sw41_arg(N, T, 0u)), r = sw41_0472(sw41_arg(N, T, 0u));
                n++;
                if (a.action != r.action || (a.action == N48_CM_SW41_SET && (a.n != r.n || a.t != r.t || a.t_us != r.t_us)) ||
                    (a.action == N48_CM_SW41_SET && a.unit_100ms != 0u)) bad++;
            }
        expect_u("473 sw41: bit 28 CLEAR - every (N, T 0..1023) decodes exactly as 0.0.472 (10240 cases), mismatches", bad, 0u);
        expect_u("473 sw41:   cases run", n, 10240u);
    }
    {
        const n48_cm_sw41 d = n48_cm_sw41_decode(131087401ull);   /* 10a's value: N 60, T 500 x 10 ms */
        expect_u("473 sw41: 10a's 131087401 is still SET", d.action, (uint32_t)N48_CM_SW41_SET);
        expect_u("473 sw41:   N 60", d.n, 60u);
        expect_u("473 sw41:   T 500, 10-ms units", d.t | (d.unit_100ms << 16), 500u);
        expect_u("473 sw41:   5,000,000 us", d.t_us, 5000000ull);
        expect_u("473 sw41: T 501 x 10 ms still REFUSED", n48_cm_sw41_decode(sw41_arg(60u, 501u, 0u)).action, (uint32_t)N48_CM_SW41_REFUSED);
        expect_u("473 sw41: T 600 x 10 ms (bit 28 clear) still REFUSED - the unit is not guessed", n48_cm_sw41_decode(sw41_arg(600u, 600u, 0u)).action,
                 (uint32_t)N48_CM_SW41_REFUSED);
    }
    /* BIT 28 SET: 100-ms units, one cap. */
    {
        const uint64_t a10b = sw41_arg(600u, 600u, 1u);
        expect_u("473 sw41: the 10b argument (N 600, T 60 s) is 425875497", a10b, 425875497ull);
        const n48_cm_sw41 d = n48_cm_sw41_decode(a10b);
        expect_u("473 sw41: bit 28 SET, T 600 x 100 ms - SET", d.action, (uint32_t)N48_CM_SW41_SET);
        expect_u("473 sw41:   unit is 100 ms", d.unit_100ms, 1u);
        expect_u("473 sw41:   N 600", d.n, 600u);
        expect_u("473 sw41:   T = 60,000,000 us (60.0 s)", d.t_us, 60000000ull);
        expect_u("473 sw41:   exactly N48_CM_CONT_T_MAX_US", d.t_us, (uint64_t)N48_CM_CONT_T_MAX_US);
        expect_u("473 sw41: T 1 x 100 ms = 100,000 us", n48_cm_sw41_decode(sw41_arg(60u, 1u, 1u)).t_us, 100000ull);
        expect_u("473 sw41: T 50 x 100 ms = 5 s (10a's T in the new unit)", n48_cm_sw41_decode(sw41_arg(60u, 50u, 1u)).t_us, 5000000ull);
        expect_u("473 sw41: T 601 x 100 ms (60.1 s) REFUSED", n48_cm_sw41_decode(sw41_arg(60u, 601u, 1u)).action, (uint32_t)N48_CM_SW41_REFUSED);
        expect_u("473 sw41:   and carries no T", n48_cm_sw41_decode(sw41_arg(60u, 601u, 1u)).t_us, 0ull);
        expect_u("473 sw41: T 1023 x 100 ms REFUSED", n48_cm_sw41_decode(sw41_arg(60u, 1023u, 1u)).action, (uint32_t)N48_CM_SW41_REFUSED);
        expect_u("473 sw41: T 0 with N set, bit 28 SET - REFUSED", n48_cm_sw41_decode(sw41_arg(60u, 0u, 1u)).action, (uint32_t)N48_CM_SW41_REFUSED);
        expect_u("473 sw41: T 0 with N set, bit 28 clear - REFUSED", n48_cm_sw41_decode(sw41_arg(60u, 0u, 0u)).action, (uint32_t)N48_CM_SW41_REFUSED);
        expect_u("530 sw41: N 601 with a legal 100-ms T - now SET", n48_cm_sw41_decode(sw41_arg(601u, 600u, 1u)).action, (uint32_t)N48_CM_SW41_SET);
        expect_u("530 sw41: N 1022 with a legal 100-ms T - SET (the ceiling)", n48_cm_sw41_decode(sw41_arg(1022u, 600u, 1u)).action, (uint32_t)N48_CM_SW41_SET);
        expect_u("530 sw41: N 1023 is CLEAR, never a set value (any T, either unit)",
                 (n48_cm_sw41_decode(sw41_arg(1023u, 600u, 1u)).action == N48_CM_SW41_CLEAR &&
                  n48_cm_sw41_decode(sw41_arg(1023u, 5u, 0u)).action == N48_CM_SW41_CLEAR &&
                  n48_cm_shot_arm_cont_us(nullptr, nullptr, 0ull, 1023u, 1ull) == 0u) ? 1u : 0u, 1u);
        { n48_cm_shot t {}; n48_cm_ws_mark mk {};
          expect_u("530 arm_us: N 1023 is refused by the header's own guard", n48_cm_shot_arm_cont_us(&t, &mk, 0ull, 1023u, 1000000ull), 0u); }
        expect_u("530 sw41: N 600 with a legal 100-ms T - still SET", n48_cm_sw41_decode(sw41_arg(600u, 600u, 1u)).action, (uint32_t)N48_CM_SW41_SET);
        expect_u("473 sw41: N 0 with a legal 100-ms T - REFUSED", n48_cm_sw41_decode(sw41_arg(0u, 600u, 1u)).action, (uint32_t)N48_CM_SW41_REFUSED);
        expect_u("473 sw41: N 0x3FF with bit 28 - CLEAR", n48_cm_sw41_decode(sw41_arg(0x3FFu, 600u, 1u)).action, (uint32_t)N48_CM_SW41_CLEAR);
        expect_u("473 sw41: bit 28 alone (N 0, T 0) is a READ", n48_cm_sw41_decode(sw41_arg(0u, 0u, 1u)).action, (uint32_t)N48_CM_SW41_READ);
        expect_u("473 sw41: bits above 28 are ignored (bit 29 set, same answer)",
                 n48_cm_sw41_decode(a10b | (1ull << 29)).t_us, 60000000ull);
    }
    /* THE ONE CAP, DIRECTLY, AND THE ARM'S OWN SECOND GUARD. */
    {
        expect_u("473 T: 500 x 10 ms = 5,000,000", n48_cm_cont_t_us(500u, 0u), 5000000ull);
        expect_u("473 T: 501 x 10 ms refused", n48_cm_cont_t_us(501u, 0u), 0ull);
        expect_u("473 T: 600 x 100 ms = 60,000,000", n48_cm_cont_t_us(600u, 1u), 60000000ull);
        expect_u("473 T: 601 x 100 ms refused", n48_cm_cont_t_us(601u, 1u), 0ull);
        expect_u("473 T: 0 refused in either unit", n48_cm_cont_t_us(0u, 0u) | n48_cm_cont_t_us(0u, 1u), 0ull);
        n48_cm_ws_mark m0 {};
        n48_cm_shot t {};
        expect_u("473 arm_us: T 60,000,001 us refused (the header's own second guard)",
                 n48_cm_shot_arm_cont_us(&t, &m0, 0ull, 60u, 60000001ull), 0u);
        expect_u("473 arm_us:   and the shot is untouched (still OFF)", t.state, (uint32_t)N48_CM_SHOT_OFF);
        expect_u("473 arm_us: T 0 refused", n48_cm_shot_arm_cont_us(&t, &m0, 0ull, 60u, 0ull), 0u);
        expect_u("473 arm_us: T 60 s accepted", n48_cm_shot_arm_cont_us(&t, &m0, 0ull, 600u, 60000000ull), 1u);
        expect_u("473 arm_us:   cont_t_us is what was frozen", t.cont_t_us, 60000000ull);
        /* THE 10-ms ARM IS 0.0.472's, over every T its 10-bit field carries: same accept/refuse, same frozen fields. */
        uint32_t bad = 0u;
        for (uint32_t T = 0u; T <= 1023u; T++) {
            n48_cm_shot a {}, r {};
            const uint32_t ra = n48_cm_shot_arm_cont(&a, &m0, 77ull, 60u, T);
            const uint32_t want = (T != 0u && T <= 500u) ? 1u : 0u;
            if (want) { r.state = N48_CM_SHOT_ARMED; r.armed_at_us = 77ull; r.cont = 1u; r.cont_n = 60u; r.cont_t_us = (uint64_t)T * 10000ull; }
            if (ra != want || std::memcmp(&a, &r, sizeof(a)) != 0) bad++;
        }
        expect_u("473 arm (10-ms): every T 0..1023 arms exactly as 0.0.472 (accept iff 1..500, cont_t_us = T*10000), mismatches", bad, 0u);
    }
}

/* A judged frame at the top of gfxsrc_decide_frame: the stops in the kext's order (decide_frame_top_stops_sim above). */
static void cont_t60_stop_checks()
{
    n48_cm_ws_mark m0 {};
    const n48_cm_sw41 d = n48_cm_sw41_decode(sw41_arg(600u, 600u, 1u));
    /* T = 60 s through the REAL stop function, a synthetic clock at arm35's measured ~6.9 judged frames/s (145 ms). */
    {
        n48_cm_shot t {};
        expect_u("473 T60: armed with the decoded 60 s", n48_cm_shot_arm_cont_us(&t, &m0, 1000000ull, d.n, d.t_us), 1u);
        const uint64_t start = 9000000ull;               /* the first plane commit, 8 s after the arm */
        n48_cm_shot_cont_start(&t, start, 1u);
        uint64_t stoppedAt = 0ull; uint32_t frames = 0u, early = 0u;
        for (uint64_t now = start; now <= start + 61000000ull; now += 145000ull) {
            frames++;
            const uint32_t why = decide_frame_top_stops_sim(&t, now, 0u,0u, 5,5, 5,5, 5,5, 5,5, 5,5);
            if (why != N48_CM_STOP_NONE && !stoppedAt) { stoppedAt = now; if (now - start < 60000000ull) early++; }
            if (t.state != N48_CM_SHOT_ARMED) break;
        }
        expect_u("473 T60: no stop before 60 s (the 0.0.472 5-s ceiling would have stopped at 5 s)", early, 0u);
        expect_u("473 T60: stop_why is T", t.stop_why, (uint32_t)N48_CM_STOP_T);
        expect_u("473 T60: it stops on the FIRST judged frame at or past 60 s (within one 145-ms frame)",
                 (stoppedAt >= start + 60000000ull && stoppedAt < start + 60000000ull + 145000ull) ? 1u : 0u, 1u);
        expect_u("473 T60:   SPENT (then xd_shot_finish writes DECIDE)", t.state, (uint32_t)N48_CM_SHOT_SPENT);
        expect_u("473 T60:   ~414 judged frames inside the window", (frames >= 413u && frames <= 416u) ? 1u : 0u, 1u);
        expect_u("473 T60: one microsecond short of 60 s does NOT stop (fresh shot)", [&]{
            n48_cm_shot u {}; n48_cm_shot_arm_cont_us(&u, &m0, 0ull, 600u, d.t_us); n48_cm_shot_cont_start(&u, start, 1u);
            return n48_cm_shot_stop_if_t(&u, start + 59999999ull); }(), 0u);
    }
    /* EVERY OTHER STOP STILL FIRES LATE IN THE LONGER WINDOW (55 s in, long past the old 5 s): each alone. */
    {
        struct { const char *what; uint32_t want; uint32_t lane; } lanes[] = {
            { "vm-fault transition at 55 s", N48_CM_STOP_VM_FAULT, 0u }, { "withdrawal at 55 s", N48_CM_STOP_WITHDRAWAL, 1u },
            { "defer hazard at 55 s", N48_CM_STOP_DEFER_HAZARD, 2u }, { "fence late at 55 s", N48_CM_STOP_FENCE_LATE, 3u },
            { "fence corrupt at 55 s", N48_CM_STOP_FENCE_CORRUPT, 4u }, { "out-of-order at 55 s", N48_CM_STOP_OUT_OF_ORDER, 5u },
        };
        for (auto &l : lanes) {
            n48_cm_shot t {};
            n48_cm_shot_arm_cont_us(&t, &m0, 0ull, 600u, d.t_us);
            n48_cm_shot_cont_start(&t, 1000ull, 1u);
            for (uint64_t now = 1000ull; now < 1000ull + 55000000ull; now += 145000ull)
                (void)decide_frame_top_stops_sim(&t, now, 0u,0u, 5,5, 5,5, 5,5, 5,5, 5,5);
            const uint64_t now = 1000ull + 55000000ull;
            const uint32_t why = decide_frame_top_stops_sim(&t, now, 0u, l.lane == 0u ? 1u : 0u,
                5, l.lane == 1u ? 6 : 5, 5, l.lane == 2u ? 6 : 5, 5, l.lane == 3u ? 6 : 5,
                5, l.lane == 4u ? 6 : 5, 5, l.lane == 5u ? 6 : 5);
            char lbl[96]; std::snprintf(lbl, sizeof(lbl), "473 T60: %s still stops the arm", l.what);
            expect_u(lbl, why, l.want);
        }
        /* N still binds inside a 60-s window: 600 commits at 10 per second end the arm at the 600th (59.9 s), before T. */
        n48_cm_shot t {};
        n48_cm_shot_arm_cont_us(&t, &m0, 0ull, 600u, d.t_us);
        uint32_t seq = 0u;
        for (uint64_t now = 1000ull; now < 1000ull + 60000000ull && t.state == N48_CM_SHOT_ARMED; now += 100000ull) {
            (void)decide_frame_top_stops_sim(&t, now, 0u,0u, 5,5, 5,5, 5,5, 5,5, 5,5);
            if (t.state == N48_CM_SHOT_ARMED) commit_sequence_sim(&t, now, 1u, ++seq);
        }
        expect_u("473 T60: N 600 at 10 commits/s is reached first - stop_why N", t.stop_why, (uint32_t)N48_CM_STOP_N);
        expect_u("473 T60:   spent exactly 600", t.spent, 600u);
        /* WindowServer rebinding at 50 s still drops the level to DECIDE (the CANCELLED path reads this). */
        n48_cm_shot c {};
        n48_cm_shot_arm_cont_us(&c, &m0, 0ull, 600u, d.t_us);
        n48_cm_shot_cont_start(&c, 1000ull, 1u);
        n48_cm_ws_mark moved = m0; moved.rebinds = 1u;
        expect_u("473 T60: a WindowServer rebind at 50 s drops the level to DECIDE", n48_cm_shot_level(&c, N48_SD_ARM_COMMIT, &moved),
                 (uint32_t)N48_SD_ARM_DECIDE);
        /* The pre-plane bound is UNCHANGED (30 s from the arm), whatever T is. */
        n48_cm_shot p {};
        n48_cm_shot_arm_cont_us(&p, &m0, 0ull, 600u, d.t_us);
        expect_u("473 T60: the pre-plane bound is still 30 s with T = 60 s", n48_cm_shot_stop_if_preplane(&p, N48_CM_PREPLANE_BOUND_US), 1u);
    }
}

/* item 3 — `cont` cleared at DONE, in the refusal-safe direction; and xd_shot_finish's read-first order. */
static void cont_done_clears_cont_checks()
{
    n48_cm_ws_mark m0 {};
    /* THE REAL ORDER: arm (60 s) -> the first plane commit -> spends -> T at 60 s -> xd_shot_finish's finish. */
    n48_cm_shot t {};
    n48_cm_shot_arm_cont_us(&t, &m0, 0ull, 600u, 60000000ull);
    commit_sequence_sim(&t, 1000ull, 1u, 1u);
    commit_sequence_sim(&t, 2000ull, 0u, 2u);
    expect_u("473 done: T stops at 60 s", n48_cm_shot_stop_if_t(&t, 1000ull + 60000000ull), 1u);
    expect_u("473 done:   SPENT still carries cont (the in-flight frame's exemption window)", t.cont, 1u);
    /* xd_shot_finish reads these BEFORE the finish (its own order, pinned below): */
    const uint32_t wasCont = t.cont, budgetAtFinish = n48_cm_shot_budget_of(&t);
    expect_u("473 done: SPENT -> DONE", n48_cm_shot_finish(&t), 1u);
    expect_u("473 done:   DONE", t.state, (uint32_t)N48_CM_SHOT_DONE);
    expect_u("473 done:   cont CLEARED at DONE", t.cont, 0u);
    expect_u("473 done:   stop_why kept (T)", t.stop_why, (uint32_t)N48_CM_STOP_T);
    expect_u("473 done:   cont_n kept (600)", t.cont_n, 600u);
    expect_u("473 done:   cont_t_us kept (60 s)", t.cont_t_us, 60000000ull);
    expect_u("473 done:   cont_start_us kept", t.cont_start_us, 1000ull);
    expect_u("473 done: the stop line's inputs read first: wasCont 1", wasCont, 1u);
    expect_u("473 done:   budget read first is N (600)", budgetAtFinish, 600u);
    expect_u("473 done:   ...and after the finish it would read the one-shot's 1 - why it is read first",
             n48_cm_shot_budget_of(&t), 1u);
    /* REFUSAL-SAFE: a cleared flag re-enables nothing. */
    expect_u("473 done: the level for an operator arm of COMMIT is DECIDE", n48_cm_shot_level(&t, N48_SD_ARM_COMMIT, &m0),
             (uint32_t)N48_SD_ARM_DECIDE);
    expect_u("473 done: a spend is refused", n48_cm_shot_spend(&t, 99u), 0u);
    expect_u("473 done: nothing left to spend", n48_cm_shot_left(&t), 0u);
    expect_u("473 done: a stop is refused (not ARMED)", n48_cm_shot_stop(&t, N48_CM_STOP_VM_FAULT), 0u);
    expect_u("473 done: a second finish answers 0", n48_cm_shot_finish(&t), 0u);
    expect_u("473 done: the live predicate at the level it now hands out (DECIDE) is 0",
             n48_cm_live(n48_cm_shot_level(&t, N48_SD_ARM_COMMIT, &m0), N48_XV_TRANSLATE, 1u, 1u, 1u, 0u, 0u, 0u, t.cont, 0u, 0u, 0u, 0u), 0u);
    /* THE LOW ITSELF: a later rehearsal's probe copies c.cont_on from gXdShot.cont; DECIDE never places a fence. */
    n48_cm_frame c; good_frame(c, 1040u, 3u);
    c.arm = n48_cm_shot_level(&t, N48_SD_ARM_COMMIT, &m0);
    c.cont_on = t.cont; c.cont_fence_ok = 0u;
    n48_cm_frame p; rh_probe_of(c, p);
    uint32_t g = 0u, dd = 0u;
    expect_u("473 done: a later rehearsal is NOT named CONTINUOUS-NO-FENCE (it answers WOULD-COMMIT)",
             n48_cm_rehearse(&p, 1u, 0u, 0u, 0u, &g, &dd), (uint32_t)N48_CM_RH_WOULD);
    n48_cm_frame stale = p; stale.cont_on = 1u;
    expect_u("473 done:   (non-vacuity: with 0.0.472's stale flag it WAS CONTINUOUS-NO-FENCE)",
             n48_cm_rehearse(&stale, 1u, 0u, 0u, 0u, &g, &dd), (uint32_t)N48_CM_RH_CONT_NO_FENCE);
    /* THE N PATH TOO: exhausting N moves to SPENT at the spend; the finish clears cont the same way. */
    n48_cm_shot u {};
    n48_cm_shot_arm_cont_us(&u, &m0, 0ull, 1u, 60000000ull);
    commit_sequence_sim(&u, 1000ull, 1u, 1u);
    expect_u("473 done: N 1 exhausted - SPENT, stop_why N", u.state == N48_CM_SHOT_SPENT && u.stop_why == N48_CM_STOP_N, 1u);
    n48_cm_shot_finish(&u);
    expect_u("473 done:   finish clears cont on the N path", u.cont, 0u);
    /* A ONE-SHOT: cont was 0 and stays 0 - the finish is 0.0.472's. */
    n48_cm_shot o {};
    n48_cm_shot_arm(&o, &m0, 0ull, 1u);
    n48_cm_shot_spend(&o, 1u);
    expect_u("473 done: a one-shot's finish answers 1", n48_cm_shot_finish(&o), 1u);
    expect_u("473 done:   cont still 0, budget still 1", o.cont == 0u && n48_cm_shot_budget_of(&o) == 1u, 1u);
    /* A RE-ARM after DONE is a fresh arm (nothing inherited either way). */
    expect_u("473 done: a continuous re-arm after DONE sets cont again", n48_cm_shot_arm_cont_us(&t, &m0, 0ull, 60u, 5000000ull) && t.cont == 1u, 1u);
}

/* item 2 — THE LOG CAPS, DRIVEN OVER 600 COMMITS IN 60 s THROUGH THE PURE FUNCTIONS THE KEXT CALLS, IN ITS ORDER.
 * Per judged frame (10 frames/s, every frame commits): the frame top (stops, then the summary on the clock), the POLL
 * line (one per judged frame while a fence is watched), then commit_try (GATE OK, the spend and its commitFull, SPENT,
 * the summary on a commit), the keystone (VERDICT L1/L2, armed contexts, NO-OP, 4 x (WALK + rootwrite WALK), THE ARM
 * IS), hook_gfxCommitIB (fence828 COMMITTED, TRANSLATED, RING EXEMPTION), the unmap's APPLYING, and one refusal
 * (a keystone refusal: its 3 verdict lines + THE ARM IS + NOT COMMITTED + COMMIT WITHDRAWN) on every 5th commit. */
struct LogSim {
    uint32_t fullCommits, commitFull, pollLines, applyLines, refusalLines;
    uint64_t suppressed, refusalSuppressed;
    uint32_t commits, lastSummaryCommit; uint64_t lastSummaryUs;
};
static bool sim_line(uint32_t cont, LogSim &L, bool refusal)   /* AppleHardwareHook.cpp's xd_cont_line, over the pure calls */
{
    if (n48_cm_cont_log_commit(cont, L.commitFull, nullptr)) return true;
    if (refusal) return n48_cm_cont_log_take(cont, &L.refusalLines, N48_CM_CONT_LOG_REFUSAL_LINES, &L.refusalSuppressed) != 0u;
    L.suppressed++;
    return false;
}
struct LogCount { uint64_t routine, poll, apply, refusal, summary, attempted, maxGapUs; };
static LogCount run_log_sim(uint32_t cont, uint32_t nCommits, uint64_t frameUs, uint32_t commitEvery)
{
    LogSim L {}; LogCount C {};
    const uint64_t start = 5000000ull;
    uint64_t lastSummaryAt = start;
    uint32_t spent = 0u;
    for (uint64_t now = start; now < start + 60000000ull; now += frameUs) {
        const uint64_t frame = (now - start) / frameUs;
        /* frame top: the summary on the clock (count trigger off) */
        if (cont && n48_cm_cont_summary_due(L.commits, L.lastSummaryCommit, 0u, now, L.lastSummaryUs, start, 1000000ull)) {
            C.summary += 2u; if (now - lastSummaryAt > C.maxGapUs) C.maxGapUs = now - lastSummaryAt; lastSummaryAt = now;
            L.lastSummaryCommit = L.commits; L.lastSummaryUs = now;
        }
        /* the legacy POLL, once per judged frame while a committed fence is watched */
        if (spent) { C.attempted++; if (n48_cm_cont_log_take(cont, &L.pollLines, N48_CM_CONT_LOG_POLL_LINES, &L.suppressed)) C.poll++; }
        if (spent >= nCommits || (frame % commitEvery) != 0u) continue;
        /* commit_try: GATE OK (one step early), the spend's commitFull, SPENT */
        C.attempted++;
        if (n48_cm_cont_log_commit(cont, L.fullCommits < N48_CM_CONT_LOG_FULL_COMMITS ? 1u : 0u, &L.suppressed)) C.routine++;
        spent++;
        if (cont) { L.commits++; L.commitFull = n48_cm_cont_log_take(1u, &L.fullCommits, N48_CM_CONT_LOG_FULL_COMMITS, nullptr); }
        C.attempted++; if (n48_cm_cont_log_commit(cont, L.commitFull, &L.suppressed)) C.routine++;
        if (cont && n48_cm_cont_summary_due(L.commits, L.lastSummaryCommit, 32u, now, L.lastSummaryUs, start, 1000000ull)) {
            C.summary += 2u; if (now - lastSummaryAt > C.maxGapUs) C.maxGapUs = now - lastSummaryAt; lastSummaryAt = now;
            L.lastSummaryCommit = L.commits; L.lastSummaryUs = now;
        }
        const bool refused = (spent % 5u) == 0u;
        /* the keystone: 3 verdict lines, NO-OP, 4 x 2 walk lines, THE ARM IS */
        for (int i = 0; i < 3; i++) { C.attempted++; if (sim_line(cont, L, refused)) (refused ? C.refusal : C.routine)++; }
        C.attempted++; if (sim_line(cont, L, false)) C.routine++;
        for (int i = 0; i < 8; i++) { C.attempted++; if (sim_line(cont, L, false)) C.routine++; }
        C.attempted++; if (sim_line(cont, L, refused)) (refused ? C.refusal : C.routine)++;
        if (refused) {   /* hook: NOT COMMITTED + COMMIT WITHDRAWN AT THE KEYSTONE */
            for (int i = 0; i < 2; i++) { C.attempted++; if (sim_line(cont, L, true)) C.refusal++; }
        } else {         /* hook: fence828 COMMITTED, TRANSLATED, RING EXEMPTION (SPARED) */
            for (int i = 0; i < 3; i++) { C.attempted++; if (sim_line(cont, L, false)) C.routine++; }
            /* hook_unmapVA: the flight's deferred withdrawal APPLYING */
            C.attempted++; if (n48_cm_cont_log_take(cont, &L.applyLines, N48_CM_CONT_LOG_APPLY_LINES, &L.suppressed)) C.apply++;
        }
    }
    if (cont && start + 60000000ull - lastSummaryAt > C.maxGapUs) C.maxGapUs = start + 60000000ull - lastSummaryAt;
    /* every attempted line is printed or counted */
    const uint64_t printed = C.routine + C.poll + C.apply + C.refusal;
    expect_u(cont ? "473 caps: every capped line is printed or counted (cont)" : "473 caps: every line printed (one-shot)",
             printed + L.suppressed + L.refusalSuppressed, C.attempted);
    return C;
}
static void cont_log_caps_checks()
{
    /* 600 commits in 60 s (10 judged frames/s, every frame commits). */
    const LogCount c = run_log_sim(1u, 600u, 100000ull, 1u);
    const uint64_t routinePerCommit = 2u /* GATE OK, SPENT */ + 3u + 1u + 8u + 1u + 3u;   /* = 18 */
    expect_u("473 caps: routine per-commit lines = the first 8 commits x 18 (minus the refused commits' own)",
             c.routine, 8u * routinePerCommit - 1u * (3u + 1u + 3u));   /* commit 5 of the first 8 is a refusal */
    expect_u("473 caps: fence828 POLL lines = its per-arm cap (64)", c.poll, (uint64_t)N48_CM_CONT_LOG_POLL_LINES);
    expect_u("473 caps: APPLYING lines = its per-arm cap (8)", c.apply, (uint64_t)N48_CM_CONT_LOG_APPLY_LINES);
    expect_u("473 caps: refusal lines past the full commits <= the shared cap + the first 8 commits' own",
             c.refusal <= (uint64_t)N48_CM_CONT_LOG_REFUSAL_LINES + 6u ? 1u : 0u, 1u);
    expect_u("473 caps: summary lines printed at most ~1/s (each of the two formats <= 100 in 60 s)",
             (c.summary / 2u) <= 100u ? 1u : 0u, 1u);
    expect_u("473 caps: summaries printed (each format >= 12, i.e. at least every 5 s)", (c.summary / 2u) >= 12u ? 1u : 0u, 1u);
    expect_u("473 caps: the longest gap between summaries <= 5 s", c.maxGapUs <= 5000000ull ? 1u : 0u, 1u);
    if (!gQuiet) std::printf("      473 caps over 600 commits / 60 s: routine %llu (18 formats, <= 8 each), POLL %llu, "
                             "APPLYING %llu, refusal %llu, summary %llu (both formats), longest summary gap %llu us\n",
                             (unsigned long long)c.routine, (unsigned long long)c.poll, (unsigned long long)c.apply,
                             (unsigned long long)c.refusal, (unsigned long long)c.summary, (unsigned long long)c.maxGapUs);
    expect_u("473 caps: no single line format prints more than 100 times in 60 s (routine <= 8 per format)",
             (c.routine <= 8u * routinePerCommit && c.poll <= 100u && c.apply <= 100u && c.refusal <= 100u) ? 1u : 0u, 1u);
    expect_u("473 caps: total printed (all classes + summaries) under 450 for 600 commits (0.0.472: ~11,000)",
             (c.routine + c.poll + c.apply + c.refusal + c.summary) < 450u ? 1u : 0u, 1u);
    /* THE SUMMARY ON THE CLOCK: a 60-s window with NO commit at all still prints about once a second. */
    {
        LogSim L {}; uint64_t last = 5000000ull, maxGap = 0ull; uint32_t sums = 0u;
        for (uint64_t now = 5000000ull; now < 65000000ull; now += 145000ull)
            if (n48_cm_cont_summary_due(0u, 0u, 0u, now, L.lastSummaryUs, 5000000ull, 1000000ull)) {
                sums++; if (now - last > maxGap) maxGap = now - last; last = now; L.lastSummaryUs = now;
            }
        expect_u("473 cadence: no commits for 60 s - a summary about every second (>= 55)", sums >= 55u ? 1u : 0u, 1u);
        expect_u("473 cadence:   longest gap <= 1.145 s", maxGap <= 1145000ull ? 1u : 0u, 1u);
        expect_u("473 cadence: not due before T started (baseline 0)", n48_cm_cont_summary_due(0u, 0u, 0u, 9000000ull, 0ull, 0ull, 1000000ull), 0u);
        expect_u("473 cadence: not due on a torn clock (now < baseline)", n48_cm_cont_summary_due(0u, 0u, 0u, 4000ull, 0ull, 5000ull, 1000000ull), 0u);
        expect_u("473 cadence: the count trigger alone (32 commits, no time)", n48_cm_cont_summary_due(32u, 0u, 32u, 1ull, 1ull, 1ull, 1000000ull), 1u);
        expect_u("473 cadence: count trigger OFF (0) never fires on count", n48_cm_cont_summary_due(999u, 0u, 0u, 2ull, 1ull, 1ull, 1000000ull), 0u);
    }
    /* OFF IDENTITY: `cont` 0 (a one-shot, DECIDE, or a continuous shot already DONE) prints every line. */
    {
        uint32_t pr = 0u; uint64_t sup = 0u;
        uint32_t ok = 1u;
        for (int i = 0; i < 1000; i++) if (!n48_cm_cont_log_take(0u, &pr, 4u, &sup)) ok = 0u;
        expect_u("473 caps: cont 0 - n48_cm_cont_log_take always prints and counts nothing", ok && pr == 0u && sup == 0u, 1u);
        expect_u("473 caps: cont 0 - n48_cm_cont_log_commit always prints", n48_cm_cont_log_commit(0u, 0u, &sup) && sup == 0u, 1u);
        const LogCount o = run_log_sim(0u, 600u, 100000ull, 1u);
        expect_u("473 caps: one-shot-shaped run (cont 0): no line is capped (POLL every frame after the first commit)",
                 o.poll, 599u);
    }
}


// =====================================================================================================================
// build 0.0.487 (an internal design note Q6 item 5, Q7, Q8 T5/T6; contract C3) — SWITCH 57, THE COMPUTE-N ELIDE.
//   T5  COMPUTE-ELIDE-R1: a frame whose candidate carries an elision (cs_elided > 0) commits ONLY with 42 ENFORCE
//       (md_switch) and R1 clean (md_ok); with cs_elided 0 the rung is absent (every md combination is the old gate).
//   P4  n48_cm_cs_is_n (the kext's `ex->cs_is_n`): 1 only for THIS frame's own compute program at exactly that VA, with
//       BufferClear_CS's key, class XLAT and our bytes; everything else 0.
//   the cselide57 line fits the log cap. (T6, the mid-arm guard, is cont_switch_refused_checks' list, 57 included.)
// =====================================================================================================================
static void cs_elide_gate_checks()
{
    n48_cm_frame c; uint32_t d = 0u;
    // OFF IDENTITY: cs_elided 0, every (md_switch, md_ok) - the gate answers what it answered without the rung.
    for (uint32_t m = 0; m < 4u; m++) {
        good_frame(c, 1040u, 3u);
        c.md_switch = m & 1u; c.md_ok = (m >> 1) & 1u; c.cs_elided = 0u;
        const uint32_t want = (c.md_switch && !c.md_ok) ? (uint32_t)N48_CM_MEMDST : (uint32_t)N48_CM_OK;
        char lbl[128]; std::snprintf(lbl, sizeof lbl, "0.0.487 T5: no elision, md %u/%u - the old gate", c.md_switch, c.md_ok);
        expect_u(lbl, gGate(&c, &d), want);
    }
    good_frame(c, 1040u, 3u); c.cs_elided = 2u; c.md_switch = 0u; c.md_ok = 0u; d = 0u;
    expect_u("0.0.487 T5: elided, 42 OFF/SHADOW - COMPUTE-ELIDE-R1", gGate(&c, &d), N48_CM_CS_ELIDE_R1);
    expect_u("0.0.487 T5:   its detail is the elided count", d, 2u);
    good_frame(c, 1040u, 3u); c.cs_elided = 1u; c.md_switch = 0u; c.md_ok = 1u;
    expect_u("0.0.487 T5: elided, md_ok 1 without ENFORCE - still COMPUTE-ELIDE-R1 (SHADOW judges, never admits)", gGate(&c, &d), N48_CM_CS_ELIDE_R1);
    good_frame(c, 1040u, 3u); c.cs_elided = 1u; c.md_switch = 1u; c.md_ok = 0u;
    expect_u("0.0.487 T5: elided, ENFORCE, R1 not clean - MEMORY-DESTINATION (its own rung first)", gGate(&c, &d), N48_CM_MEMDST);
    good_frame(c, 1040u, 3u); c.cs_elided = 1u; c.md_switch = 1u; c.md_ok = 1u;
    expect_u("0.0.487 T5: elided, ENFORCE, R1 clean - COMMIT", gGate(&c, &d), N48_CM_OK);
    // ORDER: a WORLD rung like MEMDST - after TOKEN/RING/CONT, before DEPENDENCY-STALE.
    good_frame(c, 1040u, 3u); c.cs_elided = 1u; c.token_ok = 0u;
    expect_u("0.0.487 T5: TOKEN still answers before COMPUTE-ELIDE-R1", gGate(&c, &d), N48_CM_TOKEN);
    good_frame(c, 1040u, 3u); c.cs_elided = 1u; c.dep_ok = 0u;
    expect_u("0.0.487 T5: COMPUTE-ELIDE-R1 answers before DEPENDENCY-STALE", gGate(&c, &d), N48_CM_CS_ELIDE_R1);
    good_frame(c, 1040u, 3u); c.cs_elided = 1u; c.md_switch = 1u; c.md_ok = 1u; c.dep_ok = 0u;
    expect_u("0.0.487 T5: an admitted elision still meets DEPENDENCY-STALE", gGate(&c, &d), N48_CM_DEP_STALE);
    // THE PRIMARY GATE (n48_cm_live), the same rule before the write.
    expect_u("0.0.487 live: elided under SHADOW - 0", n48_cm_live(N48_SD_ARM_COMMIT, N48_XV_TRANSLATE, 1u, 1u, 1u, 0u, 1u, 0u, 0u, 0u, 1u, 0u, 0u), 0u);
    expect_u("0.0.487 live: elided, ENFORCE, R1 clean - 1", n48_cm_live(N48_SD_ARM_COMMIT, N48_XV_TRANSLATE, 1u, 1u, 1u, 1u, 1u, 0u, 0u, 0u, 1u, 0u, 0u), 1u);
    expect_u("0.0.495 live: the same frame with heap_refuse (switch 62) - 0", n48_cm_live(N48_SD_ARM_COMMIT, N48_XV_TRANSLATE, 1u, 1u, 1u, 1u, 1u, 0u, 0u, 0u, 1u, 1u, 0u), 0u);
    expect_u("0.0.487 live: not elided under SHADOW - 1 (0.0.485's)", n48_cm_live(N48_SD_ARM_COMMIT, N48_XV_TRANSLATE, 1u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u), 1u);
}
static void cs_is_n_checks()
{
    static n48_xv_program pg[N48_XV_MAX_PGMS];
    static uint64_t va[N48_XV_MAX_PGMS];
    static uint32_t st[N48_XV_MAX_PGMS];
    const uint64_t kN = 0x400017a00ull;
    auto reset = [&]() {
        for (uint32_t k = 0; k < N48_XV_MAX_PGMS; k++) { pg[k] = n48_xv_program {}; va[k] = 0ull; st[k] = 0u; }
        // program 0: a fragment program, ours; program 1: N, as the gather records it on hardware (decide49's CENSUS
        // #274 key, m4c-r18's SUBSTITUTE entry, compute stage 6)
        pg[0].key = 0x1111222233334444ull; pg[0].key_class = N48_XV_PGM_KEY_XLAT; pg[0].bytes_are_ours = 1u; va[0] = 0x400020000ull; st[0] = 0u;
        pg[1].key = N48_CM_CS_N_KEY; pg[1].key_class = N48_XV_PGM_KEY_XLAT; pg[1].bytes_are_ours = 1u; va[1] = kN; st[1] = 6u;
    };
    reset();
    expect_u("0.0.487 P4: N at its VA, compute stage, key, XLAT, our bytes - 1", n48_cm_cs_is_n(pg, va, st, 2u, kN), 1u);
    expect_u("0.0.487 P4: another VA the frame holds (a fragment program) - 0", n48_cm_cs_is_n(pg, va, st, 2u, va[0]), 0u);
    expect_u("0.0.487 P4: a VA the frame never gathered - 0", n48_cm_cs_is_n(pg, va, st, 2u, 0x400017b00ull), 0u);
    expect_u("0.0.487 P4: VA 0 - 0", n48_cm_cs_is_n(pg, va, st, 2u, 0ull), 0u);
    expect_u("0.0.487 P4: N past npgm (not this frame's) - 0", n48_cm_cs_is_n(pg, va, st, 1u, kN), 0u);
    expect_u("0.0.487 P4: the gather's table overflow (npgm MAX + 1) - 0", n48_cm_cs_is_n(pg, va, st, N48_XV_MAX_PGMS + 1u, kN), 0u);
    reset(); st[1] = 0u;
    expect_u("0.0.487 P4: N's key at a fragment stage - 0", n48_cm_cs_is_n(pg, va, st, 2u, kN), 0u);
    reset(); pg[1].key = 0x9b226a39a78fe877ull;
    expect_u("0.0.487 P4: another compute key - 0", n48_cm_cs_is_n(pg, va, st, 2u, kN), 0u);
    reset(); pg[1].bytes_are_ours = 0u;
    expect_u("0.0.487 P4: Apple's own gfx10 N (not substituted) - 0", n48_cm_cs_is_n(pg, va, st, 2u, kN), 0u);
    reset(); pg[1].key_class = N48_XV_PGM_KEY_NO_XLAT;
    expect_u("0.0.487 P4: class NO_XLAT - 0", n48_cm_cs_is_n(pg, va, st, 2u, kN), 0u);
    reset(); pg[1].bytes_are_ours = 0u; pg[1].relocated = 1u;
    expect_u("0.0.487 P4: a relocated copy (Apple's bytes stay at the VA) - 0", n48_cm_cs_is_n(pg, va, st, 2u, kN), 0u);
    reset(); va[0] = kN;   // the FIRST entry at that VA decides (the gather dedupes VAs; a second never exists)
    expect_u("0.0.487 P4: the first entry at the VA is a fragment program - 0", n48_cm_cs_is_n(pg, va, st, 2u, kN), 0u);
    expect_u("0.0.487 P4: null inputs - 0", n48_cm_cs_is_n(nullptr, va, st, 2u, kN) | n48_cm_cs_is_n(pg, nullptr, st, 2u, kN) |
                                            n48_cm_cs_is_n(pg, va, nullptr, 2u, kN), 0u);
}
static void cselide_line_bound()
{
    char b[2048];
    const uint64_t M = 0xffffffffffffffffull;
    const int n = std::snprintf(b, sizeof b, N48_CM_CSELIDE_FMT, "OFF (default)", "`gfxneuter 57` REFUSED - a continuous arm stands",
                                " - 42 is not ENFORCE: an elided frame is refused at the gate",
                                (unsigned long long)M, (unsigned long long)M, (unsigned long long)M, (unsigned long long)M,
                                (unsigned long long)M, (unsigned long long)M, (unsigned long long)M, (unsigned long long)M,
                                (unsigned long long)M);
    if (!gQuiet) std::printf("      cselide57 line worst case: %d bytes (cap %u)\n", n, N48_LOG_CAP_BODY);
    expect_u("0.0.487: the cselide57 line fits under the log cap with every counter at 20 digits", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u);
    uint32_t k20 = 0u; for (const char *q = b; (q = std::strstr(q, "18446744073709551615")) != nullptr; q += 20) k20++;
    expect_u("0.0.487: ... all nine cselide57 counters on it", k20, 9u);
}

// =====================================================================================================================
// build 0.0.500 (an internal design note Q4 T5/T6) — SWITCH 66, THE DRAW ELIDE.
//   T5  DRAW-ELIDE-R1: a frame whose candidate carries an elided draw (draw_elided > 0) commits ONLY with 42 ENFORCE and R1
//       clean; with draw_elided 0 the rung is absent (every md combination is the old gate); n48_cm_live carries it.
//   T6  the verb's values (n48_cm_de_set: 1 -> U/Y, 3 -> U/Y+AO, 2 -> OFF, others refused) and the mid-arm guard (66 is in
//       cont_switch_refused_checks' guarded list and pinned in the source); the kext backstop; the report line fits.
// =====================================================================================================================
static void de_gate_checks()
{
    n48_cm_frame c; uint32_t d = 0u;
    for (uint32_t m = 0; m < 4u; m++) {
        good_frame(c, 1040u, 3u);
        c.md_switch = m & 1u; c.md_ok = (m >> 1) & 1u; c.draw_elided = 0u;
        const uint32_t want = (c.md_switch && !c.md_ok) ? (uint32_t)N48_CM_MEMDST : (uint32_t)N48_CM_OK;
        char lbl[128]; std::snprintf(lbl, sizeof lbl, "0.0.500 T5: no elided draw, md %u/%u - the old gate", c.md_switch, c.md_ok);
        expect_u(lbl, gGate(&c, &d), want);
    }
    good_frame(c, 1040u, 3u); c.draw_elided = 2u; c.md_switch = 0u; c.md_ok = 0u; d = 0u;
    expect_u("0.0.500 T5: elided draws, 42 OFF/SHADOW - DRAW-ELIDE-R1", gGate(&c, &d), N48_CM_DRAW_ELIDE_R1);
    expect_u("0.0.500 T5:   its detail is the elided count", d, 2u);
    good_frame(c, 1040u, 3u); c.draw_elided = 1u; c.md_switch = 0u; c.md_ok = 1u;
    expect_u("0.0.500 T5: elided, md_ok 1 without ENFORCE (SHADOW) - still DRAW-ELIDE-R1", gGate(&c, &d), N48_CM_DRAW_ELIDE_R1);
    good_frame(c, 1040u, 3u); c.draw_elided = 1u; c.md_switch = 1u; c.md_ok = 0u;
    expect_u("0.0.500 T5: elided, ENFORCE, R1 not clean - MEMORY-DESTINATION first", gGate(&c, &d), N48_CM_MEMDST);
    good_frame(c, 1040u, 3u); c.draw_elided = 1u; c.md_switch = 1u; c.md_ok = 1u;
    expect_u("0.0.500 T5: elided, ENFORCE, R1 clean - COMMIT", gGate(&c, &d), N48_CM_OK);
    good_frame(c, 1040u, 3u); c.draw_elided = 1u; c.cs_elided = 1u;
    expect_u("0.0.500 T5: with a compute elision too, COMPUTE-ELIDE-R1 answers first", gGate(&c, &d), N48_CM_CS_ELIDE_R1);
    good_frame(c, 1040u, 3u); c.draw_elided = 1u; c.dep_ok = 0u;
    expect_u("0.0.500 T5: DRAW-ELIDE-R1 answers before DEPENDENCY-STALE", gGate(&c, &d), N48_CM_DRAW_ELIDE_R1);
    good_frame(c, 1040u, 3u); c.draw_elided = 1u; c.token_ok = 0u;
    expect_u("0.0.500 T5: TOKEN still answers before DRAW-ELIDE-R1", gGate(&c, &d), N48_CM_TOKEN);
    expect_u("0.0.500 live: elided draws under SHADOW - 0",
             n48_cm_live(N48_SD_ARM_COMMIT, N48_XV_TRANSLATE, 1u, 1u, 1u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 1u), 0u);
    expect_u("0.0.500 live: elided draws, ENFORCE, R1 clean - 1",
             n48_cm_live(N48_SD_ARM_COMMIT, N48_XV_TRANSLATE, 1u, 1u, 1u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 1u), 1u);
    expect_u("0.0.500 live: none elided under SHADOW - 1 (0.0.499's)",
             n48_cm_live(N48_SD_ARM_COMMIT, N48_XV_TRANSLATE, 1u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u), 1u);
    // T6: the verb's values
    uint32_t r = 0xAAu;
    expect_u("0.0.500 T6: M 1 sets the U/Y class (1)", n48_cm_de_set(1u, &r) * 10u + r, 11u);
    expect_u("0.0.500 T6: M 3 sets U/Y and AO (3)", n48_cm_de_set(3u, &r) * 10u + r, 13u);
    expect_u("0.0.500 T6: M 2 is OFF (0)", n48_cm_de_set(2u, &r) * 10u + r, 10u);
    r = 0xAAu;
    expect_u("0.0.500 T6: M 0 (a read), 4, 255 change nothing", n48_cm_de_set(0u, &r) + n48_cm_de_set(4u, &r) + n48_cm_de_set(255u, &r) + (r == 0xAAu ? 0u : 9u), 0u);
    // build 0.0.512: M 7 (= 1858) = U/Y, AO and the glass class (mask 7, xlat12's XLAT12_DE_CLASS_GLASS = 4); 5 and 6 refused
    expect_u("0.0.512 T6: M 7 sets U/Y, AO and GLASS (7)", n48_cm_de_set(7u, &r) * 10u + r, 17u);
    r = 0xAAu;
    expect_u("0.0.512 T6: M 5 and 6 change nothing", n48_cm_de_set(5u, &r) + n48_cm_de_set(6u, &r) + (r == 0xAAu ? 0u : 9u), 0u);
    expect_u("0.0.512 T6: the glass bit is xlat12's XLAT12_DE_CLASS_GLASS (4): mask 1|2|4", n48_cm_de_rows_of(7u) * 10u + n48_cm_de_rows_of(3u), 73u);
    expect_u("0.0.512 T6: 66 | 7 << 8 is 1858", 66u | 7u << 8, 1858u);
    expect_s("0.0.512: the mask names", (std::string(n48_cm_de_mask_name(7u)) + "|" + n48_cm_de_mask_name(3u) + "|" + n48_cm_de_mask_name(1u) + "|" + n48_cm_de_mask_name(0u)).c_str(),
             "ON U/Y+AO+glass|ON U/Y+AO|ON U/Y|OFF (default)");
    expect_u("0.0.500 T6: the class mask is xlat12's (UY 1, AO 2)", n48_cm_de_rows_of(1u) * 10u + n48_cm_de_rows_of(3u), 13u);
    expect_u("0.0.500 T6: 66 is guarded; a read is not refused while armed",
             n48_cm_cont_switch_guarded(66u) * 10u + n48_cm_cont_switch_refused(66u, 1u, 1u, N48_CM_SHOT_ARMED), 10u);
    expect_u("0.0.500 T6: a change is refused while a continuous arm stands",
             n48_cm_cont_switch_refused(66u, 0u, 1u, N48_CM_SHOT_ARMED), 1u);
    // the kext's backstop
    expect_u("0.0.500 backstop: 2 draws, 2 elided, 0 left - admitted", n48_cm_de_backstop(0u, 2u, 2u), 0u);
    expect_u("0.0.500 backstop: 2 draws, 2 elided, but 1 still in the output (counted, copied) - refused", n48_cm_de_backstop(1u, 2u, 2u), 1u);
    expect_u("0.0.500 backstop: 2 draws, 1 elided, 2 in the output - refused", n48_cm_de_backstop(2u, 2u, 1u), 1u);
    expect_u("0.0.500 backstop: more elided than draws - refused", n48_cm_de_backstop(0u, 1u, 2u), 1u);
    expect_u("0.0.500 backstop: nothing elided, all draws kept - admitted", n48_cm_de_backstop(5u, 5u, 0u), 0u);
    expect_s("0.0.500: the row names", (std::string(n48_cm_de_row_name(1u)) + n48_cm_de_row_name(2u) + n48_cm_de_row_name(3u) + n48_cm_de_row_name(0u)).c_str(), "UYAO?");
    // build 0.0.552: row 6 is AN (xlat12's XLAT12_DE_ROW_AN, switch 110); 7 is the first unknown row.
    expect_s("0.0.512: the glass row names (xlat12's XLAT12_DE_ROW_BD 4 / _BA 5); 0.0.552: AN 6, 7 unknown", (std::string(n48_cm_de_row_name(4u)) + n48_cm_de_row_name(5u) + n48_cm_de_row_name(6u) + n48_cm_de_row_name(7u)).c_str(), "BDBAAN?");
}
static void drawelide_line_bound()
{
    char b[2048];
    const uint64_t M = 0xffffffffffffffffull;
    const int n = std::snprintf(b, sizeof b, N48_CM_DRAWELIDE_FMT, "ON U/Y+AO+glass", " - `gfxneuter 66` REFUSED: a continuous arm stands, unchanged",
                                " - 42 not ENFORCE: elided frames refused",
                                (unsigned long long)M, (unsigned long long)M, (unsigned long long)M, (unsigned long long)M,
                                (unsigned long long)M, (unsigned long long)M, (unsigned long long)M, (unsigned long long)M,
                                (unsigned long long)M, (unsigned long long)M, (unsigned long long)M);
    if (!gQuiet) std::printf("      drawelide66 line worst case: %d bytes (cap %u)\n", n, N48_LOG_CAP_BODY);
    expect_u("0.0.500: the drawelide66 line fits under the log cap with every counter at 20 digits", n > 0 && (unsigned)n <= N48_LOG_CAP_BODY, 1u);
    uint32_t k20 = 0u; for (const char *q = b; (q = std::strstr(q, "18446744073709551615")) != nullptr; q += 20) k20++;
    expect_u("0.0.500: ... all eleven drawelide66 counters on it", k20, 11u);
    const int nc = std::snprintf(b, sizeof b, N48_CM_DRAWELIDE_CENSUS_FMT,
                                 0xffffffffu, 0xffffffffu, (unsigned long long)M, 0xffffffffu, 0xffffffffu, (unsigned long long)M,
                                 0xffffffffu, 0xffffffffu, (unsigned long long)M, 0xffffffffu, 0xffffffffu, (unsigned long long)M);
    expect_u("0.0.500: the census line fits under the log cap", nc > 0 && (unsigned)nc <= N48_LOG_CAP_BODY, 1u);
    k20 = 0u; for (const char *q = b; (q = std::strstr(q, "18446744073709551615")) != nullptr; q += 20) k20++;
    expect_u("0.0.500: ... with its four counts", k20, 4u);
    const int n1 = std::snprintf(b, sizeof b, N48_CM_DRAWELIDE_ONE_FMT, (unsigned long long)M, 0xffffffffu, 0xffffffffu, "AO",
                                 (unsigned long long)M, 0xffffffffu, "segment refused later");
    expect_u("0.0.500: the per-elision line fits under the log cap", n1 > 0 && (unsigned)n1 <= N48_LOG_CAP_BODY, 1u);
}

static void all_checks()
{
    rehearsal_line_bound();
    mutant_every_dword(1040u);
    mutant_every_dword(1472u);
    every_other_refusal();
    token_checks();
    mib_gate_checks();
    headless_checks();
    kind_name_checks();
    arm_gate_checks();
    one_shot_checks();
    lut_checks();
    fillset_checks();
    ring_full_checks();
    mib_required_checks();
    commit_try_spend_reachability();
    retire_checks();
    planeset_checks();
    cont_no_fence_checks();
    cont_arm_missing_checks();
    cont_fence_late_checks();
    cont_switch_refused_checks();
    arm_enforce_hl_checks();   // build 0.0.490 item 1
    cont_stop_report_checks();
    cont_summary_baseline_checks();
    cont_shot_checks();
    commit_order_m1_checks();
    decide_frame_top_reachability();
    // build 0.0.473 — the longer window.
    sw41_decode_checks();
    cont_t60_stop_checks();
    cont_done_clears_cont_checks();
    cont_log_caps_checks();
    // build 0.0.487 — switch 57.
    cs_elide_gate_checks();
    cs_is_n_checks();
    cselide_line_bound();
    de_gate_checks();          // build 0.0.500
    drawelide_line_bound();    // build 0.0.500
}

// ---------------------------------------------------------------------------------------------------------------------
// The planted defects. Each is a plausible wrong gate; each must be caught.
// ---------------------------------------------------------------------------------------------------------------------
/* M1: the read-back result is ignored — the exact defect the specification names. */
static uint32_t m1(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; k.mismatch = 0u; return n48_cm_gate(&k, d);
}
/* M2: target_vram's own fail-open shape, transplanted — a page that did not resolve sets no flag and so does not refuse. */
static uint32_t m2(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; if (k.sys_pages != k.pages) k.sys_pages = k.pages; return n48_cm_gate(&k, d);
}
/* M3: `pages` is not required to be non-zero, so an un-walked range passes by 0 == 0. */
static uint32_t m3(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; if (!k.pages) { k.pages = 1u; k.sys_pages = 1u; k.back_sys_pages = 1u; } return n48_cm_gate(&k, d);
}
/* M4: the multi-IB rung is missing — IBs 1..3 would reach the CP untranslated. */
static uint32_t m4(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; if (k.nib != 1u) k.nib = 1u; return n48_cm_gate(&k, d);
}
/* M5: coverage is not required to tile — only that at least one segment translated. */
static uint32_t m5(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c;
    uint32_t at = 0;
    for (uint32_t i = 0; i < k.nseg && i < N48_XV_MAX_SEGS; i++) {
        k.seg[i].head = at; k.seg[i].start = at + 2u;
        if (k.seg[i].end <= k.seg[i].start || k.seg[i].end > k.n)
            k.seg[i].end = (i + 1u == k.nseg) ? k.n : at + 2u + k.seg[i].out_len;
        k.seg[i].out_len = k.seg[i].end - k.seg[i].start;
        at = k.seg[i].end;
    }
    k.n = at ? at : k.n;
    return n48_cm_gate(&k, d);
}
/* M6: the frame identity is not re-established at the hook. */
static uint32_t m6(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; k.token_ok = 1u; return n48_cm_gate(&k, d);
}
/* M7: a short write is treated as "wrote something", the classic >= instead of ==. */
static uint32_t m7(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; if (k.wrote) k.wrote = k.n; return n48_cm_gate(&k, d);
}

/* 0.0.369 (notes 804) - THE SEGMENT-KIND DEFECTS. Each is a plausible way to get the new rung wrong, and each is exactly one
 * of the five the safety review named. */
/* M8: seg_kind is not required - a frame that never said which recogniser found its segments is treated as ENCODER. This is
 *     the fail-open shape the whole "0 refuses" construction exists to prevent. */
static uint32_t m8(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; if (!k.seg_kind) k.seg_kind = N48_CM_KIND_ENCODER; return n48_cm_gate(&k, d);
}
/* M9: the per-segment start rule is not enforced - every segment is coerced to its declared kind's shape, so a MIXED frame
 *     (and a headless segment whose start is not its head) is accepted. */
static uint32_t m9(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c;
    for (uint32_t i = 0; i < k.nseg && i < N48_XV_MAX_SEGS; i++) {
        const uint32_t want = k.seg_kind == N48_CM_KIND_HEADLESS ? k.seg[i].head : k.seg[i].head + 2u;
        if (k.seg[i].start != want && k.seg[i].end > want) { k.seg[i].start = want; k.seg[i].out_len = k.seg[i].end - want; }
    }
    return n48_cm_gate(&k, d);
}
/* M10: HEADLESS does not require that xlat12_ib_segments found NOTHING on the same buffer - the fallback becomes an
 *      alternative, and a stream both recognisers accept gets whichever answer was asked for second. */
static uint32_t m10(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; k.hl_ib_segments = 0u; return n48_cm_gate(&k, d);
}
/* M11: HEADLESS does not require `total == nseg` - passes the table could not hold are silently dropped, and the dwords
 *      they covered would reach the CP untranslated. */
static uint32_t m11(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; if (k.seg_kind == N48_CM_KIND_HEADLESS) k.hl_total = k.nseg; return n48_cm_gate(&k, d);
}
/* M12: HEADLESS does not require the recogniser's own report to have been OK - a non-zero return with a refusal in the
 *      report is read as an acceptance. */
static uint32_t m12(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; k.hl_ok = 1u; return n48_cm_gate(&k, d);
}

/* 0.0.370 (notes 807) - THE RECORD DEFECTS. Neither changes a decision; both make a RUN say the wrong thing about which
 * recogniser carried a frame, which is the whole reason the name is printed. */
/* M13: UNSET is labelled ENCODER - the fail-open READING of a frame that declared no kind, put back into the log after the
 *      gate took it out. A run would then read "ENCODER" on every frame whose policy pass never ran. */
static const char *m13_kind(uint32_t k)
{
    return k == N48_CM_KIND_HEADLESS ? "HEADLESS" : "ENCODER";
}
/* M14: HEADLESS is labelled ENCODER - the hp9 question answered backwards: the three frames the recogniser actually carried
 *      would be recorded as Apple's encoder shape. */
static const char *m14_kind(uint32_t k)
{
    return k == N48_CM_KIND_UNSET ? "UNSET" : "ENCODER";
}

/* 0.0.371 (notes 809) - THE ARM'S OWN MUTANTS. 808's phantom lever is the reason these exist: the arm is the one lever
 * every standing rule guards, and a lever whose conditions live in an `if` is reviewed once and then drifts. */
/* M15: ARMING WITH A CONDITION MISSING - the real gate with N1's rung deleted. This is the shape of every "we were in a
 *      hurry" arm: 806 proved N1 is exactly what moved three frames to TRANSLATE, so arming without it is arming into a
 *      ladder that refuses every frame at target-in-vram - and, worse, it is arming on an unchecked world. */
static uint32_t m15_arm(const n48_cm_arm_req *a)
{
    if (!a) return N48_CM_ARM_ALL;
    n48_cm_arm_req b = *a;
    b.n1_on = 1u;
    return n48_cm_arm_missing(&b) & ~(uint32_t)N48_CM_ARM_N1;
}
/* M16: THE GATE ACCEPTS A FRAME WHILE NOT ARMED - the first rung deleted. In 0.0.352-0.0.370 that rung was the ONLY thing
 *      standing between a clean frame and the CP (808), so it is the single most load-bearing line in this header. */
static uint32_t m16(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return N48_CM_NOT_ARMED;
    n48_cm_frame k = *c; k.arm = N48_SD_ARM_COMMIT; return n48_cm_gate(&k, d);
}
/* M17: THE ONE-SHOT NEVER DISARMS ITSELF - `finish` is a no-op, so the arm level would be left at COMMIT after the one
 *      committed frame and the NEXT clean frame would commit too. The blast radius stops being one frame. */
static uint32_t m17_finish(n48_cm_shot *sh) { (void)sh; return 0u; }
/* M18: THE DISARM DOES NOT WORK - `4 | 2 << 8` reports success and leaves the one-shot armed. The operator's only way
 *      back short of a reboot silently does nothing. */
static uint32_t m18_disarm(n48_cm_shot *sh) { (void)sh; return 1u; }
/* M19: THE ARM SURVIVES A WINDOWSERVER REBIND - the identity clause dropped from the level. The arm was granted against
 *      one compositor's world and would then fire into another's. */
static uint32_t m19_level(const n48_cm_shot *sh, uint32_t arm, const n48_cm_ws_mark *now)
{
    (void)now;
    if (arm == N48_SD_ARM_OFF) return N48_SD_ARM_OFF;
    if (arm != N48_SD_ARM_COMMIT) return arm;
    if (!sh || sh->state != N48_CM_SHOT_ARMED) return N48_SD_ARM_DECIDE;
    return N48_SD_ARM_COMMIT;
}

/* ---- 0.0.381 (notes 853): THE BUDGET'S OWN PLANTED DEFECTS ------------------------------------------------------
 * M20: THE BUDGET IS CHARGED PER CALL, NOT PER TOKEN SEQ - the dedupe key dropped. This is the defect the split exists
 *      for: above budget 1 the state is still ARMED after a spend, so a second call for the SAME frame halves the
 *      budget silently, and the operator's N is not the N that runs. */
static uint32_t m20_spend(n48_cm_shot *sh, uint32_t seq)
{
    if (!sh || sh->state != N48_CM_SHOT_ARMED) return 0u;
    sh->last_seq = seq;
    sh->spent++;
    if (sh->spent >= n48_cm_shot_budget_of(sh)) { sh->state = N48_CM_SHOT_SPENT; sh->spent_seq = seq; }
    return 1u;
}
/* M21: THE ARM SURVIVES ITS BOUND - the exhaustion test deleted, so the shot never leaves ARMED and EVERY later clean
 *      frame commits. The bound stops being a bound: this is the commit-side of "a grant that survives its bound". */
static uint32_t m21_spend(n48_cm_shot *sh, uint32_t seq)
{
    if (!sh || sh->state != N48_CM_SHOT_ARMED) return 0u;
    if (sh->spent != 0u && seq == sh->last_seq) return 0u;
    sh->last_seq = seq;
    sh->spent++;
    return 1u;
}
/* M22: THE DEFAULT IS NOT TODAY - a budget of 0 is read as UNLIMITED instead of 1, so every caller that did not ask for
 *      a budget (which is every caller before 853, and the boot value) silently gets an arm that never spends. */
static uint32_t m22_spend(n48_cm_shot *sh, uint32_t seq)
{
    if (!sh || sh->state != N48_CM_SHOT_ARMED) return 0u;
    if (sh->spent != 0u && seq == sh->last_seq) return 0u;
    sh->last_seq = seq;
    sh->spent++;
    if (sh->budget != 0u && sh->spent >= sh->budget) { sh->state = N48_CM_SHOT_SPENT; sh->spent_seq = seq; }
    return 1u;
}
/* M23: A DISARM LEAVES THE BUDGET STANDING - `spent` is not cleared, so the NEXT arm starts part-spent (or, with M21's
 *      shape, a stale raised budget outlives the arm that asked for it). */
static uint32_t m23_disarm(n48_cm_shot *sh)
{
    if (!sh) return 0u;
    const uint32_t was = sh->state;
    sh->state = N48_CM_SHOT_OFF;
    sh->spent_seq = 0u;
    return was == N48_CM_SHOT_ARMED || was == N48_CM_SHOT_SPENT ? 1u : 0u;
}

/* ---- 0.0.401 (a item 3,b): THE IDENTITY LUT RUNG'S OWN PLANTED DEFECTS --------------------------------
 * M24: THE RUNG IS DELETED - a plane candidate commits while the identity LUT has never been written. This is the exact
 *      null the rung exists to prevent: the run would score cause (A) against a LUT that is still all zero. */
static uint32_t m24(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; if (k.lut_switch && k.lut_plane) k.lut_ready = 1u; return n48_cm_gate(&k, d);
}
/* M25: THE RUNG IGNORES THE SWITCH - with switch 32 OFF it still refuses plane candidates, so the default path is no
 *      longer 0.0.400's and an ordinary boot loses frames it used to commit. The byte-identity clause, broken. */
static uint32_t m25(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; if (k.lut_plane && !k.lut_ready) k.lut_switch = 1u; return n48_cm_gate(&k, d);
}
/* M26: THE RUNG FIRES ON THE FILL - it ignores `lut_plane`, so the one-shot's FIRST commit (the fill, which does not
 *      sample the LUT) is refused and NO frame ever commits on an armed run: a null run caused by the guard itself. */
static uint32_t m26(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; if (k.lut_switch && !k.lut_ready) k.lut_plane = 1u; return n48_cm_gate(&k, d);
}

/* ---- 0.0.404 (a item 2,b): THE FILL-SET RESERVATION RUNG'S OWN PLANTED DEFECTS -------------------------
 * M27: THE RUNG IS DELETED — a non-fill commits while the window is open. This is the exact null the reservation exists to
 *      prevent: the second shot is spent on the plane before the twin fill, and on a twin-shaped boot nothing ever paints
 *      the surface the presenter reads.
 * M28: THE RUNG IGNORES THE SWITCH — with switch 33 OFF it still refuses non-fills, so the default path is no longer
 *      0.0.403's and an ordinary boot loses frames it used to commit. The byte-identity clause, broken.
 * M29: THE RUNG IGNORES THE WINDOW — it fires even after both members have committed, so the plane the reservation exists
 *      to let through is refused forever. The vacuous-other-way shape. */
static uint32_t m27(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; if (k.fs_switch && k.fs_open) k.fs_reserve = 1u; return n48_cm_gate(&k, d);
}
static uint32_t m28(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; if (k.fs_open && !k.fs_reserve) k.fs_switch = 1u; return n48_cm_gate(&k, d);
}
static uint32_t m29(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; if (k.fs_switch && !k.fs_reserve) k.fs_open = 1u; return n48_cm_gate(&k, d);
}

/* ---- 0.0.420 (STEP10-PLAN P1): THE SECOND WINDOW RUNG'S OWN PLANTED DEFECTS ---------------------------------------
 * M30: THE RUNG IS DELETED — a non-plane spends the shot the second window held for the plane. This is the exact null P1
 *      exists to prevent (arm29/: the "second pair" took the shots and no plane frame committed).
 * M31: THE RUNG IGNORES THE SWITCH — with switch 35 OFF it still refuses non-planes, so the default path is no longer
 *      0.0.419's. The byte-identity clause, broken.
 * M32: THE RUNG IGNORES THE WINDOW — it fires even after the plane committed or the window expired, so today's rule is
 *      never restored. The vacuous-other-way shape. */
static uint32_t m30(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; k.fp_reserve = 1u; return n48_cm_gate(&k, d);
}
static uint32_t m31(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; if (k.fp_open && !k.fp_reserve) k.fp_switch = 1u; return n48_cm_gate(&k, d);
}
static uint32_t m32(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; if (k.fp_switch && !k.fp_reserve) k.fp_open = 1u; return n48_cm_gate(&k, d);
}

/* ---- 0.0.426 (MIB-COMMIT B5): THE PER-IB RUNG'S OWN PLANTED DEFECTS ------------------------------------------------
 * M33: IB 0's EVIDENCE IS USED FOR EVERY IB ("entry 0 only") - the per-IB loop runs but reads the first IB's pages,
 *      write, read-back and mismatch for all of them, so a fault in IB 1..3 is invisible and the whole frame commits.
 *      This is exactly T5's mutant "entry 0 only".
 * M34: THE MULTI_IB RUNG IGNORES THE SWITCH - its `!c->mib` clause is deleted, so a nib > 1 frame with the switch OFF is
 *      no longer refused by name; it falls through to LEN on the zeroed ib_n and the refusal reason a run reads is wrong
 *      (and a caller that happened to fill ib_n would be judged as if the switch were on). */
static uint32_t m33(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c;
    if (k.mib) for (uint32_t j = 1u; j < k.nib && j < N48_XV_MAX_IBS; j++) {
        k.pages_ib[j] = k.pages_ib[0]; k.sys_pages_ib[j] = k.sys_pages_ib[0];
        k.wrote_ib[j] = k.wrote_ib[0]; k.got_ib[j] = k.got_ib[0];
        k.back_sys_pages_ib[j] = k.back_sys_pages_ib[0]; k.mismatch_ib[j] = k.mismatch_ib[0];
    }
    return n48_cm_gate(&k, d);
}
static uint32_t m34(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c;
    if (k.nib != 1u) k.mib = 1u;   // the `!c->mib ||` clause of MULTI_IB deleted
    return n48_cm_gate(&k, d);
}

/* ---- build 0.0.456 item 1: THE MIB-REQUIRED RUNG'S OWN PLANTED DEFECTS -------------------
 * M35: THE RUNG IS DELETED - `mib_req_switch` is read and then ignored, so a single-IB frame commits even with switch
 *      53 ON. This is the exact null the rung exists to prevent: the one armed shot could then land on a one-head
 *      frame. Simulated by clearing the dedicated switch field before the real gate ever sees it, which defeats only
 *      THIS rung (the field is read nowhere else) and leaves every other rung's answer for the frame untouched.
 * M36: THE CHECK IS INVERTED - it refuses exactly the frames it should admit (mib with nib >= 2) and admits exactly
 *      the frames it should refuse (mib 0, or nib < 2), whenever the switch is on. Run against the real gate with
 *      the real rung disabled first (so only the inverted logic below can fire, and only when nothing earlier already
 *      refused), this is caught by every ON-direction check in mib_required_checks: the single-IB and mib/nib-1
 *      cases that must refuse instead commit, and the genuine two-IB case that must commit is instead refused. */
static uint32_t m35(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; k.mib_req_switch = 0u; return n48_cm_gate(&k, d);
}
/* build 0.0.487 — M37: the COMPUTE-ELIDE-R1 rung is DELETED (an elided frame commits under SHADOW). */
static uint32_t m37(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; k.cs_elided = 0u; return n48_cm_gate(&k, d);
}
/* build 0.0.500 — M39: the DRAW-ELIDE-R1 rung is DELETED (an elided draw commits under SHADOW). */
static uint32_t m39(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c; k.draw_elided = 0u; return n48_cm_gate(&k, d);
}
/* M40: DRAW-ELIDE-R1 asks md_ok ALONE. */
static uint32_t m40(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c;
    if (k.draw_elided && k.md_ok && !k.md_switch) k.draw_elided = 0u;
    return n48_cm_gate(&k, d);
}
/* M38: the rung asks md_ok ALONE (SHADOW's clean answer admits an elision). */
static uint32_t m38(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c;
    if (k.cs_elided && k.md_ok && !k.md_switch) k.cs_elided = 0u;
    return n48_cm_gate(&k, d);
}
static uint32_t m36(const n48_cm_frame *c, uint32_t *d)
{
    if (!c) return n48_cm_gate(c, d);
    n48_cm_frame k = *c;
    const uint32_t on = k.mib_req_switch;
    k.mib_req_switch = 0u;                          // the real rung disabled; only the inverted logic below may fire
    const uint32_t r = n48_cm_gate(&k, d);
    if (r != N48_CM_OK) return r;                    // an earlier rung already refused - the inverted rung cannot hide it
    if (on && k.mib && k.nib >= 2u) { if (d) *d = k.nib; return N48_CM_MIB_REQUIRED; }
    return N48_CM_OK;
}

/* =====================================================================================================================
 * C4/C5 (0.0.461 review) — SOURCE PINS ON AppleHardwareHook.cpp, PAIRED WITH THE REAL FUNCTIONS ABOVE (the same
 * combination gfx_keystone_test.cpp's own PIN tests use: a pin proves WHICH variable/condition the kext wires to
 * a real function; cont_shot_checks/cont_no_fence_checks above already prove what that function DOES with it -
 * "literal pins alone are not accepted", so neither stands without the other here).
 *   C4: n48_cm_shot_cont_start(&gXdShot, gXdShotSpentUs, gXdBuild.plane) with the plane argument FORCED TO 1 (T
 *       starts at any commit). cont_shot_checks already proves the function itself only starts T when its OWN
 *       `is_plane` argument is 1 - what this pin proves is that the ARGUMENT is `gXdBuild.plane` (the policy's
 *       real per-frame reading), not a literal that can never be 0.
 *   C5: `gfxneuter 3`'s refusal while a continuous arm stands, removed. Pins the guard's own condition text AND
 *       that it precedes the buffer-allocation/gXdArm-write block it exists to gate.
 * ===================================================================================================================== */
static char *pin_slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)std::malloc((size_t)n + 1);
    if (!b) { fclose(f); return nullptr; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { std::free(b); fclose(f); return nullptr; }
    b[n] = 0; fclose(f);
    return b;
}
static uint32_t pin_count(const char *hay, const char *needle)
{
    uint32_t c = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != nullptr; p += strlen(needle)) c++;
    return c;
}
static void source_pins(const char *path)
{
    char *src = pin_slurp(path);
    if (!src) { std::printf("  SKIP source pins: cannot read %s\n", path); gFail++; gRun++; return; }
    // C4: the real call, with the real argument.
    const char *contStartCall = "n48_cm_shot_cont_start(&gXdShot, gXdShotSpentUs, gXdBuild.plane)";
    expect_u("PIN C4: the continuous-start call uses gXdBuild.plane (the REAL per-frame plane reading), exactly once",
             pin_count(src, contStartCall), 1u);
    // Non-vacuity: the forced-to-1 shape the review actually planted must NOT be present either.
    expect_u("PIN C4: the forced-to-1 shape (the planted break) is ABSENT",
             pin_count(src, "n48_cm_shot_cont_start(&gXdShot, gXdShotSpentUs, 1u)"), 0u);
    // C5: the guard and its placement, BEFORE the buffer allocation / gXdArm write it exists to gate.
    const char *contGuard = "else if (gXdShot.cont && gXdShot.state == N48_CM_SHOT_ARMED) { st = 5; st3ContRefused = true; }";
    const char *decideWrite = "gXdArm = N48_SD_ARM_DECIDE;";
    expect_u("PIN C5: `gfxneuter 3`'s continuous-arm guard is present, exactly once", pin_count(src, contGuard), 1u);
    const char *g = strstr(src, contGuard);
    const char *d = g ? strstr(g, decideWrite) : nullptr;
    expect_u("PIN C5: ...and it precedes the DECIDE write it exists to gate", (g && d) ? 1u : 0u, 1u);
    // C3 (the wiring half): the kext's ONE call site passes gKsDeferOn's own reading as the first argument - since
    // the six-`if` chain is gone (replaced by n48_cm_cont_arm_missing, above), the only way this could now be
    // wrong is the WRONG boolean (or a literal) reaching the pure function's first parameter.
    expect_u("PIN C3: the kext's call passes gKsDeferOn's own reading, exactly once",
             pin_count(src, "n48_cm_cont_arm_missing(gKsDeferOn ? 1u : 0u,"), 1u);
    // F3: the judged-frame-top stop sequence is BEFORE n48_cm_shot_level, and the re-check is BEFORE commit_try.
    const char *topStopSeq = "THE JUDGED-FRAME-TOP\n    // CONTINUOUS STOP SEQUENCE, MOVED BEFORE n48_cm_shot_level.";
    const char *levelCall = "const uint32_t arm = n48_cm_shot_level(&gXdShot, gXdArm, &wsNow);";
    expect_u("PIN F3: the stop sequence precedes n48_cm_shot_level", strstr(src, topStopSeq) && strstr(src, levelCall) &&
             strstr(src, topStopSeq) < strstr(src, levelCall), 1u);
    const char *recheckVar = "bool contStoppedNow = false;";
    const char *commitTryCall = "gfxsrc_commit_try(vm, info, ib0Va, f.ib[0].got, f.nib, arm, verdict, stampNow, tgtVa)";
    expect_u("PIN F3: the re-check precedes gfxsrc_commit_try", strstr(src, recheckVar) && strstr(src, commitTryCall) &&
             strstr(src, recheckVar) < strstr(src, commitTryCall), 1u);
    expect_u("PIN F3: commit_try's own call is gated on !contStoppedNow, exactly once",
             pin_count(src, "f.reader_ok && !contStoppedNow)"), 1u);
    // F4: the frame-cap stop was gated on gXdShot.cont. build 0.0.517: the judge has no frame cap, so the
    // stop is GONE (gfx_judge_unbounded_test pins the mark that replaced it); a continuous arm keeps every other stop.
    expect_u("PIN F4 (0.0.517): the frame-cap stop `xd_cont_stop_if(...FRAME_CAP)` is gone",
             pin_count(src, "N48_CM_STOP_FRAME_CAP"), 0u);
    // F5: FENCE_LATE reads the per-entry counter, never gKsRing.expired, as a stop input. The 4-poll/250ms
    // COMPARISON itself is now gfx_commit.h's own pure n48_cm_cont_fence_late (cont_fence_late_checks, above,
    // including the "byPolls alone" case the review's own planted break slipped past); this pin only proves the
    // WIRING (which counter feeds the stop), which is not reducible to a pure-function call.
    expect_u("PIN F5: the FENCE_LATE stop compares gXdContFenceLate4Polls, exactly once",
             pin_count(src, "gXdContArmSnap.fenceLate4Polls, gXdContFenceLate4Polls,\n                                                     N48_CM_STOP_FENCE_LATE"), 1u);
    expect_u("PIN F5: gKsRing.expired is NOT compared as a FENCE_LATE stop input",
             pin_count(src, "gXdContArmSnap.ringExpired,\n                                                     (uint64_t)gKsRing.expired, N48_CM_STOP_FENCE_LATE"), 0u);
    expect_u("PIN F5: the kext's own late decision calls the pure n48_cm_cont_fence_late, exactly once",
             pin_count(src, "n48_cm_cont_fence_late(cep->polls, kXdContFenceLatePolls,"), 1u);
    // F6/F7 (0.0.462 review, "tests") — THE ARITHMETIC ITSELF IS NOW PURE (n48_cm_stop_report,
    // n48_cm_cont_summary_baseline; cont_stop_report_checks/cont_summary_baseline_checks, above, sequence-drive
    // both directly, replacing 0.0.461's literal-arithmetic pins). What remains a pin is the WIRING: that the
    // kext calls these functions at all, at the sites that need them.
    expect_u("PIN F6: the stop line calls the pure n48_cm_stop_report, exactly once",
             pin_count(src, "n48_cm_stop_report(&gXdShot, stopNowUs, gXdContLastCommitUs, &sinceStart, &lastBeforeStop, &spentOut);"), 1u);
    expect_u("PIN F6: the stop line prints spentOut (n48_cm_stop_report's own answer), not a caller-side tracker",
             pin_count(src, "HWLOG(N48_CM_CONT_STOP_FMT, n48_cm_stop_name(gXdShot.stop_why), gXdShot.stop_why, spentOut,"), 1u);
    expect_u("PIN F7: the summary trigger calls the pure n48_cm_cont_summary_baseline",
             pin_count(src, "n48_cm_cont_summary_baseline(gXdContLog.lastSummaryUs, gXdShot.cont_start_us)"), 3u);
    // M3: the summary is no longer gated on `!contFull`, and prints at the stop too.
    expect_u("M3: the periodic summary's own `if` no longer tests !contFull",
             pin_count(src, "if (gXdShot.cont &&\n              (gXdContLog.commits - gXdContLog.lastSummaryCommit >= kXdContLogSummaryEvery"), 1u);
    // build 0.0.473 item 3 re-anchors this pin (it counted the literal `xd_cont_print_summary();`, which no longer
    // exists: the function now takes the arm's `cont` from its caller, because the stop-time call runs AFTER
    // n48_cm_shot_finish has cleared it). Every one of 0.0.472's three sites is still named, exactly once each, plus
    // 0.0.473's frame-top call.
    expect_u("M3: the forward declaration takes the caller's `cont`, exactly once",
             pin_count(src, "static void xd_cont_print_summary(bool contArm);"), 1u);
    expect_u("M3: the stop-time summary passes wasCont (read before the finish), exactly once",
             pin_count(src, "xd_cont_print_summary(wasCont);"), 1u);
    expect_u("M3: the per-commit trigger's summary, exactly once",
             pin_count(src, "xd_cont_print_summary(gXdShot.cont != 0u);"), 1u);
    expect_u("M3: no call is left without the argument", pin_count(src, "xd_cont_print_summary();"), 0u);
    // M1: cont_start is called BEFORE n48_cm_shot_spend (the reorder), and only once per frame.
    const char *contStartCallM1 = "contStartedThisFrame = n48_cm_shot_cont_start(&gXdShot, gXdShotSpentUs, gXdBuild.plane);";
    const char *spendCallM1 = "const bool spent = live && reason == N48_CM_OK && n48_cm_shot_spend(&gXdShot, gXdCmToken.seq);";
    expect_u("PIN M1: cont_start is called, exactly once", pin_count(src, contStartCallM1), 1u);
    expect_u("PIN M1: ...and it precedes n48_cm_shot_spend", strstr(src, contStartCallM1) && strstr(src, spendCallM1) &&
             strstr(src, contStartCallM1) < strstr(src, spendCallM1), 1u);
    // M5: the arm refuses on a latched fault (fail closed), and prints the status on the arm line.
    expect_u("PIN M5: the arm-refusal input is noFaultLatched = (vfArmOk && vfArmLo == 0u)",
             pin_count(src, "const uint32_t noFaultLatched = (vfArmOk && vfArmLo == 0u) ? 1u : 0u;"), 1u);
    expect_u("PIN M5: the arm line prints the VM-fault status, exactly once",
             pin_count(src, "VM-fault status at this instant (M5)"), 1u);
    // build 0.0.463 item 2 — THE MID-ARM SWITCH GUARD'S WIRING. cont_switch_refused_checks() above already
    // proves what n48_cm_cont_switch_refused DOES with real booleans; this proves each of the ten verb handlers
    // (43, 44, 45, 46, 47, 48, 49, 50, 52, 53) actually CALLS it with ITS OWN selector and the real
    // gXdShot.cont/gXdShot.state readings - "literal pins alone are not accepted", so neither stands without the
    // other (the same C4/C5 combination above).
    // build 0.0.471 item 2 — switch 51 joins this same wiring proof (PIN SWITCH-GUARD:51).
    // build 0.0.472 item 2 — switch 54 joins it too (PIN SWITCH-GUARD:54).
    // build 0.0.480 — switch 55 joins it too (PIN SWITCH-GUARD:55).
    // build 0.0.481 — switch 56 joins it too (PIN SWITCH-GUARD:56).
    // build 0.0.485 — switch 58 joins it too (PIN SWITCH-GUARD:58).
    // build 0.0.487 — switch 57 joins it too (PIN SWITCH-GUARD:57).
    // build 0.0.488 — switch 60 joins it too (PIN SWITCH-GUARD:60).
    // build 0.0.486 — switch 59 joins it too (PIN SWITCH-GUARD:59).
    // build 0.0.490 item 2 — switch 42 joins it too (PIN SWITCH-GUARD:42).
    // build 0.0.494 — switch 61 joins it too (PIN SWITCH-GUARD:61).
    // build 0.0.495 — switch 62 joins it too (PIN SWITCH-GUARD:62).
    // build 0.0.496 — switch 63 joins it too (PIN SWITCH-GUARD:63).
    // build 0.0.497 — switch 64 joins it too (PIN SWITCH-GUARD:64).
    // build 0.0.498 — switch 65 joins it too (PIN SWITCH-GUARD:65).
    // build 0.0.500 — switch 66 joins it too (PIN SWITCH-GUARD:66).
    // build 0.0.501 — switch 67 joins it too (PIN SWITCH-GUARD:67).
    // build 0.0.503 — switch 68 joins it too (PIN SWITCH-GUARD:68).
    // build 0.0.505 — switch 69 joins it too (PIN SWITCH-GUARD:69).
    // build 0.0.506 — switch 70 joins it too (PIN SWITCH-GUARD:70).
    // build 0.0.508 — switch 71 joins it too (PIN SWITCH-GUARD:71).
    // build 0.0.510 — switch 72 joins it too (PIN SWITCH-GUARD:72).
    // build 0.0.516 — switch 73 joins it too (PIN SWITCH-GUARD:73).
    // build 0.0.519 — switch 75 joins it too (PIN SWITCH-GUARD:75). (74 is pinned in gfx_flipmode_test.cpp T10: its M 3 form.)
    // build 0.0.522 — switch 76 joins it too (PIN SWITCH-GUARD:76).
    // build 0.0.523 — switches 77 and 78 join it too (PIN SWITCH-GUARD:77, PIN SWITCH-GUARD:78).
    // build 0.0.524 — switch 79 joins it too (PIN SWITCH-GUARD:79).
    // build 0.0.525 — switch 80 joins it too (PIN SWITCH-GUARD:80).
    // build 0.0.526 — switch 81 joins it too (PIN SWITCH-GUARD:81).
    // build 0.0.527 — switch 82 joins it too (PIN SWITCH-GUARD:82).
    // build 0.0.528 — switch 83 joins it too (PIN SWITCH-GUARD:83).
    // build 0.0.529 — switch 84 joins it too (PIN SWITCH-GUARD:84).
    // build 0.0.530 — switch 85 joins it too (PIN SWITCH-GUARD:85).
    // build 0.0.531 — switches 86 and 87 join it too (PIN SWITCH-GUARD:86, PIN SWITCH-GUARD:87).
    // build 0.0.533 — switch 88 joins it too (PIN SWITCH-GUARD:88).
    // build 0.0.534 — switches 89 and 90 join it too (PIN SWITCH-GUARD:89, PIN SWITCH-GUARD:90).
    // build 0.0.538 — switches 94 and 95 join it too (PIN SWITCH-GUARD:94, PIN SWITCH-GUARD:95).
    // build 0.0.540 — switches 96 and 97 join it too (PIN SWITCH-GUARD:96, PIN SWITCH-GUARD:97).
    // build 0.0.541 — switches 98 and 99 join it too (PIN SWITCH-GUARD:98, PIN SWITCH-GUARD:99).
    // build 0.0.544 — switch 103 joins it too (PIN SWITCH-GUARD:103).
    {
        const uint32_t guarded[] = { 42u, 43u, 44u, 45u, 46u, 47u, 48u, 49u, 50u, 51u, 52u, 53u, 54u, 55u, 56u, 57u, 58u, 59u, 60u, 61u, 62u, 63u, 64u, 65u, 66u, 67u, 68u, 69u, 70u, 71u, 72u, 73u, 75u, 76u, 77u, 78u, 79u, 80u, 81u, 82u, 83u, 84u, 85u, 86u, 87u, 88u, 89u, 90u, 94u, 95u, 96u, 97u, 98u, 99u, 103u };
        for (uint32_t sw : guarded) {
            char needle[160], lbl[160];
            std::snprintf(needle, sizeof(needle),
                          "n48_cm_cont_switch_refused(%uu, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)", sw);
            std::snprintf(lbl, sizeof(lbl), "PIN SWITCH-GUARD: switch %u calls n48_cm_cont_switch_refused with its "
                                            "own selector and the real gXdShot readings, exactly once", sw);
            expect_u(lbl, pin_count(src, needle), 1u);
        }
    }
    // build 0.0.490 items 1-2 — THE WIRING AND ORDER OF THE ARM-VERB REFUSAL AND OF 42's GUARDS. arm_enforce_hl_checks()
    // proves what n48_cm_arm_enforce_hl_refused DOES; these prove the arm verb computes it from the three live readings
    // (switch 41's request, 42's mode, 13), BEFORE the arm branch, and that the arm branch's ONE condition carries it, ahead
    // of both arm calls (continuous and one-shot); that the refusal is named on its own line; and that switch 42 answers its
    // two refusals BEFORE it can set any mode.
    {
        const char *ehCalc = "const uint32_t ehRefused = n48_cm_arm_enforce_hl_refused(gXdContN ? 1u : 0u,\n"
                             "                                                                 gMdMode == N48_MD_MODE_ENFORCE ? 1u : 0u,\n"
                             "                                                                 gXdHeadless ? 1u : 0u);";
        const char *armIf = "if (!missing && !contMissing && !ehRefused) {";
        const char *armCont = "n48_cm_shot_arm_cont_us(&gXdShot, &wsAt, latch_now_us(), gXdContN, gXdContTUs);";
        const char *armOne = "n48_cm_shot_arm(&gXdShot, &wsAt, latch_now_us(), gXdShotBudget);";
        const char *pc = strstr(src, ehCalc), *pi = strstr(src, armIf), *pa = strstr(src, armCont), *po = strstr(src, armOne);
        expect_u("PIN 490-1: the arm verb computes ehRefused from gXdContN, gMdMode == ENFORCE and gXdHeadless, exactly once",
                 pin_count(src, ehCalc), 1u);
        expect_u("PIN 490-1: the arm branch's condition carries ehRefused, exactly once", pin_count(src, armIf), 1u);
        expect_u("PIN 490-1: ORDER - computed, then the condition, then BOTH arm calls (continuous and one-shot)",
                 (pc && pi && pa && po && pc < pi && pi < pa && pi < po && pin_count(src, armCont) == 1u &&
                  pin_count(src, armOne) == 1u) ? 1u : 0u, 1u);
        expect_u("PIN 490-1: the refusal is named on its own line, gated on ehRefused",
                 (pin_count(src, "if (m != 2u && ehRefused)\n            HWLOG(\"gfx-commit:   ARM %s: switch 42 is ENFORCE and switch 13") == 1u)
                     ? 1u : 0u, 1u);
        const char *g42 = "if (contRefused42) { st = 5; why = \" *** REFUSED: a continuous arm stands ***\"; }";
        const char *e42 = "else if (ehRefused42) { st = 5;";
        const char *e42c = "const bool ehRefused42 = m == 1u && !gXdShot.cont && gXdShot.state == N48_CM_SHOT_ARMED &&\n"
                           "                                 n48_cm_arm_enforce_hl_refused(0u, 1u, gXdHeadless ? 1u : 0u) != 0u;";
        const char *set42 = "else if (m == 1u) { gMdMode = N48_MD_MODE_ENFORCE; why = \" - CHANGED BY THIS VERB\"; }";
        const char *pg = strstr(src, g42), *pe = strstr(src, e42), *pec = strstr(src, e42c), *ps = strstr(src, set42);
        expect_u("PIN 490-2: switch 42's continuous guard is honoured, exactly once", pin_count(src, g42), 1u);
        expect_u("PIN 490-1: switch 42 computes the one-shot ENFORCE refusal from the live shot and 13, exactly once",
                 pin_count(src, e42c), 1u);
        expect_u("PIN 490-1/2: ORDER - both refusals precede the only ENFORCE assignment",
                 (pg && pe && pec && ps && pec < pg && pg < pe && pe < ps && pin_count(src, set42) == 1u &&
                  pin_count(src, "gMdMode = N48_MD_MODE_ENFORCE;") == 1u) ? 1u : 0u, 1u);
    }
    // ================================================================================================================
    // build 0.0.473 — THE WIRING OF THE LONGER WINDOW. Each pure function is proven above by driving it; these prove
    // the kext calls it, at the site that needs it, in the order that matters.
    // ================================================================================================================
    {
        // item 1: switch 41 decodes through the pure function ALONE, and the arm freezes the decoded microseconds.
        expect_u("PIN 473-1: switch 41 decodes with n48_cm_sw41_decode(arg), exactly once",
                 pin_count(src, "const n48_cm_sw41 d41 = n48_cm_sw41_decode(arg);"), 1u);
        expect_u("PIN 473-1: no hand-written T decode is left in the kext", pin_count(src, "(arg >> 18) & 0x3FFull"), 0u);
        expect_u("PIN 473-1: SET stores N, T, its unit and its microseconds together, exactly once",
                 pin_count(src, "gXdContN = d41.n; gXdContT = d41.t; gXdContTUnit100 = d41.unit_100ms; gXdContTUs = d41.t_us;"), 1u);
        expect_u("PIN 473-1: the arm freezes gXdContTUs through n48_cm_shot_arm_cont_us, exactly once",
                 pin_count(src, "n48_cm_shot_arm_cont_us(&gXdShot, &wsAt, latch_now_us(), gXdContN, gXdContTUs);"), 1u);
        expect_u("PIN 473-1: the 10-ms-only arm is no longer called by the kext", pin_count(src, "n48_cm_shot_arm_cont(&gXdShot"), 0u);
        expect_u("PIN 473-1: the switch-41 line is the bounded N48_CM_SW41_FMT, exactly once", pin_count(src, "HWLOG(N48_CM_SW41_FMT,"), 1u);
        // item 3: xd_shot_finish reads the arm BEFORE the finish that clears `cont`.
        const char *budFirst = "const uint32_t budgetAtFinish = n48_cm_shot_budget_of(&gXdShot);";
        const char *wasFirst = "const bool wasCont = gXdShot.cont ? true : false;";
        const char *finCall = "if (!n48_cm_shot_finish(&gXdShot)) return;";
        const char *pb = strstr(src, budFirst), *pw = strstr(src, wasFirst), *pf = strstr(src, finCall);
        expect_u("PIN 473-3: xd_shot_finish reads wasCont and the budget BEFORE n48_cm_shot_finish",
                 (pb && pw && pf && pw < pf && pb < pf && pin_count(src, finCall) == 1u) ? 1u : 0u, 1u);
        expect_u("PIN 473-3: the DISARMED line prints the budget read first",
                 pin_count(src, "gXdShot.spent, budgetAtFinish);"), 1u);
        expect_u("PIN 473-3: the gate's cont_on still reads gXdShot.cont (cleared at DONE by the finish itself)",
                 pin_count(src, "c.cont_on = gXdShot.cont ? 1u : 0u;"), 1u);
        // item 2: the summary on the clock, after every stop at the frame top and before the level is read.
        const char *ooo = "(uint64_t)gKsRing.outOfOrder, N48_CM_STOP_OUT_OF_ORDER));\n        // build 0.0.473 item 2";
        const char *due = "n48_cm_cont_summary_due(gXdContLog.commits, gXdContLog.lastSummaryCommit, 0u, contNowUs,";
        const char *lvl = "const uint32_t arm = n48_cm_shot_level(&gXdShot, gXdArm, &wsNow);";
        const char *po = strstr(src, ooo), *pd = strstr(src, due), *pl = strstr(src, lvl);
        expect_u("PIN 473-2: the frame-top summary follows the last stop and precedes n48_cm_shot_level",
                 (po && pd && pl && po < pd && pd < pl && pin_count(src, due) == 1u) ? 1u : 0u, 1u);
        expect_u("PIN 473-2: ...and prints through xd_cont_print_summary(true), exactly once",
                 pin_count(src, "xd_cont_print_summary(true);"), 1u);
        // The whole gate of that block, verbatim: its only condition is the arm still standing and the pure cadence
        // (a planted `if (false && ...` left the call text above in place and was NOT caught before this pin).
        expect_u("PIN 473-2: the frame-top summary's condition is exactly (ARMED && n48_cm_cont_summary_due(...))",
                 pin_count(src, "        if (gXdShot.state == N48_CM_SHOT_ARMED &&\n"
                                "            n48_cm_cont_summary_due(gXdContLog.commits, gXdContLog.lastSummaryCommit, 0u, contNowUs,\n"
                                "                                    gXdContLog.lastSummaryUs, gXdShot.cont_start_us, kXdContLogSummaryUs)) {\n"
                                "            xd_cont_print_summary(true);\n"
                                "            gXdContLog.lastSummaryCommit = gXdContLog.commits;\n"
                                "            gXdContLog.lastSummaryUs = contNowUs;"), 1u);
        // item 2: the per-commit answer is taken once, at the spend, before the SPENT line reads it.
        const char *take = "gXdContLog.commitFull = n48_cm_cont_log_take(1u, &gXdContLog.fullCommits, N48_CM_CONT_LOG_FULL_COMMITS,";
        const char *spentRead = "const bool contFull = xd_cont_commit_line();";
        expect_u("PIN 473-2: commitFull is taken at the spend, exactly once, before the SPENT line reads it",
                 (pin_count(src, take) == 1u && pin_count(src, spentRead) == 1u && strstr(src, take) < strstr(src, spentRead)) ? 1u : 0u, 1u);
        // item 2: every capped site, exactly once each (a site whose gate is deleted drops its count).
        struct { const char *what; const char *needle; uint32_t n; } sites[] = {
            { "POLL",                      "if (xd_cont_poll_line())\n            HWLOG(\"fence828: POLL", 1u },
            { "APPLYING",                  "if (xd_cont_apply_line())\n        HWLOG(\"kstone-defer: APPLYING", 1u },
            { "GATE OK",                   "&gXdContLog.suppressed))\n            HWLOG(\"fence828: GATE OK", 1u },
            { "REWRITE REFUSED",           "live && reason != N48_CM_OK && xd_cont_refusal_line())", 1u },
            { "NOT COMMITTED (gate)",      "} else if (xd_cont_refusal_line()) {   // build 0.0.473 item 2: a refusal line", 1u },
            { "REGION-MOVED",              "if (xd_cont_refusal_line())   // build 0.0.473 item 2: a refusal line, per-arm capped\n            HWLOG(\"fence828: REGION-MOVED", 1u },
            { "NOT_RUN expiring",          "if (frExpiringNotRun && xd_cont_refusal_line())", 1u },
            { "WITHDRAWAL/RE-ARM REFUSED", "if (xd_cont_refusal_line())   // 0.0.473 item 2: per-arm capped under a continuous arm; counted above", 2u },
            { "keystone verdict lines",    "if (ksVLines)\n    HWLOG(", 3u },
            { "keystone NO-OP",            "if (xd_cont_line(false))\n        HWLOG(\"keystone: NO-OP", 1u },
            { "keystone WALK",             "const bool walkLog = xd_cont_line(false);", 1u },
            { "rootwrite WALK (quiet)",    "\"AFTER the keystone\", walkLog))", 1u },
            // build 0.0.497: +1 - the (B) fill-member retirement line in the keystone refusal branch (a refusal line).
            // build 0.0.519: +1 - switch 75's RETIRED AT ONCE AS NOPED line (the tail of a ring-walk refusal).
            { "keystone/hook refusal lines", "xd_cont_line(true)", 10u },
            { "kswait64 WAIT (0.0.497)",   "&& xd_cont_line(o == N48_KSW_TIMEOUT))", 1u },
            { "THE ARM IS",                "if (xd_cont_line(!kr.permitted))", 1u },
            { "COMMITTED/TRANSLATED",      "if (xd_cont_line(false))   // 0.0.473 item 2: a routine line of this commit", 2u },
            { "RING EXEMPTION",            "if (xd_cont_line(exWhy != N48_GFXN_EX_SPARED))", 1u },
        };
        for (auto &k : sites) {
            char lbl[160]; std::snprintf(lbl, sizeof(lbl), "PIN 473-2: the %s cap is wired (%u site(s))", k.what, k.n);
            expect_u(lbl, pin_count(src, k.needle), k.n);
        }
        // the wrappers are the pure functions with their own caps (a wrapper that stopped capping drops out here).
        expect_u("PIN 473-2: xd_cont_poll_line uses N48_CM_CONT_LOG_POLL_LINES",
                 pin_count(src, "n48_cm_cont_log_take(gXdShot.cont, &gXdContLog.pollLines, N48_CM_CONT_LOG_POLL_LINES,"), 1u);
        expect_u("PIN 473-2: xd_cont_apply_line uses N48_CM_CONT_LOG_APPLY_LINES",
                 pin_count(src, "n48_cm_cont_log_take(gXdShot.cont, &gXdContLog.applyLines, N48_CM_CONT_LOG_APPLY_LINES,"), 1u);
        expect_u("PIN 473-2: xd_cont_refusal_line uses N48_CM_CONT_LOG_REFUSAL_LINES",
                 pin_count(src, "n48_cm_cont_log_take(gXdShot.cont, &gXdContLog.refusalLines, N48_CM_CONT_LOG_REFUSAL_LINES,"), 1u);
        // item 4: both report lines print the bounded formats.
        expect_u("PIN 473-4: tvscan457 prints N48_TVSCAN457_FMT", pin_count(src, "HWLOG(N48_TVSCAN457_FMT,"), 1u);
        expect_u("PIN 473-4: would457 prints N48_CM_WOULD457_FMT", pin_count(src, "HWLOG(N48_CM_WOULD457_FMT,"), 1u);
        expect_u("PIN 473-4: no inline tvscan457 wording is left", pin_count(src, "HWLOG(\"tvscan457:"), 0u);
        expect_u("PIN 473-4: no inline would457 wording is left", pin_count(src, "HWLOG(\"would457:"), 0u);
    }
    std::free(src);
}


// build 0.0.487 — SWITCH 57's WIRING, IN THE KEXT'S ORDER (reachability over the real AppleHardwareHook.cpp, argv[1]).
// The pure pieces are driven above (cs_elide_gate_checks, cs_is_n_checks) and over real captured bytes in gfx_memdst_test
// (T3n: translate -> R1 -> n48_cm_live) and xlat12's test_cs_elide (T2); these prove the kext runs them in the order
// that makes the block REACHABLE: the pass-top clear, the once-per-pass latch, the flag on the segment's `ex`, the
// translate, THEN the accumulation (after the switch 56 retry, so the count is the final attempt's); the gather records
// P4's facts beside each identification and BEFORE the policy runs; the commit path reads the count BEFORE its `live`
// line and before the rehearsal's probe copies `c`. A block that ran before its input existed fails.
static void cselide_wiring_pins(const char *ahh)
{
    const std::string src = rh_slurp(ahh);
    const size_t np = std::string::npos;
    expect_u("0.0.487 PIN: the hook source was read", src.empty() ? 0u : 1u, 1u);
    if (src.empty()) return;
    const std::string pol = rh_body(src, "static void gfxsrc_policy(");
    const std::string df = rh_body(src, "static uint32_t gfxsrc_decide_frame(");
    const std::string ct = rh_body(src, "static uint32_t gfxsrc_commit_try(");
    const size_t p1 = pol.find("gXdBuild.csElided = 0u;   // build 0.0.487: a previous frame's elision never describes this one");
    const size_t p2 = pol.find("const uint32_t csOn = gCsElideOn ? 1u : 0u;");
    const size_t p3 = pol.find("gXdCsN.f = f;");
    const size_t p4 = pol.find("if (csOn) { ex.flags |= XLAT12_EXTRA_CS_ELIDE; ex.cs_ctx = &gXdCsN; ex.cs_is_n = &gfxsrc_cs_is_n; gCsElideS.segsOn++; }");
    const size_t p5 = pol.find("uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from], to - from,");
    const size_t p6 = pol.find("st = gfxsrc_unit_retry(&ex, k, from, to - from, out, &olen, &ds, st, dp, build, &rwhy);");
    const size_t p7 = pol.find("gXdBuild.csElided += ds.cs_elided;");
    expect_u("0.0.487 PIN policy: clear -> latch -> ctx -> flag -> translate -> retry -> accumulate, in that order",
             (p1 != np && p2 != np && p3 != np && p4 != np && p5 != np && p6 != np && p7 != np &&
              p1 < p2 && p2 < p3 && p3 < p4 && p4 < p5 && p5 < p6 && p6 < p7) ? 1u : 0u, 1u);
    expect_u("0.0.487 PIN policy: switch 57 is read ONCE per pass (the latch), nowhere else in the policy",
             (p2 != np && pol.find("gCsElideOn", p2 + 30u) == np) ? 1u : 0u, 1u);
    expect_u("0.0.487 PIN policy: the accumulation is under the flag the segment was translated with",
             pol.find("if (ex.flags & XLAT12_EXTRA_CS_ELIDE) {\n            gXdBuild.csElided += ds.cs_elided;") != np ? 1u : 0u, 1u);
    const size_t q1 = df.find("gfxsrc_identify(vm, it.va, it.index, pid, &f.pgm[f.npgm]);");
    const size_t q2 = df.find("gXdCsN.va[f.npgm] = it.va; gXdCsN.stage[f.npgm] = it.index;");
    const size_t q3 = q2 != np ? df.find("f.npgm++;", q2) : np;
    const size_t q4 = df.find("gfxsrc_policy(vm, &f, f.ib[0].got, ib0Va, dp, dkey, arm, boundCtx, wsBound);");
    const size_t q5 = df.find("gXdBuild.csElided = 0u;   // build 0.0.487: and switch 57's count");
    const size_t q6 = df.find("gfxsrc_commit_try(vm, info, ib0Va,");
    expect_u("0.0.487 PIN decide_frame: identify -> P4 facts -> npgm++ -> policy -> commit_try",
             (q1 != np && q2 != np && q3 != np && q4 != np && q6 != np && q1 < q2 && q2 < q3 && q3 < q4 && q4 < q6) ? 1u : 0u, 1u);
    expect_u("0.0.487 PIN decide_frame: the policy-skipping branch clears the count too (the second reset site)",
             (q5 != np && q5 < q6) ? 1u : 0u, 1u);
    const size_t r1 = ct.find("c.cs_elided = gXdBuild.csElided;");
    const size_t r2 = ct.find("(!c.cs_elided || (gMdMode == N48_MD_MODE_ENFORCE && gXdBuild.md_ok)) &&");   /* 0.0.495: + a clause */
    const size_t r3 = ct.find("probe = c;");
    const size_t r4 = ct.find("n48_cm_rehearse(&probe,");
    expect_u("0.0.487 PIN commit_try: the count is read before the live line, and the probe copies it before the rehearsal",
             (r1 != np && r2 != np && r3 != np && r4 != np && r1 < r2 && r2 < r3 && r3 < r4) ? 1u : 0u, 1u);
    expect_u("0.0.487 PIN: switch 57 is declared OFF", pin_count(src.c_str(), "static volatile uint32_t gCsElideOn { 0u };"), 1u);
    expect_u("0.0.487 PIN: ... and written by the verb alone (one assignment)", pin_count(src.c_str(), "gCsElideOn = "), 1u);
    expect_u("0.0.487 PIN: the verb's selector exists exactly once", pin_count(src.c_str(), "(arg & 0xffull) == 57ull"), 1u);
    expect_u("0.0.487 PIN: the verb prints the bounded cselide57 line", pin_count(src.c_str(), "HWLOG(N48_CM_CSELIDE_FMT,"), 1u);
}

// build 0.0.500 — SWITCH 66's WIRING, IN THE KEXT'S ORDER (reachability over the real AppleHardwareHook.cpp, argv[1]):
// the pass-top clear and once-per-pass latch, the flag on the segment's `ex` inside the descriptor block beside the strip,
// the translate and the switch-56 retry, THEN the accumulation and the kext's backstop BEFORE the copy guard; the second
// reset site; the commit path reads the count BEFORE its `live` line (which carries the clause) and before the probe copy.
static void drawelide_wiring_pins(const char *ahh)
{
    const std::string src = rh_slurp(ahh);
    const size_t np = std::string::npos;
    expect_u("0.0.500 PIN: the hook source was read", src.empty() ? 0u : 1u, 1u);
    if (src.empty()) return;
    const std::string pol = rh_body(src, "static void gfxsrc_policy(");
    const std::string df = rh_body(src, "static uint32_t gfxsrc_decide_frame(");
    const std::string ct = rh_body(src, "static uint32_t gfxsrc_commit_try(");
    const size_t p1 = pol.find("gXdBuild.drawElided = 0u; // build 0.0.500: nor switch 66's");
    const size_t p2 = pol.find("gXdBuild.deRows = gDrawElideOn & 0x7u;");   // 0.0.512: M 7
    const size_t p3 = pol.find("ex.desc_dcc_ok = &gfxsrc_desc_dcc_ok;");
    const size_t p4 = pol.find("if (gXdBuild.deRows && gDccStripOn) { ex.flags |= XLAT12_EXTRA_DRAW_ELIDE; ex.draw_elide_rows = gXdBuild.deRows; gDrawElideS.segsOn++; }");
    const size_t p5 = pol.find("uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from], to - from,");
    const size_t p6 = pol.find("st = gfxsrc_unit_retry(&ex, k, from, to - from, out, &olen, &ds, st, dp, build, &rwhy);");
    const size_t p7 = pol.find("gXdBuild.drawElided += ds.draw_elided;");
    const size_t p8 = pol.find("if (!st && n48_cm_de_backstop(xlat12_ib_count_draws(out, olen), ds.draws, ds.draw_elided)) {");
    const size_t p9 = pol.find("const uint32_t cgReason = navi48_cg_seg_check();");
    expect_u("0.0.500 PIN policy: clear -> latch -> desc block -> flag -> translate -> retry -> accumulate -> backstop -> copy guard",
             (p1 != np && p2 != np && p3 != np && p4 != np && p5 != np && p6 != np && p7 != np && p8 != np && p9 != np &&
              p1 < p2 && p2 < p3 && p3 < p4 && p4 < p5 && p5 < p6 && p6 < p7 && p7 < p8 && p8 < p9) ? 1u : 0u, 1u);
    expect_u("0.0.500 PIN policy: switch 66 is read ONCE per pass (the latch), nowhere else in the policy",
             (p2 != np && pol.find("gDrawElideOn", p2 + 30u) == np) ? 1u : 0u, 1u);
    expect_u("0.0.500 PIN policy: the backstop refuses with the translator's own code",
             pol.find("st = XLAT12_IB_ERR_VERIFY; ds.err_op = XLAT12_DE_BACKSTOP;") != np ? 1u : 0u, 1u);
    const size_t q5 = df.find("gXdBuild.drawElided = 0u; // build 0.0.500: and switch 66's, for the same reason");
    const size_t q6 = df.find("gfxsrc_commit_try(vm, info, ib0Va,");
    expect_u("0.0.500 PIN decide_frame: the policy-skipping branch clears the count too (the second reset site)",
             (q5 != np && q6 != np && q5 < q6) ? 1u : 0u, 1u);
    const size_t r1 = ct.find("c.draw_elided = gXdBuild.drawElided;");
    const size_t r2 = ct.find("(!c.draw_elided || (gMdMode == N48_MD_MODE_ENFORCE && gXdBuild.md_ok)) &&");
    const size_t r3 = ct.find("probe = c;");
    const size_t r4 = ct.find("n48_cm_rehearse(&probe,");
    expect_u("0.0.500 PIN commit_try: the count is read before the live line, and the probe copies it before the rehearsal",
             (r1 != np && r2 != np && r3 != np && r4 != np && r1 < r2 && r2 < r3 && r3 < r4) ? 1u : 0u, 1u);
    expect_u("0.0.500 PIN: switch 66 is declared OFF", pin_count(src.c_str(), "static volatile uint32_t gDrawElideOn { 0u };"), 1u);
    expect_u("0.0.500 PIN: ... and written by the verb alone (one assignment)", pin_count(src.c_str(), "gDrawElideOn = "), 1u);
    expect_u("0.0.500 PIN: the verb's selector exists exactly once", pin_count(src.c_str(), "(arg & 0xffull) == 66ull"), 1u);
    expect_u("0.0.500 PIN: the verb sets through n48_cm_de_set", pin_count(src.c_str(), "changed66 = n48_cm_de_set(m, &fde);"), 1u);
    expect_u("0.0.500 PIN: the verb prints the bounded drawelide66 line", pin_count(src.c_str(), "HWLOG(N48_CM_DRAWELIDE_FMT,"), 1u);
    expect_u("0.0.500 PIN: ... and the census line", pin_count(src.c_str(), "HWLOG(N48_CM_DRAWELIDE_CENSUS_FMT,"), 1u);
    expect_u("0.0.500 PIN: XLAT12_EXTRA_DRAW_ELIDE is set at exactly one site", pin_count(src.c_str(), "ex.flags |= XLAT12_EXTRA_DRAW_ELIDE;"), 1u);
}

// =====================================================================================================================
// build 0.0.550 ( PLAN step 1; apple/gfx_lut107.h) — SWITCH 107 "lutretry".
//   R1  OFF and SHADOW learn exactly as 0.0.549 (one shot per scope), over every input
//   R2  ON never learns from a non-plane frame; retries after a refused record; capped at N48_LR_TRIES_MAX
//   R3  a modelled scope with the REAL decode: [plane bad, plane good] -> OFF spent on the bad record, ON learns the good one;
//       [non-plane good, plane bad, plane good] -> ON learns only from the plane frame
//   R4  the fail-closed gate inputs, through the REAL gate: a plane frame at gLutHave 0 is LUT-NOT-READY (even with a stale
//       ready 1), at gLutHave 1 the inputs are untouched; refusal direction only; OFF and SHADOW never change an input
//   R5  the plane-slot set; the BLACK-RISK predicate; the peek is SHADOW only
//   R6  every new 107 line <= 491 bytes at its widest
//   R7  source pins: the learn site (OFF calls 0.0.549's learn directly), the retry clears only gLutAttempted, gfxsrc_lut_learn and
//       lutfill_thread byte-identical to 0.0.549 (FNV), the gate call's place, the frame facts, the verb, SHADOW writes nothing
// =====================================================================================================================
static uint64_t lr_fnv(const std::string &b)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : b) { h ^= c; h *= 0x100000001b3ull; }
    return h;
}
static size_t lr_count(const std::string &s, const std::string &n)
{
    size_t c = 0u, p = 0u;
    while (!n.empty() && (p = s.find(n, p)) != std::string::npos) { c++; p += n.size(); }
    return c;
}
/* The model of one arm scope's learn site: `plane[i]` says frame i is a GPUPass plane frame, `good[i]` that its slot-4 record is the
 * real LUT record (else a not-32_FLOAT one). Returns the frame index the LUT was learned on (or -1). The decode is the REAL one. */
static int lr_model2(uint32_t mode, const uint32_t *plane, const uint32_t *good, const uint32_t *setn, const uint32_t *seggp, uint32_t n,
                     uint32_t *tries);
static int lr_model(uint32_t mode, const uint32_t *plane, const uint32_t *good, uint32_t n, uint32_t *tries)
{
    static const uint32_t one[16] = { 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u };
    return lr_model2(mode, plane, good, one, one, n, tries);   /* 0.0.550's scopes: a slot exists, every segment GPUPass */
}
/* build 0.0.551: the same model with the plane-slot set's size and the segment's GPUPass fact per frame. */
static int lr_model2(uint32_t mode, const uint32_t *plane, const uint32_t *good, const uint32_t *setn, const uint32_t *seggp, uint32_t n,
                     uint32_t *tries)
{
    static const uint32_t kLut[8] = { 0x04011400u, 0xc1600000u, 0x80000fffu, 0xc0000204u, 0x00000002u, 0u, 0u, 0u };
    static const uint32_t kBad[8] = { 0x04011400u, 0xc0e00000u, 0x80000fffu, 0xc0000204u, 0x00000002u, 0u, 0u, 0u };
    uint32_t have = 0u, attempted = 0u;
    *tries = 0u;
    for (uint32_t i = 0u; i < n; i++) {
        if (have) break;
        const uint32_t act = n48_lr_learn_act(mode, attempted, plane[i], *tries, setn[i], seggp[i]);
        if (act != N48_LR_ACT_LEGACY && act != N48_LR_ACT_RETRY) continue;
        if (act == N48_LR_ACT_RETRY) { (*tries)++; attempted = 0u; }
        if (attempted) continue;          /* gfxsrc_lut_learn's own first clause */
        attempted = 1u;
        n48_lut_desc d {};
        uint32_t why = n48_lut_decode_record(good[i] ? kLut : kBad, &d);
        if (why == N48_LUT_OK) why = n48_lut_instrument_ok(&d);
        if (why == N48_LUT_OK) { have = 1u; return (int)i; }
    }
    return -1;
}
static void lut107_checks(const char *ahh)
{
    std::printf("== 0.0.550: switch 107 (lutretry) ==\n");
    // R1
    {
        uint32_t bad = 0u;
        for (uint32_t mode = 0u; mode < 3u; mode++) {
            if (mode == N48_LR_ON) continue;
            for (uint32_t att = 0u; att < 2u; att++)
                for (uint32_t pl = 0u; pl < 2u; pl++)
                    for (uint32_t t = 0u; t <= N48_LR_TRIES_MAX + 1u; t += 7u)
                        for (uint32_t sn = 0u; sn < 2u; sn++)
                            for (uint32_t sg = 0u; sg < 2u; sg++)
                                if (n48_lr_learn_act(mode, att, pl, t, sn, sg) != (att ? N48_LR_ACT_NONE : N48_LR_ACT_LEGACY)) bad++;
        }
        expect_u("107 R1 OFF and SHADOW: the learn is 0.0.549's one shot over every input (LEGACY once, then NONE)", bad, 0u);
    }
    // R2
    {
        uint32_t nonPlaneLearns = 0u;
        for (uint32_t att = 0u; att < 2u; att++)
            for (uint32_t t = 0u; t < 2u * N48_LR_TRIES_MAX; t++)
                for (uint32_t sg = 0u; sg < 2u; sg++)
                    if (n48_lr_learn_act(N48_LR_ON, att, 0u, t, 1u + (t & 7u), sg) != N48_LR_ACT_SKIP_NONPLANE) nonPlaneLearns++;
        expect_u("107 R2 ON: a frame that is not a GPUPass plane frame never learns (every attempted / tries)", nonPlaneLearns, 0u);
        expect_u("107 R2 ON: a plane frame after a REFUSED record (attempted 1) is retried",
                 n48_lr_learn_act(N48_LR_ON, 1u, 1u, 1u, 3u, 1u), N48_LR_ACT_RETRY);
        expect_u("107 R2 ON: the first plane frame (attempted 0) learns", n48_lr_learn_act(N48_LR_ON, 0u, 1u, 0u, 3u, 1u), N48_LR_ACT_RETRY);
        expect_u("107 R2 ON: the last try under the cap", n48_lr_learn_act(N48_LR_ON, 1u, 1u, N48_LR_TRIES_MAX - 1u, 3u, 1u),
                 N48_LR_ACT_RETRY);
        expect_u("107 R2 ON: capped at N48_LR_TRIES_MAX", n48_lr_learn_act(N48_LR_ON, 1u, 1u, N48_LR_TRIES_MAX, 3u, 1u), N48_LR_ACT_CAPPED);
    }
    // R3
    {
        const uint32_t pl1[] = { 1u, 1u }, gd1[] = { 0u, 1u };
        uint32_t tries = 0u;
        expect_u("107 R3 [plane bad, plane good]: OFF spends its one shot on the bad record and never learns (RUN AX/AY)",
                 (uint64_t)(int64_t)lr_model(N48_LR_OFF, pl1, gd1, 2u, &tries), (uint64_t)(int64_t)-1);
        expect_u("107 R3 [plane bad, plane good]: SHADOW learns exactly as OFF",
                 (uint64_t)(int64_t)lr_model(N48_LR_SHADOW, pl1, gd1, 2u, &tries), (uint64_t)(int64_t)-1);
        expect_u("107 R3 [plane bad, plane good]: ON retries and learns frame 1",
                 (uint64_t)(int64_t)lr_model(N48_LR_ON, pl1, gd1, 2u, &tries), 1u);
        expect_u("107 R3 ... in two tries", tries, 2u);
        const uint32_t pl2[] = { 0u, 1u, 0u, 1u }, gd2[] = { 1u, 0u, 1u, 1u };
        expect_u("107 R3 [non-plane good, plane bad, non-plane good, plane good]: ON learns ONLY from a plane frame (frame 3)",
                 (uint64_t)(int64_t)lr_model(N48_LR_ON, pl2, gd2, 4u, &tries), 3u);
        expect_u("107 R3 ... the non-plane frames spent no try", tries, 2u);
        const uint32_t pl3[] = { 0u, 0u, 0u }, gd3[] = { 1u, 1u, 1u };
        expect_u("107 R3 [non-plane good x3]: ON never learns", (uint64_t)(int64_t)lr_model(N48_LR_ON, pl3, gd3, 3u, &tries),
                 (uint64_t)(int64_t)-1);
    }
    // R4: through the REAL gate
    {
        n48_cm_frame c; uint32_t d = 0u;
        good_frame(c, 1040u, 3u);
        c.lut_switch = 1u; c.lut_plane = 0u; c.lut_ready = 1u;   /* a stale ready from an earlier arm scope; 0.0.549 would commit */
        expect_u("107 R4 baseline: a plane frame 0.0.549 did not mark (gLutHave 0) commits", gGate(&c, &d), N48_CM_OK);
        uint32_t pln = c.lut_plane, rdy = c.lut_ready;
        expect_u("107 R4 ON at gLutHave 0: the inputs are changed", n48_lr_gate(N48_LR_ON, 1u, 1u, 0u, &pln, &rdy), 1u);
        c.lut_plane = pln; c.lut_ready = rdy;
        expect_u("107 R4 ON at gLutHave 0: the REAL gate refuses it LUT-NOT-READY", gGate(&c, &d), N48_CM_LUT_NOT_READY);
        good_frame(c, 1040u, 3u);
        c.lut_switch = 1u; c.lut_plane = 0u; c.lut_ready = 1u;
        pln = c.lut_plane; rdy = c.lut_ready;
        expect_u("107 R4 ON at gLutHave 1: the inputs are untouched", n48_lr_gate(N48_LR_ON, 1u, 1u, 1u, &pln, &rdy) == 0u &&
                 pln == 0u && rdy == 1u ? 1u : 0u, 1u);
        c.lut_plane = pln; c.lut_ready = rdy;
        expect_u("107 R4 ON at gLutHave 1: the REAL gate admits it", gGate(&c, &d), N48_CM_OK);
        uint32_t lowered = 0u, offChanged = 0u, nonPlane = 0u, sw0 = 0u;
        for (uint32_t mode = 0u; mode < 3u; mode++)
            for (uint32_t m = 0u; m < 32u; m++) {
                const uint32_t sw = m & 1u, pl = (m >> 1) & 1u, have = (m >> 2) & 1u, ip = (m >> 3) & 1u, ir = (m >> 4) & 1u;
                uint32_t p2 = ip, r2 = ir;
                const uint32_t ch = n48_lr_gate(mode, sw, pl, have, &p2, &r2);
                if (p2 < ip || r2 > ir) lowered++;                              /* refusal direction only */
                if (mode != N48_LR_ON && (ch || p2 != ip || r2 != ir)) offChanged++;   /* OFF / SHADOW identity */
                if (!pl && (p2 != ip || r2 != ir)) nonPlane++;                  /* a non-plane frame is never touched */
                if (!sw && (p2 != ip || r2 != ir)) sw0++;                       /* 32 OFF: inert */
            }
        expect_u("107 R4 refusal direction only: lut_plane never lowered, lut_ready never raised (every mode, every input)", lowered, 0u);
        expect_u("107 R4 OFF and SHADOW never change a gate input (SHADOW never changes a verdict)", offChanged, 0u);
        expect_u("107 R4 a non-plane frame's inputs are never changed", nonPlane, 0u);
        expect_u("107 R4 inert with switch 32 OFF", sw0, 0u);
    }
    // R5
    {
        const uint64_t set[] = { 0x401800000ull, 0ull, ~0ull, 0x400100000ull };
        expect_u("107 R5 a slot VA is in the set", n48_lr_in_set(0x400100000ull, set, 4u), 1u);
        expect_u("107 R5 another VA is not", n48_lr_in_set(0x400800000ull, set, 4u), 0u);
        expect_u("107 R5 CB0 0 and the unlearned marker never match", n48_lr_in_set(0ull, set, 4u) + n48_lr_in_set(~0ull, set, 4u), 0u);
        expect_u("107 R5 a null set matches nothing", n48_lr_in_set(0x400100000ull, nullptr, 4u), 0u);
        expect_u("107 R5 the plane frame needs GPUPass AND a slot", n48_lr_plane_frame(1u, 1u) == 1u && n48_lr_plane_frame(0u, 1u) == 0u &&
                 n48_lr_plane_frame(1u, 0u) == 0u ? 1u : 0u, 1u);
        uint32_t br = 0u;
        for (uint32_t m = 0u; m < 8u; m++) br += n48_lr_black_risk(m & 1u, (m >> 1) & 1u, (m >> 2) & 1u) != ((m & 3u) == 3u && !(m & 4u)) ? 1u : 0u;
        expect_u("107 R5 BLACK-RISK = committed AND plane AND no LUT (truth table)", br, 0u);
        uint32_t peekBad = 0u;
        for (uint32_t mode = 0u; mode < 3u; mode++)
            for (uint32_t m = 0u; m < 4u; m++)
                for (uint32_t pk = 0u; pk <= N48_LR_PEEKS_MAX; pk += N48_LR_PEEKS_MAX)
                    if (n48_lr_peek(mode, m & 1u, pk, (m >> 1) & 1u) != (mode == N48_LR_SHADOW && (m & 1u) && !(m & 2u) && pk < N48_LR_PEEKS_MAX))
                        peekBad++;
        expect_u("107 R5 the peek: SHADOW only, a plane frame, until one would-learn, under its cap", peekBad, 0u);
        expect_u("107 R5 the verb's M: 1 ON, 2 OFF, 3 SHADOW, others refused", n48_lr_mode_of_m(1u) == N48_LR_ON &&
                 n48_lr_mode_of_m(2u) == N48_LR_OFF && n48_lr_mode_of_m(3u) == N48_LR_SHADOW && n48_lr_mode_of_m(4u) == N48_LR_MODES &&
                 n48_lr_mode_of_m(0xFFu) == N48_LR_MODES && (107u | 1u << 8) == 363u && (107u | 2u << 8) == 619u &&
                 (107u | 3u << 8) == 875u ? 1u : 0u, 1u);
    }
    // R6 widths
    {
        char b[2048];
        const char *pg = "ws_D_GPUPass_but_a_much_longer_program_identity_name_for_the_width_x";   /* 67 chars */
        const unsigned long long M = ~0ull;
        uint32_t w = 0u, n;
        n = (uint32_t)std::snprintf(b, sizeof b, N48_LR_FMT, "SHADOW", "OFF - 107 does nothing",
                                    " - `gfxneuter 107` CHANGED BY THIS VERB (counters reset)");
        if (n > w) w = n;
        std::printf("   107 widths: report %u", n);
        n = (uint32_t)std::snprintf(b, sizeof b, N48_LR_FMT2, M, M, M, M, M, M, M, M, M, M, M);
        if (n > w) w = n;
        std::printf(", report2 %u", n);
        n = (uint32_t)std::snprintf(b, sizeof b, N48_LR_REF_FMT, M, 0xFFFFFFFFu, "SHADOW", 0xFFFFFFFFu, pg, M, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                    0xFFFFFFFFu, 0xFFFFFFFFu, M, "VA-differs-from-the-draw's-LUT-input (111)", M, 0xFFFFFFFFu);
        if (n > w) w = n;
        std::printf(", ref %u", n);
        n = (uint32_t)std::snprintf(b, sizeof b, N48_LR_LEARN_FMT, M, 0xFFFFFFFFu, 0xFFFFFFFFu, pg, M, M, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                    "was started (one thread per arm scope)");
        if (n > w) w = n;
        std::printf(", learn %u", n);
        n = (uint32_t)std::snprintf(b, sizeof b, N48_LR_PEEK_FMT, M, "REFUSE this record", 0xFFFFFFFFu, pg, M, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                    0xFFFFFFFFu, 0xFFFFFFFFu, M, "decoded, this instrument's shape (pages not walked in SHADOW)",
                                    0xFFFFFFFFu, 0xFFFFFFFFu);
        if (n > w) w = n;
        std::printf(", peek %u", n);
        n = (uint32_t)std::snprintf(b, sizeof b, N48_LR_BLACK_FMT, M, 1u, M, "OFF", "SHADOW", M);
        if (n > w) w = n;
        std::printf(", black %u", n);
        n = (uint32_t)std::snprintf(b, sizeof b, N48_LR_FMT3, M, M, M, M, M, 0xFFFFFFFFu, " - CONTINUOUS STOP");   /* 0.0.551 */
        if (n > w) w = n;
        std::printf(", counts %u", n);
        n = (uint32_t)std::snprintf(b, sizeof b, N48_LR_GPNO_FMT, M, 1u, M, "(not stamped this frame) - not tested against",
                                    0xFFFFFFFFu, "OFF", "SHADOW", M);
        if (n > w) w = n;
        std::printf(", gp-no-lut %u", n);
        n = (uint32_t)std::snprintf(b, sizeof b, N48_LR_GPOTHER_FMT, M, M, 1u, M, "OFF", "SHADOW", M);
        if (n > w) w = n;
        std::printf(", gp-other-lut %u\n", n);
        expect_u("107 R6 every new 107 line fits 491 bytes at its widest", w <= 491u ? 1u : 0u, 1u);
    }
    // R7 pins
    const std::string src = rh_slurp(ahh);
    expect_u("107 R7 PIN: the hook source was read", src.empty() ? 0u : 1u, 1u);
    if (src.empty()) return;
    const size_t np = std::string::npos;
    const std::string pol = rh_body(src, "static void gfxsrc_policy(");
    // build 0.0.553 (switch 111) DELIBERATE re-baseline: the two learn calls are wrapped by 111's pick (OFF never asks it: the
    // `li111_mode() == N48_LI_OFF ||` short-circuit) and gLiSel is cleared after them; 0.0.549's clauses and the two calls are unchanged.
    const char *site =
        "            if (arm == N48_SD_ARM_COMMIT && wsBound && n48_lut_plane_shape(ds.in_mode, ds.in_n) &&\n"
        "                !gLutHave && ds.in_img_va) {\n"
        "                // build 0.0.553 (switch 111): OFF never asks (entry 4, as 0.0.552); ON learns only from the draw's own entry\n"
        "                if (li111_mode() == N48_LI_OFF || li111_pick(vm, &ds)) {\n"
        "                if (lr107_mode() == N48_LR_OFF) gfxsrc_lut_learn(vm, ds.in_img_va);   // 107 OFF: 0.0.549's one-shot, byte for byte\n"
        "                else lr107_learn(vm, ds.in_img_va);   // build 0.0.550 (switch 107): ON retries on plane frames / SHADOW peeks\n"
        "                }\n"
        "                gLiSel.use = 0u;   // build 0.0.553: the selection lives for this one learn call only\n"
        "            }\n";
    expect_u("107 R7 PIN the learn site: 0.0.549's clauses unchanged; OFF calls gfxsrc_lut_learn directly; else lr107_learn",
             lr_count(pol, site) == 1u && lr_count(src, "gfxsrc_lut_learn(vm, ds.in_img_va)") == 1u &&
             lr_count(src, "lr107_learn(vm, ds.in_img_va)") == 1u ? 1u : 0u, 1u);
    {   // build 0.0.553 (switch 111): gfxsrc_lut_learn's ONLY changes are the entry it reads (li111_learn_off(), twice: the read and
        // its census note) and ONE extra acceptance line; with those reversed it hashes exactly as 0.0.549's (the unchanged pin value),
        // so every other clause, the page walk, the destination guard and the kick are byte for byte 0.0.552's.
        std::string lb = rh_body(src, "static void gfxsrc_lut_learn(const GfxcVm &vm, uint64_t imgVa) {");
        const std::string cm = "    // build 0.0.553 (switch 111): li111_learn_off() is N48_LUT_SLOT4_OFF unless 111 ON selected the draw's own entry\n";
        const std::string ac = "    if (why == N48_LUT_OK) why = li111_accept(rec, &d);       // build 0.0.553 (switch 111 ON): 1D_ARRAY and VA == in_va[1]\n";
        const size_t pc = lb.find(cm), pa = lb.find(ac);
        const size_t pi = lb.find("    if (why == N48_LUT_OK) why = n48_lut_instrument_ok(&d);");
        const size_t pm = lb.find("    if (gLutProdSeen && gLutProdVa != d.va)");
        expect_u("111 R7 PIN gfxsrc_lut_learn: the comment once, the 111 acceptance once, right after the instrument's shape and before the "
                 "producer cross-check, and li111_learn_off() exactly twice (the read and its census note)",
                 pc != np && pa != np && pi != np && pm != np && pi < pa && pa < pm && lr_count(lb, cm) == 1u && lr_count(lb, ac) == 1u &&
                 lr_count(lb, "imgVa + li111_learn_off()") == 2u && lr_count(lb, "N48_LUT_SLOT4_OFF") == 1u ? 1u : 0u, 1u);
        if (pa != np) lb.erase(pa, ac.size());
        if (pc != np) lb.erase(pc, cm.size());
        for (size_t q; (q = lb.find("imgVa + li111_learn_off()")) != np; ) lb.replace(q, 25u, "imgVa + (uint64_t)N48_LUT_SLOT4_OFF");
        expect_u("107 R7 PIN gfxsrc_lut_learn with switch 111's three edits reversed is byte-identical to 0.0.549 (FNV of its body)",
                 lr_fnv(lb), 0xa18ccb7ee8eb31b1ull);
    }
    {   // build 0.0.551 (SHOULD 3): the thread's ONLY change is the one tag store before the flag; without that line it hashes
        // exactly as 0.0.549's (0.0.550's pin value), so the ramp, the writes, the read-back and the HDP flush are unchanged.
        const std::string th = rh_body(src, "static void lutfill_thread(void * /*param*/, wait_result_t /*wr*/) {");
        const std::string tag = "    gLutReadyScope = scope;   // build 0.0.551 (SHOULD 3): the tag, stored BEFORE the flag (both volatile: program order kept)\n";
        const size_t tp = th.find(tag);
        std::string th0 = th;
        if (tp != np) th0.erase(tp, tag.size());
        expect_u("107 R7 PIN THE WRITE PATH: lutfill_thread minus the 0.0.551 tag line is byte-identical to 0.0.549 (FNV of its body)",
                 lr_fnv(th0), 0xae6737c5beed7b24ull);
        expect_u("107 S3 PIN the tag: stored once, in lutfill_thread, from the scope it verified, right before `gLutReady = 1u;`",
                 tp != np && th.find("    gLutReady = 1u;", tp) == tp + tag.size() && lr_count(src, "gLutReadyScope = ") == 1u ? 1u : 0u, 1u);
    }
    const std::string ln = rh_body(src, "static __attribute__((noinline)) void lr107_learn(const GfxcVm &vm, uint64_t imgVa)");
    const size_t a1 = ln.find("const uint32_t plane = lr107_plane_now();");
    const size_t a2 = ln.find("const uint32_t act = n48_lr_learn_act(mode, gLutAttempted, plane, gLr.tries, setN, segGp);");
    const size_t a3 = ln.find("} else if (act == N48_LR_ACT_RETRY) {");
    const size_t a4 = ln.find("gLutAttempted = 0u;");
    const size_t a5 = a4 != np ? ln.find("gfxsrc_lut_learn(vm, imgVa);", a4) : np;
    expect_u("107 R7 PIN lr107_learn: the plane fact, THEN the act, and ONLY the RETRY branch clears gLutAttempted, right before the "
             "unchanged learn", a1 != np && a2 != np && a3 != np && a4 != np && a5 != np && a1 < a2 && a2 < a3 && a3 < a4 && a4 < a5 &&
             lr_count(ln, "gLutAttempted = 0u;") == 1u && lr_count(src, "gLutAttempted = 0u;") == 3u &&
             lr_count(ln, "gLutHave = ") == 0u && lr_count(ln, "gLutKicked") == 1u ? 1u : 0u, 1u);
    const std::string pk = rh_body(src, "static __attribute__((noinline)) void lr107_peek(");
    const std::string rr = rh_body(src, "static __attribute__((noinline)) uint32_t lr107_read_rec(");
    expect_u("107 R7 PIN SHADOW writes nothing: the peek and its read call no learn, no write, no thread, no table",
             !pk.empty() && !rr.empty() && lr_count(pk + rr, "gfxsrc_lut_learn") + lr_count(pk + rr, "write") +
             lr_count(pk + rr, "kernel_thread_start") + lr_count(pk + rr, "gLutTable") + lr_count(pk + rr, "gLutHave") +
             lr_count(pk + rr, "gLutAttempted") == 0u ? 1u : 0u, 1u);
    const std::string ct = rh_body(src, "static uint32_t gfxsrc_commit_try(");
    const size_t g1 = ct.find("    c.lut_ready  = gLutReady ? 1u : 0u;\n");
    const size_t g2 = ct.find("    if (lr107_mode() == N48_LR_ON) lr107_gate(c.lut_switch, &c.lut_plane, &c.lut_ready);\n");
    const size_t g3 = ct.find("n48_cm_gate(&c,");
    expect_u("107 R7 PIN the gate inputs: right after switch 32's three flags, before the gate, ON only, once",
             g1 != np && g2 != np && g3 != np && g1 < g2 && g2 < g3 && lr_count(src, "lr107_gate(") == 2u ? 1u : 0u, 1u);
    const std::string gt = rh_body(src, "static __attribute__((noinline)) void lr107_gate(");
    expect_u("107 R7 PIN lr107_gate asks n48_lr_gate with THIS frame's plane fact and gLutHave",
             lr_count(gt, "n48_lr_gate(lr107_mode(), lutSwitch, lr107_plane_now(), gLutHave, lutPlane, lutReady)") == 1u ? 1u : 0u, 1u);
    const std::string pp = rh_body(src, "static int gfxsrc_pgm_profile(");
    const size_t p1 = pp.find("if (gFs.plane_on && stage == 0u && gfxsrc_id_is_plane((int)out->ps_id)) gXdBuild.plane = 1u;");
    const size_t p2 = pp.find("    if (stage == 0u) lr107_note_ps((int)out->ps_id);");
    expect_u("107 R7 PIN the GPUPass fact: stamped in gfxsrc_pgm_profile beside switch 35's, from the same ps_id, every mode",
             p1 != np && p2 != np && p1 < p2 && lr_count(src, "lr107_note_ps(") == 2u ? 1u : 0u, 1u);
    const std::string df = rh_body(src, "static uint32_t gfxsrc_decide_frame(");
    const size_t c1 = df.find("        gLrF.cb0 = tgtN ? tgtVa : 0ull; gLrF.cb0Frame = gXdC.judged + 1u;");
    const size_t c2 = df.find("gfxsrc_policy(vm, &f, f.ib[0].got, ib0Va, dp, dkey, arm, boundCtx, wsBound);");
    const size_t c3 = df.find("    n550_frame_end(commitOk ? 1u : 0u, pShape);");
    const size_t c4 = df.find("? gfxsrc_commit_try(vm, info, ib0Va, f.ib[0].got, f.nib, arm, verdict, stampNow, tgtVa) : 0u;");
    expect_u("107 R7 PIN the CB0 fact is stamped BEFORE the policy pass; the frame end runs AFTER the gate answered",
             c1 != np && c2 != np && c3 != np && c4 != np && c1 < c2 && c4 < c3 && lr_count(src, "n550_frame_end(") == 2u ? 1u : 0u, 1u);
    const std::string pn = rh_body(src, "static __attribute__((noinline)) uint32_t lr107_plane_now()");
    const std::string sb = rh_body(src, "static __attribute__((noinline)) uint32_t lr107_set_build(uint64_t *set)");
    expect_u("107 R7 PIN the plane set: GPUPass THIS frame, CB0 THIS frame; present73 slots, the fill members, the learner's CB0s",
             lr_count(pn, "if (gLrF.gpFrame != fr || gLrF.cb0Frame != fr) return 0u;") == 1u &&
             lr_count(pn, "const uint32_t n = lr107_set_build(gLrSet);") == 1u &&
             lr_count(pn, "return n48_lr_plane_frame(1u, n48_lr_in_set(gLrF.cb0, gLrSet, n));") == 1u &&
             lr_count(sb, "gP73.s[i].va") == 1u && lr_count(sb, "gFs.member_va[m]") == 1u && lr_count(sb, "gFsL.va[i]") == 1u &&
             lr_count(sb, "n++;") == 3u && lr_count(sb, "if (set) set[n] = ") == 3u ? 1u : 0u, 1u);
    const std::string fe = rh_body(src, "static __attribute__((noinline)) void n550_frame_end(");
    expect_u("107 R7 PIN BLACK-RISK is asked in EVERY mode (no switch test before it)",
             lr_count(fe, "if (!n48_lr_black_risk(commitOk, lr107_plane_now(), gLutHave)) return;") == 1u &&
             fe.find("lr107_mode()") > fe.find("n48_lr_black_risk") ? 1u : 0u, 1u);
    const std::string vb = rh_body(src, "    } else if ((arg & 0xffull) == 107ull) {");
    expect_u("107 R7 PIN the switch: OFF at boot; the verb is its only writer; the mid-arm guards; SWITCH-GUARD:107",
             lr_count(src, "static volatile uint32_t gLr107Mode { N48_LR_OFF };") == 1u && lr_count(src, "&gLr107Mode, want") == 1u &&
             lr_count(src, "__atomic_store_n(&gLr107Mode") == 1u &&
             lr_count(src, "n48_cm_cont_switch_refused(107u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") == 1u &&
             lr_count(src, "const bool armRefused107 = m != 0u && hw_cm_armed() != 0u;") == 1u &&
             n48_cm_cont_switch_guarded(107u) == 1u ? 1u : 0u, 1u);
}


// =====================================================================================================================
// build 0.0.551 (the 0.0.550 review): switch 107's MUST-FIXes and SHOULDs.
//   M1  the slot-blind counts: a committed GPUPass frame with no LUT, or with a LUT no input sampled, WHATEVER the slot set
//   M2  ON with an EMPTY plane-slot set learns exactly as OFF; once a slot exists ON retries as 0.0.550
//   S3  a ready flag tagged with an earlier scope never admits under ON (through the REAL gate); OFF and SHADOW unchanged
//   S4  ON learns only from a GPUPass SEGMENT of a GPUPass plane frame
//   P   source pins for each
// =====================================================================================================================

// =====================================================================================================================
// build 0.0.552 ( PLAN (2)/(3); apple/gfx_bb552.h) — SWITCHES 109 "rp109" AND 110 "an110": the verbs' modes, the
// three lines' widths at worst-case values, and the glue pins (defaults OFF, one writer each, the mid-arm guards, the two record
// sites and their order, the latch beside 66's, the counts in the flagged block, no 0.0.551 record call left in the hook).
static void bb552_checks(const char *ahh)
{
    std::printf("== 0.0.552: switches 109 and 110 ==\n");
    expect_u("552 M -> mode: 1 ON, 2 OFF, 3 SHADOW, 0 / 4 / 255 refused (MODES)",
             (n48_bb_mode_of_m(1u) == N48_BB_ON && n48_bb_mode_of_m(2u) == N48_BB_OFF && n48_bb_mode_of_m(3u) == N48_BB_SHADOW &&
              n48_bb_mode_of_m(0u) == N48_BB_MODES && n48_bb_mode_of_m(4u) == N48_BB_MODES && n48_bb_mode_of_m(255u) == N48_BB_MODES) ? 1u : 0u, 1u);
    expect_u("552 operator values: 109 ON 365 OFF 621 SHADOW 877; 110 ON 366 OFF 622 SHADOW 878",
             (109u | 1u << 8) == 365u && (109u | 2u << 8) == 621u && (109u | 3u << 8) == 877u &&
             (110u | 1u << 8) == 366u && (110u | 2u << 8) == 622u && (110u | 3u << 8) == 878u ? 1u : 0u, 1u);
    expect_u("552 109's mode -> n48_rp_record_x's `on`: ON 1, SHADOW 2, OFF 0",
             (n48_rp109_on_arg(N48_BB_ON) == 1u && n48_rp109_on_arg(N48_BB_SHADOW) == 2u && n48_rp109_on_arg(N48_BB_OFF) == 0u &&
              n48_rp109_on_arg(N48_BB_MODES) == 0u) ? 1u : 0u, 1u);
    {
        const unsigned long long M = ~0ull; const uint32_t U = 0xFFFFFFFFu;
        char b[2048];
        const int w1 = std::snprintf(b, sizeof b, N48_RP109_FMT, "SHADOW", " - `gfxneuter 109` REFUSED (unknown M), unchanged", U, U, U, M, M, U, M, M);
        const int w2 = std::snprintf(b, sizeof b, N48_RP109_FULL_FMT, M, M, U, M, U, U, U, U, U, U, U, "OFF (default)", M, U, U);
        const int w3 = std::snprintf(b, sizeof b, N48_AN110_FMT, "OFF (default)", " - INERT: switch 66 or 60 is OFF",
                                     " - `gfxneuter 110` CHANGED BY THIS VERB (counters reset)", M, M, M, M, M, M, M);
        std::printf("      widths: rp109 %d, rp109 FULL %d, an110 %d (cap %u)\n", w1, w2, w3, N48_LOG_CAP_BODY);
        expect_u("552 the three lines fit the log body at worst-case values", (w1 > 0 && w2 > 0 && w3 > 0 && (unsigned)w1 <= N48_LOG_CAP_BODY &&
                 (unsigned)w2 <= N48_LOG_CAP_BODY && (unsigned)w3 <= N48_LOG_CAP_BODY) ? 1u : 0u, 1u);
        expect_u("552 the table: storage 128 >= capacity ON 128 > OFF 32", (N48_RP_MAX >= N48_RP_CAP_ON && N48_RP_CAP_ON == 128u &&
                 N48_RP_CAP_OFF == 32u) ? 1u : 0u, 1u);
    }
    const std::string src = rh_slurp(ahh);
    const size_t np = std::string::npos;
    if (src.empty()) { expect_u("552 PIN: the hook source was read", 0u, 1u); return; }
    const char *c = src.c_str();
    expect_u("552 PIN switch 109: OFF at boot, the verb its only writer, both mid-arm guards, the selector once",
             (pin_count(c, "static volatile uint32_t gRp109Mode { N48_BB_OFF };") == 1u && pin_count(c, "__atomic_store_n(&gRp109Mode") == 1u &&
              pin_count(c, "gRp109Mode = ") == 0u &&
              pin_count(c, "n48_cm_cont_switch_refused(109u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") == 1u &&
              pin_count(c, "const bool armRefused109 = m != 0u && hw_cm_armed() != 0u;") == 1u &&
              pin_count(c, "(arg & 0xffull) == 109ull") == 1u && n48_cm_cont_switch_guarded(109u) == 1u) ? 1u : 0u, 1u);
    expect_u("552 PIN switch 110: OFF at boot, the verb its only writer, both mid-arm guards, the selector once",
             (pin_count(c, "static volatile uint32_t gAn110Mode { N48_BB_OFF };") == 1u && pin_count(c, "__atomic_store_n(&gAn110Mode") == 1u &&
              pin_count(c, "gAn110Mode = ") == 0u &&
              pin_count(c, "n48_cm_cont_switch_refused(110u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") == 1u &&
              pin_count(c, "const bool armRefused110 = m != 0u && hw_cm_armed() != 0u;") == 1u &&
              pin_count(c, "(arg & 0xffull) == 110ull") == 1u && n48_cm_cont_switch_guarded(110u) == 1u) ? 1u : 0u, 1u);
    // the two record sites: both through n48_rp_record_x with 109's mode and the epoch NOW, each followed by the FULL line; 0.0.551's
    // plain record is gone from the hook (a site left on it would ignore 109)
    const std::string nc = rh_body(src, "uint32_t hw_resprov_note_copy(n48_rp_copy *c, int32_t pid, uint64_t *ctxOut, uint64_t copyNo) {");
    const std::string dr = rh_body(src, "static void gfxsrc_rp_drain_locked() {");
    const size_t a1 = nc.find("if (!IOLockTryLock(gXdLock)) {"), a2 = nc.find("n48_rp_rebind(&gXdRp, wsKey);");
    const size_t a3 = nc.find("const uint32_t why = n48_rp_record_x(&gXdRp, c, n48_rp109_on_arg(rp109_mode()), gXdDpEpoch);");
    const size_t a4 = nc.find("if (why == N48_RP_REC_FULL) rp109_full_line(c);");
    const size_t a5 = nc.find("IOLockUnlock(gXdLock);", a3 == np ? 0 : a3);
    expect_u("552 PIN hw_resprov_note_copy: lock taken -> rebind -> record_x(109, epoch now) -> FULL line -> unlock",
             (a1 != np && a2 != np && a3 != np && a4 != np && a5 != np && a1 < a2 && a2 < a3 && a3 < a4 && a4 < a5) ? 1u : 0u, 1u);
    const size_t b1 = dr.find("if (!n48_rp_pend_fresh(&it[i], &gXdRpClr, &rw)) {"), b2 = dr.find("n48_rp_pend_who(&it[i],");
    const size_t b3 = dr.find("const uint32_t why = n48_rp_record_x(&gXdRp, &it[i].c, n48_rp109_on_arg(rp109_mode()), gXdDpEpoch);");
    const size_t b4 = dr.find("if (why == N48_RP_REC_FULL) rp109_full_line(&it[i].c);");
    expect_u("552 PIN gfxsrc_rp_drain_locked: freshness -> who -> record_x(109, epoch now) -> FULL line",
             (b1 != np && b2 != np && b3 != np && b4 != np && b1 < b2 && b2 < b3 && b3 < b4) ? 1u : 0u, 1u);
    expect_u("552 PIN no 0.0.551 record call is left on the kext's table, and record_x is called at exactly the two sites",
             (pin_count(c, "n48_rp_record(&gXdRp") == 0u && pin_count(c, "n48_rp_record_x(&gXdRp") == 2u &&
              pin_count(c, "rp109_full_line(") == 3u) ? 1u : 0u, 1u);
    const std::string fl = rh_body(src, "static __attribute__((noinline)) void rp109_full_line(const n48_rp_copy *c)");
    expect_u("552 PIN the FULL line is log-only and capped: no store to the table, the cap before the HWLOG",
             (!fl.empty() && fl.find("gXdRp.n =") == np && fl.find("gXdRp.e[") == np && fl.find("n48_rp_drop_at") == np &&
              fl.find("if (gRp109Lines >= N48_RP109_FULL_LINES) return;") != np &&
              fl.find("if (gRp109Lines >= N48_RP109_FULL_LINES) return;") < fl.find("HWLOG(N48_RP109_FULL_FMT,")) ? 1u : 0u, 1u);
    // 110: the latch right after 66's, read once per pass; the counts in 66's flagged block, after de515, before the backstop
    const std::string pol = rh_body(src, "static void gfxsrc_policy(");
    const size_t l1 = pol.find("gXdBuild.deRows = gDrawElideOn & 0x7u;");
    const size_t l2 = pol.find("gXdBuild.deRows = n48_an110_rows(an110_mode(), gXdBuild.deRows);");
    const size_t l3 = pol.find("if (gXdBuild.deRows && gDccStripOn) { ex.flags |= XLAT12_EXTRA_DRAW_ELIDE; ex.draw_elide_rows = gXdBuild.deRows;");
    const size_t l4 = pol.find("de515_seg(k, ds.draw_elided, ds.de_at, ds.de_va8, ds.de_row);");
    const size_t l5 = pol.find("an110_seg(&ds, ex.draw_elide_rows);");
    const size_t l6 = pol.find("if (!st && n48_cm_de_backstop(xlat12_ib_count_draws(out, olen), ds.draws, ds.draw_elided)) {");
    expect_u("552 PIN policy: 66's latch -> 110's bit beside it -> the flag -> de515 -> an110 counts -> the backstop",
             (l1 != np && l2 != np && l3 != np && l4 != np && l5 != np && l6 != np && l1 < l2 && l2 < l3 && l3 < l4 && l4 < l5 && l5 < l6 &&
              l2 - l1 < 300u) ? 1u : 0u, 1u);
    expect_u("552 PIN policy: 110 is read ONCE per pass (the latch), nowhere else in the policy",
             (l2 != np && pol.find("an110_mode()", l2 + std::strlen("gXdBuild.deRows = n48_an110_rows(an110_mode(), ")) == np &&
              pol.find("gAn110Mode") == np) ? 1u : 0u, 1u);
}

static void lut551_checks(const char *ahh)
{
    std::printf("== 0.0.551: switch 107 must-fixes ==\n");
    // M1
    {
        uint32_t bad = 0u;
        for (uint32_t m = 0u; m < 16u; m++) {
            const uint32_t c = m & 1u, gp = (m >> 1) & 1u, have = (m >> 2) & 1u, lp = (m >> 3) & 1u;
            const uint32_t want = (!c || !gp) ? N48_LR_GP_NONE : !have ? N48_LR_GP_NOLUT : lp ? N48_LR_GP_NONE : N48_LR_GP_OTHERLUT;
            if (n48_lr_gp_class(c, gp, have, lp) != want) bad++;
        }
        expect_u("551 M1 the class truth table (committed, GPUPass draw, gLutHave, lutPlane)", bad, 0u);
        // THE CASE THE REVIEW NAMED: a GPUPass frame whose CB0 is not yet in the slot set (plane 0) commits with no LUT.
        const uint32_t plane = n48_lr_plane_frame(1u, 0u);
        expect_u("551 M1 a GPUPass frame outside the slot set: BLACK-RISK is blind to it (0.0.550)", n48_lr_black_risk(1u, plane, 0u), 0u);
        expect_u("551 M1 ... the slot-blind count sees it: GP-NO-LUT", n48_lr_gp_class(1u, 1u, 0u, 0u), N48_LR_GP_NOLUT);
        expect_u("551 M1 a GPUPass commit with a LUT that no input sampled: GP-OTHER-LUT", n48_lr_gp_class(1u, 1u, 1u, 0u),
                 N48_LR_GP_OTHERLUT);
        expect_u("551 M1 an uncommitted or non-GPUPass frame is never counted",
                 n48_lr_gp_class(0u, 1u, 0u, 0u) + n48_lr_gp_class(1u, 0u, 0u, 0u) + n48_lr_gp_class(1u, 0u, 1u, 0u), 0u);
    }
    // M2
    {
        uint32_t diff = 0u;
        for (uint32_t att = 0u; att < 2u; att++)
            for (uint32_t pl = 0u; pl < 2u; pl++)
                for (uint32_t sg = 0u; sg < 2u; sg++)
                    for (uint32_t t = 0u; t <= N48_LR_TRIES_MAX + 1u; t++)
                        if (n48_lr_learn_act(N48_LR_ON, att, pl, t, 0u, sg) != n48_lr_learn_act(N48_LR_OFF, att, pl, t, 0u, sg)) diff++;
        expect_u("551 M2 ON with an EMPTY plane-slot set acts exactly as OFF over every input", diff, 0u);
        expect_u("551 M2 ON, empty set, the first plane-shaped frame: LEGACY (0.0.549's one-shot learn), not a skip",
                 n48_lr_learn_act(N48_LR_ON, 0u, 0u, 0u, 0u, 0u), N48_LR_ACT_LEGACY);
        const uint32_t pl[] = { 0u, 0u, 0u }, gd[] = { 0u, 1u, 1u }, sn0[] = { 0u, 0u, 0u }, gp1[] = { 1u, 1u, 1u };
        uint32_t tO = 0u, tN = 0u;
        const int off = lr_model2(N48_LR_OFF, pl, gd, sn0, gp1, 3u, &tO), on = lr_model2(N48_LR_ON, pl, gd, sn0, gp1, 3u, &tN);
        expect_u("551 M2 model [bad, good, good] with an empty set: ON's answer equals OFF's (both spend the one shot on frame 0)",
                 (uint64_t)(int64_t)on * 1000u + tN, (uint64_t)(int64_t)off * 1000u + tO);
        const uint32_t pl2[] = { 0u, 1u, 1u }, gd2[] = { 1u, 0u, 1u }, sn2[] = { 0u, 2u, 2u };
        const uint32_t gd3[] = { 0u, 0u, 1u };
        expect_u("551 M2 model [empty-set good, ...]: ON learns on frame 0 like OFF",
                 (uint64_t)(int64_t)lr_model2(N48_LR_ON, pl2, gd2, sn2, gp1, 3u, &tN), 0u);
        expect_u("551 M2 model [empty-set bad, slot bad, slot good]: once a slot exists ON retries and learns frame 2",
                 (uint64_t)(int64_t)lr_model2(N48_LR_ON, pl2, gd3, sn2, gp1, 3u, &tN) * 16u + tN, 2u * 16u + 2u);
        expect_u("551 M2 ... OFF over the same scope never learns (its one shot went on frame 0)",
                 (uint64_t)(int64_t)lr_model2(N48_LR_OFF, pl2, gd3, sn2, gp1, 3u, &tO), (uint64_t)(int64_t)-1);
    }
    // S4
    {
        uint32_t seg = 0u;
        for (uint32_t att = 0u; att < 2u; att++)
            for (uint32_t t = 0u; t < N48_LR_TRIES_MAX; t++)
                if (n48_lr_learn_act(N48_LR_ON, att, 1u, t, 3u, 0u) != N48_LR_ACT_SKIP_SEG) seg++;
        expect_u("551 S4 ON, a plane frame whose SEGMENT is not ws_D_GPUPass: never learned from (SKIP_SEG), every attempted/tries", seg, 0u);
        const uint32_t pl[] = { 1u, 1u }, gd[] = { 1u, 1u }, sn[] = { 3u, 3u }, sg[] = { 0u, 1u };
        uint32_t t = 0u;
        expect_u("551 S4 model [plane frame / non-GPUPass segment, plane frame / GPUPass segment]: ON learns frame 1 in one try",
                 (uint64_t)(int64_t)lr_model2(N48_LR_ON, pl, gd, sn, sg, 2u, &t) * 16u + t, 1u * 16u + 1u);
    }
    // S3 through the REAL gate
    {
        n48_cm_frame c; uint32_t d = 0u;
        good_frame(c, 1040u, 3u);
        c.lut_switch = 1u; c.lut_plane = 1u; c.lut_ready = 1u;   /* a LUT sampled, ready 1 set by the PREVIOUS scope's thread */
        expect_u("551 S3 baseline (0.0.550): a stale ready admits the plane frame", gGate(&c, &d), N48_CM_OK);
        uint32_t r = c.lut_ready;
        expect_u("551 S3 ON: a ready tagged with scope 4 under scope 5 is lowered", n48_lr_ready_scope(N48_LR_ON, 4u, 5u, &r) * 16u + r, 16u);
        c.lut_ready = r;
        expect_u("551 S3 ON: ... and the REAL gate refuses it LUT-NOT-READY", gGate(&c, &d), N48_CM_LUT_NOT_READY);
        r = 1u;
        expect_u("551 S3 ON: a ready tagged with THIS scope stands", n48_lr_ready_scope(N48_LR_ON, 5u, 5u, &r) * 16u + r, 1u);
        uint32_t offCh = 0u, raised = 0u;
        for (uint32_t mode = 0u; mode < 3u; mode++)
            for (uint32_t m = 0u; m < 8u; m++) {
                uint32_t rr = m & 1u;
                const uint32_t ch = n48_lr_ready_scope(mode, (m >> 1) & 1u, (m >> 2) & 1u, &rr);
                if (rr > (m & 1u)) raised++;
                if (mode != N48_LR_ON && (ch || rr != (m & 1u))) offCh++;
            }
        expect_u("551 S3 refusal only: ready is never raised", raised, 0u);
        expect_u("551 S3 OFF and SHADOW never change ready", offCh, 0u);
    }
    // P: source pins
    const std::string src = rh_slurp(ahh);
    expect_u("551 P the hook source was read", src.empty() ? 0u : 1u, 1u);
    if (src.empty()) return;
    const size_t np = std::string::npos;
    const std::string fe = rh_body(src, "static __attribute__((noinline)) void n550_frame_end(");
    const size_t f1 = fe.find("    const uint32_t gpc = n48_lr_gp_class(commitOk, gLrF.gpFrame == fr ? 1u : 0u, gLutHave ? 1u : 0u, gXdBuild.lutPlane ? 1u : 0u);\n");
    const size_t f2 = fe.find("    if (gpc != N48_LR_GP_NONE) lr107_gp_note(gpc, fr);\n");
    const size_t f3 = fe.find("    if (!commitOk || gLutHave) return;");
    expect_u("551 P M1: the class is asked from THIS frame's GPUPass fact (no slot test, no switch test), BEFORE the gLutHave return",
             f1 != np && f2 != np && f3 != np && f1 < f2 && f2 < f3 && fe.substr(0, f3).find("lr107_plane_now") == np &&
             fe.substr(0, f3).find("lr107_mode") == np && lr_count(src, "lr107_gp_note(") == 2u ? 1u : 0u, 1u);
    const std::string gn = rh_body(src, "static __attribute__((noinline)) void lr107_gp_note(uint32_t gpc, uint64_t fr)");
    expect_u("551 P M1: the note counts before its line cap, both kinds",
             gn.find("gLr.gpNoLut++;") != np && gn.find("gLr.gpNoLut++;") < gn.find("if (gLr.gpNoLines >= N48_LR_GP_LINES) return;") &&
             gn.find("gLr.gpOtherLut++;") != np && gn.find("gLr.gpOtherLut++;") < gn.find("if (gLr.gpOtherLines >= N48_LR_GP_LINES) return;")
             ? 1u : 0u, 1u);
    const std::string rl = rh_body(src, "static void lr107_report_line(const char *how)");
    expect_u("551 P M1/M2: the counts line (with the set size) is printed by the verb's report and at every continuous STOP",
             lr_count(rl, "lr107_counts_line(\"\");") == 1u && lr_count(src, "static void lr107_arm_stop() { lr107_counts_line(\" - CONTINUOUS STOP\"); }") == 1u &&
             lr_count(src, "        lr107_arm_stop();                 // build 0.0.551") == 1u ? 1u : 0u, 1u);
    const std::string ln = rh_body(src, "static __attribute__((noinline)) void lr107_learn(const GfxcVm &vm, uint64_t imgVa)");
    const size_t l1 = ln.find("const uint32_t setN = lr107_set_build(nullptr);");
    const size_t l2 = ln.find("const uint32_t segGp = (gLrF.psFrame == fr && gfxsrc_id_is_plane(gLrF.psId)) ? 1u : 0u;");
    const size_t l3 = ln.find("const uint32_t act = n48_lr_learn_act(mode, gLutAttempted, plane, gLr.tries, setN, segGp);");
    expect_u("551 P M2/S4: the learn is asked with the live set size and THIS segment's GPUPass fact",
             l1 != np && l2 != np && l3 != np && l1 < l3 && l2 < l3 ? 1u : 0u, 1u);
    expect_u("551 P M2: the refused-record line carries the set size", lr_count(ln, "(unsigned long long)gLr.refused, setN);") == 1u ? 1u : 0u, 1u);
    const std::string gt = rh_body(src, "static __attribute__((noinline)) void lr107_gate(");
    expect_u("551 P S3: the gate lowers a ready tagged with another scope (ON only: lr107_gate is reached under ON)",
             lr_count(gt, "if (n48_lr_ready_scope(lr107_mode(), gLutReadyScope, gLutScope, lutReady)) {") == 1u ? 1u : 0u, 1u);
}

// =====================================================================================================================
// build 0.0.553 (; apple/gfx_lutidx111.h) — SWITCH 111 "lutidx": the lines' widths and the glue pins. The
// decisions themselves (AX idx 3 learned, AW idx 4 == OFF, BB's entry-4 plane never learned, VA mismatch refused, SHADOW never
// selects, OFF identity) are driven from the measured records in tests/gfx_lutfill_test.cpp section 111; these pins prove the
// hook composes exactly those pure calls, in that order, and nothing else.
//   L1  widths: the verb line, the ON skip line, the SHADOW peek line <= 491 at worst case
//   L2  the switch: OFF at boot, one writer, both mid-arm guards, SWITCH-GUARD:111, 114 still unclaimed (112 is 0.0.554's), ONE verb line
//   L3  the selection: set only in li111_pick's USE branch, cleared there and after the learn call; the learn reads through it
//   L4  the pick: GPUPass by the ABI row AND by the segment's program; the draw's own export (in_mode/in_va/in_idx); SKIP -> 0
//   L5  SHADOW writes nothing: the peek reads one record through lr107_read_rec at the selection's offset and never learns
//   L6  107's reads and lines follow the same entry (li111_learn_off / li111_learn_idx)
// =====================================================================================================================
static void lutidx111_checks(const char *ahh)
{
    std::printf("== 0.0.553: switch 111 ==\n");
    {
        const unsigned long long M = ~0ull; const uint32_t U = 0xFFFFFFFFu;
        char b[2048];
        const int w1 = std::snprintf(b, sizeof b, N48_LI_FMT, "OFF (default)", "OFF - 111 does nothing",
                                     " - `gfxneuter 111` CHANGED BY THIS VERB (counters reset)", U, U, U, U, U, U, U, U, U, U, U, U, U);
        const int w2 = std::snprintf(b, sizeof b, N48_LI_SKIP_FMT, M, "input-list-incomplete", U, U, U, M, U, U);
        const int w3 = std::snprintf(b, sizeof b, N48_LI_PEEK_FMT, M, "REFUSE this record", U, M, U, U, U, U, U, M,
                                     "decoded, this instrument's shape, the draw's own LUT (pages not walked in SHADOW)", U, U);
        std::printf("      widths: lutidx111 %d, skip %d, peek %d (cap %u)\n", w1, w2, w3, N48_LOG_CAP_BODY);
        expect_u("111 L1 the three lines fit the log body at worst-case values", (w1 > 0 && w2 > 0 && w3 > 0 && (unsigned)w1 <= N48_LOG_CAP_BODY &&
                 (unsigned)w2 <= N48_LOG_CAP_BODY && (unsigned)w3 <= N48_LOG_CAP_BODY) ? 1u : 0u, 1u);
    }
    const std::string src = rh_slurp(ahh);
    const size_t np = std::string::npos;
    if (src.empty()) { expect_u("111 PIN: the hook source was read", 0u, 1u); return; }
    // L2
    std::string vb;
    {   // the verb's own block: from its selector to the next selector in the chain
        const size_t v0 = src.find("    } else if ((arg & 0xffull) == 111ull) {");
        const size_t v1 = v0 != np ? src.find("    } else if ((arg & 0xffull) == ", v0 + 10u) : np;
        if (v0 != np && v1 != np) vb = src.substr(v0, v1 - v0);
    }
    expect_u("111 L2 the switch: OFF at boot; the verb is its only writer; both mid-arm guards; SWITCH-GUARD:111; the selector once",
             lr_count(src, "static volatile uint32_t gLi111Mode { N48_LI_OFF };") == 1u && lr_count(src, "__atomic_store_n(&gLi111Mode, want, __ATOMIC_RELEASE);") == 1u &&
             lr_count(src, "__atomic_store_n(&gLi111Mode") == 1u && lr_count(src, "gLi111Mode = ") == 0u &&
             lr_count(src, "} else if ((arg & 0xffull) == 111ull) {") == 1u && !vb.empty() &&
             lr_count(src, "n48_cm_cont_switch_refused(111u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") == 1u &&
             lr_count(src, "const bool armRefused111 = m != 0u && hw_cm_armed() != 0u;") == 1u &&
             n48_cm_cont_switch_guarded(111u) == 1u && n48_cm_cont_switch_guarded(114u) == 0u &&
             n48_cm_cont_switch_refused(111u, 0u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 1u &&
             n48_cm_cont_switch_refused(111u, 1u, 1u, (uint32_t)N48_CM_SHOT_ARMED) == 0u ? 1u : 0u, 1u);
    const std::string rl = rh_body(src, "static void li111_report_line(const char *how)");
    expect_u("111 L2 the verb prints exactly ONE line (li111_report_line, one HWLOG); the counters reset only by the verb",
             lr_count(vb, "li111_report_line(how111);") == 1u && lr_count(vb, "HWLOG(") == 0u && lr_count(rl, "HWLOG(") == 1u &&
             lr_count(src, "li111_report_line(") == 2u && lr_count(src, "gLi = {};") == 1u && lr_count(vb, "gLi = {};") == 1u ? 1u : 0u, 1u);
    // L3
    const std::string pk = rh_body(src, "static __attribute__((noinline)) uint32_t li111_pick(const GfxcVm &vm, const xlat12_draw_stats *ds)");
    const size_t u1 = pk.find("    gLiSel.use = 0u;\n");
    const size_t u2 = pk.find("    if (act == N48_LI_ACT_USE) {\n        gLiSel = sel;\n");
    expect_u("111 L3 the selection is set ONLY in li111_pick's USE branch (after clearing it), and nowhere else",
             u1 != np && u2 != np && u1 < u2 && lr_count(src, "gLiSel = ") == 1u && lr_count(src, "gLiSel.use = ") == 2u &&
             lr_count(src, "gLiSel.off") + lr_count(src, "gLiSel.va") + lr_count(src, "gLiSel.idx") == 0u &&
             lr_count(src, "static n48_li_sel gLiSel {};") == 1u ? 1u : 0u, 1u);
    expect_u("111 L3 the learn reads through the selection: learn_off / learn_idx / accept are the pure header's",
             lr_count(src, "static inline uint64_t li111_learn_off() { return n48_li_learn_off(&gLiSel); }") == 1u &&
             lr_count(src, "static inline uint32_t li111_learn_idx() { return n48_li_learn_idx(&gLiSel); }") == 1u &&
             lr_count(src, "static inline uint32_t li111_accept(const uint32_t *rec, const n48_lut_desc *d) { return n48_li_accept_sel(&gLiSel, rec, d); }") == 1u &&
             lr_count(src, "li111_accept(rec, &d)") == 1u && lr_count(src, "li111_accept(gLrRec, &d)") == 1u ? 1u : 0u, 1u);
    const std::string pol = rh_body(src, "static void gfxsrc_policy(");
    const size_t s1 = pol.find("                if (li111_mode() == N48_LI_OFF || li111_pick(vm, &ds)) {\n");
    const size_t s2 = s1 != np ? pol.find("gfxsrc_lut_learn(vm, ds.in_img_va);", s1) : np;
    const size_t s3 = s2 != np ? pol.find("lr107_learn(vm, ds.in_img_va);", s2) : np;
    const size_t s4 = s3 != np ? pol.find("                gLiSel.use = 0u;", s3) : np;
    expect_u("111 L3 the learn site: OFF short-circuits the pick; both learn calls inside it; the selection cleared after them",
             s1 != np && s2 != np && s3 != np && s4 != np && lr_count(src, "li111_pick(vm, &ds)") == 1u ? 1u : 0u, 1u);
    // L4
    const size_t k1 = pk.find("const uint32_t gpSeg = (gLrF.psFrame == fr && gfxsrc_id_is_plane(gLrF.psId)) ? 1u : 0u;");
    const size_t k2 = pk.find("const uint32_t gpAbi = (ds->in_abi != 0u && ds->in_abi == xlat12_table_abi_find(N48_LI_GP_NDW, N48_LI_GP_FNV)) ? 1u : 0u;");
    const size_t k3 = pk.find("n48_li_pick(mode, gpSeg & gpAbi, ds->in_n, ds->in_over, ds->in_mode, ds->in_va, ds->in_idx, &sel);");
    const size_t k4 = pk.find("    if (act == N48_LI_ACT_SKIP) {");
    const size_t k5 = k4 != np ? pk.find("        return 0u;\n", k4) : np;
    expect_u("111 L4 the pick: GPUPass by segment AND by ABI row, then the pure pick on the draw's own export; SKIP returns 0 (no learn)",
             k1 != np && k2 != np && k3 != np && k4 != np && k5 != np && k1 < k3 && k2 < k3 && k3 < k4 &&
             lr_count(pk, "return 0u;") == 1u ? 1u : 0u, 1u);
    // L5
    const std::string pe = rh_body(src, "static __attribute__((noinline)) void li111_peek(const GfxcVm &vm, uint64_t imgVa, const n48_li_sel *sel, uint64_t fr)");
    expect_u("111 L5 SHADOW writes nothing: the peek reads through lr107_read_rec at sel->off and never learns, writes, kicks or selects",
             !pe.empty() && lr_count(pe, "lr107_read_rec(vm, imgVa, sel->off, &fl)") == 1u &&
             lr_count(pe, "gfxsrc_lut_learn") + lr_count(pe, "write") + lr_count(pe, "kernel_thread_start") + lr_count(pe, "gLutTable") +
             lr_count(pe, "gLutHave") + lr_count(pe, "gLutAttempted") + lr_count(pe, "gLiSel") == 0u &&
             lr_count(pk, "else if (!gLi.wouldLearned && gLi.peeks < N48_LI_PEEKS_MAX) li111_peek(vm, ds->in_img_va, &sel, fr);") == 1u &&
             lr_count(pk, "if (mode == N48_LI_SHADOW) {") == 1u ? 1u : 0u, 1u);
    // L6
    const std::string rr = rh_body(src, "static __attribute__((noinline)) uint32_t lr107_read_rec(");
    const std::string lp = rh_body(src, "static __attribute__((noinline)) void lr107_peek(");
    expect_u("111 L6 107's record reads and lines follow the learn's entry; lr107_read_rec reads exactly imgVa + off",
             lr_count(rr, "gfxc_read(vm, imgVa + off, gLrRec, N48_LUT_RECORD_DWORDS, nullptr)") == 1u &&
             lr_count(src, "lr107_read_rec(vm, imgVa, li111_learn_off(), &fl)") == 2u && lr_count(src, "lr107_read_rec(") == 4u &&
             lr_count(src, "li111_learn_idx(),") == 3u &&
             lr_count(lp, "if (why == N48_LUT_OK) why = li111_accept(gLrRec, &d);") == 1u &&
             lp.find("n48_lut_instrument_ok(&d)") < lp.find("li111_accept(gLrRec, &d)") ? 1u : 0u, 1u);
}

int main(int argc, char **argv)
{
    std::printf("== gfx_commit: the COMMIT gate, and the mutant it exists for ==\n");
    all_checks();
    if (argc > 1) source_pins(argv[1]);
    else std::printf("  SKIP source pins (no AppleHardwareHook.cpp argument)\n");
    rehearsal457_checks(argc > 1 ? argv[1] : nullptr);
    if (argc > 1) cselide_wiring_pins(argv[1]);   // build 0.0.487
    if (argc > 1) drawelide_wiring_pins(argv[1]);   // build 0.0.500
    lut107_checks(argc > 1 ? argv[1] : nullptr);   // build 0.0.550 (switch 107)
    lut551_checks(argc > 1 ? argv[1] : nullptr);   // build 0.0.551 (switch 107: the 0.0.550 review's must-fixes)
    bb552_checks(argc > 1 ? argv[1] : nullptr);   // build 0.0.552 (switches 109 and 110)
    lutidx111_checks(argc > 1 ? argv[1] : nullptr);   // build 0.0.553 (switch 111)
    const int realFail = gFail, realRun = gRun;
    std::printf("-- %d check(s), %d failure(s)\n", realRun, realFail);

    struct Mut { const char *what; GateFn fn; KindFn kn; ArmFn af; LevelFn lf; FinishFn ff; DisarmFn df; SpendFn sf; };
    const Mut mut[] = {
        { "M1 the read-back mismatch is IGNORED (the named mutant)", &m1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M2 an unresolvable page does not refuse (target_vram's shape)", &m2, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M3 an un-walked range passes by 0 == 0", &m3, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M4 the multi-IB rung is missing", &m4, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M5 the segments need not tile the IB", &m5, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M6 the frame identity is not re-established", &m6, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M7 a short write counts as a write", &m7, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M8 an unset seg_kind is treated as ENCODER", &m8, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M9 the segment start rule is not enforced (MIXED accepted)", &m9, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M10 HEADLESS without xlat12_ib_segments == 0", &m10, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M11 HEADLESS without total == nseg", &m11, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M12 HEADLESS without the recogniser answering OK", &m12, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M13 seg_kind UNSET is LABELLED ENCODER in the record", nullptr, &m13_kind, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M14 seg_kind HEADLESS is LABELLED ENCODER in the record", nullptr, &m14_kind, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M15 ARMING with a condition missing (N1's rung deleted)", nullptr, nullptr, &m15_arm, nullptr, nullptr, nullptr, nullptr },
        { "M16 the gate ACCEPTS A FRAME WHILE NOT ARMED", &m16, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M17 the ONE-SHOT NEVER DISARMS (finish is a no-op)", nullptr, nullptr, nullptr, nullptr, &m17_finish, nullptr, nullptr },
        { "M18 the DISARM DOES NOT WORK (it reports success)", nullptr, nullptr, nullptr, nullptr, nullptr, &m18_disarm, nullptr },
        { "M19 the ARM SURVIVES A WINDOWSERVER REBIND", nullptr, nullptr, nullptr, &m19_level, nullptr, nullptr, nullptr },
        { "M20 the BUDGET IS CHARGED PER CALL, not per token seq", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &m20_spend },
        { "M21 the ARM SURVIVES ITS BOUND (the budget never exhausts)", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &m21_spend },
        { "M22 a budget of 0 is UNLIMITED (the default is not today)", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &m22_spend },
        { "M23 a DISARM leaves the budget standing", nullptr, nullptr, nullptr, nullptr, nullptr, &m23_disarm, nullptr },
        { "M24 the LUT rung is DELETED (a zero LUT commits)", &m24, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M25 the LUT rung IGNORES THE SWITCH (breaks the default)", &m25, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M26 the LUT rung FIRES ON THE FILL (no frame commits)", &m26, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M27 the fill-set rung is DELETED (a non-fill spends a reserved shot)", &m27, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M28 the fill-set rung IGNORES THE SWITCH (breaks the default)", &m28, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M29 the fill-set rung IGNORES THE WINDOW (the plane is refused forever)", &m29, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M30 the plane rung is DELETED (a non-plane spends the held shot)", &m30, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M31 the plane rung IGNORES THE SWITCH (breaks the default)", &m31, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M32 the plane rung IGNORES THE WINDOW (today's rule never returns)", &m32, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M33 IB 0's evidence is USED FOR EVERY IB (entry 0 only)", &m33, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M34 the MULTI_IB rung IGNORES THE SWITCH (nib > 1 without mib)", &m34, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M35 the MIB-REQUIRED rung is DELETED (a single-IB frame commits)", &m35, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M36 the MIB-REQUIRED rung is INVERTED (refuses two-IB, admits single-IB)", &m36, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M37 the COMPUTE-ELIDE-R1 rung is DELETED (0.0.487)", &m37, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M38 COMPUTE-ELIDE-R1 asks md_ok without ENFORCE (0.0.487)", &m38, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M39 the DRAW-ELIDE-R1 rung is DELETED (0.0.500)", &m39, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        { "M40 DRAW-ELIDE-R1 asks md_ok without ENFORCE (0.0.500)", &m40, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
    };
    int caught = 0;
    for (const Mut &m : mut) {
        gQuiet = 1; gFail = 0; gRun = 0;
        gGate = m.fn ? m.fn : &n48_cm_gate;
        gKindName = m.kn ? m.kn : &n48_cm_kind_name;
        gArmMissing = m.af ? m.af : &n48_cm_arm_missing;
        gShotLevel = m.lf ? m.lf : &n48_cm_shot_level;
        gShotFinish = m.ff ? m.ff : &n48_cm_shot_finish;
        gShotDisarm = m.df ? m.df : &n48_cm_shot_disarm;
        gShotSpend = m.sf ? m.sf : &n48_cm_shot_spend;
        all_checks();
        const int f = gFail, r = gRun;
        gGate = &n48_cm_gate;
        gKindName = &n48_cm_kind_name;
        gArmMissing = &n48_cm_arm_missing;
        gShotLevel = &n48_cm_shot_level;
        gShotFinish = &n48_cm_shot_finish;
        gShotDisarm = &n48_cm_shot_disarm;
        gShotSpend = &n48_cm_shot_spend;
        gQuiet = 0;
        std::printf("mutant %-56s %s (%d of %d checks fail)\n", m.what, f ? "CAUGHT" : "NOT CAUGHT", f, r);
        if (f) caught++;
    }
    const int nmut = (int)(sizeof(mut) / sizeof(mut[0]));
    std::printf("mutants caught %d/%d\n", caught, nmut);
    std::printf("%s\n", (realFail == 0 && caught == nmut) ? "gfx_commit: PASS" : "gfx_commit: FAIL");
    return (realFail || caught != nmut) ? 1 : 0;
}
