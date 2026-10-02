// sdma_gcr_test.cpp — 0.0.416 (notes/design/SDMA-GCR.md,), binding G5. The pure half of the SDMA
// cache-rinse build, host-tested:
//   1. G1 — the ONE emitter's five dwords are EXACTLY 0x00000011, 0x00000000, 0xC3A10000, 0x00000000,
//      0x00000000, and Linux's gcr_cntl is the sum its own flags name;
//   2. G2 — the source-window refusal rules (alignment, VRAM range, our own allocation pools) and the
//      mode-7 scalar packing/decoding;
//   3. G3 — the ring-space arithmetic (18 dwords plain, 23 with the GCR_REQ; the linear copy 12/17).
// The kext compiles the SAME header (src/apple/sdma_gcr.h). NON-VACUITY is the scanout_selftest_test style:
// the same checks are re-run against MUTANTS — local copies with one defect each — and every mutant must be
// CAUGHT. In addition the reviewer separately breaks the REAL header and shows this test fail (G5).
//
// Build and run on the host Mac (no kernel headers, no hardware, no PC):
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/sdma_gcr_test.cpp -o /tmp/gcrtest && /tmp/gcrtest
#include <cstdio>
#include <cstdint>

#include "sdma_gcr.h"

static int gFail = 0, gRun = 0;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        std::printf("FAIL  %-78s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
    } else {
        std::printf("ok    %-78s %#llx\n", what, (unsigned long long)got);
    }
}

// -------------------------------------------------------------------------------------------------------------------
// The functions under test, in one struct so a mutant can replace exactly one.
// -------------------------------------------------------------------------------------------------------------------
struct Fns {
    uint32_t (*emit)(uint32_t *);
    uint32_t (*src)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    uint32_t (*tiled)(int);
    const char *name;
};

