/*
 * dcn41_irq.c - DCN 4.1 display interrupt sources: IH classification, enable/disable, acknowledge, status.
 *
 * Mirrors Linux 238650ef6c7c dc/irq/dcn401/irq_service_dcn401.c (the source switch and the IRQ_REG_ENTRY table) and
 * dc/irq/irq_service.c (dal_irq_service_set/_ack, *_generic, hpd0_ack). See dcn41.h for the deliberate differences.
 */
#include "dcn41_io.h"

#pragma GCC poison float double   /* after the includes: <stddef.h> itself names long double */

struct dcn41_irq dcn41_ih_to_irq(uint32_t client_id, uint32_t src_id, uint32_t src_data0)
{
    struct dcn41_irq r = { DCN41_IRQ_NONE, 0 };

    if (client_id != DCN41_IH_CLIENT_DCE)
        return r;
    if (src_id >= DCN41_IH_SRC_VSTARTUP_OTG0 && src_id <= DCN41_IH_SRC_VSTARTUP_OTG0 + 5) {
        r.kind = DCN41_IRQ_VSTARTUP;                                  /* DC_D1..D6_OTG_VSTARTUP -> VBLANK1..6 */
        r.inst = (uint8_t)(src_id - DCN41_IH_SRC_VSTARTUP_OTG0);
    } else if (src_id >= DCN41_IH_SRC_VLINE0_OTG0 && src_id <= DCN41_IH_SRC_VLINE0_OTG0 + 4) {
        r.kind = DCN41_IRQ_VLINE0;                                    /* OTG1..5_VERTICAL_INTERRUPT0 -> DC1..5_VLINE0 */
        r.inst = (uint8_t)(src_id - DCN41_IH_SRC_VLINE0_OTG0);
    } else if (src_id == DCN41_IH_SRC_VLINE0_OTG5) {
        r.kind = DCN41_IRQ_VLINE0;                                    /* OTG6_VERTICAL_INTERRUPT0 -> DC6_VLINE0 */
        r.inst = 5;
    } else if (src_id >= DCN41_IH_SRC_PFLIP_HUBP0 && src_id <= DCN41_IH_SRC_PFLIP_HUBP0 + 5) {
        r.kind = DCN41_IRQ_PFLIP;                                     /* HUBP0..5_FLIP_INTERRUPT -> PFLIP1..6 */
        r.inst = (uint8_t)(src_id - DCN41_IH_SRC_PFLIP_HUBP0);
    } else if (src_id >= DCN41_IH_SRC_VUPDATE_NO_LOCK_OTG0 && src_id <= DCN41_IH_SRC_VUPDATE_NO_LOCK_OTG0 + 5) {
        r.kind = DCN41_IRQ_VUPDATE_NO_LOCK;                           /* OTG0..5_IHC_V_UPDATE_NO_LOCK -> VUPDATE1..6 */
        r.inst = (uint8_t)(src_id - DCN41_IH_SRC_VUPDATE_NO_LOCK_OTG0);
    } else if (src_id == DCN41_IH_SRC_DMCUB_OUTBOX) {
        r.kind = DCN41_IRQ_DMCUB_OUTBOX;
    } else if (src_id == DCN41_IH_SRC_HPD) {
        /* "generic src_id for all HPD and HPDRX interrupts": DCN_1_0__CTXID__DC_HPD1..6_INT 0..5, _RX_INT 6..11 */
        if (src_data0 <= 5) {
            r.kind = DCN41_IRQ_HPD;
            r.inst = (uint8_t)src_data0;
        } else if (src_data0 <= 11) {
            r.kind = DCN41_IRQ_HPD_RX;
            r.inst = (uint8_t)(src_data0 - 6);
        }
    }
    return r;
}

/* one irq_source_info_dcn401[] row, resolved to absolute addresses */
struct dcn41_irq_row {
    uint32_t enable_reg, enable_mask, ack_reg, ack_mask, status_reg;
    bool hpd0_ack;
};

