// gfx_fence828.h — RE-ENABLE APPLE'S OWN END-OF-PIPE FENCE INSIDE OUR COMMITTED FRAME, BY CHANGING ONE DWORD.
// Pure C, host-tested by tests/gfx_fence828_test.cpp (with planted defects); the kext compiles the SAME header.
// 0.0.377. DEFAULT-INERT: nothing here runs unless the caller is on the armed COMMIT path
// AND the switch `accel gfxneuter 16 | 1 << 8` was thrown this boot.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHY THIS EXISTS
// ---------------------------------------------------------------------------------------------------------------------
// arm5/arm6/arm7 each committed exactly one translated frame among ~740 neutered ones and could not attribute a single
// fault or a single completion to it. closed the channel-array route (the array is identity-indexed 0..24;
// `fh.chanId` is not an index into it and no slot moved across the commit) and showed the SQG fault address is not even
// deterministic. What is left is a signal the GPU itself writes: an end-of-pipe fence.
//
// proved we cannot APPEND one: `xlat12_ib_translate_draw_ex`'s contract is `*out_len == n`, and the tail pad is 0
// dwords on 618 of arm6's 641 OK segments including the committed frame. found we do not have to. Apple's own
// compositor stream already CARRIES a complete end-of-pipe `RELEASE_MEM`, pre-built, at the end of the frame — and then
// buries it inside a NOP packet so the CP steps over it. 649 of them in arm6's capture, across a contiguous array of
// slots at 0x4_0004_0000..0x4_0004_00c8, with 0 executed, 0 polled, and NOTHING executed anywhere in that 4 KiB page.
//
// So the change is: overwrite the NOP HEADER with a ONE-DWORD NOP. The CP then consumes 1 dword instead of 9, reads
// Apple's own `RELEASE_MEM` as the next packet, executes it, and lands on exactly the dword it would have landed on
// before. Zero growth, nothing synthesized, Apple's own event, Apple's own address. Plus one more dword — `DATA_LO` —
// carrying a value WE choose, so the slot we read back says OUR frame and not Apple's.
//
//     CONFIRMED (bytes, notes/logs/runs/arm6/capture.bin, all 43 segments of the committed frame's shape):
//         seg[1017] c0071000   PACKET3(NOP, count 7) = 9 dw   <- THE ONE DWORD WE CHANGE
//         seg[1018] c0064900   PACKET3(RELEASE_MEM, count 6) = 8 dw, body of that NOP
//         seg[1019] 00000514   EVENT_TYPE 0x14 CACHE_FLUSH_AND_INV_TS, EVENT_INDEX 5 (end of pipe)
//         seg[1020] 20000000   DST_SEL 0 (memory controller), INT_SEL 0 (NO INTERRUPT), DATA_SEL 1 (32-bit low)
//         seg[1021] 00040000   seg[1022] 00000004   -> VA 0x4_0004_0000 + slot     (THE DEAD PAGE)
//         seg[1023] 00000001   DATA_LO   <- THE OTHER DWORD WE CHANGE
//         seg[1024] 00000000   DATA_HI   seg[1025] 00000000  INT_CTXID
//         seg[1026] c00a1000   PACKET3(NOP, count 10) = 12 dw — where the CP lands either way
//     Field layout CONFIRMED against Mesa's own gfx12 table, re/graphics/src/mesa/src/amd/packets/
//     cp_pm4_table_data_gfx12.json, pm4_packets/pfp/RELEASE_MEM (word 3: dst_sel 17:16, int_sel 26:24, data_sel 31:29).
//
// THE POSITION IS NOT FIXED, AND THIS HEADER DOES NOT ASSUME IT IS.'s sketch named "segment dword 1017". That is
// true of only 7 of arm6's 43 committed-shape segments; the other 36 carry the same packet at segment dword 1024. So
// this SEARCHES for the packet and VERIFIES every field of it, and refuses if it does not find exactly one.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHAT IT REFUSES, AND WHY EACH REFUSAL IS A SEPARATE REASON
// ---------------------------------------------------------------------------------------------------------------------
// The lesson is the whole design: NEVER OVERWRITE A DWORD YOU HAVE NOT READ AND MATCHED. found the HS
// write clobbering dwords on a positional guard alone. Every condition below is read from the candidate at the instant
// of the decision and again at the instant of the write, and each one has its own reason code so a refusal in a driver
// log names WHICH assumption failed rather than "it did not fire".
//
//   N48_F828_ARG        a null pointer, a stream shorter than the packet, or a candidate that is not the segment
//   N48_F828_WALK       the candidate is not a well-formed PM4 stream that ends exactly at its own length
//   N48_F828_NO_NOP     no NOP packet in this segment has a RELEASE_MEM as its body (93 of arm6's 641 have no
//                       trailing NOP at all; those land here and REFUSE, they are not forced)
//   N48_F828_AMBIGUOUS  more than one candidate: we will not guess which fence Apple meant
//   N48_F828_EVENT      the buried packet is not CACHE_FLUSH_AND_INV_TS / EVENT_INDEX 5 (end of pipe)
//   N48_F828_INT_SEL    INT_SEL != 0 — it would raise an EOP interrupt and perturb the IRQEop counters we read
//   N48_F828_DATA_SEL   DATA_SEL != 1 — the 32 bits we write into DATA_LO would not be what lands in memory
//   N48_F828_DST_SEL    DST_SEL != 0 (memory controller) — the destination is not a memory address at all
//   N48_F828_LIVE_PAGE  the destination is Apple's LIVE fence page 0x4_0000_1000 (28 executed RELEASE_MEM, 28
//                       executed WAIT_REG_MEM, a monotone 0x111111xx sequence Apple actively waits on). Writing
//                       there could hang the machine. It has its own reason so the log can NAME it.
//   N48_F828_NOT_DEAD   the destination is not inside the measured dead page, or is not 4-byte aligned
//   N48_F828_NOT_MATCHED  between the decision and the write, a dword we matched changed. Nothing is written.
//
// 0.0.413 (notes/design/FENCE-OWNED-SLOT.md,) ADDS THE OWNED-SLOT REFUSALS, each its own named, counted reason:
//   N48_F828_NO_RING        the ring map is not built, or its VA base is 0 — there is no arena to point the packet at
//   N48_F828_NO_ROOT        root[511] is not armed for the committing context at the instant of the decision (B4)
//   N48_F828_SLOT_VA        the computed slot VA is not inside our own fence page (the last 4 KiB of the arena)
//   N48_F828_ADDR_UNPROVEN  the packet's ADDRESS words were not re-proven at write time (a dword moved under us)
//   N48_F828_SLOT_PRE       the slot already held the exact target BEFORE the commit — stale, or an alias
//   N48_F828_EPOCH          the per-boot epoch cannot be stamped (zero): a watch must never be armed
//   N48_F828_REGION_MOVED   the ring region was rebuilt/moved under a live flight, so the slot VA names other bytes
//
// 0.0.415 (the review of 0.0.413, Q1/Q3/Q4) ADDS:
//   N48_F828_PRE_UNREADABLE the pre-commit read of the slot FAILED. Through 0.0.413 the read failure left `pre` 0, which
//                           the gate then compared against `want` and passed — an unreadable slot was treated as an
//                           empty one and the positive control silently vanished (Q3).
//   N48_F828_MULTI_SEGMENT  the frame has more than one segment. The packet sits in ONE segment and fires at that
//                           segment's end, so for a multi-segment frame the fence would claim end of pipe before the
//                           later segments ran (Q1, the step-10 flag). The fence site CAN know (the translator's own
//                           segment count is in scope), so it refuses instead of guessing.
//
// 0.0.418 (notes/design/BUILD-0.0.418.md, E2/E3):
//   E2  `n48_f828_eop_seen` — the committed record carries the `flight` it was promoted on, and `ks_eop_seen()` answers
//       end-of-pipe only when that flight is the CURRENT one. A later flight whose own fence was refused keeps the old
//       latch (0.0.415 Q4) but can no longer be read as its own end-of-pipe.
//   E3  `n48_f828_region_moved_reason` — a REGION-MOVED candidate NEUTERS the frame instead of merely dropping the
//       candidate while the frame runs: an executed, un-promoted packet would write `want` into the slot the next
//       candidate recomputes, pinning it (and every candidate after it) at SLOT-PRE for the rest of the boot.
//
// ---------------------------------------------------------------------------------------------------------------------
// FALSIFIERS — what the next armed run can show, and what each outcome means
// ---------------------------------------------------------------------------------------------------------------------
//   value present at the VA and EQUAL to N48_F828_VALUE  =>  our committed frame reached END OF PIPE. Attribution is
//                                                            solved: a completion is ours, by a value only we write.
//   value absent (the slot still reads Apple's own DATA_LO) => the frame did not reach end of pipe, OR the packet
//                                                            never executed (the CP was NOPed, or it faulted first).
//                                                            The two are told apart by the RING EXEMPTION line.
//   value present but stale or wrong  =>  the slot is shared with something else, or our write did not take. The
//                                         "before" read in the same log is what makes this distinguishable.
//   the machine hangs, or a WAIT_REG_MEM stalls on this VA  =>  the page was NOT as dead as measured. STOP;
//                                         the whole premise of this change is that nothing waits on 0x4_0004_0000.
//
#ifndef N48_GFX_FENCE828_H
#define N48_GFX_FENCE828_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The two pages, both measured in over arm6's capture. The dead one is the ONLY one this header will ever
 * enable a write to; the live one is named so that a refusal can say so out loud. */
