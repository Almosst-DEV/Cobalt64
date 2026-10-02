//
//  sdma_v7_0.cpp — SDMA 7.0.1 ring bringup for Navi 48 (RX 9070 XT).
//
//  Ported from lemonade-sdk/mac-amdgpu (MIT) @ commit 3bdeed2:
//      dext/amdgpu/sdma_v7_0.cpp                (the whole file)
//      dext/amdgpu/MacAMDGPU.cpp                (the SDMA halves of the
//                                                LoadFirmware and
//                                                SDMACopyVRAM selectors)
//  which in turn port:
//      drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c
//          (sdma_v7_1_gfx_resume_instance, sdma_v7_1_inst_enable,
//           sdma_v7_1_inst_gfx_stop, sdma_v7_1_get_reg_offset,
//           sdma_v7_0_ring_set_wptr, sdma_v7_0_ring_test_ring)
//
//  The reference file name is kept (mac-amdgpu keeps it for git history;
//  the implementation mirrors sdma_v7_1.c, IP_VERSION(7,0,1)). Register
//  offsets and field shifts come from amdgpu_sdma.h, which selects the
//  gc_12_0_0 or gc_12_1_0 table from the discovered GC IP version.
//
//  =================================================================
//  Deviations from reference
//  =================================================================
//   D1. Kernel, not DriverKit: os_log(%{public}s) → SDMA_LOG (IOLog),
//       IOBufferMemoryDescriptor+IODMACommand → amdgpu::SysMem,
//       dev.pci->MemoryWrite64(bar2MemIndex, …) → WDOORBELL64.
//   D2. The writeback page is sysmem bound into GART with
//       gmc_bind_existing() (x86 has no IOMMU, so the engine reaches
//       system memory only through the GART aperture; the reference
//       could hand the engine a raw DART iova). `wb_bus` still means
//       "the address the engine writes to". If the bind fails we fall
//       back to a VRAM writeback page — the same reasoning the
//       reference already applies to the ring — and CPU access then
//       goes through BAR0 (sdma_wb_read32/sdma_wb_write32).
//   D3. sdma_load_microcode() is new: the reference PSP-loads the SDMA
//       ucode from its userspace LoadFirmware selector and only flips
//       SDMAContext::microcode_loaded. We have no userspace, so the same
//       LOAD_IP_FW submission lives here, in the same position relative
//       to gfx_resume. Firmware bytes are a parameter, so this module
//       never depends on src/fw/fw_table.h.
//   D4. sdma_vram_copy_test() is new: a port of the reference's
//       kMacAMDGPUMethodSDMACopyVRAM selector. The reference builds the
//       pattern in a 16 KB stack array — a kernel stack is 16 KB total,
//       so the pattern is generated on the fly instead, and the two VRAM
//       slots are released afterwards (the reference's allocator had no
//       free(); ours does).
//   D5. The reference's fence poll is a crude spin (`for (i<1000)
//       scratch ^= *fence`) that calls each pass "50 us". Replaced with
//       IODelay(10)/IOSleep(1) so the reported elapsed time is real.
//       Timeouts themselves are unchanged.
//   D6. Guards added where a wrong register would be written silently:
//       MCU_CNTL needs GC BASE_IDX 1 (the v0.1.40 fix), and the ring's
//       VRAM offset must be inside the BAR0 aperture for CPU writes.
//       Both log loudly and refuse instead of poking an unrelated
//       register. SDMA1 is skipped (with a log) when its STATUS_REG
//       reads all-ones, since its discovery entry can be missing.
//   D7. Extra logging only: every programmed address, and a full
//       QUEUE0 register dump before and after gfx_resume.
//
//  Nothing in the reference's register sequence, ordering or timeouts
//  was changed.
//

#include <IOKit/IOLib.h>

#include "amdgpu_sdma.h"
#include "amdgpu_gmc.h"
#include "amdgpu_log.h"
#include "amdgpu_psp.h"
// amdgpu_ucode_extract.h carries its own copy of the PSPGfxFwType
// constants, guarded by AMDGPU_PSP_GFX_FW_TYPE_DEFINED. amdgpu_psp.h
// (included above) defines the same namespace but does NOT define that
// guard, so define it here to keep the two copies from colliding in
// this translation unit. See the final report: amdgpu_psp.h should
// adopt the guard itself.
#define AMDGPU_PSP_GFX_FW_TYPE_DEFINED 1
#include "amdgpu_ucode_extract.h"

