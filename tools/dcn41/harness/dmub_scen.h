/* dmub_scen.h - DMUB scenarios run through Linux's compiled command_table2.c / dmub_srv.c / dmub_dcn401.c
 * (dmub_linux_side.c) and through src/dcn41/dcn41_dmub.c (dmub_ours_side.c). */
#ifndef DCN41_HARNESS_DMUB_SCEN_H
#define DCN41_HARNESS_DMUB_SCEN_H
#include <stdint.h>

enum dmub_scen_kind {
    DS_TRANSMITTER, DS_ENCODER, DS_PIXEL_CLOCK, DS_POWER_GATING,   /* command bytes, no register I/O */
    DS_GPINT,                                                     /* full register trace */
    DS_RING_SUBMIT,                                               /* register writes + ring bytes */
    DS_WAIT_IDLE,                                                 /* full register trace */
    DS_RETURN_DATA,                                               /* bytes */
    DS_FW_META,                                                   /* result */
    DS_KIND_COUNT
};

/* DC-level inputs (Linux enums as plain ints); the Linux side translates them with Linux's own helper functions into
 * the ATOM-level inputs our builders take, and hands those back in `atom` so our side builds from exactly them. */
struct dmub_scen {
    int kind;
    int action, transmitter, engine, hpo_engine, signal, lanes, hpd, connector, color_depth, pll, controller;
    int encoder_obj, tx_color_depth, flags, lane_settings, dp_audio;
    uint32_t pixel_clock_khz, pixel_clock_100hz;
    /* GPINT */
    uint32_t gpint_cmd; uint16_t gpint_param; uint32_t timeout; int gpint_after;
    /* ring */
    uint32_t start_ptr, rptr_lag, ncmds, rptr_step;
    /* fw meta */
    uint32_t meta_blob_size, meta_at, bss;     /* magic placed meta_at bytes before the end; bss=1 legacy path */
};

struct dmub_atom {
    uint8_t phyid, action, mode_laneset, lanenum, hpdsel, digfe_sel, connobj_id, hpo_instance, skip_ssc;
    uint8_t digid, digmode, bitpercolor, is_hdmi;
    uint8_t pll_id, encoderobjid, encoder_mode, miscinfo, crtc_id, deep_color_ratio, pg_pipe, pg_enable;
};

struct dmub_out {
    int rc;
    uint8_t bytes[64 * 8];
    uint32_t nbytes;
    int64_t v[8];
    int nv;
};

void DL_init(void);
int DL_run(const struct dmub_scen *s, struct dmub_atom *atom, struct dmub_out *o, uint8_t *ring, uint32_t ring_size);
uint32_t DL_abs(const char *name);            /* Linux's own absolute offset of a DMCUB register (dmub_srv_dcn401_regs) */
int DL_enum(const char *name);                /* numeric value of a Linux DC enum constant used by the scenarios */

void DO_init(void);
int DO_run(const struct dmub_scen *s, const struct dmub_atom *atom, struct dmub_out *o, uint8_t *ring, uint32_t ring_size);
#endif