#define N48_F828_DEAD_PAGE 0x0000000400040000ull   /* 649 RELEASE_MEM slots, 0 executed, 0 polled, 0 anything */
#define N48_F828_LIVE_PAGE 0x0000000400001000ull   /* Apple's live EOP fence: 28 written, 28 waited on. NEVER. */

/* The one-dword NOP. CONFIRMED: `PKT3_NOP_PAD = PKT3(PKT3_NOP, 0x3fff, 0)` in the vendored Mesa,
 * re/graphics/src/mesa/src/amd/common/sid.h:300, commented "header-only version" —
 * PKT_TYPE_S(3)|PKT_COUNT_S(0x3fff)|(0x10 << 8) = 0xC0000000|0x3FFF0000|0x1000 = 0xFFFF1000.
 * Same constant as XLAT12_IB_NOP (src/xlat12/xlat12_ib.h:46); duplicated rather than included so this header stays
 * standalone, and asserted equal to it by the kext's own static_assert at the call site. */
#define N48_F828_NOP1      0xFFFF1000u

/* The exact dwords we require. Exactness is the guard: a 9-dword NOP whose body is exactly the 8-dword RELEASE_MEM
 * leaves the CP landing on the SAME dword after the rewrite as before it. A longer NOP would leave orphan dwords
 * after the un-NOPed packet, which the CP would read as a header — so a longer NOP is REFUSED, not trimmed. */
#define N48_F828_NOP9      0xC0071000u   /* PACKET3(NOP, count 7) = 9 dw: header + an 8-dword body */
#define N48_F828_RELMEM    0xC0064900u   /* PACKET3(RELEASE_MEM, count 6) = 8 dw */

/* The value we write into DATA_LO. Chosen so that it cannot be confused with anything already in this page: Apple's
 * own slot values in arm6's capture are 0x1..0x30, and the LIVE page's sequence is 0x111111xx. */
#define N48_F828_VALUE     0x4E480828u   /* "N48" + the notes section that found the packet */

/* =====================================================================================================================
 * 0.0.413 (notes/design/FENCE-OWNED-SLOT.md,) — THE OWNED SLOT.
 * Apple's per-submission slot is TRANSIENT: something zeroes it within a frame interval, so end-of-pipe is missed for
 * every commit after the first and the keystone deferral always ends by its 2 s timeout. This build re-points
 * the SAME buried Apple RELEASE_MEM at a slot in the LAST 4 KiB of the relocation arena — memory WE own, mapped through
 * root[511], never recycled by Apple — and writes `epoch|ordinal` there. `ringmap` zeroes this page when it builds the
 * ring map; the per-boot epoch makes a value left by a previous boot unable to match (VRAM survives a warm reboot,
 *); and a candidate the COMMIT gate refuses no longer touches the committed frame's watch, because the reset has
 * moved to the commit promotion site.
 *
 * N48_F828_FENCE_PAGE_OFF is XLAT12_RELOC_ARENA_OFF + XLAT12_RELOC_ARENA_BYTES (0xA81000 + 0xE000 = 0xA8F000). This
 * header is standalone and deliberately does not include xlat12_ib.h; the kext carries a static_assert tying the two.
 * ===================================================================================================================== */
#define N48_F828_FENCE_PAGE_OFF    0x00A8F000u   /* the last 4 KiB of the relocation arena */
#define N48_F828_FENCE_PAGE_BYTES  0x00001000u
#define N48_F828_SLOTS             1024u         /* one 4-byte slot per commit ordinal, ordinal mod 1024 */
#define N48_F828_SLOT_BYTES        4u

