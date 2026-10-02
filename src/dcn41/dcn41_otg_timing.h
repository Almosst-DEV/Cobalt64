/*
 * dcn41_otg_timing.h - read, write back and compare an OTG's timing register set.
 *
 * WHAT THIS IS FOR, AND THE SAFETY PROPERTY IT RESTS ON
 *
 * The first mode-set this project attempts is a SAME-MODE RE-TIMING of the already-lit,
 * already-link-trained DisplayPort output: read the timing registers, write back exactly the values
 * that were read, then read again and require every dword to match. Nothing is computed, so nothing
 * can be computed wrongly; the raster cannot change because the values written ARE the values read.
 * It still exercises the whole path a real mode-set uses - the register set, the write order, the
 * double buffering and the latch - which is the part nobody has ever run on this card.
 *
 * Two deliberate departures from Linux's optc1_program_timing, both to make this safe on a LIVE pipe:
 *
 *   1. Whole-dword read-modify-nothing-write. Linux uses REG_UPDATE on fields; we write the dword we
 *      just read, so every field we do not know about is preserved bit for bit by construction.
 *   2. The WRITE set excludes every register in that sequence that carries a control, status or
 *      event bit, because writing back a status bit can have a side effect. In particular it
 *      excludes `REG_UPDATE(CONTROL, VTG0_ENABLE, 0)`, which Linux does because it programs timing on
 *      a DISABLED pipe - disabling the VTG on a live one would blank the screen. It also excludes
 *      OTG_CONTROL, OTG_V_TOTAL_CONTROL, OTG_INTERLACE_CONTROL, OTG_STEREO_CONTROL, the
 *      OTG_GLOBAL_CONTROLn registers and OTG_MASTER_UPDATE_LOCK. Those are captured READ-ONLY, for
 *      the record and for the comparison, and never written.
 *
 * Every offset below is the instance-0 value from Linux 238650ef6c7c dcn_4_1_0_offset.h, BASE_IDX 2;
 * the OTG instance stride is 0x80 (regOTG0_OTG_H_TOTAL 0x1b2a, regOTG1_OTG_H_TOTAL 0x1baa). The
 * absolute BAR5 dword is seg[2] + offset + 0x80 * instance, and dcn41_abs() does that arithmetic.
 *
 * Freestanding: no heap, no libc, no floating point. C11 and C++17, and x86_64 kernel code.
 */
#ifndef N48_DCN41_OTG_TIMING_H
#define N48_DCN41_OTG_TIMING_H

#include <stdbool.h>
#include <stdint.h>

#include "dcn41.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The registers written back, in Linux's optc1_program_timing order. */
enum dcn41_timing_w {
    DCN41_TW_H_TOTAL = 0,
    DCN41_TW_H_SYNC_A,
    DCN41_TW_H_BLANK_START_END,
    DCN41_TW_H_SYNC_A_CNTL,
    DCN41_TW_V_TOTAL,
    DCN41_TW_V_TOTAL_MIN,
    DCN41_TW_V_TOTAL_MAX,
    DCN41_TW_V_SYNC_A,
    DCN41_TW_V_BLANK_START_END,
    DCN41_TW_V_SYNC_A_CNTL,
    DCN41_TW_H_TIMING_CNTL,
    DCN41_TW_VSTARTUP_PARAM,
    DCN41_TW_VUPDATE_PARAM,
    DCN41_TW_VREADY_PARAM,
    DCN41_TIMING_W_COUNT
};

/* Captured for the record and the comparison, NEVER written. */
enum dcn41_timing_r {
    DCN41_TR_OTG_CONTROL = 0,
    DCN41_TR_V_TOTAL_CONTROL,
    DCN41_TR_INTERLACE_CONTROL,
    DCN41_TR_STEREO_CONTROL,
    DCN41_TR_GLOBAL_CONTROL0,
    DCN41_TR_MASTER_UPDATE_LOCK,
    DCN41_TIMING_R_COUNT
};