namespace amdgpu {

// Host fw-type encoding understood by amdgpu_ucode_extract()
// (amdgpu_ucode_extract.cpp:380-434): 0x100 + <psp fw_type> is the
// single-payload IP path, and SDMA_UCODE_TH0 there is routed to the
// sdma_firmware_header_v3_0 extractor — the same code the file-typed
// spelling (kHostFwFile_SDMA = 0x200 + 0) reaches. We use this form
// because kHostFwFile_SDMA is .cpp-local to the extractor.
constexpr uint64_t kHostFwType_SDMA = 0x100ULL + PSPGfxFwType::SDMA_UCODE_TH0;

//------------------------------------------------------------------
// Writeback-page access. The page is either GART-bound sysmem (CPU
// pointer, PCIe-snooped so plain volatile loads see engine writes) or
// VRAM (CPU access through the BAR0 aperture, HDP-flushed first).
//------------------------------------------------------------------
uint32_t
sdma_wb_read32(const DeviceContext &dev, const SDMAInstance &inst,
               uint32_t byte_offset)
{
    if (byte_offset + 4 > kSDMAWBPageBytes) return 0xFFFFFFFFu;
    if (inst.wb_in_vram) {
        // Drain HDP so the CPU-side read sees what the engine wrote.
        amdgpu_hdp_flush(dev);
        return RBAR0_32(dev, inst.wb_vram_off + byte_offset);
    }
    if (inst.wb_cpu == nullptr) return 0xFFFFFFFFu;
    sysmem_rmb();
    return *reinterpret_cast<const volatile uint32_t *>(
        static_cast<const uint8_t *>(inst.wb_cpu) + byte_offset);
}

void
sdma_wb_write32(const DeviceContext &dev, const SDMAInstance &inst,
                uint32_t byte_offset, uint32_t value)
{
    if (byte_offset + 4 > kSDMAWBPageBytes) return;
    if (inst.wb_in_vram) {
        WBAR0_32(dev, inst.wb_vram_off + byte_offset, value);
        amdgpu_hdp_flush(dev);
        return;
    }
    if (inst.wb_cpu == nullptr) return;
    *reinterpret_cast<volatile uint32_t *>(
        static_cast<uint8_t *>(inst.wb_cpu) + byte_offset) = value;
    sysmem_wmb();
}

//------------------------------------------------------------------
// sdma_poll_fence — wait for a dword in the WB page to take `want`.
// Deviation D5: real delays instead of the reference's fake spin.
//------------------------------------------------------------------
static kern_return_t
sdma_poll_fence(const DeviceContext &dev, const SDMAInstance &inst,
                uint32_t wb_offset, uint32_t want, uint64_t timeout_us,
                uint64_t *out_elapsed_us, uint32_t *out_last)
{
    uint64_t elapsed = 0;
    uint32_t v = 0;
    for (;;) {
        v = sdma_wb_read32(dev, inst, wb_offset);
        if (v == want) {
            if (out_elapsed_us) *out_elapsed_us = elapsed;
            if (out_last) *out_last = v;
            return kIOReturnSuccess;
        }
        if (elapsed >= timeout_us) break;
        // Tight spin for the first millisecond (a healthy engine lands
        // the fence in single-digit microseconds), then back off to the
        // 1 ms cadence poll_reg() uses everywhere else in this driver.
        if (elapsed < 1000) { IODelay(10);  elapsed += 10; }
        else                { IOSleep(1);   elapsed += 1000; }
    }
    if (out_elapsed_us) *out_elapsed_us = elapsed;
    if (out_last) *out_last = v;
    return kIOReturnTimeout;
}

//------------------------------------------------------------------
// sdma_log_queue_regs — full QUEUE0 dump (deviation D7). Called
// before and after gfx_resume so a hardware log shows every
// before/after value.
//------------------------------------------------------------------
static void
sdma_log_queue_regs(const DeviceContext &dev, uint32_t i, const char *tag)
{
    const SDMARegOffsets &R = sdma_regs(dev);
    auto rd = [&](uint32_t r) { return RREG32(dev, sdma_reg_offset(dev, i, r)); };
    SDMA_LOG("SDMA%u regs [%s]: RB_CNTL=%#010x RB_BASE=%#010x:%#010x "
             "RPTR=%#010x WPTR=%#010x IB_CNTL=%#010x",
             i, tag, rd(R.QUEUE0_RB_CNTL), rd(R.QUEUE0_RB_BASE_HI),
             rd(R.QUEUE0_RB_BASE), rd(R.QUEUE0_RB_RPTR),
             rd(R.QUEUE0_RB_WPTR), rd(R.QUEUE0_IB_CNTL));
    SDMA_LOG("SDMA%u regs [%s]: RPTR_ADDR=%#010x:%#010x "
             "WPTR_POLL_ADDR=%#010x:%#010x DOORBELL=%#010x OFFSET=%#010x "
             "MCU_CNTL=%#010x STATUS=%#010x",
             i, tag, rd(R.QUEUE0_RB_RPTR_ADDR_HI), rd(R.QUEUE0_RB_RPTR_ADDR_LO),
             rd(R.QUEUE0_RB_WPTR_POLL_ADDR_HI), rd(R.QUEUE0_RB_WPTR_POLL_ADDR_LO),
             rd(R.QUEUE0_DOORBELL), rd(R.QUEUE0_DOORBELL_OFFSET),
             rd(R.MCU_CNTL), rd(R.STATUS_REG));
}

//------------------------------------------------------------------
// sdma_alloc_storage — ring + WB page for one instance.
//------------------------------------------------------------------
kern_return_t
sdma_alloc_storage(DeviceContext &dev, SDMAInstance &inst, GMCContext &gmc)
{
    if (inst.inited) return kIOReturnSuccess;
    if (!gmc.vram_alloc.is_inited()) {
        SDMA_LOG("instance %u: vram_alloc not ready", inst.instance);
        return kIOReturnNotReady;
    }

    // Ring goes in VRAM. The reference moved it there because on AS+TB5
    // GPU-initiated reads of DART-mapped sysmem return zeros, so a
    // sysmem ring would be fetched as all-zero NOPs and never reach the
    // FENCE packet. Here the reason is simpler: the FB aperture is
    // GMC-routable with no GART entry at all, so the engine can fetch
    // packets before we have proven anything about GART.
    VRAMAllocation ring_alloc{};
    if (!gmc.vram_alloc.alloc(kSDMARingDefaultBytes, kASPageSize, &ring_alloc)) {
        SDMA_LOG("instance %u: ring VRAM alloc failed (need %u, free=%llu)",
                 inst.instance, kSDMARingDefaultBytes,
                 (unsigned long long)gmc.vram_alloc.bytes_free());
        return kIOReturnNoMemory;
    }
    inst.ring_gpu_va      = ring_alloc.gpu_va;
    inst.ring_vram_off    = ring_alloc.gpu_va - gmc.vram_start;  // BAR0 offset
    inst.ring_size_dwords = kSDMARingDefaultBytes / 4;
    inst.ring_ptr_mask    = inst.ring_size_dwords - 1;

    // Deviation D6: CPU ring writes go through BAR0, so the ring must
    // land inside the mapped aperture — otherwise WBAR0_32 silently
    // drops every packet and the engine spins on stale memory.
    if (inst.ring_vram_off + kSDMARingDefaultBytes > dev.bar0Size) {
        SDMA_LOG("instance %u: ring vram_off %#llx + %u outside BAR0 "
                 "(bar0Size=%llu) — refusing",
                 inst.instance, (unsigned long long)inst.ring_vram_off,
                 kSDMARingDefaultBytes, (unsigned long long)dev.bar0Size);
        gmc.vram_alloc.free(ring_alloc);
        return kIOReturnNoMemory;
    }
    // Zero the ring in VRAM via BAR0.
    bar0_memset_vram(dev, inst.ring_vram_off, 0, kSDMARingDefaultBytes);

    // Writeback page (deviation D2): sysmem + GART bind first, VRAM
    // fallback second.
    inst.wb_in_vram = false;
    inst.wb_cpu     = nullptr;
    inst.rptr_cpu   = nullptr;
    inst.wb_vram_off = 0;
    inst.wb_phys     = 0;
    inst.wb_bus      = 0;

    kern_return_t r = sysmem_alloc(inst.wb_sysmem, kSDMAWBPageBytes,
                                   kAMDGPUGPUPageSize);
    if (r == kIOReturnSuccess) {
        uint64_t wb_mc = 0;
        r = gmc_bind_existing(dev, gmc, inst.wb_sysmem.bus,
                              inst.wb_sysmem.size, &wb_mc);
        if (r == kIOReturnSuccess) {
            inst.wb_bus   = wb_mc;
            inst.wb_phys  = inst.wb_sysmem.bus;
            inst.wb_cpu   = inst.wb_sysmem.cpu;
            inst.rptr_cpu = reinterpret_cast<volatile uint32_t *>(
                                inst.wb_sysmem.cpu);
            SDMA_LOG("instance %u: WB page sysmem phys=%#llx -> GART mc=%#llx "
                     "(%u bytes)",
                     inst.instance, (unsigned long long)inst.wb_sysmem.bus,
                     (unsigned long long)wb_mc, kSDMAWBPageBytes);
        } else {
            SDMA_LOG("instance %u: GART bind of WB page failed: %#x — "
                     "falling back to a VRAM writeback page",
                     inst.instance, r);
            sysmem_free(inst.wb_sysmem);
        }
    } else {
        SDMA_LOG("instance %u: WB sysmem alloc failed: %#x — falling back "
                 "to a VRAM writeback page", inst.instance, r);
    }

    if (inst.wb_bus == 0) {
        VRAMAllocation wb_alloc{};
        if (!gmc.vram_alloc.alloc(kSDMAWBPageBytes, kASPageSize, &wb_alloc)) {
            SDMA_LOG("instance %u: WB VRAM alloc failed (free=%llu)",
                     inst.instance,
                     (unsigned long long)gmc.vram_alloc.bytes_free());
            gmc.vram_alloc.free(ring_alloc);
            return kIOReturnNoMemory;
        }
        inst.wb_in_vram  = true;
        inst.wb_bus      = wb_alloc.gpu_va;
        inst.wb_vram_off = wb_alloc.gpu_va - gmc.vram_start;
        if (inst.wb_vram_off + kSDMAWBPageBytes > dev.bar0Size) {
            SDMA_LOG("instance %u: WB vram_off %#llx outside BAR0 — refusing",
                     inst.instance, (unsigned long long)inst.wb_vram_off);
            gmc.vram_alloc.free(wb_alloc);
            gmc.vram_alloc.free(ring_alloc);
            return kIOReturnNoMemory;
        }
        bar0_memset_vram(dev, inst.wb_vram_off, 0, kSDMAWBPageBytes);
        SDMA_LOG("instance %u: WB page in VRAM mc=%#llx vram_off=%#llx",
                 inst.instance, (unsigned long long)inst.wb_bus,
                 (unsigned long long)inst.wb_vram_off);
    }

    inst.rptr_gpu_addr      = inst.wb_bus + kSDMAWBRptrOffset;
    inst.wptr_poll_gpu_addr = inst.wb_bus + kSDMAWBWptrOffset;

    inst.wptr           = 0;
    // Doorbell index — DWORD offset into the doorbell BAR (BAR2).
    // SOC21 layout (nv.c:580-583, amdgpu_doorbell.h:210-211):
    //     adev->doorbell_index.sdma_engine[0] = 0x100
    //     adev->doorbell_index.sdma_engine[1] = 0x10A
    // sdma_v7_1.c:1329-1330 assigns
    //     ring->doorbell_index = adev->doorbell_index.sdma_engine[i] << 1;
    // which is the DWORD offset written into the SDMA_QUEUE0_DOORBELL_OFFSET
    // register and consumed by the engine to filter doorbell traffic.
    // We shift by 1 here to match the upstream pattern.
    inst.doorbell_index = (dev.doorbell.index.sdma_engine[inst.instance] << 1);
    inst.inited         = true;

    SDMA_LOG("instance %u: ring %u dwords @ mc %#llx (vram_off %#llx), "
             "wb @ mc %#llx (%s), rptr_addr=%#llx wptr_poll_addr=%#llx, "
             "doorbell slot %#x",
             inst.instance, inst.ring_size_dwords,
             (unsigned long long)inst.ring_gpu_va,
             (unsigned long long)inst.ring_vram_off,
             (unsigned long long)inst.wb_bus,
             inst.wb_in_vram ? "vram" : "sysmem/gart",
             (unsigned long long)inst.rptr_gpu_addr,
             (unsigned long long)inst.wptr_poll_gpu_addr,
             inst.doorbell_index);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_release_storage — not in the reference (a dext leaks these on
// unload; a kext must not). Frees the VRAM slots and the sysmem WB.
//------------------------------------------------------------------
void
sdma_release_storage(GMCContext &gmc, SDMAInstance &inst)
{
    if (!inst.inited) return;
    if (gmc.vram_alloc.is_inited()) {
        VRAMAllocation a{};
        a.gpu_va = inst.ring_gpu_va;
        a.size   = kSDMARingDefaultBytes;
        a.alignment = kASPageSize;
        a.cpu_ptr = nullptr;
        gmc.vram_alloc.free(a);
        if (inst.wb_in_vram) {
            a.gpu_va = inst.wb_bus;
            a.size   = kSDMAWBPageBytes;
            gmc.vram_alloc.free(a);
        }
    }
    if (!inst.wb_in_vram) sysmem_free(inst.wb_sysmem);
    inst.inited  = false;
    inst.enabled = false;
    inst.wb_cpu   = nullptr;
    inst.rptr_cpu = nullptr;
    inst.wb_bus   = 0;
}

//------------------------------------------------------------------
// sdma_engine_halt — write SDMA0_SDMA_MCU_CNTL.HALT.
//------------------------------------------------------------------
kern_return_t
sdma_engine_halt(const DeviceContext &dev, uint32_t instance, bool halt)
{
    if (!dev.ip.isResolved(IPBlock::GC)) {
        SDMA_LOG("engine_halt: GC IP base not resolved");
        return kIOReturnNotReady;
    }
    // Deviation D6. MCU_CNTL (0x588e) is in the hyp-dec range, so it
    // resolves through GC BASE_IDX 1 — the v0.1.40 fix. If that base
    // never came out of discovery we would write an unrelated register
    // and "unhalt" would silently do nothing (the 0x92929292 symptom).
    if (!dev.ip.isResolved(IPBlock::GC, 1)) {
        SDMA_LOG("engine_halt: GC BASE_IDX 1 unresolved — MCU_CNTL would "
                 "land on an unrelated register; refusing");
        return kIOReturnNotReady;
    }
    const uint32_t reg = sdma_reg_offset(dev, instance, sdma_regs(dev).MCU_CNTL);
    uint32_t v = RREG32(dev, reg);
    const uint32_t before = v;
    v = REG_SET_FIELD(v, SDMA0_SDMA_MCU_CNTL, HALT, halt ? 1u : 0u);
    v = REG_SET_FIELD(v, SDMA0_SDMA_MCU_CNTL, RESET, 0u);
    WREG32(dev, reg, v);
    SDMA_LOG("SDMA%u MCU_CNTL @dword %#x: %#010x -> %#010x (HALT=%u) "
             "readback=%#010x",
             instance, reg, before, v, halt ? 1u : 0u, RREG32(dev, reg));
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_gfx_stop_instance — clear RB_ENABLE + IB_ENABLE on QUEUE0.
// Mirrors the per-instance body of sdma_v7_0_gfx_stop.
//------------------------------------------------------------------
kern_return_t
sdma_gfx_stop_instance(const DeviceContext &dev, uint32_t instance)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    const uint32_t rb_cntl_reg =
        sdma_reg_offset(dev, instance, sdma_regs(dev).QUEUE0_RB_CNTL);
    const uint32_t ib_cntl_reg =
        sdma_reg_offset(dev, instance, sdma_regs(dev).QUEUE0_IB_CNTL);

    uint32_t rb_cntl = RREG32(dev, rb_cntl_reg);
    const uint32_t rb_before = rb_cntl;
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, RB_ENABLE, 0);
    WREG32(dev, rb_cntl_reg, rb_cntl);

    uint32_t ib_cntl = RREG32(dev, ib_cntl_reg);
    const uint32_t ib_before = ib_cntl;
    ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_SDMA_QUEUE0_IB_CNTL, IB_ENABLE, 0);
    WREG32(dev, ib_cntl_reg, ib_cntl);
    SDMA_LOG("SDMA%u gfx_stop: RB_CNTL %#010x -> %#010x, IB_CNTL %#010x -> "
             "%#010x", instance, rb_before, rb_cntl, ib_before, ib_cntl);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// order_base_2 — log2 of a power-of-two. SDMA RB_SIZE expects the
// ring size as log2(dwords).
//------------------------------------------------------------------
static inline uint32_t order_base_2(uint32_t x)
{
    uint32_t r = 0;
    while ((1u << r) < x) r++;
    return r;
}

//------------------------------------------------------------------
// sdma_gfx_resume_instance — port of sdma_v7_1_gfx_resume_instance
// (sdma_v7_1.c:456-604, restore=false branch).
//
// Programs all the QUEUE0 registers (RB_CNTL/BASE/WPTR/RPTR/DOORBELL),
// unhalts the engine via MCU_CNTL, enables the ring + IB queue.
// Skipped from upstream:
//   • SR-IOV branches (we never run SR-IOV)
//   • __BIG_ENDIAN swap-enable fields (x86 is LE)
//   • amdgpu_ring_test_helper (we have our own sdma_ring_test)
//   • nbio.funcs->sdma_doorbell_range (set elsewhere when we wire
//     up the doorbell aperture; not needed for the first NOP test)
//------------------------------------------------------------------
kern_return_t
sdma_gfx_resume_instance(const DeviceContext &dev, SDMAInstance &inst)
{
    if (!inst.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    const uint32_t i = inst.instance;
    auto reg = [&](uint32_t r) { return sdma_reg_offset(dev, i, r); };

    // Mirrors upstream sdma_v7_0_gfx_resume_instance (sdma_v7_1.c:456-604).
    // Each step logs the register name + value so the kext log reads
    // like Linux dyndbg=+p amdgpu when bringup runs.
    SDMA_LOG("SDMA%u gfx_resume: starting (ring_gpu_va=%#llx, rb_size=%u dwords, "
             "rptr_addr=%#llx, doorbell_slot=%#x)",
             i, (unsigned long long)inst.ring_gpu_va,
             inst.ring_size_dwords,
             (unsigned long long)inst.rptr_gpu_addr,
             inst.doorbell_index);
    sdma_log_queue_regs(dev, i, "before gfx_resume");

    // 1) Initial RB_CNTL — set RB_SIZE, set RB_PRIV.
    uint32_t rb_bufsz = order_base_2(inst.ring_size_dwords);
    uint32_t rb_cntl = RREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_CNTL));
    SDMA_LOG("SDMA%u  RB_CNTL initial = %#010x (programming RB_SIZE=%u, RB_PRIV=1)",
             i, rb_cntl, rb_bufsz);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, RB_SIZE, rb_bufsz);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, RB_PRIV, 1);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_CNTL), rb_cntl);

    // 2) Reset RPTR/WPTR.
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_RPTR),    0);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_RPTR_HI), 0);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_WPTR),    0);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_WPTR_HI), 0);

    // 3) WPTR poll address (shadow in WB page; engine doesn't use it
    //    when WPTR_POLL_ENABLE=0, but we still program it).
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_WPTR_POLL_ADDR_LO),
           static_cast<uint32_t>(inst.wptr_poll_gpu_addr));
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_WPTR_POLL_ADDR_HI),
           static_cast<uint32_t>(inst.wptr_poll_gpu_addr >> 32));
    SDMA_LOG("SDMA%u  WPTR_POLL_ADDR = %#010x:%#010x (mc %#llx)",
             i, static_cast<uint32_t>(inst.wptr_poll_gpu_addr >> 32),
             static_cast<uint32_t>(inst.wptr_poll_gpu_addr),
             (unsigned long long)inst.wptr_poll_gpu_addr);

    // 4) RPTR write-back address — engine deposits read-pointer here.
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_RPTR_ADDR_HI),
           static_cast<uint32_t>(inst.rptr_gpu_addr >> 32));
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_RPTR_ADDR_LO),
           static_cast<uint32_t>(inst.rptr_gpu_addr & 0xFFFFFFFC));
    SDMA_LOG("SDMA%u  RB_RPTR_ADDR = %#010x:%#010x (mc %#llx, %s)",
             i, static_cast<uint32_t>(inst.rptr_gpu_addr >> 32),
             static_cast<uint32_t>(inst.rptr_gpu_addr & 0xFFFFFFFC),
             (unsigned long long)inst.rptr_gpu_addr,
             inst.wb_in_vram ? "vram" : "sysmem/gart");

    // 5) Enable RPTR writeback + MCU_WPTR_POLL.
    //
    // WPTR_POLL_ENABLE — only enabled on SR-IOV (upstream line 526).
    // It makes the engine poll memory at wptr_poll_gpu_addr. Bare-metal
    // sets it to 0 (matches us).
    //
    // MCU_WPTR_POLL_ENABLE — the SDMA microcontroller (MCU) uses this
    // to watch for WPTR updates. Upstream sets this to 1 UNCONDITIONALLY
    // (line 530), bare-metal AND SR-IOV. **Earlier the reference
    // incorrectly set it to 0 thinking it was a sysmem-polling enable;
    // it is not — it gates the MCU's ability to react to
    // doorbell-delivered WPTR updates.** Without it, the doorbell
    // aperture write arrives at the engine, but the MCU never picks up
    // the new WPTR → RB_WPTR register stays at 0 and no packets are
    // processed. (mac-amdgpu v0.1.33-v0.1.37 symptom.)
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL,
                            RPTR_WRITEBACK_ENABLE, 1);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL,
                            WPTR_POLL_ENABLE, 0);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL,
                            MCU_WPTR_POLL_ENABLE, 1);

    // 6) Ring base — RB_BASE is bus_addr >> 8, BASE_HI is >> 40.
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_BASE),
           static_cast<uint32_t>(inst.ring_gpu_va >> 8));
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_BASE_HI),
           static_cast<uint32_t>(inst.ring_gpu_va >> 40));
    SDMA_LOG("SDMA%u  RB_BASE = %#010x:%#010x (ring_gpu_va %#llx >> 8 / >> 40)",
             i,
             static_cast<uint32_t>(inst.ring_gpu_va >> 40),
             static_cast<uint32_t>(inst.ring_gpu_va >> 8),
             (unsigned long long)inst.ring_gpu_va);

    inst.wptr = 0;

    // 7) MINOR_PTR_UPDATE handshake — set 1 before writing WPTR,
    //    write WPTR, then clear MINOR_PTR_UPDATE.
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_MINOR_PTR_UPDATE), 1);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_WPTR),    static_cast<uint32_t>(inst.wptr << 2));
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_WPTR_HI), 0);

    // 8) Doorbell config — enable doorbell + offset = doorbell_index.
    uint32_t doorbell        = RREG32(dev, reg(sdma_regs(dev).QUEUE0_DOORBELL));
    uint32_t doorbell_offset = RREG32(dev, reg(sdma_regs(dev).QUEUE0_DOORBELL_OFFSET));
    SDMA_LOG("SDMA%u  DOORBELL before = %#010x, DOORBELL_OFFSET before = %#010x",
             i, doorbell, doorbell_offset);
    doorbell        = REG_SET_FIELD(doorbell,
                                    SDMA0_SDMA_QUEUE0_DOORBELL, ENABLE, 1);
    doorbell_offset = REG_SET_FIELD(doorbell_offset,
                                    SDMA0_SDMA_QUEUE0_DOORBELL_OFFSET, OFFSET,
                                    inst.doorbell_index);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_DOORBELL),        doorbell);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_DOORBELL_OFFSET), doorbell_offset);
    SDMA_LOG("SDMA%u  DOORBELL=%#x DOORBELL_OFFSET=%#x",
             i, doorbell, doorbell_offset);

    // 9) Clear MINOR_PTR_UPDATE after wptr.
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_MINOR_PTR_UPDATE), 0);

    // 10) Watchdog: 100ms per unit, usec_timeout/100000 floored at 1.
    //     For now we just write 1 (we don't carry a usec_timeout var).
    {
        uint32_t v = RREG32(dev, reg(sdma_regs(dev).WATCHDOG_CNTL));
        const uint32_t before = v;
        v = REG_SET_FIELD(v, SDMA0_SDMA_WATCHDOG_CNTL, QUEUE_HANG_COUNT, 1);
        WREG32(dev, reg(sdma_regs(dev).WATCHDOG_CNTL), v);
        SDMA_LOG("SDMA%u  WATCHDOG_CNTL %#010x -> %#010x", i, before, v);
    }

    // 11) UTCL1 RESP_MODE=3, REDO_DELAY=9.
    {
        uint32_t v = RREG32(dev, reg(sdma_regs(dev).UTCL1_CNTL));
        const uint32_t before = v;
        v = REG_SET_FIELD(v, SDMA0_SDMA_UTCL1_CNTL, RESP_MODE,  3);
        v = REG_SET_FIELD(v, SDMA0_SDMA_UTCL1_CNTL, REDO_DELAY, 9);
        WREG32(dev, reg(sdma_regs(dev).UTCL1_CNTL), v);
        SDMA_LOG("SDMA%u  UTCL1_CNTL %#010x -> %#010x", i, before, v);
    }

    // 12) UTCL1_PAGE — clean read+write policy bits, set L2 defaults
    //     (CACHE_READ_POLICY_L2__DEFAULT = 0 → bits [13:12] = 0,
    //      CACHE_WRITE_POLICY_L2__DEFAULT = 0 → bits [15:14] = 0).
    {
        uint32_t v = RREG32(dev, reg(sdma_regs(dev).UTCL1_PAGE));
        const uint32_t before = v;
        v &= 0xFF0FFFu;
        WREG32(dev, reg(sdma_regs(dev).UTCL1_PAGE), v);
        SDMA_LOG("SDMA%u  UTCL1_PAGE %#010x -> %#010x", i, before, v);
    }

    // 13) Unhalt engine via MCU_CNTL.
    SDMA_LOG("SDMA%u  unhalting MCU (writing MCU_CNTL.HALT=0, RESET=0)", i);
    sdma_engine_halt(dev, i, false);

    // 14) Enable the ring + IB.
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, RB_ENABLE, 1);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_CNTL), rb_cntl);

    uint32_t ib_cntl = RREG32(dev, reg(sdma_regs(dev).QUEUE0_IB_CNTL));
    ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_SDMA_QUEUE0_IB_CNTL, IB_ENABLE, 1);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_IB_CNTL), ib_cntl);

    inst.enabled = true;
    SDMA_LOG("SDMA%u: gfx_resume done — RB_CNTL=%#010x IB_CNTL=%#010x "
             "(RB_ENABLED on QUEUE 0, IB_ENABLED, engine running)",
             i, rb_cntl, ib_cntl);
    sdma_log_queue_regs(dev, i, "after gfx_resume");
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_kick_external_doorbell — ring QUEUE1's doorbell with Apple's
// wptr. See the header: this is the call that makes the GPU fetch.
//------------------------------------------------------------------
kern_return_t
sdma_kick_external_doorbell(const DeviceContext &dev, const SDMAInstance &inst,
                            uint32_t wptrDwords, uint32_t doorbellIndex)
{
    if (!inst.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    const uint64_t v = static_cast<uint64_t>(wptrDwords) << 2;   // byte wptr

    // 1) WPTR shadow in the EXTERNAL slot -- not kSDMAWBWptrOffset, which is
    //    QUEUE0's and belongs to the ring we are actively running.
    sdma_wb_write32(dev, inst, kSDMAWBExtWptrOffset, static_cast<uint32_t>(v));
    sdma_wb_write32(dev, inst, kSDMAWBExtWptrOffset + 4,
                    static_cast<uint32_t>(v >> 32));

    // 2) HDP flush so the engine's read of wptr_poll_addr drains past our write.
    amdgpu_hdp_flush(dev);

    // 3) BAR2 doorbell, dword-indexed (byte offset = index * 4).
    const uint64_t offs = static_cast<uint64_t>(doorbellIndex) * 4ull;
    if (dev.bar2 == nullptr || offs + 8 > dev.bar2Size) {
        SDMA_LOG("SDMA%u ext-kick: doorbell offset %#llx outside BAR2 "
                 "(bar2Size=%llu) — MMIO WPTR path only",
                 inst.instance, (unsigned long long)offs,
                 (unsigned long long)dev.bar2Size);
    } else {
        WDOORBELL64(dev, offs, v);
    }

    // 4) MMIO RB_WPTR fallback, against the QUEUE1 block.
    const auto &R = sdma_regs(dev);
    const uint32_t wptr_reg = sdma_reg_offset(
        dev, inst.instance, R.QUEUE0_RB_WPTR + kQueue0ToQueue1Stride);
    const uint32_t wptr_hi_reg = sdma_reg_offset(
        dev, inst.instance, R.QUEUE0_RB_WPTR_HI + kQueue0ToQueue1Stride);
    WREG32(dev, wptr_reg,    static_cast<uint32_t>(v));
    WREG32(dev, wptr_hi_reg, static_cast<uint32_t>(v >> 32));

    const uint32_t rptr_reg = sdma_reg_offset(
        dev, inst.instance, R.QUEUE0_RB_RPTR + kQueue0ToQueue1Stride);
    SDMA_LOG("SDMA%u ext-kick: wptr=%u (byte %#llx) doorbell@BAR2+%#llx; "
             "readback QUEUE1 RB_WPTR=%#010x RB_RPTR=%#010x ext_wb_rptr=%#010x",
             inst.instance, wptrDwords, (unsigned long long)v,
             (unsigned long long)offs, RREG32(dev, wptr_reg),
             RREG32(dev, rptr_reg),
             sdma_wb_read32(dev, inst, kSDMAWBExtRptrOffset));
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_log_external_queue_regs — read back the QUEUE1 block.
//------------------------------------------------------------------
void
sdma_log_external_queue_regs(const DeviceContext &dev,
                             const SDMAInstance &inst, const char *tag,
                             uint32_t queue)
{
    if (queue == 0 || queue >= kSDMAQueuesPerInstance) return;
    const uint32_t i = inst.instance;
    const auto &R = sdma_regs(dev);
    auto q1 = [&](uint32_t r) {
        return sdma_reg_offset(dev, i, r + queue * kQueue0ToQueue1Stride);
    };
    SDMA_LOG("SDMA%u QUEUE%u [%s]: RB_CNTL=%#010x RB_BASE=%#010x:%#010x "
             "RPTR=%#010x WPTR=%#010x IB_CNTL=%#010x DOORBELL=%#010x OFF=%#010x",
             i, queue, tag,
             RREG32(dev, q1(R.QUEUE0_RB_CNTL)),
             RREG32(dev, q1(R.QUEUE0_RB_BASE_HI)),
             RREG32(dev, q1(R.QUEUE0_RB_BASE)),
             RREG32(dev, q1(R.QUEUE0_RB_RPTR)),
             RREG32(dev, q1(R.QUEUE0_RB_WPTR)),
             RREG32(dev, q1(R.QUEUE0_IB_CNTL)),
             RREG32(dev, q1(R.QUEUE0_DOORBELL)),
             RREG32(dev, q1(R.QUEUE0_DOORBELL_OFFSET)));
}

//------------------------------------------------------------------
// sdma_read_external_rptr — QUEUE1's live read pointer, byte domain.
//
// Same q1() offset arithmetic as the dump above, narrowed to the one register
// needs. Reads only: nothing here writes a register or resets a pointer.
//------------------------------------------------------------------
uint32_t
sdma_read_external_rptr(const DeviceContext &dev, const SDMAInstance &inst,
                        uint32_t *wb_rptr_out)
{
    const uint32_t i = inst.instance;
    const auto &R = sdma_regs(dev);
    const uint32_t rptr_reg = sdma_reg_offset(
        dev, i, R.QUEUE0_RB_RPTR + kQueue0ToQueue1Stride);
    if (wb_rptr_out)
        *wb_rptr_out = sdma_wb_read32(dev, inst, kSDMAWBExtRptrOffset);
    return RREG32(dev, rptr_reg);
}

//------------------------------------------------------------------
// sdma_program_external_queue — bridge. See the header for
// why steps 10-13 of gfx_resume are omitted and why enable_ring must
// be false on the first pass.
//------------------------------------------------------------------
kern_return_t
sdma_program_external_queue(const DeviceContext &dev, SDMAInstance &inst,
                            uint64_t ring_gpu_va, uint32_t ring_size_dwords,
                            uint32_t doorbell_index, bool enable_ring)
{
    return sdma_program_external_queue_ex(dev, inst, ring_gpu_va, ring_size_dwords,
                                          doorbell_index, /*rptr_addr=*/0,
                                          /*wptr_poll_addr=*/0, enable_ring);
}

kern_return_t
sdma_program_external_queue_ex(const DeviceContext &dev, SDMAInstance &inst,
                               uint64_t ring_gpu_va, uint32_t ring_size_dwords,
                               uint32_t doorbell_index, uint64_t rptr_addr_in,
                               uint64_t wptr_poll_addr_in, bool enable_ring,
                               uint32_t queue)
{
    // 0.0.196: QUEUE0 is OURS on both instances and is never programmable from
    // here; QUEUE7 is the last block the header defines (kSDMAQueuesPerInstance).
    if (queue == 0 || queue >= kSDMAQueuesPerInstance) {
        SDMA_LOG("sdma_program_external_queue_ex: queue %u is out of range "
                 "(1..%u; QUEUE0 is ours) - REFUSING", queue,
                 kSDMAQueuesPerInstance - 1);
        return kIOReturnBadArgument;
    }
    if (!inst.inited)                       return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC))    return kIOReturnNotReady;
    if (ring_gpu_va == 0 || (ring_gpu_va & 0xFF) != 0) return kIOReturnBadArgument;
    if (ring_size_dwords == 0)              return kIOReturnBadArgument;
    if (inst.wb_bus == 0)                   return kIOReturnNotReady;
    // RB_SIZE is log2(dwords); a non-power-of-two ring cannot be expressed.
    if (ring_size_dwords & (ring_size_dwords - 1)) return kIOReturnBadArgument;

    const uint32_t i = inst.instance;
    const auto &R = sdma_regs(dev);
    auto q1 = [&](uint32_t r) {
        return sdma_reg_offset(dev, i, r + queue * kQueue0ToQueue1Stride);
    };

    // A caller-supplied address wins; 0 means "use our own external slot".
    const uint64_t rptr_addr = rptr_addr_in ? rptr_addr_in
                                            : (inst.wb_bus + kSDMAWBExtRptrOffset);
    const uint64_t wptr_addr = wptr_poll_addr_in ? wptr_poll_addr_in
                                                 : (inst.wb_bus + kSDMAWBExtWptrOffset);
    SDMA_LOG("SDMA%u QUEUE%u external: RB_RPTR_ADDR <- %#llx (%s), "
             "RB_WPTR_POLL_ADDR <- %#llx (%s)",
             i, queue, (unsigned long long)rptr_addr,
             rptr_addr_in ? "CALLER's rptr report (ring->0xb8)" : "our WB +0x100",
             (unsigned long long)wptr_addr,
             wptr_poll_addr_in ? "CALLER's wptr write-back (ring->0xd0)"
                               : "our WB +0x140");

    SDMA_LOG("SDMA%u QUEUE%u external: ring_gpu_va=%#llx size=%u dwords "
             "rptr_wb=%#llx wptr_poll=%#llx doorbell=%#x RB_ENABLE=%d",
             i, queue, (unsigned long long)ring_gpu_va, ring_size_dwords,
             (unsigned long long)rptr_addr, (unsigned long long)wptr_addr,
             doorbell_index, enable_ring ? 1 : 0);
    sdma_log_external_queue_regs(dev, inst, "before", queue);

    // 1) RB_CNTL — RB_SIZE + RB_PRIV, ring still DISABLED.
    uint32_t rb_cntl = RREG32(dev, q1(R.QUEUE0_RB_CNTL));
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, RB_SIZE,
                            order_base_2(ring_size_dwords));
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, RB_PRIV, 1);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, RB_ENABLE, 0);
    WREG32(dev, q1(R.QUEUE0_RB_CNTL), rb_cntl);

    // 2) Reset RPTR/WPTR.
    WREG32(dev, q1(R.QUEUE0_RB_RPTR),    0);
    WREG32(dev, q1(R.QUEUE0_RB_RPTR_HI), 0);
    WREG32(dev, q1(R.QUEUE0_RB_WPTR),    0);
    WREG32(dev, q1(R.QUEUE0_RB_WPTR_HI), 0);

    // 3) WPTR poll address.
    WREG32(dev, q1(R.QUEUE0_RB_WPTR_POLL_ADDR_LO), (uint32_t)wptr_addr);
    WREG32(dev, q1(R.QUEUE0_RB_WPTR_POLL_ADDR_HI), (uint32_t)(wptr_addr >> 32));

    // 4) RPTR writeback address — this is what Apple's getHead will read.
    WREG32(dev, q1(R.QUEUE0_RB_RPTR_ADDR_HI), (uint32_t)(rptr_addr >> 32));
    WREG32(dev, q1(R.QUEUE0_RB_RPTR_ADDR_LO), (uint32_t)(rptr_addr & 0xFFFFFFFC));

    // 5) RPTR writeback on; MCU_WPTR_POLL on (gates the MCU reacting to
    //    doorbell-delivered WPTR updates); WPTR_POLL off (bare metal).
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, RPTR_WRITEBACK_ENABLE, 1);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, WPTR_POLL_ENABLE, 0);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, MCU_WPTR_POLL_ENABLE, 1);

    // 6) Ring base.
    WREG32(dev, q1(R.QUEUE0_RB_BASE),    (uint32_t)(ring_gpu_va >> 8));
    WREG32(dev, q1(R.QUEUE0_RB_BASE_HI), (uint32_t)(ring_gpu_va >> 40));

    // 7-9) MINOR_PTR_UPDATE handshake around the WPTR write, then doorbell.
    WREG32(dev, q1(R.QUEUE0_MINOR_PTR_UPDATE), 1);
    WREG32(dev, q1(R.QUEUE0_RB_WPTR),    0);
    WREG32(dev, q1(R.QUEUE0_RB_WPTR_HI), 0);

    uint32_t doorbell        = RREG32(dev, q1(R.QUEUE0_DOORBELL));
    uint32_t doorbell_offset = RREG32(dev, q1(R.QUEUE0_DOORBELL_OFFSET));
    doorbell        = REG_SET_FIELD(doorbell, SDMA0_SDMA_QUEUE0_DOORBELL,
                                    ENABLE, 1);
    doorbell_offset = REG_SET_FIELD(doorbell_offset,
                                    SDMA0_SDMA_QUEUE0_DOORBELL_OFFSET,
                                    OFFSET, doorbell_index);
    WREG32(dev, q1(R.QUEUE0_DOORBELL),        doorbell);
    WREG32(dev, q1(R.QUEUE0_DOORBELL_OFFSET), doorbell_offset);
    WREG32(dev, q1(R.QUEUE0_MINOR_PTR_UPDATE), 0);

    // Steps 10-13 (WATCHDOG_CNTL, UTCL1_CNTL, UTCL1_PAGE, MCU unhalt) are
    // INSTANCE-wide and already done by the QUEUE0 resume whose ring is live.
    // Touching them here would perturb a running queue. Omitted on purpose.

    // 14) Ring enable — gated. RB_ENABLE=0 means the engine fetches NOTHING.
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, RB_ENABLE,
                            enable_ring ? 1u : 0u);
    WREG32(dev, q1(R.QUEUE0_RB_CNTL), rb_cntl);

    uint32_t ib_cntl = RREG32(dev, q1(R.QUEUE0_IB_CNTL));
    ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_SDMA_QUEUE0_IB_CNTL, IB_ENABLE,
                            enable_ring ? 1u : 0u);
    WREG32(dev, q1(R.QUEUE0_IB_CNTL), ib_cntl);

    sdma_log_external_queue_regs(dev, inst, "after", queue);
    SDMA_LOG("SDMA%u QUEUE%u external: programmed, RB_ENABLE=%d (%s)",
             i, queue, enable_ring ? 1 : 0,
             enable_ring ? "ENGINE WILL FETCH"
                         : "inert - engine fetches nothing, no doorbell rung");
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_disable_external_queue — THE ESCAPE HATCH (0.0.185).
//
// Two register writes, both QUEUE1-scoped. After them the engine fetches nothing
// from QUEUE1 whatever its RB_BASE still says, and QUEUE0 (our live copy_linear
// ring, the control this whole design keeps running) is untouched.
//------------------------------------------------------------------
kern_return_t
sdma_disable_external_queue(const DeviceContext &dev, const SDMAInstance &inst,
                            uint32_t queue)
{
    if (queue == 0 || queue >= kSDMAQueuesPerInstance) return kIOReturnBadArgument;
    if (!inst.inited)                    return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    const auto &R = sdma_regs(dev);
    auto q1 = [&](uint32_t r) {
        return sdma_reg_offset(dev, inst.instance, r + queue * kQueue0ToQueue1Stride);
    };
    uint32_t rb_cntl = RREG32(dev, q1(R.QUEUE0_RB_CNTL));
    uint32_t ib_cntl = RREG32(dev, q1(R.QUEUE0_IB_CNTL));
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, RB_ENABLE, 0);
    ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_SDMA_QUEUE0_IB_CNTL, IB_ENABLE, 0);
    WREG32(dev, q1(R.QUEUE0_RB_CNTL), rb_cntl);
    WREG32(dev, q1(R.QUEUE0_IB_CNTL), ib_cntl);
    SDMA_LOG("SDMA%u QUEUE%u: DISABLED — RB_CNTL=%#010x IB_CNTL=%#010x; the engine "
             "fetches nothing from this queue. QUEUE0 untouched.",
             inst.instance, queue, rb_cntl, ib_cntl);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_read_external_queue_regs — the whole QUEUE1 block, reads only.
