// gfx_memdst.h — R1: THE MEMORY-DESTINATION RUNG (notes/design/R1-MEMDST.md Q2). Pure C++ header (value
// initialisation, like gfx_cp_build.h); host-tested by tests/gfx_memdst_test.cpp; the kext compiles the SAME header.
//
// WHAT THIS CLOSES. Ancestors: gfx_dep.h's n48_cp_scan_frame (R5') and xlat12_ib.c's r4_waits/r4_memwrites both COUNT
// memory-destination packets (WRITE_DATA, RELEASE_MEM, WAIT_REG_MEM, COPY_DATA) in a translated candidate, but nothing
// RESOLVES or JUDGES them: a candidate the translator emits with a live WRITE_DATA to an unmapped, root[511]-window
// or in-flight-IB address would commit as a GPU write to a page nobody has verified. R1-MEMDST.md Q1 names the
// correct rule per packet kind; this header is that rule, over the FINAL CANDIDATE (gXdNew) the frame is about to
// commit - not Apple's input, and not a held-back frame (that is R5', a different scan for a different purpose).
//
// 0.0.441 ( (B), — nine changes over 0.0.440's first cut):
//   R1 the fence828 exemption is now a TIGHTENED IDENTITY (kind RELEASE_MEM, VA == slot, DATA == want, Apple's OWN
//       INPUT holds the buried NOP9-then-RELEASE_MEM bytes at that offset, and there is EXACTLY ONE such row) and
//       exempts that one row from OWN-REGION, the page walk and COUNT's write total — not just ORIGIN.
//   R2 ORIGIN no longer searches Apple's whole input for a byte match anywhere; it walks Apple's input with the
//       SAME classifier and requires the output's live memory packets (minus the one exempt fence row) to equal
//       that list ONE-TO-ONE, IN ORDER, byte for byte, PER INTER-DRAW REGION (a segment never reorders across
//       another segment's rows), with the output's own offset <= the matched input offset (compaction only ever
//       moves a packet EARLIER within its own region, never later, never across a region boundary).
//   R3 the self-satisfied rule now stops at the LAST live write (of EITHER kind) to the WAIT's dword, not the
//       nearest matching RELEASE_MEM — a WRITE_DATA in between now correctly leaves the WAIT unsatisfied.
//   R4 OWN-IB is checked against EACH of a multi-IB frame's own IB ranges, not IB 0's VA over the whole
//       concatenated length.
//   R6 a real, counted (never refusing) NOT-WRITABLE tally — the caller's own responsibility (see kMdNotWritableConfirmed).
//   R7 RELEASE_MEM DST_SEL 1 (TC_L2) is now recognised as a memory destination too (not just DST_SEL 0); DST_SEL 2/3
//       and a WRITE_DATA to a register (DST_SEL 0) or to GDS (DST_SEL 3) are now explicit KIND refusals instead of
//       being silently unrecorded.
// (R0, R5, R8, R9 are call-site/reachability/report changes in AppleHardwareHook.cpp, not in this pure header.)
//
// TWO PHASES, THE SAME SPLIT gfx_dep.h's R5' HAS: `n48_md_scan` is PURE - it walks `out[]` and `in[]` (both already
// in hand, no page table touched) and decides everything that needs no live system: WALK, OVER, KIND, ORIGIN,
// OWN-REGION (by VA alone), OWN-IB (by VA alone), WAIT-UNSATISFIED (a self-contained comparison within the SAME
// scan). It fills one destination row per live memory packet, `resolved`/`writable` left 0 (unknown). The CALLER
// walks `resolved`/`writable` in between (the SAME gfxc_page the R1-R4 consumer already uses, extended to return
// the leaf), then `n48_md_judge` reads the completed record and answers UNRESOLVED / NOT-WRITABLE / COUNT.
//
// THE HAZARD FILTER (R1-MEMDST.md Q2): a WRITE destination inside the arm-scoped hazard set (gfx_hazard.h) is
// RECORDED, never refused, here - our write REPLACES the dropped one, and the later consumer's own R3 (gfx_dep.h)
// covers a stale reader of that page. This header does not read the hazard set at all; it is not part of this rung.
//
// root[511]: NOTHING HERE EVER WALKS IT. OWN-REGION is
// answered by VA alone against `[vm.startVa + 511*2^28, +256 MiB)`, the caller's own scalars - the same reason
// D4-PRIME's arena-range decline (gfx_cp_build.h's n48_cp_readset_in_arena) never walks it either.
#ifndef N48_GFX_MEMDST_H
#define N48_GFX_MEMDST_H

#include <stdint.h>
#include "gfx_hazard.h"   /* not read by this header; included so a caller building both records has one include */
#include "xlat12_ib.h"     /* build 0.0.535 fix round (c): xlat12_ib_nclear_pkt_ok, switch 91's fill packet */

#define N48_MD_MAX 32u   /* R1-MEMDST.md Q2 item OVER: the record's own cap (also the IN-side list's own cap, R2) */