enum {
    N48_F828_OK = 0,
    N48_F828_ARG,
    N48_F828_WALK,
    N48_F828_NO_NOP,
    N48_F828_AMBIGUOUS,
    N48_F828_EVENT,
    N48_F828_INT_SEL,
    N48_F828_DATA_SEL,
    N48_F828_DST_SEL,
    N48_F828_LIVE_PAGE_REFUSED,
    N48_F828_NOT_DEAD,
    N48_F828_NOT_MATCHED,
    /* 0.0.413 — THE OWNED-SLOT REFUSALS. Each is a named, counted reason. */
    N48_F828_NO_RING,        /* the ring map is not built, or its VA base is 0: there is no arena to point at */
    N48_F828_NO_ROOT,        /* root[511] is not armed for the committing context at the instant of the decision */
    N48_F828_SLOT_VA,        /* the computed slot VA is not inside our own fence page */
    N48_F828_ADDR_UNPROVEN,  /* an address word was not re-proven at write time - a dword moved under us */
    N48_F828_SLOT_PRE,       /* the slot already held the exact target BEFORE the commit: stale or an alias */
    N48_F828_EPOCH,          /* the per-boot epoch cannot be stamped (zero): a watch must never be armed */
    N48_F828_REGION_MOVED,   /* the ring region was rebuilt/moved under a live flight */
    /* 0.0.415 — the two refusals the 0.0.413 review found missing (Q3, Q1). Each is named and counted. */
    N48_F828_PRE_UNREADABLE, /* the pre-commit read of the slot FAILED: there is no positive control (Q3) */
    N48_F828_MULTI_SEGMENT,  /* the frame has more than one segment: the fence would claim end of pipe at the
                              * end of THIS segment, before the later ones run (Q1, the step-10 flag) */
    /* build 0.0.508 (switch 71): n48_f828_find_last only. A packet that is NOT a NOP follows the chosen
     * (last) candidate inside the searched slice: the release would signal completion before that work. */
    N48_F828_TAIL_WORK,
    N48_F828_REASONS
};

static inline const char *n48_f828_reason_name(uint32_t why)
{
    switch (why) {
    case N48_F828_OK:                 return "OK";
    case N48_F828_ARG:                return "ARG";
    case N48_F828_WALK:               return "WALK";
    case N48_F828_NO_NOP:             return "NO-TRAILING-NOP";
    case N48_F828_AMBIGUOUS:          return "AMBIGUOUS";
    case N48_F828_EVENT:              return "EVENT";
    case N48_F828_INT_SEL:            return "INT-SEL";
    case N48_F828_DATA_SEL:           return "DATA-SEL";
    case N48_F828_DST_SEL:            return "DST-SEL";
    case N48_F828_LIVE_PAGE_REFUSED:  return "LIVE-PAGE-REFUSED";
    case N48_F828_NOT_DEAD:           return "NOT-DEAD-PAGE";
    case N48_F828_NOT_MATCHED:        return "NOT-MATCHED";
    case N48_F828_NO_RING:            return "NO-RING";
    case N48_F828_NO_ROOT:            return "NO-ROOT";
    case N48_F828_SLOT_VA:            return "SLOT-VA-OUTSIDE-PAGE";
    case N48_F828_ADDR_UNPROVEN:      return "ADDR-WORD-UNPROVEN";
    case N48_F828_SLOT_PRE:           return "SLOT-ALREADY-TARGET";
    case N48_F828_EPOCH:              return "EPOCH-UNSTAMPABLE";
    case N48_F828_REGION_MOVED:       return "REGION-MOVED";
    case N48_F828_PRE_UNREADABLE:     return "PRE-UNREADABLE";
    case N48_F828_MULTI_SEGMENT:      return "MULTI-SEGMENT";
    case N48_F828_TAIL_WORK:          return "TAIL-WORK";
    default:                          return "?";
    }
}

/* Everything the decision found, so that the write can re-prove it and the log can print it. */
typedef struct {
    uint32_t why;        /* N48_F828_* */
    uint32_t nop_at;     /* index of the 9-dword NOP header — ONE OF THE FOUR DWORDS THE REWRITE CHANGES */
    uint32_t rel_at;     /* index of the RELEASE_MEM header (== nop_at + 1) */
    uint32_t data_at;    /* index of DATA_LO (== rel_at + 5) */
    uint32_t next_at;    /* index of the packet the CP lands on either way (== nop_at + 9) */
    uint32_t addr_lo;    /* 0.0.413: the packet's OWN address low dword, as read (rel_at + 3) — re-proven at apply */
    uint32_t addr_hi;    /* 0.0.413: the packet's OWN address high dword, as read (rel_at + 4) — re-proven at apply */
    uint32_t nop_hdr;    /* Apple's own NOP header dword, as read */
    uint32_t old_data;   /* Apple's own DATA_LO, as read */
    uint32_t candidates; /* how many buried RELEASE_MEM packets the walk saw (OK requires exactly 1) */
    uint64_t va;         /* the fence VA the packet targets (Apple's dead page) */
} n48_f828;

/* One PM4 packet's length in dwords, 0 if the header is not one we will walk over. The one-dword NOP is the special
 * case that MUST come first: by its count field alone it would read as 16385 dwords, and the translator's own pad is
 * made of these (src/xlat12/tests/test_xlat12_ib.c:1148 checks a whole pad run of them). */
static inline uint32_t n48_f828_pkt_len(uint32_t h)
{
    if (h == N48_F828_NOP1) return 1u;
    if ((h >> 30) == 3u) return ((h >> 16) & 0x3FFFu) + 2u;
    if ((h >> 30) == 2u) return 1u;                      /* TYPE2 is a one-dword NOP */
    return 0u;
}

/* 0.0.426 (notes/design/MIB-COMMIT.md binding B3) — WHETHER THE FENCE MAY BE OFFERED AT THIS SEGMENT.
 *
 * The packet fires at the END of the segment it sits in. For a single-IB frame that is the frame's end. For a multi-IB
 * frame read into one buffer and segmented on its own, the frame's end is the FINAL segment (the last segment of the LAST
 * IB); a fence offered anywhere earlier would claim end of pipe before the later segments ran - the same hazard Q1 named
 * for a multi-segment frame and the reason `multiSeg` refuses. This helper is the offer decision ALONE and is pure so the
 * call site and a host test agree:
 *     mib 0 -> 1 for every segment: 0.0.425's rule, where the caller's `!gXdF828Pending` holds it to one fence per frame
 *              and `multiSeg` refuses a multi-segment frame later.
 *     mib 1 -> 1 only for the frame's FINAL segment (k + 1 == nseg); every earlier segment is NOT offered and the call site
 *              counts N48_F828_MULTI_SEGMENT for it, exactly as the multi-segment refusal would.
 * `nseg == 0` is never a frame (no segment), so it offers nothing. */
static inline uint32_t n48_f828_offered(uint32_t k, uint32_t nseg, uint32_t mib)
{
    if (!mib) return 1u;
    if (nseg == 0u) return 0u;
    return (k + 1u == nseg) ? 1u : 0u;
}

