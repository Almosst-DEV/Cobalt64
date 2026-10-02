/*
 * dcn41_dmub.h - DMUB (display microcontroller) access for DCN 4.1: read-only state probe, the GPINT register
 * channel, the inbox1 command ring, and byte-exact builders for the VBIOS-service commands that enable an output.
 *
 * NOT WIRED INTO ANY BUILD YET. Same rules as dcn41.h: C11/C++17, no heap, no floating point, every register from
 * dcn41_regs.h. The design and the evidence for every choice are in notes/DISPLAY-DESIGN.md (DMUB sections).
 *
 * Linux references (238650ef6c7c, MIT):
 *   ABI and ring        display/dmub/inc/dmub_cmd.h (struct dmub_cmd_header, union dmub_rb_cmd, dmub_rb_*),
 *                       include/atomfirmware.h (the *_parameters_v1_x payloads)
 *   register access     display/dmub/src/dmub_dcn401.c (reset, gpint, inbox1 pointers, boot status)
 *   service logic       display/dmub/src/dmub_srv.c (send_gpint_command, fb_cmd_queue/_execute, wait_for_idle)
 *   command builders    display/dc/bios/command_table2.c (encoder_control_digx_v1_5, transmitter_control_v1_7,
 *                       set_pixel_clock_v7, enable_disp_power_gating_v2_1), dc/dc_dmub_srv.c (query caps)
 * Layout: dcn41_dmub_layout.h holds _Static_asserts GENERATED from Linux's own headers (tools/dcn41/gen_dmub_layout.py)
 * for every struct size, field offset, header bit position and enum value below.
 * Byte and register equivalence: tools/dcn41/harness.py (dmub scenarios) against Linux's compiled command_table2.c,
 * dmub_srv.c and dmub_dcn401.c.
 */
#ifndef N48_DCN41_DMUB_H
#define N48_DCN41_DMUB_H

#include "dcn41.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ABI constants (dmub_cmd.h) ------------------------------------------------------------------------ */
#define DCN41_DMUB_CMD_SIZE 64u                   /* DMUB_RB_CMD_SIZE */
#define DCN41_DMUB_RB_SIZE (64u * 128u)           /* DMUB_RB_SIZE = DMUB_RB_CMD_SIZE * DMUB_RB_MAX_ENTRY */
#define DCN41_DMUB_CMD_QUERY_FEATURE_CAPS 6u      /* DMUB_CMD__QUERY_FEATURE_CAPS */
#define DCN41_DMUB_CMD_VBIOS 128u                 /* DMUB_CMD__VBIOS */
#define DCN41_DMUB_VBIOS_DIGX_ENCODER_CONTROL 0u
#define DCN41_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL 1u
#define DCN41_DMUB_VBIOS_SET_PIXEL_CLOCK 2u
#define DCN41_DMUB_VBIOS_ENABLE_DISP_POWER_GATING 3u
#define DCN41_DMUB_FW_META_MAGIC 0x444D5542u      /* "DMUB" */
#define DCN41_DMUB_GPINT_STOP_FW_RESPONSE 0xDEADDEADu
#define DCN41_DMUB_GPINT_GET_FW_VERSION 1u        /* DMUB_GPINT__GET_FW_VERSION: "Queries the firmware version." */

/* struct dmub_cmd_header bit positions (encoded explicitly; dcn41_dmub_layout.h checks them against Linux's bitfields) */
#define DCN41_DMUB_HDR_TYPE_SHIFT 0u
#define DCN41_DMUB_HDR_TYPE_MASK 0x000000FFu
#define DCN41_DMUB_HDR_SUB_TYPE_SHIFT 8u
#define DCN41_DMUB_HDR_SUB_TYPE_MASK 0x0000FF00u
#define DCN41_DMUB_HDR_RET_STATUS_MASK 0x00010000u
#define DCN41_DMUB_HDR_MULTI_CMD_PENDING_MASK 0x00020000u
#define DCN41_DMUB_HDR_IS_REG_BASED_MASK 0x00040000u
#define DCN41_DMUB_HDR_PAYLOAD_BYTES_SHIFT 24u
#define DCN41_DMUB_HDR_PAYLOAD_BYTES_MASK 0x3F000000u

