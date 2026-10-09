// sdma_dcc_test.cpp — 0.0.417 (an internal design note, binding D5,). The pure half of the
// SDMA0_DCC_CNTL no-PTE compression build, host-tested:
//   1. D1's constants: the two register offsets, and the mask 0x00015554 = exactly the eight
//      *_COMP_EN_n bits, sharing no bit with the eight *_OVERRIDE_n bits (0x0000AAAA) or the bypass
//      (bit 0) — gc_12_0_0_sh_mask.h:334-367;
//   2. D1's arithmetic: 0xaabe -> 0xaaaa, idempotent, and the clear never touches an override or the
//      bypass bit;
//   3. D1's per-set field decode;
//   4. D1's argument rules: 0 read, 1 set, 2 restore only AFTER a capture, everything else refused.
// The kext compiles the SAME header (src/apple/sdma_dcc.h). NON-VACUITY is the sdma_gcr_test style: the
// same checks are re-run against MUTANTS — local copies with one defect each — and every mutant must be
// CAUGHT. In addition the reviewer separately breaks the REAL header and shows this test fail (D5).
//
// Build and run on the host Mac (no kernel headers, no hardware, no PC):
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/sdma_dcc_test.cpp -o /tmp/dcctest && /tmp/dcctest
#include <cstdio>
#include <cstdint>

#include "sdma_dcc.h"

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
    uint32_t (*clear)(uint32_t);
    uint32_t (*op)(uint64_t, int);
    uint32_t (*def)(uint32_t, uint32_t);   // E1 (0.0.418): the default-on decision (boot_present, boot_value)
    const char *name;
};

