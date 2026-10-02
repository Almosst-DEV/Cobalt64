//
//  cp_v12_0.cpp — CP (PFP / ME / MEC) bring-up + GFX ring / KIQ storage +
//  PM4 ring writer for GFX12 (RDNA4), x86 kernel kext.
//
//  Origin: lemonade-sdk/mac-amdgpu (MIT) @ commit 3bdeed2,
//          dext/amdgpu/cp_v12_0.cpp (arm64 DriverKit dext).
//  MIT License. The reference is MIT-licensed; see ../../NOTICE.
//
//  Upstream sources behind the reference (line numbers are this tree's
//  ref/linux-amdgpu/gfx_v12_0.c, which is the authority for every citation
//  in this file — the reference dext's own numbers were stale):
//      drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c
//          gfx_v12_0_config_gfx_rs64      (2142)   <-- was never ported
//          gfx_v12_0_cp_gfx_enable        (2365)
//          gfx_v12_0_cp_gfx_start         (2699)
//          gfx_v12_0_cp_gfx_switch_pipe   (2712)
//          gfx_v12_0_cp_gfx_set_doorbell  (2723)
//          gfx_v12_0_cp_gfx_resume        (2748)
//          gfx_v12_0_cp_compute_enable    (2811)
//          gfx_v12_0_cp_set_doorbell_range(2987)
//          gfx_v12_0_kcq_resume           (3115)
//          gfx_v12_0_cp_resume            (3522)
//          gfx_v12_0_hw_init              (3765)
//      drivers/gpu/drm/amd/amdgpu/soc24.c  soc24_grbm_select (102)
//      drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c
//
//  Coverage:
//      cp_alloc_storage       ring + MQD + write-back page
//      cp_release_storage     teardown
//      cp_ring_write          stage PM4 dwords at the software wptr
//      cp_emit_eop_fence      NOP + RELEASE_MEM into the ring
//      cp_config_gfx_rs64     RS64 PFP/ME/MEC program counters + pipe resets
//      cp_hqd_program         cp_gfx_resume register sequence
//      cp_gfx_start           CP_MAX_CONTEXT / CP_DEVICE_ID / cp_gfx_enable
//      cp_ring_fetch_probe    NOP block + doorbell + RB_RPTR poll (local)
//      cp_dump_state          CP/GRBM failure dump (local)
//      cp_compute_enable      CP_MEC_RS64_CNTL (RS64 MEC halt/unhalt)
//      cp_set_doorbell_range  CP_RB_/CP_MEC_DOORBELL_RANGE_{LOWER,UPPER}
//      cp_enable              CP_ME_CNTL.{PFP_HALT, ME_HALT}
//      cp_kick_doorbell       wptr shadow + BAR2 doorbell write
//      cp_submit_eop_test     emit + kick + poll *fence_cpu
//      cp_kiq_smoke_test      PM4 NOP + RELEASE_MEM to a VRAM fence slot
//      cp_init_full           BringupStage::CPInit entry
//
//  RS64 PFP/ME/MEC microcode is loaded through the PSP by earlier stages
//  (PSPFwLoad) and the RLC is started by the RLC stage. This module does
//  the RS64 entry-point configuration, the halt / unhalt and the queue
//  programming, exactly like upstream's PSP-autoload path
//  (gfx_v12_0_hw_init's `load_type == AMDGPU_FW_LOAD_PSP` branch plus
//  gfx_v12_0_cp_resume with the legacy direct-load branches stripped).
//
// ===========================================================================
//  Deviations from reference (dext/amdgpu/cp_v12_0.cpp @ 3bdeed2)
// ===========================================================================
//  1. Memory. cp_alloc_dma_block's IOBufferMemoryDescriptor::Create +
//     IODMACommand::Create + PrepareForDMA becomes one amdgpu::sysmem_alloc
//     (physically contiguous, zero-filled, no IOMMU). Alignment is 4096
//     (x86 page + the GPU page size gmc_bind_existing requires) instead of
//     the reference's kASPageSize (16 KB Apple Silicon page). sysmem_alloc
//     already zero-fills, so the reference's explicit memset()s are gone.
//
//  2. GART. THE substantive platform change. On Apple Silicon the reference
//     handed the CP the DART iova returned by PrepareForDMA. There is no
//     IOMMU here, so SysMem::bus is a raw host physical address the GPU
//     cannot reach; every buffer is therefore bound into the GART with
//     gmc_bind_existing() and the returned MC address is what goes into
//     ring_bus / mqd_bus / wb_bus (and hence CP_RB0_BASE,
//     CP_RB0_RPTR_ADDR*, CP_RB_WPTR_POLL_ADDR_*). Field names and every
//     consumer are unchanged. cp_alloc_storage consequently uses its `gmc`
//     argument, which the reference explicitly discarded with `(void)gmc`.
//     ORDERING NOTE: GART PTEs live in the VRAM-resident page table that
//     GMCInit allocates, so binding at CPInit time is safe even though
//     GFXHUB's gart_enable only runs later (GFXInit) — see the report.
//
//  3. Doorbells. `dev.pci->MemoryWrite32(dev.bar5MemIndex, off, wptr)`
//     becomes WDOORBELL32(dev, off, wptr) on the kext's BAR2 mapping
//     (amdgpu_regs.h). Same byte offset (doorbell_index * 8, the GFX12
//     8-byte doorbell stride). A sysmem_wmb() is issued before the
//     doorbell write so the staged PM4 dwords and the wptr shadow are
//     visible to the device first; the reference relied on DriverKit's
//     MemoryWrite32 barrier alone.
//
//  4. Delays. The reference used IOSleep(1) where upstream has udelay(50),
//     with a comment that IOSleep(1) is "the coarsest granularity available
//     in DriverKit". The kernel has IODelay(), so cp_compute_enable uses
//     IODelay(50) — i.e. upstream's exact wait. gfx_v12_0.c:2756's
//     mdelay(1) stays IOSleep(1) (already exact). cp_kiq_smoke_test's
//     100 us poll step is a real IODelay(100) instead of the reference's
//     busy-XOR-loop approximation; the total timeout is unchanged.
//
//  5. MES. cp_kiq_smoke_test's `MESContext &mes` parameter is a
//     `bool mes_kiq_armed` here (same argument position, same gate).
//     amdgpu_mes.h is not part of this port yet.
//
//  6. Logging. Every control-register write goes through a local helper
//     that logs `before -> written -> readback` with the absolute dword
//     address (task requirement; PORTING.md rule 6). This adds one MMIO
//     read before and one after each write — CP config registers only, no
//     side effects. All ring / MQD / WB addresses (bus, MC and CPU) are
//     logged. cp_compute_enable additionally reads CP_MEC_RS64_CNTL at
//     BOTH GC segments before writing (read-only) as a regression witness.
//     os_log/%{public}s → IOLog CP_LOG (amdgpu_log.h).
//
//  8. ADDRESSING. The reference addressed every CP register at SOC15
//     BASE_IDX 0. Per gc_12_0_0_offset.h, CP_ME_CNTL (0x0803) and
//     CP_MEC_RS64_CNTL (0x2904) are BASE_IDX 1 and GRBM_GFX_CNTL (0x0900)
//     is BASE_IDX 1; the rest of the CP window really is BASE_IDX 0.
//     Every access now goes through CP_REG() / GFX_REG(), which bake in
//     the header's per-register BASE_IDX (CPRegBaseIdx in amdgpu_cp.h,
//     GFXRegBaseIdx in amdgpu_gfx.h). cp_enable and cp_compute_enable
//     additionally refuse to run if that segment is unresolved, rather
//     than writing into a hole.
//
//  7. No dynamic C++ allocation, no STL, no floating point, no DriverKit.
//     All PM4 staging buffers are fixed-size stack arrays as in the
//     reference.
//
//  9. THE MISSING STAGE (new here, absent from the reference and from every
//     previous build): cp_config_gfx_rs64, a faithful port of
//     gfx_v12_0_config_gfx_rs64 for the PSP-load flow. The reference
//     released the RS64 cores from halt without ever programming
//     CP_{PFP,ME,MEC_RS64}_PRGRM_CNTR_START, so PFP/ME/MEC started
//     executing at address 0. cp_init_full now runs it between the halt
//     and the queue programming, where gfx_v12_0_hw_init:3814 runs it.
//
// 12. THE SECOND MISSING STAGE (0.0.15): cp_config_rs64_caches. With
//     PRGRM_CNTR_START in place, 0.0.14 showed the PFP/ME start and
//     immediately fault at VA page 0 (GCVM 0x0d33, client 6 = CPG) and
//     re-halt themselves — the RS64 instruction-cache bases are 0,
//     because the PSP does not program CP_PFP_IC_BASE / CP_ME_IC_BASE /
//     CP_CPC_IC_BASE (or the DC bases) in this flow, and upstream's PSP
//     path assumes it does. We now program them from the PSP's per-image
//     tmr_fw_addr, using the register sequence, ordering and waits of
//     upstream's DIRECT-load loaders (gfx_v12_0.c:2386-2520 PFP,
//     2530-2665 ME, 2843-2970 MEC).
//
// 10. PM4 OPCODES. Everything this file emits now uses cp_pm4_gfx12.h,
//     because amdgpu_pm4.h's `kPM4OpNop = 0x00` is wrong (IT_NOP is 0x10;
//     see that header). cp_emit_eop_fence and cp_kiq_smoke_test used to
//     prepend a 0xC03F0000 / 0xC0001000 "NOP" that is really a reserved
//     type-3 opcode — i.e. the first dword the CP would ever have parsed
//     was a bad-opcode fault. amdgpu_pm4.h is outside this change's file
//     ownership; see the report.
//
// 11. DOORBELL WIDTH. gfx_v12_0_ring_set_wptr_gfx does
//     atomic64_set(wptr_cpu_addr, wptr) + WDOORBELL64(index, wptr) — a
//     64-bit wptr shadow and a 64-bit doorbell store at byte offset
//     index * 4. cp_kick_doorbell wrote only the low 32 bits of each.
//     Now matches upstream.
//
//  Faithfully carried over (NOT fixed — reference behaviour the audit
//  trail should keep; see the report's risk list):
//    * cp_hqd_program re-writes CP_RB_DOORBELL_RANGE_{LOWER,UPPER} with a
//      different encoding than cp_set_doorbell_range used moments earlier
//      in cp_init_full. Upstream's cp_gfx_set_doorbell (gfx_v12_0.c:2723)
//      does the same thing in the same order.
//

#include <IOKit/IOLib.h>

#include "amdgpu_cp.h"
#include "amdgpu_gmc.h"
#include "amdgpu_gfx.h"
#include "amdgpu_mes.h"
#include "amdgpu_log.h"
#include "amdgpu_ucode_psp.h"
#include "cp_pm4_gfx12.h"
#include "../fw/fw_table.h"

namespace amdgpu {

//------------------------------------------------------------------
// GC register addressing. Every CP register carries its own SOC15
// BASE_IDX from gc_12_0_0_offset.h (CPRegBaseIdx, amdgpu_cp.h); the
// reference addressed them all at BASE_IDX 0. GFX_REG() reaches the two
// GRBM registers this module touches through GFXRegBaseIdx
// (amdgpu_gfx.h) for the same reason.
//------------------------------------------------------------------
#define CP_REG(dev, name) \
    SOC15_REG_OFFSET_BIDX((dev), IPBlock::GC, \
                          CPRegBaseIdx::name, CPRegs::name)
#define GFX_REG(dev, name) \
    SOC15_REG_OFFSET_BIDX((dev), IPBlock::GC, \
                          GFXRegBaseIdx::name, GFXRegs::name)

//============================================================
// Local helpers
//============================================================

// Write a register and log before / written / readback with the absolute
// dword address. PORTING.md rule 6 + the task's "before/after of each
// control register" requirement. Not in the reference.
static inline void
cp_wreg_logged(const DeviceContext &dev, const char *name,
               uint32_t reg, uint32_t value)
{
    const uint32_t before = RREG32(dev, reg);
    WREG32(dev, reg, value);
    const uint32_t after = RREG32(dev, reg);
    CP_LOG("  %s [dw %#x]: before=%#010x wrote=%#010x after=%#010x",
           name, reg, before, value, after);
}

// Read a register and log it with its absolute dword address.
static inline uint32_t
cp_rreg_logged(const DeviceContext &dev, const char *name, uint32_t reg)
{
    const uint32_t v = RREG32(dev, reg);
    CP_LOG("  %s [dw %#x] = %#010x", name, reg, v);
    return v;
}

// ----- cp_grbm_select: soc24_grbm_select (soc24.c:102) -----
//
// GRBM_GFX_CNTL selects which (ME, PIPE, QUEUE, VMID) instance the
// following GC register accesses are routed to. Upstream builds the value
// from ZERO — it is a select register, not a control register, so there is
// nothing to preserve; gfx_v12_0_cp_gfx_switch_pipe is the one caller that
// does a read-modify-write of PIPEID only.
//
// Every per-pipe write in cp_config_gfx_rs64 has to be bracketed by this,
// or all four MEC pipes get pipe 0's program counter and the other three
// stay at 0.
static void
cp_grbm_select(const DeviceContext &dev, uint32_t me, uint32_t pipe,
               uint32_t queue, uint32_t vmid)
{
    const uint32_t reg = GFX_REG(dev, GRBM_GFX_CNTL);
    uint32_t v = 0;
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, PIPEID,  pipe);
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, MEID,    me);
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, VMID,    vmid);
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, QUEUEID, queue);
    WREG32(dev, reg, v);
    const uint32_t rb = RREG32(dev, reg);
    CP_LOG("  grbm_select(me=%u pipe=%u queue=%u vmid=%u) [dw %#x] "
           "wrote=%#010x after=%#010x", me, pipe, queue, vmid, reg, v, rb);
}

// ----- cp_dump_state: everything that explains a stalled ring -----
void
cp_dump_state(const DeviceContext &dev, const CPContext &cp,
              const char *where)
{
    if (!dev.ip.isResolved(IPBlock::GC)) {
        CP_LOG("state dump (%s): GC IP base unresolved", where);
        return;
    }
    CP_LOG("=== CP state dump (%s) ===", where);
    CP_LOG("  sw wptr=%u  ring_mc=%#llx (%u dwords)  doorbell=%u",
           cp.wptr, (unsigned long long)cp.ring_bus,
           cp.ring_size_dwords, cp.doorbell_index);
    if (cp.wb_cpu) {
        sysmem_rmb();
        CP_LOG("  wb rptr=%u  wb wptr=%u  wb fence=%#llx "
               "(rptr_mc=%#llx wptr_mc=%#llx fence_mc=%#llx)",
               cp.rptr_cpu ? *cp.rptr_cpu : 0u,
               cp.wptr_cpu ? *cp.wptr_cpu : 0u,
               (unsigned long long)(cp.fence_cpu ? *cp.fence_cpu : 0ull),
               (unsigned long long)cp.rptr_gpu_addr,
               (unsigned long long)cp.wptr_gpu_addr,
               (unsigned long long)cp.fence_gpu_addr);
    }
    cp_rreg_logged(dev, "CP_RB0_RPTR",       CP_REG(dev, CP_RB0_RPTR));
    cp_rreg_logged(dev, "CP_RB0_WPTR",       CP_REG(dev, CP_RB0_WPTR));
    cp_rreg_logged(dev, "CP_RB0_WPTR_HI",    CP_REG(dev, CP_RB0_WPTR_HI));
    cp_rreg_logged(dev, "CP_RB0_CNTL",       CP_REG(dev, CP_RB0_CNTL));
    cp_rreg_logged(dev, "CP_RB0_BASE",       CP_REG(dev, CP_RB0_BASE));
    cp_rreg_logged(dev, "CP_RB0_BASE_HI",    CP_REG(dev, CP_RB0_BASE_HI));
    cp_rreg_logged(dev, "CP_RB_ACTIVE",      CP_REG(dev, CP_RB_ACTIVE));
    cp_rreg_logged(dev, "CP_RB_DOORBELL_CONTROL",
                   CP_REG(dev, CP_RB_DOORBELL_CONTROL));
    cp_rreg_logged(dev, "CP_RB_DOORBELL_RANGE_LOWER",
                   CP_REG(dev, CP_RB_DOORBELL_RANGE_LOWER));
    cp_rreg_logged(dev, "CP_RB_DOORBELL_RANGE_UPPER",
                   CP_REG(dev, CP_RB_DOORBELL_RANGE_UPPER));
    cp_rreg_logged(dev, "CP_ME_CNTL",        CP_REG(dev, CP_ME_CNTL));
    cp_rreg_logged(dev, "CP_MEC_RS64_CNTL",  CP_REG(dev, CP_MEC_RS64_CNTL));
    cp_rreg_logged(dev, "CP_STAT",           CP_REG(dev, CP_STAT));
    cp_rreg_logged(dev, "GRBM_STATUS",
                   SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC,
                                         kGFXRegBaseIdx_GRBM_STATUS,
                                         GFXRegsRO::GRBM_STATUS));
    cp_rreg_logged(dev, "GRBM_GFX_CNTL",     GFX_REG(dev, GRBM_GFX_CNTL));
    cp_rreg_logged(dev, "CP_PFP_PRGRM_CNTR_START",
                   CP_REG(dev, CP_PFP_PRGRM_CNTR_START));
    cp_rreg_logged(dev, "CP_PFP_PRGRM_CNTR_START_HI",
                   CP_REG(dev, CP_PFP_PRGRM_CNTR_START_HI));
    cp_rreg_logged(dev, "CP_ME_PRGRM_CNTR_START",
                   CP_REG(dev, CP_ME_PRGRM_CNTR_START));
    cp_rreg_logged(dev, "CP_ME_PRGRM_CNTR_START_HI",
                   CP_REG(dev, CP_ME_PRGRM_CNTR_START_HI));
    cp_rreg_logged(dev, "CP_MEC_RS64_PRGRM_CNTR_START",
                   CP_REG(dev, CP_MEC_RS64_PRGRM_CNTR_START));
    cp_rreg_logged(dev, "CP_MEC_RS64_PRGRM_CNTR_START_HI",
                   CP_REG(dev, CP_MEC_RS64_PRGRM_CNTR_START_HI));
    cp_rreg_logged(dev, "CP_PFP_IC_BASE_LO", CP_REG(dev, CP_PFP_IC_BASE_LO));
    cp_rreg_logged(dev, "CP_PFP_IC_BASE_HI", CP_REG(dev, CP_PFP_IC_BASE_HI));
    cp_rreg_logged(dev, "CP_PFP_IC_OP_CNTL", CP_REG(dev, CP_PFP_IC_OP_CNTL));
    cp_rreg_logged(dev, "CP_ME_IC_BASE_LO",  CP_REG(dev, CP_ME_IC_BASE_LO));
    cp_rreg_logged(dev, "CP_ME_IC_BASE_HI",  CP_REG(dev, CP_ME_IC_BASE_HI));
    cp_rreg_logged(dev, "CP_ME_IC_OP_CNTL",  CP_REG(dev, CP_ME_IC_OP_CNTL));
    cp_rreg_logged(dev, "CP_CPC_IC_OP_CNTL", CP_REG(dev, CP_CPC_IC_OP_CNTL));
    CP_LOG("  rs64_configured=%d (pfp pc=%#010x/%#010x me pc=%#010x/%#010x "
           "mec pc=%#010x/%#010x) caches_configured=%d fetch_proven=%d "
           "gfx_mqd_inited=%d kgq_mapped=%d ring_test_passed=%d",
           cp.rs64_configured ? 1 : 0, cp.pfp_pc, cp.pfp_pc_hi,
           cp.me_pc, cp.me_pc_hi, cp.mec_pc, cp.mec_pc_hi,
           cp.caches_configured ? 1 : 0, cp.fetch_proven ? 1 : 0,
           cp.gfx_mqd_inited ? 1 : 0, cp.kgq_mapped ? 1 : 0,
           cp.ring_test_passed ? 1 : 0);
    cp_dump_gfx_hqd(dev, where);
    CP_LOG("  TMR: pfp ic=%#llx dc=%#llx | me ic=%#llx dc=%#llx | "
           "mec ic=%#llx dc0=%#llx dc1=%#llx",
           (unsigned long long)cp.tmr_pfp_ic, (unsigned long long)cp.tmr_pfp_dc,
           (unsigned long long)cp.tmr_me_ic,  (unsigned long long)cp.tmr_me_dc,
           (unsigned long long)cp.tmr_mec_ic, (unsigned long long)cp.tmr_mec_dc0,
           (unsigned long long)cp.tmr_mec_dc1);
    CP_LOG("=== end CP state dump (%s) ===", where);
}

// Local copy of the alloc_dma_block helper from gmc_v12_0.cpp, adapted to
// SysMem + GART (deviations 1 and 2). Allocates `size` bytes of physically
// contiguous, zero-filled system memory and binds it into the GART, so the
// CP can reach it through GMC translation.
static kern_return_t
cp_alloc_dma_block(DeviceContext &dev, GMCContext &gmc, uint64_t size,
                   const char *what,
                   SysMem   *outSys,
                   uint64_t *outMc,
                   void    **outCpu)
{
    *outMc = 0; *outCpu = nullptr;

    // GPU page granularity is what gmc_bind_existing requires of the bus
    // address; the reference used kASPageSize because DART demanded 16 KB.
    kern_return_t r = sysmem_alloc(*outSys, size, kAMDGPUGPUPageSize);
    if (r != kIOReturnSuccess || !outSys->valid()) {
        CP_LOG("%s sysmem alloc failed: %#x", what, r);
        return (r != kIOReturnSuccess) ? r : kIOReturnNoMemory;
    }

    uint64_t mc = 0;
    r = gmc_bind_existing(dev, gmc, outSys->bus, outSys->size, &mc);
    if (r != kIOReturnSuccess) {
        CP_LOG("%s GART bind failed: %#x (bus=%#llx size=%llu)",
               what, r, (unsigned long long)outSys->bus,
               (unsigned long long)outSys->size);
        sysmem_free(*outSys);
        return r;
    }

    *outMc  = mc;
    *outCpu = outSys->cpu;
    CP_LOG("%s: %llu bytes cpu=%p bus=%#llx -> gart_mc=%#llx",
           what, (unsigned long long)outSys->size, outSys->cpu,
           (unsigned long long)outSys->bus, (unsigned long long)mc);
    return kIOReturnSuccess;
}

