// gfx_classa_test.cpp — the class-A instrument's arithmetic and its register table, offline.
//
// THE STRONGEST CHECK IN THIS FILE IS NOT A UNIT TEST. Section 1 #includes the VENDOR HEADER
// (ref/linux-asic-reg/gc_12_0_0_offset.h) and asserts, for every entry of n48_ca_reg_table() and
// n48_ca_pair_table(), that the offset equals reg<NAME> AND that `seg` equals reg<NAME>_BASE_IDX. A register read
// through the wrong segment base lands on a completely different register and would be logged as a real value, so
// this is the check that decides whether the instrument reports the truth. It already caught two errors in the brief
// this was built from: SQ_SHADER_TBA_LO/HI and SH_MEM_CONFIG are BASE_IDX 1, not 0, and 0x1aa4/0x1aa9 are
// SPI_SHADER_PGM_LO_HS / LO_LS, not "RSRC4_PS/GS".
//
// PLANTED-DEFECT CONTROL (rule: a test no mutation can break is not testing anything). ELEVEN mutants are run
// against the SAME checks and each must be caught by at least one of them.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_classa_test.cpp -o /tmp/catest && /tmp/catest
#include <cstdio>
#include <cstdint>
#include "gfx_classa.h"
#include "../ref/linux-asic-reg/gc_12_0_0_offset.h"

static int gFail = 0, gRun = 0, gQuiet = 0;

static void ck(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) printf("  FAIL %-66s got %#llx want %#llx\n", what, (unsigned long long)got,
                            (unsigned long long)want);
    } else if (!gQuiet) printf("  ok   %-66s %#llx\n", what, (unsigned long long)got);
}

