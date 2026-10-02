/*
 * dcn41_otg.c - DCN 4.1 OTG master update lock, scanout position, frame counter and active-size readout.
 *
 * Mirrors Linux 238650ef6c7c dc/optc/dcn30/dcn30_optc.c optc3_lock and dc/optc/dcn10/dcn10_optc.c optc1_unlock,
 * optc1_get_position, optc1_get_vblank_counter, optc1_get_crtc_scanoutpos, optc1_get_otg_active_size and
 * optc1_is_counter_moving (the dcn401_tg_funcs entries in dc/optc/dcn401/dcn401_optc.c), register for register.
 */
#include "dcn41_io.h"

#pragma GCC poison float double   /* after the includes: <stddef.h> itself names long double */

int dcn41_otg_lock(struct dcn41_dev *dev, uint32_t otg)
{
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (otg >= DCN41_NUM_PIPES)
        return DCN41_E_INST;
    /* REG_UPDATE(OTG_GLOBAL_CONTROL2, OTG_MASTER_UPDATE_LOCK_SEL, optc->inst); */
    rc = dcn41_update(dev, DCN41_ADDR_I(dev, DCN41_OTG_OTG_GLOBAL_CONTROL2, otg),
                      DCN41_M(DCN41_OTG_OTG_GLOBAL_CONTROL2, OTG_MASTER_UPDATE_LOCK_SEL),
                      DCN41_FV(DCN41_OTG_OTG_GLOBAL_CONTROL2, OTG_MASTER_UPDATE_LOCK_SEL, otg));
    if (rc)
        return rc;
    /* REG_SET(OTG_MASTER_UPDATE_LOCK, 0, OTG_MASTER_UPDATE_LOCK, 1); */
    rc = dcn41_set(dev, DCN41_ADDR_I(dev, DCN41_OTG_OTG_MASTER_UPDATE_LOCK, otg), 0,
                   DCN41_M(DCN41_OTG_OTG_MASTER_UPDATE_LOCK, OTG_MASTER_UPDATE_LOCK),
                   DCN41_FV(DCN41_OTG_OTG_MASTER_UPDATE_LOCK, OTG_MASTER_UPDATE_LOCK, 1));
    if (rc)
        return rc;
    /* REG_WAIT(OTG_MASTER_UPDATE_LOCK, UPDATE_LOCK_STATUS, 1, 1, 10); */
    return dcn41_wait(dev, DCN41_ADDR_I(dev, DCN41_OTG_OTG_MASTER_UPDATE_LOCK, otg),
                      DCN41_M(DCN41_OTG_OTG_MASTER_UPDATE_LOCK, UPDATE_LOCK_STATUS),
                      DCN41_OTG_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS__SHIFT, 1, 1, 10);
}

int dcn41_otg_unlock(struct dcn41_dev *dev, uint32_t otg)
{
    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (otg >= DCN41_NUM_PIPES)
        return DCN41_E_INST;
    /* REG_SET(OTG_MASTER_UPDATE_LOCK, 0, OTG_MASTER_UPDATE_LOCK, 0); */
    return dcn41_set(dev, DCN41_ADDR_I(dev, DCN41_OTG_OTG_MASTER_UPDATE_LOCK, otg), 0,
                     DCN41_M(DCN41_OTG_OTG_MASTER_UPDATE_LOCK, OTG_MASTER_UPDATE_LOCK),
                     DCN41_FV(DCN41_OTG_OTG_MASTER_UPDATE_LOCK, OTG_MASTER_UPDATE_LOCK, 0));
}

int dcn41_otg_get_position(struct dcn41_dev *dev, uint32_t otg, struct dcn41_otg_position *pos)
{
    uint32_t v = 0, nom = 0;
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (!pos)
        return DCN41_E_ARG;
    if (otg >= DCN41_NUM_PIPES)
        return DCN41_E_INST;
    /* REG_GET_2(OTG_STATUS_POSITION, OTG_HORZ_COUNT, ..., OTG_VERT_COUNT, ...): one read, two fields */
    rc = dcn41_read(dev, DCN41_ADDR_I(dev, DCN41_OTG_OTG_STATUS_POSITION, otg), &v);
    if (rc)
        return rc;
    /* REG_GET(OTG_NOM_VERT_POSITION, OTG_VERT_COUNT_NOM, ...) */
    rc = dcn41_get(dev, DCN41_ADDR_I(dev, DCN41_OTG_OTG_NOM_VERT_POSITION, otg),
                   DCN41_M(DCN41_OTG_OTG_NOM_VERT_POSITION, OTG_VERT_COUNT_NOM),
                   DCN41_OTG_OTG_NOM_VERT_POSITION__OTG_VERT_COUNT_NOM__SHIFT, &nom);
    if (rc)
        return rc;
    pos->horizontal_count = DCN41_FG(DCN41_OTG_OTG_STATUS_POSITION, OTG_HORZ_COUNT, v);
    pos->vertical_count = DCN41_FG(DCN41_OTG_OTG_STATUS_POSITION, OTG_VERT_COUNT, v);
    pos->nominal_vcount = nom;
    return DCN41_OK;
}

