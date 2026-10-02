// PORTED from lemonade-sdk/mac-amdgpu (MIT) dext/amdgpu/smu_v14_0.cpp @ 3bdeed2.
//
//  smu_v14_0.cpp — SMU v14_0_3 (PMFW) mailbox primitives.
//
//  Reference provenance, kept verbatim from the file we ported:
//
//      Source: upstream/linux/drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c
//          smu_cmn_wait_for_response          (line ~125)
//          smu_cmn_send_smc_msg_with_param    (line ~162)
//          smu_cmn_send_smc_msg               (line ~186)
//
//      Note: real Linux path runs through a msg_ctl abstraction
//      (smu_msg_v1_send_msg / smu_msg_v1_wait_response) so different
//      SMU generations can share code. We collapse to direct register
//      pokes here because we only target SMU v14_0_3.
//
//  mac-amdgpu is MIT-licensed (Copyright (c) lemonade-sdk contributors);
//  see NOTICE at the root of this kext. Register semantics cross-checked
//  against Linux amdgpu (GPL-2.0) used as documentation only.
//
//  HARDWARE (RX 9070 XT, 1002:7550, SMU 14.0.3): the MP1 mailbox lives at
//  MP1 BASE_IDX **1** — msg regMP1_SMN_C2PMSG_66 (dword 0x0082), param
//  C2PMSG_82 (0x0092), resp C2PMSG_90 (0x009A); responses 1 OK, 0xFF
//  failed, 0xFE unknown, 0xFD prereq, 0xFC busy. These match the
//  reference and our own regs.hpp/surveySMU() constants exactly — no
//  divergence was found, so nothing here had to be re-derived.
//
// ---------------------------------------------------------------------
//  Deviations from reference
// ---------------------------------------------------------------------
//  1. Platform: kernel kext, not a DriverKit dext. os_log -> IOLog via
//     SMU_LOG (amdgpu_log.h); no <os/log.h> / <DriverKit/IOLib.h>; no
//     "%{public}s"; "%#x" spelled "0x%x" because the kernel's _doprnt is
//     the only printf we get. No dynamic allocation of any kind here.
//  2. smu_smc_hw_setup() takes `uint64_t driver_table_mc` instead of
//     `PSPContext &psp`. The reference bump-allocates the 64 KB driver
//     table out of psp.fwBuf; Navi48Bringup has no PSPContext yet (that
//     module is in flight under another owner) and DeviceContext carries
//     no VRAMBumpAllocator, so the default is a FIXED VRAM slot at
//     dev.vramBase + 0x0E00000 (64 KB) — in the free gap below fw_buf (+16 MiB) in the
//     PORTING.md layout, colliding with nothing fixed, but overlapping
//     the first 64 KB of the future bump-allocator arena. Callers that
//     own an allocator pass their own MC address instead.
//     MC address = dev.vramMC(dev.vramBase + 0x0E00000), i.e. the same
//     "VRAM through GMC" path the reference used.
//  3. The fixed slot is zeroed (bar0_memset_vram) before its address is
//     given to PMFW. The reference does not: its slot came from a bump
//     allocator over memory the PSP module had already staged. Ours is
//     raw post-UEFI VRAM, so we make the handoff deterministic. This is
//     a VRAM write at +32 MiB — far above the console framebuffer at
//     [0, 8 MiB) — and is the only VRAM traffic in this file.
//  4. smu_send_msg_with_param() bounds-checks the three mailbox dwords
//     against dev.rmmioSize before poking them. Required here: our
//     RREG32() returns 0xFFFFFFFF for an out-of-range dword, which the
//     "wait for non-zero response" loop would happily mistake for a real
//     (bogus) SMU response. The reference's accessors can't do that.
//  5. Every message logs id + name + param on the way out and response +
//     return value on the way back (project rule 6: on hardware the log
//     is all we get). The reference only logs failures. No extra MMIO.
//  6. Added, with no change to any reference function's name or
//     signature: smu_get_driver_if_version() (the reference inlines that
//     message inside smc_hw_setup), smu_hw_init() (the body of
//     `case BringupStage::SMUInit` from the reference's
//     amdgpu_init.cpp:785, which is not ported yet), and
//     smu_disable_all_features() / smu_set_soft_freq_range() /
//     smu_set_power_state() (ported from the reference's user-client,
//     dext/MacAMDGPU.cpp selectors 33 and 40 — this kext has no user
//     client, so only the PMFW-facing halves come across; message ids,
//     param encoding, order and the MHz table are unchanged).
//  7. Nothing else was left unported. smu_v14_0.cpp has no table
//     staging beyond the driver table, no IODMACommand use, and no
//     sysmem buffers, so amdgpu::SysMem is not needed by this module.
//

#include "amdgpu_smu.h"
#include "amdgpu_log.h"

#include <kern/thread.h>          // current_thread(): the owner of the shared mailbox lock (0.0.604)
#include <libkern/OSAtomic.h>