//------------------------------------------------------------------
void
sdma_read_external_queue_regs(const DeviceContext &dev, const SDMAInstance &inst,
                              SDMAQueue1Regs *out, uint32_t queue)
{
    if (!out) return;
    *out = SDMAQueue1Regs {};
    if (queue == 0 || queue >= kSDMAQueuesPerInstance) return;
    if (!inst.inited || !dev.ip.isResolved(IPBlock::GC)) return;
    const auto &R = sdma_regs(dev);
    auto q1 = [&](uint32_t r) {
        return sdma_reg_offset(dev, inst.instance, r + queue * kQueue0ToQueue1Stride);
    };
    out->rb_cntl          = RREG32(dev, q1(R.QUEUE0_RB_CNTL));
    out->rb_base          = RREG32(dev, q1(R.QUEUE0_RB_BASE));
    out->rb_base_hi       = RREG32(dev, q1(R.QUEUE0_RB_BASE_HI));
    out->rb_rptr          = RREG32(dev, q1(R.QUEUE0_RB_RPTR));
    out->rb_rptr_hi       = RREG32(dev, q1(R.QUEUE0_RB_RPTR_HI));
    out->rb_wptr          = RREG32(dev, q1(R.QUEUE0_RB_WPTR));
    out->rb_wptr_hi       = RREG32(dev, q1(R.QUEUE0_RB_WPTR_HI));
    out->rb_rptr_addr_lo  = RREG32(dev, q1(R.QUEUE0_RB_RPTR_ADDR_LO));
    out->rb_rptr_addr_hi  = RREG32(dev, q1(R.QUEUE0_RB_RPTR_ADDR_HI));
    out->ib_cntl          = RREG32(dev, q1(R.QUEUE0_IB_CNTL));
    out->doorbell         = RREG32(dev, q1(R.QUEUE0_DOORBELL));
    out->doorbell_offset  = RREG32(dev, q1(R.QUEUE0_DOORBELL_OFFSET));
    out->wptr_poll_lo     = RREG32(dev, q1(R.QUEUE0_RB_WPTR_POLL_ADDR_LO));
    out->wptr_poll_hi     = RREG32(dev, q1(R.QUEUE0_RB_WPTR_POLL_ADDR_HI));
    out->minor_ptr_update = RREG32(dev, q1(R.QUEUE0_MINOR_PTR_UPDATE));
}

//------------------------------------------------------------------
// sdma_q1_kick_doorbell_only / sdma_q1_set_wptr_mmio — the two halves of
// sdma_kick_external_doorbell, separated so the routing question has an answer.
//------------------------------------------------------------------
kern_return_t
sdma_q1_kick_doorbell_only(const DeviceContext &dev, const SDMAInstance &inst,
                           uint32_t wptrDwords, uint32_t doorbellIndex,
                           uint32_t queue)
{
    if (queue == 0 || queue >= kSDMAQueuesPerInstance) return kIOReturnBadArgument;
    if (!inst.inited)                    return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    const uint64_t v = static_cast<uint64_t>(wptrDwords) << 2;   // byte wptr

    sdma_wb_write32(dev, inst, kSDMAWBExtWptrOffset, static_cast<uint32_t>(v));
    sdma_wb_write32(dev, inst, kSDMAWBExtWptrOffset + 4,
                    static_cast<uint32_t>(v >> 32));
    amdgpu_hdp_flush(dev);

    const uint64_t offs = static_cast<uint64_t>(doorbellIndex) * 4ull;
    if (dev.bar2 == nullptr || offs + 8 > dev.bar2Size) {
        SDMA_LOG("SDMA%u q1-kick: doorbell dword %#x (byte %#llx) is outside BAR2 "
                 "(%llu bytes) — REFUSING", inst.instance, doorbellIndex,
                 (unsigned long long)offs, (unsigned long long)dev.bar2Size);
        return kIOReturnBadArgument;
    }
    WDOORBELL64(dev, offs, v);
    SDMA_LOG("SDMA%u QUEUE%u q1-kick: DOORBELL ONLY — wptr=%u (byte %#llx) to "
             "BAR2+%#llx (dword index %#x). No MMIO RB_WPTR write: if the queue "
             "advances, the doorbell routed.", inst.instance, queue, wptrDwords,
             (unsigned long long)v, (unsigned long long)offs, doorbellIndex);
    return kIOReturnSuccess;
}