// ----- cp_alloc_storage: ring + MQD + write-back page (all sysmem) -----
kern_return_t
cp_alloc_storage(DeviceContext &dev, GMCContext &gmc, CPContext &cp)
{
    if (cp.inited) return kIOReturnSuccess;

    void *cpu = nullptr;
    kern_return_t r;

    // GFX/KIQ ring — power-of-two, page-aligned, in system memory. The CP
    // fetches PM4 through GART translation; sysmem is the easy path because
    // the CPU can write PM4 straight into it with no aperture involved.
    r = cp_alloc_dma_block(dev, gmc, kCPRingDefaultBytes, "ring",
                           &cp.ring_sys, &cp.ring_bus, &cpu);
    if (r != kIOReturnSuccess) {
        CP_LOG("KIQ ring sysmem alloc failed: %#x", r);
        return r;
    }
    cp.ring_cpu         = cpu;
    cp.ring_size_dwords = kCPRingDefaultBytes / 4;
    cp.ring_ptr_mask    = cp.ring_size_dwords - 1;

    // KIQ MQD — 4 KB sysmem. CP reads it once at queue-create and
    // doesn't touch it after.
    r = cp_alloc_dma_block(dev, gmc, kCPMQDBytes, "mqd",
                           &cp.mqd_sys, &cp.mqd_bus, &cpu);
    if (r != kIOReturnSuccess) {
        CP_LOG("KIQ MQD sysmem alloc failed: %#x", r);
        cp_release_storage(cp);
        return r;
    }
    cp.mqd_cpu = cpu;

    // Write-back page — sysmem, 16 KB. CP writes rptr/wptr/fence
    // values here; host reads from the same backing.
    r = cp_alloc_dma_block(dev, gmc, kCPWBPageBytes, "wb",
                           &cp.wb_sys, &cp.wb_bus, &cpu);
    if (r != kIOReturnSuccess) {
        CP_LOG("WB page alloc failed: %#x", r);
        cp_release_storage(cp);
        return r;
    }
    auto *wb = static_cast<uint8_t *>(cpu);
    cp.wb_cpu        = cpu;
    cp.rptr_cpu      = reinterpret_cast<volatile uint32_t *>(wb + kCPWBOffsetRptr);
    cp.wptr_cpu      = reinterpret_cast<volatile uint32_t *>(wb + kCPWBOffsetWptr);
    cp.fence_cpu     = reinterpret_cast<volatile uint64_t *>(wb + kCPWBOffsetFence);
    cp.rptr_gpu_addr  = cp.wb_bus + kCPWBOffsetRptr;
    cp.wptr_gpu_addr  = cp.wb_bus + kCPWBOffsetWptr;
    cp.fence_gpu_addr = cp.wb_bus + kCPWBOffsetFence;

    cp.wptr           = 0;
    cp.fence_counter  = 0;
    cp.doorbell_index = 0;  // assigned by cp_init_full
    cp.inited         = true;

    CP_LOG("storage ok: ring mc=%#llx (%u dwords, cpu=%p, phys=%#llx), "
           "mqd mc=%#llx (cpu=%p), wb mc=%#llx (cpu=%p)",
           (unsigned long long)cp.ring_bus, cp.ring_size_dwords,
           cp.ring_cpu, (unsigned long long)cp.ring_sys.bus,
           (unsigned long long)cp.mqd_bus, cp.mqd_cpu,
           (unsigned long long)cp.wb_bus, cp.wb_cpu);
    CP_LOG("storage wb slots: rptr mc=%#llx wptr mc=%#llx fence mc=%#llx",
           (unsigned long long)cp.rptr_gpu_addr,
           (unsigned long long)cp.wptr_gpu_addr,
           (unsigned long long)cp.fence_gpu_addr);
    return kIOReturnSuccess;
}

void
cp_release_storage(CPContext &cp)
{
    // GART slots stay "used" — the GART bump allocator has no free — so the
    // PTEs keep pointing at memory we are about to hand back. Nothing reads
    // them until the page table is re-zeroed by GMC; same bump-only
    // behaviour the reference documents for its VRAM allocator.
    sysmem_free(cp.wb_sys);
    sysmem_free(cp.mqd_sys);
    sysmem_free(cp.ring_sys);
    cp.wb_bus = 0; cp.wb_cpu = nullptr;
    cp.mqd_bus = 0; cp.mqd_cpu = nullptr;
    cp.ring_bus = 0; cp.ring_cpu = nullptr;
    cp.ring_size_dwords = 0; cp.ring_ptr_mask = 0;
    cp.rptr_cpu = nullptr; cp.wptr_cpu = nullptr; cp.fence_cpu = nullptr;
    cp.rptr_gpu_addr = 0; cp.wptr_gpu_addr = 0; cp.fence_gpu_addr = 0;
    cp.inited = false;
    CP_LOG("storage released");
}

// ----- cp_ring_write: stage PM4 dwords into the ring -----
//
// The ring lives in GART-mapped system memory, so the CPU writes the PM4
// dwords straight through the kernel VA (cp.ring_cpu). Unlike the
// reference's Apple Silicon situation there is no BAR aperture in the way,
// so this actually writes — the reference's "pretends to write" caveat in
// its header comment no longer applies.
//
// Does NOT kick the doorbell — the caller does that once every packet is
// staged (cp_kick_doorbell).
uint32_t
cp_ring_write(CPContext &cp, const uint32_t *src, uint32_t dwords)
{
    if (!cp.inited || src == nullptr || dwords == 0) return 0;
    if (dwords > cp.ring_size_dwords / 2) {
        CP_LOG("ring_write: %u dwords exceeds half-ring %u",
               dwords, cp.ring_size_dwords / 2);
        return 0;
    }
    auto *ring = static_cast<uint32_t *>(cp.ring_cpu);
    for (uint32_t i = 0; i < dwords; i++) {
        ring[cp.wptr] = src[i];
        cp.wptr = (cp.wptr + 1) & cp.ring_ptr_mask;
    }
    return dwords;
}

// ----- cp_emit_eop_fence: build NOP + RELEASE_MEM -----
//
// Returns the fence value the EOP write will deposit. Caller's
// responsibility to (a) kick the doorbell, (b) poll *fence_cpu.
uint32_t
cp_emit_eop_fence(CPContext &cp)
{
    if (!cp.inited) return 0;

    uint32_t pkt[16];
    uint32_t n = 0;

    // 1) NOP — sanity warm-up. cp_p3_nop1() (0xFFFF1000), NOT
    //    amdgpu_pm4.h's pm4_nop(), which builds opcode 0x00 instead of
    //    IT_NOP's 0x10 and would fault the CP on the very first dword.
    //    See cp_pm4_gfx12.h and deviation 10.
    pkt[n++] = cp_p3_nop1();

    // 2) RELEASE_MEM with INT_SEL_SEND_INT so the IH ring sees it.
    //    count = 6 → 7 payload dwords → 8 dwords total, which is exactly
    //    dw1, dw2, addr_lo, addr_hi, data_lo, data_hi and the trailing 0,
    //    matching gfx_v12_0_ring_emit_fence. (An older comment in this
    //    file claimed the trailing dword was unaccounted for; it is not —
    //    PACKET3(op, n) spans n + 2 dwords.)
    uint32_t fence = ++cp.fence_counter;
    pkt[n++] = cp_p3(kP3_RELEASE_MEM, 6);
    pkt[n++] = pm4_release_mem_dw1();
    pkt[n++] = pm4_release_mem_dw2(kPM4RMDataSel64, kPM4RMIntSelSendInt);
    pkt[n++] = static_cast<uint32_t>(cp.fence_gpu_addr & 0xFFFFFFFCu);
    pkt[n++] = static_cast<uint32_t>(cp.fence_gpu_addr >> 32);
    pkt[n++] = fence;       // fence_value lo
    pkt[n++] = 0;           // fence_value hi (we use 32-bit values for now)
    pkt[n++] = 0;           // pad

    cp_ring_write(cp, pkt, n);
    CP_LOG("emitted EOP fence value %u (wptr=%u, fence slot mc=%#llx)",
           fence, cp.wptr, (unsigned long long)cp.fence_gpu_addr);
    return fence;
}

//============================================================
// cp_config_rs64_caches — RS64 instruction / data cache bases
//============================================================
//
// Build 0.0.14 proved the PFP/ME now START (PC = byte 0x3000) and fault
// instantly at VA page 0: GCVM fault status 0x0d33, client 6 (CPG),
// walker + mapping error on a read, four IH fault IVs all at page 0, and
// the CP re-halts itself (CP_ME_CNTL 0x0100a000 -> 0x1500a000,
// CP_STAT 0x80021000 = CP_BUSY|ME_BUSY|ROQ_STATE_BUSY). A PC of byte
// 0x3000 that faults at page 0 says the instruction-cache BASE is 0 —
// the PSP did not program CP_PFP_IC_BASE / CP_ME_IC_BASE /
// CP_CPC_IC_BASE, nor the DC bases, in our flow.
//
// Linux programs these only on AMDGPU_FW_LOAD_DIRECT. The register
// sequence, its ordering and its waits below are taken verbatim from
// those loaders; only the *address* differs (their own VRAM bo vs. the
// PSP's TMR copy, whose MC address the ladder puts in cp.tmr_*).
//
// ADDRESS FORM — read out of the source, not assumed: Linux writes
//     WREG32(regCP_*_IC_BASE_LO, lower_32_bits(gpu_addr));
//     WREG32(regCP_*_IC_BASE_HI, upper_32_bits(gpu_addr));
// (gfx_v12_0.c:2443-2446, 2587-2590, 2924-2927), and the same plain
// split for the DC / MDBASE pairs (2492-2495, 2639-2642, 2919-2923).
// There is NO >> 8 and no >> 2 anywhere in this path — unlike
// CP_RB0_BASE, which does shift by 8.

// Poll one register field until it matches. Upstream uses
// usec_timeout = 50000 with udelay(1) in every one of these loops.
static bool
cp_poll_bit(const DeviceContext &dev, const char *what, uint32_t reg,
            uint32_t mask, uint32_t expected, uint32_t timeout_us)
{
    uint32_t v = RREG32(dev, reg);
    uint32_t waited = 0;
    while ((v & mask) != expected && waited < timeout_us) {
        IODelay(1);
        waited++;
        v = RREG32(dev, reg);
    }
    const bool ok = ((v & mask) == expected);
    CP_LOG("  %s: %s after %u us (dw %#x = %#010x, mask %#010x, want %#010x)",
           what, ok ? "ok" : "TIMEOUT", waited, reg, v, mask, expected);
    return ok;
}

// True when a LO/HI base pair reads all-zero, i.e. nobody programmed it.
static bool
cp_base_is_zero(const DeviceContext &dev, uint32_t lo_reg, uint32_t hi_reg)
{
    return RREG32(dev, lo_reg) == 0 && RREG32(dev, hi_reg) == 0;
}

// Shared PFP/ME instruction-cache programming. The CP_PFP_IC_* and
// CP_ME_IC_* field layouts are bit-identical (gc_12_0_0_sh_mask.h
// 19488-19504 vs 19512-19528), so the PFP macro names stand in for both;
// the register ADDRESSES are passed in, and they are what differ.
//
// THE :2455 COMMENT — upstream, immediately after the CNTL write:
//     "Programming any of the CP_PFP_IC_BASE registers forces
//      invalidation of the ME L1 I$. Wait for the invalidation complete"
// The invalidation is therefore a SIDE EFFECT of the base write, not
// something the driver triggers. What it requires of us: (a) do not
// assume the cache is usable straight after the write, (b) spin on
// CP_PFP_IC_OP_CNTL.INVALIDATE_CACHE_COMPLETE, and only then (c) set
// PRIME_ICACHE and spin on ICACHE_PRIMED. Both waits are implemented
// below, in that order, with upstream's 50 ms budget.
static bool
cp_program_ic_base(const DeviceContext &dev, const char *engine,
                   uint32_t lo_reg, uint32_t hi_reg, uint32_t cntl_reg,
                   uint32_t op_reg, uint64_t addr)
{
    CP_LOG("rs64_caches: %s IC base <- %#llx (lower/upper_32_bits, no shift)",
           engine, (unsigned long long)addr);

    // gfx_v12_0.c:2443-2446 / 2587-2590.
    cp_wreg_logged(dev, "IC_BASE_LO", lo_reg,
                   static_cast<uint32_t>(addr & 0xFFFFFFFFu));
    cp_wreg_logged(dev, "IC_BASE_HI", hi_reg,
                   static_cast<uint32_t>(addr >> 32));

    // gfx_v12_0.c:2448-2452 / 2592-2596 — RMW VMID=0, CACHE_POLICY=0,
    // EXE_DISABLE=0.
    {
        uint32_t tmp = RREG32(dev, cntl_reg);
        tmp = REG_SET_FIELD(tmp, CP_PFP_IC_BASE_CNTL, VMID, 0);
        tmp = REG_SET_FIELD(tmp, CP_PFP_IC_BASE_CNTL, CACHE_POLICY, 0);
        tmp = REG_SET_FIELD(tmp, CP_PFP_IC_BASE_CNTL, EXE_DISABLE, 0);
        cp_wreg_logged(dev, "IC_BASE_CNTL", cntl_reg, tmp);
    }

    // gfx_v12_0.c:2454-2469 / 2598-2613 — the :2455 comment's wait.
    if (!cp_poll_bit(dev, "IC invalidate complete", op_reg,
                     CP_PFP_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE_MASK,
                     CP_PFP_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE_MASK,
                     50000)) {
        CP_LOG("rs64_caches: %s instruction cache invalidation timed out",
               engine);
        return false;
    }

    // gfx_v12_0.c:2471-2489 / 2615-2632 — prime, then wait for primed.
    {
        uint32_t tmp = RREG32(dev, op_reg);
        tmp = REG_SET_FIELD(tmp, CP_PFP_IC_OP_CNTL, PRIME_ICACHE, 1);
        cp_wreg_logged(dev, "IC_OP_CNTL (PRIME_ICACHE)", op_reg, tmp);
    }
    if (!cp_poll_bit(dev, "IC primed", op_reg,
                     CP_PFP_IC_OP_CNTL__ICACHE_PRIMED_MASK,
                     CP_PFP_IC_OP_CNTL__ICACHE_PRIMED_MASK, 50000)) {
        CP_LOG("rs64_caches: %s instruction cache prime timed out", engine);
        return false;
    }
    CP_LOG("rs64_caches: %s IC base programmed, invalidated and primed",
           engine);
    return true;
}

// Shared PFP/ME data-cache programming. PFP owns CP_GFX_RS64_DC_BASE0,
// ME owns DC_BASE1 (gfx_v12_0.c:2492-2495 vs 2639-2642); both are
// per-pipe registers written under a GRBM pipe select, followed by the
// shared DC_BASE_CNTL and a DC_OP_CNTL invalidate.
static bool
cp_program_gfx_dc_base(const DeviceContext &dev, const char *engine,
                       uint32_t lo_reg, uint32_t hi_reg, uint64_t addr)
{
    CP_LOG("rs64_caches: %s DC base <- %#llx (both GFX pipes)",
           engine, (unsigned long long)addr);

    // gfx_v12_0.c:2490-2499 / 2636-2645 — num_pipe_per_me = 2.
    for (uint32_t pipe_id = 0; pipe_id < 2; pipe_id++) {
        cp_grbm_select(dev, /*me=*/0, pipe_id, 0, 0);
        cp_wreg_logged(dev, "CP_GFX_RS64_DC_BASE_LO", lo_reg,
                       static_cast<uint32_t>(addr & 0xFFFFFFFFu));
        cp_wreg_logged(dev, "CP_GFX_RS64_DC_BASE_HI", hi_reg,
                       static_cast<uint32_t>(addr >> 32));
    }
    cp_grbm_select(dev, 0, 0, 0, 0);

    // gfx_v12_0.c:2501-2505 / 2647-2651 — VMID=0, CACHE_POLICY=0.
    {
        const uint32_t cntl = CP_REG(dev, CP_GFX_RS64_DC_BASE_CNTL);
        uint32_t tmp = RREG32(dev, cntl);
        tmp = REG_SET_FIELD(tmp, CP_GFX_RS64_DC_BASE_CNTL, VMID, 0);
        tmp = REG_SET_FIELD(tmp, CP_GFX_RS64_DC_BASE_CNTL, CACHE_POLICY, 0);
        cp_wreg_logged(dev, "CP_GFX_RS64_DC_BASE_CNTL", cntl, tmp);
    }

    // Invalidate the data caches and wait.
    {
        const uint32_t op = CP_REG(dev, CP_GFX_RS64_DC_OP_CNTL);
        uint32_t tmp = RREG32(dev, op);
        tmp = REG_SET_FIELD(tmp, CP_GFX_RS64_DC_OP_CNTL,
                            INVALIDATE_DCACHE, 1);
        cp_wreg_logged(dev, "CP_GFX_RS64_DC_OP_CNTL (INVALIDATE_DCACHE)",
                       op, tmp);
        if (!cp_poll_bit(dev, "GFX RS64 DC invalidate complete", op,
                CP_GFX_RS64_DC_OP_CNTL__INVALIDATE_DCACHE_COMPLETE_MASK,
                CP_GFX_RS64_DC_OP_CNTL__INVALIDATE_DCACHE_COMPLETE_MASK,
                50000)) {
            CP_LOG("rs64_caches: %s data cache invalidation timed out",
                   engine);
            return false;
        }
    }
    return true;
}

// Survey-only pass: read and log every RS64 cache register, including the
// per-pipe ones under their GRBM select. Nothing here writes anything
// except GRBM_GFX_CNTL (a select register), restored to (0,0,0,0).
static void
cp_log_rs64_caches(const DeviceContext &dev, const char *when)
{
    CP_LOG("=== RS64 cache base survey (%s) ===", when);
    cp_rreg_logged(dev, "CP_PFP_IC_BASE_LO",   CP_REG(dev, CP_PFP_IC_BASE_LO));
    cp_rreg_logged(dev, "CP_PFP_IC_BASE_HI",   CP_REG(dev, CP_PFP_IC_BASE_HI));
    cp_rreg_logged(dev, "CP_PFP_IC_BASE_CNTL", CP_REG(dev, CP_PFP_IC_BASE_CNTL));
    cp_rreg_logged(dev, "CP_PFP_IC_OP_CNTL",   CP_REG(dev, CP_PFP_IC_OP_CNTL));
    cp_rreg_logged(dev, "CP_ME_IC_BASE_LO",    CP_REG(dev, CP_ME_IC_BASE_LO));
    cp_rreg_logged(dev, "CP_ME_IC_BASE_HI",    CP_REG(dev, CP_ME_IC_BASE_HI));
    cp_rreg_logged(dev, "CP_ME_IC_BASE_CNTL",  CP_REG(dev, CP_ME_IC_BASE_CNTL));
    cp_rreg_logged(dev, "CP_ME_IC_OP_CNTL",    CP_REG(dev, CP_ME_IC_OP_CNTL));
    cp_rreg_logged(dev, "CP_CPC_IC_BASE_CNTL", CP_REG(dev, CP_CPC_IC_BASE_CNTL));
    cp_rreg_logged(dev, "CP_CPC_IC_OP_CNTL",   CP_REG(dev, CP_CPC_IC_OP_CNTL));
    cp_rreg_logged(dev, "CP_GFX_RS64_DC_BASE_CNTL",
                   CP_REG(dev, CP_GFX_RS64_DC_BASE_CNTL));
    cp_rreg_logged(dev, "CP_GFX_RS64_DC_OP_CNTL",
                   CP_REG(dev, CP_GFX_RS64_DC_OP_CNTL));
    cp_rreg_logged(dev, "CP_MEC_DC_BASE_CNTL",
                   CP_REG(dev, CP_MEC_DC_BASE_CNTL));
    cp_rreg_logged(dev, "CP_MEC_DC_OP_CNTL",
                   CP_REG(dev, CP_MEC_DC_OP_CNTL));

    // Per-pipe: CP_GFX_RS64_DC_BASE0/1 under the GFX pipe select.
    for (uint32_t pipe_id = 0; pipe_id < 2; pipe_id++) {
        cp_grbm_select(dev, 0, pipe_id, 0, 0);
        CP_LOG("  [gfx pipe %u] DC_BASE0 = %08x_%08x  DC_BASE1 = %08x_%08x",
               pipe_id,
               RREG32(dev, CP_REG(dev, CP_GFX_RS64_DC_BASE0_HI)),
               RREG32(dev, CP_REG(dev, CP_GFX_RS64_DC_BASE0_LO)),
               RREG32(dev, CP_REG(dev, CP_GFX_RS64_DC_BASE1_HI)),
               RREG32(dev, CP_REG(dev, CP_GFX_RS64_DC_BASE1_LO)));
    }
    // Per-pipe: CP_MEC_MDBASE (== CP_MEC_DC_BASE) and CP_CPC_IC_BASE
    // under the MEC pipe select.
    for (uint32_t pipe_id = 0; pipe_id < 4; pipe_id++) {
        cp_grbm_select(dev, 1, pipe_id, 0, 0);
        CP_LOG("  [mec pipe %u] MDBASE = %08x_%08x  CPC_IC_BASE = %08x_%08x",
               pipe_id,
               RREG32(dev, CP_REG(dev, CP_MEC_MDBASE_HI)),
               RREG32(dev, CP_REG(dev, CP_MEC_MDBASE_LO)),
               RREG32(dev, CP_REG(dev, CP_CPC_IC_BASE_HI)),
               RREG32(dev, CP_REG(dev, CP_CPC_IC_BASE_LO)));
    }
    cp_grbm_select(dev, 0, 0, 0, 0);
    CP_LOG("=== end RS64 cache base survey (%s) ===", when);
}