namespace amdgpu {

// ---------------------------------------------------------------------------------------------------------------------
//  0.0.604 (notes/design/NATIVE-S2-DISPCLK.md C6): ONE lock for the PPSMC mailbox and the DAL mailbox (smu_dal.cpp), so a
//  metrics read, a power-state change, the boot-time init and a DAL experiment step can never interleave. It is RECURSIVE
//  for its owning thread, so a caller that needs several messages back to back (the user client's doMetrics / doPowerState)
//  holds it across the whole sequence while each message still takes it itself. If IOLockAlloc fails smu_lock_enter()
//  returns false and the PPSMC path runs unlocked exactly as before 0.0.604; the DAL sender refuses instead.
// ---------------------------------------------------------------------------------------------------------------------
static IOLock *gSmuLock;
static thread_t gSmuOwner;
static uint32_t gSmuDepth;

bool smu_lock_enter()
{
    if (gSmuLock == nullptr) {
        IOLock *l = IOLockAlloc();
        if (l != nullptr && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gSmuLock)) IOLockFree(l);
    }
    IOLock *l = gSmuLock;
    if (l == nullptr) return false;
    const thread_t me = current_thread();
    if (__atomic_load_n(&gSmuOwner, __ATOMIC_ACQUIRE) == me) { gSmuDepth++; return true; }   // only this thread ever stores `me`
    IOLockLock(l);
    __atomic_store_n(&gSmuOwner, me, __ATOMIC_RELEASE);
    gSmuDepth = 1;
    return true;
}

void smu_lock_exit()
{
    if (gSmuDepth == 0 || gSmuLock == nullptr) return;
    if (--gSmuDepth == 0) {
        __atomic_store_n(&gSmuOwner, (thread_t)nullptr, __ATOMIC_RELEASE);
        IOLockUnlock(gSmuLock);
    }
}

// Message-id -> name, for the log. Diagnostic only; no MMIO.
static const char *smu_msg_name(uint32_t msgId)
{
    switch (msgId) {
    case PPSMC::TestMessage:                return "TestMessage";
    case PPSMC::GetSmuVersion:              return "GetSmuVersion";
    case PPSMC::GetDriverIfVersion:         return "GetDriverIfVersion";
    case PPSMC::SetAllowedFeaturesMaskLow:  return "SetAllowedFeaturesMaskLow";
    case PPSMC::SetAllowedFeaturesMaskHigh: return "SetAllowedFeaturesMaskHigh";
    case PPSMC::EnableAllSmuFeatures:       return "EnableAllSmuFeatures";
    case PPSMC::DisableAllSmuFeatures:      return "DisableAllSmuFeatures";
    case PPSMC::GetRunningSmuFeaturesLow:   return "GetRunningSmuFeaturesLow";
    case PPSMC::GetRunningSmuFeaturesHigh:  return "GetRunningSmuFeaturesHigh";
    case PPSMC::SetDriverDramAddrHigh:      return "SetDriverDramAddrHigh";
    case PPSMC::SetDriverDramAddrLow:       return "SetDriverDramAddrLow";
    case PPSMC::SetToolsDramAddrHigh:       return "SetToolsDramAddrHigh";
    case PPSMC::SetToolsDramAddrLow:        return "SetToolsDramAddrLow";
    case PPSMC::TransferTableSmu2Dram:      return "TransferTableSmu2Dram";
    case PPSMC::TransferTableDram2Smu:      return "TransferTableDram2Smu";
    case PPSMC::UseDefaultPPTable:          return "UseDefaultPPTable";
    case PPSMC::SetSoftMinByFreq:           return "SetSoftMinByFreq";
    case PPSMC::SetSoftMaxByFreq:           return "SetSoftMaxByFreq";
    case PPSMC::NotifyPowerSource:          return "NotifyPowerSource";
    case PPSMC::RunDcBtc:                   return "RunDcBtc";
    default:                                return "?";
    }
}

// Response-code -> name, for the log. SMUResp lives in amdgpu_ip.h.
static const char *smu_resp_name(uint32_t resp)
{
    switch (resp) {
    case SMUResp::OK:                return "OK";
    case SMUResp::Failed:            return "Failed";
    case SMUResp::UnknownCmd:        return "UnknownCmd";
    case SMUResp::CmdRejectedPrereq: return "CmdRejectedPrereq";
    case SMUResp::CmdRejectedBusy:   return "CmdRejectedBusy";
    default:                         return "?";
    }
}

//
// smu_wait_for_response — port of smu_cmn_wait_for_response.
// Polls C2PMSG_90 until non-zero or timeout. Returns the latched
// response value via *outResp (zero on timeout).
//
static bool
smu_wait_for_response(const DeviceContext &dev, uint32_t *outResp)
{
    if (!dev.ip.isResolved(IPBlock::MP1, /*baseIdx=*/1)) {
        if (outResp) *outResp = 0;
        return false;
    }
    const uint32_t reg = SOC15_REG_OFFSET_BIDX(dev, IPBlock::MP1, 1,
                                               MP1Regs::C2PMSG_90);
    // Upstream uses `adev->usec_timeout * 20 = 100 ms * 20 = 2 s` here.
    const uint64_t kBudgetUs = 2 * 1000000;
    uint32_t v = 0;
    bool ok = poll_reg(dev, reg, 0xFFFFFFFFu, 0u, /*invert below*/
                       0, &v);
    (void)ok;
    // poll_reg's mask/expected semantics don't fit "wait for non-zero"
    // — do it explicitly instead.
    // (The vestigial poll_reg call above is retained from the reference
    //  so the MMIO trace matches it read-for-read; with timeout 0 it is
    //  one side-effect-free read of the response register.)
    uint32_t cur = 0;
    uint64_t elapsed = 0;
    const uint64_t kStep = 1000;  // 1 ms
    while (elapsed < kBudgetUs) {
        cur = RREG32(dev, reg);
        if (cur != 0) {
            if (outResp) *outResp = cur;
            return true;
        }
        IOSleep(1);
        elapsed += kStep;
    }
    if (outResp) *outResp = 0;
    return false;
}

