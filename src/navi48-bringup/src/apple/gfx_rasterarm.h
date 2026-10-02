// gfx_rasterarm.h — 0.0.389 (notes 880 H6 (e), 881): GAP 1, AND THE SWITCH THAT CLOSES IT.
// Pure C, host-tested by tests/gfx_rasterarm_test.cpp (with planted defects); the kext compiles the SAME header.
// NOTHING HERE READS OR WRITES HARDWARE: it is two pure functions over one flag.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHAT GAP 1 IS
// ---------------------------------------------------------------------------------------------------------------------
// (run r64, 0.0.227) MEASURED the 0.0.220 raster/CB delta as REQUIRED: with it and NO preamble delta all four
// tri-suite cases PASS and every readback is byte-identical to r62's; without it (r63,) the same draws execute
// and write the right colour in the WRONG PLACE - sheared fragments crowded into the top quarter. The clean control
// ran `flags 0x4` (raster 1, preamble 0) with `RSRC3_GS 0xfffffdfd`.
//
// The ARMED WindowServer path has never set it. `gfxsrc_policy` builds its `xlat12_draw_extra` with `ring_va`,
// `gs_sgpr0_va`, `pgm_ctx`, `pgm_profile` and (only under the descriptor switch) the three descriptor flags -
// `flags` is otherwise 0 and `rsrc3_gs` is 0. XLAT12_EXTRA_RASTER and XLAT12_EXTRA_PREAMBLE are set ONLY on the
// test-verb path (`hw_hook_render_xlat`'s `xflags`, scalar bits 0x400 / 0x1000). recorded this as "Gap 1".
//
// So every armed frame has run with PA_SC_VRS_OVERRIDE_CNTL, PA_SC_VRS_INFO and PA_SC_HISZ_RENDER_OVERRIDE holding
// whatever an earlier submission left in them, and with no SPI_SHADER_PGM_RSRC3_GS of ours at all.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHAT THE SWITCH DOES, AND WHAT IT DELIBERATELY DOES NOT
// ---------------------------------------------------------------------------------------------------------------------
// `accel gfxneuter 27 | M << 8`: M 1 ON (= 283), M 2 OFF, bare `27` reports asks/applied and writes nothing.
// OFF BY DEFAULT AND OFF AT BOOT, so an arm16 boot that never throws it is 0.0.388's armed path dword for dword.
//
// ON, the armed path's `ex` gains EXACTLY TWO fields:
//     flags    |= XLAT12_EXTRA_RASTER      (12 dwords in the extra block + 2 follow-on writes merged into the
//                                           stream's own CB packets)
//     rsrc3_gs  = XLAT12_RSRC3_GS_CU_EN    (0xfffffdfd, 3 dwords) - the value the clean controls used, taken from
//                                           the same constant `hw_hook_render_xlat` uses for scalar bit 0x200,
//                                           NOT invented here.
// It does NOT set XLAT12_EXTRA_PREAMBLE. measured the preamble delta as NOT needed for these draws, and
// 0.0.389's change A leaves too little pad to carry both (tests/test_xlat12_ib.c checks that refusal positively).
// It changes nothing else: no register write, no ring write, no page-table write, no fault handling.
//
// ROOM. arm15's committed segment reported `pad-sum 67 dw`. Offline over that exact frame
// (src/xlat12/tests/fixture_arm16_ws.h, arm15 capture frame 1, IB0 1040 dw): change A alone leaves 60, and change A
// plus this switch leaves 43, with `raster_follow` reaching 2 (one VIEW2, one FDCC) - so's `raster_follow != 2`
// refusal, written for a two-draw render half, does NOT fire on this single ENCODER segment.

#ifndef N48_GFX_RASTERARM_H
#define N48_GFX_RASTERARM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The two values the armed path's `ex` gains. `on` is the switch; `raster_flag` and `rsrc3` are the caller's own
// XLAT12_EXTRA_RASTER and XLAT12_RSRC3_GS_CU_EN, passed in so that this header never carries a second copy of a
// constant that lives in xlat12_ib.h - a drifted copy is exactly the defect this project has paid for before.
static inline uint32_t n48_ra_flags(uint32_t on, uint32_t raster_flag) { return on ? raster_flag : 0u; }
static inline uint32_t n48_ra_rsrc3(uint32_t on, uint32_t rsrc3)       { return on ? rsrc3 : 0u; }

// The verb's mode byte: 1 = on, 2 = off, anything else = read only. Returns 1 when `*flag` was written.
static inline int n48_ra_set(uint32_t m, uint32_t *flag)
{
    if (m != 1u && m != 2u) return 0;
    if (flag) *flag = (m == 1u) ? 1u : 0u;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* N48_GFX_RASTERARM_H */