/* PM4 opcodes this rung classifies. A LOCAL copy, not shared with xlat12_ib.c's private enum (that file's opcodes
 * are file-scope static), and not shared with gfx_dep.h's n48_cp_scan_frame either - three independent readings of
 * the same silicon, on purpose: this project's own discipline is that a scan a test cannot break by feeding it the
 * wrong opcode is not proving anything, and three copies triple the chance a wrong constant is caught by a check
 * that compares this header's own counts against the translator's (COUNT, below). */
enum {
    N48_MD_OP_WRITE_DATA      = 0x37,
    N48_MD_OP_WAIT_REG_MEM    = 0x3C,
    N48_MD_OP_COPY_DATA       = 0x40,
    N48_MD_OP_EVENT_WRITE     = 0x46,
    N48_MD_OP_EVENT_WRITE_EOP = 0x47,
    N48_MD_OP_RELEASE_MEM     = 0x49,
    N48_MD_OP_DMA_DATA        = 0x50   /* build 0.0.535 fix round (c) */
};

/* R1 (0.0.441): the buried fence828 candidate's own wrapper, LOCAL copies matching gfx_fence828.h's N48_F828_NOP9 /
 * N48_F828_RELMEM exactly - same discipline as the opcode enum above: an independent reading, not a shared include,
 * so a wrong constant in one file cannot silently make the other agree with it. */
#define N48_MD_F828_NOP9   0xC0071000u   /* PACKET3(NOP, count 7) = 9 dw: header + an 8-dword body (gfx_fence828.h) */
#define N48_MD_F828_RELMEM 0xC0064900u   /* PACKET3(RELEASE_MEM, count 6) = 8 dw, UN-rewritten (gfx_fence828.h) */

enum {
    N48_MD_OK = 0,
    N48_MD_WALK,             /* the scan did not cover exactly n dwords, or met a packet it cannot size */
    N48_MD_OVER,             /* more live memory destinations than N48_MD_MAX can record */
    N48_MD_KIND,             /* COPY_DATA; an EVENT_WRITE longer than 2 dwords; EVENT_WRITE_EOP; RELEASE_MEM DST_SEL
                               * 2/3; WRITE_DATA to a register (DST_SEL 0) or to GDS (DST_SEL 3) */
    N48_MD_ORIGIN,           /* R2: the output's live memory packets (minus the one exempt fence) do not equal, one-to-one
                               * in order and byte for byte per region, Apple's own input's live memory packets */
    N48_MD_OWN_REGION,       /* a destination VA lands inside [vm.startVa + 511*2^28, +256 MiB) - root[511]'s own window */
    N48_MD_OWN_IB,           /* a destination VA lands inside one of this frame's own IB ranges - a write into the stream being read */
    N48_MD_WAIT_UNSATISFIED, /* a WAIT_REG_MEM on memory whose reference this candidate does not itself satisfy */
    N48_MD_UNRESOLVED,       /* a destination whose page did not resolve through the consumer's own VM */
    N48_MD_NOT_WRITABLE,     /* the resolved leaf's WRITEABLE bit (PTEFlags::WRITEABLE, amdgpu_ip.h:306) is not set */
    N48_MD_COUNT,            /* the scan's own wait/write counts (fence row excluded) do not equal r4_waits/r4_memwrites */
    /* build 0.0.492 - APPENDED: a segment of this frame was REFUSED by the translator (or a later per-segment
     * check), or translated to a length other than its input's. The kext restores Apple's own bytes into such a segment, so the
     * scan counts Apple's whole wait/write triplet there while the translator's r4 sums stop at the refusal (RUN A: scan 4/8 vs
     * r4 2/4 on all 10 CNT frames) - a counting artifact, not a hazard. The frame can never commit (the segment-policy verdict
     * and the gate's non-zero-status rung refuse it), so this clause is judged FIRST, is fail-closed (md_ok stays 0) and is NOT
     * a shadow-bad frame (n48_md_shadow_bad). */
    N48_MD_SEG_REFUSED,
    N48_MD_REASONS
};

static inline const char *n48_md_reason_name(uint32_t r)
{
    static const char *const n[N48_MD_REASONS] = {
        "clean", "walk", "over", "KIND", "ORIGIN", "OWN-REGION", "OWN-IB", "WAIT-UNSATISFIED", "UNRESOLVED",
        "NOT-WRITABLE", "COUNT", "SEG-REFUSED" };
    return r < N48_MD_REASONS ? n[r] : "?";
}

/* R4 (0.0.441): one of this frame's own IB ranges, dword-VA-based (`bytes` == 4 * that IB's own declared length).
 * A single-IB frame passes ONE entry; a multi-IB (`gXdBuild.mib`) frame passes one entry PER IB, each IB's OWN VA -
 * not IB 0's VA stretched over the whole concatenated length (the 0.0.440 bug this fixes). */
typedef struct { uint64_t va; uint64_t bytes; } n48_md_ib_range;