// The PPSMC message body: the pre-0.0.604 smu_send_msg_with_param text, unchanged (tests/native_s2_dal_test.cpp pins the wrapper/body split in the
// source; there is no automated disassembly test - the 0.0.604 builder compared the object code by hand against a pristine build of the base and found
// it identical apart from alignment padding and its jump table); the public entry below only adds the shared lock around it.
static __attribute__((noinline)) kern_return_t
smu_send_msg_with_param_body(const DeviceContext &dev,
                             uint32_t msgId, uint32_t param,
                             uint32_t *outReturn)
{
    // SMU mailbox registers `regMP1_SMN_C2PMSG_*` declare BASE_IDX 1 in
    // upstream mp_14_0_2_offset.h — NOT BASE_IDX 0. Using base[0] (the
    // historical default) routes the writes to a completely different
    // physical register and SMU never responds. Confirmed against
    // upstream `smu_v14_0_send_msg_with_param`.
    if (!dev.ip.isResolved(IPBlock::MP1, /*baseIdx=*/1)) {
        SMU_LOG("MP1 BASE_IDX 1 not resolved — SMU mailbox unreachable");
        return kIOReturnNotReady;
    }

    const uint32_t regMsg   = SOC15_REG_OFFSET_BIDX(dev, IPBlock::MP1, 1,
                                                    MP1Regs::C2PMSG_66);
    const uint32_t regParam = SOC15_REG_OFFSET_BIDX(dev, IPBlock::MP1, 1,
                                                    MP1Regs::C2PMSG_82);
    const uint32_t regResp  = SOC15_REG_OFFSET_BIDX(dev, IPBlock::MP1, 1,
                                                    MP1Regs::C2PMSG_90);

    // [DEVIATION 4] Our RREG32 answers 0xFFFFFFFF for a dword outside
    // the BAR5 mapping and WREG32 silently drops the write — the wait
    // loop below would then read a non-zero "response" that the SMU
    // never wrote. Refuse up front instead.
    uint32_t highDword = regMsg;
    if (regParam > highDword) highDword = regParam;
    if (regResp  > highDword) highDword = regResp;
    if (!dev.rmmio || ((size_t)highDword * 4 + 4) > dev.rmmioSize) {
        SMU_LOG("mailbox dwords out of BAR5 range (msg 0x%x param 0x%x resp 0x%x, rmmio %llu bytes)",
                regMsg, regParam, regResp, (unsigned long long)dev.rmmioSize);
        return kIOReturnNoDevice;
    }

    SMU_LOG("send msg=0x%02x (%s) param=0x%08x [regs msg=0x%05x param=0x%05x resp=0x%05x]",
            msgId, smu_msg_name(msgId), param, regMsg, regParam, regResp);

    // 1. Clear any stale response.
    WREG32(dev, regResp, 0);
    // 2. Stage the parameter.
    WREG32(dev, regParam, param);
    // 3. Kick.
    WREG32(dev, regMsg, msgId);

    // 4. Wait for SMU to write a non-zero response.
    uint32_t resp = 0;
    if (!smu_wait_for_response(dev, &resp)) {
        SMU_LOG("msg=0x%x param=0x%x timeout (no response)", msgId, param);
        return kIOReturnTimeout;
    }

    // 5. Read return value if caller asked. (Only then: the param
    //    register read is part of the reference's MMIO trace exactly
    //    when outReturn != nullptr, so the log below branches rather
    //    than issuing a read of its own.)
    if (outReturn != nullptr) {
        *outReturn = RREG32(dev, regParam);
        SMU_LOG("recv msg=0x%02x (%s) resp=0x%02x (%s) ret=0x%08x",
                msgId, smu_msg_name(msgId), resp, smu_resp_name(resp),
                *outReturn);
    } else {
        SMU_LOG("recv msg=0x%02x (%s) resp=0x%02x (%s)",
                msgId, smu_msg_name(msgId), resp, smu_resp_name(resp));
    }

    if (resp != SMUResp::OK) {
        SMU_LOG("msg=0x%x param=0x%x resp=0x%x (not OK)", msgId, param, resp);
        // Translate to a useful errno-ish thing.
        switch (resp) {
        case SMUResp::Failed:           return kIOReturnError;
        case SMUResp::UnknownCmd:       return kIOReturnUnsupported;
        case SMUResp::CmdRejectedPrereq:return kIOReturnNotReady;
        case SMUResp::CmdRejectedBusy:  return kIOReturnBusy;
        default:                        return kIOReturnInternalError;
        }
    }
    return kIOReturnSuccess;
}

kern_return_t
smu_send_msg_with_param(const DeviceContext &dev,
                        uint32_t msgId, uint32_t param,
                        uint32_t *outReturn)
{
    SmuSeq seq;   // the shared mailbox lock (held = false only when IOLockAlloc failed: then the message goes out unlocked, as before 0.0.604)
    return smu_send_msg_with_param_body(dev, msgId, param, outReturn);
}

kern_return_t
smu_send_msg(const DeviceContext &dev, uint32_t msgId)
{
    return smu_send_msg_with_param(dev, msgId, 0, nullptr);
}

kern_return_t
smu_test_message(const DeviceContext &dev, uint32_t *outEcho)
{
    // SMU echoes back param+1.
    const uint32_t param = 0xABCD0001u;
    uint32_t echoed = 0;
    kern_return_t ret = smu_send_msg_with_param(dev, PPSMC::TestMessage,
                                                param, &echoed);
    if (outEcho) *outEcho = echoed;
    if (ret != kIOReturnSuccess) return ret;
    if (echoed != param + 1) {
        SMU_LOG("test_message: bad echo (sent 0x%x got 0x%x)",
                param, echoed);
        return kIOReturnInternalError;
    }
    SMU_LOG("test_message: ok (echo 0x%x)", echoed);
    return kIOReturnSuccess;
}