// ---------------------------------------------------------------------------------------------------------------------
// 1. THE TABLE vs THE VENDOR HEADER. Not mutated: this is a fact about gc_12_0_0_offset.h, not about our arithmetic.
// ---------------------------------------------------------------------------------------------------------------------
struct Vendor { const char *name; uint16_t off; uint8_t seg; };
static const Vendor kVendor[] = {
    { "SPI_ATTRIBUTE_RING_BASE", regSPI_ATTRIBUTE_RING_BASE, regSPI_ATTRIBUTE_RING_BASE_BASE_IDX },
    { "SPI_ATTRIBUTE_RING_SIZE", regSPI_ATTRIBUTE_RING_SIZE, regSPI_ATTRIBUTE_RING_SIZE_BASE_IDX },
    { "GE_POS_RING_BASE",        regGE_POS_RING_BASE,        regGE_POS_RING_BASE_BASE_IDX },
    { "GE_POS_RING_SIZE",        regGE_POS_RING_SIZE,        regGE_POS_RING_SIZE_BASE_IDX },
    { "GE_PRIM_RING_BASE",       regGE_PRIM_RING_BASE,       regGE_PRIM_RING_BASE_BASE_IDX },
    { "GE_PRIM_RING_SIZE",       regGE_PRIM_RING_SIZE,       regGE_PRIM_RING_SIZE_BASE_IDX },
    { "SPI_SHADER_PGM_LO_PS",    regSPI_SHADER_PGM_LO_PS,    regSPI_SHADER_PGM_LO_PS_BASE_IDX },
    { "SPI_SHADER_PGM_HI_PS",    regSPI_SHADER_PGM_HI_PS,    regSPI_SHADER_PGM_HI_PS_BASE_IDX },
    { "SPI_SHADER_PGM_RSRC4_PS", regSPI_SHADER_PGM_RSRC4_PS, regSPI_SHADER_PGM_RSRC4_PS_BASE_IDX },
    { "SPI_SHADER_PGM_LO_GS",    regSPI_SHADER_PGM_LO_GS,    regSPI_SHADER_PGM_LO_GS_BASE_IDX },
    { "SPI_SHADER_PGM_HI_GS",    regSPI_SHADER_PGM_HI_GS,    regSPI_SHADER_PGM_HI_GS_BASE_IDX },
    { "SPI_SHADER_PGM_HI_ES",    regSPI_SHADER_PGM_HI_ES,    regSPI_SHADER_PGM_HI_ES_BASE_IDX },
    { "SPI_SHADER_PGM_LO_ES",    regSPI_SHADER_PGM_LO_ES,    regSPI_SHADER_PGM_LO_ES_BASE_IDX },
    { "SPI_SHADER_PGM_RSRC4_GS", regSPI_SHADER_PGM_RSRC4_GS, regSPI_SHADER_PGM_RSRC4_GS_BASE_IDX },
    { "SPI_SHADER_PGM_LO_HS",    regSPI_SHADER_PGM_LO_HS,    regSPI_SHADER_PGM_LO_HS_BASE_IDX },
    { "SPI_SHADER_PGM_HI_HS",    regSPI_SHADER_PGM_HI_HS,    regSPI_SHADER_PGM_HI_HS_BASE_IDX },
    { "SPI_SHADER_PGM_HI_LS",    regSPI_SHADER_PGM_HI_LS,    regSPI_SHADER_PGM_HI_LS_BASE_IDX },
    { "SPI_SHADER_PGM_LO_LS",    regSPI_SHADER_PGM_LO_LS,    regSPI_SHADER_PGM_LO_LS_BASE_IDX },
    { "SQG_CONFIG",              regSQG_CONFIG,              regSQG_CONFIG_BASE_IDX },
    { "SH_MEM_CONFIG",           regSH_MEM_CONFIG,           regSH_MEM_CONFIG_BASE_IDX },
    { "SQ_SHADER_TBA_LO",        regSQ_SHADER_TBA_LO,        regSQ_SHADER_TBA_LO_BASE_IDX },
    { "SQ_SHADER_TBA_HI",        regSQ_SHADER_TBA_HI,        regSQ_SHADER_TBA_HI_BASE_IDX },
    // 0.0.387: the 23rd dword, appended for arm14's H1 falsifier. Its position at the END is asserted
    // below, because inserting it anywhere else would move every later row and silently re-key the pair decode.
    { "GRBM_GFX_INDEX",          regGRBM_GFX_INDEX,          regGRBM_GFX_INDEX_BASE_IDX },
    // 0.0.389 (notes 880 H6 -> 881, arm16): the SC-side rows, in the table's own order. Sixteen of them are the
    // halves of the eight new base PAIRS and are here because n48_ca_reg_index() searches the register table - a
    // pair whose halves are not rows is silently skipped by ca_fault_dump's `if (ih >= n || il >= n) continue;`.
    { "PA_SC_HIZ_INFO",                  regPA_SC_HIZ_INFO,                  regPA_SC_HIZ_INFO_BASE_IDX },
    { "PA_SC_HIS_INFO",                  regPA_SC_HIS_INFO,                  regPA_SC_HIS_INFO_BASE_IDX },
    { "PA_SC_HIZ_BASE",                  regPA_SC_HIZ_BASE,                  regPA_SC_HIZ_BASE_BASE_IDX },
    { "PA_SC_HIZ_BASE_EXT",              regPA_SC_HIZ_BASE_EXT,              regPA_SC_HIZ_BASE_EXT_BASE_IDX },
    { "PA_SC_HIZ_SIZE_XY",               regPA_SC_HIZ_SIZE_XY,               regPA_SC_HIZ_SIZE_XY_BASE_IDX },
    { "PA_SC_HIS_BASE",                  regPA_SC_HIS_BASE,                  regPA_SC_HIS_BASE_BASE_IDX },
    { "PA_SC_HIS_BASE_EXT",              regPA_SC_HIS_BASE_EXT,              regPA_SC_HIS_BASE_EXT_BASE_IDX },
    { "PA_SC_HIS_SIZE_XY",               regPA_SC_HIS_SIZE_XY,               regPA_SC_HIS_SIZE_XY_BASE_IDX },
    { "PA_SC_HISZ_CONTROL",              regPA_SC_HISZ_CONTROL,              regPA_SC_HISZ_CONTROL_BASE_IDX },
    { "PA_SC_HISZ_RENDER_OVERRIDE",      regPA_SC_HISZ_RENDER_OVERRIDE,      regPA_SC_HISZ_RENDER_OVERRIDE_BASE_IDX },
    { "PA_SC_VRS_OVERRIDE_CNTL",         regPA_SC_VRS_OVERRIDE_CNTL,         regPA_SC_VRS_OVERRIDE_CNTL_BASE_IDX },
    { "PA_SC_VRS_RATE_FEEDBACK_BASE",    regPA_SC_VRS_RATE_FEEDBACK_BASE,    regPA_SC_VRS_RATE_FEEDBACK_BASE_BASE_IDX },
    { "PA_SC_VRS_RATE_FEEDBACK_BASE_EXT",regPA_SC_VRS_RATE_FEEDBACK_BASE_EXT,regPA_SC_VRS_RATE_FEEDBACK_BASE_EXT_BASE_IDX },
    { "PA_SC_VRS_RATE_FEEDBACK_SIZE_XY", regPA_SC_VRS_RATE_FEEDBACK_SIZE_XY, regPA_SC_VRS_RATE_FEEDBACK_SIZE_XY_BASE_IDX },
    { "PA_SC_VRS_INFO",                  regPA_SC_VRS_INFO,                  regPA_SC_VRS_INFO_BASE_IDX },
    { "PA_SC_VRS_RATE_BASE",             regPA_SC_VRS_RATE_BASE,             regPA_SC_VRS_RATE_BASE_BASE_IDX },
    { "PA_SC_VRS_RATE_BASE_EXT",         regPA_SC_VRS_RATE_BASE_EXT,         regPA_SC_VRS_RATE_BASE_EXT_BASE_IDX },
    { "PA_SC_VRS_RATE_SIZE_XY",          regPA_SC_VRS_RATE_SIZE_XY,          regPA_SC_VRS_RATE_SIZE_XY_BASE_IDX },
    { "DB_RENDER_OVERRIDE",              regDB_RENDER_OVERRIDE,              regDB_RENDER_OVERRIDE_BASE_IDX },
    { "DB_Z_INFO",                       regDB_Z_INFO,                       regDB_Z_INFO_BASE_IDX },
    { "DB_DEPTH_CONTROL",                regDB_DEPTH_CONTROL,                regDB_DEPTH_CONTROL_BASE_IDX },
    { "SPI_TMPRING_SIZE",                regSPI_TMPRING_SIZE,                regSPI_TMPRING_SIZE_BASE_IDX },
    { "SPI_GFX_SCRATCH_BASE_LO",         regSPI_GFX_SCRATCH_BASE_LO,         regSPI_GFX_SCRATCH_BASE_LO_BASE_IDX },
    { "SPI_GFX_SCRATCH_BASE_HI",         regSPI_GFX_SCRATCH_BASE_HI,         regSPI_GFX_SCRATCH_BASE_HI_BASE_IDX },
    { "SQ_SHADER_TMA_LO",                regSQ_SHADER_TMA_LO,                regSQ_SHADER_TMA_LO_BASE_IDX },
    { "SQ_SHADER_TMA_HI",                regSQ_SHADER_TMA_HI,                regSQ_SHADER_TMA_HI_BASE_IDX },
    { "VGT_TF_RING_SIZE",                regVGT_TF_RING_SIZE,                regVGT_TF_RING_SIZE_BASE_IDX },
    { "VGT_TF_MEMORY_BASE",              regVGT_TF_MEMORY_BASE,              regVGT_TF_MEMORY_BASE_BASE_IDX },
    { "VGT_TF_MEMORY_BASE_HI",           regVGT_TF_MEMORY_BASE_HI,           regVGT_TF_MEMORY_BASE_HI_BASE_IDX },
    { "SQ_CONFIG",                       regSQ_CONFIG,                       regSQ_CONFIG_BASE_IDX },
    { "COMPUTE_DISPATCH_SCRATCH_BASE_LO",regCOMPUTE_DISPATCH_SCRATCH_BASE_LO,regCOMPUTE_DISPATCH_SCRATCH_BASE_LO_BASE_IDX },
    { "COMPUTE_DISPATCH_SCRATCH_BASE_HI",regCOMPUTE_DISPATCH_SCRATCH_BASE_HI,regCOMPUTE_DISPATCH_SCRATCH_BASE_HI_BASE_IDX },
    { "COMPUTE_TMPRING_SIZE",            regCOMPUTE_TMPRING_SIZE,            regCOMPUTE_TMPRING_SIZE_BASE_IDX },
};

