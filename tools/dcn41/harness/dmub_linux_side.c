/* dmub_linux_side.c - the Linux half of the DMUB harness.
 *
 * Linked with UNMODIFIED Linux objects: dc/bios/command_table2.c, command_table_helper2.c,
 * dce112/command_table_helper2_dce112.c (the DMUB command builders and DC->ATOM translations), dmub/src/dmub_srv.c,
 * dmub_dcn401.c and dmub_reg.c (GPINT, the inbox1 ring queue/execute/idle wait, fw meta lookup). The bios parser is
 * set up the way the vendored Sapphire ROM makes Linux set it up: no digxencodercontrol / setpixelclock /
 * enabledisppowergating / dig1transmittercontrol command tables (their master-table entries are 0, see
 * notes/DISPLAY-DESIGN.md), so amdgpu_atom_parse_cmd_header() fails and every call takes the *_fallback -> DMUB path. */
#include "dm_services.h"
#include "dc.h"
#include "atomfirmware.h"
#include "atom.h"
#include "amdgpu.h"
#include "include/bios_parser_interface.h"
#include "bios/command_table2.h"
#include "bios/command_table_helper2.h"
#include "bios/bios_parser_types_internal2.h"
#include "dc_dmub_srv.h"
#include "dmub/dmub_srv.h"
#include "dmub/src/dmub_dcn401.h"

#include "trace.h"
#include "dmub_scen.h"

/* ---- stand-ins for the amdgpu/DM functions the compiled Linux files call ---- */
static uint8_t captured[64];
static int captured_n;

bool dc_wake_and_execute_dmub_cmd(const struct dc_context *ctx, union dmub_rb_cmd *cmd, enum dm_dmub_wait_type wait_type)
{
    (void)ctx; (void)wait_type;
    memcpy(captured, cmd, sizeof(captured));
    captured_n++;
    return true;
}

bool amdgpu_atom_parse_cmd_header(struct atom_context *ctx, int index, uint8_t *frev, uint8_t *crev)
{
    (void)ctx; (void)index; (void)frev; (void)crev;
    return false;          /* the ROM's master command table holds 0 for these functions */
}

int amdgpu_atom_execute_table(struct atom_context *ctx, int index, uint32_t *params, int params_size)
{
    (void)ctx; (void)index; (void)params; (void)params_size;
    fprintf(stderr, "HARNESS: amdgpu_atom_execute_table called (an ATOM table path was taken)\n");
    abort();
}

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

static uint32_t dmub_rr(void *ctx, uint32_t a) { (void)ctx; return model_read(a); }
static void dmub_ww(void *ctx, uint32_t a, uint32_t v) { (void)ctx; model_write(a, v); }

/* ---- the objects ---- */
static struct dc_context g_ctx;
static struct dc g_dc;
static struct bios_parser g_bp;
static struct amdgpu_device g_adev;
static struct dc_link g_links[MAX_LINKS];
static struct link_encoder g_link_encs[MAX_LINKS];
static struct dmub_srv g_dmub;
static int fake_dmub_srv_object;

void DL_init(void)
{
    struct dmub_srv_create_params p;
    int i;
    memset(&g_ctx, 0, sizeof(g_ctx)); memset(&g_dc, 0, sizeof(g_dc)); memset(&g_bp, 0, sizeof(g_bp));
    g_ctx.dc = &g_dc;
    g_dc.ctx = &g_ctx;
    g_ctx.dce_version = DCN_VERSION_4_01;
    g_ctx.driver_context = &g_adev;
    g_ctx.dmub_srv = (struct dc_dmub_srv *)&fake_dmub_srv_object;   /* only tested for non-NULL by command_table2.c */
    g_dc.debug.dmub_command_table = true;                           /* dcn401_resource.c debug defaults */
    for (i = 0; i < MAX_LINKS; i++) {                               /* get_link_by_phy_id() walks every slot */
        g_link_encs[i].transmitter = (enum transmitter)0x7f;        /* no PHY matches: no ACPI interlock, no SSC flag */
        g_links[i].link_enc = &g_link_encs[i];
        g_dc.links[i] = &g_links[i];
    }
    g_bp.base.ctx = &g_ctx;
    if (!dal_bios_parser_init_cmd_tbl_helper2(&g_bp.cmd_helper, DCN_VERSION_4_01)) {
        fprintf(stderr, "DL_init: helper2 refused DCN_VERSION_4_01\n"); abort();
    }
    dal_firmware_parser_init_cmd_tbl(&g_bp);

    memset(&p, 0, sizeof(p));
    p.funcs.reg_read = dmub_rr;
    p.funcs.reg_write = dmub_ww;
    p.asic = DMUB_ASIC_DCN401;
    if (dmub_srv_create(&g_dmub, &p) != DMUB_STATUS_OK) { fprintf(stderr, "DL_init: dmub_srv_create\n"); abort(); }
}

