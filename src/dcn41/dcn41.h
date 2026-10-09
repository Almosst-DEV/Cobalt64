/*
 * dcn41.h - DCN 4.1 (Navi 48, gfx1201) display register layer for VBL, flip and scanout position (stage 1).
 *
 * NOT WIRED INTO ANY BUILD YET. Kext-ready: plain C11 that also compiles as C++17, no heap, no floating point
 * (the .c files #pragma GCC poison float and double), no libc beyond <stdint.h>/<stddef.h>/<stdbool.h>; every register
 * address comes from the generated dcn41_regs.h (Linux dcn_4_1_0_offset.h / _sh_mask.h) plus the DMU segment
 * bases the caller got from IP discovery.
 *
 * Linux equivalence (tools/dcn41/harness.py diffs register traces against Linux's own compiled functions):
 *   dcn41_irq_set / dcn41_irq_ack      dal_irq_service_set / dal_irq_service_ack over irq_source_info_dcn401[]
 *                                      (irq_service.c, irq_service_dcn401.c; HPD ack = hpd0_ack)
 *   dcn41_ih_to_irq                    to_dal_irq_source_dcn401() (irq_service_dcn401.c), plus the client-id check
 *                                      amdgpu_irq_add_id(adev, SOC15_IH_CLIENTID_DCE, ...) performs
 *   dcn41_hubp_program_flip            hubp401_program_surface_flip_and_addr(), PLN_ADDR_TYPE_GRAPHICS path
 *   dcn41_hubp_is_flip_pending         hubp2_is_flip_pending() (dcn401 hubp_funcs .hubp_is_flip_pending)
 *   dcn41_hubp_set_flip_int            hubp401_set_flip_int()
 *   dcn41_hubp_in_blank                hubp401_in_blank()
 *   dcn41_otg_lock / dcn41_otg_unlock  optc3_lock() / optc1_unlock() (dcn401_tg_funcs .lock / .unlock)
 *   dcn41_otg_get_position             optc1_get_position()
 *   dcn41_otg_get_frame_count          optc1_get_vblank_counter()
 *   dcn41_otg_get_scanoutpos           optc1_get_crtc_scanoutpos()
 *   dcn41_otg_get_active_size          optc1_get_otg_active_size()
 *   dcn41_otg_is_counter_moving        optc1_is_counter_moving()
 * Deliberate differences, all BEFORE any register I/O (a refused call touches nothing): instance numbers >= 4 are
 * refused (Linux indexes past its 4-entry tables or dummy entries); a flip address of 0, above 48 bits, or outside the
 * caller's scanout window is refused (Linux skips the address writes for 0 but still writes the flip type and VMID,
 * and silently truncates high bits); a VMID above 15 is refused.
 *
 * Which interrupt paces VBL (see notes/DISPLAY-DESIGN.md): Linux 238650ef6c7c's amdgpu_dm uses VUPDATE_NO_LOCK (IH src
 * 0x57 + otg) as the single DCN vblank and flip-completion interrupt ("VSTARTUP and GRPH_PFLIP are not used",
 * amdgpu_dm_irq.c dm_crtc_high_irq_handler). DC's irq table still maps VSTARTUP (0x3C + otg) and HUBP flip
 * (0x4F + hubp), so all three are supported here.
 */
#ifndef N48_DCN41_H
#define N48_DCN41_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dcn41_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DCN41_NUM_SEGS 5u
#define DCN41_NUM_PIPES 4u                     /* res_cap_dcn4_01: 4 OTG, 4 HUBP (dcn401_resource.c) */
#define DCN41_NUM_HPD 4u

/* The DMU (discovery hw id 271, v4.1.0) segment bases measured on this card:
 * notes/logs/runs/sacomp1/driverlog-stream.txt:1413
 * "Navi48Ttl:   hwId 271 inst 0  v4.1.0  5 segs: 00000012 000000c0 000034c0 00009000 02403c00 00000000"
 * and Linux's own irq_service_dcn401.c: "#define DCN_BASE__INST0_SEG2 0x000034C0". */
#define DCN41_SEG0_EXPECTED 0x00000012u
#define DCN41_SEG1_EXPECTED 0x000000c0u
#define DCN41_SEG2_EXPECTED 0x000034c0u
#define DCN41_SEG3_EXPECTED 0x00009000u
#define DCN41_SEG4_EXPECTED 0x02403c00u