/* ONE live memory-destination packet the scan found. `kind` is N48_MD_OP_WRITE_DATA / _RELEASE_MEM / _WAIT_REG_MEM
 * (the only three kinds that ever reach here as a RECORDED destination - COPY_DATA and EVENT_WRITE KIND refusals are
 * counted but never recorded as a destination row, because there is nothing to resolve about a packet already
 * refused outright). `resolved`/`writable`/`page` are 0 (unknown) until the CALLER's page walk fills them; the
 * scan itself never sets them (and R5, AppleHardwareHook.cpp, skips that walk for a row a pure clause already
 * refused - `ownRegion`, `ownIb`, `!originOk` or `fenceExempt`). */
typedef struct {
    uint32_t kind;
    uint64_t va;
    uint32_t dwOff, len;      /* the packet's own [dwOff, dwOff+len) window in `out`/`in`, for ORIGIN's byte-compare */
    uint32_t originOk;        /* filled by the scan: matched Apple's input one-to-one in its own region (R2), OR the fence828 identity */
    uint32_t ownRegion;       /* filled by the scan: VA inside root[511]'s window (always 0 for the exempt fence row, R1) */
    uint32_t ownIb;           /* filled by the scan: VA inside one of this frame's own IB ranges (R4) */
    uint32_t fenceExempt;     /* R1 (0.0.441): this is THE exempt fence828 row - identity-matched, not byte-matched */
    /* WAIT_REG_MEM's own fields (0 for WRITE_DATA/RELEASE_MEM rows) */
    uint32_t isWait;
    uint32_t waitFunc;        /* WAIT_REG_MEM FUNCTION [2:0]; 3 = equal, matching gfx_fence828.h's own convention */
    uint32_t waitRef, waitMask;
    uint32_t waitSatisfied;   /* filled by the scan: the LAST live write (R3: WRITE_DATA or RELEASE_MEM, whichever is
                               * LAST) to that dword before this WAIT, in this SAME scan, is a RELEASE_MEM whose data
                               * matches waitRef under waitMask (the self-satisfied rule) */
    /* filled by the CALLER, between scan and judge, from ONE gfxc_page walk (extended to return the leaf) */
    uint32_t resolved;
    uint32_t writable;
} n48_md_dst;

typedef struct {
    n48_md_dst d[N48_MD_MAX];
    uint32_t n;
    uint32_t over;      /* more destinations than N48_MD_MAX: the record is INCOMPLETE */
    uint32_t kindBad;   /* a KIND-refusing packet was seen anywhere in the walk */
    uint32_t walkOk;    /* the walk covered exactly `n` dwords with no unsizeable packet */
    uint32_t nWaits, nWrites;   /* the scan's OWN counts (the exempt fence row excluded from nWrites, R1), for COUNT */
    uint32_t nFills;    /* build 0.0.535 (c): switch 91's zero-fill packets seen (xlat12_ib_nclear_pkt_ok) - not rows */
    uint64_t fenceVa;   /* the fence828 candidate's slot VA and `want`, carried through for reporting */
    uint32_t fenceWant;
} n48_md_scan_result;

/* Packet length in dwords at in[i], 0 if not sizeable. The ONE-dword NOP (0xFFFF1000, xlat12_ib.h's XLAT12_IB_NOP)
 * and TYPE2 are both counted as ONE dword, exactly as n48_f828_pkt_len and gfx_dep.h's n48_cp_scan_frame already
 * do; anything else is sized by its own PACKET3 count field, so a NOP-BURIED record (the table step's own T#/S#
 * pad, or a dead-page RELEASE_MEM under a NOP header) is skipped as ONE unit and never separately inspected -
 * "NOP bodies skipped", the same contract n48_f828_find's own walk states. */
static inline uint32_t n48_md_pkt_len(uint32_t h, uint32_t n, uint32_t i)
{
    if (h == 0xFFFF1000u) return 1u;
    const uint32_t type = h >> 30;
    if (type == 2u) return 1u;
    if (type != 3u) return 0u;
    const uint32_t total = 2u + ((h >> 16) & 0x3FFFu);
    if (total > n - i) return 0u;
    return total;
}

/* THE ONE CLASSIFIER, shared by n48_md_scan's own walk (over `out`) and n48_md_list_live (over `in`, R2) so the two
 * can never quietly disagree about what counts as a live memory destination. Reads ONE already-sized packet
 * `buf[i..i+len)`; sets `*kindBadOut` (KIND: COPY_DATA, EVENT_WRITE_EOP, an address-carrying EVENT_WRITE, RELEASE_MEM
 * DST_SEL 2/3, WRITE_DATA to a register or to GDS - R7) or fills `*vaOut`/`*isWaitOut`/wait fields and returns 1
 * (isMem). Never called for a NOP/TYPE2 packet (the caller skips those before this). */
