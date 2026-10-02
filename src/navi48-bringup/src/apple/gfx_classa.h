// gfx_classa.h — THE ONE INSTRUMENT THAT NAMES CLASS A'S SOURCE. Pure C, host-tested by
// tests/gfx_classa_test.cpp (with planted defects); the kext compiles the SAME header.
// 0.0.385. NOTHING HERE READS OR WRITES HARDWARE: it is arithmetic and a table.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHAT CLASS A IS, AND WHY THE LOGS CANNOT NAME IT
// ---------------------------------------------------------------------------------------------------------------------
// Class A is the SQG (CID 0xb) wild-address READ fault, GCVM_L2_PROTECTION_FAULT_STATUS_LO32 = 0x00201733, that has
// killed our committed frame on five of six armed boots. Its base is boot-random; its walk is regular — one request per
// 4 KiB page, each element two consecutive pages, stride S = 2 KiB x {28, 27, 32, 8}, two interleaved streams, 43 faults
// on three boots and 102 on a fourth. decoded the frame and ruled out EVERYTHING in it: both shader programs' only
// memory ops are client-TCP and SQC, never SQG; every SGPR is SPI-initialised and holds a captured VA; a dword-aligned
// scan of all five captures for each run's base in five encodings returned ZERO hits; Apple never writes the gfx12 ring
// registers. Three sources remain and the logs cannot pick between them:
//
//     1. a LIVE REGISTER      — something holds the base right now and we have never read it back.
//     2. GE/SPI-INTERNAL      — allocation state left by Apple's own pre-arm gfx10.3 draws. Nothing we write reaches it.
//     3. OUR OWN CARVE        — the 10.5 MiB GE ring region is NEVER CLEARED. Only the 4 KiB descriptor page is zeroed
//                               (ringmap step 4b). VRAM is not zeroed at boot, so the rings hold whatever stale
//                               image-like content the card was left with, and a walker that reads a base out of them
//                               reads garbage that changes every boot — which is exactly the fingerprint.
//
// ---------------------------------------------------------------------------------------------------------------------
// THE READOUT. ONE RUN DECIDES, AND EVERY OUTCOME IS A RESULT
// ---------------------------------------------------------------------------------------------------------------------
//   the pattern 0x5A5A.... visible in the fault VA  =>  MEMORY-SOURCED, out of our own carve. THE FILL IS THE FIX.
//   a register value within 2 MiB of the fault VA   =>  REGISTER-SOURCED. That register is the next target.
//   neither                                          =>  HARDWARE-INTERNAL: the class nothing we write reaches, and the
//                                                       two cheap fixes are off the table before they cost a run.
//
// ---------------------------------------------------------------------------------------------------------------------
// THE TWO DECODINGS, AND WHY THE TOLERANCE IS 2 MiB
// ---------------------------------------------------------------------------------------------------------------------
// gfx12 stores a ring base as base>>16 in one dword (SPI_ATTRIBUTE_RING_BASE, GE_POS/PRIM_RING_BASE) and a program
// address as a 40-bit LO/HI pair, (HI & 0xff) << 40 | LO << 8. So a register V is a candidate base under EITHER
// (V << 16) or the pair decode. The walk itself spans at most 102 elements x 0x10000 = 0x660000, and the fault that
// latches is whichever request lost the race, so the latched VA is the BASE PLUS AN UNKNOWN MULTIPLE OF THE STRIDE.
// 2 MiB covers every observed walk (the widest is 102 x 2 KiB x 32 = 6.375 MiB total, but the LATCHED entry is the
// first fault, and arm5/7/8/9 latched inside the first 43 x 0x10000 = 2.6 MiB). A near-miss is therefore reported as a
// match candidate for a human to judge, never as proof: the line prints the delta in words.
//
// ---------------------------------------------------------------------------------------------------------------------
// THE REGISTER TABLE — EVERY OFFSET VERIFIED AGAINST ref/linux-asic-reg/gc_12_0_0_offset.h BY THE HOST TEST
// ---------------------------------------------------------------------------------------------------------------------
// tests/gfx_classa_test.cpp #includes the vendor header and asserts, for every entry below, that the offset equals
// reg<NAME> and that `seg` equals reg<NAME>_BASE_IDX. That test is the reason two of's own entries are corrected
// here rather than copied:
//
//   * SQ_SHADER_TBA_LO/HI (0x09e6/7) and SH_MEM_CONFIG (0x09e4) are BASE_IDX **1**, not 0. listed all three under
//     "GC base[0]". Read through base[0] they would land on completely different registers.
//   * 0x1aa4 and 0x1aa9 are NOT "RSRC4_PS/GS". They are SPI_SHADER_PGM_LO_HS and SPI_SHADER_PGM_LO_LS. The real
//     RSRC4_PS is 0x19a7 and RSRC4_GS is 0x1a28, both of which also lists and both of which are correct.
//     Because 0x1aa4/0x1aa9 are the LOW halves of program-address pairs, their HIGH halves (0x1aa5 HI_HS, 0x1aa6
//     HI_LS) are added here so the 40-bit decode can be applied to them exactly as it is to PS/GS/ES/TBA. Those two
//     are the ONLY registers in this table that did not name.
//
// No register here is written, ever. There is no write primitive in this header.