/* union dmub_gpint_data_register (DMCUB_GPINT_DATAIN1): param[15:0], command_code[27:16], status[31:28] */
#define DCN41_DMUB_GPINT_PARAM_MASK 0x0000FFFFu
#define DCN41_DMUB_GPINT_COMMAND_SHIFT 16u
#define DCN41_DMUB_GPINT_COMMAND_MASK 0x0FFF0000u
#define DCN41_DMUB_GPINT_STATUS_SHIFT 28u
#define DCN41_DMUB_GPINT_STATUS_MASK 0xF0000000u

/* union dmub_fw_boot_status (DMCUB_SCRATCH0) */
#define DCN41_DMUB_BOOT_DAL_FW 0x001u
#define DCN41_DMUB_BOOT_MAILBOX_RDY 0x002u
#define DCN41_DMUB_BOOT_OPTIMIZED_INIT_DONE 0x004u
#define DCN41_DMUB_BOOT_RESTORE_REQUIRED 0x008u
#define DCN41_DMUB_BOOT_DEFER_LOAD 0x010u
#define DCN41_DMUB_BOOT_FAMS_ENABLED 0x020u
#define DCN41_DMUB_BOOT_DETECTION_REQUIRED 0x040u
#define DCN41_DMUB_BOOT_HW_POWER_INIT_DONE 0x080u
#define DCN41_DMUB_BOOT_ONO_REGIONS_ENABLED 0x100u

/* firmware address-space constants of the two mailbox mappings (dmub_srv.c DMUB_CW4_BASE; dmub_dcn20.c setup_mailbox
 * writes 0x80000000 to DMCUB_INBOX1_BASE_ADDRESS when the inbox is uncached and mapped through REGION4) */
#define DCN41_DMUB_CW4_BASE 0x64000000u
#define DCN41_DMUB_REGION4_INBOX1_BASE 0x80000000u

#pragma pack(push, 1)
/* union dmub_rb_cmd: one 64-byte ring entry */
struct dcn41_dmub_cmd {
    uint32_t header;
    uint8_t payload[60];
};
/* struct dig_encoder_stream_setup_parameters_v1_5 (atomfirmware.h) */
struct dcn41_dmub_encoder_stream_setup_v1_5 {
    uint8_t digid, action, digmode, lanenum;
    uint32_t pclk_10khz;
    uint8_t bitpercolor, dplinkrate_270mhz;
    uint8_t reserved[2];
};
/* struct dmub_dig_transmitter_control_data_v1_7 (dmub_cmd.h) */
struct dcn41_dmub_transmitter_control_v1_7 {
    uint8_t phyid, action, mode_laneset, lanenum;
    uint32_t symclk;                     /* symclk_10khz, or symclk_Hz for FRL (a union in Linux) */
    uint8_t hpdsel, digfe_sel, connobj_id, hpo_instance, txffe_lane_sel, skip_phy_ssc_reduction;
    uint8_t reserved2[2];
    uint32_t reserved3[11];
};
/* struct set_pixel_clock_parameter_v1_7 (atomfirmware.h) */
struct dcn41_dmub_set_pixel_clock_v1_7 {
    uint32_t pixclk_100hz;
    uint8_t pll_id, encoderobjid, encoder_mode, miscinfo, crtc_id, deep_color_ratio;
    uint8_t reserved1[2];
    uint32_t reserved2;
};
/* struct enable_disp_power_gating_parameters_v2_1 (atomfirmware.h) */
struct dcn41_dmub_disp_power_gating_v2_1 {
    uint8_t disp_pipe_id, enable;
    uint8_t padding[2];
};
/* struct dmub_feature_caps (dmub_cmd.h), the QUERY_FEATURE_CAPS reply payload */
struct dcn41_dmub_feature_caps {
    uint8_t psr, fw_assisted_mclk_switch_ver;
    uint8_t reserved[4];
    uint8_t subvp_psr_support, gecc_enable, replay_supported;
    uint8_t replay_reserved[3];
    uint8_t abm_aux_backlight_support, lsdma_support_in_dmu;
};
/* struct dmub_fw_meta_info (dmub_cmd.h): at DMUB_FW_META_OFFSET (0x24) from the end of a firmware image */
struct dcn41_dmub_fw_meta_info {
    uint32_t magic_value, fw_region_size, trace_buffer_size, fw_version;
    uint8_t dal_fw;
    uint8_t reserved[3];
    uint32_t shared_state_size;
    uint16_t shared_state_features, reserved2;
    uint32_t feature_bits;
};
#pragma pack(pop)

