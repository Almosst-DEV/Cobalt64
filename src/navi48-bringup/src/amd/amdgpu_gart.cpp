//
//  amdgpu_gart.cpp — GART page table binding helpers.
//
//  Ported from lemonade-sdk/mac-amdgpu (MIT) @ commit 3bdeed2,
//  origin file: dext/amdgpu/amdgpu_gart.cpp (arm64 DriverKit dext).
//
//  Copyright (c) lemonade-sdk contributors. Licensed MIT — see NOTICE.
//
//  Mirrors upstream amdgpu_gart.c (amdgpu_gart_table_vram_alloc,
//  amdgpu_gart_map / amdgpu_gart_bind). The gart_enable() register
//  programming lives in gmc_v12_0.cpp (gmc_mmhub_gart_enable).
//
// =====================================================================
//  Deviations from reference
// =====================================================================
//  1. Platform: x86 IOKit kext. os_log → IOLog, no `%{public}s`, no
//     <DriverKit/…> / <PCIDriverKit/…>.
//  2. IOBufferMemoryDescriptor + IODMACommand (+ PrepareForDMA /
//     GetAddressRange / CompleteDMA) → amdgpu::SysMem + sysmem_alloc /
//     sysmem_free. SysMem allocates physically contiguous, zeroed memory
//     with kIOMemoryMapperNone, so `bus` is the host physical address the
//     GPU sees (no IOMMU) and a single segment is guaranteed — the
//     reference's segCount != 1 failure path has no analogue.
//  3. `gart.reads_supported` defaults TRUE. The reference hard-codes
//     FALSE because Apple Silicon + Thunderbolt 5 DART silently zeroes
//     every GPU-initiated read of mapped system memory. That is an
//     Apple-Silicon platform defect; on an x86 desktop PCIe slot with no
//     IOMMU in the path, device reads of system memory are the ordinary
//     path. Higher layers gate GTT allocations on this flag, so leaving
//     it false here would needlessly disable the whole GTT domain.
//  4. Alignment: the reference coerces alignment up to kASPageSize
//     (16 KB, the Apple Silicon CPU page) because DART rejects anything
//     smaller. Here the floor is kAMDGPUGPUPageSize (4 KB) — the GPU page
//     size — which is also the host page size on x86.
//  5. pageTableVRAMOffset is taken from gmc.gart_pt_vram_offset when the
//     GMC published it. The reference reconstructs it as
//     (gart_pt_bus - vram_start) because its PT offset constant is
//     file-local to gmc_v12_0.cpp; that fallback is kept.
//  6. The reference's unused file-local `kGARTStart` constant (a 4 GB
//     placeholder superseded by gmc.gart_start) is not ported.
//

#include <string.h>
#include <IOKit/IOLib.h>

#include "amdgpu_gart.h"
#include "amdgpu_gmc.h"
#include "amdgpu_log.h"

