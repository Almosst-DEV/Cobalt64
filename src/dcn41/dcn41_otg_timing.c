/*
 * dcn41_otg_timing.c - see dcn41_otg_timing.h for what this is and the safety property it rests on.
 */
#include "dcn41_otg_timing.h"
#include "dcn41_io.h"

#pragma GCC poison float double

/* Instance-0 offsets, BASE_IDX 2, from Linux 238650ef6c7c dcn_4_1_0_offset.h. Absolute for OTG0 in
 * the comment; OTG instance stride is 0x80. */
static const uint32_t k_w_off[DCN41_TIMING_W_COUNT] = {
    0x1b2a,   /* OTG_H_TOTAL             0x4fea */
    0x1b2c,   /* OTG_H_SYNC_A            0x4fec */
    0x1b2b,   /* OTG_H_BLANK_START_END   0x4feb */
    0x1b2d,   /* OTG_H_SYNC_A_CNTL       0x4fed */
    0x1b2f,   /* OTG_V_TOTAL             0x4fef */
    0x1b30,   /* OTG_V_TOTAL_MIN         0x4ff0 */
    0x1b31,   /* OTG_V_TOTAL_MAX         0x4ff1 */
    0x1b39,   /* OTG_V_SYNC_A            0x4ff9 */
    0x1b38,   /* OTG_V_BLANK_START_END   0x4ff8 */
    0x1b3a,   /* OTG_V_SYNC_A_CNTL       0x4ffa */
    0x1b2e,   /* OTG_H_TIMING_CNTL       0x4fee */
    0x1b85,   /* OTG_VSTARTUP_PARAM      0x5045 */
    0x1b86,   /* OTG_VUPDATE_PARAM       0x5046 */
    0x1b87,   /* OTG_VREADY_PARAM        0x5047 */
};

static const char *const k_w_name[DCN41_TIMING_W_COUNT] = {
    "OTG_H_TOTAL", "OTG_H_SYNC_A", "OTG_H_BLANK_START_END", "OTG_H_SYNC_A_CNTL",
    "OTG_V_TOTAL", "OTG_V_TOTAL_MIN", "OTG_V_TOTAL_MAX", "OTG_V_SYNC_A",
    "OTG_V_BLANK_START_END", "OTG_V_SYNC_A_CNTL", "OTG_H_TIMING_CNTL",
    "OTG_VSTARTUP_PARAM", "OTG_VUPDATE_PARAM", "OTG_VREADY_PARAM",
};

static const uint32_t k_r_off[DCN41_TIMING_R_COUNT] = {
    0x1b43,   /* OTG_CONTROL             0x5003 */
    0x1b33,   /* OTG_V_TOTAL_CONTROL     0x4ff3 */
    0x1b45,   /* OTG_INTERLACE_CONTROL   0x5005 */
    0x1b55,   /* OTG_STEREO_CONTROL      0x5015 */
    0x1b8e,   /* OTG_GLOBAL_CONTROL0     0x504e */
    0x1b89,   /* OTG_MASTER_UPDATE_LOCK  0x5049 */
};

static const char *const k_r_name[DCN41_TIMING_R_COUNT] = {
    "OTG_CONTROL", "OTG_V_TOTAL_CONTROL", "OTG_INTERLACE_CONTROL", "OTG_STEREO_CONTROL",
    "OTG_GLOBAL_CONTROL0", "OTG_MASTER_UPDATE_LOCK",
};

#define OTG_STRIDE 0x80u

const char *dcn41_timing_w_name(uint32_t i)
{
    return i < DCN41_TIMING_W_COUNT ? k_w_name[i] : "?";
}

const char *dcn41_timing_r_name(uint32_t i)
{
    return i < DCN41_TIMING_R_COUNT ? k_r_name[i] : "?";
}

uint32_t dcn41_timing_w_offset(uint32_t i)
{
    return i < DCN41_TIMING_W_COUNT ? k_w_off[i] : DCN41_BAD_OFFSET;
}

uint32_t dcn41_timing_r_offset(uint32_t i)
{
    return i < DCN41_TIMING_R_COUNT ? k_r_off[i] : DCN41_BAD_OFFSET;
}

static uint32_t abs_w(const struct dcn41_dev *dev, uint32_t i, uint32_t otg)
{
    return dcn41_abs(dev, k_w_off[i] + OTG_STRIDE * otg, 2u);
}

static uint32_t abs_r(const struct dcn41_dev *dev, uint32_t i, uint32_t otg)
{
    return dcn41_abs(dev, k_r_off[i] + OTG_STRIDE * otg, 2u);
}

int dcn41_otg_read_timing(struct dcn41_dev *dev, uint32_t otg, struct dcn41_otg_timing *t)
{
    uint32_t i;

    if (!t)
        return DCN41_E_ARG;
    t->valid = 0;
    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (otg >= DCN41_NUM_PIPES)
        return DCN41_E_INST;

    t->otg = otg;
    for (i = 0; i < DCN41_TIMING_W_COUNT; i++) {
        int rc = dcn41_read(dev, abs_w(dev, i, otg), &t->w[i]);
        if (rc)
            return rc;
    }
    for (i = 0; i < DCN41_TIMING_R_COUNT; i++) {
        int rc = dcn41_read(dev, abs_r(dev, i, otg), &t->r[i]);
        if (rc)
            return rc;
    }
    if (dcn41_otg_get_frame_count(dev, otg, &t->frame_count) != DCN41_OK)
        t->frame_count = 0;
    t->valid = 1;
    return DCN41_OK;
}