uint32_t DL_abs(const char *name)
{
    const struct dmub_srv_dcn401_reg_offset *o = &dmub_srv_dcn401_regs.offset;
    if (!strcmp(name, "GPINT_DATAIN1")) return o->DMCUB_GPINT_DATAIN1;
    if (!strcmp(name, "SCRATCH7")) return o->DMCUB_SCRATCH7;
    if (!strcmp(name, "INBOX1_RPTR")) return o->DMCUB_INBOX1_RPTR;
    if (!strcmp(name, "INBOX1_WPTR")) return o->DMCUB_INBOX1_WPTR;
    fprintf(stderr, "DL_abs: %s\n", name); abort();
}

int DL_enum(const char *n)
{
#define E(x) if (!strcmp(n, #x)) return (int)(x);
    E(TRANSMITTER_CONTROL_DISABLE) E(TRANSMITTER_CONTROL_ENABLE) E(TRANSMITTER_CONTROL_INIT)
    E(TRANSMITTER_CONTROL_SET_VOLTAGE_AND_PREEMPASIS) E(TRANSMITTER_CONTROL_POWER_ON)
    E(TRANSMITTER_UNIPHY_A) E(TRANSMITTER_UNIPHY_B) E(TRANSMITTER_UNIPHY_C) E(TRANSMITTER_UNIPHY_D) E(TRANSMITTER_UNIPHY_E)
    E(ENGINE_ID_DIGA) E(ENGINE_ID_DIGB) E(ENGINE_ID_DIGC) E(ENGINE_ID_DIGD) E(ENGINE_ID_HPO_0) E(ENGINE_ID_HPO_DP_0)
    E(ENGINE_ID_HPO_DP_1) E(ENGINE_ID_UNKNOWN)
    E(SIGNAL_TYPE_HDMI_TYPE_A) E(SIGNAL_TYPE_DISPLAY_PORT) E(SIGNAL_TYPE_DVI_SINGLE_LINK) E(SIGNAL_TYPE_DVI_DUAL_LINK)
    E(HPD_SOURCEID1) E(HPD_SOURCEID2) E(HPD_SOURCEID3) E(HPD_SOURCEID4)
    E(CONNECTOR_ID_HDMI_TYPE_A) E(CONNECTOR_ID_DISPLAY_PORT)
    E(LANE_COUNT_ONE) E(LANE_COUNT_FOUR)
    E(ENCODER_CONTROL_DISABLE) E(ENCODER_CONTROL_ENABLE) E(ENCODER_CONTROL_SETUP) E(ENCODER_CONTROL_INIT)
    E(COLOR_DEPTH_888) E(COLOR_DEPTH_101010) E(COLOR_DEPTH_121212) E(COLOR_DEPTH_161616)
    E(CONTROLLER_ID_D0) E(CONTROLLER_ID_D1) E(CONTROLLER_ID_D2) E(CONTROLLER_ID_D3)
    E(CLOCK_SOURCE_COMBO_PHY_PLL0) E(CLOCK_SOURCE_COMBO_PHY_PLL1) E(CLOCK_SOURCE_COMBO_PHY_PLL2)
    E(CLOCK_SOURCE_COMBO_PHY_PLL3) E(CLOCK_SOURCE_ID_DP_DTO)
    E(ENCODER_ID_INTERNAL_UNIPHY) E(ENCODER_ID_INTERNAL_UNIPHY1) E(ENCODER_ID_INTERNAL_UNIPHY2) E(ENCODER_ID_INTERNAL_UNIPHY3)
    E(TRANSMITTER_COLOR_DEPTH_24) E(TRANSMITTER_COLOR_DEPTH_30) E(TRANSMITTER_COLOR_DEPTH_36) E(TRANSMITTER_COLOR_DEPTH_48)
    E(ASIC_PIPE_DISABLE) E(ASIC_PIPE_ENABLE) E(ASIC_PIPE_INIT)
    E(DMUB_GPINT__GET_FW_VERSION) E(DMUB_GPINT__STOP_FW)
#undef E
    fprintf(stderr, "DL_enum: %s\n", n); abort();
}

