// VERBATIM from lemonade-sdk/mac-amdgpu (MIT) dext/amdgpu/amdgpu_ucode_psp.h @ 3bdeed2 — platform-independent; keep in sync, do not fork.
//
//  amdgpu_ucode_psp.h — vendored verbatim from upstream amdgpu_ucode.h
//
//  AMD firmware ".bin" files have a common header followed by version-
//  specific sub-firmware descriptors. This file mirrors the relevant
//  structs and enums 1:1 from upstream so the parser can be a direct
//  port of `psp_init_sos_microcode` in amdgpu_psp.c.
//
//  Sources (Linux 6.x):
//    drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h:30-148
//    drivers/gpu/drm/amd/amdgpu/amdgpu_psp.h:93-107  (bootloader cmds)
//
//  IMPORTANT: do not "simplify" these — see [[mac-amdgpu-port-full-
//  parsers-not-shortcuts]]. Every field exists for a reason.
//

#pragma once

#include <stdint.h>

namespace amdgpu {

// ---- Common firmware header --------------------------------------------

struct common_firmware_header {
    uint32_t size_bytes;             // total file size in bytes
    uint32_t header_size_bytes;      // header struct size
    uint16_t header_version_major;   // 1 or 2
    uint16_t header_version_minor;
    uint16_t ip_version_major;       // e.g. 14 for psp_v14
    uint16_t ip_version_minor;
    uint32_t ucode_version;
    uint32_t ucode_size_bytes;       // size of payload region in bytes
    uint32_t ucode_array_offset_bytes;  // payload offset from header start
    uint32_t crc32;
} __attribute__((packed));
static_assert(sizeof(common_firmware_header) == 32,
              "common_firmware_header must be 32 bytes");

// ---- PSP v1 legacy descriptors (3 fields: version, offset, size) -------
//
// Used by older PSP firmware versions (v1.0 through v1.3). The sub-bins
// are named directly in the struct (sos, kdb, toc, etc.).

struct psp_fw_legacy_bin_desc {
    uint32_t fw_version;
    uint32_t offset_bytes;
    uint32_t size_bytes;
} __attribute__((packed));
static_assert(sizeof(psp_fw_legacy_bin_desc) == 12,
              "psp_fw_legacy_bin_desc must be 12 bytes");

// version_major=1, version_minor=0
struct psp_firmware_header_v1_0 {
    common_firmware_header     header;
    psp_fw_legacy_bin_desc     sos;
} __attribute__((packed));

// version_major=1, version_minor=1
struct psp_firmware_header_v1_1 {
    psp_firmware_header_v1_0   v1_0;
    psp_fw_legacy_bin_desc     toc;
    psp_fw_legacy_bin_desc     kdb;
} __attribute__((packed));

// version_major=1, version_minor=2
struct psp_firmware_header_v1_2 {
    psp_firmware_header_v1_0   v1_0;
    psp_fw_legacy_bin_desc     res;
    psp_fw_legacy_bin_desc     kdb;
} __attribute__((packed));

// version_major=1, version_minor=3
struct psp_firmware_header_v1_3 {
    psp_firmware_header_v1_1   v1_1;
    psp_fw_legacy_bin_desc     spl;
    psp_fw_legacy_bin_desc     rl;
    psp_fw_legacy_bin_desc     sys_drv_aux;
    psp_fw_legacy_bin_desc     sos_aux;
} __attribute__((packed));

// ---- PSP v2 typed descriptors (fw_type-tagged, flexible array) ---------
//
// Used by psp_v13+ (so RDNA3 onward, including RDNA4). Sub-bins are
// identified by fw_type enum, not by struct field name.

struct psp_fw_bin_desc {
    uint32_t fw_type;        // psp_fw_type enum
    uint32_t fw_version;
    uint32_t offset_bytes;   // relative to ucode_array_offset_bytes
    uint32_t size_bytes;
} __attribute__((packed));
static_assert(sizeof(psp_fw_bin_desc) == 16,
              "psp_fw_bin_desc must be 16 bytes");

enum psp_fw_type {
    PSP_FW_TYPE_UNKOWN              = 0,
    PSP_FW_TYPE_PSP_SOS             = 1,
    PSP_FW_TYPE_PSP_SYS_DRV         = 2,
    PSP_FW_TYPE_PSP_KDB             = 3,
    PSP_FW_TYPE_PSP_TOC             = 4,
    PSP_FW_TYPE_PSP_SPL             = 5,
    PSP_FW_TYPE_PSP_RL              = 6,
    PSP_FW_TYPE_PSP_SOC_DRV         = 7,
    PSP_FW_TYPE_PSP_INTF_DRV        = 8,
    PSP_FW_TYPE_PSP_DBG_DRV         = 9,
    PSP_FW_TYPE_PSP_RAS_DRV         = 10,
    PSP_FW_TYPE_PSP_IPKEYMGR_DRV    = 11,
    PSP_FW_TYPE_PSP_SPDM_DRV        = 12,
    PSP_FW_TYPE_MAX_INDEX,
};

// version_major=2, version_minor=0
struct psp_firmware_header_v2_0 {
    common_firmware_header     header;
    uint32_t                   psp_fw_bin_count;
    psp_fw_bin_desc            psp_fw_bin[];  // flexible array
} __attribute__((packed));

// version_major=2, version_minor=1
struct psp_firmware_header_v2_1 {
    common_firmware_header     header;
    uint32_t                   psp_fw_bin_count;
    uint32_t                   psp_aux_fw_bin_index;
    psp_fw_bin_desc            psp_fw_bin[];
} __attribute__((packed));

// ---- TA (Trusted Application) v2 layout --------------------------------
//
// psp_<chip>_ta.bin uses the same byte layout as psp_firmware_header_v2_0
// (common header + uint32_t bin_count + psp_fw_bin_desc[]) but the
// descriptors are tagged with ta_fw_type instead of psp_fw_type. Walked
// by parse_ta_v2_microcode in upstream amdgpu_psp.c:3947.

enum ta_fw_type {
    TA_FW_TYPE_UNKOWN              = 0,
    TA_FW_TYPE_PSP_ASD             = 1,
    TA_FW_TYPE_PSP_XGMI            = 2,
    TA_FW_TYPE_PSP_RAS             = 3,
    TA_FW_TYPE_PSP_HDCP            = 4,
    TA_FW_TYPE_PSP_DTM             = 5,
    TA_FW_TYPE_PSP_RAP             = 6,
    TA_FW_TYPE_PSP_SECUREDISPLAY   = 7,
    TA_FW_TYPE_PSP_XGMI_AUX        = 8,
    TA_FW_TYPE_MAX_INDEX,
};

// Same byte layout as psp_firmware_header_v2_0; renamed for clarity at
// the parser call sites.
struct ta_firmware_header_v2_0 {
    common_firmware_header     header;
    uint32_t                   ta_fw_bin_count;
    psp_fw_bin_desc            ta_fw_bin[];
} __attribute__((packed));

// =======================================================================
// Per-IP firmware headers — vendored verbatim from upstream amdgpu_ucode.h
// =======================================================================
//
// These are the secondary headers that follow `common_firmware_header`
// inside a per-IP `.bin` file. The dext's per-IP extractor reads them
// to pick out (ucode_offset, ucode_size) tuples for each LOAD_IP_FW
// frame. Layouts MUST match upstream byte-for-byte — they're the file
// format AMD ships, not an in-driver convention.

// version_major=1, version_minor=0
// Upstream: drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h:181
struct gfx_firmware_header_v1_0 {
    common_firmware_header header;
    uint32_t ucode_feature_version;
    uint32_t jt_offset;
    uint32_t jt_size;
} __attribute__((packed));
static_assert(sizeof(gfx_firmware_header_v1_0) == 32 + 12,
              "gfx_firmware_header_v1_0 must be 44 bytes");

// version_major=2, version_minor=0
// Upstream: drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h:189-198
//
// Used for RS64 CP firmwares (PFP/ME/MEC on RDNA4/gfx12). The single
// .bin file contains both ucode + data; PSP gets fed:
//   - ucode payload at (header.ucode_array_offset_bytes,
//                       cpv2_hdr->ucode_size_bytes)
//   - 1..N stack payloads ALL at (cpv2_hdr->data_offset_bytes,
//                                 cpv2_hdr->data_size_bytes) — same
//     bytes, just submitted multiple times with different fw_types
//     (P0_STACK, P1_STACK, …). See amdgpu_ucode.c:1032-1085.
struct gfx_firmware_header_v2_0 {
    common_firmware_header header;
    uint32_t ucode_feature_version;
    uint32_t ucode_size_bytes;
    uint32_t ucode_offset_bytes;
    uint32_t data_size_bytes;
    uint32_t data_offset_bytes;
    uint32_t ucode_start_addr_lo;
    uint32_t ucode_start_addr_hi;
} __attribute__((packed));
static_assert(sizeof(gfx_firmware_header_v2_0) == 32 + 28,
              "gfx_firmware_header_v2_0 must be 60 bytes");

// version_major=1, version_minor=0
// Upstream: drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h:201-213
//
// Used for both legacy mes ("amdgpu/<prefix>_mes.bin") and unified mes
// ("amdgpu/<prefix>_uni_mes.bin") packaging. ucode and data both live
// in the same .bin and are submitted as separate LOAD_IP_FW frames.
struct mes_firmware_header_v1_0 {
    common_firmware_header header;
    uint32_t mes_ucode_version;
    uint32_t mes_ucode_size_bytes;
    uint32_t mes_ucode_offset_bytes;
    uint32_t mes_ucode_data_version;
    uint32_t mes_ucode_data_size_bytes;
    uint32_t mes_ucode_data_offset_bytes;
    uint32_t mes_uc_start_addr_lo;
    uint32_t mes_uc_start_addr_hi;
    uint32_t mes_data_start_addr_lo;
    uint32_t mes_data_start_addr_hi;
} __attribute__((packed));
static_assert(sizeof(mes_firmware_header_v1_0) == 32 + 40,
              "mes_firmware_header_v1_0 must be 72 bytes");

// version_major=2, version_minor=0
// Upstream: drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h:226-246
struct rlc_firmware_header_v2_0 {
    common_firmware_header header;
    uint32_t ucode_feature_version;
    uint32_t jt_offset;
    uint32_t jt_size;
    uint32_t save_and_restore_offset;
    uint32_t clear_state_descriptor_offset;
    uint32_t avail_scratch_ram_locations;
    uint32_t reg_restore_list_size;
    uint32_t reg_list_format_start;
    uint32_t reg_list_format_separate_start;
    uint32_t starting_offsets_start;
    uint32_t reg_list_format_size_bytes;
    uint32_t reg_list_format_array_offset_bytes;
    uint32_t reg_list_size_bytes;
    uint32_t reg_list_array_offset_bytes;
    uint32_t reg_list_format_separate_size_bytes;
    uint32_t reg_list_format_separate_array_offset_bytes;
    uint32_t reg_list_separate_size_bytes;
    uint32_t reg_list_separate_array_offset_bytes;
} __attribute__((packed));
static_assert(sizeof(rlc_firmware_header_v2_0) == 32 + 18 * 4,
              "rlc_firmware_header_v2_0 must be 104 bytes");

// version_major=2, version_minor=1
// Upstream: drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h:249-264
struct rlc_firmware_header_v2_1 {
    rlc_firmware_header_v2_0 v2_0;
    uint32_t reg_list_format_direct_reg_list_length;
    uint32_t save_restore_list_cntl_ucode_ver;
    uint32_t save_restore_list_cntl_feature_ver;
    uint32_t save_restore_list_cntl_size_bytes;
    uint32_t save_restore_list_cntl_offset_bytes;
    uint32_t save_restore_list_gpm_ucode_ver;
    uint32_t save_restore_list_gpm_feature_ver;
    uint32_t save_restore_list_gpm_size_bytes;
    uint32_t save_restore_list_gpm_offset_bytes;
    uint32_t save_restore_list_srm_ucode_ver;
    uint32_t save_restore_list_srm_feature_ver;
    uint32_t save_restore_list_srm_size_bytes;
    uint32_t save_restore_list_srm_offset_bytes;
} __attribute__((packed));
static_assert(sizeof(rlc_firmware_header_v2_1) ==
                  sizeof(rlc_firmware_header_v2_0) + 13 * 4,
              "rlc_firmware_header_v2_1 must be v2_0 + 52 bytes");

// version_major=2, version_minor=2
// Upstream: drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h:267-273
struct rlc_firmware_header_v2_2 {
    rlc_firmware_header_v2_1 v2_1;
    uint32_t rlc_iram_ucode_size_bytes;
    uint32_t rlc_iram_ucode_offset_bytes;
    uint32_t rlc_dram_ucode_size_bytes;
    uint32_t rlc_dram_ucode_offset_bytes;
} __attribute__((packed));
static_assert(sizeof(rlc_firmware_header_v2_2) ==
                  sizeof(rlc_firmware_header_v2_1) + 16,
              "rlc_firmware_header_v2_2 must be v2_1 + 16 bytes");

// version_major=2, version_minor=3
// Upstream: drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h:276-286
struct rlc_firmware_header_v2_3 {
    rlc_firmware_header_v2_2 v2_2;
    uint32_t rlcp_ucode_version;
    uint32_t rlcp_ucode_feature_version;
    uint32_t rlcp_ucode_size_bytes;
    uint32_t rlcp_ucode_offset_bytes;
    uint32_t rlcv_ucode_version;
    uint32_t rlcv_ucode_feature_version;
    uint32_t rlcv_ucode_size_bytes;
    uint32_t rlcv_ucode_offset_bytes;
} __attribute__((packed));
static_assert(sizeof(rlc_firmware_header_v2_3) ==
                  sizeof(rlc_firmware_header_v2_2) + 8 * 4,
              "rlc_firmware_header_v2_3 must be v2_2 + 32 bytes");

// version_major=2, version_minor=4
// Upstream: drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h:289-301
struct rlc_firmware_header_v2_4 {
    rlc_firmware_header_v2_3 v2_3;
    uint32_t global_tap_delays_ucode_size_bytes;
    uint32_t global_tap_delays_ucode_offset_bytes;
    uint32_t se0_tap_delays_ucode_size_bytes;
    uint32_t se0_tap_delays_ucode_offset_bytes;
    uint32_t se1_tap_delays_ucode_size_bytes;
    uint32_t se1_tap_delays_ucode_offset_bytes;
    uint32_t se2_tap_delays_ucode_size_bytes;
    uint32_t se2_tap_delays_ucode_offset_bytes;
    uint32_t se3_tap_delays_ucode_size_bytes;
    uint32_t se3_tap_delays_ucode_offset_bytes;
} __attribute__((packed));
static_assert(sizeof(rlc_firmware_header_v2_4) ==
                  sizeof(rlc_firmware_header_v2_3) + 10 * 4,
              "rlc_firmware_header_v2_4 must be v2_3 + 40 bytes");

// version_major=3, version_minor=0
// Upstream: drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h:371-376
//
// Used by sdma_v7_x (RDNA4 / gfx12). Single payload, but the
// payload offset is `header.ucode_array_offset_bytes` and the
// size is `ucode_size_bytes` (a v3-specific field, NOT the
// common header's ucode_size_bytes — the common one for a v3
// .bin is the total file size). See amdgpu_ucode.c:901-905
// (AMDGPU_UCODE_ID_SDMA_RS64 case).
struct sdma_firmware_header_v3_0 {
    common_firmware_header header;
    uint32_t ucode_feature_version;
    uint32_t ucode_offset_bytes;
    uint32_t ucode_size_bytes;
} __attribute__((packed));
static_assert(sizeof(sdma_firmware_header_v3_0) == 32 + 12,
              "sdma_firmware_header_v3_0 must be 44 bytes");

// version_major=1, version_minor=0
// Upstream: drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h:432-438
//
// One .bin contains both iram + dram ucode. PSP gets two LOAD_IP_FW
// frames (IMU_I=68 and IMU_D=69). Per amdgpu_ucode.c:1021-1031, the
// IMU_D payload sits IMMEDIATELY AFTER the iram payload (no
// independent offset field for dram — it's iram_offset + iram_size).
struct imu_firmware_header_v1_0 {
    common_firmware_header header;
    uint32_t imu_iram_ucode_size_bytes;
    uint32_t imu_iram_ucode_offset_bytes;
    uint32_t imu_dram_ucode_size_bytes;
    uint32_t imu_dram_ucode_offset_bytes;
} __attribute__((packed));
static_assert(sizeof(imu_firmware_header_v1_0) == 32 + 16,
              "imu_firmware_header_v1_0 must be 48 bytes");

// ---- Bootloader command codes ------------------------------------------
//
// Written to MP0_C2PMSG_35 to tell the PSP bootloader which sub-firmware
// to load (after the host has written the buffer's bus addr >> 20 to
// MP0_C2PMSG_36).

enum psp_bootloader_cmd_e {
    PSP_BL__LOAD_SYSDRV         = 0x10000,
    PSP_BL__LOAD_SOSDRV         = 0x20000,
    PSP_BL__LOAD_KEY_DATABASE   = 0x80000,
    PSP_BL__LOAD_SOCDRV         = 0xB0000,
    PSP_BL__LOAD_DBGDRV         = 0xC0000,
    PSP_BL__LOAD_HADDRV         = 0xC0000,  // alias on psp_v14
    PSP_BL__LOAD_INTFDRV        = 0xD0000,
    PSP_BL__LOAD_RASDRV         = 0xE0000,
    PSP_BL__LOAD_IPKEYMGRDRV    = 0xF0000,
    PSP_BL__DRAM_LONG_TRAIN     = 0x100000,
    PSP_BL__DRAM_SHORT_TRAIN    = 0x200000,
    PSP_BL__LOAD_TOS_SPL_TABLE  = 0x10000000,
    PSP_BL__LOAD_SPDMDRV        = 0x20000000,
};

} // namespace amdgpu