/* THE DECISION. Pure: reads `out[0..n)` and writes NOTHING. Returns the reason; `r` is always filled. */
static inline uint32_t n48_f828_find(const uint32_t *out, uint32_t n, n48_f828 *r)
{
    n48_f828 z;
    uint32_t i, found = 0u, at = 0u;

    z.why = N48_F828_ARG; z.nop_at = 0u; z.rel_at = 0u; z.data_at = 0u; z.next_at = 0u;
    z.addr_lo = 0u; z.addr_hi = 0u;
    z.nop_hdr = 0u; z.old_data = 0u; z.candidates = 0u; z.va = 0ull;
    if (!r) return N48_F828_ARG;
    *r = z;
    if (!out || n < 10u) return N48_F828_ARG;

    /* Walk the WHOLE candidate. A stream that does not end exactly on its own length is one we do not understand,
     * and a stream we do not understand is one we do not edit. */
    for (i = 0u; i < n; ) {
        const uint32_t len = n48_f828_pkt_len(out[i]);
        if (!len || i + len > n) { r->why = N48_F828_WALK; return r->why; }
        if (out[i] == N48_F828_NOP9 && out[i + 1u] == N48_F828_RELMEM) { found++; at = i; }
        i += len;
    }
    if (i != n) { r->why = N48_F828_WALK; return r->why; }

    r->candidates = found;
    if (!found) { r->why = N48_F828_NO_NOP; return r->why; }
    if (found > 1u) { r->why = N48_F828_AMBIGUOUS; return r->why; }

    /* `at` is a 9-dword packet inside [0, n) by the walk above, so at+8 is in range. */
    r->nop_at = at; r->rel_at = at + 1u; r->data_at = at + 6u; r->next_at = at + 9u;
    r->addr_lo = out[at + 4u]; r->addr_hi = out[at + 5u];
    r->nop_hdr = out[at]; r->old_data = out[at + 6u];
    r->va = ((uint64_t)r->addr_hi << 32) | (uint64_t)r->addr_lo;

    {   /* word 2: the event. */
        const uint32_t ev = out[at + 2u];
        if ((ev & 0x3Fu) != 0x14u || ((ev >> 8) & 0xFu) != 5u) { r->why = N48_F828_EVENT; return r->why; }
    }
    {   /* word 3: where it writes, whether it interrupts, and what it writes. */
        const uint32_t c = out[at + 3u];
        if (((c >> 16) & 0x3u) != 0u) { r->why = N48_F828_DST_SEL;  return r->why; }
        if (((c >> 24) & 0x7u) != 0u) { r->why = N48_F828_INT_SEL;  return r->why; }
        if (((c >> 29) & 0x7u) != 1u) { r->why = N48_F828_DATA_SEL; return r->why; }
    }
    if ((r->va & ~0xFFFull) == N48_F828_LIVE_PAGE) { r->why = N48_F828_LIVE_PAGE_REFUSED; return r->why; }
    if ((r->va & ~0xFFFull) != N48_F828_DEAD_PAGE || (r->va & 3ull)) { r->why = N48_F828_NOT_DEAD; return r->why; }

    r->why = N48_F828_OK;
    return r->why;
}

/* =====================================================================================================================
 * build 0.0.508 (switch 71) — THE LAST-CANDIDATE RULE. PURE, host-tested (gfx_fence828_test.cpp section
 * 16 and the corpus over run10p/run10q); the kext calls it INSTEAD of n48_f828_find only while switch 71 is latched ON.
 *
 * WHY. In frame b (run10p F52 IB1) and family e (8768|12912) the final segment the kext searches holds TWO NOP-buried
 * RELEASE_MEM candidates to one VA with DATA rising one per pass; n48_f828_find refuses AMBIGUOUS. The FIRST precedes a
 * DISPATCH_DIRECT and the last draw (picking it would signal completion before the frame's work: the DANGEROUS direction);
 * the LAST follows all work, then only NOPs to the end.
 *
 * THE RULE. OK with the LAST candidate only when ALL of:
 *   (a) every candidate passes n48_f828_find's own per-candidate field checks - by CALLING n48_f828_find on the slice from
 *       that candidate to the next candidate (or the end), which holds exactly that one candidate, so the checks are the
 *       same code, not a copy. A slice of exactly 9 dwords (two adjacent candidates, or a candidate that is the slice's
 *       last packet) is refused ARG by that call: FAIL CLOSED, never read around;
 *   (b) all candidates target the same VA, and (c) their DATA_LO values strictly increase in stream order - either
 *       violated: N48_F828_AMBIGUOUS, exactly today's answer;
 *   (d) every packet after the chosen candidate, to the end of the slice, is a NOP (type-3 opcode 0x10 of any length - which
 *       includes the translator's NOP-wrapped pool records, D_TBL_NOP_HDR - the one-dword NOP, or type 2); anything else is
 *       N48_F828_TAIL_WORK.
 * ONE candidate: n48_f828_find's answer, plus (d) (: changes 0 frames in the corpus). NO candidate, a malformed walk or
 * a short slice: exactly n48_f828_find's answer. `r` is always filled; on OK it describes the LAST candidate exactly as
 * n48_f828_find would describe a lone one (indices into `out`), with `candidates` the total seen, so n48_f828_apply
 * re-proves and rewrites that one and no other. Reads `out[0..n)` and writes NOTHING there. No local record: the per-slice
 * calls fill `r` itself (the kext's stack budget). */
static inline uint32_t n48_f828_tail_nop(const uint32_t *out, uint32_t from, uint32_t n)
{
    uint32_t i;
    for (i = from; i < n; ) {
        const uint32_t h = out[i], len = n48_f828_pkt_len(h);
        if (!len || i + len > n) return 0u;
        if (!(h == N48_F828_NOP1 || (h >> 30) == 2u || ((h >> 30) == 3u && ((h >> 8) & 0xFFu) == 0x10u))) return 0u;
        i += len;
    }
    return i == n ? 1u : 0u;
}

static inline uint32_t n48_f828_find_last(const uint32_t *out, uint32_t n, n48_f828 *r)
{
    uint32_t i, cur = 0u, have = 0u, seen = 0u, total, prev_data = 0u;
    uint64_t va0 = 0ull;
    const uint32_t why = n48_f828_find(out, n, r);
    if (why == N48_F828_ARG || why == N48_F828_WALK || why == N48_F828_NO_NOP) return why;   /* exactly today's answer */
    total = r->candidates;
    if (total == 1u) {
        if (why != N48_F828_OK) return why;                                /* today's field refusal, unchanged */
        if (!n48_f828_tail_nop(out, r->next_at, n)) { r->why = N48_F828_TAIL_WORK; return r->why; }
        return why;                                                        /* (d) holds: today's OK */
    }
    /* total >= 2: n48_f828_find answered AMBIGUOUS over a walk it proved well formed and ending exactly at n. */
    for (i = 0u; i <= n; ) {
        const uint32_t at_end = (i == n) ? 1u : 0u;
        const uint32_t is_cand = (!at_end && out[i] == N48_F828_NOP9 && out[i + 1u] == N48_F828_RELMEM) ? 1u : 0u;
        if ((at_end || is_cand) && have) {
            /* (a): the candidate at `cur`, alone in [cur, i), through n48_f828_find's own checks. */
            const uint32_t w = n48_f828_find(out + cur, i - cur, r);
            if (w != N48_F828_OK || r->candidates != 1u) {
                r->why = (w == N48_F828_OK) ? N48_F828_AMBIGUOUS : w; r->candidates = total; return r->why;
            }
            /* (b) one VA, (c) DATA strictly rising in stream order. */
            if (seen && (r->va != va0 || r->old_data <= prev_data)) {
                r->why = N48_F828_AMBIGUOUS; r->candidates = total; return r->why;
            }
            if (!seen) va0 = r->va;
            prev_data = r->old_data; seen++;
        }
        if (at_end) break;
        if (is_cand) { have = 1u; cur = i; }
        i += n48_f828_pkt_len(out[i]);   /* > 0 and in range: the walk above proved it */
    }
    if (seen != total) { r->why = N48_F828_AMBIGUOUS; r->candidates = total; return r->why; }
    /* `r` describes the LAST candidate relative to `cur`; make its indices absolute. */
    r->nop_at += cur; r->rel_at += cur; r->data_at += cur; r->next_at += cur;
    r->candidates = total;
    if (!n48_f828_tail_nop(out, r->next_at, n)) { r->why = N48_F828_TAIL_WORK; return r->why; }   /* (d) */
    r->why = N48_F828_OK;
    return r->why;
}

