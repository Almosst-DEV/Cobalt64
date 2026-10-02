//
//  psp_v14_0.cpp — post-SOS PSP v14 protocol (GPCOM ring, TOC/TMR,
//  LOAD_IP_FW, AUTOLOAD_RLC, REG_LIST).
//
//  Origin: lemonade-sdk/mac-amdgpu (MIT) @ commit 3bdeed2,
//          dext/amdgpu/psp_v14_0.cpp  (+ dext/amdgpu/amdgpu_psp.h)
//  which ports upstream Linux
//          drivers/gpu/drm/amd/amdgpu/{psp_v14_0.c, amdgpu_psp.c,
//                                      psp_gfx_if.h}
//
//  Copyright (c) the mac-amdgpu authors. MIT licensed — see NOTICE.
//  Permission is hereby granted, free of charge, to any person obtaining a
//  copy of this software and associated documentation files (the "Software"),
//  to deal in the Software without restriction, including without limitation
//  the rights to use, copy, modify, merge, publish, distribute, sublicense,
//  and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the above copyright notice and
//  this permission notice being included in all copies or substantial
//  portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT
//  WARRANTY OF ANY KIND.
//
//  Translations (DriverKit arm64 dext → x86 IOKit kext):
//      os_log("%{public}s")          → IOLog via PSP_LOG (plain %s)
//      IOBufferMemoryDescriptor/
//        IODMACommand               → dropped; every buffer is VRAM and is
//                                     reached with bar0_* / RBAR2_32
//      hardcoded VRAM offset k…      → dev.vramBase + k…  (PORTING.md)
//      gmc.vram_start                → dev.vramMcBase
//      mdelay(20) / IOSleep(20)      → IOSleep(20)   (unchanged)
//      poll_reg / poll_psp_response  → unchanged (amdgpu_regs.h)
//
//  ============ Deviations from the reference ============
//  D1. psp_load_sos, psp_bootloader_load_component, psp_load_sos_package and
//      psp_wait_for_bootloader are NOT ported: the bootloader chain already
//      exists and is hardware-proven in src/psp.cpp. psp_adopt_sos_alive()
//      (new) records the state psp_load_sos would have left.
//  D2. psp_setup_fw_buf_sysmem is NOT ported: it needs gmc_bind_existing()
//      and a GARTContext, neither of which exists in this kext yet. fw_buf
//      therefore stays VRAM-backed (which is the path the reference's own
//      hardware runs take anyway — its GART self-test comment records that
//      PSP rejects GART addresses on this silicon). psp_fw_buf_stage() (new)
//      is the VRAM equivalent of the staging half.
//  D3. psp_parse_ta_microcode is NOT ported (TA package, not the SOS
//      container). psp_asd_initialize IS ported in full but is a logged
//      no-op until psp.asd.parsed becomes true.
//  D4. resp.tmr_size is read at cmd_buf + 864 + 16. The reference reads it
//      at cmd_buf + 16, which is resp_buf_addr_hi (a dword we write as 0) —
//      its own resp.status (+864) and resp.uresp (+864+64) reads show the
//      864 base. We log BOTH dwords so the hardware run settles it.
//  D5. Every VRAM slot is dev.vramBase + the reference's constant, because
//      VRAM [0, 8 MiB) here is the live UEFI console framebuffer. Bounds are
//      checked against dev.vramLimit in psp_init.
//  D6. std::function → function pointer + void* ctx (FirmwareLoader), and
//      extra PSP_LOG lines were added for command inputs / fence / status
//      (on hardware the log is all we get). No register sequence, delay or
//      timeout was changed.
//

#include "amdgpu_psp.h"
#include "amdgpu_ucode_psp.h"
#include "amdgpu_log.h"

// bzero comes from the kernel's string.h via IOKit/IOLib.h (amdgpu_regs.h
// declares memcpy the same way). Every zero-fill the reference writes as
// memset(x, 0, n) is bzero(x, n) here: `_bzero`/`___bzero` are proven to
// resolve on this machine (they are in RDNA4FB's and src/psp.cpp's undefined
// symbol lists), whereas `_memset` is not used anywhere in this kext today.
extern "C" void bzero(void *, size_t);