/* ---- read-only probe ------------------------------------------------------------------------------------ */
enum dcn41_dmub_verdict {
    DCN41_DMUB_ALIVE_IDLE = 0,       /* enabled, not in reset, powered up (dmub_dcn35_is_hw_powered_up: mailbox_rdy, plus
                                      * hw_power_init_done when dal_fw; VBIOS firmware has dal_fw 0), pointers equal */
    DCN41_DMUB_ALIVE_BUSY,           /* as above but RPTR != WPTR */
    DCN41_DMUB_NOT_RUNNING,          /* DMCUB_ENABLE clear */
    DCN41_DMUB_IN_RESET,             /* DMCUB_SOFT_RESET set */
    DCN41_DMUB_NOT_READY,            /* enabled but not powered up: mailbox_rdy clear, or dal_fw without hw_power_init_done */
    DCN41_DMUB_RING_INSANE           /* size 0, not a multiple of 64, above 64 KiB, or a pointer beyond the size */
};

enum dcn41_dmub_ring_map {
    DCN41_DMUB_MAP_UNKNOWN = 0,
    DCN41_DMUB_MAP_CW4,              /* INBOX1_BASE inside the enabled REGION3_CW4 window (cached inbox) */
    DCN41_DMUB_MAP_REGION4           /* INBOX1_BASE 0x80000000 and REGION4 enabled (uncached inbox, older firmware) */
};

struct dcn41_dmub_state {
    uint32_t cntl, cntl2, sec_cntl, scratch0, scratch7, scratch14, scratch15, gpint_datain1, fault_addr;
    uint32_t inbox1_base, inbox1_size, inbox1_wptr, inbox1_rptr;
    uint32_t outbox1_base, outbox1_size, outbox1_wptr, outbox1_rptr;
    uint32_t cw4_offset, cw4_offset_high, cw4_base, cw4_top;
    uint32_t region4_offset, region4_offset_high, region4_top;
    uint32_t fb_location_base, fb_location_top, fb_offset;
    uint8_t enabled, soft_reset, dal_fw, mailbox_rdy;
    uint8_t verdict;                 /* enum dcn41_dmub_verdict */
    uint8_t ring_map;                /* enum dcn41_dmub_ring_map */
    uint64_t ring_addr;              /* the window OFFSET value + (INBOX1_BASE - window base): GPU address of ring byte 0 */
    uint64_t fb_base_mc;             /* DCN_VM_FB_LOCATION_BASE << 24 */
};
/* Reads only (27 registers, no write, no delay). Classifies the firmware and locates the inbox1 ring. */
int dcn41_dmub_probe(struct dcn41_dev *dev, struct dcn41_dmub_state *st);

/* ---- GPINT: the register command channel ----------------------------------------------------------------- */
/* dmub_srv_send_gpint_command: DATAIN1 = status 1 | command << 16 | param; then up to timeout_us times udelay(1) and
 * compare DATAIN1 with the status nibble cleared. DCN41_E_TIMEOUT if never acknowledged. */
int dcn41_dmub_gpint(struct dcn41_dev *dev, uint32_t command, uint16_t param, uint32_t timeout_us);
/* dmub_dcn401_get_gpint_response: DMCUB_SCRATCH7 */
int dcn41_dmub_gpint_response(struct dcn41_dev *dev, uint32_t *response);

/* ---- the inbox1 ring -------------------------------------------------------------------------------------- */
typedef uint32_t (*dcn41_mem_read32_fn)(void *cookie, uint64_t gpu_addr);
typedef void (*dcn41_mem_write32_fn)(void *cookie, uint64_t gpu_addr, uint32_t value);

struct dcn41_dmub_ring {
    struct dcn41_dev *dev;
    dcn41_mem_read32_fn mread;
    dcn41_mem_write32_fn mwrite;
    void *mcookie;
    uint64_t ring_addr;              /* GPU address of ring byte 0 (dcn41_dmub_state.ring_addr) */
    uint32_t capacity;               /* DMCUB_INBOX1_SIZE */
    uint32_t wptr;                   /* dmub->inbox1.rb.wrpt: our last written pointer */
    uint32_t rptr;                   /* dmub->inbox1.rb.rptr: last hardware read pointer seen */
    uint32_t submitted, reported;
    uint32_t magic;
};

/* Attach to a live, idle ring: requires verdict ALIVE_IDLE and a known mapping. Adopts the hardware pointers the way
 * dmub_srv_sync_inbox1 does. No register writes. */