/* build 0.0.508 — THE fence71 REPORT LINE (one line per `gfxneuter 71` verb, bounded under the logger's 512 bytes by
 * gfx_fence828_test.cpp section 16 at 20-digit counters). `71 | M << 8`: M 1 ON (= 327), M 2 OFF (= 583, the default and the
 * boot value), bare `71` reads. args: "ON (last-candidate)" / "OFF (default)", how; then, at the armed fence site (switch 16)
 * while 71 was latched ON: OK, of those OK over several candidates (the last picked), TAIL-WORK, AMBIGUOUS kept, any other
 * refusal; then the same four-plus-one at the final-segment census site. */
#define N48_FENCE71_FMT "fence71: switch 71 %s%s. fence site: last-rule OK %llu (last of several %llu), TAIL-WORK %llu, " \
                        "AMBIGUOUS kept %llu, other %llu; census site: OK %llu (last of several %llu), TAIL-WORK %llu, " \
                        "AMBIGUOUS kept %llu, other %llu."

/* THE WRITE, AND IT RE-PROVES EVERY DWORD IT IS ABOUT TO TOUCH OR TO RELY ON. 0.0.413: it RE-POINTS the packet at
 * `slot_va` (a slot in OUR fence page) and puts `value` (epoch|ordinal) in DATA_LO. FOUR dwords change — the NOP header,
 * the two ADDRESS words, and DATA_LO. Returns N48_F828_OK on success, else the named refusal; a refusal writes NOTHING,
 * not even one dword. The two address words are re-proven as their OWN step so that "an address word moved under us"
 * has its own reason (N48_F828_ADDR_UNPROVEN) rather than hiding among the other re-reads. `slot_va` must lie in the
 * caller's fence page; n48_f828_owned_gate() proves that BEFORE the call, and this re-proves it too. */
static inline uint32_t n48_f828_apply(uint32_t *out, uint32_t n, const n48_f828 *r,
                                      uint64_t slot_va, uint64_t fence_page_va, uint32_t value)
{
    if (!out || !r) return N48_F828_ARG;
    if (r->why != N48_F828_OK) return r->why;
    if (r->nop_at + 9u > n) return N48_F828_ARG;
    if (r->rel_at != r->nop_at + 1u || r->data_at != r->nop_at + 6u || r->next_at != r->nop_at + 9u)
        return N48_F828_ARG;
    if ((slot_va & ~0xFFFull) != fence_page_va) return N48_F828_SLOT_VA;
    if (out[r->nop_at]  != r->nop_hdr || r->nop_hdr != N48_F828_NOP9) return N48_F828_NOT_MATCHED;
    if (out[r->rel_at]  != N48_F828_RELMEM) return N48_F828_NOT_MATCHED;
    if (out[r->data_at] != r->old_data) return N48_F828_NOT_MATCHED;
    /* THE ADDRESS WORDS, RE-PROVEN SEPARATELY (0.0.413). */
    if (out[r->rel_at + 3u] != r->addr_lo || out[r->rel_at + 4u] != r->addr_hi) return N48_F828_ADDR_UNPROVEN;
    if ((r->va & ~0xFFFull) != N48_F828_DEAD_PAGE) return N48_F828_NOT_DEAD;
    out[r->nop_at]      = N48_F828_NOP1;
    out[r->rel_at + 3u] = (uint32_t)(slot_va & 0xFFFFFFFFull);
    out[r->rel_at + 4u] = (uint32_t)(slot_va >> 32);
    out[r->data_at]     = value;
    return N48_F828_OK;
}

/* =====================================================================================================================
 * 0.0.413 — THE OWNED-SLOT VALUE AND GATE (pure; host-tested).
 * The value is 32 bits: the nonzero per-boot epoch in the high 16 and the commit ordinal in the low 16 (B3). The slot is
 * the ordinal mod 1024, four bytes each. A value left by ANOTHER boot, ANOTHER ordinal, or equal to the value read
 * BEFORE the commit never counts as end-of-pipe; the pre-read is the positive control (memo).
 * ===================================================================================================================== */
static inline uint32_t n48_f828_slot(uint32_t ordinal) { return ordinal % N48_F828_SLOTS; }

static inline uint64_t n48_f828_slot_va(uint64_t fence_page_va, uint32_t ordinal)
{
    return fence_page_va + (uint64_t)n48_f828_slot(ordinal) * (uint64_t)N48_F828_SLOT_BYTES;
}

static inline uint32_t n48_f828_value(uint32_t epoch, uint32_t ordinal)
{
    return ((epoch & 0xFFFFu) << 16) | (ordinal & 0xFFFFu);
}

/* A value proves THIS commit only when the epoch is stampable AND the whole 32 bits equal ours. A value from another
 * boot (different epoch) or another ordinal (different value) never matches, even at the same slot. */
static inline uint32_t n48_f828_value_matches(uint32_t v, uint32_t epoch, uint32_t ordinal)
{
    if (epoch == 0u) return 0u;
    return v == n48_f828_value(epoch, ordinal) ? 1u : 0u;
}

/* =====================================================================================================================
 * 0.0.415 — THE CANDIDATE RECORD AND THE COMMITTED RECORD ARE SEPARATE (Q4,).
 *
 * 0.0.413 moved the WATCH RESET to the promotion site but left the translate-time CANDIDATE site writing the committed
 * frame's want/epoch/ordinal/slot/pre AND clearing `committed` in place. `ks_eop_seen()` is `committed && ever` and the
 * poll is gated on `committed`, so a candidate the COMMIT gate (or the keystone) then refused WIPED an earlier committed
 * frame's watch and poll target and turned its end-of-pipe window into a timeout. The fix is structural: the candidate's
 * numbers live HERE, in their own record, and only a SUCCESSFUL commit promotion copies them into the committed frame's
 * record. Nothing at the candidate site can reach the committed record or its latch.
 * ===================================================================================================================== */