namespace amdgpu {

// MP0 C2PMSG_58/59 — SOS firmware version / TOS component version. Not in
// the verbatim amdgpu_ip.h MP0Regs list; regMPASP_SMN_C2PMSG_n = 0x40 + n on
// this IP (checked against C2PMSG_35=0x63, _64=0x80, _81=0x91). Hardware
// confirmed on this card: 0x003a1014 / 0x003a0c43 once SOS is alive.
constexpr uint32_t kMP0_C2PMSG_58 = 0x007A;
constexpr uint32_t kMP0_C2PMSG_59 = 0x007B;

// Read the GPU's vram_start MC address. The reference reads MMHUB
// regMMMC_VM_FB_LOCATION_BASE live every time; we return the value the kext
// already resolved and validated (dev.vramMcBase) but keep the live read as
// a cross-check in the log — on this card the two agree at 0x8000000000.
static uint64_t
psp_read_vram_start(const DeviceContext &dev)
{
    uint64_t live = 0;
    uint32_t raw = 0;
    if (dev.ip.isResolved(IPBlock::MMHUB)) {
        const uint32_t mmhub_base = dev.ip.get(IPBlock::MMHUB);
        raw = RREG32(dev, mmhub_base + MMHUBRegs::MMMC_VM_FB_LOCATION_BASE);
        live = ((uint64_t)(raw & MMHUBRegs::kFBBaseMask))
               << MMHUBRegs::kFBBaseShift;
    }
    if (live != dev.vramMcBase) {
        PSP_LOG("read_vram_start: WARNING MMHUB FB_LOCATION_BASE=%#010x -> "
                "mc %#llx disagrees with dev.vramMcBase %#llx — using "
                "dev.vramMcBase (hardware-proven on this card)",
                raw, (unsigned long long)live,
                (unsigned long long)dev.vramMcBase);
    }
    return dev.vramMcBase;
}

// VRAM layout for PSP-accessed buffers. IDENTICAL to the reference's
// constants; every one of them is used as `dev.vramBase + k…` because VRAM
// [0, vramBase) here is the UEFI console framebuffer the kernel keeps
// drawing into. See PORTING.md "VRAM layout".
static constexpr uint64_t kFwPriVRAMOffset  = 0;          // [0, 1MB)
static constexpr uint64_t kRingVRAMOffset   = 0x100000;   // 1 MB
static constexpr uint64_t kCmdBufVRAMOffset = 0x104000;   // 1 MB + 16 KB
static constexpr uint64_t kFenceVRAMOffset  = 0x108000;   // 1 MB + 32 KB
static constexpr uint64_t kTMRVRAMOffset    = 0x200000;   // 2 MB (size 4 MB)
// firmware.fw_buf mirror — unique per-payload addresses for LOAD_IP_FW.
// GART PT lives at +0x700000; place fw_buf well past that. 8 MB spans all
// gfx12 ucodes + stacks.
static constexpr uint64_t kFwBufVRAMOffset  = 0x01000000;  // 16 MB
static constexpr uint64_t kFwBufSize        = 0x00800000;  // 8 MB
// One past the last byte psp_init reserves — used for the vramLimit check.
static constexpr uint64_t kPSPVRAMSpanEnd   = kFwBufVRAMOffset + kFwBufSize;

//============================================================
// psp_init
//============================================================
kern_return_t
psp_init(DeviceContext &dev, PSPContext &psp)
{
    if (psp.fwPriSize != 0) {
        return kIOReturnSuccess; // idempotent
    }
    if (!dev.ip.isResolved(IPBlock::MP0)) {
        PSP_LOG("MP0 IP base not resolved — IP discovery missing");
        return kIOReturnNotReady;
    }
    if (!dev.ip.isResolved(IPBlock::MMHUB)) {
        PSP_LOG("MMHUB IP base not resolved — IP discovery missing");
        return kIOReturnNotReady;
    }

    // VRAM-backed fw_pri: PSP DMAs via the GMC internal path using an MC
    // address. "Allocation" is just picking a VRAM offset; CPU writes go
    // through the visible BAR0 aperture (256 MB on this card).
    const uint64_t vram_start = psp_read_vram_start(dev);
    if (vram_start == 0) {
        PSP_LOG("psp_init: vram_start is 0 — MMHUB FB_LOCATION_BASE not "
                "populated / dev.vramMcBase unset");
        return kIOReturnNotReady;
    }
    if (dev.vramLimit != 0 &&
        dev.vramBase + kPSPVRAMSpanEnd > dev.vramLimit) {
        PSP_LOG("psp_init: PSP VRAM span vram+[%#llx, %#llx) overruns the "
                "bring-up region (vramLimit %#llx) — refusing",
                (unsigned long long)dev.vramBase,
                (unsigned long long)(dev.vramBase + kPSPVRAMSpanEnd),
                (unsigned long long)dev.vramLimit);
        return kIOReturnNoSpace;
    }

    psp.fwPriVRAMOffset = dev.vramBase + kFwPriVRAMOffset;
    psp.fwPriBusAddr    = dev.vramMC(psp.fwPriVRAMOffset);   // GPU MC addr
    psp.fwPriSize       = kPSPFwPriBufSize;

    psp.ringVRAMOffset  = dev.vramBase + kRingVRAMOffset;
    psp.ringBusAddr     = dev.vramMC(psp.ringVRAMOffset);
    psp.ringSize        = kPSPKMRingSize;
    psp.ringCreated     = false;

    psp.cmdVRAMOffset   = dev.vramBase + kCmdBufVRAMOffset;
    psp.cmdBusAddr      = dev.vramMC(psp.cmdVRAMOffset);
    psp.fenceVRAMOffset = dev.vramBase + kFenceVRAMOffset;
    psp.fenceBusAddr    = dev.vramMC(psp.fenceVRAMOffset);
    psp.fenceCounter    = 0;

    psp.tmrVRAMOffset   = dev.vramBase + kTMRVRAMOffset;
    psp.tmrBusAddr      = dev.vramMC(psp.tmrVRAMOffset);
    psp.tmrSize         = kPSPTMRDefaultSize;
    psp.tmrSetUp        = false;

    // firmware.fw_buf mirror: dedicated VRAM region for per-payload
    // LOAD_IP_FW staging. Each stage bumps fwBufBumpOffset by the
    // page-aligned payload size and passes fwBufBaseMC + (old bump offset)
    // as cmd.fw_phy_addr — matching upstream's firmware.fw_buf_mc + per-ucode
    // offset scheme.
    psp.fwBufVRAMOffset = dev.vramBase + kFwBufVRAMOffset;
    psp.fwBufBaseMC     = dev.vramMC(psp.fwBufVRAMOffset);
    psp.fwBufSize       = kFwBufSize;
    psp.fwBufBumpOffset = 0;

    psp.sosAlive        = false;

    PSP_LOG("init: vram_start(mc)=%#llx vramBase=%#llx vramLimit=%#llx",
            (unsigned long long)vram_start,
            (unsigned long long)dev.vramBase,
            (unsigned long long)dev.vramLimit);
    PSP_LOG("init: fw_pri vram+%#llx mc=%#llx size=%llu | ring vram+%#llx "
            "mc=%#llx size=%llu",
            (unsigned long long)psp.fwPriVRAMOffset,
            (unsigned long long)psp.fwPriBusAddr,
            (unsigned long long)psp.fwPriSize,
            (unsigned long long)psp.ringVRAMOffset,
            (unsigned long long)psp.ringBusAddr,
            (unsigned long long)psp.ringSize);
    PSP_LOG("init: cmd vram+%#llx mc=%#llx | fence vram+%#llx mc=%#llx | "
            "tmr vram+%#llx mc=%#llx size=%llu",
            (unsigned long long)psp.cmdVRAMOffset,
            (unsigned long long)psp.cmdBusAddr,
            (unsigned long long)psp.fenceVRAMOffset,
            (unsigned long long)psp.fenceBusAddr,
            (unsigned long long)psp.tmrVRAMOffset,
            (unsigned long long)psp.tmrBusAddr,
            (unsigned long long)psp.tmrSize);
    PSP_LOG("init: fw_buf vram+%#llx mc=%#llx size=%#llx (VRAM-backed; the "
            "reference's GART/sysmem fw_buf is not ported)",
            (unsigned long long)psp.fwBufVRAMOffset,
            (unsigned long long)psp.fwBufBaseMC,
            (unsigned long long)psp.fwBufSize);
    return kIOReturnSuccess;
}

//============================================================
// psp_adopt_sos_alive — NOT in the reference (see D1).
//
// src/psp.cpp already ran KDB → SPL → SYS_DRV → SOC_DRV → INTF_DRV →
// HAD_DRV → RAS_DRV → IPKEYMGR → SOS through the bootloader. Record what
// psp_load_sos would have left in PSPContext so psp_ring_create's
// `if (!psp.sosAlive)` gate passes and the version stamps are in the log.
//============================================================
kern_return_t
psp_adopt_sos_alive(DeviceContext &dev, PSPContext &psp)
{
    if (!dev.ip.isResolved(IPBlock::MP0)) {
        PSP_LOG("adopt_sos_alive: MP0 IP base unresolved");
        return kIOReturnNotReady;
    }
    const uint32_t sol = RREG32(dev,
        SOC15_REG_OFFSET(dev, IPBlock::MP0, MP0Regs::C2PMSG_81));
    const uint32_t v58 = RREG32(dev,
        SOC15_REG_OFFSET(dev, IPBlock::MP0, kMP0_C2PMSG_58));
    const uint32_t v59 = RREG32(dev,
        SOC15_REG_OFFSET(dev, IPBlock::MP0, kMP0_C2PMSG_59));
    const uint32_t v64 = RREG32(dev,
        SOC15_REG_OFFSET(dev, IPBlock::MP0, MP0Regs::C2PMSG_64));
    const uint32_t v35 = RREG32(dev,
        SOC15_REG_OFFSET(dev, IPBlock::MP0, MP0Regs::C2PMSG_35));

    if (sol == 0 || sol == 0xFFFFFFFFu) {
        PSP_LOG("adopt_sos_alive: SOS is NOT alive (C2PMSG_81=%#010x, "
                "C2PMSG_35=%#010x) — run the bootloader chain first",
                sol, v35);
        psp.sosAlive = false;
        return kIOReturnNotReady;
    }

    psp.sosAlive          = true;
    psp.sosSignOfLife     = sol;
    psp.sosFwVersion      = v58;
    psp.sosFeatureVersion = v59;
    dev.psoCAlive         = true;

    PSP_LOG("adopt_sos_alive: SOS already alive (loaded by src/psp.cpp) — "
            "C2PMSG_81=%#010x C2PMSG_58=%#010x (sos fw ver) C2PMSG_59=%#010x "
            "C2PMSG_64=%#010x C2PMSG_35=%#010x",
            sol, v58, v59, v64, v35);
    if (psp.sos.fw_version != 0 && psp.sos.fw_version != v58) {
        PSP_LOG("adopt_sos_alive: NOTE container SOS fw_version %#010x != "
                "running %#010x — the card is running a different SOS build",
                psp.sos.fw_version, v58);
    }
    // The ring-create handshake wants the response flag latched in
    // C2PMSG_64; log whether it already is (0x80c00000 on this card).
    PSP_LOG("adopt_sos_alive: C2PMSG_64 masked=%#010x (ring_create expects "
            "%#010x)", v64 & kPSPMboxRespMask, kPSPMboxRespFlag);
    return kIOReturnSuccess;
}

//============================================================
// psp_read_runtime_db — port of upstream psp_get_runtime_db_entry +
// the calls in psp_sw_init (amdgpu_psp.c:376-449, 471-503).
//
// Reads the PSP runtime data header at (vram_size - 0x100000), then walks
// the directory looking for PSP_RUNTIME_ENTRY_TYPE_BOOT_CONFIG (0x5) and
// PSP_RUNTIME_ENTRY_TYPE_PPTABLE_ERR_STATUS (0x6).
//
// NOTE: db_pos is an offset from VRAM byte 0, NOT from dev.vramBase —
// upstream's amdgpu_device_vram_access() addresses the whole card, and the
// DB sits ~1 MiB below the TOP of VRAM (far outside the 256 MiB BAR0
// aperture), which is exactly why RVRAM32_via_mm (MM_INDEX/MM_DATA) is used.
//
// For psp_v14_0_3 the runtime DB is informational — IFWI POSTs the card so
// we don't drive memory training. We read it for parity with upstream.
//============================================================
kern_return_t
psp_read_runtime_db(DeviceContext &dev, PSPContext &psp,
                    uint64_t vram_size_bytes)
{
    if (psp.runtimeDbRead) return kIOReturnSuccess;  // idempotent

    constexpr uint64_t kPSP_RUNTIME_DB_OFFSET    = 0x100000ULL;
    constexpr uint16_t kPSP_RUNTIME_DB_COOKIE_ID = 0x0ed5;
    constexpr uint32_t kMAX_ENTRY_COUNT          = 0x40;
    constexpr uint32_t kENTRY_TYPE_BOOT_CONFIG   = 0x5;
    constexpr uint32_t kENTRY_TYPE_PPTABLE_ERR   = 0x6;

    if (vram_size_bytes <= kPSP_RUNTIME_DB_OFFSET) {
        PSP_LOG("runtime_db: vram_size %llu too small",
                (unsigned long long)vram_size_bytes);
        return kIOReturnNotReady;
    }

    const uint64_t db_pos = vram_size_bytes - kPSP_RUNTIME_DB_OFFSET;

    // Header: uint16 cookie + uint16 version (4 bytes).
    uint32_t hdr_dw = RVRAM32_via_mm(dev, db_pos);
    uint16_t cookie  = static_cast<uint16_t>(hdr_dw & 0xFFFFu);
    uint16_t version = static_cast<uint16_t>(hdr_dw >> 16);

    psp.runtimeDbCookie  = cookie;
    psp.runtimeDbVersion = version;
    psp.runtimeDbRead    = true;

    if (cookie != kPSP_RUNTIME_DB_COOKIE_ID) {
        PSP_LOG("runtime_db: cookie %#x at VRAM+%#llx (expected %#x) — "
                "runtime DB absent",
                cookie, (unsigned long long)db_pos, kPSP_RUNTIME_DB_COOKIE_ID);
        return kIOReturnSuccess;  // not an error — db just doesn't exist
    }

    // Directory: uint16 entry_count, then entry_list[N] of 8-byte entries.
    // entry_count lives in low 16 of the first dword after the header.
    const uint64_t dir_pos = db_pos + 4ULL;
    uint32_t dir_first_dw = RVRAM32_via_mm(dev, dir_pos);
    uint16_t entry_count = static_cast<uint16_t>(dir_first_dw & 0xFFFFu);

    if (entry_count >= kMAX_ENTRY_COUNT) {
        PSP_LOG("runtime_db: invalid entry_count=%u (max %u)",
                entry_count, kMAX_ENTRY_COUNT);
        return kIOReturnInvalid;
    }

    PSP_LOG("runtime_db: cookie=%#x ver=%#x entry_count=%u (db @ VRAM+%#llx)",
            cookie, version, entry_count, (unsigned long long)db_pos);

    // entry_list starts 4 bytes after dir_pos (after the entry_count u16
    // + 2-byte pad — the directory header is itself dword-aligned).
    // Each entry: u32 type, u16 offset, u16 size = 8 bytes.
    const uint64_t entry_list_pos = dir_pos + 4ULL;

    for (uint16_t i = 0; i < entry_count; i++) {
        const uint64_t e_pos = entry_list_pos + (uint64_t)i * 8ULL;
        uint32_t e_type   = RVRAM32_via_mm(dev, e_pos);
        uint32_t e_offsz  = RVRAM32_via_mm(dev, e_pos + 4ULL);
        uint16_t e_offset = static_cast<uint16_t>(e_offsz & 0xFFFFu);
        uint16_t e_size   = static_cast<uint16_t>(e_offsz >> 16);

        if (e_type == kENTRY_TYPE_BOOT_CONFIG && e_size >= 4) {
            uint32_t bitmask = RVRAM32_via_mm(dev, db_pos + e_offset);
            psp.bootCfgBitmask = bitmask;
            PSP_LOG("runtime_db: BOOT_CONFIG bitmask=%#x", bitmask);
        } else if (e_type == kENTRY_TYPE_PPTABLE_ERR && e_size >= 4) {
            uint32_t status = RVRAM32_via_mm(dev, db_pos + e_offset);
            psp.scpmStatus = status;
            psp.scpmEnabled = (status != 0);
            PSP_LOG("runtime_db: PPTABLE_ERR scpm_status=%u (enabled=%d)",
                    status, psp.scpmEnabled ? 1 : 0);
        }
    }
    return kIOReturnSuccess;
}

//============================================================
// psp_release — the reference released six IOBufferMemoryDescriptor /
// IODMACommand pairs; every one of those buffers is VRAM here, so there is
// nothing to free. Reset the tracked state so a re-init starts clean.
//============================================================
void
psp_release(PSPContext &psp)
{
    psp.tmrBusAddr      = 0;
    psp.tmrVRAMOffset   = 0;
    psp.tmrSize         = 0;
    psp.tmrSetUp        = false;

    psp.ringBusAddr     = 0;
    psp.ringVRAMOffset  = 0;
    psp.ringSize        = 0;
    psp.ringCreated     = false;

    psp.cmdBusAddr      = 0;
    psp.cmdVRAMOffset   = 0;
    psp.fenceBusAddr    = 0;
    psp.fenceVRAMOffset = 0;
    psp.fenceCounter    = 0;

    psp.fwPriBusAddr    = 0;
    psp.fwPriVRAMOffset = 0;
    psp.fwPriSize       = 0;
    psp.sosAlive        = false;

    psp.fwBufVRAMOffset = 0;
    psp.fwBufBaseMC     = 0;
    psp.fwBufBumpOffset = 0;

    PSP_LOG("release: PSP state reset (VRAM slots are static — nothing to "
            "free)");
}

bool
psp_is_sos_alive(const DeviceContext &dev)
{
    if (!dev.ip.isResolved(IPBlock::MP0)) return false;
    uint32_t sol = RREG32(dev,
        SOC15_REG_OFFSET(dev, IPBlock::MP0, MP0Regs::C2PMSG_81));
    // Match upstream psp_v14_0_is_sos_alive exactly: any non-zero value
    // indicates SOS booted. PSP writes a build/version stamp here once SOS
    // init completes (0x0191ae05 on this card).
    return sol != 0;
}

//============================================================
// psp_ring_create — port of psp_v14_0_ring_create (non-SR-IOV path).
//============================================================
// ----- psp_ring_destroy: psp_v14_0_ring_stop (non-SR-IOV) -----
kern_return_t
psp_ring_destroy(DeviceContext &dev, PSPContext &psp)
{
    if (!dev.ip.isResolved(IPBlock::MP0)) return kIOReturnNotReady;
    if (!psp.ringCreated) return kIOReturnSuccess;

    const uint32_t reg64 = SOC15_REG_OFFSET(dev, IPBlock::MP0, MP0Regs::C2PMSG_64);
    PSP_LOG("ring_destroy: C2PMSG_64=%#010x -> GFX_CTRL_CMD_ID_DESTROY_RINGS",
            RREG32(dev, reg64));
    WREG32(dev, reg64, kPSPGfxCtrlDestroyRings);
    IOSleep(20);

    uint32_t v = 0;
    kern_return_t r = poll_psp_response(dev, reg64, kPSPMboxRespMask,
                                        kPSPMboxRespFlag, 5 * 1000000, &v);
    if (r != kIOReturnSuccess) {
        PSP_LOG("ring_destroy: response wait failed %#x — final C2PMSG_64=%#010x "
                "(status=%#x); the next load will have to adopt the stale ring",
                r, v, v & 0xFFFFu);
        return r;
    }
    psp.ringCreated = false;
    psp.fenceCounter = 0;
    PSP_LOG("ring_destroy: rings destroyed (response=%#010x)", v);
    return kIOReturnSuccess;
}

kern_return_t
psp_ring_create(DeviceContext &dev, PSPContext &psp)
{
    if (psp.ringCreated) {
        return kIOReturnSuccess;
    }
    if (!psp.sosAlive) {
        PSP_LOG("ring_create: SOS not alive yet");
        return kIOReturnNotReady;
    }
    if (!dev.ip.isResolved(IPBlock::MP0)) {
        return kIOReturnNotReady;
    }
    if (psp.fwPriSize == 0) {
        PSP_LOG("ring_create: psp_init not called");
        return kIOReturnNotReady;
    }

    // PSP ring/cmd/fence live in VRAM addressed via the FB aperture
    // (MC = vram_start + vram_offset) — NOT via GART. PSP's internal fetch
    // path for RB frames bypasses MMHUB/GFXHUB and dereferences the address
    // as a raw VRAM physical offset. Confirmed by upstream
    // amdgpu_bo_fb_aper_addr() at amdgpu_object.c:1493 +
    // psp_update_gpu_addresses() at amdgpu_psp.c:2475-2486.
    uint64_t vram_start = psp_read_vram_start(dev);
    if (vram_start == 0) {
        PSP_LOG("ring_create: vram_start=0 — MMHUB not ready");
        return kIOReturnNotReady;
    }

    psp.ringBusAddr = dev.vramMC(psp.ringVRAMOffset);
    psp.ringSize    = kPSPKMRingSize;
    // Zero the full ring buffer in VRAM via BAR0.
    bar0_memset_vram(dev, psp.ringVRAMOffset, 0, kPSPKMRingBufSize);

    const uint32_t reg64 = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                            MP0Regs::C2PMSG_64);
    const uint32_t reg69 = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                            MP0Regs::C2PMSG_69);
    const uint32_t reg70 = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                            MP0Regs::C2PMSG_70);
    const uint32_t reg71 = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                            MP0Regs::C2PMSG_71);

    // Diagnostic: read C2PMSG_64 + C2PMSG_81 (SOS sign-of-life) before we
    // touch anything.
    uint32_t v = 0;
    uint32_t reg81 = SOC15_REG_OFFSET(dev, IPBlock::MP0, MP0Regs::C2PMSG_81);
    uint32_t pre64 = RREG32(dev, reg64);
    uint32_t pre81 = RREG32(dev, reg81);
    PSP_LOG("ring_create entry: C2PMSG_64=%#010x C2PMSG_81=%#010x sosAlive=%d",
            pre64, pre81, (int)psp.sosAlive);

    // 1. Wait for SOS ready to accept ring creation. Always do this (Linux
    //    does it unconditionally).
    if (!poll_reg(dev, reg64, kPSPMboxRespMask, kPSPMboxRespFlag,
                  5 * 1000000, &v)) {
        PSP_LOG("ring_create: SOS not ready — C2PMSG_64=%#010x "
                "(masked=%#010x, want %#010x)",
                v, v & kPSPMboxRespMask, kPSPMboxRespFlag);
        return kIOReturnTimeout;
    }
    PSP_LOG("ring_create: SOS ready, C2PMSG_64=%#010x", v);

    // 2. Program ring address (low + high) + size, then kick.
    WREG32(dev, reg69, static_cast<uint32_t>(psp.ringBusAddr & 0xFFFFFFFFu));
    WREG32(dev, reg70, static_cast<uint32_t>(psp.ringBusAddr >> 32));
    WREG32(dev, reg71, static_cast<uint32_t>(psp.ringSize));
    WREG32(dev, reg64, kPSPRingTypeKM << 16);
    PSP_LOG("ring_create: wrote ring addr=%#llx size=%u type=%u (kick=%#x)",
            (unsigned long long)psp.ringBusAddr, (unsigned)psp.ringSize,
            kPSPRingTypeKM, kPSPRingTypeKM << 16);

    IOSleep(20);

    // 3. Wait for the response flag. poll_psp_response surfaces PSP-side
    //    error statuses (bit 31 set + non-zero low 16) as kIOReturnIOError
    //    instead of letting us spin until timeout.
    kern_return_t pr = poll_psp_response(dev, reg64,
                                         kPSPMboxRespMask, kPSPMboxRespFlag,
                                         5 * 1000000, &v);
    if (pr != kIOReturnSuccess) {
        uint32_t now81 = RREG32(dev, reg81);
        const char *why = (pr == kIOReturnIOError) ? "ERROR" : "TIMEOUT";
        PSP_LOG("ring_create: response wait %s — final C2PMSG_64=%#010x "
                "(status=%#x) C2PMSG_81=%#010x",
                why, v, v & 0xFFFFu, now81);
        return pr;
    }

    psp.ringCreated = true;
    // Fresh ring: make sure the wptr mailbox agrees with our slot 0 (a stale
    // non-zero C2PMSG_67 from an earlier session would desynchronise us).
    WREG32(dev, SOC15_REG_OFFSET(dev, IPBlock::MP0, MP0Regs::C2PMSG_67), 0);
    PSP_LOG("ring_created mc=%#llx size=%llu (response=%#010x)",
            (unsigned long long)psp.ringBusAddr,
            (unsigned long long)psp.ringSize, v);

    // VRAM-backed cmd + fence buffers (FB-aperture MC addresses).
    psp.cmdBusAddr   = dev.vramMC(psp.cmdVRAMOffset);
    psp.fenceBusAddr = dev.vramMC(psp.fenceVRAMOffset);
    psp.fenceCounter = 0;
    bar0_memset_vram(dev, psp.cmdVRAMOffset,   0, kPSPCmdBufSize);
    bar0_memset_vram(dev, psp.fenceVRAMOffset, 0, kPSPFenceBufSize);

    PSP_LOG("cmd_buf mc=%#llx (vram+%#llx) fence_buf mc=%#llx (vram+%#llx)",
            (unsigned long long)psp.cmdBusAddr,
            (unsigned long long)psp.cmdVRAMOffset,
            (unsigned long long)psp.fenceBusAddr,
            (unsigned long long)psp.fenceVRAMOffset);
    return kIOReturnSuccess;
}

