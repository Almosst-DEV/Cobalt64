//
//  ih_v7_0.cpp — Interrupt Handler v7_0 port (RDNA4), x86 kernel kext.
//
//  Origin: lemonade-sdk/mac-amdgpu (MIT) @ commit 3bdeed2,
//          dext/amdgpu/ih_v7_0.cpp (arm64 DriverKit dext).
//
//  Upstream sources behind the reference:
//      drivers/gpu/drm/amd/amdgpu/ih_v7_0.c
//      drivers/gpu/drm/amd/amdgpu/amdgpu_ih.c
//
//  MIT License. The reference is MIT-licensed; see ../../NOTICE.
//
//  Coverage:
//    - ih_init                amdgpu_ih_ring_init (allocations only)
//    - ih_enable_ring         ih_v7_0_enable_ring (base/cntl/wptr/doorbell regs)
//    - ih_toggle              RB_ENABLE only (reference's ih_v7_0_toggle_interrupts)
//    - ih_enable_interrupts   ih_v7_0_enable_interrupts
//    - ih_disable_interrupts  ih_v7_0_disable_interrupts
//    - ih_program_msi_storm   storm + flood + msi-storm config
//    - ih_get_wptr            ih_v7_0_get_wptr (incl. overflow recovery)
//    - ih_set_rptr            ih_v7_0_set_rptr (MMIO or doorbell)
//    - ih_drain / ih_process  amdgpu_ih_process ring walk + IV decode
//    - ih_poll                polled front end filling a caller array
//    - ih_v7_0_hw_init        ih_v7_0_hw_init -> ih_v7_0_irq_init
//    - ih_v7_0_hw_fini        ih_v7_0_hw_fini -> ih_v7_0_irq_disable
//    - ih_init_full           reference's orchestrator name (= ih_v7_0_hw_init)
//
// ===========================================================================
//  Deviations from reference (dext/amdgpu/ih_v7_0.cpp @ 3bdeed2)
// ===========================================================================
//  1. Memory. IOBufferMemoryDescriptor::Create + IODMACommand::PrepareForDMA
//     (two per buffer) become one amdgpu::sysmem_alloc each. No IOMMU on this
//     board, so SysMem::bus == physical and that is what IH_RB_BASE gets.
//     Alignment is 4096 (x86 page) instead of the reference's kASPageSize
//     (16 KB Apple Silicon page). No DriverKit includes, no os_log, no
//     %{public}s; IH_LOG comes from amdgpu_log.h (IOLog).
//
//  2. NO CPU-SIDE INTERRUPT DELIVERY. The reference runs the ring under
//     MSI-X and sets IH_RB_CNTL.RPTR_REARM unconditionally ("we use MSI").
//     Navi48Bringup has no IOInterruptEventSource / MSI setup, so:
//        - RPTR_REARM is programmed as !!ih.msi (i.e. ih.enable_cpu_intr),
//          which is exactly upstream's `!!adev->irq.msi_enabled`, and
//          defaults to 0 here;
//        - IH_RB_CNTL.ENABLE_INTR is only set when ih.enable_cpu_intr,
//          which defaults to false. ih_v7_0_hw_init therefore brings the
//          ring up with RB_ENABLE=1 / ENABLE_INTR=0 — the IH fills the ring
//          but never signals the host. This module never touches the PCI
//          command register, MSI/MSI-X capability, or INTX_DISABLE; it
//          cannot, by itself, start an interrupt storm on the host.
//     The host consumes the ring by calling ih_poll()/ih_process().
//
//  3. get_wptr/set_rptr split out. The reference inlined the wptr read and
//     the rptr write into ih_drain. Split into ih_get_wptr/ih_set_rptr per
//     the task and upstream's vfunc pair — same registers, same order. The
//     overflow path additionally adopts upstream's early-out: if the shadow
//     said OVERFLOW but the IH_RB_WPTR re-read does not, we treat the shadow
//     as stale and skip the recovery instead of blindly advancing rptr.
//
//  4. Doorbell rptr path. The reference deferred it ("writes IH_RB_RPTR via
//     MMIO; slower but simpler"). The path is implemented here and
//     IH_DOORBELL_RPTR is programmed per upstream ih_v7_0_doorbell_rptr, but
//     IHContext::use_doorbell defaults to FALSE: BAR2 is mapped on demand and
//     dev.bar2 may be null, and the NBIO doorbell aperture / ih_doorbell_range
//     programming is not ported (see 6). With use_doorbell=false the register
//     is written with ENABLE=0, which is what upstream does in that case.
//
//  5. IH_RB_WPTR_ADDR_HI mask. Reference wrote `(bus >> 32) & 0xFF` (caps the
//     writeback address at 40 bits). Upstream writes
//     `upper_32_bits(wptr_addr) & 0xFFFF` and the register field really is 16
//     bits. Followed upstream — on an x86 host with > 1 TiB of RAM the
//     reference's mask would silently truncate the writeback address and the
//     GPU would DMA the wptr into someone else's page. ih_init also logs
//     loudly if either buffer lands above 48 bits.
//
//  6. Not ported (also absent from the reference), because they live in other
//     IP blocks that this module does not own:
//        - nbio_v7_11_ih_control (BIF_BX0_INTERRUPT_CNTL / _CNTL2 dummy-read
//          + IH_REQ_NONSNOOP_EN). We set IH_RB_CNTL.MC_SNOOP=1 as upstream
//          does, which is the half that matters for a cached sysmem ring.
//        - nbio ih_doorbell_range (needed before use_doorbell can work).
//        - IH_CHICKEN.MC_SPACE_GPA_ENABLE — upstream only writes it for
//          AMDGPU_FW_LOAD_DIRECT / RLC_BACKDOOR_AUTO. PSP is alive here, so
//          the load type is PSP and upstream skips it too.
//        - pci_set_master(). Bus mastering must already be on or the IH
//          cannot DMA into the ring; that is the PCI nub's job, not ours.
//
//  7. The ring is read through a `volatile` pointer and fenced with
//     sysmem_rmb(). The reference read it with a plain pointer because it
//     only ran inside an interrupt handler; we spin on it from the CPU, so
//     the compiler must not hoist the loads.
//
//  8. IHContext members have default initializers (the dext relied on
//     IIG's IONewZero to zero the parent). Added config flags
//     (use_bus_addr/use_doorbell/doorbell_index/enable_cpu_intr/
//     wptr_from_mmio) and a log budget; the reference's field names that
//     other modules use are unchanged except ring_buf/ring_dma/
//     wptr_shadow_buf/wptr_shadow_dma -> ring_mem/wptr_mem (SysMem).
//
//  9. ih_drain's walk is bounded at one full ring lap. Upstream caps each
//     pass at AMDGPU_IH_MAX_NUM_IVS (32) and restarts; the reference dropped
//     the bound. A full lap keeps the reference's drain-everything behaviour
//     while making a bad wptr unable to spin forever in the kernel — which
//     matters much more here, since we walk the ring from a polling loop
//     rather than from an interrupt handler.
// ===========================================================================
//