static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void table_checks()
{
    uint32_t n = 0;
    const n48_ca_reg *t = n48_ca_reg_table(&n);
    const uint32_t nv = (uint32_t)(sizeof(kVendor) / sizeof(kVendor[0]));
    ck("table has exactly as many entries as the vendor list", n, nv);
    for (uint32_t i = 0; i < n && i < nv; i++) {
        char w[128];
        snprintf(w, sizeof(w), "%s offset == gc_12_0_0_offset.h", t[i].name);
        ck(w, t[i].off, kVendor[i].off);
        snprintf(w, sizeof(w), "%s BASE_IDX == gc_12_0_0_offset.h", t[i].name);
        ck(w, t[i].seg, kVendor[i].seg);
        snprintf(w, sizeof(w), "%s name matches the vendor name", t[i].name);
        ck(w, (uint64_t)streq(t[i].name, kVendor[i].name), 1u);
    }
    // no duplicate (seg, off): a duplicate would MMIO-read the same dword twice and print it twice
    uint32_t dup = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i + 1; j < n; j++)
            if (t[i].off == t[j].off && t[i].seg == t[j].seg) dup++;
    ck("no duplicate (seg, offset) in the table", dup, 0u);
    // the kext indexes a two-entry base array with `seg`, so a seg above 1 would silently read the wrong aperture
    uint32_t badSeg = 0;
    for (uint32_t i = 0; i < n; i++) if (t[i].seg > 1u) badSeg++;
    ck("every seg is 0 or 1 (the kext's base array has two entries)", badSeg, 0u);
    // every pair member must BE in the single table, so the pair decode indexes values already read
    uint32_t np = 0;
    const n48_ca_pair *p = n48_ca_pair_table(&np);
    ck("fourteen LO/HI pairs (6 program + 8 SC/ring bases, 0.0.389)", np, 14u);
    for (uint32_t i = 0; i < np; i++) {
        char w[128];
        snprintf(w, sizeof(w), "pair %s HI is in the register table", p[i].name);
        ck(w, (uint64_t)(n48_ca_reg_index(p[i].hi, p[i].seg) != 0xFFFFFFFFu), 1u);
        snprintf(w, sizeof(w), "pair %s LO is in the register table", p[i].name);
        ck(w, (uint64_t)(n48_ca_reg_index(p[i].lo, p[i].seg) != 0xFFFFFFFFu), 1u);
    }
    ck("a (seg, off) not in the table indexes 0xffffffff", n48_ca_reg_index(0x2446, 0), 0xFFFFFFFFu);
    // 0.0.387: GRBM_GFX_INDEX is the 23rd dword and it is the LAST. An insertion anywhere earlier would
    // shift every later row's index; the kext's dump prints four per line and the pair decode indexes the value array
    // by (off, seg), so nothing would crash - the run's 22 previously-comparable columns would simply move, and the
    // two arm13 dumps could no longer be read side by side. Pinned as a fact, not left to review.
    ck("the table has 56 entries (0.0.389: 23 + 33 SC-side rows)", n, 56u);
    ck("GRBM_GFX_INDEX is still the 23rd (index 22) entry", n48_ca_reg_index(regGRBM_GFX_INDEX, regGRBM_GFX_INDEX_BASE_IDX), 22u);
    // 0.0.389 (notes 881): THE PAIR RULE IS THE VENDOR'S OWN BASE_256B / _EXT COMPOSITION, proved rather than
    // asserted. gfx12 keeps a 48-bit SC surface base as <NAME>_BASE.BASE_256B = VA[39:8] and
    // <NAME>_BASE_EXT.BASE_256B[7:0] = VA[47:40]; rule (b) is ((hi & 0xff) << 40) | (lo << 8). Rebuilt here from a
    // VA and compared, over VAs that exercise every byte of both halves - including arm15's own fault VA.
    {
        static const uint64_t kVa[] = { 0x270059a80000ull, 0xaf0059a84000ull, 0x0000000000100ull,
                                        0xffffffffffff00ull & 0xFFFFFFFFFFFFull, 0x123456789ab00ull, 0ull };
        uint32_t bad = 0;
        for (uint32_t i = 0; i < sizeof(kVa) / sizeof(kVa[0]); i++) {
            const uint64_t va = kVa[i] & 0xFFFFFFFFFF00ull;      // a 256 B-aligned 48-bit VA, as the field pair holds
            const uint32_t lo = (uint32_t)((va >> 8) & 0xFFFFFFFFu);   // BASE_256B: VA bits 39:8
            const uint32_t hi = (uint32_t)((va >> 40) & 0xFFu);        // _EXT BASE_256B[7:0]: VA bits 47:40
            if (n48_ca_addr40(hi, lo) != va) bad++;
        }
        ck("rule (b) == the vendor BASE_256B / _EXT composition over 6 VAs", bad, 0u);
        // and the other direction: a register pair holding arm15's own reported walk base composes to it
        ck("arm15's fault VA 0x270059a80000 composes from (hi 0x27, lo 0x59a800)",
           n48_ca_addr40(0x27u, 0x0059a800u), 0x270059a80000ull);
        ck("arm14's fault VA 0xaf0059a84000 composes from (hi 0xaf, lo 0x59a840)",
           n48_ca_addr40(0xafu, 0x0059a840u), 0xaf0059a84000ull);
        // the _EXT half is ONE byte: bits above 7 must not leak into the address
        ck("rule (b) masks the EXT half to 8 bits", n48_ca_addr40(0xFFFFFF27u, 0x0059a800u), 0x270059a80000ull);
    }
    {
        uint32_t np2 = 0; const n48_ca_pair *p2 = n48_ca_pair_table(&np2); uint32_t inPair = 0;
        for (uint32_t i = 0; i < np2; i++)
            if ((p2[i].hi == regGRBM_GFX_INDEX || p2[i].lo == regGRBM_GFX_INDEX) && p2[i].seg == regGRBM_GFX_INDEX_BASE_IDX) inPair++;
        ck("no pair names GRBM_GFX_INDEX", inPair, 0u);
    }
    // The broadcast value can never be read as a candidate base: rule (a) sends it decades above any fault VA.
    {
        uint64_t c = 0;
        ck("rule (a) on the broadcast index 0xE0000000 is not near arm13's fault VA",
           (uint64_t)n48_ca_match16(0xE0000000u, 0xaf0059a80000ull, &c), 0u);
        ck("...and it decodes to 0xe00000000000", c, 0xe00000000000ull);
    }
    //'s own two errors, asserted as facts so a future edit cannot quietly reintroduce them
    ck("SQ_SHADER_TBA_LO is BASE_IDX 1, NOT 0 (the brief said 0)", regSQ_SHADER_TBA_LO_BASE_IDX, 1u);
    ck("SH_MEM_CONFIG is BASE_IDX 1, NOT 0 (the brief said 0)",    regSH_MEM_CONFIG_BASE_IDX, 1u);
    ck("0x1aa4 is LO_HS, not RSRC4_PS (which is 0x19a7)",          regSPI_SHADER_PGM_LO_HS, 0x1aa4u);
    ck("0x1aa9 is LO_LS, not RSRC4_GS (which is 0x1a28)",          regSPI_SHADER_PGM_LO_LS, 0x1aa9u);
}

