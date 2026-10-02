//
//  amdgpu_ih.h — Interrupt Handler (IH) v7_0 ring + drain interface.
//
//  Origin: lemonade-sdk/mac-amdgpu (MIT) @ commit 3bdeed2,
//          dext/amdgpu/amdgpu_ih.h  — ported to the x86 kernel kext.
//
//  Upstream sources behind the reference:
//      drivers/gpu/drm/amd/amdgpu/ih_v7_0.c
//      drivers/gpu/drm/amd/amdgpu/amdgpu_ih.c
//      drivers/gpu/drm/amd/include/ivsrcid/*
//
//  Hardware model: every IP block on the GPU posts interrupts into
//  a unified ring buffer in system memory (the IH ring). Upstream,
//  when the ring becomes non-empty the GPU fires an MSI-X line to
//  the host, the host walks the ring entries, and dispatches each
//  to a per-(client_id, src_id) handler.
//
//  *** THIS PORT IS POLLED, NOT INTERRUPT-DRIVEN ***
//  Navi48Bringup has no IOInterruptEventSource / MSI wired up yet,
//  so nothing on the CPU side would service an asserted line. The
//  ring is therefore brought up with CPU-side interrupt delivery
//  DISABLED (IH_RB_CNTL.ENABLE_INTR = 0, RPTR_REARM = 0) and the
//  host reads the wptr itself — from the writeback shadow in system
//  memory, or straight out of IH_RB_WPTR. See ih_poll()/ih_process().
//  Do not set IHContext::enable_cpu_intr until a real handler exists;
//  a level-triggered INTx assertion with no handler wedges the host.
//
//  Entry format on RDNA4 / GFX12 (32 bytes = 8 dwords):
//      DW0: [7:0] client_id  [15:8] src_id  [23:16] ring_id
//           [27:24] vmid     [31] vmid_src
//      DW1: timestamp_lo
//      DW2: [15:0] timestamp_hi  [31] timestamp_src
//      DW3: [15:0] pasid  [23:16] node_id
//      DW4..7: src_data[0..3] — meaning is per-source-handler
//
//  We allocate the ring in physically contiguous system memory
//  (amdgpu::SysMem; no IOMMU here, so bus == physical) at page
//  alignment. Default 256 KB capacity matches upstream
//  amdgpu_ih_ring_init's default, with use_bus_addr = true.
//
//  Deviations from reference — see the list at the top of ih_v7_0.cpp.
//

#pragma once

#include <stdint.h>

#include "amdgpu_ip.h"
#include "amdgpu_regs.h"
#include "amdgpu_sysmem.h"

namespace amdgpu {

constexpr uint32_t kIHRingDefaultBytes = 256 * 1024;
constexpr uint32_t kIHEntryDwords      = 8;
constexpr uint32_t kIHEntryBytes       = kIHEntryDwords * 4;

// Reference used kASPageSize (16 KB, Apple Silicon). x86 pages are
// 4 KB and sysmem_alloc's minimum alignment is one page.
constexpr uint64_t kIHRingAlignBytes   = 4096;
constexpr uint64_t kIHWptrBufBytes     = 4096;

// How many decoded entries ih_process() dumps to the log before it
// goes quiet (bring-up aid; there is no other way to see the ring).
constexpr uint32_t kIHEntryLogBudget   = 16;

// Single decoded IH entry — what handlers receive from the dispatcher.
struct IHEntry {
    uint8_t  client_id;     // OSSSYS / GFX / SDMA / etc.
    uint8_t  src_id;
    uint16_t ring_id;
    uint8_t  vmid;
    uint8_t  vmid_src;
    uint64_t timestamp;
    uint16_t pasid;
    uint8_t  node_id;
    uint32_t src_data[4];   // raw src_data[0..3]
};

// Per-ring state — we only run one ring (Ring0) for first PM4.
struct IHContext {
    bool      inited  { false };
    bool      enabled { false };

    // Ring buffer + wptr writeback — physically contiguous sysmem.
    // Replaces the reference's IOBufferMemoryDescriptor/IODMACommand
    // pairs (ring_buf/ring_dma, wptr_shadow_buf/wptr_shadow_dma).
    SysMem    ring_mem {};
    SysMem    wptr_mem {};