#ifndef N48_GFX_CLASSA_H
#define N48_GFX_CLASSA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// (a) THE CARVE FILL — the value written into page `page` of the GE ring carve
// ---------------------------------------------------------------------------------------------------------------------
// One constant dword per 4 KiB page, so ANY dword of the fault's page identifies the page it came from and the whole
// carve is one recognisable 16-bit-tagged space. The carve is 0xA80000 bytes = 2688 pages, so the index never reaches
// the 16-bit field's ceiling; n48_ca_fill_pages_ok() is the guard that says so and the caller refuses without it.
#define N48_CA_TAG        0x5A5A0000u
#define N48_CA_TAG_MASK   0xFFFF0000u
#define N48_CA_PAGE_MASK  0x0000FFFFu
#define N48_CA_PAGE_BYTES 4096u

static inline uint32_t n48_ca_fill_dw(uint32_t page) {
    return N48_CA_TAG | (page & N48_CA_PAGE_MASK);
}

// The caller may fill only a carve whose every page index fits the tag's low half. 0 pages is refused too: a fill that
// writes nothing must not be reported as a fill that landed.
static inline int n48_ca_fill_pages_ok(uint64_t bytes, uint32_t *pages) {
    const uint64_t n = bytes / N48_CA_PAGE_BYTES;
    if (pages) *pages = 0u;
    if (bytes == 0u || (bytes % N48_CA_PAGE_BYTES) != 0u) return 0;
    if (n == 0u || n > (uint64_t)N48_CA_PAGE_MASK + 1u) return 0;
    if (pages) *pages = (uint32_t)n;
    return 1;
}