int dcn41_otg_get_frame_count(struct dcn41_dev *dev, uint32_t otg, uint32_t *frame_count)
{
    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (!frame_count)
        return DCN41_E_ARG;
    if (otg >= DCN41_NUM_PIPES)
        return DCN41_E_INST;
    /* REG_GET(OTG_STATUS_FRAME_COUNT, OTG_FRAME_COUNT, &frame_count); */
    return dcn41_get(dev, DCN41_ADDR_I(dev, DCN41_OTG_OTG_STATUS_FRAME_COUNT, otg),
                     DCN41_M(DCN41_OTG_OTG_STATUS_FRAME_COUNT, OTG_FRAME_COUNT),
                     DCN41_OTG_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT__SHIFT, frame_count);
}

int dcn41_otg_get_scanoutpos(struct dcn41_dev *dev, uint32_t otg, uint32_t *v_blank_start, uint32_t *v_blank_end,
                             uint32_t *h_position, uint32_t *v_position)
{
    struct dcn41_otg_position pos;
    uint32_t v = 0;
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (!v_blank_start || !v_blank_end || !h_position || !v_position)
        return DCN41_E_ARG;
    if (otg >= DCN41_NUM_PIPES)
        return DCN41_E_INST;
    /* REG_GET_2(OTG_V_BLANK_START_END, OTG_V_BLANK_START, ..., OTG_V_BLANK_END, ...); optc1_get_position(...) */
    rc = dcn41_read(dev, DCN41_ADDR_I(dev, DCN41_OTG_OTG_V_BLANK_START_END, otg), &v);
    if (rc)
        return rc;
    rc = dcn41_otg_get_position(dev, otg, &pos);
    if (rc)
        return rc;
    *v_blank_start = DCN41_FG(DCN41_OTG_OTG_V_BLANK_START_END, OTG_V_BLANK_START, v);
    *v_blank_end = DCN41_FG(DCN41_OTG_OTG_V_BLANK_START_END, OTG_V_BLANK_END, v);
    *h_position = pos.horizontal_count;
    *v_position = pos.vertical_count;
    return DCN41_OK;
}

int dcn41_otg_get_active_size(struct dcn41_dev *dev, uint32_t otg, bool *enabled, uint32_t *width, uint32_t *height)
{
    uint32_t en = 0, vb = 0, hb = 0;
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (!enabled || !width || !height)
        return DCN41_E_ARG;
    if (otg >= DCN41_NUM_PIPES)
        return DCN41_E_INST;
    rc = dcn41_get(dev, DCN41_ADDR_I(dev, DCN41_OTG_OTG_CONTROL, otg), DCN41_M(DCN41_OTG_OTG_CONTROL, OTG_MASTER_EN),
                   DCN41_OTG_OTG_CONTROL__OTG_MASTER_EN__SHIFT, &en);
    if (rc)
        return rc;
    *enabled = en != 0;
    if (!en)
        return DCN41_OK;
    rc = dcn41_read(dev, DCN41_ADDR_I(dev, DCN41_OTG_OTG_V_BLANK_START_END, otg), &vb);
    if (rc)
        return rc;
    rc = dcn41_read(dev, DCN41_ADDR_I(dev, DCN41_OTG_OTG_H_BLANK_START_END, otg), &hb);
    if (rc)
        return rc;
    /* *otg_active_width = h_blank_start - h_blank_end; *otg_active_height = v_blank_start - v_blank_end; */
    *width = DCN41_FG(DCN41_OTG_OTG_H_BLANK_START_END, OTG_H_BLANK_START, hb) -
             DCN41_FG(DCN41_OTG_OTG_H_BLANK_START_END, OTG_H_BLANK_END, hb);
    *height = DCN41_FG(DCN41_OTG_OTG_V_BLANK_START_END, OTG_V_BLANK_START, vb) -
              DCN41_FG(DCN41_OTG_OTG_V_BLANK_START_END, OTG_V_BLANK_END, vb);
    return DCN41_OK;
}

int dcn41_otg_is_counter_moving(struct dcn41_dev *dev, uint32_t otg, bool *moving)
{
    struct dcn41_otg_position p1, p2;
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (!moving)
        return DCN41_E_ARG;
    rc = dcn41_otg_get_position(dev, otg, &p1);
    if (rc)
        return rc;
    rc = dcn41_otg_get_position(dev, otg, &p2);
    if (rc)
        return rc;
    *moving = !(p1.horizontal_count == p2.horizontal_count && p1.vertical_count == p2.vertical_count);
    return DCN41_OK;
}

int dcn41_otg_get_totals(struct dcn41_dev *dev, uint32_t otg, uint32_t *h_total_minus1, uint32_t *v_total_minus1)
{
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (!h_total_minus1 || !v_total_minus1)
        return DCN41_E_ARG;
    if (otg >= DCN41_NUM_PIPES)
        return DCN41_E_INST;
    rc = dcn41_get(dev, DCN41_ADDR_I(dev, DCN41_OTG_OTG_H_TOTAL, otg), DCN41_M(DCN41_OTG_OTG_H_TOTAL, OTG_H_TOTAL),
                   DCN41_OTG_OTG_H_TOTAL__OTG_H_TOTAL__SHIFT, h_total_minus1);
    if (rc)
        return rc;
    return dcn41_get(dev, DCN41_ADDR_I(dev, DCN41_OTG_OTG_V_TOTAL, otg), DCN41_M(DCN41_OTG_OTG_V_TOTAL, OTG_V_TOTAL),
                     DCN41_OTG_OTG_V_TOTAL__OTG_V_TOTAL__SHIFT, v_total_minus1);
}