static int dcn41_irq_row(struct dcn41_dev *dev, struct dcn41_irq irq, struct dcn41_irq_row *row)
{
    uint32_t i = irq.inst;

    row->status_reg = DCN41_BAD_OFFSET;
    row->hpd0_ack = false;
    switch (irq.kind) {
    case DCN41_IRQ_VSTARTUP:          /* vblank_int_entry: OTG_GLOBAL_SYNC_STATUS VSTARTUP_INT_EN / VSTARTUP_EVENT_CLEAR */
        if (i >= DCN41_NUM_PIPES)
            return DCN41_E_INST;
        row->enable_reg = row->ack_reg = DCN41_ADDR_I(dev, DCN41_OTG_OTG_GLOBAL_SYNC_STATUS, i);
        row->enable_mask = DCN41_M(DCN41_OTG_OTG_GLOBAL_SYNC_STATUS, VSTARTUP_INT_EN);
        row->ack_mask = DCN41_M(DCN41_OTG_OTG_GLOBAL_SYNC_STATUS, VSTARTUP_EVENT_CLEAR);
        break;
    case DCN41_IRQ_VUPDATE_NO_LOCK:   /* vupdate_no_lock_int_entry: VUPDATE_NO_LOCK_INT_EN / VUPDATE_NO_LOCK_EVENT_CLEAR */
        if (i >= DCN41_NUM_PIPES)
            return DCN41_E_INST;
        row->enable_reg = row->ack_reg = DCN41_ADDR_I(dev, DCN41_OTG_OTG_GLOBAL_SYNC_STATUS, i);
        row->enable_mask = DCN41_M(DCN41_OTG_OTG_GLOBAL_SYNC_STATUS, VUPDATE_NO_LOCK_INT_EN);
        row->ack_mask = DCN41_M(DCN41_OTG_OTG_GLOBAL_SYNC_STATUS, VUPDATE_NO_LOCK_EVENT_CLEAR);
        break;
    case DCN41_IRQ_PFLIP:             /* pflip_int_entry: DCSURF_SURFACE_FLIP_INTERRUPT SURFACE_FLIP_INT_MASK / _CLEAR */
        if (i >= DCN41_NUM_PIPES)
            return DCN41_E_INST;
        row->enable_reg = row->ack_reg = DCN41_ADDR_I(dev, DCN41_HUBPREQ_DCSURF_SURFACE_FLIP_INTERRUPT, i);
        row->enable_mask = DCN41_M(DCN41_HUBPREQ_DCSURF_SURFACE_FLIP_INTERRUPT, SURFACE_FLIP_INT_MASK);
        row->ack_mask = DCN41_M(DCN41_HUBPREQ_DCSURF_SURFACE_FLIP_INTERRUPT, SURFACE_FLIP_CLEAR);
        break;
    case DCN41_IRQ_HPD:               /* hpd_int_entry: DC_HPD_INT_CONTROL DC_HPD_INT_EN / DC_HPD_INT_ACK, ack hpd0_ack */
        if (i >= DCN41_NUM_HPD)
            return DCN41_E_INST;
        row->enable_reg = row->ack_reg = DCN41_ADDR_I(dev, DCN41_HPD_DC_HPD_INT_CONTROL, i);
        row->enable_mask = DCN41_M(DCN41_HPD_DC_HPD_INT_CONTROL, DC_HPD_INT_EN);
        row->ack_mask = DCN41_M(DCN41_HPD_DC_HPD_INT_CONTROL, DC_HPD_INT_ACK);
        row->status_reg = DCN41_ADDR_I(dev, DCN41_HPD_DC_HPD_INT_STATUS, i);
        row->hpd0_ack = true;
        break;
    case DCN41_IRQ_HPD_RX:            /* hpd_rx_int_entry: DC_HPD_RX_INT_EN / DC_HPD_RX_INT_ACK, generic ack */
        if (i >= DCN41_NUM_HPD)
            return DCN41_E_INST;
        row->enable_reg = row->ack_reg = DCN41_ADDR_I(dev, DCN41_HPD_DC_HPD_INT_CONTROL, i);
        row->enable_mask = DCN41_M(DCN41_HPD_DC_HPD_INT_CONTROL, DC_HPD_RX_INT_EN);
        row->ack_mask = DCN41_M(DCN41_HPD_DC_HPD_INT_CONTROL, DC_HPD_RX_INT_ACK);
        break;
    case DCN41_IRQ_DMCUB_OUTBOX:      /* dmub_outbox_int_entry: DMCUB_INTERRUPT_ENABLE / DMCUB_INTERRUPT_ACK OUTBOX1_READY */
        if (i != 0)
            return DCN41_E_INST;
        row->enable_reg = DCN41_ADDR(dev, DCN41_DMCUB_INTERRUPT_ENABLE);
        row->enable_mask = DCN41_M(DCN41_DMCUB_INTERRUPT_ENABLE, DMCUB_OUTBOX1_READY_INT_EN);
        row->ack_reg = DCN41_ADDR(dev, DCN41_DMCUB_INTERRUPT_ACK);
        row->ack_mask = DCN41_M(DCN41_DMCUB_INTERRUPT_ACK, DMCUB_OUTBOX1_READY_INT_ACK);
        break;
    default:                          /* VLINE0 and NONE: classified by dcn41_ih_to_irq, not driven by this layer */
        return DCN41_E_ARG;
    }
    if (row->enable_reg == DCN41_BAD_OFFSET || row->ack_reg == DCN41_BAD_OFFSET ||
        (row->hpd0_ack && row->status_reg == DCN41_BAD_OFFSET))
        return DCN41_E_WINDOW;
    return DCN41_OK;
}

