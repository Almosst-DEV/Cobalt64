// gfx_ringidle.h — THE IDLE-TIME RING WRITE'S RULE AND TABLE. Pure C, host-tested by
// tests/gfx_ringidle_test.cpp (with planted defects); the kext compiles the SAME header.
// NOTHING HERE READS OR WRITES HARDWARE: it is arithmetic, a table and a refusal ladder.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHAT THIS IS FOR
// ---------------------------------------------------------------------------------------------------------------------
// Class A (GCVM_L2_PROTECTION_FAULT_STATUS_LO32 = 0x00201733, VMID 2, walker error 1 = RANGE, CID 0xb, READ, a wild
// boot-random base) fires inside our committed frame's flight and its address is derivable from NO register the
// class-A dump reads.'s top hypothesis H2 is that the SPI/GE consume the attribute/position/primitive
// ring registers ONLY WHEN THE PIPELINE IS IDLE - mesa's radv_queue.c says so in as many words ("We must wait for
// idle using an EOP event before changing the attribute ring registers") and this project's own r62/r66
// ran the IDENTICAL d_rings block with 0 faults on an otherwise idle GPU. If H2 holds, the MMIO-visible registers
// carry OUR values (they do - the dump reads them back) while the hardware's working copies still hold power-on
// garbage, and the walk we see is that garbage. H1 is the weaker sibling: GRBM_GFX_INDEX was not broadcasting when
// our SET_UCONFIG_REG writes landed, so they reached one SE.
//
// ONE RUN TESTS BOTH: program the same six registers, with the same values, FROM THE KEXT BY MMIO, at a moment the
// pipeline is provably idle, with GRBM_GFX_INDEX read, forced to broadcast and restored around the write.
//
// ---------------------------------------------------------------------------------------------------------------------
// THE SIX REGISTERS, AND WHERE THE OFFSETS AND VALUES COME FROM
// ---------------------------------------------------------------------------------------------------------------------
// THE SOURCE OF TRUTH IS d_rings() IN src/xlat12/xlat12_ib.c - the function that emits these same six writes into
// every translated frame. It is NOT modified (: its extra block has ONE spare dword and growth refuses TOO_LONG),
// and nothing here is retyped from a note. The host test reads xlat12_ib.c ITSELF and asserts, for each of the six,
// that d_rings' own `d_add(e, <byte>, 0u, <value>)` line is present with the byte address and the value expression
// this table implies. Edit d_rings and the suite fails.
//
// THE BYTE <-> DWORD MAPPING. d_rings names each register by its PM4 uconfig BYTE address; this table names it by the
// vendor header's dword offset and BASE_IDX, which is what navi48_reg_read32/write32 take. The relation for a
// BASE_IDX 1 register is byte = (off + 0xA000) * 4, exactly analogous to the `byte = (dw + 0x1260) * 4` mapping
// xlat12_ib.c's own comment uses for BASE_IDX 0, and it is CONFIRMED on this card three ways: GC base[0] = 0x1260
// (regCP_RB0_RPTR 0x0f60 + 0x1260 = 0x21c0, the constant AppleHardwareHook.cpp already reads) and GC base[1] = 0xA000
// (regGRBM_GFX_CNTL 0x0900 + 0xA000 = 0xa900, kRegGrbmGfxCntl). Every one of the six lands on the register the vendor
// header names, and the host test asserts the mapping entry by entry.
//
//   d_rings byte   dword off (BASE_IDX 1)   register                   value d_rings emits
//   0x31118        0x2446                   SPI_ATTRIBUTE_RING_BASE    (uint32_t)(ring_va >> 16)
//   0x3111c        0x2447                   SPI_ATTRIBUTE_RING_SIZE    0x00020015
//   0x309a0        0x2268                   GE_POS_RING_BASE           (uint32_t)((ring_va + POS_OFF) >> 16)
//   0x309a4        0x2269                   GE_POS_RING_SIZE           0x00002000
//   0x309a8        0x226a                   GE_PRIM_RING_BASE          (uint32_t)((ring_va + PRIM_OFF) >> 16)
//   0x309ac        0x226b                   GE_PRIM_RING_SIZE          0x0C6E07FE
//
// SIX, NOT EIGHT, AND THIS IS A DELIBERATE NARROWING OF's PARENTHESIS. says "the six ring registers
// (0x31118/1c, 0x309a0-0x309ac, the two SPI_GS_THROTTLE words)" - a list of EIGHT addresses under the word six.
// d_rings' `if (ex->ring_va)` block does write eight: SPI_GS_THROTTLE_CNTL1 (0x31110 -> 0x2444) and CNTL2
// (0x31114 -> 0x2445) come first. They are NOT here. Neither carries a ring address or a ring size, so neither can
// be the source of a wild ring walk, and this is a hardware write on a pre-frame path: the smallest write set that
// can test H1/H2 is the right one. A reviewer who wants the throttle pair written too need only add two rows.
//
// ---------------------------------------------------------------------------------------------------------------------
// "PROVABLY IDLE" ON THIS HARDWARE - WHAT THE CODE ALREADY READS, AND NOTHING NEW
// ---------------------------------------------------------------------------------------------------------------------
// Three witnesses, all of them registers this kext already reads somewhere else, so none of them is a new probe:
//   (1) CP_RB0_RPTR == CP_RB0_WPTR  - the ring is drained. Both are read on EVERY `vm-fault:` line today.
//   (2) GRBM_STATUS GUI_ACTIVE == 0 - the graphics pipeline, not just the CP, has nothing in it. GRBM_STATUS is read
//       by the `vmib:` instrument today, which also decodes GUI_ACTIVE and CP_BUSY by hand.
//   (3) GRBM_STATUS CP_BUSY == 0    - the same line's second decoded bit.
// (1) alone is what asked for and it is the weakest of the three: an empty ring says nothing about work
// already handed to the SPI/GE, which is precisely the unit H2 is about. GUI_ACTIVE is the OR of the graphics-pipe
// busy bits (GE_BUSY and SPI_BUSY among them), so requiring it clear is strictly stronger and is the closest thing
// this register file has to mesa's "wait for idle". GE_BUSY and SPI_BUSY are read and LOGGED beside the verdict but
// are not separate clauses: GUI_ACTIVE already covers them and a fourth clause would only add ways to refuse.
//
// THE LADDER IS FAIL-CLOSED. Every clause that cannot be answered refuses. An all-ones read (what RREG32 answers for
// a register it could not reach) is NOT READ and refuses; it is never treated as "no bits set, therefore idle".
//
// ---------------------------------------------------------------------------------------------------------------------
// ★ WHAT arm14 MEASURED, AND WHY THERE IS A SECOND RULE
// ---------------------------------------------------------------------------------------------------------------------
// arm14 issued `gfxneuter 282` FOUR times in the post-wskill window and the rule above REFUSED all four, on its own
// GUI_ACTIVE clause, with four byte-identical readings:
//
//     GRBM_STATUS 0xa800382c (GUI_ACTIVE 1 CP_BUSY 1 GE_BUSY 0 SPI_BUSY 0), CP_RB0_RPTR 0x80 WPTR 0x80
//
//'s OWN criterion (RPTR == WPTR) was met, and the two units H2 is about - the GE and the SPI - were IDLE.
// What stopped the write was the paragraph above: the clause added here, not the one asked for. And GUI_ACTIVE
// has NEVER been observed clear after CP init in ANY run on disk, so waiting for it is waiting for something this
// project has no evidence ever happens.
//
// SO THERE ARE TWO RULES, SELECTED BY M, AND THE ONLY DIFFERENCE BETWEEN THEM IS THE IDLE PREDICATE:
//
//   M = 1  N48_RI_RULE_FULL   RPTR == WPTR  AND  GUI_ACTIVE == 0  AND  CP_BUSY == 0     (arm14's rule, UNCHANGED)
//   M = 3  N48_RI_RULE_GESPI  RPTR == WPTR  AND  GE_BUSY    == 0  AND  SPI_BUSY == 0    (arm15's rule)
//
// M = 3 DOES NOT CONSULT GUI_ACTIVE OR CP_BUSY AT ALL - it still DECODES AND PRINTS all four bits, so the reading is
// on the record whichever rule is in force. Everything else is identical and shared, line for line: the same six
// rows, the same values, the same GRBM_GFX_INDEX read/force-broadcast/restore, the same read-back, the same ONE
// WRITE PER BOOT (shared across both rules - the second rule cannot spend a second write), the same all-ones
// NOT READ refusal, the same fail-closed order. M = 2 still locks the boot out for both. A bare `26` reports
// asks/refusals/writes PER RULE, so a run that throws both is readable.
//
// WHAT M = 3 COSTS, STATED PLAINLY: the write lands with the command processor busy. That is not a new kind of
// exposure - d_rings emits these SAME six registers as SET_UCONFIG_REG writes inside EVERY translated frame, i.e.
// from the CP itself while the CP is busy by definition; what is new is only that the write arrives by MMIO from
// our thread rather than from the CP's own stream, so it is not ordered against the CP's outstanding work. The two
// units that consume these registers are proven idle at the moment we write. See  for the full argument.
#ifndef N48_GFX_RINGIDLE_H
#define N48_GFX_RINGIDLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// (a) THE REGISTER TABLE
// ---------------------------------------------------------------------------------------------------------------------
#define N48_RI_REGS 6u

