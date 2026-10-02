/* ours_side.c - the src/dcn41 half of the register-trace harness: the same scenarios through our layer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dcn41.h"
#include "trace.h"
#include "scen.h"

static struct dcn41_dev g_dev;

static uint32_t rr(void *c, uint32_t a) { (void)c; return model_read(a); }
static void ww(void *c, uint32_t a, uint32_t v) { (void)c; model_write(a, v); }
static void dd(void *c, uint32_t us) { (void)c; model_delay(us); }

void O_init(void)
{
    static const uint32_t seg[5] = { 0x00000012, 0x000000c0, 0x000034c0, 0x00009000, 0x02403c00 };
    if (dcn41_dev_init(&g_dev, NULL, rr, ww, dd, seg, MODEL_DWORDS, 0) != DCN41_OK) {
        fprintf(stderr, "O_init: dcn41_dev_init refused\n");
        abort();
    }
}

int O_run(const struct scen_args *a, struct scen_out *o)
{
    struct dcn41_irq irq = { (uint8_t)a->irq_kind, (uint8_t)a->inst };
    struct dcn41_otg_position pos;
    uint32_t w = 0, h = 0, x = 0, y = 0, fc = 0;
    bool b = false;
    int rc = 0;
    o->n = 0;
    switch (a->kind) {
    case SC_IRQ_SET:
        o->v[o->n++] = dcn41_irq_set(&g_dev, irq, a->enable != 0) == DCN41_OK;
        return 0;
    case SC_IRQ_ACK:
        o->v[o->n++] = dcn41_irq_ack(&g_dev, irq) == DCN41_OK;
        return 0;
    case SC_IH_MAP:
        irq = dcn41_ih_to_irq(DCN41_IH_CLIENT_DCE, a->src, a->ext);
        o->v[0] = irq.kind; o->v[1] = irq.inst; o->n = 2;
        return 0;
    case SC_FLIP:
    case SC_FLIP_PENDING:
        o->v[o->n++] = dcn41_hubp_program_flip(&g_dev, (uint32_t)a->inst, a->addr, a->vmid, a->tmz != 0,
                                               a->immediate != 0) == DCN41_OK;
        if (a->kind == SC_FLIP)
            return 0;
        if (a->force_latched) {
            model_poke(L_abs_earliest_inuse(a->inst, 0), (uint32_t)a->addr);
            model_poke(L_abs_earliest_inuse(a->inst, 1),
                       (model_peek(L_abs_earliest_inuse(a->inst, 1)) & ~0xFFFFu) | (uint32_t)(a->addr >> 32));
        }
        model_poke(L_abs_flip_control(a->inst),
                   (model_peek(L_abs_flip_control(a->inst)) & ~0x100u) | (a->pending_bit ? 0x100u : 0));
        rc = dcn41_hubp_is_flip_pending(&g_dev, (uint32_t)a->inst, &b, NULL);
        o->v[o->n++] = (rc == DCN41_OK || rc == DCN41_E_NOREQ) ? b : -1;
        return 0;
    case SC_SET_FLIP_INT:
        return dcn41_hubp_set_flip_int(&g_dev, (uint32_t)a->inst) == DCN41_OK ? 0 : -1;
    case SC_IN_BLANK:
        rc = dcn41_hubp_in_blank(&g_dev, (uint32_t)a->inst, &b);
        o->v[o->n++] = rc == DCN41_OK ? b : -1;
        return 0;
    case SC_LOCK:
        rc = dcn41_otg_lock(&g_dev, (uint32_t)a->inst);
        return (rc == DCN41_OK || rc == DCN41_E_TIMEOUT) ? 0 : -1;
    case SC_UNLOCK:
        return dcn41_otg_unlock(&g_dev, (uint32_t)a->inst) == DCN41_OK ? 0 : -1;
    case SC_POSITION:
        if (dcn41_otg_get_position(&g_dev, (uint32_t)a->inst, &pos) != DCN41_OK) return -1;
        o->v[0] = pos.horizontal_count; o->v[1] = pos.vertical_count; o->v[2] = pos.nominal_vcount; o->n = 3;
        return 0;
    case SC_FRAME_COUNT:
        if (dcn41_otg_get_frame_count(&g_dev, (uint32_t)a->inst, &fc) != DCN41_OK) return -1;
        o->v[o->n++] = fc;
        return 0;
    case SC_SCANOUTPOS:
        if (dcn41_otg_get_scanoutpos(&g_dev, (uint32_t)a->inst, &w, &h, &x, &y) != DCN41_OK) return -1;
        o->v[0] = w; o->v[1] = h; o->v[2] = x; o->v[3] = y; o->n = 4;
        return 0;
    case SC_ACTIVE_SIZE:
        model_poke(L_abs_otg_control(a->inst),
                   (model_peek(L_abs_otg_control(a->inst)) & ~1u) | (a->otg_enabled ? 1u : 0u));
        if (dcn41_otg_get_active_size(&g_dev, (uint32_t)a->inst, &b, &w, &h) != DCN41_OK) return -1;
        o->v[0] = b; o->v[1] = b ? w : 0; o->v[2] = b ? h : 0; o->n = 3;
        return 0;
    case SC_COUNTER_MOVING:
        if (dcn41_otg_is_counter_moving(&g_dev, (uint32_t)a->inst, &b) != DCN41_OK) return -1;
        o->v[o->n++] = b;
        return 0;
    default:
        return -1;
    }
}