kern_return_t
cp_config_rs64_caches(DeviceContext &dev, CPContext &cp)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    // Every register in this function is BASE_IDX 1, plus GRBM_GFX_CNTL.
    if (!dev.ip.isResolved(IPBlock::GC, CPRegBaseIdx::CP_PFP_IC_BASE_LO) ||
        !dev.ip.isResolved(IPBlock::GC, GFXRegBaseIdx::GRBM_GFX_CNTL)) {
        CP_LOG("rs64_caches: GC BASE_IDX 1 unresolved — refusing");
        return kIOReturnNotReady;
    }

    CP_LOG("rs64_caches: TMR addresses from the ladder — "
           "pfp ic=%#llx dc=%#llx | me ic=%#llx dc=%#llx | "
           "mec ic=%#llx dc0=%#llx dc1=%#llx",
           (unsigned long long)cp.tmr_pfp_ic,  (unsigned long long)cp.tmr_pfp_dc,
           (unsigned long long)cp.tmr_me_ic,   (unsigned long long)cp.tmr_me_dc,
           (unsigned long long)cp.tmr_mec_ic,  (unsigned long long)cp.tmr_mec_dc0,
           (unsigned long long)cp.tmr_mec_dc1);

    cp_log_rs64_caches(dev, "before");

    bool ok = true;

    // ---------------- PFP ----------------
    {
        const uint32_t lo = CP_REG(dev, CP_PFP_IC_BASE_LO);
        const uint32_t hi = CP_REG(dev, CP_PFP_IC_BASE_HI);
        if (!cp_base_is_zero(dev, lo, hi)) {
            CP_LOG("rs64_caches: PFP IC base already nonzero (%08x_%08x) — "
                   "leaving it alone", RREG32(dev, hi), RREG32(dev, lo));
        } else if (cp.tmr_pfp_ic == 0) {
            CP_LOG("rs64_caches: PFP IC base is 0 and no TMR address was "
                   "supplied (PSP fw_type 87) — the PFP would fetch at VA 0");
            ok = false;
        } else {
            ok &= cp_program_ic_base(dev, "PFP", lo, hi,
                                     CP_REG(dev, CP_PFP_IC_BASE_CNTL),
                                     CP_REG(dev, CP_PFP_IC_OP_CNTL),
                                     cp.tmr_pfp_ic);
        }
    }
    {
        // DC_BASE0 is per-pipe; inspect it under pipe 0's select.
        cp_grbm_select(dev, 0, 0, 0, 0);
        const uint32_t lo = CP_REG(dev, CP_GFX_RS64_DC_BASE0_LO);
        const uint32_t hi = CP_REG(dev, CP_GFX_RS64_DC_BASE0_HI);
        if (!cp_base_is_zero(dev, lo, hi))
            CP_LOG("rs64_caches: PFP DC_BASE0 already nonzero (%08x_%08x) — "
                   "leaving it alone", RREG32(dev, hi), RREG32(dev, lo));
        else if (cp.tmr_pfp_dc == 0)
            CP_LOG("rs64_caches: PFP DC_BASE0 is 0 and no TMR stack address "
                   "was supplied (PSP fw_type 90)");
        else
            ok &= cp_program_gfx_dc_base(dev, "PFP", lo, hi, cp.tmr_pfp_dc);
    }

    // ---------------- ME ----------------
    {
        const uint32_t lo = CP_REG(dev, CP_ME_IC_BASE_LO);
        const uint32_t hi = CP_REG(dev, CP_ME_IC_BASE_HI);
        if (!cp_base_is_zero(dev, lo, hi)) {
            CP_LOG("rs64_caches: ME IC base already nonzero (%08x_%08x) — "
                   "leaving it alone", RREG32(dev, hi), RREG32(dev, lo));
        } else if (cp.tmr_me_ic == 0) {
            CP_LOG("rs64_caches: ME IC base is 0 and no TMR address was "
                   "supplied (PSP fw_type 88) — the ME would fetch at VA 0");
            ok = false;
        } else {
            ok &= cp_program_ic_base(dev, "ME", lo, hi,
                                     CP_REG(dev, CP_ME_IC_BASE_CNTL),
                                     CP_REG(dev, CP_ME_IC_OP_CNTL),
                                     cp.tmr_me_ic);
        }
    }
    {
        cp_grbm_select(dev, 0, 0, 0, 0);
        const uint32_t lo = CP_REG(dev, CP_GFX_RS64_DC_BASE1_LO);
        const uint32_t hi = CP_REG(dev, CP_GFX_RS64_DC_BASE1_HI);
        if (!cp_base_is_zero(dev, lo, hi))
            CP_LOG("rs64_caches: ME DC_BASE1 already nonzero (%08x_%08x) — "
                   "leaving it alone", RREG32(dev, hi), RREG32(dev, lo));
        else if (cp.tmr_me_dc == 0)
            CP_LOG("rs64_caches: ME DC_BASE1 is 0 and no TMR stack address "
                   "was supplied (PSP fw_type 92)");
        else
            ok &= cp_program_gfx_dc_base(dev, "ME", lo, hi, cp.tmr_me_dc);
    }

    // ---------------- MEC / CPC ----------------
    // gfx_v12_0_cp_compute_load_microcode_rs64, gfx_v12_0.c:2902-2968.
    // Structurally different from PFP/ME: both CNTLs are written FIRST,
    // then the per-pipe MDBASE + CPC_IC_BASE pairs, then the invalidates
    // last — DC first, then IC, both explicit. There is no prime step.
    {
        cp_grbm_select(dev, 1, 0, 0, 0);
        const bool ic_zero = cp_base_is_zero(dev,
                                             CP_REG(dev, CP_CPC_IC_BASE_LO),
                                             CP_REG(dev, CP_CPC_IC_BASE_HI));
        cp_grbm_select(dev, 0, 0, 0, 0);

        if (!ic_zero) {
            CP_LOG("rs64_caches: CPC IC base already nonzero — leaving the "
                   "whole MEC cache block alone");
        } else if (cp.tmr_mec_ic == 0) {
            CP_LOG("rs64_caches: CPC IC base is 0 and no TMR address was "
                   "supplied (PSP fw_type 89) — MEC pipes will fault");
        } else {
            // :2902-2906 — CP_CPC_IC_BASE_CNTL VMID=0 EXE_DISABLE=0
            // CACHE_POLICY=0.
            {
                const uint32_t cntl = CP_REG(dev, CP_CPC_IC_BASE_CNTL);
                uint32_t tmp = RREG32(dev, cntl);
                tmp = REG_SET_FIELD(tmp, CP_CPC_IC_BASE_CNTL, VMID, 0);
                tmp = REG_SET_FIELD(tmp, CP_CPC_IC_BASE_CNTL, EXE_DISABLE, 0);
                tmp = REG_SET_FIELD(tmp, CP_CPC_IC_BASE_CNTL, CACHE_POLICY, 0);
                cp_wreg_logged(dev, "CP_CPC_IC_BASE_CNTL", cntl, tmp);
            }
            // :2908-2911 — CP_MEC_DC_BASE_CNTL VMID=0 CACHE_POLICY=0.
            {
                const uint32_t cntl = CP_REG(dev, CP_MEC_DC_BASE_CNTL);
                uint32_t tmp = RREG32(dev, cntl);
                tmp = REG_SET_FIELD(tmp, CP_MEC_DC_BASE_CNTL, VMID, 0);
                tmp = REG_SET_FIELD(tmp, CP_MEC_DC_BASE_CNTL, CACHE_POLICY, 0);
                cp_wreg_logged(dev, "CP_MEC_DC_BASE_CNTL", cntl, tmp);
            }

            // :2913-2930 — per pipe: MDBASE = that pipe's stack,
            // CPC_IC_BASE = the single ucode image.
            //
            // Upstream derives pipe i's stack as
            //   mec_fw_data_gpu_addr + i * ALIGN(fw_data_size, 64 KiB)
            // because it laid the four copies out itself. Here the PSP
            // owns the layout and reports one address per fw_type; today
            // only P0 (94) and P1 (95) are loaded, so pipes 2 and 3 have
            // no stack address. Rather than extrapolate a stride into
            // memory the PSP may not have mapped, those pipes' MDBASE is
            // left as-is and logged. If MEC pipes 2/3 are ever used, PSP
            // fw_types 96/97 (MEC_P2_STACK / P3_STACK) must be loaded and
            // plumbed into CPContext.
            const uint64_t stack[4] = {
                cp.tmr_mec_dc0, cp.tmr_mec_dc1, 0, 0
            };
            for (uint32_t pipe_id = 0; pipe_id < 4; pipe_id++) {
                cp_grbm_select(dev, /*me=*/1, pipe_id, 0, 0);
                if (stack[pipe_id] != 0) {
                    cp_wreg_logged(dev, "CP_MEC_MDBASE_LO",
                                   CP_REG(dev, CP_MEC_MDBASE_LO),
                                   static_cast<uint32_t>(
                                       stack[pipe_id] & 0xFFFFFFFFu));
                    cp_wreg_logged(dev, "CP_MEC_MDBASE_HI",
                                   CP_REG(dev, CP_MEC_MDBASE_HI),
                                   static_cast<uint32_t>(
                                       stack[pipe_id] >> 32));
                } else {
                    CP_LOG("  [mec pipe %u] no TMR stack address (PSP "
                           "fw_type %u not loaded) — MDBASE left at "
                           "%08x_%08x", pipe_id, 94u + pipe_id,
                           RREG32(dev, CP_REG(dev, CP_MEC_MDBASE_HI)),
                           RREG32(dev, CP_REG(dev, CP_MEC_MDBASE_LO)));
                }
                cp_wreg_logged(dev, "CP_CPC_IC_BASE_LO",
                               CP_REG(dev, CP_CPC_IC_BASE_LO),
                               static_cast<uint32_t>(
                                   cp.tmr_mec_ic & 0xFFFFFFFFu));
                cp_wreg_logged(dev, "CP_CPC_IC_BASE_HI",
                               CP_REG(dev, CP_CPC_IC_BASE_HI),
                               static_cast<uint32_t>(cp.tmr_mec_ic >> 32));
            }
            cp_grbm_select(dev, 0, 0, 0, 0);   // :2931

            // :2933-2950 — invalidate the MEC data cache, wait.
            {
                const uint32_t op = CP_REG(dev, CP_MEC_DC_OP_CNTL);
                uint32_t tmp = RREG32(dev, op);
                tmp = REG_SET_FIELD(tmp, CP_MEC_DC_OP_CNTL,
                                    INVALIDATE_DCACHE, 1);
                cp_wreg_logged(dev, "CP_MEC_DC_OP_CNTL (INVALIDATE_DCACHE)",
                               op, tmp);
                ok &= cp_poll_bit(dev, "MEC DC invalidate complete", op,
                        CP_MEC_DC_OP_CNTL__INVALIDATE_DCACHE_COMPLETE_MASK,
                        CP_MEC_DC_OP_CNTL__INVALIDATE_DCACHE_COMPLETE_MASK,
                        50000);
            }
            // :2952-2968 — invalidate the CPC instruction cache, wait.
            // This one is an explicit INVALIDATE_CACHE write; the PFP/ME
            // path instead relies on the base write triggering it.
            {
                const uint32_t op = CP_REG(dev, CP_CPC_IC_OP_CNTL);
                uint32_t tmp = RREG32(dev, op);
                tmp = REG_SET_FIELD(tmp, CP_CPC_IC_OP_CNTL,
                                    INVALIDATE_CACHE, 1);
                cp_wreg_logged(dev, "CP_CPC_IC_OP_CNTL (INVALIDATE_CACHE)",
                               op, tmp);
                ok &= cp_poll_bit(dev, "CPC IC invalidate complete", op,
                        CP_CPC_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE_MASK,
                        CP_CPC_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE_MASK,
                        50000);
            }
        }
    }

    cp_log_rs64_caches(dev, "after");
    cp.caches_configured = true;

    if (!ok) {
        CP_LOG("rs64_caches: FAILED — at least one required cache base is "
               "still unusable; unhalting the CP now would repeat the "
               "0.0.14 VA-0 CPG fault");
        return kIOReturnIOError;
    }
    CP_LOG("rs64_caches: all required RS64 cache bases are in place");
    return kIOReturnSuccess;
}

// ----- cp_config_gfx_rs64: RS64 entry points + pipe resets -----
//
// Faithful port of gfx_v12_0_config_gfx_rs64 (gfx_v12_0.c:2142), which
// gfx_v12_0_hw_init calls at line 3814 under
//     if (adev->firmware.load_type == AMDGPU_FW_LOAD_PSP)
// i.e. exactly our situation: the PSP already loaded RS64_PFP(87)/
// _PFP_P0(90)/_ME(88)/_ME_P0(92)/_MEC(89)/_MEC_P0(94)/_MEC_P1(95) and
// placed each image in the TMR itself. What the PSP does NOT do is tell
// the RS64 cores where their entry point is — that is the driver's job,
// and it is the only thing between "firmware is in memory" and "the CP
// executes it".
//
// NOT programmed here, deliberately: CP_PFP_IC_BASE_{LO,HI,CNTL},
// CP_ME_IC_BASE_*, CP_CPC_IC_BASE_*. Linux writes those only inside the
// AMDGPU_FW_LOAD_DIRECT microcode loaders (gfx_v12_0.c:2443-2452 for PFP,
// 2587-2596 for ME, 2902-2930 for MEC), where they point at the driver's
// own GPU-resident copy (adev->gfx.pfp.pfp_fw_gpu_addr & friends). On the
// PSP path there is no driver copy and config_gfx_rs64 touches none of
// them — so neither do we, and the PSP's per-image tmr_fw_addr is not
// needed. (PSPContext does not record it anyway; psp_v14_0.cpp:849 only
// logs it.)
//
// The program-counter value is the firmware header's own:
//     PRGRM_CNTR_START    = (ucode_start_addr_hi << 30) |
//                           (ucode_start_addr_lo >> 2)
//     PRGRM_CNTR_START_HI = ucode_start_addr_hi >> 2
// (gfx_v12_0.c:2159-2163, 2181-2185, 2203-2207 — note the MEC pair spells
// the same expression with the operands swapped, which is identical.)
// The `<< 30` is a 32-bit shift in upstream, i.e. only the low two bits of
// ucode_start_addr_hi survive into the low register; we reproduce that
// truncation exactly rather than "fixing" it.
//
// For gc_12_0_1 the shipped headers all carry
// ucode_start_addr_lo = 0x00003000, ucode_start_addr_hi = 0x00070000
//   -> PRGRM_CNTR_START = 0x00000c00, PRGRM_CNTR_START_HI = 0x0001c000
// so a log line showing 0x00000000 after the write means the write did
// not land (wrong segment / pipe not selected), not that the header is
// empty.
static bool
cp_rs64_start_addr(FwId id, const char *what,
                   uint32_t *out_lo, uint32_t *out_hi)
{
    *out_lo = 0; *out_hi = 0;
    const FwBlob *b = fw_get(id);
    if (b == nullptr || b->data == nullptr) {
        CP_LOG("config_gfx_rs64: %s firmware blob missing", what);
        return false;
    }
    if (b->size < sizeof(gfx_firmware_header_v2_0)) {
        CP_LOG("config_gfx_rs64: %s blob too small (%llu bytes)",
               what, (unsigned long long)b->size);
        return false;
    }
    const gfx_firmware_header_v2_0 *h =
        reinterpret_cast<const gfx_firmware_header_v2_0 *>(b->data);
    const uint16_t maj = h->header.header_version_major;
    const uint16_t min = h->header.header_version_minor;
    if (maj != 2) {
        CP_LOG("config_gfx_rs64: %s is header v%u.%u, not the v2.0 RS64 "
               "layout — refusing to read ucode_start_addr",
               what, maj, min);
        return false;
    }
    *out_lo = h->ucode_start_addr_lo;
    *out_hi = h->ucode_start_addr_hi;
    CP_LOG("config_gfx_rs64: %s (%s, %llu bytes, hdr v%u.%u, ucode_ver=%#x): "
           "ucode_start_addr lo=%#010x hi=%#010x",
           what, b->name, (unsigned long long)b->size, maj, min,
           h->header.ucode_version, *out_lo, *out_hi);
    return true;
}

kern_return_t
cp_config_gfx_rs64(DeviceContext &dev, CPContext &cp)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    // The MEC half of this sequence lives in GC[1] (CP_MEC_RS64_CNTL and
    // CP_MEC_RS64_PRGRM_CNTR_START(_HI)), as does GRBM_GFX_CNTL. Writing
    // into an unresolved segment would silently poke unrelated registers —
    // the exact failure mode CP_ME_CNTL already cost us once.
    if (!dev.ip.isResolved(IPBlock::GC, CPRegBaseIdx::CP_ME_CNTL) ||
        !dev.ip.isResolved(IPBlock::GC, CPRegBaseIdx::CP_MEC_RS64_CNTL) ||
        !dev.ip.isResolved(IPBlock::GC, GFXRegBaseIdx::GRBM_GFX_CNTL)) {
        CP_LOG("config_gfx_rs64: GC BASE_IDX 1 unresolved — refusing");
        return kIOReturnNotReady;
    }

    uint32_t pfp_lo = 0, pfp_hi = 0, me_lo = 0, me_hi = 0,
             mec_lo = 0, mec_hi = 0;
    const bool have_pfp = cp_rs64_start_addr(FwId::GC_PFP, "PFP",
                                             &pfp_lo, &pfp_hi);
    const bool have_me  = cp_rs64_start_addr(FwId::GC_ME,  "ME",
                                             &me_lo,  &me_hi);
    const bool have_mec = cp_rs64_start_addr(FwId::GC_MEC, "MEC",
                                             &mec_lo, &mec_hi);
    if (!have_pfp || !have_me || !have_mec) {
        CP_LOG("config_gfx_rs64: missing RS64 firmware headers "
               "(pfp=%d me=%d mec=%d) — the CP would start at PC 0",
               have_pfp, have_me, have_mec);
        return kIOReturnNotFound;
    }

    cp.pfp_pc    = (pfp_hi << 30) | (pfp_lo >> 2);
    cp.pfp_pc_hi = pfp_hi >> 2;
    cp.me_pc     = (me_hi  << 30) | (me_lo  >> 2);
    cp.me_pc_hi  = me_hi  >> 2;
    cp.mec_pc    = (mec_lo >> 2)  | (mec_hi << 30);
    cp.mec_pc_hi = mec_hi >> 2;

    CP_LOG("config_gfx_rs64: start — GC base[0]=%#x base[1]=%#x; "
           "PFP pc=%#010x/%#010x ME pc=%#010x/%#010x MEC pc=%#010x/%#010x",
           dev.ip.getBase(IPBlock::GC, 0), dev.ip.getBase(IPBlock::GC, 1),
           cp.pfp_pc, cp.pfp_pc_hi, cp.me_pc, cp.me_pc_hi,
           cp.mec_pc, cp.mec_pc_hi);
    cp_rreg_logged(dev, "CP_ME_CNTL (before rs64 config)",
                   CP_REG(dev, CP_ME_CNTL));
    cp_rreg_logged(dev, "CP_MEC_RS64_CNTL (before rs64 config)",
                   CP_REG(dev, CP_MEC_RS64_CNTL));

    // ---- gfx_v12_0.c:2156-2165 — PFP program start addr, both pipes ----
    // adev->gfx.me.num_pipe_per_me is 2 on gfx12; config_gfx_rs64 hardcodes
    // the bound as `pipe_id < 2`.
    for (uint32_t pipe_id = 0; pipe_id < 2; pipe_id++) {
        cp_grbm_select(dev, /*me=*/0, pipe_id, /*queue=*/0, /*vmid=*/0);
        cp_wreg_logged(dev, "CP_PFP_PRGRM_CNTR_START",
                       CP_REG(dev, CP_PFP_PRGRM_CNTR_START), cp.pfp_pc);
        cp_wreg_logged(dev, "CP_PFP_PRGRM_CNTR_START_HI",
                       CP_REG(dev, CP_PFP_PRGRM_CNTR_START_HI), cp.pfp_pc_hi);
    }
    cp_grbm_select(dev, 0, 0, 0, 0);   // gfx_v12_0.c:2165

    // ---- gfx_v12_0.c:2168-2177 — pulse PFP_PIPE{0,1}_RESET ----
    // Upstream reads CP_ME_CNTL once, sets both reset bits, writes, then
    // clears both bits on the SAME `tmp` and writes again (no re-read).
    {
        uint32_t tmp = RREG32(dev, CP_REG(dev, CP_ME_CNTL));
        tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_PIPE0_RESET, 1);
        tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_PIPE1_RESET, 1);
        cp_wreg_logged(dev, "CP_ME_CNTL (PFP pipe reset assert)",
                       CP_REG(dev, CP_ME_CNTL), tmp);
        tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_PIPE0_RESET, 0);
        tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_PIPE1_RESET, 0);
        cp_wreg_logged(dev, "CP_ME_CNTL (PFP pipe reset clear)",
                       CP_REG(dev, CP_ME_CNTL), tmp);
    }

    // ---- gfx_v12_0.c:2178-2187 — ME program start addr, both pipes ----
    for (uint32_t pipe_id = 0; pipe_id < 2; pipe_id++) {
        cp_grbm_select(dev, /*me=*/0, pipe_id, 0, 0);
        cp_wreg_logged(dev, "CP_ME_PRGRM_CNTR_START",
                       CP_REG(dev, CP_ME_PRGRM_CNTR_START), cp.me_pc);
        cp_wreg_logged(dev, "CP_ME_PRGRM_CNTR_START_HI",
                       CP_REG(dev, CP_ME_PRGRM_CNTR_START_HI), cp.me_pc_hi);
    }
    cp_grbm_select(dev, 0, 0, 0, 0);   // gfx_v12_0.c:2187

    // ---- gfx_v12_0.c:2190-2199 — pulse ME_PIPE{0,1}_RESET ----
    {
        uint32_t tmp = RREG32(dev, CP_REG(dev, CP_ME_CNTL));
        tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_PIPE0_RESET, 1);
        tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_PIPE1_RESET, 1);
        cp_wreg_logged(dev, "CP_ME_CNTL (ME pipe reset assert)",
                       CP_REG(dev, CP_ME_CNTL), tmp);
        tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_PIPE0_RESET, 0);
        tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_PIPE1_RESET, 0);
        cp_wreg_logged(dev, "CP_ME_CNTL (ME pipe reset clear)",
                       CP_REG(dev, CP_ME_CNTL), tmp);
    }

    // ---- gfx_v12_0.c:2201-2209 — MEC program start addr, 4 pipes ----
    // me = 1 selects the MEC (compute) front-end; the loop bound is
    // hardcoded `pipe_id < 4` in config_gfx_rs64.
    for (uint32_t pipe_id = 0; pipe_id < 4; pipe_id++) {
        cp_grbm_select(dev, /*me=*/1, pipe_id, 0, 0);
        cp_wreg_logged(dev, "CP_MEC_RS64_PRGRM_CNTR_START",
                       CP_REG(dev, CP_MEC_RS64_PRGRM_CNTR_START), cp.mec_pc);
        cp_wreg_logged(dev, "CP_MEC_RS64_PRGRM_CNTR_START_HI",
                       CP_REG(dev, CP_MEC_RS64_PRGRM_CNTR_START_HI),
                       cp.mec_pc_hi);
    }
    cp_grbm_select(dev, 0, 0, 0, 0);   // gfx_v12_0.c:2209

    // ---- gfx_v12_0.c:2212-2226 — pulse MEC_PIPE{0..3}_RESET ----
    {
        uint32_t tmp = RREG32(dev, CP_REG(dev, CP_MEC_RS64_CNTL));
        tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE0_RESET, 1);
        tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE1_RESET, 1);
        tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE2_RESET, 1);
        tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE3_RESET, 1);
        cp_wreg_logged(dev, "CP_MEC_RS64_CNTL (MEC pipe reset assert)",
                       CP_REG(dev, CP_MEC_RS64_CNTL), tmp);
        tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE0_RESET, 0);
        tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE1_RESET, 0);
        tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE2_RESET, 0);
        tmp = REG_SET_FIELD(tmp, CP_MEC_RS64_CNTL, MEC_PIPE3_RESET, 0);
        cp_wreg_logged(dev, "CP_MEC_RS64_CNTL (MEC pipe reset clear)",
                       CP_REG(dev, CP_MEC_RS64_CNTL), tmp);
    }

    cp.rs64_configured = true;
    CP_LOG("config_gfx_rs64: done — PFP/ME/MEC program counters programmed "
           "and all pipes pulsed");
    cp_dump_state(dev, cp, "after cp_config_gfx_rs64");
    return kIOReturnSuccess;
}