// GC BASE_IDX 1's MMIO dword base on this card, and the PM4 uconfig byte address of a dword in it. Asserted against
// kRegGrbmGfxCntl by the kext and against the vendor header by the host test.
#define N48_RI_GC1_BASE   0xA000u
#define N48_RI_UCONFIG_BYTE(off) (((uint32_t)(off) + N48_RI_GC1_BASE) * 4u)

// The two ring-relative offsets d_rings adds before shifting. These MUST equal xlat12_ib.h's XLAT12_GE_RING_POS_OFF
// and XLAT12_GE_RING_PRIM_OFF; the kext static_asserts it and the host test asserts it against the header itself.
#define N48_RI_POS_OFF    0x00580000u
#define N48_RI_PRIM_OFF   0x00980000u

enum { N48_RI_K_CONST = 0u, N48_RI_K_BASE16 = 1u };

// `kind` N48_RI_K_CONST: the value is `arg`. `kind` N48_RI_K_BASE16: the value is (uint32_t)((ring_va + arg) >> 16),
// which is d_rings' own expression for all three base registers (arg 0 for the attribute ring).
struct n48_ri_reg { const char *name; const char *shortName; uint8_t seg; uint16_t off; uint8_t kind; uint32_t arg; };

static inline const struct n48_ri_reg *n48_ri_reg_table(uint32_t *n) {
    static const struct n48_ri_reg t[] = {
        { "SPI_ATTRIBUTE_RING_BASE", "ATTR_B", 1, 0x2446, N48_RI_K_BASE16, 0u },
        { "SPI_ATTRIBUTE_RING_SIZE", "ATTR_S", 1, 0x2447, N48_RI_K_CONST,  0x00020015u },
        { "GE_POS_RING_BASE",        "POS_B",  1, 0x2268, N48_RI_K_BASE16, N48_RI_POS_OFF },
        { "GE_POS_RING_SIZE",        "POS_S",  1, 0x2269, N48_RI_K_CONST,  0x00002000u },
        { "GE_PRIM_RING_BASE",       "PRIM_B", 1, 0x226a, N48_RI_K_BASE16, N48_RI_PRIM_OFF },
        { "GE_PRIM_RING_SIZE",       "PRIM_S", 1, 0x226b, N48_RI_K_CONST,  0x0C6E07FEu },
    };
    if (n) *n = (uint32_t)(sizeof(t) / sizeof(t[0]));
    return t;
}