//
// psp_ring_cmd_submit — direct port of upstream's algorithm
// (drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c:3449 psp_ring_cmd_submit
//  + amdgpu_psp.c:705 psp_cmd_submit_buf).
//
// Flow:
//   1. Read current wptr from C2PMSG_67.
//   2. Compute the write_frame slot in ring memory from wptr.
//   3. Copy `cmd` into the cmd buffer in VRAM.
//   4. Build a 64-byte psp_gfx_rb_frame in the ring slot pointing at
//      cmdBusAddr + fenceBusAddr with our incremented fence value.
//   5. HDP flush, then update wptr (in dwords) into C2PMSG_67.
//   6. Poll the fence dword in VRAM until it equals the fence value.
//   7. Read resp.status from cmd_buf + 864.
//
kern_return_t
psp_ring_cmd_submit(DeviceContext &dev, PSPContext &psp,
                    const void *cmd, uint32_t cmdSize,
                    uint32_t *outRespStatus)
{
    if (!psp.ringCreated) {
        return kIOReturnNotReady;
    }
    if (cmd == nullptr || cmdSize != kPSPGfxCmdRespSize) {
        return kIOReturnBadArgument;
    }
    if (!dev.ip.isResolved(IPBlock::MP0)) {
        return kIOReturnNotReady;
    }

    const uint32_t ringSizeBytes = (uint32_t)psp.ringSize;
    const uint32_t ringSizeDw    = ringSizeBytes / 4;
    const uint32_t frameSizeDw   = sizeof(PSPGfxRBFrame) / 4;
    const uint32_t regWptr = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                              MP0Regs::C2PMSG_67);

    // The command id is the third dword of the frame the caller handed us —
    // log it so every submit is attributable.
    uint32_t cmd_id = 0;
    memcpy(&cmd_id, static_cast<const uint8_t *>(cmd) + 8, 4);

    // 1. Get current wptr.
    uint32_t wptr_dw = RREG32(dev, regWptr);

    // 2. Locate the frame slot by VRAM byte offset (the ring is in VRAM).
    uint32_t frame_slot;
    if ((wptr_dw % ringSizeDw) == 0) {
        frame_slot = 0;
    } else {
        frame_slot = wptr_dw / frameSizeDw;
    }
    uint32_t max_slot = ringSizeBytes / sizeof(PSPGfxRBFrame);
    if (frame_slot >= max_slot) {
        PSP_LOG("ring_cmd_submit: wptr %u out of range", wptr_dw);
        return kIOReturnInternalError;
    }
    const uint64_t frameVRAMOff = psp.ringVRAMOffset +
        (uint64_t)frame_slot * sizeof(PSPGfxRBFrame);

    // 3. Stage the command buffer in VRAM via BAR0.
    bar0_memcpy_to_vram(dev, psp.cmdVRAMOffset, cmd, cmdSize);

    // 4. Bump the fence counter, zero the fence dword in VRAM, build the
    //    frame in a stack buffer and copy it into the ring slot.
    uint32_t fence_index = ++psp.fenceCounter;
    WBAR0_32(dev, psp.fenceVRAMOffset, 0);

    // Upstream amdgpu_psp.c:3485-3492 — memset the frame to zero, then set
    // ONLY these 5 fields. cmd_buf_size, sid, vmid, frame_type and every
    // reserved field stay zero. Setting cmd_buf_size to a non-zero value
    // makes PSP silently drop the frame — match upstream exactly.
    PSPGfxRBFrame frame;
    bzero(&frame, sizeof(frame));
    frame.cmd_buf_addr_hi = static_cast<uint32_t>(psp.cmdBusAddr >> 32);
    frame.cmd_buf_addr_lo = static_cast<uint32_t>(psp.cmdBusAddr & 0xFFFFFFFFu);
    frame.fence_addr_hi   = static_cast<uint32_t>(psp.fenceBusAddr >> 32);
    frame.fence_addr_lo   = static_cast<uint32_t>(psp.fenceBusAddr & 0xFFFFFFFFu);
    frame.fence_value     = fence_index;
    bar0_memcpy_to_vram(dev, frameVRAMOff, &frame, sizeof(frame));

    PSP_LOG("ring_cmd_submit: cmd_id=%#x wptr=%u slot=%u frame@vram+%#llx "
            "cmd_buf mc=%#llx fence mc=%#llx fence_value=%u",
            cmd_id, wptr_dw, frame_slot, (unsigned long long)frameVRAMOff,
            (unsigned long long)psp.cmdBusAddr,
            (unsigned long long)psp.fenceBusAddr, fence_index);

    // 5a. HDP flush — drain host-side write buffers so PSP sees our ring
    //     frame + cmd_buf. Upstream calls amdgpu_device_flush_hdp here.
    amdgpu_hdp_flush(dev);

    // 5b. Advance and publish wptr (in dwords).
    wptr_dw = (wptr_dw + frameSizeDw) % ringSizeDw;
    WREG32(dev, regWptr, wptr_dw);

    // 6. Wait for PSP to write the fence value into VRAM. Read it back via
    //    the BAR0 aperture — BAR0 reads bypass any CPU cache and go straight
    //    to PCIe / VRAM, so we always see the latest value.
    const uint64_t kBudgetUs = 20 * 1000000;   // Linux: 20000 x msleep(1)
    uint64_t elapsed = 0;
    uint32_t observed = 0;
    while (elapsed < kBudgetUs) {
        observed = RBAR2_32(dev, psp.fenceVRAMOffset);
        if (observed == fence_index) break;
        IOSleep(1);
        elapsed += 1000;
    }
    if (observed != fence_index) {
        PSP_LOG("ring_cmd_submit: fence timeout (cmd_id=%#x, expected %u, "
                "got %u, wptr now %u, resp.status=%#x, C2PMSG_64=%#010x, C2PMSG_81=%#010x)",
                cmd_id, fence_index, observed, RREG32(dev, regWptr),
                RBAR2_32(dev, psp.cmdVRAMOffset + kPSPGfxRespStatusOffset),
                RREG32(dev, SOC15_REG_OFFSET(dev, IPBlock::MP0, MP0Regs::C2PMSG_64)),
                RREG32(dev, SOC15_REG_OFFSET(dev, IPBlock::MP0, MP0Regs::C2PMSG_81)));
        return kIOReturnTimeout;
    }

    // 7. Read the response status from the cmd buffer in VRAM via BAR0.
    //    psp_gfx_cmd_resp.resp.status lives at offset 864.
    uint32_t resp_status = RBAR2_32(dev,
        psp.cmdVRAMOffset + kPSPGfxRespStatusOffset);
    if (outRespStatus) *outRespStatus = resp_status;
    if (resp_status != 0) {
        PSP_LOG("ring_cmd_submit: cmd_id=%#x PSP returned status %#x "
                "(fence=%u took %llu us)",
                cmd_id, resp_status, fence_index,
                (unsigned long long)elapsed);
        return kIOReturnError;
    }
    PSP_LOG("ring_cmd_submit: cmd_id=%#x ok (fence=%u, status=0, %llu us)",
            cmd_id, fence_index, (unsigned long long)elapsed);
    return kIOReturnSuccess;
}

