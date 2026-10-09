//
//  navi48_dmubcmd.h - build 0.0.625 (multi-monitor track, stages M2 / M3: the FIRST code that sends commands to the display firmware; an internal design note, "M1 result: the DMUB dialect" and
//  "M1.5 result: ... M2/M3 plans", section 4 "Plans"). The pure half (decisions, allowlists, constants, argument and result layouts), shared by the kext (dcn/navi48_dmubcmd_flow.h + the glue in dcn/navi48_dcn.cpp), the CLI
//  (tools/pc/navi48test.c includes this very file) and the host test (tests/native_dmubcmd_test.cpp). Plain C99 + C++17: no kernel header.
//
//    95  dmubsend <slotspec>   append ONE command to the DMUB inbox1 ring and wait for it (RPTR == the new WPTR, at most 100 ms, never retried).
//    96  dmubmode <op>         the 32-byte mode block at REGION4 + 0x4c00: save | restore | 0x1a6 | 0x1d4.
//    97  dmubctx <ctx> status <value>   the M2 marker: REGION4 + ctx + 0x1a0 (ctx 0x7000 or 0x7800 only).
//
//  ALL THREE are behind boot-arg navi48-dmubcmd=1 (default OFF, latched at start). OFF: every one returns N48DR_CMD_OFF and touches nothing.
//
//  What is written, ALL of it (the brief's list and nothing else):
//    - 16 dwords (one 64-byte slot) in VRAM at REGION4 base + WPTR, through navi48_vram_write_mm (the existing locked MM_INDEX writer);
//    - ONE register: DMCUB_INBOX1_WPTR (dword 0x01d6 of the DMU segment 2, absolute 0x3696 on this card), through the DCN write allowlist (it is inside the range "DC..MMHUBBUB" 0x35e6..0x3771);
//    - three VBIOS ring-state dwords (REGION4 + 0x3114 = 0x40, +0x3118 = the old RPTR, +0x311c = the new WPTR) after a command completed;
//    - the 8-dword mode block at REGION4 + 0x4c00 (96) and one dword at REGION4 + ctx + 0x1a0 (97).
//
#ifndef N48_DMUBCMD_H
#define N48_DMUBCMD_H

#include <stdint.h>
#include "navi48_dispread.h"

