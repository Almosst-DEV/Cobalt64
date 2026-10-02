/* xlat12_headless.h - a recogniser for Apple's HEADLESS render passes.
 *
 * IN THE KEXT SINCE 0.0.369, BEHIND A SWITCH THAT IS OFF BY DEFAULT (notes 804). Through 0.0.368 this file was offline only.
 * The reviewed decision that admitted it is the project notes, "SAFETY REVIEW - E1 / headless coverage / N1",
 * Decision 2. What it is held to there, and what must not be weakened on its account:
 *   - the kext compiles it (src/navi48-bringup/Makefile, $(BUILD)/xlat12_headless.o) and calls it from gfxsrc_policy ONLY
 *     when xlat12_ib_segments found NOTHING and ONLY while `gXdHeadless` is on. That switch is 0 at boot and is set by
 *     `accel gfxneuter 13 | 1 << 8` (2 << 8 turns it off, 13 alone reads it). Offline it is still tools/m4-xlat/replay.c's
 *     --headless, also off by default, and tests/test_headless.c.
 *   - it WIDENS what the translator accepts, and it judges SHAPE, NOT PROVENANCE (it accepts 12,521 of 20,000 shape-valid
 *     corruptions of a real setup half), so on its own it fails OPEN. What stands behind it is the COMMIT gate: a frame
 *     whose segments came from here must declare N48_CM_KIND_HEADLESS, which the gate grants only when this function
 *     answered XLAT12_HL_OK, with *total == nseg, on a buffer xlat12_ib_segments returned 0 for, and every segment's
 *     start == head. The gate then reads the IB back and compares it dword for dword against the translator's OWN output
 *     and re-establishes the frame's identity. Neither that read-back nor those identity checks may be relaxed because this
 *     recogniser accepted something, and this recogniser may not be relaxed because they exist.
 *
 * WHAT IT RECOGNISES. xlat12_ib_segments accepts a stream only when Apple's encoder head (EVENT_WRITE 0x16, ACQUIRE_MEM,
 * EVENT_WRITE 0xE) is at dword 0, and that is correct (: seg_start_at must NOT be relaxed). The compositor's "setup half"
 * submissions (1456 dwords) carry no such head anywhere: they are three complete render passes, each opened by
 * the proven CONTEXT_CONTROL c0012800 80000000 80000000, then one pass's full state block, a pipeline drain, exactly one
 * DRAW_INDEX_AUTO and an ACQUIRE_MEM trailer, and after the last pass one NOP. This function accepts a stream
 * ONLY when the WHOLE of it is such passes, end to end:
 *   H1  dword 0 is the proven CONTEXT_CONTROL, three dwords exactly;
 *   H2  no packet-aligned encoder head anywhere (a stream that has one belongs to xlat12_ib_segments, never to this);
 *   H3  the packet walk covers exactly n dwords (no truncated or runaway packet, nothing after the walk);
 *   H4  every packet is in the pass alphabet: CONTEXT_CONTROL (proven form only), SET_CONTEXT_REG, SET_SH_REG(_INDEX),
 *       SET_UCONFIG_REG(_INDEX), NUM_INSTANCES, WRITE_DATA, RELEASE_MEM, WAIT_REG_MEM, EVENT_WRITE, ACQUIRE_MEM,
 *       DRAW_INDEX_AUTO and NOP. Anything else - a nested IB, DMA_DATA, LOAD_*, CLEAR_STATE, COND_EXEC, SET_CONFIG_REG, an
 *       indexed or indirect draw, a dispatch - refuses;
 *   H5  each pass (from one proven CONTEXT_CONTROL to the next) contains EXACTLY ONE draw;
 *   H6  after its draw a pass carries only ACQUIRE_MEM and NOP: no register write may leak forward into the next submission;
 *   H7  each pass WRITES, before its draw, every register of the pass-completeness set below: the programs, their resources,
 *       the fragment inputs, colour target 0 and the raster/blend/depth words. A pass that would inherit any of them from
 *       whatever ran before it is not self-contained and refuses;
 *   H8  every pass ends in its own trailer: an ACQUIRE_MEM after its draw, optionally followed by NOPs only.
 * On success each pass is one xlat12_ib_segment with head == start == the CONTEXT_CONTROL dword (the draw policy translates
 * the proven CONTEXT_CONTROL itself), end == the next pass's CONTEXT_CONTROL (the last: n), draws == 1.
 *
 * WHAT IT DOES NOT DO, stated so nobody reads acceptance as more than it is:
 *   - It does not identify WHO submitted the stream. A foreign client that happens to write the same shape is accepted; the
 *     per-stage resolver (program identity), the interpolation guard and the rest of the ladder still stand behind it.
 *   - The completeness set is a LIST, not a proof: a pass can still depend on a register outside it ('s
 *     SPI_SHADER_IDX_FORMAT / VGT_GS_OUT_PRIM_TYPE are exactly that shape, which is why XLAT12_EXTRA_SYNTH_IDXPRIM exists).
 *   - Through 0.0.368 the COMMIT gate required start == head + 2 - the encoder head's two dwords - so a headless segment was
 *     refused there at `coverage`. 0.0.369 replaced that with a per-frame kind (gfx_commit.h, N48_CM_KIND_*): the start rule
 *     is now the kind's rule, a frame that declares no kind still refuses, and a MIXED frame refuses at N48_CM_SEG_KIND
 *     whichever kind it claims. The gate still fails closed; it now fails closed on a rule that can tell the two shapes apart.
 * The planted-defect suite (tests/test_headless.c) disables each of H1-H8 in turn and requires a negative control to catch it.
 */