#include "amdgpu_ih.h"
#include "amdgpu_log.h"

namespace amdgpu {

// Compute log2 of ring size in dwords / 4 for IH_RB_CNTL.RB_SIZE.
// Upstream amdgpu_ih_ring_init expects ring size in bytes; the
// register field expects log2(ring_size_dwords).
static inline uint32_t log2u32(uint32_t v) {
    uint32_t r = 0;
    while (v >>= 1) r++;
    return r;
}

// BAR2 doorbell write. amdgpu_regs.h has no WDOORBELL32 yet (BAR2 is mapped
// on demand by the kext and dev.bar2 is null until then), so keep a guarded
// local one rather than reaching into another module's header.
static inline bool ih_wdoorbell32(const DeviceContext &dev, uint32_t index,
                                  uint32_t value) {
    const uint64_t off = (uint64_t)index * 4;
    if (dev.bar2 == nullptr || off + 4 > dev.bar2Size) return false;
    *reinterpret_cast<volatile uint32_t *>(dev.bar2 + off) = value;
    storeFence();
    return true;
}

// ----- ih_init: allocate ring + wptr writeback -----
//
// Mirrors amdgpu_ih_ring_init(adev, ih, 256 KB, use_bus_addr=true).
// Two physically contiguous sysmem buffers:
//   1) ring buffer  — 256 KB, page-aligned
//   2) wptr shadow  — one page (the GPU writes the current wptr into
//                     dword 0; dword 1 is our rptr shadow for the
//                     doorbell path, the rest is unused)
kern_return_t
ih_init(DeviceContext &dev, IHContext &ih)
{
    if (ih.inited) return kIOReturnSuccess;

    const uint32_t ring_size = kIHRingDefaultBytes;

    kern_return_t r = sysmem_alloc(ih.ring_mem, ring_size, kIHRingAlignBytes);
    if (r != kIOReturnSuccess || !ih.ring_mem.valid()) {
        IH_LOG("ring alloc failed: %#x", r);
        sysmem_free(ih.ring_mem);
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }

    r = sysmem_alloc(ih.wptr_mem, kIHWptrBufBytes, kIHWptrBufBytes);
    if (r != kIOReturnSuccess || !ih.wptr_mem.valid()) {
        IH_LOG("wptr writeback alloc failed: %#x", r);
        sysmem_free(ih.wptr_mem);
        sysmem_free(ih.ring_mem);
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }

    // IH_RB_BASE carries bits [39:8] and IH_RB_BASE_HI bits [47:40];
    // IH_RB_WPTR_ADDR_{LO,HI} cover [31:0] / [47:32]. Anything above
    // 48 bits cannot be expressed — fail loud instead of DMAing to a
    // truncated address. sysmem_alloc already masks to 48 bits, so
    // this is a belt-and-braces check.
    if ((ih.ring_mem.bus >> 48) != 0 || (ih.wptr_mem.bus >> 48) != 0) {
        IH_LOG("bus address above 48 bits: ring=%#llx wptr=%#llx",
               (unsigned long long)ih.ring_mem.bus,
               (unsigned long long)ih.wptr_mem.bus);
        sysmem_free(ih.wptr_mem);
        sysmem_free(ih.ring_mem);
        return kIOReturnNotAligned;
    }

    ih.ring_bus         = ih.ring_mem.bus;
    ih.ring_cpu         = ih.ring_mem.cpu;
    ih.ring_size_bytes  = ring_size;
    ih.ring_size_dwords = ring_size / 4;
    // ptr_mask is byte-granular per upstream amdgpu_ih_ring_init
    // (amdgpu_ih.c:52). rptr/wptr in this driver are both byte
    // offsets into the ring.
    ih.ptr_mask         = ring_size - 1;
    ih.wptr_shadow_bus  = ih.wptr_mem.bus;
    ih.wptr_shadow_cpu  =
        reinterpret_cast<volatile uint32_t *>(ih.wptr_mem.cpu);
    ih.rptr_shadow_cpu  = ih.wptr_shadow_cpu + 1;
    ih.rptr             = 0;
    ih.entries_processed = 0;
    ih.overflows_seen   = 0;
    ih.log_budget       = kIHEntryLogBudget;
    ih.use_bus_addr     = true;
    // Keep the doorbell index in sync with the doorbell module even
    // though use_doorbell defaults to false (deviation 4).
    ih.doorbell_index   = dev.doorbell.index.ih;

    // sysmem_alloc zero-fills both buffers; make that visible to the
    // device before it can be told about them.
    sysmem_wmb();

    ih.inited = true;
    IH_LOG("ring alloc ok: ring bus=%#llx cpu=%p (%u B), wptr_shadow bus=%#llx",
           (unsigned long long)ih.ring_bus, ih.ring_cpu, ring_size,
           (unsigned long long)ih.wptr_shadow_bus);
    return kIOReturnSuccess;
}

// ----- ih_release: teardown -----
void
ih_release(IHContext &ih)
{
    sysmem_free(ih.wptr_mem);
    sysmem_free(ih.ring_mem);
    ih.ring_bus = 0; ih.ring_cpu = nullptr;
    ih.wptr_shadow_bus = 0; ih.wptr_shadow_cpu = nullptr;
    ih.rptr_shadow_cpu = nullptr;
    ih.ring_size_bytes = 0; ih.ring_size_dwords = 0; ih.ptr_mask = 0;
    ih.rptr = 0;
    ih.inited = false; ih.enabled = false;
}

// ----- ih_toggle: flip RB_ENABLE in IH_RB_CNTL -----
kern_return_t
ih_toggle(const DeviceContext &dev, IHContext &ih, bool enable)
{
    (void)ih;
    if (!dev.ip.isResolved(IPBlock::OSSSYS)) return kIOReturnNotReady;
    const uint32_t reg = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                          IHRegs::IH_RB_CNTL);
    uint32_t v = RREG32(dev, reg);
    if (enable) v |=  (1u << kIH_RB_CNTL__RB_ENABLE__SHIFT);
    else        v &= ~(1u << kIH_RB_CNTL__RB_ENABLE__SHIFT);
    WREG32(dev, reg, v);
    return kIOReturnSuccess;
}