//
// psp_setup_tmr — port of upstream psp_tmr_load (amdgpu_psp.c:916).
//
// IMPORTANT — on psp_v14_0 IP_VERSION(14,0,2) and (14,0,3), upstream's
// `psp_skip_tmr` returns true because the chip defaults both
// `boot_time_tmr = true` and `autoload_supported = true` (amdgpu_psp.c:
// 173-174, no override for 14,0,2/3 at lines 257-261). PSP's SOS sets up
// the TMR itself during boot, before the driver's ring even exists. Sending
// SETUP_TMR after SOS boot is silently dropped by PSP (ring submit lands in
// C2PMSG_67 but fence_buf is never written).
//
// So for v14_0_2/3 we skip the submit entirely. Tracked state still flips to
// tmrSetUp = true so downstream LOAD_IP_FW is allowed to proceed.
//
kern_return_t
psp_setup_tmr(DeviceContext &dev, PSPContext &psp)
{
    if (psp.tmrSetUp) return kIOReturnSuccess;
    if (!psp.ringCreated) {
        return kIOReturnNotReady;
    }

    PSP_LOG("setup_tmr: pinned PSP IP v%u.%u.%u, discovered MP0 v%u.%u.%u",
            kIP_PSP.major, kIP_PSP.minor, kIP_PSP.rev,
            dev.ip.version[(int)IPBlock::MP0].major,
            dev.ip.version[(int)IPBlock::MP0].minor,
            dev.ip.version[(int)IPBlock::MP0].rev);

    // psp_v14_0_x: boot_time_tmr + autoload → skip SETUP_TMR. SOS owns the
    // TMR allocation. But upstream's psp_tmr_init (amdgpu_psp.c:881-890)
    // ALSO calls psp_load_toc when `psp->toc.start_addr` is non-null — and
    // on psp_v14_0_3 it IS non-null because parse_sos_bin_descriptor
    // extracts the PSP_FW_TYPE_PSP_TOC sub-bin from psp_14_0_3_sos.bin.
    // Without that LOAD_TOC submit, PSP doesn't know the per-IP TMR slot
    // layout, so it rejects every TMR-resident LOAD_IP_FW (SDMA/CP_RS64/
    // MES/RLC) with TEE_BAD_PARAMETERS (0xFFFF0006) and AUTOLOAD_RLC returns
    // TEE_ERROR_ITEM_NOT_FOUND (0xFFFF0007). SMU + IMU pass because they
    // bypass TMR slotting.
    if (kIP_PSP.major == 14) {
        psp.tmrBusAddr = 0;  // SOS-managed; we have no MC address
        psp.tmrSize    = 0;

        if (psp.toc.start_addr != nullptr && psp.toc.size_bytes > 0) {
            uint32_t tmr_size = 0, toc_resp = 0;
            kern_return_t tr = psp_load_toc_subbin(
                dev, psp,
                psp.toc.start_addr,
                static_cast<uint32_t>(psp.toc.size_bytes),
                &tmr_size, &toc_resp);
            if (tr != kIOReturnSuccess) {
                PSP_LOG("setup_tmr: LOAD_TOC sub-bin FAILED kr=%#x resp=%#x "
                        "(toc_size=%llu); TMR-resident LOAD_IP_FW will "
                        "likely reject",
                        tr, toc_resp,
                        (unsigned long long)psp.toc.size_bytes);
                return tr;
            }
            PSP_LOG("setup_tmr: LOAD_TOC sub-bin ok — PSP reported "
                    "tmr_size=%u (%#x) from %llu-byte sub-bin",
                    tmr_size, tmr_size,
                    (unsigned long long)psp.toc.size_bytes);
        } else {
            PSP_LOG("setup_tmr: NO TOC sub-bin in SOS package "
                    "(toc.start_addr=%p size=%llu) — skipping LOAD_TOC; "
                    "expect TMR-resident LOAD_IP_FW to fail",
                    (const void *)psp.toc.start_addr,
                    (unsigned long long)psp.toc.size_bytes);
        }

        psp.tmrSetUp = true;
        PSP_LOG("setup_tmr: SKIP SETUP_TMR (psp_v14_0_%u — boot_time_tmr by "
                "SOS, see psp_skip_tmr in upstream amdgpu_psp.c:906); the "
                "reserved TMR slot at vram+%#llx stays unused",
                (unsigned)kIP_PSP.rev,
                (unsigned long long)psp.tmrVRAMOffset);
        return kIOReturnSuccess;
    }

    // Legacy / other PSP families that DO accept SETUP_TMR from the driver —
    // port of upstream psp_setup_tmr (amdgpu_psp.c:825 sets virt_phy_addr=1
    // unconditionally; system_phy_addr is the CPU-visible BAR phys address
    // of the TMR BO). Unreachable on this chip; kept for fidelity.
    uint64_t vram_start = psp_read_vram_start(dev);
    if (vram_start == 0) {
        PSP_LOG("setup_tmr: vram_start=0; MMHUB not ready");
        return kIOReturnNotReady;
    }
    psp.tmrBusAddr = dev.vramMC(psp.tmrVRAMOffset);
    psp.tmrSize    = kPSPTMRDefaultSize;

    uint8_t cmd_buf[kPSPGfxCmdRespSize];
    bzero(cmd_buf, sizeof(cmd_buf));
    auto *hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
    // buf_size/buf_version intentionally left at 0 — upstream's
    // acquire_psp_cmd_buf memsets the whole struct and never writes these
    // fields; PSP firmware on v14_0_3 may treat nonzero values as a version
    // mismatch. Only cmd_id is set.
    hdr->cmd_id = PSPGfxCmd::SETUP_TMR;

    auto *tmr = reinterpret_cast<PSPGfxCmdSetupTmr *>(
        cmd_buf + kPSPGfxCmdUnionOffset);
    tmr->buf_phy_addr_lo    = static_cast<uint32_t>(psp.tmrBusAddr & 0xFFFFFFFFu);
    tmr->buf_phy_addr_hi    = static_cast<uint32_t>(psp.tmrBusAddr >> 32);
    tmr->buf_size           = static_cast<uint32_t>(psp.tmrSize);
    tmr->tmr_flags          = 0x2;  // bit 1 = virt_phy_addr per upstream
    tmr->system_phy_addr_lo = static_cast<uint32_t>(psp.tmrBusAddr & 0xFFFFFFFFu);
    tmr->system_phy_addr_hi = static_cast<uint32_t>(psp.tmrBusAddr >> 32);

    PSP_LOG("SETUP_TMR: buf mc=%#llx size=%llu flags=%#x",
            (unsigned long long)psp.tmrBusAddr,
            (unsigned long long)psp.tmrSize, tmr->tmr_flags);

    uint32_t resp = 0;
    kern_return_t ret = psp_ring_cmd_submit(dev, psp, cmd_buf,
                                            kPSPGfxCmdRespSize, &resp);
    if (ret != kIOReturnSuccess) {
        PSP_LOG("SETUP_TMR submit failed: %#x (resp=%#x)", ret, resp);
        return ret;
    }
    psp.tmrSetUp = true;
    PSP_LOG("SETUP_TMR ok — TMR at mc=%#llx (vram+%#llx) size=%llu resp=%#x",
            (unsigned long long)psp.tmrBusAddr,
            (unsigned long long)psp.tmrVRAMOffset,
            (unsigned long long)psp.tmrSize, resp);
    return kIOReturnSuccess;
}