// ---------------------------------------------------------------------------------------------------------------------
// 2. THE ARITHMETIC, through function pointers so a mutant can replace exactly one of them.
// ---------------------------------------------------------------------------------------------------------------------
typedef uint32_t (*FillFn)(uint32_t page);
typedef int      (*PagesFn)(uint64_t bytes, uint32_t *pages);
typedef int      (*M16Fn)(uint32_t v, uint64_t va, uint64_t *cand);
typedef int      (*M40Fn)(uint32_t hi, uint32_t lo, uint64_t va, uint64_t *cand);
typedef int      (*VaFn)(uint64_t va, uint32_t pages, uint32_t *shift, uint32_t *page);

static uint32_t f_real(uint32_t p)                                  { return n48_ca_fill_dw(p); }
static int      g_real(uint64_t b, uint32_t *n)                     { return n48_ca_fill_pages_ok(b, n); }
static int      m16_real(uint32_t v, uint64_t va, uint64_t *c)      { return n48_ca_match16(v, va, c); }
static int      m40_real(uint32_t h, uint32_t l, uint64_t va, uint64_t *c) { return n48_ca_match40(h, l, va, c); }
static int      va_real(uint64_t va, uint32_t n, uint32_t *s, uint32_t *p) { return n48_ca_va_tagged(va, n, s, p); }