namespace amdgpu {

//============================================================
// gart_init — populate GARTContext from a just-enabled GMC GART.
//
// Run AFTER gmc_mmhub_gart_enable / gmc_gfxhub_gart_enable so that
// gmc.gart_start / gmc.gart_size are valid. Sets up the bump
// allocator and the platform-gated `reads_supported` flag.
//============================================================
kern_return_t
gart_init(DeviceContext &dev, const GMCContext &gmc, GARTContext &gart)
{
    (void)dev;
    if (gmc.gart_size == 0) {
        GART_LOG("init: gmc.gart_size == 0 (gart_enable hasn't run?)");
        return kIOReturnNotReady;
    }
    if (gmc.gart_pt_bus == 0) {
        GART_LOG("init: gmc.gart_pt_bus == 0 (alloc_resources hasn't run?)");
        return kIOReturnNotReady;
    }

    // Absolute VRAM byte offset of the page table, for bar0_memcpy_to_vram.
    // Prefer the value GMC published; fall back to the reference's
    // reconstruction (gart_pt_bus - vram_start) — both yield
    // dev.vramBase + kGMCGartPTVRAMOffset here.
    gart.enabled = true;
    if (gmc.gart_pt_vram_offset != 0) {
        gart.pageTableVRAMOffset = gmc.gart_pt_vram_offset;
    } else {
        gart.pageTableVRAMOffset = (gmc.gart_pt_bus > gmc.vram_start)
            ? (gmc.gart_pt_bus - gmc.vram_start) : 0;
    }
    gart.pageTableSize  = gmc.gart_pt_size;
    gart.numPTEs        = (uint32_t)(gmc.gart_size / kAMDGPUGPUPageSize);
    gart.gartStart      = gmc.gart_start;
    gart.gartEnd        = gmc.gart_start + gmc.gart_size - 1;
    gart.gartSize       = gmc.gart_size;
    gart.nextFreeOffset = 0;

    // **Platform gate — GPU-initiated system-memory reads.**
    //
    // GART is fully functional from a software standpoint:
    //   • Page table is in VRAM and zero-initialised
    //   • MMHUB / GFXHUB program the PT base, aperture start/end and the
    //     VMID0 control register exactly per upstream
    //   • gart_bind_sysmem / gart_bind_existing write PTEs correctly
    //   • Engine MC resolution returns the right bus address
    //
    // The reference keeps this FALSE on every platform it supports,
    // because Apple Silicon + TB5 DART zeroes every GPU-initiated sysmem
    // read: the PCIe transaction reaches DART, the data comes back all
    // zero, so the PTE points at the right host RAM but the engine
    // receives nothing. Higher layers refuse GTT BO allocations when the
    // flag is false so clients fail loud.
    //
    // Navi48Bringup runs on an x86 desktop PCIe slot with no IOMMU in the
    // path (amdgpu_sysmem.cpp allocates with kIOMemoryMapperNone, so the
    // bus address is the host physical address). Device-initiated reads of
    // system memory are the ordinary PC path, so this defaults TRUE — see
    // Deviation 3. If a hardware run shows PSP reading zeros out of a
    // GART-bound buffer, flip it back and use the VRAM staging path.
    gart.reads_supported = true;

    GART_LOG("init: gart aperture [%#llx..%#llx) size=%llu bytes, "
             "%u PTEs, pt_bus=%#llx pt_vram_off=%#llx (%llu bytes), "
             "reads_supported=%d (x86, no IOMMU: bus addr == host phys)",
             (unsigned long long)gart.gartStart,
             (unsigned long long)(gart.gartEnd + 1),
             (unsigned long long)gart.gartSize,
             (unsigned)gart.numPTEs,
             (unsigned long long)gmc.gart_pt_bus,
             (unsigned long long)gart.pageTableVRAMOffset,
             (unsigned long long)gart.pageTableSize,
             gart.reads_supported ? 1 : 0);
    return kIOReturnSuccess;
}

//============================================================
// gart_bind_sysmem — allocate a system-memory buffer, write PTEs,
// return the GART MC address.
//
// Limitations:
//   - Single contiguous segment only (sysmem_alloc guarantees one).
//   - Bump allocator: no free path until gart_unbind is called, and
//     even then the slot stays reserved.
//============================================================
kern_return_t
gart_bind_sysmem(DeviceContext &dev, GARTContext &gart,
                 uint64_t sizeBytes, uint64_t alignment,
                 GARTBinding *outBinding)
{
    if (outBinding == nullptr) return kIOReturnBadArgument;
    if (gart.numPTEs == 0) {
        GART_LOG("bind: GART not initialised");
        return kIOReturnNotReady;
    }
    if (alignment < kAMDGPUGPUPageSize) alignment = kAMDGPUGPUPageSize;
    // Round size up to a GPU page boundary so we map whole PTEs.
    uint64_t roundedSize = (sizeBytes + kAMDGPUGPUPageSize - 1) &
                           ~((uint64_t)kAMDGPUGPUPageSize - 1);
    uint32_t numPTEs = (uint32_t)(roundedSize / kAMDGPUGPUPageSize);
    if (gart.nextFreeOffset + roundedSize > gart.gartSize) {
        GART_LOG("bind: out of GART space (need %llu, free %llu)",
                 (unsigned long long)roundedSize,
                 (unsigned long long)(gart.gartSize - gart.nextFreeOffset));
        return kIOReturnNoSpace;
    }

    // Allocate the system-memory buffer (replaces the reference's
    // IOBufferMemoryDescriptor::Create + IODMACommand::PrepareForDMA).
    SysMem m {};
    kern_return_t ret = sysmem_alloc(m, roundedSize, alignment);
    if (ret != kIOReturnSuccess || !m.valid()) {
        GART_LOG("bind: sysmem_alloc failed: %#x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }

    // Write PTEs into the VRAM page table.
    //
    // Each PTE is 8 bytes: high bits = host physical address, low bits =
    // flags. PTEFlags::SYSMEM_RW = VALID|SYSTEM|SNOOPED|EXEC|R|W plus
    // IS_PTE (bit 63, mandatory on GFX12) and MTYPE_GFX12_UC.
    uint64_t gartOff = gart.nextFreeOffset;
    uint64_t pteStartIndex = gartOff / kAMDGPUGPUPageSize;
    for (uint32_t i = 0; i < numPTEs; i++) {
        uint64_t physAddr = m.bus + (uint64_t)i * kAMDGPUGPUPageSize;
        uint64_t pte = (physAddr & ~((uint64_t)0xFFFULL)) |
                       PTEFlags::SYSMEM_RW;
        uint64_t pteOffsetInPT = (pteStartIndex + i) * 8ULL;
        bar0_memcpy_to_vram(dev,
                            gart.pageTableVRAMOffset + pteOffsetInPT,
                            &pte, sizeof(pte));
    }

    // HDP flush after writing PTEs so GMC sees the new entries before
    // any GART access. Mirrors upstream amdgpu_gart_invalidate_tlb's
    // amdgpu_device_flush_hdp call.
    amdgpu_hdp_flush(dev);

    outBinding->sysmem      = m;
    outBinding->busAddr     = m.bus;
    outBinding->cpuAddr     = m.cpu;
    outBinding->sizeBytes   = roundedSize;
    outBinding->gartOffset  = gartOff;
    outBinding->gartMCAddr  = gart.gartStart + gartOff;
    outBinding->numGPUPages = numPTEs;

    gart.nextFreeOffset += roundedSize;

    GART_LOG("bind: %llu bytes @ bus=%#llx -> gart_mc=%#llx "
             "(%u PTEs at PT idx %llu)",
             (unsigned long long)roundedSize, (unsigned long long)m.bus,
             (unsigned long long)outBinding->gartMCAddr,
             (unsigned)numPTEs, (unsigned long long)pteStartIndex);
    return kIOReturnSuccess;
}

//============================================================
// gart_bind_existing — write PTEs for a pre-existing DMA-able bus
// address range. No buffer alloc. The caller owns the memory; this
// only programs the page table.
//
// Reuse semantics: pass a binding with non-zero gartMCAddr to RE-USE
// the previously assigned slot (avoids burning bump-allocator space
// when the host swaps the buffer's contents but the busAddr/size are
// stable). Pass a zero-initialised binding to allocate fresh.
//============================================================
kern_return_t
gart_bind_existing(DeviceContext &dev, GARTContext &gart,
                   uint64_t busAddr, uint64_t sizeBytes,
                   GARTBinding *binding)
{
    if (binding == nullptr) return kIOReturnBadArgument;
    if (gart.numPTEs == 0) {
        GART_LOG("bind_existing: GART not initialised");
        return kIOReturnNotReady;
    }
    if ((busAddr & (kAMDGPUGPUPageSize - 1)) != 0) {
        GART_LOG("bind_existing: busAddr %#llx not 4 KB aligned",
                 (unsigned long long)busAddr);
        return kIOReturnNotAligned;
    }
    uint64_t rounded = (sizeBytes + kAMDGPUGPUPageSize - 1) &
                       ~((uint64_t)kAMDGPUGPUPageSize - 1);
    uint32_t numPTEs = (uint32_t)(rounded / kAMDGPUGPUPageSize);

    // Choose the GART slot: reuse if the binding already has one, else bump.
    uint64_t gartOff;
    if (binding->gartMCAddr != 0 && binding->numGPUPages >= numPTEs) {
        gartOff = binding->gartOffset;
    } else {
        if (gart.nextFreeOffset + rounded > gart.gartSize) {
            GART_LOG("bind_existing: out of GART (need %llu, free %llu)",
                     (unsigned long long)rounded,
                     (unsigned long long)(gart.gartSize - gart.nextFreeOffset));
            return kIOReturnNoSpace;
        }
        gartOff = gart.nextFreeOffset;
        gart.nextFreeOffset += rounded;
    }

    uint64_t pteStartIndex = gartOff / kAMDGPUGPUPageSize;
    for (uint32_t i = 0; i < numPTEs; i++) {
        uint64_t physAddr = busAddr + (uint64_t)i * kAMDGPUGPUPageSize;
        uint64_t pte = (physAddr & ~((uint64_t)0xFFFULL)) |
                       PTEFlags::SYSMEM_RW;
        uint64_t pteOffsetInPT = (pteStartIndex + i) * 8ULL;
        bar0_memcpy_to_vram(dev,
                            gart.pageTableVRAMOffset + pteOffsetInPT,
                            &pte, sizeof(pte));
    }
    amdgpu_hdp_flush(dev);

    binding->sysmem      = SysMem{};   // caller owns the memory
    binding->busAddr     = busAddr;
    binding->cpuAddr     = nullptr;    // caller has the CPU pointer
    binding->sizeBytes   = rounded;
    binding->gartOffset  = gartOff;
    binding->gartMCAddr  = gart.gartStart + gartOff;
    binding->numGPUPages = numPTEs;

    GART_LOG("bind_existing: %llu bytes @ bus=%#llx -> gart_mc=%#llx "
             "(%u PTEs at PT idx %llu)",
             (unsigned long long)rounded, (unsigned long long)busAddr,
             (unsigned long long)binding->gartMCAddr,
             (unsigned)numPTEs, (unsigned long long)pteStartIndex);
    return kIOReturnSuccess;
}

//============================================================
// gart_unbind — release the buffer this binding owns.
// (Bump allocator: the GART slot stays "used" until the table is
// re-zeroed by gmc_alloc_resources.)
//============================================================
void
gart_unbind(GARTContext &gart, GARTBinding *binding)
{
    (void)gart;
    if (binding == nullptr || !binding->sysmem.valid()) return;
    sysmem_free(binding->sysmem);
    binding->busAddr = 0;
    binding->cpuAddr = nullptr;
    // PTE invalidation is handled by the full-table memset in
    // gmc_alloc_resources; the bump allocator means a single unbind
    // doesn't free the slot.
}

} // namespace amdgpu