struct dcn41_otg_timing {
    uint32_t otg;
    uint32_t w[DCN41_TIMING_W_COUNT];      /* the writable timing set */
    uint32_t r[DCN41_TIMING_R_COUNT];      /* read-only context */
    uint32_t frame_count;                  /* sampled with the set, so a caller can prove the OTG ran */
    uint8_t  valid;
};

/* Names, for logs. Index by enum; returns "?" out of range. */
const char *dcn41_timing_w_name(uint32_t i);
const char *dcn41_timing_r_name(uint32_t i);
/* Instance-0 offsets, so a caller can log absolute addresses without duplicating the table. */
uint32_t dcn41_timing_w_offset(uint32_t i);
uint32_t dcn41_timing_r_offset(uint32_t i);

/* Read the whole set. No writes. Refuses instance >= DCN41_NUM_PIPES. */
int dcn41_otg_read_timing(struct dcn41_dev *dev, uint32_t otg, struct dcn41_otg_timing *t);

/* Write back t->w[] in Linux's order. REFUSES unless t->valid and t->otg == otg, so a caller cannot
 * write one pipe's timing into another's. Writes nothing from t->r[]. */
int dcn41_otg_write_timing(struct dcn41_dev *dev, uint32_t otg, const struct dcn41_otg_timing *t);

/* Compare two captures. Returns the number of differing dwords across w[] AND r[]; *first_w and
 * *first_r receive the first differing index of each, or DCN41_TIMING_W_COUNT / _R_COUNT if none. */
uint32_t dcn41_otg_timing_diff(const struct dcn41_otg_timing *a, const struct dcn41_otg_timing *b,
                               uint32_t *first_w, uint32_t *first_r);

/* Change ONE field of a capture: OTG_V_TOTAL, i.e. the number of lines per frame. This is the whole
 * of the first REAL timing change, and every guard that makes it safe lives here rather than in the
 * kext, so it is host-testable.
 *
 * Why v_total alone is the safest real change: h_total and the pixel clock are untouched, so the
 * HORIZONTAL line rate does not move (88.79 kHz on this card either way) and the DP link, its symbol
 * clock and the PHY see nothing at all. Only the number of blanking lines per frame changes, which
 * is exactly what adaptive sync does frame to frame. The active area is untouched by construction:
 * this function refuses to write a value that would collide with the vertical blank start.
 *
 * `delta_lines` is signed and capped at DCN41_VTOTAL_MAX_DELTA. Refuses, doing nothing:
 *   - a capture that is not valid;
 *   - |delta| above the cap;
 *   - a resulting v_total - 1 at or below OTG_V_BLANK_START (the raster would be inconsistent);
 *   - a resulting value outside the 15-bit register field.
 * Returns DCN41_OK and writes t->w[DCN41_TW_V_TOTAL], or an error with t untouched. */
#define DCN41_VTOTAL_MAX_DELTA 256
int dcn41_otg_timing_adjust_v_total(struct dcn41_otg_timing *t, int32_t delta_lines);

/* Decoded fields a human can check against a mode table, from a capture. Pure arithmetic, no I/O. */
struct dcn41_timing_decoded {
    uint32_t h_total, v_total;          /* register value + 1 */
    uint32_t h_blank_start, h_blank_end, v_blank_start, v_blank_end;
    uint32_t h_active, v_active;        /* blank_start - blank_end, as optc1_get_otg_active_size does */
    uint32_t h_sync_width, v_sync_width;
    uint32_t vstartup, vupdate_offset, vupdate_width, vready_offset;
    uint8_t  master_en, update_lock_held;
};
void dcn41_otg_timing_decode(const struct dcn41_otg_timing *t, struct dcn41_timing_decoded *d);

#ifdef __cplusplus
}
#endif

#endif /* N48_DCN41_OTG_TIMING_H */