//
// psp_load_ip_fw — submit a GFX_CMD_ID_LOAD_IP_FW with the firmware already
// staged at `fwBusAddr`. PSP reads the bytes, validates the signature,
// copies them into the TMR and then to the target IP, and asserts the IP's
// reset. Returns success only if PSP's response status is 0.
//
kern_return_t
psp_load_ip_fw(DeviceContext &dev, PSPContext &psp,
               uint64_t fwBusAddr, uint32_t fwSize, uint32_t fwType)
{
    if (!psp.tmrSetUp) {
        PSP_LOG("load_ip_fw(type=%u): TMR not set up — call psp_setup_tmr "
                "first", fwType);
        return kIOReturnNotReady;
    }
    if (fwBusAddr == 0 || fwSize == 0) {
        return kIOReturnBadArgument;
    }
    // Upstream `psp_prep_load_ip_fw_cmd_buf` does NOT enforce 4 KB alignment
    // on fw_phy_addr — it just stuffs whatever came out of
    // `amdgpu_bo_gpu_offset(bo) + ucode_array_offset_bytes`. The BO is
    // page-aligned but ucode_array_offset_bytes is typically 32, so the
    // address is dword-aligned, not 4 KB. PSP accepts that; a 4 KB check
    // rejects every real call. Keep only the dword check.
    if (fwBusAddr & 0x3) {
        return kIOReturnNotAligned;
    }

    uint8_t cmd_buf[kPSPGfxCmdRespSize];
    bzero(cmd_buf, sizeof(cmd_buf));
    auto *hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
    // buf_size/buf_version intentionally left at 0 (upstream behaviour).
    hdr->cmd_id = PSPGfxCmd::LOAD_IP_FW;

    auto *load = reinterpret_cast<PSPGfxCmdLoadIpFw *>(
        cmd_buf + kPSPGfxCmdUnionOffset);
    load->fw_phy_addr_lo = static_cast<uint32_t>(fwBusAddr & 0xFFFFFFFFu);
    load->fw_phy_addr_hi = static_cast<uint32_t>(fwBusAddr >> 32);
    load->fw_size        = fwSize;
    load->fw_type        = fwType;

    PSP_LOG("LOAD_IP_FW: fw_type=%u fw_phy_addr=%#llx fw_size=%u",
            fwType, (unsigned long long)fwBusAddr, fwSize);

    uint32_t resp = 0;
    kern_return_t ret = psp_ring_cmd_submit(dev, psp, cmd_buf,
                                            kPSPGfxCmdRespSize, &resp);
    if (ret != kIOReturnSuccess) {
        PSP_LOG("LOAD_IP_FW(type=%u, size=%u, bus=%#llx) failed: %#x resp=%#x",
                fwType, fwSize, (unsigned long long)fwBusAddr, ret, resp);
        return ret;
    }
    // resp.fw_addr_{lo,hi} is where PSP placed the firmware inside the TMR.
    uint32_t fw_lo = RBAR2_32(dev, psp.cmdVRAMOffset + kPSPGfxRespFwAddrLoOffset);
    uint32_t fw_hi = RBAR2_32(dev, psp.cmdVRAMOffset + kPSPGfxRespFwAddrHiOffset);
    PSP_LOG("LOAD_IP_FW(type=%u, size=%u) ok — resp=%#x tmr_fw_addr=%#llx",
            fwType, fwSize, resp,
            (unsigned long long)(((uint64_t)fw_hi << 32) | fw_lo));
    if (fwType < 128) psp.tmr_fw_addr_by_type[fwType] = ((uint64_t)fw_hi << 32) | fw_lo;   // for the RS64 IC/DC bases
    return kIOReturnSuccess;
}

//
// psp_query_fw_reservation — port of upstream `psp_update_fw_reservation`
// (amdgpu_psp.c:1040). For psp_v14_0_2/3 with SOS firmware >= 0x3a0e14,
// upstream sends GFX_CMD_ID_FB_FW_RESERV_ADDR + _EXT_ADDR via the ring right
// after ring_create and BEFORE any LOAD_IP_FW.
//
kern_return_t
psp_query_fw_reservation(DeviceContext &dev, PSPContext &psp)
{
    if (!psp.ringCreated) {
        return kIOReturnNotReady;
    }
    auto submit_one = [&](uint32_t cmd_id, const char *name) -> kern_return_t {
        uint8_t cmd_buf[kPSPGfxCmdRespSize];
        bzero(cmd_buf, sizeof(cmd_buf));
        auto *hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
        // buf_size/buf_version left at 0 (upstream behaviour).
        hdr->cmd_id = cmd_id;
        PSP_LOG("query_fw_reservation: submitting %s (cmd_id=%#x, no payload)",
                name, cmd_id);
        uint32_t resp = 0;
        kern_return_t r = psp_ring_cmd_submit(dev, psp, cmd_buf,
                                              kPSPGfxCmdRespSize, &resp);
        if (r == kIOReturnSuccess) {
            // Pull reserve_base_address + reserve_size out of the response.
            // psp_gfx_uresp_fw_reserve_info lives inside psp_gfx_resp.uresp
            // (+64 within resp, resp itself at +864). Field order is
            // hi-then-lo (unusual).
            uint32_t addr_hi = RBAR2_32(dev,
                psp.cmdVRAMOffset + kPSPGfxRespUrespOffset + 0);
            uint32_t addr_lo = RBAR2_32(dev,
                psp.cmdVRAMOffset + kPSPGfxRespUrespOffset + 4);
            uint32_t rsv_sz  = RBAR2_32(dev,
                psp.cmdVRAMOffset + kPSPGfxRespUrespOffset + 8);
            uint64_t addr    = ((uint64_t)addr_hi << 32) | addr_lo;
            PSP_LOG("query_fw_reservation: %s ok (resp=%#x) addr=%#llx "
                    "size=%#x", name, resp, (unsigned long long)addr, rsv_sz);
            return kIOReturnSuccess;
        }
        // PSP_ERR_UNKNOWN_COMMAND (0x100) means SOS is too old to know this
        // command — upstream treats it as success + (addr=0, size=0).
        if (r == kIOReturnError && (resp & 0xFFFFu) == kPSPErrUnknownCommand) {
            PSP_LOG("query_fw_reservation: %s — SOS doesn't implement "
                    "(resp=%#x); ignoring", name, resp);
            return kIOReturnSuccess;
        }
        PSP_LOG("query_fw_reservation: %s FAILED kr=%#x resp=%#x",
                name, r, resp);
        return r;
    };
    kern_return_t r1 = submit_one(PSPGfxCmd::FB_FW_RESERV_ADDR,
                                  "FB_FW_RESERV_ADDR");
    if (r1 != kIOReturnSuccess) return r1;
    kern_return_t r2 = submit_one(PSPGfxCmd::FB_FW_RESERV_EXT_ADDR,
                                  "FB_FW_RESERV_EXT_ADDR");
    return r2;
}

// Read PSP's reported TMR size out of the response. Upstream psp_gfx_resp
// puts tmr_size at +16 WITHIN the resp struct, and resp itself lives at
// cmd_buf+864 (the same base the status and uresp reads use). The reference
// omitted the 864, reading cmd_buf+16 (= resp_buf_addr_hi, which we always
// write as 0). Log both so a hardware run settles which one PSP fills.
static uint32_t
psp_read_toc_tmr_size(const DeviceContext &dev, const PSPContext &psp)
{
    uint32_t tmr_size = RBAR2_32(dev,
        psp.cmdVRAMOffset + kPSPGfxRespTmrSizeOffset);
    uint32_t legacy   = RBAR2_32(dev, psp.cmdVRAMOffset + 16);
    PSP_LOG("load_toc: resp.tmr_size@%u=%u (%#x); reference's cmd_buf+16 "
            "dword=%#x", kPSPGfxRespTmrSizeOffset, tmr_size, tmr_size, legacy);
    return tmr_size;
}

//
// psp_load_toc — port of upstream `psp_load_toc` (amdgpu_psp.c:840) for a
// standalone gc_<v>_toc.bin FILE. Stages the payload in fw_pri (VRAM via
// BAR0) and submits GFX_CMD_ID_LOAD_TOC. PSP parses the TOC, validates, and
// writes the total TMR size needed for autoload back into the response.
//
kern_return_t
psp_load_toc(DeviceContext &dev, PSPContext &psp,
             const uint8_t *tocBin, uint32_t tocSize,
             uint32_t *outTmrSize, uint32_t *outRespStatus)
{
    if (!psp.ringCreated) {
        PSP_LOG("load_toc: ring not created");
        return kIOReturnNotReady;
    }
    if (tocBin == nullptr || tocSize < sizeof(common_firmware_header)) {
        return kIOReturnBadArgument;
    }

    // Parse the common header — upstream psp_init_toc_microcode pulls the
    // payload offset + size from `header.ucode_array_offset_bytes` and
    // `header.ucode_size_bytes`, and psp_load_toc submits only that PAYLOAD
    // (NOT the whole file). Sending the file as-is is rejected with status
    // 0x11 because the signature & size don't match what was signed.
    auto *hdr = reinterpret_cast<const common_firmware_header *>(tocBin);
    uint32_t payload_offset = hdr->ucode_array_offset_bytes;
    uint32_t payload_size   = hdr->ucode_size_bytes;
    if (payload_offset == 0 ||
        (uint64_t)payload_offset + payload_size > tocSize) {
        PSP_LOG("load_toc: header bad — off=%u size=%u file=%u",
                payload_offset, payload_size, tocSize);
        return kIOReturnBadArgument;
    }
    if (payload_size > psp.fwPriSize) {
        PSP_LOG("load_toc: payload %u B > fw_pri %llu B",
                payload_size, (unsigned long long)psp.fwPriSize);
        return kIOReturnNoSpace;
    }

    // 1. Stage the TOC PAYLOAD ONLY in fw_pri via BAR0. Mirrors upstream
    //    psp_copy_fw(psp, psp->toc.start_addr, psp->toc.size_bytes), which
    //    zeros the WHOLE fw_pri buffer first (amdgpu_psp.c:4191) THEN
    //    memcpys the payload. Without the zero, stale bytes from prior
    //    sub-bin loads sit at fw_pri[payload_size..]; PSP hashes past the
    //    declared toc_size and rejects with status 0x11.
    bar0_memset_vram(dev, psp.fwPriVRAMOffset, 0, psp.fwPriSize);
    bar0_memcpy_to_vram(dev, psp.fwPriVRAMOffset,
                        tocBin + payload_offset, payload_size);
    amdgpu_hdp_flush(dev);

    // 2. Build the LOAD_TOC frame.
    uint8_t cmd_buf[kPSPGfxCmdRespSize];
    bzero(cmd_buf, sizeof(cmd_buf));
    auto *cmd_hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
    // buf_size/buf_version intentionally left at 0 (upstream behaviour).
    cmd_hdr->cmd_id = PSPGfxCmd::LOAD_TOC;

    auto *toc = reinterpret_cast<PSPGfxCmdLoadToc *>(
        cmd_buf + kPSPGfxCmdUnionOffset);
    toc->toc_phy_addr_lo = static_cast<uint32_t>(psp.fwPriBusAddr & 0xFFFFFFFFu);
    toc->toc_phy_addr_hi = static_cast<uint32_t>(psp.fwPriBusAddr >> 32);
    toc->toc_size        = payload_size;

    PSP_LOG("LOAD_TOC: file=%u payload off=%u size=%u staged at vram+%#llx "
            "mc=%#llx", tocSize, payload_offset, payload_size,
            (unsigned long long)psp.fwPriVRAMOffset,
            (unsigned long long)psp.fwPriBusAddr);

    uint32_t resp = 0;
    kern_return_t r = psp_ring_cmd_submit(dev, psp, cmd_buf,
                                          kPSPGfxCmdRespSize, &resp);
    if (outRespStatus) *outRespStatus = resp;
    if (r != kIOReturnSuccess) {
        PSP_LOG("LOAD_TOC FAILED kr=%#x resp=%#x (payload off=%u size=%u)",
                r, resp, payload_offset, payload_size);
        return r;
    }

    // 3. Read tmr_size from the response.
    uint32_t tmr_size = psp_read_toc_tmr_size(dev, psp);
    if (outTmrSize) *outTmrSize = tmr_size;
    PSP_LOG("LOAD_TOC ok — resp=%#x PSP reported tmr_size=%u (%#x)",
            resp, tmr_size, tmr_size);
    return kIOReturnSuccess;
}