typedef struct {
    uint32_t want;        /* n48_f828_value(epoch, ordinal) — the value only this commit writes */
    uint32_t epoch;       /* the per-boot epoch stamped into `want` */
    uint32_t ordinal;     /* the per-boot commit ordinal stamped into `want` */
    uint32_t slot;        /* n48_f828_slot(ordinal) */
    uint32_t pre;         /* the slot's value read BEFORE the commit — the positive control */
    uint32_t pre_ok;      /* 1 when that read succeeded: an unreadable pre-read refuses (Q3) */
    uint32_t old_data;    /* Apple's own DATA_LO, as read */
    uint64_t va;          /* Apple's dead-page VA, as read (informational) */
    uint64_t slot_va;     /* the owned slot VA the packet is re-pointed at */
    uint64_t vram_off;    /* the same slot's VRAM offset — how every poll reads it */
    uint64_t arm_va_base; /* the ring region identity at placement (REGION-MOVED is asked against THIS) */
    uint64_t arm_carve;
    /* 0.0.418 (notes/design/BUILD-0.0.418.md, E2): THE FLIGHT THIS COMMITTED FENCE BELONGS TO, recorded at promotion
     * from the then-current `gKsFlight.seq`. `ks_eop_seen()` requires it to equal the CURRENT flight's, so a committed
     * frame's sticky `ever` latch can never answer end-of-pipe for a LATER flight whose own fence was refused (the
     * candidate gate dropped it, the keystone withdrew it, or REGION-MOVED neutered it) and therefore never reset the
     * latch. One field, no other state: the value the deferral reads and the value promotion writes. */
    uint32_t flight;      /* `gKsFlight.seq` at the instant this commit was promoted; 0 never matches a live flight */
} n48_f828_owned;

/* Fill a candidate record from the translate-time site. This is the ONLY write the candidate site makes: it takes no
 * committed record and no watch, so it cannot clear `committed` or overwrite the committed frame's target. */
/* =====================================================================================================================
 * 0.0.444 (notes/design/C5-RING-REVIEW.md (B) item 1) — THE FLIGHT-RING FENCE HANDOFF, PURE.
 *
 * gfxsrc_commit_try's gate block clears `gXdF828Pending` (its own per-frame "have I got a fence candidate to judge"
 * flag) several lines BEFORE the flight-ring push runs, later in the SAME call (gfx_flightring.h's n48_fr_push).
 * The 0.0.443 push read `gXdF828Pending ? gXdF828Cand.* : 0` at that later point — which, because the flag had
 * already been cleared for THIS frame by the time the push ran, always read 0: the review's HEADLINE, every entry
 * pushed with `want = 0`, so nothing ever retired by fence.
 *
 * The durable, per-frame proof that THIS commit's fence candidate is the one the gate answered OK for is
 * `gXdF828GateOk && gXdF828GateSeq == seq` — both written in the SAME gate block, a few lines above the clear, and
 * not touched again before the push. The `seq` comparison is not decorative: `gXdF828GateOk` is a plain global that
 * is reset to 0 inside the gate block on EVERY frame that offered a fence candidate, but a frame that offers NO
 * candidate at all (the switch off, or Apple's buried RELEASE_MEM not found) skips that block entirely and leaves
 * whatever the LAST candidate frame wrote standing — so a stale `gXdF828GateOk == 1` from several frames ago could
 * otherwise be misread as belonging to the CURRENT commit. Every real commit has a distinct, non-zero token seq
 * (`gXdCmSeq` pre-increments), so comparing `gXdF828GateSeq == seq` is what tells "this frame's own GATE OK" apart
 * from "a stale flag from an earlier frame that never offered a candidate at all".
 *
 * Returns 1 (this commit owns a fence) or 0 (fence-less: the switch is off, no candidate was offered, the gate
 * refused it, or the stale-flag case above). On 0 every output is written 0/0/0, matching gfx_flightring.h's own
 * "ordinal/vram_off/want all 0" meaning for a fence-less push (n48_fr_poll_entry never retires a `want == 0`
 * entry). Host-tested in gfx_fence828_test.cppa, driven through the REAL call order (gate block, then this
 * handoff) so the 0.0.443 defect — reading `gXdF828Pending` AFTER the clear — is a planted break that must FAIL. */
static inline uint32_t n48_f828_ring_handoff(uint32_t gateOk, uint32_t gateSeq, uint32_t seq,
                                             uint32_t candOrdinal, uint64_t candVramOff, uint32_t candWant,
                                             uint32_t *outOrdinal, uint64_t *outVramOff, uint32_t *outWant)
{
    const uint32_t has = (gateOk && seq && gateSeq == seq) ? 1u : 0u;
    if (outOrdinal) *outOrdinal = has ? candOrdinal : 0u;
    if (outVramOff) *outVramOff = has ? candVramOff : 0ull;
    if (outWant)    *outWant    = has ? candWant    : 0u;
    return has;
}

static inline void n48_f828_owned_place(n48_f828_owned *o, uint32_t want, uint32_t epoch, uint32_t ordinal,
                                        uint64_t slot_va, uint64_t vram_off, uint64_t va, uint32_t pre,
                                        uint32_t pre_ok, uint32_t old_data, uint64_t arm_va_base,
                                        uint64_t arm_carve)
{
    if (!o) return;
    o->want = want; o->epoch = epoch; o->ordinal = ordinal; o->slot = n48_f828_slot(ordinal);
    o->pre = pre; o->pre_ok = pre_ok; o->old_data = old_data;
    o->va = va; o->slot_va = slot_va; o->vram_off = vram_off;
    o->arm_va_base = arm_va_base; o->arm_carve = arm_carve;
}

/* THE OWNED-SLOT GATE, pure and BEFORE the rewrite. Every refusal is a named reason. `root_armed` is the committing
 * context's root[511] state at the instant of the decision; `pre` is the slot's value read before the commit (the
 * positive control) and `pre_ok` says whether that read succeeded at all; `want` is n48_f828_value(epoch, ordinal);
 * `multi_segment` is 1 when THIS frame has more than one segment (0.0.415, Q1). Returns N48_F828_OK when the rewrite may
 * proceed. 0.0.415: an UNREADABLE pre-read is its own refusal (Q3) — an unreadable slot read as 0 is not a control at
 * all — and a multi-segment frame is refused before the packet would claim end of pipe at the end of this segment. */