// The carve, exactly: XLAT12_GE_RING_TOTAL = 0x00A80000 = 2688 pages of 4 KiB.
static const uint64_t kCarve = 0x00A80000ull;
static const uint32_t kPages = 2688u;
static const uint64_t kMiB2  = (2ull << 20);

static void checks(FillFn fdw, PagesFn pgok, M16Fn m16, M40Fn m40, VaFn vat)
{
    // ---- (a) the fill dword
    ck("page 0 fills 0x5a5a0000",        fdw(0u), 0x5A5A0000u);
    ck("page 1 fills 0x5a5a0001",        fdw(1u), 0x5A5A0001u);
    ck("page 2687 (the last) fills 0x5a5a0a7f", fdw(kPages - 1u), 0x5A5A0A7Fu);
    ck("every page's dword is distinct", (uint64_t)(fdw(0u) != fdw(1u) && fdw(1u) != fdw(kPages - 1u)), 1u);
    {
        uint32_t pg = 0xdeadu;
        ck("the last page's dword decodes back to 2687", (uint64_t)(n48_ca_is_fill(fdw(kPages - 1u), &pg) && pg == kPages - 1u), 1u);
        ck("a stale image dword is NOT read as a fill",  (uint64_t)n48_ca_is_fill(0x1c1c1d1du, &pg), 0u);
        ck("0xffffffff is NOT read as a fill",           (uint64_t)n48_ca_is_fill(0xFFFFFFFFu, &pg), 0u);
        ck("0x5a5a0000 exactly IS a fill, page 0",       (uint64_t)(n48_ca_is_fill(0x5A5A0000u, &pg) && pg == 0u), 1u);
    }
    // ---- (a) the page-count guard
    {
        uint32_t n = 0;
        ck("the carve's page count is 2688",     (uint64_t)(pgok(kCarve, &n) && n == kPages), 1u);
        ck("a 0-byte carve is REFUSED",          (uint64_t)pgok(0ull, &n), 0u);
        ck("a non-page-multiple carve REFUSED",  (uint64_t)pgok(kCarve + 1ull, &n), 0u);
        ck("exactly 65536 pages is accepted",    (uint64_t)pgok(65536ull * 4096ull, &n), 1u);
        ck("65537 pages is REFUSED (the index would wrap the tag)", (uint64_t)pgok(65537ull * 4096ull, &n), 0u);
    }
    // ---- (b) rule (a): V << 16
    {
        uint64_t c = 0;
        const uint64_t va = 0x400800000ull;
        ck("V = va>>16 matches exactly",              (uint64_t)m16((uint32_t)(va >> 16), va, &c), 1u);
        ck("  and the candidate is the VA itself",    c, va);
        ck("V one page below still matches",          (uint64_t)m16((uint32_t)((va - 0x10000ull) >> 16), va, &c), 1u);
        ck("V exactly 2 MiB below does NOT match",    (uint64_t)m16((uint32_t)((va - kMiB2) >> 16), va, &c), 0u);
        ck("V 2 MiB - 64 KiB below matches",          (uint64_t)m16((uint32_t)((va - kMiB2 + 0x10000ull) >> 16), va, &c), 1u);
        ck("V exactly 2 MiB above does NOT match",    (uint64_t)m16((uint32_t)((va + kMiB2) >> 16), va, &c), 0u);
        ck("a far register does NOT match",           (uint64_t)m16(0x0000C000u, va, &c), 0u);
        ck("0xffffffff (NOT READ) never matches",     (uint64_t)m16(0xFFFFFFFFu, 0xFFFFFFFF0000ull, &c), 0u);
        ck("a zero register only matches a VA near 0", (uint64_t)m16(0u, va, &c), 0u);
    }
    // ---- (b) rule (b): the 40-bit LO/HI pair
    {
        uint64_t c = 0;
        const uint64_t pgm = 0x4000c0000ull;                 /* arm9's captured program space */
        const uint32_t lo = (uint32_t)(pgm >> 8), hi = (uint32_t)(pgm >> 40);
        ck("the pair decodes to the program VA",      n48_ca_addr40(hi, lo), pgm);
        ck("the pair matches its own VA",             (uint64_t)m40(hi, lo, pgm, &c), 1u);
        ck("HI's upper 24 bits are ignored (value)",  n48_ca_addr40(hi | 0xFFFFFF00u, lo), pgm);
        ck("HI's upper 24 bits are ignored (match)",  (uint64_t)m40(hi | 0xFFFFFF00u, lo, pgm, &c), 1u);
        ck("the pair does NOT match 2 MiB away",      (uint64_t)m40(hi, lo, pgm + kMiB2, &c), 0u);
        ck("the pair matches 2 MiB - 256 away",       (uint64_t)m40(hi, lo, pgm + kMiB2 - 256ull, &c), 1u);
        ck("an unreadable LO never matches",          (uint64_t)m40(hi, 0xFFFFFFFFu, n48_ca_addr40(hi, 0xFFFFFFFFu), &c), 0u);
        ck("an unreadable HI never matches",          (uint64_t)m40(0xFFFFFFFFu, lo, n48_ca_addr40(0xFFu, lo), &c), 0u);
    }
    // ---- the fault VA carrying the pattern, at each of the three shifts
    {
        uint32_t sh = 0, pg = 0;
        const uint32_t base = 0x5A5A0123u;               /* a base read out of page 0x123 of the carve */
        ck("pattern used as base<<16 is seen",   (uint64_t)vat((uint64_t)base << 16, kPages, &sh, &pg), 1u);
        ck("  shift reported is 16",             sh, 16u);
        ck("  page reported is 0x123",           pg, 0x123u);
        ck("pattern used as LO<<8 is seen",      (uint64_t)vat((uint64_t)base << 8, kPages, &sh, &pg), 1u);
        ck("  shift reported is 8",              sh, 8u);
        ck("pattern used raw is seen",           (uint64_t)vat((uint64_t)base, kPages, &sh, &pg), 1u);
        ck("  shift reported is 0",              sh, 0u);
        // the walk's own offset from the base: arm5/9's 43 elements x 0xE000 stride
        ck("base<<16 + 42*0xe000 still seen",    (uint64_t)vat(((uint64_t)base << 16) + 42ull * 0xE000ull, kPages, &sh, &pg), 1u);
        // and the negatives, which are the whole point
        ck("arm9's real fault VA is NOT tagged", (uint64_t)vat(0x400800000ull, kPages, &sh, &pg), 0u);
        ck("a program VA is NOT tagged",         (uint64_t)vat(0x4000c0070ull, kPages, &sh, &pg), 0u);
        ck("VA 0 is NOT tagged",                 (uint64_t)vat(0ull, kPages, &sh, &pg), 0u);
        ck("a page BEYOND the carve is NOT tagged (0x5a5a2000<<16)",
                                                 (uint64_t)vat((uint64_t)0x5A5A2000u << 16, kPages, &sh, &pg), 0u);
        ck("pages = 0 can never be tagged",      (uint64_t)vat((uint64_t)base << 16, 0u, &sh, &pg), 0u);
    }
    // ---- the verdict, and its precedence
    ck("pattern beats a register match",  n48_ca_verdict(1, 3u), (uint64_t)N48_CA_V_MEMORY);
    ck("pattern alone is MEMORY",         n48_ca_verdict(1, 0u), (uint64_t)N48_CA_V_MEMORY);
    ck("a register match alone is REGISTER", n48_ca_verdict(0, 1u), (uint64_t)N48_CA_V_REGISTER);
    ck("neither is HARDWARE-INTERNAL",    n48_ca_verdict(0, 0u), (uint64_t)N48_CA_V_INTERNAL);
}