kern_return_t
sdma_q1_set_wptr_mmio(const DeviceContext &dev, const SDMAInstance &inst,
                      uint32_t wptrDwords, uint32_t queue)
{
    if (queue == 0 || queue >= kSDMAQueuesPerInstance) return kIOReturnBadArgument;
    if (!inst.inited)                    return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    const uint64_t v = static_cast<uint64_t>(wptrDwords) << 2;
    const auto &R = sdma_regs(dev);
    auto q1 = [&](uint32_t r) {
        return sdma_reg_offset(dev, inst.instance, r + queue * kQueue0ToQueue1Stride);
    };
    WREG32(dev, q1(R.QUEUE0_RB_WPTR),    static_cast<uint32_t>(v));
    WREG32(dev, q1(R.QUEUE0_RB_WPTR_HI), static_cast<uint32_t>(v >> 32));
    SDMA_LOG("SDMA%u q1-kick: MMIO CONTROL — RB_WPTR <- %#llx; readback RPTR=%#010x "
             "WPTR=%#010x", inst.instance, (unsigned long long)v,
             RREG32(dev, q1(R.QUEUE0_RB_RPTR)), RREG32(dev, q1(R.QUEUE0_RB_WPTR)));
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_q1_selftest — does BAR2 dword `doorbell_index` reach SDMA0 QUEUE1?
//
// Open question 1 of notes/re/sdma-takeover-design.md, settled with OUR OWN ring
// before Apple's is ever involved. 4 KiB of VRAM, one FENCE packet, RB_ENABLE=1,
// a doorbell-only kick, a 200 ms poll; only if that fails, the MMIO control leg.
// QUEUE1 ends DISABLED and the ring is freed either way.
//------------------------------------------------------------------
kern_return_t
sdma_q1_selftest(DeviceContext &dev, GMCContext &gmc, SDMAInstance &inst,
                 uint32_t doorbell_index, SDMAQ1TestResult *out, uint32_t queue)
{
    SDMAQ1TestResult t {};
    t.doorbell_index = doorbell_index;
    t.instance       = inst.instance;
    t.queue          = queue;
    if (out) *out = t;
    if (queue == 0 || queue >= kSDMAQueuesPerInstance) {
        SDMA_LOG("q1-selftest: queue %u out of range (1..%u) — skipping",
                 queue, kSDMAQueuesPerInstance - 1);
        return kIOReturnBadArgument;
    }
    if (!inst.inited || !inst.enabled) {
        SDMA_LOG("q1-selftest: SDMA%u QUEUE0 is not running — skipping (the MCU "
                 "unhalt and the instance-wide registers come from its resume)",
                 inst.instance);
        return kIOReturnNotReady;
    }
    if (inst.wb_bus == 0) return kIOReturnNotReady;

    constexpr uint32_t kRingBytes  = 4096;
    constexpr uint32_t kRingDwords = kRingBytes / 4;      // 1024 -> RB_SIZE 10

    VRAMAllocation ring {};
    if (!gmc.vram_alloc.alloc(kRingBytes, kASPageSize, &ring)) {
        SDMA_LOG("q1-selftest: no VRAM for the scratch ring — skipping");
        return kIOReturnNoMemory;
    }
    const uint64_t ring_off = ring.gpu_va - gmc.vram_start;
    // CPU ring writes go through BAR0: outside the aperture WBAR0_32 drops them
    // silently and the engine would fetch stale memory (the same trap
    // sdma_alloc_storage guards).
    if (ring_off + kRingBytes > dev.bar0Size) {
        SDMA_LOG("q1-selftest: scratch ring vram_off %#llx + %u is outside BAR0 "
                 "(%llu bytes) — skipping", (unsigned long long)ring_off,
                 kRingBytes, (unsigned long long)dev.bar0Size);
        gmc.vram_alloc.free(ring);
        return kIOReturnNoMemory;
    }
    t.ran         = true;
    t.ring_gpu_va = ring.gpu_va;
    t.ring_dwords = kRingDwords;

    // Zero the ring, then one FENCE at dword 0. Four dwords of work.
    bar0_memset_vram(dev, ring_off, 0, kRingBytes);
    const uint64_t fence_gpu    = inst.wb_bus + kSDMAWBExtFenceOffset;
    const uint32_t fence_value  = 0x51CAFE01u;            // 'Q1 CAFE'
    t.fence_gpu = fence_gpu;
    sdma_wb_write32(dev, inst, kSDMAWBExtFenceOffset, 0);

    uint32_t pkt[4];
    pkt[0] = SDMA_PKT_HEADER_OP(SDMA_OP_FENCE);
    pkt[1] = static_cast<uint32_t>(fence_gpu);
    pkt[2] = static_cast<uint32_t>(fence_gpu >> 32);
    pkt[3] = fence_value;
    for (uint32_t i = 0; i < 4; i++)
        WBAR0_32(dev, ring_off + i * 4u, pkt[i]);
    amdgpu_hdp_flush(dev);

    SDMA_LOG("q1-selftest: SDMA%u QUEUE%u, scratch ring MC %#llx (%u dwords), FENCE "
             "%#010x -> %#llx, doorbell dword index %#x. QUEUE0 stays live.",
             inst.instance, queue, (unsigned long long)ring.gpu_va, kRingDwords,
             fence_value, (unsigned long long)fence_gpu, doorbell_index);

    kern_return_t r = sdma_program_external_queue_ex(
        dev, inst, ring.gpu_va, kRingDwords, doorbell_index,
        /*rptr_addr=*/0, /*wptr_poll_addr=*/0, /*enable_ring=*/true, queue);
    if (r != kIOReturnSuccess) {
        SDMA_LOG("q1-selftest: programming SDMA%u QUEUE%u failed %#x",
                 inst.instance, queue, r);
        gmc.vram_alloc.free(ring);
        if (out) *out = t;
        return r;
    }

    // ---- leg A: the doorbell, and nothing else.
    uint64_t elapsed = 0;
    uint32_t last    = 0;
    r = sdma_q1_kick_doorbell_only(dev, inst, /*wptrDwords=*/4, doorbell_index, queue);
    if (r == kIOReturnSuccess) {
        r = sdma_poll_fence(dev, inst, kSDMAWBExtFenceOffset, fence_value,
                            /*timeout_us=*/200000ull, &elapsed, &last);
        t.doorbell_ok  = (r == kIOReturnSuccess);
        t.doorbell_us  = elapsed;
        t.fence_last   = last;
        SDMA_LOG("q1-selftest: SDMA%u QUEUE%u doorbell dword %#x leg %s — fence "
                 "reads %#010x after %llu us", inst.instance, queue, doorbell_index,
                 t.doorbell_ok ? "PASSED (BAR2 dword ROUTES to this queue)"
                               : "FAILED (no fence from the doorbell alone)",
                 last, (unsigned long long)elapsed);
    }

    // ---- leg B, only as the control: was it the doorbell or the queue?
    if (!t.doorbell_ok) {
        elapsed = 0; last = 0;
        (void)sdma_q1_set_wptr_mmio(dev, inst, /*wptrDwords=*/4, queue);
        r = sdma_poll_fence(dev, inst, kSDMAWBExtFenceOffset, fence_value,
                            /*timeout_us=*/200000ull, &elapsed, &last);
        t.mmio_ok    = (r == kIOReturnSuccess);
        t.mmio_us    = elapsed;
        t.fence_last = last;
        SDMA_LOG("q1-selftest: MMIO control leg %s — fence reads %#010x after %llu us. "
                 "%s", t.mmio_ok ? "PASSED" : "FAILED", last,
                 (unsigned long long)elapsed,
                 t.mmio_ok ? "So QUEUE1 itself works and the DOORBELL is what does "
                             "not route: widen the S2A window before the "
                             "takeover."
                           : "Neither path fenced — QUEUE1 is not executing at all; "
                             "do NOT run sdmamap on this boot.");
    }

    SDMAQueue1Regs regs {};
    sdma_read_external_queue_regs(dev, inst, &regs, queue);
    t.rb_cntl = regs.rb_cntl;
    t.rb_rptr = regs.rb_rptr;
    t.rb_wptr = regs.rb_wptr;
    SDMA_LOG("q1-selftest: SDMA%u QUEUE%u after — RB_CNTL=%#010x RB_BASE=%#010x:%#010x "
             "RPTR=%#010x WPTR=%#010x DOORBELL=%#010x OFF=%#010x (expected OFF "
             "%#010x for index %#x)",
             inst.instance, queue, regs.rb_cntl, regs.rb_base_hi, regs.rb_base, regs.rb_rptr,
             regs.rb_wptr, regs.doorbell, regs.doorbell_offset,
             (doorbell_index << 2) & 0x0FFFFFFCu, doorbell_index);

    // Leave nothing armed: disable the queue, free the scratch ring.
    (void)sdma_disable_external_queue(dev, inst, queue);
    gmc.vram_alloc.free(ring);
    if (out) *out = t;
    return t.doorbell_ok ? kIOReturnSuccess : kIOReturnTimeout;
}

//------------------------------------------------------------------
// sdma_srbm_selftest (0.0.191) — does SDMA_OP_SRBM_WRITE change a register?
//
// See amdgpu_sdma.h for the question, the two operand encodings and the leg
// numbering. Nothing here runs unless navi48-srbm-test=1.
//
// Registers under test:
//   regSCRATCH_REG7 = 0x2047, BASE_IDX 1  (gc_12_0_0_offset.h:4362-4363).
//     SCRATCH_REG0 is NOT free — cp_ring_test (PM4Test, stage 16) writes and
//     polls it — and nothing in this tree names SCRATCH_REG1..7.
//   regGCVM_CONTEXT2_PAGE_TABLE_BASE_ADDR_LO32 = 0x1693, BASE_IDX 0
//     (gc_12_0_0_offset.h:3114-3115). Free for us: hub_setup_vmid_config
//     (gmc_v12_0.cpp:1074) programs CONTEXT1..15 CNTL / PAGE_TABLE_START /
//     PAGE_TABLE_END and never a PAGE_TABLE_BASE, and no stage of ours submits
//     under VMID 2. measured it as 0 after Apple's SDMA IB fenced.
//------------------------------------------------------------------
constexpr uint32_t kSRBMScratchReg   = 0x2047;   // regSCRATCH_REG7, BASE_IDX 1
constexpr uint32_t kSRBMCtx2Reg      = 0x1693;   // GCVM_CONTEXT2 PT base LO, BASE_IDX 0
constexpr uint32_t kSRBMScratchValue = 0x5AA5C3C3u;
constexpr uint32_t kSRBMCtx2Value    = 0x12345000u;   // 0x00d6c000-style PT base
constexpr uint32_t kSRBMScratchClear = 0xDEADBEEFu;
constexpr uint32_t kSRBMIBDwords     = 8;        // SRBM (3) + NOP pad to 8

static const char *srbm_leg_name(uint32_t leg)
{
    switch (leg) {
    case 0:  return "ring/apple-form";
    case 1:  return "ring/linux-form";
    case 2:  return "ib(apple hdr 0x80000004)/apple-form";
    case 3:  return "ib(apple hdr 0x80000004)/linux-form";
    default: return "ib(plain hdr 0x00000004)";
    }
}

// One leg: clear the register by MMIO, submit ONE SRBM_WRITE (in the ring or
// through an IB) plus a FENCE, wait <= 200 ms, read the register back, restore
// it. Returns true when the register actually took the value.
static bool
srbm_run_leg(const DeviceContext &dev, SDMAInstance &inst,
             SDMASRBMTargetResult &tr, uint32_t leg,
             bool via_ib, bool linux_enc, bool apple_ibhdr,
             uint32_t clear, uint64_t ib_gpu_va, uint64_t ib_vram_off,
             const char *tag)
{
    if (leg >= 5) return false;
    tr.tried[leg] = true;

    // The register write itself. APPLE: header 0xf000000e + DWORD index.
    // LINUX (sdma_v7_0.c:1209-1211): header 0x0000000e + (reg << 2), a BYTE
    // offset.
    uint32_t srbm[3];
    srbm[0] = linux_enc
            ? SDMA_PKT_HEADER_OP(SDMA_OP_SRBM_WRITE)
            : (SDMA_PKT_HEADER_OP(SDMA_OP_SRBM_WRITE) | SDMA_PKT_HEADER_BYTE_EN(0xf));
    srbm[1] = linux_enc ? (tr.reg_dw << 2) : tr.reg_dw;
    srbm[2] = tr.value;

    WREG32(dev, tr.reg_dw, clear);
    const uint32_t pre = RREG32(dev, tr.reg_dw);

    const uint64_t fence_gpu   = inst.wb_bus + kSDMAWBFenceOffset;
    const uint32_t fence_value = 0x5B000000u | (leg << 4) | (tr.reg_dw & 0xFu);
    sdma_wb_write32(dev, inst, kSDMAWBFenceOffset, 0);

    uint32_t pkt[24];
    uint32_t n = 0;
    uint32_t ib_hdr = 0;
    if (!via_ib) {
        pkt[n++] = srbm[0]; pkt[n++] = srbm[1]; pkt[n++] = srbm[2];
    } else {
        // Stage the IB: the SRBM_WRITE then NOPs out to 8 dwords, matching
        // sdma_v7_0_ring_pad_ib's "IB length is a multiple of 8" rule.
        for (uint32_t i = 0; i < kSRBMIBDwords; i++) {
            const uint32_t d = (i < 3) ? srbm[i] : SDMA_PKT_HEADER_OP(SDMA_OP_NOP);
            WBAR0_32(dev, ib_vram_off + i * 4u, d);
        }
        amdgpu_hdp_flush(dev);
        // sdma_v7_0_ring_emit_ib:283 — insert_nop((2 - wptr) & 7) so the dword
        // AFTER the 6-dword IB packet lands on an 8-dword boundary.
        const uint32_t nops = (2u - static_cast<uint32_t>(inst.wptr)) & 7u;
        for (uint32_t i = 0; i < nops; i++)
            pkt[n++] = SDMA_PKT_HEADER_OP(SDMA_OP_NOP);
        ib_hdr = SDMA_PKT_HEADER_OP(SDMA_OP_INDIRECT)
               | (apple_ibhdr ? SDMA_PKT_INDIRECT_HEADER_PRIV(1)
                              : SDMA_PKT_INDIRECT_HEADER_VMID(0));
        pkt[n++] = ib_hdr;
        pkt[n++] = static_cast<uint32_t>(ib_gpu_va) & 0xFFFFFFE0u;   // 32B aligned
        pkt[n++] = static_cast<uint32_t>(ib_gpu_va >> 32);
        pkt[n++] = kSRBMIBDwords;
        pkt[n++] = 0;    // csa lo — no CSA, the ring has none either
        pkt[n++] = 0;    // csa hi
    }
    pkt[n++] = SDMA_PKT_HEADER_OP(SDMA_OP_FENCE);
    pkt[n++] = static_cast<uint32_t>(fence_gpu);
    pkt[n++] = static_cast<uint32_t>(fence_gpu >> 32);
    pkt[n++] = fence_value;

    if (via_ib) {
        SDMA_LOG("srbm-test: %s %s — IB hdr %#010x base %#llx len %u dw; IB[0..2] "
                 "%#010x %#010x %#010x; fence %#010x -> %#llx; %u ring dwords at "
                 "wptr %llu", tag, srbm_leg_name(leg), ib_hdr,
                 (unsigned long long)ib_gpu_va, kSRBMIBDwords,
                 srbm[0], srbm[1], srbm[2], fence_value,
                 (unsigned long long)fence_gpu, n, (unsigned long long)inst.wptr);
    } else {
        SDMA_LOG("srbm-test: %s %s — ring pkt %#010x %#010x %#010x; fence %#010x "
                 "-> %#llx; %u ring dwords at wptr %llu", tag, srbm_leg_name(leg),
                 srbm[0], srbm[1], srbm[2], fence_value,
                 (unsigned long long)fence_gpu, n, (unsigned long long)inst.wptr);
    }
    SDMA_LOG("srbm-test: %s %s — target reg dword %#x, before %#010x, cleared to "
             "%#010x (reads %#010x), want %#010x",
             tag, srbm_leg_name(leg), tr.reg_dw, tr.before, clear, pre, tr.value);

    if (sdma_ring_write(dev, inst, pkt, n) != n) {
        SDMA_LOG("srbm-test: %s %s — ring_write failed", tag, srbm_leg_name(leg));
        WREG32(dev, tr.reg_dw, tr.before);
        return false;
    }
    if (sdma_kick_doorbell(dev, inst) != kIOReturnSuccess) {
        WREG32(dev, tr.reg_dw, tr.before);
        return false;
    }

    uint64_t elapsed = 0;
    uint32_t last    = 0;
    const kern_return_t r = sdma_poll_fence(dev, inst, kSDMAWBFenceOffset,
                                            fence_value, /*timeout_us=*/200000ull,
                                            &elapsed, &last);
    tr.fenced[leg]   = (r == kIOReturnSuccess);
    tr.readback[leg] = RREG32(dev, tr.reg_dw);
    tr.landed[leg]   = tr.fenced[leg] && (tr.readback[leg] == tr.value);

    SDMA_LOG("srbm-test: %s %s — %s; fence %s after %llu us (slot %#010x); reg "
             "dword %#x reads %#010x (want %#010x)",
             tag, srbm_leg_name(leg),
             tr.landed[leg] ? "*** THE REGISTER TOOK THE VALUE ***"
                            : (tr.fenced[leg] ? "FAILED — the packet was consumed "
                                                "but the register did not change"
                                              : "VOID — the engine never fenced"),
             tr.fenced[leg] ? "landed" : "TIMED OUT",
             (unsigned long long)elapsed, last, tr.reg_dw,
             tr.readback[leg], tr.value);
    if (!tr.fenced[leg]) sdma_log_status(dev, inst.instance);

    // Always put the register back the way we found it.
    WREG32(dev, tr.reg_dw, tr.before);
    return tr.landed[leg];
}

static void
srbm_run_target(const DeviceContext &dev, SDMAInstance &inst,
                SDMASRBMTargetResult &tr, uint32_t clear,
                uint64_t ib_gpu_va, uint64_t ib_vram_off, const char *tag)
{
    tr.before = RREG32(dev, tr.reg_dw);
    // Is the register MMIO-writable at all? Without this a dead register would
    // read "SRBM_WRITE does not work" for entirely the wrong reason.
    WREG32(dev, tr.reg_dw, tr.value);
    tr.mmio_ok = (RREG32(dev, tr.reg_dw) == tr.value);
    WREG32(dev, tr.reg_dw, tr.before);
    SDMA_LOG("srbm-test: %s — reg dword %#x before %#010x; MMIO write of %#010x "
             "%s", tag, tr.reg_dw, tr.before, tr.value,
             tr.mmio_ok ? "READS BACK (the instrument is sound)"
                        : "DOES NOT read back — every leg below would be VOID");
    if (!tr.mmio_ok) return;

    // Ring, Apple form; the Linux form only if the Apple one did not land, so
    // the alternate encoding's stray write never happens needlessly.
    (void)srbm_run_leg(dev, inst, tr, 0, false, false, true, clear,
                       ib_gpu_va, ib_vram_off, tag);
    if (!tr.landed[0])
        (void)srbm_run_leg(dev, inst, tr, 1, false, true, true, clear,
                           ib_gpu_va, ib_vram_off, tag);

    // Same two through an INDIRECT_BUFFER carrying Apple's header.
    (void)srbm_run_leg(dev, inst, tr, 2, true, false, true, clear,
                       ib_gpu_va, ib_vram_off, tag);
    if (!tr.landed[2])
        (void)srbm_run_leg(dev, inst, tr, 3, true, true, true, clear,
                           ib_gpu_va, ib_vram_off, tag);

    // Only when the ring worked and neither IB leg did is bit 31 of the IB
    // header worth a leg of its own: rerun the WINNING operand form with the
    // plain Linux header, so one dword is the only difference.
    if ((tr.landed[0] || tr.landed[1]) && !tr.landed[2] && !tr.landed[3])
        (void)srbm_run_leg(dev, inst, tr, 4, true, tr.landed[1], false, clear,
                           ib_gpu_va, ib_vram_off, tag);

    WREG32(dev, tr.reg_dw, tr.before);
}

kern_return_t
sdma_srbm_selftest(DeviceContext &dev, GMCContext &gmc, SDMAInstance &inst,
                   SDMASRBMTestResult *out)
{
    SDMASRBMTestResult t {};
    if (out) *out = t;

    if (!inst.inited || !inst.enabled) {
        SDMA_LOG("srbm-test: SDMA%u QUEUE0 is not running — skipping",
                 inst.instance);
        return kIOReturnNotReady;
    }
    if (inst.wb_bus == 0) {
        SDMA_LOG("srbm-test: no writeback page — skipping");
        return kIOReturnNotReady;
    }
    if (!dev.ip.isResolved(IPBlock::GC, 0) || !dev.ip.isResolved(IPBlock::GC, 1)) {
        SDMA_LOG("srbm-test: GC base[0]/base[1] unresolved — skipping");
        return kIOReturnNotReady;
    }

    // A 4 KiB VRAM page for the IB. VRAM because the engine already fetches its
    // RING from VRAM through the same GMC path (stage 15 proves it every boot),
    // so an IB that does not execute cannot be blamed on residency.
    VRAMAllocation ib {};
    if (!gmc.vram_alloc.alloc(4096, kASPageSize, &ib)) {
        SDMA_LOG("srbm-test: no VRAM for the IB page — skipping");
        return kIOReturnNoMemory;
    }
    const uint64_t ib_off = ib.gpu_va - gmc.vram_start;
    if (ib_off + 4096 > dev.bar0Size) {
        SDMA_LOG("srbm-test: IB page vram_off %#llx is outside BAR0 (%llu bytes) "
                 "— skipping", (unsigned long long)ib_off,
                 (unsigned long long)dev.bar0Size);
        gmc.vram_alloc.free(ib);
        return kIOReturnNoMemory;
    }
    bar0_memset_vram(dev, ib_off, 0, 4096);
    amdgpu_hdp_flush(dev);

    t.ran          = true;
    t.ib_gpu_va    = ib.gpu_va;
    t.scratch.reg_dw = SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, 1, kSRBMScratchReg);
    t.scratch.value  = kSRBMScratchValue;
    t.ctx2.reg_dw    = SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, 0, kSRBMCtx2Reg);
    t.ctx2.value     = kSRBMCtx2Value;

    SDMA_LOG("srbm-test: SDMA%u QUEUE0, IB page MC %#llx (vram+%#llx, %u dwords "
             "used). SCRATCH_REG7 = GC base[1] + %#x = dword %#x; GCVM_CONTEXT2_"
             "PAGE_TABLE_BASE_ADDR_LO32 = GC base[0] + %#x = dword %#x.",
             inst.instance, (unsigned long long)ib.gpu_va,
             (unsigned long long)ib_off, kSRBMIBDwords,
             kSRBMScratchReg, t.scratch.reg_dw, kSRBMCtx2Reg, t.ctx2.reg_dw);
    SDMA_LOG("srbm-test: if the engine wants the OTHER operand form, the "
             "apple-form legs land on dword %#x / %#x instead and the linux-form "
             "legs on dword %#x / %#x — both outside anything this driver "
             "programs, and the apple form is what Apple's own driver already "
             "emits on this card every boot.",
             t.scratch.reg_dw >> 2, t.ctx2.reg_dw >> 2,
             t.scratch.reg_dw << 2, t.ctx2.reg_dw << 2);

    srbm_run_target(dev, inst, t.scratch, kSRBMScratchClear,
                    ib.gpu_va, ib_off, "scratch(SCRATCH_REG7)");
    srbm_run_target(dev, inst, t.ctx2, 0u,
                    ib.gpu_va, ib_off, "ctx2(GCVM_CONTEXT2 PT base LO)");

    gmc.vram_alloc.free(ib);

    // One line that answers the question on its own.
    SDMA_LOG("srbm-test: ring-scratch %s, ib-scratch %s, ring-ctx2 %s, ib-ctx2 %s "
             "(apple form = hdr 0xf000000e + dword index; linux form = hdr "
             "0x0000000e + reg<<2)",
             t.scratch.landed[0] ? "ok(apple)" : (t.scratch.landed[1] ? "ok(linux)" : "FAILED"),
             t.scratch.landed[2] ? "ok(apple)" : (t.scratch.landed[3] ? "ok(linux)"
                                   : (t.scratch.landed[4] ? "ok(plain-hdr)" : "FAILED")),
             t.ctx2.landed[0]    ? "ok(apple)" : (t.ctx2.landed[1] ? "ok(linux)" : "FAILED"),
             t.ctx2.landed[2]    ? "ok(apple)" : (t.ctx2.landed[3] ? "ok(linux)"
                                   : (t.ctx2.landed[4] ? "ok(plain-hdr)" : "FAILED")));

    if (out) *out = t;
    const bool any = t.scratch.landed[0] || t.scratch.landed[1] ||
                     t.scratch.landed[2] || t.scratch.landed[3] ||
                     t.scratch.landed[4];
    return any ? kIOReturnSuccess : kIOReturnTimeout;
}


