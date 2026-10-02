//
//  navi48_liveraster.h — build 0.0.515: the lit OTG's raster through a READ-ONLY dcn41 device
//  that does NOT depend on n48dcn::bind. Pure (no IOKit), host-tested by tests/dcn_liveraster_test.cpp; the kext's
//  n48dcn::liveRaster (navi48_dcn.cpp) is a one-line call into n48lr_live over the device n48dcn::attach built at start().
//
//  WHY. Through 0.0.514 liveRaster went through lit_otg(), which returns -1 unless gDcn.armed, and gDcn.armed is set only by
//  n48dcn::bind, which only the DCN verbs 74-77 run. So a native-1440p boot's FIRST AGDC LINKCFG reply carried the CEA-derived
//  2840x1485 at 253.0 MHz and a reply after a DCN verb could differ.
//
//  THE READ-ONLY DEVICE. n48lr_ro_build hands dcn41_dev_init (which does NO register I/O: dcn41_core.c only checks its
//  arguments, the measured DMU bases and the BAR5 window) a write callback that writes NOTHING (n48lr_wreg_refuse) and a
//  delay callback that does nothing. The only dcn41 calls n48lr_live makes are dcn41_otg_get_active_size and
//  dcn41_otg_get_totals, which only READ (OTG_CONTROL, OTG_H/V_BLANK_START_END, OTG_H/V_TOTAL) - the same registers, through
//  the same accessors, that lit_otg()/liveRaster read after bind in 0.0.514 and that `dcnstate` (verb 74) has read on this
//  hardware since T1. Built once at start() (no register I/O), so no caller can race its construction.
//  A device that did not build (bases not the measured ones, no BAR5) answers 0 = unknown, and the caller falls back to
//  0.0.513's CEA timing - the fail direction 0.0.514 already had.
#ifndef N48_LIVERASTER_H
#define N48_LIVERASTER_H

#include <stdint.h>
#include <stddef.h>
extern "C" {
#include "dcn41.h"
#include "dcn41_modes.h"
}

enum { N48LR_NONE = 0u, N48LR_READY = 1u, N48LR_FAILED = 2u };

typedef struct {
    struct dcn41_dev d;
    uint32_t state;      /* N48LR_NONE until n48lr_ro_build ran once; then READY or FAILED, never rebuilt */
    int init_rc;         /* dcn41_dev_init's answer (DCN41_OK when READY) */
} n48lr_ro;

/* The read-only device's write callback: it writes NOTHING. dcn41_dev_init requires a non-null callback; the readers
 * n48lr_live calls never invoke it. */
static inline void n48lr_wreg_refuse(void *cookie, uint32_t abs_dword, uint32_t value)
{
    (void)cookie; (void)abs_dword; (void)value;
}
static inline void n48lr_udelay_none(void *cookie, uint32_t us) { (void)cookie; (void)us; }

/* Build once. 1 = READY. `rreg` is the caller's plain register read; `seg` the DMU bases from IP discovery. No register I/O. */
static inline uint32_t n48lr_ro_build(n48lr_ro *ro, void *cookie, dcn41_rreg_fn rreg, const uint32_t seg[DCN41_NUM_SEGS],
                                      uint32_t mmio_dwords)
{
    if (!ro) return 0u;
    if (ro->state != N48LR_NONE) return ro->state == N48LR_READY ? 1u : 0u;
    if (!rreg || !seg || !mmio_dwords) { ro->init_rc = DCN41_E_ARG; ro->state = N48LR_FAILED; return 0u; }
    ro->init_rc = dcn41_dev_init(&ro->d, cookie, rreg, n48lr_wreg_refuse, n48lr_udelay_none, seg, mmio_dwords, 0u);
    ro->state = ro->init_rc == DCN41_OK ? N48LR_READY : N48LR_FAILED;
    return ro->state == N48LR_READY ? 1u : 0u;
}