static int checks(const Fns &fn)
{
    const int f0 = gFail;

    // --- G1: the five dwords, exactly ---
    {
        uint32_t pkt[8] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                            0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
        const uint32_t n = fn.emit(pkt);
        expect_u("emitter returns 5 dwords", n, N48_SDMA_GCR_REQ_DWORDS);
        expect_u("dw0 is 0x00000011 (op 17, sub_op 0)", pkt[0], 0x00000011u);
        expect_u("dw1 is 0x00000000 (base_va_31_7 = 0)", pkt[1], 0x00000000u);
        expect_u("dw2 is 0xC3A10000 (gcr_control_15_0 = 0xC3A1)", pkt[2], 0xC3A10000u);
        expect_u("dw3 is 0x00000000 (control_18_16 | limit_va_31_7)", pkt[3], 0x00000000u);
        expect_u("dw4 is 0x00000000 (limit_va_47_32 | vmid)", pkt[4], 0x00000000u);
        // The emitter must not have written a sixth dword.
        expect_u("dw5 is untouched", pkt[5], 0xFFFFFFFFu);
        // The control word the emitter packs, from its own flag definitions.
        expect_u("gcr_cntl is 0xC3A1", N48_SDMA_GCR_CNTL, 0xC3A1u);
        expect_u("GL2_WB is bit 15", SDMA_GCR_GL2_WB, 1u << 15);
        expect_u("GL2_INV is bit 14", SDMA_GCR_GL2_INV, 1u << 14);
        expect_u("control fits the 15_0 field", N48_SDMA_GCR_CNTL & 0xffffu, N48_SDMA_GCR_CNTL);
        expect_u("control has no 18_16 bits", (N48_SDMA_GCR_CNTL >> 16) & 0x7u, 0u);
    }

    // --- G2: the mode-7 scalar packing and decoding ---
    {
        const uint64_t off = 0x11200000ull;                    // the live plane's VRAM offset (arm30,)
        expect_u("mode 7, no gcr, packs to off|7", n48_scanout7_scalar(off, 0), off | 7u);
        expect_u("mode 7, gcr, packs to off|0x87", n48_scanout7_scalar(off, 1), off | 0x87u);
        expect_u("scalar decodes the offset (plain)", n48_scanout7_off(n48_scanout7_scalar(off, 0)), off);
        expect_u("scalar decodes the offset (gcr)", n48_scanout7_off(n48_scanout7_scalar(off, 1)), off);
        expect_u("scalar decodes gcr=0", n48_scanout7_gcr(n48_scanout7_scalar(off, 0)), 0u);
        expect_u("scalar decodes gcr=1", n48_scanout7_gcr(n48_scanout7_scalar(off, 1)), 1u);
        expect_u("the mode bits are 7", n48_scanout7_scalar(off, 1) & N48_SCANOUT_MODE_MASK, 7u);
        expect_u("control (off 0) is mode 7", n48_scanout7_scalar(0, 0), 7u);
        expect_u("the gcr flag does not leak into the offset", n48_scanout7_off(0x87u), 0ull);
        // D2 (0.0.417): the scalar is 64-bit. An offset >= 4 GiB must survive the packing; with
        // the 0.0.416 uint32_t return the high half was truncated and the CLI could not send it.
        const uint64_t big = 0x123450000ull;                   // > 4 GiB, 4-KiB aligned
        expect_u("mode 7, 64-bit offset above 4 GiB", n48_scanout7_scalar(big, 0), big | 7u);
        expect_u("mode 7, 64-bit offset with GCR", n48_scanout7_scalar(big, 1), big | 0x87u);
        expect_u("scalar decodes a 64-bit offset", n48_scanout7_off(n48_scanout7_scalar(big, 0)), big);
        expect_u("scalar decodes a 64-bit offset with GCR", n48_scanout7_off(n48_scanout7_scalar(big, 1)), big);
        expect_u("the high 32 bits are not lost", (uint64_t)(n48_scanout7_scalar(big, 0) >> 32), big >> 32);
    }

    // --- G2: the source-window refusals. Pools: main [0x2000000, 0x10000000), hi [0x400000000, 0x100000000). ---
    {
        const uint64_t vram = 16ull << 30;                     // 16 GiB
        const uint64_t aB = 0x2000000ull, aS = 0x0E000000ull;  // [0x02000000, 0x10000000)
        const uint64_t hB = 0x380000000ull, hS = 0x08000000ull;   // [14 GiB, 14.125 GiB)
        const uint64_t W = 0x40000ull;                         // the 256 KiB window
        expect_u("aligned, in range, clear of both pools -> ok",
                 fn.src(0x11200000ull, W, vram, aB, aS, hB, hS), (uint64_t)kN48GcrSrcOk);
        expect_u("the window ending exactly at VRAM -> ok",
                 fn.src(vram - W, W, vram, aB, aS, hB, hS), (uint64_t)kN48GcrSrcOk);
        expect_u("the window ending exactly at a pool's base -> ok",
                 fn.src(aB - W, W, vram, aB, aS, hB, hS), (uint64_t)kN48GcrSrcOk);
        expect_u("misaligned offset -> align", fn.src(0x11200800ull, W, vram, aB, aS, hB, hS),
                 (uint64_t)kN48GcrSrcAlign);
        expect_u("zero bytes -> range", fn.src(0x11200000ull, 0, vram, aB, aS, hB, hS),
                 (uint64_t)kN48GcrSrcRange);
        expect_u("past the card's VRAM -> range", fn.src(vram - W + 0x1000ull, W, vram, aB, aS, hB, hS),
                 (uint64_t)kN48GcrSrcRange);
        expect_u("overlapping the main pool -> overlap", fn.src(aB + 0x1000ull, W, vram, aB, aS, hB, hS),
                 (uint64_t)kN48GcrSrcOverlap);
        expect_u("overlapping the hi pool -> overlap", fn.src(hB + 0x8000ull, W, vram, aB, aS, hB, hS),
                 (uint64_t)kN48GcrSrcOverlap);
        expect_u("the live plane is clear of both pools", fn.src(0x11200000ull, W, vram, aB, aS, hB, hS),
                 (uint64_t)kN48GcrSrcOk);
        expect_u("an uninitialised pool is ignored", fn.src(0x11200000ull, W, vram, 0, 0, 0, 0),
                 (uint64_t)kN48GcrSrcOk);
    }

    // --- G3: the ring-space arithmetic ---
    {
        expect_u("tiled copy plain is 18 dwords", fn.tiled(0), 18u);
        expect_u("tiled copy with GCR is 23 dwords", fn.tiled(1), 23u);
        expect_u("the GCR adds exactly 5", fn.tiled(1) - fn.tiled(0), 5u);
        expect_u("tiled copy is 14 + 4", fn.tiled(0), N48_SDMA_TILED_SUB_WINDOW_DWORDS + N48_SDMA_FENCE_DWORDS);
        expect_u("linear copy plain", n48_sdma_linear_copy_dwords(0), 12u);
        expect_u("linear copy with GCR", n48_sdma_linear_copy_dwords(1), 17u);
    }

    return gFail - f0;
}

