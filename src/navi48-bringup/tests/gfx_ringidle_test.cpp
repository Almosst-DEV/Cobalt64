// gfx_ringidle_test.cpp — the idle-time ring write's table, its values, its refusal ladder and its log budget,
// offline ( ->, run arm14).
//
// THE TWO STRONGEST CHECKS IN THIS FILE ARE NOT UNIT TESTS.
//
//   Section 1 #includes the VENDOR HEADER (ref/linux-asic-reg/gc_12_0_0_offset.h and gc_12_0_0_sh_mask.h) and
//   asserts, for every row of n48_ri_reg_table() and for GRBM_GFX_INDEX and GRBM_STATUS, that the offset equals
//   reg<NAME> and `seg` equals reg<NAME>_BASE_IDX, and that every bit mask equals the vendor mask. gave
//   GRBM_GFX_INDEX as "base[1] + 0x2200, unverified"; this is where it stops being unverified.
//
//   Section 2 reads src/xlat12/xlat12_ib.c ITSELF (argv[1], default ../../xlat12/xlat12_ib.c relative to the repo
//   root) and asserts that d_rings' own `d_add(e, <byte>, 0u, <value>)` line exists for each of the six, with the
//   byte address this table implies under byte = (off + 0xA000) * 4 and the value expression this table implies.
//   THE SIX OFFSETS AND THE SIX VALUES ARE THEREFORE DERIVED FROM d_rings AND NOT RETYPED FROM A NOTE, and editing
//   d_rings breaks this suite. It also asserts the two SPI_GS_THROTTLE rows d_rings writes and this table
//   deliberately does NOT, so "six, not eight" is a recorded decision rather than an omission.
//
//   Section 5 measures the four log formats against n48_logf's real 512-byte buffer with worst-case arguments.
//
// PLANTED-DEFECT CONTROL (rule: a test no mutation can break is not testing anything). SIXTEEN mutants are run
// against the SAME checks and each must be caught by at least one of them.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_ringidle_test.cpp -o /tmp/ritest && \
//         /tmp/ritest src/xlat12/xlat12_ib.c src/navi48-bringup/src/apple/AppleHardwareHook.cpp
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "gfx_ringidle.h"
#include "../ref/linux-asic-reg/gc_12_0_0_offset.h"
#include "../ref/linux-asic-reg/gc_12_0_0_sh_mask.h"
#include "../../xlat12/xlat12_ib.h"

static int gFail = 0, gRun = 0, gQuiet = 0;

static void ck(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) printf("  FAIL %-72s got %#llx want %#llx\n", what, (unsigned long long)got,
                            (unsigned long long)want);
    } else if (!gQuiet) printf("  ok   %-72s %#llx\n", what, (unsigned long long)got);
}

static void cks(const char *what, int ok)
{
    gRun++;
    if (!ok) { gFail++; if (!gQuiet) printf("  FAIL %s\n", what); }
    else if (!gQuiet) printf("  ok   %s\n", what);
}

static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

// ---------------------------------------------------------------------------------------------------------------------
// 1. THE TABLE vs THE VENDOR HEADER. Not mutated: these are facts about gc_12_0_0_*.h, not about our arithmetic.
// ---------------------------------------------------------------------------------------------------------------------
struct Vendor { const char *name; uint16_t off; uint8_t seg; };
static const Vendor kVendor[] = {
    { "SPI_ATTRIBUTE_RING_BASE", regSPI_ATTRIBUTE_RING_BASE, regSPI_ATTRIBUTE_RING_BASE_BASE_IDX },
    { "SPI_ATTRIBUTE_RING_SIZE", regSPI_ATTRIBUTE_RING_SIZE, regSPI_ATTRIBUTE_RING_SIZE_BASE_IDX },
    { "GE_POS_RING_BASE",        regGE_POS_RING_BASE,        regGE_POS_RING_BASE_BASE_IDX },
    { "GE_POS_RING_SIZE",        regGE_POS_RING_SIZE,        regGE_POS_RING_SIZE_BASE_IDX },
    { "GE_PRIM_RING_BASE",       regGE_PRIM_RING_BASE,       regGE_PRIM_RING_BASE_BASE_IDX },
    { "GE_PRIM_RING_SIZE",       regGE_PRIM_RING_SIZE,       regGE_PRIM_RING_SIZE_BASE_IDX },
};