// ----- ih_enable_interrupts: ih_v7_0_enable_interrupts (ih_v7_0.c:39-63) -----
//
// Upstream sets RB_ENABLE and ENABLE_INTR together. ENABLE_INTR is what makes
// the IH raise the interrupt line at the host; with no handler installed a
// level-triggered assertion never gets acked. So ENABLE_INTR follows
// ih.enable_cpu_intr, which defaults to false (deviation 2). With the default
// this writes exactly what the reference's ih_toggle(true) wrote.
kern_return_t
ih_enable_interrupts(const DeviceContext &dev, IHContext &ih)
{
    if (!dev.ip.isResolved(IPBlock::OSSSYS)) return kIOReturnNotReady;
    const uint32_t reg = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                          IHRegs::IH_RB_CNTL);
    uint32_t cntl = RREG32(dev, reg);
    cntl |= (1u << kIH_RB_CNTL__RB_ENABLE__SHIFT);
    if (ih.enable_cpu_intr) cntl |=  (1u << kIH_RB_CNTL__ENABLE_INTR__SHIFT);
    else                    cntl &= ~(1u << kIH_RB_CNTL__ENABLE_INTR__SHIFT);
    WREG32(dev, reg, cntl);
    ih.enabled = true;
    IH_LOG("interrupts enabled: IH_RB_CNTL=%#010x (ENABLE_INTR=%u, polled mode=%s)",
           RREG32(dev, reg), ih.enable_cpu_intr ? 1u : 0u,
           ih.enable_cpu_intr ? "no" : "yes");
    return kIOReturnSuccess;
}