int dcn41_otg_write_timing(struct dcn41_dev *dev, uint32_t otg, const struct dcn41_otg_timing *t)
{
    uint32_t i;

    if (!t)
        return DCN41_E_ARG;
    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (otg >= DCN41_NUM_PIPES)
        return DCN41_E_INST;
    /* A capture belongs to the pipe it was taken from. Refusing here is what stops one pipe's timing
     * being written into another's - the single worst thing this file could do. */
    if (!t->valid || t->otg != otg)
        return DCN41_E_ARG;

    for (i = 0; i < DCN41_TIMING_W_COUNT; i++) {
        int rc = dcn41_write(dev, abs_w(dev, i, otg), t->w[i]);
        if (rc)
            return rc;
    }
    return DCN41_OK;
}

uint32_t dcn41_otg_timing_diff(const struct dcn41_otg_timing *a, const struct dcn41_otg_timing *b,
                               uint32_t *first_w, uint32_t *first_r)
{
    uint32_t i, n = 0;
    uint32_t fw = DCN41_TIMING_W_COUNT, fr = DCN41_TIMING_R_COUNT;

    if (!a || !b) {
        if (first_w) *first_w = fw;
        if (first_r) *first_r = fr;
        return 0xFFFFFFFFu;
    }
    for (i = 0; i < DCN41_TIMING_W_COUNT; i++) {
        if (a->w[i] != b->w[i]) {
            n++;
            if (fw == DCN41_TIMING_W_COUNT)
                fw = i;
        }
    }
    for (i = 0; i < DCN41_TIMING_R_COUNT; i++) {
        if (a->r[i] != b->r[i]) {
            n++;
            if (fr == DCN41_TIMING_R_COUNT)
                fr = i;
        }
    }
    if (first_w) *first_w = fw;
    if (first_r) *first_r = fr;
    return n;
}

int dcn41_otg_timing_adjust_v_total(struct dcn41_otg_timing *t, int32_t delta_lines)
{
    uint32_t cur, blank_start;
    int64_t next;

    if (!t || !t->valid)
        return DCN41_E_ARG;
    if (delta_lines == 0)
        return DCN41_E_ARG;
    if (delta_lines > DCN41_VTOTAL_MAX_DELTA || delta_lines < -DCN41_VTOTAL_MAX_DELTA)
        return DCN41_E_ARG;

    cur = t->w[DCN41_TW_V_TOTAL] & 0x7FFFu;
    blank_start = t->w[DCN41_TW_V_BLANK_START_END] & 0x7FFFu;
    next = (int64_t)cur + delta_lines;

    /* The register holds v_total - 1, and the vertical blank must start strictly inside the frame.
     * A value at or below the blank start is a raster that cannot scan out; refuse it here rather
     * than discover it on the user's screen. */
    if (next <= (int64_t)blank_start)
        return DCN41_E_ARG;
    if (next < 0 || next > 0x7FFF)
        return DCN41_E_ARG;

    t->w[DCN41_TW_V_TOTAL] = (t->w[DCN41_TW_V_TOTAL] & ~0x7FFFu) | ((uint32_t)next & 0x7FFFu);
    return DCN41_OK;
}

void dcn41_otg_timing_decode(const struct dcn41_otg_timing *t, struct dcn41_timing_decoded *d)
{
    if (!t || !d)
        return;
    /* Field positions from dcn_4_1_0_sh_mask.h; the registers hold total - 1, as optc1_program_timing
     * writes them (dcn41_modes.tsv's otg_* columns are generated from the same formulas). */
    d->h_total = (t->w[DCN41_TW_H_TOTAL] & 0x7FFFu) + 1u;
    d->v_total = (t->w[DCN41_TW_V_TOTAL] & 0x7FFFu) + 1u;
    d->h_blank_start = t->w[DCN41_TW_H_BLANK_START_END] & 0x7FFFu;
    d->h_blank_end = (t->w[DCN41_TW_H_BLANK_START_END] >> 16) & 0x7FFFu;
    d->v_blank_start = t->w[DCN41_TW_V_BLANK_START_END] & 0x7FFFu;
    d->v_blank_end = (t->w[DCN41_TW_V_BLANK_START_END] >> 16) & 0x7FFFu;
    d->h_active = d->h_blank_start > d->h_blank_end ? d->h_blank_start - d->h_blank_end : 0u;
    d->v_active = d->v_blank_start > d->v_blank_end ? d->v_blank_start - d->v_blank_end : 0u;
    d->h_sync_width = (t->w[DCN41_TW_H_SYNC_A] >> 16) & 0x7FFFu;
    d->v_sync_width = (t->w[DCN41_TW_V_SYNC_A] >> 16) & 0x7FFFu;
    d->vstartup = t->w[DCN41_TW_VSTARTUP_PARAM] & 0x3FFu;
    d->vupdate_offset = t->w[DCN41_TW_VUPDATE_PARAM] & 0xFFFFu;
    d->vupdate_width = (t->w[DCN41_TW_VUPDATE_PARAM] >> 16) & 0x3FFu;
    d->vready_offset = t->w[DCN41_TW_VREADY_PARAM] & 0xFFFFu;
    d->master_en = (uint8_t)(t->r[DCN41_TR_OTG_CONTROL] & 1u);
    d->update_lock_held = (uint8_t)(t->r[DCN41_TR_MASTER_UPDATE_LOCK] & 1u);
}