#define RING_CAP 8192u   /* DMUB_RB_SIZE */

int DL_run(const struct dmub_scen *s, struct dmub_atom *a, struct dmub_out *o, uint8_t *ring, uint32_t ring_size)
{
    const struct command_table_helper *h = g_bp.cmd_helper;
    enum bp_result r;
    uint32_t i;

    memset(o, 0, sizeof(*o));
    captured_n = 0;
    switch (s->kind) {
    case DS_TRANSMITTER: {
        struct bp_transmitter_control c;
        memset(&c, 0, sizeof(c));
        c.action = (enum bp_transmitter_control_action)s->action;
        c.engine_id = (enum engine_id)s->engine;
        c.hpo_engine_id = (enum engine_id)s->hpo_engine;
        c.transmitter = (enum transmitter)s->transmitter;
        c.lanes_number = (enum dc_lane_count)s->lanes;
        c.signal = (enum signal_type)s->signal;
        c.hpd_sel = (enum hpd_source_id)s->hpd;
        c.connector_obj_id = dal_graphics_object_id_init((uint32_t)s->connector, ENUM_ID_1, OBJECT_TYPE_CONNECTOR);
        c.pixel_clock = s->pixel_clock_khz;
        c.lane_settings = (uint32_t)s->lane_settings;
        r = g_bp.cmd_tbl.transmitter_control(&g_bp, &c);
        /* the ATOM-level values our builder gets: Linux's own helper translations, and v1_7's two arithmetic lines */
        a->phyid = h->phy_id_to_atom(c.transmitter);
        a->action = (uint8_t)c.action;
        a->mode_laneset = c.action == TRANSMITTER_CONTROL_SET_VOLTAGE_AND_PREEMPASIS ? (uint8_t)c.lane_settings
                                                                                   : h->signal_type_to_atom_dig_mode(c.signal);
        a->lanenum = (uint8_t)c.lanes_number;
        a->hpdsel = h->hpd_sel_to_atom(c.hpd_sel);
        a->digfe_sel = h->dig_encoder_sel_to_atom(c.engine_id);
        a->connobj_id = (uint8_t)c.connector_obj_id.id;
        a->hpo_instance = (uint8_t)c.hpo_engine_id - ENGINE_ID_HPO_0;
        if (dc_is_dp_signal(c.signal))
            a->hpo_instance = (uint8_t)c.hpo_engine_id - ENGINE_ID_HPO_DP_0;
        a->skip_ssc = 0;
        break;
    }
    case DS_ENCODER: {
        struct bp_encoder_control c;
        memset(&c, 0, sizeof(c));
        c.action = (enum bp_encoder_control_action)s->action;
        c.engine_id = (enum engine_id)s->engine;
        c.signal = (enum signal_type)s->signal;
        c.lanes_number = (enum dc_lane_count)s->lanes;
        c.color_depth = (enum dc_color_depth)s->color_depth;
        c.enable_dp_audio = s->dp_audio != 0;
        c.pixel_clock = s->pixel_clock_khz;
        r = g_bp.cmd_tbl.dig_encoder_control(&g_bp, &c);
        a->digid = (uint8_t)c.engine_id;
        a->action = h->encoder_action_to_atom(c.action);
        a->digmode = (uint8_t)h->encoder_mode_bp_to_atom(c.signal, c.enable_dp_audio);
        a->lanenum = (uint8_t)c.lanes_number;
        a->bitpercolor = c.color_depth == COLOR_DEPTH_888 ? PANEL_8BIT_PER_COLOR :
                         c.color_depth == COLOR_DEPTH_101010 ? PANEL_10BIT_PER_COLOR :
                         c.color_depth == COLOR_DEPTH_121212 ? PANEL_12BIT_PER_COLOR :
                         c.color_depth == COLOR_DEPTH_161616 ? PANEL_16BIT_PER_COLOR : 0;
        a->is_hdmi = c.signal == SIGNAL_TYPE_HDMI_TYPE_A;
        break;
    }
    case DS_PIXEL_CLOCK: {
        struct bp_pixel_clock_parameters c;
        uint8_t crtc = 0;
        uint32_t pll = 0;
        memset(&c, 0, sizeof(c));
        c.controller_id = (enum controller_id)s->controller;
        c.pll_id = (enum clock_source_id)s->pll;
        c.signal_type = (enum signal_type)s->signal;
        c.target_pixel_clock_100hz = s->pixel_clock_100hz;
        c.encoder_object_id = dal_graphics_object_id_init((uint32_t)s->encoder_obj, ENUM_ID_1, OBJECT_TYPE_ENCODER);
        c.color_depth = (enum transmitter_color_depth)s->tx_color_depth;
        c.flags.FORCE_PROGRAMMING_OF_PLL = (s->flags >> 0) & 1;
        c.flags.PROGRAM_PHY_PLL_ONLY = (s->flags >> 1) & 1;
        c.flags.SUPPORT_YUV_420 = (s->flags >> 2) & 1;
        c.flags.SET_XTALIN_REF_SRC = (s->flags >> 3) & 1;
        c.flags.SET_GENLOCK_REF_DIV_SRC = (s->flags >> 4) & 1;
        r = g_bp.cmd_tbl.set_pixel_clock(&g_bp, &c);
        h->clock_source_id_to_atom(c.pll_id, &pll);
        h->controller_id_to_atom(c.controller_id, &crtc);
        a->crtc_id = crtc;
        a->pll_id = (uint8_t)pll;
        a->encoderobjid = h->encoder_id_to_atom(dal_graphics_object_id_get_encoder_id(c.encoder_object_id));
        a->encoder_mode = (uint8_t)h->encoder_mode_bp_to_atom(c.signal_type, false);
        a->deep_color_ratio = h->transmitter_color_depth_to_atom(c.color_depth);
        a->miscinfo = (c.flags.FORCE_PROGRAMMING_OF_PLL ? PIXEL_CLOCK_V7_MISC_FORCE_PROG_PPLL : 0) |
                      (c.flags.PROGRAM_PHY_PLL_ONLY ? PIXEL_CLOCK_V7_MISC_PROG_PHYPLL : 0) |
                      (c.flags.SUPPORT_YUV_420 ? PIXEL_CLOCK_V7_MISC_YUV420_MODE : 0) |
                      (c.flags.SET_XTALIN_REF_SRC ? PIXEL_CLOCK_V7_MISC_REF_DIV_SRC_XTALIN : 0) |
                      (c.flags.SET_GENLOCK_REF_DIV_SRC ? PIXEL_CLOCK_V7_MISC_REF_DIV_SRC_GENLK : 0) |
                      (c.signal_type == SIGNAL_TYPE_DVI_DUAL_LINK ? PIXEL_CLOCK_V7_MISC_DVI_DUALLINK_EN : 0);
        break;
    }
    case DS_POWER_GATING: {
        uint8_t crtc = 0;
        r = g_bp.cmd_tbl.enable_disp_power_gating(&g_bp, (enum controller_id)s->controller,
                                                  (enum bp_pipe_control_action)s->action);
        h->controller_id_to_atom((enum controller_id)s->controller, &crtc);
        a->pg_pipe = crtc;
        a->pg_enable = h->disp_power_gating_action_to_atom((enum bp_pipe_control_action)s->action);
        break;
    }
    case DS_GPINT: {
        uint32_t resp = 0;
        o->v[0] = dmub_srv_send_gpint_command(&g_dmub, (enum dmub_gpint_command)s->gpint_cmd, s->gpint_param,
                                              s->timeout) == DMUB_STATUS_OK;
        o->v[1] = dmub_srv_get_gpint_response(&g_dmub, &resp) == DMUB_STATUS_OK;
        o->v[2] = resp;
        o->nv = 3;
        return 0;
    }
    case DS_RING_SUBMIT:
    case DS_WAIT_IDLE:
    case DS_RETURN_DATA: {
        /* the state dmub_srv_hw_init leaves (inbox1 rb over the mailbox, D0), then dmub_srv_sync_inboxes adopts the
         * hardware pointers: the Linux function for taking over a ring whose firmware kept running */
        union dmub_rb_cmd cmd;
        if (ring_size != RING_CAP) abort();
        g_dmub.hw_init = true;
        g_dmub.power_state = DMUB_POWER_STATE_D0;
        memset(&g_dmub.inbox1, 0, sizeof(g_dmub.inbox1));
        g_dmub.inbox1.rb.base_address = ring;
        g_dmub.inbox1.rb.capacity = RING_CAP;
        o->v[0] = dmub_srv_sync_inboxes(&g_dmub);
        o->nv = 1;
        if (s->kind == DS_RING_SUBMIT) {
            for (i = 0; i < s->ncmds && o->nv < 8; i++) {
                enum dmub_status st;
                memset(&cmd, 0, sizeof(cmd));
                cmd.cmd_common.header.type = DMUB_CMD__VBIOS;
                cmd.cmd_common.header.sub_type = (i % 4);
                cmd.cmd_common.header.payload_bytes = 60;
                memset(cmd.cmd_common.cmd_buffer, (int)(0x11 * (i + 1)), sizeof(cmd.cmd_common.cmd_buffer));
                st = dmub_srv_fb_cmd_queue(&g_dmub, &cmd);
                if (st == DMUB_STATUS_OK)
                    st = dmub_srv_fb_cmd_execute(&g_dmub);
                o->v[o->nv++] = st == DMUB_STATUS_OK ? 0 : st == DMUB_STATUS_QUEUE_FULL ? 1 : 2;
                if (st != DMUB_STATUS_OK)
                    break;
            }
        } else if (s->kind == DS_WAIT_IDLE) {
            enum dmub_status st = dmub_srv_wait_for_idle(&g_dmub, s->timeout);
            o->v[o->nv++] = st == DMUB_STATUS_OK ? 0 : st == DMUB_STATUS_TIMEOUT ? 1 : 2;
        } else {
            dmub_rb_get_return_data(&g_dmub.inbox1.rb, &cmd);
            memcpy(o->bytes, &cmd, 64);
            o->nbytes = 64;
        }
        return 0;
    }
    case DS_FW_META: {
        struct dmub_srv_fw_meta_info_params p;
        struct dmub_fw_meta_info info;
        static uint8_t blob[4096];
        memset(blob, 0x5a, sizeof(blob));
        {
            uint32_t at = s->meta_blob_size - s->meta_at - 64;
            uint32_t magic = DMUB_FW_META_MAGIC, ver = 0x0a0b0c0d, reg = 0x10000, trace = 0x20000;
            memcpy(blob + at, &magic, 4); memcpy(blob + at + 4, &reg, 4); memcpy(blob + at + 8, &trace, 4);
            memcpy(blob + at + 12, &ver, 4);
        }
        memset(&p, 0, sizeof(p));
        memset(&info, 0, sizeof(info));
        if (s->bss) {
            p.fw_bss_data = blob;
            p.bss_data_size = s->meta_blob_size;
            p.fw_inst_const = blob;           /* unused on the bss path */
            p.inst_const_size = s->meta_blob_size + 0x100;
        } else {
            p.fw_inst_const = blob;
            p.inst_const_size = s->meta_blob_size + 0x100;   /* dmub_srv subtracts PSP_FOOTER_BYTES_256 first */
        }
        o->v[0] = dmub_srv_get_fw_meta_info_from_raw_fw(&p, &info) == DMUB_STATUS_OK;
        o->v[1] = info.fw_version;
        o->v[2] = info.fw_region_size;
        o->nv = 3;
        return 0;
    }
    default:
        return -1;
    }
    o->rc = r == BP_RESULT_OK ? 0 : 1;
    if (captured_n != 1) { o->rc = 100 + captured_n; return 0; }
    memcpy(o->bytes, captured, 64);
    o->nbytes = 64;
    return 0;
}