static int dcn41_irq_ack_row(struct dcn41_dev *dev, const struct dcn41_irq_row *row)
{
    int rc;
    uint32_t sense = 0, v = 0;

    if (!row->hpd0_ack)       /* dal_irq_service_ack_generic: (v & ~ack_mask) | (ack_value & ack_mask), ack_value = mask */
        return dcn41_update(dev, row->ack_reg, row->ack_mask, row->ack_mask);
    /* hpd0_ack: read DC_HPD_SENSE_DELAYED, generic ack, then DC_HPD_INT_POLARITY = sense ? 0 : 1 */
    rc = dcn41_get(dev, row->status_reg, DCN41_M(DCN41_HPD_DC_HPD_INT_STATUS, DC_HPD_SENSE_DELAYED),
                   DCN41_HPD_DC_HPD_INT_STATUS__DC_HPD_SENSE_DELAYED__SHIFT, &sense);
    if (rc)
        return rc;
    rc = dcn41_update(dev, row->ack_reg, row->ack_mask, row->ack_mask);
    if (rc)
        return rc;
    rc = dcn41_read(dev, row->enable_reg, &v);
    if (rc)
        return rc;
    v = (v & ~DCN41_M(DCN41_HPD_DC_HPD_INT_CONTROL, DC_HPD_INT_POLARITY)) |
        DCN41_FV(DCN41_HPD_DC_HPD_INT_CONTROL, DC_HPD_INT_POLARITY, sense ? 0 : 1);
    return dcn41_write(dev, row->enable_reg, v);
}

int dcn41_irq_ack(struct dcn41_dev *dev, struct dcn41_irq irq)
{
    struct dcn41_irq_row row;
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    rc = dcn41_irq_row(dev, irq, &row);
    if (rc)
        return rc;
    return dcn41_irq_ack_row(dev, &row);
}

int dcn41_irq_set(struct dcn41_dev *dev, struct dcn41_irq irq, bool enable)
{
    struct dcn41_irq_row row;
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    rc = dcn41_irq_row(dev, irq, &row);
    if (rc)
        return rc;
    rc = dcn41_irq_ack_row(dev, &row);      /* dal_irq_service_set acknowledges before it sets */
    if (rc)
        return rc;
    /* dal_irq_service_set_generic: enable_value[0] = mask (enable), enable_value[1] = ~mask (disable) */
    return dcn41_update(dev, row.enable_reg, row.enable_mask, enable ? row.enable_mask : 0);
}

int dcn41_otg_irq_status(struct dcn41_dev *dev, uint32_t otg, struct dcn41_otg_irq_status *st)
{
    int rc;
    uint32_t v = 0;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (!st)
        return DCN41_E_ARG;
    if (otg >= DCN41_NUM_PIPES)
        return DCN41_E_INST;
    rc = dcn41_read(dev, DCN41_ADDR_I(dev, DCN41_OTG_OTG_GLOBAL_SYNC_STATUS, otg), &v);
    if (rc)
        return rc;
    st->raw = v;
    st->vstartup_int_en = (uint8_t)DCN41_FG(DCN41_OTG_OTG_GLOBAL_SYNC_STATUS, VSTARTUP_INT_EN, v);
    st->vstartup_occurred = (uint8_t)DCN41_FG(DCN41_OTG_OTG_GLOBAL_SYNC_STATUS, VSTARTUP_EVENT_OCCURRED, v);
    st->vstartup_int_status = (uint8_t)DCN41_FG(DCN41_OTG_OTG_GLOBAL_SYNC_STATUS, VSTARTUP_INT_STATUS, v);
    st->vupdate_no_lock_int_en = (uint8_t)DCN41_FG(DCN41_OTG_OTG_GLOBAL_SYNC_STATUS, VUPDATE_NO_LOCK_INT_EN, v);
    st->vupdate_no_lock_occurred =
        (uint8_t)DCN41_FG(DCN41_OTG_OTG_GLOBAL_SYNC_STATUS, VUPDATE_NO_LOCK_EVENT_OCCURRED, v);
    st->vupdate_no_lock_int_status = (uint8_t)DCN41_FG(DCN41_OTG_OTG_GLOBAL_SYNC_STATUS, VUPDATE_NO_LOCK_INT_STATUS, v);
    return DCN41_OK;
}

int dcn41_hubp_flip_irq_status(struct dcn41_dev *dev, uint32_t hubp, struct dcn41_flip_irq_status *st)
{
    int rc;
    uint32_t v = 0;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (!st)
        return DCN41_E_ARG;
    if (hubp >= DCN41_NUM_PIPES)
        return DCN41_E_INST;
    rc = dcn41_read(dev, DCN41_ADDR_I(dev, DCN41_HUBPREQ_DCSURF_SURFACE_FLIP_INTERRUPT, hubp), &v);
    if (rc)
        return rc;
    st->raw = v;
    st->int_mask = (uint8_t)DCN41_FG(DCN41_HUBPREQ_DCSURF_SURFACE_FLIP_INTERRUPT, SURFACE_FLIP_INT_MASK, v);
    st->int_type = (uint8_t)DCN41_FG(DCN41_HUBPREQ_DCSURF_SURFACE_FLIP_INTERRUPT, SURFACE_FLIP_INT_TYPE, v);
    st->occurred = (uint8_t)DCN41_FG(DCN41_HUBPREQ_DCSURF_SURFACE_FLIP_INTERRUPT, SURFACE_FLIP_OCCURRED, v);
    st->int_status = (uint8_t)DCN41_FG(DCN41_HUBPREQ_DCSURF_SURFACE_FLIP_INTERRUPT, SURFACE_FLIP_INT_STATUS, v);
    return DCN41_OK;
}