#ifdef __cplusplus
extern "C" {
#endif

#define N48DM_ACT_SEND 95u
#define N48DM_ACT_MODE 96u
#define N48DM_ACT_CTX  97u

// ---- the boot-arg latch (default OFF): only "navi48-dmubcmd=1" turns the verbs on --------------------------------------------------------------------------------------------------------------------------
#define N48DM_LATCH_UNSET 0u
#define N48DM_LATCH_OFF   1u
#define N48DM_LATCH_ON    2u
static inline uint32_t n48dm_latch_value(int present, uint32_t value) { return (present && value == 1u) ? N48DM_LATCH_ON : N48DM_LATCH_OFF; }
static inline int n48dm_latch_is_on(uint32_t latch) { return latch == N48DM_LATCH_ON; }

// ---- registers (DMU dword offsets, BASE_IDX 2; checked by the host test against dcn41_regs.h and Linux's dcn_4_1_0_offset.h) ---------------------------------------------------------------------------------
#define N48DM_REG_INBOX1_WPTR 0x01d6u    /* regDMCUB_INBOX1_WPTR: absolute 0x34c0 + 0x1d6 = 0x3696 on this card (the VBIOS' own send routine bumps register 0x3696) */
#define N48DM_REG_INBOX1_RPTR 0x01d7u    /* regDMCUB_INBOX1_RPTR: 0x3697 */
#define N48DM_REG_BASE_IDX 2u
#define N48DM_REG_OTG0_FRAMECOUNT 0x1b4du /* regOTG0_OTG_STATUS_FRAME_COUNT, BASE_IDX 2 (the census table's entry 16) */

// ---- the window (offsets from the REGION4 base) -----------------------------------------------------------------------------------------------------------------------------------------------------------
#define N48DM_SLOT_BYTES 64u             /* one inbox1 command */
#define N48DM_SLOT_DWORDS 16u
#define N48DM_VAR_STATUS   0x3100u       /* NEVER written by this code (the VBIOS' 9 / 0 handshake byte; the plan leaves it at 0) */
#define N48DM_VAR_LASTSIZE 0x3114u       /* written 0x40 */
#define N48DM_VAR_RPTR     0x3118u       /* written the RPTR that preceded the command */
#define N48DM_VAR_WPTR     0x311cu       /* written the new WPTR */
#define N48DM_VAR_CMDSIZE_VALUE 0x40u
#define N48DM_MODE_BLOCK   0x4c00u
#define N48DM_MODE_DWORDS  8u
#define N48DM_CTX_STATUS   0x1a0u        /* display context + 0x1a0: the status dword (bit 1 = detected) */
#define N48DM_RING_MAX     0x10000u
#define N48DM_RING_SIZE    0x2000u       /* 0.0.626: the MEASURED inbox1 ring size (notes/logs/runs/m1-622/region4.txt): the only size this code will write into, so slot + 0x40 can never reach +0x3100 */

// ---- the poll bound -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// RPTR is read once per 10 us step, at most 10000 times: 100 ms, the plan's bound. No retry of any kind.
#define N48DM_POLL_STEP_US 10u
#define N48DM_POLL_MAX 10000u
#define N48DM_POLL_BOUND_US (N48DM_POLL_STEP_US * N48DM_POLL_MAX)
// 0.0.630 (M4c): the mainline VBIOS sub-types (1 TRANSMITTER_CONTROL, 2 SET_PIXEL_CLOCK) may take the firmware much longer (a PHY / PLL sequence): 2 s, the same 10 us step. Still no retry.
#define N48DM_POLL_MAX_VBIOS 200000u
#define N48DM_POLL_BOUND_VBIOS_US (N48DM_POLL_STEP_US * N48DM_POLL_MAX_VBIOS)

// ---- the allowlist (the brief; the dialect is an internal design note section "M1 result" 2) -----------------------------------------------------------------------------------------------------------
#define N48DM_HDR_DETECT  0x04000a80u    /* sub 0xa  detect plus EDID into the context */
#define N48DM_HDR_BEGIN   0x08000680u    /* sub 6    mode change begin / end */
#define N48DM_HDR_SETMODE 0x08000580u    /* sub 5    set mode on the display */
#define N48DM_HDR_ENABLE  0x04000780u    /* sub 7    enable output */
#define N48DM_HDR_DISABLE 0x04000880u    /* sub 8    disable output */
#define N48DM_CTX_DFP2 0x6800u           /* the live DP context (SINK-A) */
#define N48DM_CTX_DFP3 0x7000u           /* HDMI (SINK-B) */
#define N48DM_CTX_DFP4 0x7800u           /* HDMI (the monitor B) */
#define N48DM_D2_BEGIN 0x101u

static inline int n48dm_hdr_ok(uint32_t h)
{
    return h == N48DM_HDR_DETECT || h == N48DM_HDR_BEGIN || h == N48DM_HDR_SETMODE || h == N48DM_HDR_ENABLE || h == N48DM_HDR_DISABLE;
}
static inline int n48dm_ctx3_ok(uint32_t c) { return c == N48DM_CTX_DFP2 || c == N48DM_CTX_DFP3 || c == N48DM_CTX_DFP4; }
// The two HDMI contexts (never 0x6800, the live DP context): dmubctx's set, and since 0.0.626 sub 8 (disable)'s set.
static inline int n48dm_ctx_ok(uint32_t c) { return c == N48DM_CTX_DFP3 || c == N48DM_CTX_DFP4; }
// 0 = both on the allowlist; else the refusing N48DR_ status. The header is judged first, then (d1, d2) per sub-type. Pure: NOTHING is touched.
static inline uint32_t n48dm_slot_check(uint32_t h, uint32_t d1, uint32_t d2)
{
    if (!n48dm_hdr_ok(h)) return N48DR_HDR_REFUSED;
    switch ((h >> 8) & 0xFFu) {
    case 0x0au: case 0x07u:               /* detect / enable: d1 = a context, d2 = 0 */
        return (n48dm_ctx3_ok(d1) && d2 == 0u) ? 0u : (uint32_t)N48DR_PAYLOAD_REFUSED;
    case 0x08u:                           /* disable (0.0.626): ONLY the HDMI contexts 0x7000 / 0x7800, never 0x6800 (on the live DP context it is a console kill switch); d2 = 0 */
        return (n48dm_ctx_ok(d1) && d2 == 0u) ? 0u : (uint32_t)N48DR_PAYLOAD_REFUSED;
    case 0x06u:                           /* mode-change begin (d2 = 0x101) / end (d2 = 0): d1 = 0 */
        return (d1 == 0u && (d2 == N48DM_D2_BEGIN || d2 == 0u)) ? 0u : (uint32_t)N48DR_PAYLOAD_REFUSED;
    case 0x05u:                           /* set mode: d1 = the mode block, d2 = a context */
        return (d1 == N48DM_MODE_BLOCK && n48dm_ctx3_ok(d2)) ? 0u : (uint32_t)N48DR_PAYLOAD_REFUSED;
    default: return N48DR_HDR_REFUSED;
    }
}
// The 64-byte slot: header, d1, d2, thirteen zero dwords (the firmware ignores d3..d5 and d6 is never written; the plan zero-fills them).
static inline void n48dm_build_slot(uint32_t slot[N48DM_SLOT_DWORDS], uint32_t h, uint32_t d1, uint32_t d2)
{
    unsigned i;
    for (i = 0; i < N48DM_SLOT_DWORDS; i++) slot[i] = 0u;
    slot[0] = h; slot[1] = d1; slot[2] = d2;
}

// ---- the CLI commands -> slot specs (one source of truth for the CLI and the test) -----------------------------------------------------------------------------------------------------------------------------
enum { N48DM_K_DETECT = 1, N48DM_K_BEGIN = 2, N48DM_K_END = 3, N48DM_K_SETMODE = 4, N48DM_K_ENABLE = 5, N48DM_K_DISABLE = 6 };
// 0 = ok (*h, *d1, *d2 set); N48DR_BAD_ARG for an unknown kind. ctx is used by detect / setmode / enable / disable only.
static inline uint32_t n48dm_spec(uint32_t kind, uint32_t ctx, uint32_t *h, uint32_t *d1, uint32_t *d2)
{
    switch (kind) {
    case N48DM_K_DETECT:  *h = N48DM_HDR_DETECT;  *d1 = ctx; *d2 = 0u; return 0u;
    case N48DM_K_BEGIN:   *h = N48DM_HDR_BEGIN;   *d1 = 0u;  *d2 = N48DM_D2_BEGIN; return 0u;
    case N48DM_K_END:     *h = N48DM_HDR_BEGIN;   *d1 = 0u;  *d2 = 0u; return 0u;
    case N48DM_K_SETMODE: *h = N48DM_HDR_SETMODE; *d1 = N48DM_MODE_BLOCK; *d2 = ctx; return 0u;
    case N48DM_K_ENABLE:  *h = N48DM_HDR_ENABLE;  *d1 = ctx; *d2 = 0u; return 0u;
    case N48DM_K_DISABLE: *h = N48DM_HDR_DISABLE; *d1 = ctx; *d2 = 0u; return 0u;
    default: *h = 0u; *d1 = 0u; *d2 = 0u; return N48DR_BAD_ARG;
    }
}

// ---- dmubsend's ONE scalar argument: header (bits 0..31) | d1 << 32 (16 bits) | d2 << 48 (16 bits). Every allowlisted d1 / d2 fits 16 bits; a value that does not is not encodable and not allowlisted. -----------------
static inline int n48dm_send_encodable(uint32_t d1, uint32_t d2) { return d1 <= 0xFFFFu && d2 <= 0xFFFFu; }
static inline uint64_t n48dm_send_arg(uint32_t h, uint32_t d1, uint32_t d2) { return (uint64_t)h | ((uint64_t)(d1 & 0xFFFFu) << 32) | ((uint64_t)(d2 & 0xFFFFu) << 48); }
static inline void n48dm_send_unarg(uint64_t a, uint32_t *h, uint32_t *d1, uint32_t *d2) { *h = (uint32_t)(a & 0xFFFFFFFFull); *d1 = (uint32_t)((a >> 32) & 0xFFFFu); *d2 = (uint32_t)((a >> 48) & 0xFFFFu); }

// ---- the live pre-checks (judged AFTER the slot check, BEFORE anything is written) -------------------------------------------------------------------------------------------------------------------------------
struct n48dm_pre {
    uint32_t enabled, soft_reset;        /* DMCUB_CNTL.DMCUB_ENABLE, DMCUB_CNTL2.DMCUB_SOFT_RESET */
    uint32_t ring_region4, ring_at_base; /* the inbox is mapped through REGION4, and ring byte 0 IS the REGION4 window base */
    uint32_t size, wptr, rptr;           /* DMCUB_INBOX1_SIZE / WPTR / RPTR */
};
// 0 = go; else the refusing status. Order: DMCUB state, ring placement, ring sanity, ring size (0.0.626: must be exactly N48DM_RING_SIZE), busy, wrap.
static inline uint32_t n48dm_precheck(const struct n48dm_pre *p)
{
    if (!p->enabled || p->soft_reset) return N48DR_DMUB_NOT_READY;
    if (!p->ring_region4 || !p->ring_at_base) return N48DR_RING_NOT_REGION4;
    if (p->size == 0u || (p->size % N48DM_SLOT_BYTES) != 0u || p->size > N48DM_RING_MAX || (p->wptr % N48DM_SLOT_BYTES) != 0u || (p->rptr % N48DM_SLOT_BYTES) != 0u ||
        p->wptr >= p->size || p->rptr >= p->size) return N48DR_NO_RING;
    if (p->size != N48DM_RING_SIZE) return N48DR_RING_SIZE_REFUSED;     /* 0.0.626 (F7): any other ring size could overlap the VBIOS variables / mode block / contexts at +0x3100.. */
    if (p->wptr != p->rptr) return N48DR_RING_BUSY;
    if (p->wptr + N48DM_SLOT_BYTES >= p->size) return N48DR_RING_WRAP;   /* the new WPTR would be the ring size itself: the ring wraps there, and no wrap design exists */
    return 0u;
}

// 0.0.626 (F6): dmubmode (every WRITE op) and dmubctx need an IDLE ring (live WPTR == RPTR): after a send that timed out the firmware may still be reading what they would write.
static inline uint32_t n48dm_idle_gate(const struct n48dm_pre *p) { return p->wptr == p->rptr ? 0u : (uint32_t)N48DR_RING_NOT_IDLE; }

// ---- replay (0.0.626, F8) --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// `dmubsend replay <slot_off>`: copy an EXISTING ring slot, byte for byte (all 64 bytes, including the leftover dwords the GOP's own commands carry in d2..d5), into the next slot. The argument is
// N48DM_REPLAY_TAG in bits 0..31 (never an allowlisted header, so an ordinary send can never be mistaken for it), the source slot offset in bits 32..47, and bits 48..63 zero.
#define N48DM_REPLAY_TAG 0x52504c59u     /* "RPLY" */
#define N48DM_NO_REPLAY 0xFFFFFFFFu
static inline uint64_t n48dm_replay_arg(uint32_t slotOff) { return (uint64_t)N48DM_REPLAY_TAG | ((uint64_t)(slotOff & 0xFFFFu) << 32); }
static inline int n48dm_is_replay_arg(uint64_t a) { return (uint32_t)(a & 0xFFFFFFFFull) == N48DM_REPLAY_TAG; }
// 0 = well-formed (*off set); else N48DR_REPLAY_REFUSED. The slot must be 64-byte aligned and inside the ring; "below WPTR" is judged against the live WPTR by the flow.
static inline uint32_t n48dm_replay_unarg(uint64_t a, uint32_t *off)
{
    *off = N48DM_NO_REPLAY;
    if (!n48dm_is_replay_arg(a) || (a >> 48) != 0u) return N48DR_REPLAY_REFUSED;
    *off = (uint32_t)((a >> 32) & 0xFFFFu);
    if ((*off % N48DM_SLOT_BYTES) != 0u || *off >= N48DM_RING_SIZE) { *off = N48DM_NO_REPLAY; return N48DR_REPLAY_REFUSED; }
    return 0u;
}

// ---- the mainline VBIOS packets (0.0.630, stage M4c; an internal design note "M4b analysis" section 4 (a)) ------------------------------------------------------------------------------------------
// Sub 2 SET_PIXEL_CLOCK (Linux struct dmub_rb_cmd_set_pixel_clock: header + struct set_pixel_clock_parameter_v1_7, 16 bytes -> payload_bytes 16, header 0x10000280) and sub 1 TRANSMITTER_CONTROL
// (struct dmub_rb_cmd_dig1_transmitter_control: header + a 60-byte union (dmub_dig_transmitter_control_data_v1_7, reserved3[11]) -> payload_bytes 60 = sizeof(cmd) - sizeof(header), header 0x3C000180; command_table2.c
// transmitter_control_dmcub_v1_7 memsets the whole command first, so every payload byte past the 16 below is ZERO). Each allowed packet is a FULL FIXED slot: the allowlist is "this exact 16-dword slot", never a
// field check; a userland caller names a template by ID and cannot pass a dword. Nothing else of sub-type 1 or 2 can reach the ring (n48dm_slot_check still refuses their headers).
#define N48DM_HDR_PCLK  0x10000280u      /* type 0x80, sub 2, payload_bytes 16 */
#define N48DM_HDR_XMIT  0x3C000180u      /* type 0x80, sub 1, payload_bytes 60 */
// 0.0.632 (M4d follow-up): template 7, the mainline DIGX_ENCODER_CONTROL (sub 0) exactly as Linux sends it for a single-link DVI/TMDS stream on DIGC. command_table2.c encoder_control_dmcub: cmd = memset 0;
// header.type = DMUB_CMD__VBIOS (0x80); header.sub_type = DMUB_CMD__VBIOS_DIGX_ENCODER_CONTROL (0); header.payload_bytes = sizeof(cmd.digx_encoder_control) - sizeof(header). The command is the 4-byte header plus
// union dig_encoder_control_parameters_v1_5 (atomfirmware.h), whose largest member dig_encoder_stream_setup_parameters_v1_5 is 12 bytes (digid, action, digmode, lanenum, pclk_10khz (u32), bitpercolor,
// dplinkrate_270mhz, reserved[2]) -> sizeof(cmd) 16, payload_bytes 12 -> header 0x0C000080 (= 12 << 24 | 0x00 << 8 | 0x80). Field values (encoder_control_digx_v1_5 + enc401_stream_encoder_dvi_set_stream_attribute,
// is_dual_link = false, 1080p60 = 148.5 MHz): digid = ENGINE_ID_DIGC = 2; action = encoder_action_to_atom(ENCODER_CONTROL_SETUP) = ATOM_ENCODER_CMD_STREAM_SETUP = 0x0F (dce112 table, used for DCN 4.01);
// digmode = ATOM_ENCODER_MODE_DVI = 2; lanenum = LANE_COUNT_FOUR = 4; pclk_10khz = (pix_clk_100hz / 10) / 10 = 14850 = 0x3A02; bitpercolor = 0: dvi_set_stream_attribute declares `struct bp_encoder_control cntl = {0}`
// and never sets color_depth, so encoder_control_digx_v1_5's switch (COLOR_DEPTH_UNDEFINED) hits `default: break` and leaves the memset-0 value (NOT PANEL_8BIT_PER_COLOR = 2); dplinkrate_270mhz = 0 and reserved = 0 (never set).
// Linux has NO encoder-control DISABLE on the dcn401 TMDS path (ENCODER_CONTROL_DISABLE appears only in dce_link_encoder.c's DAC path), so there is no second template.
#define N48DM_HDR_DIGENC 0x0C000080u     /* type 0x80, sub 0, payload_bytes 12 */
// 0.0.633 (multi-monitor, the monitor B SINK-C on UNIPHY_D / DIG3 / HDMI-A ddc3 hpd4): templates 8..12 are templates 3 / 4 / 5 / 6 / 7 with ONLY the instance fields changed, each derived field by field from the same Linux structs:
//   8  pclk-otg2-on   / 9  pclk-otg2-off : set_pixel_clock_parameter_v1_7 as 3 / 4 but crtc_id = 2 (controller_id_to_atom(CONTROLLER_ID_D2) = ATOM_CRTC3 = 2, atomfirmware.h enum atom_crtc_def: CRTC1 0, CRTC2 1, CRTC3 2 - OTG2 is
//      controller D2) and pll_id = 0x17 = ATOM_COMBOPHY_PLL3 (clock_source_id_to_atom(CLOCK_SOURCE_COMBO_PHY_PLL3)). WHY PLL3: the previous builder gave PLL2 (0x16) to OTG1 because UNIPHY_C is PHY 2 (the PLL number equals the PHY it
//      feeds; SUSPECTED, an internal design note M4c: "Whether a PLL number is tied to a PHY is SUSPECTED"); the monitor B's PHY D is PHY 3, so the SAME rule gives PLL3, which is also simply the next free PLL after PLL2
//      (PLL0 is Linux's first-free choice but not the card's PHY-tied choice; the DP uses the DTO and no PHY PLL). PLL3 was also the pll of the harmless OTG3 probe (templates 1 / 2): never have pclk-otg3-on and pclk-otg2-on up together.
//      d2 = pll_id 0x17 | encoderobjid 0x20 << 8 (ENCODER_OBJECT_ID_INTERNAL_UNIPHY1: display_objid 0x340c's encoder object 0x2220 is UNIPHY1 enum 2, id 0x20, as the monitor A's 0x2120 is UNIPHY1 enum 1) | encoder_mode 2 (DVI) << 16.
//   10 phyd-enable-dvi / 11 phyd-disable : dmub_dig_transmitter_control_data_v1_7 as 5 / 6 but phyid = 3 (UNIPHYD, dmub_cmd.h "3=UNIPHYD"), hpdsel = 4 (ATOM_TRANSMITTER_V6_HPD4_SEL, 1-based: the monitor B's connector list entry is ddc3/hpd4),
//      connobj_id = 0x0C: the connector object id Linux sends is connector_obj_id.id = CONNECTOR_ID_HDMI_TYPE_A = 12 (grph_object_id.h), and the monitor B's ROM path 3 display_objid is 0x340c - object id (low 8 bits) 0x0c = HDMI type A, exactly
//      the monitor A's 0x330c low byte (DISPLAY-DESIGN.md section 3.5 path table). digfe_sel stays 0 (dig_encoder_sel_to_atom returns 0 for every engine, command_table_helper2_dce112.c), HPO_instance 0.
//   12 digd-setup-dvi : dig_encoder_stream_setup_parameters_v1_5 as 7 but digid = ENGINE_ID_DIGD = 3 (enum engine_id: DIGA 0, DIGB 1, DIGC 2, DIGD 3; encoder_control_digx_v1_5 sets digid = (uint8_t)cntl->engine_id).
// 0.0.634 (M4b, the monitor B at 2560x1440@60, 241.50 MHz): templates 13..15 are templates 8 / 10 / 12 with ONLY the clock field changed (the lane / link / mode / instance fields are the 1080p twins', decoded field by field in
// notes: tests/native_dmubcmd_test.cpp "0.0.634"):
//   13 pclk-otg2-1440-on     : set_pixel_clock_parameter_v1_7.pixclk_100hz = 241.5 MHz / 100 Hz = 2415000 = 0x0024D998 (the 1080p twin carries 148.5 MHz / 100 Hz = 1485000 = 0x0016A8C8); pll_id / encoderobjid / encoder_mode / crtc_id as 8.
//   14 phyd-enable-dvi-1440  : dig_v1_7.symclk_units.symclk_10khz = 241.5 MHz / 10 kHz = 24150 = 0x00005E56 (the 1080p twin: 14850 = 0x3A02); phyid 3, action 1, digmode 2, lanenum 4, hpdsel 4, connobj 0x0C exactly as 10.
//   15 digd-setup-dvi-1440   : dig_encoder_stream_setup_parameters_v1_5.pclk_10khz = 24150 = 0x5E56; digid 3, action 0x0F, digmode 2, lanenum 4 as 12.
// The disable templates (6, 11) are reused unchanged: Linux's dcn10_link_encoder_disable_output sends TRANSMITTER_CONTROL_DISABLE with pixel_clock 0 (cntl is zero-initialised), so the disable's symclk never has to match the enable's.
enum { N48DM_TPL_NONE = 0, N48DM_TPL_PCLK_OTG3_ON = 1, N48DM_TPL_PCLK_OTG3_OFF = 2, N48DM_TPL_PCLK_OTG1_ON = 3, N48DM_TPL_PCLK_OTG1_OFF = 4, N48DM_TPL_PHYC_ENABLE_DVI = 5, N48DM_TPL_PHYC_DISABLE = 6, N48DM_TPL_DIGC_SETUP_DVI = 7,
       N48DM_TPL_PCLK_OTG2_ON = 8, N48DM_TPL_PCLK_OTG2_OFF = 9, N48DM_TPL_PHYD_ENABLE_DVI = 10, N48DM_TPL_PHYD_DISABLE = 11, N48DM_TPL_DIGD_SETUP_DVI = 12,
       N48DM_TPL_PCLK_OTG2_1440_ON = 13, N48DM_TPL_PHYD_ENABLE_DVI_1440 = 14, N48DM_TPL_DIGD_SETUP_DVI_1440 = 15, N48DM_TPL_COUNT = 15 };
struct n48dm_tpl { const char *name; uint32_t dw[5]; };       /* header, d1..d4; dwords 5..15 of the slot are zero */
static const struct n48dm_tpl n48dm_tpls[N48DM_TPL_COUNT] = {
    { "pclk-otg3-on",  { N48DM_HDR_PCLK, 0x0016A8C8u, 0x00022017u, 0x00000003u, 0u } },
    { "pclk-otg3-off", { N48DM_HDR_PCLK, 0x00000000u, 0x00022017u, 0x00000003u, 0u } },
    { "pclk-otg1-on",  { N48DM_HDR_PCLK, 0x0016A8C8u, 0x00022016u, 0x00000001u, 0u } },
    { "pclk-otg1-off", { N48DM_HDR_PCLK, 0x00000000u, 0x00022016u, 0x00000001u, 0u } },
    { "phyc-enable-dvi", { N48DM_HDR_XMIT, 0x04020102u, 0x00003A02u, 0x000C0003u, 0u } },
    { "phyc-disable",    { N48DM_HDR_XMIT, 0x04020002u, 0x00003A02u, 0x000C0003u, 0u } },
    { "digc-setup-dvi",  { N48DM_HDR_DIGENC, 0x04020F02u, 0x00003A02u, 0x00000000u, 0u } },
    { "pclk-otg2-on",    { N48DM_HDR_PCLK, 0x0016A8C8u, 0x00022017u, 0x00000002u, 0u } },
    { "pclk-otg2-off",   { N48DM_HDR_PCLK, 0x00000000u, 0x00022017u, 0x00000002u, 0u } },
    { "phyd-enable-dvi", { N48DM_HDR_XMIT, 0x04020103u, 0x00003A02u, 0x000C0004u, 0u } },
    { "phyd-disable",    { N48DM_HDR_XMIT, 0x04020003u, 0x00003A02u, 0x000C0004u, 0u } },
    { "digd-setup-dvi",  { N48DM_HDR_DIGENC, 0x04020F03u, 0x00003A02u, 0x00000000u, 0u } },
    { "pclk-otg2-1440-on",    { N48DM_HDR_PCLK, 0x0024D998u, 0x00022017u, 0x00000002u, 0u } },       /* 0.0.634: 2415000 x 100 Hz = 241.5 MHz */
    { "phyd-enable-dvi-1440", { N48DM_HDR_XMIT, 0x04020103u, 0x00005E56u, 0x000C0004u, 0u } },       /* 0.0.634: 24150 x 10 kHz = 241.5 MHz */
    { "digd-setup-dvi-1440",  { N48DM_HDR_DIGENC, 0x04020F03u, 0x00005E56u, 0x00000000u, 0u } },     /* 0.0.634: 24150 x 10 kHz = 241.5 MHz */
};
static inline const struct n48dm_tpl *n48dm_tpl_get(uint32_t id) { return (id >= 1u && id <= N48DM_TPL_COUNT) ? &n48dm_tpls[id - 1u] : (const struct n48dm_tpl *)0; }
/* 0.0.652 (M5), widened in 0.0.653: while the monitor B's plane is HELD (disp2 fbhold 2) the DMUB must not touch what the framebuffer scans. 0.0.652 refused only the two monitor B TEARDOWN templates (pclk-otg2-off, phyd-disable); the M5 review found the
 * ON templates (pclk otg2-on / otg2-1440-on, phyd-enable*, digd-setup*) and raw `dmubsend disable 0x7800` still passed. 0.0.653: EVERY monitor B template (ids 8..15: otg2 / phyd / digd) is refused while HELD; the monitor A's (3..7: otg1 / phyc / digc) and
 * the OTG3 probe (1, 2) are unaffected. `dmubsend replay` is refused as well while HELD (it could replay a teardown slot sent earlier). Pure. */
static inline int n48dm_tpl_held_refuses(uint32_t id) { return id >= N48DM_TPL_PCLK_OTG2_ON && id <= N48DM_TPL_DIGD_SETUP_DVI_1440; }
/* 0.0.655: the same for the MONA (instance 1): while ITS plane is HELD every monitor A template (ids 3..7: pclk-otg1-on / -off, phyc-enable-dvi, phyc-disable, digc-setup-dvi - the teardowns are 4 and 6) is refused, and so is a raw slot aimed at
 * the monitor A's DMUB context 0x7000 (DFP3, "HDMI (SINK-B)" above; SUSPECTED: the monitor A's connector object id was never established). The monitor B's refusals above are unchanged and tied to the MONB's HELD state. Pure. */
static inline int n48dm_tpl_mona_held_refuses(uint32_t id) { return id >= N48DM_TPL_PCLK_OTG1_ON && id <= N48DM_TPL_DIGC_SETUP_DVI; }
/* 0.0.653: a RAW slot that targets the monitor B's DMUB context 0x7800 (DFP4): detect / enable / disable carry it in d1, set-mode in d2; begin / end carry no context and are unaffected. Judged on an otherwise allowlisted slot
 * (a malformed one keeps its own refusal). While HELD the glue refuses these with N48DR_HELD_REFUSED. Pure. */
static inline int n48dm_slot_held_refuses(uint32_t h, uint32_t d1, uint32_t d2)
{
    if (n48dm_slot_check(h, d1, d2) != 0u) return 0;
    switch ((h >> 8) & 0xFFu) {
    case 0x0au: case 0x07u: case 0x08u: return d1 == N48DM_CTX_DFP4;
    case 0x05u: return d2 == N48DM_CTX_DFP4;
    default: return 0;
    }
}
static inline int n48dm_slot_mona_held_refuses(uint32_t h, uint32_t d1, uint32_t d2)      /* 0.0.655: the monitor A's context 0x7000 */
{
    if (n48dm_slot_check(h, d1, d2) != 0u) return 0;
    switch ((h >> 8) & 0xFFu) {
    case 0x0au: case 0x07u: case 0x08u: return d1 == N48DM_CTX_DFP3;
    case 0x05u: return d2 == N48DM_CTX_DFP3;
    default: return 0;
    }
}
static inline int n48dm_hdr_is_vbios_new(uint32_t h) { return h == N48DM_HDR_PCLK || h == N48DM_HDR_XMIT || h == N48DM_HDR_DIGENC; }
// The 64-byte slot of a template: the five dwords, then eleven zero dwords.
static inline void n48dm_tpl_slot(uint32_t slot[N48DM_SLOT_DWORDS], const struct n48dm_tpl *t)
{
    unsigned i;
    for (i = 0; i < N48DM_SLOT_DWORDS; i++) slot[i] = i < 5u ? t->dw[i] : 0u;
}
// 0 = this 16-dword slot is EXACTLY one of the fifteen templates; else N48DR_PAYLOAD_REFUSED.
static inline uint32_t n48dm_tpl_slot_check(const uint32_t slot[N48DM_SLOT_DWORDS])
{
    unsigned t, i;
    for (t = 0; t < N48DM_TPL_COUNT; t++) {
        int same = 1;
        for (i = 0; i < N48DM_SLOT_DWORDS; i++)
            if (slot[i] != (i < 5u ? n48dm_tpls[t].dw[i] : 0u)) same = 0;
        if (same) return 0u;
    }
    return (uint32_t)N48DR_PAYLOAD_REFUSED;
}
// the poll bound (steps of N48DM_POLL_STEP_US) by header: 2 s for the three mainline sub-types (0, 1, 2), 100 ms for everything else
static inline uint32_t n48dm_poll_max_for(uint32_t h) { return n48dm_hdr_is_vbios_new(h) ? N48DM_POLL_MAX_VBIOS : N48DM_POLL_MAX; }
// dmubsend's template argument: N48DM_TPL_TAG in bits 0..31 (never an allowlisted header or the replay tag), the template ID in bits 32..39, bits 40..63 zero.
#define N48DM_TPL_TAG 0x314c5054u        /* "TPL1" */
static inline uint64_t n48dm_tpl_arg(uint32_t id) { return (uint64_t)N48DM_TPL_TAG | ((uint64_t)(id & 0xFFu) << 32); }
static inline int n48dm_is_tpl_arg(uint64_t a) { return (uint32_t)(a & 0xFFFFFFFFull) == N48DM_TPL_TAG; }
// 0 = well-formed (*id set, 1..15); else N48DR_PAYLOAD_REFUSED
static inline uint32_t n48dm_tpl_unarg(uint64_t a, uint32_t *id)
{
    *id = N48DM_TPL_NONE;
    if (!n48dm_is_tpl_arg(a) || (a >> 40) != 0u) return N48DR_PAYLOAD_REFUSED;
    *id = (uint32_t)((a >> 32) & 0xFFu);
    if (n48dm_tpl_get(*id) == 0) { *id = N48DM_TPL_NONE; return N48DR_PAYLOAD_REFUSED; }
    return 0u;
}

// ---- dmubmode ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
#define N48DM_MODE_SAVE 1u
#define N48DM_MODE_RESTORE 2u
#define N48DM_VBE_1080P 0x1a6u
#define N48DM_VBE_DP 0x1d4u
// The plan's 1080p block and the measured DP block (an internal design note M1.5 section 4, step 2, and Rollback step 1; the DP block is notes/logs/runs/m1-622/region4.txt +0x4c00..+0x4c1f).
static const uint32_t n48dm_block_1a6[N48DM_MODE_DWORDS] = { 0x04380780u, 0x00020780u, 0x41a60001u, 0x08081008u, 0x00000008u, 0x3a02007fu, 0x00000000u, 0x00000000u };
static const uint32_t n48dm_block_1d4[N48DM_MODE_DWORDS] = { 0x05a00a00u, 0x00020a00u, 0x41d40001u, 0x08081008u, 0x00000008u, 0x000000e2u, 0x00000000u, 0x00000000u };
static inline int n48dm_mode_op_ok(uint64_t op) { return op == N48DM_MODE_SAVE || op == N48DM_MODE_RESTORE || op == N48DM_VBE_1080P || op == N48DM_VBE_DP; }
struct n48dm_mode_state {
    uint32_t saved, dirty;               /* saved: a copy exists; dirty: a block was written since the last save / restore */
    uint32_t buf[N48DM_MODE_DWORDS];
};
// 0 = allowed; else the refusing status. save: refused while dirty (it would overwrite the original with our own block). restore and a VBE write: refused without a saved copy.
static inline uint32_t n48dm_mode_gate(uint64_t op, const struct n48dm_mode_state *s)
{
    if (!n48dm_mode_op_ok(op)) return N48DR_MODE_REFUSED;
    if (op == N48DM_MODE_SAVE) return s->dirty ? (uint32_t)N48DR_SAVE_DIRTY : 0u;
    return s->saved ? 0u : (uint32_t)N48DR_NO_SAVE;
}
// The 8 dwords a write op puts at +0x4c00 (restore = the saved copy).
static inline void n48dm_mode_words(uint64_t op, const struct n48dm_mode_state *s, uint32_t out[N48DM_MODE_DWORDS])
{
    unsigned i;
    for (i = 0; i < N48DM_MODE_DWORDS; i++)
        out[i] = op == N48DM_VBE_1080P ? n48dm_block_1a6[i] : op == N48DM_VBE_DP ? n48dm_block_1d4[i] : s->buf[i];
}

// ---- dmubctx ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Argument: ctx (bits 0..15) | value << 32; bits 16..31 must be zero. Only 0x7000 and 0x7800: NEVER 0x6800, the live DP context. (n48dm_ctx_ok is defined with the allowlist above.)
static inline uint64_t n48dm_ctx_arg(uint32_t ctx, uint32_t value) { return (uint64_t)(ctx & 0xFFFFu) | ((uint64_t)value << 32); }
static inline uint32_t n48dm_ctx_gate(uint64_t arg)
{
    return ((arg & 0xFFFF0000ull) == 0u && n48dm_ctx_ok((uint32_t)(arg & 0xFFFFu))) ? 0u : (uint32_t)N48DR_CTX_REFUSED;
}

// ---- the result layouts (13 scalars) ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
// 95: v[0] = status | sent << 8 | vars << 9 | polls << 32     v[1] = old WPTR | new WPTR << 32     v[2] = RPTR at the end | elapsed_us << 32     v[3] = OTG0 frames before | after << 32
//     v[4] = window base (VRAM offset)     v[5] = header | d1 << 32     v[6] = d2 | RPTR at the start << 32
struct n48dm_send_out {
    uint32_t status, sent, vars, polls, old_wptr, new_wptr, rptr_end, elapsed_us, frames0, frames1, hdr, d1, d2, rptr_start;
    uint64_t base;
};
static inline void n48dm_send_pack(uint64_t *v, const struct n48dm_send_out *o)
{
    unsigned i;
    for (i = 0; i < 13u; i++) v[i] = 0u;
    v[0] = (uint64_t)(o->status & 0xFFu) | ((uint64_t)(o->sent & 1u) << 8) | ((uint64_t)(o->vars & 1u) << 9) | ((uint64_t)o->polls << 32);
    v[1] = (uint64_t)o->old_wptr | ((uint64_t)o->new_wptr << 32);
    v[2] = (uint64_t)o->rptr_end | ((uint64_t)o->elapsed_us << 32);
    v[3] = (uint64_t)o->frames0 | ((uint64_t)o->frames1 << 32);
    v[4] = o->base;
    v[5] = (uint64_t)o->hdr | ((uint64_t)o->d1 << 32);
    v[6] = (uint64_t)o->d2 | ((uint64_t)o->rptr_start << 32);
}
static inline void n48dm_send_unpack(const uint64_t *v, struct n48dm_send_out *o)
{
    o->status = (uint32_t)(v[0] & 0xFFu); o->sent = (uint32_t)((v[0] >> 8) & 1u); o->vars = (uint32_t)((v[0] >> 9) & 1u); o->polls = (uint32_t)(v[0] >> 32);
    o->old_wptr = (uint32_t)v[1]; o->new_wptr = (uint32_t)(v[1] >> 32);
    o->rptr_end = (uint32_t)v[2]; o->elapsed_us = (uint32_t)(v[2] >> 32);
    o->frames0 = (uint32_t)v[3]; o->frames1 = (uint32_t)(v[3] >> 32);
    o->base = v[4];
    o->hdr = (uint32_t)v[5]; o->d1 = (uint32_t)(v[5] >> 32);
    o->d2 = (uint32_t)v[6]; o->rptr_start = (uint32_t)(v[6] >> 32);
}
// 96: v[0] = status | op << 8 (12 bits) | saved << 24 | dirty << 25     v[1] = window base     v[2..5] = the 8 dwords of the block now in the window (read back) or, for a refusal, 0     v[6..9] = the 8 dwords read before the write
// 97: v[0] = status     v[1] = window base     v[2] = ctx | (value requested) << 32     v[3] = the dword before | the dword read back << 32
static inline void n48dm_dwords_pack(uint64_t *v, unsigned firstSlot, const uint32_t *d, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) v[firstSlot + i / 2u] |= (uint64_t)d[i] << (32u * (i & 1u));
}
static inline void n48dm_dwords_unpack(const uint64_t *v, unsigned firstSlot, uint32_t *d, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) d[i] = (uint32_t)(v[firstSlot + i / 2u] >> (32u * (i & 1u)));
}

#ifdef __cplusplus
}
#endif
#endif /* N48_DMUBCMD_H */