static void table_checks()
{
    uint32_t n = 0;
    const n48_ri_reg *t = n48_ri_reg_table(&n);
    const uint32_t nv = (uint32_t)(sizeof(kVendor) / sizeof(kVendor[0]));
    ck("the table has exactly N48_RI_REGS rows", n, N48_RI_REGS);
    ck("the table has exactly as many rows as the vendor list", n, nv);
    for (uint32_t i = 0; i < n && i < nv; i++) {
        char w[160];
        snprintf(w, sizeof(w), "%s offset == gc_12_0_0_offset.h", t[i].name);
        ck(w, t[i].off, kVendor[i].off);
        snprintf(w, sizeof(w), "%s BASE_IDX == gc_12_0_0_offset.h", t[i].name);
        ck(w, t[i].seg, kVendor[i].seg);
        snprintf(w, sizeof(w), "%s name matches the vendor name", t[i].name);
        ck(w, (uint64_t)streq(t[i].name, kVendor[i].name), 1u);
        snprintf(w, sizeof(w), "%s is BASE_IDX 1 (the kext writes through ONE base)", t[i].name);
        ck(w, t[i].seg, 1u);
    }
    uint32_t dup = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i + 1; j < n; j++)
            if (t[i].off == t[j].off && t[i].seg == t[j].seg) dup++;
    ck("no duplicate (seg, offset): a duplicate would write the same dword twice", dup, 0u);
    // GRBM_GFX_INDEX and GRBM_STATUS: named the first "base[1] + 0x2200 - unverified". It is verified here.
    ck("GRBM_GFX_INDEX offset == gc_12_0_0_offset.h", N48_RI_GFX_INDEX_OFF, (uint64_t)regGRBM_GFX_INDEX);
    ck("GRBM_GFX_INDEX BASE_IDX == gc_12_0_0_offset.h (so's `base[1]` is right)",
       N48_RI_GFX_INDEX_SEG, (uint64_t)regGRBM_GFX_INDEX_BASE_IDX);
    ck("GRBM_STATUS offset == gc_12_0_0_offset.h", N48_RI_GRBM_STATUS_OFF, (uint64_t)regGRBM_STATUS);
    ck("GRBM_STATUS BASE_IDX == gc_12_0_0_offset.h", N48_RI_GRBM_STATUS_SEG, (uint64_t)regGRBM_STATUS_BASE_IDX);
    // The broadcast value, assembled from the vendor masks rather than trusted as a constant.
    ck("0xE0000000 == SE|SA|INSTANCE broadcast from gc_12_0_0_sh_mask.h", N48_RI_BROADCAST_ALL,
       (uint64_t)(GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK | GRBM_GFX_INDEX__SA_BROADCAST_WRITES_MASK |
                  GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK));
    ck("GUI_ACTIVE mask == gc_12_0_0_sh_mask.h", N48_RI_GUI_ACTIVE,  (uint64_t)GRBM_STATUS__GUI_ACTIVE_MASK);
    ck("CP_BUSY  mask == gc_12_0_0_sh_mask.h",   N48_RI_CP_BUSY_BIT, (uint64_t)GRBM_STATUS__CP_BUSY_MASK);
    ck("SPI_BUSY mask == gc_12_0_0_sh_mask.h",   N48_RI_SPI_BUSY_BIT,(uint64_t)GRBM_STATUS__SPI_BUSY_MASK);
    ck("GE_BUSY  mask == gc_12_0_0_sh_mask.h",   N48_RI_GE_BUSY_BIT, (uint64_t)GRBM_STATUS__GE_BUSY_MASK);
    // The ring offsets: gfx_ringidle.h keeps its own copies so it stays dependency-free. These are the copies.
    ck("N48_RI_POS_OFF  == xlat12_ib.h's XLAT12_GE_RING_POS_OFF",  N48_RI_POS_OFF,  (uint64_t)XLAT12_GE_RING_POS_OFF);
    ck("N48_RI_PRIM_OFF == xlat12_ib.h's XLAT12_GE_RING_PRIM_OFF", N48_RI_PRIM_OFF, (uint64_t)XLAT12_GE_RING_PRIM_OFF);
    // The byte <-> dword mapping, both directions, at the two anchors the kext static_asserts.
    ck("GC base[1] 0xA000: regGRBM_GFX_CNTL 0x0900 -> the kext's kRegGrbmGfxCntl 0xa900",
       N48_RI_GC1_BASE + regGRBM_GFX_CNTL, 0xa900u);
    ck("uconfig byte of SPI_ATTRIBUTE_RING_BASE is d_rings' 0x31118", N48_RI_UCONFIG_BYTE(t[0].off), 0x31118u);
    ck("uconfig byte of GE_PRIM_RING_SIZE   is d_rings' 0x309ac",    N48_RI_UCONFIG_BYTE(t[5].off), 0x309acu);
}

// ---------------------------------------------------------------------------------------------------------------------
// 2. d_rings IS THE SOURCE OF TRUTH — asserted against xlat12_ib.c's own text.
// ---------------------------------------------------------------------------------------------------------------------
static char *slurp(const char *path, size_t *lenOut)
{
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return nullptr; }
    char *b = (char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return nullptr; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = 0;
    if (lenOut) *lenOut = got;
    return b;
}

// The value expression d_rings uses for each row, written the way d_rings writes it.
static void drings_value_expr(const n48_ri_reg *r, char *out, size_t n)
{
    if (r->kind == N48_RI_K_CONST) snprintf(out, n, "0x%08Xu", r->arg);
    else if (r->arg == 0u)         snprintf(out, n, "(uint32_t)(a >> 16)");
    else snprintf(out, n, "(uint32_t)((a + %s) >> 16)",
                  r->arg == (uint32_t)XLAT12_GE_RING_POS_OFF ? "XLAT12_GE_RING_POS_OFF" : "XLAT12_GE_RING_PRIM_OFF");
}

static void drings_checks(const char *path)
{
    size_t len = 0;
    char *src = slurp(path, &len);
    if (!src) { cks("xlat12_ib.c could not be read - THE SOURCE-OF-TRUTH CHECK DID NOT RUN", 0); return; }
    uint32_t n = 0;
    const n48_ri_reg *t = n48_ri_reg_table(&n);
    for (uint32_t i = 0; i < n; i++) {
        char val[96], want[224], w[256];
        drings_value_expr(&t[i], val, sizeof(val));
        // d_rings writes `d_add(e, 0x31118u, 0u, <value>)`. The byte address comes from OUR table through the
        // mapping, so a wrong offset or a wrong BASE_IDX in the table makes the needle miss.
        snprintf(want, sizeof(want), "d_add(e, 0x%xu, 0u, %s)", N48_RI_UCONFIG_BYTE(t[i].off), val);
        snprintf(w, sizeof(w), "d_rings emits `%s` for %s", want, t[i].name);
        cks(w, strstr(src, want) != nullptr);
    }
    // THE TWO ROWS THIS TABLE DELIBERATELY OMITS.'s parenthesis names eight addresses under the word "six";
    // d_rings does write both SPI_GS_THROTTLE words, and neither is a ring base or a ring size. Their absence from
    // our table is a decision, and these two checks are what make it a recorded one rather than a slip.
    cks("d_rings DOES write SPI_GS_THROTTLE_CNTL1 (0x31110) - and this table deliberately does not",
        strstr(src, "d_add(e, 0x31110u, 0u, 0x12355123u)") != nullptr);
    cks("d_rings DOES write SPI_GS_THROTTLE_CNTL2 (0x31114) - and this table deliberately does not",
        strstr(src, "d_add(e, 0x31114u, 0u, 0x0001544Du)") != nullptr);
    for (uint32_t i = 0; i < n; i++)
        ck("no row of this table is a SPI_GS_THROTTLE register",
           (uint64_t)(N48_RI_UCONFIG_BYTE(t[i].off) == 0x31110u || N48_RI_UCONFIG_BYTE(t[i].off) == 0x31114u), 0u);
    free(src);
}

// ★ arm14's OWN READING, byte for byte from : four byte-identical ATTEMPT lines, all of them
//   `GRBM_STATUS 0xa800382c (GUI_ACTIVE 1 CP_BUSY 1 GE_BUSY 0 SPI_BUSY 0), CP_RB0_RPTR 0x80 WPTR 0x80`.
#define ARM14_GRBM 0xa800382cu
#define ARM14_PTR  0x80u