/* Evidence, not equivalence: the register sequence Linux's dmub_srv_hw_reset() (dmub_dcn401_reset) performs on a live
 * firmware, i.e. the first thing amdgpu_dm's "Reset DMCUB if it was previously running" does. The model firmware:
 * DMCUB_ENABLE set, not in reset, answers STOP_FW with 0xDEADDEAD in SCRATCH7 and sets PWAIT at once. */
void DL_reset_evidence(FILE *f)
{
    static struct trace t;
    struct model_behaviour b;
    const struct dmub_srv_dcn401_reg_offset *off = &dmub_srv_dcn401_regs.offset;
    int i;
    memset(&b, 0, sizeof(b));
    b.lock_after = -1;
    model_reset(0, &b, &t);
    model_poke(off->DMCUB_CNTL, 0x00010000u | 0x00100000u);     /* DMCUB_ENABLE, DMCUB_PWAIT_MODE_STATUS */
    model_poke(off->DMCUB_CNTL2, 0);
    model_poke(off->DMCUB_SCRATCH7, 0xDEADDEADu);
    dmub_srv_hw_reset(&g_dmub);
    fprintf(f, "EVIDENCE dmub_srv_hw_reset (Linux dmub_dcn401_reset) on a live firmware: %d events\n", t.n);
    for (i = 0; i < t.n; i++) {
        const char *nm = "?";
#define N(r) if (t.ev[i].addr == off->r) nm = #r;
        N(DMCUB_CNTL) N(DMCUB_CNTL2) N(DMCUB_SCRATCH7) N(DMCUB_GPINT_DATAIN1) N(DMCUB_INBOX1_RPTR) N(DMCUB_INBOX1_WPTR)
        N(DMCUB_OUTBOX1_RPTR) N(DMCUB_OUTBOX1_WPTR) N(DMCUB_OUTBOX0_RPTR) N(DMCUB_OUTBOX0_WPTR) N(DMCUB_SCRATCH0)
        N(DMCUB_REGION3_CW2_TOP_ADDRESS) N(DMCUB_REGION3_CW3_TOP_ADDRESS) N(DMCUB_REGION3_CW4_TOP_ADDRESS)
        N(DMCUB_REGION3_CW5_TOP_ADDRESS) N(DMCUB_REGION3_CW6_TOP_ADDRESS) N(DMCUB_REGION3_CW7_TOP_ADDRESS)
#undef N
        fprintf(f, "  %c %05x %08x %s\n", t.ev[i].op, t.ev[i].addr, t.ev[i].val, t.ev[i].op == 'D' ? "(udelay)" : nm);
    }
}

