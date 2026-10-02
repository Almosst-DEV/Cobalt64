/* linux_side.c - the Linux half of the dcn41 register-trace harness.
 *
 * Compiled against the UNMODIFIED Linux DC headers (re/linux-dc, 238650ef6c7c) and linked with the unmodified Linux
 * objects dc_helper.o, irq_service.o, irq_service_dcn401.o, dcn401_hubp.o, dcn20_hubp.o, dcn10_optc.o, dcn30_optc.o
 * and dcn401_optc.o. The register tables are built by Linux's own text: harness.py extracts the register macro block,
 * the optc/hubp register arrays and the dcn401_timing_generator_create / dcn401_hubp_create functions from
 * dc/resource/dcn401/dcn401_resource.c into linux_tables.inc, verbatim, and this file includes it.
 * dm_read_reg_func / dm_write_reg_func (provided by amdgpu_dm in Linux) are the model's recorder here. */
#include "dm_services.h"
#include "dc.h"
#include "reg_helper.h"
#include "dcn401/dcn401_hubp.h"
#include "dcn20/dcn20_hubp.h"
#include "dcn401/dcn401_optc.h"
#include "dcn10/dcn10_optc.h"
#include "dcn30/dcn30_optc.h"
#include "irq/dcn401/irq_service_dcn401.h"
#include "irq/irq_service.h"
#include "dce/dce_audio.h"     /* defines SF(), as dcn401_resource.c gets it */
#include "resource/dcn401/dcn401_resource.h"   /* HUBP_REG_LIST_DCN401_RI, OPTC_COMMON_REG_LIST_DCN401_RI */
#include "dcn/dcn_4_1_0_offset.h"
#include "dcn/dcn_4_1_0_sh_mask.h"

#include "dcn41.h"      /* only for enum dcn41_irq_kind: the neutral names both sides report in */
#include "trace.h"
#include "scen.h"

uint32_t dm_read_reg_func(const struct dc_context *ctx, uint32_t address, const char *func_name)
{
    (void)ctx; (void)func_name;
    return model_read(address);
}

void dm_write_reg_func(const struct dc_context *ctx, uint32_t address, uint32_t value, const char *func_name)
{
    (void)ctx; (void)func_name;
    model_write(address, value);
}

void dcn41_shim_udelay(unsigned long us) { model_delay((uint32_t)us); }
void dcn41_shim_warn(const char *file, int line) { (void)file; (void)line; }

#include "linux_tables.inc"

static uint32_t dcn_offsets[] = { 0x00000012, 0x000000c0, 0x000034c0, 0x00009000, 0x02403c00 };
static struct dc_context g_ctx;
static struct timing_generator *g_tg[4];
static struct hubp *g_hubp[4];
static struct irq_service *g_irqs;

void L_init(void)
{
    struct irq_service_init_data init = { 0 };
    int i;
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.dcn_reg_offsets = dcn_offsets;
    for (i = 0; i < 4; i++) {
        g_tg[i] = dcn401_timing_generator_create(&g_ctx, (uint32_t)i);
        g_hubp[i] = dcn401_hubp_create(&g_ctx, (uint32_t)i);
        if (!g_tg[i] || !g_hubp[i]) { fprintf(stderr, "L_init: create failed\n"); abort(); }
    }
    init.ctx = &g_ctx;
    g_irqs = dal_irq_service_dcn401_create(&init);
    if (!g_irqs) { fprintf(stderr, "L_init: irq service\n"); abort(); }
}

uint32_t L_abs_lock_reg(int otg) { return DCN10TG_FROM_TG(g_tg[otg])->tg_regs->OTG_MASTER_UPDATE_LOCK; }
uint32_t L_abs_position_reg(int otg) { return DCN10TG_FROM_TG(g_tg[otg])->tg_regs->OTG_STATUS_POSITION; }
uint32_t L_abs_otg_control(int otg) { return DCN10TG_FROM_TG(g_tg[otg])->tg_regs->OTG_CONTROL; }
uint32_t L_abs_flip_control(int hubp) { return TO_DCN20_HUBP(g_hubp[hubp])->hubp_regs->DCSURF_FLIP_CONTROL; }
uint32_t L_abs_earliest_inuse(int hubp, int high)
{
    const struct dcn_hubp2_registers *r = TO_DCN20_HUBP(g_hubp[hubp])->hubp_regs;
    return high ? r->DCSURF_SURFACE_EARLIEST_INUSE_HIGH : r->DCSURF_SURFACE_EARLIEST_INUSE;
}

static enum dc_irq_source to_linux_source(int kind, int inst)
{
    switch (kind) {
    case DCN41_IRQ_HPD: return (enum dc_irq_source)(DC_IRQ_SOURCE_HPD1 + inst);
    case DCN41_IRQ_HPD_RX: return (enum dc_irq_source)(DC_IRQ_SOURCE_HPD1RX + inst);
    case DCN41_IRQ_VLINE0: return (enum dc_irq_source)(DC_IRQ_SOURCE_DC1_VLINE0 + inst);
    case DCN41_IRQ_VSTARTUP: return (enum dc_irq_source)(DC_IRQ_SOURCE_VBLANK1 + inst);
    case DCN41_IRQ_PFLIP: return (enum dc_irq_source)(DC_IRQ_SOURCE_PFLIP1 + inst);
    case DCN41_IRQ_VUPDATE_NO_LOCK: return (enum dc_irq_source)(DC_IRQ_SOURCE_VUPDATE1 + inst);
    case DCN41_IRQ_DMCUB_OUTBOX: return DC_IRQ_SOURCE_DMCUB_OUTBOX;
    default: return DC_IRQ_SOURCE_INVALID;
    }
}

