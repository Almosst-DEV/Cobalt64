//
//  amdgpu_ucode_extract.cpp — ported from lemonade-sdk/mac-amdgpu (MIT),
//  dext/amdgpu/amdgpu_ucode_extract.cpp @ commit 3bdeed2
//  (https://github.com/lemonade-sdk/mac-amdgpu).
//
//  Original header comment (unchanged below): port of upstream Linux
//  `amdgpu_ucode_init_single_fw` and `psp_get_fw_type` for the per-IP
//  firmware files the dext loads during bringup. See amdgpu_ucode_extract.h
//  for the overall design.
//
//  Deviations from reference:
//    - Does NOT `#include "amdgpu_psp.h"`. That header is being rewritten
//      separately for this kext (not owned by this task) and, in the
//      reference, transitively includes <DriverKit/IOBufferMemoryDescriptor.h>,
//      <DriverKit/IODMACommand.h> and <functional> — none valid in this
//      `-fapple-kext` kernel build (no DriverKit, no STL/libc++). The one
//      thing this file actually needs from it — the `PSPGfxFwType`
//      constexpr fw-type constants — is instead defined in
//      amdgpu_ucode_extract.h, guarded by `AMDGPU_PSP_GFX_FW_TYPE_DEFINED`
//      so it won't clash once the real amdgpu_psp.h exists. See that
//      header for the full explanation.
//    - No os_log / DriverKit / `%{public}s` usage to convert: this
//      particular file has zero logging calls in the reference (it's a
//      pure header-parsing/table-building module with no PSP_LOG/etc.
//      call sites), so there was nothing to port to UCODE_LOG here.
//    - `#include <string.h>` for `memset` only — within this kernel's
//      guaranteed subset (memcpy/memset/strlen/strncmp per PORTING.md).
//    - Function names/signatures, every upstream Linux file:line citation
//      in the comments, and all per-IP extraction logic are otherwise
//      unchanged from the reference — do not "simplify" a step without
//      checking upstream first (see amdgpu_ucode_extract.h).
//
//  Every case here cites the corresponding upstream file:line so a
//  reviewer can audit the (offset_bytes, size_bytes, fw_type) tuple
//  against AMD's reference driver.
//
//  Constraints baked in:
//    - For psp_v14_0 / GMC v12 / gfx12 / RDNA4 specifically.
//    - We don't support legacy SDMA v1/v2 packaging (not shipped on
//      R9700); RDNA4 always uses sdma_firmware_header_v3_0.
//    - cp_v12_0 (RDNA4) always uses RS64 firmwares; legacy CP_ME etc.
//      paths are not exercised.
//

#include "amdgpu_ucode_extract.h"
#include "amdgpu_ucode_psp.h"

#include <string.h>
#include <stdint.h>