// The value for one row. `ring_va` 0 is refused by the ladder before this is ever called, but a 0 here still produces
// a 0 base rather than anything wild.
static inline uint32_t n48_ri_value(const struct n48_ri_reg *r, uint64_t ringVa) {
    if (!r) return 0u;
    if (r->kind == N48_RI_K_BASE16) return (uint32_t)((ringVa + (uint64_t)r->arg) >> 16);
    return r->arg;
}

static inline void n48_ri_values(uint64_t ringVa, uint32_t *out, uint32_t max) {
    uint32_t n = 0;
    const struct n48_ri_reg *t = n48_ri_reg_table(&n);
    for (uint32_t i = 0; i < n && i < max; i++) out[i] = n48_ri_value(&t[i], ringVa);
}

// ---------------------------------------------------------------------------------------------------------------------
// (b) GRBM_GFX_INDEX
// ---------------------------------------------------------------------------------------------------------------------
// regGRBM_GFX_INDEX 0x2200, BASE_IDX 1 (asserted against the vendor header by the host test - gave this
// unverified). The broadcast value is SE_BROADCAST_WRITES | INSTANCE_BROADCAST_WRITES | SA_BROADCAST_WRITES from
// gc_12_0_0_sh_mask.h, i.e. 0xE0000000, which is also this register's reset value and what the kext's own init probe
// reads on this card (amdgpu_gfx.h: "GRBM_GFX_INDEX reads ... the 0xe0000000 reset pattern at GC[1]").
#define N48_RI_GFX_INDEX_OFF   0x2200u
#define N48_RI_GFX_INDEX_SEG   1u
#define N48_RI_BROADCAST_ALL   0xE0000000u