//------------------------------------------------------------------
// sdma_ring_write — stage dwords into the VRAM-resident ring via
// the BAR0 framebuffer aperture. Engine fetches them from the same
// addresses via the FB aperture (GMC-routable).
//------------------------------------------------------------------
uint32_t
sdma_ring_write(const DeviceContext &dev, SDMAInstance &inst,
                const uint32_t *src, uint32_t dwords)
{
    if (!inst.inited || dwords == 0) return 0;
    if (dwords > inst.ring_size_dwords) return 0;
    for (uint32_t i = 0; i < dwords; i++) {
        // ring INDEX — upstream's buf_mask (amdgpu_ring.c:122, `ring->wptr & ring->buf_mask`).
        uint32_t slot = static_cast<uint32_t>((inst.wptr + i) & inst.ring_ptr_mask);
        WBAR0_32(dev, inst.ring_vram_off + slot * 4u, src[i]);
    }
    // Ensure HDP write buffers drain so the engine sees the new
    // packets when it processes the doorbell.
    amdgpu_hdp_flush(dev);
    // 0.0.343 — sdma-wptr-freerunning. The POINTER, upstream's ptr_mask
    // (amdgpu_ring.c:134/347-349), which is all-ones for SDMA v7. Masking it here with buf_mask was
    // the bug: it folded the wptr to 0 every 4096 dwords, so sdma_kick_doorbell handed the engine a
    // value that went backwards and the engine executed nothing from that submission on.
    inst.wptr = inst.wptr + dwords;
    return dwords;
}