//
// psp_load_toc_subbin — submit GFX_CMD_ID_LOAD_TOC using the TOC SUB-BINARY
// that lives INSIDE psp_<chip>_sos.bin (v2 descriptor PSP_FW_TYPE_PSP_TOC).
// Unlike psp_load_toc, `subBin/subSize` are ALREADY the payload bytes — the
// SOS parser (psp_parse_sos_microcode) extracts those.
//
// Mirrors upstream psp_tmr_init's branch (amdgpu_psp.c:881-890):
//   if (psp->toc.start_addr && psp->toc.size_bytes && psp->fw_pri_buf)
//       psp_load_toc(psp, &tmr_size);
// whose body (amdgpu_psp.c:840-859) is:
//   psp_copy_fw(psp, psp->toc.start_addr, psp->toc.size_bytes);
//   psp_prep_load_toc_cmd_buf(cmd, psp->fw_pri_mc_addr, psp->toc.size_bytes);
//   psp_cmd_submit_buf(...)
//
kern_return_t
psp_load_toc_subbin(DeviceContext &dev, PSPContext &psp,
                    const uint8_t *subBin, uint32_t subSize,
                    uint32_t *outTmrSize, uint32_t *outRespStatus)
{
    if (!psp.ringCreated) {
        PSP_LOG("load_toc_subbin: ring not created");
        return kIOReturnNotReady;
    }
    if (subBin == nullptr || subSize == 0) {
        PSP_LOG("load_toc_subbin: empty (subBin=%p subSize=%u)",
                (const void *)subBin, subSize);
        return kIOReturnBadArgument;
    }
    if (subSize > psp.fwPriSize) {
        PSP_LOG("load_toc_subbin: payload %u B > fw_pri %llu B",
                subSize, (unsigned long long)psp.fwPriSize);
        return kIOReturnNoSpace;
    }

    // Mirror upstream psp_copy_fw: zero the whole 1 MB fw_pri, memcpy the
    // payload at offset 0.
    bar0_memset_vram(dev, psp.fwPriVRAMOffset, 0, psp.fwPriSize);
    bar0_memcpy_to_vram(dev, psp.fwPriVRAMOffset, subBin, subSize);
    amdgpu_hdp_flush(dev);

    uint8_t cmd_buf[kPSPGfxCmdRespSize];
    bzero(cmd_buf, sizeof(cmd_buf));
    auto *cmd_hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
    // buf_size/buf_version intentionally left at 0 (upstream behaviour).
    cmd_hdr->cmd_id = PSPGfxCmd::LOAD_TOC;

    auto *toc = reinterpret_cast<PSPGfxCmdLoadToc *>(
        cmd_buf + kPSPGfxCmdUnionOffset);
    toc->toc_phy_addr_lo = static_cast<uint32_t>(psp.fwPriBusAddr & 0xFFFFFFFFu);
    toc->toc_phy_addr_hi = static_cast<uint32_t>(psp.fwPriBusAddr >> 32);
    toc->toc_size        = subSize;

    PSP_LOG("LOAD_TOC (sub-bin): size=%u staged at vram+%#llx mc=%#llx "
            "(fw_pri zeroed first, %llu B)",
            subSize, (unsigned long long)psp.fwPriVRAMOffset,
            (unsigned long long)psp.fwPriBusAddr,
            (unsigned long long)psp.fwPriSize);

    uint32_t resp = 0;
    kern_return_t r = psp_ring_cmd_submit(dev, psp, cmd_buf,
                                          kPSPGfxCmdRespSize, &resp);
    if (outRespStatus) *outRespStatus = resp;
    if (r != kIOReturnSuccess) {
        PSP_LOG("LOAD_TOC (sub-bin) FAILED kr=%#x resp=%#x (size=%u, "
                "fw_pri_mc=%#llx)",
                r, resp, subSize, (unsigned long long)psp.fwPriBusAddr);
        return r;
    }

    uint32_t tmr_size = psp_read_toc_tmr_size(dev, psp);
    if (outTmrSize) *outTmrSize = tmr_size;
    PSP_LOG("LOAD_TOC (sub-bin) ok — resp=%#x PSP reported tmr_size=%u (%#x)",
            resp, tmr_size, tmr_size);
    return kIOReturnSuccess;
}

//
// psp_rlc_autoload_start — port of upstream `psp_rlc_autoload_start`
// (amdgpu_psp.c:3434). Submits a cmd-id-only frame
// (GFX_CMD_ID_AUTOLOAD_RLC = 0x21) telling SOS that all GFX firmware has
// been pre-staged; SOS then drives the per-IP autoload sequence. The caller
// MUST have already loaded all RS64 CP / MES / IMU / RLC sub-bins ending
// with RLC_G.
//
kern_return_t
psp_rlc_autoload_start(DeviceContext &dev, PSPContext &psp)
{
    if (!psp.ringCreated) {
        return kIOReturnNotReady;
    }
    uint8_t cmd_buf[kPSPGfxCmdRespSize];
    bzero(cmd_buf, sizeof(cmd_buf));
    auto *hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
    // buf_size/buf_version intentionally left at 0 (upstream behaviour).
    hdr->cmd_id = PSPGfxCmd::AUTOLOAD_RLC;

    PSP_LOG("AUTOLOAD_RLC: submitting cmd_id=%#x (no payload)",
            PSPGfxCmd::AUTOLOAD_RLC);

    uint32_t resp = 0;
    kern_return_t r = psp_ring_cmd_submit(dev, psp, cmd_buf,
                                          kPSPGfxCmdRespSize, &resp);
    if (r != kIOReturnSuccess) {
        PSP_LOG("rlc_autoload_start: FAILED kr=%#x resp=%#x", r, resp);
        return r;
    }
    PSP_LOG("rlc_autoload_start: ok (resp=%#x)", resp);
    return kIOReturnSuccess;
}

//
// psp_rl_load — direct port of upstream `psp_rl_load` (amdgpu_psp.c:1152).
// Submits the PSP-embedded Register List firmware via LOAD_IP_FW with
// fw_type = GFX_FW_TYPE_REG_LIST = 67. The RL bytes were extracted from the
// v2 SOS package by psp_parse_sos_microcode (PSP_FW_TYPE_PSP_RL).
//
// CRITICAL for autoload: upstream calls this right after psp_load_non_psp_fw
// (which ended with AUTOLOAD_RLC). PSP's autoload state machine waits for
// REG_LIST to arrive before completing GC bringup — without it,
// BOOTLOAD_STATUS never transitions and GC stays in reset.
//
// Mirrors upstream byte-for-byte:
//   memset(psp->fw_pri_buf, 0, PSP_1_MEG);
//   memcpy(psp->fw_pri_buf, psp->rl.start_addr, psp->rl.size_bytes);
//   cmd.cmd_load_ip_fw.fw_phy_addr = psp->fw_pri_mc_addr;
//   cmd.cmd_load_ip_fw.fw_size     = psp->rl.size_bytes;
//   cmd.cmd_load_ip_fw.fw_type     = GFX_FW_TYPE_REG_LIST;
//
kern_return_t
psp_rl_load(DeviceContext &dev, PSPContext &psp)
{
    if (!psp.ringCreated) return kIOReturnNotReady;
    if (psp.rl.start_addr == nullptr || psp.rl.size_bytes == 0) {
        PSP_LOG("rl_load: psp.rl absent (size=%llu, ptr=%p) — skipping "
                "(SOS package may not include RL)",
                (unsigned long long)psp.rl.size_bytes,
                (const void *)psp.rl.start_addr);
        return kIOReturnSuccess;
    }
    if (psp.fwPriSize == 0) {
        PSP_LOG("rl_load: fwPri not initialized");
        return kIOReturnNotReady;
    }
    if (psp.rl.size_bytes > psp.fwPriSize) {
        PSP_LOG("rl_load: RL %llu > fwPri %llu — too large",
                (unsigned long long)psp.rl.size_bytes,
                (unsigned long long)psp.fwPriSize);
        return kIOReturnNoMemory;
    }

    // memset fwPri to zero (PSP_1_MEG), then memcpy the RL bytes into it.
    bar0_memset_vram(dev, psp.fwPriVRAMOffset, 0, psp.fwPriSize);
    bar0_memcpy_to_vram(dev, psp.fwPriVRAMOffset,
                        psp.rl.start_addr, psp.rl.size_bytes);
    amdgpu_hdp_flush(dev);

    PSP_LOG("rl_load: staged RL (%llu B, fw_version=%#x) at vram+%#llx "
            "mc=%#llx",
            (unsigned long long)psp.rl.size_bytes, psp.rl.fw_version,
            (unsigned long long)psp.fwPriVRAMOffset,
            (unsigned long long)psp.fwPriBusAddr);

    // Submit LOAD_IP_FW with fw_phy_addr = fwPriBusAddr, fw_type = REG_LIST.
    kern_return_t r = psp_load_ip_fw(dev, psp, psp.fwPriBusAddr,
                                     static_cast<uint32_t>(psp.rl.size_bytes),
                                     PSPGfxFwType::REG_LIST);
    if (r != kIOReturnSuccess) {
        PSP_LOG("rl_load: LOAD_IP_FW(REG_LIST, size=%llu, mc=%#llx) "
                "FAILED kr=%#x",
                (unsigned long long)psp.rl.size_bytes,
                (unsigned long long)psp.fwPriBusAddr, r);
        return r;
    }
    PSP_LOG("rl_load: ok (size=%llu, mc=%#llx)",
            (unsigned long long)psp.rl.size_bytes,
            (unsigned long long)psp.fwPriBusAddr);
    return kIOReturnSuccess;
}

//============================================================
// psp_asd_initialize — port of upstream psp_asd_initialize + psp_ta_load
// (amdgpu_psp.c:1225, 1380) for the ASD case.
//
// Builds a GFX_CMD_ID_LOAD_ASD (0x4) cmd_buf pointing at the VRAM-staged ASD
// ucode and submits via the PSP ring. Upstream calls this between
// psp_rlc_autoload_start and psp_rl_load (amdgpu_psp.c:3153).
//
// PSP_ASD_SHARED_MEM_SIZE = 0 upstream (amdgpu_psp.h:68), so the shared-mem
// fields stay zero.
//
// psp.asd is populated by psp_parse_ta_microcode, which is NOT ported here
// (deviation D3) — so today this is always the no-op branch.
//============================================================
kern_return_t
psp_asd_initialize(DeviceContext &dev, PSPContext &psp)
{
    if (!psp.ringCreated) {
        PSP_LOG("asd_initialize: PSP ring not created");
        return kIOReturnNotReady;
    }
    if (!psp.asd.parsed || psp.asd.size_bytes == 0) {
        PSP_LOG("asd_initialize: no ASD staged (parsed=%d, size=%u) — "
                "skipping (psp_parse_ta_microcode is not ported; no TA "
                "package is embedded yet)",
                (int)psp.asd.parsed, psp.asd.size_bytes);
        return kIOReturnSuccess;
    }

    uint8_t cmd_buf[kPSPGfxCmdRespSize];
    bzero(cmd_buf, kPSPGfxCmdRespSize);

    auto *hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
    hdr->cmd_id = PSPGfxCmd::LOAD_ASD;

    // psp_gfx_cmd_load_ta lives at offset 28 (psp_gfx_if.h:129).
    auto *ta = reinterpret_cast<PSPGfxCmdLoadTa *>(
        cmd_buf + kPSPGfxCmdUnionOffset);
    ta->app_phy_addr_lo     = static_cast<uint32_t>(psp.asd.ucode_mc_addr & 0xFFFFFFFFu);
    ta->app_phy_addr_hi     = static_cast<uint32_t>(psp.asd.ucode_mc_addr >> 32);
    ta->app_len             = psp.asd.size_bytes;
    ta->cmd_buf_phy_addr_lo = 0;   // no shared mem for ASD
    ta->cmd_buf_phy_addr_hi = 0;
    ta->cmd_buf_len         = 0;   // PSP_ASD_SHARED_MEM_SIZE == 0

    PSP_LOG("asd_initialize: submitting LOAD_ASD (mc=%#llx, size=%u)",
            (unsigned long long)psp.asd.ucode_mc_addr, psp.asd.size_bytes);

    uint32_t resp_status = 0;
    kern_return_t r = psp_ring_cmd_submit(dev, psp, cmd_buf,
                                          kPSPGfxCmdRespSize, &resp_status);
    psp.asd.resp_status = resp_status;
    if (r != kIOReturnSuccess) {
        PSP_LOG("asd_initialize: psp_ring_cmd_submit FAILED kr=%#x "
                "resp_status=%#x", r, resp_status);
        return r;
    }

    // psp_gfx_resp = { uint32_t status; uint32_t session_id; ... }, so the
    // session id is the dword right after the status.
    psp.asd.session_id = RBAR2_32(dev,
        psp.cmdVRAMOffset + kPSPGfxRespSessionIdOffset);

    PSP_LOG("asd_initialize: ok (session_id=%#x, resp=%#x)",
            psp.asd.session_id, resp_status);
    return kIOReturnSuccess;
}