// ---------------------------------------------------------------------------------------------------------------------
// 3. THE MUTANTS
// ---------------------------------------------------------------------------------------------------------------------
static uint32_t f_D1(uint32_t)                  { return N48_CA_TAG; }                       /* no page index */
static uint32_t f_D2(uint32_t p)                { return 0xA5A50000u | (p & 0xFFFFu); }      /* wrong tag */
static int      g_D3(uint64_t b, uint32_t *n)   { const uint64_t k = b / 4096u; if (n) *n = (uint32_t)k; return b != 0; }
static int      m16_D4(uint32_t v, uint64_t va, uint64_t *c) { const uint64_t x = (uint64_t)v << 16; if (c) *c = x; return n48_ca_near(x, va); }
static int      m16_D5(uint32_t v, uint64_t va, uint64_t *c) { const uint64_t x = (uint64_t)v << 12; if (c) *c = x; return n48_ca_readable(v) && n48_ca_near(x, va); }
static int      m16_D6(uint32_t v, uint64_t va, uint64_t *c) { const uint64_t x = (uint64_t)v << 16; if (c) *c = x;
                                                               return n48_ca_readable(v) && n48_ca_delta(x, va) <= N48_CA_NEAR_BYTES; }
static int      m40_D7(uint32_t h, uint32_t l, uint64_t va, uint64_t *c) { const uint64_t x = (((uint64_t)h & 0xFFFFu) << 40) | ((uint64_t)l << 8);
                                                               if (c) *c = x; return n48_ca_readable(h) && n48_ca_readable(l) && n48_ca_near(x, va); }
