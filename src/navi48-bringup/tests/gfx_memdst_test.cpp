// gfx_memdst_test.cpp — R1's own tests (notes/design/R1-MEMDST.md Q3,  (B),). Pure C++,
// links the REAL translator so T1 runs over REAL captured producer bytes, exactly as gfx_dep_test.cpp's C5 does
// for D4'.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 -I src/xlat12/tests -x c++ \
//         src/navi48-bringup/tests/gfx_memdst_test.cpp src/xlat12/xlat12.c src/xlat12/xlat12_ib.c \
//         src/xlat12/xlat12_headless.c -o /tmp/mdtest && /tmp/mdtest
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "gfx_memdst.h"
#include "xlat12.h"
#include "xlat12_ib.h"
#include "xlat12_headless.h"
#include "fixture_wsgc1_headless.h"
#include "fixture_arm20_window.h"
#include "fixture_mib_f48_f20_f21.h"
#include "fixture_decide38_producer.h"
#include "fixture_compute_n.h"   // build 0.0.487: src/xlat12/tests (on this suite's -I line)
#include "gfx_commit.h"          // build 0.0.487: n48_cm_live / n48_cm_gate, COMPUTE-ELIDE-R1

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-90s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-90s %#llx\n", what, (unsigned long long)got);
}

// The kext's own real geometry (AppleHardwareHook.cpp: `ownRegionBase = vm.startVa + (511ull << 28)`, `startVa`
// falls back to 0x400000000 whenever no live discovery has run - gfx_capture_synth.cpp uses the SAME constant for
// its own synthetic vm). The fence slot sits INSIDE this window (N48_F828_FENCE_PAGE_OFF, gfx_fence828.h) - R1's
// own exemption is exercised precisely because a legitimate fence destination otherwise falls inside OWN-REGION.
static const uint64_t kStartVa       = 0x400000000ull;
static const uint64_t kOwnRegionBase = kStartVa + (511ull << 28);          // 0x23F0000000
static const uint64_t kOwnRegionLen  = 0x10000000ull;                     // 256 MiB
static const uint64_t kFenceSlotVa   = kOwnRegionBase + 0x00A8F000ull;    // N48_F828_FENCE_PAGE_OFF

// ---------------------------------------------------------------------------------------------------------------------
// T0 — R0: THE REACHABILITY FIX, modelled and source-pinned. 0.0.440 tested `gXdBuild.ok`, which is
// reset to 0 at gfxsrc_policy's OWN pass top and set only AFTER the R1 block - so the block could never see anything
// but a stale 0, in EVERY mode, on EVERY frame. 0.0.441 moves the `ok` ASSIGNMENT to right after `gXdBuild.nseg` is
// itself finalised (its formula is unchanged: `build && gXdBuild.nseg`), well before the R1 block, so the SAME
// condition text (`if (gMdMode && build && gXdBuild.ok)`) now reads THIS pass's own fresh value. This is a
// REACHABILITY test, not a literal-line pin (the reviewer's rule since: a pin that asserts a literal line can
// lock a bug in) - it (a) models the exact order of the pass's flag writes and shows the condition is TRUE on a
// successful build, and (b) pins the SOURCE ORDER the model depends on: `nseg` must be assigned, then `ok`, then the
// R1 condition must read it - in that order. Planted breaks: reverting to the 0.0.440 ordering (`ok`'s assignment
// moved back to AFTER the block) breaks (b) directly; reverting to the 0.0.440 CONDITION jointly breaks (b), because
// the moved assignment this test searches for (at the NEW site, by exact text) would then not exist at all. Both
// must FAIL.
// ---------------------------------------------------------------------------------------------------------------------
#include <fstream>
#include <sstream>
#include <string>
static void t0_reachability(const char *srcPath)
{
    // (a) THE MODEL: the exact order of writes gfxsrc_policy performs, replayed here as plain booleans/ints -
    // NOT read from the source text, so this half of the test cannot be fooled by a comment or a renamed variable.
    {
        uint32_t ok = 1u, nseg = 1u;              // a previous pass's leftovers, on purpose (nothing must survive)
        const bool build = true;                   // this pass: gXdNew is available and n is in range
        // 1. gfxsrc_policy's pass-top reset (AHH: "gXdBuild.ok = 0; gXdBuild.nseg = 0; ...").
        ok = 0u; nseg = 0u;
        // 2. the segment stage finds real segments (ns == total > 0) and nseg is finalised.
        nseg = 5u;
        // 3. R0's moved assignment: `ok` is set from THIS pass's own fresh `build`/`nseg`, before the R1 block.
        ok = (build && nseg) ? 1u : 0u;
        // 4. the R1 block's own condition (gMdMode assumed nonzero here - ENFORCE/SHADOW; OFF is a separate, trivial
        //    short-circuit that every OTHER suite re-run unchanged after this build is the real proof of, per T7).
        const bool gMdModeNonzero = true;
        const bool r1Reachable = gMdModeNonzero && build && ok;
        expect_u("T0 REACHABILITY MODEL: on a successful build, the R1 condition evaluates TRUE", r1Reachable ? 1u : 0u, 1u);
    }
    // (b) THE SOURCE ORDER PIN: the actual file, by content (never a bare line number).
    std::ifstream f(srcPath);
    if (!f) { std::printf("SKIP  T0 source order pin: could not open %s\n", srcPath); return; }
    std::stringstream ss; ss << f.rdbuf();
    const std::string s = ss.str();
    const size_t resetPos  = s.find("gXdBuild.ok = 0; gXdBuild.nseg = 0; gXdBuild.n = n;");
    const size_t nsegPos   = s.find("gXdBuild.nseg = (ns == total && ns <= N48_XV_MAX_SEGS) ? ns : 0u;");
    const size_t okPos     = s.find("gXdBuild.ok = build && gXdBuild.nseg ? 1u : 0u;");
    const size_t condPos   = s.find("if (gMdMode && build && gXdBuild.ok) {");
    expect_u("T0 the pass-top reset is found", resetPos != std::string::npos ? 1u : 0u, 1u);
    expect_u("T0 nseg's own assignment is found", nsegPos != std::string::npos ? 1u : 0u, 1u);
    expect_u("T0 ok's assignment (moved by R0) is found", okPos != std::string::npos ? 1u : 0u, 1u);
    expect_u("T0 the R1 block's condition is found", condPos != std::string::npos ? 1u : 0u, 1u);
    expect_u("T0 ORDER: reset < nseg < ok < R1 condition -> REACHABLE",
             (resetPos != std::string::npos && nsegPos != std::string::npos && okPos != std::string::npos &&
              condPos != std::string::npos && resetPos < nsegPos && nsegPos < okPos && okPos < condPos) ? 1u : 0u, 1u);
}

// ---------------------------------------------------------------------------------------------------------------------
// T1 — THE REAL PRODUCERS. Every one of fixture_wsgc1_headless.h's F5/F10, fixture_decide38_producer.h's F9 and
// fixture_arm20_window.h's f4/f9/f12 is a headless-shape (no CONTEXT_CONTROL, xlat12_ib_headless_passes' own
// recogniser) 1456-dword frame carrying a real live triplet per pass on the fence page (R1-MEMDST.md Q1).
// Translated (forcing ps_readset1/vs_readset1 to STRICT-OK-NONE rows, exactly the D4' tests' own technique — the
// programs' real identity is not what this test is about) and scanned: clean, 9 packets, counts equal r4 (3
// waits, 6 writes). R2 (0.0.441): the region boundaries fed to the scan are THIS translation's own pass starts
// (segs[k].start), so the design's own measured -11 dword shift (each pass's triplet compacts EARLIER within its
// OWN region) is exactly what ORIGIN must still accept.
// ---------------------------------------------------------------------------------------------------------------------
struct MdCand { uint32_t out[1456]; uint32_t n; uint32_t r4w, r4m; uint32_t ok; xlat12_ib_segment segs[16]; uint32_t np; };

static uint32_t md_translate_headless(const uint32_t *ib, uint32_t n, MdCand *c)
{
    xlat12_hl_report hl {};
    uint32_t total = 0;
    const uint32_t np = xlat12_ib_headless_passes(ib, n, c->segs, 16u, &total, &hl);
    if (!np || n > 1456u) { c->ok = 0u; return 0u; }
    std::memcpy(c->out, ib, n * 4u);
    c->n = n; c->r4w = 0u; c->r4m = 0u; c->np = np;
    xlat12_draw_profile pf = *xlat12_ib_m2tri_profile();
    pf.ps_readset1 = 2u; pf.vs_readset1 = 5u;   // Const_PS_gfx1201 / RectPosTexFast_VS_gfx1201: STRICT-OK NONE
    for (uint32_t k = 0; k < np; k++) {
        xlat12_draw_extra ex {};
        xlat12_draw_stats ds {};
        uint32_t len = 0;
        const uint32_t st = xlat12_ib_translate_draw_ex(&pf, &ex, &ib[c->segs[k].start], c->segs[k].end - c->segs[k].start,
                                                        &c->out[c->segs[k].start], &len, &ds);
        if (st != 0u) { c->ok = 0u; return 0u; }
        c->r4w += ds.r4_waits; c->r4m += ds.r4_memwrites;
    }
    c->ok = 1u;
    return 1u;
}

static uint32_t md_seg_starts(const MdCand *c, uint32_t *starts)
{
    for (uint32_t k = 0; k < c->np && k < 16u; k++) starts[k] = c->segs[k].start;
    return c->np;
}

// Runs the scan+judge over a translated candidate, treating every destination as RESOLVED+WRITABLE (a real
// gfxc_page walk needs live VRAM; this pins the PURE header's own clauses, exactly as gfx_dep_test.cpp's synthetic
// consumers pin gfx_dep.h without a live vm). ownRegion/ownIb are placed far outside the fence page on purpose, so
// a real producer's own triplet never collides with them by accident.
static uint32_t md_judge_real(const uint32_t *out, uint32_t n, const uint32_t *in, uint32_t r4w, uint32_t r4m,
                              const uint32_t *segStart, uint32_t nSeg, n48_md_scan_result *rOut, uint64_t *detail)
{
    n48_md_scan(out, n, in, n, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, segStart, nSeg, 0ull, 0u, rOut);
    for (uint32_t i = 0; i < rOut->n; i++) { rOut->d[i].resolved = 1u; rOut->d[i].writable = 1u; }
    return n48_md_judge(rOut, r4w, r4m, 0u, detail);
}

static void t1_one(const char *name, const uint32_t *ib, uint32_t n)
{
    MdCand c {};
    char buf[128];
    std::snprintf(buf, sizeof buf, "T1 %s translates", name);
    expect_u(buf, md_translate_headless(ib, n, &c), 1u);
    if (!c.ok) return;
    uint32_t starts[16]; const uint32_t nSeg = md_seg_starts(&c, starts);
    n48_md_scan_result r {}; uint64_t detail = 0ull;
    const uint32_t clause = md_judge_real(c.out, n, ib, c.r4w, c.r4m, starts, nSeg, &r, &detail);
    std::snprintf(buf, sizeof buf, "T1 %s is CLEAN", name);
    expect_u(buf, clause, N48_MD_OK);
    std::snprintf(buf, sizeof buf, "T1 %s: 9 packets (3 waits + 6 writes)", name);
    expect_u(buf, r.n, 9u);
    std::snprintf(buf, sizeof buf, "T1 %s: counts equal r4 (3 waits)", name);
    expect_u(buf, r.nWaits, 3u);
    std::snprintf(buf, sizeof buf, "T1 %s: counts equal r4 (6 writes)", name);
    expect_u(buf, r.nWrites, 6u);
}