// ----- cp_hqd_program: write CP_RB0_* registers -----
//
// Mirrors gfx_v12_0_cp_gfx_resume (gfx_v12_0.c:2715) line-by-line —
// the writes happen in upstream's exact order. Audit-7 #3 added the
// missing fields: CP_RB_WPTR_DELAY, CP_RB0_WPTR_HI,
// CP_RB_WPTR_POLL_ADDR_{LO,HI}, CP_RB_ACTIVE=1, CP_MAX_CONTEXT,
// CP_DEVICE_ID=1, and the per-pipe GRBM_GFX_CNTL select that picks
// PIPE_ID0 (cp_gfx_switch_pipe). The RB_BUFSZ encoding also follows
// upstream: log2(ring_size_bytes/8), with RB_BLKSZ = BUFSZ - 2.
kern_return_t
cp_hqd_program(const DeviceContext &dev, CPContext &cp)
{
    if (!cp.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    CP_LOG("cp_hqd_program: GC base[0]=%#x base[1]=%#x ring_mc=%#llx "
           "rptr_mc=%#llx wptr_mc=%#llx doorbell_index=%u",
           dev.ip.getBase(IPBlock::GC, 0), dev.ip.getBase(IPBlock::GC, 1),
           (unsigned long long)cp.ring_bus,
           (unsigned long long)cp.rptr_gpu_addr,
           (unsigned long long)cp.wptr_gpu_addr, cp.doorbell_index);

    // gfx_v12_0.c:2723 — CP_RB_WPTR_DELAY = 0.
    cp_wreg_logged(dev, "CP_RB_WPTR_DELAY", CP_REG(dev, CP_RB_WPTR_DELAY), 0);

    // gfx_v12_0.c:2726 — CP_RB_VMID = 0.
    cp_wreg_logged(dev, "CP_RB_VMID", CP_REG(dev, CP_RB_VMID), 0);

    // gfx_v12_0.c:2730 — cp_gfx_switch_pipe(adev, PIPE_ID0): write
    // GRBM_GFX_CNTL.PIPEID = 0. PIPEID field is bits [1:0]; we only
    // change PIPEID and leave other fields at their current values.
    // (The reference sourced the GRBM_GFX_CNTL field defs from
    // amdgpu_mes.h; in this tree they live in amdgpu_gfx.h — same
    // single canonical layout for the GRBM select register.)
    {
        const uint32_t grbm_reg = GFX_REG(dev, GRBM_GFX_CNTL);
        uint32_t v = RREG32(dev, grbm_reg);
        v = REG_SET_FIELD(v, GRBM_GFX_CNTL, PIPEID, 0);
        cp_wreg_logged(dev, "GRBM_GFX_CNTL (switch_pipe PIPE_ID0)",
                       grbm_reg, v);
    }

    // gfx_v12_0.c:2734-2737 — RB_BUFSZ = order_base_2(ring_size/8);
    // RB_BLKSZ = BUFSZ - 2. ring_size is in BYTES upstream; ours
    // tracked in dwords, so ring_size_bytes = ring_size_dwords * 4.
    // order_base_2(N) = ceil(log2(N)).
    auto order_base_2 = [](uint32_t x) -> uint32_t {
        uint32_t r = 0;
        while ((1u << r) < x) r++;
        return r;
    };
    const uint32_t rb_bufsz = order_base_2(cp.ring_size_dwords * 4u / 8u);
    {
        uint32_t tmp = 0;
        tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, RB_BUFSZ, rb_bufsz);
        tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, RB_BLKSZ,
                            (rb_bufsz >= 2) ? (rb_bufsz - 2) : 0);
        cp_wreg_logged(dev, "CP_RB0_CNTL (1st)",
                       CP_REG(dev, CP_RB0_CNTL), tmp);
    }

    // gfx_v12_0.c:2741-2742 — initialize wptr lo + hi to 0.
    cp.wptr = 0;
    *cp.wptr_cpu = 0;
    *cp.rptr_cpu = 0;
    *cp.fence_cpu = 0;
    sysmem_wmb();
    cp_wreg_logged(dev, "CP_RB0_WPTR",    CP_REG(dev, CP_RB0_WPTR),    0);
    cp_wreg_logged(dev, "CP_RB0_WPTR_HI", CP_REG(dev, CP_RB0_WPTR_HI), 0);

    // gfx_v12_0.c:2746-2748 — RPTR write-back address. The HI write
    // upstream masks to RB_RPTR_ADDR_HI_MASK.
    cp_wreg_logged(dev, "CP_RB0_RPTR_ADDR",
                   CP_REG(dev, CP_RB0_RPTR_ADDR),
                   static_cast<uint32_t>(cp.rptr_gpu_addr & 0xFFFFFFFCu));
    cp_wreg_logged(dev, "CP_RB0_RPTR_ADDR_HI",
                   CP_REG(dev, CP_RB0_RPTR_ADDR_HI),
                   static_cast<uint32_t>(cp.rptr_gpu_addr >> 32) &
                   CP_RB_RPTR_ADDR_HI__RB_RPTR_ADDR_HI_MASK);

    // gfx_v12_0.c:2751-2754 — WPTR poll address pair. Required for
    // wptr-poll-driven CP rings; missing in our previous implementation.
    // Audit-7 #3.
    cp_wreg_logged(dev, "CP_RB_WPTR_POLL_ADDR_LO",
                   CP_REG(dev, CP_RB_WPTR_POLL_ADDR_LO),
                   static_cast<uint32_t>(cp.wptr_gpu_addr));
    cp_wreg_logged(dev, "CP_RB_WPTR_POLL_ADDR_HI",
                   CP_REG(dev, CP_RB_WPTR_POLL_ADDR_HI),
                   static_cast<uint32_t>(cp.wptr_gpu_addr >> 32));

    // gfx_v12_0.c:2756 — mdelay(1) before re-writing CP_RB0_CNTL.
    // Upstream literally writes the same value twice with a 1 ms gap
    // — there's a CP-internal latch that requires the redundant write.
    IOSleep(1);
    {
        uint32_t tmp = 0;
        tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, RB_BUFSZ, rb_bufsz);
        tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, RB_BLKSZ,
                            (rb_bufsz >= 2) ? (rb_bufsz - 2) : 0);
        cp_wreg_logged(dev, "CP_RB0_CNTL (2nd, after mdelay(1))",
                       CP_REG(dev, CP_RB0_CNTL), tmp);
    }

    // gfx_v12_0.c:2759-2761 — ring base, low + high.
    {
        const uint64_t rb_addr = cp.ring_bus >> 8;
        cp_wreg_logged(dev, "CP_RB0_BASE", CP_REG(dev, CP_RB0_BASE),
                       static_cast<uint32_t>(rb_addr));
        cp_wreg_logged(dev, "CP_RB0_BASE_HI", CP_REG(dev, CP_RB0_BASE_HI),
                       static_cast<uint32_t>(rb_addr >> 32));
    }

    // gfx_v12_0.c:2763 — CP_RB_ACTIVE = 1.  Audit-7 #3.
    cp_wreg_logged(dev, "CP_RB_ACTIVE", CP_REG(dev, CP_RB_ACTIVE), 1);

    // gfx_v12_0.c:2690-2712 — cp_gfx_set_doorbell. Doorbell control +
    // range registers. Mirrors upstream's REG_SET_FIELD pattern.
    {
        const uint32_t db_reg = CP_REG(dev, CP_RB_DOORBELL_CONTROL);
        uint32_t v = RREG32(dev, db_reg);
        v = REG_SET_FIELD(v, CP_RB_DOORBELL_CONTROL, DOORBELL_OFFSET,
                          cp.doorbell_index);
        v = REG_SET_FIELD(v, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 1);
        cp_wreg_logged(dev, "CP_RB_DOORBELL_CONTROL", db_reg, v);

        uint32_t lower = 0;
        lower = REG_SET_FIELD(lower, CP_RB_DOORBELL_RANGE_LOWER,
                              DOORBELL_RANGE_LOWER, cp.doorbell_index);
        cp_wreg_logged(dev, "CP_RB_DOORBELL_RANGE_LOWER (set_doorbell)",
                       CP_REG(dev, CP_RB_DOORBELL_RANGE_LOWER), lower);
        // Upstream writes the full mask to RANGE_UPPER — accept any
        // doorbell index in our window.
        cp_wreg_logged(dev, "CP_RB_DOORBELL_RANGE_UPPER (set_doorbell)",
                       CP_REG(dev, CP_RB_DOORBELL_RANGE_UPPER),
                       CP_RB_DOORBELL_RANGE_UPPER__DOORBELL_RANGE_UPPER_MASK);
    }

    // gfx_v12_0.c:2770 — switch to PIPE_ID0 (second switch — the
    // start/stop pattern; upstream brackets cp_gfx_start with two
    // switches even though they're idempotent for PIPE_ID0).
    {
        const uint32_t grbm_reg = GFX_REG(dev, GRBM_GFX_CNTL);
        uint32_t v = RREG32(dev, grbm_reg);
        v = REG_SET_FIELD(v, GRBM_GFX_CNTL, PIPEID, 0);
        cp_wreg_logged(dev, "GRBM_GFX_CNTL (switch_pipe PIPE_ID0, 2nd)",
                       grbm_reg, v);
    }

    // gfx_v12_0.c:2805 — cp_gfx_resume's last act is cp_gfx_start(adev).
    // CP_MAX_CONTEXT / CP_DEVICE_ID / the CP unhalt used to live here;
    // they are now in cp_gfx_start(), where upstream keeps them, so
    // cp_init_full can run them after cp_compute_enable(true) exactly as
    // gfx_v12_0_cp_resume does.

    CP_LOG("HQD programmed: ring_mc=%#llx bufsz=%u blksz=%u doorbell=%u "
           "(CP_RB0_RPTR=%#010x — still halted, expected 0)",
           (unsigned long long)cp.ring_bus, rb_bufsz,
           (rb_bufsz >= 2) ? (rb_bufsz - 2) : 0, cp.doorbell_index,
           RREG32(dev, CP_REG(dev, CP_RB0_RPTR)));
    return kIOReturnSuccess;
}

// ----- cp_gfx_start: gfx_v12_0_cp_gfx_start (gfx_v12_0.c:2699) -----
//
// IMPORTANT, and contrary to what a GFX11 reading suggests: on GFX12 this
// function submits NO packets. The whole body upstream is
//
//     WREG32_SOC15(GC, 0, regCP_MAX_CONTEXT,
//                  adev->gfx.config.max_hw_contexts - 1);   // :2702
//     WREG32_SOC15(GC, 0, regCP_DEVICE_ID, 1);              // :2704
//     if (!amdgpu_async_gfx_ring)
//             gfx_v12_0_cp_gfx_enable(adev, true);          // :2707
//     return 0;
//
// GFX11's cp_gfx_start (gfx_v11_0.c:3677) is the one that builds the
// clear-state preamble — PACKET3_PREAMBLE_CNTL (0x4A) with
// PREAMBLE_BEGIN_CLEAR_STATE, PACKET3_CONTEXT_CONTROL (0x28) with
// 0x80000000/0x80000000, a SET_CONTEXT_REG (0x69) walk over gfx11_cs_data,
// SET_CONTEXT_REG of PA_SC_TILE_STEERING_OVERRIDE, PREAMBLE_CNTL with
// END_CLEAR_STATE and CLEAR_STATE (0x12). None of that exists for GFX12:
// there is no gfx12_cs_data table in the tree, gfx_v12_0.c never
// references PACKET3_PREAMBLE_CNTL or PACKET3_CLEAR_STATE, and
// pa_sc_tile_steering_override is not part of GFX12's config. Emitting a
// GFX11 clear-state blob here would push SET_CONTEXT_REG packets carrying
// GFX11 register offsets at a GFX12 context — so this port does what the
// GFX12 driver does: registers only.
//
// The ring-fetch proof the CP still owes us is therefore a separate,
// clearly-labelled diagnostic: cp_ring_fetch_probe below.
//
// max_hw_contexts is 8 on GFX12 (gfx_v12_0_gpu_early_init), so the value
// written is 7.
kern_return_t
cp_gfx_start(DeviceContext &dev, CPContext &cp, bool legacy_unhalt)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    CP_LOG("cp_gfx_start: CP_MAX_CONTEXT / CP_DEVICE_ID%s",
           legacy_unhalt
               ? ", then unhalt (the !amdgpu_async_gfx_ring branch, "
                 "gfx_v12_0.c:2706-2707)"
               : " (async_gfx_ring path: CP already running, queue mapped "
                 "by the MES — gfx_v12_0_cp_async_gfx_ring_resume)");

    // gfx_v12_0.c:2702
    cp_wreg_logged(dev, "CP_MAX_CONTEXT", CP_REG(dev, CP_MAX_CONTEXT),
                   8u - 1u);
    // gfx_v12_0.c:2704
    cp_wreg_logged(dev, "CP_DEVICE_ID", CP_REG(dev, CP_DEVICE_ID), 1);

    if (legacy_unhalt) {
        // gfx_v12_0.c:2706-2707. cp_enable() is our gfx_v12_0_cp_gfx_enable;
        // it also carries upstream's post-unhalt CP_STAT==0 wait.
        kern_return_t r = cp_enable(dev, true);
        if (r != kIOReturnSuccess) {
            CP_LOG("cp_gfx_start: cp_enable(true) failed %#x", r);
            return r;
        }
    }

    CP_LOG("cp_gfx_start: done (CP_RB0_RPTR=%#010x CP_STAT=%#010x "
           "CP_ME_CNTL=%#010x)",
           RREG32(dev, CP_REG(dev, CP_RB0_RPTR)),
           RREG32(dev, CP_REG(dev, CP_STAT)),
           RREG32(dev, CP_REG(dev, CP_ME_CNTL)));
    (void)cp;
    return kIOReturnSuccess;
}

// ----- cp_dump_gfx_hqd: the per-queue GFX HQD / MQD register block -----
//
// Not upstream. Selects me0/pipe0/queue0 (the kernel GFX ring) and reads the
// block gfx_v12_0_gfx_mqd_init describes. Before the MES ADD_QUEUE these
// should be the reset values (ACTIVE 0, MQD_BASE 0); after it, the values
// from our MQD — that is the proof the firmware consumed the MQD.
void
cp_dump_gfx_hqd(const DeviceContext &dev, const char *where)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return;
    CP_LOG("--- GFX HQD block me0/pipe0/queue0 (%s) ---", where);
    cp_grbm_select(dev, 0, 0, 0, 0);
    cp_rreg_logged(dev, "CP_GFX_HQD_ACTIVE",       CP_REG(dev, CP_GFX_HQD_ACTIVE));
    cp_rreg_logged(dev, "CP_GFX_HQD_MAPPED",       CP_REG(dev, CP_GFX_HQD_MAPPED));
    cp_rreg_logged(dev, "CP_GFX_MQD_BASE_ADDR",    CP_REG(dev, CP_GFX_MQD_BASE_ADDR));
    cp_rreg_logged(dev, "CP_GFX_MQD_BASE_ADDR_HI", CP_REG(dev, CP_GFX_MQD_BASE_ADDR_HI));
    cp_rreg_logged(dev, "CP_GFX_MQD_CONTROL",      CP_REG(dev, CP_GFX_MQD_CONTROL));
    cp_rreg_logged(dev, "CP_GFX_HQD_VMID",         CP_REG(dev, CP_GFX_HQD_VMID));
    cp_rreg_logged(dev, "CP_GFX_HQD_QUEUE_PRIORITY", CP_REG(dev, CP_GFX_HQD_QUEUE_PRIORITY));
    cp_rreg_logged(dev, "CP_GFX_HQD_QUANTUM",      CP_REG(dev, CP_GFX_HQD_QUANTUM));
    cp_rreg_logged(dev, "CP_GFX_HQD_BASE",         CP_REG(dev, CP_GFX_HQD_BASE));
    cp_rreg_logged(dev, "CP_GFX_HQD_BASE_HI",      CP_REG(dev, CP_GFX_HQD_BASE_HI));
    cp_rreg_logged(dev, "CP_GFX_HQD_CNTL",         CP_REG(dev, CP_GFX_HQD_CNTL));
    cp_rreg_logged(dev, "CP_GFX_HQD_RPTR",         CP_REG(dev, CP_GFX_HQD_RPTR));
    cp_rreg_logged(dev, "CP_GFX_HQD_RPTR_ADDR",    CP_REG(dev, CP_GFX_HQD_RPTR_ADDR));
    cp_rreg_logged(dev, "CP_GFX_HQD_RPTR_ADDR_HI", CP_REG(dev, CP_GFX_HQD_RPTR_ADDR_HI));
    cp_rreg_logged(dev, "CP_GFX_HQD_WPTR",         CP_REG(dev, CP_GFX_HQD_WPTR));
    cp_rreg_logged(dev, "CP_GFX_HQD_WPTR_HI",      CP_REG(dev, CP_GFX_HQD_WPTR_HI));
    cp_rreg_logged(dev, "CP_GFX_HQD_OFFSET",       CP_REG(dev, CP_GFX_HQD_OFFSET));
    cp_rreg_logged(dev, "CP_GFX_HQD_CSMD_RPTR",    CP_REG(dev, CP_GFX_HQD_CSMD_RPTR));
    cp_rreg_logged(dev, "CP_GFX_HQD_QUE_MGR_CONTROL", CP_REG(dev, CP_GFX_HQD_QUE_MGR_CONTROL));
    cp_rreg_logged(dev, "CP_GFX_HQD_HQ_STATUS0",   CP_REG(dev, CP_GFX_HQD_HQ_STATUS0));
    cp_rreg_logged(dev, "CP_GFX_HQD_HQ_CONTROL0",  CP_REG(dev, CP_GFX_HQD_HQ_CONTROL0));
    cp_rreg_logged(dev, "CP_RB_WPTR_POLL_ADDR_LO", CP_REG(dev, CP_RB_WPTR_POLL_ADDR_LO));
    cp_rreg_logged(dev, "CP_RB_WPTR_POLL_ADDR_HI", CP_REG(dev, CP_RB_WPTR_POLL_ADDR_HI));
    cp_rreg_logged(dev, "CP_RB_DOORBELL_CONTROL",  CP_REG(dev, CP_RB_DOORBELL_CONTROL));
    CP_LOG("--- end GFX HQD block (%s) ---", where);
}