// Does dword `v` carry the fill tag, and from which page?
static inline int n48_ca_is_fill(uint32_t v, uint32_t *page) {
    if (page) *page = 0u;
    if ((v & N48_CA_TAG_MASK) != N48_CA_TAG) return 0;
    if (page) *page = v & N48_CA_PAGE_MASK;
    return 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// (b) THE MATCH RULE
// ---------------------------------------------------------------------------------------------------------------------
#define N48_CA_NEAR_BYTES (2ull << 20)      /* 2 MiB,'s rule verbatim */

static inline uint64_t n48_ca_delta(uint64_t a, uint64_t b) { return (a > b) ? (a - b) : (b - a); }
static inline int      n48_ca_near(uint64_t a, uint64_t b) { return n48_ca_delta(a, b) < N48_CA_NEAR_BYTES; }

// A register value is NOT READ when RREG32 answers all-ones; it must never become a candidate base (0xffffffff << 16
// is 0xffff_ffff_0000, a perfectly plausible-looking address). Every match entry point refuses it.
static inline int n48_ca_readable(uint32_t v) { return v != 0xFFFFFFFFu; }

// Rule (a): the register holds base >> 16.
static inline int n48_ca_match16(uint32_t v, uint64_t va, uint64_t *cand) {
    const uint64_t c = (uint64_t)v << 16;
    if (cand) *cand = c;
    if (!n48_ca_readable(v)) return 0;
    return n48_ca_near(c, va);
}

// Rule (b): a 40-bit LO/HI program-address pair.
static inline uint64_t n48_ca_addr40(uint32_t hi, uint32_t lo) {
    return (((uint64_t)(hi & 0xFFu)) << 40) | ((uint64_t)lo << 8);
}
static inline int n48_ca_match40(uint32_t hi, uint32_t lo, uint64_t va, uint64_t *cand) {
    const uint64_t c = n48_ca_addr40(hi, lo);
    if (cand) *cand = c;
    if (!n48_ca_readable(hi) || !n48_ca_readable(lo)) return 0;
    return n48_ca_near(c, va);
}

// Is the FAULT VA itself inside the filled pattern's address space? A base read out of a filled page is some
// 0x5A5A00nn, and the hardware may use it raw, as LO<<8, or as base<<16 — three windows, decades apart, so a hit is
// never ambiguous. `pages` is the carve's page count, so the window is exactly the values the fill can produce; the
// same 2 MiB tolerance covers the walk's own offset from its base. `shift` and `page` name which window hit and which
// page's dword the base most plausibly came from.
static inline int n48_ca_va_tagged(uint64_t va, uint32_t pages, uint32_t *shift, uint32_t *page) {
    static const uint32_t sh[3] = { 0u, 8u, 16u };
    if (shift) *shift = 0u;
    if (page)  *page  = 0u;
    if (pages == 0u || pages > (uint32_t)N48_CA_PAGE_MASK + 1u) return 0;
    for (int i = 0; i < 3; i++) {
        const uint64_t lo = (uint64_t)N48_CA_TAG << sh[i];
        const uint64_t hi = ((uint64_t)N48_CA_TAG + pages - 1u) << sh[i];
        if (va + N48_CA_NEAR_BYTES < lo) continue;          /* below the window, tolerance included */
        if (va > hi + N48_CA_NEAR_BYTES) continue;          /* above it */
        if (shift) *shift = sh[i];
        if (page) {
            const uint64_t w = va >> sh[i];
            *page = (w >= (uint64_t)N48_CA_TAG && w - (uint64_t)N48_CA_TAG < (uint64_t)pages)
                        ? (uint32_t)(w - (uint64_t)N48_CA_TAG) : 0xFFFFFFFFu;   /* 0xffffffff = inside the tolerance, not the window */
        }
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// THE TABLE. `seg` is the vendor header's BASE_IDX and the kext reads through navi48_gc_base_seg(seg).
// ---------------------------------------------------------------------------------------------------------------------
struct n48_ca_reg  { const char *name; uint8_t seg; uint16_t off; };
struct n48_ca_pair { const char *name; uint8_t seg; uint16_t hi; uint16_t lo; };

// Every offset below is asserted equal to the vendor header's reg<NAME> / reg<NAME>_BASE_IDX by the host test.
static inline const struct n48_ca_reg *n48_ca_reg_table(uint32_t *n) {
    static const struct n48_ca_reg t[] = {
        { "SPI_ATTRIBUTE_RING_BASE", 1, 0x2446 },
        { "SPI_ATTRIBUTE_RING_SIZE", 1, 0x2447 },
        { "GE_POS_RING_BASE",        1, 0x2268 },
        { "GE_POS_RING_SIZE",        1, 0x2269 },
        { "GE_PRIM_RING_BASE",       1, 0x226a },
        { "GE_PRIM_RING_SIZE",       1, 0x226b },
        { "SPI_SHADER_PGM_LO_PS",    0, 0x19a8 },
        { "SPI_SHADER_PGM_HI_PS",    0, 0x19a9 },
        { "SPI_SHADER_PGM_RSRC4_PS", 0, 0x19a7 },
        { "SPI_SHADER_PGM_LO_GS",    0, 0x1a24 },
        { "SPI_SHADER_PGM_HI_GS",    0, 0x1a25 },
        { "SPI_SHADER_PGM_HI_ES",    0, 0x1a26 },
        { "SPI_SHADER_PGM_LO_ES",    0, 0x1a29 },
        { "SPI_SHADER_PGM_RSRC4_GS", 0, 0x1a28 },
        { "SPI_SHADER_PGM_LO_HS",    0, 0x1aa4 },
        { "SPI_SHADER_PGM_HI_HS",    0, 0x1aa5 },
        { "SPI_SHADER_PGM_HI_LS",    0, 0x1aa6 },
        { "SPI_SHADER_PGM_LO_LS",    0, 0x1aa9 },
        { "SQG_CONFIG",              0, 0x10ba },
        { "SH_MEM_CONFIG",           1, 0x09e4 },
        { "SQ_SHADER_TBA_LO",        1, 0x09e6 },
        { "SQ_SHADER_TBA_HI",        1, 0x09e7 },
        // 0.0.387 — THE 23rd DWORD.'s H1 is that GRBM_GFX_INDEX was not broadcasting when
        // our SET_UCONFIG_REG ring writes landed, so they reached one shader engine and the others kept power-on
        // values. The dump is the one place that reads the machine at the instant class A latches, so the index
        // belongs in it: H1's falsifier is literally "the index reads 0xE0000000 in the dump". It is appended, never
        // inserted, so every earlier row keeps its position and the six LO/HI pairs still index the same values.
        // It cannot manufacture a match: rule (a) sends 0xE0000000 to 0xe00000000000, decades above any fault VA,
        // and it is in no pair. regGRBM_GFX_INDEX 0x2200 / BASE_IDX 1, asserted against the vendor header like the
        // rest of this table.
        { "GRBM_GFX_INDEX",          1, 0x2200 },
        // 0.0.389 (notes 880 H6 -> 881, arm16) — THE SC-SIDE ROWS.'s H6 is that class A's reader is the SC
        // walking a gfx12-ONLY HiZ / HiS / VRS surface whose base and enable live in CONTEXT registers nobody on the
        // armed path has ever written. Every one of the 33 rows below is a register of that hypothesis or of the
        // state that sizes it, and all are APPENDED so every earlier row keeps its index and the six original
        // LO/HI pairs still index the same values.
        //
        // THE SIXTEEN BASE HALVES ARE HERE BECAUSE THE PAIR DECODE NEEDS THEM. n48_ca_reg_index() searches THIS
        // table, and ca_fault_dump's pair loop does `if (ih >= n || il >= n) continue;` - a pair whose halves are
        // not rows is SILENTLY SKIPPED. Listing the eight new pairs without their sixteen halves would have made
        // the whole addition vacuous.
        //
        // CAVEAT, and it bounds what a hit means: which context copy an MMIO read of a CONTEXT register returns is
        // unknown (the CP reloads context state per submission). A MATCH is conclusive; a MISS is not.
        //
        // Every offset and `seg` below was re-derived from ref/linux-asic-reg/gc_12_0_0_offset.h for this build and
        // is re-asserted against it by tests/gfx_classa_test.cpp.
        { "PA_SC_HIZ_INFO",                  1, 0x02e5 },   // SURFACE_ENABLE bit 0 - H6's enable
        { "PA_SC_HIS_INFO",                  1, 0x02e6 },   // SURFACE_ENABLE bit 0
        { "PA_SC_HIZ_BASE",                  1, 0x02e7 },   // BASE_256B, VA bits 39:8   (pair HIZ, lo)
        { "PA_SC_HIZ_BASE_EXT",              1, 0x02e8 },   // BASE_256B[7:0], VA 47:40  (pair HIZ, hi)
        { "PA_SC_HIZ_SIZE_XY",               1, 0x02e9 },
        { "PA_SC_HIS_BASE",                  1, 0x02ea },   // (pair HIS, lo)
        { "PA_SC_HIS_BASE_EXT",              1, 0x02eb },   // (pair HIS, hi)
        { "PA_SC_HIS_SIZE_XY",               1, 0x02ec },
        { "PA_SC_HISZ_CONTROL",              1, 0x02ef },
        { "PA_SC_HISZ_RENDER_OVERRIDE",      1, 0x02f0 },   // the SC-side override the raster block zeroes
        { "PA_SC_VRS_OVERRIDE_CNTL",         1, 0x00f4 },
        { "PA_SC_VRS_RATE_FEEDBACK_BASE",    1, 0x00f5 },   // (pair VRS_FB, lo)
        { "PA_SC_VRS_RATE_FEEDBACK_BASE_EXT",1, 0x00f6 },   // (pair VRS_FB, hi)
        { "PA_SC_VRS_RATE_FEEDBACK_SIZE_XY", 1, 0x00f7 },
        { "PA_SC_VRS_INFO",                  1, 0x00f8 },
        { "PA_SC_VRS_RATE_BASE",             1, 0x00fc },   // (pair VRS_RATE, lo)
        { "PA_SC_VRS_RATE_BASE_EXT",         1, 0x00fd },   // (pair VRS_RATE, hi)
        { "PA_SC_VRS_RATE_SIZE_XY",          1, 0x00fe },
        { "DB_RENDER_OVERRIDE",              1, 0x0003 },   // Apple's 0x0001002a IS repacked and emitted
        { "DB_Z_INFO",                       1, 0x0006 },
        { "DB_DEPTH_CONTROL",                1, 0x001c },
        { "SPI_TMPRING_SIZE",                1, 0x01ba },
        { "SPI_GFX_SCRATCH_BASE_LO",         1, 0x01bb },   // (pair GFX_SCRATCH, lo)
        { "SPI_GFX_SCRATCH_BASE_HI",         1, 0x01bc },   // (pair GFX_SCRATCH, hi)
        { "SQ_SHADER_TMA_LO",                1, 0x09e8 },   // (pair TMA, lo) - TBA_LO/HI are already rows above
        { "SQ_SHADER_TMA_HI",                1, 0x09e9 },   // (pair TMA, hi)
        { "VGT_TF_RING_SIZE",                1, 0x224e },
        { "VGT_TF_MEMORY_BASE",              1, 0x2250 },   // (pair TF, lo)
        { "VGT_TF_MEMORY_BASE_HI",           1, 0x2267 },   // (pair TF, hi)
        { "SQ_CONFIG",                       0, 0x10a0 },
        { "COMPUTE_DISPATCH_SCRATCH_BASE_LO",0, 0x1bb0 },   // (pair CS_SCRATCH, lo)
        { "COMPUTE_DISPATCH_SCRATCH_BASE_HI",0, 0x1bb1 },   // (pair CS_SCRATCH, hi)
        { "COMPUTE_TMPRING_SIZE",            0, 0x1bb8 },
    };
    if (n) *n = (uint32_t)(sizeof(t) / sizeof(t[0]));
    return t;
}

static inline const struct n48_ca_pair *n48_ca_pair_table(uint32_t *n) {
    static const struct n48_ca_pair t[] = {
        { "PS",  0, 0x19a9, 0x19a8 },
        { "GS",  0, 0x1a25, 0x1a24 },
        { "ES",  0, 0x1a26, 0x1a29 },
        { "HS",  0, 0x1aa5, 0x1aa4 },
        { "LS",  0, 0x1aa6, 0x1aa9 },
        { "TBA", 1, 0x09e7, 0x09e6 },
        // 0.0.389 (notes 880 H6 (b), 881) — THE EIGHT SC/RING BASE PAIRS. gfx12 stores each of these as a 48-bit
        // two-field value: <NAME>_BASE.BASE_256B holds VA bits 39:8 and <NAME>_BASE_EXT.BASE_256B[7:0] holds bits
        // 47:40. That composition IS rule (b) - n48_ca_addr40(hi, lo) = ((hi & 0xff) << 40) | (lo << 8) - and the
        // host test proves the two expressions equal rather than asserting it here. This is the shape
        // exactly: a LO word bit-stable across five boots with an EXT byte that flips (0x27 / 0xa7 / 0xaf).
        { "HIZ",         1, 0x02e8, 0x02e7 },
        { "HIS",         1, 0x02eb, 0x02ea },
        { "VRS_RATE",    1, 0x00fd, 0x00fc },
        { "VRS_FB",      1, 0x00f6, 0x00f5 },
        { "TF",          1, 0x2267, 0x2250 },
        { "GFX_SCRATCH", 1, 0x01bc, 0x01bb },
        { "CS_SCRATCH",  0, 0x1bb1, 0x1bb0 },
        { "TMA",         1, 0x09e9, 0x09e8 },
    };
    if (n) *n = (uint32_t)(sizeof(t) / sizeof(t[0]));
    return t;
}

// The index of `off` in the register table, or 0xffffffff. The kext reads every register ONCE into an array and the
// pair decode then indexes that array rather than issuing a second MMIO read of the same dword.
static inline uint32_t n48_ca_reg_index(uint16_t off, uint8_t seg) {
    uint32_t n = 0;
    const struct n48_ca_reg *t = n48_ca_reg_table(&n);
    for (uint32_t i = 0; i < n; i++)
        if (t[i].off == off && t[i].seg == seg) return i;
    return 0xFFFFFFFFu;
}

// The verdict, as the readout states it. `tagged` = the fault VA carried the fill pattern; `matches` = how many
// register candidates landed within 2 MiB.
#define N48_CA_V_MEMORY   1u   /* the pattern is in the VA: our carve is the source, and the fill IS the fix */
#define N48_CA_V_REGISTER 2u   /* a register candidate matched: that register is the next target */
#define N48_CA_V_INTERNAL 3u   /* neither: hardware-internal, nothing we write reaches it */
static inline uint32_t n48_ca_verdict(int tagged, uint32_t matches) {
    if (tagged)  return N48_CA_V_MEMORY;     /* memory wins: the pattern is a value only we write */
    if (matches) return N48_CA_V_REGISTER;
    return N48_CA_V_INTERNAL;
}
static inline const char *n48_ca_verdict_name(uint32_t v) {
    return v == N48_CA_V_MEMORY   ? "MEMORY-SOURCED (the fill pattern is in the fault VA: the base came out of OUR carve, and pre-arm filling IS the fix)"
         : v == N48_CA_V_REGISTER ? "REGISTER-SOURCED (a live register is within 2 MiB of the fault VA: that register is the next target)"
         : "HARDWARE-INTERNAL (neither the pattern nor any register we can read: GE/SPI-internal state, the class nothing we write reaches)";
}

#ifdef __cplusplus
}
#endif

#endif /* N48_GFX_CLASSA_H */