// ---------------------------------------------------------------------------------------------------------------------
// 2b. THE TWO RULES AS NAMES AND NUMBERS, and arm14's own dword decoded against the vendor masks. Not mutated:
//     these are facts about the selectors and about 0xa800382c, not about our arithmetic.
// ---------------------------------------------------------------------------------------------------------------------
static void rule_checks()
{
    ck("M 1 is the FULL rule",                (uint64_t)N48_RI_RULE_FULL,  1u);
    ck("M 3 is the GE/SPI rule",              (uint64_t)N48_RI_RULE_GESPI, 3u);
    ck("`gfxneuter 26 | 1 << 8` is verb 282", n48_ri_verb(N48_RI_RULE_FULL),  282u);
    ck("`gfxneuter 26 | 3 << 8` is verb 794", n48_ri_verb(N48_RI_RULE_GESPI), 794u);
    cks("M 2 (the lock-out) is not a rule",  !n48_ri_rule_known(2u));
    cks("M 0 is not a rule",                 !n48_ri_rule_known(0u));
    cks("the two rules occupy different counter columns",
        n48_ri_rule_slot(N48_RI_RULE_FULL) != n48_ri_rule_slot(N48_RI_RULE_GESPI));
    cks("slot -> rule round-trips for both",
        n48_ri_rule_of_slot(n48_ri_rule_slot(N48_RI_RULE_FULL))  == N48_RI_RULE_FULL &&
        n48_ri_rule_of_slot(n48_ri_rule_slot(N48_RI_RULE_GESPI)) == N48_RI_RULE_GESPI);
    // the rule NAMES are what a log reader will use to tell a WROTE under one rule from a WROTE under the other
    cks("the GE/SPI rule's name says GE/SPI",            strstr(n48_ri_rule_name(N48_RI_RULE_GESPI), "GE/SPI") != nullptr);
    cks("...and says GUI_ACTIVE is ignored",             strstr(n48_ri_rule_name(N48_RI_RULE_GESPI), "GUI_ACTIVE ignored") != nullptr);
    cks("...and names both of its busy clauses",         strstr(n48_ri_rule_name(N48_RI_RULE_GESPI), "GE_BUSY 0") != nullptr &&
                                                         strstr(n48_ri_rule_name(N48_RI_RULE_GESPI), "SPI_BUSY 0") != nullptr);
    cks("the FULL rule's name still names GUI_ACTIVE",   strstr(n48_ri_rule_name(N48_RI_RULE_FULL), "GUI_ACTIVE 0") != nullptr);
    cks("the two rule names are different strings",     !streq(n48_ri_rule_name(N48_RI_RULE_FULL), n48_ri_rule_name(N48_RI_RULE_GESPI)));
    cks("the two short names are different strings",    !streq(n48_ri_rule_short(N48_RI_RULE_FULL), n48_ri_rule_short(N48_RI_RULE_GESPI)));
    cks("an unknown rule names itself unknown",          strstr(n48_ri_rule_name(2u), "UNKNOWN") != nullptr);
    // every refusal reason must name a clause or a condition; none may be "unknown"
    for (uint32_t r = 0; r < N48_RI_REASONS; r++) cks("every reason has a name", !streq(n48_ri_reason_name(r), "unknown"));
    cks("the GE_BUSY refusal names its clause",  strstr(n48_ri_reason_name(N48_RI_GE_BUSY),  "GE_BUSY == 0 FAILED") != nullptr);
    cks("the SPI_BUSY refusal names its clause", strstr(n48_ri_reason_name(N48_RI_SPI_BUSY), "SPI_BUSY == 0 FAILED") != nullptr);
    cks("the GUI_ACTIVE refusal names its clause", strstr(n48_ri_reason_name(N48_RI_GUI_BUSY), "GUI_ACTIVE == 0 FAILED") != nullptr);
    // ★ arm14's own GRBM_STATUS, decoded against the vendor masks: the log line said GUI_ACTIVE 1 CP_BUSY 1
    //   GE_BUSY 0 SPI_BUSY 0, and that reading is the entire reason M 3 exists.
    ck("arm14: GUI_ACTIVE is SET in 0xa800382c",  (uint64_t)((ARM14_GRBM & N48_RI_GUI_ACTIVE)   != 0u), 1u);
    ck("arm14: CP_BUSY is SET in 0xa800382c",     (uint64_t)((ARM14_GRBM & N48_RI_CP_BUSY_BIT)  != 0u), 1u);
    ck("arm14: GE_BUSY is CLEAR in 0xa800382c",   (uint64_t)((ARM14_GRBM & N48_RI_GE_BUSY_BIT)  != 0u), 0u);
    ck("arm14: SPI_BUSY is CLEAR in 0xa800382c",  (uint64_t)((ARM14_GRBM & N48_RI_SPI_BUSY_BIT) != 0u), 0u);
}

// ---------------------------------------------------------------------------------------------------------------------
// 3. THE VALUES AND THE LADDER — the mutated half.
// ---------------------------------------------------------------------------------------------------------------------
typedef uint32_t (*ValFn)(const n48_ri_reg *, uint64_t);
typedef uint32_t (*EvalFn)(const n48_ri_obs *);
typedef uint32_t (*VerFn)(const uint32_t *, const uint32_t *, uint32_t, uint32_t *);
typedef void     (*CntFn)(n48_ri_counts *, uint32_t, uint32_t);

static uint32_t v_real(const n48_ri_reg *r, uint64_t va) { return n48_ri_value(r, va); }
static uint32_t e_real(const n48_ri_obs *o) { return n48_ri_eval(o); }
static uint32_t k_real(const uint32_t *a, const uint32_t *b, uint32_t n, uint32_t *f) { return n48_ri_verify(a, b, n, f); }
static void     c_real(n48_ri_counts *c, uint32_t rule, uint32_t why) { n48_ri_count(c, rule, why); }