// ---------------------------------------------------------------------------------------------------------------------
// (c) GRBM_STATUS
// ---------------------------------------------------------------------------------------------------------------------
// regGRBM_STATUS 0x0da4, BASE_IDX 0. Bit shifts from gc_12_0_0_sh_mask.h, all four asserted by the host test.
#define N48_RI_GRBM_STATUS_OFF 0x0DA4u
#define N48_RI_GRBM_STATUS_SEG 0u
#define N48_RI_GUI_ACTIVE      0x80000000u   /* GRBM_STATUS__GUI_ACTIVE_MASK  */
#define N48_RI_CP_BUSY_BIT     0x20000000u   /* GRBM_STATUS__CP_BUSY_MASK     */
#define N48_RI_SPI_BUSY_BIT    0x00400000u   /* GRBM_STATUS__SPI_BUSY_MASK    */
#define N48_RI_GE_BUSY_BIT     0x00200000u   /* GRBM_STATUS__GE_BUSY_MASK     */

// ---------------------------------------------------------------------------------------------------------------------
// (d) THE TWO IDLE RULES
// ---------------------------------------------------------------------------------------------------------------------
// The rule number IS the verb's M, so `n48_ri_verb(rule)` reproduces the number the operator types and the REFUSED
// line can name the exact call to re-issue. M 2 is the lock-out and never reaches the ladder as a rule.
#define N48_RI_RULE_FULL   1u   /* RPTR == WPTR, GUI_ACTIVE 0, CP_BUSY 0           - arm14's rule, unchanged */
#define N48_RI_RULE_GESPI  3u   /* RPTR == WPTR, GE_BUSY 0, SPI_BUSY 0             - arm15's rule            */
#define N48_RI_RULE_SLOTS  2u

// Slot 0 is FULL, slot 1 is GE/SPI. An unknown rule is refused (N48_RI_BAD_RULE) before anything is written, and its
// ask is counted in slot 0; that is a deliberate choice, because an uncounted ask would be a silent one.
static inline uint32_t n48_ri_rule_slot(uint32_t rule) { return rule == N48_RI_RULE_GESPI ? 1u : 0u; }
static inline uint32_t n48_ri_rule_of_slot(uint32_t s)  { return s == 1u ? N48_RI_RULE_GESPI : N48_RI_RULE_FULL; }
static inline uint32_t n48_ri_verb(uint32_t rule)       { return 26u | (rule << 8); }
static inline int      n48_ri_rule_known(uint32_t rule) { return rule == N48_RI_RULE_FULL || rule == N48_RI_RULE_GESPI; }