/* SOC15_IH_CLIENTID_DCE = 0x04 (include/soc15_ih_clientid.h) */
#define DCN41_IH_CLIENT_DCE 0x04u
/* include/ivsrcid/dcn/irqsrcs_dcn_1_0.h */
#define DCN41_IH_SRC_HPD 0x09u                           /* DCN_1_0__SRCID__DC_HPD1_INT (all HPD and HPD RX; ctx = ext id) */
#define DCN41_IH_SRC_VLINE0_OTG0 0x1Eu                   /* DCN_1_0__SRCID__OTG1_VERTICAL_INTERRUPT0_CONTROL .. OTG5 0x22 */
#define DCN41_IH_SRC_VLINE0_OTG5 0x38u                   /* DCN_1_0__SRCID__OTG6_VERTICAL_INTERRUPT0_CONTROL */
#define DCN41_IH_SRC_VSTARTUP_OTG0 0x3Cu                 /* DCN_1_0__SRCID__DC_D1_OTG_VSTARTUP .. D6 0x41 */
#define DCN41_IH_SRC_PFLIP_HUBP0 0x4Fu                   /* DCN_1_0__SRCID__HUBP0_FLIP_INTERRUPT .. HUBP5 0x54 (HUBP7 0x56) */
#define DCN41_IH_SRC_VUPDATE_NO_LOCK_OTG0 0x57u          /* DCN_1_0__SRCID__OTG0_IHC_V_UPDATE_NO_LOCK_INTERRUPT .. OTG5 0x5C */
#define DCN41_IH_SRC_DMCUB_OUTBOX 0x68u                  /* DCN_1_0__SRCID__DMCUB_OUTBOX_LOW_PRIORITY_READY_INT */

enum {
    DCN41_OK = 0,
    DCN41_E_ARG = -1,        /* null pointer or malformed argument */
    DCN41_E_INST = -2,       /* instance out of range for this call */
    DCN41_E_BASES = -3,      /* segment bases differ from the measured discovery values */
    DCN41_E_WINDOW = -4,     /* an address would leave the caller's BAR5 window */
    DCN41_E_ADDR = -5,       /* flip address 0, above 48 bits, or outside the scanout window */
    DCN41_E_TIMEOUT = -6,    /* a wait expired (the register I/O still happened, exactly as Linux's REG_WAIT) */
    DCN41_E_UNINIT = -7,     /* dcn41_dev_init() did not succeed on this device */
    DCN41_E_NOREQ = -8,      /* no flip was requested on this HUBP through this layer */
    DCN41_E_RACE = -9,       /* DMUB: the hardware inbox WPTR moved under us (another writer) */
    DCN41_E_FULL = -10,      /* DMUB: ring full (one entry always unusable, as dmub_rb_full) */
    DCN41_E_READBACK = -11,  /* DMUB: the command read back from ring memory differs; WPTR not moved */
    DCN41_E_STATE = -12,     /* DMUB: the probed firmware state does not permit this operation */
    DCN41_E_HW = -13         /* DMUB: a hardware pointer beyond the ring capacity (DMUB_STATUS_HW_FAILURE) */
};

typedef uint32_t (*dcn41_rreg_fn)(void *cookie, uint32_t abs_dword);
typedef void (*dcn41_wreg_fn)(void *cookie, uint32_t abs_dword, uint32_t value);
typedef void (*dcn41_udelay_fn)(void *cookie, uint32_t microseconds);

#define DCN41_DEV_MAGIC 0x31344e44u             /* "DN41" */
#define DCN41_F_ALLOW_OTHER_BASES 0x1u          /* accept segment bases other than the measured ones (never on this card) */

struct dcn41_dev {
    uint32_t magic;
    uint32_t flags;
    void *cookie;
    dcn41_rreg_fn rreg;
    dcn41_wreg_fn wreg;
    dcn41_udelay_fn udelay;
    uint32_t seg[DCN41_NUM_SEGS];
    uint32_t mmio_dwords;                        /* BAR5 size in dwords; every address used is checked against it */
    uint64_t scanout_lo, scanout_hi;             /* flip addresses must lie in [lo, hi) (MC space); hi 0 = no window */
    uint64_t hubp_request_addr[DCN41_NUM_PIPES]; /* hubp->request_address.grph.addr.quad_part in Linux */
    uint8_t hubp_request_valid[DCN41_NUM_PIPES];
    uint8_t hubp_power_gated[DCN41_NUM_PIPES];   /* hubp->power_gated in Linux; set by the caller */
    /* build 0.0.518 (flip mode F1): while flip_exact_n != 0, a flip address must ALSO equal one of the first
     * flip_exact_n entries (the console buffer A and the back buffer B) - the window above still applies. 0 = no exact set (every
     * caller before 0.0.518: dev_init zeroes it). No Linux counterpart: a refusal before any I/O, like the window. */
    uint64_t flip_exact[2];
    uint32_t flip_exact_n;
};