/* D1 the base is shifted by 8 instead of 16 (the program-address shift, not the ring one) */
static uint32_t v_D1(const n48_ri_reg *r, uint64_t va) {
    if (r->kind == N48_RI_K_BASE16) return (uint32_t)((va + r->arg) >> 8);
    return r->arg;
}
/* D2 the ring-relative offset is dropped: every base is the attribute ring's */
static uint32_t v_D2(const n48_ri_reg *r, uint64_t va) {
    if (r->kind == N48_RI_K_BASE16) return (uint32_t)(va >> 16);
    return r->arg;
}
/* D3 the POS and PRIM offsets are swapped */
static uint32_t v_D3(const n48_ri_reg *r, uint64_t va) {
    if (r->kind == N48_RI_K_BASE16) {
        uint32_t a = r->arg;
        if (a == N48_RI_POS_OFF) a = N48_RI_PRIM_OFF; else if (a == N48_RI_PRIM_OFF) a = N48_RI_POS_OFF;
        return (uint32_t)((va + a) >> 16);
    }
    return r->arg;
}
// The mutated ladders are written out in full rather than as flags, so each one is readable as "the real rule with
// exactly this deleted". `drop` selects what is missing; everything else is the specification.
enum EDrop { E_NONE = 0, E_NOTREAD, E_GUI, E_DRAIN, E_M3_GUI, E_M3_GE, E_RULE_IGNORED };
static uint32_t e_variant(const n48_ri_obs *o, EDrop drop) {
    if (!o || !o->ask) return N48_RI_NOT_ASKED;
    if (!n48_ri_rule_known(o->rule)) return N48_RI_BAD_RULE;
    if (o->locked) return N48_RI_LOCKED;
    if (o->done) return N48_RI_ALREADY;
    if (!o->haveGc0 || !o->haveGc1) return N48_RI_NO_GC_BASE;
    if (o->ringVa == 0u) return N48_RI_NO_RING_VA;
    if (drop != E_NOTREAD && (!n48_ri_readable(o->grbm) || !n48_ri_readable(o->gfxIndex))) return N48_RI_NOT_READ;
    if (drop != E_DRAIN && o->rptr != o->wptr) return N48_RI_CP_UNDRAINED;
    const int gespi = (drop == E_RULE_IGNORED) ? 1 : (o->rule == N48_RI_RULE_GESPI);
    if (gespi) {
        if (drop == E_M3_GUI && (o->grbm & N48_RI_GUI_ACTIVE)) return N48_RI_GUI_BUSY;  /* M3 consults GUI_ACTIVE */
        if (drop != E_M3_GE && (o->grbm & N48_RI_GE_BUSY_BIT)) return N48_RI_GE_BUSY;
        if (o->grbm & N48_RI_SPI_BUSY_BIT) return N48_RI_SPI_BUSY;
        return N48_RI_OK;
    }
    if (drop != E_GUI && (o->grbm & N48_RI_GUI_ACTIVE)) return N48_RI_GUI_BUSY;
    if (o->grbm & N48_RI_CP_BUSY_BIT) return N48_RI_CP_BUSY;
    return N48_RI_OK;
}
/* D4 the ladder forgets the NOT-READ refusal: all-ones reads as "no busy bits set, therefore idle" */
static uint32_t e_D4(const n48_ri_obs *o) { return e_variant(o, E_NOTREAD); }
/* D5 the FULL rule drops the GUI_ACTIVE clause: an empty ring is taken for an idle pipeline */
static uint32_t e_D5(const n48_ri_obs *o) { return e_variant(o, E_GUI); }
/* D6 the ladder drops the CP_RB0_RPTR == WPTR clause —'s own criterion, a clause of BOTH rules */
static uint32_t e_D6(const n48_ri_obs *o) { return e_variant(o, E_DRAIN); }
/* D7 the one-per-boot latch is gone */
static uint32_t e_D7(const n48_ri_obs *o) {
    n48_ri_obs c = *o; c.done = 0u; return n48_ri_eval(&c);
}
/* D8 the lock-out is gone */
static uint32_t e_D8(const n48_ri_obs *o) {
    n48_ri_obs c = *o; c.locked = 0u; return n48_ri_eval(&c);
}
/* D9 a ring_va of 0 is accepted — every base would be written as 0 */
static uint32_t e_D9(const n48_ri_obs *o) {
    n48_ri_obs c = *o; if (c.ringVa == 0u) c.ringVa = 0x23f0000000ull; return n48_ri_eval(&c);
}
/* D10 the verb writes even when it was not asked to */
static uint32_t e_D10(const n48_ri_obs *o) {
    n48_ri_obs c = *o; c.ask = 1u; return n48_ri_eval(&c);
}
/* D11 the read-back verdict reports no mismatch ever (fail-open) */
static uint32_t k_D11(const uint32_t *, const uint32_t *, uint32_t n, uint32_t *f) { if (f) *f = n; return 0u; }
/* D12 the read-back verdict loses the first-bad row */
static uint32_t k_D12(const uint32_t *a, const uint32_t *b, uint32_t n, uint32_t *f) {
    uint32_t bad = 0; for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) bad++;
    if (f) *f = 0u; return bad;
}
/* D13 the GE/SPI rule consults GUI_ACTIVE after all — i.e. M 3 is M 1 in disguise and arm14 would refuse again */
static uint32_t e_D13(const n48_ri_obs *o) { return e_variant(o, E_M3_GUI); }
/* D14 the GE/SPI rule drops its GE_BUSY clause — it would write with the geometry engine running */
static uint32_t e_D14(const n48_ri_obs *o) { return e_variant(o, E_M3_GE); }
/* D15 the rule selector is ignored: every ask gets the GE/SPI predicate, so M 1's rule is silently widened */
static uint32_t e_D15(const n48_ri_obs *o) { return e_variant(o, E_RULE_IGNORED); }
/* D16 the per-rule counters blend the two rules into one column */
static void c_D16(n48_ri_counts *c, uint32_t, uint32_t why) {
    if (!c) return; c[0].asks++; if (why == N48_RI_OK) c[0].writes++; else c[0].refusals++;
}

static n48_ri_obs idle_obs(uint32_t rule)
{
    n48_ri_obs o {};
    o.ask = 1u; o.rule = rule; o.locked = 0u; o.done = 0u; o.haveGc0 = 1u; o.haveGc1 = 1u;
    o.ringVa = 0x23f0000000ull;           // arm13's own ring base (gRingMap.vaBase): ATTR_BASE read back 0x0023f000
    o.gfxIndex = N48_RI_BROADCAST_ALL;
    o.grbm = 0x00000000u;
    o.rptr = 0x1234u; o.wptr = 0x1234u;
    return o;
}