static int checks(const Fns &fn)
{
    const int f0 = gFail;

    // --- the mask arithmetic ---
    {
        const uint32_t v = 0x0000aabeu;                       // the value the parked PC reads
        expect_u("0xaabe clears to 0xaaaa", fn.clear(v), 0x0000aaaau);
        expect_u("the clear is idempotent", fn.clear(fn.clear(v)), 0x0000aaaau);
        expect_u("clearing 0xaaaa is 0xaaaa", fn.clear(0x0000aaaau), 0x0000aaaau);
        expect_u("clearing 0 is 0", fn.clear(0u), 0u);
        // Every override bit and the bypass bit survive the clear, for a spread of inputs.
        const uint32_t inputs[] = { 0x0000aabeu, 0x0000aaaau, 0xffffffffu, 0x0001ffffu, 0x00010000u, 0x0000fffeu };
        for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
            const uint32_t r = fn.clear(inputs[i]);
            expect_u("the clear preserves the OVERRIDE bits", r & N48_DCC_NOPTE_OVERRIDE_MASK,
                     inputs[i] & N48_DCC_NOPTE_OVERRIDE_MASK);
            expect_u("the clear preserves DCC_FORCE_BYPASS", r & N48_DCC_FORCE_BYPASS_MASK,
                     inputs[i] & N48_DCC_FORCE_BYPASS_MASK);
            expect_u("the clear removes every COMP_EN bit", r & N48_DCC_NOPTE_COMP_EN_MASK, 0u);
        }
        // The mask itself: the eight COMP_EN bits, disjoint from overrides and bypass.
        expect_u("the mask is 0x00015554", N48_DCC_NOPTE_COMP_EN_MASK, 0x00015554u);
        expect_u("the mask shares no bit with the overrides",
                 N48_DCC_NOPTE_COMP_EN_MASK & N48_DCC_NOPTE_OVERRIDE_MASK, 0u);
        expect_u("the mask shares no bit with the bypass",
                 N48_DCC_NOPTE_COMP_EN_MASK & N48_DCC_FORCE_BYPASS_MASK, 0u);
        expect_u("the override union is 0x0000aaaa", N48_DCC_NOPTE_OVERRIDE_MASK, 0x0000aaaau);
        expect_u("the defined bits are 0x0001ffff", N48_DCC_DEFINED_MASK, 0x0001ffffu);
    }

    // --- the per-set field decode ---
    {
        // set0 rd ovr/comp = 1/0, wr ovr/comp = 1/0 ; set1..3 all 1 -> build the value from the shifts.
        const uint32_t v = 0x0000aabeu;   // rd ovr 1, rd comp 1, wr ovr 1, wr comp 1 in set 0; overrides set in 1..3
        expect_u("set0 rd override", n48_sdma_dcc_rd_override(v, 0), 1u);
        expect_u("set0 rd comp", n48_sdma_dcc_rd_comp(v, 0), 1u);
        expect_u("set0 wr override", n48_sdma_dcc_wr_override(v, 0), 1u);
        expect_u("set0 wr comp", n48_sdma_dcc_wr_comp(v, 0), 1u);
        expect_u("set1 rd comp is OFF on the live value", n48_sdma_dcc_rd_comp(v, 1), 0u);
        expect_u("set1 rd override is ON", n48_sdma_dcc_rd_override(v, 1), 1u);
        expect_u("set3 wr comp is OFF", n48_sdma_dcc_wr_comp(v, 3), 0u);
        expect_u("set3 wr override is ON", n48_sdma_dcc_wr_override(v, 3), 1u);
        expect_u("the bypass bit is 0 on the live value", n48_sdma_dcc_force_bypass(v), 0u);
        expect_u("the bypass bit decodes", n48_sdma_dcc_force_bypass(0x00000001u), 1u);
        expect_u("an out-of-range set reads as 0", n48_sdma_dcc_rd_override(v, 4), 0u);
        expect_u("after the clear every set reads comp OFF", n48_sdma_dcc_rd_comp(fn.clear(v), 0) |
                                                               n48_sdma_dcc_rd_comp(fn.clear(v), 1) |
                                                               n48_sdma_dcc_rd_comp(fn.clear(v), 2) |
                                                               n48_sdma_dcc_rd_comp(fn.clear(v), 3), 0u);
    }

    // --- the argument rules ---
    {
        expect_u("0 -> READ (captured or not)", fn.op(0u, 0), kN48DccOpRead);
        expect_u("0 -> READ after a capture", fn.op(0u, 1), kN48DccOpRead);
        expect_u("1 -> SET", fn.op(1u, 0), kN48DccOpSet);
        expect_u("1 -> SET after a capture", fn.op(1u, 1), kN48DccOpSet);
        expect_u("2 before a capture -> REFUSED", fn.op(2u, 0), kN48DccOpRefused);
        expect_u("2 after a capture -> RESTORE", fn.op(2u, 1), kN48DccOpRestore);
        expect_u("3 -> REFUSED", fn.op(3u, 0), kN48DccOpRefused);
        expect_u("3 -> REFUSED after a capture", fn.op(3u, 1), kN48DccOpRefused);
        expect_u("0x1000000000000001 -> REFUSED", fn.op(0x1000000000000001ull, 1), kN48DccOpRefused);
        expect_u("the status codes are distinct", (kN48DccStOk == 0 && kN48DccStBadArg != kN48DccStOk &&
                 kN48DccStNoContext != kN48DccStBadArg && kN48DccStNoGc != kN48DccStNoContext &&
                 kN48DccStMismatch != kN48DccStNoGc) ? 1u : 0u, 1u);
    }

    // --- E1 (0.0.418, an internal design note): THE SDMA0_DCC_CNTL CLEAR IS THE DEFAULT. ---
    // The default is ON when the boot-arg is absent or nonzero, and SKIPPED by an explicit `navi48-sdmadcc=0`.
    // Its write is the SAME arithmetic `sdmadcc 1` writes: `n48_sdma_dcc_cleared` of the captured value.
    {
        expect_u("E1: boot-arg ABSENT -> the default RUNS", fn.def(0u, 0u), 1u);
        expect_u("E1: navi48-sdmadcc=1 -> RUNS", fn.def(1u, 1u), 1u);
        expect_u("E1: navi48-sdmadcc=0 -> SKIPPED", fn.def(1u, 0u), 0u);
        expect_u("E1: any other nonzero value -> RUNS", fn.def(1u, 2u), 1u);
        expect_u("E1: the default target is cleared(captured) on the live value",
                 fn.clear(0x0000aabeu), 0x0000aaaau);
        expect_u("E1: the default target preserves the overrides and the bypass",
                 fn.clear(0x0000aabeu) & (N48_DCC_NOPTE_OVERRIDE_MASK | N48_DCC_FORCE_BYPASS_MASK), 0x0000aaaau);
        // The one DEFAULT line the kext prints, WITH the logger's own prefix and newline, at maximal fields.
        {
            char line[512];
            const int ln = std::snprintf(line, sizeof line,
                "Navi48Bringup: sdmadcc: DEFAULT SDMA0_DCC_CNTL %#010x -> %#010x (mask %#x), read back %#010x %s\n",
                0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, "MISMATCH");
            expect_u("E1: the DEFAULT log line fits under 512 bytes", (uint64_t)ln < 512u, 1u);
        }
    }

    return gFail - f0;
}