//============================================================
// psp_fw_buf_stage — NOT in the reference (deviation D2).
//
// Upstream amdgpu_ucode_create_bo allocates ONE fw_buf and gives EACH ucode
// its own MC address inside it. The reference did the bump-allocation in its
// LoadFirmware path (host-streamed .bin files); we have the blobs compiled
// in, so the equivalent is: bump a page-aligned slot, BAR0-copy into it,
// HDP-flush, return the MC address for LOAD_IP_FW.fw_phy_addr.
//============================================================
kern_return_t
psp_fw_buf_stage(DeviceContext &dev, PSPContext &psp,
                 const void *bytes, uint32_t size, uint64_t *outBusAddr)
{
    if (psp.fwBufSize == 0) {
        PSP_LOG("fw_buf_stage: psp_init not called");
        return kIOReturnNotReady;
    }
    if (bytes == nullptr || size == 0 || outBusAddr == nullptr) {
        return kIOReturnBadArgument;
    }
    constexpr uint64_t kFwBufAlign = 0x1000;
    const uint64_t slot_off =
        (psp.fwBufBumpOffset + kFwBufAlign - 1) & ~(kFwBufAlign - 1);
    const uint64_t slot_sz = (size + kFwBufAlign - 1) & ~(kFwBufAlign - 1);
    if (slot_off + slot_sz > psp.fwBufSize) {
        PSP_LOG("fw_buf_stage: fw_buf exhausted (want %llu @ %llu, cap %llu)",
                (unsigned long long)slot_sz, (unsigned long long)slot_off,
                (unsigned long long)psp.fwBufSize);
        return kIOReturnNoSpace;
    }

    const uint64_t slot_vram = psp.fwBufVRAMOffset + slot_off;
    const uint64_t slot_mc   = psp.fwBufBaseMC     + slot_off;
    bar0_memcpy_to_vram(dev, slot_vram, bytes, size);
    amdgpu_hdp_flush(dev);
    psp.fwBufBumpOffset = slot_off + slot_sz;
    *outBusAddr = slot_mc;

    PSP_LOG("fw_buf_stage: %u B at vram+%#llx mc=%#llx (slot %llu..%llu of "
            "%llu)",
            size, (unsigned long long)slot_vram, (unsigned long long)slot_mc,
            (unsigned long long)slot_off,
            (unsigned long long)(slot_off + slot_sz),
            (unsigned long long)psp.fwBufSize);
    return kIOReturnSuccess;
}

//============================================================
// psp_parse_sos_microcode — port of upstream amdgpu_psp.c
// psp_init_sos_microcode. Auto-detects v1 vs v2 header and populates
// psp.sos / psp.kdb / psp.sys / etc. with pointers into the input blob.
//============================================================

static void
set_sub_bin(PSPContext::PSPSubBin &dst,
            const uint8_t *base, uint32_t offset_bytes,
            uint32_t size_bytes, uint32_t fw_version)
{
    dst.start_addr = (size_bytes > 0) ? (base + offset_bytes) : nullptr;
    dst.size_bytes = size_bytes;
    dst.fw_version = fw_version;
}

