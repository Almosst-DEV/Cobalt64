/* xlat12_scissor.h - build 0.0.499: THE SCISSOR BOTTOM-RIGHT RULE, gfx10.3 -> gfx12.
 *
 * gfx10.3 scissor BOTTOM-RIGHT corners are EXCLUSIVE (the first column/row NOT drawn); gfx12's are INCLUSIVE (the last
 * column/row drawn). Our translator used to copy gfx10's value unchanged (repack_PA_SC_*_SCISSOR_BR bit copies; the
 * screen scissor a plain MOVED row), so every translated draw could cover ONE EXTRA column and row. In RUN I (run10o F49,
 * IB0 dword 11219) a draw into a 64x64 target ending exactly at WindowServer's sampler heap wrote column x = 64 into it
 *. This is a memory-safety fix, not a new switch.
 *
 * Mesa, re/graphics/src/mesa (the gfx12 branches, quoted by content):
 *   PA_SC_VPORT_SCISSOR_n_BR  si_state_viewport.c si_emit_one_scissor: `S_028254_BR_X(final.maxx - 1)` under
 *                             `ctx->gfx_level >= GFX12`, and the gfx6-11 branch writes `S_028254_BR_X(final.maxx)`;
 *                             radv_cmd_buffer.c writes `S_028254_BR_X(maxx - 1)` on GFX12. CONFIRMED inclusive.
 *   PA_SC_WINDOW_SCISSOR_BR   si_state.c gfx12 framebuffer: `S_028208_BR_X(state->width - 1) |    /+ inclusive +/` (gfx6-11:
 *                             `S_028208_BR_X(state->width)`); radv_cmd_buffer.c GFX12: `S_028208_BR_X(maxx - 1) | ...
 *                             /+ inclusive +/`. CONFIRMED inclusive.
 *   PA_SC_GENERIC_SCISSOR_BR  ac_cmdbuf.c gfx12 preamble: `S_028244_BR_X(65535) | S_028244_BR_Y(65535)); /+ inclusive
 *                             bounds +/` (gfx6-11 preamble: BR_X(16384), the exclusive full range). CONFIRMED inclusive.
 *   PA_SC_SCREEN_SCISSOR_BR   ac_cmdbuf.c gfx12 preamble: `S_028184_BR_X(65535) | S_028184_BR_Y(65535)); /+ inclusive
 *                             bounds +/` - but radv's gfx12 render-begin writes `S_028184_BR_X(screen_scissor.width)` with
 *                             NO -1. AMBIGUOUS in mesa (SUSPECTED inclusive: the preamble says so, the register type is
 *                             the same PA_SC_VPORT_0_BR as the other three in gfx12.json, and radv's window scissor -1
 *                             clips the same extra column anyway). Taken INCLUSIVE here, the FAIL-CLOSED direction: -1
 *                             can only shrink coverage by one pixel, never grow it.
 *   PA_SC_CLIPRECT_n_BR       NOT touched: si_state_viewport.c si_emit_window_rectangles writes `S_028214_BR_X(rects[i].maxx)`
 *                             unchanged on gfx12 AND gfx6-11 ("corner coordinates are inclusive" on every generation), and
 *                             the translator has no row for 0x28210-0x2822c at all (a write refuses as untabled).
 *
 * EMPTY SCISSORS. mesa's gfx12 empty scissor is TL (1,1) BR (0,0) - "An empty scissor must be done like this because the
 * bottom-right bounds are inclusive" (si_state_viewport.c; radv the same). That is, gfx12 draws nothing when BR < TL. A
 * gfx10 empty scissor with BR == TL > 0 therefore maps, through the -1 alone, to BR = TL - 1 < TL: still empty. A BR
 * component of 0 cannot be made inclusive without also rewriting its TL (mesa's encoding moves TL to 1) - and TL is a
 * DIFFERENT register write this per-register mapping does not see - so it is REFUSED (XLAT12_ERR_SCISSOR), never
 * admitted: passing 0 through would draw column/row 0 of a scissor gfx10 meant to be empty. A census of every IB in
 * run10o/run10l/run10j found no BR component of 0 on any of these registers (the refusal costs no measured traffic).
 *
 * Every field: the four gfx12 BR registers have exactly two fields, BR_X [15:0] and BR_Y [31:16] (gfx12.json type
 * PA_SC_VPORT_0_BR), so there is no other field to keep. The input is the value the table ALREADY produced for gfx12
 * (repack for window/generic/viewport - gfx10's 15-bit fields widened to 16 - or the unchanged MOVED value for the screen
 * scissor, whose gfx10 fields are already [15:0]/[31:16]); only BR_X and BR_Y change, each by exactly -1.
 *
 * Pure and header-only (static inline) so xlat12.c's generic path and xlat12_ib.c's draw policy share ONE rule without a
 * new exported symbol (the kextcheck's undefined-symbol allow-list is unchanged). No state, no register or memory write. */
#ifndef XLAT12_SCISSOR_H
#define XLAT12_SCISSOR_H

#include <stdint.h>

enum {
    XLAT12_SCISSOR_NOT_BR   = 0,   /* not a scissor BR register: *out12 = the input, unchanged */
    XLAT12_SCISSOR_ADJUSTED = 1,   /* BR_X -= 1 and BR_Y -= 1 */
    XLAT12_SCISSOR_EMPTY    = 2    /* a BR component is 0: the caller REFUSES (XLAT12_ERR_SCISSOR) */
};

/* The gfx10.3 addresses whose gfx12 destination is an INCLUSIVE bottom-right corner: PA_SC_SCREEN_SCISSOR_BR 0x28034
 * (-> gfx12 0x28184), PA_SC_WINDOW_SCISSOR_BR 0x28208, PA_SC_GENERIC_SCISSOR_BR 0x28244, PA_SC_VPORT_SCISSOR_0..15_BR
 * 0x28254 + 8n (n = 0..15, last 0x282cc). */
static inline int xlat12_scissor_is_br_g10(uint32_t g10)
{
    if (g10 == 0x28034u || g10 == 0x28208u || g10 == 0x28244u) return 1;
    return g10 >= 0x28254u && g10 <= 0x282ccu && ((g10 - 0x28254u) & 7u) == 0u;
}

/* g10 = the gfx10.3 register address; v12 = the gfx12 value the table already produced for it. */
static inline uint32_t xlat12_scissor_br_g12(uint32_t g10, uint32_t v12, uint32_t *out12)
{
    *out12 = v12;
    if (!xlat12_scissor_is_br_g10(g10)) return XLAT12_SCISSOR_NOT_BR;
    const uint32_t x = v12 & 0xFFFFu, y = v12 >> 16;
    if (x == 0u || y == 0u) return XLAT12_SCISSOR_EMPTY;
    *out12 = ((y - 1u) << 16) | (x - 1u);
    return XLAT12_SCISSOR_ADJUSTED;
}

#endif /* XLAT12_SCISSOR_H */