    uint64_t  ring_bus         { 0 };
    void     *ring_cpu         { nullptr };
    uint32_t  ring_size_bytes  { 0 };   // power of two
    uint32_t  ring_size_dwords { 0 };
    // ptr_mask is in BYTES — (ring_size_bytes - 1). Matches upstream
    // amdgpu_ih.c:52 (`ih->ptr_mask = ih->ring_size - 1`). rptr is a
    // byte offset; the HW wptr from the shadow is also bytes.
    uint32_t  ptr_mask         { 0 };
    uint64_t  wptr_shadow_bus  { 0 };
    volatile uint32_t *wptr_shadow_cpu { nullptr };  // GPU writes wptr here
    // rptr shadow, dword 1 of the same page. Only written on the
    // doorbell path (upstream `*ih->rptr_cpu = ih->rptr`).
    volatile uint32_t *rptr_shadow_cpu { nullptr };
    // host read pointer, in BYTES — advances 32 per dispatched entry.
    uint32_t  rptr             { 0 };

    // --- configuration (upstream amdgpu_ih_ring fields) ---
    // use_bus_addr: ring lives in system memory, IH_RB_BASE carries a
    // bus address and IH_RB_CNTL.MC_SPACE = 2. Always true here.
    bool      use_bus_addr     { true };
    // use_doorbell: advance rptr by ringing a BAR2 doorbell instead of
    // writing IH_RB_RPTR over MMIO. Default OFF — BAR2 may not be
    // mapped yet (dev.bar2 == nullptr) and the MMIO path always works.
    bool      use_doorbell     { false };
    uint32_t  doorbell_index   { 0 };   // filled from dev.doorbell.index.ih
    // enable_cpu_intr: set IH_RB_CNTL.ENABLE_INTR + RPTR_REARM so the
    // IH actually signals the host. MUST stay false until an MSI/INTx
    // handler exists — see the banner at the top of this file.
    bool      enable_cpu_intr  { false };
    // wptr_from_mmio: read IH_RB_WPTR over MMIO instead of trusting
    // the writeback shadow. Bring-up escape hatch if writeback looks
    // dead; the reference always prefers the shadow.
    bool      wptr_from_mmio   { false };

    // Counters
    uint64_t  entries_processed { 0 };
    uint64_t  overflows_seen    { 0 };
    uint32_t  log_budget        { 0 };   // remaining entries to IH_LOG
};

// IH v7 has known interrupt source IDs we care about for "Hello PM4".
// Full list in drivers/gpu/drm/amd/include/ivsrcid/{gfx,sdma,vmc}/*.h
namespace IHSourceID {
    // GFX clients
    constexpr uint8_t CLIENT_GFX          = 0x0A;   // SOC21_IH_CLIENTID_GFX
    constexpr uint8_t CLIENT_ATHUB        = 0x02;
    constexpr uint8_t CLIENT_VMC          = 0x07;
    constexpr uint8_t CLIENT_SDMA0        = 0x0E;
    constexpr uint8_t CLIENT_SDMA1        = 0x0F;

    // GFX sources (within CLIENT_GFX)
    constexpr uint8_t SRC_CP_EOP          = 0xB5;   // 181
    constexpr uint8_t SRC_CP_ECC_ERROR    = 0xC5;   // 197
    constexpr uint8_t SRC_UTCL2_FAULT     = 0x00;   // (with CLIENT_ATHUB)

