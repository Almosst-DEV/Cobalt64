//
//  amdgpu_gart.h — GART (Graphics Address Remapping Table) binding helpers.
//
//  Ported from lemonade-sdk/mac-amdgpu (MIT) @ commit 3bdeed2:
//      dext/amdgpu/amdgpu_gart.h
//      dext/amdgpu/amdgpu_gart.cpp
//  Mirrors upstream `amdgpu_gart.c` (amdgpu_gart_table_vram_alloc,
//  amdgpu_gart_map / amdgpu_gart_bind).
//
//  The GART page table itself is managed by GMC (gmc_v12_0.cpp).
//  This module binds system-memory buffers into the GART by writing
//  PTEs into the VRAM-resident page table. The gart_enable() register
//  programming lives in gmc_v12_0.cpp (gmc_mmhub_gart_enable).
//
//  Layout on RDNA4:
//      - 4 KB GPU page granularity (kAMDGPUGPUPageSize), independent of
//        the host CPU page size.
//      - PTE is 8 bytes: high bits = host bus address, low bits = flags
//        (VALID, SYSTEM, SNOOPED, R/W/X, MTYPE, IS_PTE) — PTEFlags.
//
//  Deviations from reference:
//    * IOBufferMemoryDescriptor + IODMACommand → amdgpu::SysMem.
//    * `reads_supported` defaults TRUE here (x86, no IOMMU) — see below.
//
#pragma once

#include <stdint.h>
#include <IOKit/IOReturn.h>

#include "amdgpu_regs.h"
#include "amdgpu_ip.h"
#include "amdgpu_sysmem.h"

namespace amdgpu {

// One BO bound into GART. Tracks the sysmem buffer + the GART MC address
// PSP (or any other GPU IP) should use to reach it.
struct GARTBinding {
    // Replaces the reference's `IOBufferMemoryDescriptor *sysmemBuffer`
    // + `IODMACommand *dmaCommand` pair. Only valid (md != nullptr) for
    // bindings created by gart_bind_sysmem; gart_bind_existing leaves it
    // empty because the caller owns the memory.
    SysMem    sysmem {};

    uint64_t  busAddr     { 0 };  // GPU-visible bus address (== physical, no IOMMU)
    void     *cpuAddr     { nullptr };  // CPU pointer for writes/reads
    uint64_t  sizeBytes   { 0 };
    uint64_t  gartOffset  { 0 };  // byte offset into the GART aperture
    uint64_t  gartMCAddr  { 0 };  // gartStart + gartOffset — what PSP uses
    uint32_t  numGPUPages { 0 };  // number of 4 KB PTEs used
};

struct GARTContext {
    bool        enabled { false };

    // Page-table storage: in VRAM. Accessed CPU-side via the BAR0
    // aperture using bar0_memcpy_to_vram / bar0_memset_vram (see
    // amdgpu_regs.h). One 4 KB page = 512 PTEs = 2 MB of GART space.
    uint64_t    pageTableVRAMOffset { 0 };  // absolute VRAM byte offset of the table
    uint64_t    pageTableSize       { 0 };  // bytes
    uint32_t    numPTEs             { 0 };  // gartSize / kAMDGPUGPUPageSize

    // GART address space layout in MC space.
    uint64_t    gartStart { 0 };  // MC address where GART aperture begins
    uint64_t    gartEnd   { 0 };  // gartStart + gartSize - 1
    uint64_t    gartSize  { 0 };

    // Bump-allocator state — next free GART offset (in bytes).
    uint64_t    nextFreeOffset { 0 };

    // Platform gate: are GPU-initiated reads through GART → system
    // memory actually returning real bytes?
    //
    // On the reference platform (Apple Silicon + Thunderbolt 5) the
    // answer was NO — DART silently zeroed every GPU-initiated read of
    // mapped sysmem, so the reference defaults this FALSE and higher
    // layers refuse GTT BO allocations.
    //
    // **Navi48Bringup deviates: this defaults TRUE.** We are an x86 kext
    // on a desktop PCIe slot with no IOMMU in the path (SysMem allocates
    // with kIOMemoryMapperNone, so bus == host physical). Device-initiated
    // reads of system memory are the ordinary PC path and do return real
    // data. gart_init still records the decision in the log so a hardware
    // run makes the assumption auditable.
    bool        reads_supported { false };
};

//
// gart_bind_sysmem — allocate a system-memory buffer, bind it into
// GART, return the GART MC address to pass to PSP. The SysMem handle is
// stashed in the GARTBinding so gart_unbind can release it.
//
// alignment is coerced up to kAMDGPUGPUPageSize (4 KB) at minimum.
//
kern_return_t gart_bind_sysmem(DeviceContext &dev, GARTContext &gart,
                               uint64_t sizeBytes, uint64_t alignment,
                               GARTBinding *outBinding);

//
// gart_unbind — release a binding (free the SysMem buffer, drop the
// binding's addresses). Bump allocator: the GART slot stays "used"
// until the table is re-zeroed.
//
void gart_unbind(GARTContext &gart, GARTBinding *binding);

//
// gart_bind_existing — bind an EXISTING bus address range into GART.
// Used when the buffer is already allocated elsewhere (e.g. a shared
// firmware-staging buffer) and we just need a GMC MC address for it.
//
// Writes PTEs at the next free GART slot. The caller retains ownership
// of the underlying memory — this function doesn't take a reference.
// PTEs stay live until the GART is reset (GMC re-zeroes the page table)
// or the binding is overwritten by another bind at the same offset.
//
// Idempotent across re-binds of the same buffer: pass the previous
// binding back in to reuse its `gartOffset` (avoids bumping the
// allocator); pass a zero-init binding to allocate a fresh slot.
//
kern_return_t gart_bind_existing(DeviceContext &dev, GARTContext &gart,
                                 uint64_t busAddr, uint64_t sizeBytes,
                                 GARTBinding *binding);

//
// gart_init — populate GARTContext from the just-enabled GMC GART
// aperture. Run AFTER gmc_mmhub_gart_enable / gmc_gfxhub_gart_enable
// (so gmc.gart_start, gmc.gart_size, gmc.gart_pt_bus are valid). After
// this returns, gart_bind_sysmem / gart_bind_existing / gart_unbind
// are operational. Also sets `gart.reads_supported`.
//
struct GMCContext;
kern_return_t gart_init(DeviceContext &dev, const GMCContext &gmc,
                        GARTContext &gart);

} // namespace amdgpu