//------------------------------------------------------------------
// sdma_kick_doorbell — write the new wptr into the BAR2 doorbell
// aperture.
//
// Upstream (sdma_v7_0_ring_set_wptr → WDOORBELL64) does a 64-bit
// write to the doorbell at:
//     BAR2 + ring->doorbell_index * 8
//
// where `ring->doorbell_index = sdma_engine[i] << 1` (already in
// qword-index units, e.g. 0x200 for SDMA0). The PCIe write must be
// 64-bit (WDOORBELL64, not WDOORBELL32) — the doorbell controller
// distinguishes; a 32-bit write may be silently dropped on RDNA4.
//
// Linux uses BAR2 via pci_resource_start(pdev, 2) in
// amdgpu_doorbell_mgr.c. BAR5 holds MMIO registers, NOT doorbells.
//
// Value: `wptr << 2` (byte_wptr); high 32 bits zero.
//------------------------------------------------------------------
kern_return_t
sdma_kick_doorbell(const DeviceContext &dev, const SDMAInstance &inst)
{
    if (!inst.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    const uint64_t v = static_cast<uint64_t>(inst.wptr) << 2;

    // 1) WPTR shadow in the writeback page — upstream
    //    sdma_v7_0_ring_set_wptr line 220 writes (ring->wptr << 2) to
    //    ring->wptr_cpu_addr. With MCU_WPTR_POLL_ENABLE=1 the SDMA MCU
    //    polls this slot. Upstream's contract is the contract.
    sdma_wb_write32(dev, inst, kSDMAWBWptrOffset,
                    static_cast<uint32_t>(v));
    sdma_wb_write32(dev, inst, kSDMAWBWptrOffset + 4,
                    static_cast<uint32_t>(v >> 32));

    // 2) HDP flush so the engine's read of wptr_poll_addr drains past
    //    our CPU write.
    amdgpu_hdp_flush(dev);

    // 3) BAR2 doorbell aperture write — port of upstream WDOORBELL64.
    //    On the reference platform (Apple Silicon + TB5) this write did
    //    NOT cause the SDMA MCU to update its internal RB_WPTR: verified
    //    across mac-amdgpu v0.1.30-v0.1.46 with every plausible NBIF
    //    routing, SELFRING base, WC-flush readback and engine-side
    //    DOORBELL_OFFSET configuration, so there step 4 was the
    //    functional path. [[feedback_mac_amdgpu_doorbell_mmio_mode_as_tb5]]
    //    On x86 in a plain PCIe slot BAR2 is a normal doorbell aperture
    //    and this is expected to be the path that works; both writes are
    //    kept (they carry the same value) and the readback below says
    //    which one the MCU honoured.
    // Dword-indexed aperture: byte offset = doorbell_index * 4 (Linux WDOORBELL64 →
    // amdgpu_mm_wdoorbell64: u32 *ptr + index). The reference used *8. Value stays
    // wptr<<2 (bytes) as in sdma_v7_0_ring_set_wptr.
    const uint64_t offs =
        static_cast<uint64_t>(inst.doorbell_index) * 4ull;
    if (dev.bar2 == nullptr || offs + 8 > dev.bar2Size) {
        SDMA_LOG("SDMA%u kick: doorbell offset %#llx outside BAR2 "
                 "(bar2Size=%llu) — MMIO WPTR path only",
                 inst.instance, (unsigned long long)offs,
                 (unsigned long long)dev.bar2Size);
    } else {
        WDOORBELL64(dev, offs, v);
    }

    // 4) MMIO RB_WPTR write — port of upstream sdma_v7_0_ring_set_wptr's
    //    else-branch (the use_doorbell=false fallback, sdma_v7_0.c:233-241):
    //    write lower<<2 to regSDMA0_QUEUE0_RB_WPTR and upper<<2 to _HI.
    //    Even with engine-side QUEUE0_DOORBELL.ENABLE=1 (PSP default),
    //    the MCU accepts this MMIO update and processes the ring
    //    (mac-amdgpu v0.1.44 verified).
    const uint32_t wptr_reg = sdma_reg_offset(
        dev, inst.instance, sdma_regs(dev).QUEUE0_RB_WPTR);
    const uint32_t wptr_hi_reg = sdma_reg_offset(
        dev, inst.instance, sdma_regs(dev).QUEUE0_RB_WPTR_HI);
    WREG32(dev, wptr_reg,    static_cast<uint32_t>(v));
    WREG32(dev, wptr_hi_reg, static_cast<uint32_t>(v >> 32));

    const uint32_t rptr_reg = sdma_reg_offset(
        dev, inst.instance, sdma_regs(dev).QUEUE0_RB_RPTR);
    SDMA_LOG("SDMA%u kick: wptr=%llu (byte %#llx) doorbell@BAR2+%#llx; "
             "readback RB_WPTR=%#010x RB_RPTR=%#010x wb_rptr=%#010x",
             inst.instance, (unsigned long long)inst.wptr, (unsigned long long)v,
             (unsigned long long)offs, RREG32(dev, wptr_reg),
             RREG32(dev, rptr_reg),
             sdma_wb_read32(dev, inst, kSDMAWBRptrOffset));
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_ring_test — emit FENCE writing a known value to the WB page
// at offset 0x80 then poll for it. Doesn't depend on the host CPU
// having a coherent view; we rely on the engine's writeback.
//
// FENCE packet format (sdma_pkt_open.h):
//   DW0: header   (OP=FENCE)
//   DW1: addr_lo  (dword-aligned bus addr to write)
//   DW2: addr_hi
//   DW3: data     (the fence value)
//------------------------------------------------------------------
kern_return_t
sdma_ring_test(const DeviceContext &dev, SDMAInstance &inst,
               uint64_t timeout_us)
{
    if (!inst.inited || !inst.enabled) return kIOReturnNotReady;

    // Use an unused part of the WB page (offset 0x80) as the fence
    // landing slot. Pre-clear it.
    sdma_wb_write32(dev, inst, kSDMAWBFenceOffset, 0);
    const uint64_t fence_gpu = inst.wb_bus + kSDMAWBFenceOffset;
    const uint32_t fence_value = 0xCAFEC0DEu;

    uint32_t pkt[4];
    pkt[0] = SDMA_PKT_HEADER_OP(SDMA_OP_FENCE);
    pkt[1] = static_cast<uint32_t>(fence_gpu);
    pkt[2] = static_cast<uint32_t>(fence_gpu >> 32);
    pkt[3] = fence_value;

    SDMA_LOG("instance %u: ring_test — FENCE %#010x -> mc %#llx "
             "(ring mc %#llx, wptr %llu)",
             inst.instance, fence_value, (unsigned long long)fence_gpu,
             (unsigned long long)inst.ring_gpu_va, (unsigned long long)inst.wptr);

    if (sdma_ring_write(dev, inst, pkt, 4) != 4) {
        SDMA_LOG("instance %u: ring_test ring_write failed",
                 inst.instance);
        return kIOReturnNoSpace;
    }
    kern_return_t r = sdma_kick_doorbell(dev, inst);
    if (r != kIOReturnSuccess) return r;

    uint64_t elapsed = 0;
    uint32_t last = 0;
    r = sdma_poll_fence(dev, inst, kSDMAWBFenceOffset, fence_value,
                        timeout_us, &elapsed, &last);
    if (r == kIOReturnSuccess) {
        SDMA_LOG("instance %u: ring_test ok in ~%llu us",
                 inst.instance, (unsigned long long)elapsed);
        return kIOReturnSuccess;
    }
    SDMA_LOG("instance %u: ring_test timeout after %llu us (last=%#x)",
             inst.instance, (unsigned long long)elapsed, last);
    sdma_log_status(dev, inst.instance);
    return kIOReturnTimeout;
}

//------------------------------------------------------------------
// sdma_copy_linear_test — emit COPY_LINEAR + FENCE, kick doorbell,
// poll. Proves the engine actually services reads + writes through
// whatever address space the caller hands it (VRAM MC addresses, or
// GART MC addresses for bound sysmem).
//
// Packet shape (sdma_v7_0_emit_copy_buffer in upstream):
//   DW0 = OP_COPY | (SUBOP_COPY_LINEAR << 8) | CPV(1)
//   DW1 = byte_count - 1
//   DW2 = parameters  (0 = no endian swap)
//   DW3 = src lo
//   DW4 = src hi
//   DW5 = dst lo
//   DW6 = dst hi
//   DW7 = 0 (CPV byte)
//------------------------------------------------------------------
//------------------------------------------------------------------
// sdma_const_fill_test — CONSTANT_FILL + FENCE to an arbitrary
// GPU-visible address (Navi48Bringup addition, 0.0.183).
//
// Packet shape, verbatim from sdma_v7_0_emit_fill_buffer
// (ref/linux-amdgpu/sdma_v7_0.c:1796-1808):
//   DW0 = OP_CONST_FILL | COMPRESS(1)      (COMPRESS is bit 16, :56-60)
//   DW1 = dst lo
//   DW2 = dst hi
//   DW3 = fill data (32-bit)
//   DW4 = byte_count - 1
// Then the same FENCE the copy test uses, so the caller knows the engine
// finished rather than merely accepted the packet.
//------------------------------------------------------------------
kern_return_t
sdma_const_fill_test(const DeviceContext &dev, SDMAInstance &inst,
                     uint64_t dst, uint32_t pattern,
                     uint32_t byte_count, uint64_t timeout_us)
{
    if (!inst.inited || !inst.enabled) return kIOReturnNotReady;
    if (byte_count == 0 || (byte_count & 3u) != 0 ||
        byte_count > kSDMACopyLinearMaxBytes) {
        return kIOReturnBadArgument;
    }

    sdma_wb_write32(dev, inst, kSDMAWBFenceOffset, 0);
    const uint64_t fence_gpu   = inst.wb_bus + kSDMAWBFenceOffset;
    const uint32_t fence_value = 0xF111DA7Au;

    uint32_t pkt[9];
    uint32_t n = 0;
    pkt[n++] = SDMA_PKT_HEADER_OP(SDMA_OP_CONST_FILL)
             | SDMA_PKT_CONST_FILL_COMPRESS(1);
    pkt[n++] = static_cast<uint32_t>(dst);
    pkt[n++] = static_cast<uint32_t>(dst >> 32);
    pkt[n++] = pattern;
    pkt[n++] = byte_count - 1;
    pkt[n++] = SDMA_PKT_HEADER_OP(SDMA_OP_FENCE);
    pkt[n++] = static_cast<uint32_t>(fence_gpu);
    pkt[n++] = static_cast<uint32_t>(fence_gpu >> 32);
    pkt[n++] = fence_value;

    SDMA_LOG("instance %u: const_fill %u bytes of %#010x -> %#llx, fence %#010x "
             "-> mc %#llx (%u dwords at ring wptr %llu)",
             inst.instance, byte_count, pattern, (unsigned long long)dst,
             fence_value, (unsigned long long)fence_gpu, n, (unsigned long long)inst.wptr);

    if (sdma_ring_write(dev, inst, pkt, n) != n) {
        SDMA_LOG("const_fill_test: ring_write failed");
        return kIOReturnNoSpace;
    }
    kern_return_t r = sdma_kick_doorbell(dev, inst);
    if (r != kIOReturnSuccess) return r;

    uint64_t elapsed = 0;
    uint32_t last = 0;
    r = sdma_poll_fence(dev, inst, kSDMAWBFenceOffset, fence_value,
                        timeout_us, &elapsed, &last);
    if (r == kIOReturnSuccess) {
        SDMA_LOG("const_fill_test ok: %u bytes -> %#llx in ~%llu us",
                 byte_count, (unsigned long long)dst,
                 (unsigned long long)elapsed);
        return kIOReturnSuccess;
    }
    SDMA_LOG("const_fill_test: timeout after %llu us (last fence=%#x)",
             (unsigned long long)elapsed, last);
    sdma_log_status(dev, inst.instance);
    return kIOReturnTimeout;
}

kern_return_t
sdma_copy_linear_test(const DeviceContext &dev, SDMAInstance &inst,
                      uint64_t src_bus, uint64_t dst_bus,
                      uint32_t byte_count, uint64_t timeout_us, bool cpv)
{
    if (!inst.inited || !inst.enabled) return kIOReturnNotReady;
    if (byte_count == 0 || byte_count > kSDMACopyLinearMaxBytes) {
        return kIOReturnBadArgument;
    }

    sdma_wb_write32(dev, inst, kSDMAWBFenceOffset, 0);
    const uint64_t fence_gpu   = inst.wb_bus + kSDMAWBFenceOffset;
    const uint32_t fence_value = 0xDEC0FFEEu;

    uint32_t pkt[12];
    uint32_t n = 0;
    // COPY_LINEAR
    pkt[n++] = SDMA_PKT_HEADER_OP(SDMA_OP_COPY)
             | SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR)
             | (cpv ? SDMA_PKT_HEADER_CPV(1) : 0u);
    pkt[n++] = byte_count - 1;
    pkt[n++] = 0;                          // endian swap params = 0
    pkt[n++] = static_cast<uint32_t>(src_bus);
    pkt[n++] = static_cast<uint32_t>(src_bus >> 32);
    pkt[n++] = static_cast<uint32_t>(dst_bus);
    pkt[n++] = static_cast<uint32_t>(dst_bus >> 32);
    // 0.0.339: the DCC/compression control dword exists ONLY when CPV is set.
    // Emitting it with CPV clear would desynchronise the ring.
    if (cpv) pkt[n++] = 0;                 // CPV byte
    // FENCE — engine writes fence_value to fence_gpu after the copy.
    pkt[n++] = SDMA_PKT_HEADER_OP(SDMA_OP_FENCE);
    pkt[n++] = static_cast<uint32_t>(fence_gpu);
    pkt[n++] = static_cast<uint32_t>(fence_gpu >> 32);
    pkt[n++] = fence_value;

    SDMA_LOG("instance %u: copy_linear %u bytes %#llx -> %#llx, cpv %u, header "
             "%#010x, fence %#010x -> mc %#llx (%u dwords at ring wptr %llu)",
             inst.instance, byte_count, (unsigned long long)src_bus,
             (unsigned long long)dst_bus, (unsigned)(cpv ? 1u : 0u), pkt[0],
             fence_value, (unsigned long long)fence_gpu, n, (unsigned long long)inst.wptr);

    if (sdma_ring_write(dev, inst, pkt, n) != n) {
        SDMA_LOG("copy_linear_test: ring_write failed");
        return kIOReturnNoSpace;
    }
    kern_return_t r = sdma_kick_doorbell(dev, inst);
    if (r != kIOReturnSuccess) return r;

    uint64_t elapsed = 0;
    uint32_t last = 0;
    r = sdma_poll_fence(dev, inst, kSDMAWBFenceOffset, fence_value,
                        timeout_us, &elapsed, &last);
    if (r == kIOReturnSuccess) {
        SDMA_LOG("copy_linear_test ok: %u bytes %#llx -> %#llx in ~%llu us",
                 byte_count,
                 (unsigned long long)src_bus,
                 (unsigned long long)dst_bus,
                 (unsigned long long)elapsed);
        return kIOReturnSuccess;
    }
    SDMA_LOG("copy_linear_test: timeout after %llu us (last fence=%#x)",
             (unsigned long long)elapsed, last);
    sdma_log_status(dev, inst.instance);
    return kIOReturnTimeout;
}

//------------------------------------------------------------------
// sdma_ib_copy_linear_test (0.0.193) — the SAME COPY_LINEAR packet, but
// executed from an INDIRECT_BUFFER that carries a VMID, so `src` and `dst` are
// VIRTUAL addresses resolved through that VMID's page table instead of VMID-0
// MC addresses.
//
// This is the only way to exercise a per-process context from a queue we own:
// the ring itself always runs on the queue's VMID (0 for SDMA0 QUEUE0), and
// sdma_v7_0_ring_emit_ib (ref/linux-amdgpu/sdma_v7_0.c:283-292) puts the IB's
// VMID in header bits [19:16].
//
// The FENCE stays in the RING, i.e. on VMID 0, deliberately: if the IB's own
// VMID is broken the fence still lands and the failure reads "the copy did not
// happen" rather than "the engine vanished".
//
// Instrument discipline (rule 27): every part is already proven on this
// silicon — the COPY_LINEAR packet is stage 15's, the IB submission path is the
// 0.0.191 SRBM self-test's (leg 3 landed), the fence and poll are stage 15's.
// The only new thing under test is the VMID field and the table behind it.
//------------------------------------------------------------------
kern_return_t
sdma_ib_copy_linear_test(const DeviceContext &dev, SDMAInstance &inst,
                         uint64_t ib_gpu_va, uint64_t ib_vram_off,
                         uint64_t src, uint64_t dst, uint32_t byte_count,
                         uint32_t vmid, uint32_t tag, uint64_t timeout_us)
{
    if (!inst.inited || !inst.enabled) return kIOReturnNotReady;
    if (byte_count == 0 || byte_count > kSDMACopyLinearMaxBytes)
        return kIOReturnBadArgument;
    if (vmid > 15) return kIOReturnBadArgument;
    if ((ib_gpu_va & 0x1Full) != 0) return kIOReturnNotAligned;

    // The IB: COPY_LINEAR padded with NOPs to 8 dwords (sdma_v7_0_ring_pad_ib's
    // "IB length is a multiple of 8" rule).
    constexpr uint32_t kIbDwords = 8;
    uint32_t ibd[kIbDwords] = {};
    ibd[0] = SDMA_PKT_HEADER_OP(SDMA_OP_COPY)
           | SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR)
           | SDMA_PKT_HEADER_CPV(1);
    ibd[1] = byte_count - 1;
    ibd[2] = 0;
    ibd[3] = static_cast<uint32_t>(src);
    ibd[4] = static_cast<uint32_t>(src >> 32);
    ibd[5] = static_cast<uint32_t>(dst);
    ibd[6] = static_cast<uint32_t>(dst >> 32);
    ibd[7] = 0;   // CPV byte
    for (uint32_t i = 0; i < kIbDwords; i++)
        WBAR0_32(dev, ib_vram_off + i * 4u, ibd[i]);
    amdgpu_hdp_flush(dev);

    const uint64_t fence_gpu   = inst.wb_bus + kSDMAWBFenceOffset;
    const uint32_t fence_value = 0x5C000000u | (tag & 0xFFFFu);
    sdma_wb_write32(dev, inst, kSDMAWBFenceOffset, 0);

    uint32_t pkt[20];
    uint32_t n = 0;
    const uint32_t nops = (2u - static_cast<uint32_t>(inst.wptr)) & 7u;
    for (uint32_t i = 0; i < nops; i++) pkt[n++] = SDMA_PKT_HEADER_OP(SDMA_OP_NOP);
    const uint32_t ib_hdr = SDMA_PKT_HEADER_OP(SDMA_OP_INDIRECT)
                          | SDMA_PKT_INDIRECT_HEADER_VMID(vmid);
    pkt[n++] = ib_hdr;
    pkt[n++] = static_cast<uint32_t>(ib_gpu_va) & 0xFFFFFFE0u;
    pkt[n++] = static_cast<uint32_t>(ib_gpu_va >> 32);
    pkt[n++] = kIbDwords;
    pkt[n++] = 0;   // csa lo
    pkt[n++] = 0;   // csa hi
    pkt[n++] = SDMA_PKT_HEADER_OP(SDMA_OP_FENCE);
    pkt[n++] = static_cast<uint32_t>(fence_gpu);
    pkt[n++] = static_cast<uint32_t>(fence_gpu >> 32);
    pkt[n++] = fence_value;

    SDMA_LOG("ib-copy(vmid %u): %u bytes VA %#llx -> VA %#llx; IB hdr %#010x at "
             "mc %#llx (vram+%#llx, %u dw); fence %#010x -> %#llx; %u ring dwords "
             "at wptr %llu", vmid, byte_count, (unsigned long long)src,
             (unsigned long long)dst, ib_hdr, (unsigned long long)ib_gpu_va,
             (unsigned long long)ib_vram_off, kIbDwords, fence_value,
             (unsigned long long)fence_gpu, n, (unsigned long long)inst.wptr);

    if (sdma_ring_write(dev, inst, pkt, n) != n) {
        SDMA_LOG("ib-copy: ring_write failed");
        return kIOReturnNoSpace;
    }
    kern_return_t r = sdma_kick_doorbell(dev, inst);
    if (r != kIOReturnSuccess) return r;

    uint64_t elapsed = 0;
    uint32_t last = 0;
    r = sdma_poll_fence(dev, inst, kSDMAWBFenceOffset, fence_value,
                        timeout_us, &elapsed, &last);
    if (r == kIOReturnSuccess) {
        SDMA_LOG("ib-copy(vmid %u) fenced in ~%llu us", vmid,
                 (unsigned long long)elapsed);
        return kIOReturnSuccess;
    }
    SDMA_LOG("ib-copy(vmid %u): timeout after %llu us (last fence=%#x)", vmid,
             (unsigned long long)elapsed, last);
    sdma_log_status(dev, inst.instance);
    return kIOReturnTimeout;
}

//------------------------------------------------------------------
// sdma_log_status — read and log per-instance SDMA_STATUS_REG.
// Mirrors what `sdma_v7_0_wait_for_idle` reads in upstream
// (sdma_v7_0.c:1478). Use this as a quick "is this engine alive"
// check from any diagnostic path. Bits of interest:
//   [0]   IDLE         — engine is idle
//   [1]   REG_IDLE     — register interface idle
//   [4]   RB_EMPTY     — queue 0 ring buffer empty
//   [8]   RB_CMD_IDLE  — ring command unit idle
//   [9]   RB_CMD_FULL  — ring command unit full (back-pressure)
//   [12]  IB_CMD_IDLE  — IB command unit idle
//   [16]  MC_WR_IDLE   — memory controller writes idle
//   [17]  SRBM_IDLE    — SRBM idle
//   [18]  CONTEXT_EMPTY
//   [19]  DELTA_RPTR_FULL
//   [24]  PREV_CMD_IDLE
//   [25]  PREV_HASHTAG_VALID
//   [29]  REG_CG_REQ
//   [30]  REG_CG_GRANT
//------------------------------------------------------------------
void
sdma_log_status(const DeviceContext &dev, uint32_t inst)
{
    if (!dev.ip.isResolved(IPBlock::GC)) {
        SDMA_LOG("status[%u]: GC IP not resolved", inst);
        return;
    }
    uint32_t status = RREG32(dev,
        sdma_reg_offset(dev, inst, sdma_regs(dev).STATUS_REG));
    uint32_t rb_rptr = RREG32(dev,
        sdma_reg_offset(dev, inst, sdma_regs(dev).QUEUE0_RB_RPTR));
    uint32_t rb_wptr = RREG32(dev,
        sdma_reg_offset(dev, inst, sdma_regs(dev).QUEUE0_RB_WPTR));
    uint32_t rb_cntl = RREG32(dev,
        sdma_reg_offset(dev, inst, sdma_regs(dev).QUEUE0_RB_CNTL));
    // STATUS_REG bit positions per gc_12_0_0_sh_mask.h:142-161.
    SDMA_LOG("SDMA%u status: STATUS_REG=%#010x RB_CNTL=%#010x "
             "rptr=%#x wptr=%#x  "
             "(idle=%u rb_empty=%u rb_full=%u ib_idle=%u srbm_idle=%u)",
             inst, status, rb_cntl, rb_rptr, rb_wptr,
             (status >> 0)  & 1,   // IDLE
             (status >> 2)  & 1,   // RB_EMPTY
             (status >> 3)  & 1,   // RB_FULL
             (status >> 6)  & 1,   // IB_CMD_IDLE
             (status >> 14) & 1);  // SRBM_IDLE
}