/* THE LIT OTG'S RASTER (0.0.514's liveRaster body, unchanged in what it reads and answers): the first master-enabled OTG's
 * active size and totals, and the pixel clock and porches of the sink's EDID row with the SAME active size and totals.
 * 1 = read and matched; 0 = unknown (no device, no lit OTG, a read refused, no row). It depends on NOTHING but `ro`. */
static inline uint32_t n48lr_live(n48lr_ro *ro, uint32_t *hAct, uint32_t *vAct, uint32_t *hTot, uint32_t *vTot,
                                  uint32_t *hFront, uint32_t *hSync, uint32_t *vFront, uint32_t *vSync, uint64_t *pixelClockHz)
{
    if (!hAct || !vAct || !hTot || !vTot || !hFront || !hSync || !vFront || !vSync || !pixelClockHz) return 0u;
    if (!ro || ro->state != N48LR_READY) return 0u;
    struct dcn41_dev *d = &ro->d;
    int otg = -1;
    uint32_t w = 0u, h = 0u;
    for (uint32_t i = 0; i < DCN41_NUM_PIPES; i++) {
        bool en = false;
        uint32_t wi = 0u, hi = 0u;
        if (dcn41_otg_get_active_size(d, i, &en, &wi, &hi) == DCN41_OK && en) { otg = (int)i; w = wi; h = hi; break; }
    }
    if (otg < 0 || !w || !h) return 0u;
    uint32_t ht1 = 0u, vt1 = 0u;
    if (dcn41_otg_get_totals(d, (uint32_t)otg, &ht1, &vt1) != DCN41_OK) return 0u;
    for (size_t i = 0; i < sizeof(dcn41_modes) / sizeof(dcn41_modes[0]); i++) {
        const struct dcn41_mode &m = dcn41_modes[i];
        if (m.h_active != w || m.v_active != h || m.h_total != ht1 + 1u || m.v_total != vt1 + 1u) continue;
        *hAct = w; *vAct = h; *hTot = ht1 + 1u; *vTot = vt1 + 1u;
        *hFront = m.h_front; *hSync = m.h_sync; *vFront = m.v_front; *vSync = m.v_sync;
        *pixelClockHz = (uint64_t)m.pix_clk_100hz * 100u;
        return 1u;
    }
    return 0u;
}

/* build 0.0.542 (apple/scanout_full.h, `accel scanout full`): WHAT THE DISPLAY ENGINE IS SCANNING, READ-ONLY, through the SAME
 * read-only device (its write callback writes nothing; no dcn41 call below writes). The lit OTG (dcn41_otg_get_active_size, as
 * n48lr_live), OTG0's frame counter (dcn41_otg_get_frame_count), and HUBP0's registers by plain reads: DCSURF_FLIP_CONTROL
 * (SURFACE_FLIP_PENDING, bit 8), SURFACE_EARLIEST_INUSE(_HIGH), PRIMARY_SURFACE_ADDRESS(_HIGH) (dcn41_regs.h), and the four flip mode's
 * n48dcn::fmHubpGeom reads through the bound device - DCSURF_SURFACE_CONFIG 0x05e5 (pixel format, bits 0..6), DCSURF_TILING_CONFIG
 * 0x05e7 (SW_MODE, bits 0..4), DCSURF_PRI_VIEWPORT_DIMENSION 0x05eb (width 0..15, height 16..31), DCSURF_SURFACE_PITCH 0x0607
 * (pitch - 1, bits 0..15) - at the same instance-0 offsets and BASE_IDX 2 (navi48_dcn.cpp kOff*; pinned equal by the host test).
 * HUBP0 only, as flip mode: a lit OTG other than 0 answers lit_otg with dcn_ok 1 and nothing else read. 1 = every read made. */
#include "../apple/scanout_full.h"
enum : uint32_t { N48LR_OFF_SURF_CONFIG = 0x05e5u, N48LR_OFF_TILING_CONFIG = 0x05e7u, N48LR_OFF_VIEWPORT_DIM = 0x05ebu,
                  N48LR_OFF_SURF_PITCH = 0x0607u,
                  /* build 0.0.543 item E5: regHUBP0_DCSURF_PRI_VIEWPORT_START 0x05e9, BASE_IDX 2 (re/linux-dc dcn_4_1_0_offset.h:2092;
                   * PRI_VIEWPORT_X_START 0..15, PRI_VIEWPORT_Y_START 16..31, dcn_4_1_0_sh_mask.h:7123-7126). A READ. */
                  N48LR_OFF_VIEWPORT_START = 0x05e9u };