// ----- cp_gfx_mqd_init: gfx_v12_0_gfx_mqd_init (gfx_v12_0.c:3003) -----
//
// On GFX12 the driver does NOT program the kernel GFX ring through
// CP_RB0_*. gfx_v12_0_cp_resume with amdgpu_async_gfx_ring=1 (the only
// configuration that runs on real hardware; the module default) goes
// cp_gfx_enable(true) -> [MES init] -> gfx_v12_0_cp_async_gfx_ring_resume:
// kgq_init_queue (this MQD) -> amdgpu_gfx_enable_kgq -> MES ADD_QUEUE with
// map_legacy_kq=1 -> cp_gfx_start (CP_MAX_CONTEXT / CP_DEVICE_ID). The
// MES/CP firmware then loads the HQD register block from the MQD.
//
// Stage 16 runs 1-3 (0.0.13-0.0.15) used the !async CP_RB0_* path instead:
// the CP came up (CP_STAT idle), took the doorbell (DOORBELL_HIT latched),
// then halted itself with a CPG read fault at VA 0 and never fetched.
// CP_GFX_MQD_BASE_ADDR was 0 the whole time — the RS64 firmware went
// looking for a queue descriptor that did not exist. Hence this function.
//
// Values follow gfx_v12_0_gfx_mqd_init line by line, starting from the
// register defaults gfx_v12_0.c:55-61 defines (GfxMqd::kDefault_*).
// prop->shadow_addr / csa_addr / fence_address are 0 for a kernel queue
// (amdgpu_ring_to_mqd_prop leaves them unset), prop->kernel_queue = true
// (RB_NON_PRIV stays 0), prop->tmz_queue = false.
kern_return_t
cp_gfx_mqd_init(const DeviceContext &dev, CPContext &cp)
{
    if (!cp.inited || cp.mqd_cpu == nullptr || cp.mqd_bus == 0)
        return kIOReturnNotReady;
    if (cp.mqd_sys.size < GfxMqd::kDwords * 4u) {
        CP_LOG("gfx_mqd_init: MQD buffer is %llu bytes, v12_gfx_mqd needs %u",
               (unsigned long long)cp.mqd_sys.size, GfxMqd::kDwords * 4u);
        return kIOReturnNoSpace;
    }
    if (cp.ring_size_dwords < 8 ||
        (cp.ring_size_dwords & (cp.ring_size_dwords - 1u)) != 0) {
        CP_LOG("gfx_mqd_init: ring size %u dwords is not a power of two",
               cp.ring_size_dwords);
        return kIOReturnBadArgument;
    }

    uint32_t *m = static_cast<uint32_t *>(cp.mqd_cpu);
    memset(m, 0, GfxMqd::kDwords * 4u);

    // set up gfx hqd wptr
    m[GfxMqd::cp_gfx_hqd_wptr]    = 0;
    m[GfxMqd::cp_gfx_hqd_wptr_hi] = 0;

    // set the pointer to the MQD
    m[GfxMqd::cp_mqd_base_addr]    = static_cast<uint32_t>(cp.mqd_bus & 0xfffffffcull);
    m[GfxMqd::cp_mqd_base_addr_hi] = static_cast<uint32_t>(cp.mqd_bus >> 32);

    // set up mqd control
    uint32_t tmp = GfxMqd::kDefault_CP_GFX_MQD_CONTROL;
    tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, VMID, 0);
    tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, PRIV_STATE, 1);
    tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, CACHE_POLICY, 0);
    m[GfxMqd::cp_gfx_mqd_control] = tmp;

    // set up gfx_hqd_vmid with 0x0 to indicate the ring buffer's vmid
    m[GfxMqd::cp_gfx_hqd_vmid] = 0;

    // set up default queue priority level (0x0 = low)
    tmp = GfxMqd::kDefault_CP_GFX_HQD_QUEUE_PRIORITY;
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_QUEUE_PRIORITY, PRIORITY_LEVEL, 0);
    m[GfxMqd::cp_gfx_hqd_queue_priority] = tmp;

    // set up time quantum
    tmp = GfxMqd::kDefault_CP_GFX_HQD_QUANTUM;
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_QUANTUM, QUANTUM_EN, 1);
    m[GfxMqd::cp_gfx_hqd_quantum] = tmp;

    // set up gfx hqd base. this is similar as CP_RB_BASE
    const uint64_t hqd_gpu_addr = cp.ring_bus >> 8;
    m[GfxMqd::cp_gfx_hqd_base]    = static_cast<uint32_t>(hqd_gpu_addr);
    m[GfxMqd::cp_gfx_hqd_base_hi] = static_cast<uint32_t>(hqd_gpu_addr >> 32);

    // set up hqd_rptr_addr/_hi, similar as CP_RB_RPTR
    m[GfxMqd::cp_gfx_hqd_rptr_addr]    = static_cast<uint32_t>(cp.rptr_gpu_addr & 0xfffffffcull);
    m[GfxMqd::cp_gfx_hqd_rptr_addr_hi] = static_cast<uint32_t>(cp.rptr_gpu_addr >> 32) & 0xffffu;

    // set up rb_wptr_poll addr
    m[GfxMqd::cp_rb_wptr_poll_addr_lo] = static_cast<uint32_t>(cp.wptr_gpu_addr & 0xfffffffcull);
    m[GfxMqd::cp_rb_wptr_poll_addr_hi] = static_cast<uint32_t>(cp.wptr_gpu_addr >> 32) & 0xffffu;

    // set up the gfx_hqd_control, similar as CP_RB0_CNTL:
    //   rb_bufsz = order_base_2(queue_size_bytes / 4) - 1
    uint32_t log2_dwords = 0;
    while ((1u << log2_dwords) < cp.ring_size_dwords) log2_dwords++;
    const uint32_t rb_bufsz = log2_dwords - 1u;
    tmp = GfxMqd::kDefault_CP_GFX_HQD_CNTL;
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, RB_BUFSZ, rb_bufsz);
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, RB_BLKSZ, rb_bufsz - 2u);
    // kernel_queue -> RB_NON_PRIV 0; not a TMZ queue -> TMZ_MATCH 0.
    m[GfxMqd::cp_gfx_hqd_cntl] = tmp;

    // set up cp_doorbell_control
    tmp = GfxMqd::kDefault_CP_RB_DOORBELL_CONTROL;
    tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_OFFSET, cp.doorbell_index);
    tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 1);
    m[GfxMqd::cp_rb_doorbell_control] = tmp;

    // reset read and write pointers, similar to CP_RB0_WPTR/_RPTR
    m[GfxMqd::cp_gfx_hqd_rptr] = GfxMqd::kDefault_CP_GFX_HQD_RPTR;

    // active the queue
    m[GfxMqd::cp_gfx_hqd_active] = 1;

    // set gfx UQ items — all 0 for a kernel queue (no shadow / CSA / fence)
    m[GfxMqd::shadow_base_lo]       = 0;
    m[GfxMqd::shadow_base_hi]       = 0;
    m[GfxMqd::fw_work_area_base_lo] = 0;
    m[GfxMqd::fw_work_area_base_hi] = 0;
    m[GfxMqd::fence_address_lo]     = 0;
    m[GfxMqd::fence_address_hi]     = 0;

    // Publish: the MQD is GART sysmem, so a store fence is what matters;
    // the HDP flush mirrors amdgpu_gfx_enable_kgq's amdgpu_device_flush_hdp.
    sysmem_wmb();
    amdgpu_hdp_flush(dev);
    cp.gfx_mqd_inited = true;

    CP_LOG("gfx_mqd_init: v12_gfx_mqd @ mc=%#llx (%u dwords) — mqd_base=%08x_%08x "
           "mqd_control=%#010x hqd_base=%08x_%08x (ring_mc=%#llx >> 8) "
           "rptr_addr=%08x_%08x wptr_poll=%08x_%08x cntl=%#010x (bufsz=%u blksz=%u) "
           "doorbell_control=%#010x (idx %u) quantum=%#010x prio=%#010x vmid=%u active=%u",
           (unsigned long long)cp.mqd_bus, GfxMqd::kDwords,
           m[GfxMqd::cp_mqd_base_addr_hi], m[GfxMqd::cp_mqd_base_addr],
           m[GfxMqd::cp_gfx_mqd_control],
           m[GfxMqd::cp_gfx_hqd_base_hi], m[GfxMqd::cp_gfx_hqd_base],
           (unsigned long long)cp.ring_bus,
           m[GfxMqd::cp_gfx_hqd_rptr_addr_hi], m[GfxMqd::cp_gfx_hqd_rptr_addr],
           m[GfxMqd::cp_rb_wptr_poll_addr_hi], m[GfxMqd::cp_rb_wptr_poll_addr_lo],
           m[GfxMqd::cp_gfx_hqd_cntl], rb_bufsz, rb_bufsz - 2u,
           m[GfxMqd::cp_rb_doorbell_control], cp.doorbell_index,
           m[GfxMqd::cp_gfx_hqd_quantum], m[GfxMqd::cp_gfx_hqd_queue_priority],
           m[GfxMqd::cp_gfx_hqd_vmid], m[GfxMqd::cp_gfx_hqd_active]);
    return kIOReturnSuccess;
}

// ----- cp_gfx_mqd_init_external: the same MQD, for Apple's ring -----
//
// 0.0.178 "gfxmap". doStart's post-MAP_QUEUES gate (0xbe24357) reads
// CP_RB0_RPTR / CP_RB0_WPTR{,_HI} and requires all three to be zero: it is
// asking whether the hardware GFX ring it just mapped is idle. On this machine
// those registers describe OUR kernel GFX queue, because the ladder mapped it
// onto GFX pipe0/queue0 long before Apple's accelerator loaded, and it is not
// idle. The fix is not to fake the registers — it is to hand pipe0/queue0 over
// to Apple's ring, which is what this MQD describes.
//
// Only four inputs differ from cp_gfx_mqd_init, and each is an address or a
// size that belongs to Apple's ring rather than ours. Everything else — the
// register defaults, VMID 0, PRIV_STATE 1, the quantum, the priority, the
// active flag, the kernel-queue zeros for shadow/CSA/fence — is identical,
// because a kernel GFX queue is a kernel GFX queue whoever wrote the packets.
kern_return_t
cp_gfx_mqd_init_external(const DeviceContext &dev, CPContext &cp,
                         CPExternalGfxQueue &q)
{
    if (!cp.inited || cp.mqd_cpu == nullptr || cp.mqd_bus == 0)
        return kIOReturnNotReady;
    if (cp.mqd_sys.size < GfxMqd::kDwords * 4u) {
        CP_LOG("gfx_mqd_external: MQD buffer is %llu bytes, v12_gfx_mqd needs %u",
               (unsigned long long)cp.mqd_sys.size, GfxMqd::kDwords * 4u);
        return kIOReturnNoSpace;
    }
    if (q.ring_gpu == 0 || (q.ring_gpu & 0xFFull) != 0) {
        CP_LOG("gfx_mqd_external: ring %#llx is 0 or not 256-byte aligned "
               "(CP_GFX_HQD_BASE is addr>>8) - REFUSING",
               (unsigned long long)q.ring_gpu);
        return kIOReturnBadArgument;
    }
    const uint32_t dwords = q.ring_bytes / 4u;
    if (dwords < 8 || (dwords & (dwords - 1u)) != 0) {
        CP_LOG("gfx_mqd_external: ring size %u bytes (%u dwords) is not a power "
               "of two >= 32 - REFUSING", q.ring_bytes, dwords);
        return kIOReturnBadArgument;
    }
    if (q.rptr_report_gpu == 0 || q.wptr_poll_gpu == 0) {
        CP_LOG("gfx_mqd_external: rptr_report=%#llx wptr_poll=%#llx — a zero "
               "write-back address would make the CP report into page 0 - REFUSING",
               (unsigned long long)q.rptr_report_gpu,
               (unsigned long long)q.wptr_poll_gpu);
        return kIOReturnBadArgument;
    }

    uint32_t *m = static_cast<uint32_t *>(cp.mqd_cpu);
    memset(m, 0, GfxMqd::kDwords * 4u);

    m[GfxMqd::cp_gfx_hqd_wptr]    = 0;
    m[GfxMqd::cp_gfx_hqd_wptr_hi] = 0;

    m[GfxMqd::cp_mqd_base_addr]    = static_cast<uint32_t>(cp.mqd_bus & 0xfffffffcull);
    m[GfxMqd::cp_mqd_base_addr_hi] = static_cast<uint32_t>(cp.mqd_bus >> 32);

    uint32_t tmp = GfxMqd::kDefault_CP_GFX_MQD_CONTROL;
    tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, VMID, 0);
    tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, PRIV_STATE, 1);
    tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, CACHE_POLICY, 0);
    m[GfxMqd::cp_gfx_mqd_control] = tmp;

    m[GfxMqd::cp_gfx_hqd_vmid] = 0;

    tmp = GfxMqd::kDefault_CP_GFX_HQD_QUEUE_PRIORITY;
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_QUEUE_PRIORITY, PRIORITY_LEVEL, 0);
    m[GfxMqd::cp_gfx_hqd_queue_priority] = tmp;

    tmp = GfxMqd::kDefault_CP_GFX_HQD_QUANTUM;
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_QUANTUM, QUANTUM_EN, 1);
    m[GfxMqd::cp_gfx_hqd_quantum] = tmp;

    // ---- (1) APPLE'S ring base
    const uint64_t hqd_gpu_addr = q.ring_gpu >> 8;
    m[GfxMqd::cp_gfx_hqd_base]    = static_cast<uint32_t>(hqd_gpu_addr);
    m[GfxMqd::cp_gfx_hqd_base_hi] = static_cast<uint32_t>(hqd_gpu_addr >> 32);

    // ---- (2) APPLE'S rptr report address: the qword AMDGFX10PM4Engine::
    //          initGraphicsMQD copies out of its ring+0xb8 into its own MQD's
    //          dwords 139-140 (`movq 0xb8(%rcx),%rax ; movq %rax,0x22c(%r13)`
    //          @0xbe25549). 0x22c/4 = 139 = cp_gfx_hqd_rptr_addr, so the two
    //          layouts agree on this field.
    m[GfxMqd::cp_gfx_hqd_rptr_addr]    = static_cast<uint32_t>(q.rptr_report_gpu & 0xfffffffcull);
    m[GfxMqd::cp_gfx_hqd_rptr_addr_hi] = static_cast<uint32_t>(q.rptr_report_gpu >> 32) & 0xffffu;

    // ---- (3) APPLE'S wptr poll address: MAP_QUEUES dw5/dw6, which is the
    //          ring+0xd0 doStart passed to submitMapQueuesPacket.
    m[GfxMqd::cp_rb_wptr_poll_addr_lo] = static_cast<uint32_t>(q.wptr_poll_gpu & 0xfffffffcull);
    m[GfxMqd::cp_rb_wptr_poll_addr_hi] = static_cast<uint32_t>(q.wptr_poll_gpu >> 32) & 0xffffu;

    // rb_bufsz = order_base_2(queue_size_bytes / 4) - 1, the same expression
    // cp_gfx_mqd_init uses. 0x80000 bytes -> 131072 dwords -> 17 - 1 = 16.
    uint32_t log2_dwords = 0;
    while ((1u << log2_dwords) < dwords) log2_dwords++;
    const uint32_t rb_bufsz = log2_dwords - 1u;
    tmp = GfxMqd::kDefault_CP_GFX_HQD_CNTL;
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, RB_BUFSZ, rb_bufsz);
    tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, RB_BLKSZ, rb_bufsz - 2u);
    // RB_NO_UPDATE (bit 27) stays CLEAR: initGraphicsMQD clears the same bit in
    // Apple's own copy (`andb $-0x9, 0x247(%r13)` @0xbe2553d — byte 0x247 is
    // byte 3 of dword 145, i.e. bit 27 of CP_GFX_HQD_CNTL), so the CP is meant
    // to keep updating the pointers itself.
    m[GfxMqd::cp_gfx_hqd_cntl] = tmp;

    // ---- (4) APPLE'S doorbell index — the one WE handed back at OUT+0x08 of
    //          startEngineQueue(queue=9), which initGraphicsMQD stored at
    //          engine+0x248 (@0xbe25565) and doStart passed to
    //          submitMapQueuesPacket as its `unsigned int` argument.
    tmp = GfxMqd::kDefault_CP_RB_DOORBELL_CONTROL;
    tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_OFFSET, q.doorbell_index);
    tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 1);
    m[GfxMqd::cp_rb_doorbell_control] = tmp;

    m[GfxMqd::cp_gfx_hqd_rptr]   = GfxMqd::kDefault_CP_GFX_HQD_RPTR;
    m[GfxMqd::cp_gfx_hqd_active] = 1;

    m[GfxMqd::shadow_base_lo]       = 0;
    m[GfxMqd::shadow_base_hi]       = 0;
    m[GfxMqd::fw_work_area_base_lo] = 0;
    m[GfxMqd::fw_work_area_base_hi] = 0;
    m[GfxMqd::fence_address_lo]     = 0;
    m[GfxMqd::fence_address_hi]     = 0;

    sysmem_wmb();
    amdgpu_hdp_flush(dev);

    q.rb_bufsz         = rb_bufsz;
    q.hqd_cntl         = m[GfxMqd::cp_gfx_hqd_cntl];
    q.doorbell_control = m[GfxMqd::cp_rb_doorbell_control];

    // Our KGQ no longer owns this buffer, and nothing may go on believing it
    // does — the next cp_map_gfx_kgq_mes would map Apple's ring by accident.
    cp.gfx_mqd_inited = false;
    cp.kgq_mapped     = false;

    CP_LOG("gfx_mqd_external: v12_gfx_mqd @ mc=%#llx for AN EXTERNAL ring — "
           "hqd_base=%08x_%08x (ring_mc=%#llx >> 8, %u bytes = %u dwords) "
           "rptr_addr=%08x_%08x wptr_poll=%08x_%08x cntl=%#010x (bufsz=%u blksz=%u, "
           "RB_NO_UPDATE clear) doorbell_control=%#010x (dword idx %u) vmid=0 active=1",
           (unsigned long long)cp.mqd_bus,
           m[GfxMqd::cp_gfx_hqd_base_hi], m[GfxMqd::cp_gfx_hqd_base],
           (unsigned long long)q.ring_gpu, q.ring_bytes, dwords,
           m[GfxMqd::cp_gfx_hqd_rptr_addr_hi], m[GfxMqd::cp_gfx_hqd_rptr_addr],
           m[GfxMqd::cp_rb_wptr_poll_addr_hi], m[GfxMqd::cp_rb_wptr_poll_addr_lo],
           m[GfxMqd::cp_gfx_hqd_cntl], rb_bufsz, rb_bufsz - 2u,
           m[GfxMqd::cp_rb_doorbell_control], q.doorbell_index);
    return kIOReturnSuccess;
}

// ----- cp_map_gfx_kgq_mes: amdgpu_gfx_enable_kgq via the MES -----
//
// amdgpu_gfx.c amdgpu_gfx_enable_kgq: with adev->mes.enable_legacy_queue_map
// (true on GFX12, mes_v12_0_early_init) every kernel GFX ring goes through
// amdgpu_mes_map_legacy_queue -> mes_v12_0_map_legacy_queue:
//
//     header ADD_QUEUE; pipe_id = ring->pipe (0); queue_id = ring->queue (0);
//     doorbell_offset = ring->doorbell_index; mqd_addr = MQD gpu addr;
//     wptr_addr = ring->wptr_gpu_addr; queue_type = MES_QUEUE_TYPE_GFX;
//     map_legacy_kq = 1;  pipe = SCHED (uni_mes, non-MES queue)
//
// Nothing else in the frame is set. Our mes_add_hw_queue builds exactly
// that frame from MESAddQueueInput (zeros elsewhere) and submits it on the
// SCHED pipe with the api_status fence + chained QUERY_SCHEDULER_STATUS,
// which is mes_v12_0_submit_pkt_and_poll_completion.
kern_return_t
cp_map_gfx_kgq_mes(const DeviceContext &dev, CPContext &cp, MESContext &mes)
{
    if (!cp.inited) return kIOReturnNotReady;
    if (!cp.gfx_mqd_inited) {
        CP_LOG("map_gfx_kgq_mes: MQD not initialised — call cp_gfx_mqd_init first");
        return kIOReturnNotReady;
    }
    if (!mes.pipe[0].inited || !mes.pipe[0].enabled) {
        CP_LOG("map_gfx_kgq_mes: MES SCHED pipe is not running (inited=%d enabled=%d) — "
               "the GFX12 kernel GFX queue can only be mapped by the MES",
               mes.pipe[0].inited ? 1 : 0, mes.pipe[0].enabled ? 1 : 0);
        return kIOReturnNotReady;
    }

    cp_dump_gfx_hqd(dev, "before MES ADD_QUEUE(map_legacy_kq)");

    // amdgpu_gfx_enable_kgq: amdgpu_device_flush_hdp before the map.
    sysmem_wmb();
    amdgpu_hdp_flush(dev);

    MESAddQueueInput in;
    memset(&in, 0, sizeof(in));
    in.queue_type      = kMESQueueType_GFX;
    in.pipe_id         = 0;                 // gfx_ring[0]: me 0, pipe 0, queue 0
    in.queue_id        = 0;
    in.doorbell_offset = cp.doorbell_index; // ring->doorbell_index (dword index)
    in.mqd_addr        = cp.mqd_bus;
    in.wptr_addr       = cp.wptr_gpu_addr;
    in.flags           = kAddQueueFlag_map_legacy_kq;

    CP_LOG("map_gfx_kgq_mes: MES ADD_QUEUE type=GFX pipe=0 queue=0 doorbell=%u "
           "mqd_mc=%#llx wptr_mc=%#llx flags=map_legacy_kq — "
           "mes_v12_0_map_legacy_queue, uni_mes -> SCHED pipe",
           in.doorbell_offset, (unsigned long long)in.mqd_addr,
           (unsigned long long)in.wptr_addr);

    kern_return_t r = mes_add_hw_queue(dev, mes, in);
    if (r != kIOReturnSuccess) {
        CP_LOG("map_gfx_kgq_mes: MES did not ack ADD_QUEUE: %#x", r);
        cp_dump_gfx_hqd(dev, "after FAILED MES ADD_QUEUE");
        return r;
    }
    cp.kgq_mapped = true;

    // What the firmware wrote back into the MQD (it owns it from here on).
    sysmem_rmb();
    const volatile uint32_t *m = static_cast<const volatile uint32_t *>(cp.mqd_cpu);
    CP_LOG("map_gfx_kgq_mes: ACKED. MQD now: active=%u mapped=%u rptr=%u wptr=%u/%u "
           "offset=%#x csmd_rptr=%#x hq_status0=%#010x que_mgr_control=%#010x",
           m[GfxMqd::cp_gfx_hqd_active], m[GfxMqd::cp_gfx_hqd_mapped],
           m[GfxMqd::cp_gfx_hqd_rptr], m[GfxMqd::cp_gfx_hqd_wptr],
           m[GfxMqd::cp_gfx_hqd_wptr_hi], m[GfxMqd::cp_gfx_hqd_offset],
           m[GfxMqd::cp_gfx_hqd_csmd_rptr], m[GfxMqd::cp_gfx_hqd_hq_status0],
           m[GfxMqd::cp_gfx_hqd_que_mgr_control]);
    cp_dump_gfx_hqd(dev, "after MES ADD_QUEUE(map_legacy_kq)");
    return kIOReturnSuccess;
}

