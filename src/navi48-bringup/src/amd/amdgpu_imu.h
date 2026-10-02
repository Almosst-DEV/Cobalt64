// From lemonade-sdk/mac-amdgpu (MIT) dext/amdgpu/amdgpu_imu.h @ 3bdeed2.
// Content is UNCHANGED from the reference (only this banner and the note
// below were added) — it is platform-independent; keep in sync.
//
// NOTE ON THE BOOT GATE: there is deliberately no register poll and no
// timeout in this subsystem. The reference tree contains no GFX_IMU /
// IMU_STATUS / BOOT_STATUS access anywhere; the IMU gate is the software
// flag IMUContext::microcode_loaded. The register-level check that the
// autoload chain (and therefore the IMU) came up lives in the RLC module
// (regCP_STAT == 0 and regRLC_RLCS_BOOTLOAD_STATUS bit 31), not here.
//
//  amdgpu_imu.h — Image Management Unit (IMU) subsystem for GFX12.
//
//  On RDNA4 / GFX12 with the PSP firmware load path, the driver
//  does NO direct MMIO to the IMU. The bring-up sequence is:
//
//      1. PSP loads IMU_I + IMU_D into TMR via LOAD_IP_FW (id 68/69).
//      2. PSP triggers the RLC autoload chain.
//      3. The RLC firmware, once running, brings the IMU online and
//         then chains PFP/ME/MEC/MES.
//
//  Upstream confirms this: gfx_v12_0_hw_init (gfx_v12_0.c:3651)
//  takes the AMDGPU_FW_LOAD_RLC_BACKDOOR_AUTO branch on R9700 and
//  only calls imu_v12_0_setup_imu when the firmware load type is
//  AMDGPU_FW_LOAD_DIRECT (not PSP). On the PSP path the IMU stage
//  is essentially empty — its only obligation is that IMU_I and
//  IMU_D have been LOAD_IP_FW'd into the PSP TMR before RLC
//  autoload starts.
//
//  This header / stub-cpp pair exists so the bringup orchestrator
//  has a real `IMUInit` stage handler that:
//      - validates imu.microcode_loaded
//      - logs the IMU firmware version (if known)
//      - returns success
//
//  Source: drivers/gpu/drm/amd/amdgpu/imu_v12_0.c
//

#pragma once

#include <stdint.h>
#include "amdgpu_ip.h"
#include "amdgpu_regs.h"  // brings in DeviceContext + kern_return_t

namespace amdgpu {

struct PSPContext;  // forward — we may use it to look up the IMU
                    // firmware version once the firmware extractor
                    // wires that up.

//
// IMU subsystem state. The PSP path doesn't program any IMU MMIO
// registers from the driver, so this struct only tracks whether
// firmware was handed off correctly.
//
struct IMUContext {
    bool      inited;
    bool      microcode_loaded;   // Set TRUE by the firmware
                                  // dispatcher AFTER PSP has acked
                                  // both IMU_I (68) and IMU_D (69)
                                  // LOAD_IP_FW frames.
    uint32_t  imu_fw_version;     // Cached from the IMU firmware
                                  // header by the firmware
                                  // extractor.
};

//
// IMUInit stage handler. Confirms IMU microcode has been pushed
// to PSP and (on the PSP path) returns success. No MMIO writes.
// Mirrors the no-op tail of imu_v12_0_setup_imu (imu_v12_0.c) on
// the PSP/RLC-autoload firmware path.
//
// Audit-7 #11.
//
kern_return_t imu_init_full(const DeviceContext &dev, IMUContext &imu);

} // namespace amdgpu