static inline const char *n48_ri_rule_name(uint32_t rule) {
    return rule == N48_RI_RULE_FULL  ? "FULL (RPTR==WPTR, GUI_ACTIVE 0, CP_BUSY 0)"
         : rule == N48_RI_RULE_GESPI ? "GE/SPI (RPTR==WPTR, GE_BUSY 0, SPI_BUSY 0; GUI_ACTIVE ignored)"
         : "UNKNOWN RULE";
}
static inline const char *n48_ri_rule_short(uint32_t rule) {
    return rule == N48_RI_RULE_FULL ? "FULL" : rule == N48_RI_RULE_GESPI ? "GE/SPI" : "UNKNOWN";
}

// ---------------------------------------------------------------------------------------------------------------------
// (e) THE REFUSAL LADDER
// ---------------------------------------------------------------------------------------------------------------------
#define N48_RI_OK           0u
#define N48_RI_NOT_ASKED    1u   /* a bare read of the verb: this is not a request to write                   */
#define N48_RI_LOCKED       2u   /* `26 | 2 << 8` was thrown: no write may happen for the rest of the boot     */
#define N48_RI_ALREADY      3u   /* one write per boot, and it has been spent                                 */
#define N48_RI_NO_GC_BASE   4u   /* a GC segment base did not resolve: we cannot address the registers        */
#define N48_RI_NO_RING_VA   5u   /* gXdRing published no ring base: there is no value to write                */
#define N48_RI_NOT_READ     6u   /* GRBM_STATUS or GRBM_GFX_INDEX read all-ones: the witness is absent         */
#define N48_RI_CP_UNDRAINED 7u   /* CP_RB0_RPTR != CP_RB0_WPTR            - a clause of BOTH rules            */
#define N48_RI_GUI_BUSY     8u   /* GRBM_STATUS GUI_ACTIVE                - a clause of the FULL rule ONLY    */
#define N48_RI_CP_BUSY      9u   /* GRBM_STATUS CP_BUSY                   - a clause of the FULL rule ONLY    */
#define N48_RI_GE_BUSY     10u   /* GRBM_STATUS GE_BUSY                   - a clause of the GE/SPI rule ONLY  */
#define N48_RI_SPI_BUSY    11u   /* GRBM_STATUS SPI_BUSY                  - a clause of the GE/SPI rule ONLY  */
#define N48_RI_BAD_RULE    12u   /* M is neither 1 nor 3: there is no rule to apply                           */
#define N48_RI_TABLE_BAD   13u   /* the register table is not the one this code was written for               */
#define N48_RI_REASONS     14u

struct n48_ri_obs {
    uint32_t ask;        /* the verb asked for the write (M == 1 or M == 3)  */
    uint32_t rule;       /* N48_RI_RULE_FULL or N48_RI_RULE_GESPI            */
    uint32_t locked;     /* `26 | 2 << 8` was thrown this boot               */
    uint32_t done;       /* a write has already been performed this boot     */
    uint32_t haveGc0;    /* navi48_gc_base()      resolved                   */
    uint32_t haveGc1;    /* navi48_gc_base_seg(1) resolved                   */
    uint64_t ringVa;     /* gXdRing.ring_va - the value d_rings was given    */
    uint32_t gfxIndex;   /* GRBM_GFX_INDEX as read                           */
    uint32_t grbm;       /* GRBM_STATUS as read                              */
    uint32_t rptr, wptr; /* CP_RB0_RPTR / CP_RB0_WPTR as read                */
};

static inline int n48_ri_readable(uint32_t v) { return v != 0xFFFFFFFFu; }