// ★ arm14's OWN READING, byte for byte from : four byte-identical ATTEMPT lines, all of them
//   `GRBM_STATUS 0xa800382c (GUI_ACTIVE 1 CP_BUSY 1 GE_BUSY 0 SPI_BUSY 0), CP_RB0_RPTR 0x80 WPTR 0x80`.
static n48_ri_obs arm14_obs(uint32_t rule)
{
    n48_ri_obs o = idle_obs(rule);
    o.grbm = ARM14_GRBM;
    o.rptr = ARM14_PTR; o.wptr = ARM14_PTR;
    return o;
}

static void checks(ValFn V, EvalFn E, VerFn K, CntFn C)
{
    uint32_t n = 0;
    const n48_ri_reg *t = n48_ri_reg_table(&n);

    // ---- the values, against arm13's own dumped register readings ----
    // arm13's OWN ring base. It is not a guess: the dump read SPI_ATTRIBUTE_RING_BASE 0x0023f000 and
    // GE_POS_RING_BASE 0x0023f058, and names gRingMap.vaBase + XLAT12_GE_RING_POS_OFF = 0x23f0580000 as
    // arm3's own fault VA - both give vaBase = 0x23f0000000, from two independent runs.
    const uint64_t ring = 0x23f0000000ull;
    ck("SPI_ATTRIBUTE_RING_BASE value == arm13's dumped 0x0023f000", V(&t[0], ring), 0x0023f000u);
    ck("SPI_ATTRIBUTE_RING_SIZE value == arm13's dumped 0x00020015", V(&t[1], ring), 0x00020015u);
    ck("GE_POS_RING_BASE  value == arm13's dumped 0x0023f058",       V(&t[2], ring), 0x0023f058u);
    ck("GE_POS_RING_SIZE  value == arm13's dumped 0x00002000",       V(&t[3], ring), 0x00002000u);
    ck("GE_PRIM_RING_BASE value == arm13's dumped 0x0023f098",       V(&t[4], ring), 0x0023f098u);
    ck("GE_PRIM_RING_SIZE value == arm13's dumped 0x0c6e07fe",       V(&t[5], ring), 0x0c6e07feu);
    // the three bases are distinct and ordered, at ANY 64 KiB-aligned base
    for (uint64_t b = 0x100000000ull; b < 0x900000000ull; b += 0x40000000ull) {
        const uint32_t a0 = V(&t[0], b), p = V(&t[2], b), q = V(&t[4], b);
        cks("ATTR < POS < PRIM at every tested ring base", a0 < p && p < q);
        ck("POS  base is exactly ATTR + POS_OFF  >> 16", (uint64_t)(p - a0), N48_RI_POS_OFF  >> 16);
        ck("PRIM base is exactly ATTR + PRIM_OFF >> 16", (uint64_t)(q - a0), N48_RI_PRIM_OFF >> 16);
    }
    // a size row never depends on the base
    for (uint32_t i = 0; i < n; i++)
        if (t[i].kind == N48_RI_K_CONST)
            cks("a CONST row is the same at every base", V(&t[i], 0ull) == V(&t[i], 0x7ffff0000ull));

    // ---- the ladder, in the part BOTH rules share ----
    for (uint32_t s = 0; s < N48_RI_RULE_SLOTS; s++) {
        const uint32_t R = n48_ri_rule_of_slot(s);
        { auto o = idle_obs(R);                  ck("the idle observation passes",       E(&o), N48_RI_OK); }
        { auto o = idle_obs(R); o.ask = 0u;      ck("a bare read never writes",          E(&o), N48_RI_NOT_ASKED); }
        { auto o = idle_obs(R); o.locked = 1u;   ck("a locked boot refuses",             E(&o), N48_RI_LOCKED); }
        { auto o = idle_obs(R); o.done = 1u;     ck("a spent write refuses (one per boot, SHARED by both rules)",
                                                                                         E(&o), N48_RI_ALREADY); }
        { auto o = idle_obs(R); o.haveGc0 = 0u;  ck("no GC base[0] refuses",             E(&o), N48_RI_NO_GC_BASE); }
        { auto o = idle_obs(R); o.haveGc1 = 0u;  ck("no GC base[1] refuses",             E(&o), N48_RI_NO_GC_BASE); }
        { auto o = idle_obs(R); o.ringVa = 0ull; ck("no ring base refuses",              E(&o), N48_RI_NO_RING_VA); }
        { auto o = idle_obs(R); o.grbm = 0xFFFFFFFFu;
          ck("an all-ones GRBM_STATUS is NOT READ, never `no bits set`",                 E(&o), N48_RI_NOT_READ); }
        { auto o = idle_obs(R); o.gfxIndex = 0xFFFFFFFFu;
          ck("an all-ones GRBM_GFX_INDEX is NOT READ",                                   E(&o), N48_RI_NOT_READ); }
        { auto o = idle_obs(R); o.wptr = o.rptr + 1u;
          ck("RPTR != WPTR refuses under BOTH rules ('s own criterion)",          E(&o), N48_RI_CP_UNDRAINED); }
        { auto o = idle_obs(R); o.rule = 0u;
          ck("M 0 names no rule and refuses",                                            E(&o), N48_RI_BAD_RULE); }
        { auto o = idle_obs(R); o.rule = 2u;
          ck("M 2 is the lock-out, not a rule, and refuses if it reaches the ladder",     E(&o), N48_RI_BAD_RULE); }
    }
    // ---- rule M 1 (FULL): arm14's rule, UNCHANGED ----
    { auto o = idle_obs(N48_RI_RULE_FULL); o.grbm = N48_RI_GUI_ACTIVE;
      ck("M1: GUI_ACTIVE refuses even with the ring drained",                    E(&o), N48_RI_GUI_BUSY); }
    { auto o = idle_obs(N48_RI_RULE_FULL); o.grbm = N48_RI_CP_BUSY_BIT;
      ck("M1: CP_BUSY refuses even with the ring drained",                       E(&o), N48_RI_CP_BUSY); }
    { auto o = idle_obs(N48_RI_RULE_FULL); o.grbm = N48_RI_GE_BUSY_BIT | N48_RI_SPI_BUSY_BIT;
      ck("M1: GE_BUSY/SPI_BUSY alone are LOGGED, not clauses (GUI_ACTIVE covers them)", E(&o), N48_RI_OK); }
    // (c) M 1 STILL REFUSES arm14's own dword, on the same clause it refused on for real.
    { auto o = arm14_obs(N48_RI_RULE_FULL);
      ck("(c) M1 still REFUSES arm14's 0xa800382c, on GUI_ACTIVE, exactly as it did",    E(&o), N48_RI_GUI_BUSY); }

    // ---- rule M 3 (GE/SPI): the new rule ----
    // (a) THE CASE THE WHOLE CHANGE EXISTS FOR: arm14's exact reading must WRITE under M 3.
    { auto o = arm14_obs(N48_RI_RULE_GESPI);
      ck("(a) M3 WRITES on arm14's 0xa800382c with RPTR == WPTR == 0x80",                E(&o), N48_RI_OK); }
    // (b) and it must refuse on each of its own clauses, and on an unreadable witness.
    { auto o = arm14_obs(N48_RI_RULE_GESPI); o.grbm = ARM14_GRBM | N48_RI_GE_BUSY_BIT;
      ck("(b) M3 REFUSES when GE_BUSY is set",                                           E(&o), N48_RI_GE_BUSY); }
    { auto o = arm14_obs(N48_RI_RULE_GESPI); o.grbm = ARM14_GRBM | N48_RI_SPI_BUSY_BIT;
      ck("(b) M3 REFUSES when SPI_BUSY is set",                                          E(&o), N48_RI_SPI_BUSY); }
    { auto o = arm14_obs(N48_RI_RULE_GESPI); o.wptr = o.rptr + 1u;
      ck("(b) M3 REFUSES when RPTR != WPTR",                                             E(&o), N48_RI_CP_UNDRAINED); }
    { auto o = arm14_obs(N48_RI_RULE_GESPI); o.grbm = 0xFFFFFFFFu;
      ck("(b) M3 REFUSES an all-ones GRBM_STATUS - all ones is NOT READ, never idle",    E(&o), N48_RI_NOT_READ); }
    { auto o = arm14_obs(N48_RI_RULE_GESPI); o.gfxIndex = 0xFFFFFFFFu;
      ck("(b) M3 REFUSES an all-ones GRBM_GFX_INDEX",                                    E(&o), N48_RI_NOT_READ); }
    { auto o = idle_obs(N48_RI_RULE_GESPI); o.grbm = N48_RI_GUI_ACTIVE | N48_RI_CP_BUSY_BIT;
      ck("M3 does NOT consult GUI_ACTIVE or CP_BUSY, even both together",                E(&o), N48_RI_OK); }
    { auto o = arm14_obs(N48_RI_RULE_GESPI); o.locked = 1u;
      ck("M3 is still locked out by `26 | 2 << 8`",                                      E(&o), N48_RI_LOCKED); }
    { auto o = arm14_obs(N48_RI_RULE_GESPI); o.done = 1u;
      ck("M3 cannot spend a second write after M1 spent the first",                      E(&o), N48_RI_ALREADY); }

    // the full cross product, now over BOTH rules and all four decoded status bits: OK exactly when the clauses of
    // THAT rule pass, and the other rule's bits must make no difference at all.
    {
        uint32_t wrong = 0, cases = 0;
        for (uint32_t s = 0; s < N48_RI_RULE_SLOTS; s++)
        for (uint32_t ask = 0; ask < 2; ask++)
        for (uint32_t lock = 0; lock < 2; lock++)
        for (uint32_t done = 0; done < 2; done++)
        for (uint32_t gc = 0; gc < 2; gc++)
        for (uint32_t rv = 0; rv < 2; rv++)
        for (uint32_t rd = 0; rd < 2; rd++)
        for (uint32_t dr = 0; dr < 2; dr++)
        for (uint32_t gui = 0; gui < 2; gui++)
        for (uint32_t cpb = 0; cpb < 2; cpb++)
        for (uint32_t geb = 0; geb < 2; geb++)
        for (uint32_t spb = 0; spb < 2; spb++) {
            const uint32_t R = n48_ri_rule_of_slot(s);
            n48_ri_obs o = idle_obs(R);
            o.ask = ask; o.locked = lock; o.done = done;
            o.haveGc0 = gc; o.haveGc1 = gc;
            o.ringVa = rv ? 0x23f0000000ull : 0ull;
            o.wptr = o.rptr + (dr ? 0u : 1u);
            o.grbm = rd ? ((gui ? N48_RI_GUI_ACTIVE : 0u) | (cpb ? N48_RI_CP_BUSY_BIT : 0u) |
                           (geb ? N48_RI_GE_BUSY_BIT : 0u) | (spb ? N48_RI_SPI_BUSY_BIT : 0u))
                        : 0xFFFFFFFFu;
            const int busy = (R == N48_RI_RULE_GESPI) ? (geb || spb) : (gui || cpb);
            const int shouldPass = ask && !lock && !done && gc && rv && rd && dr && !busy;
            if ((E(&o) == N48_RI_OK) != (shouldPass != 0)) wrong++;
            cases++;
        }
        ck("over all 4096 input combinations x 2 rules, OK exactly when that rule's clauses pass", wrong, 0u);
        ck("...and the cross product really covered every combination", cases, 4096u);
    }

    // ---- (d) THE PER-RULE COUNTERS. A mixed run must stay readable: arm14's dword refused under M1 and written
    //      under M3 must land in two different columns, and the totals must still add up.
    {
        n48_ri_counts c[N48_RI_RULE_SLOTS] {};
        auto f = arm14_obs(N48_RI_RULE_FULL);
        C(c, N48_RI_RULE_FULL,  E(&f));
        C(c, N48_RI_RULE_FULL,  E(&f));
        auto g = arm14_obs(N48_RI_RULE_GESPI);
        C(c, N48_RI_RULE_GESPI, E(&g));
        const uint32_t sf = n48_ri_rule_slot(N48_RI_RULE_FULL), sg = n48_ri_rule_slot(N48_RI_RULE_GESPI);
        ck("(d) the two rules count in different columns", (uint64_t)(sf != sg), 1u);
        ck("(d) M1 asks 2",      c[sf].asks,     2u);
        ck("(d) M1 refusals 2",  c[sf].refusals, 2u);
        ck("(d) M1 writes 0",    c[sf].writes,   0u);
        ck("(d) M3 asks 1",      c[sg].asks,     1u);
        ck("(d) M3 refusals 0",  c[sg].refusals, 0u);
        ck("(d) M3 writes 1",    c[sg].writes,   1u);
        for (uint32_t s = 0; s < N48_RI_RULE_SLOTS; s++)
            ck("(d) every ask is exactly one refusal or one write", c[s].asks, c[s].refusals + c[s].writes);
    }

    // ---- the read-back verdict ----
    {
        uint32_t a[N48_RI_REGS] = { 1, 2, 3, 4, 5, 6 }, b[N48_RI_REGS] = { 1, 2, 3, 4, 5, 6 }, f = 99;
        ck("a clean read-back is 0 mismatches", K(a, b, N48_RI_REGS, &f), 0u);
        ck("...and firstBad is the row count", f, N48_RI_REGS);
        b[2] = 0xdeadbeefu; b[4] = 0u;
        ck("two changed rows are 2 mismatches", K(a, b, N48_RI_REGS, &f), 2u);
        ck("...and firstBad is the FIRST of them", f, 2u);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 5. THE 512-BYTE LOG BUDGET, against n48_logf's real buffer, with worst-case arguments. Not mutated: it is a
//    measurement of the format strings, and a format string that overruns loses its tail with no marker.
// ---------------------------------------------------------------------------------------------------------------------
static void budget(const char *what, int n)
{
    char w[160];
    const int total = n + (int)strlen(N48_RI_LOG_PREFIX) + 1;   /* + HWLOG's "\n" */
    snprintf(w, sizeof(w), "%s worst case is %d byte(s), under n48_logf's %u", what, total, N48_RI_LOG_CAP);
    cks(w, total > 0 && (uint32_t)total <= N48_RI_LOG_BUDGET);
}

static void log_budget_checks()
{
    char b[4096];
    const char *worst = n48_ri_reason_name(N48_RI_CP_UNDRAINED);
    for (uint32_t r = 0; r < N48_RI_REASONS; r++)
        if (strlen(n48_ri_reason_name(r)) > strlen(worst)) worst = n48_ri_reason_name(r);
    // the worst-case RULE name, for the same reason: both are printed on the same lines
    const char *wrule = n48_ri_rule_name(N48_RI_RULE_FULL);
    for (uint32_t s = 0; s < N48_RI_RULE_SLOTS; s++) {
        const char *r = n48_ri_rule_name(n48_ri_rule_of_slot(s));
        if (strlen(r) > strlen(wrule)) wrule = r;
    }
    uint32_t n = 0;
    const n48_ri_reg *t = n48_ri_reg_table(&n);
    budget("ATTEMPT", snprintf(b, sizeof(b), N48_RI_FMT_ATTEMPT, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
                               0xffffffffffffffffull, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
                               0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, wrule, worst));
    cks("the ATTEMPT line names the rule in force", strstr(b, wrule) != nullptr);
    budget("WROTE", snprintf(b, sizeof(b), N48_RI_FMT_WROTE, 0xffffffffu, wrule, 0xffffffffu, 0xffffffffu,
                             0xffffffffu, 0xffffffffu,
                             t[0].shortName, 0xffffffffu, 0xffffffffu, t[1].shortName, 0xffffffffu, 0xffffffffu,
                             t[2].shortName, 0xffffffffu, 0xffffffffu, t[3].shortName, 0xffffffffu, 0xffffffffu,
                             t[4].shortName, 0xffffffffu, 0xffffffffu, t[5].shortName, 0xffffffffu, 0xffffffffu,
                             0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu));
    cks("the WROTE line names the rule in force", strstr(b, wrule) != nullptr);
    budget("REFUSED", snprintf(b, sizeof(b), N48_RI_FMT_REFUSED, wrule, worst, 0xffffffffu, 0xffffffffu, 0xffffffffu,
                               0xffffffffu));
    cks("the REFUSED line names the rule in force", strstr(b, wrule) != nullptr);
    cks("...and names the clause that failed",     strstr(b, worst) != nullptr);
    budget("REPORT", snprintf(b, sizeof(b), N48_RI_FMT_REPORT, 0xffffffffu,
                              "`gfxneuter 26` read it only, nothing was written",   /* the longest of the four */
                              n48_ri_rule_short(N48_RI_RULE_FULL),  0xffffffffu, 0xffffffffu, 0xffffffffu,
                              n48_ri_rule_short(N48_RI_RULE_GESPI), 0xffffffffu, 0xffffffffu, 0xffffffffu,
                              worst, 0xffffffffu, 0xffffffffffffffffull));
    cks("the REPORT line has an M1 column",  strstr(b, "M1 FULL") != nullptr);
    cks("the REPORT line has an M3 column",  strstr(b, "M3 GE/SPI") != nullptr);
    // every short name is short enough that the WROTE line's budget holds
    for (uint32_t i = 0; i < n; i++) cks("every shortName is at most 6 characters", strlen(t[i].shortName) <= 6);
}

// ---------------------------------------------------------------------------------------------------------------------
// 6. THE KEXT SIDE, asserted against AppleHardwareHook.cpp's own text (argv[2]) — the same pattern
//    gfx_desc_port_test.cpp uses. These are the claims a reader of the header cannot check from the header.
// ---------------------------------------------------------------------------------------------------------------------
static void kext_checks(const char *path)
{
    size_t len = 0;
    char *src = slurp(path, &len);
    if (!src) { cks("AppleHardwareHook.cpp could not be read - THE KEXT-SIDE CHECKS DID NOT RUN", 0); return; }
    cks("the verb's selector is 26", strstr(src, "(arg & 0xffull) == 26ull") != nullptr);
    cks("the write is reached ONLY from the verb, and the verb passes M through as the rule",
        strstr(src, "ri_program_rings(m)") != nullptr);
    cks("M 1 and M 3 are the only values that reach the performer",
        strstr(src, "if (m == 1u || m == 3u) (void)ri_program_rings(m);") != nullptr);
    cks("M 2 still locks the boot out", strstr(src, "if (m == 2u) gRi.locked = 1u;") != nullptr);
    {   // exactly one call site, and it is the verb's
        const char *p = src; int calls = 0;
        while ((p = strstr(p, "ri_program_rings(")) != nullptr) { calls++; p += 17; }
        ck("ri_program_rings appears exactly twice (its definition and its ONE call)", (uint64_t)calls, 2u);
    }
    {   // the counters are PER RULE and the one-write-per-boot latch is not
        cks("the kext keeps one counter column per rule", strstr(src, "n48_ri_counts perRule[N48_RI_RULE_SLOTS];") != nullptr);
        cks("the report prints both columns", strstr(src, "n48_ri_rule_short(N48_RI_RULE_GESPI)") != nullptr &&
                                              strstr(src, "n48_ri_rule_short(N48_RI_RULE_FULL)") != nullptr);
        const char *p = src; int bumps = 0;
        while ((p = strstr(p, "n48_ri_count(")) != nullptr) { bumps++; p += 13; }
        ck("counting happens in exactly ONE place, so an ask cannot be counted twice", (uint64_t)bumps, 1u);
        cks("the one-write-per-boot latch is a single flag, not one per rule",
            strstr(src, "uint32_t done, locked, lastWhy;") != nullptr);
    }
    {   // the ONLY register writes in the performer are the seven this instrument declares
        const char *fn = strstr(src, "static uint32_t ri_program_rings(");
        cks("ri_program_rings is present", fn != nullptr);
        if (fn) {
            const char *end = strstr(fn, "\nstatic uint32_t gfxsrc_commit_try(");
            cks("the performer's extent is bounded", end != nullptr);
            if (end) {
                int writes = 0;
                for (const char *p = fn; p < end && (p = strstr(p, "navi48_reg_write32(")) != nullptr && p < end; p += 19) writes++;
                ck("the performer contains exactly 3 register-write statements "
                   "(force, the six-row loop, restore)", (uint64_t)writes, 3u);
                cks("it writes no VRAM",       strstr(fn, "navi48_vram_write_mm") == nullptr || strstr(fn, "navi48_vram_write_mm") > end);
                cks("it never clears the fault status", strstr(fn, "faultclear") == nullptr || strstr(fn, "faultclear") > end);
                cks("it never touches root[511] / the keystone", (strstr(fn, "rootwrite_") == nullptr || strstr(fn, "rootwrite_") > end) &&
                                                                 (strstr(fn, "gKsFlight") == nullptr || strstr(fn, "gKsFlight") > end));
                cks("it never touches the class-A dump's state", (strstr(fn, "gCaDumped") == nullptr || strstr(fn, "gCaDumped") > end) &&
                                                                 (strstr(fn, "gCaFill") == nullptr || strstr(fn, "gCaFill") > end));
            }
        }
    }
    free(src);
}

int main(int argc, char **argv)
{
    const char *ibPath   = argc > 1 ? argv[1] : "src/xlat12/xlat12_ib.c";
    const char *kextPath = argc > 2 ? argv[2] : "src/navi48-bringup/src/apple/AppleHardwareHook.cpp";
    printf("gfx_ringidle_test — the idle-time ring write ( ->)\n\n");
    printf("THE TABLE vs ref/linux-asic-reg/gc_12_0_0_offset.h and _sh_mask.h:\n");
    table_checks();
    printf("\nTHE TWO IDLE RULES, AND arm14's OWN GRBM_STATUS DECODED:\n");
    rule_checks();
    printf("\nd_rings IS THE SOURCE OF TRUTH (%s):\n", ibPath);
    drings_checks(ibPath);
    printf("\nTHE KEXT SIDE (%s):\n", kextPath);
    kext_checks(kextPath);
    printf("\nTHE 512-BYTE LOG BUDGET:\n");
    log_budget_checks();
    printf("\nTHE ARITHMETIC AND THE LADDER (the real code):\n");
    checks(v_real, e_real, k_real, c_real);
    const int realFail = gFail, realRun = gRun;

    struct Mut { const char *name; ValFn v; EvalFn e; VerFn k; CntFn c; };
    const Mut muts[] = {
        { "D1  the base is shifted by 8, not 16",                         v_D1,   e_real, k_real, c_real },
        { "D2  the ring-relative offset is dropped",                      v_D2,   e_real, k_real, c_real },
        { "D3  the POS and PRIM offsets are swapped",                     v_D3,   e_real, k_real, c_real },
        { "D4  the ladder forgets the all-ones NOT-READ refusal",         v_real, e_D4,   k_real, c_real },
        { "D5  the FULL rule drops the GUI_ACTIVE clause",                v_real, e_D5,   k_real, c_real },
        { "D6  the ladder drops CP_RB0_RPTR == WPTR",                     v_real, e_D6,   k_real, c_real },
        { "D7  the one-write-per-boot latch is gone",                     v_real, e_D7,   k_real, c_real },
        { "D8  the lock-out is gone",                                     v_real, e_D8,   k_real, c_real },
        { "D9  a ring base of 0 is accepted",                             v_real, e_D9,   k_real, c_real },
        { "D10 it writes even when it was not asked to",                  v_real, e_D10,  k_real, c_real },
        { "D11 the read-back verdict never reports a mismatch",           v_real, e_real, k_D11 , c_real },
        { "D12 the read-back verdict loses the first bad row",            v_real, e_real, k_D12 , c_real },
        { "D13 the GE/SPI rule consults GUI_ACTIVE after all",            v_real, e_D13,  k_real, c_real },
        { "D14 the GE/SPI rule drops its GE_BUSY clause",                 v_real, e_D14,  k_real, c_real },
        { "D15 the rule selector is ignored (M1 gets M3's predicate)",    v_real, e_D15,  k_real, c_real },
        { "D16 the per-rule counters blend into one column",              v_real, e_real, k_real, c_D16  },
    };
    const int nm = (int)(sizeof(muts) / sizeof(muts[0]));
    int caught = 0;
    printf("\nPLANTED DEFECTS (each must be caught by at least one named check):\n");
    for (int i = 0; i < nm; i++) {
        gFail = 0; gRun = 0; gQuiet = 1;
        checks(muts[i].v, muts[i].e, muts[i].k, muts[i].c);
        const int f = gFail;
        gQuiet = 0;
        printf("  %-62s %s (%d check(s) failed)\n", muts[i].name, f ? "CAUGHT" : "*** MISSED ***", f);
        if (f) caught++;
    }
    printf("\ngfx_ringidle: %d check(s), %d failed, %d of %d planted defect(s) caught — %s\n",
           realRun, realFail, caught, nm, (realFail == 0 && caught == nm) ? "PASS" : "FAIL");
    return (realFail == 0 && caught == nm) ? 0 : 1;
}