//
// SMU table-transfer wrappers — see amdgpu_smu.h for shape.
// Linux equivalents live in smu_v14_0.c (smu_v14_0_set_driver_table_location,
// smu_v14_0_set_tool_table_location, smu_v14_0_transfer_table_*).
//
kern_return_t
smu_set_driver_dram_addr(const DeviceContext &dev, uint64_t bus_addr)
{
    kern_return_t r = smu_send_msg_with_param(
        dev, PPSMCTable::SetDriverDramAddrHigh,
        static_cast<uint32_t>(bus_addr >> 32), nullptr);
    if (r != kIOReturnSuccess) return r;
    return smu_send_msg_with_param(
        dev, PPSMCTable::SetDriverDramAddrLow,
        static_cast<uint32_t>(bus_addr & 0xFFFFFFFFu), nullptr);
}

kern_return_t
smu_set_tools_dram_addr(const DeviceContext &dev, uint64_t bus_addr)
{
    kern_return_t r = smu_send_msg_with_param(
        dev, PPSMCTable::SetToolsDramAddrHigh,
        static_cast<uint32_t>(bus_addr >> 32), nullptr);
    if (r != kIOReturnSuccess) return r;
    return smu_send_msg_with_param(
        dev, PPSMCTable::SetToolsDramAddrLow,
        static_cast<uint32_t>(bus_addr & 0xFFFFFFFFu), nullptr);
}

kern_return_t
smu_transfer_table_dram_to_smu(const DeviceContext &dev, uint32_t table_id)
{
    return smu_send_msg_with_param(dev,
                                   PPSMCTable::TransferTableDram2Smu,
                                   table_id, nullptr);
}

kern_return_t
smu_transfer_table_smu_to_dram(const DeviceContext &dev, uint32_t table_id)
{
    return smu_send_msg_with_param(dev,
                                   PPSMCTable::TransferTableSmu2Dram,
                                   table_id, nullptr);
}

kern_return_t
smu_get_version(const DeviceContext &dev, uint32_t *outVer)
{
    uint32_t v = 0;
    kern_return_t ret = smu_send_msg_with_param(dev, PPSMC::GetSmuVersion,
                                                0, &v);
    if (outVer) *outVer = v;
    if (ret == kIOReturnSuccess) {
        SMU_LOG("smu_version: %u.%u.%u.%u",
                (v >> 24) & 0xFF, (v >> 16) & 0xFF,
                (v >>  8) & 0xFF,  v        & 0xFF);
    }
    return ret;
}

// [ADDED] The reference sends this message inline in smc_hw_setup;
// broken out so a read-only probe can ask for it too. Same message,
// same param, same handling (logged, never fatal to the caller unless
// the mailbox itself failed).
kern_return_t
smu_get_driver_if_version(const DeviceContext &dev, uint32_t *outVer)
{
    uint32_t v = 0;
    kern_return_t ret = smu_send_msg_with_param(dev, PPSMC::GetDriverIfVersion,
                                                0, &v);
    if (outVer) *outVer = v;
    if (ret == kIOReturnSuccess) {
        SMU_LOG("driver_if_version: 0x%x", v);
    }
    return ret;
}

//============================================================
// Driver-table VRAM slot. [DEVIATION 2/3] — see the file header
// and amdgpu_smu.h. Fixed offset because DeviceContext has no
// VRAMBumpAllocator and PSPContext does not exist in this kext yet.
//============================================================

uint64_t
smu_driver_table_mc(const DeviceContext &dev)
{
    // Refuse rather than guess when the context has no VRAM geometry —
    // a caller in that position must pass its own MC address.
    if (dev.vramMcBase == 0 || dev.vramLimit == 0) return 0;
    const uint64_t off = dev.vramBase + kSMUDriverTableVRAMOffset;
    if (off + kSMUDriverTableSize > dev.vramLimit) return 0;
    return dev.vramMC(off);
}

//============================================================
// smu_smc_hw_setup — minimal port of upstream smu_smc_hw_setup
// (amdgpu_smu.c:1662). v0.1.20 hypothesis: PMFW must enable DPM
// features for the IMU autoload state machine to fire after
// AUTOLOAD_RLC. PSP-side LOAD_IP_FW for SMU brings PMFW up; this
// function then completes the SMU<->driver handshake.
//
// Sequence (matches upstream order, minimal subset):
//   1. GetDriverIfVersion         — sanity-check IF version.
//   2. SetDriverDramAddrHigh+Low  — point SMU at a 64 KB driver_table
//                                    region (VRAM, reached via GMC).
//   3. RunDcBtc                   — boot-time calibration.
//   4. SetAllowedFeaturesMaskLow  — 0xFFFFFFFF
//      SetAllowedFeaturesMaskHigh — 0xFFFFFFFF
//   5. EnableAllSmuFeatures       — master DPM enable.
//   6. GetRunningSmuFeaturesLow+High — log what came up.
//
// Driver-table allocation: the reference bumps 64 KB out of psp.fwBuf
// (VRAM slot allocator also used by ASD + LOAD_IP_FW per-payload
// staging). Here it is the fixed VRAM slot at dev.vramBase +
// kSMUDriverTableVRAMOffset unless the caller supplies its own MC
// address. SMU is on-die and reads via the same GMC PSP uses — the
// VRAM MC address resolves either way.
//============================================================