//------------------------------------------------------------------
// sdma_load_microcode — deviation D3.
//
// Port of the SDMA half of the reference's LoadFirmware selector
// (dext/MacAMDGPU.cpp): decode the .bin into LOAD_IP_FW payloads,
// stage each into the PSP fw_buf (VRAM, reachable through the FB
// aperture), submit it, and flip microcode_loaded once PSP acks.
// RDNA4 packs both engines into a single RS64 image tagged
// GFX_FW_TYPE_SDMA_UCODE_TH0 (71).
//------------------------------------------------------------------
kern_return_t
sdma_load_microcode(DeviceContext &dev, PSPContext &psp, SDMAContext &sdma,
                    const void *bin, uint32_t size)
{
    if (sdma.microcode_loaded) return kIOReturnSuccess;
    if (bin == nullptr || size == 0) {
        SDMA_LOG("load_microcode: no firmware bytes supplied");
        return kIOReturnBadArgument;
    }

    UcodePayload payloads[kMaxUcodePayloadsPerFile];
    const uint8_t *bytes = static_cast<const uint8_t *>(bin);
    uint32_t count = amdgpu_ucode_extract(kHostFwType_SDMA, bytes, size,
                                          payloads);
    if (count == 0) {
        SDMA_LOG("load_microcode: extractor returned 0 payloads for a "
                 "%u-byte sdma .bin (expected sdma_firmware_header_v3_0)",
                 size);
        return kIOReturnUnsupported;
    }

    SDMA_LOG("load_microcode: %u payload(s) from a %u-byte image",
             count, size);
    for (uint32_t i = 0; i < count; i++) {
        if (static_cast<uint64_t>(payloads[i].offset_bytes) +
                payloads[i].size_bytes > size) {
            SDMA_LOG("load_microcode: payload[%u] out of bounds "
                     "(off=%u size=%u file=%u)",
                     i, payloads[i].offset_bytes, payloads[i].size_bytes,
                     size);
            return kIOReturnBadArgument;
        }
        uint64_t fwBusAddr = 0;
        kern_return_t r = psp_fw_buf_stage(dev, psp,
                                           bytes + payloads[i].offset_bytes,
                                           payloads[i].size_bytes,
                                           &fwBusAddr);
        if (r != kIOReturnSuccess) {
            SDMA_LOG("load_microcode: fw_buf_stage payload[%u] failed kr=%#x",
                     i, r);
            return r;
        }
        SDMA_LOG("load_microcode: payload[%u] fw_type=%u src_off=%u size=%u "
                 "staged at mc=%#llx",
                 i, payloads[i].fw_type, payloads[i].offset_bytes,
                 payloads[i].size_bytes, (unsigned long long)fwBusAddr);
        r = psp_load_ip_fw(dev, psp, fwBusAddr, payloads[i].size_bytes,
                           payloads[i].fw_type);
        if (r != kIOReturnSuccess) {
            SDMA_LOG("load_microcode: LOAD_IP_FW(fw_type=%u) FAILED kr=%#x",
                     payloads[i].fw_type, r);
            return r;
        }
        SDMA_LOG("load_microcode: fw_type=%u loaded (size=%u)",
                 payloads[i].fw_type, payloads[i].size_bytes);
    }

    sdma.microcode_loaded = true;
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_vram_copy_test — deviation D4.
//
// Port of the reference's kMacAMDGPUMethodSDMACopyVRAM selector. Both
// buffers come from the VRAM allocator, so the test is independent of
// GART and of any host aperture: CPU stages the source through BAR0,
// the engine copies VRAM→VRAM through the GMC walk, and the result is
// read back through MM_INDEX/MM_DATA (the GPU's own view of VRAM).
// This is the "the GPU wrote memory" milestone.
//------------------------------------------------------------------
kern_return_t
sdma_vram_copy_test(DeviceContext &dev, GMCContext &gmc, SDMAInstance &inst,
                    uint32_t bytes, SDMAVRAMCopyResult *out)
{
    SDMAVRAMCopyResult res{};
    res.kr = kIOReturnNotReady;
    if (out) *out = res;

    if (!gmc.vram_alloc.is_inited()) {
        SDMA_LOG("vram_copy_test: vram_alloc not initialised (GMC stage "
                 "hasn't run)");
        return kIOReturnNotReady;
    }
    if (!inst.inited || !inst.enabled) {
        SDMA_LOG("vram_copy_test: SDMA%u not ready (inited=%d enabled=%d)",
                 inst.instance, (int)inst.inited, (int)inst.enabled);
        return kIOReturnNotReady;
    }

    // Reference default was 4096 bytes, capped at 16 KB because it kept
    // the pattern on the stack. We generate the pattern on the fly, so
    // the cap is just "one VRAM allocator granule".
    constexpr uint32_t kMaxBytes = 64 * 1024;
    if (bytes == 0) bytes = 4096;
    if (bytes > kMaxBytes) bytes = kMaxBytes;
    bytes &= ~3u;                      // dword-aligned readback
    if (bytes == 0) return kIOReturnBadArgument;

    VRAMAllocation src{};
    VRAMAllocation dst{};
    if (!gmc.vram_alloc.alloc(bytes, kASPageSize, &src)) {
        SDMA_LOG("vram_copy_test: VRAM alloc for src failed (bytes=%u)", bytes);
        return kIOReturnNoSpace;
    }
    if (!gmc.vram_alloc.alloc(bytes, kASPageSize, &dst)) {
        SDMA_LOG("vram_copy_test: VRAM alloc for dst failed (bytes=%u)", bytes);
        gmc.vram_alloc.free(src);
        return kIOReturnNoSpace;
    }
    const uint64_t src_vram_off = src.gpu_va - gmc.vram_start;
    const uint64_t dst_vram_off = dst.gpu_va - gmc.vram_start;
    const uint32_t n_dwords     = bytes / 4;

    // Stage src with an incrementing pattern (0xCAFE0000 + i) and
    // pre-poison dst so a no-op copy would be visible.
    for (uint32_t i = 0; i < n_dwords; i++) {
        WBAR0_32(dev, src_vram_off + static_cast<uint64_t>(i) * 4,
                 0xCAFE0000u + i);
    }
    bar0_memset_vram(dev, dst_vram_off, 0xDEADBEEFu, bytes);
    amdgpu_hdp_flush(dev);

    SDMA_LOG("vram_copy_test: SDMA%u bytes=%u src.gpu_va=%#llx "
             "(vram_off=%#llx) dst.gpu_va=%#llx (vram_off=%#llx)",
             inst.instance, bytes,
             (unsigned long long)src.gpu_va, (unsigned long long)src_vram_off,
             (unsigned long long)dst.gpu_va, (unsigned long long)dst_vram_off);

    kern_return_t r = sdma_copy_linear_test(dev, inst, src.gpu_va, dst.gpu_va,
                                            bytes, /*timeout_us=*/100000ull);

    // Read back regardless of fence status — even a partial copy tells
    // us whether the engine touched dst at all.
    uint32_t mismatched = 0;
    uint32_t first_bad_off = 0;
    bool first_bad_set = false;
    for (uint32_t i = 0; i < n_dwords; i++) {
        uint32_t g = RVRAM32_via_mm(dev,
                                    dst_vram_off + static_cast<uint64_t>(i) * 4);
        if (g != 0xCAFE0000u + i) {
            if (!first_bad_set) {
                first_bad_off = i * 4;
                first_bad_set = true;
                SDMA_LOG("vram_copy_test: first mismatch at +%#x: got %#010x "
                         "want %#010x", first_bad_off, g, 0xCAFE0000u + i);
            }
            mismatched++;
        }
    }

    res.kr            = r;
    res.bytes         = bytes;
    res.dwords        = n_dwords;
    res.mismatched    = mismatched;
    res.first_bad_off = first_bad_off;
    res.src_gpu_va    = src.gpu_va;
    res.dst_gpu_va    = dst.gpu_va;
    res.elapsed_us    = 0;
    if (out) *out = res;

    SDMA_LOG("vram_copy_test: result kr=%#x mismatched=%u/%u first_bad_off=%#x",
             r, mismatched, n_dwords, first_bad_off);
    if (r == kIOReturnSuccess && mismatched == 0) {
        SDMA_LOG("vram_copy_test: *** SDMA%u COPIED %u BYTES OF VRAM — the "
                 "GPU executed a command buffer ***", inst.instance, bytes);
    }

    gmc.vram_alloc.free(dst);
    gmc.vram_alloc.free(src);

    if (r != kIOReturnSuccess) return r;
    return (mismatched == 0) ? kIOReturnSuccess : kIOReturnIOError;
}

//------------------------------------------------------------------
// sdma_vram_copy_sweep (0.0.339) — WHY THIS EXISTS.
//
// sdma_vram_copy_test above PASSES — `mismatched=0/1024`, SDMA0, VRAM
// 0x200c000 -> 0x2010000 — in every one of the seven broken-era boots that
// were checked, minutes before the scanout pre-flight's 64 KiB copy at
// 0x2024000 -> 0x2034000 mis-delivers 7904 of 16384 dwords. Same engine, same
// instance, same packet builder, same boot. So COPY_LINEAR is not broken and
// the DRAM is not marginal: the discriminator is SIZE, the ADDRESS PAIR, or
// the packet header. This walks all three.
//
// Three rules the earlier instrument broke, and why they are here:
//  1. ONE REGION. Every case is carved out of a single 256 KiB allocation, so
//     changing a size never changes an address behind our back — the allocator
//     rounds every request up to kASPageSize and first-fits, so allocating per
//     case would confound the two axes completely.
//  2. UNIQUE PER DWORD. The pre-flight's pattern (bar colour, row byte) repeats
//     every 32 pixels, which is the entire reason could only say
//     "128-byte columns". Here a dword names its own case AND its own index.
//  3. LOG THE MAP, NOT COUNTS. Counts have been printed for 25 runs and named
//     nothing. Every mismatch is decoded back to the source dword that owns the
//     value, deltas are histogrammed, and the first 64 are printed in full.
//
// Discipline: no new register and no new register offset. WBAR0_32 / RBAR0_32 /
// RVRAM32_via_mm / bar0_memset_vram / amdgpu_hdp_flush / sdma_copy_linear_test
// are exactly the calls sdma_vram_copy_test already makes. Nothing outside our
// own allocation is written, and the region is freed before returning, which
// returns the allocator's free list to the state it was in (free() coalesces),
// so every later stage's VRAM address is unchanged.
//------------------------------------------------------------------
namespace {

// Top byte 0xCA marks a sweep pattern value; bits 23:20 the case index; bits
// 19:0 the dword index inside that case's source buffer. Unique per position,
// and a value that arrives from the WRONG CASE is identifiable as such.
inline uint32_t sweep_pat(uint32_t c, uint32_t i) {
    return 0xCA000000u | ((c & 0xFu) << 20) | (i & 0x000FFFFFu);
}
inline bool sweep_decode(uint32_t v, uint32_t *c, uint32_t *i) {
    if ((v >> 24) != 0xCAu) return false;
    *c = (v >> 20) & 0xFu;
    *i = v & 0x000FFFFFu;
    return true;
}
// Lowest set bit of an offset = its alignment in bytes (0 offset -> the whole
// region's alignment, reported as 0 and read as "base").
inline uint32_t sweep_align(uint64_t off) {
    if (off == 0) return 0;
    uint32_t a = 1;
    while ((off & a) == 0 && a < 0x400000u) a <<= 1;
    return a;
}

struct SweepCase {
    uint32_t src_off;     // byte offset inside the region
    uint32_t dst_off;
    uint32_t bytes;
    uint8_t  cpv;
    const char *what;
};

} // namespace

kern_return_t
sdma_vram_copy_sweep(DeviceContext &dev, GMCContext &gmc, SDMAInstance &inst)
{
    if (!gmc.vram_alloc.is_inited() || !inst.inited || !inst.enabled) {
        SDMA_LOG("copy_sweep: NOT RUN (vram_alloc %d, SDMA%u inited %d enabled %d)",
                 (int)gmc.vram_alloc.is_inited(), inst.instance,
                 (int)inst.inited, (int)inst.enabled);
        return kIOReturnNotReady;
    }

    // Decoder self-test — a positive AND a negative control for the map code
    // itself, printed whether or not any case fails. Without it, a sweep in
    // which everything passes would leave the decode path unexercised and
    // unproven (the "check void by its own control" failure).
    {
        uint32_t dc = 0xff, di = 0xffffffffu;
        const uint32_t p = sweep_pat(3u, 0x1234u);
        const bool ok   = sweep_decode(p, &dc, &di);
        uint32_t nc = 0, ni = 0;
        const bool bad1 = sweep_decode(0xDEADBEEFu, &nc, &ni);
        const bool bad2 = sweep_decode(0x00FFFFFFu, &nc, &ni);
        SDMA_LOG("copy_sweep: decoder self-test: pat(3,0x1234)=%#010x decodes=%d "
                 "case=%u idx=%#x (want 1/3/0x1234); 0xdeadbeef decodes=%d, "
                 "0x00ffffff decodes=%d (want 0/0)",
                 p, (int)ok, dc, di, (int)bad1, (int)bad2);
    }

    constexpr uint32_t kRegionBytes = 256u * 1024u;
    VRAMAllocation rgn{};
    if (!gmc.vram_alloc.alloc(kRegionBytes, kASPageSize, &rgn)) {
        SDMA_LOG("copy_sweep: region alloc of %u bytes FAILED", kRegionBytes);
        return kIOReturnNoSpace;
    }
    const uint64_t rOff = rgn.gpu_va - gmc.vram_start;
    if (rOff + kRegionBytes > dev.bar0Size) {
        SDMA_LOG("copy_sweep: region vram_off %#llx + %u is outside the BAR0 "
                 "aperture (%#llx) — REFUSING (we stage through BAR0)",
                 (unsigned long long)rOff, kRegionBytes,
                 (unsigned long long)dev.bar0Size);
        gmc.vram_alloc.free(rgn);
        return kIOReturnNoSpace;
    }

    // The case table. Offsets are chosen so that, when the region lands where
    // sdma_vram_copy_test's src lands (VRAM 0x200c000 in every logged boot),
    // +0x18000 is 0x2024000 and +0x28000 is 0x2034000 — the pre-flight's OWN
    // source and destination, to the byte. No dst range ever overlaps its src.
    static const SweepCase kCases[] = {
        { 0x00000u, 0x04000u,  4u * 1024u, 1, "CONTROL REPLICA 4K, 16K apart" },
        { 0x00000u, 0x20000u,  4u * 1024u, 1, "SIZE axis 4K" },
        { 0x00000u, 0x20000u,  8u * 1024u, 1, "SIZE axis 8K" },
        { 0x00000u, 0x20000u, 16u * 1024u, 1, "SIZE axis 16K" },
        { 0x00000u, 0x20000u, 32u * 1024u, 1, "SIZE axis 32K" },
        { 0x00000u, 0x20000u, 64u * 1024u, 1, "SIZE axis 64K -- DECISIVE" },
        { 0x01000u, 0x21000u,  4u * 1024u, 1, "ADDR axis 4K-aligned only" },
        { 0x02000u, 0x22000u,  4u * 1024u, 1, "ADDR axis 8K-aligned only" },
        { 0x18000u, 0x28000u,  4u * 1024u, 1, "PRE-FLIGHT address pair, 4K" },
        { 0x18000u, 0x28000u, 64u * 1024u, 1, "PRE-FLIGHT exact geometry -- DECISIVE" },
        { 0x00000u, 0x10000u, 64u * 1024u, 1, "64K adjacent, different address" },
        { 0x00000u, 0x20000u, 64u * 1024u, 0, "SIZE axis 64K, CPV=0" },
        { 0x18000u, 0x28000u, 64u * 1024u, 0, "PRE-FLIGHT geometry, CPV=0" },
        // 0.0.340: three IN-BOOT REPEATS, appended so cases 0-12 keep their
        // exact addresses, sizes, order and pattern and stay comparable with
        // run sweep1. They answer the one thing sweep1 could not: is the fault
        // DETERMINISTIC by address, or does the same address pair give a
        // different answer the second time in the same boot? c=8 read 1024 of
        // 1024 wrong and c=12 read 0 of 16384 wrong at the SAME destination.
        { 0x18000u, 0x28000u,  4u * 1024u, 1, "REPEAT of c=8 (pre-flight pair 4K)" },
        { 0x02000u, 0x22000u,  4u * 1024u, 1, "REPEAT of c=7 (the persistently bad region)" },
        { 0x00000u, 0x04000u,  4u * 1024u, 1, "REPEAT of c=0 (control replica, LAST)" },
    };
    constexpr uint32_t kNCases = sizeof(kCases) / sizeof(kCases[0]);

    SDMA_LOG("copy_sweep: START %u cases in ONE region: gpu_va %#llx vram_off "
             "%#llx size %u (region+0x18000 = %#llx, region+0x28000 = %#llx — "
             "the scanout pre-flight uses 0x2024000 -> 0x2034000)",
             kNCases, (unsigned long long)rgn.gpu_va, (unsigned long long)rOff,
             kRegionBytes, (unsigned long long)(rOff + 0x18000u),
             (unsigned long long)(rOff + 0x28000u));

    uint32_t casesBad = 0, mapsLogged = 0;

    for (uint32_t c = 0; c < kNCases; c++) {
        const SweepCase &K = kCases[c];
        const uint32_t nd  = K.bytes / 4u;
        const uint64_t sOff = rOff + K.src_off;
        const uint64_t dOff = rOff + K.dst_off;

        // Stage: unique pattern in src, poison in dst.
        for (uint32_t i = 0; i < nd; i++)
            WBAR0_32(dev, sOff + (uint64_t)i * 4u, sweep_pat(c, i));
        bar0_memset_vram(dev, dOff, 0xDEADBEEFu, K.bytes);
        amdgpu_hdp_flush(dev);

        // Controls BEFORE the copy: did the pattern land, did the poison land?
        //
        // 0.0.340 — THE ONE sweep1 DID NOT HAVE, and the one its result demands.
        // In sweep1 every wrong dword the map printed was 0x00000000, the poison
        // had gone, and the failures tracked the SOURCE address rather than the
        // destination. The hypothesis that fits is that our own BAR0 staging is
        // not visible to the engine when it reads — and `srcStaged bad 0` cannot
        // refute it, because a BAR0 read is served from the same host aperture
        // that holds the pending BAR0 writes. That is a control alive while the
        // method is blind. So read the WHOLE source a second way as well, through
        // MM_INDEX/MM_DATA, before the copy. Still a host path, but a different
        // one, and the one this file uses for memory the engine touches. If the
        // MM window sees zeros where BAR0 sees the pattern, the staging is the
        // fault and the fix is ours; if both see the pattern, the source really
        // is correct in memory and the fault is in the engine's read.
        uint32_t srcCtl = 0, poisonCtl = 0, srcCtlMm = 0, srcCtlMmZero = 0;
        for (uint32_t i = 0; i < nd; i++) {
            const uint32_t want = sweep_pat(c, i);
            if (RBAR0_32(dev, sOff + (uint64_t)i * 4u) != want) srcCtl++;
            const uint32_t mmv = RVRAM32_via_mm(dev, sOff + (uint64_t)i * 4u);
            if (mmv != want) { srcCtlMm++; if (mmv == 0u) srcCtlMmZero++; }
            if (RBAR0_32(dev, dOff + (uint64_t)i * 4u) != 0xDEADBEEFu) poisonCtl++;
        }

        const kern_return_t r =
            sdma_copy_linear_test(dev, inst, rgn.gpu_va + K.src_off,
                                  rgn.gpu_va + K.dst_off, K.bytes,
                                  /*timeout_us=*/500000ull, K.cpv != 0);

        // Read back. The MM window is the verdict (it is what
        // sdma_vram_copy_test uses); BAR0 is carried as a second opinion so the
        // two can never disagree silently.
        uint32_t mmBad = 0, barBad = 0, poisonLeft = 0, zeros = 0, alien = 0, otherCase = 0;
        int32_t  dKey[16] = { 0 };
        uint32_t dCnt[16] = { 0 };
        uint32_t nKeys = 0, dOther = 0;
        uint32_t sIdx[24] = { 0 }, sGot[24] = { 0 }, nS = 0;
        // 0.0.340: the DISPLACED samples, collected separately. In run sweep1
        // every one of the first 64 mismatches was 0x00000000, so the 64
        // sampled mismatches carried no permutation at all while the delta
        // histogram showed 960 displaced dwords existed. These are the ones
        // that name a source.
        uint32_t pIdx[24] = { 0 }, pGot[24] = { 0 }, nP = 0;
        // Where inside a 256-byte block do the bad dwords sit? Run sweep1's
        // first-64 map said "bytes 0x80-0xff of every 256-byte block", which is
        // 128-byte structure; these two histograms measure it over the WHOLE
        // buffer and at 64-byte resolution, so the granularity is read off the
        // data instead of eyeballed from 64 samples.
        uint32_t half[2] = { 0 }, quarter[4] = { 0 };
        uint64_t firstBadOff = 0, lastBadOff = 0;
        bool haveFirstBad = false;

        for (uint32_t i = 0; i < nd; i++) {
            const uint32_t want = sweep_pat(c, i);
            const uint64_t at   = dOff + (uint64_t)i * 4u;
            const uint32_t got  = RVRAM32_via_mm(dev, at);
            if (RBAR0_32(dev, at) != want) barBad++;
            if (got == want) continue;
            mmBad++;
            if (!haveFirstBad) { firstBadOff = at; haveFirstBad = true; }
            lastBadOff = at;
            half[(at >> 7) & 1u]++;
            quarter[(at >> 6) & 3u]++;
            if (nS < 24) { sIdx[nS] = i; sGot[nS] = got; nS++; }
            if (got == 0xDEADBEEFu) { poisonLeft++; continue; }
            if (got == 0u) { zeros++; continue; }
            uint32_t gc = 0, gi = 0;
            if (!sweep_decode(got, &gc, &gi)) { alien++; continue; }
            if (nP < 24) { pIdx[nP] = i; pGot[nP] = got; nP++; }
            if (gc != c) { otherCase++; continue; }
            const int32_t delta = (int32_t)gi - (int32_t)i;
            uint32_t k = 0;
            for (; k < nKeys; k++) if (dKey[k] == delta) { dCnt[k]++; break; }
            if (k == nKeys) {
                if (nKeys < 16) { dKey[nKeys] = delta; dCnt[nKeys] = 1; nKeys++; }
                else dOther++;
            }
        }

        // 0.0.340, the read-path control sweep1 lacked: read the whole
        // destination ONCE MORE through BAR0 after an HDP flush. The same call
        // the scanout pre-flight makes for the same reason. If this number
        // differs from barBad, the first read was stale and every count above
        // is suspect; if it matches, the bytes in memory really are wrong.
        amdgpu_hdp_flush(dev);
        uint32_t barBad2 = 0;
        for (uint32_t i = 0; i < nd; i++)
            if (RBAR0_32(dev, dOff + (uint64_t)i * 4u) != sweep_pat(c, i)) barBad2++;

        // The source, re-read after the copy: if anything else writes VRAM
        // under us, this is where it shows.
        uint32_t srcAfter = 0;
        for (uint32_t i = 0; i < nd; i++)
            if (RBAR0_32(dev, sOff + (uint64_t)i * 4u) != sweep_pat(c, i)) srcAfter++;

        SDMA_LOG("copy_sweep c=%u [%s]: src vram_off %#llx dst %#llx bytes %u "
                 "cpv %u dist %#x srcAlign %u dstAlign %u -> kr=%#x | MM bad "
                 "%u of %u, BAR0 bad %u, BAR0 bad after hdp_flush %u | zeros %u "
                 "poisonLeft %u alien %u fromOtherCase %u displaced %u | first "
                 "bad vram_off %#llx last %#llx | controls: srcStaged bad %u "
                 "(BAR0) / %u (MM WINDOW, of which %u read as zero), "
                 "poisonLanded bad %u, srcAfterCopy bad %u",
                 c, K.what, (unsigned long long)sOff, (unsigned long long)dOff,
                 K.bytes, (unsigned)K.cpv, K.dst_off - K.src_off,
                 sweep_align(sOff), sweep_align(dOff), r, mmBad, nd, barBad,
                 barBad2, zeros, poisonLeft, alien, otherCase,
                 mmBad - zeros - poisonLeft - alien,
                 (unsigned long long)firstBadOff, (unsigned long long)lastBadOff,
                 srcCtl, srcCtlMm, srcCtlMmZero, poisonCtl, srcAfter);

        if (mmBad == 0) continue;
        casesBad++;

        SDMA_LOG("copy_sweep c=%u STRUCTURE: bad dwords by position inside each "
                 "256-byte block — first half (byte 0x00-0x7f) %u, second half "
                 "(0x80-0xff) %u; by 64-byte quarter %u %u %u %u. A clean split "
                 "names the granularity; an even spread refutes block structure",
                 c, half[0], half[1], quarter[0], quarter[1], quarter[2], quarter[3]);

        {
            char db[420];
            uint32_t dp = 0;
            db[0] = 0;
            for (uint32_t k = 0; k < nKeys && dp + 32u < sizeof(db); k++) {
                const int w = snprintf(db + dp, sizeof(db) - dp, "%s%d x%u",
                                       dp ? ", " : "", (int)dKey[k], dCnt[k]);
                if (w <= 0) break;
                dp += (uint32_t)w;
            }
            SDMA_LOG("copy_sweep c=%u DELTA (source dword index MINUS "
                     "destination dword index) x count, %u distinct: %s%s",
                     c, nKeys, db, dOther ? " (+more, table full)" : "");
        }

        // The permutation, when there is one: every sampled mismatch whose
        // value IS a pattern value, with the source dword it belongs to.
        for (uint32_t s = 0; s < nP; s += 4) {
            char pb[460];
            uint32_t pp = 0;
            pb[0] = 0;
            for (uint32_t k = s; k < nP && k < s + 4 && pp + 96u < sizeof(pb); k++) {
                uint32_t gc = 0, gi = 0;
                (void)sweep_decode(pGot[k], &gc, &gi);
                const int w = snprintf(pb + pp, sizeof(pb) - pp,
                                       "%sdst+%#x got %#010x <- case %u src+%#x "
                                       "(delta %d B)", pp ? " | " : "",
                                       pIdx[k] * 4u, pGot[k], gc, gi * 4u,
                                       ((int)gi - (int)pIdx[k]) * 4);
                if (w <= 0) break;
                pp += (uint32_t)w;
            }
            SDMA_LOG("copy_sweep c=%u PERMUTATION[%u of %u displaced samples]: %s",
                     c, s, nP, pb);
        }

        if (mapsLogged >= 6) continue;      // keep the log bounded
        mapsLogged++;
        for (uint32_t s = 0; s < nS; s += 4) {
            char sb[460];
            uint32_t sp = 0;
            sb[0] = 0;
            for (uint32_t k = s; k < nS && k < s + 4 && sp + 96u < sizeof(sb); k++) {
                uint32_t gc = 0, gi = 0;
                int w;
                if (!sweep_decode(sGot[k], &gc, &gi)) {
                    w = snprintf(sb + sp, sizeof(sb) - sp,
                                 "%sdst+%#x got %#010x <- NOT-A-PATTERN-VALUE",
                                 sp ? " | " : "", sIdx[k] * 4u, sGot[k]);
                } else if (gc != c) {
                    w = snprintf(sb + sp, sizeof(sb) - sp,
                                 "%sdst+%#x got %#010x <- CASE %u src+%#x",
                                 sp ? " | " : "", sIdx[k] * 4u, sGot[k], gc, gi * 4u);
                } else {
                    w = snprintf(sb + sp, sizeof(sb) - sp,
                                 "%sdst+%#x got %#010x <- src+%#x (delta %d dw, "
                                 "%d B)", sp ? " | " : "", sIdx[k] * 4u, sGot[k],
                                 gi * 4u, (int)gi - (int)sIdx[k],
                                 ((int)gi - (int)sIdx[k]) * 4);
                }
                if (w <= 0) break;
                sp += (uint32_t)w;
            }
            SDMA_LOG("copy_sweep c=%u MAP[%u of %u]: %s", c, s, nS, sb);
        }
    }

    SDMA_LOG("copy_sweep: END — %u of %u cases wrong. Freeing the region "
             "(vram_off %#llx, %u bytes); every later VRAM address is unchanged.",
             casesBad, kNCases, (unsigned long long)rOff, kRegionBytes);

    gmc.vram_alloc.free(rgn);
    return casesBad == 0 ? kIOReturnSuccess : kIOReturnIOError;
}

//------------------------------------------------------------------
// sdma_instance_present — deviation D6.
//
// SDMA registers all resolve through the GC IP base (sdma_v7_1.c), so a
// missing SDMA1 discovery entry does not by itself stop us driving
// instance 1. But if the second engine really is absent, its STATUS_REG
// reads back all-ones; skip it rather than programming registers that
// answer nothing.
//------------------------------------------------------------------
static bool
sdma_instance_present(const DeviceContext &dev, uint32_t i)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return false;
    if (i == 0) return true;
    const uint32_t status =
        RREG32(dev, sdma_reg_offset(dev, i, sdma_regs(dev).STATUS_REG));
    if (status == 0xFFFFFFFFu || status == 0u) {
        SDMA_LOG("instance %u: STATUS_REG reads %#010x — treating the engine "
                 "as absent (SDMA%u IP entry %s)",
                 i, status, i,
                 dev.ip.isResolved(i == 0 ? IPBlock::SDMA0 : IPBlock::SDMA1)
                     ? "resolved" : "unresolved");
        return false;
    }
    return true;
}