static inline uint32_t n48_md_classify(const uint32_t *buf, uint32_t i, uint32_t len, uint32_t *kindBadOut,
                                       uint64_t *vaOut, uint32_t *isWaitOut, uint32_t *waitFuncOut,
                                       uint32_t *waitRefOut, uint32_t *waitMaskOut)
{
    *kindBadOut = 0u; *vaOut = 0ull; *isWaitOut = 0u; *waitFuncOut = 0u; *waitRefOut = 0u; *waitMaskOut = 0u;
    const uint32_t h = buf[i];
    const uint32_t op = (h >> 8) & 0xFFu;
    const uint32_t *b = &buf[i + 1u];
    if (op == N48_MD_OP_COPY_DATA) { *kindBadOut = 1u; return 0u; }
    if (op == N48_MD_OP_EVENT_WRITE_EOP) { *kindBadOut = 1u; return 0u; }
    if (op == N48_MD_OP_EVENT_WRITE) { if (len > 2u) *kindBadOut = 1u; return 0u; }   /* the 2-dword form is fine */
    /* build 0.0.535 fix round (c): DMA_DATA is no longer silently ignored. Switch 91's zero fill (exactly
     * xlat12_ib_nclear_pkt_ok's packet) is RECOGNISED and is not a destination row: it has no counterpart in Apple's input
     * (ORIGIN could never match it), and it is judged instead by the kext's own check (nclear_seg: the packets are exactly the
     * recorded fills of N's own colour target, every page resolving to VRAM in the frame's VM); n48_md_scan counts it. A
     * DMA_DATA whose DST_SEL [21:20] is 2 (dst_nowhere: Apple's shader prefetch) writes nothing: not a row, as before. ANY
     * other DMA_DATA writes memory this rung cannot vouch for: an explicit KIND refusal. */
    if (op == N48_MD_OP_DMA_DATA) {
        if (xlat12_ib_nclear_pkt_ok(&buf[i], len)) return 0u;
        if (len >= 2u && ((buf[i + 1u] >> 20) & 3u) == 2u) return 0u;
        *kindBadOut = 1u; return 0u;
    }
    if (op == N48_MD_OP_WRITE_DATA && len >= 5u) {
        /* R7: DST_SEL [11:8] (WRITE_DATA word 0). 0 (register) and 3 (GDS) are explicit KIND refusals - through
         * 0.0.440 both were silently unrecorded (neither refused nor resolved). Any other non-zero value (memory,
         * TC/L2, etc.) is a memory destination, exactly as gfx_dep.h's R5' and xlat12_ib.c's operand_ok already read
         * this same field (their own comment: "NON-ZERO is a memory destination"). */
        const uint32_t dstSel = (b[0] >> 8) & 0xFu;
        if (dstSel == 0u || dstSel == 3u) { *kindBadOut = 1u; return 0u; }
        if (dstSel != 0u) { *vaOut = ((uint64_t)b[2] << 32) | (uint64_t)(b[1] & ~3u); return 1u; }
        return 0u;
    }
    if (op == N48_MD_OP_RELEASE_MEM && len >= 6u) {
        /* build 0.0.537 (; switch 93, xlat12_ib.h XLAT12_EXTRA_PWS): OUR RELEASE_MEM(PWS) - exactly its 8 dwords
         * (xlat12_ib_pws_release_ok: C0064900 C1704514 and six zero dwords: DST_SEL/INT_SEL/DATA_SEL 0, address 0, data 0) - is
         * RECOGNISED and is not a destination row: it writes no memory (DATA_SEL 0: its only effect is the event, the cache
         * operations and the PWS counter), it has no counterpart in Apple's input (ORIGIN could never match it), and it is judged
         * instead by the kext's own pairing check (pws93_seg: xlat12_ib_pws_check). Any other RELEASE_MEM - one bit different -
         * keeps today's classification below (DATA_SEL 0 is a KIND refusal). */
        if (xlat12_ib_pws_release_ok(&buf[i], len)) return 0u;
        const uint32_t ctrl = buf[i + 2u];
        const uint32_t dstSel = (ctrl >> 16) & 0x3u, intSel = (ctrl >> 24) & 0x7u, dataSel = (ctrl >> 29) & 0x7u;
        /* R7 (0.0.441): a "live" RELEASE_MEM is DST_SEL 0 (memory controller) OR DST_SEL 1 (TC_L2) - BOTH are memory
         * destinations (0.0.440's comment said DST_SEL != 0 "routes elsewhere ... and is not a memory destination at
         * all", which was wrong for DST_SEL 1; corrected here). Either one's own further requirement is unchanged:
         * INT_SEL 0, DATA_SEL 1 or 2. DST_SEL 2 or 3 are now explicit KIND refusals (0.0.440 silently ignored them). */
        if (dstSel == 2u || dstSel == 3u) { *kindBadOut = 1u; return 0u; }
        if (dstSel == 0u || dstSel == 1u) {
            if (intSel != 0u || (dataSel != 1u && dataSel != 2u)) { *kindBadOut = 1u; return 0u; }
            *vaOut = ((uint64_t)buf[i + 4u] << 32) | (uint64_t)buf[i + 3u];
            return 1u;
        }
        return 0u;
    }
    if (op == N48_MD_OP_WAIT_REG_MEM && len >= 7u) {
        /* WAIT_REG_MEM word 0: FUNCTION [2:0], MEM_SPACE bit [4] (0 register / 1 memory - a single bit, not a
         * field), MEM/REG OPERATION [6:5]. MEASURED on the real producers (fixture_wsgc1_headless.h F5/F10):
         * word 0 = 0x13 = 0b10011 -> FUNCTION 3 (equal, matching gfx_fence828.h's own convention) and bit 4 set
         * (memory space), body[1]/[2] = 0x400001000 - the live fence page R1-MEMDST.md Q1 names. */
        const uint32_t engsel = b[0];
        if (((engsel >> 4) & 0x1u) == 1u) {
            *vaOut = ((uint64_t)b[2] << 32) | (uint64_t)(b[1] & ~3u);
            *isWaitOut = 1u; *waitFuncOut = engsel & 0x7u; *waitRefOut = b[3]; *waitMaskOut = b[4];
            return 1u;
        }
        return 0u;
    }
    return 0u;
}