/* Evidence: Linux's own DC -> ATOM translations for DCN 4.01 (command_table_helper2_dce112.c via helper2), the values a
 * kext must put into the DMUB commands. encoder_id_to_atom is NOT printed: its ENCODER_OBJECT_ID_* values come from
 * ObjectID.h, absent from the clone (the harness uses placeholders). */
void DL_dump_translations(FILE *f)
{
    const struct command_table_helper *h = g_bp.cmd_helper;
    static const struct { const char *n; int v; } tx[] = { {"UNIPHY_A", TRANSMITTER_UNIPHY_A}, {"UNIPHY_B", TRANSMITTER_UNIPHY_B},
        {"UNIPHY_C", TRANSMITTER_UNIPHY_C}, {"UNIPHY_D", TRANSMITTER_UNIPHY_D}, {"UNIPHY_E", TRANSMITTER_UNIPHY_E} };
    static const struct { const char *n; int v; } sig[] = { {"HDMI_TYPE_A", SIGNAL_TYPE_HDMI_TYPE_A},
        {"DISPLAY_PORT", SIGNAL_TYPE_DISPLAY_PORT}, {"DVI_SINGLE_LINK", SIGNAL_TYPE_DVI_SINGLE_LINK} };
    static const struct { const char *n; int v; } eng[] = { {"DIGA", ENGINE_ID_DIGA}, {"DIGB", ENGINE_ID_DIGB},
        {"DIGC", ENGINE_ID_DIGC}, {"DIGD", ENGINE_ID_DIGD} };
    static const struct { const char *n; int v; } pll[] = { {"COMBO_PHY_PLL0", CLOCK_SOURCE_COMBO_PHY_PLL0},
        {"COMBO_PHY_PLL1", CLOCK_SOURCE_COMBO_PHY_PLL1}, {"COMBO_PHY_PLL2", CLOCK_SOURCE_COMBO_PHY_PLL2},
        {"COMBO_PHY_PLL3", CLOCK_SOURCE_COMBO_PHY_PLL3}, {"ID_DP_DTO", CLOCK_SOURCE_ID_DP_DTO} };
    int i;
    uint32_t u; uint8_t b;
    fprintf(f, "TRANSLATIONS (Linux 238650ef6c7c, DCN_VERSION_4_01 helper)\n");
    for (i = 0; i < 5; i++) fprintf(f, "  phy_id_to_atom(TRANSMITTER_%s) = %u\n", tx[i].n, h->phy_id_to_atom((enum transmitter)tx[i].v));
    for (i = 0; i < 3; i++) fprintf(f, "  signal_type_to_atom_dig_mode(SIGNAL_TYPE_%s) = %u\n", sig[i].n,
                                    h->signal_type_to_atom_dig_mode((enum signal_type)sig[i].v));
    for (i = 0; i < 3; i++) fprintf(f, "  encoder_mode_bp_to_atom(SIGNAL_TYPE_%s, false) = %u\n", sig[i].n,
                                    h->encoder_mode_bp_to_atom((enum signal_type)sig[i].v, false));
    for (i = 0; i < 4; i++) fprintf(f, "  hpd_sel_to_atom(HPD_SOURCEID%d) = %u\n", i + 1, h->hpd_sel_to_atom((enum hpd_source_id)i));
    for (i = 0; i < 4; i++) fprintf(f, "  dig_encoder_sel_to_atom(ENGINE_ID_%s) = 0x%02x\n", eng[i].n,
                                    h->dig_encoder_sel_to_atom((enum engine_id)eng[i].v));
    for (i = 0; i < 5; i++) { u = 0xffff; b = h->clock_source_id_to_atom((enum clock_source_id)pll[i].v, &u);
        fprintf(f, "  clock_source_id_to_atom(CLOCK_SOURCE_%s) = %u (ok=%u)\n", pll[i].n, u, b); }
    for (i = 0; i < 4; i++) { b = 0xff; h->controller_id_to_atom((enum controller_id)(CONTROLLER_ID_D0 + i), &b);
        fprintf(f, "  controller_id_to_atom(CONTROLLER_ID_D%d) = %u\n", i, b); }
    fprintf(f, "  encoder_action_to_atom(ENCODER_CONTROL_SETUP) = 0x%02x, ENABLE = 0x%02x, DISABLE = 0x%02x\n",
            h->encoder_action_to_atom(ENCODER_CONTROL_SETUP), h->encoder_action_to_atom(ENCODER_CONTROL_ENABLE),
            h->encoder_action_to_atom(ENCODER_CONTROL_DISABLE));
    for (i = 0; i < 4; i++) fprintf(f, "  transmitter_color_depth_to_atom(TRANSMITTER_COLOR_DEPTH_%d) = %u\n", 24 + 6 * i + (i == 3 ? 6 : 0),
                                    h->transmitter_color_depth_to_atom((enum transmitter_color_depth)i));
    fprintf(f, "  disp_power_gating_action_to_atom(ASIC_PIPE_DISABLE/ENABLE/INIT) = %u/%u/%u\n",
            h->disp_power_gating_action_to_atom(ASIC_PIPE_DISABLE), h->disp_power_gating_action_to_atom(ASIC_PIPE_ENABLE),
            h->disp_power_gating_action_to_atom(ASIC_PIPE_INIT));
    fprintf(f, "  TRANSMITTER_CONTROL_INIT/ENABLE/DISABLE (passed through as action) = %d/%d/%d\n",
            TRANSMITTER_CONTROL_INIT, TRANSMITTER_CONTROL_ENABLE, TRANSMITTER_CONTROL_DISABLE);
}