static void from_linux_source(enum dc_irq_source s, int64_t *kind, int64_t *inst)
{
    *inst = 0;
    if (s == DC_IRQ_SOURCE_INVALID) { *kind = DCN41_IRQ_NONE; return; }
#define RANGE(first, last, k) if (s >= first && s <= last) { *kind = k; *inst = s - first; return; }
    RANGE(DC_IRQ_SOURCE_HPD1, DC_IRQ_SOURCE_HPD6, DCN41_IRQ_HPD)
    RANGE(DC_IRQ_SOURCE_HPD1RX, DC_IRQ_SOURCE_HPD6RX, DCN41_IRQ_HPD_RX)
    RANGE(DC_IRQ_SOURCE_DC1_VLINE0, DC_IRQ_SOURCE_DC6_VLINE0, DCN41_IRQ_VLINE0)
    RANGE(DC_IRQ_SOURCE_VBLANK1, DC_IRQ_SOURCE_VBLANK6, DCN41_IRQ_VSTARTUP)
    RANGE(DC_IRQ_SOURCE_PFLIP1, DC_IRQ_SOURCE_PFLIP6, DCN41_IRQ_PFLIP)
    RANGE(DC_IRQ_SOURCE_VUPDATE1, DC_IRQ_SOURCE_VUPDATE6, DCN41_IRQ_VUPDATE_NO_LOCK)
#undef RANGE
    if (s == DC_IRQ_SOURCE_DMCUB_OUTBOX) { *kind = DCN41_IRQ_DMCUB_OUTBOX; return; }
    *kind = 1000 + s;     /* a source our layer has no name for: always a mismatch */
}

int L_run(const struct scen_args *a, struct scen_out *o)
{
    struct crtc_position pos;
    struct dc_plane_address addr;
    uint32_t w = 0, h = 0, x = 0, y = 0;
    o->n = 0;
    switch (a->kind) {
    case SC_IRQ_SET:
        o->v[o->n++] = dal_irq_service_set(g_irqs, to_linux_source(a->irq_kind, a->inst), a->enable != 0);
        return 0;
    case SC_IRQ_ACK:
        o->v[o->n++] = dal_irq_service_ack(g_irqs, to_linux_source(a->irq_kind, a->inst));
        return 0;
    case SC_IH_MAP:
        from_linux_source(dal_irq_service_to_irq_source(g_irqs, a->src, a->ext), &o->v[0], &o->v[1]);
        o->n = 2;
        return 0;
    case SC_FLIP:
    case SC_FLIP_PENDING:
        memset(&addr, 0, sizeof(addr));
        addr.type = PLN_ADDR_TYPE_GRAPHICS;
        addr.grph.addr.quad_part = (int64_t)a->addr;
        addr.vmid = (uint8_t)a->vmid;
        addr.tmz_surface = a->tmz != 0;
        o->v[o->n++] = g_hubp[a->inst]->funcs->hubp_program_surface_flip_and_addr(g_hubp[a->inst], &addr,
                                                                                 a->immediate != 0);
        if (a->kind == SC_FLIP)
            return 0;
        if (a->force_latched) {
            model_poke(L_abs_earliest_inuse(a->inst, 0), (uint32_t)a->addr);
            model_poke(L_abs_earliest_inuse(a->inst, 1),
                       (model_peek(L_abs_earliest_inuse(a->inst, 1)) & ~0xFFFFu) | (uint32_t)(a->addr >> 32));
        }
        model_poke(L_abs_flip_control(a->inst),
                   (model_peek(L_abs_flip_control(a->inst)) & ~0x100u) | (a->pending_bit ? 0x100u : 0));
        o->v[o->n++] = g_hubp[a->inst]->funcs->hubp_is_flip_pending(g_hubp[a->inst]);
        return 0;
    case SC_SET_FLIP_INT:
        g_hubp[a->inst]->funcs->hubp_set_flip_int(g_hubp[a->inst]);
        return 0;
    case SC_IN_BLANK:
        o->v[o->n++] = g_hubp[a->inst]->funcs->hubp_in_blank(g_hubp[a->inst]);
        return 0;
    case SC_LOCK:
        optc3_lock(g_tg[a->inst]);
        return 0;
    case SC_UNLOCK:
        optc1_unlock(g_tg[a->inst]);
        return 0;
    case SC_POSITION:
        optc1_get_position(g_tg[a->inst], &pos);
        o->v[0] = pos.horizontal_count; o->v[1] = pos.vertical_count; o->v[2] = pos.nominal_vcount; o->n = 3;
        return 0;
    case SC_FRAME_COUNT:
        o->v[o->n++] = optc1_get_vblank_counter(g_tg[a->inst]);
        return 0;
    case SC_SCANOUTPOS:
        optc1_get_crtc_scanoutpos(g_tg[a->inst], &w, &h, &x, &y);
        o->v[0] = w; o->v[1] = h; o->v[2] = x; o->v[3] = y; o->n = 4;
        return 0;
    case SC_ACTIVE_SIZE:
        model_poke(L_abs_otg_control(a->inst),
                   (model_peek(L_abs_otg_control(a->inst)) & ~1u) | (a->otg_enabled ? 1u : 0u));
        o->v[0] = optc1_get_otg_active_size(g_tg[a->inst], &w, &h);
        o->v[1] = o->v[0] ? w : 0; o->v[2] = o->v[0] ? h : 0; o->n = 3;
        return 0;
    case SC_COUNTER_MOVING:
        o->v[o->n++] = optc1_is_counter_moving(g_tg[a->inst]);
        return 0;
    default:
        return -1;
    }
}