/* R2 (0.0.441): walk `buf[0..n)` with the SAME classifier as n48_md_scan's own walk (n48_md_classify), filling ONLY
 * kind/va/dwOff/len - no origin/ownRegion/ownIb/fence/wait-satisfied context, because this exists to enumerate
 * APPLE'S OWN INPUT for ORIGIN, which needs nothing else. A KIND-refusing packet in `buf` (there should never be one
 * in Apple's own input, but this makes no assumption) is skipped, exactly as n48_md_scan skips it from `d[]`.
 * Returns 0 if the walk did not cover exactly `n` dwords (fail closed: an unwalkable input makes ORIGIN unprovable,
 * never assumed satisfied). */
static inline uint32_t n48_md_list_live(const uint32_t *buf, uint32_t n, n48_md_dst *list, uint32_t maxN, uint32_t *countOut)
{
    uint32_t cnt = 0u, i = 0u;
    while (i < n) {
        const uint32_t h = buf[i];
        const uint32_t len = n48_md_pkt_len(h, n, i);
        if (!len) { *countOut = cnt; return 0u; }
        if (h == 0xFFFF1000u || (h >> 30) == 2u) { i += len; continue; }
        uint32_t kindBad = 0u, isWait = 0u, waitFunc = 0u, waitRef = 0u, waitMask = 0u; uint64_t va = 0ull;
        const uint32_t isMem = n48_md_classify(buf, i, len, &kindBad, &va, &isWait, &waitFunc, &waitRef, &waitMask);
        if (isMem && cnt < maxN) {
            list[cnt] = n48_md_dst {};
            list[cnt].kind = (buf[i] >> 8) & 0xFFu; list[cnt].va = va; list[cnt].dwOff = i; list[cnt].len = len;
            cnt++;
        }
        i += len;
    }
    *countOut = cnt;
    return (i == n) ? 1u : 0u;
}

/* R2 (0.0.441): which inter-draw region dword offset `off` falls in. `segStart[0..nSeg)` are each region's own
 * starting dword offset, ascending, in the SAME coordinate space `out`/`in` share (a segment's translated output is
 * written back at its OWN input start - xlat12_ib_translate_draw_ex's own contract). `nSeg == 0` (no boundaries
 * supplied) puts everything in region 0, the conservative "one region" reading. */
static inline uint32_t n48_md_region_of(const uint32_t *segStart, uint32_t nSeg, uint32_t off)
{
    uint32_t reg = 0u;
    for (uint32_t k = 0u; k < nSeg; k++) { if (segStart[k] <= off) reg = k; else break; }
    return reg;
}

/* THE SCAN. Pure: reads `out[0..n)` and `in[0..inN)`, writes only `r`. `ownRegionBase`/`ownRegionLen` are the
 * caller's own `vm.startVa + 511*2^28` / 256 MiB (never derived here); `ibs[0..nIbs)` are this frame's own IB ranges
 * (R4: one per IB for a multi-IB frame); `segStart[0..nSeg)` are the inter-draw region boundaries ORIGIN matches
 * within (R2); `fenceSlotVa`/`fenceWant` are the fence828 candidate's own slot VA and value (0/0 when no fence is
 * offered this frame - the exemption then simply never grants, which is fail-closed). */