static inline uint32_t n48_f828_owned_gate(uint32_t ring_built, uint64_t ring_va_base, uint32_t root_armed,
                                           uint32_t epoch, uint64_t fence_page_va, uint64_t slot_va,
                                           uint32_t pre, uint32_t pre_ok, uint32_t want, uint32_t multi_segment)
{
    if (!ring_built || ring_va_base == 0ull) return N48_F828_NO_RING;
    if (!root_armed)                         return N48_F828_NO_ROOT;
    if (epoch == 0u)                         return N48_F828_EPOCH;
    if (multi_segment)                       return N48_F828_MULTI_SEGMENT;
    if ((slot_va & ~0xFFFull) != fence_page_va) return N48_F828_SLOT_VA;
    if (!pre_ok)                             return N48_F828_PRE_UNREADABLE;
    if (pre == want)                         return N48_F828_SLOT_PRE;
    return N48_F828_OK;
}

/* =====================================================================================================================
 * 0.0.418 (notes/design/BUILD-0.0.418.md, E3) — REGION-MOVED MUST NEUTER THE FRAME, NOT MERELY DROP THE CANDIDATE.
 *
 * The gate at translate time RE-POINTS Apple's buried RELEASE_MEM at a slot in our fence page and arms a candidate.
 * If the ring region is rebuilt or moved before the COMMIT gate runs, the slot VA names different bytes, so the
 * candidate is refused (REGION-MOVED). Through 0.0.417 that branch dropped the CANDIDATE but left the frame's verdict
 * at COMMIT: the frame still ran, the un-NOPed packet still executed, and because no promotion advanced the committed
 * record's `ordinal`, it wrote `want` into the SAME slot the NEXT candidate would compute. That candidate reads the
 * slot as its `pre`, finds `pre == want`, and is refused SLOT-PRE - and so is every candidate after it, for the rest of
 * the boot. NOTHING writes it back: the slot is re-pointed by that one executed packet and no later promotion runs.
 *
 * The answer is the one every other gate refusal takes: THE FRAME IS NEUTERED AT SOURCE, so the CP never runs the
 * packet and the slot is never written. This function is the frame-level verdict: N48_F828_OK when the region did not
 * move (the positive control), N48_F828_REGION_MOVED when it did. It is NEVER N48_F828_OK for a moved region, and the
 * caller folds that into its own gate refusal. Host-tested in gfx_fence828_test.cpp section 14, whose planted break is
 * the 0.0.417 behaviour (returns OK, the frame runs, the next candidate is pinned SLOT-PRE). */
static inline uint32_t n48_f828_region_moved_reason(uint32_t region_moved)
{
    return region_moved ? N48_F828_REGION_MOVED : N48_F828_OK;
}

/* The property the rewrite must preserve, for the tests and for anybody reading this later: the stream still walks
 * end to end and still ends exactly at n, and it now holds exactly one more packet than it did. */
static inline uint32_t n48_f828_walk_ok(const uint32_t *out, uint32_t n, uint32_t *packets)
{
    uint32_t i, p = 0u;
    if (packets) *packets = 0u;
    if (!out || !n) return 0u;
    for (i = 0u; i < n; ) {
        const uint32_t len = n48_f828_pkt_len(out[i]);
        if (!len || i + len > n) return 0u;
        p++; i += len;
    }
    if (packets) *packets = p;
    return i == n ? 1u : 0u;
}

// ---------------------------------------------------------------------------------------------------------------------
// THE READ-BACK LATCH — 0.0.378. WHY THIS IS A SEPARATE, PURE, TESTED THING.
// ---------------------------------------------------------------------------------------------------------------------
// `arm8` PROVED the committed frame reached end of pipe: polls 1..6 of 8 each read N48_F828_VALUE back out of the slot.
// Polls 7 and 8 then read 0 — the slot reverted, SUSPECTED because Apple re-uses that page. And the VERDICT line printed
// "the slot is UNCHANGED … last read 0 after 8 poll(s) of 8" — THE OPPOSITE OF THE TRUTH — because it branched on the
// LAST read alone. That is the FOURTH instrument on this project to report the inverse of what it measured (rule 103,
// "a watcher's silence is not the machine's"), and the cause is always the same shape: a summary computed from the most
// recent sample of a sequence whose QUESTION is existential ("did it EVER hold our value?").
//
// So the question is answered by a LATCH, not by a sample, and the latch is pure C here rather than three assignments in
// the kext, so that "a mid-sequence match is reported as unchanged" is a planted defect a host test catches (M10) rather
// than a code review. The last read and the poll count are KEPT and still printed: the reversion is real and is itself
// evidence about the page, and discarding it would trade one lie for another.
//
// AN EXISTENTIAL LATCH IS NOT SYMMETRIC, AND THAT IS DELIBERATE. `ever` proves a positive — the GPU wrote a value only
// we write, so our frame retired. Nothing a later poll reads can un-prove it: a 0 afterwards means the slot moved on,
// not that the fence never fired. Hence n48_f828_state tests `ever` BEFORE it tests readability (M12): a poll that
// cannot read the slot proves nothing, and "proves nothing" must never overrule "proved something".
typedef struct {
    uint32_t polls;       /* polls fed, readable or not */
    uint32_t reads_ok;    /* of those, how many actually read the slot */
    uint32_t last;        /* the value the LAST poll read (0 if it could not read) */
    uint32_t last_ok;     /* 1 when the LAST poll read at all */
    uint32_t ever;        /* 1 once ANY readable poll equalled `want`. STICKY: never cleared (M11) */
    uint32_t first_poll;  /* 1-based index of the poll that FIRST matched; 0 = never matched */
    uint32_t matches;     /* how many polls matched in total */
    uint32_t reverted;    /* 1 when a readable poll AFTER the first match did NOT equal `want` */
    uint32_t revert_poll; /* 1-based index of the first such poll; 0 = never reverted */
} n48_f828_watch;

/* The states the latch distinguishes. Every one of them is a different fact about the machine, and no two of them can
 * be true at once: n48_f828_state is a ladder, not a set of flags. */
enum {
    N48_F828_ST_NEVER_ENABLED = 0, /* the switch was never thrown, or no segment ever passed the rule */
    N48_F828_ST_NOT_COMMITTED,     /* a fence was placed in a CANDIDATE and the COMMIT gate refused the frame */
    N48_F828_ST_NOT_POLLED,        /* committed, but no later frame in that context ever re-read the slot */
    N48_F828_ST_MATCHED,           /* ENABLED AND EVER EQUALLED OURS, and it still did on the last readable poll */
    N48_F828_ST_MATCHED_REVERTED,  /* ENABLED AND EVER EQUALLED OURS, and a later poll read something else */
    N48_F828_ST_UNREADABLE,        /* committed and polled, and NOT ONE poll could read the slot */
    N48_F828_ST_NEVER_MATCHED,     /* committed, polled, readable — and the slot never once held our value */
    N48_F828_ST_COUNT
};