// ----- cp_ring_test_scratch: gfx_v12_0_ring_test_ring -----
//
//     WREG32(scratch, 0xCAFEDEAD);
//     PACKET3(PACKET3_SET_UCONFIG_REG, 1); scratch - SET_UCONFIG_REG_START;
//     0xDEADBEEF; commit; poll RREG32(scratch) == 0xDEADBEEF (usec_timeout)
//
// `scratch` is SOC15_REG_OFFSET(GC, 0, regSCRATCH_REG0) = GC base[1] +
// 0x2040, and the packet carries that minus 0xC000. This is upstream's
// first-ever packet on every GFX ring, and it proves PM4 execution with
// no memory write from the CP: if it passes while the EOP fence fails,
// the problem is the CP's memory path, not the CP.
kern_return_t
cp_ring_test_scratch(const DeviceContext &dev, CPContext &cp, uint64_t timeout_us)
{
    if (!cp.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC) ||
        !dev.ip.isResolved(IPBlock::GC, CPRegBaseIdx::SCRATCH_REG0))
        return kIOReturnNotReady;

    const uint32_t scratch = CP_REG(dev, SCRATCH_REG0);   // absolute dword offset
    if (scratch < kP3_SET_UCONFIG_REG_START) {
        CP_LOG("ring_test: SCRATCH_REG0 @%#x is below the UCONFIG space %#x — "
               "GC base[1] is wrong", scratch, kP3_SET_UCONFIG_REG_START);
        return kIOReturnInternalError;
    }

    WREG32(dev, scratch, 0xCAFEDEADu);
    const uint32_t before = RREG32(dev, scratch);
    cp.ring_test_passed = false;

    uint32_t pkt[3];
    pkt[0] = cp_p3(kP3_SET_UCONFIG_REG, 1);            // 2 payload dwords
    pkt[1] = scratch - kP3_SET_UCONFIG_REG_START;      // 0x40 for base[1]=0xA000
    pkt[2] = 0xDEADBEEFu;
    if (cp_ring_write(cp, pkt, 3) != 3) {
        CP_LOG("ring_test: ring_write refused 3 dwords");
        return kIOReturnNoSpace;
    }
    CP_LOG("ring_test: SCRATCH_REG0 [dw %#x] = %#010x before; staged %#010x %#010x %#010x "
           "(SET_UCONFIG_REG reg %#x <- 0xDEADBEEF), wptr -> %u",
           scratch, before, pkt[0], pkt[1], pkt[2], pkt[1], cp.wptr);

    amdgpu_hdp_flush(dev);
    kern_return_t r = cp_kick_doorbell(dev, cp);
    if (r != kIOReturnSuccess) return r;

    uint64_t elapsed = 0;
    uint32_t v = before;
    while (elapsed < timeout_us) {
        v = RREG32(dev, scratch);
        if (v == 0xDEADBEEFu) break;
        IODelay(1);
        elapsed += 1;
    }
    cp.ring_test_value = v;
    sysmem_rmb();
    if (v == 0xDEADBEEFu) {
        cp.ring_test_passed = true;
        cp.fetch_proven = true;
        CP_LOG("ring_test: PASSED — SCRATCH_REG0 = %#010x after %llu us "
               "(CP_RB0_RPTR=%#010x wb_rptr=%u wptr=%u). THE CP EXECUTES PM4.",
               v, (unsigned long long)elapsed,
               RREG32(dev, CP_REG(dev, CP_RB0_RPTR)),
               cp.rptr_cpu ? *cp.rptr_cpu : 0u, cp.wptr);
        return kIOReturnSuccess;
    }
    CP_LOG("ring_test: TIMEOUT after %llu us — SCRATCH_REG0 = %#010x (want 0xDEADBEEF), "
           "CP_RB0_RPTR=%#010x wb_rptr=%u wptr=%u",
           (unsigned long long)elapsed, v,
           RREG32(dev, CP_REG(dev, CP_RB0_RPTR)),
           cp.rptr_cpu ? *cp.rptr_cpu : 0u, cp.wptr);
    cp_dump_state(dev, cp, "ring_test timeout");
    return kIOReturnTimeout;
}

// ----- cp_submit_ib / cp_wait_fence: the shared submission path -----
kern_return_t
cp_submit_ib(const DeviceContext &dev, CPContext &cp, uint64_t ib_gpu_va,
             uint32_t length_dw, uint32_t vmid, uint32_t *out_fence)
{
    if (!cp.inited || cp.fence_cpu == nullptr) return kIOReturnNotReady;
    if (length_dw == 0 || length_dw > 0xFFFFFu) return kIOReturnBadArgument;
    if ((ib_gpu_va & 0x3ull) != 0) return kIOReturnBadArgument;
    if (vmid > 15) return kIOReturnBadArgument;

    uint32_t ring[7];
    uint32_t k = 0;
    ring[k++] = cp_p3(kP3_CONTEXT_CONTROL, 1);
    ring[k++] = 0x80000000u;          // load_enable, nothing else
    ring[k++] = 0u;
    ring[k++] = cp_p3(kP3_INDIRECT_BUFFER, 2);
    ring[k++] = (uint32_t)(ib_gpu_va & 0xFFFFFFFFu);
    ring[k++] = (uint32_t)(ib_gpu_va >> 32);
    ring[k++] = (length_dw & 0xFFFFFu) | (vmid << 24);
    if (cp_ring_write(cp, ring, k) != k) return kIOReturnNoSpace;

    const uint32_t fence = cp_emit_eop_fence(cp);
    if (fence == 0) return kIOReturnInternalError;

    amdgpu_hdp_flush(dev);
    kern_return_t r = cp_kick_doorbell(dev, cp);
    if (r != kIOReturnSuccess) return r;
    if (out_fence) *out_fence = fence;
    return kIOReturnSuccess;
}

kern_return_t
cp_wait_fence(const CPContext &cp, uint32_t fence, uint64_t timeout_us,
              uint64_t *observed, uint64_t *elapsed_us)
{
    if (!cp.inited || cp.fence_cpu == nullptr) return kIOReturnNotReady;
    // First 5 ms at 1 us steps (the GPU answers in microseconds when healthy),
    // then 1 ms sleeps so a wedged ring does not hold a core for the timeout.
    constexpr uint64_t kFastUs = 5000;
    uint64_t elapsed = 0, v = 0;
    while (elapsed < timeout_us) {
        sysmem_rmb();
        v = *cp.fence_cpu;
        if ((v & 0xFFFFFFFFull) >= fence) break;
        if (elapsed < kFastUs) { IODelay(1); elapsed += 1; }
        else                   { IOSleep(1); elapsed += 1000; }
    }
    sysmem_rmb();
    v = *cp.fence_cpu;
    if (observed)   *observed = v;
    if (elapsed_us) *elapsed_us = elapsed;
    return ((v & 0xFFFFFFFFull) >= fence) ? kIOReturnSuccess : kIOReturnTimeout;
}

// ----- cp_set_eop_interrupt: gfx_v12_0_set_gfx_eop_interrupt_state -----
kern_return_t
cp_set_eop_interrupt(const DeviceContext &dev, bool enable)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    const uint32_t reg = CP_REG(dev, CP_INT_CNTL_RING0);
    uint32_t v = RREG32(dev, reg);
    const uint32_t bits = CP_INT_CNTL_RING0__TIME_STAMP_INT_ENABLE_MASK |
                          CP_INT_CNTL_RING0__GENERIC0_INT_ENABLE_MASK |
                          CP_INT_CNTL_RING0__OPCODE_ERROR_INT_ENABLE_MASK |
                          CP_INT_CNTL_RING0__PRIV_REG_INT_ENABLE_MASK |
                          CP_INT_CNTL_RING0__PRIV_INSTR_INT_ENABLE_MASK;
    if (enable) v |= bits; else v &= ~bits;
    cp_wreg_logged(dev, "CP_INT_CNTL_RING0", reg, v);
    CP_LOG("EOP + command-stream-error interrupts %s", enable ? "enabled" : "disabled");
    return kIOReturnSuccess;
}

// ----- cp_ring_fetch_probe: does the CP fetch at all? -----
//
// Not upstream. This is the smallest experiment that distinguishes
// "the CP front-end is alive and consuming the ring" from "the CP is
// wedged / never started", which is the exact question left open by an
// EOP fence timing out with CP_RB0_RPTR == 0.
//
// It writes `nops` single-dword PM4 NOPs (0xFFFF1000 — IT_NOP with the
// COUNT==0x3FFF one-dword special case, byte-identical to the dword
// gfx_v12_0.c:5358 installs as the GFX ring's `.nop`), publishes the wptr
// and rings the doorbell, then polls until CP_RB0_RPTR reaches the wptr.
//
// A NOP needs no context state, no shader, no VMID, no clear state and
// touches no memory outside the ring, so a successful probe isolates one
// fact and nothing else: the PFP/ME pair executes microcode and walks the
// ring. If RPTR advances here and the EOP fence still fails, the problem
// is downstream (RELEASE_MEM / GCR / the write-back address), not the CP.
kern_return_t
cp_ring_fetch_probe(const DeviceContext &dev, CPContext &cp,
                    uint32_t nops, uint64_t timeout_us)
{
    if (!cp.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    if (nops == 0) nops = 16;
    if (nops > cp.ring_size_dwords / 2) nops = cp.ring_size_dwords / 2;

    const uint32_t rb_rptr_reg = CP_REG(dev, CP_RB0_RPTR);
    const uint32_t rptr_before_reg = RREG32(dev, rb_rptr_reg);
    sysmem_rmb();
    const uint32_t rptr_before_wb = cp.rptr_cpu ? *cp.rptr_cpu : 0u;
    const uint32_t wptr_before    = cp.wptr;

    CP_LOG("ring_fetch_probe: BEFORE — CP_RB0_RPTR=%#010x wb_rptr=%u "
           "sw_wptr=%u CP_STAT=%#010x CP_ME_CNTL=%#010x",
           rptr_before_reg, rptr_before_wb, wptr_before,
           RREG32(dev, CP_REG(dev, CP_STAT)),
           RREG32(dev, CP_REG(dev, CP_ME_CNTL)));

    // Stage the NOP block. cp_ring_write advances cp.wptr.
    {
        uint32_t block[64];
        const uint32_t nop = cp_p3_nop1();
        uint32_t left = nops;
        while (left > 0) {
            const uint32_t chunk = (left > 64u) ? 64u : left;
            for (uint32_t i = 0; i < chunk; i++) block[i] = nop;
            if (cp_ring_write(cp, block, chunk) != chunk) {
                CP_LOG("ring_fetch_probe: ring_write refused %u dwords",
                       chunk);
                return kIOReturnNoSpace;
            }
            left -= chunk;
        }
    }
    CP_LOG("ring_fetch_probe: staged %u NOP dwords (%#010x) — wptr %u -> %u",
           nops, cp_p3_nop1(), wptr_before, cp.wptr);

    // Drain posted writes so the CP sees the ring contents, then kick.
    amdgpu_hdp_flush(dev);
    kern_return_t r = cp_kick_doorbell(dev, cp);
    if (r != kIOReturnSuccess) return r;

    const uint32_t target = cp.wptr;
    const uint64_t step_us = 100;
    uint64_t elapsed = 0;
    uint32_t rptr_reg = rptr_before_reg;
    uint32_t rptr_wb  = rptr_before_wb;
    while (elapsed < timeout_us) {
        rptr_reg = RREG32(dev, rb_rptr_reg);
        sysmem_rmb();
        rptr_wb = cp.rptr_cpu ? *cp.rptr_cpu : 0u;
        if (rptr_reg == target || rptr_wb == target) {
            cp.fetch_proven = true;
            cp.probe_rptr   = rptr_reg;
            CP_LOG("ring_fetch_probe: AFTER — THE CP FETCHES. "
                   "CP_RB0_RPTR %#010x -> %#010x, wb_rptr %u -> %u, "
                   "target wptr=%u, %llu us",
                   rptr_before_reg, rptr_reg, rptr_before_wb, rptr_wb,
                   target, (unsigned long long)elapsed);
            return kIOReturnSuccess;
        }
        IODelay((unsigned int)step_us);
        elapsed += step_us;
    }

    cp.probe_rptr = rptr_reg;
    CP_LOG("ring_fetch_probe: AFTER — TIMEOUT after %llu us. "
           "CP_RB0_RPTR %#010x -> %#010x, wb_rptr %u -> %u, target wptr=%u "
           "(rptr==0 means the CP never fetched a single dword)",
           (unsigned long long)elapsed, rptr_before_reg, rptr_reg,
           rptr_before_wb, rptr_wb, target);
    cp_dump_state(dev, cp, "ring_fetch_probe timeout");
    return kIOReturnTimeout;
}

// ----- cp_compute_enable: program CP_MEC_RS64_CNTL -----
//
// Direct port of gfx_v12_0_cp_compute_enable (gfx_v12_0.c:2778).
// Brings the RS64 MEC out of reset by clearing PIPE{0..3}_RESET and
// MEC_INVALIDATE_ICACHE, then sets PIPE{0..3}_ACTIVE and clears
// MEC_HALT. Disable path inverts all bits.
//
// Audit-7 #2 — without this, MEC pipes stay in reset and any compute
// queue (KCQ / KIQ if not MES-driven) goes nowhere.
kern_return_t
cp_compute_enable(const DeviceContext &dev, bool enable)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    // gc_12_0_0_offset.h: regCP_MEC_RS64_CNTL_BASE_IDX 1.
    const uint32_t r = CP_REG(dev, CP_MEC_RS64_CNTL);
    if (!dev.ip.isResolved(IPBlock::GC, CPRegBaseIdx::CP_MEC_RS64_CNTL)) {
        CP_LOG("cp_compute_enable: GC BASE_IDX %d unresolved — "
               "CP_MEC_RS64_CNTL would land on an unrelated register; "
               "refusing", CPRegBaseIdx::CP_MEC_RS64_CNTL);
        return kIOReturnNotReady;
    }

    // Diagnostic (not in the reference): log the register at BOTH GC
    // segments before writing. Hardware on this card reads 0 at GC[0] and
    // 0x40030000 at GC[1], and a write at GC[0] did not stick — which is
    // what the header's BASE_IDX 1 predicts. Kept as a regression witness.
    {
        const uint32_t alt =
            SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, 0, CPRegs::CP_MEC_RS64_CNTL);
        CP_LOG("CP_MEC_RS64_CNTL: GC[1]+0x2904=dw %#x -> %#010x (USED, "
               "header BASE_IDX 1), GC[0]+0x2904=dw %#x -> %#010x (ignored)",
               r, RREG32(dev, r), alt, RREG32(dev, alt));
    }

    // gfx_v12_0.c:2782-2803 — full RMW for every field.
    uint32_t data = RREG32(dev, r);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_INVALIDATE_ICACHE,
                         enable ? 0 : 1);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE0_RESET,
                         enable ? 0 : 1);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE1_RESET,
                         enable ? 0 : 1);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE2_RESET,
                         enable ? 0 : 1);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE3_RESET,
                         enable ? 0 : 1);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE0_ACTIVE,
                         enable ? 1 : 0);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE1_ACTIVE,
                         enable ? 1 : 0);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE2_ACTIVE,
                         enable ? 1 : 0);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE3_ACTIVE,
                         enable ? 1 : 0);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_HALT,
                         enable ? 0 : 1);
    cp_wreg_logged(dev, "CP_MEC_RS64_CNTL", r, data);

    // gfx_v12_0.c:2807 — short settling delay so MEC sees the
    // write before any queue programming follows. Upstream: udelay(50).
    // The reference had to round this up to IOSleep(1) (DriverKit has no
    // IODelay); the kernel does, so this is upstream-exact.
    IODelay(50);

    CP_LOG("CP_MEC_RS64_CNTL %s: value=%#010x (readback=%#010x)",
           enable ? "enabled (MEC running)" : "disabled (MEC halted)",
           data, RREG32(dev, r));
    return kIOReturnSuccess;
}

// ----- cp_set_doorbell_range: program GFX + MEC doorbell windows -----
//
// Direct port of gfx_v12_0_cp_set_doorbell_range (gfx_v12_0.c:2954).
// Both ranges accept any in-window doorbell. We pick conservative
// caller-set bounds: GFX gets [cp.doorbell_index, cp.doorbell_index+1)
// (one slot); MEC (compute) gets the caller's window so MES-assigned
// compute doorbells fall inside it.
//
// Audit-7 #4 — without CP_MEC_DOORBELL_RANGE_*, compute queues built
// via MES SET_HW_RESOURCES (or KIQ MAP_QUEUES) will be rejected by
// the CP doorbell aperture filter and never wake up the MEC.
kern_return_t
cp_set_doorbell_range(const DeviceContext &dev, const CPContext &cp,
                      uint32_t mec_first_doorbell,
                      uint32_t mec_last_doorbell)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    // gfx_v12_0.c:2957-2960 — GFX ring window. Upstream encodes the
    // doorbell index as `(idx * 2) << 2` (GFX12 8-byte doorbell
    // stride × shift-into-DOORBELL_RANGE_LOWER field). Total
    // multiplier: × 8 (byte offset). Same formula for the MEC.
    cp_wreg_logged(dev, "CP_RB_DOORBELL_RANGE_LOWER",
                   CP_REG(dev, CP_RB_DOORBELL_RANGE_LOWER),
                   (cp.doorbell_index * 2u) << 2);
    cp_wreg_logged(dev, "CP_RB_DOORBELL_RANGE_UPPER",
                   CP_REG(dev, CP_RB_DOORBELL_RANGE_UPPER),
                   (dev.doorbell.index.gfx_userqueue_end * 2u) << 2);

    // gfx_v12_0.c:2963-2966 — MEC window. Same encoding as GFX
    // (× 8 byte offset).  Audit-7 #4.
    cp_wreg_logged(dev, "CP_MEC_DOORBELL_RANGE_LOWER",
                   CP_REG(dev, CP_MEC_DOORBELL_RANGE_LOWER),
                   (mec_first_doorbell * 2u) << 2);
    cp_wreg_logged(dev, "CP_MEC_DOORBELL_RANGE_UPPER",
                   CP_REG(dev, CP_MEC_DOORBELL_RANGE_UPPER),
                   (mec_last_doorbell  * 2u) << 2);

    CP_LOG("doorbell ranges: GFX=[%#x..%#x] MEC=[%#x..%#x]",
           cp.doorbell_index, dev.doorbell.index.gfx_userqueue_end,
           mec_first_doorbell, mec_last_doorbell);
    return kIOReturnSuccess;
}

// ----- cp_enable: toggle CP_ME_CNTL.{ME_HALT,PFP_HALT} -----
//
// Direct port of gfx_v12_0_cp_gfx_enable (gfx_v12_0.c:2332).
// Both PFP and ME halts must drop together — clearing only ME_HALT
// leaves PFP_HALT set, so the PFP never fetches packets into the ME's
// queue and ME_HALT=0 looks active but the ring is dead.
//
// Audit-7 #1 — bit positions sourced from gc_12_0_0_sh_mask.h:
// PFP_HALT=0x1a, ME_HALT=0x1c.
//
// ADDRESSING: gc_12_0_0_offset.h says regCP_ME_CNTL = 0x0803 BASE_IDX 1.
// The reference wrote it at GC[0], where the readback "matched" — i.e. it
// was writing and reading some other register entirely while PFP/ME stayed
// halted. Fixed to the header's segment.
kern_return_t
cp_enable(const DeviceContext &dev, bool enable)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC, CPRegBaseIdx::CP_ME_CNTL)) {
        CP_LOG("cp_enable: GC BASE_IDX %d unresolved — CP_ME_CNTL would "
               "land on an unrelated register; refusing",
               CPRegBaseIdx::CP_ME_CNTL);
        return kIOReturnNotReady;
    }
    const uint32_t r = CP_REG(dev, CP_ME_CNTL);

    // gfx_v12_0.c:2368-2372 — RMW.
    uint32_t tmp = RREG32(dev, r);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_HALT,  enable ? 0 : 1);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_HALT, enable ? 0 : 1);
    cp_wreg_logged(dev, "CP_ME_CNTL", r, tmp);

    // gfx_v12_0.c:2374-2382 — wait for CP_STAT to read 0 (adev->usec_timeout
    // = 100000 us in 1 us steps). Upstream only DRM_ERRORs on timeout and
    // still returns 0; we do the same so the caller keeps going and the log
    // carries the evidence. This wait was missing from the reference: the
    // CP is not actually idle/settled the instant the halt bits change.
    {
        const uint32_t stat_reg = CP_REG(dev, CP_STAT);
        uint32_t stat = RREG32(dev, stat_reg);
        uint32_t waited_us = 0;
        while (stat != 0 && waited_us < 100000u) {
            IODelay(1);
            waited_us++;
            stat = RREG32(dev, stat_reg);
        }
        if (stat != 0)
            CP_LOG("cp_enable(%s): CP_STAT did not reach 0 within %u us "
                   "(CP_STAT=%#010x) — upstream logs this and continues",
                   enable ? "unhalt" : "halt", waited_us, stat);
        else
            CP_LOG("cp_enable(%s): CP_STAT reached 0 after %u us",
                   enable ? "unhalt" : "halt", waited_us);
    }

    CP_LOG("CP_ME_CNTL %s (ME_HALT=%u PFP_HALT=%u, value=%#010x, "
           "readback=%#010x)",
           enable ? "running" : "halted",
           enable ? 0u : 1u, enable ? 0u : 1u, tmp, RREG32(dev, r));
    return kIOReturnSuccess;
}