// -------------------------------------------------------------------------------------------------------------------
// Mutants: one defect each. A mutant that passes every check means the check is vacuous.
// -------------------------------------------------------------------------------------------------------------------
static uint32_t emit_mut_op(uint32_t *pkt)          // the wrong opcode (18, not 17)
{
    const uint32_t n = n48_sdma_gcr_req(pkt);
    pkt[0] = 18u;
    return n;
}
static uint32_t emit_mut_no_wb(uint32_t *pkt)      // drops GL2_WB: a rinse that does not write back
{
    const uint32_t n = n48_sdma_gcr_req(pkt);
    pkt[2] = (uint32_t)(((N48_SDMA_GCR_CNTL & ~(uint32_t)SDMA_GCR_GL2_WB) & 0xffffu) << 16);
    return n;
}
static uint32_t emit_mut_shift(uint32_t *pkt)      // control moved to the wrong payload dword
{
    const uint32_t n = n48_sdma_gcr_req(pkt);
    pkt[2] = 0;
    pkt[3] = (uint32_t)((N48_SDMA_GCR_CNTL & 0xffffu) << 16);
    return n;
}
static uint32_t src_mut_no_align(uint64_t off, uint64_t b, uint64_t v, uint64_t aB, uint64_t aS, uint64_t hB, uint64_t hS)
{   // alignment not checked
    if (b == 0 || off > v || b > v - off) return kN48GcrSrcRange;
    if (aS && off < aB + aS && aB < off + b) return kN48GcrSrcOverlap;
    if (hS && off < hB + hS && hB < off + b) return kN48GcrSrcOverlap;
    return kN48GcrSrcOk;
}
static uint32_t src_mut_no_overlap(uint64_t off, uint64_t b, uint64_t v, uint64_t aB, uint64_t aS, uint64_t hB, uint64_t hS)
{   // our own allocation pools not checked
    (void)aB; (void)aS; (void)hB; (void)hS;
    if (b == 0) return kN48GcrSrcRange;
    if (off & 0xfffull) return kN48GcrSrcAlign;
    if (off > v || b > v - off) return kN48GcrSrcRange;
    return kN48GcrSrcOk;
}
static uint32_t src_mut_no_range(uint64_t off, uint64_t b, uint64_t v, uint64_t aB, uint64_t aS, uint64_t hB, uint64_t hS)
{   // VRAM bound not checked (only the power-of-two wraparound)
    (void)v;
    if (b == 0) return kN48GcrSrcRange;
    if (off & 0xfffull) return kN48GcrSrcAlign;
    if (aS && off < aB + aS && aB < off + b) return kN48GcrSrcOverlap;
    if (hS && off < hB + hS && hB < off + b) return kN48GcrSrcOverlap;
    return kN48GcrSrcOk;
}
static uint32_t tiled_mut_no_gcr(int gcr)          // the GCR_REQ is not accounted for in the ring space
{
    (void)gcr;
    return N48_SDMA_TILED_SUB_WINDOW_DWORDS + N48_SDMA_FENCE_DWORDS;
}

int main()
{
    const Fns real = { n48_sdma_gcr_req, n48_gcr_src_reason, n48_sdma_tiled_copy_dwords, "real" };
    const Fns mutants[] = {
        { emit_mut_op,      n48_gcr_src_reason,     n48_sdma_tiled_copy_dwords, "emitter wrong opcode" },
        { emit_mut_no_wb,   n48_gcr_src_reason,     n48_sdma_tiled_copy_dwords, "emitter drops GL2_WB" },
        { emit_mut_shift,   n48_gcr_src_reason,     n48_sdma_tiled_copy_dwords, "emitter control in dw3" },
        { n48_sdma_gcr_req, src_mut_no_align,       n48_sdma_tiled_copy_dwords, "source ignores alignment" },
        { n48_sdma_gcr_req, src_mut_no_overlap,     n48_sdma_tiled_copy_dwords, "source ignores our pools" },
        { n48_sdma_gcr_req, src_mut_no_range,       n48_sdma_tiled_copy_dwords, "source ignores the VRAM bound" },
        { n48_sdma_gcr_req, n48_gcr_src_reason,     tiled_mut_no_gcr,            "ring space misses the GCR" },
    };

    std::printf("--- real (sdma_gcr.h) ---\n");
    const int r0 = gRun;
    const int realFail = checks(real);
    std::printf("\nREAL: %d check(s), %d failure(s)\n", gRun - r0, realFail);

    int caught = 0, total = 0;
    for (unsigned m = 0; m < sizeof(mutants) / sizeof(mutants[0]); m++) {
        std::printf("\n--- MUTANT %s ---\n", mutants[m].name);
        const int m0 = gRun;
        const int f = checks(mutants[m]);
        total++;
        if (f > 0) { caught++; std::printf("MUTANT %s CAUGHT: %d check(s) failed of %d\n", mutants[m].name, f, gRun - m0); }
        else std::printf("MUTANT %s ESCAPED (the test does not discriminate)\n", mutants[m].name);
    }

    const bool ok = (realFail == 0) && (caught == total);
    std::printf("\nREAL: %d/%d passed. non-vacuity: %d of %d mutants caught.\n", gRun - r0 - realFail, gRun - r0, caught, total);
    return ok ? 0 : 1;
}