// -------------------------------------------------------------------------------------------------------------------
// Mutants: one defect each. A mutant that passes every check means the check is vacuous.
// -------------------------------------------------------------------------------------------------------------------
static uint32_t clear_mut_override(uint32_t v)   // the "mask" also clears the override bits
{
    return v & ~(N48_DCC_NOPTE_COMP_EN_MASK | N48_DCC_NOPTE_OVERRIDE_MASK);
}
static uint32_t clear_mut_bypass(uint32_t v)     // the "mask" also clears DCC_FORCE_BYPASS
{
    return v & ~(N48_DCC_NOPTE_COMP_EN_MASK | N48_DCC_FORCE_BYPASS_MASK);
}
static uint32_t clear_mut_nothing(uint32_t v)    // the mask is a no-op (never compresses-off)
{
    (void)v;
    return v;
}
static uint32_t op_mut_restore_any(uint64_t arg, int captured)   // 2 restores before a capture
{
    (void)captured;
    if (arg == 0u) return kN48DccOpRead;
    if (arg == 1u) return kN48DccOpSet;
    if (arg == 2u) return kN48DccOpRestore;
    return kN48DccOpRefused;
}
static uint32_t op_mut_accept3(uint64_t arg, int captured)       // 3 is accepted as READ
{
    (void)captured;
    if (arg == 0u || arg == 3u) return kN48DccOpRead;
    if (arg == 1u) return kN48DccOpSet;
    if (arg == 2u) return kN48DccOpRestore;
    return kN48DccOpRefused;
}
// E1 (0.0.418): the two plausible ways to write the default-on decision wrongly. `always` ignores the boot-arg
// (so `navi48-sdmadcc=0` could not skip the write); `never` never runs the default. Both must be CAUGHT.
static uint32_t def_mut_always(uint32_t, uint32_t) { return 1u; }
static uint32_t def_mut_never (uint32_t, uint32_t) { return 0u; }

int main()
{
    const Fns real = { n48_sdma_dcc_cleared, n48_sdma_dcc_op, n48_sdma_dcc_default_on, "real" };
    const Fns mutants[] = {
        { clear_mut_override, n48_sdma_dcc_op, n48_sdma_dcc_default_on, "the mask also clears overrides" },
        { clear_mut_bypass,   n48_sdma_dcc_op, n48_sdma_dcc_default_on, "the mask also clears the bypass" },
        { clear_mut_nothing,  n48_sdma_dcc_op, n48_sdma_dcc_default_on, "the mask is a no-op" },
        { n48_sdma_dcc_cleared, op_mut_restore_any, n48_sdma_dcc_default_on, "2 restores before a capture" },
        { n48_sdma_dcc_cleared, op_mut_accept3,     n48_sdma_dcc_default_on, "3 is accepted" },
        { n48_sdma_dcc_cleared, n48_sdma_dcc_op, def_mut_always, "E1: the boot-arg cannot skip the default" },
        { n48_sdma_dcc_cleared, n48_sdma_dcc_op, def_mut_never,  "E1: the default never runs" },
    };

    std::printf("--- real (sdma_dcc.h) ---\n");
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