/* Initialise: checks the callbacks, that seg[] equals the measured DMU bases (unless DCN41_F_ALLOW_OTHER_BASES), and
 * that seg[b] + the largest offset this layer uses for BASE_IDX b is below mmio_dwords. No register I/O. */
int dcn41_dev_init(struct dcn41_dev *dev, void *cookie, dcn41_rreg_fn rreg, dcn41_wreg_fn wreg,
                   dcn41_udelay_fn udelay, const uint32_t seg[DCN41_NUM_SEGS], uint32_t mmio_dwords, uint32_t flags);

/* Restrict flip addresses to [lo, hi) in GPU MC space, e.g. DCN_VM_FB_LOCATION_BASE/TOP << 24 (T1 reads them). */
int dcn41_dev_set_scanout_window(struct dcn41_dev *dev, uint64_t lo, uint64_t hi);
/* build 0.0.518: restrict flip addresses to EXACTLY these n (0..2) addresses on top of the window; n 0 clears. Each must be
 * non-zero, below 2^48 and inside the window (when one is set); DCN41_E_ARG otherwise, and the previous set is kept. No I/O. */
int dcn41_dev_set_flip_exact(struct dcn41_dev *dev, const uint64_t *addrs, uint32_t n);

/* Absolute BAR5 dword for (offset, base index); DCN41_BAD_OFFSET when either is invalid. */
uint32_t dcn41_abs(const struct dcn41_dev *dev, uint32_t offset, uint32_t base_idx);
/* BASE_IDX that dcn41_abs_rd (READS only) resolves: 1 (DCCG), 2, 3. Deliberately NOT in the generated dcn41_regs.h: the write layer stays on DCN41_BASE_IDX_USED_MASK (0x4). */
#define DCN41_BASE_IDX_READ_MASK 0xEu
/* 0.0.628: READ-ONLY twin, additionally resolving BASE_IDX 1 and 3 (dcn41_abs still refuses them). For register READS only (dispcensus, ddcread's prescale read). */
uint32_t dcn41_abs_rd(const struct dcn41_dev *dev, uint32_t offset, uint32_t base_idx);

/* ---- interrupts ------------------------------------------------------------------------------------------- */
enum dcn41_irq_kind {
    DCN41_IRQ_NONE = 0,
    DCN41_IRQ_HPD,              /* inst = HPD pin 0..5 (DC_IRQ_SOURCE_HPD1 + inst) */
    DCN41_IRQ_HPD_RX,           /* inst = HPD pin 0..5 (DC_IRQ_SOURCE_HPD1RX + inst) */
    DCN41_IRQ_VLINE0,           /* inst = OTG 0..5 (DC_IRQ_SOURCE_DC1_VLINE0 + inst) */
    DCN41_IRQ_VSTARTUP,         /* inst = OTG 0..5 (DC_IRQ_SOURCE_VBLANK1 + inst) */
    DCN41_IRQ_PFLIP,            /* inst = HUBP 0..5 (DC_IRQ_SOURCE_PFLIP1 + inst) */
    DCN41_IRQ_VUPDATE_NO_LOCK,  /* inst = OTG 0..5 (DC_IRQ_SOURCE_VUPDATE1 + inst) */
    DCN41_IRQ_DMCUB_OUTBOX,     /* inst = 0 (DC_IRQ_SOURCE_DMCUB_OUTBOX) */
    DCN41_IRQ_KIND_COUNT
};

struct dcn41_irq {
    uint8_t kind;               /* enum dcn41_irq_kind */
    uint8_t inst;
};

/* Classify one IH ring entry as the kext's ring decode sees it (amd/ih_v7_0.cpp ih_decode_iv: client_id = DW0[7:0],
 * src_id = DW0[15:8], src_data[0] = DW4). Entries whose client is not DCE classify as NONE. */
struct dcn41_irq dcn41_ih_to_irq(uint32_t client_id, uint32_t src_id, uint32_t src_data0);

/* Enable or disable one source: acknowledges it first, then read-modify-writes its enable bit (dal_irq_service_set).
 * Supported: VSTARTUP, VUPDATE_NO_LOCK and PFLIP for inst 0..3, HPD and HPD_RX for 0..3, DMCUB_OUTBOX. */
int dcn41_irq_set(struct dcn41_dev *dev, struct dcn41_irq irq, bool enable);
/* Acknowledge one source (dal_irq_service_ack). */
int dcn41_irq_ack(struct dcn41_dev *dev, struct dcn41_irq irq);