static inline uint32_t n48_md_scan(const uint32_t *out, uint32_t n, const uint32_t *in, uint32_t inN,
                                   uint64_t ownRegionBase, uint64_t ownRegionLen,
                                   const n48_md_ib_range *ibs, uint32_t nIbs,
                                   const uint32_t *segStart, uint32_t nSeg,
                                   uint64_t fenceSlotVa, uint32_t fenceWant, n48_md_scan_result *r)
{
    if (!r) return N48_MD_WALK;
    *r = n48_md_scan_result {};
    r->fenceVa = fenceSlotVa; r->fenceWant = fenceWant;
    if (!out) return N48_MD_WALK;
    uint32_t i = 0u;
    while (i < n) {
        const uint32_t h = out[i];
        const uint32_t len = n48_md_pkt_len(h, n, i);
        if (!len) { r->walkOk = 0u; break; }
        if (h == 0xFFFF1000u || (h >> 30) == 2u) { i += len; continue; }   /* NOP / TYPE2: skipped whole, never inspected */
        if (((h >> 8) & 0xFFu) == N48_MD_OP_DMA_DATA && xlat12_ib_nclear_pkt_ok(&out[i], len)) r->nFills++;   /* 0.0.535 (c) */
        uint32_t kindBad = 0u, isWait = 0u, waitFunc = 0u, waitRef = 0u, waitMask = 0u; uint64_t va = 0ull;
        const uint32_t isMem = n48_md_classify(out, i, len, &kindBad, &va, &isWait, &waitFunc, &waitRef, &waitMask);
        if (kindBad) { r->kindBad = 1u; i += len; continue; }
        if (isMem) {
            if (r->n >= N48_MD_MAX) { r->over = 1u; i += len; continue; }
            n48_md_dst *d = &r->d[r->n++];
            *d = n48_md_dst {};
            d->kind = (out[i] >> 8) & 0xFFu; d->va = va; d->dwOff = i; d->len = len;
            d->ownRegion = (ownRegionLen && va >= ownRegionBase && va < ownRegionBase + ownRegionLen) ? 1u : 0u;
            /* R4: EACH of this frame's own IB ranges, not just IB 0's over the concatenated length. */
            d->ownIb = 0u;
            for (uint32_t bi = 0u; bi < nIbs; bi++)
                if (ibs && ibs[bi].bytes && va >= ibs[bi].va && va < ibs[bi].va + ibs[bi].bytes) { d->ownIb = 1u; break; }
            if (isWait) {
                d->isWait = 1u; d->waitFunc = waitFunc; d->waitRef = waitRef; d->waitMask = waitMask;
                /* R3 (0.0.441): THE LAST LIVE WRITE RULE, CORRECTED. Scan backward from the WAIT's own row for the
                 * NEAREST earlier row (of EITHER kind) at the SAME dword - that is "the last live write to that
                 * dword before the WAIT". It satisfies the WAIT only if it is itself a RELEASE_MEM whose DATA
                 * matches under the WAIT's mask, with FUNCTION 3 (equal). 0.0.440's bug: it skipped PAST any
                 * WRITE_DATA row to find the nearest RELEASE_MEM, so a WRITE_DATA planted between a real RELEASE_MEM
                 * and the WAIT was invisible to this rule - exactly the hazard R1-MEMDST.md Q1 names. */
                if (waitFunc == 3u) {
                    for (uint32_t k = r->n - 1u; k-- > 0u; ) {
                        const n48_md_dst &e = r->d[k];
                        if (e.va != va) continue;
                        if (e.kind == N48_MD_OP_RELEASE_MEM) {
                            const uint32_t data = out[e.dwOff + 5u];   /* RELEASE_MEM's DATA dword: header+5 */
                            if ((data & waitMask) == (waitRef & waitMask)) d->waitSatisfied = 1u;
                        }
                        break;   /* stop at the LAST live write to this dword, whatever kind it is */
                    }
                }
                r->nWaits++;
            } else {
                r->nWrites++;
            }
        }
        i += len;
    }
    r->walkOk = (i == n) ? 1u : 0u;

    // -----------------------------------------------------------------------------------------------------------
    // R1 (0.0.441): THE TIGHTENED FENCE828 IDENTITY. A row is a fence828 candidate only if it is a RELEASE_MEM,
    // its VA equals the caller's slot VA, its DATA dword (header+5) equals `want`, AND Apple's OWN INPUT holds the
    // buried NOP9-then-RELEASE_MEM bytes at that SAME offset (gfx_fence828.h's own nop_at/rel_at: nop_at == dwOff-1,
    // rel_at == dwOff). The exemption is granted only when EXACTLY ONE row meets all four - "exactly one fence row"
    // is itself part of the identity, so two candidates (however that could happen) exempts neither.
    // -----------------------------------------------------------------------------------------------------------
    uint32_t fenceCandCount = 0u, fenceRowIdx = N48_MD_MAX;
    if (fenceSlotVa) {
        for (uint32_t qi = 0u; qi < r->n; qi++) {
            n48_md_dst &d = r->d[qi];
            if (d.kind != N48_MD_OP_RELEASE_MEM || d.va != fenceSlotVa) continue;
            if (d.len <= 5u || out[d.dwOff + 5u] != fenceWant) continue;
            if (d.dwOff < 1u || !in || d.dwOff >= inN) continue;
            if (in[d.dwOff - 1u] != N48_MD_F828_NOP9 || in[d.dwOff] != N48_MD_F828_RELMEM) continue;
            fenceCandCount++; fenceRowIdx = qi;
        }
    }
    const uint32_t fenceGranted = (fenceCandCount == 1u) ? 1u : 0u;
    if (fenceGranted) {
        r->d[fenceRowIdx].fenceExempt = 1u;
        r->d[fenceRowIdx].originOk = 1u;
        r->d[fenceRowIdx].ownRegion = 0u;               /* R1: exempt from OWN-REGION */
        if (r->nWrites > 0u) r->nWrites--;               /* R1: exempt from COUNT's write total */
    }

    // -----------------------------------------------------------------------------------------------------------
    // R2 (0.0.441): ORIGIN, region by region. Apple's own input's live packets (n48_md_list_live, the SAME
    // classifier) must equal the output's live packets (minus the exempt fence row) one-to-one, in order, byte for
    // byte, WITHIN each inter-draw region - a segment's own compaction never reaches into another segment's rows.
    //
    // `inList` is STATIC, not a stack local: this function is `static inline` and n48_md_scan's own caller
    // (gfxsrc_policy's R1 block) inlines into gfxsrc_decide_frame (otool -tV confirms it, the same way the R1 call
    // site's own `mdRes`/`mdIbs`/`mdSegStart` already are) - a 32-entry n48_md_dst array as a STACK local there
    // pushed gfxsrc_decide_frame from 0xb18 to 0x11a8 bytes (measured, build-armg), past the point macOS's
    // ___chkstk_darwin guard-page probe engages, undoing more of D6's own stack saving than D6 gained. `gXdLock`
    // (gfxsrc_policy's own try-lock, held for its whole pass) is n48_md_scan's real caller's own serialisation, the
    // SAME one every other static in this rung already relies on - this header is "pure" in the sense of reading no
    // global system state and deciding deterministically from its arguments, not in the sense of being reentrant
    // under concurrent callers, which nothing in this call chain ever does.
    static n48_md_dst inList[N48_MD_MAX];
    uint32_t inCount = 0u;
    const uint32_t inWalkOk = in ? n48_md_list_live(in, inN, inList, N48_MD_MAX, &inCount) : 0u;
    if (!inWalkOk) {
        for (uint32_t qi = 0u; qi < r->n; qi++) if (!(fenceGranted && qi == fenceRowIdx)) r->d[qi].originOk = 0u;
    } else {
        uint32_t oi = 0u, ii = 0u;
        while (oi < r->n) {
            if (fenceGranted && oi == fenceRowIdx) { oi++; continue; }
            const uint32_t ro = n48_md_region_of(segStart, nSeg, r->d[oi].dwOff);
            uint32_t oEnd = oi;
            while (oEnd < r->n && !(fenceGranted && oEnd == fenceRowIdx) &&
                   n48_md_region_of(segStart, nSeg, r->d[oEnd].dwOff) == ro) oEnd++;
            while (ii < inCount && n48_md_region_of(segStart, nSeg, inList[ii].dwOff) < ro) ii++;
            uint32_t iEnd = ii;
            while (iEnd < inCount && n48_md_region_of(segStart, nSeg, inList[iEnd].dwOff) == ro) iEnd++;
            const uint32_t oCount = oEnd - oi, iCount = iEnd - ii;
            if (oCount != iCount) {
                for (uint32_t j = oi; j < oEnd; j++) r->d[j].originOk = 0u;
            } else {
                for (uint32_t j = 0u; j < oCount; j++) {
                    n48_md_dst &od = r->d[oi + j];
                    const n48_md_dst &id = inList[ii + j];
                    uint32_t byteOk = (od.len == id.len) ? 1u : 0u;
                    if (byteOk) for (uint32_t kk = 0u; kk < od.len; kk++) if (out[od.dwOff + kk] != in[id.dwOff + kk]) { byteOk = 0u; break; }
                    const uint32_t orderOk = (od.dwOff <= id.dwOff) ? 1u : 0u;
                    od.originOk = (byteOk && orderOk) ? 1u : 0u;
                }
            }
            oi = oEnd; ii = iEnd;
        }
        /* any IN-side rows left over past the last OUT region (IN had more live packets, in a region OUT reported
         * none of) also breaks one-to-one - but nothing in `r->d[]` names them, so nothing here to mark; the count
         * mismatch is already caught the moment an OUT region with oCount==0 is compared (iCount would be nonzero,
         * oCount != iCount -> the branch above never fires for an empty oEnd..oi range - a region with ZERO output
         * rows but a nonzero input list is invisible unless a later output row's own region walk reaches it, which
         * it will (regions are visited in the SAME ascending order both lists share). A trailing IN-only region
         * after the LAST output row is the one genuine gap this loop cannot see; ORIGIN only ever compares rows a
         * live OUTPUT packet exists at, matching its own contract ("the output's live memory packets ... must equal
         * that list") - an input region with no output counterpart at all is a packet the translator DROPPED, not
         * one it forged, and is not this rung's rule to enforce. */
    }

    return r->walkOk ? N48_MD_OK : N48_MD_WALK;
}