kern_return_t
smu_smc_hw_setup(DeviceContext &dev, uint64_t driver_table_mc)
{
    constexpr uint64_t kDriverTableSize  = kSMUDriverTableSize;  // 64 KB
    constexpr uint64_t kFwBufAlign       = 0x1000;               // PAGE_SIZE

    // 1. Sanity-check the SMU driver IF version. Upstream logs but does
    //    NOT abort on version mismatch on most chips — we mirror that.
    {
        uint32_t if_ver = 0;
        kern_return_t r = smu_get_driver_if_version(dev, &if_ver);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: GetDriverIfVersion FAILED kr=0x%x", (unsigned)r);
            return r;
        }
        SMU_LOG("smc_hw_setup: SMC IF version = 0x%x", if_ver);
    }

    // 2. Place the driver_table. SMU stores tool/metric/dpm tables here
    //    when we request transfers. Upstream uses amdgpu_bo_create_kernel;
    //    the reference reused the PSP fwBuf bump allocator. We use the
    //    fixed slot documented in amdgpu_smu.h (and zero it first) unless
    //    the caller passed an address it owns.
    uint64_t slot_sz = (kDriverTableSize + kFwBufAlign - 1) & ~(kFwBufAlign - 1);
    if (driver_table_mc == 0) {
        driver_table_mc = smu_driver_table_mc(dev);
        if (driver_table_mc == 0) {
            SMU_LOG("smc_hw_setup: no VRAM slot for driver_table "
                    "(mcBase=0x%llx base=0x%llx limit=0x%llx)",
                    dev.vramMcBase, dev.vramBase, dev.vramLimit);
            return kIOReturnNoSpace;
        }
        // [DEVIATION 3] deterministic handoff — raw post-UEFI VRAM at
        // +24 MiB inside our region (absolute +32 MiB), far above the
        // console framebuffer.
        if (dev.bar0 != nullptr) {
            bar0_memset_vram(dev, dev.vramBase + kSMUDriverTableVRAMOffset,
                             0u, slot_sz);
            SMU_LOG("smc_hw_setup: driver_table slot zeroed at vram+0x%llx",
                    dev.vramBase + kSMUDriverTableVRAMOffset);
        } else {
            SMU_LOG("smc_hw_setup: BAR0 aperture not mapped — driver_table "
                    "slot at vram+0x%llx left with whatever it held",
                    dev.vramBase + kSMUDriverTableVRAMOffset);
        }
    }

    SMU_LOG("smc_hw_setup: driver_table @ mc=0x%llx size=%llu",
            driver_table_mc, (unsigned long long)slot_sz);

    // 3. Send SetDriverDramAddrHigh + Low. Upstream calls these
    //    unconditionally in smu_v14_0_set_driver_table_location (line 641).
    {
        uint32_t hi = static_cast<uint32_t>(driver_table_mc >> 32);
        uint32_t lo = static_cast<uint32_t>(driver_table_mc & 0xFFFFFFFFu);
        kern_return_t r;

        r = smu_send_msg_with_param(dev, PPSMC::SetDriverDramAddrHigh, hi, nullptr);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: SetDriverDramAddrHigh(0x%x) FAILED kr=0x%x", hi, (unsigned)r);
            return r;
        }
        r = smu_send_msg_with_param(dev, PPSMC::SetDriverDramAddrLow, lo, nullptr);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: SetDriverDramAddrLow(0x%x) FAILED kr=0x%x", lo, (unsigned)r);
            return r;
        }
        SMU_LOG("smc_hw_setup: SetDriverDramAddr ok (hi=0x%x lo=0x%x)", hi, lo);
    }

    // 3.5 v0.1.29 — UseDefaultPPTable. The full pptable-from-VBIOS
    //     parser is documented as a follow-up; for now we ask PMFW to
    //     fall back to the IFWI-baked default powerplay table. This is
    //     what populates the fan curve + chip-specific DPM tables that
    //     PMFW otherwise leaves zeroed (which is why the fan defaults
    //     to MAX after EnableAllSmuFeatures).
    //
    //     Best-effort like SetAllowedFeaturesMask{Low,High}: if this
    //     PMFW build doesn't expose the message (UnknownCmd / 0xFE),
    //     log and continue. RunDcBtc still runs; the chip still boots.
    {
        kern_return_t r = smu_send_msg(dev, PPSMC::UseDefaultPPTable);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: UseDefaultPPTable non-fatal kr=0x%x — "
                    "PMFW may already have applied its IFWI default. "
                    "Full VBIOS pptable parse is deferred.", (unsigned)r);
        } else {
            SMU_LOG("smc_hw_setup: UseDefaultPPTable ok — IFWI default "
                    "pptable applied");
        }
    }

    // 4. RunDcBtc — boot-time calibration. Upstream: smu_v14_0.c:1558.
    //    No parameter, no return value parsing (resp=0 == success).
    {
        kern_return_t r = smu_send_msg(dev, PPSMC::RunDcBtc);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: RunDcBtc FAILED kr=0x%x", (unsigned)r);
            return r;
        }
        SMU_LOG("smc_hw_setup: RunDcBtc ok");
    }

    // 4.5 v0.1.29 — NotifyPowerSource(AC). Upstream amdgpu_smu.c:1662
    //     smu_smc_hw_setup calls smu_notify_display_change /
    //     smu_set_power_source after RunDcBtc with the current power
    //     source. The reference is an external GPU so it assumes AC
    //     (param=1); this card is a desktop PCIe board, which is AC
    //     too. Some PMFW builds don't expose this message — same
    //     non-fatal pattern as SetAllowedFeaturesMask{Low,High} below.
    {
        kern_return_t r = smu_send_msg_with_param(
            dev, PPSMC::NotifyPowerSource, /*AC*/ 1, nullptr);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: NotifyPowerSource(AC) non-fatal kr=0x%x", (unsigned)r);
        } else {
            SMU_LOG("smc_hw_setup: NotifyPowerSource(AC) ok");
        }
    }

    // 5. Set allowed features mask. v0.1.20 test result: PMFW returns
    //    UnknownCmd (0xFE) for both SetAllowedFeaturesMaskLow and
    //    SetAllowedFeaturesMaskHigh on this firmware build, even though
    //    upstream smu_v14_0_2_ppt.c's message map registers them.
    //
    //    Theory: the deployed SMU 14.0.3 PMFW (version 0.104.76.0) uses
    //    a baked-in default allow-mask from IFWI and doesn't expose the
    //    runtime mask-set messages. We skip these and try
    //    EnableAllSmuFeatures directly — if PMFW honors its IFWI default,
    //    DPM features still come up.
    //
    //    Best-effort: log the failure but don't abort. Re-evaluate if
    //    EnableAllSmuFeatures also fails.
    {
        kern_return_t r;
        r = smu_send_msg_with_param(dev, PPSMC::SetAllowedFeaturesMaskLow,
                                    0xFFFFFFFFu, nullptr);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: SetAllowedFeaturesMaskLow non-fatal "
                    "kr=0x%x — proceeding to EnableAllSmuFeatures with "
                    "PMFW default mask", (unsigned)r);
        }
        r = smu_send_msg_with_param(dev, PPSMC::SetAllowedFeaturesMaskHigh,
                                    0xFFFFFFFFu, nullptr);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: SetAllowedFeaturesMaskHigh non-fatal "
                    "kr=0x%x", (unsigned)r);
        }
    }

    // 6. EnableAllSmuFeatures — THE master DPM switch.
    //    Upstream calls smu_system_features_control(smu, true), which
    //    sends this message. After this, PMFW starts driving GFX/SOC
    //    clocks out of bootup-idle.
    //
    //    This is the message we MOST want to succeed. If PMFW also
    //    returns UnknownCmd here, we'd know feature control is wholly
    //    PMFW-internal on this chip and the autoload state machine must
    //    be unblocked by some other means.
    //
    //    NOTE (this port): the display is still being scanned out by
    //    firmware when this runs. This is the message that can move
    //    clocks under that scanout and pin the fan to max.
    {
        kern_return_t r = smu_send_msg(dev, PPSMC::EnableAllSmuFeatures);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: EnableAllSmuFeatures FAILED kr=0x%x — "
                    "if UnknownCmd, PMFW feature control is autoload-"
                    "internal on this chip", (unsigned)r);
            // Continue to the diagnostic readback — log what features
            // ARE running even though we couldn't toggle them.
        } else {
            SMU_LOG("smc_hw_setup: EnableAllSmuFeatures ok");
        }
    }

    // 7. Read back which features actually came online. Pure diagnostic
    //    — upstream stores into smu->smu_feature.supported_bits, we just
    //    log. Failures here are non-fatal (some old PMFW silently drops
    //    GetRunningSmuFeatures*).
    {
        uint32_t lo = 0, hi = 0;
        kern_return_t r;
        r = smu_send_msg_with_param(dev, PPSMC::GetRunningSmuFeaturesLow,
                                    0, &lo);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: GetRunningSmuFeaturesLow FAILED kr=0x%x — "
                    "(non-fatal)", (unsigned)r);
        }
        r = smu_send_msg_with_param(dev, PPSMC::GetRunningSmuFeaturesHigh,
                                    0, &hi);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: GetRunningSmuFeaturesHigh FAILED kr=0x%x — "
                    "(non-fatal)", (unsigned)r);
        }
        SMU_LOG("smc_hw_setup: running features = 0x%x_0x%x", hi, lo);
    }

    SMU_LOG("smc_hw_setup: ok");
    return kIOReturnSuccess;
}