// ----- ih_disable_interrupts: ih_v7_0_disable_interrupts (ih_v7_0.c:70-93) -----
kern_return_t
ih_disable_interrupts(const DeviceContext &dev, IHContext &ih)
{
    if (!dev.ip.isResolved(IPBlock::OSSSYS)) return kIOReturnNotReady;
    const uint32_t cntl_reg = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                               IHRegs::IH_RB_CNTL);
    uint32_t cntl = RREG32(dev, cntl_reg);
    cntl &= ~(1u << kIH_RB_CNTL__RB_ENABLE__SHIFT);
    cntl &= ~(1u << kIH_RB_CNTL__ENABLE_INTR__SHIFT);
    WREG32(dev, cntl_reg, cntl);

    // set rptr, wptr to 0
    WREG32(dev, SOC15_REG_OFFSET(dev, IPBlock::OSSSYS, IHRegs::IH_RB_RPTR), 0);
    WREG32(dev, SOC15_REG_OFFSET(dev, IPBlock::OSSSYS, IHRegs::IH_RB_WPTR), 0);
    ih.enabled = false;
    ih.rptr    = 0;
    return kIOReturnSuccess;
}

// ----- ih_rb_cntl: ih_v7_0_rb_cntl (ih_v7_0.c:187-208) -----
//
// Field shifts taken from osssys_7_0_0_sh_mask.h.
//   RB_SIZE               = log2(ring_dwords)        bits[5:1]
//   MC_SPACE              = 2 (bus_addr) / 4 (GPUVA) bits[30:28]
//   WPTR_OVERFLOW_CLEAR   = 1                        bit[31] (self-clearing)
//   WPTR_OVERFLOW_ENABLE  = 1                        bit[16]
//   WPTR_WRITEBACK_ENABLE = 1                        bit[8]
//   MC_SNOOP              = 1                        bit[20]
//   MC_RO / MC_VMID       = 0
static uint32_t
ih_rb_cntl(const IHContext &ih, uint32_t cntl)
{
    const uint32_t rb_bufsz = log2u32(ih.ring_size_dwords);
    const uint32_t mc_space = ih.use_bus_addr ? kIH_RB_CNTL_MC_SPACE_BUS_ADDR
                                              : kIH_RB_CNTL_MC_SPACE_GPUVA;

    cntl &= ~(kIH_RB_CNTL__MC_SPACE__MASK << kIH_RB_CNTL__MC_SPACE__SHIFT);
    cntl |=  ((mc_space & kIH_RB_CNTL__MC_SPACE__MASK)
              << kIH_RB_CNTL__MC_SPACE__SHIFT);
    cntl |=  (1u << kIH_RB_CNTL__WPTR_OVERFLOW_CLEAR__SHIFT);
    cntl |=  (1u << kIH_RB_CNTL__WPTR_OVERFLOW_ENABLE__SHIFT);
    cntl &= ~(kIH_RB_CNTL__RB_SIZE__MASK << kIH_RB_CNTL__RB_SIZE__SHIFT);
    cntl |=  ((rb_bufsz & kIH_RB_CNTL__RB_SIZE__MASK)
              << kIH_RB_CNTL__RB_SIZE__SHIFT);
    // Ring Buffer write pointer writeback: the IH copies IH_RB_WPTR into
    // memory at IH_RB_WPTR_ADDR_{LO,HI}. That is what the polled host reads.
    cntl |=  (1u << kIH_RB_CNTL__WPTR_WRITEBACK_ENABLE__SHIFT);
    cntl |=  (1u << kIH_RB_CNTL__MC_SNOOP__SHIFT);
    cntl &= ~(1u << kIH_RB_CNTL__MC_RO__SHIFT);
    cntl &= ~(0xFu << kIH_RB_CNTL__MC_VMID__SHIFT);
    return cntl;
}

// ----- ih_doorbell_rptr: ih_v7_0_doorbell_rptr (ih_v7_0.c:215-232) -----
static uint32_t
ih_doorbell_rptr(const IHContext &ih)
{
    uint32_t v = 0;
    if (ih.use_doorbell) {
        v |= ((ih.doorbell_index & kIH_DOORBELL_RPTR__OFFSET__MASK)
              << kIH_DOORBELL_RPTR__OFFSET__SHIFT);
        v |= (1u << kIH_DOORBELL_RPTR__ENABLE__SHIFT);
    } else {
        v &= ~(1u << kIH_DOORBELL_RPTR__ENABLE__SHIFT);
    }
    return v;
}