static inline uint32_t n48lr_rd2(n48lr_ro *ro, uint32_t off, uint32_t *v)
{
    const uint32_t a = dcn41_abs(&ro->d, off, 2u);
    if (a == DCN41_BAD_OFFSET) return 0u;
    *v = ro->d.rreg(ro->d.cookie, a);
    return 1u;
}
static inline uint32_t n48lr_scan_surface(n48lr_ro *ro, n48_sf_dcn *s)
{
    if (!s) return 0u;
    memset(s, 0, sizeof(*s));
    s->lit_otg = 0xffffffffu;
    if (!ro || ro->state != N48LR_READY) return 0u;
    struct dcn41_dev *d = &ro->d;
    for (uint32_t i = 0; i < DCN41_NUM_PIPES; i++) {
        bool en = false;
        uint32_t wi = 0u, hi = 0u;
        if (dcn41_otg_get_active_size(d, i, &en, &wi, &hi) != DCN41_OK) return 0u;
        if (en) { s->lit_otg = i; break; }
    }
    if (s->lit_otg != 0u) { s->dcn_ok = 1u; return 1u; }
    uint32_t flip = 0u, elo = 0u, ehi = 0u, plo = 0u, phi = 0u, cfg = 0u, tile = 0u, dim = 0u, pitch = 0u, fc = 0u, vst = 0u;
    if (!n48lr_rd2(ro, DCN41_HUBPREQ_DCSURF_FLIP_CONTROL(0), &flip) ||
        !n48lr_rd2(ro, DCN41_HUBPREQ_DCSURF_SURFACE_EARLIEST_INUSE(0), &elo) ||
        !n48lr_rd2(ro, DCN41_HUBPREQ_DCSURF_SURFACE_EARLIEST_INUSE_HIGH(0), &ehi) ||
        !n48lr_rd2(ro, DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS(0), &plo) ||
        !n48lr_rd2(ro, DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH(0), &phi) ||
        !n48lr_rd2(ro, N48LR_OFF_SURF_CONFIG, &cfg) || !n48lr_rd2(ro, N48LR_OFF_TILING_CONFIG, &tile) ||
        !n48lr_rd2(ro, N48LR_OFF_VIEWPORT_DIM, &dim) || !n48lr_rd2(ro, N48LR_OFF_SURF_PITCH, &pitch) ||
        !n48lr_rd2(ro, N48LR_OFF_VIEWPORT_START, &vst))   /* build 0.0.543 item E5 */
        return 0u;
    if (dcn41_otg_get_frame_count(d, 0u, &fc) != DCN41_OK) return 0u;
    s->pending = (flip & DCN41_HUBPREQ_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK) ? 1u : 0u;
    s->earliest = ((uint64_t)(ehi & DCN41_HUBPREQ_DCSURF_SURFACE_EARLIEST_INUSE_HIGH__SURFACE_EARLIEST_INUSE_ADDRESS_HIGH_MASK) << 32) | elo;
    s->primary = ((uint64_t)(phi & DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH_MASK) << 32) | plo;
    s->fmt = cfg & 0x7Fu;
    s->sw_mode = tile & 0x1Fu;
    s->vp_w = dim & 0xFFFFu;
    s->vp_h = (dim >> 16) & 0xFFFFu;
    s->vp_x = vst & 0xFFFFu;             /* build 0.0.543 item E5 */
    s->vp_y = (vst >> 16) & 0xFFFFu;
    s->pitch_px = (pitch & 0xFFFFu) + 1u;
    s->frame_count = fc;
    s->dcn_ok = 1u;
    return 1u;
}

#endif /* N48_LIVERASTER_H */