//============================================================
// [ADDED] smu_hw_init — the SMU IP block's hw_init. Body ported from
// `case BringupStage::SMUInit` in the reference's amdgpu_init.cpp:785,
// which is not ported into this kext yet.
//
// Steps 1-3 only poke the mailbox (same traffic Navi48Bringup's
// read-only surveySMU() already does on this card). Step 5 is the
// power-state-changing part and is gated by the caller.
//============================================================

kern_return_t
smu_hw_init(DeviceContext &dev, bool runSmcHwSetup)
{
    // 1. Existing alive-check: TestMessage round-trip.
    uint32_t echo = 0;
    kern_return_t r = smu_test_message(dev, &echo);
    if (r != kIOReturnSuccess) {
        SMU_LOG("hw_init: TestMessage failed kr=0x%x — PMFW not answering "
                "(is smu_14_0_3.bin loaded through the PSP yet?)", (unsigned)r);
        return r;
    }

    uint32_t ver = 0;
    (void)smu_get_version(dev, &ver);

    // [ADDED vs the reference stage] also log the SMC interface version
    // here, so a read-only run records it without running smc_hw_setup.
    uint32_t ifver = 0;
    (void)smu_get_driver_if_version(dev, &ifver);

    dev.smuOnline = true;
    SMU_LOG("hw_init: SMU online (pmfw 0x%08x, if 0x%08x)", ver, ifver);

    if (!runSmcHwSetup) {
        SMU_LOG("hw_init: smc_hw_setup skipped by caller — no clock/power "
                "state was changed");
        return kIOReturnSuccess;
    }

    // 2. v0.1.20: SMU PMFW handshake (smu_smc_hw_setup). Mirrors
    //    upstream amdgpu_smu.c:1662. Hypothesis is that PMFW must
    //    enable DPM features for the IMU autoload state machine to
    //    fire — without this, BOOTLOAD_STATUS stays at 0 even though
    //    PSP returns resp=0 for every command (v0.1.18-0.1.19
    //    symptom). Failures here are NON-FATAL for the SMUInit
    //    stage itself (SMU is still "alive" per TestMessage); we
    //    log and continue so RLCInit's BOOTLOAD_STATUS poll surfaces
    //    the real diagnostic signal.
    kern_return_t hwr = smu_smc_hw_setup(dev, /*driver_table_mc=*/0);
    if (hwr != kIOReturnSuccess) {
        SMU_LOG("hw_init: smc_hw_setup non-fatal failure: 0x%x — "
                "proceeding anyway", (unsigned)hwr);
    }
    return kIOReturnSuccess;
}