// ----- ih_enable_ring: program IH_RB_BASE/CNTL/WPTR_ADDR/DOORBELL_RPTR -----
//
// Mirrors ih_v7_0_enable_ring (ih_v7_0.c:239-272):
//   1. Write the ring bus address (IH_RB_BASE = bus>>8, BASE_HI = bus>>40)
//   2. IH_RB_CNTL from ih_rb_cntl() + RPTR_REARM = !!msi_enabled
//   3. Write wptr writeback address (lo/hi)
//   4. Reset wptr then rptr to 0
//   5. IH_DOORBELL_RPTR
//
// We don't touch RB_ENABLE here; ih_enable_interrupts() does that last.
kern_return_t
ih_enable_ring(const DeviceContext &dev, IHContext &ih)
{
    if (!ih.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::OSSSYS)) return kIOReturnNotReady;

    const uint32_t base_reg   = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                                 IHRegs::IH_RB_BASE);
    const uint32_t basehi_reg = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                                 IHRegs::IH_RB_BASE_HI);
    const uint32_t cntl_reg   = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                                 IHRegs::IH_RB_CNTL);
    const uint32_t rptr_reg   = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                                 IHRegs::IH_RB_RPTR);
    const uint32_t wptr_reg   = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                                 IHRegs::IH_RB_WPTR);
    const uint32_t wptr_lo    = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                                 IHRegs::IH_RB_WPTR_ADDR_LO);
    const uint32_t wptr_hi    = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                                 IHRegs::IH_RB_WPTR_ADDR_HI);
    const uint32_t db_reg     = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                                 IHRegs::IH_DOORBELL_RPTR);

    // The IH_RB_BASE register expects bits [39:8] of the ring's bus
    // address (256-byte granularity per upstream code). Higher bits
    // go into IH_RB_BASE_HI.
    const uint32_t base_lo_v = (uint32_t)((ih.ring_bus >> 8) & 0xFFFFFFFFu);
    const uint32_t base_hi_v = (uint32_t)((ih.ring_bus >> 40) & 0xFFu);
    WREG32(dev, base_reg,   base_lo_v);
    WREG32(dev, basehi_reg, base_hi_v);

    // RB_CNTL: the reference builds this from zero rather than RMWing the
    // live register, which also guarantees RB_ENABLE / ENABLE_INTR start
    // clear. Keep that.
    uint32_t cntl = ih_rb_cntl(ih, 0);
    // RPTR_REARM re-arms interrupt delivery when the host writes rptr.
    // Upstream: REG_SET_FIELD(tmp, IH_RB_CNTL, RPTR_REARM, !!msi_enabled).
    // We have no MSI, so this stays 0 (deviation 2).
    if (ih.enable_cpu_intr) cntl |= (1u << kIH_RB_CNTL__RPTR_REARM__SHIFT);
    WREG32(dev, cntl_reg, cntl);

    // wptr writeback address. LO is the full low 32 bits (the reference
    // masked off the low 2 bits; the buffer is page aligned so it is the
    // same value). HI is a 16-bit field — see deviation 5.
    const uint32_t waddr_lo_v = (uint32_t)(ih.wptr_shadow_bus & 0xFFFFFFFCu);
    const uint32_t waddr_hi_v = (uint32_t)((ih.wptr_shadow_bus >> 32) & 0xFFFFu);
    WREG32(dev, wptr_lo, waddr_lo_v);
    WREG32(dev, wptr_hi, waddr_hi_v);

    // set rptr, wptr to 0
    WREG32(dev, wptr_reg, 0);
    WREG32(dev, rptr_reg, 0);
    ih.rptr = 0;
    if (ih.wptr_shadow_cpu != nullptr) *ih.wptr_shadow_cpu = 0;
    if (ih.rptr_shadow_cpu != nullptr) *ih.rptr_shadow_cpu = 0;
    sysmem_wmb();

    const uint32_t db_v = ih_doorbell_rptr(ih);
    WREG32(dev, db_reg, db_v);

    IH_LOG("ring programmed: BASE=%#010x BASE_HI=%#010x (bus=%#llx, %u B / %u dwords)",
           base_lo_v, base_hi_v, (unsigned long long)ih.ring_bus,
           ih.ring_size_bytes, ih.ring_size_dwords);
    IH_LOG("  IH_RB_CNTL=%#010x (RB_SIZE=%u MC_SPACE=%u WB=1 SNOOP=1 OVF=1 REARM=%u)",
           cntl, log2u32(ih.ring_size_dwords),
           ih.use_bus_addr ? kIH_RB_CNTL_MC_SPACE_BUS_ADDR
                           : kIH_RB_CNTL_MC_SPACE_GPUVA,
           ih.enable_cpu_intr ? 1u : 0u);
    IH_LOG("  WPTR_ADDR_LO=%#010x HI=%#010x (bus=%#llx)",
           waddr_lo_v, waddr_hi_v, (unsigned long long)ih.wptr_shadow_bus);
    IH_LOG("  IH_DOORBELL_RPTR=%#010x (use_doorbell=%u index=%u bar2=%s)",
           db_v, ih.use_doorbell ? 1u : 0u, ih.doorbell_index,
           dev.bar2 != nullptr ? "mapped" : "null");
    IH_LOG("  readback: CNTL=%#010x BASE=%#010x RPTR=%#010x WPTR=%#010x",
           RREG32(dev, cntl_reg), RREG32(dev, base_reg),
           RREG32(dev, rptr_reg), RREG32(dev, wptr_reg));
    return kIOReturnSuccess;
}