#ifndef XLAT12_HEADLESS_H
#define XLAT12_HEADLESS_H
#include <stdint.h>
#include "xlat12_ib.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {
    XLAT12_HL_OK = 0,
    XLAT12_HL_ARG,          /* null pointer, n == 0, max == 0 */
    XLAT12_HL_NOT_CTXCTL,   /* H1: dword 0 is not c0012800 80000000 80000000, or a pass opens with another form */
    XLAT12_HL_HAS_HEAD,     /* H2: a packet-aligned encoder segment head exists - not a headless stream */
    XLAT12_HL_WALK,         /* H3: the packet walk does not cover exactly n dwords */
    XLAT12_HL_FOREIGN,      /* H4: an opcode outside the pass alphabet (or a non-proven CONTEXT_CONTROL mid-pass) */
    XLAT12_HL_DRAWS,        /* H5: a pass with no draw or with more than one */
    XLAT12_HL_TAIL,         /* H6: after a pass's draw, something other than ACQUIRE_MEM / NOP */
    XLAT12_HL_STATE,        /* H7: a pass does not write every register of the completeness set before its draw */
    XLAT12_HL_TRAILER,      /* H8: a pass does not end in its trailer (ACQUIRE_MEM after the draw) plus NOPs */
    XLAT12_HL_TOO_MANY,     /* more passes than the caller's table holds */
    XLAT12_HL_REASONS
};

/* Checks a planted-defect run may switch off (tests only; the public entry point passes 0). */
#define XLAT12_HL_SKIP_H1 0x01u
#define XLAT12_HL_SKIP_H2 0x02u
#define XLAT12_HL_SKIP_H3 0x04u
#define XLAT12_HL_SKIP_H4 0x08u
#define XLAT12_HL_SKIP_H5 0x10u
#define XLAT12_HL_SKIP_H6 0x20u
#define XLAT12_HL_SKIP_H7 0x40u
#define XLAT12_HL_SKIP_H8 0x80u

typedef struct {
    uint32_t why;        /* XLAT12_HL_* */
    uint32_t at;         /* dword the refusal is about */
    uint32_t detail;     /* opcode (H4/H6), gfx10.3 register byte address (H7), pass index (H5/H7), walk length (H3) */
    uint32_t passes;     /* passes recognised before the refusal, or in total */
} xlat12_hl_report;

/* The pass-completeness set (H7), gfx10.3 byte addresses. Every one is written by each of the three passes of wsgc1 F5. */
#define XLAT12_HL_REQUIRED 27u
extern const uint32_t kXlat12HlRequired[XLAT12_HL_REQUIRED];

const char *xlat12_hl_reason_name(uint32_t why);
/* Returns the number of passes (segments filled, at most max) or 0 with rep->why set. *total = passes found. */
uint32_t xlat12_ib_headless_passes(const uint32_t *in, uint32_t n, xlat12_ib_segment *seg, uint32_t max, uint32_t *total,
                                   xlat12_hl_report *rep);
/* The same with checks switched off - FOR THE PLANTED-DEFECT SUITE ONLY. */
uint32_t xlat12_ib_headless_passes_m(const uint32_t *in, uint32_t n, xlat12_ib_segment *seg, uint32_t max, uint32_t *total,
                                     xlat12_hl_report *rep, uint32_t skip);

#ifdef __cplusplus
}
#endif
#endif