// THE ORDER IS THE SPECIFICATION. Cheapest and most structural first, so a refusal names the outermost reason rather
// than an inner one that is merely a consequence of it. A NULL observation refuses; there is no fall-through to OK.
// THE ONLY PLACE THE TWO RULES DIFFER IS THE LAST BLOCK: everything above it is shared, including the drained-ring
// clause asked for and the all-ones NOT READ refusal.
static inline uint32_t n48_ri_eval(const struct n48_ri_obs *o) {
    if (!o || !o->ask)                                   return N48_RI_NOT_ASKED;
    if (!n48_ri_rule_known(o->rule))                     return N48_RI_BAD_RULE;
    if (o->locked)                                       return N48_RI_LOCKED;
    if (o->done)                                         return N48_RI_ALREADY;
    if (!o->haveGc0 || !o->haveGc1)                      return N48_RI_NO_GC_BASE;
    if (o->ringVa == 0u)                                 return N48_RI_NO_RING_VA;
    if (!n48_ri_readable(o->grbm) || !n48_ri_readable(o->gfxIndex)) return N48_RI_NOT_READ;
    if (o->rptr != o->wptr)                              return N48_RI_CP_UNDRAINED;
    if (o->rule == N48_RI_RULE_GESPI) {
        if (o->grbm & N48_RI_GE_BUSY_BIT)                return N48_RI_GE_BUSY;
        if (o->grbm & N48_RI_SPI_BUSY_BIT)               return N48_RI_SPI_BUSY;
        return N48_RI_OK;                                /* GUI_ACTIVE and CP_BUSY are printed, never consulted */
    }
    if (o->grbm & N48_RI_GUI_ACTIVE)                     return N48_RI_GUI_BUSY;
    if (o->grbm & N48_RI_CP_BUSY_BIT)                    return N48_RI_CP_BUSY;
    return N48_RI_OK;
}

static inline const char *n48_ri_reason_name(uint32_t r) {
    return r == N48_RI_OK           ? "IDLE - every clause of the rule in force passed"
         : r == N48_RI_NOT_ASKED    ? "not asked (a bare `gfxneuter 26` reads this line and writes nothing)"
         : r == N48_RI_LOCKED       ? "LOCKED OUT for this boot by `gfxneuter 26 | 2 << 8` - no write can happen"
         : r == N48_RI_ALREADY      ? "ALREADY PROGRAMMED this boot - one write per boot, and it is spent"
         : r == N48_RI_NO_GC_BASE   ? "a GC segment base did not resolve - the registers cannot be addressed"
         : r == N48_RI_NO_RING_VA   ? "no ring base has been published - there is no value to write"
         : r == N48_RI_NOT_READ     ? "GRBM_STATUS or GRBM_GFX_INDEX read all-ones - NOT READ, idleness is unwitnessed"
         : r == N48_RI_CP_UNDRAINED ? "THE CP RING IS NOT DRAINED (CP_RB0_RPTR != CP_RB0_WPTR) - work is outstanding"
         : r == N48_RI_GUI_BUSY     ? "clause GUI_ACTIVE == 0 FAILED - the graphics pipeline is running work"
         : r == N48_RI_CP_BUSY      ? "clause CP_BUSY == 0 FAILED - the command processor is busy"
         : r == N48_RI_GE_BUSY      ? "clause GE_BUSY == 0 FAILED - the geometry engine is running work"
         : r == N48_RI_SPI_BUSY     ? "clause SPI_BUSY == 0 FAILED - the shader processor input is running work"
         : r == N48_RI_BAD_RULE     ? "M names no idle rule this code implements (M must be 1 or 3)"
         : r == N48_RI_TABLE_BAD    ? "the register table is not the one this code was written for"
         : "unknown";
}

// ---------------------------------------------------------------------------------------------------------------------
// (f) THE PER-RULE COUNTERS. A boot may throw BOTH rules, and a total that blends them is unreadable - so every ask
// is counted in its own rule's column and the report prints both. Exactly one of writes/refusals is bumped per ask.
// ---------------------------------------------------------------------------------------------------------------------
struct n48_ri_counts { uint32_t asks, refusals, writes; };

