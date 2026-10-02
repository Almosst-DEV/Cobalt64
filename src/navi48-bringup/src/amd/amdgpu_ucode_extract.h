// VERBATIM-derived from lemonade-sdk/mac-amdgpu (MIT) dext/amdgpu/amdgpu_ucode_extract.h @ 3bdeed2,
// kernel-clean: takes PSPGfxFwType from our amdgpu_psp.h and publishes the host firmware-file ids
// (kHostFw*) that amdgpu_ucode_extract() dispatches on.
#pragma once
#include "amdgpu_psp.h"
#include <stdint.h>

namespace amdgpu {

// Maximum payloads from a single .bin file:
//   - rlc.bin (v2.4) can emit up to: RLC_G + GPM + SRM + SRM_CNTL +
//     IRAM + DRAM + RLC_P + RLC_V + 5 tap-delay variants = 13
//   - rs64 cp.bin emits up to: ucode + 4 stacks = 5
//   - uni_mes.bin emits 2.
// 16 leaves headroom without inflating LoadFirmware stack frames.
constexpr uint32_t kMaxUcodePayloadsPerFile = 16;

// Resolved location of a single LOAD_IP_FW payload within a .bin file.
struct UcodePayload {
    uint32_t fw_type;       // psp_gfx_fw_type (e.g. GFX_FW_TYPE_RS64_PFP = 87)
    uint32_t offset_bytes;  // byte offset within the .bin file
    uint32_t size_bytes;    // payload size
};

// Decode a single firmware .bin into one-or-more LOAD_IP_FW payloads.
//
// `hostFwType` is the public-API host fw_type passed via LoadFirmware
// (the kFwIP_* / kFwIP_FILE_* constants).
//
// `bin` points at the start of the .bin file (the common_firmware_header).
// `size_bytes` is the total file size.
//
// On success, returns the number of payloads written into `out[]`
// (1..kMaxUcodePayloadsPerFile). Returns 0 if hostFwType is unknown,
// the file is too small, or the header data is internally inconsistent.
// The caller should treat 0 as "fall through to the legacy
// common-header-only path".
//
// IMPORTANT: out[i].offset_bytes is relative to `bin`, not relative to
// the ucode_array region. The caller adds it to whatever GPU bus address
// the .bin starts at to produce fw_phy_addr.
uint32_t amdgpu_ucode_extract(uint64_t hostFwType,
                              const uint8_t *bin,
                              uint64_t size_bytes,
                              UcodePayload out[kMaxUcodePayloadsPerFile]);

// Host firmware-file ids understood by amdgpu_ucode_extract():
//   0x100 + psp_fw_type   single-payload IP firmware (e.g. SMU = 0x100 + 18)
//   kHostFwFile_*         multi-payload gc/sdma files (payload types derived from the headers)
constexpr uint64_t kHostFwIPBase       = 0x100;
constexpr uint64_t kHostFwFile_SDMA    = 0x200 + 0;  // sdma_<v>.bin (v3.0)
constexpr uint64_t kHostFwFile_RLC     = 0x200 + 1;  // gc_<v>_rlc.bin (v2.x)
constexpr uint64_t kHostFwFile_IMU     = 0x200 + 2;  // gc_<v>_imu.bin
constexpr uint64_t kHostFwFile_MES_UNI = 0x200 + 3;  // gc_<v>_uni_mes.bin
constexpr uint64_t kHostFwFile_CP_PFP  = 0x200 + 4;  // gc_<v>_pfp.bin (RS64)
constexpr uint64_t kHostFwFile_CP_ME   = 0x200 + 5;  // gc_<v>_me.bin  (RS64)
constexpr uint64_t kHostFwFile_CP_MEC  = 0x200 + 6;  // gc_<v>_mec.bin (RS64)
 // AMDGPU_PSP_GFX_FW_TYPE_DEFINED

} // namespace amdgpu