//------------------------------------------------------------------
// sdma_init_full — top-level SDMAInit stage entry.
//
// Caller must have done PSP bringup + SMU + GMC + IH + RLC + CP
// first; this stage just adds the two SDMA engines on top.
//
// Order:
//   1) Stop any running queues (in case of warm reset).
//   2) Allocate ring + WB for each instance.
//   3) PSP-load the SDMA microcode if the caller left the bytes in
//      sdma.ucode_bin (the reference did this from its LoadFirmware
//      selector before this stage ran). If microcode_loaded is still
//      false we skip gfx_resume and just log, so the caller can retry
//      after the firmware load.
//   4) gfx_resume_instance on each.
//   5) sdma_ring_test on each.
//------------------------------------------------------------------
kern_return_t
sdma_init_full(DeviceContext &dev,
               PSPContext &psp,
               GMCContext &gmc,
               SDMAContext &sdma)
{
    if (!dev.ip.isResolved(IPBlock::GC)) {
        SDMA_LOG("init_full: GC IP base not resolved");
        return kIOReturnNotReady;
    }
    const IPVersion gcv = dev.ip.getVersion(IPBlock::GC);
    const SDMARegOffsets &R = sdma_regs(dev);
    SDMA_LOG("init_full: starting SDMA bringup (instances=%u, "
             "microcode_loaded=%d)",
             kSDMAInstanceCount, (int)sdma.microcode_loaded);
    SDMA_LOG("init_full: GC v%u.%u.%u -> reg table %s (QUEUE0_RB_CNTL=%#06x, "
             "MCU_CNTL=%#06x); GC base[0]=%#x base[1]=%#x; SDMA0 IP %s, "
             "SDMA1 IP %s",
             gcv.major, gcv.minor, gcv.rev, R.name, R.QUEUE0_RB_CNTL,
             R.MCU_CNTL, dev.ip.get(IPBlock::GC),
             dev.ip.getBase(IPBlock::GC, 1),
             dev.ip.isResolved(IPBlock::SDMA0) ? "resolved" : "unresolved",
             dev.ip.isResolved(IPBlock::SDMA1) ? "resolved" : "unresolved");
    SDMA_LOG("init_full: doorbell slots sdma_engine[0]=%#x sdma_engine[1]=%#x "
             "(programmed as index<<1), BAR2 %llu KiB",
             dev.doorbell.index.sdma_engine[0],
             dev.doorbell.index.sdma_engine[1],
             (unsigned long long)(dev.bar2Size >> 10));

    // 1) Bare-metal start path — port of sdma_v7_0_start (sdma_v7_0.c:837).
    //    Upstream does NOT halt the MCU here in bare-metal mode; PSP
    //    autoload has already loaded SDMA microcode and started the MCU,
    //    so we'd be stomping a live engine. Just unhalt (idempotent if
    //    already running) to mirror upstream line 862:
    //        sdma_v7_0_enable(adev, true);   // unhalt the MEs
    //
    //    mac-amdgpu's v0.1.40 fix uncovered this: when sdma_reg_offset
    //    was using the wrong base for the hyp-dec range, the previous
    //    "defensive stop" silently failed (writing HALT=1 to a different
    //    register). After fixing the base, the halt actually landed and
    //    tore down the PSP-loaded MCU between unhalt → ring program →
    //    re-unhalt. Now we follow upstream's bare-metal path and never
    //    halt.
    //
    //    Clearing RB_ENABLE+IB_ENABLE on QUEUE0 IS still safe: the queue
    //    control bits are at GC[0] (regular range) and the MCU just sees
    //    "queue disabled until ring is programmed", same as a fresh boot.
    SDMA_LOG("init_full: step 1/5 — clear RB/IB enable, unhalt MCU "
             "(bare-metal path, PSP-autoloaded MCU stays live)");
    for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {
        sdma.instance_present[i] = sdma_instance_present(dev, i);
        if (!sdma.instance_present[i]) continue;
        sdma_gfx_stop_instance(dev, i);
        sdma_engine_halt(dev, i, /*halt=*/false);
        sdma_log_status(dev, i);
    }
    if (!sdma.instance_present[0]) {
        SDMA_LOG("init_full: SDMA0 does not answer — aborting");
        return kIOReturnNoDevice;
    }

    // 2) Allocate storage.
    SDMA_LOG("init_full: step 2/5 — allocate ring + WB per instance");
    for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {
        if (!sdma.instance_present[i]) continue;
        sdma.instance[i].instance = i;
        kern_return_t r = sdma_alloc_storage(dev, sdma.instance[i], gmc);
        if (r != kIOReturnSuccess) {
            SDMA_LOG("SDMA%u init_full: storage alloc failed: %#x", i, r);
            return r;
        }
    }

    // 3) Microcode. The reference loads this from its userspace
    //    LoadFirmware selector before the SDMAInit stage runs and only
    //    flips microcode_loaded; we keep that placement but can also do
    //    the load ourselves when the caller handed us the .bin bytes.
    if (!sdma.microcode_loaded && sdma.ucode_bin != nullptr &&
        sdma.ucode_size != 0) {
        SDMA_LOG("init_full: step 3/5 — PSP-load SDMA microcode "
                 "(%u bytes, GFX_FW_TYPE_SDMA_UCODE_TH0)", sdma.ucode_size);
        kern_return_t r = sdma_load_microcode(dev, psp, sdma,
                                              sdma.ucode_bin,
                                              sdma.ucode_size);
        if (r != kIOReturnSuccess) {
            SDMA_LOG("init_full: SDMA microcode load failed: %#x", r);
        }
    }

    if (!sdma.microcode_loaded) {
        SDMA_LOG("init_full: microcode_loaded=false — storage allocated, "
                 "deferring gfx_resume + ring_test until the SDMA ucode "
                 "(GFX_FW_TYPE_SDMA_UCODE_TH0) has been PSP-loaded");
        return kIOReturnSuccess;
    }

    // 4) gfx_resume each. Mirrors upstream sdma_v7_0_gfx_resume.
    SDMA_LOG("init_full: step 4/5 — gfx_resume each instance "
             "(program RB_BASE/CNTL/WPTR/RPTR, doorbell, watchdog, "
             "unhalt MCU, enable RB+IB)");
    for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {
        if (!sdma.instance_present[i]) continue;
        kern_return_t r = sdma_gfx_resume_instance(dev, sdma.instance[i]);
        if (r != kIOReturnSuccess) {
            SDMA_LOG("SDMA%u init_full: gfx_resume failed: %#x", i, r);
            sdma_log_status(dev, i);
            return r;
        }
        // Linux dyndbg pattern: "SDMA %d use_doorbell being set to: [yes]"
        SDMA_LOG("SDMA%u: use_doorbell=yes (slot %#x), engine unhalted",
                 i, sdma.instance[i].doorbell_index);
        sdma_log_status(dev, i);
    }

    // 5) Ring test on each. Failure is logged but doesn't kill the
    //    init — the caller can re-run it. Mirrors upstream
    //    sdma_v7_0_ring_test_ring (sdma_v7_0.c:934).
    SDMA_LOG("init_full: step 5/5 — sdma_ring_test on each instance "
             "(submit FENCE pkt, watch WB write)");
    for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {
        if (!sdma.instance_present[i]) continue;
        sdma_ring_test(dev, sdma.instance[i], /*timeout_us=*/100000);
    }

    SDMA_LOG("init_full: done — final per-instance status:");
    for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {
        if (!sdma.instance_present[i]) continue;
        sdma_log_status(dev, i);
    }
    return kIOReturnSuccess;
}

} // namespace amdgpu