//============================================================
// [ADDED] Feature control + GFXCLK clamps, ported from the
// reference's user-client handlers (dext/MacAMDGPU.cpp:1586 and
// :1608). Message ids, param encoding, order and the MHz table are
// unchanged; only the IOUserClient plumbing was dropped.
//============================================================

kern_return_t
smu_disable_all_features(const DeviceContext &dev)
{
    // v0.1.24 — turn DPM off via PMFW. Without a chip-specific
    // fan curve (smu_set_default_dpm_table), PMFW defaults fan to
    // MAX when DPM is enabled. Sending DisableAllSmuFeatures parks
    // it and the fan drops to idle. Reverse by re-running
    // smu_smc_hw_setup (which re-sends EnableAllSmuFeatures).
    if (!dev.smuOnline) return kIOReturnNotReady;
    kern_return_t r = smu_send_msg(dev, PPSMC::DisableAllSmuFeatures);
    SMU_LOG("disable_all_features: kr=0x%x", (unsigned)r);
    return r;
}

kern_return_t
smu_set_soft_freq_range(const DeviceContext &dev, uint32_t clk_id,
                        uint32_t min_mhz, uint32_t max_mhz)
{
    // Encoding per upstream smu_v14_0_set_soft_freq_limited_range
    // (smu_v14_0.c:1099): param = (clk_id << 16) | freq_mhz.
    if (!dev.smuOnline) return kIOReturnNotReady;
    kern_return_t firstErr = kIOReturnSuccess;

    // SetSoftMaxByFreq first (upstream order).
    if (max_mhz != 0) {
        uint32_t param = (clk_id << 16) | (max_mhz & 0xFFFFu);
        kern_return_t r = smu_send_msg_with_param(
            dev, PPSMC::SetSoftMaxByFreq, param, nullptr);
        SMU_LOG("set_soft_freq_range: SetSoftMaxByFreq(clk %u, %u MHz) kr=0x%x",
                clk_id, max_mhz, (unsigned)r);
        if (r != kIOReturnSuccess && firstErr == kIOReturnSuccess) firstErr = r;
    }
    // Then SetSoftMinByFreq. min_mhz == 0 is a meaningful value here
    // ("unclamp the lower bound"), so the caller decides by passing it;
    // smu_set_power_state() reproduces the reference's exact choices.
    {
        uint32_t param = (clk_id << 16) | (min_mhz & 0xFFFFu);
        kern_return_t r = smu_send_msg_with_param(
            dev, PPSMC::SetSoftMinByFreq, param, nullptr);
        SMU_LOG("set_soft_freq_range: SetSoftMinByFreq(clk %u, %u MHz) kr=0x%x",
                clk_id, min_mhz, (unsigned)r);
        if (r != kIOReturnSuccess && firstErr == kIOReturnSuccess) firstErr = r;
    }
    return firstErr;
}

kern_return_t
smu_set_power_state(const DeviceContext &dev, uint32_t state)
{
    // v0.1.29 — per-state GFXCLK soft-clamp via PMFW.
    //
    // Maps a coarse "power state" (auto / low / nominal / high / peak)
    // to a pair of SetSoftMin/MaxByFreq PMFW messages on PPCLK_GFXCLK
    // (clk_id=0 on v14).
    if (!dev.smuOnline) return kIOReturnNotReady;

    // Build (min_mhz, max_mhz). Zero means "skip that side".
    // 0xFFFF means "PMFW pick" (passed through unchanged into the
    // low 16 bits of the param).
    uint32_t min_mhz = 0, max_mhz = 0;
    bool sendMin = true;
    switch (state) {
    case SMUPowerState::Auto:
        min_mhz = 0;       // SetSoftMin(0)   → unclamp lower bound
        max_mhz = 0xFFFFu; // SetSoftMax(FFFF)→ PMFW pick
        break;
    case SMUPowerState::Low:
        min_mhz = 0;
        max_mhz = 200;
        break;
    case SMUPowerState::Nominal:
        // Same as auto.
        min_mhz = 0;
        max_mhz = 0xFFFFu;
        break;
    case SMUPowerState::High:
        min_mhz = 1500;
        max_mhz = 0;       // leave max alone
        break;
    case SMUPowerState::Peak:
        min_mhz = 2400;
        max_mhz = 2400;
        break;
    default:
        SMU_LOG("set_power_state: bad state=%u", state);
        return kIOReturnBadArgument;
    }
    // Reference gating: min is sent when it is non-zero, or for the two
    // states that deliberately clamp the lower bound back to zero.
    sendMin = (min_mhz != 0) ||
              (state == SMUPowerState::Auto) ||
              (state == SMUPowerState::Nominal);

    const uint32_t clk_id = PPCLK::GFXCLK;  // 0
    kern_return_t firstErr = kIOReturnSuccess;

    if (max_mhz != 0) {
        uint32_t param = (clk_id << 16) | (max_mhz & 0xFFFFu);
        kern_return_t r = smu_send_msg_with_param(
            dev, PPSMC::SetSoftMaxByFreq, param, nullptr);
        SMU_LOG("set_power_state: SetSoftMaxByFreq(GFXCLK,%u) kr=0x%x",
                max_mhz, (unsigned)r);
        if (r != kIOReturnSuccess && firstErr == kIOReturnSuccess) firstErr = r;
    }
    if (sendMin) {
        uint32_t param = (clk_id << 16) | (min_mhz & 0xFFFFu);
        kern_return_t r = smu_send_msg_with_param(
            dev, PPSMC::SetSoftMinByFreq, param, nullptr);
        SMU_LOG("set_power_state: SetSoftMinByFreq(GFXCLK,%u) kr=0x%x",
                min_mhz, (unsigned)r);
        if (r != kIOReturnSuccess && firstErr == kIOReturnSuccess) firstErr = r;
    }

    SMU_LOG("set_power_state: state=%u min=%u max=%u kr=0x%x",
            state, min_mhz, max_mhz, (unsigned)firstErr);
    return firstErr;
}