static int      m40_D8(uint32_t h, uint32_t l, uint64_t va, uint64_t *c) { const uint64_t x = n48_ca_addr40(h, l); if (c) *c = x; return n48_ca_near(x, va); }
static int      va_D9(uint64_t va, uint32_t n, uint32_t *s, uint32_t *p) {   /* only the <<16 window */
    if (s) *s = 16u; if (p) *p = 0u;
    if (!n) return 0;
    const uint64_t lo = (uint64_t)N48_CA_TAG << 16, hi = ((uint64_t)N48_CA_TAG + n - 1u) << 16;
    if (va + N48_CA_NEAR_BYTES < lo || va > hi + N48_CA_NEAR_BYTES) return 0;
    if (p) { const uint64_t w = va >> 16; *p = (w >= N48_CA_TAG && w - N48_CA_TAG < n) ? (uint32_t)(w - N48_CA_TAG) : 0xFFFFFFFFu; }
    return 1;
}
static int      va_D10(uint64_t, uint32_t, uint32_t *s, uint32_t *p) { if (s) *s = 16u; if (p) *p = 0u; return 1; }
static int      va_D11(uint64_t va, uint32_t n, uint32_t *s, uint32_t *p) {   /* no tolerance for the walk's offset */
    static const uint32_t sh[3] = { 0u, 8u, 16u };
    if (s) *s = 0u; if (p) *p = 0u;
    if (!n) return 0;
    for (int i = 0; i < 3; i++) {
        const uint64_t w = va >> sh[i];
        if ((va & (((uint64_t)1 << sh[i]) - 1u)) != 0u) continue;
        if (w >= N48_CA_TAG && w - N48_CA_TAG < n) { if (s) *s = sh[i]; if (p) *p = (uint32_t)(w - N48_CA_TAG); return 1; }
    }
    return 0;
}

