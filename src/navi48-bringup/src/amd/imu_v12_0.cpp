//
//  imu_v12_0.cpp — IMU stage handler for GFX12 (RDNA4) PSP path.
//
//  Ported from lemonade-sdk/mac-amdgpu (MIT), commit 3bdeed2,
//  dext/amdgpu/imu_v12_0.cpp (arm64 DriverKit dext) onto the
//  Navi48Bringup x86 kernel kext compatibility layer in src/amd/.
//
//  Copyright (c) the mac-amdgpu authors. MIT licence — see ../../NOTICE.
//
//  On the PSP firmware load path the IMU is driven entirely by
//  PSP + RLC. The driver never writes IMU registers; it only has
//  to confirm that PSP has been handed both IMU_I (id 68) and
//  IMU_D (id 69) firmware blobs before RLC autoload kicks off.
//
//  Audit-7 #11.
//
//  Upstream reference: imu_v12_0_setup_imu is empty on the PSP
//  load path; the GFX12 autoload chain (gfx_v12_0.c:3651) takes
//  the AMDGPU_FW_LOAD_RLC_BACKDOOR_AUTO branch on Navi 48 which
//  bypasses all imu_v12_0_*_microcode helpers.
//
//  ================= Deviations from reference =========================
//
//  1. Platform shim only: os_log -> IOLog via IMU_LOG (amdgpu_log.h).
//     No DriverKit includes, no floating point, no allocation.
//
//  2. NONE of substance. In particular: the IMU boot gate here reads
//     and polls NO registers and has NO timeout, because the reference
//     has none — its gate is the software flag imu.microcode_loaded,
//     set by the firmware dispatcher after PSP acks LOAD_IP_FW for
//     IMU_I (68) and IMU_D (69). Grep of the whole reference tree for
//     GFX_IMU_*, IMU_STATUS and BOOT_STATUS returns nothing; the only
//     register-level "did autoload work" check in the reference is
//     RLC's, which polls regCP_STAT == 0 and
//     regRLC_RLCS_BOOTLOAD_STATUS bit 31 (rlc_v12_0.cpp, owned by the
//     RLC module — NOT duplicated here). Adding speculative GFX_IMU
//     MMIO on a card whose display is live, whose IMU is owned by PSP
//     and whose GFX_IMU register offsets we cannot verify against
//     gc_12_0_0_offset.h would be inventing hardware access the
//     reference never performs.
//
//  3. Added a log of the gate's inputs on the success path so the
//     stage is visible in the hardware log (PORTING.md rule 6).
//  =====================================================================
//

#include "amdgpu_imu.h"
#include "amdgpu_log.h"

namespace amdgpu {

kern_return_t
imu_init_full(const DeviceContext &dev, IMUContext &imu)
{
    (void)dev;

    if (!imu.microcode_loaded) {
        // The firmware extractor / PSP loader must call psp_load_ip_fw
        // twice (IMU_I, IMU_D) and then set imu.microcode_loaded = true.
        // If it hasn't run, IMUInit returns NotReady so the orchestrator
        // can retry once the firmware list is loaded.
        IMU_LOG("init_full: IMU microcode not yet handed to PSP "
                "(IMU_I/IMU_D LOAD_IP_FW not acked) - deferring");
        return kIOReturnNotReady;
    }

    imu.inited = true;

    // On PSP path imu_v12_0_setup_imu is a no-op — RLC autoload
    // takes care of bringing IMU online. We just log version info
    // (if the firmware extractor stashed it) and return.
    IMU_LOG("init_full: IMU microcode handed to PSP (version=%#x). "
            "RLC autoload will bring IMU online - no driver MMIO needed. "
            "IMU liveness is observed by RLCInit's CP_STAT / "
            "RLC_RLCS_BOOTLOAD_STATUS poll, not here.",
            imu.imu_fw_version);
    return kIOReturnSuccess;
}

} // namespace amdgpu