    // SDMA sources
    constexpr uint8_t SRC_SDMA_TRAP       = 0xE0;   // 224
}

// Register offsets (OSSSYS IP block — IHv7), BASE_IDX 0.
// From drivers/gpu/drm/amd/include/asic_reg/oss/osssys_7_0_0_offset.h.
namespace IHRegs {
    constexpr uint32_t IH_RB_CNTL                  = 0x0080; // line 112
    constexpr uint32_t IH_RB_RPTR                  = 0x0081; // line 114
    constexpr uint32_t IH_RB_WPTR                  = 0x0082; // line 116
    constexpr uint32_t IH_RB_BASE                  = 0x0083; // line 118
    constexpr uint32_t IH_RB_BASE_HI               = 0x0084; // line 120
    constexpr uint32_t IH_RB_WPTR_ADDR_HI          = 0x0085; // line 122
    constexpr uint32_t IH_RB_WPTR_ADDR_LO          = 0x0086; // line 124
    constexpr uint32_t IH_DOORBELL_RPTR            = 0x0087; // line 126
    constexpr uint32_t IH_STORM_CLIENT_LIST_CNTL   = 0x00aa; // line 150
    constexpr uint32_t IH_INT_FLOOD_CNTL           = 0x00d5; // line 192
    constexpr uint32_t IH_MSI_STORM_CTRL           = 0x00f1; // line 222
    constexpr uint32_t IH_CHICKEN                  = 0x018a; // line 262
}

// IH_RB_CNTL bit positions per osssys_7_0_0_sh_mask.h lines 170-185.
constexpr uint32_t kIH_RB_CNTL__RB_ENABLE__SHIFT             = 0x00; // line 170
constexpr uint32_t kIH_RB_CNTL__RB_SIZE__SHIFT               = 0x01; // line 171
constexpr uint32_t kIH_RB_CNTL__WPTR_WRITEBACK_ENABLE__SHIFT = 0x08; // line 172
constexpr uint32_t kIH_RB_CNTL__RB_FULL_DRAIN_ENABLE__SHIFT  = 0x09; // line 173
constexpr uint32_t kIH_RB_CNTL__WPTR_OVERFLOW_ENABLE__SHIFT  = 0x10; // line 177
constexpr uint32_t kIH_RB_CNTL__ENABLE_INTR__SHIFT           = 0x11; // line 178
constexpr uint32_t kIH_RB_CNTL__MC_SWAP__SHIFT               = 0x12; // line 179
constexpr uint32_t kIH_RB_CNTL__MC_SNOOP__SHIFT              = 0x14; // line 180
constexpr uint32_t kIH_RB_CNTL__RPTR_REARM__SHIFT            = 0x15; // line 181
constexpr uint32_t kIH_RB_CNTL__MC_RO__SHIFT                 = 0x16; // line 182
constexpr uint32_t kIH_RB_CNTL__MC_VMID__SHIFT               = 0x18; // line 183
constexpr uint32_t kIH_RB_CNTL__MC_SPACE__SHIFT              = 0x1c; // line 184
constexpr uint32_t kIH_RB_CNTL__WPTR_OVERFLOW_CLEAR__SHIFT   = 0x1f; // line 185

// Field widths, as the value's mask BEFORE shifting:
//   IH_RB_CNTL__RB_SIZE_MASK  = 0x0000003E  -> 5 bits at shift 1
//   IH_RB_CNTL__MC_SPACE_MASK = 0x70000000  -> 3 bits at shift 28
constexpr uint32_t kIH_RB_CNTL__RB_SIZE__MASK                = 0x1Fu;
constexpr uint32_t kIH_RB_CNTL__MC_SPACE__MASK               = 0x07u;

// IH_RB_WPTR fields (osssys_7_0_0_sh_mask.h:206-207). The HW puts
// the overflow flag in bit 0 of IH_RB_WPTR — NOT bit 31 of either
// IH_RB_WPTR or IH_RB_CNTL.
constexpr uint32_t kIH_RB_WPTR__RB_OVERFLOW__SHIFT           = 0x00; // line 206
constexpr uint32_t kIH_RB_WPTR__OFFSET__SHIFT                = 0x02; // line 207

// IH_DOORBELL_RPTR fields (osssys_7_0_0_sh_mask.h). OFFSET is the
// dword index into the BAR2 doorbell aperture, ENABLE routes rptr
// writes through it instead of IH_RB_RPTR.
constexpr uint32_t kIH_DOORBELL_RPTR__OFFSET__SHIFT          = 0x00;
constexpr uint32_t kIH_DOORBELL_RPTR__OFFSET__MASK           = 0x03FFFFFFu;  // value pre-shift
constexpr uint32_t kIH_DOORBELL_RPTR__ENABLE__SHIFT          = 0x1c;

// Storm / flood / MSI throttle field shifts.
constexpr uint32_t kIH_STORM_CLIENT_LIST_CNTL__CLIENT18_IS_STORM_CLIENT__SHIFT = 0x12; // line 342
constexpr uint32_t kIH_INT_FLOOD_CNTL__FLOOD_CNTL_ENABLE__SHIFT                = 0x03; // line 583
constexpr uint32_t kIH_MSI_STORM_CTRL__DELAY__SHIFT                            = 0x00; // line 855
constexpr uint32_t kIH_MSI_STORM_CTRL__DELAY__MASK                             = 0x3u; // value pre-shift

// MC_SPACE values (ih_v7_0.c:192): 2 = bus_addr / sysmem ring,
// 4 = GPUVA. We always run with use_bus_addr=true so the IH ring is
// physically contiguous sysmem and the engine treats the programmed
// RB_BASE as a system bus address.
constexpr uint32_t kIH_RB_CNTL_MC_SPACE_BUS_ADDR = 2;
constexpr uint32_t kIH_RB_CNTL_MC_SPACE_GPUVA    = 4;

//
// API — same names/signatures as the reference, plus the upstream
// ih_v7_0_* entry points and a polling front end.
//

// Allocate ring buffer + wptr shadow. Idempotent.
kern_return_t ih_init(DeviceContext &dev, IHContext &ih);

// Free everything ih_init allocated.
void ih_release(IHContext &ih);

// Toggle IH_RB_CNTL.RB_ENABLE only. Safe to call any time.
kern_return_t ih_toggle(const DeviceContext &dev, IHContext &ih,
                        bool enable);

// Upstream ih_v7_0_enable_interrupts / ih_v7_0_disable_interrupts.
// enable_interrupts sets RB_ENABLE, and ENABLE_INTR only when
// ih.enable_cpu_intr is true (it is false by default here).
kern_return_t ih_enable_interrupts(const DeviceContext &dev, IHContext &ih);
kern_return_t ih_disable_interrupts(const DeviceContext &dev, IHContext &ih);

// Program the ring registers (BASE/CNTL/WPTR_ADDR/DOORBELL_RPTR) and
// reset the pointers. Does NOT set RB_ENABLE.
kern_return_t ih_enable_ring(const DeviceContext &dev, IHContext &ih);

// Configure storm + flood control + MSI storm throttle.
kern_return_t ih_program_msi_storm(const DeviceContext &dev,
                                   const IHContext &ih);

// Upstream ih_v7_0_get_wptr / ih_v7_0_set_rptr.
// ih_get_wptr returns a masked byte offset and handles ring overflow
// (it may advance ih.rptr). ih_set_rptr publishes ih.rptr to HW.
uint32_t ih_get_wptr(const DeviceContext &dev, IHContext &ih);
void     ih_set_rptr(const DeviceContext &dev, IHContext &ih);

// Drain the ring — process every entry between rptr and the current
// wptr. Caller-provided dispatcher is invoked per entry with the
// decoded form. Returns the number of entries processed.
typedef void (*IHDispatchFn)(const IHEntry &entry, void *user);

uint32_t ih_drain(const DeviceContext &dev, IHContext &ih,
                  IHDispatchFn dispatch, void *user);

// Upstream's name for the same walk (amdgpu_ih_process). Identical.
uint32_t ih_process(const DeviceContext &dev, IHContext &ih,
                    IHDispatchFn dispatch, void *user);

// Polled front end: spin for up to timeout_ms waiting for the ring to
// become non-empty, then decode into the caller's array. Returns the
// number of entries written to `out` (<= max_entries). Entries beyond
// max_entries are still consumed (rptr advances) and counted in
// ih.entries_processed; the drop is logged.
// out/max_entries may be null/0 to just drain-and-log.
uint32_t ih_poll(const DeviceContext &dev, IHContext &ih,
                 IHEntry *out, uint32_t max_entries, uint32_t timeout_ms);

// Top-level stage entries. ih_v7_0_hw_init allocates + programs +
// enables the ring; ih_init_full is the reference's name for it so
// the ported amdgpu_init.cpp call site keeps compiling.
kern_return_t ih_v7_0_hw_init(DeviceContext &dev, IHContext &ih);
kern_return_t ih_v7_0_hw_fini(const DeviceContext &dev, IHContext &ih);
kern_return_t ih_init_full(DeviceContext &dev, IHContext &ih);

} // namespace amdgpu