// ----- cp_kick_doorbell: write wptr to the BAR2 doorbell slot -----
//
// gfx_v12_0_ring_set_wptr_gfx (gfx_v12_0.c, use_doorbell branch):
//     atomic64_set((atomic64_t *)ring->wptr_cpu_addr, ring->wptr);
//     WDOORBELL64(ring->doorbell_index, ring->wptr);
// Both stores are 64 bits wide. amdgpu_mm_wdoorbell64 indexes a u32*, so
// the byte offset is doorbell_index * 4 (kCPDoorbellStride) and the store
// covers 8 bytes. Writing only the low half (what this did before) leaves
// the doorbell's upper dword untouched — the CP samples the pair as one
// 64-bit wptr, so a half-write is not guaranteed to register at all.
kern_return_t
cp_kick_doorbell(const DeviceContext &dev, const CPContext &cp)
{
    if (!cp.inited) return kIOReturnNotReady;
    if (dev.bar2 == nullptr) return kIOReturnNotAttached;
    if (cp.wb_cpu == nullptr) return kIOReturnNotReady;

    // 64-bit wptr shadow, matching atomic64_set on wptr_cpu_addr. Our
    // wptr never exceeds the ring size, so the upper dword is 0 — but it
    // has to be *written*, not merely assumed zero.
    const uint64_t wptr64 = static_cast<uint64_t>(cp.wptr);
    *reinterpret_cast<volatile uint64_t *>(
        static_cast<uint8_t *>(cp.wb_cpu) + kCPWBOffsetWptr) = wptr64;

    // Make the staged PM4 dwords + the wptr shadow visible to the device
    // before the doorbell write (deviation 3).
    sysmem_wmb();
    const uint64_t off = static_cast<uint64_t>(cp.doorbell_index) *
                         kCPDoorbellStride;
    WDOORBELL64(dev, off, wptr64);
    CP_LOG("doorbell rung: slot=%u bar2_off=%#llx wptr=%llu (64-bit store, "
           "readback=%#010x)",
           cp.doorbell_index, (unsigned long long)off,
           (unsigned long long)wptr64, RDOORBELL32(dev, off));
    return kIOReturnSuccess;
}

// ----- cp_submit_eop_test: end-to-end submit + fence wait -----
kern_return_t
cp_submit_eop_test(const DeviceContext &dev, CPContext &cp,
                   uint64_t timeout_us, uint32_t *outFence)
{
    if (outFence) *outFence = 0;
    if (!cp.inited) return kIOReturnNotReady;

    uint32_t fence = cp_emit_eop_fence(cp);
    if (fence == 0) return kIOReturnInternalError;

    kern_return_t r = cp_kick_doorbell(dev, cp);
    if (r != kIOReturnSuccess) return r;

    const uint64_t kStepUs = 1000;
    uint64_t elapsed = 0;
    while (elapsed < timeout_us) {
        sysmem_rmb();
        uint64_t observed = *cp.fence_cpu;
        // Linux uses a 32-bit fence in the low half on simple paths.
        if ((observed & 0xFFFFFFFFu) == fence) {
            if (outFence) *outFence = fence;
            CP_LOG("EOP fence %u observed after %llu us (rptr=%u)",
                   fence, (unsigned long long)elapsed, *cp.rptr_cpu);
            return kIOReturnSuccess;
        }
        IOSleep(1);
        elapsed += kStepUs;
    }
    CP_LOG("EOP fence %u timeout (observed=%#llx, wb_rptr=%u, wptr=%u)",
           fence, (unsigned long long)*cp.fence_cpu, *cp.rptr_cpu, cp.wptr);
    cp_dump_state(dev, cp, "cp_submit_eop_test timeout");
    return kIOReturnTimeout;
}

// ----- cp_kiq_smoke_test -----
//
// First-PM4 smoke test on the KIQ ring. Builds PACKET3_NOP +
// PACKET3_RELEASE_MEM and verifies the CP firmware writes
// `expected_fence_value` to a VRAM-resident fence slot.
//
// "KIQ ring" in the uni-MES architecture is the CP GFX RB0 ring stored
// in CPContext — that's the CP-managed kernel ring with a PM4-fetching
// CP front-end. MES "owns" it via RLC_CP_SCHEDULERS but the ring buffer
// and doorbell live in CPContext.
//
// PACKET3 macro:  0xC0000000 | (opcode << 8) | ((count & 0x3FFF) << 16)
//
// RELEASE_MEM body (SEVEN dwords after the header; count=6 means
// payload_dwords - 1, so the packet spans count + 2 = 8 dwords — the
// trailing ctxid dword is part of the packet, not padding):
//   DW1: CACHE_FLUSH_AND_INV_TS_EVENT(20) | (EVENT_INDEX(5) << 8)
//        = 0x14 | (0x5 << 8) = 0x514
//   DW2: (DATA_SEL(1) << 29) | (INT_SEL(0) << 24) | (DST_SEL(1) << 16)
//   DW3: fence_gpu_va & 0xFFFFFFFC  (dword-aligned)
//   DW4: (fence_gpu_va >> 32) & 0xFFFF
//   DW5: expected_fence_value (data_lo)
//   DW6: 0 (data_hi)
//   DW7: 0 (ctxid)
//
kern_return_t
cp_kiq_smoke_test(DeviceContext &dev,
                  CPContext &cp,
                  bool mes_kiq_armed,
                  GMCContext &gmc,
                  uint32_t expected_fence_value,
                  uint32_t timeout_us,
                  uint64_t *out_elapsed_us,
                  uint64_t *out_fence_gpu_va,
                  uint32_t *out_observed_fence)
{
    if (out_elapsed_us)     *out_elapsed_us     = 0;
    if (out_fence_gpu_va)   *out_fence_gpu_va   = 0;
    if (out_observed_fence) *out_observed_fence = 0;

    // (1) Bail if CP/MES KIQ aren't initialized.
    if (!cp.inited) {
        CP_LOG("cp_kiq_smoke: CP storage not initialized");
        return kIOReturnNotReady;
    }
    if (!mes_kiq_armed) {
        // MES SCHED arms the KIQ via set_hw_resources; without it the
        // CP firmware may not have a valid scheduler context.
        CP_LOG("cp_kiq_smoke: MES SCHED pipe not enabled (KIQ not armed)");
        return kIOReturnNotReady;
    }
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    CP_LOG("cp_kiq_smoke: starting (expected=%#x, timeout=%u us)",
           expected_fence_value, timeout_us);

    // (2) Allocate a 64-byte fence target in VRAM. The allocator yields a
    // gpu_va = vram_start + offset_in_window.
    VRAMAllocation fence_alloc{};
    if (!gmc.vram_alloc.alloc(64, kASPageSize, &fence_alloc)) {
        CP_LOG("cp_kiq_smoke: VRAM fence alloc failed");
        return kIOReturnNoMemory;
    }
    const uint64_t fence_gpu_va    = fence_alloc.gpu_va;
    const uint64_t fence_vram_off  = fence_gpu_va - gmc.vram_start;
    if (out_fence_gpu_va) *out_fence_gpu_va = fence_gpu_va;
    CP_LOG("cp_kiq_smoke: fence_target @ gpu_va=%#llx vram_off=%#llx",
           (unsigned long long)fence_gpu_va,
           (unsigned long long)fence_vram_off);

    // (3) Pre-fill the fence dword with 0xCAFEBABE so a "no write"
    // outcome is distinguishable from accidental zero.
    bar0_memset_vram(dev, fence_vram_off, 0xCAFEBABEu, 4);
    // HDP flush so the GPU sees the pre-fill (paranoid — RELEASE_MEM
    // overwrites it anyway, but keeps the readback clean).
    amdgpu_hdp_flush(dev);

    // (4) Build the PM4 packet sequence in a local CPU buffer.
    //
    // Header for NOP: PACKET3(IT_NOP=0x10, count=0x3FFF) = 0xFFFF1000
    //   (the one-dword filler form; see cp_pm4_gfx12.h).
    // Header for RELEASE_MEM: PACKET3(opcode=0x49, count=6) = 0xC0064900,
    //   which spans 8 dwords (header + 7 payload).
    uint32_t pkt[16];
    uint32_t n = 0;

    // -- NOP --
    // 0xFFFF1000 = PACKET3(IT_NOP=0x10, COUNT=0x3FFF), the one-dword
    // filler form (cp_pm4_gfx12.h). This used to be 0xC0001000 =
    // PACKET3(NOP, 0), which is a TWO-dword packet: the CP would have
    // eaten the RELEASE_MEM header that follows as the NOP's payload.
    pkt[n++] = cp_p3_nop1();

    // -- RELEASE_MEM (count=6 → 8 dwords total = header + 7 payload) --
    pkt[n++] = cp_p3(kP3_RELEASE_MEM, 6);   // 0xC0064900
    // DW1: event_type=0x14 (CACHE_FLUSH_AND_INV_TS_EVENT)
    //      event_index=5 (EOP) at bit 8
    pkt[n++] = 0x00000514u;
    // DW2: DATA_SEL=1 (immediate 32-bit) at bit 29
    //      INT_SEL=0 (no interrupt) at bit 24
    //      DST_SEL=1 (memory) at bit 16
    pkt[n++] = (1u << 29) | (0u << 24) | (1u << 16);  // = 0x20010000
    // DW3: fence_gpu_va lo, dword-aligned
    pkt[n++] = static_cast<uint32_t>(fence_gpu_va & 0xFFFFFFFCu);
    // DW4: fence_gpu_va hi, low 16 bits only (RELEASE_MEM addr_hi field
    // is 16 bits per upstream IT_RELEASE_MEM encoding).
    pkt[n++] = static_cast<uint32_t>((fence_gpu_va >> 32) & 0xFFFFu);
    // DW5: data_lo = expected_fence_value
    pkt[n++] = expected_fence_value;
    // DW6: data_hi = 0
    pkt[n++] = 0;
    // DW7: ctxid / pad = 0. count=6 spans 7 payload dwords (8 total);
    // this dword was missing, so the CP would have consumed the *next*
    // ring dword as the packet's last word.
    pkt[n++] = 0;

    // (5) The ring lives in GART-mapped sysmem (cp.ring_cpu is the kernel
    // VA of the same pages the GPU reaches through the GART PTEs written
    // by cp_alloc_storage). Direct stores are correct.
    if (cp.ring_cpu == nullptr || cp.ring_size_dwords == 0) {
        CP_LOG("cp_kiq_smoke: CP ring CPU mapping unavailable");
        return kIOReturnNotReady;
    }
    if (n > cp.ring_size_dwords / 2) {
        CP_LOG("cp_kiq_smoke: %u dwords exceeds half-ring %u",
               n, cp.ring_size_dwords / 2);
        return kIOReturnNoSpace;
    }

    // (6) Write packets at the current software wptr; update wptr.
    auto *ring = static_cast<uint32_t *>(cp.ring_cpu);
    const uint32_t start_wptr = cp.wptr;
    for (uint32_t i = 0; i < n; i++) {
        ring[(cp.wptr + i) & cp.ring_ptr_mask] = pkt[i];
    }
    cp.wptr = (cp.wptr + n) & cp.ring_ptr_mask;
    CP_LOG("cp_kiq_smoke: %u dwords written to KIQ ring @ wptr=%u "
           "(start=%u, new=%u, ring_mc=%#llx)",
           n, start_wptr, start_wptr, cp.wptr,
           (unsigned long long)cp.ring_bus);

    // (7) HDP flush — write + readback per upstream amdgpu_hdp_flush.
    // Drains posted writes so CP sees fresh ring contents.
    amdgpu_hdp_flush(dev);

    // (8) Update wptr shadow + ring the GFX ring's doorbell on BAR2.
    // Goes through cp_kick_doorbell so there is exactly one place that
    // knows the shadow and the doorbell are 64-bit stores (see that
    // function and gfx_v12_0_ring_set_wptr_gfx).
    {
        kern_return_t dr = cp_kick_doorbell(dev, cp);
        if (dr != kIOReturnSuccess) return dr;
    }

    // (9) Poll the fence slot up to timeout_us in 100 us steps. The
    // reference approximated 100 us with a busy XOR loop because
    // DriverKit only offers millisecond IOSleep; IODelay is exact here.
    const uint32_t step_us = 100;
    uint32_t elapsed_us = 0;
    uint32_t observed = 0;
    while (elapsed_us < timeout_us) {
        observed = RVRAM32_via_mm(dev, fence_vram_off);
        if (observed == expected_fence_value) {
            if (out_observed_fence) *out_observed_fence = observed;
            if (out_elapsed_us)     *out_elapsed_us     = elapsed_us;
            CP_LOG("cp_kiq_smoke: fence wait expected=%#x observed=%#x "
                   "in %u us (rptr=%u)",
                   expected_fence_value, observed, elapsed_us,
                   *cp.rptr_cpu);
            return kIOReturnSuccess;
        }
        IODelay(step_us);
        elapsed_us += step_us;
    }

    if (out_observed_fence) *out_observed_fence = observed;
    if (out_elapsed_us)     *out_elapsed_us     = elapsed_us;
    CP_LOG("cp_kiq_smoke: fence wait TIMEOUT expected=%#x observed=%#x "
           "after %u us (bar0 readback=%#010x, rptr=%u, wptr=%u)",
           expected_fence_value, observed, elapsed_us,
           RBAR0_32(dev, fence_vram_off), *cp.rptr_cpu, cp.wptr);
    return kIOReturnTimeout;
}

// ----- cp_init_full: BringupStage::CPInit entry -----
//
// Mirrors gfx_v12_0_hw_init's PSP branch (gfx_v12_0.c:3810-3849) followed
// by gfx_v12_0_cp_resume (3522) on the `!amdgpu_async_gfx_ring` path, with
// the legacy direct-load branches stripped (the PSP already loaded and
// placed the RS64 images).
//
// The order below is upstream's, and the ordering questions the previous
// build got wrong are called out where they occur:
//
//   hw_init:3814   if (load_type == PSP) config_gfx_rs64()   <-- WAS MISSING
//   hw_init:3833   constants_init()
//   cp_resume:3541 cp_set_doorbell_range()
//   cp_resume:3544 if (amdgpu_async_gfx_ring) { compute_enable(true);
//                                               gfx_enable(true); }
//                  ... the async branch enables the CP *before* the ring
//                  is programmed; we are the !async path, so we do not.
//   cp_resume:3565 kcq_resume() -> gfx_v12_0.c:3118-3119:
//                     if (!amdgpu_async_gfx_ring) cp_compute_enable(true);
//                  i.e. on the !async path the MEC comes up BEFORE
//                  cp_gfx_resume, not after the GFX unhalt.
//   cp_resume:3570 cp_gfx_resume() = our cp_hqd_program(), which ends with
//                  CP_RB_ACTIVE=1 (2796) then cp_gfx_set_doorbell (2798)
//                  then switch_pipe (2802) — so **CP_RB_ACTIVE and the
//                  doorbell are both programmed BEFORE the CP is
//                  unhalted**, and the unhalt happens inside
//                  cp_gfx_start (2707). Our previous order (hqd_program,
//                  then cp_enable, then cp_compute_enable) had the
//                  RB_ACTIVE/doorbell relationship right and the
//                  compute/gfx relationship inverted.
kern_return_t
cp_init_full(DeviceContext &dev, GMCContext &gmc, CPContext &cp,
             bool legacy_rb)
{
    kern_return_t r = cp_alloc_storage(dev, gmc, cp);
    if (r != kIOReturnSuccess) return r;

    // Pin doorbell index for the GFX ring from the doorbell_index map.
    // gfx_ring0 = 0 for RDNA4 (gfx1201). Real driver would allocate
    // from a doorbell ID pool.
    cp.doorbell_index = dev.doorbell.index.gfx_ring0;
    CP_LOG("cp_init_full: doorbell_index=%u (bar2=%p size=%llu)",
           cp.doorbell_index, dev.bar2,
           (unsigned long long)dev.bar2Size);

    // Skip MMIO programming if IP base isn't resolved (e.g. discovery
    // hasn't run yet). Storage stays staged.
    if (!dev.ip.isResolved(IPBlock::GC)) {
        CP_LOG("CP storage staged; GC IP base unresolved — HQD/CP enable deferred");
        return kIOReturnSuccess;
    }

    cp_dump_state(dev, cp, "cp_init_full entry");

    // (1) Halt both front-ends before touching their configuration.
    // Upstream's PSP path arrives here already halted (RLC autoload left
    // PFP/ME/MEC halted and gfx_v12_0_cp_gfx_load_microcode's explicit
    // cp_gfx_enable(false) is on the DIRECT path only, gfx_v12_0.c:2682).
    // Halting explicitly is a superset and makes the log unambiguous.
    cp_enable(dev, false);
    cp_compute_enable(dev, false);

    // (2) RS64 instruction/data cache bases. Not in upstream's PSP path —
    // upstream assumes the PSP set them. On this card it does not, and
    // build 0.0.14 proved it: PFP/ME started at PC 0x3000 and faulted at
    // VA page 0 (GCVM 0x0d33, client 6 CPG). MUST run before the pipe
    // resets in cp_config_gfx_rs64 and before any unhalt, because
    // programming an IC base invalidates that core's L1 I$.
    r = cp_config_rs64_caches(dev, cp);
    if (r != kIOReturnSuccess) {
        CP_LOG("cp_config_rs64_caches failed: %#x — refusing to unhalt the "
               "CP with a zero instruction-cache base", r);
        return r;
    }

    // (3) gfx_v12_0_hw_init:3813-3814 —
    //         if (adev->firmware.load_type == AMDGPU_FW_LOAD_PSP)
    //                 gfx_v12_0_config_gfx_rs64(adev);
    // THE missing step. Programs the RS64 PFP/ME/MEC program counters
    // from the firmware headers and pulses every pipe reset. Without it
    // the cores below start at PC 0.
    r = cp_config_gfx_rs64(dev, cp);
    if (r != kIOReturnSuccess) {
        CP_LOG("cp_config_gfx_rs64 failed: %#x — refusing to unhalt the CP "
               "with an unprogrammed program counter (that is what wedges "
               "PFP/ME and faults the GFX hub at VA 0)", r);
        return r;
    }

    // (4) gfx_v12_0_hw_init:3833 — constants_init. Upstream runs it in
    // hw_init between gfxhub_enable and rlc_resume; the ladder also calls
    // gfx_constants_init at GFXInit. Kept here so CPInit stays
    // self-contained and the constants are in place before the CP fetches.
    r = gfx_constants_init(dev);
    if (r != kIOReturnSuccess) {
        CP_LOG("gfx_constants_init failed: %#x", r);
        return r;
    }

    // (5) gfx_v12_0_cp_resume:3541 — cp_set_doorbell_range, before any
    // queue resume. We default the MEC window to [0x10, 0x100) to cover
    // the MES SCHED + KIQ + KFD-style compute doorbells handed out later.
    // (cp_v12_0 only owns RB0; the MES module owns the compute slots.)
    // gfx_v12_0_cp_set_doorbell_range (gfx_v12_0.c:2957-2966): the compute
    // window is [kiq .. userqueue_end] and the graphics window is
    // [gfx_ring0 .. gfx_userqueue_end]; both come from the doorbell map now
    // rather than the hand-picked 0x10..0x100 the reference used.
    r = cp_set_doorbell_range(dev, cp,
                              /*mec_first_doorbell=*/dev.doorbell.index.kiq,
                              /*mec_last_doorbell =*/dev.doorbell.index.userqueue_end);
    if (r != kIOReturnSuccess) {
        CP_LOG("cp_set_doorbell_range failed: %#x", r);
        return r;
    }

    // (6) gfx_v12_0_cp_resume:3546-3549 (amdgpu_async_gfx_ring=1, the
    // configuration real hardware runs) — cp_compute_enable(true) then
    // cp_gfx_enable(true), with NO queue programmed yet. On the legacy
    // !async path (gfx_v12_0_kcq_resume:3118) only the MEC is enabled here.
    cp_compute_enable(dev, true);

    if (!legacy_rb) {
        // (7) async path: unhalt PFP/ME now. The kernel GFX queue is created
        // later from a v12_gfx_mqd and mapped by the MES (ADD_QUEUE with
        // map_legacy_kq=1) — in this ladder at PM4Test, after MESInit — and
        // only then does cp_gfx_start write CP_MAX_CONTEXT / CP_DEVICE_ID
        // (gfx_v12_0_cp_async_gfx_ring_resume). The CP_RB0_* registers are
        // never written on this path; stage 16 runs 1-3 showed the RS64
        // firmware halting itself on the first doorbell when they were.
        r = cp_enable(dev, true);
        if (r != kIOReturnSuccess) return r;
        cp_dump_state(dev, cp, "after cp_gfx_enable(true), no queue mapped");
        CP_LOG("CP front-ends running, GFX queue NOT mapped yet — PM4Test will build "
               "the v12_gfx_mqd and have the MES map it (navi48-cp-legacy-rb=1 "
               "selects the old CP_RB0_* path instead)");
        CP_LOG("CP ready: doorbell %u reserved for the GFX ring (ring_mc=%#llx), "
               "MEC pipes active, caches_configured=%d, rs64_configured=%d",
               cp.doorbell_index, (unsigned long long)cp.ring_bus,
               cp.caches_configured ? 1 : 0, cp.rs64_configured ? 1 : 0);
        return kIOReturnSuccess;
    }

    // ---- legacy (!amdgpu_async_gfx_ring) path, navi48-cp-legacy-rb=1 ----
    // (7) gfx_v12_0_cp_resume:3570 -> cp_gfx_resume (2748). Ends with
    // CP_RB_ACTIVE=1 (2796), cp_gfx_set_doorbell (2798) and switch_pipe
    // (2802): the ring is fully described and its doorbell armed while
    // the CP is still halted.
    r = cp_hqd_program(dev, cp);
    if (r != kIOReturnSuccess) return r;

    // (8) gfx_v12_0_cp_gfx_resume:2805 -> cp_gfx_start (2699):
    // CP_MAX_CONTEXT, CP_DEVICE_ID, then cp_gfx_enable(true). This is the
    // moment the GFX front-end starts executing.
    r = cp_gfx_start(dev, cp, /*legacy_unhalt=*/true);
    if (r != kIOReturnSuccess) return r;

    cp_dump_state(dev, cp, "after cp_gfx_start");

    // (9) Diagnostic, not upstream: prove the CP fetches before anything
    // depends on it. 16 one-dword NOPs, doorbell, poll CP_RB0_RPTR.
    // Non-fatal on its own — the ladder's PM4Test is the gate — but the
    // log line it prints is the first unambiguous answer to "does the
    // command processor run our microcode".
    kern_return_t pr = cp_ring_fetch_probe(dev, cp, /*nops=*/16,
                                           /*timeout_us=*/500000);
    if (pr != kIOReturnSuccess)
        CP_LOG("cp_init_full: ring fetch probe FAILED %#x — the CP is not "
               "consuming the GFX ring; PM4Test will time out", pr);

    CP_LOG("CP ready (legacy CP_RB0_* path): GFX ring active at doorbell %u "
           "(ring_mc=%#llx), MEC pipes active, caches_configured=%d, "
           "rs64_configured=%d, fetch_proven=%d",
           cp.doorbell_index, (unsigned long long)cp.ring_bus,
           cp.caches_configured ? 1 : 0, cp.rs64_configured ? 1 : 0,
           cp.fetch_proven ? 1 : 0);
    return kIOReturnSuccess;
}