// ----- ih_program_msi_storm: storm/flood control -----
//
// Mirrors ih_v7_0.c:353-370 — read-modify-write each register and
// set just one field per the upstream sequence. Field shifts come
// from osssys_7_0_0_sh_mask.h. These throttle how often the IH may
// raise an interrupt; harmless (and still correct) in polled mode.
kern_return_t
ih_program_msi_storm(const DeviceContext &dev, const IHContext &ih)
{
    (void)ih;
    if (!dev.ip.isResolved(IPBlock::OSSSYS)) return kIOReturnNotReady;

    // IH_STORM_CLIENT_LIST_CNTL: set CLIENT18_IS_STORM_CLIENT = 1
    // (ih_v7_0.c:353-356).
    const uint32_t storm_reg =
        SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                         IHRegs::IH_STORM_CLIENT_LIST_CNTL);
    uint32_t storm = RREG32(dev, storm_reg);
    storm |= (1u << kIH_STORM_CLIENT_LIST_CNTL__CLIENT18_IS_STORM_CLIENT__SHIFT);
    WREG32(dev, storm_reg, storm);

    // IH_INT_FLOOD_CNTL: FLOOD_CNTL_ENABLE = 1 (ih_v7_0.c:358-360).
    const uint32_t flood_reg =
        SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                         IHRegs::IH_INT_FLOOD_CNTL);
    uint32_t flood = RREG32(dev, flood_reg);
    flood |= (1u << kIH_INT_FLOOD_CNTL__FLOOD_CNTL_ENABLE__SHIFT);
    WREG32(dev, flood_reg, flood);

    // IH_MSI_STORM_CTRL: DELAY = 3 (ih_v7_0.c:367-370).
    const uint32_t msi_reg =
        SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                         IHRegs::IH_MSI_STORM_CTRL);
    uint32_t msi = RREG32(dev, msi_reg);
    // DELAY is a 2-bit field at [1:0] per sh_mask:
    //   #define IH_MSI_STORM_CTRL__DELAY_MASK 0x3
    msi = (msi & ~(kIH_MSI_STORM_CTRL__DELAY__MASK
                   << kIH_MSI_STORM_CTRL__DELAY__SHIFT))
        | (3u << kIH_MSI_STORM_CTRL__DELAY__SHIFT);
    WREG32(dev, msi_reg, msi);

    IH_LOG("storm/flood: STORM_CLIENT_LIST_CNTL=%#010x INT_FLOOD_CNTL=%#010x MSI_STORM_CTRL=%#010x",
           storm, flood, msi);
    return kIOReturnSuccess;
}

// ----- ih_get_wptr: ih_v7_0_get_wptr (ih_v7_0.c:455-492) -----
//
// Returns the current write pointer as a masked byte offset. On overflow it
// performs upstream's recovery: re-read from MMIO, drop the oldest entries by
// advancing rptr to wptr+32, and pulse IH_RB_CNTL.WPTR_OVERFLOW_CLEAR so a
// new overflow can latch.
uint32_t
ih_get_wptr(const DeviceContext &dev, IHContext &ih)
{
    if (!dev.ip.isResolved(IPBlock::OSSSYS)) return ih.rptr;
    const uint32_t wptr_reg = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                               IHRegs::IH_RB_WPTR);

    // Prefer the writeback shadow (avoids a slow BAR read per poll).
    // wptr_from_mmio is the bring-up escape hatch if writeback looks dead.
    uint32_t wptr;
    if (ih.wptr_from_mmio || ih.wptr_shadow_cpu == nullptr) {
        wptr = RREG32(dev, wptr_reg);
    } else {
        sysmem_rmb();
        wptr = __atomic_load_n(ih.wptr_shadow_cpu, __ATOMIC_ACQUIRE);
    }

    // Overflow flag lives in IH_RB_WPTR.RB_OVERFLOW (bit 0) per
    // osssys_7_0_0_sh_mask.h:206 and ih_v7_0_get_wptr (ih_v7_0.c:462).
    // NOT bit 31 of IH_RB_WPTR or IH_RB_CNTL.
    if (!(wptr & (1u << kIH_RB_WPTR__RB_OVERFLOW__SHIFT)))
        return wptr & ih.ptr_mask;

    // Re-read from MMIO to make sure the overflow really latched (the
    // shadow can race with the engine on the same cache line) — mirrors
    // ih_v7_0_get_wptr's second RREG32_NO_KIQ + its early-out.
    wptr = RREG32(dev, wptr_reg);
    if (!(wptr & (1u << kIH_RB_WPTR__RB_OVERFLOW__SHIFT)))
        return wptr & ih.ptr_mask;

    wptr &= ~(1u << kIH_RB_WPTR__RB_OVERFLOW__SHIFT);

    // When a ring buffer overflow happens, start parsing interrupts from the
    // last not-overwritten vector (wptr + 32) so we can catch up.
    const uint32_t tmp = (wptr + 32) & ih.ptr_mask;
    ih.overflows_seen++;
    IH_LOG("ring buffer overflow (wptr=%#010x rptr=%#010x -> %#010x, count=%llu)",
           wptr, ih.rptr, tmp, (unsigned long long)ih.overflows_seen);
    ih.rptr = tmp;

    const uint32_t cntl_reg = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                               IHRegs::IH_RB_CNTL);
    uint32_t cntl = RREG32(dev, cntl_reg);
    cntl |= (1u << kIH_RB_CNTL__WPTR_OVERFLOW_CLEAR__SHIFT);
    WREG32(dev, cntl_reg, cntl);
    // Unset CLEAR_OVERFLOW immediately so new overflows can be detected.
    cntl &= ~(1u << kIH_RB_CNTL__WPTR_OVERFLOW_CLEAR__SHIFT);
    WREG32(dev, cntl_reg, cntl);

    return wptr & ih.ptr_mask;
}