kern_return_t
psp_parse_sos_microcode(PSPContext &psp,
                        const uint8_t *fw_data, uint64_t fw_size)
{
    // Reset every sub-bin so a re-parse doesn't leave stale pointers.
    psp.sos = psp.sys = psp.kdb = psp.toc = psp.spl = psp.rl =
        psp.soc_drv = psp.intf_drv = psp.dbg_drv = psp.ras_drv =
        psp.ipkeymgr_drv = psp.spdm_drv = psp.sys_drv_aux =
        psp.sos_aux = PSPContext::PSPSubBin{};

    if (fw_data == nullptr || fw_size < sizeof(common_firmware_header)) {
        PSP_LOG("parse_sos: fw_data null or too small (%llu)",
                (unsigned long long)fw_size);
        return kIOReturnBadArgument;
    }

    auto *hdr = reinterpret_cast<const common_firmware_header *>(fw_data);
    uint32_t ucode_off = hdr->ucode_array_offset_bytes;
    if (ucode_off > fw_size) {
        PSP_LOG("parse_sos: ucode_array_offset_bytes %#x exceeds fw_size %llu",
                ucode_off, (unsigned long long)fw_size);
        return kIOReturnBadArgument;
    }
    const uint8_t *ucode_base = fw_data + ucode_off;

    PSP_LOG("parse_sos: hdr ver %u.%u, ip %u.%u, ucode_size=%u, "
            "ucode_off=%#x, total=%u",
            hdr->header_version_major, hdr->header_version_minor,
            hdr->ip_version_major, hdr->ip_version_minor,
            hdr->ucode_size_bytes, ucode_off, hdr->size_bytes);

    psp.sos_fw_blob      = fw_data;
    psp.sos_fw_blob_size = fw_size;

    switch (hdr->header_version_major) {
    case 1: {
        // v1.0: only sos. v1.1/1.2: + kdb (and maybe toc). v1.3: +spl/rl/aux.
        // v1 layouts pack sub-bins WITHIN the ucode region — the start
        // address is `ucode_base + sub.offset_bytes`, as upstream does.
        auto *h10 = reinterpret_cast<const psp_firmware_header_v1_0 *>(fw_data);
        // The "sys" sub-bin doesn't exist as a separate field on v1.0 —
        // upstream's psp_init_sos_base_fw fills sys.start_addr from
        // sos.offset_bytes (sys precedes sos in the ucode region for legacy
        // chips). Mirror that.
        set_sub_bin(psp.sys, ucode_base,
                    0,
                    h10->sos.offset_bytes,  // sys spans [0, sos_off)
                    hdr->ucode_version);
        set_sub_bin(psp.sos, ucode_base,
                    h10->sos.offset_bytes,
                    h10->sos.size_bytes,
                    h10->sos.fw_version);

        if (hdr->header_version_minor >= 1) {
            auto *h11 = reinterpret_cast<const psp_firmware_header_v1_1 *>(fw_data);
            set_sub_bin(psp.toc, ucode_base,
                        h11->toc.offset_bytes,
                        h11->toc.size_bytes, h11->toc.fw_version);
            set_sub_bin(psp.kdb, ucode_base,
                        h11->kdb.offset_bytes,
                        h11->kdb.size_bytes, h11->kdb.fw_version);
        }
        if (hdr->header_version_minor == 2) {
            // v1.2 redefines: kdb instead of toc
            auto *h12 = reinterpret_cast<const psp_firmware_header_v1_2 *>(fw_data);
            set_sub_bin(psp.kdb, ucode_base,
                        h12->kdb.offset_bytes,
                        h12->kdb.size_bytes, h12->kdb.fw_version);
        }
        if (hdr->header_version_minor == 3) {
            auto *h13 = reinterpret_cast<const psp_firmware_header_v1_3 *>(fw_data);
            set_sub_bin(psp.spl, ucode_base,
                        h13->spl.offset_bytes,
                        h13->spl.size_bytes, h13->spl.fw_version);
            set_sub_bin(psp.rl, ucode_base,
                        h13->rl.offset_bytes,
                        h13->rl.size_bytes, h13->rl.fw_version);
            set_sub_bin(psp.sys_drv_aux, ucode_base,
                        h13->sys_drv_aux.offset_bytes,
                        h13->sys_drv_aux.size_bytes,
                        h13->sys_drv_aux.fw_version);
            set_sub_bin(psp.sos_aux, ucode_base,
                        h13->sos_aux.offset_bytes,
                        h13->sos_aux.size_bytes,
                        h13->sos_aux.fw_version);
        }
        return kIOReturnSuccess;
    }
    case 2: {
        // v2.0/v2.1: flexible array of psp_fw_bin_desc tagged by fw_type.
        // v2.1 has an extra `psp_aux_fw_bin_index` field we don't need (only
        // relevant for chips with auxiliary SOS variants we don't support).
        auto *h20 = reinterpret_cast<const psp_firmware_header_v2_0 *>(fw_data);
        const psp_fw_bin_desc *bin = h20->psp_fw_bin;
        uint32_t count = h20->psp_fw_bin_count;
        if (hdr->header_version_minor == 1) {
            auto *h21 = reinterpret_cast<const psp_firmware_header_v2_1 *>(fw_data);
            bin = h21->psp_fw_bin;
        }
        if (count > 64) {
            PSP_LOG("parse_sos: implausible bin_count=%u", count);
            return kIOReturnBadArgument;
        }
        for (uint32_t i = 0; i < count; i++) {
            const auto &d = bin[i];
            switch (d.fw_type) {
            case PSP_FW_TYPE_PSP_SOS:
                set_sub_bin(psp.sos, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_SYS_DRV:
                set_sub_bin(psp.sys, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_KDB:
                set_sub_bin(psp.kdb, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_TOC:
                set_sub_bin(psp.toc, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_SPL:
                set_sub_bin(psp.spl, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_RL:
                set_sub_bin(psp.rl, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_SOC_DRV:
                set_sub_bin(psp.soc_drv, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_INTF_DRV:
                set_sub_bin(psp.intf_drv, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_DBG_DRV:
                set_sub_bin(psp.dbg_drv, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_RAS_DRV:
                set_sub_bin(psp.ras_drv, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_IPKEYMGR_DRV:
                set_sub_bin(psp.ipkeymgr_drv, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_SPDM_DRV:
                set_sub_bin(psp.spdm_drv, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            default:
                PSP_LOG("parse_sos: ignoring unknown fw_type=%u "
                        "(offset=%#x size=%u)",
                        d.fw_type, d.offset_bytes, d.size_bytes);
                break;
            }
        }
        PSP_LOG("parse_sos: %u sub-images — SOS %llu (ver %#x) TOC %llu "
                "(ver %#x) RL %llu (ver %#x) KDB %llu SPL %llu SYS %llu",
                count,
                (unsigned long long)psp.sos.size_bytes, psp.sos.fw_version,
                (unsigned long long)psp.toc.size_bytes, psp.toc.fw_version,
                (unsigned long long)psp.rl.size_bytes,  psp.rl.fw_version,
                (unsigned long long)psp.kdb.size_bytes,
                (unsigned long long)psp.spl.size_bytes,
                (unsigned long long)psp.sys.size_bytes);
        return kIOReturnSuccess;
    }
    default:
        PSP_LOG("parse_sos: unsupported header_version_major=%u",
                hdr->header_version_major);
        return kIOReturnUnsupported;
    }
}

//
// psp_load_non_psp_fw — port of upstream amdgpu_psp.c:3051
//
// Loads all non-PSP firmware (SMU, IMU, RLC, CP, SDMA, MES) through the PSP
// ring in the correct upstream order.
//
// The caller provides firmware data via the FirmwareLoader callback. Each
// callback receives a fw_type and returns (bus_addr, size) for the payload;
// the caller is responsible for staging the bytes where PSP can read them
// (psp_fw_buf_stage does this for the VRAM fw_buf).
//
// Upstream order (AMDGPU_UCODE_ID_* enum, amdgpu_ucode.h:515-529):
//   1. SMU (GFX_FW_TYPE_SMU = 18)
//   2. IMU_I (68) + IMU_D (69)
//   3. RLC sub-bins in order:
//      a. GLOBAL_TAP_DELAYS (v2.4 only)
//      b. SE0-3_TAP_DELAYS (v2.4 only)
//      c. RLC_RESTORE_LIST_SRM_CNTL (v2.1+)
//      d. RLC_RESTORE_LIST_GPM_MEM (v2.1+)
//      e. RLC_RESTORE_LIST_SRM_MEM (v2.1+)
//      f. RLC_IRAM (v2.2+)
//      g. RLC_DRAM (v2.2+)
//      h. RLC_P (v2.3+)
//      i. RLC_V (v2.3+)
//      j. RLC_G (always — LAST, triggers autoload)
//   4. psp_rlc_autoload_start() ← CRITICAL
//   5. CP_RS64: PFP + stacks, ME + stacks, MEC + stacks
//   6. SDMA (GFX_FW_TYPE_SDMA_UCODE_TH0 = 71)
//   7. MES: CP_MES + CP_MES_DATA
//
kern_return_t
psp_load_non_psp_fw(DeviceContext &dev, PSPContext &psp,
                    const FirmwareLoader &loader)
{
    kern_return_t r;
    uint64_t fwBusAddr = 0;
    uint32_t fwSize = 0;
    bool rlcGLoaded = false;

    PSP_LOG("load_non_psp_fw: starting firmware load sequence (Linux AMDGPU_UCODE_ID order: SMU, SDMA, CP, MES, IMU, RLC; autoload last)");

    // 1. Load SMU firmware
    r = loader.get_payload(PSPGfxFwType::SMU, fwBusAddr, fwSize);
    if (r != kIOReturnSuccess) {
        PSP_LOG("load_non_psp_fw: SMU load failed kr=%#x", r);
        return r;
    }
    r = psp_load_ip_fw(dev, psp, fwBusAddr, fwSize, PSPGfxFwType::SMU);
    if (r != kIOReturnSuccess) {
        PSP_LOG("load_non_psp_fw: SMU LOAD_IP_FW failed kr=%#x", r);
        return r;
    }
    PSP_LOG("load_non_psp_fw: SMU loaded (size=%u)", fwSize);

    // 2. Load SDMA firmware (single RS64 TH0 payload for RDNA4).
    r = loader.get_payload(PSPGfxFwType::SDMA_UCODE_TH0, fwBusAddr, fwSize);
    if (r == kIOReturnUnsupported) {
        PSP_LOG("load_non_psp_fw: SDMA not present");
    } else if (r != kIOReturnSuccess) {
        PSP_LOG("load_non_psp_fw: SDMA get_payload failed kr=%#x", r);
        return r;
    } else {
        r = psp_load_ip_fw(dev, psp, fwBusAddr, fwSize,
                           PSPGfxFwType::SDMA_UCODE_TH0);
        if (r != kIOReturnSuccess) {
            PSP_LOG("load_non_psp_fw: SDMA LOAD_IP_FW failed kr=%#x", r);
            return r;
        }
        PSP_LOG("load_non_psp_fw: SDMA loaded (size=%u)", fwSize);
    }

    // 3. Load CP RS64 firmware (PFP, ME, MEC + per-pipe stacks).
    //    Each CP file emits: ucode + 2-4 stack payloads.
    {
        struct CPSubBin {
            uint32_t fw_type;
            const char *name;
        } bins[] = {
            { PSPGfxFwType::RS64_PFP,       "RS64_PFP" },
            { PSPGfxFwType::RS64_PFP_P0,    "RS64_PFP_P0" },
            { PSPGfxFwType::RS64_PFP_P1,    "RS64_PFP_P1" },
            { PSPGfxFwType::RS64_ME,        "RS64_ME" },
            { PSPGfxFwType::RS64_ME_P0,     "RS64_ME_P0" },
            { PSPGfxFwType::RS64_ME_P1,     "RS64_ME_P1" },
            { PSPGfxFwType::RS64_MEC,       "RS64_MEC" },
            { PSPGfxFwType::RS64_MEC_P0,    "RS64_MEC_P0" },
            { PSPGfxFwType::RS64_MEC_P1,    "RS64_MEC_P1" },
            { PSPGfxFwType::RS64_MEC_P2,    "RS64_MEC_P2" },
            { PSPGfxFwType::RS64_MEC_P3,    "RS64_MEC_P3" },
        };

        for (auto &bin : bins) {
            r = loader.get_payload(bin.fw_type, fwBusAddr, fwSize);
            if (r == kIOReturnUnsupported) {
                PSP_LOG("load_non_psp_fw: %s not present", bin.name);
                continue;
            }
            if (r != kIOReturnSuccess) {
                PSP_LOG("load_non_psp_fw: %s get_payload failed kr=%#x",
                        bin.name, r);
                return r;
            }
            r = psp_load_ip_fw(dev, psp, fwBusAddr, fwSize, bin.fw_type);
            if (r != kIOReturnSuccess) {
                PSP_LOG("load_non_psp_fw: %s LOAD_IP_FW failed kr=%#x",
                        bin.name, r);
                return r;
            }
            PSP_LOG("load_non_psp_fw: %s loaded (size=%u)", bin.name, fwSize);
        }
    }

    // 4. Load MES firmware (CP_MES + CP_MES_DATA for uni_mes packaging).
    {
        struct MESBin {
            uint32_t fw_type;
            const char *name;
        } bins[] = {
            { PSPGfxFwType::CP_MES,       "CP_MES" },
            { PSPGfxFwType::CP_MES_DATA,  "CP_MES_DATA" },
        };

        for (auto &bin : bins) {
            r = loader.get_payload(bin.fw_type, fwBusAddr, fwSize);
            if (r == kIOReturnUnsupported) {
                PSP_LOG("load_non_psp_fw: %s not present", bin.name);
                continue;
            }
            if (r != kIOReturnSuccess) {
                PSP_LOG("load_non_psp_fw: %s get_payload failed kr=%#x",
                        bin.name, r);
                return r;
            }
            r = psp_load_ip_fw(dev, psp, fwBusAddr, fwSize, bin.fw_type);
            if (r != kIOReturnSuccess) {
                PSP_LOG("load_non_psp_fw: %s LOAD_IP_FW failed kr=%#x",
                        bin.name, r);
                return r;
            }
            PSP_LOG("load_non_psp_fw: %s loaded (size=%u)", bin.name, fwSize);
        }
    }

    // 5. Load IMU firmware (I + D)
    r = loader.get_payload(PSPGfxFwType::IMU_I, fwBusAddr, fwSize);
    if (r == kIOReturnSuccess) {
        r = psp_load_ip_fw(dev, psp, fwBusAddr, fwSize, PSPGfxFwType::IMU_I);
        if (r != kIOReturnSuccess) {
            PSP_LOG("load_non_psp_fw: IMU_I LOAD_IP_FW failed kr=%#x", r);
            return r;
        }
        PSP_LOG("load_non_psp_fw: IMU_I loaded (size=%u)", fwSize);
    }
    r = loader.get_payload(PSPGfxFwType::IMU_D, fwBusAddr, fwSize);
    if (r == kIOReturnSuccess) {
        r = psp_load_ip_fw(dev, psp, fwBusAddr, fwSize, PSPGfxFwType::IMU_D);
        if (r != kIOReturnSuccess) {
            PSP_LOG("load_non_psp_fw: IMU_D LOAD_IP_FW failed kr=%#x", r);
            return r;
        }
        PSP_LOG("load_non_psp_fw: IMU_D loaded (size=%u)", fwSize);
    }

    // 6. Load RLC sub-bins in correct upstream enum order. RLC_G must be
    //    LAST because upstream calls psp_rlc_autoload_start() immediately
    //    after RLC_G loads (amdgpu_psp.c:3113-3121).
    {
        struct RLCSubBin {
            uint32_t fw_type;
            const char *name;
        } bins[] = {
            { PSPGfxFwType::GLOBAL_TAP_DELAYS,    "GLOBAL_TAP_DELAYS" },
            { PSPGfxFwType::SE0_TAP_DELAYS,       "SE0_TAP_DELAYS" },
            { PSPGfxFwType::SE1_TAP_DELAYS,       "SE1_TAP_DELAYS" },
            { PSPGfxFwType::SE2_TAP_DELAYS,       "SE2_TAP_DELAYS" },
            { PSPGfxFwType::SE3_TAP_DELAYS,       "SE3_TAP_DELAYS" },
            { PSPGfxFwType::RLC_RESTORE_LIST_SRM_CNTL, "RLC_RESTORE_LIST_SRM_CNTL" },
            { PSPGfxFwType::RLC_RESTORE_LIST_GPM_MEM,  "RLC_RESTORE_LIST_GPM_MEM" },
            { PSPGfxFwType::RLC_RESTORE_LIST_SRM_MEM,  "RLC_RESTORE_LIST_SRM_MEM" },
            { PSPGfxFwType::RLC_IRAM,             "RLC_IRAM" },
            { PSPGfxFwType::RLC_DRAM_BOOT,        "RLC_DRAM_BOOT" },
            { PSPGfxFwType::RLC_P,                "RLC_P" },
            { PSPGfxFwType::RLC_V,                "RLC_V" },
            { PSPGfxFwType::RLC_G,                "RLC_G" },
        };

        for (auto &bin : bins) {
            r = loader.get_payload(bin.fw_type, fwBusAddr, fwSize);
            if (r == kIOReturnUnsupported) {
                // This sub-bin is not present in this firmware version.
                PSP_LOG("load_non_psp_fw: %s not present (v2.x variant)",
                        bin.name);
                continue;
            }
            if (r != kIOReturnSuccess) {
                PSP_LOG("load_non_psp_fw: %s get_payload failed kr=%#x",
                        bin.name, r);
                return r;
            }
            r = psp_load_ip_fw(dev, psp, fwBusAddr, fwSize, bin.fw_type);
            if (r != kIOReturnSuccess) {
                PSP_LOG("load_non_psp_fw: %s LOAD_IP_FW failed kr=%#x",
                        bin.name, r);
                return r;
            }
            PSP_LOG("load_non_psp_fw: %s loaded (size=%u)", bin.name, fwSize);

            // Autoload is started AFTER every gfx firmware has been received
            // (Linux amdgpu_psp.c: "Start rlc autoload after psp received all
            // the gfx firmware"; RLC_G is the last AMDGPU_UCODE_ID gfx entry).
            if (bin.fw_type == PSPGfxFwType::RLC_G) rlcGLoaded = true;
        }
    }

    // 7. RLC autoload — only now that PSP holds every gfx firmware (Linux
    //    amdgpu_psp.c:3113-3121 fires this right after RLC_G, which is the
    //    LAST gfx entry in AMDGPU_UCODE_ID order — i.e. after SDMA/CP/MES/IMU).
    if (rlcGLoaded) {
        r = psp_rlc_autoload_start(dev, psp);
        if (r != kIOReturnSuccess) {
            PSP_LOG("load_non_psp_fw: rlc_autoload_start FAILED kr=%#x", r);
            return r;
        }
        PSP_LOG("load_non_psp_fw: RLC autoload started (after all gfx firmware)");
    } else {
        PSP_LOG("load_non_psp_fw: RLC_G was not loaded — autoload NOT started");
    }

    PSP_LOG("load_non_psp_fw: all firmware loaded successfully");
    return kIOReturnSuccess;
}

} // namespace amdgpu