/* THE JUDGE. Reads a COMPLETED record (the caller has filled resolved/writable). `notWritableConfirmed` is the
 * caller's OWN positive reading of whether the writable bit's meaning is trusted for THIS build (see
 * AppleHardwareHook.cpp's own comment on it) - 0 makes NOT-WRITABLE counted-only, never refusing, exactly as
 * R1-MEMDST.md item D requires when the bit cannot be confirmed. `r4Waits`/`r4Memwrites` are the translator's own
 * sums (xlat12_draw_stats.r4_waits/r4_memwrites, summed over the WHOLE FRAME's segments, independent of in_abi -
 * the same binding D4-PRIME.md's own read-set builder carries) and never include the fence (n48_f828_apply runs
 * AFTER translation, so the translator's own r4 sums never counted it either - R1's COUNT exemption keeps that
 * true on this side too). Returns the FIRST clause in the design's order. */
static inline uint32_t n48_md_judge(const n48_md_scan_result *r, uint32_t r4Waits, uint32_t r4Memwrites,
                                    uint32_t notWritableConfirmed, uint64_t *detail)
{
    if (detail) *detail = 0ull;
    if (!r) return N48_MD_WALK;
    if (!r->walkOk) return N48_MD_WALK;
    if (r->over) return N48_MD_OVER;
    if (r->kindBad) return N48_MD_KIND;
    for (uint32_t i = 0; i < r->n; i++) if (!r->d[i].originOk) { if (detail) *detail = r->d[i].va; return N48_MD_ORIGIN; }
    for (uint32_t i = 0; i < r->n; i++) if (r->d[i].ownRegion) { if (detail) *detail = r->d[i].va; return N48_MD_OWN_REGION; }
    for (uint32_t i = 0; i < r->n; i++) if (r->d[i].ownIb) { if (detail) *detail = r->d[i].va; return N48_MD_OWN_IB; }
    for (uint32_t i = 0; i < r->n; i++) if (r->d[i].isWait && !r->d[i].waitSatisfied) { if (detail) *detail = r->d[i].va; return N48_MD_WAIT_UNSATISFIED; }
    for (uint32_t i = 0; i < r->n; i++) if (!r->d[i].fenceExempt && !r->d[i].resolved) { if (detail) *detail = r->d[i].va; return N48_MD_UNRESOLVED; }
    if (notWritableConfirmed)
        for (uint32_t i = 0; i < r->n; i++) if (!r->d[i].fenceExempt && !r->d[i].writable) { if (detail) *detail = r->d[i].va; return N48_MD_NOT_WRITABLE; }
    if (r->nWaits != r4Waits || r->nWrites != r4Memwrites) {
        if (detail) *detail = ((uint64_t)r->nWaits << 48) | ((uint64_t)r4Waits << 32) | ((uint64_t)r->nWrites << 16) | r4Memwrites;
        return N48_MD_COUNT;
    }
    return N48_MD_OK;
}