static inline void n48_ri_count(struct n48_ri_counts *c, uint32_t rule, uint32_t why) {
    struct n48_ri_counts *s;
    if (!c) return;
    s = &c[n48_ri_rule_slot(rule)];
    s->asks++;
    if (why == N48_RI_OK) s->writes++; else s->refusals++;
}

// ---------------------------------------------------------------------------------------------------------------------
// (g) THE LOG LINES, AS FORMAT STRINGS, SO THE 512-BYTE BUDGET IS A TEST AND NOT A HOPE
// ---------------------------------------------------------------------------------------------------------------------
// n48_logf (src/amd/n48log.cpp) formats into `char line[512]` and vsnprintf TRUNCATES SILENTLY past that. HWLOG
// prepends "AppleHardwareHook: " and appends "\n". A line that overruns loses its tail with no marker, which for an
// instrument whose whole output is four lines would be a liar of a kind the project's rules list. So the format
// strings live HERE, in one copy, used by the kext through HWLOG and measured by tests/gfx_ringidle_test.cpp with
// worst-case arguments against N48_RI_LOG_BUDGET. Two lines on every path: ATTEMPT + (WROTE | REFUSED).
#define N48_RI_LOG_CAP     512u
#define N48_RI_LOG_PREFIX  "AppleHardwareHook: "
#define N48_RI_LOG_BUDGET  (N48_RI_LOG_CAP - 1u)   /* vsnprintf keeps at most 511 chars plus the NUL */

// EVERY ONE OF THE THREE PER-ASK LINES NAMES THE RULE IN FORCE, because a boot may throw both and a reader who
// cannot tell which predicate produced a WROTE has nothing. All four status bits are printed under BOTH rules.
#define N48_RI_FMT_ATTEMPT \
    "ringidle: ATTEMPT %u (verb %u) - GC base[0] %#x base[1] %#x, ring_va %#llx; GRBM_GFX_INDEX %#010x, " \
    "GRBM_STATUS %#010x (GUI_ACTIVE %u CP_BUSY %u GE_BUSY %u SPI_BUSY %u), CP_RB0_RPTR %#x WPTR %#x. " \
    "IDLE RULE: %s. VERDICT: %s"

#define N48_RI_FMT_WROTE \
    "ringidle: WROTE %u reg(s). IDLE RULE: %s. GRBM_GFX_INDEX %#010x -> %#010x (in-window %#010x) -> restored " \
    "%#010x. want/got %s %08x/%08x %s %08x/%08x %s %08x/%08x %s %08x/%08x %s %08x/%08x %s %08x/%08x. MISMATCH %u " \
    "first %u. RPTR %#x->%#x WPTR %#x->%#x"

#define N48_RI_FMT_REFUSED \
    "ringidle: REFUSED - NOTHING WAS WRITTEN, not one ring register and not GRBM_GFX_INDEX. IDLE RULE: %s. " \
    "CLAUSE: %s. under this rule: asks %u refusals %u writes %u. A refusal latches nothing: re-issue " \
    "`accel gfxneuter %u` while the pipeline is still idle"

#define N48_RI_FMT_REPORT \
    "ringidle: `gfxneuter 26 | M << 8` programs %u register(s) ONCE per boot (both rules share it), only while " \
    "idle BY THE RULE M SELECTS (%s). M1 %s: asks %u refusals %u writes %u. M3 %s: asks %u refusals %u writes %u. " \
    "last: %s. locked %u. ring_va %#llx. d_rings is UNCHANGED"

// The read-back verdict. `firstBad` is the row index of the first mismatch, N48_RI_REGS when there is none.
static inline uint32_t n48_ri_verify(const uint32_t *want, const uint32_t *got, uint32_t n, uint32_t *firstBad) {
    uint32_t bad = 0, first = n;
    for (uint32_t i = 0; i < n; i++)
        if (want[i] != got[i]) { if (bad == 0u) first = i; bad++; }
    if (firstBad) *firstBad = first;
    return bad;
}

#ifdef __cplusplus
}
#endif

#endif /* N48_GFX_RINGIDLE_H */