static void t1_checks()
{
    t1_one("wsgc1 F5",  kWsgc1F5Setup,  KWSGC1F5SETUP_N);
    t1_one("wsgc1 F10", kWsgc1F10Setup, KWSGC1F10SETUP_N);
    t1_one("decide38 F9", kF9Mib, kF9MibIbs[0].len);
    t1_one("arm20 f4",  &kArm20Dwords[2160],  1456u);
    t1_one("arm20 f9",  &kArm20Dwords[8640],  1456u);
    t1_one("arm20 f12", &kArm20Dwords[11568], 1456u);

    // --- T1's planted breaks, over wsgc1 F5, each re-derived from the SAME translated candidate. ---
    MdCand c {};
    (void)md_translate_headless(kWsgc1F5Setup, KWSGC1F5SETUP_N, &c);
    uint32_t starts[16]; const uint32_t nSeg = md_seg_starts(&c, starts);
    n48_md_scan_result r0 {}; uint64_t d0 = 0ull;
    const uint32_t base = md_judge_real(c.out, c.n, kWsgc1F5Setup, c.r4w, c.r4m, starts, nSeg, &r0, &d0);
    expect_u("T1 positive control: F5 is clean before any break", base, N48_MD_OK);

    // (a) drop the fence page from the resolved map -> UNRESOLVED.
    {
        n48_md_scan_result r {}; uint64_t d = 0ull;
        n48_md_scan(c.out, c.n, kWsgc1F5Setup, c.n, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, starts, nSeg, 0ull, 0u, &r);
        for (uint32_t i = 0; i < r.n; i++) { r.d[i].resolved = (i == 0u) ? 0u : 1u; r.d[i].writable = 1u; }
        expect_u("T1 break(a) drop the fence page from the map -> UNRESOLVED", n48_md_judge(&r, c.r4w, c.r4m, 0u, &d), N48_MD_UNRESOLVED);
    }
    // (b) change a WAIT's reference - AFTER the scan, on the SCAN RESULT (not the raw bytes: a byte-level change
    // would also fail ORIGIN's own region match, testing the wrong clause). This isolates WAIT-UNSATISFIED exactly
    // as T3 already does, but starting from the REAL scan of F5's own real triplet.
    {
        n48_md_scan_result r {}; uint64_t d = 0ull;
        n48_md_scan(c.out, c.n, kWsgc1F5Setup, c.n, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, starts, nSeg, 0ull, 0u, &r);
        for (uint32_t i = 0; i < r.n; i++) { r.d[i].resolved = 1u; r.d[i].writable = 1u; }
        for (uint32_t i = 0; i < r.n; i++) if (r.d[i].isWait) r.d[i].waitRef ^= 1u;   /* the break: a changed reference */
        for (uint32_t i = 0; i < r.n; i++) if (r.d[i].isWait) r.d[i].waitSatisfied = 0u;
        const uint32_t cl = n48_md_judge(&r, c.r4w, c.r4m, 0u, &d);
        expect_u("T1 break(b) change a WAIT's reference -> WAIT-UNSATISFIED", cl, N48_MD_WAIT_UNSATISFIED);
    }
    // (c) — R3 (0.0.441,  (B)): a REAL planted WRITE_DATA packet, overwriting one of F5's own trailing
    // NOP1 dwords immediately BEFORE its own WAIT_REG_MEM, to the SAME fence-page dword the WAIT/RELEASE_MEM triplet
    // uses - the exact hazard R1-MEMDST.md Q1 names. Replaces 0.0.440's `r.nWrites++` stand-in (deleted, per R3).
    {
        // Locate the WAIT_REG_MEM row (isWait) in the positive-control scan; its dword offset is where we must
        // insert the planted WRITE_DATA BEFORE (a real NOP1 pad dword sits immediately ahead of it in F5's own
        // translated stream - xlat12's own register-drop compaction leaves these pads, confirmed by r0 above).
        uint32_t waitAt = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < r0.n; i++) if (r0.d[i].isWait) { waitAt = r0.d[i].dwOff; break; }
        expect_u("T1(c) positive control found the WAIT row", waitAt != 0xFFFFFFFFu ? 1u : 0u, 1u);
        if (waitAt != 0xFFFFFFFFu && waitAt >= 5u) {
            MdCand cb = c;
            // WRITE_DATA(count 4)=8 dw total? Use the minimal 5-dword live form this header requires (len>=5):
            // header, ctrl(DST_SEL 1: memory), addr_lo, addr_hi, data. Overwrite the 5 NOP1 dwords right before the
            // WAIT (F5's own compaction leaves a run of NOP1 there - the positive control's own clean pass proves
            // they are not live content the byte-compare would miss).
            const uint32_t at = waitAt - 5u;
            const uint64_t fenceVa = 0x400001000ull;
            cb.out[at + 0u] = 0xC0034700u;             // PACKET3(WRITE_DATA=0x37, count 3) = 5 dw
            cb.out[at + 1u] = (1u << 8);                // DST_SEL 1 (memory)
            cb.out[at + 2u] = (uint32_t)fenceVa;
            cb.out[at + 3u] = (uint32_t)(fenceVa >> 32);
            cb.out[at + 4u] = 0xDEADBEEFu;              // a value that does NOT satisfy the WAIT's reference
            n48_md_scan_result r {}; uint64_t d = 0ull;
            n48_md_scan(cb.out, cb.n, kWsgc1F5Setup, cb.n, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, starts, nSeg, 0ull, 0u, &r);
            for (uint32_t i = 0; i < r.n; i++) { r.d[i].resolved = 1u; r.d[i].writable = 1u; }
            // The planted WRITE_DATA is a REAL packet Apple's own input does not carry at that offset/region -> it
            // also trips ORIGIN before WAIT-UNSATISFIED even gets a chance (ORIGIN precedes it in the design's own
            // clause order) - which is itself the CORRECT, safe-direction refusal for an injected packet. Confirm
            // ORIGIN refuses it (the packet is genuinely forged, so this is the right clause).
            expect_u("T1(c) R3: a real planted WRITE_DATA before the WAIT is a forged packet -> ORIGIN refuses it",
                     n48_md_judge(&r, c.r4w + 1u, c.r4m, 0u, &d), N48_MD_ORIGIN);
        }
    }
    // (d) INT_SEL 1 on the first RELEASE_MEM -> KIND (R1-MEMDST.md Q1: a live RELEASE_MEM needs INT_SEL 0).
    {
        MdCand cb = c;
        cb.out[198] |= (1u << 24);   // dw 196 = RELEASE_MEM header, ctrl dword at 196+2=198
        n48_md_scan_result r {}; uint64_t d = 0ull;
        const uint32_t cl = md_judge_real(cb.out, cb.n, kWsgc1F5Setup, cb.r4w, cb.r4m, starts, nSeg, &r, &d);
        expect_u("T1 break(d) INT_SEL 1 on the RELEASE_MEM -> KIND", cl, N48_MD_KIND);
    }
    // (e) size the RELEASE_MEM's own header by its count field to 0xFFFF1000-shaped (F5 ends in exactly that
    //     dword, per D4-PRIME.md's own T1 mutation list) -> WALK.
    {
        MdCand cb = c;
        cb.out[cb.n - 1u] = 0xC0FFFFFFu;   // an unsizeable trailing header: count field claims more than remains
        n48_md_scan_result r {}; uint64_t d = 0ull;
        const uint32_t cl = md_judge_real(cb.out, cb.n, kWsgc1F5Setup, cb.r4w, cb.r4m, starts, nSeg, &r, &d);
        expect_u("T1 break(e) an unsizeable trailing packet -> WALK", cl, N48_MD_WALK);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// T2 — F48 (fixture_mib_f48_f20_f21.h, R1-MEMDST.md Q6: zero live WRITE_DATA/RELEASE_MEM/WAIT_REG_MEM/COPY_DATA/
// EVENT_WRITE_EOP of its own) FIRST TRANSLATED CLEAN (confirming Q6's own claim against the real fixture), THEN
// judged WITH ONE FENCE828 RE-POINT APPLIED via n48_f828_apply's OWN mechanism (the buried NOP9-then-RELEASE_MEM
// shape, un-buried exactly as gfx_fence828.h's apply() does: the 9-dword NOP header becomes a 1-dword NOP, the
// buried RELEASE_MEM's address/data words rewritten to the owned slot/`want`) — R1 (0.0.441)'s TIGHTENED identity:
// clean, 1 fence exempted, with the KEXT'S REAL GEOMETRY (the slot INSIDE the OWN-REGION window) and the
// translator's own r4 counts (no hand-added +1: the fence is excluded from nWrites, so r4m alone must match).
// ---------------------------------------------------------------------------------------------------------------------
static void t2_checks()
{
    xlat12_ib_segment sg[16]; uint32_t tot = 0;
    const uint32_t ns = xlat12_ib_segments(kF48Mib, kF48MibIbs[0].len, sg, 16u, &tot);
    expect_u("T2 F48 IB0 segments", ns > 0u ? 1u : 0u, 1u);
    if (!ns) return;
    xlat12_draw_profile pf = *xlat12_ib_m2tri_profile();
    pf.ps_readset1 = 2u; pf.vs_readset1 = 5u;
    static uint32_t out[16384];
    std::memcpy(out, kF48Mib, kF48MibIbs[0].len * 4u);
    uint32_t r4w = 0u, r4m = 0u;
    uint32_t okAll = 1u;
    uint32_t starts[16];
    for (uint32_t k = 0; k < ns; k++) {
        xlat12_draw_extra ex {}; xlat12_draw_stats ds {};
        uint32_t len = 0;
        const uint32_t st = xlat12_ib_translate_draw_ex(&pf, &ex, &kF48Mib[sg[k].start], sg[k].end - sg[k].start,
                                                        &out[sg[k].start], &len, &ds);
        if (st != 0u) okAll = 0u;
        r4w += ds.r4_waits; r4m += ds.r4_memwrites;
        starts[k] = sg[k].start;
    }
    expect_u("T2 F48 IB0 translates clean over every segment", okAll, 1u);
    n48_md_scan_result r0 {}; uint64_t d0 = 0ull;
    n48_md_scan(out, kF48MibIbs[0].len, kF48Mib, kF48MibIbs[0].len, kOwnRegionBase, kOwnRegionLen,
               nullptr, 0u, starts, ns, 0ull, 0u, &r0);
    for (uint32_t i = 0; i < r0.n; i++) { r0.d[i].resolved = 1u; r0.d[i].writable = 1u; }
    expect_u("T2 F48 (Q6): zero live destinations of its own", r0.n, 0u);
    expect_u("T2 F48 (Q6): clean before any fence is offered", n48_md_judge(&r0, r4w, r4m, 0u, &d0), N48_MD_OK);

    // Find a buried NOP9-then-RELEASE_MEM in F48's own translated segment 0 (Q6: 15 of these, all at the dead page)
    // and apply n48_f828_apply's OWN rewrite by hand, at the KEXT'S REAL fence slot (inside OWN-REGION's window).
    uint32_t buriedAt = 0xFFFFFFFFu;
    for (uint32_t i = 0; i + 9u <= sg[0].end; i++)
        if (out[i] == N48_MD_F828_NOP9 && out[i + 1u] == N48_MD_F828_RELMEM) { buriedAt = i; break; }
    expect_u("T2 a buried NOP9-then-RELEASE_MEM exists in F48's own segment 0", buriedAt != 0xFFFFFFFFu ? 1u : 0u, 1u);
    if (buriedAt == 0xFFFFFFFFu) return;
    const uint32_t want = 0x12345678u;
    out[buriedAt] = 0xFFFF1000u;                        // NOP9 header -> 1-dword NOP (n48_f828_apply's own rewrite)
    out[buriedAt + 4u] = (uint32_t)kFenceSlotVa;         // rel_at+3: addr_lo
    out[buriedAt + 5u] = (uint32_t)(kFenceSlotVa >> 32); // rel_at+4: addr_hi
    out[buriedAt + 6u] = want;                           // rel_at+5: DATA
    n48_md_scan_result r {}; uint64_t detail = 0ull;
    n48_md_scan(out, kF48MibIbs[0].len, kF48Mib, kF48MibIbs[0].len, kOwnRegionBase, kOwnRegionLen,
               nullptr, 0u, starts, ns, kFenceSlotVa, want, &r);
    for (uint32_t i = 0; i < r.n; i++) { r.d[i].resolved = 1u; r.d[i].writable = 1u; }
    // NO HAND-ADDED +1 (R1, 0.0.441): the fence row is excluded from nWrites, so the translator's OWN r4m (which
    // never counted the fence - n48_f828_apply runs AFTER translation) must equal the scan's nWrites directly.
    const uint32_t clause = n48_md_judge(&r, r4w, r4m, 0u, &detail);
    expect_u("T2 F48 with the fence applied (real geometry) is CLEAN", clause, N48_MD_OK);
    expect_u("T2 F48: exactly 1 live destination (the fence)", r.n, 1u);
    expect_u("T2 F48: the fence row is exempted by identity", r.n && r.d[0].fenceExempt && r.d[0].originOk, 1u);
    expect_u("T2 F48: the fence is exempt from OWN-REGION although its VA is inside the window",
             r.n && r.d[0].ownRegion, 0u);
    expect_u("T2 F48: the fence is excluded from nWrites (COUNT)", r.nWrites, 0u);
    {
        // Fresh scan, WITHOUT the test's own blanket resolved=1 loop, to prove the CALLER (AppleHardwareHook.cpp's
        // own R5 skip) is what the pure header expects: the scan itself never sets resolved/writable for any row,
        // fence or not - this is the header's own contract, unchanged from 0.0.440.
        n48_md_scan_result rf {}; uint64_t df = 0ull;
        n48_md_scan(out, kF48MibIbs[0].len, kF48Mib, kF48MibIbs[0].len, kOwnRegionBase, kOwnRegionLen,
                   nullptr, 0u, starts, ns, kFenceSlotVa, want, &rf);
        expect_u("T2 F48: the pure scan itself never resolves the fence row (the caller's R5 skip is what matters)",
                 rf.n && rf.d[0].resolved == 0u, 1u);
        // Judged with resolved/writable left 0 (as R5's skip leaves them): still CLEAN, because n48_md_judge's own
        // UNRESOLVED/NOT-WRITABLE loops both exempt a fenceExempt row explicitly.
        expect_u("T2 F48: CLEAN even with resolved/writable left 0 for the exempt row (judge's own fenceExempt skip)",
                 n48_md_judge(&rf, r4w, r4m, 0u, &df), N48_MD_OK);
    }

    // Break: remove the exemption (offer no fence identity at all) -> ORIGIN.
    {
        n48_md_scan_result rb {}; uint64_t d = 0ull;
        n48_md_scan(out, kF48MibIbs[0].len, kF48Mib, kF48MibIbs[0].len, kOwnRegionBase, kOwnRegionLen,
                   nullptr, 0u, starts, ns, 0ull, 0u, &rb);
        for (uint32_t i = 0; i < rb.n; i++) { rb.d[i].resolved = 1u; rb.d[i].writable = 1u; }
        expect_u("T2 break: remove the exemption -> ORIGIN", n48_md_judge(&rb, r4w, r4m, 0u, &d), N48_MD_ORIGIN);
    }
    // Break: exempt by VA only while DATA is wrong -> ORIGIN (the exemption needs BOTH).
    {
        n48_md_scan_result rb {}; uint64_t d = 0ull;
        n48_md_scan(out, kF48MibIbs[0].len, kF48Mib, kF48MibIbs[0].len, kOwnRegionBase, kOwnRegionLen,
                   nullptr, 0u, starts, ns, kFenceSlotVa, want ^ 1u, &rb);
        for (uint32_t i = 0; i < rb.n; i++) { rb.d[i].resolved = 1u; rb.d[i].writable = 1u; }
        expect_u("T2 break: VA matches but DATA is wrong -> ORIGIN (identity needs BOTH)", n48_md_judge(&rb, r4w, r4m, 0u, &d), N48_MD_ORIGIN);
    }
    // Break (R1's own third requirement): claim the identity while Apple's OWN INPUT does NOT hold the buried
    // NOP9-then-RELEASE_MEM bytes at that offset (corrupt `in`'s own copy at nop_at) -> the tightened identity must
    // refuse -> ORIGIN (there is no longer a legitimate fence828 shape to exempt by).
    {
        uint32_t inCorrupt[16384];
        std::memcpy(inCorrupt, kF48Mib, kF48MibIbs[0].len * 4u);
        inCorrupt[buriedAt] = 0xFFFF1000u;   // Apple's "input" no longer shows the buried NOP9 at nop_at
        n48_md_scan_result rb {}; uint64_t d = 0ull;
        n48_md_scan(out, kF48MibIbs[0].len, inCorrupt, kF48MibIbs[0].len, kOwnRegionBase, kOwnRegionLen,
                   nullptr, 0u, starts, ns, kFenceSlotVa, want, &rb);
        for (uint32_t i = 0; i < rb.n; i++) { rb.d[i].resolved = 1u; rb.d[i].writable = 1u; }
        expect_u("T2 break: Apple's input does not hold the buried NOP9/RELEASE_MEM bytes -> ORIGIN (identity needs it)",
                 n48_md_judge(&rb, r4w, r4m, 0u, &d), N48_MD_ORIGIN);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// R2 (0.0.441,  (B)) — ORIGIN REDESIGNED: a fence828-style un-NOP of a DIFFERENT buried dead-page
// RELEASE_MEM (no fence identity offered at all: fenceSlotVa 0), with r4 set EQUAL TO THE SCAN'S OWN COUNT so ONLY
// ORIGIN can refuse (COUNT cannot fire). Must refuse. The real wsgc1 F5 (-11-dword shift, T1 above) already proves
// the positive control (region-aware ORIGIN still accepts real compaction).
// ---------------------------------------------------------------------------------------------------------------------
static void r2_checks()
{
    xlat12_ib_segment sg[16]; uint32_t tot = 0;
    const uint32_t ns = xlat12_ib_segments(kF48Mib, kF48MibIbs[0].len, sg, 16u, &tot);
    if (!ns) { expect_u("R2 F48 segments (setup)", 0u, 1u); return; }
    xlat12_draw_profile pf = *xlat12_ib_m2tri_profile();
    pf.ps_readset1 = 2u; pf.vs_readset1 = 5u;
    static uint32_t out[16384];
    std::memcpy(out, kF48Mib, kF48MibIbs[0].len * 4u);
    uint32_t starts[16];
    for (uint32_t k = 0; k < ns; k++) {
        xlat12_draw_extra ex {}; xlat12_draw_stats ds {}; uint32_t len = 0;
        xlat12_ib_translate_draw_ex(&pf, &ex, &kF48Mib[sg[k].start], sg[k].end - sg[k].start, &out[sg[k].start], &len, &ds);
        starts[k] = sg[k].start;
    }
    // Find a SECOND buried NOP9/RELEASE_MEM (Q6 names 15 of them) so this break is independent of T2's own.
    uint32_t firstAt = 0xFFFFFFFFu, secondAt = 0xFFFFFFFFu;
    for (uint32_t i = 0; i + 9u <= sg[0].end; i++) {
        if (out[i] == N48_MD_F828_NOP9 && out[i + 1u] == N48_MD_F828_RELMEM) {
            if (firstAt == 0xFFFFFFFFu) firstAt = i;
            else { secondAt = i; break; }
        }
    }
    expect_u("R2 setup: a second buried NOP9/RELEASE_MEM exists", secondAt != 0xFFFFFFFFu ? 1u : 0u, 1u);
    if (secondAt == 0xFFFFFFFFu) return;
    // Un-NOP it WITHOUT offering any fence identity: the 9-dword NOP header's OPCODE alone changes to a live
    // RELEASE_MEM (DST_SEL 0, INT_SEL 0, DATA_SEL 1), keeping the SAME 9-dword total so every later offset holds.
    out[secondAt] = 0xC0074900u; out[secondAt + 2u] = (1u << 29);
    n48_md_scan_result r {}; uint64_t d = 0ull;
    n48_md_scan(out, kF48MibIbs[0].len, kF48Mib, kF48MibIbs[0].len, kOwnRegionBase, kOwnRegionLen,
               nullptr, 0u, starts, ns, 0ull, 0u, &r);
    for (uint32_t i = 0; i < r.n; i++) { r.d[i].resolved = 1u; r.d[i].writable = 1u; }
    // r4 set EQUAL to the scan's own count: ONLY ORIGIN can refuse (COUNT is neutralised by construction).
    const uint32_t cl = n48_md_judge(&r, r.nWaits, r.nWrites, 0u, &d);
    expect_u("R2 un-NOP a buried RELEASE_MEM, r4 == scan's own count (COUNT cannot fire) -> ORIGIN must refuse", cl, N48_MD_ORIGIN);
}

// ---------------------------------------------------------------------------------------------------------------------
// R4 (0.0.441,  (B)) — OWN-IB checked against EACH IB's own range. A synthetic 2-IB frame: IB 0's range
// is far away; IB 1's range covers the destination. 0.0.440's single-window rule (IB 0's VA over the concatenated
// length) would have MISSED this; the fixed rule must refuse it.
// ---------------------------------------------------------------------------------------------------------------------
static void r4_checks()
{
    // RELEASE_MEM: header (count 6 -> 8 dw) + ev + ctrl(DST_SEL0,INT_SEL0,DATA_SEL1) + addr_lo + addr_hi + data + pad.
    // Destination VA = 0x600001000 (hi=6, lo=0x1000) - inside IB 1's own [0x600000000, 0x600000000+0x2000) range,
    // OUTSIDE IB 0's [0x500000000, 0x500000000+0x2000) range and outside a single concatenated-length window too.
    static uint32_t out[8] = { 0xC0064900u, 0u, (1u << 29), 0x00001000u, 0x00000006u, 0x1u, 0u, 0u };
    const n48_md_ib_range ibs2[2] = { { 0x500000000ull, 0x2000ull }, { 0x600000000ull, 0x2000ull } };
    n48_md_scan_result r {};
    n48_md_scan(out, 8u, out, 8u, 0x23F0000000ull, 0x10000000ull, ibs2, 2u, nullptr, 0u, 0ull, 0u, &r);
    uint64_t d = 0ull;
    expect_u("R4 a 2-IB frame whose destination lands in IB 1 -> OWN-IB", n48_md_judge(&r, 0u, 1u, 0u, &d), N48_MD_OWN_IB);
    // Control: the SAME destination judged against ONLY IB 0's range (the 0.0.440 shape) does NOT catch it.
    n48_md_scan_result rCtl {};
    const n48_md_ib_range ib0Only[1] = { { 0x500000000ull, 0x2000ull } };
    n48_md_scan(out, 8u, out, 8u, 0x23F0000000ull, 0x10000000ull, ib0Only, 1u, nullptr, 0u, 0ull, 0u, &rCtl);
    expect_u("R4 control: IB 0's range alone (0.0.440's shape) does NOT see IB 1's own destination", rCtl.d[0].ownIb, 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// R5 (0.0.441,  (B)) — NO PAGE WALK FOR A ROW A PURE CLAUSE HAS ALREADY REFUSED. This is a CALL-SITE
// property (AppleHardwareHook.cpp's own loop, skipping gfxc_page for ownRegion/ownIb/fenceExempt/!originOk rows) -
// pinned here with a COUNTED STUB resolver standing in for gfxc_page, run over the SAME shapes T4/T5 already use.
// ---------------------------------------------------------------------------------------------------------------------
static uint32_t gStubResolverCalls = 0;
static void stub_walk(n48_md_scan_result &r) {
    for (uint32_t qi = 0; qi < r.n; qi++) {
        if (r.d[qi].fenceExempt || !r.d[qi].originOk || r.d[qi].ownRegion || r.d[qi].ownIb) continue;   // R5's own skip
        gStubResolverCalls++;
        r.d[qi].resolved = 1u; r.d[qi].writable = 1u;
    }
}
static void r5_checks()
{
    // A row inside root[511]'s window (T4's own shape): 0 resolver calls.
    {
        gStubResolverCalls = 0;
        static uint32_t out[8] = { 0xC0064900u, 0u, (1u << 29), 0x00000000u, 0x00000005u, 0x1u, 0u, 0u };
        n48_md_scan_result r {};
        n48_md_scan(out, 8u, out, 8u, 0x500000000ull, 0x10000000ull, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r);
        stub_walk(r);
        expect_u("R5 a row inside root[511]'s window: 0 resolver calls", gStubResolverCalls, 0u);
    }
    // A row inside the own IB: 0 resolver calls.
    {
        gStubResolverCalls = 0;
        static uint32_t out[8] = { 0xC0064900u, 0u, (1u << 29), 0x00001000u, 0x00000004u, 0x1u, 0u, 0u };
        const n48_md_ib_range ib1[1] = { { 0x400001000ull, 0x2000ull } };
        n48_md_scan_result r {};
        n48_md_scan(out, 8u, out, 8u, 0x23F0000000ull, 0x10000000ull, ib1, 1u, nullptr, 0u, 0ull, 0u, &r);
        stub_walk(r);
        expect_u("R5 a row inside the own IB: 0 resolver calls", gStubResolverCalls, 0u);
    }
    // Control: a clean self-identical row that clears every pure clause DOES get walked (1 call).
    {
        gStubResolverCalls = 0;
        static uint32_t out[8] = { 0xC0064900u, 0u, (1u << 29), 0x00000000u, 0x00000005u, 0x1u, 0u, 0u };
        n48_md_scan_result r {};
        n48_md_scan(out, 8u, out, 8u, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r);
        stub_walk(r);
        expect_u("R5 control: a row no pure clause refused IS walked", gStubResolverCalls, 1u);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// R6 (0.0.441,  (B)) — A REAL NOT-WRITABLE COUNTER, still counted-only (never refusing while
// kMdNotWritableConfirmed is 0). Pinned at the pure-judge level: notWritableConfirmed=0 never returns NOT_WRITABLE
// however many rows are unwritable; =1 (the CONFIRMED reading, not this build's own) does - proving the clause
// itself works and the count/refuse split is exactly the `notWritableConfirmed` gate, matching AppleHardwareHook.cpp's
// own kMdNotWritableConfirmed = 0 (so the caller's `gMemDst.notWritable` tally can count without gating anything).
// ---------------------------------------------------------------------------------------------------------------------
static void r6_checks()
{
    n48_md_scan_result r {};
    r.walkOk = 1u; r.n = 1u;
    r.d[0] = n48_md_dst {}; r.d[0].kind = N48_MD_OP_RELEASE_MEM; r.d[0].va = 0x400001000ull;
    r.d[0].originOk = 1u; r.d[0].resolved = 1u; r.d[0].writable = 0u;   // NOT writable
    r.nWrites = 1u;
    uint64_t d = 0ull;
    expect_u("R6 notWritableConfirmed=0: an unwritable row is COUNTED, never refused", n48_md_judge(&r, 0u, 1u, 0u, &d), N48_MD_OK);
    expect_u("R6 notWritableConfirmed=1: the SAME row DOES refuse (proves the clause itself is live)",
             n48_md_judge(&r, 0u, 1u, 1u, &d), N48_MD_NOT_WRITABLE);
}

// ---------------------------------------------------------------------------------------------------------------------
// R7 (0.0.441,  (B)) — KIND, corrected. RELEASE_MEM DST_SEL 1 (TC_L2) IS a memory destination now (not a
// silent no-op); DST_SEL 2/3 are explicit KIND refusals; WRITE_DATA to a register (DST_SEL 0) and to GDS (DST_SEL 3)
// are explicit KIND refusals too (0.0.440 silently ignored all four).
// ---------------------------------------------------------------------------------------------------------------------
static void r7_checks()
{
    // RELEASE_MEM DST_SEL 1 (TC_L2): recorded as a memory destination (isMem), not silently dropped.
    {
        static uint32_t out[8] = { 0xC0064900u, 0u, (1u << 16) | (1u << 29), 0x1000u, 0x4u, 0x1u, 0u, 0u };
        n48_md_scan_result r {};
        n48_md_scan(out, 8u, out, 8u, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r);
        expect_u("R7 RELEASE_MEM DST_SEL 1 (TC_L2): recorded as a memory destination", r.n, 1u);
        expect_u("R7   ... not a KIND refusal", r.kindBad, 0u);
    }
    // RELEASE_MEM DST_SEL 2: explicit KIND refusal.
    {
        static uint32_t out[8] = { 0xC0064900u, 0u, (2u << 16) | (1u << 29), 0x1000u, 0x4u, 0x1u, 0u, 0u };
        n48_md_scan_result r {};
        n48_md_scan(out, 8u, out, 8u, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r);
        expect_u("R7 RELEASE_MEM DST_SEL 2 -> KIND", r.kindBad, 1u);
        expect_u("R7   ... and is NOT recorded as a destination", r.n, 0u);
    }
    // RELEASE_MEM DST_SEL 3: explicit KIND refusal.
    {
        static uint32_t out[8] = { 0xC0064900u, 0u, (3u << 16) | (1u << 29), 0x1000u, 0x4u, 0x1u, 0u, 0u };
        n48_md_scan_result r {};
        n48_md_scan(out, 8u, out, 8u, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r);
        expect_u("R7 RELEASE_MEM DST_SEL 3 -> KIND", r.kindBad, 1u);
    }
    // WRITE_DATA to a register (DST_SEL 0): explicit KIND refusal (0.0.440: silently unrecorded).
    {
        static uint32_t out[6] = { 0xC0033700u, 0u, 0x100u, 0u, 0x1u, 0xFFFF1000u };   // count 3 -> 5 dw
        n48_md_scan_result r {};
        n48_md_scan(out, 6u, out, 6u, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r);
        expect_u("R7 WRITE_DATA to a register (DST_SEL 0) -> KIND", r.kindBad, 1u);
        expect_u("R7   ... and is NOT recorded as a destination", r.n, 0u);
    }
    // WRITE_DATA to GDS (DST_SEL 3): explicit KIND refusal.
    {
        static uint32_t out[6] = { 0xC0033700u, (3u << 8), 0x100u, 0u, 0x1u, 0xFFFF1000u };
        n48_md_scan_result r {};
        n48_md_scan(out, 6u, out, 6u, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r);
        expect_u("R7 WRITE_DATA to GDS (DST_SEL 3) -> KIND", r.kindBad, 1u);
    }
    // Control: WRITE_DATA DST_SEL 1 (memory) is still a plain memory destination, unaffected.
    {
        static uint32_t out[6] = { 0xC0033700u, (1u << 8), 0x1000u, 0x4u, 0x1u, 0xFFFF1000u };
        n48_md_scan_result r {};
        n48_md_scan(out, 6u, out, 6u, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r);
        expect_u("R7 control: WRITE_DATA DST_SEL 1 (memory) is still isMem", r.n, 1u);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// T3 — F20/F21's raw live triplets (R1-MEMDST.md Q1: the SAME shape as wsgc1's F5/F10, on the SAME fence page)
// through the PURE judge directly: self-satisfied, clean. Built by hand (no translator needed - the pure header's
// own contract), matching the design's own "self-satisfied rule" wording exactly.
// ---------------------------------------------------------------------------------------------------------------------
static void t3_checks()
{
    n48_md_scan_result r {};
    r.walkOk = 1u; r.n = 3u;
    r.d[0] = n48_md_dst {}; r.d[0].kind = N48_MD_OP_WRITE_DATA; r.d[0].va = 0x400001000ull; r.d[0].originOk = 1u;
    r.d[0].resolved = 1u; r.d[0].writable = 1u;
    r.d[1] = n48_md_dst {}; r.d[1].kind = N48_MD_OP_RELEASE_MEM; r.d[1].va = 0x400001000ull; r.d[1].originOk = 1u;
    r.d[1].resolved = 1u; r.d[1].writable = 1u; r.d[1].dwOff = 100u;
    r.d[2] = n48_md_dst {}; r.d[2].kind = N48_MD_OP_WAIT_REG_MEM; r.d[2].va = 0x400001000ull; r.d[2].originOk = 1u;
    r.d[2].resolved = 1u; r.d[2].writable = 1u; r.d[2].isWait = 1u; r.d[2].waitFunc = 3u; r.d[2].waitRef = 0x11111111u;
    r.d[2].waitMask = 0xFFFFFFFFu; r.d[2].waitSatisfied = 1u;   // hand-set to the self-satisfied answer, as the design intends
    r.nWaits = 1u; r.nWrites = 2u;
    uint64_t detail = 0ull;
    expect_u("T3 F20/F21-shaped raw triplet, self-satisfied, is CLEAN", n48_md_judge(&r, 1u, 2u, 0u, &detail), N48_MD_OK);
    r.d[2].waitSatisfied = 0u;
    expect_u("T3   ... and refuses WAIT-UNSATISFIED when it is not", n48_md_judge(&r, 1u, 2u, 0u, &detail), N48_MD_WAIT_UNSATISFIED);
}

// ---------------------------------------------------------------------------------------------------------------------
// T4 — a destination re-pointed into the root[511] window refuses with the resolver called ZERO times (OWN-REGION
// is decided by VA alone, before UNRESOLVED - which is the whole point: root[511] is never walked).
// ---------------------------------------------------------------------------------------------------------------------
static void t4_checks()
{
    static uint32_t out[8] = { 0xC0064900u, 0u, (1u << 29), 0x00000000u, 0x00000005u, 0x1u, 0u, 0u };
    const uint64_t ownLen = 0x10000000ull;
    n48_md_scan_result r {};
    n48_md_scan(out, 8u, out, 8u, 0x500000000ull, ownLen, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r);
    expect_u("T4 walk ok", r.walkOk, 1u);
    expect_u("T4 one destination found", r.n, 1u);
    expect_u("T4 the resolver is never asked (resolved/writable stay 0 - scan never calls gfxc_page)",
             r.d[0].resolved | r.d[0].writable, 0u);
    uint64_t detail = 0ull;
    expect_u("T4 a destination inside root[511]'s window REFUSES OWN-REGION, by VA alone",
             n48_md_judge(&r, 0u, 1u, 0u, &detail), N48_MD_OWN_REGION);
}

// ---------------------------------------------------------------------------------------------------------------------
// T5 — OWN-IB, COPY_DATA, a 4-dword EVENT_WRITE and EVENT_WRITE_EOP planted in a NOP pad: all refuse.
// ---------------------------------------------------------------------------------------------------------------------
static void t5_checks()
{
    // OWN-IB: a RELEASE_MEM whose VA lands inside the frame's own IB range.
    {
        static uint32_t out[8] = { 0xC0064900u, 0u, (1u << 29), 0x00001000u, 0x00000004u, 0x1u, 0u, 0u };
        const n48_md_ib_range ib1[1] = { { 0x400001000ull, 0x2000ull } };
        n48_md_scan_result r {};
        n48_md_scan(out, 8u, out, 8u, 0x23F0000000ull, 0x10000000ull, ib1, 1u, nullptr, 0u, 0ull, 0u, &r);
        uint64_t d = 0ull;
        expect_u("T5 OWN-IB: a destination inside this frame's own IB range REFUSES", n48_md_judge(&r, 0u, 1u, 0u, &d), N48_MD_OWN_IB);
    }
    // COPY_DATA anywhere: KIND.
    {
        static uint32_t out[8] = { 0xC0044000u, 0u, 0u, 0u, 0u, 0u, 0xFFFF1000u, 0xFFFF1000u };
        n48_md_scan_result r {};
        n48_md_scan(out, 8u, out, 8u, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r);
        uint64_t d = 0ull;
        expect_u("T5 COPY_DATA present -> KIND", n48_md_judge(&r, 0u, 0u, 0u, &d), N48_MD_KIND);
    }
    // A 4-dword EVENT_WRITE (carries an address): KIND.
    {
        static uint32_t out[6] = { 0xC0024600u, 0x14u, 0x1000u, 0x4u, 0xFFFF1000u, 0xFFFF1000u };
        n48_md_scan_result r {};
        n48_md_scan(out, 6u, out, 6u, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r);
        uint64_t d = 0ull;
        expect_u("T5 a 4-dword EVENT_WRITE (address-carrying) -> KIND", n48_md_judge(&r, 0u, 0u, 0u, &d), N48_MD_KIND);
        // control: the 2-dword form (F48's own shape) does NOT refuse.
        static uint32_t out2[3] = { 0xC0004600u, 0x14u, 0xFFFF1000u };
        n48_md_scan_result r2 {};
        n48_md_scan(out2, 3u, out2, 3u, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r2);
        expect_u("T5   ... but the 2-dword form is fine (F48's own EVENT_WRITE shape)", r2.kindBad, 0u);
    }
    // EVENT_WRITE_EOP: KIND.
    {
        static uint32_t out[9] = { 0xC0064700u, 0, 0, 0, 0, 0, 0, 0, 0xFFFF1000u };
        n48_md_scan_result r {};
        n48_md_scan(out, 9u, out, 9u, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r);
        uint64_t d = 0ull;
        expect_u("T5 EVENT_WRITE_EOP present -> KIND", n48_md_judge(&r, 0u, 0u, 0u, &d), N48_MD_KIND);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// T6 — a scanner mutant skipping WAITs trips COUNT.
// ---------------------------------------------------------------------------------------------------------------------
static void t6_checks()
{
    n48_md_scan_result r {};
    r.walkOk = 1u; r.nWaits = 0u; r.nWrites = 2u;   // the mutant: a real WAIT was not counted
    uint64_t d = 0ull;
    expect_u("T6 a scanner mutant that skips WAITs trips COUNT", n48_md_judge(&r, /*r4Waits=*/1u, /*r4Memwrites=*/2u, 0u, &d), N48_MD_COUNT);
    r.nWaits = 1u;
    expect_u("T6   ... and the real count is clean", n48_md_judge(&r, 1u, 2u, 0u, &d), N48_MD_OK);
}

// ---------------------------------------------------------------------------------------------------------------------
// T8 — the R4 discharge refuses on unequal counts.
// ---------------------------------------------------------------------------------------------------------------------
static void t8_checks()
{
    n48_md_scan_result r {};
    r.walkOk = 1u; r.nWaits = 3u; r.nWrites = 6u;
    uint64_t d = 0ull;
    expect_u("T8 matching counts -> CLEAN (md_ok would be 1, so the discharge fires)", n48_md_judge(&r, 3u, 6u, 0u, &d), N48_MD_OK);
    expect_u("T8 UNEQUAL wait count -> COUNT (md_ok would be 0, so the discharge never fires)", n48_md_judge(&r, 4u, 6u, 0u, &d), N48_MD_COUNT);
    expect_u("T8 UNEQUAL write count -> COUNT", n48_md_judge(&r, 3u, 7u, 0u, &d), N48_MD_COUNT);
}

// ---------------------------------------------------------------------------------------------------------------------
// T7 — OFF identity (every OTHER suite re-run unchanged after this build is the real proof: gMdMode's default is 0
// and every call site here is `if (gMdMode ...)`-guarded) PLUS the structural pins T7 names: the scan follows the
// fence apply (textually AFTER the fence828 site, inside the segment loop, in gfxsrc_policy's own source), and
// `live` carries md_ok.
// ---------------------------------------------------------------------------------------------------------------------
static void t7_wiring_checks(const char *srcPath)
{
    std::ifstream f(srcPath);
    if (!f) { std::printf("SKIP  T7 wiring pin: could not open %s\n", srcPath); return; }
    std::stringstream ss; ss << f.rdbuf();
    const std::string s = ss.str();
    const size_t fenceSite = s.find("gXdF828Pending = 1u; gXdF828GateOk = 0u;");
    const size_t mdSite = s.find("if (gMdMode && build && gXdBuild.ok) {");
    expect_u("T7 the fence828 apply site is found", fenceSite != std::string::npos ? 1u : 0u, 1u);
    expect_u("T7 the memory-destination scan site is found", mdSite != std::string::npos ? 1u : 0u, 1u);
    expect_u("T7 the scan follows the fence apply (textually, in gfxsrc_policy)",
             (fenceSite != std::string::npos && mdSite != std::string::npos && fenceSite < mdSite) ? 1u : 0u, 1u);
    const size_t liveSite = s.find("const bool live = (arm == N48_SD_ARM_COMMIT)");
    expect_u("T7 `live` carries md_ok", (liveSite != std::string::npos &&
             s.find("gMdMode != N48_MD_MODE_ENFORCE || gXdBuild.md_ok", liveSite) != std::string::npos) ? 1u : 0u, 1u);
    // R8: the SECOND reset site (the policy-skipped branch) now clears md_ok too.
    const size_t resetSite2 = s.find("gXdBuild.ok = 0; gXdBuild.nseg = 0;\n        // R8");
    expect_u("R8 the second reset site is found and immediately followed by the R8 comment", resetSite2 != std::string::npos ? 1u : 0u, 1u);
    expect_u("R8 md_ok/md_clause/md_detail are cleared at that same site",
             s.find("gXdBuild.md_ok = 0u; gXdBuild.md_clause = 0u; gXdBuild.md_detail = 0ull;",
                     resetSite2 == std::string::npos ? 0u : resetSite2) != std::string::npos ? 1u : 0u, 1u);
}

// ---------------------------------------------------------------------------------------------------------------------
// R9 (0.0.441,  (B), item 9) — PER-CLASS COUNTERS, and the report-line BYTE BUDGET, measured against the
// REAL format strings (copy-pasted from AppleHardwareHook.cpp) at a documented, bounded "worst case": 10 digits per
// counter (~1e10) rather than uint64_t's own 20-digit range. With 18-24 raw %llu fields on one line, a literal
// uint64_max worst case is UNSATISFIABLE under 512 B by simple arithmetic (24*20 = 480, before one byte of the
// "AppleHardwareHook: " prefix or any label) - so this bound is the only consistent reading of "the worst case" for
// a report with this many fields, and is itself extremely conservative for a per-BOOT counter (see the code comment
// beside the HWLOG calls for the reachability argument).
// ---------------------------------------------------------------------------------------------------------------------
static void r9_checks()
{
    char line1[600];
    std::snprintf(line1, sizeof line1,
        "AppleHardwareHook: memdst42: R1(gfxneuter42)=%s%s asks %llu judged %llu walked %llu fenced %llu notW %llu shBad %llu "
        "us %llu last %s d%#llx tally: clean %llu walk %llu over %llu KIND %llu ORIGIN %llu OWNR %llu "
        "OWNIB %llu WAITU %llu UNRES %llu NOTW %llu CNT %llu SEGR %llu.\n",   /* build 0.0.492: + SEGR */
        "OFF", " - REFUSED (unknown M), unchanged",
        9999999999ull, 9999999999ull, 9999999999ull, 9999999999ull, 9999999999ull, 9999999999ull, 9999999999ull,
        "WAIT-UNSATISFIED", (unsigned long long)0xFFFFFFFFFFFFFFFFull,
        9999999999ull, 9999999999ull, 9999999999ull, 9999999999ull, 9999999999ull, 9999999999ull, 9999999999ull,
        9999999999ull, 9999999999ull, 9999999999ull, 9999999999ull, 9999999999ull);
    expect_u("R9 memdst42: worst-case (10-digit bound) length is under 512 B", std::strlen(line1) < 512u ? 1u : 0u, 1u);
    std::printf("      (memdst42: worst-case length = %zu)\n", std::strlen(line1));

    char line2[600];
    std::snprintf(line2, sizeof line2,
        "AppleHardwareHook: memdst42c: class(scanned/pkts/waits/writes/clean/refuse) prod %llu/%llu/%llu/%llu/%llu/%llu "
        "comp %llu/%llu/%llu/%llu/%llu/%llu plane %llu/%llu/%llu/%llu/%llu/%llu "
        "other %llu/%llu/%llu/%llu/%llu/%llu.\n",
        9999999999ull,9999999999ull,9999999999ull,9999999999ull,9999999999ull,9999999999ull,
        9999999999ull,9999999999ull,9999999999ull,9999999999ull,9999999999ull,9999999999ull,
        9999999999ull,9999999999ull,9999999999ull,9999999999ull,9999999999ull,9999999999ull,
        9999999999ull,9999999999ull,9999999999ull,9999999999ull,9999999999ull,9999999999ull);
    expect_u("R9 memdst42c: worst-case (10-digit bound) length is under 512 B", std::strlen(line2) < 512u ? 1u : 0u, 1u);
    std::printf("      (memdst42c: worst-case length = %zu)\n", std::strlen(line2));

    // The per-class counter logic itself (classification + tally), independent of the report text: modelled here
    // exactly as AppleHardwareHook.cpp classifies (kind==HEADLESS && n==1456 -> producer; mib && nib>=2 -> composite;
    // !mib && kind==ENCODER -> plane; else other).
    struct Frame { uint32_t kind, n, mib, nib; };
    enum { KIND_ENCODER = 0, KIND_HEADLESS = 2 };   // matches N48_CM_KIND_ENCODER/_HEADLESS's own relative order (pin below)
    auto classify = [](const Frame &f) -> int {
        if (f.kind == KIND_HEADLESS && f.n == 1456u) return 0;   // producer
        if (f.mib && f.nib >= 2u) return 1;                       // composite
        if (!f.mib && f.kind == KIND_ENCODER) return 2;           // plane
        return 3;                                                  // other
    };
    expect_u("R9 classify: HEADLESS 1456-dword -> producer", (uint32_t)classify(Frame{KIND_HEADLESS, 1456u, 0u, 1u}), 0u);
    expect_u("R9 classify: mib && nib>=2 -> composite", (uint32_t)classify(Frame{KIND_ENCODER, 900u, 1u, 2u}), 1u);
    expect_u("R9 classify: single-IB ENCODER -> plane", (uint32_t)classify(Frame{KIND_ENCODER, 900u, 0u, 1u}), 2u);
    expect_u("R9 classify: anything else -> other", (uint32_t)classify(Frame{KIND_HEADLESS, 800u, 0u, 1u}), 3u);
}


// ---------------------------------------------------------------------------------------------------------------------
// build 0.0.487 (notes/design/COMPUTE-N.md Q8 T3 and the gate rule, Q4/Q6 item 5) — R1 OVER THE ELIDED CANDIDATE, AND
// THE KEXT'S ORDER TO THE GATE. Every captured N segment (fixture_compute_n.h, decide49/run10c capture.bin) translated
// with XLAT12_EXTRA_CS_ELIDE and a program answer: R1 scans the output CLEAN - Apple's triplet (WRITE_DATA, RELEASE_MEM,
// WAIT_REG_MEM) kept byte for byte and self-satisfied, 1 wait and 2 writes, equal to the translator's own r4 (COUNT).
// Destinations are forced resolved/writable exactly as T1 does (a real page walk needs live VRAM). Then the SAME numbers
// through the kext's order: ds.cs_elided -> gXdBuild.csElided -> c.cs_elided -> n48_cm_live / n48_cm_gate: an elided
// frame commits ONLY with 42 ENFORCE and R1 clean. Breaks in the test: NOP the WRITE_DATA (ORIGIN), NOP the whole triplet
// (COUNT: no row is left for ORIGIN to compare, and r4 still counts 1/2).
// ---------------------------------------------------------------------------------------------------------------------
static int csn_md_is_n(void *ctx, uint64_t va) { (void)ctx; return va == 0x400017a00ull ? 1 : 0; }
static uint32_t csn_md_len(const uint32_t *d, uint32_t i);
static void seg_refused_checks(const char *srcPath);
static uint32_t csn_md_len(const uint32_t *d, uint32_t i)
{
    const uint32_t h = d[i];
    return (h == 0xFFFF1000u || (h >> 30) == 2u) ? 1u : ((h >> 30) == 3u ? ((h >> 16) & 0x3FFFu) + 2u : 1u);
}
static void cs_elide_checks()
{
    static uint32_t out[4096], mut[4096];
    char lbl[200];
    for (uint32_t s = 0; s < XLAT12_FIXTURE_CSN_COUNT; s++) {
        const xlat12_fixture_csn *f = &kCsNSegs[s];
        if (f->n > 4096u) { expect_u("T3n fixture fits", 0u, 1u); continue; }
        xlat12_draw_extra ex {};
        ex.flags = XLAT12_EXTRA_CS_ELIDE; ex.cs_is_n = csn_md_is_n;
        xlat12_draw_stats ds {};
        uint32_t len = 0;
        const uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, f->dw, f->n, out, &len, &ds);
        std::snprintf(lbl, sizeof lbl, "T3n %s: translates with the dispatch elided", f->name);
        expect_u(lbl, st == 0u && ds.cs_elided == 1u ? 1u : 0u, 1u);
        if (st) continue;
        const uint32_t starts[1] = { 0u };
        n48_md_scan_result r {}; uint64_t detail = 0ull;
        const uint32_t clause = md_judge_real(out, f->n, f->dw, ds.r4_waits, ds.r4_memwrites, starts, 1u, &r, &detail);
        std::snprintf(lbl, sizeof lbl, "T3n %s: R1 CLEAN over the elided candidate", f->name);
        expect_u(lbl, clause, N48_MD_OK);
        std::snprintf(lbl, sizeof lbl, "T3n %s: 1 wait + 2 writes (Apple's triplet), = r4 %u/%u", f->name, ds.r4_waits, ds.r4_memwrites);
        expect_u(lbl, (r.nWaits == 1u && r.nWrites == 2u && ds.r4_waits == 1u && ds.r4_memwrites == 2u) ? 1u : 0u, 1u);
        uint32_t sat = 0u; for (uint32_t i = 0; i < r.n; i++) if (r.d[i].isWait && r.d[i].waitSatisfied) sat++;
        std::snprintf(lbl, sizeof lbl, "T3n %s: the WAIT is self-satisfied by the RELEASE_MEM before it", f->name);
        expect_u(lbl, sat, 1u);
        // THE GATE RULE, in the kext's order, from these real numbers.
        const uint32_t csElided = ds.cs_elided;          // gfxsrc_policy: gXdBuild.csElided += ds.cs_elided
        const uint32_t mdOk = clause == N48_MD_OK ? 1u : 0u;   // gXdBuild.md_ok = (mdClause == N48_MD_OK)
        std::snprintf(lbl, sizeof lbl, "T3n %s: live, 42 ENFORCE + R1 clean -> 1", f->name);
        expect_u(lbl, n48_cm_live(N48_SD_ARM_COMMIT, N48_XV_TRANSLATE, 1u, 1u, 1u, 1u, mdOk, 0u, 0u, 0u, csElided, 0u, 0u), 1u);
        std::snprintf(lbl, sizeof lbl, "T3n %s: live, 42 SHADOW (md_enforce 0) -> 0", f->name);
        expect_u(lbl, n48_cm_live(N48_SD_ARM_COMMIT, N48_XV_TRANSLATE, 1u, 1u, 1u, 0u, mdOk, 0u, 0u, 0u, csElided, 0u, 0u), 0u);
        std::snprintf(lbl, sizeof lbl, "T3n %s: live, 42 ENFORCE but R1 not clean -> 0", f->name);
        expect_u(lbl, n48_cm_live(N48_SD_ARM_COMMIT, N48_XV_TRANSLATE, 1u, 1u, 1u, 1u, 0u, 0u, 0u, 0u, csElided, 0u, 0u), 0u);
        if (s != 0u) continue;
        // In-test breaks, over the first segment only.
        uint32_t wd = ~0u;
        for (uint32_t i = 0; i < f->n; i += csn_md_len(out, i))
            if ((out[i] >> 30) == 3u && out[i] != 0xFFFF1000u && ((out[i] >> 8) & 0xFFu) == 0x37u && ((out[i + 1u] >> 8) & 0xFu) != 0u) { wd = i; break; }
        expect_u("T3n positive control: the output carries Apple's memory WRITE_DATA", wd != ~0u ? 1u : 0u, 1u);
        if (wd == ~0u) continue;
        std::memcpy(mut, out, f->n * 4u);
        const uint32_t l = csn_md_len(mut, wd);
        mut[wd] = 0xC0001000u | ((l - 2u) << 16); for (uint32_t k = 1; k < l; k++) mut[wd + k] = 0u;
        expect_u("T3n break: the WRITE_DATA NOPed (the triplet partly elided too) -> ORIGIN",
                 md_judge_real(mut, f->n, f->dw, ds.r4_waits, ds.r4_memwrites, starts, 1u, &r, &detail), N48_MD_ORIGIN);
        for (uint32_t i = 0; i < f->n; i += csn_md_len(mut, i)) {
            const uint32_t op = (mut[i] >> 8) & 0xFFu;
            if ((mut[i] >> 30) == 3u && mut[i] != 0xFFFF1000u && (op == 0x49u || op == 0x3Cu)) {
                const uint32_t ll = csn_md_len(mut, i);
                mut[i] = 0xC0001000u | ((ll - 2u) << 16); for (uint32_t k = 1; k < ll; k++) mut[i + k] = 0u;
            }
        }
        expect_u("T3n break: the whole triplet NOPed -> COUNT (r4 still 1/2, the scan 0/0)",
                 md_judge_real(mut, f->n, f->dw, ds.r4_waits, ds.r4_memwrites, starts, 1u, &r, &detail), N48_MD_COUNT);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// build 0.0.490 item 3 — THE FENCE IN THE TRANSLATOR'S OUTPUT. Under 42 ENFORCE every commit needs md_ok
// and every continuous commit needs our owned-slot fence, and R1's fence identity (gfx_memdst.h, R1) grants the exemption
// ONLY when Apple's INPUT holds the buried NOP9-then-RELEASE_MEM at the SAME dword the output's re-pointed RELEASE_MEM sits
// at. Nothing tested that on the translator's own output: a tail re-encoded from `cursor` and NOP-padded could move it, and
// then ENFORCE would refuse every commit (ORIGIN). This runs run10e's COMMITTED frames (fixture_fio_run10e.h: two two-part
// 23136-dw A frames, a 1040-dw fill and a 1040-dw plane) through the kext's own order: the segment stage (n48_mib_segment
// for two IBs, xlat12_ib_segments for one), the REAL translator per segment with the flags an armed run sets (the kext's
// own per-segment block, gfxsrc_policy: 27 RASTER + 50 PER_DRAW + rsrc3, 31 FILL_COLOR, 34 PAIR_PRE, 40 READSET, the
// descriptor port 10 with 49 APPLE_HEAD (only where the head executes), 48 TABLE_REUSE, 44 UD_REEMIT + carry, 52 VS_KNOWN,
// and in the second pass 57 CS_ELIDE and 60 DCC_STRIP), then n48_f828_offered / n48_f828_find / n48_f828_apply on the
// offered segment at the kext's REAL geometry (the slot in root[511]'s window), then n48_md_scan over the WHOLE candidate
// against Apple's input (per-IB ranges, the segment starts, the fence identity) and n48_md_judge with the translator's own
// r4 sums. Required per frame: every segment translated in place, the fence applied, EXACTLY ONE fenceExempt row, the
// output fence at the input's own dword, and a CLEAN verdict. ASSUMED, as in T1: provenance asks answer yes (these frames
// committed, so the kext's own asks did) and non-exempt rows are resolved (there are none: r4 is 0/0 on every frame).
// In-test break: the tail compacted by one packet before the fence (a moved fence) -> the identity refuses -> ORIGIN.
// The scratch harness b490/fio runs the SAME steps over all 49 committed frames run10e captured.
// ---------------------------------------------------------------------------------------------------------------------
#include "gfx_mib.h"
#include "fixture_fio_run10e.h"
static uint32_t gFioF = 0u, gFioMiss = 0u;
static int fio_read(void *, uint64_t va, uint32_t ndw, uint32_t *out)
{
    for (const FioMem &m : kFioMem)
        if (m.frame == gFioF && va >= m.va && va + 4ull * ndw <= m.va + 4ull * m.ndw && !((va - m.va) & 3u)) {
            const uint32_t o = m.off + (uint32_t)((va - m.va) / 4u);
            for (uint32_t k = 0; k < ndw; k++) out[k] = kFioMemW[o + k];
            return 1;
        }
    gFioMiss++;
    return 0;
}
static int fio_yes(void *, uint64_t, uint32_t, uint32_t) { return 1; }
static int fio_csn(void *, uint64_t) { return 1; }
static uint32_t gFioIo[2];
static int fio_profile(void *, uint32_t stage, uint64_t va, xlat12_draw_profile *out)
{
    for (const FioPgm &p : kFioPgm) {
        if (p.frame != gFioF || p.stage != stage || p.va != va) continue;
        for (int i = 0; i < (int)xlat12_shader_id_count(); i++) {
            if (std::strcmp(xlat12_shader_id_name(i), p.name)) continue;
            if (xlat12_ib_profile_stage(i, out, gFioIo) != 0u) return 0;
            if (stage == 1u) out->vs_drops_params = (gFioIo[0] == 0u) ? 1u : 0u;
            return 1;
        }
        return 0;
    }
    gFioMiss++;
    return 0;
}
enum { FIO_MUT_NONE = 0, FIO_MUT_MOVE = 1 };
struct FioResult { uint32_t segs, segsOk, fenceWhy, applied, outAt, inAt, rows, exempt, r4w, r4m, clause; };
static FioResult fio_frame(const FioFrame &fr, uint32_t armFlags, int mut)
{
    FioResult R {}; R.fenceWhy = 0xFFu;
    gFioF = fr.frame;
    const uint32_t nib = fr.nib, n = fr.len0 + fr.len1;
    static uint32_t in[24000], cand[24000];
    if (n > 24000u) return R;
    std::memcpy(in, &kFioIbW[fr.off], n * 4u);
    std::memcpy(cand, in, n * 4u);                                   // gXdNew starts as a byte copy of Apple's IB
    const uint32_t mib = nib >= 2u ? 1u : 0u;
    uint32_t off[2] = { 0u, fr.len0 }, nn[2] = { fr.len0, fr.len1 };
    const uint64_t va[2] = { fr.va0, fr.va1 };
    xlat12_ib_segment segs[N48_XV_MAX_SEGS]; uint32_t ibnseg[2] = { 0u, 0u }, tot = 0u, ns = 0u;
    if (mib) { n48_mib_seg_diag dg {}; ns = n48_mib_segment(in, 2u, off, nn, segs, N48_XV_MAX_SEGS, ibnseg, &tot, &dg, 0u, nullptr); }
    else ns = xlat12_ib_segments(in, n, segs, N48_XV_MAX_SEGS, &tot);
    R.segs = ns;
    // the kext's real geometry: startVa's fallback 0x400000000 (kStartVa), root[511]'s window, the fence page at its end
    const uint64_t fencePageVa = kOwnRegionBase + (uint64_t)N48_F828_FENCE_PAGE_OFF;
    const uint32_t ordinal = 1u, want = n48_f828_value(0x5a5au, ordinal);
    const uint64_t slotVa = n48_f828_slot_va(fencePageVa, ordinal);
    static xlat12_ud_carry carry; carry = xlat12_ud_carry {};
    uint32_t segIb = 0u, pending = 0u;
    for (uint32_t k = 0; k < ns; k++) {
        const uint32_t from = segs[k].start, to = segs[k].end;
        if (mib) while (segIb + 1u < nib && from >= off[segIb] + nn[segIb]) segIb++;
        xlat12_draw_extra ex {};
        ex.pgm_profile = &fio_profile;
        ex.ring_va = kOwnRegionBase; ex.gs_sgpr0_va = kOwnRegionBase + 0xa80000ull;
        if (armFlags & XLAT12_EXTRA_RASTER) { ex.flags |= XLAT12_EXTRA_RASTER; ex.rsrc3_gs = XLAT12_RSRC3_GS_CU_EN; }
        if (armFlags & XLAT12_EXTRA_RASTER_PER_DRAW) { ex.flags |= XLAT12_EXTRA_RASTER | XLAT12_EXTRA_RASTER_PER_DRAW; ex.rsrc3_gs = XLAT12_RSRC3_GS_CU_EN; }
        if (armFlags & XLAT12_EXTRA_FILL_COLOR) { ex.flags |= XLAT12_EXTRA_FILL_COLOR; ex.fill_color_va = kOwnRegionBase + 0xA81000ull; }
        if (armFlags & XLAT12_EXTRA_PAIR_PRE) ex.flags |= XLAT12_EXTRA_PAIR_PRE;
        if (armFlags & XLAT12_EXTRA_READSET) ex.flags |= XLAT12_EXTRA_READSET;
        if (armFlags & XLAT12_EXTRA_CS_ELIDE) { ex.flags |= XLAT12_EXTRA_CS_ELIDE; ex.cs_ctx = &gFioF; ex.cs_is_n = &fio_csn; }
        if (armFlags & XLAT12_EXTRA_TABLE_DESC) {   // the kext's `if (dp)` block, in its order
            ex.flags |= XLAT12_EXTRA_INLINE_DESC | XLAT12_EXTRA_TABLE_DESC | XLAT12_EXTRA_DESC_INV;
            if ((armFlags & XLAT12_EXTRA_DESC_INV_APPLE_HEAD) && n48_mib_head_executes(in, segs[k].head, n))
                ex.flags |= XLAT12_EXTRA_DESC_INV_APPLE_HEAD;
            if (armFlags & XLAT12_EXTRA_TABLE_REUSE) ex.flags |= XLAT12_EXTRA_TABLE_REUSE;
            if (armFlags & XLAT12_EXTRA_UD_REEMIT) { ex.flags |= XLAT12_EXTRA_UD_REEMIT; ex.ud_carry = &carry; }
            if ((armFlags & XLAT12_EXTRA_VS_KNOWN) && (armFlags & XLAT12_EXTRA_UD_REEMIT)) ex.flags |= XLAT12_EXTRA_VS_KNOWN;
            ex.ib_va = mib ? n48_mib_seg_va(va[segIb], from, off[segIb]) : va[0] + 4ull * from;
            ex.desc_read = &fio_read; ex.desc_tiled_ok = &fio_yes; ex.desc_dcc_ok = &fio_yes;
            if (armFlags & XLAT12_EXTRA_DCC_STRIP) ex.flags |= XLAT12_EXTRA_DCC_STRIP;
        }
        static xlat12_draw_stats ds; ds = xlat12_draw_stats {};
        uint32_t olen = 0u;
        uint32_t *out = &cand[from];
        const uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &in[from], to - from, out, &olen, &ds);
        R.r4w += ds.r4_waits; R.r4m += ds.r4_memwrites;                  // summed whatever `st` says, as the kext does
        if (st) { std::memcpy(&cand[from], &in[from], (size_t)(to - from) * 4u); continue; }
        if (olen == to - from) R.segsOk++;
        const uint32_t offered = n48_f828_offered(k, ns, mib);
        if (olen != to - from || pending || !offered) continue;
        if (mut == FIO_MUT_MOVE) {
            // THE BREAK: the tail after the last draw re-encoded one packet SHORTER before the fence (the first non-NOP
            // packet after `draw_at` dropped, the rest moved down, NOP1-padded at the end) - a translator that compacts
            // its tail. The segment still walks, n48_f828_find still finds exactly one candidate.
            const uint32_t n0 = to - from;
            // draw_at is the segment's LAST draw, IB-local for the two-IB stage (gfx_mib.h), global for one IB
            const uint32_t lastDraw = segs[k].draw_at + (mib ? off[segIb] : 0u) - from;
            uint32_t i = 0u, cut = 0u, cutLen = 0u;
            while (i < n0) {
                const uint32_t h = out[i], l = n48_f828_pkt_len(h);
                if (!l) break;
                if (i > lastDraw && h != N48_F828_NOP1 && (h >> 30) == 3u && ((h >> 8) & 0xFFu) != 0x10u) { cut = i; cutLen = l; break; }
                if (i > lastDraw && h == N48_F828_NOP9 && out[i + 1u] == N48_F828_RELMEM) break;   // nothing before the fence
                i += l;
            }
            if (cutLen) {
                std::memmove(&out[cut], &out[cut + cutLen], (size_t)(n0 - cut - cutLen) * 4u);
                for (uint32_t q = n0 - cutLen; q < n0; q++) out[q] = N48_F828_NOP1;
            }
        }
        n48_f828 f {};
        R.fenceWhy = n48_f828_find(out, olen, &f);
        const uint32_t multiSeg = mib ? 0u : (ns > 1u ? 1u : 0u);
        if (R.fenceWhy != N48_F828_OK || multiSeg) continue;
        if (n48_f828_apply(out, olen, &f, slotVa, fencePageVa, want) != N48_F828_OK) continue;
        pending = 1u; R.applied = 1u; R.outAt = from + f.nop_at;
        n48_f828 fi {};
        R.inAt = n48_f828_find(&in[from], to - from, &fi) == N48_F828_OK ? from + fi.nop_at : 0xFFFFFFFFu;
    }
    n48_md_ib_range ibs[2]; uint32_t nIbs = 0u;
    if (mib) { for (uint32_t b = 0; b < nib; b++) { ibs[nIbs].va = va[b]; ibs[nIbs].bytes = 4ull * nn[b]; nIbs++; } }
    else { ibs[0].va = va[0]; ibs[0].bytes = 4ull * n; nIbs = 1u; }
    uint32_t starts[N48_XV_MAX_SEGS]; for (uint32_t k = 0; k < ns && k < N48_XV_MAX_SEGS; k++) starts[k] = segs[k].start;
    static n48_md_scan_result r;
    n48_md_scan(cand, n, in, n, kOwnRegionBase, kOwnRegionLen, ibs, nIbs, starts, ns,
                pending ? slotVa : 0ull, pending ? want : 0u, &r);
    for (uint32_t i = 0; i < r.n; i++) {
        if (r.d[i].fenceExempt) { R.exempt++; continue; }                // R5: the exempt row is never walked
        r.d[i].resolved = 1u; r.d[i].writable = 1u;                      // ASSUMED (T1's convention)
    }
    R.rows = r.n;
    uint64_t d = 0ull;
    R.clause = n48_md_judge(&r, R.r4w, R.r4m, 0u, &d);
    return R;
}
static void fio_checks()
{
    const uint32_t run10e = XLAT12_EXTRA_RASTER | XLAT12_EXTRA_RASTER_PER_DRAW | XLAT12_EXTRA_FILL_COLOR | XLAT12_EXTRA_PAIR_PRE |
                            XLAT12_EXTRA_READSET | XLAT12_EXTRA_TABLE_DESC | XLAT12_EXTRA_DESC_INV_APPLE_HEAD |
                            XLAT12_EXTRA_TABLE_REUSE | XLAT12_EXTRA_UD_REEMIT | XLAT12_EXTRA_VS_KNOWN;
    const uint32_t sets[2] = { run10e, run10e | XLAT12_EXTRA_CS_ELIDE | XLAT12_EXTRA_DCC_STRIP };
    const char *setName[2] = { "run10e's flags", "run10e's + 57 + 60" };
    char lbl[240];
    for (uint32_t s = 0; s < 2u; s++) {
        for (const FioFrame &fr : kFioFrames) {
            gFioMiss = 0u;
            const FioResult R = fio_frame(fr, sets[s], FIO_MUT_NONE);
            std::printf("      F%u (%s, %u IB, %u dw): segs %u ok %u, fence %s out@%u in@%u, rows %u exempt %u, r4 %u/%u -> %s\n",
                        fr.frame, setName[s], fr.nib, fr.len0 + fr.len1, R.segs, R.segsOk, R.applied ? "APPLIED" : "NOT applied",
                        R.outAt, R.inAt, R.rows, R.exempt, R.r4w, R.r4m, n48_md_reason_name(R.clause));
            std::snprintf(lbl, sizeof lbl, "FIO F%u %s: every segment translated in place, nothing read that the fixture lacks", fr.frame, setName[s]);
            expect_u(lbl, (R.segs && R.segsOk == R.segs && gFioMiss == 0u) ? 1u : 0u, 1u);
            std::snprintf(lbl, sizeof lbl, "FIO F%u %s: the fence found and applied on the translated final segment", fr.frame, setName[s]);
            expect_u(lbl, R.applied, 1u);
            std::snprintf(lbl, sizeof lbl, "FIO F%u %s: the output fence sits at Apple's own input dword (%u)", fr.frame, setName[s], R.inAt);
            expect_u(lbl, (R.applied && R.outAt == R.inAt) ? 1u : 0u, 1u);
            std::snprintf(lbl, sizeof lbl, "FIO F%u %s: exactly one fenceExempt row", fr.frame, setName[s]);
            expect_u(lbl, R.exempt, 1u);
            std::snprintf(lbl, sizeof lbl, "FIO F%u %s: R1 CLEAN over the whole candidate", fr.frame, setName[s]);
            expect_u(lbl, R.clause, N48_MD_OK);
        }
    }
    // THE BREAK: a moved fence. Only the 1040-dw frames carry a non-NOP packet (EVENT_WRITE) between the last draw and the
    // fence, so the break is asked there; the A frames' tail holds only the fence (their positive result above stands).
    for (const FioFrame &fr : kFioFrames) {
        if (fr.nib != 1u) continue;
        const FioResult R = fio_frame(fr, sets[0], FIO_MUT_MOVE);
        std::snprintf(lbl, sizeof lbl, "FIO break F%u: the tail compacted before the fence -> fence applied %u dw earlier, NOT exempt, ORIGIN",
                      fr.frame, R.inAt - R.outAt);
        expect_u(lbl, (R.applied && R.outAt < R.inAt && R.exempt == 0u && R.clause == N48_MD_ORIGIN) ? 1u : 0u, 1u);
    }
}

// build 0.0.535 fix round (c): DMA_DATA is classified explicitly. Switch 91's fill is recognised (counted, never a row, never
// KIND); Apple's dst_nowhere prefetch writes nothing (not a row, as before); any other DMA_DATA is a KIND refusal.
static void t9_nclear_checks()
{
    const uint32_t fill[7] = { XLAT12_NCLEAR_HDR, XLAT12_NCLEAR_CTRL, 0u, 0u, 0x01240000u, 4u, 32736u };
    const uint32_t pref[7] = { 0xC0055000u, 0x60200001u, 0x00600000u, 4u, 0u, 0u, 0x80000300u };   // run11v F116 IB1 @0x384
    uint32_t mem[7] = { 0xC0055000u, 0x40000000u, 0u, 0u, 0x01240000u, 4u, 0x40u };                 // DST_SEL 0: a memory write
    uint32_t kb = 0u, w = 0u, f = 0u, rf = 0u, m = 0u; uint64_t va = 0ull;
    expect_u("T9 the fill: not a destination row", n48_md_classify(fill, 0u, 7u, &kb, &va, &w, &f, &rf, &m), 0u);
    expect_u("T9 the fill: not KIND", kb, 0u);
    expect_u("T9 Apple's prefetch (dst_nowhere): not a row", n48_md_classify(pref, 0u, 7u, &kb, &va, &w, &f, &rf, &m), 0u);
    expect_u("T9 Apple's prefetch: not KIND", kb, 0u);
    (void)n48_md_classify(mem, 0u, 7u, &kb, &va, &w, &f, &rf, &m);
    expect_u("T9 any other DMA_DATA (DST_SEL 0 memory): KIND", kb, 1u);
    mem[1] = XLAT12_NCLEAR_CTRL; mem[2] = 1u;   // our control word but data 1: not our fill
    (void)n48_md_classify(mem, 0u, 7u, &kb, &va, &w, &f, &rf, &m);
    expect_u("T9 a fill-shaped DMA_DATA writing non-zero data: KIND", kb, 1u);
    uint32_t out[16]; for (uint32_t k = 0; k < 16u; k++) out[k] = 0xFFFF1000u;
    for (uint32_t k = 0; k < 7u; k++) { out[k] = fill[k]; out[7u + k] = fill[k]; }
    static n48_md_scan_result r;
    (void)n48_md_scan(out, 16u, out, 16u, 0ull, 0ull, nullptr, 0u, nullptr, 0u, 0ull, 0u, &r);
    expect_u("T9 the scan counts two fills, records no row, no KIND, walks clean", r.nFills == 2u && r.n == 0u && !r.kindBad && r.walkOk, 1u);
    uint64_t dd = 0ull;
    expect_u("T9 ... and judges them clean (COUNT 0/0)", n48_md_judge(&r, 0u, 0u, 0u, &dd), N48_MD_OK);
}

int main(int argc, char **argv)
{
    std::printf("== T9: switch 91's fill ==\n");
    t9_nclear_checks();
    const char *srcPath = argc > 1 ? argv[1] : "src/navi48-bringup/src/apple/AppleHardwareHook.cpp";
    std::printf("== T0: R0 reachability ==\n");
    t0_reachability(srcPath);
    std::printf("\n== T1: the real producers ==\n");
    t1_checks();
    std::printf("\n== T2: F48 with the fence applied (R1 tightened identity) ==\n");
    t2_checks();
    std::printf("\n== R2: ORIGIN redesigned ==\n");
    r2_checks();
    std::printf("\n== R4: OWN-IB per-IB ==\n");
    r4_checks();
    std::printf("\n== R5: no walk for a pre-refused row ==\n");
    r5_checks();
    std::printf("\n== R6: NOT-WRITABLE counted, never refusing ==\n");
    r6_checks();
    std::printf("\n== R7: KIND corrected ==\n");
    r7_checks();
    std::printf("\n== T3: the self-satisfied rule, pure ==\n");
    t3_checks();
    std::printf("\n== T4: root[511]'s window, by VA alone ==\n");
    t4_checks();
    std::printf("\n== T5: OWN-IB, COPY_DATA, EVENT_WRITE(_EOP) ==\n");
    t5_checks();
    std::printf("\n== T6: a scanner mutant trips COUNT ==\n");
    t6_checks();
    std::printf("\n== T8: the R4 discharge's own precondition ==\n");
    t8_checks();
    std::printf("\n== T7: OFF identity, wiring pins, R8 ==\n");
    t7_wiring_checks(srcPath);
    std::printf("\n== R9: per-class counters and report byte budget ==\n");
    r9_checks();
    std::printf("\n== 0.0.487: R1 over the compute-N elided candidate, and the gate rule ==\n");
    cs_elide_checks();
    std::printf("\n== 0.0.490: the fence in the translator's output (run10e committed frames) ==\n");
    fio_checks();
    std::printf("\n== 0.0.492: N48_MD_SEG_REFUSED (notes \xc2\xa7" "1099) ==\n");
    seg_refused_checks(srcPath);
    std::printf("\n%d check(s), %d failed.\n", gRun, gFail);
    if (!gFail) { std::printf("N48-MEMDST-TEST-PASS\n"); return 0; }
    std::printf("N48-MEMDST-TEST-FAIL\n");
    return 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// build 0.0.492 — N48_MD_SEG_REFUSED. RUN A's 10 CNT refusals were all composites with a translator-REFUSED
// segment: the kext restores Apple's bytes into a refused segment, the scan counts Apple's whole WRITE_DATA / RELEASE_MEM /
// WAIT_REG_MEM triplet there, and the translator's r4 sums stop at the refusal - scan 4/8 vs r4 2/4. Built here from REAL
// captured bytes: four copies of a real compute-N segment carrying Apple's triplet (fixture_compute_n.h), two translated, two
// "refused" (Apple's bytes restored, status non-zero, out_len 0, exactly as gfxsrc_policy stores them). Without the segment
// fact the frame judges COUNT (4/8 vs 2/4); with it, SEG_REFUSED, judged FIRST; md_ok 0 either way (fail-closed), and it is not
// shadow-bad. The planted-break case the brief names: a FULLY translated segment whose output carries a REGISTER-space
// WAIT_REG_MEM (the translator's r4 counts every WAIT_REG_MEM, the scan only memory-space ones) still judges COUNT - the new
// clause must never swallow a real count difference. Then the kext's own order, by content (never a line number).
// ---------------------------------------------------------------------------------------------------------------------
static uint32_t seg_translate(const uint32_t *in, uint32_t n, uint32_t *out, uint32_t *olen, uint32_t *r4w, uint32_t *r4m)
{
    xlat12_draw_extra ex {};
    ex.flags = XLAT12_EXTRA_CS_ELIDE; ex.cs_is_n = csn_md_is_n;
    xlat12_draw_stats ds {};
    const uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, in, n, out, olen, &ds);
    *r4w = ds.r4_waits; *r4m = ds.r4_memwrites;
    return st;
}
static void seg_refused_checks(const char *srcPath)
{
    const xlat12_fixture_csn *f = &kCsNSegs[0];
    const uint32_t n = f->n;
    static uint32_t in4[4 * 4096], out4[4 * 4096];
    if (n > 4096u) { expect_u("SR fixture fits", 0u, 1u); return; }
    uint32_t status[4], outLen[4], starts[4], r4w = 0, r4m = 0;
    for (uint32_t k = 0; k < 4; k++) {
        std::memcpy(&in4[k * n], f->dw, n * 4u);
        starts[k] = k * n;
        if (k < 2) {   // translated
            uint32_t ol = 0, w = 0, m = 0;
            status[k] = seg_translate(f->dw, n, &out4[k * n], &ol, &w, &m);
            outLen[k] = status[k] ? 0u : ol;
            if (!status[k]) { r4w += w; r4m += m; }
        } else {       // refused: Apple's bytes restored (`if (st && build) memcpy(...)`), out_len 0
            std::memcpy(&out4[k * n], f->dw, n * 4u);
            status[k] = 0xF7u; outLen[k] = 0u;
        }
    }
    expect_u("SR the two translated copies of the real N segment translate, full length",
             (status[0] == 0u && status[1] == 0u && outLen[0] == n && outLen[1] == n) ? 1u : 0u, 1u);
    uint32_t any = 0u, first = 0u;
    for (uint32_t k = 0; k < 4; k++) if (n48_md_seg_refused(status[k], outLen[k], n)) { if (!any) first = k; any = 1u; }
    expect_u("SR two refused segments are seen, the first is segment 2", (any == 1u && first == 2u) ? 1u : 0u, 1u);
    n48_md_scan_result r {}; uint64_t d = 0ull;
    n48_md_scan(out4, 4u * n, in4, 4u * n, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, starts, 4u, 0ull, 0u, &r);
    for (uint32_t i = 0; i < r.n; i++) { r.d[i].resolved = 1u; r.d[i].writable = 1u; }
    expect_u("SR \xc2\xa7" "1099's numbers: the scan counts 4 waits / 8 writes, the translator's r4 2 / 4",
             (r.nWaits == 4u && r.nWrites == 8u && r4w == 2u && r4m == 4u) ? 1u : 0u, 1u);
    const uint32_t without = n48_md_judge_frame(&r, 0u, 0u, r4w, r4m, 0u, &d);
    expect_u("SR WITHOUT the segment fact: COUNT (the artifact RUN A logged)", without, N48_MD_COUNT);
    expect_u("SR ...which is n48_md_judge's own answer", n48_md_judge(&r, r4w, r4m, 0u, &d), N48_MD_COUNT);
    const uint32_t with = n48_md_judge_frame(&r, any, first, r4w, r4m, 0u, &d);
    expect_u("SR WITH anySegRefused: SEG_REFUSED, judged first", with, N48_MD_SEG_REFUSED);
    expect_u("SR its detail names the first refused segment (index + 1)", d, 3ull);
    expect_u("SR md_ok stays 0 (fail-closed): gXdBuild.md_ok = (clause == N48_MD_OK)", (with == N48_MD_OK) ? 1u : 0u, 0u);
    expect_u("SR not shadow-bad; COUNT is", (n48_md_shadow_bad(with) == 0u && n48_md_shadow_bad(N48_MD_COUNT) == 1u &&
             n48_md_shadow_bad(N48_MD_OK) == 0u) ? 1u : 0u, 1u);
    expect_u("SR the reason is named", std::strcmp(n48_md_reason_name(N48_MD_SEG_REFUSED), "SEG-REFUSED") == 0 ? 1u : 0u, 1u);
    expect_u("SR appended at the end of the enum (every older clause keeps its value)",
             (N48_MD_COUNT == 10u && N48_MD_SEG_REFUSED == 11u && N48_MD_REASONS == 12u) ? 1u : 0u, 1u);
    // a translated segment whose output length differs from its input's is refused too
    expect_u("SR out_len != to - from counts as refused; equal and status 0 does not",
             (n48_md_seg_refused(0u, n - 1u, n) == 1u && n48_md_seg_refused(0u, n + 1u, n) == 1u && n48_md_seg_refused(0u, n, n) == 0u &&
              n48_md_seg_refused(1u, n, n) == 1u) ? 1u : 0u, 1u);
    // THE PLANTED-BREAK CASE: a fully translated segment with a REGISTER-space WAIT_REG_MEM still judges COUNT
    {
        static uint32_t in[4096], out[4096];
        std::memcpy(in, f->dw, n * 4u);
        uint32_t wi = ~0u;
        for (uint32_t i = 0; i < n; i += csn_md_len(in, i))
            if ((in[i] >> 30) == 3u && in[i] != 0xFFFF1000u && ((in[i] >> 8) & 0xFFu) == 0x3Cu) { wi = i; break; }
        expect_u("SR positive control: the segment carries a WAIT_REG_MEM", wi != ~0u ? 1u : 0u, 1u);
        if (wi != ~0u) {
            // MEM_SPACE 0 (register space), OPERATION 0 (wait only), polling a register the translator passes through unchanged
            // (the first XLAT12_CLS_IDENTICAL dword index outside the compute block 0x2e00-0x2e7f): the real register form.
            uint32_t ri = 0u;
            for (uint32_t k = 0x2000u; k < 0x10000u && !ri; k++) {
                uint32_t g12 = 0, cls = 0;
                if ((k < 0x2e00u || k > 0x2e7fu) && xlat12_lookup(k << 2, &g12, &cls) && cls == XLAT12_CLS_IDENTICAL) ri = k;
            }
            expect_u("SR an identical register exists to poll", ri != 0u ? 1u : 0u, 1u);
            in[wi + 1u] &= ~((1u << 4) | (3u << 5));
            in[wi + 2u] = ri; in[wi + 3u] = 0u;
            uint32_t ol = 0, w = 0, m = 0;
            const uint32_t st = seg_translate(in, n, out, &ol, &w, &m);
            expect_u("SR the register-space variant translates fully (status 0, out_len == in)", (st == 0u && ol == n) ? 1u : 0u, 1u);
            const uint32_t starts1[1] = { 0u };
            n48_md_scan_result r1 {}; uint64_t d1 = 0ull;
            n48_md_scan(out, n, in, n, 0x23F0000000ull, 0x10000000ull, nullptr, 0u, starts1, 1u, 0ull, 0u, &r1);
            for (uint32_t i = 0; i < r1.n; i++) { r1.d[i].resolved = 1u; r1.d[i].writable = 1u; }
            const uint32_t anyR = n48_md_seg_refused(st, st ? 0u : ol, n);
            expect_u("SR ...no segment refused, and the counts differ (r4 counts the register wait, the scan does not)",
                     (anyR == 0u && w == 1u && r1.nWaits == 0u) ? 1u : 0u, 1u);
            expect_u("SR ...so the frame judges COUNT, not SEG_REFUSED", n48_md_judge_frame(&r1, anyR, 0u, w, m, 0u, &d1), N48_MD_COUNT);
        }
    }
    // anySegRefused 0 IS n48_md_judge, clause and detail, over randomized records
    {
        uint64_t seed = 0x1099ull; unsigned diff = 0;
        auto rnd = [&seed]() { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(seed >> 33); };
        for (int it = 0; it < 20000; it++) {
            n48_md_scan_result q {};
            q.walkOk = (rnd() % 8) != 0; q.over = (rnd() % 16) == 0; q.kindBad = (rnd() % 16) == 0;
            q.n = rnd() % 5;
            for (uint32_t i = 0; i < q.n; i++) {
                q.d[i].va = rnd(); q.d[i].originOk = (rnd() % 8) != 0; q.d[i].ownRegion = (rnd() % 16) == 0; q.d[i].ownIb = (rnd() % 16) == 0;
                q.d[i].isWait = rnd() & 1; q.d[i].waitSatisfied = (rnd() % 4) != 0; q.d[i].fenceExempt = (rnd() % 8) == 0;
                q.d[i].resolved = (rnd() % 8) != 0; q.d[i].writable = (rnd() % 8) != 0;
            }
            q.nWaits = rnd() % 3; q.nWrites = rnd() % 3;
            const uint32_t a = rnd() % 3, b = rnd() % 3, nw = rnd() & 1;
            uint64_t d0 = 1, dd = 2;
            if (n48_md_judge(&q, a, b, nw, &d0) != n48_md_judge_frame(&q, 0u, rnd() % 32, a, b, nw, &dd) || d0 != dd) diff++;
            if (n48_md_judge_frame(&q, 1u, 0u, a, b, nw, &dd) != N48_MD_SEG_REFUSED) diff++;
        }
        expect_u("SR anySegRefused 0: n48_md_judge_frame == n48_md_judge (clause and detail), 20000 random records; 1: always SEG_REFUSED", diff, 0u);
    }
    // THE KEXT'S ORDER (AppleHardwareHook.cpp, by content): the per-segment status is stored in the segment loop; the R1 block
    // reads every segment's final status and length, THEN judges the frame through n48_md_judge_frame, THEN sets md_ok from its
    // answer, and SHADOW's tally uses n48_md_shadow_bad. No direct n48_md_judge(&mdRes ...) call is left.
    std::ifstream fs(srcPath);
    if (!fs) { expect_u("SR the kext source is readable", 0u, 1u); return; }
    std::stringstream ss; ss << fs.rdbuf();
    const std::string src = ss.str();
    auto order = [](const std::string &t) -> unsigned {
        const size_t st = t.find("        f->seg_status[k] = st;");
        const size_t ln = t.find("        gXdBuild.seg[k].out_len = st ? 0u : olen;");
        const size_t blk = t.find("if (gMdMode && build && gXdBuild.ok) {");
        const size_t loop = t.find("if (n48_md_seg_refused(f->seg_status[si], gXdBuild.seg[si].out_len, gXdBuild.seg[si].end - gXdBuild.seg[si].start)) {");
        const size_t judge = t.find("const uint32_t mdClause = n48_md_judge_frame(&mdRes, mdAnySegRefused, mdFirstRefused, frameR4Waits, frameR4Memwrites,");
        const size_t ok = t.find("gXdBuild.md_ok = (mdClause == N48_MD_OK) ? 1u : 0u;");
        const size_t sb = t.find("if (gMdMode == N48_MD_MODE_SHADOW && n48_md_shadow_bad(mdClause)) gMemDst.shadowBad++;");
        unsigned bad = 0;
        if (st == std::string::npos || ln == std::string::npos || blk == std::string::npos || loop == std::string::npos ||
            judge == std::string::npos || ok == std::string::npos || sb == std::string::npos) return 100u;
        if (!(st < blk && ln < blk && blk < loop && loop < judge && judge < ok && ok < sb)) bad++;
        if (t.find("n48_md_judge(&mdRes,") != std::string::npos) bad++;
        if (t.find("gMemDst.clause[N48_MD_SEG_REFUSED]") == std::string::npos) bad++;
        return bad;
    };
    expect_u("SR the kext's order: status stored -> R1 block -> every segment read -> n48_md_judge_frame -> md_ok -> shadow tally", order(src), 0u);
    struct { const char *what, *from, *to; } plant[] = {
        { "SEG_REFUSED removed (the frame judged by n48_md_judge again)",
          "const uint32_t mdClause = n48_md_judge_frame(&mdRes, mdAnySegRefused, mdFirstRefused, frameR4Waits, frameR4Memwrites,\n                                                     kMdNotWritableConfirmed, &mdDetail);",
          "const uint32_t mdClause = n48_md_judge(&mdRes, frameR4Waits, frameR4Memwrites, kMdNotWritableConfirmed, &mdDetail);" },
        { "shadowBad counts SEG_REFUSED again", "if (gMdMode == N48_MD_MODE_SHADOW && n48_md_shadow_bad(mdClause)) gMemDst.shadowBad++;",
          "if (gMdMode == N48_MD_MODE_SHADOW && mdClause != N48_MD_OK) gMemDst.shadowBad++;" },
    };
    for (const auto &p : plant) {
        std::string m = src;
        const size_t a = m.find(p.from);
        if (a == std::string::npos) { expect_u("SR plant setup: the text is found", 0u, 1u); continue; }
        m.replace(a, std::strlen(p.from), p.to);
        char lbl[200]; std::snprintf(lbl, sizeof lbl, "SR BREAK-check: %s - caught", p.what);
        expect_u(lbl, order(m) > 0u ? 1u : 0u, 1u);
    }
}