int dcn41_dmub_ring_attach(struct dcn41_dmub_ring *ring, struct dcn41_dev *dev, const struct dcn41_dmub_state *st,
                           dcn41_mem_read32_fn mread, dcn41_mem_write32_fn mwrite, void *mcookie);
/* dmub_rb_full: one entry is always unusable */
bool dcn41_dmub_ring_full(const struct dcn41_dmub_ring *ring);
/* Queue and execute one command (dmub_srv_fb_cmd_queue + dmub_srv_fb_cmd_execute): refuses if the hardware WPTR no longer
 * equals ours (someone else wrote), or the ring is full; writes the 16 dwords at wptr, reads them back and refuses on any
 * mismatch WITHOUT moving WPTR, then writes DMCUB_INBOX1_WPTR. */
int dcn41_dmub_ring_submit(struct dcn41_dmub_ring *ring, const struct dcn41_dmub_cmd *cmd);
/* dmub_srv_wait_for_idle: poll DMCUB_INBOX1_RPTR (udelay(1) between polls) until it equals our WPTR. */
int dcn41_dmub_ring_wait_idle(struct dcn41_dmub_ring *ring, uint32_t timeout_us);
/* dmub_rb_get_return_data: the entry just behind RPTR, for commands sent with ret_status set. */
int dcn41_dmub_ring_return_data(struct dcn41_dmub_ring *ring, struct dcn41_dmub_cmd *out);

/* ---- command builders (byte-identical to command_table2.c for the same ATOM-level inputs) ------------------ */
uint32_t dcn41_dmub_header(uint32_t type, uint32_t sub_type, uint32_t payload_bytes, bool ret_status);
void dcn41_dmub_cmd_zero(struct dcn41_dmub_cmd *cmd);
/* encoder_control_digx_v1_5: pixel_clock_khz / 10, scaled 30/24, 36/24 or 48/24 for HDMI at 10, 12, 16 bpc. */
void dcn41_dmub_build_encoder_control(struct dcn41_dmub_cmd *cmd, uint8_t digid, uint8_t action, uint8_t digmode,
                                      uint8_t lanenum, uint32_t pixel_clock_khz, uint8_t atom_bitpercolor,
                                      bool is_hdmi);
/* transmitter_control_v1_7: symclk = pixel_clock_khz / 10 (the symbol clock DC passes; see the design note). */
void dcn41_dmub_build_transmitter_control(struct dcn41_dmub_cmd *cmd, uint8_t phyid, uint8_t action,
                                          uint8_t mode_laneset, uint8_t lanenum, uint32_t pixel_clock_khz,
                                          uint8_t hpdsel, uint8_t digfe_sel, uint8_t connobj_id,
                                          uint8_t hpo_instance, uint8_t skip_phy_ssc_reduction);
void dcn41_dmub_build_set_pixel_clock(struct dcn41_dmub_cmd *cmd, uint32_t pixclk_100hz, uint8_t pll_id,
                                      uint8_t encoderobjid, uint8_t encoder_mode, uint8_t miscinfo, uint8_t crtc_id,
                                      uint8_t deep_color_ratio);
void dcn41_dmub_build_disp_power_gating(struct dcn41_dmub_cmd *cmd, uint8_t disp_pipe_id, uint8_t enable);
/* dc_dmub_srv_query_caps_cmd: ret_status 1, payload sizeof(struct dmub_cmd_query_feature_caps_data) */
void dcn41_dmub_build_query_feature_caps(struct dcn41_dmub_cmd *cmd);

/* ---- decoding helpers (pure) ----------------------------------------------------------------------------- */
/* dmub_get_fw_meta_info_from_blob: union dmub_fw_meta (64 bytes) ends meta_offset bytes before the blob's end; its first
 * dword must be DMUB_FW_META_MAGIC. The bss_data blob uses meta_offset 0x24 (DMUB_FW_META_OFFSET); an inst_const blob is
 * scanned at 0..15 (dcn41_dmub_fw_meta_scan). DCN41_E_ARG if too small or no magic. */
int dcn41_dmub_fw_meta_from_blob(const uint8_t *blob, uint32_t size, uint32_t meta_offset,
                                 struct dcn41_dmub_fw_meta_info *out);
int dcn41_dmub_fw_meta_scan(const uint8_t *blob, uint32_t size, struct dcn41_dmub_fw_meta_info *out);

#ifdef __cplusplus
}
#endif

#include "dcn41_dmub_layout.h"

#endif /* N48_DCN41_DMUB_H */