//=====================================================================
//  smu_read_metrics — TransferTableSmu2Dram(TABLE_SMU_METRICS) + copy
//=====================================================================
kern_return_t
smu_read_metrics(const DeviceContext &dev, SmuMetricsResult *out)
{
    if (out == nullptr) return kIOReturnBadArgument;
    *out = SmuMetricsResult{};

    kern_return_t r = smu_get_driver_if_version(dev, &out->if_version);
    if (r != kIOReturnSuccess) {
        SMU_LOG("read_metrics: GetDriverIfVersion failed %#x", r);
        return r;
    }
    out->layout_verified = (out->if_version == kSMUDriverIfVersion_14_0_2);

    const uint64_t table_mc = smu_driver_table_mc(dev);
    if (table_mc == 0) {
        SMU_LOG("read_metrics: no driver table slot (VRAM geometry unset)");
        return kIOReturnNotReady;
    }

    r = smu_transfer_table_smu_to_dram(dev, kSMUTableMetrics);
    if (r != kIOReturnSuccess) {
        SMU_LOG("read_metrics: TransferTableSmu2Dram(%u) failed %#x",
                kSMUTableMetrics, r);
        return r;
    }

    // The SMU DMAs into VRAM; read it back through the GPU's own view so we
    // are not trusting a stale CPU mapping of the aperture.
    const uint64_t off = dev.vramBase + kSMUDriverTableVRAMOffset;
    auto *dst = reinterpret_cast<uint32_t *>(&out->metrics);
    for (uint32_t i = 0; i < sizeof(SmuMetrics) / 4; i++)
        dst[i] = RVRAM32_via_mm(dev, off + i * 4);

    // Plausibility: a Navi 48 at idle or load should sit inside these ranges.
    // This is the only defence when the layout is unverified — it catches a
    // wholesale offset shift, though not a subtle one.
    const SmuMetrics &m = out->metrics;
    const uint32_t gfx = m.CurrClock[SMUClk::GFXCLK];
    const uint32_t mem = m.CurrClock[SMUClk::UCLK];
    const uint32_t edge = m.AvgTemperature[SMUTemp::EDGE];
    const uint32_t hot  = m.AvgTemperature[SMUTemp::HOTSPOT];
    out->values_plausible =
        gfx <= 4000 && mem <= 4000 &&
        edge <= 125 && hot <= 125 && hot >= edge &&
        m.AverageGfxActivity <= 100 && m.AverageUclkActivity <= 100;
    return kIOReturnSuccess;
}

void
smu_log_metrics(const SmuMetricsResult &res)
{
    const SmuMetrics &m = res.metrics;
    if (!res.layout_verified) {
        SMU_LOG("metrics: PMFW driver-IF version %#x, this build's SmuMetrics layout "
                "is the %#x one (upstream pins that for SMU 14.0.2 and 14.0.3). The "
                "firmware is newer than any published header, so treat the decode "
                "below as UNVERIFIED: offset-0 fields are probably right, deeper "
                "ones may have shifted. Plausibility check: %s.",
                res.if_version, kSMUDriverIfVersion_14_0_2,
                res.values_plausible ? "passed" : "FAILED");
    }
    SMU_LOG("metrics: gfx %u MHz, soc %u MHz, mem %u MHz, fclk %u MHz (counter %u)",
            m.CurrClock[SMUClk::GFXCLK], m.CurrClock[SMUClk::SOCCLK],
            m.CurrClock[SMUClk::UCLK],   m.CurrClock[SMUClk::FCLK],
            m.MetricsCounter);
    SMU_LOG("metrics: temps edge %u C, hotspot %u C, mem %u C | fan %u rpm (%u%% pwm)",
            m.AvgTemperature[SMUTemp::EDGE], m.AvgTemperature[SMUTemp::HOTSPOT],
            m.AvgTemperature[SMUTemp::MEM], m.AvgFanRpm, m.AvgFanPwm);
    SMU_LOG("metrics: socket %u W, board %u W, gfx busy %u%%, mem busy %u%%, PCIe gen %u x%u",
            m.AverageSocketPower, m.AverageTotalBoardPower,
            m.AverageGfxActivity, m.AverageUclkActivity,
            m.PcieRate, m.PcieWidth);

    // Raw dump so the layout can be re-derived by eye against a known state.
    const uint32_t *w = reinterpret_cast<const uint32_t *>(&m);
    for (uint32_t i = 0; i < 32; i += 8) {
        SMU_LOG("metrics raw[%02u..%02u]: %08x %08x %08x %08x %08x %08x %08x %08x",
                i, i + 7, w[i], w[i+1], w[i+2], w[i+3], w[i+4], w[i+5], w[i+6], w[i+7]);
    }
}

} // namespace amdgpu