// ----- ih_set_rptr: ih_v7_0_set_rptr (ih_v7_0.c:499-513) -----
void
ih_set_rptr(const DeviceContext &dev, IHContext &ih)
{
    if (!dev.ip.isResolved(IPBlock::OSSSYS)) return;

    if (ih.use_doorbell) {
        if (ih.rptr_shadow_cpu != nullptr) {
            *ih.rptr_shadow_cpu = ih.rptr;
            sysmem_wmb();
        }
        if (ih_wdoorbell32(dev, ih.doorbell_index, ih.rptr)) return;
        // BAR2 not mapped (deviation 4) — fall through to MMIO so the
        // ring still drains instead of wedging at a stale rptr.
        IH_LOG("doorbell rptr write skipped (BAR2 null) - using IH_RB_RPTR");
    }
    WREG32(dev, SOC15_REG_OFFSET(dev, IPBlock::OSSSYS, IHRegs::IH_RB_RPTR),
           ih.rptr);
}

// ----- IV decode: amdgpu_ih_decode_iv_helper (amdgpu_ih.c:410-434) -----
static void
ih_decode_iv(const volatile uint32_t *e, IHEntry &d)
{
    const uint32_t dw0 = e[0], dw1 = e[1], dw2 = e[2], dw3 = e[3];
    d.client_id   =  (uint8_t)( dw0        & 0xFF);
    d.src_id      =  (uint8_t)((dw0 >> 8)  & 0xFF);
    d.ring_id     = (uint16_t)((dw0 >> 16) & 0xFF);
    d.vmid        =  (uint8_t)((dw0 >> 24) & 0xF);
    d.vmid_src    =  (uint8_t)((dw0 >> 31) & 0x1);
    d.timestamp   = ((uint64_t)(dw2 & 0xFFFF) << 32) | dw1;
    d.pasid       = (uint16_t)( dw3        & 0xFFFF);
    d.node_id     =  (uint8_t)((dw3 >> 16) & 0xFF);
    d.src_data[0] = e[4];
    d.src_data[1] = e[5];
    d.src_data[2] = e[6];
    d.src_data[3] = e[7];
}

// ----- ih_drain: walk the ring and dispatch entries -----
//
// Mirrors amdgpu_ih_process(). Reads the current wptr via ih_get_wptr, then
// for each 8-dword entry between (rptr, wptr) decodes and dispatches, and
// finally publishes the new rptr with ih_set_rptr.
//
// Returns the number of entries processed.
uint32_t
ih_drain(const DeviceContext &dev, IHContext &ih,
         IHDispatchFn dispatch, void *user)
{
    if (!ih.inited || !ih.enabled) return 0;
    if (ih.ring_cpu == nullptr) return 0;
    if (!dev.ip.isResolved(IPBlock::OSSSYS)) return 0;

    const uint32_t wptr = ih_get_wptr(dev, ih);
    if (ih.rptr == wptr) return 0;

    // Make every ring dword the engine wrote before this wptr visible.
    sysmem_rmb();

    // Bound the walk. Upstream caps each pass at AMDGPU_IH_MAX_NUM_IVS (32)
    // and restarts; the reference dropped the bound entirely. A full lap of
    // the ring is the most a legitimate drain can ever need, so cap there:
    // the reference's "drain everything in one call" behaviour is preserved,
    // but a garbage wptr (e.g. a wptr that is not 32-byte aligned, which
    // ih.rptr would step straight past) can no longer spin forever in the
    // kernel. Hitting the cap is a hardware/state bug — log it.
    const uint32_t max_iters = ih.ring_size_bytes / kIHEntryBytes;
    uint32_t count = 0;
    auto *ring = static_cast<const volatile uint32_t *>(ih.ring_cpu);
    while (ih.rptr != wptr && count < max_iters) {
        // rptr / wptr are byte offsets; the ring base is dword-typed
        // so divide by 4 to index. Each IH entry is 8 dwords = 32 B
        // (amdgpu_ih.c:295: `ih->rptr += 32`).
        const volatile uint32_t *e = &ring[ih.rptr >> 2];
        IHEntry decoded{};
        ih_decode_iv(e, decoded);

        if (ih.log_budget > 0) {
            ih.log_budget--;
            IH_LOG("iv[%llu] @%#06x client=%#04x src=%#04x ring=%u vmid=%u "
                   "pasid=%#06x ts=%#llx data=%08x %08x %08x %08x",
                   (unsigned long long)ih.entries_processed + count, ih.rptr,
                   decoded.client_id, decoded.src_id, decoded.ring_id,
                   decoded.vmid, decoded.pasid,
                   (unsigned long long)decoded.timestamp,
                   decoded.src_data[0], decoded.src_data[1],
                   decoded.src_data[2], decoded.src_data[3]);
        }

        if (dispatch != nullptr) dispatch(decoded, user);

        ih.rptr = (ih.rptr + kIHEntryBytes) & ih.ptr_mask;
        count++;
    }

    if (count >= max_iters && ih.rptr != wptr) {
        IH_LOG("drain bailed after a full ring lap: rptr=%#010x wptr=%#010x "
               "(misaligned wptr or wedged IH?)", ih.rptr, wptr);
    }

    if (count > 0) {
        // Advance hardware rptr — register also takes bytes.
        ih_set_rptr(dev, ih);
        ih.entries_processed += count;
    }
    return count;
}