int main()
{
    printf("gfx_classa_test — the class-A instrument\n\n");
    printf("THE REGISTER TABLE vs ref/linux-asic-reg/gc_12_0_0_offset.h:\n");
    table_checks();
    printf("\nTHE ARITHMETIC (the real code):\n");
    checks(f_real, g_real, m16_real, m40_real, va_real);
    const int realFail = gFail, realRun = gRun;

    struct Mut { const char *name; FillFn f; PagesFn g; M16Fn m16; M40Fn m40; VaFn va; };
    const Mut muts[] = {
        { "D1  the fill drops the page index (one constant dword)", f_D1,   g_real, m16_real, m40_real, va_real },
        { "D2  the fill uses a different tag",                      f_D2,   g_real, m16_real, m40_real, va_real },
        { "D3  the page guard accepts a carve the tag cannot index", f_real, g_D3,  m16_real, m40_real, va_real },
        { "D4  rule (a) forgets the 0xffffffff NOT-READ refusal",    f_real, g_real, m16_D4,  m40_real, va_real },
        { "D5  rule (a) shifts by 12 instead of 16",                 f_real, g_real, m16_D5,  m40_real, va_real },
        { "D6  rule (a) is <= 2 MiB instead of < 2 MiB",             f_real, g_real, m16_D6,  m40_real, va_real },
        { "D7  rule (b) takes 16 bits of HI instead of 8",           f_real, g_real, m16_real, m40_D7, va_real },
        { "D8  rule (b) forgets the NOT-READ refusal",               f_real, g_real, m16_real, m40_D8, va_real },
        { "D9  the VA test only looks at the base<<16 window",       f_real, g_real, m16_real, m40_real, va_D9 },
        { "D10 the VA test says TAGGED for every VA (fail-open)",    f_real, g_real, m16_real, m40_real, va_D10 },
        { "D11 the VA test has no tolerance for the walk's offset",  f_real, g_real, m16_real, m40_real, va_D11 },
    };
    const int nm = (int)(sizeof(muts) / sizeof(muts[0]));
    int caught = 0;
    printf("\nPLANTED DEFECTS (each must be caught by at least one named check):\n");
    for (int i = 0; i < nm; i++) {
        gFail = 0; gRun = 0; gQuiet = 1;
        checks(muts[i].f, muts[i].g, muts[i].m16, muts[i].m40, muts[i].va);
        const int f = gFail;
        printf("  %-58s %s (%d check(s) fail)\n", muts[i].name, f ? "CAUGHT" : "*** NOT CAUGHT ***", f);
        if (f) caught++;
    }
    gQuiet = 0;
    printf("\nchecks run %d, failures %d; mutants caught %d/%d\n", realRun, realFail, caught, nm);
    const bool pass = realFail == 0 && caught == nm;
    printf("gfx_classa: %s\n", pass ? "N48-CLASSA-TEST-PASS" : "FAIL");
    return pass ? 0 : 1;
}