//==================================================================
//  Kernel compute queue — gfx_v12_0_kcq_* / gfx_v12_0_compute_mqd_init
//==================================================================

kern_return_t
cp_compute_queue_alloc(DeviceContext &dev, GMCContext &gmc, ComputeQueue &cq,
                       uint32_t pipe, uint32_t queue)
{
    if (cq.inited) return kIOReturnSuccess;
    cq.me = 1; cq.pipe = pipe; cq.queue = queue;

    void *cpu = nullptr;
    kern_return_t r = cp_alloc_dma_block(dev, gmc, kCPRingDefaultBytes, "kcq ring", &cq.ring_sys, &cq.ring_bus, &cpu);
    if (r != kIOReturnSuccess) return r;
    cq.ring_cpu = cpu;
    cq.ring_size_dwords = kCPRingDefaultBytes / 4;
    cq.ring_ptr_mask    = cq.ring_size_dwords - 1;

    r = cp_alloc_dma_block(dev, gmc, kGFX_MQDPageBytes, "kcq mqd", &cq.mqd_sys, &cq.mqd_bus, &cpu);
    if (r != kIOReturnSuccess) return r;
    cq.mqd_cpu = cpu;

    r = cp_alloc_dma_block(dev, gmc, ComputeMqdOff::kMecHpdBytes, "kcq eop", &cq.eop_sys, &cq.eop_bus, &cpu);
    if (r != kIOReturnSuccess) return r;

    r = cp_alloc_dma_block(dev, gmc, kCPWBPageBytes, "kcq wb", &cq.wb_sys, &cq.wb_bus, &cpu);
    if (r != kIOReturnSuccess) return r;
    cq.wb_cpu = cpu;

    // Same write-back layout as the GFX ring so the two are easy to compare.
    auto *wb = static_cast<uint8_t *>(cq.wb_cpu);
    cq.rptr_cpu  = reinterpret_cast<volatile uint32_t *>(wb + kCPWBOffsetRptr);
    cq.wptr_cpu  = reinterpret_cast<volatile uint32_t *>(wb + kCPWBOffsetWptr);
    cq.fence_cpu = reinterpret_cast<volatile uint64_t *>(wb + kCPWBOffsetFence);
    cq.rptr_gpu_addr  = cq.wb_bus + kCPWBOffsetRptr;
    cq.wptr_gpu_addr  = cq.wb_bus + kCPWBOffsetWptr;
    cq.fence_gpu_addr = cq.wb_bus + kCPWBOffsetFence;

    // One of the compute doorbells. These sit inside CP_MEC_DOORBELL_RANGE,
    // which cp_set_doorbell_range programmed as [kiq .. userqueue_end].
    cq.doorbell_index = dev.doorbell.index.mec_ring[queue & 7];
    cq.wptr = 0;
    cq.fence_counter = 0;
    cq.inited = true;

    CP_LOG("compute queue: me=%u pipe=%u queue=%u doorbell=%#x | ring_mc=%#llx (%u dw) "
           "mqd_mc=%#llx eop_mc=%#llx wb_mc=%#llx",
           cq.me, cq.pipe, cq.queue, cq.doorbell_index,
           (unsigned long long)cq.ring_bus, cq.ring_size_dwords,
           (unsigned long long)cq.mqd_bus, (unsigned long long)cq.eop_bus,
           (unsigned long long)cq.wb_bus);
    return kIOReturnSuccess;
}

void
cp_compute_queue_release(GMCContext &gmc, ComputeQueue &cq)
{
    (void)gmc;
    sysmem_free(cq.ring_sys); sysmem_free(cq.mqd_sys);
    sysmem_free(cq.eop_sys);  sysmem_free(cq.wb_sys);
    cq = ComputeQueue{};
}

kern_return_t
cp_compute_mqd_init(const DeviceContext &dev, ComputeQueue &cq)
{
    if (!cq.inited || cq.mqd_cpu == nullptr) return kIOReturnNotReady;
    namespace M = ComputeMqdOff;

    uint32_t *m = static_cast<uint32_t *>(cq.mqd_cpu);
    memset(m, 0, M::kDwords * 4u);

    m[M::header]                         = 0xC0310800u;
    m[M::compute_pipelinestat_enable]    = 0x00000001u;
    m[M::compute_static_thread_mgmt_se0] = 0xffffffffu;
    m[M::compute_static_thread_mgmt_se1] = 0xffffffffu;
    m[M::compute_static_thread_mgmt_se2] = 0xffffffffu;
    m[M::compute_static_thread_mgmt_se3] = 0xffffffffu;
    m[M::compute_misc_reserved]          = 0x00000007u;

    const uint64_t eop = cq.eop_bus >> 8;
    m[M::cp_hqd_eop_base_addr_lo] = (uint32_t)eop;
    m[M::cp_hqd_eop_base_addr_hi] = (uint32_t)(eop >> 32);
    // EOP_SIZE is log2(dwords) - 1 over the 2 KiB HPD: log2(512) - 1 = 8.
    uint32_t tmp = M::kDefault_CP_HQD_EOP_CONTROL;
    tmp = REG_SET_FIELD(tmp, CP_HQD_EOP_CONTROL, EOP_SIZE, 8u);
    m[M::cp_hqd_eop_control] = tmp;

    tmp = M::kDefault_CP_HQD_PQ_DOORBELL_CTRL;
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_OFFSET, cq.doorbell_index);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_SOURCE, 0);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_HIT, 0);
    m[M::cp_hqd_pq_doorbell_control] = tmp;

    m[M::cp_hqd_dequeue_request] = 0;
    m[M::cp_hqd_pq_rptr]         = M::kDefault_CP_HQD_PQ_RPTR;
    m[M::cp_hqd_pq_wptr_lo]      = 0;
    m[M::cp_hqd_pq_wptr_hi]      = 0;

    m[M::cp_mqd_base_addr_lo] = (uint32_t)(cq.mqd_bus & 0xfffffffcull);
    m[M::cp_mqd_base_addr_hi] = (uint32_t)(cq.mqd_bus >> 32);
    tmp = M::kDefault_CP_MQD_CONTROL;
    tmp = REG_SET_FIELD(tmp, CP_MQD_CONTROL, VMID, 0);
    m[M::cp_mqd_control] = tmp;

    const uint64_t hqd = cq.ring_bus >> 8;
    m[M::cp_hqd_pq_base_lo] = (uint32_t)hqd;
    m[M::cp_hqd_pq_base_hi] = (uint32_t)(hqd >> 32);

    uint32_t log2dw = 0;
    while ((1u << log2dw) < cq.ring_size_dwords) log2dw++;
    tmp = M::kDefault_CP_HQD_PQ_CONTROL;
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, QUEUE_SIZE, log2dw - 1u);
    // RPTR_BLOCK_SIZE is log2(page dwords) - 1 = log2(1024) - 1 = 9.
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, RPTR_BLOCK_SIZE, 9u);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, UNORD_DISPATCH, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, 0);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, PRIV_STATE, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, KMD_QUEUE, 1);
    m[M::cp_hqd_pq_control] = tmp;

    m[M::cp_hqd_pq_rptr_report_addr_lo] = (uint32_t)(cq.rptr_gpu_addr & 0xfffffffcull);
    m[M::cp_hqd_pq_rptr_report_addr_hi] = (uint32_t)(cq.rptr_gpu_addr >> 32) & 0xffffu;
    m[M::cp_hqd_pq_wptr_poll_addr_lo]   = (uint32_t)(cq.wptr_gpu_addr & 0xfffffffcull);
    m[M::cp_hqd_pq_wptr_poll_addr_hi]   = (uint32_t)(cq.wptr_gpu_addr >> 32) & 0xffffu;

    m[M::cp_hqd_vmid] = 0;

    tmp = M::kDefault_CP_HQD_PERSISTENT_STATE;
    tmp = REG_SET_FIELD(tmp, CP_HQD_PERSISTENT_STATE, PRELOAD_SIZE, 0x55);
    m[M::cp_hqd_persistent_state] = tmp;

    tmp = M::kDefault_CP_HQD_IB_CONTROL;
    tmp = REG_SET_FIELD(tmp, CP_HQD_IB_CONTROL, MIN_IB_AVAIL_SIZE, 3);
    m[M::cp_hqd_ib_control] = tmp;

    m[M::cp_hqd_pipe_priority]  = 0;
    m[M::cp_hqd_queue_priority] = 0;

    tmp = 0;
    tmp = REG_SET_FIELD(tmp, CP_HQD_QUANTUM, QUANTUM_EN, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_QUANTUM, QUANTUM_SCALE, 1);
    tmp = REG_SET_FIELD(tmp, CP_HQD_QUANTUM, QUANTUM_DURATION, 1);
    m[M::cp_hqd_quantum] = tmp;

    // hqd_active stays 0: amdgpu_ring_to_mqd_prop only sets it for KIQ, since
    // a MES-mapped queue is activated by the map packet, not by the MQD.
    m[M::cp_hqd_active] = 0;
    m[M::fence_address_lo] = 0;
    m[M::fence_address_hi] = 0;

    sysmem_wmb();
    amdgpu_hdp_flush(dev);

    CP_LOG("compute_mqd_init: mqd_mc=%#llx pq_base=%08x_%08x (ring %#llx >> 8) "
           "pq_control=%#010x (qsize=%u rptr_blk=9) doorbell_ctrl=%#010x (idx %#x) "
           "eop=%08x_%08x eop_control=%#010x persistent=%#010x ib_control=%#010x quantum=%#010x",
           (unsigned long long)cq.mqd_bus,
           m[M::cp_hqd_pq_base_hi], m[M::cp_hqd_pq_base_lo],
           (unsigned long long)cq.ring_bus, m[M::cp_hqd_pq_control], log2dw - 1u,
           m[M::cp_hqd_pq_doorbell_control], cq.doorbell_index,
           m[M::cp_hqd_eop_base_addr_hi], m[M::cp_hqd_eop_base_addr_lo],
           m[M::cp_hqd_eop_control], m[M::cp_hqd_persistent_state],
           m[M::cp_hqd_ib_control], m[M::cp_hqd_quantum]);
    return kIOReturnSuccess;
}

kern_return_t
cp_compute_queue_map_mes(const DeviceContext &dev, ComputeQueue &cq, MESContext &mes)
{
    if (!cq.inited) return kIOReturnNotReady;
    if (!mes.pipe[0].inited || !mes.pipe[0].enabled) {
        CP_LOG("compute_queue_map: the MES scheduler is not running");
        return kIOReturnNotReady;
    }
    sysmem_wmb();
    amdgpu_hdp_flush(dev);

    MESAddQueueInput in;
    memset(&in, 0, sizeof(in));
    in.queue_type      = kMESQueueType_COMPUTE;
    in.pipe_id         = cq.pipe;
    in.queue_id        = cq.queue;
    in.doorbell_offset = cq.doorbell_index;
    in.mqd_addr        = cq.mqd_bus;
    in.wptr_addr       = cq.wptr_gpu_addr;
    in.flags           = kAddQueueFlag_map_legacy_kq;

    CP_LOG("compute_queue_map: MES ADD_QUEUE type=COMPUTE pipe=%u queue=%u doorbell=%#x "
           "mqd_mc=%#llx wptr_mc=%#llx", cq.pipe, cq.queue, cq.doorbell_index,
           (unsigned long long)cq.mqd_bus, (unsigned long long)cq.wptr_gpu_addr);

    kern_return_t r = mes_add_hw_queue(dev, mes, in);
    if (r != kIOReturnSuccess) {
        CP_LOG("compute_queue_map: MES did not ack ADD_QUEUE: %#x", r);
        return r;
    }
    cq.mapped = true;
    sysmem_rmb();
    const volatile uint32_t *m = static_cast<const volatile uint32_t *>(cq.mqd_cpu);
    CP_LOG("compute_queue_map: ACKED. MQD now: active=%u rptr=%u wptr=%u/%u dequeue_req=%u",
           m[ComputeMqdOff::cp_hqd_active], m[ComputeMqdOff::cp_hqd_pq_rptr],
           m[ComputeMqdOff::cp_hqd_pq_wptr_lo], m[ComputeMqdOff::cp_hqd_pq_wptr_hi],
           m[ComputeMqdOff::cp_hqd_dequeue_request]);
    return kIOReturnSuccess;
}

kern_return_t
cp_compute_submit_ib(const DeviceContext &dev, ComputeQueue &cq, uint64_t ib_gpu_va,
                     uint32_t length_dw, uint32_t vmid, uint32_t *out_fence)
{
    if (!cq.inited || cq.fence_cpu == nullptr) return kIOReturnNotReady;
    if (length_dw == 0 || length_dw > 0xFFFFFu) return kIOReturnBadArgument;
    if ((ib_gpu_va & 0x3ull) != 0 || vmid > 15) return kIOReturnBadArgument;

    // A compute ring takes no CONTEXT_CONTROL — that is a graphics-pipeline
    // packet. Two things differ from the GFX ring's INDIRECT_BUFFER, both from
    // gfx_v12_0_ring_emit_ib_compute:
    //   * the control dword carries INDIRECT_BUFFER_VALID;
    //   * the header is NOT compute-typed. The shader-type bit marks compute
    //     work submitted to the GRAPHICS ring (that is what stage 17 needed);
    //     on a compute ring everything is compute already, and setting it is
    //     wrong.
    // Getting this wrong is silent: the MEC takes the doorbell, clears
    // PQ_EMPTY, and never advances RPTR.
    uint32_t pkt[16];
    uint32_t n = 0;
    pkt[n++] = cp_p3(kP3_INDIRECT_BUFFER, 2);
    pkt[n++] = (uint32_t)(ib_gpu_va & 0xFFFFFFFFu);
    pkt[n++] = (uint32_t)(ib_gpu_va >> 32);
    pkt[n++] = kP3_INDIRECT_BUFFER_VALID | (length_dw & 0xFFFFFu) | (vmid << 24);

    // RELEASE_MEM: PACKET3(op, 6) declares 7 payload dwords, so the packet is
    // 8 dwords and every one must be written before the doorbell — dw1, dw2,
    // addr_lo, addr_hi, data_lo, data_hi, and a trailing 0
    // (gfx_v12_0_ring_emit_fence writes exactly these).
    //
    // The first version of this function wrote only 6 payload dwords: it used
    // the trailing 0 as data_hi and never emitted the real trailing dword. The
    // MEC then did precisely what an engine should do with a packet whose
    // header promises more data than the ring holds — it parsed the header,
    // stopped, and left RPTR pinned at the packet's first dword while the
    // indirect buffer ahead of it had already executed. A short packet looks
    // exactly like a hung engine.
    const uint32_t fence = ++cq.fence_counter;
    pkt[n++] = cp_p3_nop1();
    pkt[n++] = cp_p3(kP3_RELEASE_MEM, 6);
    pkt[n++] = pm4_release_mem_dw1();
    pkt[n++] = pm4_release_mem_dw2(kPM4RMDataSel64, kPM4RMIntSelSendInt);
    pkt[n++] = (uint32_t)(cq.fence_gpu_addr & 0xFFFFFFF8u);
    pkt[n++] = (uint32_t)(cq.fence_gpu_addr >> 32);
    pkt[n++] = fence;   // data_lo
    pkt[n++] = 0;       // data_hi (the fence counter is 32-bit)
    pkt[n++] = 0;       // trailing dword required by count = 6

    auto *ring = static_cast<uint32_t *>(cq.ring_cpu);
    for (uint32_t i = 0; i < n; i++) {
        ring[cq.wptr] = pkt[i];
        cq.wptr = (cq.wptr + 1) & cq.ring_ptr_mask;
    }

    const uint64_t wptr64 = cq.wptr;
    *reinterpret_cast<volatile uint64_t *>(
        static_cast<uint8_t *>(cq.wb_cpu) + kCPWBOffsetWptr) = wptr64;
    sysmem_wmb();
    amdgpu_hdp_flush(dev);
    if (dev.bar2 == nullptr) return kIOReturnNotAttached;
    WDOORBELL64(dev, (uint64_t)cq.doorbell_index * kCPDoorbellStride, wptr64);

    if (out_fence) *out_fence = fence;
    return kIOReturnSuccess;
}

kern_return_t
cp_compute_wait_fence(const ComputeQueue &cq, uint32_t fence, uint64_t timeout_us,
                      uint64_t *observed, uint64_t *elapsed_us)
{
    if (!cq.inited || cq.fence_cpu == nullptr) return kIOReturnNotReady;
    constexpr uint64_t kFastUs = 5000;
    uint64_t elapsed = 0, v = 0;
    while (elapsed < timeout_us) {
        sysmem_rmb();
        v = *cq.fence_cpu;
        if ((v & 0xFFFFFFFFull) >= fence) break;
        if (elapsed < kFastUs) { IODelay(1); elapsed += 1; }
        else                   { IOSleep(1); elapsed += 1000; }
    }
    sysmem_rmb();
    v = *cq.fence_cpu;
    if (observed)   *observed = v;
    if (elapsed_us) *elapsed_us = elapsed;
    return ((v & 0xFFFFFFFFull) >= fence) ? kIOReturnSuccess : kIOReturnTimeout;
}

void
cp_dump_compute_hqd(const DeviceContext &dev, const ComputeQueue &cq, const char *where)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return;
    CP_LOG("--- compute HQD me%u/pipe%u/queue%u (%s) ---", cq.me, cq.pipe, cq.queue, where);
    cp_grbm_select(dev, cq.me, cq.pipe, cq.queue, 0);
    cp_rreg_logged(dev, "CP_HQD_ACTIVE",            CP_REG(dev, CP_HQD_ACTIVE));
    cp_rreg_logged(dev, "CP_HQD_PQ_BASE",           CP_REG(dev, CP_HQD_PQ_BASE));
    cp_rreg_logged(dev, "CP_HQD_PQ_CONTROL",        CP_REG(dev, CP_HQD_PQ_CONTROL));
    cp_rreg_logged(dev, "CP_HQD_PQ_RPTR",           CP_REG(dev, CP_HQD_PQ_RPTR));
    cp_rreg_logged(dev, "CP_HQD_PQ_WPTR_LO",        CP_REG(dev, CP_HQD_PQ_WPTR_LO));
    cp_rreg_logged(dev, "CP_HQD_PQ_DOORBELL_CONTROL", CP_REG(dev, CP_HQD_PQ_DOORBELL_CONTROL));
    cp_rreg_logged(dev, "CP_MQD_BASE_ADDR",         CP_REG(dev, CP_MQD_BASE_ADDR));
    cp_grbm_select(dev, 0, 0, 0, 0);
    if (cq.wb_cpu) {
        sysmem_rmb();
        CP_LOG("  sw wptr=%u  wb rptr=%u  wb fence=%#llx", cq.wptr,
               cq.rptr_cpu ? *cq.rptr_cpu : 0u,
               (unsigned long long)(cq.fence_cpu ? *cq.fence_cpu : 0ull));
    }
    CP_LOG("--- end compute HQD (%s) ---", where);
}

} // namespace amdgpu