static inline const char *n48_f828_state_name(uint32_t st)
{
    switch (st) {
    case N48_F828_ST_NEVER_ENABLED:    return "NO FENCE WAS EVER ENABLED";
    case N48_F828_ST_NOT_COMMITTED:    return "a fence was placed in a candidate but NO frame committed it";
    case N48_F828_ST_NOT_POLLED:       return "COMMITTED, but the slot was never re-read (no later frame in that context)";
    case N48_F828_ST_MATCHED:          return "OURS - THE COMMITTED FRAME REACHED END OF PIPE. Attribution is solved";
    case N48_F828_ST_MATCHED_REVERTED: return "OURS - THE COMMITTED FRAME REACHED END OF PIPE (and the slot REVERTED after)";
    case N48_F828_ST_UNREADABLE:       return "COMMITTED, and NOT ONE poll could read the slot - this run proves nothing";
    case N48_F828_ST_NEVER_MATCHED:    return "COMMITTED and polled, and the slot NEVER held our value";
    default:                           return "?";
    }
}

static inline void n48_f828_watch_reset(n48_f828_watch *w)
{
    if (!w) return;
    w->polls = 0u; w->reads_ok = 0u; w->last = 0u; w->last_ok = 0u;
    w->ever = 0u; w->first_poll = 0u; w->matches = 0u; w->reverted = 0u; w->revert_poll = 0u;
}

/* 0.0.415 (Q4,) — THE COMMIT PROMOTION, AND THE WATCH'S ONLY RESET. The reset is NOT at the gate: the gate runs
 * BEFORE the keystone, and the keystone can still withdraw the frame, so promoting there would reset the latch (and, in
 * 0.0.413, overwrite the record) of a committed frame still in flight. This is called at the one instant the commit is
 * irrevocable — where the keystone has PROVED the frame will run, the same instant commits a reserved fill. A
 * candidate the gate refused never reaches this call; a frame the keystone refused passes permitted = 0 and changes
 * NOTHING: not `committed`, not the committed record, and not the latch. `committed` is the committed frame's own
 * record; `cand` is the candidate's, and they are different objects on purpose (see n48_f828_owned above).
 *
 * 0.0.418 (E2) — AND IT RECORDS THE FLIGHT. `flight` is the then-current `gKsFlight.seq`; the committed record carries
 * it so `n48_f828_eop_seen` can refuse to answer for a later flight. It is set HERE and only here, alongside the copy,
 * because this is the one instant the committed record is written.
 *
 * 0.0.420 ('s E2 follow-up) — THE WATCH RESET COMES FIRST, AND THE ORDER IS THE CONTRACT. Through 0.0.419 the copy
 * and the `flight` stamp were written BEFORE `n48_f828_watch_reset`, so for a few instructions another thread reading
 * `n48_f828_eop_seen(1, watch, record->flight, cur_flight)` could see the NEW flight on the record and the OLD, still-1
 * sticky `ever` in the watch: a false end-of-pipe for a flight whose fence had not retired. Resetting FIRST closes it -
 * a reader that sees the new flight necessarily sees `ever` already 0, and a reader between the reset and the stamp sees
 * the OLD flight and answers 0 (FAIL CLOSED). Only the order of two writes moved; the post-condition is identical (a
 * permitted promotion copies the candidate, stamps the flight and leaves a zeroed watch). */
static inline void n48_f828_commit_promote(n48_f828_owned *committed, const n48_f828_owned *cand,
                                           n48_f828_watch *watch, uint32_t permitted, uint32_t flight)
{
    if (!permitted) return;
    n48_f828_watch_reset(watch);
    if (committed && cand) { *committed = *cand; committed->flight = flight; }
}

/* Feed ONE poll. `got` is whether the read succeeded, `val` what it read, `want` the value only we write.
 * An unreadable poll is counted and recorded and decides NOTHING: it can neither set `ever` nor set `reverted`. */
static inline void n48_f828_watch_poll(n48_f828_watch *w, uint32_t got, uint32_t val, uint32_t want)
{
    if (!w) return;
    w->polls++;
    w->last = got ? val : 0u;
    w->last_ok = got ? 1u : 0u;
    if (!got) return;
    w->reads_ok++;
    if (val == want) {
        w->matches++;
        if (!w->ever) { w->ever = 1u; w->first_poll = w->polls; }   /* STICKY, and the FIRST index is kept */
    } else if (w->ever && !w->reverted) {
        w->reverted = 1u; w->revert_poll = w->polls;
    }
}

/* THE VERDICT, as a ladder. `enabled` is how many segments had a fence enabled; `committed` whether the COMMIT gate
 * took one of those frames. Returns one N48_F828_ST_*. */
static inline uint32_t n48_f828_state(uint64_t enabled, uint32_t committed, const n48_f828_watch *w)
{
    if (!enabled)             return N48_F828_ST_NEVER_ENABLED;
    if (!committed)           return N48_F828_ST_NOT_COMMITTED;
    if (!w || !w->polls)      return N48_F828_ST_NOT_POLLED;
    /* BEFORE readability, on purpose: a proof cannot be withdrawn by a later poll that proved nothing. */
    if (w->ever)              return w->reverted ? N48_F828_ST_MATCHED_REVERTED : N48_F828_ST_MATCHED;
    if (!w->reads_ok)         return N48_F828_ST_UNREADABLE;
    return N48_F828_ST_NEVER_MATCHED;
}

/* =====================================================================================================================
 * 0.0.418 (notes/design/BUILD-0.0.418.md, E2) — END-OF-PIPE IS ANSWERED ONLY FOR THE FLIGHT IT BELONGS TO.
 *
 * `ks_eop_seen()` through 0.0.417 was `committed && watch.ever`. `ever` is STICKY (M11) and belongs to the last
 * PROMOTED commit, while `gKsFlight.seq` advances the instant a new commit spends its shot. When that new flight's
 * fence is REFUSED - the candidate gate drops it, the keystone withdraws it, or REGION-MOVED neuters it - no promotion
 * runs, so the committed record and its latch are left alone (Q4's whole point) and the old `ever` is still 1. The
 * deferral then reads `ks_eop_seen()` as 1 for the NEW flight and ends its window immediately, on a proof that belongs
 * to a frame that already retired.
 *
 * The fix is one field and one comparison: the committed record carries the `flight` it was promoted on, and the
 * question is only answered when that flight is the CURRENT one. A refused candidate never reaches the promotion, so
 * the record keeps the older flight and the answer is 0 - FAIL CLOSED, and the deferral ends by its bounded timeout
 * exactly as it does on a boot where the fence switch was never thrown. Host-tested in section 13; the planted break is
 * the 0.0.417 rule (`committed && ever`, the flight ignored). */
static inline uint32_t n48_f828_eop_seen(uint32_t committed, const n48_f828_watch *w,
                                         uint32_t rec_flight, uint32_t cur_flight)
{
    if (!committed || !w || !w->ever) return 0u;
    return rec_flight == cur_flight ? 1u : 0u;
}

#ifdef __cplusplus
}
#endif
#endif /* N48_GFX_FENCE828_H */