namespace amdgpu {

namespace {

// Lightweight bounds check: confirm `[off, off+sz)` lies entirely
// inside the .bin file. Returns false on overflow or out-of-range.
inline bool within(uint64_t off, uint64_t sz, uint64_t file_size) {
    if (sz == 0) return false;            // empty payloads are skipped upstream
    if (off >= file_size) return false;
    if (off + sz > file_size) return false;
    return true;
}

// Push a single payload onto out[]. Returns the new count, or `count`
// if the bounds check fails (skip-on-zero matches upstream — every
// rlc_v2_x init function skips sub-bins whose size_bytes==0).
inline uint32_t push(UcodePayload out[], uint32_t count,
                     uint32_t fw_type,
                     uint64_t offset, uint64_t size,
                     uint64_t file_size) {
    if (count >= kMaxUcodePayloadsPerFile) return count;
    if (!within(offset, size, file_size)) return count;
    out[count].fw_type      = fw_type;
    out[count].offset_bytes = static_cast<uint32_t>(offset);
    out[count].size_bytes   = static_cast<uint32_t>(size);
    return count + 1;
}

// =====================================================================
// Per-IP extractors. Each takes the raw .bin file and emits one or more
// payloads into out[]. Returns the count.
// =====================================================================

// SMU is handled by the common-header path below (extract_common_header
// with caller-supplied PSPGfxFwType::SMU=18). Upstream amdgpu_ucode.c:1107-1111
// (the default switch branch) is identical to that path; SMU
// (AMDGPU_UCODE_ID_SMC → GFX_FW_TYPE_SMU, amdgpu_psp.c:2743-2744)
// doesn't need an IP-specific helper.

// SDMA on RDNA4 (sdma_v7_1) — packaging is sdma_firmware_header_v3_0,
// fw_type is SDMA_UCODE_TH0 (= 71). Upstream:
//   - amdgpu_sdma.c:291-299 (init: case 3 → AMDGPU_UCODE_ID_SDMA_RS64)
//   - amdgpu_ucode.c:901-905 (load: AMDGPU_UCODE_ID_SDMA_RS64 uses
//     `header.ucode_array_offset_bytes` + `sdmav3_hdr->ucode_size_bytes`)
//   - amdgpu_psp.c:2779-2782 (psp_get_fw_type:
//     AMDGPU_UCODE_ID_SDMA_RS64 → GFX_FW_TYPE_SDMA_UCODE_TH0 = 71)
uint32_t extract_sdma(const uint8_t *bin, uint64_t size,
                      UcodePayload out[]) {
    if (size < sizeof(sdma_firmware_header_v3_0)) return 0;
    auto *hdr = reinterpret_cast<const sdma_firmware_header_v3_0 *>(bin);
    // Only v3.0 is supported on RDNA4. Reject anything else; caller
    // will fall back to common-header path.
    if (hdr->header.header_version_major != 3) return 0;
    return push(out, 0, PSPGfxFwType::SDMA_UCODE_TH0,
                hdr->header.ucode_array_offset_bytes,
                hdr->ucode_size_bytes, size);
}

// RLC. Single .bin file emits up to ~13 sub-firmwares depending on
// header_version_minor. Upstream dispatch:
//   amdgpu_rlc.c:552-585 amdgpu_gfx_rlc_init_microcode
// Each minor-specific helper (v2_1 / v2_2 / v2_3 / v2_4 / v2_5) emits
// the sub-bins whose size_bytes is non-zero. We mirror that exactly.
//
// Order (CRITICAL: must match upstream's `firmware.ucode[]` iteration,
// which is the AMDGPU_UCODE_ID_* enum order at amdgpu_ucode.h:515-529:
//   TAP_DELAYS (v2.4)
//   RESTORE_LIST_CNTL / GPM_MEM / SRM_MEM (v2.1)
//   RLC_IRAM / RLC_DRAM (v2.2)
//   RLC_P / RLC_V (v2.3)
//   RLC_G LAST (v2.0)
//
// RLC_G must be emitted LAST — upstream `psp_load_non_psp_fw`
// triggers `psp_rlc_autoload_start` right after RLC_G loads
// (amdgpu_psp.c:3113-3121), and PSP rejects subsequent RLC sub-bins
// with TEE_BAD_PARAMETERS once autoload has been kicked.
//
// Offset fields are file-relative when computed as
//   (struct-pointer + offset_bytes) - bin
// which simplifies to plain offset_bytes since the struct lives at the
// start of bin. Upstream's `(u8 *)rlc_hdr + offset_bytes` is the same.
uint32_t extract_rlc(const uint8_t *bin, uint64_t size,
                     UcodePayload out[]) {
    if (size < sizeof(rlc_firmware_header_v2_0)) return 0;
    auto *hv2_0 = reinterpret_cast<const rlc_firmware_header_v2_0 *>(bin);
    uint16_t maj = hv2_0->header.header_version_major;
    uint16_t mnr = hv2_0->header.header_version_minor;
    if (maj != 2) return 0;

    uint32_t n = 0;

    // ---- v2.4 — tap delays (first per upstream enum 515-519) --------
    if (mnr == 4 && size >= sizeof(rlc_firmware_header_v2_4)) {
        auto *hv2_4 = reinterpret_cast<const rlc_firmware_header_v2_4 *>(bin);
        n = push(out, n, PSPGfxFwType::GLOBAL_TAP_DELAYS,
                 hv2_4->global_tap_delays_ucode_offset_bytes,
                 hv2_4->global_tap_delays_ucode_size_bytes, size);
        n = push(out, n, PSPGfxFwType::SE0_TAP_DELAYS,
                 hv2_4->se0_tap_delays_ucode_offset_bytes,
                 hv2_4->se0_tap_delays_ucode_size_bytes, size);
        n = push(out, n, PSPGfxFwType::SE1_TAP_DELAYS,
                 hv2_4->se1_tap_delays_ucode_offset_bytes,
                 hv2_4->se1_tap_delays_ucode_size_bytes, size);
        n = push(out, n, PSPGfxFwType::SE2_TAP_DELAYS,
                 hv2_4->se2_tap_delays_ucode_offset_bytes,
                 hv2_4->se2_tap_delays_ucode_size_bytes, size);
        n = push(out, n, PSPGfxFwType::SE3_TAP_DELAYS,
                 hv2_4->se3_tap_delays_ucode_offset_bytes,
                 hv2_4->se3_tap_delays_ucode_size_bytes, size);
    }

    // ---- v2.1 — save/restore lists (enum 520-522) -------------------
    if (mnr >= 1 && size >= sizeof(rlc_firmware_header_v2_1)) {
        auto *hv2_1 = reinterpret_cast<const rlc_firmware_header_v2_1 *>(bin);
        n = push(out, n, PSPGfxFwType::RLC_RESTORE_LIST_SRM_CNTL,
                 hv2_1->save_restore_list_cntl_offset_bytes,
                 hv2_1->save_restore_list_cntl_size_bytes, size);
        n = push(out, n, PSPGfxFwType::RLC_RESTORE_LIST_GPM_MEM,
                 hv2_1->save_restore_list_gpm_offset_bytes,
                 hv2_1->save_restore_list_gpm_size_bytes, size);
        n = push(out, n, PSPGfxFwType::RLC_RESTORE_LIST_SRM_MEM,
                 hv2_1->save_restore_list_srm_offset_bytes,
                 hv2_1->save_restore_list_srm_size_bytes, size);
    }

    // ---- v2.2 — IRAM + DRAM (enum 523-524) --------------------------
    if (mnr >= 2 && size >= sizeof(rlc_firmware_header_v2_2)) {
        auto *hv2_2 = reinterpret_cast<const rlc_firmware_header_v2_2 *>(bin);
        n = push(out, n, PSPGfxFwType::RLC_IRAM,
                 hv2_2->rlc_iram_ucode_offset_bytes,
                 hv2_2->rlc_iram_ucode_size_bytes, size);
        n = push(out, n, PSPGfxFwType::RLC_DRAM_BOOT,
                 hv2_2->rlc_dram_ucode_offset_bytes,
                 hv2_2->rlc_dram_ucode_size_bytes, size);
    }

    // ---- v2.3 — RLC_P + RLC_V (enum 527-528) ------------------------
    if (mnr == 3 && size >= sizeof(rlc_firmware_header_v2_3)) {
        auto *hv2_3 = reinterpret_cast<const rlc_firmware_header_v2_3 *>(bin);
        n = push(out, n, PSPGfxFwType::RLC_P,
                 hv2_3->rlcp_ucode_offset_bytes,
                 hv2_3->rlcp_ucode_size_bytes, size);
        n = push(out, n, PSPGfxFwType::RLC_V,
                 hv2_3->rlcv_ucode_offset_bytes,
                 hv2_3->rlcv_ucode_size_bytes, size);
    }

    // ---- v2.0 — RLC_G LAST (enum 529 — triggers rlc_autoload_start) -
    // amdgpu_psp.c:3113-3121 calls psp_rlc_autoload_start immediately
    // after RLC_G loads — anything submitted after this point gets
    // rejected by SOS as TEE_BAD_PARAMETERS.
    n = push(out, n, PSPGfxFwType::RLC_G,
             hv2_0->header.ucode_array_offset_bytes,
             hv2_0->header.ucode_size_bytes, size);

    return n;
}

// IMU (one .bin → two LOAD_IP_FW frames: IMU_I + IMU_D).
// Upstream:
//   imu_v12_0.c:60-75   (sets IMU_I and IMU_D ucode_ids)
//   amdgpu_ucode.c:1021-1031 (IMU_I uses header.ucode_array_offset_bytes
//                              + imu_hdr->imu_iram_ucode_size_bytes;
//                              IMU_D uses header.ucode_array_offset_bytes
//                              + imu_iram_ucode_size_bytes (i.e. starts
//                              right after iram) + imu_dram_ucode_size_bytes)
//   amdgpu_psp.c:2786-2791 (IMU_I → GFX_FW_TYPE_IMU_I=68;
//                            IMU_D → GFX_FW_TYPE_IMU_D=69)
uint32_t extract_imu(const uint8_t *bin, uint64_t size,
                     UcodePayload out[]) {
    if (size < sizeof(imu_firmware_header_v1_0)) return 0;
    auto *hdr = reinterpret_cast<const imu_firmware_header_v1_0 *>(bin);
    if (hdr->header.header_version_major != 1) return 0;
    uint32_t n = 0;
    n = push(out, n, PSPGfxFwType::IMU_I,
             hdr->header.ucode_array_offset_bytes,
             hdr->imu_iram_ucode_size_bytes, size);
    // IMU_D starts immediately after IMU_I in the same file — there is
    // no independent dram offset field. Mirror amdgpu_ucode.c:1027-1030.
    n = push(out, n, PSPGfxFwType::IMU_D,
             static_cast<uint64_t>(hdr->header.ucode_array_offset_bytes) +
                 hdr->imu_iram_ucode_size_bytes,
             hdr->imu_dram_ucode_size_bytes, size);
    return n;
}

// RS64 CP firmware (PFP / ME / MEC). Single .bin → ucode + per-pipe
// stack copies. Upstream:
//   gfx_v12_0.c:605-642 (init_microcode: PFP+P0_STACK, ME+P0_STACK,
//                         MEC+P0_STACK+P1_STACK)
//   amdgpu_ucode.c:1032-1085 (init_single_fw for RS64 ucode + stacks)
//   amdgpu_psp.c:2792-2823 (psp_get_fw_type RS64_PFP/ME/MEC/_Px_STACK)
//
// On RDNA4 + GC12.0.x:
//   PFP file emits: RS64_PFP (87) + RS64_PFP_P0_STACK (90) + RS64_PFP_P1_STACK (91)
//   ME  file emits: RS64_ME  (88) + RS64_ME_P0_STACK  (92) + RS64_ME_P1_STACK  (93)
//   MEC file emits: RS64_MEC (89) + RS64_MEC_P0_STACK (94) + RS64_MEC_P1_STACK (95)
//                                 + RS64_MEC_P2_STACK (96) + RS64_MEC_P3_STACK (97)
//
// The "stack" payloads all point at the same (data_offset_bytes,
// data_size_bytes) — upstream re-uses the same bytes per pipe, only
// the fw_type changes (so PSP routes the same data into different
// per-pipe scratch regions). See amdgpu_ucode.c:1037-1045 etc. —
// every CP_RS64_*_STACK case has identical (data_offset, data_size).

enum class RS64Family { PFP, ME, MEC };

uint32_t extract_cp_rs64(const uint8_t *bin, uint64_t size,
                         RS64Family family,
                         UcodePayload out[]) {
    if (size < sizeof(gfx_firmware_header_v2_0)) return 0;
    auto *hdr = reinterpret_cast<const gfx_firmware_header_v2_0 *>(bin);
    if (hdr->header.header_version_major != 2) return 0;

    uint32_t n = 0;
    uint32_t ucode_fw_type = 0;
    uint32_t stack_fw_types[4] = {0,0,0,0};
    uint32_t num_stacks = 0;

    // Stack counts MUST match upstream gfx_v12_0_init_microcode
     // (gfx_v12_0.c:605-642). For gfx_v12_0 (RDNA4) registered stacks:
    //   PFP: P0 only          (num_pipe_per_me=1)
    //   ME:  P0 only          (num_pipe_per_me=1)
    //   MEC: P0 + P1 only     (num_pipe_per_mec=2)
    // Older gfx_v11_0 had 2/2/4 stacks (P0+P1, P0+P1, P0..P3). We
    // initially emitted gfx_v11_0 counts which made PSP accept extra
    // fw_types (91, 93, 96, 97) that don't have TMR slots on gfx_v12_0
    // — those slots end up either discarded or potentially corrupting
    // adjacent slots.
    switch (family) {
    case RS64Family::PFP:
        ucode_fw_type   = PSPGfxFwType::RS64_PFP;
        stack_fw_types[0] = PSPGfxFwType::RS64_PFP_P0;
        num_stacks = 1;
        break;
    case RS64Family::ME:
        ucode_fw_type   = PSPGfxFwType::RS64_ME;
        stack_fw_types[0] = PSPGfxFwType::RS64_ME_P0;
        num_stacks = 1;
        break;
    case RS64Family::MEC:
        ucode_fw_type   = PSPGfxFwType::RS64_MEC;
        stack_fw_types[0] = PSPGfxFwType::RS64_MEC_P0;
        stack_fw_types[1] = PSPGfxFwType::RS64_MEC_P1;
        num_stacks = 2;
        break;
    }

    // ucode portion — amdgpu_ucode.c:1032-1036, 1047-1051, 1062-1066:
    //   ucode_size = cpv2_hdr->ucode_size_bytes
    //   addr       = fw->data + header.ucode_array_offset_bytes
    n = push(out, n, ucode_fw_type,
             hdr->header.ucode_array_offset_bytes,
             hdr->ucode_size_bytes, size);

    // Stack portions — amdgpu_ucode.c:1037-1086:
    //   data_size = cpv2_hdr->data_size_bytes
    //   addr      = fw->data + cpv2_hdr->data_offset_bytes
    // identical (offset, size) repeated with different fw_types.
    for (uint32_t i = 0; i < num_stacks; i++) {
        n = push(out, n, stack_fw_types[i],
                 hdr->data_offset_bytes,
                 hdr->data_size_bytes, size);
    }
    return n;
}

// uni_mes / mes packaging. Single .bin → CP_MES (ucode) + CP_MES_DATA
// (data). Upstream:
//   amdgpu_mes.c:719-743 (init: ucode + ucode_data)
//   amdgpu_ucode.c:976-995 (init_single_fw: mes_ucode_size_bytes /
//                            mes_ucode_offset_bytes for ucode;
//                            mes_ucode_data_size_bytes /
//                            mes_ucode_data_offset_bytes for data)
//   amdgpu_psp.c:2665-2676 (CP_MES → 33, CP_MES_DATA / MES_STACK → 34)
//
// gfx_v12_0 default is `enable_uni_mes=true` (amdgpu_discovery.c:2700-2701)
// and mes_v12_0_early_init loops over AMDGPU_MAX_MES_PIPES=2, registering
// both SCHED (pipe 0 → CP_MES=33 + CP_MES_DATA=34) and KIQ (pipe 1 →
// CP_MES_KIQ=81 + MES_KIQ_STACK=82) from the SAME uni_mes.bin bytes.
// PSP's autoload manifest references both pipes — without the KIQ
// payloads in TMR, AUTOLOAD_RLC accepts the cmd but the autoload state
// machine bails silently when it can't find the KIQ slot.
uint32_t extract_mes(const uint8_t *bin, uint64_t size,
                     UcodePayload out[]) {
    if (size < sizeof(mes_firmware_header_v1_0)) return 0;
    auto *hdr = reinterpret_cast<const mes_firmware_header_v1_0 *>(bin);
    if (hdr->header.header_version_major != 1) return 0;
    uint32_t n = 0;
    // SCHED pipe (pipe 0)
    n = push(out, n, PSPGfxFwType::CP_MES,
             hdr->mes_ucode_offset_bytes,
             hdr->mes_ucode_size_bytes, size);
    n = push(out, n, PSPGfxFwType::CP_MES_DATA,
             hdr->mes_ucode_data_offset_bytes,
             hdr->mes_ucode_data_size_bytes, size);
    // KIQ pipe (pipe 1) — same bytes, different fw_type tag
    n = push(out, n, PSPGfxFwType::CP_MES_KIQ,
             hdr->mes_ucode_offset_bytes,
             hdr->mes_ucode_size_bytes, size);
    n = push(out, n, PSPGfxFwType::MES_KIQ_STACK,
             hdr->mes_ucode_data_offset_bytes,
             hdr->mes_ucode_data_size_bytes, size);
    return n;
}

// =====================================================================
// Single-payload fallback for backward-compatible 0x100+psp_fw_type
// callers (SMU, single-type IMU_I/IMU_D, etc.). Just uses the common
// header. Matches the default branch in amdgpu_ucode.c:1107-1111.
// =====================================================================
uint32_t extract_common_header(uint32_t psp_fw_type,
                               const uint8_t *bin, uint64_t size,
                               UcodePayload out[]) {
    if (size < sizeof(common_firmware_header)) return 0;
    auto *hdr = reinterpret_cast<const common_firmware_header *>(bin);
    return push(out, 0, psp_fw_type,
                hdr->ucode_array_offset_bytes,
                hdr->ucode_size_bytes, size);
}

} // anonymous namespace

// =====================================================================
// Public entrypoint.
// =====================================================================

// Host fwType encoding:
//   0x000..0x0FF   pre-SOS bootloader components (handled elsewhere)
//   0x100..0x1FF   single-payload IP firmware (psp_fw_type = hostFwType-0x100)
//                  legacy / backward-compatible; used by SMU and any IP
//                  whose .bin is a single payload at
//                  (common_header.ucode_array_offset_bytes, ucode_size_bytes)
//   0x200..0x2FF   multi-payload .bin files (per-file extractors below)
//                  the dispatcher emits N LOAD_IP_FW frames per host call
//
// 0x200+ host fw_type IDs (file-typed):

uint32_t amdgpu_ucode_extract(uint64_t hostFwType,
                              const uint8_t *bin, uint64_t size_bytes,
                              UcodePayload out[kMaxUcodePayloadsPerFile]) {
    if (bin == nullptr || size_bytes == 0) return 0;
    memset(out, 0, sizeof(UcodePayload) * kMaxUcodePayloadsPerFile);

    // ---- Multi-payload per-file extractors --------------------------
    switch (hostFwType) {
    case kHostFwFile_SDMA:    return extract_sdma(bin, size_bytes, out);
    case kHostFwFile_RLC:     return extract_rlc(bin, size_bytes, out);
    case kHostFwFile_IMU:     return extract_imu(bin, size_bytes, out);
    case kHostFwFile_MES_UNI: return extract_mes(bin, size_bytes, out);
    case kHostFwFile_CP_PFP:
        return extract_cp_rs64(bin, size_bytes, RS64Family::PFP, out);
    case kHostFwFile_CP_ME:
        return extract_cp_rs64(bin, size_bytes, RS64Family::ME, out);
    case kHostFwFile_CP_MEC:
        return extract_cp_rs64(bin, size_bytes, RS64Family::MEC, out);
    default:
        break;
    }

    // ---- Legacy 0x100+psp_fw_type single-payload path ---------------
    //
    // Special-cased so callers that already work (SMU, plus anyone who
    // wants a coarse one-shot) don't have to migrate. Specific psp
    // fw_types that are part of a multi-payload .bin (e.g. RS64_PFP,
    // CP_MES, RLC_G) are also acceptable here when the host explicitly
    // routes to a single sub-bin — but typically you want the 0x200+
    // file-typed path instead.
    if (hostFwType >= 0x100 && hostFwType < 0x200) {
        uint32_t psp_fw_type = static_cast<uint32_t>(hostFwType - 0x100);

        // SDMA via 0x100+71 should ideally use the v3.0 extractor too —
        // emit through extract_sdma so the offset/size matches a v3.0
        // .bin's `header.ucode_array_offset_bytes` + `ucode_size_bytes`,
        // not the common-header default. This keeps the existing host
        // path (kFwIP_SDMA_TH0 = 0x100+71) working with correct fields.
        if (psp_fw_type == PSPGfxFwType::SDMA_UCODE_TH0) {
            return extract_sdma(bin, size_bytes, out);
        }

        // CP_MES / CP_MES_DATA via legacy 0x100+33/34: the host has loaded
        // a full uni_mes.bin file and is asking for only one half. Emit
        // through extract_mes and filter, same as IMU below. This is
        // required because the mes_firmware_header_v1_0 fields
        // (mes_ucode_offset_bytes etc.) differ from the common header's
        // ucode_array_offset_bytes — using the latter would feed PSP the
        // wrong slice of the file.
        if (psp_fw_type == PSPGfxFwType::CP_MES ||
            psp_fw_type == PSPGfxFwType::CP_MES_DATA) {
            UcodePayload all[kMaxUcodePayloadsPerFile] = {};
            uint32_t total = extract_mes(bin, size_bytes, all);
            uint32_t n = 0;
            for (uint32_t i = 0; i < total; i++) {
                if (all[i].fw_type == psp_fw_type) {
                    out[n++] = all[i];
                }
            }
            return n;
        }

        // CP RS64 family via legacy 0x100+8x: similar reasoning — the
        // RS64 ucode lives at (header.ucode_array_offset_bytes,
        // cpv2_hdr->ucode_size_bytes); stack payloads at
        // (data_offset_bytes, data_size_bytes). Common-header trim
        // would be wrong for the stack types.
        if (psp_fw_type >= PSPGfxFwType::RS64_PFP &&
            psp_fw_type <= PSPGfxFwType::RS64_MEC_P3) {
            // Determine family from the requested sub-type.
            RS64Family fam;
            if (psp_fw_type == PSPGfxFwType::RS64_PFP ||
                psp_fw_type == PSPGfxFwType::RS64_PFP_P0 ||
                psp_fw_type == PSPGfxFwType::RS64_PFP_P1) {
                fam = RS64Family::PFP;
            } else if (psp_fw_type == PSPGfxFwType::RS64_ME ||
                       psp_fw_type == PSPGfxFwType::RS64_ME_P0 ||
                       psp_fw_type == PSPGfxFwType::RS64_ME_P1) {
                fam = RS64Family::ME;
            } else {
                fam = RS64Family::MEC;
            }
            UcodePayload all[kMaxUcodePayloadsPerFile] = {};
            uint32_t total = extract_cp_rs64(bin, size_bytes, fam, all);
            uint32_t n = 0;
            for (uint32_t i = 0; i < total; i++) {
                if (all[i].fw_type == psp_fw_type) {
                    out[n++] = all[i];
                }
            }
            return n;
        }

        // Same for IMU_I / IMU_D shipped individually — fall through to
        // the multi-payload IMU extractor and let the caller pick which
        // payload to submit. But callers that hand-pick a sub-bin
        // typically pass a single fw_type, so just emit the matching
        // one. We compute both then filter.
        if (psp_fw_type == PSPGfxFwType::IMU_I ||
            psp_fw_type == PSPGfxFwType::IMU_D) {
            UcodePayload all[kMaxUcodePayloadsPerFile] = {};
            uint32_t total = extract_imu(bin, size_bytes, all);
            uint32_t n = 0;
            for (uint32_t i = 0; i < total; i++) {
                if (all[i].fw_type == psp_fw_type) {
                    out[n++] = all[i];
                }
            }
            return n;
        }

        // Default: common-header trim with the caller-supplied fw_type.
        return extract_common_header(psp_fw_type, bin, size_bytes, out);
    }

    return 0;
}

} // namespace amdgpu