// Upstream's name for the same walk.
uint32_t
ih_process(const DeviceContext &dev, IHContext &ih,
           IHDispatchFn dispatch, void *user)
{
    return ih_drain(dev, ih, dispatch, user);
}

// ----- ih_poll: polled front end, fills a caller array -----
namespace {
struct IHCollector {
    IHEntry *out;
    uint32_t max;
    uint32_t count;      // entries written to out
    uint32_t dropped;    // consumed but not stored
};
}  // namespace

static void
ih_collect(const IHEntry &entry, void *user)
{
    auto *c = static_cast<IHCollector *>(user);
    if (c == nullptr) return;
    if (c->out != nullptr && c->count < c->max) c->out[c->count++] = entry;
    else                                        c->dropped++;
}

uint32_t
ih_poll(const DeviceContext &dev, IHContext &ih,
        IHEntry *out, uint32_t max_entries, uint32_t timeout_ms)
{
    if (!ih.inited || !ih.enabled) {
        IH_LOG("ih_poll: ring not online (inited=%u enabled=%u)",
               ih.inited ? 1u : 0u, ih.enabled ? 1u : 0u);
        return 0;
    }

    IHCollector c{ out, (out != nullptr) ? max_entries : 0u, 0, 0 };
    uint32_t total = 0;
    uint32_t elapsed_ms = 0;
    for (;;) {
        total = ih_process(dev, ih, &ih_collect, &c);
        if (total > 0) break;
        if (elapsed_ms >= timeout_ms) break;
        IOSleep(1);
        elapsed_ms++;
    }
    if (c.dropped > 0)
        IH_LOG("ih_poll: %u of %u entries dropped (caller array holds %u)",
               c.dropped, total, max_entries);
    return (out != nullptr) ? c.count : total;
}

// ----- ih_v7_0_hw_init: ih_v7_0_hw_init -> ih_v7_0_irq_init (ih_v7_0.c:283-400) -----
//
// Sequence (upstream order, minus the pieces listed in deviation 6):
//   1. ih_v7_0_disable_interrupts  — always configure with the ring off
//   2. [nbio ih_control]           — not ported
//   3. [IH_CHICKEN MC_SPACE_GPA]   — PSP load type, upstream skips too
//   4. ih_v7_0_enable_ring         — BASE / CNTL / WPTR_ADDR / DOORBELL_RPTR
//   5. [nbio ih_doorbell_range]    — not ported
//   6. storm + flood + msi storm
//   7. [pci_set_master]            — PCI nub's job
//   8. ih_v7_0_enable_interrupts   — RB_ENABLE=1, ENABLE_INTR per config
kern_return_t
ih_v7_0_hw_init(DeviceContext &dev, IHContext &ih)
{
    if (!dev.ip.isResolved(IPBlock::OSSSYS)) {
        IH_LOG("OSSSYS IP base not resolved");
        return kIOReturnNotReady;
    }
    IH_LOG("hw_init: OSSSYS base=%#x (IH 7.0.0)", dev.ip.get(IPBlock::OSSSYS));

    kern_return_t r;
    r = ih_init(dev, ih);
    if (r != kIOReturnSuccess) return r;
    r = ih_disable_interrupts(dev, ih);   // safety: ring off before config
    if (r != kIOReturnSuccess) return r;
    r = ih_enable_ring(dev, ih);
    if (r != kIOReturnSuccess) return r;
    r = ih_program_msi_storm(dev, ih);
    if (r != kIOReturnSuccess) return r;
    r = ih_enable_interrupts(dev, ih);
    if (r != kIOReturnSuccess) return r;

    IH_LOG("IH ring online (ring=%#llx wptr_shadow=%#llx) - polled, "
           "call ih_poll()/ih_process() to consume",
           (unsigned long long)ih.ring_bus,
           (unsigned long long)ih.wptr_shadow_bus);
    return kIOReturnSuccess;
}

// ----- ih_v7_0_hw_fini: ih_v7_0_hw_fini -> ih_v7_0_irq_disable (ih_v7_0.c:539-547) -----
//
// Stops the ring and zeroes the HW pointers. Does NOT free the buffers —
// that is ih_release(), upstream's sw_fini.
kern_return_t
ih_v7_0_hw_fini(const DeviceContext &dev, IHContext &ih)
{
    if (!dev.ip.isResolved(IPBlock::OSSSYS)) return kIOReturnNotReady;
    kern_return_t r = ih_disable_interrupts(dev, ih);
    IH_LOG("hw_fini: ring disabled (processed=%llu overflows=%llu)",
           (unsigned long long)ih.entries_processed,
           (unsigned long long)ih.overflows_seen);
    return r;
}

// ----- ih_init_full: the reference's orchestrator name -----
kern_return_t
ih_init_full(DeviceContext &dev, IHContext &ih)
{
    return ih_v7_0_hw_init(dev, ih);
}

} // namespace amdgpu