/* Read-only status of an OTG's VSTARTUP and VUPDATE_NO_LOCK sources: one read of OTG_GLOBAL_SYNC_STATUS. */
struct dcn41_otg_irq_status {
    uint32_t raw;
    uint8_t vstartup_int_en, vstartup_occurred, vstartup_int_status;
    uint8_t vupdate_no_lock_int_en, vupdate_no_lock_occurred, vupdate_no_lock_int_status;
};
int dcn41_otg_irq_status(struct dcn41_dev *dev, uint32_t otg, struct dcn41_otg_irq_status *st);

/* Read-only status of a HUBP's flip interrupt: one read of DCSURF_SURFACE_FLIP_INTERRUPT. */
struct dcn41_flip_irq_status {
    uint32_t raw;
    uint8_t int_mask, int_type, occurred, int_status;
};
int dcn41_hubp_flip_irq_status(struct dcn41_dev *dev, uint32_t hubp, struct dcn41_flip_irq_status *st);

/* ---- HUBP flip -------------------------------------------------------------------------------------------- */
/* Program a graphics-plane flip: flip type, VMID (vsync flips only), stereo off, TMZ, then the address, high dword
 * first ("program high first and then the low addr, order matters!"). immediate = false latches at VUPDATE. */
int dcn41_hubp_program_flip(struct dcn41_dev *dev, uint32_t hubp, uint64_t mc_addr, uint32_t vmid, bool tmz,
                            bool immediate);
/* hubp2_is_flip_pending: SURFACE_FLIP_PENDING set, or the earliest-in-use address differs from the last request.
 * Returns DCN41_E_NOREQ (after the same reads) if no request was recorded; *pending is then computed against 0,
 * as Linux's zeroed request_address would be. A power-gated HUBP reads nothing and reports not pending. */
int dcn41_hubp_is_flip_pending(struct dcn41_dev *dev, uint32_t hubp, bool *pending, uint64_t *earliest_inuse);
int dcn41_hubp_set_flip_int(struct dcn41_dev *dev, uint32_t hubp);
int dcn41_hubp_in_blank(struct dcn41_dev *dev, uint32_t hubp, bool *in_blank);
/* Read-only: the programmed primary address (high then low). */
int dcn41_hubp_read_primary_addr(struct dcn41_dev *dev, uint32_t hubp, uint64_t *mc_addr);

/* ---- OTG lock, position, frame counter ------------------------------------------------------------------- */
int dcn41_otg_lock(struct dcn41_dev *dev, uint32_t otg);     /* DCN41_E_TIMEOUT if UPDATE_LOCK_STATUS never read 1 */
int dcn41_otg_unlock(struct dcn41_dev *dev, uint32_t otg);

struct dcn41_otg_position {
    uint32_t horizontal_count, vertical_count, nominal_vcount;
};
int dcn41_otg_get_position(struct dcn41_dev *dev, uint32_t otg, struct dcn41_otg_position *pos);
int dcn41_otg_get_frame_count(struct dcn41_dev *dev, uint32_t otg, uint32_t *frame_count);
int dcn41_otg_get_scanoutpos(struct dcn41_dev *dev, uint32_t otg, uint32_t *v_blank_start, uint32_t *v_blank_end,
                             uint32_t *h_position, uint32_t *v_position);
/* *enabled = OTG_MASTER_EN; width/height only written when enabled (optc1_get_otg_active_size). */
int dcn41_otg_get_active_size(struct dcn41_dev *dev, uint32_t otg, bool *enabled, uint32_t *width, uint32_t *height);
int dcn41_otg_is_counter_moving(struct dcn41_dev *dev, uint32_t otg, bool *moving);
/* Read-only: OTG_H_TOTAL / OTG_V_TOTAL fields (total - 1, as optc1_program_timing writes them). */
int dcn41_otg_get_totals(struct dcn41_dev *dev, uint32_t otg, uint32_t *h_total_minus1, uint32_t *v_total_minus1);

/* ---- refresh measurement (pure integer arithmetic, no I/O) ---------------------------------------------- */
#define DCN41_FRAME_COUNT_MASK DCN41_OTG_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK
/* Frames between two OTG_FRAME_COUNT samples, modulo the 24-bit counter. */
uint32_t dcn41_frame_delta(uint32_t count0, uint32_t count1);
/* Measured refresh in millihertz from two frame-count samples elapsed_ns apart; 0 if elapsed_ns is 0. */
uint64_t dcn41_refresh_mhz(uint32_t count0, uint32_t count1, uint64_t elapsed_ns);
/* Nominal refresh in millihertz from a timing: pixel clock in 100 Hz units (DMUB SET_PIXEL_CLOCK's unit), totals. */
uint64_t dcn41_nominal_refresh_mhz(uint32_t pix_clk_100hz, uint32_t h_total, uint32_t v_total);

#ifdef __cplusplus
}
#endif

#endif /* N48_DCN41_H */