/* build 0.0.492: one segment as the frame judge sees it - `status` its final per-segment status (0 = translated
 * and every later per-segment check passed), `outLen` the translator's output length, `inLen` its input span (end - start). A
 * translated segment whose output length differs from its input's is REFUSED too (the kext restores nothing there, but the scan's
 * region boundaries and the fence's placement both assume the lengths are equal). */
static inline uint32_t n48_md_seg_refused(uint32_t status, uint32_t outLen, uint32_t inLen)
{
    return (status != 0u || outLen != inLen) ? 1u : 0u;
}
/* THE FRAME JUDGE the kext calls: N48_MD_SEG_REFUSED first when any segment was refused (detail = the first refused segment's
 * index, 1-based in bits [15:0] so a detail of 0 still means none), else n48_md_judge exactly. `anySegRefused` 0 makes this
 * n48_md_judge, byte for byte (same order, same detail). */
static inline uint32_t n48_md_judge_frame(const n48_md_scan_result *r, uint32_t anySegRefused, uint32_t firstRefusedSeg,
                                          uint32_t r4Waits, uint32_t r4Memwrites, uint32_t notWritableConfirmed, uint64_t *detail)
{
    if (anySegRefused) { if (detail) *detail = (uint64_t)firstRefusedSeg + 1ull; return N48_MD_SEG_REFUSED; }
    return n48_md_judge(r, r4Waits, r4Memwrites, notWritableConfirmed, detail);
}
/* SHADOW's "would have refused under ENFORCE" tally: every clause but CLEAN and SEG_REFUSED (a segment-refused frame is refused
 * anyway, by the segment policy and the gate's own rung, whatever R1 says). */
static inline uint32_t n48_md_shadow_bad(uint32_t clause)
{
    return (clause != N48_MD_OK && clause != N48_MD_SEG_REFUSED) ? 1u : 0u;
}

#endif /* N48_GFX_MEMDST_H */
