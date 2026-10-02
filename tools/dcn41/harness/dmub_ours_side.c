/* dmub_ours_side.c - the src/dcn41/dcn41_dmub.c half of the DMUB harness. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dcn41_dmub.h"
#include "trace.h"
#include "dmub_scen.h"

static struct dcn41_dev g_dev;
static uint8_t *g_ring;
#define RING_ADDR 0x80003fe000000ull   /* an arbitrary GPU address for ring byte 0 in the harness */

static uint32_t rr(void *c, uint32_t a) { (void)c; return model_read(a); }
static void ww(void *c, uint32_t a, uint32_t v) { (void)c; model_write(a, v); }
static void dd(void *c, uint32_t us) { (void)c; model_delay(us); }
static uint32_t mr(void *c, uint64_t addr)
{
    uint32_t v;
    (void)c;
    memcpy(&v, g_ring + (addr - RING_ADDR), 4);
    return v;
}
static void mw(void *c, uint64_t addr, uint32_t v) { (void)c; memcpy(g_ring + (addr - RING_ADDR), &v, 4); }

void DO_init(void)
{
    static const uint32_t seg[5] = { 0x00000012, 0x000000c0, 0x000034c0, 0x00009000, 0x02403c00 };
    if (dcn41_dev_init(&g_dev, NULL, rr, ww, dd, seg, MODEL_DWORDS, 0) != DCN41_OK) abort();
}

int DO_run(const struct dmub_scen *s, const struct dmub_atom *a, struct dmub_out *o, uint8_t *ring, uint32_t ring_size)
{
    struct dcn41_dmub_cmd cmd;
    struct dcn41_dmub_ring rg;
    struct dcn41_dmub_state st;
    uint32_t i;
    int rc;

    memset(o, 0, sizeof(*o));
    g_ring = ring;
    switch (s->kind) {
    case DS_TRANSMITTER:
        dcn41_dmub_build_transmitter_control(&cmd, a->phyid, a->action, a->mode_laneset, a->lanenum, s->pixel_clock_khz,
                                             a->hpdsel, a->digfe_sel, a->connobj_id, a->hpo_instance, a->skip_ssc);
        break;
    case DS_ENCODER:
        dcn41_dmub_build_encoder_control(&cmd, a->digid, a->action, a->digmode, a->lanenum, s->pixel_clock_khz,
                                         a->bitpercolor, a->is_hdmi != 0);
        break;
    case DS_PIXEL_CLOCK:
        dcn41_dmub_build_set_pixel_clock(&cmd, s->pixel_clock_100hz, a->pll_id, a->encoderobjid, a->encoder_mode,
                                         a->miscinfo, a->crtc_id, a->deep_color_ratio);
        break;
    case DS_POWER_GATING:
        dcn41_dmub_build_disp_power_gating(&cmd, a->pg_pipe, a->pg_enable);
        break;
    case DS_GPINT: {
        uint32_t resp = 0;
        o->v[0] = dcn41_dmub_gpint(&g_dev, s->gpint_cmd, s->gpint_param, s->timeout) == DCN41_OK;
        o->v[1] = dcn41_dmub_gpint_response(&g_dev, &resp) == DCN41_OK;
        o->v[2] = resp;
        o->nv = 3;
        return 0;
    }
    case DS_RING_SUBMIT:
    case DS_WAIT_IDLE:
    case DS_RETURN_DATA:
        /* a live, idle firmware as dcn41_dmub_probe would classify it, with the pointers the model holds */
        memset(&st, 0, sizeof(st));
        st.verdict = DCN41_DMUB_ALIVE_IDLE;
        st.ring_map = DCN41_DMUB_MAP_CW4;
        st.ring_addr = RING_ADDR;
        st.inbox1_size = ring_size;
        st.inbox1_rptr = model_read(dcn41_abs(&g_dev, DCN41_DMCUB_INBOX1_RPTR, 2));   /* dcn41_dmub_probe's reads, in */
        st.inbox1_wptr = model_read(dcn41_abs(&g_dev, DCN41_DMCUB_INBOX1_WPTR, 2));   /* dmub_srv_sync_inbox1's order */
        o->v[0] = dcn41_dmub_ring_attach(&rg, &g_dev, &st, mr, mw, NULL) == DCN41_OK ? 0 : 99;
        o->nv = 1;
        if (s->kind == DS_RING_SUBMIT) {
            for (i = 0; i < s->ncmds && o->nv < 8; i++) {
                dcn41_dmub_cmd_zero(&cmd);
                cmd.header = dcn41_dmub_header(DCN41_DMUB_CMD_VBIOS, i % 4, 60, false);
                memset(cmd.payload, (int)(0x11 * (i + 1)), sizeof(cmd.payload));
                rc = dcn41_dmub_ring_submit(&rg, &cmd);
                o->v[o->nv++] = rc == DCN41_OK ? 0 : rc == DCN41_E_FULL ? 1 : 2;
                if (rc != DCN41_OK)
                    break;
            }
        } else if (s->kind == DS_WAIT_IDLE) {
            rc = dcn41_dmub_ring_wait_idle(&rg, s->timeout);
            o->v[o->nv++] = rc == DCN41_OK ? 0 : rc == DCN41_E_TIMEOUT ? 1 : 2;
        } else {
            dcn41_dmub_ring_return_data(&rg, &cmd);
            memcpy(o->bytes, &cmd, 64);
            o->nbytes = 64;
        }
        return 0;
    case DS_FW_META: {
        struct dcn41_dmub_fw_meta_info info;
        static uint8_t blob[4096];
        uint32_t at = s->meta_blob_size - s->meta_at - 64;
        uint32_t magic = 0x444D5542u, ver = 0x0a0b0c0d, reg = 0x10000, trace = 0x20000;
        memset(blob, 0x5a, sizeof(blob));
        memcpy(blob + at, &magic, 4); memcpy(blob + at + 4, &reg, 4); memcpy(blob + at + 8, &trace, 4);
        memcpy(blob + at + 12, &ver, 4);
        memset(&info, 0, sizeof(info));
        rc = s->bss ? dcn41_dmub_fw_meta_from_blob(blob, s->meta_blob_size, 0x24, &info)
                    : dcn41_dmub_fw_meta_scan(blob, s->meta_blob_size, &info);
        o->v[0] = rc == DCN41_OK;
        o->v[1] = rc == DCN41_OK ? info.fw_version : 0;
        o->v[2] = rc == DCN41_OK ? info.fw_region_size : 0;
        o->nv = 3;
        return 0;
    }
    default:
        return -1;
    }
    memcpy(o->bytes, &cmd, 64);
    o->nbytes = 64;
    return 0;
}
