//
//  gmc_v12_0.cpp — GMC v12 / MMHUB v4_1_0 / GFXHUB v12_0 controller.
//
//  Ported from lemonade-sdk/mac-amdgpu (MIT) @ commit 3bdeed2,
//  origin file: dext/amdgpu/gmc_v12_0.cpp (arm64 DriverKit dext).
//
//  Copyright (c) lemonade-sdk contributors. Licensed MIT — see NOTICE.
//
//  Upstream sources the reference itself ports (line references in the
//  comments below are the reference's audit trail and are preserved):
//    drivers/gpu/drm/amd/amdgpu/gmc_v12_0.c
//    drivers/gpu/drm/amd/amdgpu/mmhub_v4_1_0.c
//    drivers/gpu/drm/amd/amdgpu/gfxhub_v12_0.c
//
// =====================================================================
//  Deviations from reference
// =====================================================================
//  1. Platform: x86 IOKit kext, not an arm64 DriverKit dext. No
//     <DriverKit/…>, no os_log (GMC_LOG → IOLog), no `%{public}s`.
//  2. IOBufferMemoryDescriptor + IODMACommand (dummy_page, mem_scratch)
//     → amdgpu::SysMem / sysmem_alloc / sysmem_free. No IOMMU here, so
//     SysMem.bus is the host physical address the GPU sees directly.
//  3. VRAM offsets: every fixed VRAM offset the reference hardcodes is
//     relative to dev.vramBase (PORTING.md). The UEFI/GOP console owns
//     VRAM [0, 0x7e9000) and the kernel keeps drawing into it — we never
//     write below dev.vramBase. GART page table: dev.vramBase + 0x700000
//     (512 KiB). VRAM allocator: [dev.vramBase + 0x1800000, dev.vramLimit).
//     The reference's `kPspReservedTopBytes` heuristic is replaced by that
//     fixed layout.
//  4. real_vram_size comes from dev.vramSizeBytes (RCC_CONFIG_MEMSIZE =
//     16304 MiB on the RX 9070 XT) instead of the reference's hardcoded
//     32 GiB R9700 constant. This is what upstream gmc_v12_0_mc_init
//     actually does (adev->nbio.funcs->get_memsize). visible_vram_size
//     comes from dev.bar0Size (the 256 MiB BAR0 CPU aperture) instead of
//     the reference's dev.bar2VisibleVRAMSize — BAR0 is the VRAM aperture
//     on x86; the reference's platform exposes it as BAR2.
//  5. gart_start alignment: the reference aligns gart_start to
//     kASPageSize (16 KiB) above vram_end. Upstream amdgpu_gmc_gart_location
//     aligns it to 4 GiB (`four_gb`). On the reference's own card the two
//     agree (vram_end+1 = 0x8800000000, already 4 GiB aligned); on this
//     card they do not — 16 KiB alignment would place gart_start at
//     0x83FB000000, i.e. inside the MMHUB FB_LOCATION window
//     (0x8000000000..0x83FBFFFFFF) whose top 16 MiB holds the on-die
//     discovery TMR. We use upstream's 4 GiB alignment → 0x8400000000,
//     cleanly above the FB window. Reference behaviour is preserved on
//     reference hardware.
//  6. gmc.gart_pt_vram_offset added to GMCContext. The reference keeps
//     kGMCGartPTVRAMOffset file-local and amdgpu_gart.cpp reconstructs it
//     as (gart_pt_bus - vram_start). That still works here, but because
//     our offset is dev.vramBase-relative we publish it explicitly so PTE
//     writers have one source of truth.
//  7. `gmc.skipDisplayRisky` gate added, DEFAULT FALSE = full reference
//     behaviour. See the "Display risk" note below.
//  8. The reference's dead legacy `gmc_bind_existing(GMCContext&, …)`
//     overload (returns kIOReturnUnsupported, not declared in its header)
//     is not ported.
//  9. mem_scratch stays in system memory as in the reference, so the
//     SYSTEM_APERTURE_DEFAULT_ADDR expression
//     (mem_scratch_bus - vram_start + vram_base_offset) wraps, exactly as
//     it does in the reference. Upstream allocates mem_scratch in VRAM,
//     which makes the subtraction meaningful. We keep the reference's
//     arithmetic verbatim but log the programmed value so a hardware run
//     shows it. It only affects accesses that land in the system aperture
//     but in neither FB nor AGP — nothing on the scanout path.
//
// =====================================================================
//  Display risk (the UEFI console is LIVE while this runs)
// =====================================================================
//  DCN resolves scanout addresses through its OWN DCN_VM_FB_LOCATION_* /
//  DCN_VM_AGP_* / DCN_VM_SYSTEM_APERTURE_* registers, which this file
//  never touches. Neither does it touch MMMC_VM_FB_LOCATION_BASE/TOP
//  (read-only here, as in the reference) — the console scanout depends on
//  them. But display traffic still crosses MMHUB, so:
//
//   * MOST CRITICAL, and the reason vram_start MUST come from
//     FB_LOCATION_BASE: hub_init_system_aperture_regs programs
//     MMMC_VM_SYSTEM_APERTURE_{LOW,HIGH}_ADDR from fb_start/fb_end. With
//     vram_start = 0x8000000000 the console framebuffer (MC
//     0x8000000000..0x80007e9000) stays INSIDE the passthrough aperture.
//     Had vram_start been left 0, the aperture would cover MC [0, 16 GiB)
//     and every display fetch would fall outside it → translated, faulted,
//     black screen. This is a correctness requirement, not a knob.
//   * RISKY #1 — MMMC_VM_MX_L1_TLB_CNTL (hub_init_tlb_regs): flips MMHUB
//     from the VBIOS aperture-only model to ENABLE_ADVANCED_DRIVER_MODEL=1
//     with MTYPE=UC while the display is streaming.
//   * RISKY #2 — MMVM_L2_CNTL/2/3/4/5 (hub_init_cache_regs): CNTL3/4/5 are
//     blind full-register writes of upstream reset defaults (they discard
//     whatever VBIOS left), and CNTL2 issues a live INVALIDATE_ALL_L1_TLBS
//     + INVALIDATE_L2_CACHE under an active scanout.
//   * RISKY #3 — the AGP triple (AGP_BASE=0, BOT=0xFFFFFF, TOP=0) in
//     hub_init_system_aperture_regs: encodes "AGP disabled" (upstream
//     amdgpu_gmc_set_agp_default). Display reads FB, not AGP, so this is
//     the mildest of the three.
//   * Low risk: CONTEXT0 enable (FB addresses never reach the PT walk
//     because they are inside the system aperture), CONTEXT1..15 config
//     (unused by DCN), invalidation-engine arming (no request issued),
//     set_fault_enable_default, the engine-17 TLB flush (a stall + refill),
//     and the HDP flush.
//   * GFXHUB is a different hub entirely; the display does not use it, so
//     gmc_gfxhub_gart_enable is display-safe and is never gated.
//
//  `gmc.skipDisplayRisky` (default FALSE = reference behaviour) skips
//  exactly RISKY #1/#2/#3 on MMHUB only. Set it true to find out whether
//  the console survives the safe subset, then bisect.
//

#include <string.h>
#include <IOKit/IOLib.h>

#include "amdgpu_gmc.h"
#include "amdgpu_log.h"
#include "amdgpu_field_defs.h"
#include "amdgpu_sdma.h"   // 0.0.193: sdma_ib_copy_linear_test for the vmfrag self-test

// amdgpu_regs.h's REG_SET_FIELD complements `reg##__##field##_MASK`, and
// amdgpu_field_defs.h spells those masks as plain (signed) `int` literals
// (e.g. `0x00000001`). `uint32_t & ~int` therefore converts a negative int
// to unsigned, which this build's warning set flags as -Wsign-conversion —
// once per REG_SET_FIELD in this file (62 times). Redefine it locally with
// the arithmetic forced unsigned: identical semantics, identical result,
// but warning-free, and every call site below stays textually identical to
// the reference (the register sequences are the audit trail). Scope is this
// translation unit only; amdgpu_regs.h belongs to another module and is not
// modified.
#undef REG_SET_FIELD
#define REG_SET_FIELD(value, reg, field, val)                                \
    ((uint32_t)((((uint32_t)(value)) & ~((uint32_t)(reg##__##field##_MASK))) \
                | ((((uint32_t)(val)) << (reg##__##field##__SHIFT))          \
                   & ((uint32_t)(reg##__##field##_MASK)))))

namespace amdgpu {

// ----- Constants -----
//
// The reference hardcoded the R9700's 32 GiB total and took the visible
// window from dev.bar2VisibleVRAMSize. We read both from DeviceContext
// (see Deviation 4); these are only the fallbacks.
constexpr uint64_t kFallbackVisibleVRAM  = 256ULL * 1024 * 1024;  // BAR0 default
constexpr uint64_t kGARTInitialSize      = 256ULL * 1024 * 1024;  // 256 MB
//: window reserved at the TOP of the aperture for this driver's own buffers,
// clear of Apple's bottom-up allocations (~2218 pages, 8.7 MiB). Our measured
// footprint is 0x38000 (224 KiB); 4 MiB leaves ample headroom.
constexpr uint64_t kGARTBumpHighReserve  = 4ULL * 1024 * 1024;    // 4 MB
// Upstream amdgpu_gmc_gart_location's `four_gb` (Deviation 5).
constexpr uint64_t kGARTAlignment        = 0x0000000100000000ULL;

// Upstream `gmc_v12_0_gart_init` calls `amdgpu_gart_table_vram_alloc`
// to put the GART page table in VRAM, not sysmem. On GFX12 the PT walker
// reaches VRAM through the FB aperture (direct, no PCIe hop) but cannot
// reliably reach host memory for the PT fetch, so PT-must-be-in-VRAM is
// a hard requirement.
//
// Offsets below are relative to dev.vramBase (PORTING.md VRAM layout);
// dev.vramBase sits above the live UEFI console framebuffer:
//   +0x00000000  fw_pri            1 MiB   PSP staging
//   +0x00100000  PSP ring/cmd/fence       48 KiB
//   +0x00200000  TMR               4 MiB
//   +0x00700000  GART page table   512 KiB = 65536 PTEs = 256 MiB aperture
//   +0x01000000  fw_buf            8 MiB
//   +0x01800000  VRAM allocator …  up to dev.vramLimit
constexpr uint64_t kGMCGartPTVRAMOffset = 0x00700000;
constexpr uint32_t kGMCGartPTBytes      = 512 * 1024;
// Replaces the reference's kPspReservedTopBytes heuristic with the fixed
// layout above (Deviation 3).
constexpr uint64_t kGMCVRAMAllocOffset  = 0x01800000;

// ---- GFXHUB VMID-0 PDB0 (option B'; Navi48Bringup addition) ------------
//
// The PDB0 is one 4 KiB GPU page and lives in the gap the fixed layout above
// leaves between the GART page table (+0x700000, 512 KiB, ends at +0x780000)
// and the SMU driver table (+0xE00000, amdgpu_smu.h:158). Nothing else claims
// it, so the address is deterministic in every log — better for a page the
// GMC walker fetches on every VMID-0 access than an allocator slot that moves.
constexpr uint64_t kGMCPDB0VRAMOffset = 0x00780000;   // from dev.vramBase
constexpr uint32_t kGMCPDB0Bytes      = 4096;         // 512 entries x 8 B
constexpr uint32_t kGMCPDB0Entries    = kGMCPDB0Bytes / 8;

// PDE/PTE bit layout for GFX12, from ref/linux-amdgpu/amdgpu_vm.h. These are
// PDB0 entry flags, not GART PTE flags, so they are spelled out here rather
// than reusing PTEFlags (which is the sysmem-PTE combination).
//   AMDGPU_PTE_VALID      1<<0    amdgpu_vm.h:57
//   AMDGPU_PTE_SNOOPED    1<<2    amdgpu_vm.h:59
//   AMDGPU_PTE_EXECUTABLE 1<<4    amdgpu_vm.h:65
//   AMDGPU_PTE_READABLE   1<<5    amdgpu_vm.h:67
//   AMDGPU_PTE_WRITEABLE  1<<6    amdgpu_vm.h:68
//   AMDGPU_PTE_FRAG(x)    (x&0x1f)<<7                     amdgpu_vm.h:70
//   AMDGPU_PTE_MTYPE_GFX12_SHIFT(m)  (m)<<54              amdgpu_vm.h:126
//   AMDGPU_PDE_BFS_GFX12(a)          ((a)&0x1f)<<58       amdgpu_vm.h:138
//   AMDGPU_PTE_IS_PTE / AMDGPU_PDE_PTE_GFX12  1<<63       amdgpu_vm.h:134/141
//   pte_addr_mask for IP_VERSION(12,0,1)  0x0000FFFFFFFFF000  gmc_v12_0.c:841
constexpr uint64_t kPDEAddrMask   = 0x0000FFFFFFFFF000ULL;
constexpr uint64_t kPDEValid      = (1ULL << 0);
constexpr uint64_t kPDESnooped    = (1ULL << 2);
constexpr uint64_t kPDEExecutable = (1ULL << 4);
constexpr uint64_t kPDEReadable   = (1ULL << 5);
constexpr uint64_t kPDEWriteable  = (1ULL << 6);
constexpr uint64_t kPDEIsPTE      = (1ULL << 63);
static inline constexpr uint64_t kPDEFrag(uint32_t x) {
    return ((uint64_t)(x) & 0x1FULL) << 7;
}

// ----- mc_init -----
//
// Port of gmc_v12_0_mc_init (gmc_v12_0.c:727) + amdgpu_gmc_vram_location
// + amdgpu_gmc_gart_location + amdgpu_gmc_agp_location.
kern_return_t
gmc_mc_init(DeviceContext &dev, GMCContext &gmc)
{
    if (gmc.inited) return kIOReturnSuccess;

    // Upstream: adev->gmc.mc_vram_size = nbio.funcs->get_memsize() << 20,
    // real_vram_size = mc_vram_size. dev.vramSizeBytes is exactly that
    // (RCC_CONFIG_MEMSIZE, already shifted) — see Deviation 4.
    gmc.real_vram_size = dev.vramSizeBytes;

    if (dev.bar0Size > 0 && (gmc.real_vram_size == 0 ||
                             dev.bar0Size <= gmc.real_vram_size)) {
        gmc.visible_vram_size = dev.bar0Size;
    } else {
        gmc.visible_vram_size = kFallbackVisibleVRAM;
    }

    // Read the VBIOS/SOS-programmed FB_LOCATION_BASE + FB_OFFSET from
    // MMHUB so vram_start/fb_start/vram_base_offset reflect REAL MC space
    // (matches upstream gmc_v12_0_vram_gtt_location + mmhub_v4_1_0
    // get_fb_location / get_mc_fb_offset). The reference learned this the
    // hard way: leaving vram_start = 0 produced a system aperture computed
    // against the wrong base, which broke PSP LOAD_IP_FW for TMR-resident
    // IPs (SDMA/CP_RS64/MES/RLC) while SMU/IMU passed because they use
    // SOS-internal paths bypassing the configurable system aperture.
    //
    // On this card it is also the difference between a live console and a
    // black screen — see the "Display risk" note at the top of the file.
    // We only ever READ these registers; FB_LOCATION_BASE/TOP are never
    // reprogrammed.
    uint64_t vram_start_actual       = 0;
    uint64_t vram_base_offset_actual = 0;
    uint32_t fb_base_raw = 0, fb_top_raw = 0, fb_offset_raw = 0;
    if (dev.ip.isResolved(IPBlock::MMHUB)) {
        uint32_t mmhub_base = dev.ip.get(IPBlock::MMHUB);
        fb_base_raw = RREG32(dev,
            mmhub_base + MMHUBRegs::MMMC_VM_FB_LOCATION_BASE);
        vram_start_actual =
            ((uint64_t)(fb_base_raw & MMHUBRegs::kFBBaseMask))
            << MMHUBRegs::kFBBaseShift;
        fb_top_raw = RREG32(dev,
            mmhub_base + MMHUBRegs::MMMC_VM_FB_LOCATION_TOP);
        fb_offset_raw = RREG32(dev,
            mmhub_base + MMHUBRegs::MMMC_VM_FB_OFFSET);
        vram_base_offset_actual = (uint64_t)fb_offset_raw << 24;
    }
    // dev.vramMcBase was latched from the same register when the kext
    // built its DeviceContext; prefer the live read but fall back to it.
    if (vram_start_actual == 0) vram_start_actual = dev.vramMcBase;

    GMC_LOG("mc_init: MMHUB FB_LOCATION_BASE=%#x TOP=%#x FB_OFFSET=%#x "
            "-> vram_start=%#llx vram_base_offset=%#llx (read-only, never "
            "reprogrammed — the console scans out of this window)",
            (unsigned)fb_base_raw, (unsigned)fb_top_raw,
            (unsigned)fb_offset_raw,
            (unsigned long long)vram_start_actual,
            (unsigned long long)vram_base_offset_actual);

    if (gmc.real_vram_size == 0) {
        // Last resort: derive from the FB window (TOP..BASE inclusive).
        uint64_t fb_top_mc =
            (((uint64_t)(fb_top_raw & MMHUBRegs::kFBBaseMask))
             << MMHUBRegs::kFBBaseShift) | 0xFFFFFFULL;
        if (fb_top_mc > vram_start_actual) {
            gmc.real_vram_size = fb_top_mc - vram_start_actual + 1;
            GMC_LOG("mc_init: dev.vramSizeBytes was 0 — derived "
                    "real_vram_size=%llu MB from FB_LOCATION window",
                    (unsigned long long)(gmc.real_vram_size >> 20));
        } else {
            GMC_LOG("mc_init: no VRAM size available");
            return kIOReturnNoMemory;
        }
    }

    gmc.vram_start       = vram_start_actual;
    gmc.vram_end         = vram_start_actual + gmc.real_vram_size - 1;
    gmc.fb_start         = gmc.vram_start;
    gmc.fb_end           = gmc.vram_end;
    gmc.vram_base_offset = vram_base_offset_actual;

    // GART aperture starts above VRAM. Upstream amdgpu_gmc_gart_location
    // uses ALIGN(mc->vram_end + 1, four_gb); the reference used a 16 KiB
    // align, which coincides on its hardware but not on ours — see
    // Deviation 5.
    gmc.gart_size  = kGARTInitialSize;
    gmc.gart_start = (gmc.vram_end + 1 + kGARTAlignment - 1)
                     & ~(kGARTAlignment - 1);
    gmc.gart_end   = gmc.gart_start + gmc.gart_size - 1;

    // VM manager params for GFX12: 4-level page tables (num_level=3
    // means depth 3, i.e. 4 levels counting root), 512-entry blocks
    // (block_size=9 means log2(512)=9), 48-bit VA.
    gmc.num_level   = 3;
    gmc.block_size  = 9;
    gmc.max_pfn     = (1ULL << 48) >> 12;   // 48-bit VA / 4 KB

    // AGP aperture — upstream `amdgpu_gmc_set_agp_default` sentinel
    // (amdgpu_gmc.c:392-394): start=0xffffffffffff (48-bit max), end=0,
    // size=0. With agp_end=0 < fb_end, the system aperture HIGH bound
    // in init_system_aperture_regs picks fb_end (correct).
    gmc.agp_start = 0xFFFFFFFFFFFFULL;     // 48-bit max = AGP disabled
    gmc.agp_end   = 0ULL;

    //: keep our allocations clear of Apple's, which start at page 0.
    if (gmc.gart_high_bump && gmc.gart_size > kGARTBumpHighReserve)
        gmc.gart_bump_start = gmc.gart_size - kGARTBumpHighReserve;
    else
        gmc.gart_bump_start = 0;
    gmc.gart_bump_offset = gmc.gart_bump_start;
    GMC_LOG("mc_init: GART bump region starts at %#llx (%s)",
            (unsigned long long)gmc.gart_bump_start,
            gmc.gart_high_bump ? "HIGH - navi48-gart-high=1, clear of Apple's pages"
                               : "0 - legacy, contends with Apple for page 0");

    GMC_LOG("mc_init: real_vram=%llu MB visible=%llu MB "
            "vram=[%#llx..%#llx] gart=[%#llx..%#llx] fb_offset=%#llx",
            (unsigned long long)(gmc.real_vram_size  >> 20),
            (unsigned long long)(gmc.visible_vram_size >> 20),
            (unsigned long long)gmc.vram_start,
            (unsigned long long)gmc.vram_end,
            (unsigned long long)gmc.gart_start,
            (unsigned long long)gmc.gart_end,
            (unsigned long long)gmc.vram_base_offset);

    gmc.inited = true;
    return kIOReturnSuccess;
}

// ----- VRAM allocator setup -----
//
// The reference pins its allocator to
// [vram_start + kPspReservedTopBytes, vram_start + visible_vram_size)
// so every allocation stays inside the CPU-visible aperture (its
// bar0_memcpy_to_vram takes an aperture-relative offset, so anything
// past the aperture is unreachable; the v0.1.21 dext crashed in
// rlc_setup_csb_buffer for exactly that reason).
//
// Ours is the same idea against the Navi48Bringup fixed layout: the
// allocator owns [dev.vramBase + kGMCVRAMAllocOffset, dev.vramLimit),
// expressed in MC space. Subtracting gmc.vram_start from any returned
// gpu_va yields the absolute VRAM byte offset for bar0_* — same
// convention as the reference. cpu_base stays null; callers write
// through bar0_memcpy_to_vram / bar0_memset_vram so the WC mapping is
// always fenced.
kern_return_t
gmc_vram_alloc_init(DeviceContext &dev, GMCContext &gmc)
{
    if (!gmc.inited) return kIOReturnNotReady;
    if (gmc.vram_alloc.is_inited()) return kIOReturnSuccess;

    const uint64_t alloc_vram_off = dev.vramBase + kGMCVRAMAllocOffset;
    if (dev.vramLimit <= alloc_vram_off) {
        GMC_LOG("vram_alloc: vramLimit %#llx <= allocator base %#llx, "
                "can't allocate",
                (unsigned long long)dev.vramLimit,
                (unsigned long long)alloc_vram_off);
        return kIOReturnNoMemory;
    }
    const uint64_t alloc_size = dev.vramLimit - alloc_vram_off;
    const uint64_t alloc_base_mc = gmc.vram_start + alloc_vram_off;

    gmc.vram_alloc.init(alloc_base_mc, alloc_size, nullptr);

    // Second pool: everything above the BAR0 aperture, minus a reserved tail.
    // The tail clears TWO things, and they are defined together in amdgpu_gmc.h
    // because they are coupled to what the TTL reports to Apple — see the long
    // comment there before changing either:
    //   kVramHiTailReserve      the PSP TMR and the IP discovery table
    //   kAppleArenaCarveReserve the region Apple carves DOWNWARD from the hiTop
    //                           our Navi48Ttl::getLocalMemoryInfo reports
    if (dev.vramSizeBytes > dev.vramLimit + kVramHiTotalReserve) {
        gmc.vram_hi_base = dev.vramLimit;
        gmc.vram_hi_size = dev.vramSizeBytes - kVramHiTotalReserve - gmc.vram_hi_base;
        gmc.vram_alloc_hi.init(gmc.vram_start + gmc.vram_hi_base, gmc.vram_hi_size, nullptr);
        GMC_LOG("vram_alloc_hi: MC [%#llx..%#llx) = vram+[%#llx..%#llx) size=%llu MB "
                "(device-only — CPU reaches it through MM_INDEX, not BAR0; top %llu MB "
                "reserved: PSP TMR, IP discovery, and Apple's page-table arena)",
                (unsigned long long)(gmc.vram_start + gmc.vram_hi_base),
                (unsigned long long)(gmc.vram_start + gmc.vram_hi_base + gmc.vram_hi_size),
                (unsigned long long)gmc.vram_hi_base,
                (unsigned long long)(gmc.vram_hi_base + gmc.vram_hi_size),
                (unsigned long long)(gmc.vram_hi_size >> 20),
                (unsigned long long)(kVramHiTotalReserve >> 20));
    } else {
        GMC_LOG("vram_alloc_hi: not created (vram %llu MB, aperture %llu MB)",
                (unsigned long long)(dev.vramSizeBytes >> 20),
                (unsigned long long)(dev.vramLimit >> 20));
    }
    GMC_LOG("vram_alloc: MC [%#llx..%#llx) = vram+[%#llx..%#llx) "
            "size=%llu MB (BAR0-mapped; below vram+%#llx is reserved: "
            "console fb / PSP / GART PT / fw_buf)",
            (unsigned long long)alloc_base_mc,
            (unsigned long long)(alloc_base_mc + alloc_size),
            (unsigned long long)alloc_vram_off,
            (unsigned long long)dev.vramLimit,
            (unsigned long long)(alloc_size >> 20),
            (unsigned long long)alloc_vram_off);
    return kIOReturnSuccess;
}

// ----- MMHUB v4_1_0 offsets -----
//
// Offsets vendored from upstream
// drivers/gpu/drm/amd/include/asic_reg/mmhub/mmhub_4_1_0_offset.h.
// Audit #4 F4-F5: the previous values in this table were taken from
// the wrong IP generation (mmhub_v3 / older) and resulted in writes
// landing on unrelated registers. Every offset below is cross-
// referenced to the upstream header.
kern_return_t
gmc_mmhub_offsets_init(GMCContext &gmc)
{
    if (gmc.mmhub.inited) return kIOReturnSuccess;
    HubContext &h = gmc.mmhub;

    // ---- CONTEXT0/1 page-table-base / start / end ----
    // mmhub_4_1_0_offset.h:1094-1101  (PAGE_TABLE_BASE_ADDR)
    // mmhub_4_1_0_offset.h:1158-1163  (PAGE_TABLE_START_ADDR)
    // mmhub_4_1_0_offset.h:1222-1227  (PAGE_TABLE_END_ADDR)
    h.ctx0_pt_base_lo  = 0x05cf;
    h.ctx0_pt_base_hi  = 0x05d0;
    h.ctx0_pt_start_lo = 0x05ef;
    h.ctx0_pt_start_hi = 0x05f0;
    h.ctx0_pt_end_lo   = 0x060f;
    h.ctx0_pt_end_hi   = 0x0610;
    h.ctx1_pt_start_lo = 0x05f1;
    h.ctx1_pt_start_hi = 0x05f2;
    h.ctx1_pt_end_lo   = 0x0611;
    h.ctx1_pt_end_hi   = 0x0612;

    // ---- CONTEXT[N]_CNTL ----
    // mmhub_4_1_0_offset.h:880-882
    h.ctx0_cntl = 0x0564;
    h.ctx1_cntl = 0x0565;

    // ---- L2 cache control ----
    // mmhub_4_1_0_offset.h:730-734 + 778 + 790
    h.vm_l2_cntl  = 0x04e4;
    h.vm_l2_cntl2 = 0x04e5;
    h.vm_l2_cntl3 = 0x04e6;
    h.vm_l2_cntl4 = 0x04fd;
    h.vm_l2_cntl5 = 0x0503;

    // ---- Protection-fault CNTL + default addr ----
    // mmhub_4_1_0_offset.h:746-765
    h.vm_l2_protection_fault_cntl              = 0x04ec;
    h.vm_l2_protection_fault_cntl2             = 0x04ed;
    h.vm_l2_protection_fault_default_addr_lo32 = 0x04f4;
    h.vm_l2_protection_fault_default_addr_hi32 = 0x04f5;

    // ---- Identity aperture + offset ----
    // mmhub_4_1_0_offset.h:766-777
    h.vm_l2_ctx1_identity_aperture_low_lo32   = 0x04f7;
    h.vm_l2_ctx1_identity_aperture_low_hi32   = 0x04f8;
    h.vm_l2_ctx1_identity_aperture_high_lo32  = 0x04f9;
    h.vm_l2_ctx1_identity_aperture_high_hi32  = 0x04fa;
    h.vm_l2_ctx_identity_physical_offset_lo32 = 0x04fb;
    h.vm_l2_ctx_identity_physical_offset_hi32 = 0x04fc;

    // ---- TLB / aperture ----
    // mmhub_4_1_0_offset.h:870-874
    h.vm_l1_tlb_cntl                       = 0x055b;
    h.vm_system_aperture_low_addr          = 0x0559;
    h.vm_system_aperture_high_addr         = 0x055a;
    // mmhub_4_1_0_offset.h:692-695
    h.vm_system_aperture_default_addr_lo   = 0x04c8;
    h.vm_system_aperture_default_addr_hi   = 0x04c9;

    // ---- AGP / FB location ----
    // mmhub_4_1_0_offset.h:860-869 + 690
    h.vm_agp_top           = 0x0556;
    h.vm_agp_bot           = 0x0557;
    h.vm_agp_base          = 0x0558;
    h.vm_fb_location_base  = 0x0554;   // read-only here
    h.vm_fb_location_top   = 0x0555;   // read-only here
    h.vm_fb_offset         = 0x04c7;   // read-only here (reads 0 on this card)

    // ---- Invalidation engines ----
    // mmhub_4_1_0_offset.h:914 / 950 / 986 / 1022-1024
    h.vm_invalidate_eng0_req             = 0x0587;
    h.vm_invalidate_eng0_ack             = 0x0599;
    h.vm_invalidate_eng0_sem             = 0x0575;
    h.vm_invalidate_eng0_addr_range_lo32 = 0x05ab;
    h.vm_invalidate_eng0_addr_range_hi32 = 0x05ac;

    // ---- Strides ----  (audit #4 F6)
    // mmhub_v4_1_0.c:487-493
    //   ctx_distance      = regMMVM_CONTEXT1_CNTL                         - regMMVM_CONTEXT0_CNTL
    //                     = 0x0565 - 0x0564 = 1
    //   ctx_addr_distance = regMMVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32    - regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32
    //                     = 0x05d1 - 0x05cf = 2
    //   eng_distance      = regMMVM_INVALIDATE_ENG1_REQ                   - regMMVM_INVALIDATE_ENG0_REQ
    //                     = 0x0588 - 0x0587 = 1
    //   eng_addr_distance = regMMVM_INVALIDATE_ENG1_ADDR_RANGE_LO32       - regMMVM_INVALIDATE_ENG0_ADDR_RANGE_LO32
    //                     = 0x05ad - 0x05ab = 2
    h.ctx_distance      = 1;
    h.ctx_addr_distance = 2;
    h.eng_distance      = 1;
    h.eng_addr_distance = 2;

    h.ip = IPBlock::MMHUB;   // MMHUB has its own IP entry in the discovery table
    // Every regMMVM_* / regMMMC_* above carries _BASE_IDX 0 in
    // mmhub_4_1_0_offset.h — verified register by register.
    h.base_idx = 0;
    h.inited = true;
    GMC_LOG("mmhub v4_1_0 offsets initialised (base IP=%u BASE_IDX=%d, "
            "ctx_dist=%u eng_dist=%u eng_addr_dist=%u)",
            (unsigned)h.ip, h.base_idx, (unsigned)h.ctx_distance,
            (unsigned)h.eng_distance, (unsigned)h.eng_addr_distance);
    return kIOReturnSuccess;
}

// ----- GFXHUB v12_0 offsets -----
//
// Source: gfxhub_v12_0.c + gc/gc_12_0_0_offset.h. GFXHUB sits in the
// GC IP block (its registers are part of the GC register window).
//
// Audit #4 F4-F5: previous offsets were from the wrong IP gen.
// Every offset below cites the exact gc_12_0_0_offset.h line number.
kern_return_t
gmc_gfxhub_offsets_init(GMCContext &gmc)
{
    if (gmc.gfxhub.inited) return kIOReturnSuccess;
    HubContext &h = gmc.gfxhub;

    // ---- CONTEXT0/1 PT base / start / end ----
    // gc_12_0_0_offset.h:3106-3110 (PT_BASE_ADDR)
    // gc_12_0_0_offset.h:3170-3172 (PT_START_ADDR)
    // gc_12_0_0_offset.h:3234-3236 (PT_END_ADDR)
    h.ctx0_pt_base_lo  = 0x168f;
    h.ctx0_pt_base_hi  = 0x1690;
    h.ctx0_pt_start_lo = 0x16af;
    h.ctx0_pt_start_hi = 0x16b0;
    h.ctx0_pt_end_lo   = 0x16cf;
    h.ctx0_pt_end_hi   = 0x16d0;
    // CONTEXT1 PT regs are CONTEXT0 + ctx_addr_distance (=2).
    h.ctx1_pt_start_lo = 0x16b1;
    h.ctx1_pt_start_hi = 0x16b2;
    h.ctx1_pt_end_lo   = 0x16d1;
    h.ctx1_pt_end_hi   = 0x16d2;

    // ---- CONTEXT[N]_CNTL ----
    // gc_12_0_0_offset.h:2892-2894
    h.ctx0_cntl = 0x1624;
    h.ctx1_cntl = 0x1625;

    // ---- L2 cache control ----
    // gc_12_0_0_offset.h:2766-2770 + 2814 + 2826
    h.vm_l2_cntl  = 0x15c4;
    h.vm_l2_cntl2 = 0x15c5;
    h.vm_l2_cntl3 = 0x15c6;
    h.vm_l2_cntl4 = 0x15dd;
    h.vm_l2_cntl5 = 0x15e3;

    // ---- Protection-fault CNTL + default addr ----
    // gc_12_0_0_offset.h:2782-2800
    h.vm_l2_protection_fault_cntl              = 0x15cc;
    h.vm_l2_protection_fault_cntl2             = 0x15cd;
    h.vm_l2_protection_fault_default_addr_lo32 = 0x15d4;
    h.vm_l2_protection_fault_default_addr_hi32 = 0x15d5;

    // ---- Identity aperture + offset ----
    // gc_12_0_0_offset.h:2802-2813
    h.vm_l2_ctx1_identity_aperture_low_lo32   = 0x15d7;
    h.vm_l2_ctx1_identity_aperture_low_hi32   = 0x15d8;
    h.vm_l2_ctx1_identity_aperture_high_lo32  = 0x15d9;
    h.vm_l2_ctx1_identity_aperture_high_hi32  = 0x15da;
    h.vm_l2_ctx_identity_physical_offset_lo32 = 0x15db;
    h.vm_l2_ctx_identity_physical_offset_hi32 = 0x15dc;

    // ---- TLB / aperture ----
    // gc_12_0_0_offset.h:2882-2886
    h.vm_l1_tlb_cntl                       = 0x161b;
    h.vm_system_aperture_low_addr          = 0x1619;
    h.vm_system_aperture_high_addr         = 0x161a;
    // gc_12_0_0_offset.h:2728-2730
    h.vm_system_aperture_default_addr_lo   = 0x15a8;
    h.vm_system_aperture_default_addr_hi   = 0x15a9;

    // ---- AGP / FB location ----
    // gc_12_0_0_offset.h:2872-2880 + 2726
    h.vm_agp_top           = 0x1616;
    h.vm_agp_bot           = 0x1617;
    h.vm_agp_base          = 0x1618;
    h.vm_fb_location_base  = 0x1614;   // read-only here
    h.vm_fb_location_top   = 0x1615;   // read-only here
    h.vm_fb_offset         = 0x15a7;   // read-only here

    // ---- Invalidation engines ----
    // gc_12_0_0_offset.h:2926 + 2962 + 2998 + 3034-3036
    h.vm_invalidate_eng0_req             = 0x1647;
    h.vm_invalidate_eng0_ack             = 0x1659;
    h.vm_invalidate_eng0_sem             = 0x1635;
    h.vm_invalidate_eng0_addr_range_lo32 = 0x166b;
    h.vm_invalidate_eng0_addr_range_hi32 = 0x166c;

    // ---- Strides ----  (audit #4 F6)
    // gfxhub_v12_0.c:494-500:
    //   ctx_distance      = regGCVM_CONTEXT1_CNTL                         - regGCVM_CONTEXT0_CNTL
    //                     = 0x1625 - 0x1624 = 1
    //   ctx_addr_distance = regGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32    - regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32
    //                     = 0x1691 - 0x168f = 2
    //   eng_distance      = regGCVM_INVALIDATE_ENG1_REQ                   - regGCVM_INVALIDATE_ENG0_REQ
    //                     = 0x1648 - 0x1647 = 1
    //   eng_addr_distance = regGCVM_INVALIDATE_ENG1_ADDR_RANGE_LO32       - regGCVM_INVALIDATE_ENG0_ADDR_RANGE_LO32
    //                     = 0x166d - 0x166b = 2
    h.ctx_distance      = 1;
    h.ctx_addr_distance = 2;
    h.eng_distance      = 1;
    h.eng_addr_distance = 2;

    h.ip = IPBlock::GC;
    // Every regGCVM_* / regGCMC_* above carries _BASE_IDX 0 in
    // gc_12_0_0_offset.h — verified register by register. GFXHUB is the
    // one GC consumer that genuinely lives entirely in segment 0, which
    // is why it worked while the BASE_IDX-1 registers elsewhere did not.
    h.base_idx = 0;
    h.inited = true;
    GMC_LOG("gfxhub v12_0 offsets initialised (base IP=%u BASE_IDX=%d, "
            "ctx_dist=%u eng_dist=%u eng_addr_dist=%u)",
            (unsigned)h.ip, h.base_idx, (unsigned)h.ctx_distance,
            (unsigned)h.eng_distance, (unsigned)h.eng_addr_distance);
    return kIOReturnSuccess;
}

// ============================================================
// Resource allocation: GART page table + dummy_page + mem_scratch.
//
// GART page table size = gart_size / AMDGPU_GPU_PAGE_SIZE * 8.
// 256 MiB / 4 KiB = 65536 PTEs * 8 B = 512 KiB, VRAM-resident at
// dev.vramBase + kGMCGartPTVRAMOffset.
//
// dummy_page / mem_scratch are DMA-able system memory — the reference's
// IOBufferMemoryDescriptor + IODMACommand pairs become SysMem
// (Deviation 2). No IOMMU, so SysMem.bus is the host physical address.
// Both are kAMDGPUGPUPageSize (4 KB) here; the reference used kASPageSize
// (16 KB) only because DART rejects anything smaller on Apple Silicon.
// ============================================================

kern_return_t
gmc_alloc_resources(DeviceContext &dev, GMCContext &gmc)
{
    if (gmc.gart_pt_bus != 0) return kIOReturnSuccess;
    if (!gmc.inited) return kIOReturnNotReady;

    // GART page table — 4 KB GPU pages (matches upstream
    // AMDGPU_GPU_PAGE_SIZE; the hubs are programmed for a >>12 stride in
    // hub_init_gart_aperture_regs). 8 B PTE.
    // VRAM-resident (mirrors upstream amdgpu_gart_table_vram_alloc).
    // gart_pt_bus holds the MC address of the PT, which lives in the FB
    // aperture so the walker can fetch PT entries directly without going
    // over PCIe. CPU writes use bar0_memcpy_to_vram.
    uint64_t pt_entries = gmc.gart_size / kAMDGPUGPUPageSize;
    uint64_t pt_bytes   = pt_entries * 8;
    if (pt_bytes < kAMDGPUGPUPageSize) pt_bytes = kAMDGPUGPUPageSize;
    pt_bytes = (pt_bytes + kAMDGPUGPUPageSize - 1) &
               ~((uint64_t)kAMDGPUGPUPageSize - 1);
    if (pt_bytes > kGMCGartPTBytes) {
        // The fixed layout reserves exactly kGMCGartPTBytes at
        // dev.vramBase + kGMCGartPTVRAMOffset; growing past it would run
        // into fw_buf at +0x1000000.
        GMC_LOG("alloc_resources: PT needs %llu B but only %u B reserved "
                "at vram+%#llx",
                (unsigned long long)pt_bytes, (unsigned)kGMCGartPTBytes,
                (unsigned long long)kGMCGartPTVRAMOffset);
        return kIOReturnNoSpace;
    }
    gmc.gart_pt_size = pt_bytes;

    const uint64_t pt_vram_off = dev.vramBase + kGMCGartPTVRAMOffset;
    if (pt_vram_off + pt_bytes > dev.vramLimit) {
        GMC_LOG("alloc_resources: PT at vram+%#llx (%llu B) is outside the "
                "BAR0 aperture (limit %#llx)",
                (unsigned long long)pt_vram_off,
                (unsigned long long)pt_bytes,
                (unsigned long long)dev.vramLimit);
        return kIOReturnNoSpace;
    }

    gmc.gart_pt_vram_offset = pt_vram_off;
    gmc.gart_pt_bus = dev.vramMC(pt_vram_off);   // == gmc.vram_start + off
    gmc.gart_pt_cpu = nullptr;                   // CPU writes go via BAR0
    bar0_memset_vram(dev, pt_vram_off, 0, pt_bytes);

    kern_return_t r;

    // dummy_page — destination for protection-fault page redirects.
    r = sysmem_alloc(gmc.dummy_page, kAMDGPUGPUPageSize, kAMDGPUGPUPageSize);
    if (r != kIOReturnSuccess) {
        GMC_LOG("dummy_page alloc failed: %#x", r);
        return r;
    }
    gmc.dummy_page_bus = gmc.dummy_page.bus;

    // mem_scratch — used as default system aperture address.
    r = sysmem_alloc(gmc.mem_scratch, kAMDGPUGPUPageSize, kAMDGPUGPUPageSize);
    if (r != kIOReturnSuccess) {
        GMC_LOG("mem_scratch alloc failed: %#x", r);
        return r;
    }
    gmc.mem_scratch_bus = gmc.mem_scratch.bus;

    GMC_LOG("resources: gart_pt mc=%#llx vram+%#llx (%llu B, zeroed) "
            "dummy bus=%#llx scratch bus=%#llx",
            (unsigned long long)gmc.gart_pt_bus,
            (unsigned long long)pt_vram_off,
            (unsigned long long)pt_bytes,
            (unsigned long long)gmc.dummy_page_bus,
            (unsigned long long)gmc.mem_scratch_bus);
    return kIOReturnSuccess;
}

void
gmc_release_resources(GMCContext &gmc)
{
    gmc.vram_alloc_hi.init(0, 0, nullptr);   // drop every hi-pool node
    sysmem_free(gmc.mem_scratch);
    sysmem_free(gmc.dummy_page);
    gmc.gart_pt_bus = 0; gmc.gart_pt_cpu = nullptr; gmc.gart_pt_size = 0;
    gmc.gart_pt_vram_offset = 0;
    gmc.dummy_page_bus = 0; gmc.mem_scratch_bus = 0;
}

// ============================================================
// MMHUB v4_1_0 gart_enable port.
//
// Sub-functions follow upstream mmhub_v4_1_0.c structure 1:1.
// Each is parameterized by the GMCContext so they can be reused
// by the GFXHUB twin (since the field layout is identical, just
// the register addresses change — see GCVM_* → MMVM_* aliases in
// amdgpu_field_defs.h).
// ============================================================

// init_gart_aperture_regs: write CONTEXT0 PT base, start, end.
//
// `usePDB0` is the Navi48Bringup B' path and is ONLY ever true for GFXHUB:
// the CONTEXT0 base becomes the two-level PDB0 and the range starts at 0 so
// Apple's 0-based VRAM addresses fall inside it. Upstream's own pdb0 hub
// (gfxhub_v1_2_xcc_init_gart_aperture_regs) does exactly this — PT base from
// the pdb0 BO, START from fb_start (which amdgpu_gmc_sysvm_location sets to
// the hive's 0), END still gart_end.
static void
hub_init_gart_aperture_regs(const DeviceContext &dev,
                            const GMCContext &gmc, const HubContext &h,
                            bool usePDB0 = false)
{
    // PT base address (split lo/hi). Upstream's amdgpu_gmc_pd_addr ORs
    // AMDGPU_PTE_VALID into the PD base. Our PT lives in VRAM (per
    // upstream amdgpu_gart_table_vram_alloc on GFX12), so we use only
    // VALID — NOT SYSTEM. SYSTEM tells GMC the PT base is a sysmem
    // bus address; for a VRAM-resident PT it would mis-route the walk.
    // Linux amdgpu_gmc_pd_addr → gmc_v12_0_get_vm_pde: a VRAM-resident page
    // directory base is programmed as a VRAM-RELATIVE address,
    //     vram_base_offset + MC - vram_start   (gmc_v12_0.c:488-490),
    // not as the MC address. Writing the MC address (0x8000f00000) made the
    // walker fetch the table from an impossible VRAM offset → WALKER_ERROR +
    // MAPPING_ERROR on every GART access, stage 13/15 runs).
    const uint64_t pt_pa = usePDB0
        ? gmc.pdb0_pa
        : (gmc.vram_base_offset + gmc.gart_pt_bus - gmc.vram_start);
    uint64_t pt = pt_pa | PTEFlags::VALID;
    const uint64_t pt_start = usePDB0 ? 0ULL : gmc.gart_start;
    const uint32_t base_lo_reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx0_pt_base_lo);
    const uint32_t base_hi_reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx0_pt_base_hi);
    const uint32_t st_lo_reg   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx0_pt_start_lo);
    const uint32_t st_hi_reg   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx0_pt_start_hi);
    const uint32_t en_lo_reg   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx0_pt_end_lo);
    const uint32_t en_hi_reg   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx0_pt_end_hi);
    const uint32_t old_base_lo = RREG32(dev, base_lo_reg);
    const uint32_t old_base_hi = RREG32(dev, base_hi_reg);
    const uint32_t old_st_lo   = RREG32(dev, st_lo_reg);
    const uint32_t old_en_lo   = RREG32(dev, en_lo_reg);
    GMC_LOG("setup_vm_pt_regs(ip=%u%s): PT mc=%#llx -> pd_addr=%#llx (vram_start=%#llx vram_base_offset=%#llx)",
            (unsigned)h.ip, usePDB0 ? " PDB0/depth1" : "",
            (unsigned long long)(usePDB0 ? gmc.pdb0_mc : gmc.gart_pt_bus),
            (unsigned long long)pt,
            (unsigned long long)gmc.vram_start, (unsigned long long)gmc.vram_base_offset);
    WREG32(dev, base_lo_reg, (uint32_t)(pt & 0xFFFFFFFFu));
    WREG32(dev, base_hi_reg, (uint32_t)(pt >> 32));

    // PT start/end: encoded as page-frame numbers, with the
    // high half being the upper bits of the 44-bit VA.
    WREG32(dev, st_lo_reg, (uint32_t)(pt_start >> 12));
    WREG32(dev, st_hi_reg, (uint32_t)(pt_start >> 44));

    WREG32(dev, en_lo_reg, (uint32_t)(gmc.gart_end >> 12));
    WREG32(dev, en_hi_reg, (uint32_t)(gmc.gart_end >> 44));

    GMC_LOG("setup_vm_pt_regs(ip=%u): BASE_LO32 %#010x->%#010x BASE_HI32 %#010x->%#010x "
            "START_LO32 %#010x->%#010x END_LO32 %#010x->%#010x | readback BASE=%#010x:%#010x "
            "START=%#010x:%#010x END=%#010x:%#010x",
            (unsigned)h.ip,
            old_base_lo, (unsigned)(pt & 0xFFFFFFFFu),
            old_base_hi, (unsigned)(pt >> 32),
            old_st_lo,   (unsigned)(pt_start >> 12),
            old_en_lo,   (unsigned)(gmc.gart_end >> 12),
            RREG32(dev, base_hi_reg), RREG32(dev, base_lo_reg),
            RREG32(dev, st_hi_reg),   RREG32(dev, st_lo_reg),
            RREG32(dev, en_hi_reg),   RREG32(dev, en_lo_reg));
}

// init_system_aperture_regs — port of:
//   mmhub_v4_1_0_init_system_aperture_regs  (mmhub_v4_1_0.c:152)
//   gfxhub_v12_0_init_system_aperture_regs  (gfxhub_v12_0.c:158)
// Programs AGP base/top/bot, system aperture low/high, default addr,
// protection-fault default addr, and PFCNTL2 PTE_READ_RETRY bit.
//
// Audit #4 F21/F22 fix: previously the protection-fault default addr
// registers were never programmed; now they are pointed at dummy_page.
//
// `skipAGP` is the Navi48Bringup display gate (RISKY #3) — only ever
// true for MMHUB, and only when gmc.skipDisplayRisky is set.
static void
hub_init_system_aperture_regs(const DeviceContext &dev,
                              const GMCContext &gmc, const HubContext &h,
                              bool skipAGP)
{
    // Program the AGP BAR: base = 0, bot = agp_start>>24, top = agp_end>>24
    if (!skipAGP) {
        WREG32(dev, SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_agp_base), 0);
        WREG32(dev, SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_agp_bot),
               (uint32_t)(gmc.agp_start >> 24));
        WREG32(dev, SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_agp_top),
               (uint32_t)(gmc.agp_end >> 24));
    } else {
        GMC_LOG("skipDisplayRisky: leaving AGP_BASE/BOT/TOP as the VBIOS "
                "left them (ip=%u)", (unsigned)h.ip);
    }

    // System aperture low/high = min(fb_start, agp_start) >> 18, ditto high.
    //
    // THIS is what keeps the live console alive: fb_start/fb_end come from
    // MMHUB FB_LOCATION_BASE, so the scanout range stays inside the
    // passthrough aperture. See the "Display risk" note at the top.
    uint64_t aperture_low  = gmc.fb_start;
    uint64_t aperture_high = gmc.fb_end;
    if (gmc.agp_start < aperture_low)  aperture_low  = gmc.agp_start;
    if (gmc.agp_end   > aperture_high) aperture_high = gmc.agp_end;

    WREG32(dev, SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_system_aperture_low_addr),
           (uint32_t)(aperture_low >> 18));
    WREG32(dev, SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_system_aperture_high_addr),
           (uint32_t)(aperture_high >> 18));

    // Default page address (mem_scratch as system-aperture default).
    // Upstream computes this against a VRAM-resident mem_scratch; the
    // reference (and we) keep mem_scratch in system memory, so the
    // subtraction wraps — see Deviation 9. Kept verbatim, logged.
    uint64_t def = gmc.mem_scratch_bus - gmc.vram_start
                 + gmc.vram_base_offset;
    WREG32(dev, SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx,
                                 h.vm_system_aperture_default_addr_lo),
           (uint32_t)(def >> 12));
    WREG32(dev, SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx,
                                 h.vm_system_aperture_default_addr_hi),
           (uint32_t)(def >> 44));

    // Protection-fault default address — dummy_page  (F21/F22).
    // mmhub_v4_1_0.c:185-188 / gfxhub_v12_0.c:182-185.
    WREG32(dev,
           SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx,
                            h.vm_l2_protection_fault_default_addr_lo32),
           (uint32_t)(gmc.dummy_page_bus >> 12));
    WREG32(dev,
           SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx,
                            h.vm_l2_protection_fault_default_addr_hi32),
           (uint32_t)(gmc.dummy_page_bus >> 44));

    // PFCNTL2 ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY = 1
    // mmhub_v4_1_0.c:190-193 / gfxhub_v12_0.c:187-188.
    uint32_t pfc2_reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx,
                                         h.vm_l2_protection_fault_cntl2);
    uint32_t pfc2 = RREG32(dev, pfc2_reg);
    pfc2 = REG_SET_FIELD(pfc2, MMVM_L2_PROTECTION_FAULT_CNTL2,
                         ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY, 1);
    WREG32(dev, pfc2_reg, pfc2);

    GMC_LOG("init_system_aperture(ip=%u): aperture=[%#llx..%#llx] "
            "(>>18: %#x..%#x) default_addr=%#llx pf_default=%#llx "
            "pfcntl2=%#x",
            (unsigned)h.ip,
            (unsigned long long)aperture_low,
            (unsigned long long)aperture_high,
            (unsigned)(aperture_low >> 18), (unsigned)(aperture_high >> 18),
            (unsigned long long)def,
            (unsigned long long)gmc.dummy_page_bus,
            (unsigned)pfc2);
}

// init_tlb_regs — enable L1 TLB, MTYPE=UC, advanced driver model.
// Port of mmhub_v4_1_0_init_tlb_regs (mmhub_v4_1_0.c:196) and
//          gfxhub_v12_0_init_tlb_regs (gfxhub_v12_0.c:192).
static void
hub_init_tlb_regs(const DeviceContext &dev, const HubContext &h)
{
    uint32_t reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_l1_tlb_cntl);
    uint32_t tmp = RREG32(dev, reg);
    const uint32_t before = tmp;
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB, 1);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, SYSTEM_ACCESS_MODE, 3);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL,
                        ENABLE_ADVANCED_DRIVER_MODEL, 1);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL,
                        SYSTEM_APERTURE_UNMAPPED_ACCESS, 0);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, ECO_BITS, 0);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, MTYPE, MMHUB_MTYPE_UC);
    WREG32(dev, reg, tmp);
    GMC_LOG("init_tlb(ip=%u): MX_L1_TLB_CNTL %#x -> %#x",
            (unsigned)h.ip, (unsigned)before, (unsigned)tmp);
}

// disable_identity_aperture — port of
//   mmhub_v4_1_0_disable_identity_aperture (mmhub_v4_1_0.c:279)
//   gfxhub_v12_0_disable_identity_aperture (gfxhub_v12_0.c:275)
// Closes the identity aperture so CONTEXT1 access is fully translated.
// Constants from upstream: low=0xFFFFFFFF/0xF, high=0/0, offset=0/0.
static void
hub_disable_identity_aperture(const DeviceContext &dev, const HubContext &h)
{
    WREG32(dev,
           SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_l2_ctx1_identity_aperture_low_lo32),
           0xFFFFFFFFu);
    WREG32(dev,
           SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_l2_ctx1_identity_aperture_low_hi32),
           0x0000000Fu);

    WREG32(dev,
           SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_l2_ctx1_identity_aperture_high_lo32),
           0);
    WREG32(dev,
           SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_l2_ctx1_identity_aperture_high_hi32),
           0);

    WREG32(dev,
           SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_l2_ctx_identity_physical_offset_lo32),
           0);
    WREG32(dev,
           SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_l2_ctx_identity_physical_offset_hi32),
           0);
}

// init_cache_regs — L2 cache control. Port of
//   mmhub_v4_1_0_init_cache_regs   (mmhub_v4_1_0.c:216)
//   gfxhub_v12_0_init_cache_regs   (gfxhub_v12_0.c:212)
//
// CNTL3/4/5 are programmed from per-IP DEFAULT seeds, then have a few
// fields overridden via REG_SET_FIELD — that matches upstream exactly.
// MMHUB and GFXHUB use different DEFAULT constants:
//   MMHUB CNTL3 default = 0x80100007
//   GFXHUB CNTL3 default = 0x80120007
//   both CNTL4 default   = 0x000000c1
//   both CNTL5 default   = 0x00003fe0
static void
hub_init_cache_regs(const DeviceContext &dev, const HubContext &h)
{
    uint32_t reg, tmp;

    // ---- L2_CNTL ----
    reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_l2_cntl);
    tmp = RREG32(dev, reg);
    const uint32_t l2cntl_before = tmp;
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, ENABLE_L2_CACHE, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, ENABLE_L2_FRAGMENT_PROCESSING, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL,
                        ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL,
                        L2_PDE0_CACHE_TAG_GENERATION_MODE, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, PDE_FAULT_CLASSIFICATION, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, CONTEXT1_IDENTITY_ACCESS_MODE, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, IDENTITY_MODE_FRAGMENT_SIZE, 0);
    WREG32(dev, reg, tmp);

    // ---- L2_CNTL2 ----
    reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_l2_cntl2);
    tmp = RREG32(dev, reg);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL2, INVALIDATE_L2_CACHE, 1);
    WREG32(dev, reg, tmp);

    // ---- L2_CNTL3 ----
    // Upstream regMMVM_L2_CNTL3_DEFAULT  = 0x80100007 (MMHUB)
    //          regGCVM_L2_CNTL3_DEFAULT  = 0x80120007 (GFXHUB)
    // translate_further == false → BANK_SELECT=9, BIGK_FRAGMENT=6.
    const uint32_t kCNTL3_DEFAULT = (h.ip == IPBlock::GC)
                                  ? 0x80120007u   // GFXHUB
                                  : 0x80100007u;  // MMHUB
    reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_l2_cntl3);
    tmp = kCNTL3_DEFAULT;
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL3, BANK_SELECT, 9);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL3, L2_CACHE_BIGK_FRAGMENT_SIZE, 6);
    WREG32(dev, reg, tmp);

    // ---- L2_CNTL4 ----
    // Upstream regMMVM_L2_CNTL4_DEFAULT = 0x000000c1.
    // Audit #4 F12 fix: VMC_TAP_PDE/PTE_REQUEST_PHYSICAL shifts were
    // wrong in the field-defs (0x1/0x2 → 0x6/0x7).
    constexpr uint32_t kCNTL4_DEFAULT = 0x000000c1u;
    reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_l2_cntl4);
    tmp = kCNTL4_DEFAULT;
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL4, VMC_TAP_PDE_REQUEST_PHYSICAL, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL4, VMC_TAP_PTE_REQUEST_PHYSICAL, 0);
    WREG32(dev, reg, tmp);

    // ---- L2_CNTL5 ----
    // Upstream regMMVM_L2_CNTL5_DEFAULT = 0x00003fe0.
    constexpr uint32_t kCNTL5_DEFAULT = 0x00003fe0u;
    reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_l2_cntl5);
    tmp = kCNTL5_DEFAULT;
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL5, L2_CACHE_SMALLK_FRAGMENT_SIZE, 0);
    WREG32(dev, reg, tmp);

    GMC_LOG("init_cache(ip=%u): L2_CNTL %#x -> %#x, CNTL3=%#x (seed %#x)",
            (unsigned)h.ip, (unsigned)l2cntl_before,
            (unsigned)RREG32(dev, SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_l2_cntl)),
            (unsigned)RREG32(dev, SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.vm_l2_cntl3)),
            (unsigned)kCNTL3_DEFAULT);
}

// enable_system_domain: turn on CONTEXT0 with PT depth 0 (no multi-level
// PT for CONTEXT0 — it covers the system/FB aperture plus a flat GART).
//
// `depth`/`blockSize` default to the reference values (0/0). The GFXHUB B'
// path passes 1/12, which is what upstream gmc_v12_0_gart_init selects for a
// pdb0 device (gmc_v12_0.c:781-782: vmid0_page_table_depth = 1,
// vmid0_page_table_block_size = 12). Field positions are
// GCVM_CONTEXT0_CNTL__PAGE_TABLE_DEPTH__SHIFT 0x1 (mask 0x6) and
// __PAGE_TABLE_BLOCK_SIZE__SHIFT 0x4 (mask 0xF0), gc_12_0_0_sh_mask.h:9382-9383
// and 9401-9402 — the MMVM_* spellings in amdgpu_field_defs.h carry the same
// shifts. NOTE the CONTEXT0 field takes the RAW block size (12), unlike
// CONTEXT1..15 in hub_setup_vmid_config which takes block_size - 9.
// Every other bit (the protection-fault enables) is left exactly as found.
static void
hub_enable_system_domain(const DeviceContext &dev, const HubContext &h,
                         uint32_t depth = 0, uint32_t blockSize = 0,
                         bool writeBlockSize = false)
{
    uint32_t reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx0_cntl);
    uint32_t tmp = RREG32(dev, reg);
    const uint32_t before = tmp;
    tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT0_CNTL, ENABLE_CONTEXT, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT0_CNTL, PAGE_TABLE_DEPTH, depth);
    // Only the B' path touches PAGE_TABLE_BLOCK_SIZE. With depth 0 upstream
    // leaves the field alone, and so must we: this same function programs the
    // MMHUB CONTEXT0 the live scanout depends on.
    if (writeBlockSize)
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT0_CNTL, PAGE_TABLE_BLOCK_SIZE, blockSize);
    tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT0_CNTL,
                        RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 0);
    WREG32(dev, reg, tmp);
    GMC_LOG("enable_system_domain(ip=%u depth=%u block_size=%u): CONTEXT0_CNTL "
            "%#010x -> %#010x (readback %#010x)",
            (unsigned)h.ip, (unsigned)depth, (unsigned)blockSize,
            (unsigned)before, (unsigned)tmp, (unsigned)RREG32(dev, reg));
}

// setup_vmid_config — per-VMID CONTEXT1..15 enable with fault bits.
// Port of:
//   mmhub_v4_1_0_setup_vmid_config  (mmhub_v4_1_0.c:305)
//   gfxhub_v12_0_setup_vmid_config  (gfxhub_v12_0.c:298)
//
// Audit #4 F16: the original implementation only programmed CONTEXT_CNTL
// (and got the bit positions wrong via hand-coded shifts in
// amdgpu_gart.cpp). This matches upstream:
//   - Uses REG_SET_FIELD with the corrected MMVM_CONTEXT1_CNTL field
//     defs (RANGE=0xb DUMMY=0xd PDE0=0xf VALID=0x11 READ=0x13 WRITE=0x15
//     EXECUTE=0x17, PAGE_TABLE_BLOCK_SIZE=0x4, RETRY=0x8).
//   - Programs CONTEXT[i+1]_PAGE_TABLE_START/END with 0 / (max_pfn-1).
//   - Uses ctx_addr_distance (=2) for PT addr regs and ctx_distance (=1)
//     for the CNTL reg — these are different strides on RDNA4.
//
// 0.0.193 (the reviewer — `appleCtxGeometry`.
//
// CONTEXT1..15 on GFXHUB are used by EXACTLY ONE client on this machine:
// Apple's AMDRadeonX6000, which puts its per-process blit VM on VMID 2. Every
// queue this driver owns (our MQDs, SDMA0 QUEUE0/QUEUE1, the KIQ, the GART)
// runs on VMID 0 = CONTEXT0, which this function never touches.
//
// Apple's table is not shaped like ours. AMDGFX10VMM::init (0xbe1e101) builds a
// tree whose root entry covers 256 MiB, and initializeVmContextCntlRegs
// (0xbe31729) programs PAGE_TABLE_BLOCK_SIZE = log2(256 MiB) - 21 = 7 with
// PAGE_TABLE_DEPTH = 1 — the register write that never runs on our card,
// because Apple emits it as an SRBM_WRITE the engine used to swallow and
// because it is CONTEXT-CNTL, which Apple's SDMA stream never carries.
// Our own values (DEPTH = num_level = 3, BLOCK_SIZE = block_size - 9 = 0,
// i.e. 0x03fffd07, r18/r21) describe a completely different tree, so the walker
// enters Apple's root at the wrong stride. This makes the geometry Apple's on
// the contexts only Apple uses, and leaves MMHUB and CONTEXT0 untouched.
static constexpr uint32_t kAppleCtxPageTableDepth     = 1;   // Apple's DEPTH
static constexpr uint32_t kAppleCtxPageTableBlockSize = 7;   // log2(256 MiB) - 21
static void
hub_setup_vmid_config(const DeviceContext &dev, const GMCContext &gmc,
                      const HubContext &h, bool appleCtxGeometry = false)
{
    uint32_t cntl_base  = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_cntl);
    uint32_t st_lo_base = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_start_lo);
    uint32_t st_hi_base = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_start_hi);
    uint32_t en_lo_base = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_end_lo);
    uint32_t en_hi_base = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_end_hi);

    const uint64_t end_pfn = (gmc.max_pfn > 0) ? (gmc.max_pfn - 1) : 0;

    const uint32_t depth = appleCtxGeometry ? kAppleCtxPageTableDepth
                                            : gmc.num_level;
    const uint32_t block = appleCtxGeometry ? kAppleCtxPageTableBlockSize
                                            : (gmc.block_size - 9);
    if (appleCtxGeometry) {
        GMC_LOG("setup_vmid_config(ip=%u): APPLE CONTEXT GEOMETRY on VMIDs 1-15 "
                "— DEPTH %u / PAGE_TABLE_BLOCK_SIZE %u (expect CNTL 0x03fffd73) "
                "instead of our DEPTH %u / BLOCK %u. CONTEXT0 (VMID 0, where every "
                "queue THIS driver owns runs) and MMHUB are NOT touched; VMIDs "
                "1-15 on GFXHUB are used only by Apple's AMDRadeonX6000.",
                (unsigned)h.ip, depth, block, gmc.num_level,
                (unsigned)(gmc.block_size - 9));
    }

    // `uint32_t i` (reference: `int i`) so the stride multiplications stay
    // unsigned — behaviour-identical, keeps -Wsign-conversion quiet.
    for (uint32_t i = 0; i <= 14; i++) {
        uint32_t cntl_reg = cntl_base + i * h.ctx_distance;
        uint32_t tmp = RREG32(dev, cntl_reg);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL, ENABLE_CONTEXT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            PAGE_TABLE_DEPTH, depth);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            VALID_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            READ_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            PAGE_TABLE_BLOCK_SIZE, block);
        // no-retry on fault (matches upstream's !amdgpu_noretry default = 1).
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 1);
        WREG32(dev, cntl_reg, tmp);

        // Per-VMID PT start = 0, end = max_pfn - 1. Use ctx_addr_distance
        // for these (=2 on RDNA4), NOT ctx_distance.
        WREG32(dev, st_lo_base + i * h.ctx_addr_distance, 0);
        WREG32(dev, st_hi_base + i * h.ctx_addr_distance, 0);
        WREG32(dev, en_lo_base + i * h.ctx_addr_distance,
               (uint32_t)(end_pfn & 0xFFFFFFFFu));
        WREG32(dev, en_hi_base + i * h.ctx_addr_distance,
               (uint32_t)(end_pfn >> 32));
    }
}

// program_invalidation — arm invalidation engines 0..17 over the full
// address range. Port of:
//   mmhub_v4_1_0_program_invalidation (mmhub_v4_1_0.c:355)
//   gfxhub_v12_0_program_invalidation (gfxhub_v12_0.c:347)
//
// Audit #4 F6: must use eng_addr_distance (=2) for the ADDR_RANGE
// regs, NOT eng_distance (=1). Previously a single combined stride of
// 4 was used, which was wrong for both.
static void
hub_program_invalidation(const DeviceContext &dev, const HubContext &h)
{
    uint32_t lo_base = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx,
                                        h.vm_invalidate_eng0_addr_range_lo32);
    uint32_t hi_base = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx,
                                        h.vm_invalidate_eng0_addr_range_hi32);
    for (uint32_t i = 0; i < 18; i++) {   // reference: `int i`, see above
        WREG32(dev, lo_base + i * h.eng_addr_distance, 0xFFFFFFFFu);
        WREG32(dev, hi_base + i * h.eng_addr_distance, 0x1Fu);
    }
}

kern_return_t
gmc_mmhub_gart_enable(DeviceContext &dev, GMCContext &gmc)
{
    if (!gmc.mmhub.inited) return kIOReturnNotReady;
    if (gmc.gart_pt_bus == 0) return kIOReturnNotReady;
    if (!dev.ip.isResolved(gmc.mmhub.ip)) {
        GMC_LOG("mmhub_gart_enable: IP base not resolved (block=%u)",
                (unsigned)gmc.mmhub.ip);
        return kIOReturnNotReady;
    }

    const bool skip = gmc.skipDisplayRisky;
    if (skip) {
        GMC_LOG("mmhub_gart_enable: skipDisplayRisky=1 — AGP regs, "
                "MX_L1_TLB_CNTL and the L2_CNTL* block will NOT be written "
                "(the UEFI console is scanning out through this hub). "
                "DIAGNOSTIC MODE: MMHUB keeps the VBIOS L2/TLB config, so "
                "GART translation through MMHUB may not work — use this to "
                "find out whether the console survives the safe subset");
    }

    // Order mirrors mmhub_v4_1_0_gart_enable (mmhub_v4_1_0.c:368).
    hub_init_gart_aperture_regs(dev, gmc, gmc.mmhub);
    hub_init_system_aperture_regs(dev, gmc, gmc.mmhub, /*skipAGP*/ skip);
    if (!skip) hub_init_tlb_regs(dev, gmc.mmhub);        // display RISKY #1
    if (!skip) hub_init_cache_regs(dev, gmc.mmhub);      // display RISKY #2
    hub_enable_system_domain(dev, gmc.mmhub);
    hub_disable_identity_aperture(dev, gmc.mmhub);
    hub_setup_vmid_config(dev, gmc, gmc.mmhub);
    hub_program_invalidation(dev, gmc.mmhub);

    gmc.mmhub.gart_enabled = true;
    GMC_LOG("mmhub_gart_enable done (pt=%#llx gart=[%#llx..%#llx])",
            (unsigned long long)gmc.gart_pt_bus,
            (unsigned long long)gmc.gart_start,
            (unsigned long long)gmc.gart_end);
    return kIOReturnSuccess;
}

// ============================================================
// GFXHUB VMID-0 PDB0 — option B' (an internal review note Q3).
//
// Builds the one-page, 5-entry PDB0 described in amdgpu_gmc.h. Structure and
// flags follow amdgpu_gmc_init_pdb0 (ref/linux-amdgpu/amdgpu_gmc.c:1140-1181)
// with the entry INDICES chosen for this card rather than for an XGMI hive:
// upstream puts the VRAM leaves at 0..n-1 and the GART PTB at n, because its
// amdgpu_gmc_sysvm_location moves vram_start to 0. We cannot move vram_start
// (the console scans out of MC 0x8000000000 and hub_init_system_aperture_regs
// derives the passthrough aperture from it), so we write BOTH: the 0-based
// alias at 0..n-1 AND the identity at vram_start/2^33 .. +n-1.
//
// The entry index for a VA is VA >> (block_size + 21) with PAGE_TABLE_START
// programmed to 0, so absolute-VA indexing and START-relative indexing are the
// same thing here — which is why the 0-based alias and the identity can coexist
// in one table.
// ============================================================
kern_return_t
gmc_pdb0_build(DeviceContext &dev, GMCContext &gmc)
{
    gmc.pdb0_active = false;
    if (!gmc.inited)          return kIOReturnNotReady;
    if (gmc.gart_pt_bus == 0) return kIOReturnNotReady;

    const uint32_t bs       = gmc.pdb0_block_size;         // 12
    const uint64_t pde_span = 1ULL << (bs + 21);           // 8 GiB

    // ---- geometry guards: refuse (and say why) rather than write a table
    //      whose arithmetic does not hold on this card.
    if (bs != 12 || pde_span != 0x200000000ULL) {
        GMC_LOG("pdb0: REFUSING — block_size=%u gives a %llu MiB leaf, expected "
                "12 / 8192 MiB", (unsigned)bs, (unsigned long long)(pde_span >> 20));
        return kIOReturnUnsupported;
    }
    if (gmc.vram_start != 0x8000000000ULL) {
        GMC_LOG("pdb0: REFUSING — vram_start is %#llx, not 0x8000000000; the "
                "identity entries would land at the wrong index",
                (unsigned long long)gmc.vram_start);
        return kIOReturnUnsupported;
    }
    if (gmc.gart_start != 0x8400000000ULL) {
        GMC_LOG("pdb0: REFUSING — gart_start is %#llx, not 0x8400000000 "
                "(66 * 2^33); the GART PDE index would be wrong",
                (unsigned long long)gmc.gart_start);
        return kIOReturnUnsupported;
    }
    const uint32_t idx_ident = (uint32_t)(gmc.vram_start / pde_span);   // 64
    const uint32_t idx_gart  = (uint32_t)(gmc.gart_start / pde_span);   // 66
    const uint32_t n_leaves  =
        (uint32_t)((gmc.real_vram_size + pde_span - 1) / pde_span);     // 2
    if (idx_ident != 64 || idx_gart != 66) {
        GMC_LOG("pdb0: REFUSING — computed indices ident=%u gart=%u, expected "
                "64/66", (unsigned)idx_ident, (unsigned)idx_gart);
        return kIOReturnUnsupported;
    }
    if (n_leaves == 0 || idx_ident + n_leaves > idx_gart) {
        GMC_LOG("pdb0: REFUSING — %llu MiB of VRAM needs %u leaves, which would "
                "run the identity block (idx %u) into the GART PDE (idx %u)",
                (unsigned long long)(gmc.real_vram_size >> 20),
                (unsigned)n_leaves, (unsigned)idx_ident, (unsigned)idx_gart);
        return kIOReturnUnsupported;
    }
    if (idx_gart >= kGMCPDB0Entries) {
        GMC_LOG("pdb0: REFUSING — GART PDE index %u does not fit in a %u-entry "
                "page", (unsigned)idx_gart, (unsigned)kGMCPDB0Entries);
        return kIOReturnNoSpace;
    }
    if (gmc.gart_size > pde_span) {
        GMC_LOG("pdb0: REFUSING — the %llu MiB GART does not fit under one "
                "%llu MiB PDE", (unsigned long long)(gmc.gart_size >> 20),
                (unsigned long long)(pde_span >> 20));
        return kIOReturnUnsupported;
    }

    const uint64_t pdb0_vram_off = dev.vramBase + kGMCPDB0VRAMOffset;
    if (pdb0_vram_off + kGMCPDB0Bytes > dev.vramLimit) {
        GMC_LOG("pdb0: REFUSING — PDB0 page at vram+%#llx is outside the BAR0 "
                "aperture (limit %#llx)",
                (unsigned long long)pdb0_vram_off,
                (unsigned long long)dev.vramLimit);
        return kIOReturnNoSpace;
    }
    // Must not overlap the GART page table that precedes it in the fixed layout.
    if (pdb0_vram_off < gmc.gart_pt_vram_offset + gmc.gart_pt_size) {
        GMC_LOG("pdb0: REFUSING — PDB0 at vram+%#llx overlaps the GART PT "
                "[%#llx..%#llx)", (unsigned long long)pdb0_vram_off,
                (unsigned long long)gmc.gart_pt_vram_offset,
                (unsigned long long)(gmc.gart_pt_vram_offset + gmc.gart_pt_size));
        return kIOReturnNoSpace;
    }

    const uint64_t pdb0_mc = dev.vramMC(pdb0_vram_off);
    const uint64_t pdb0_pa = gmc.vram_base_offset + pdb0_mc - gmc.vram_start;
    if ((pdb0_pa & (kAMDGPUGPUPageSize - 1)) != 0) {
        GMC_LOG("pdb0: REFUSING — PDB0 physical %#llx is not 4 KiB aligned",
                (unsigned long long)pdb0_pa);
        return kIOReturnNotAligned;
    }

    // ---- entry values -------------------------------------------------
    // Leaf (PDE used as PTE): amdgpu_gmc_init_pdb0 builds
    //   gart_pte_flags (MTYPE_GFX12(UC) | EXECUTABLE | IS_PTE, gmc_v12_0.c:794-796)
    //   | VALID | READABLE | WRITEABLE | SNOOPED | FRAG(block_size + 9)
    //   | PDE_PTE_GFX12  (which is the same bit 63 as IS_PTE)
    // FRAG(12 + 9) = FRAG(21); MTYPE_GFX12_UC is PTEFlags::MTYPE_GFX12_UC, the
    // same MTYPE every PTE this driver writes already uses.
    const uint64_t leaf_flags = kPDEValid | kPDESnooped | kPDEExecutable |
                                kPDEReadable | kPDEWriteable |
                                kPDEFrag(bs + 9) |
                                PTEFlags::MTYPE_GFX12_UC | kPDEIsPTE;
    // GART PDE: VALID | SNOOPED | BFS(0), bit 63 CLEAR so the walker descends
    // into the flat 65536-entry PTB instead of treating it as a leaf.
    const uint64_t gart_pde_flags = kPDEValid | kPDESnooped;
    const uint64_t gart_ptb_pa =
        gmc.vram_base_offset + gmc.gart_pt_bus - gmc.vram_start;

    // Zero first: entries 2..63, 67..511 must read invalid.
    bar0_memset_vram(dev, pdb0_vram_off, 0, kGMCPDB0Bytes);

    for (uint32_t i = 0; i < n_leaves; i++) {
        const uint64_t pa    = gmc.vram_base_offset + (uint64_t)i * pde_span;
        const uint64_t entry = (pa & kPDEAddrMask) | leaf_flags;
        bar0_memcpy_to_vram(dev, pdb0_vram_off + (uint64_t)i * 8,
                            &entry, sizeof(entry));
        bar0_memcpy_to_vram(dev, pdb0_vram_off + (uint64_t)(idx_ident + i) * 8,
                            &entry, sizeof(entry));
        GMC_LOG("pdb0: entry[%u] = entry[%u] = %#018llx  (VRAM phys %#llx, "
                "%llu MiB leaf; alias MC %#llx, identity MC %#llx)",
                (unsigned)i, (unsigned)(idx_ident + i),
                (unsigned long long)entry, (unsigned long long)pa,
                (unsigned long long)(pde_span >> 20),
                (unsigned long long)((uint64_t)i * pde_span),
                (unsigned long long)(gmc.vram_start + (uint64_t)i * pde_span));
    }

    const uint64_t gart_entry = (gart_ptb_pa & kPDEAddrMask) | gart_pde_flags;
    bar0_memcpy_to_vram(dev, pdb0_vram_off + (uint64_t)idx_gart * 8,
                        &gart_entry, sizeof(gart_entry));
    GMC_LOG("pdb0: entry[%u] = %#018llx  (GART PTB phys %#llx = mc %#llx, bit63 "
            "CLEAR, BFS=0 -> MC %#llx.. walks the flat %u-PTE array)",
            (unsigned)idx_gart, (unsigned long long)gart_entry,
            (unsigned long long)gart_ptb_pa,
            (unsigned long long)gmc.gart_pt_bus,
            (unsigned long long)gmc.gart_start,
            (unsigned)(gmc.gart_size / kAMDGPUGPUPageSize));

    amdgpu_hdp_flush(dev);

    // Read the entries back through the same aperture so the log carries what
    // the walker will actually fetch, not what we intended to write.
    for (uint32_t i = 0; i < n_leaves; i++) {
        GMC_LOG("pdb0: readback entry[%u]=%#018llx entry[%u]=%#018llx",
                (unsigned)i,
                (unsigned long long)RBAR0_64(dev, pdb0_vram_off + (uint64_t)i * 8),
                (unsigned)(idx_ident + i),
                (unsigned long long)RBAR0_64(dev, pdb0_vram_off +
                                             (uint64_t)(idx_ident + i) * 8));
    }
    GMC_LOG("pdb0: readback entry[%u]=%#018llx", (unsigned)idx_gart,
            (unsigned long long)RBAR0_64(dev, pdb0_vram_off +
                                         (uint64_t)idx_gart * 8));

    gmc.pdb0_vram_offset = pdb0_vram_off;
    gmc.pdb0_mc          = pdb0_mc;
    gmc.pdb0_pa          = pdb0_pa;
    gmc.pdb0_idx_ident   = idx_ident;
    gmc.pdb0_idx_gart    = idx_gart;
    gmc.pdb0_vram_leaves = n_leaves;
    gmc.pdb0_active      = true;
    GMC_LOG("pdb0: built at vram+%#llx (mc %#llx, pa %#llx) — %u VRAM leaves at "
            "[0..%u] and [%u..%u], GART PDE at [%u]. MMHUB is NOT touched.",
            (unsigned long long)pdb0_vram_off, (unsigned long long)pdb0_mc,
            (unsigned long long)pdb0_pa, (unsigned)n_leaves,
            (unsigned)(n_leaves - 1), (unsigned)idx_ident,
            (unsigned)(idx_ident + n_leaves - 1), (unsigned)idx_gart);
    return kIOReturnSuccess;
}

void
gmc_pdb0_read_regs(const DeviceContext &dev, const GMCContext &gmc,
                   uint32_t out[7])
{
    const HubContext &h = gmc.gfxhub;
    if (out == nullptr) return;
    for (uint32_t i = 0; i < 7; i++) out[i] = 0;
    if (!h.inited || !dev.ip.isResolved(h.ip)) return;
    const uint32_t regs[7] = {
        h.ctx0_cntl,       // 0x1624
        h.ctx0_pt_base_lo, // 0x168f
        h.ctx0_pt_base_hi, // 0x1690
        h.ctx0_pt_start_lo,// 0x16af
        h.ctx0_pt_start_hi,// 0x16b0
        h.ctx0_pt_end_lo,  // 0x16cf
        h.ctx0_pt_end_hi,  // 0x16d0
    };
    for (uint32_t i = 0; i < 7; i++)
        out[i] = RREG32(dev, SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, regs[i]));
    GMC_LOG("pdb0 regs: CNTL(0x1624)=%#010x BASE(0x168f/0x1690)=%#010x:%#010x "
            "START(0x16af/0x16b0)=%#010x:%#010x END(0x16cf/0x16d0)=%#010x:%#010x "
            "| depth=%u block_size=%u enable=%u",
            out[0], out[2], out[1], out[4], out[3], out[6], out[5],
            (unsigned)((out[0] >> 1) & 0x3u), (unsigned)((out[0] >> 4) & 0xFu),
            (unsigned)(out[0] & 1u));
}

kern_return_t
gmc_pdb0_revert(DeviceContext &dev, GMCContext &gmc)
{
    if (!gmc.gfxhub.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(gmc.gfxhub.ip)) return kIOReturnNotReady;
    GMC_LOG("pdb0: REVERTING GFXHUB CONTEXT0 to the flat single-level GART "
            "table (depth 0, START=%#llx, BASE=the GART PTB)",
            (unsigned long long)gmc.gart_start);
    gmc.pdb0_active = false;
    hub_init_gart_aperture_regs(dev, gmc, gmc.gfxhub, /*usePDB0*/ false);
    hub_enable_system_domain(dev, gmc.gfxhub, /*depth*/ 0, /*blockSize*/ 0,
                             /*writeBlockSize*/ true);
    amdgpu_hdp_flush(dev);
    kern_return_t r = gmc_flush_gpu_tlb(dev, gmc, gmc.gfxhub, /*vmid*/ 0,
                                        /*type*/ 0);
    GMC_LOG("pdb0: revert done (TLB flush %#x)", r);
    return r;
}

kern_return_t
gmc_gfxhub_gart_enable(DeviceContext &dev, GMCContext &gmc)
{
    if (!gmc.gfxhub.inited) return kIOReturnNotReady;
    if (gmc.gart_pt_bus == 0) return kIOReturnNotReady;
    if (!dev.ip.isResolved(gmc.gfxhub.ip)) {
        GMC_LOG("gfxhub_gart_enable: IP base not resolved (block=%u)",
                (unsigned)gmc.gfxhub.ip);
        return kIOReturnNotReady;
    }

    // GFXHUB field layouts mirror MMHUB exactly (see GCVM_* → MMVM_*
    // aliases in amdgpu_field_defs.h); REG_SET_FIELD resolves to the
    // same SHIFT/MASK constants. Order mirrors gfxhub_v12_0_gart_enable
    // (gfxhub_v12_0.c:360).
    //
    // Never gated by skipDisplayRisky: the display does not use GFXHUB.
    //
    // navi48-pdb0=1 (option B'): build the two-level VMID-0 table AFTER the
    // GART page table exists (gmc_alloc_resources ran at GMCInit) and BEFORE
    // CONTEXT0 is enabled below. A refusal leaves usePDB0 false and this
    // function is then byte-for-byte the pre-0.0.183 sequence.
    bool usePDB0 = false;
    if (gmc.pdb0_want) {
        kern_return_t pr = gmc_pdb0_build(dev, gmc);
        usePDB0 = (pr == kIOReturnSuccess) && gmc.pdb0_active;
        GMC_LOG("gfxhub_gart_enable: navi48-pdb0=1 -> PDB0 %s (%#x)",
                usePDB0 ? "ENABLED for VMID-0 CONTEXT0" : "NOT enabled", pr);
    }

    hub_init_gart_aperture_regs(dev, gmc, gmc.gfxhub, usePDB0);
    hub_init_system_aperture_regs(dev, gmc, gmc.gfxhub, /*skipAGP*/ false);
    hub_init_tlb_regs(dev, gmc.gfxhub);
    hub_init_cache_regs(dev, gmc.gfxhub);
    hub_enable_system_domain(dev, gmc.gfxhub,
                             /*depth*/     usePDB0 ? 1u : 0u,
                             /*blockSize*/ usePDB0 ? gmc.pdb0_block_size : 0u,
                             /*writeBlockSize*/ usePDB0);
    hub_disable_identity_aperture(dev, gmc.gfxhub);
    // 0.0.193: GFXHUB VMIDs 1-15 get APPLE's context geometry (DEPTH 1 /
    // BLOCK_SIZE 7). MMHUB's call site above is unchanged.
    hub_setup_vmid_config(dev, gmc, gmc.gfxhub, /*appleCtxGeometry*/ true);
    hub_program_invalidation(dev, gmc.gfxhub);

    gmc.gfxhub.gart_enabled = true;
    GMC_LOG("gfxhub_gart_enable done (pt=%#llx gart=[%#llx..%#llx]%s)",
            (unsigned long long)gmc.gart_pt_bus,
            (unsigned long long)gmc.gart_start,
            (unsigned long long)gmc.gart_end,
            usePDB0 ? ", VMID-0 via PDB0" : "");
    if (usePDB0) {
        uint32_t regs[7];
        gmc_pdb0_read_regs(dev, gmc, regs);
    }
    return kIOReturnSuccess;
}

// gmc_set_fault_enable_default — port of:
//   mmhub_v4_1_0_set_fault_enable_default  (mmhub_v4_1_0.c:415)
//   gfxhub_v12_0_set_fault_enable_default  (gfxhub_v12_0.c:417)
//
// Audit #4 F23: must be called after gart_enable so the VM L2 actually
// redirects faults to the dummy_page we set up.
kern_return_t
gmc_set_fault_enable_default(DeviceContext &dev,
                             const HubContext &hub, bool value)
{
    if (!hub.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(hub.ip)) return kIOReturnNotReady;

    uint32_t reg = SOC15_REG_OFFSET_BIDX(dev, hub.ip, hub.base_idx,
                                    hub.vm_l2_protection_fault_cntl);
    uint32_t tmp = RREG32(dev, reg);
    const uint32_t before = tmp;
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        PDE1_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        PDE2_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        TRANSLATE_FURTHER_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        NACK_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        VALID_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        READ_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    if (!value) {
        tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                            CRASH_ON_NO_RETRY_FAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                            CRASH_ON_RETRY_FAULT, 1);
    }
    WREG32(dev, reg, tmp);
    GMC_LOG("set_fault_enable_default(ip=%u, value=%d): "
            "L2_PROTECTION_FAULT_CNTL %#x -> %#x",
            (unsigned)hub.ip, value ? 1 : 0,
            (unsigned)before, (unsigned)tmp);
    return kIOReturnSuccess;
}

// ----- GMCInit orchestrator entry (the GMC hw_init) -----
kern_return_t
gmc_init(DeviceContext &dev, GMCContext &gmc)
{
    kern_return_t r;
    r = gmc_mc_init(dev, gmc);
    if (r != kIOReturnSuccess) return r;
    r = gmc_vram_alloc_init(dev, gmc);
    if (r != kIOReturnSuccess) return r;
    r = gmc_mmhub_offsets_init(gmc);
    if (r != kIOReturnSuccess) return r;
    r = gmc_gfxhub_offsets_init(gmc);
    if (r != kIOReturnSuccess) return r;
    r = gmc_alloc_resources(dev, gmc);
    if (r != kIOReturnSuccess) return r;

    // MMHUB only — mirrors upstream gmc_v12_0_gart_enable
    // (gmc_v12_0.c:999-1024) which dispatches to mmhub_v4_1_0_gart_enable
    // and NOT to gfxhub. GFXHUB is programmed later by
    // gfxhub_v12_0_gart_enable, called from gfx_v12_0_hw_init
    // (gfx_v12_0.c:3697) AFTER psp_hw_init / psp_load_fw. We do the
    // same — gmc_init only programs MMHUB; call gmc_gfxhub_gart_enable
    // from the GFXInit stage so PSP sees GFXHUB CONTEXT0 at hardware-reset
    // values when it validates GFX-block firmware (otherwise PSP rejects
    // SDMA/CP/MES/RLC with status 0xFFFF0006 / 0x5 while still accepting
    // SMU/IMU).
    if (dev.ip.isResolved(gmc.mmhub.ip)) {
        r = gmc_mmhub_gart_enable(dev, gmc);
        if (r != kIOReturnSuccess) {
            GMC_LOG("mmhub_gart_enable failed: %#x", r);
            return r;
        }
    } else {
        GMC_LOG("skipping mmhub_gart_enable — IP base unresolved");
    }

    // HDP flush — invalidate the HDP cache so the GPU sees the page-
    // table writes we just made via the BAR.
    // Mirrors gmc_v12_0_gart_enable (gmc_v12_0.c:1019).
    if (dev.ip.isResolved(IPBlock::HDP)) {
        gmc_hdp_flush(dev);
    }

    // set_fault_enable_default — wire the L2 protection-fault unit to
    // redirect faults to dummy_page rather than crash the engines.
    // Audit #4 F23. gmc_v12_0.c:1023.
    if (dev.ip.isResolved(gmc.mmhub.ip)) {
        kern_return_t fr = gmc_set_fault_enable_default(dev, gmc.mmhub, true);
        if (fr != kIOReturnSuccess) {
            GMC_LOG("mmhub set_fault_enable_default failed: %#x", fr);
        }
    }

    // MMHUB-only TLB flush (matches upstream gmc_v12_0_gart_enable
    // dispatch to mmhub.gart_enable only). The gfxhub flush happens
    // later when gmc_gfxhub_gart_enable runs in GFXInit.
    if (dev.ip.isResolved(gmc.mmhub.ip)) {
        gmc_flush_gpu_tlb(dev, gmc, gmc.mmhub, /*vmid*/ 0, /*type*/ 0);
    }
    // Linux amdgpu_gart_map flushes every hub; once the GC hub is live (CP/MES/SDMA
    // fetch their GART rings through it) its TLB must see new PTEs as well.
    if (gmc.gfxhub.gart_enabled && dev.ip.isResolved(gmc.gfxhub.ip)) {
        gmc_flush_gpu_tlb(dev, gmc, gmc.gfxhub, /*vmid*/ 0, /*type*/ 0);
    }

    dev.gmcReady = true;
    GMC_LOG("GMCInit complete");
    return kIOReturnSuccess;
}

// ============================================================
// HDP flush — delegates to amdgpu_hdp_flush in amdgpu_regs.h, which
// is the proper port of upstream amdgpu_hdp_generic_flush
// (amdgpu_hdp.c:48-54): write 0 to the remap-HDP register + readback
// NBIO's get_memsize. hdp_v7_0_funcs (hdp_v7_0.c:128-132) only
// hooks .flush_hdp = amdgpu_hdp_generic_flush; there is no
// invalidate_hdp on this asic, and an earlier implementation wrote to
// a bogus regHDP_MEM_COHERENCY_FLUSH_CNTL=0x230C that does not exist
// in gc_12_0_0_offset.h or hdp_7_0_0_offset.h. Audit #6 step 8.
// ============================================================
kern_return_t
gmc_hdp_flush(DeviceContext &dev)
{
    amdgpu_hdp_flush(dev);
    return kIOReturnSuccess;
}

// ============================================================
// gmc_flush_gpu_tlb — invalidate TLB entries using the engines we
// armed in hub_program_invalidation. Port of:
//   gmc_v12_0_flush_gpu_tlb / gmc_v12_0_flush_vm_hub  (gmc_v12_0.c:213+305)
//   mmhub_v4_1_0_get_invalidate_req                   (mmhub_v4_1_0.c:67)
//   gfxhub_v12_0_get_invalidate_req                   (gfxhub_v12_0.c:61)
//
// Audit #4 F24: an earlier implementation hard-coded shifts of
// 0,4..11 which were wrong — the correct request-word shifts are
// 0 (PER_VMID, 16-bit field), 16 (FLUSH_TYPE), 19-23 (L2/L1 invalidates),
// 24 (CLEAR_PFS). Built via REG_SET_FIELD so the SHIFT/MASK pairs
// in amdgpu_field_defs.h are the single source of truth.
//
// Upstream also targets engine 17 for GART flushes (not engine 0) to
// avoid contention with on-the-fly per-submit flushes; we match that.
// ============================================================
// ============================================================
// build 0.0.528 (; apple/gfx_tlb83.h) — SWITCH 83: THE TLB-ONLY SPIN POLL, ITS LEAF LOCK AND ITS COUNTERS.
// gmc_flush_gpu_tlb is the ONLY user of tlb83_request_and_wait; poll_reg (amdgpu_regs.h) is unchanged, so the PSP/SMU waits keep
// sleeping. The poll itself is n48_tlb83_poll (pure; the host suite drives the same function).
//
// ITEM 5 — THE LOCK. Two threads CAN flush at once in this kext: hook_unmapVA's withdraw and re-arm run on the thread that called
// Apple's unmapVA, commit_keystone_arm -> rootwrite_arm_context on the commit thread (hook_gfxCommitIB), wsv_validate_locked under
// gWsLock, and nothing of ours serialises navi48_gmc_flush_tlb_vmid (upstream holds invalidate_lock around the same request/ack).
// While ON the request write and the ack wait are taken under gTlb83Lock. LOCK ORDER: gTlb83Lock is a LEAF - nothing is acquired
// while it is held (no other lock, no log line, no allocation; only WREG32, the ack reads, the clock, IODelay(1) and IOSleep(1)),
// so it closes no cycle whatever the caller holds (gWsLock, gXdLock, gDrainLock or none): every edge into it ends there. Blocking
// is already allowed at every caller (poll_reg sleeps). OFF takes no lock (0.0.527's behaviour).
// ============================================================
volatile uint32_t gTlb83On { N48_TLB83_OFF };   // switch 83; OFF at boot
IOLock *gTlb83Lock { nullptr };
n48_tlb83_stats gTlb83S {};

static uint64_t tlb83_now_us(void *)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000ull;
}
struct Tlb83Ctx { const DeviceContext *dev; uint32_t ack_reg; };
static uint32_t tlb83_rd(void *c)
{
    const Tlb83Ctx *x = static_cast<const Tlb83Ctx *>(c);
    return RREG32(*x->dev, x->ack_reg);
}
static void tlb83_delay1(void *) { IODelay(1); }
static void tlb83_sleep1(void *) { IOSleep(1); }

// OFF: the wait by the clock (item 2). noinline, so gmc_flush_gpu_tlb keeps its frame.
static __attribute__((noinline)) void tlb83_note_off(uint64_t t0, bool acked)
{
    const uint64_t t1 = tlb83_now_us(nullptr);
    n48_tlb83_note_off(&gTlb83S, acked ? 1u : 0u, t1 >= t0 ? t1 - t0 : 0ull);
}

// ON: the request write and the ack wait, under the leaf lock. Returns poll_reg's answer (acked); *value = the last value read.
static __attribute__((noinline)) bool tlb83_request_and_wait(DeviceContext &dev, uint32_t req_reg, uint32_t req,
                                                            uint32_t ack_reg, uint32_t expected, uint32_t *value)
{
    Tlb83Ctx c { &dev, ack_reg };
    const n48_tlb83_io io { &c, tlb83_rd, tlb83_now_us, tlb83_delay1, tlb83_sleep1 };
    n48_tlb83_res res {};
    IOLock *const lk = gTlb83Lock;
    uint32_t contended = 0u;
    if (lk && !IOLockTryLock(lk)) { contended = 1u; IOLockLock(lk); }
    const uint64_t t0 = tlb83_now_us(nullptr);
    WREG32(dev, req_reg, req);
    (void)n48_tlb83_poll(&io, expected, expected, N48_TLB83_TIMEOUT_US, &res);
    const uint64_t t1 = tlb83_now_us(nullptr);
    if (lk) IOLockUnlock(lk);
    n48_tlb83_note_on(&gTlb83S, &res, t1 >= t0 ? t1 - t0 : 0ull, contended);
    *value = res.value;
    return res.ok != 0u;
}

kern_return_t
gmc_flush_gpu_tlb(DeviceContext &dev, const GMCContext &gmc,
                  const HubContext &hub, uint32_t vmid, uint32_t flush_type)
{
    (void)gmc;
    if (!hub.inited || !dev.ip.isResolved(hub.ip)) return kIOReturnNotReady;

    // Build the request word via REG_SET_FIELD — same pattern as
    // {mmhub,gfxhub}_v*_get_invalidate_req upstream.
    uint32_t req = 0;
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ,
                        PER_VMID_INVALIDATE_REQ, 1u << vmid);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ,
                        FLUSH_TYPE, flush_type & 0x7);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PTES, 1);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE0, 1);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE1, 1);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE2, 1);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, INVALIDATE_L1_PTES, 1);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ,
                        CLEAR_PROTECTION_FAULT_STATUS_ADDR, 0);

    // Upstream gmc_v12_0_flush_vm_hub uses engine 17 for GART flushes
    // (gmc_v12_0.c:221: "Use register 17 for GART").
    constexpr unsigned kFlushEng = 17;

    const uint32_t req_reg = SOC15_REG_OFFSET_BIDX(dev, hub.ip, hub.base_idx,
                                              hub.vm_invalidate_eng0_req)
                           + kFlushEng * hub.eng_distance;
    const uint32_t ack_reg = SOC15_REG_OFFSET_BIDX(dev, hub.ip, hub.base_idx,
                                              hub.vm_invalidate_eng0_ack)
                           + kFlushEng * hub.eng_distance;

    // Poll the ack bit for this vmid. Upstream loops until usec_timeout
    // (default 100 ms).
    uint32_t expected = (1u << vmid);
    uint32_t value = 0;
    bool acked;
    // build 0.0.528 (; apple/gfx_tlb83.h) — SWITCH 83, read ONCE per call. ON: the request write and the ack wait
    // (spin, then this same sleep loop for the rest of the same 100 ms) under the leaf lock, in tlb83_request_and_wait. OFF (the
    // default): the request write and poll_reg exactly as 0.0.527; only the wait's clock reads (item 2) are added around it.
    if (__atomic_load_n(&gTlb83On, __ATOMIC_ACQUIRE) == N48_TLB83_ON) {
        acked = tlb83_request_and_wait(dev, req_reg, req, ack_reg, expected, &value);
    } else {
        WREG32(dev, req_reg, req);
        const uint64_t t0 = tlb83_now_us(nullptr);
        acked = poll_reg(dev, ack_reg, expected, expected,
                         /*timeout_us*/ 100000, &value);
        tlb83_note_off(t0, acked);
    }
    if (!acked) {
        GMC_LOG("flush_gpu_tlb: ack timeout (req=%#x ack=%#x ip=%u eng=%u)",
                (unsigned)req, (unsigned)value, (unsigned)hub.ip,
                (unsigned)kFlushEng);
        return kIOReturnTimeout;
    }
    // build 0.0.528 item 3 (log-only, UNSWITCHED): the first 32 `acked` lines per boot, then a count only (bare 83).
    if (n48_tlb83_ack_line(&gTlb83S))
        GMC_LOG("flush_gpu_tlb(ip=%u vmid=%u type=%u): req=%#x acked",
                (unsigned)hub.ip, (unsigned)vmid, (unsigned)flush_type,
                (unsigned)req);
    return kIOReturnSuccess;
}

//============================================================
// gmc_bind_existing — write GFX12-format PTEs for an external
// DMA-able bus address range into the VRAM-resident page table.
// Mirrors upstream amdgpu_gart_bind for a contiguous mapping but
// writes through the BAR0 aperture instead of going through ttm.
// Returns the GART MC address.
//
// The reference required kASPageSize (16 KB, Apple Silicon CPU page)
// alignment because DART demanded it. Here the only requirement is the
// GPU page size (kAMDGPUGPUPageSize = 4 KB) — sysmem_alloc hands out
// physically contiguous, page-aligned memory with no IOMMU in the path.
//============================================================
kern_return_t
gmc_bind_existing(DeviceContext &dev, GMCContext &gmc, uint64_t busAddr,
                  uint64_t sizeBytes, uint64_t *outMcAddr)
{
    if (!gmc.inited)                    return kIOReturnNotReady;
    if (gmc.gart_pt_bus == 0)           return kIOReturnNotReady;
    if (outMcAddr == nullptr)           return kIOReturnBadArgument;
    if (busAddr == 0 || sizeBytes == 0) return kIOReturnBadArgument;
    if ((busAddr & (kAMDGPUGPUPageSize - 1)) != 0) return kIOReturnNotAligned;

    uint64_t rounded = (sizeBytes + kAMDGPUGPUPageSize - 1) &
                       ~((uint64_t)kAMDGPUGPUPageSize - 1);
    if (gmc.gart_bump_offset + rounded > gmc.gart_size) {
        GMC_LOG("gmc_bind_existing: out of GART (need %llu, free %llu)",
                (unsigned long long)rounded,
                (unsigned long long)(gmc.gart_size - gmc.gart_bump_offset));
        return kIOReturnNoSpace;
    }
    uint64_t off = gmc.gart_bump_offset;
    gmc.gart_bump_offset += rounded;

    // GMC is programmed with 4 KB GPU-page granularity (PAGE_TABLE_START
    // = gart_start >> 12). One PTE = 4 KB of GART space. Mirrors upstream
    // amdgpu_gart_map, which writes one PTE per AMDGPU_GPU_PAGE_SIZE
    // regardless of host CPU page size.
    uint32_t numPTEs  = (uint32_t)(rounded / kAMDGPUGPUPageSize);
    uint64_t firstPTE = off / kAMDGPUGPUPageSize;
    for (uint32_t i = 0; i < numPTEs; i++) {
        uint64_t pa = busAddr + (uint64_t)i * kAMDGPUGPUPageSize;
        // GFX12 single-level GART PTE: VALID|SYSTEM|SNOOPED|EXEC|R|W,
        // MTYPE_UC, plus IS_PTE (bit 63) so the GMC walker treats the
        // entry as a leaf PTE and not a PDE pointer.
        uint64_t pte = (pa & ~((uint64_t)0xFFFULL)) | PTEFlags::SYSMEM_RW;
        uint64_t pteVRAMOffset = gmc.gart_pt_vram_offset +
                                 (firstPTE + i) * 8ULL;
        bar0_memcpy_to_vram(dev, pteVRAMOffset, &pte, sizeof(pte));
    }
    amdgpu_hdp_flush(dev);

    *outMcAddr = gmc.gart_start + off;
    GMC_LOG("bind_existing: %llu bytes @ bus=%#llx -> mc=%#llx "
            "(%u PTEs @ 4KB stride starting at PT idx %llu, PT vram_off=%#llx)",
            (unsigned long long)rounded, (unsigned long long)busAddr,
            (unsigned long long)*outMcAddr, (unsigned)numPTEs,
            (unsigned long long)firstPTE,
            (unsigned long long)gmc.gart_pt_vram_offset);
    return kIOReturnSuccess;
}

//============================================================
// gmc_bind_at — write GFX12 GART PTEs at a caller-chosen offset.
//
// gmc_bind_existing allocates the offset itself, which is right for buffers this
// driver owns. Apple's accelerator allocates inside the aperture our TTL reports
// and then expects those exact offsets to be mapped, so it needs this form.
//
// Everything about the PTE is identical to gmc_bind_existing; only the offset
// source differs. Kept as a separate entry point rather than a flag so the
// bump allocator's invariant stays obvious at every call site.
//============================================================
// The GART sync calls this once per page — 2193 times on this card. Left
// unthrottled it emits 2193 lines and blows the 1 MiB kernel buffer, taking every
// earlier line with it: on an earlier run that destroyed the hook-install records
// and made a real "MMIO writes are not being blocked" bug look like a logging
// gap. Errors below still log every time; only the success line is throttled.
static uint32_t gBindAtCalls = 0;
// 0.0.368: RULE E1's clause 2 half two. Every time gmc_bind_at REFUSES an Apple bind that overlaps the region
// this driver has handed out, something of Apple's tried to map over pages we own — among them the clear-state NOP page E1
// certifies. One refusal is enough to retire "only this kext writes that page" as an observation, so E1 requires ZERO.
// hp1 and hp2 both logged 0 refusals against 12 `bind_at #` lines. Latched per boot; never cleared.
static uint64_t gBindAtOverlapRefusals = 0;
uint64_t gmc_bind_at_overlap_refusals(void) { return gBindAtOverlapRefusals; }

kern_return_t
gmc_bind_at(DeviceContext &dev, GMCContext &gmc, uint64_t gartOffset,
            uint64_t busAddr, uint64_t sizeBytes)
{
    if (!gmc.inited)                    return kIOReturnNotReady;
    if (gmc.gart_pt_bus == 0)           return kIOReturnNotReady;
    if (busAddr == 0 || sizeBytes == 0) return kIOReturnBadArgument;
    if ((busAddr    & (kAMDGPUGPUPageSize - 1)) != 0) return kIOReturnNotAligned;
    if ((gartOffset & (kAMDGPUGPUPageSize - 1)) != 0) return kIOReturnNotAligned;

    uint64_t rounded = (sizeBytes + kAMDGPUGPUPageSize - 1) &
                       ~((uint64_t)kAMDGPUGPUPageSize - 1);
    if (gartOffset + rounded > gmc.gart_size) {
        GMC_LOG("bind_at: [%#llx..%#llx) is outside the %llu MiB GART",
                (unsigned long long)gartOffset,
                (unsigned long long)(gartOffset + rounded),
                (unsigned long long)(gmc.gart_size >> 20));
        return kIOReturnBadArgument;
    }
    // One page table, two owners. Refuse only a REAL overlap with the region the
    // bump allocator has actually handed out: [gart_bump_start, gart_bump_offset).
    //
    // The old test was `gartOffset < gart_bump_offset` — "anything below the
    // high-water mark" — which is correct ONLY while the region starts at 0. With
    // the region moved to the top of the aperture that test would refuse
    // every one of Apple's low-page binds and mirror nothing at all, which is
    // worse than the collision it was meant to prevent.
    if (gartOffset < gmc.gart_bump_offset &&
        gartOffset + rounded > gmc.gart_bump_start) {
        GMC_LOG("bind_at: [%#llx..%#llx) overlaps this driver's own bump region "
                "[%#llx..%#llx) — REFUSING",
                (unsigned long long)gartOffset,
                (unsigned long long)(gartOffset + rounded),
                (unsigned long long)gmc.gart_bump_start,
                (unsigned long long)gmc.gart_bump_offset);
        gBindAtOverlapRefusals++;   // 0.0.368: RULE E1 requires this to be 0 at arm time
        return kIOReturnBusy;
    }

    uint32_t numPTEs  = (uint32_t)(rounded / kAMDGPUGPUPageSize);
    uint64_t firstPTE = gartOffset / kAMDGPUGPUPageSize;
    for (uint32_t i = 0; i < numPTEs; i++) {
        uint64_t pa  = busAddr + (uint64_t)i * kAMDGPUGPUPageSize;
        uint64_t pte = (pa & ~((uint64_t)0xFFFULL)) | PTEFlags::SYSMEM_RW;
        uint64_t pteVRAMOffset = gmc.gart_pt_vram_offset +
                                 (firstPTE + i) * 8ULL;
        bar0_memcpy_to_vram(dev, pteVRAMOffset, &pte, sizeof(pte));
    }
    amdgpu_hdp_flush(dev);

    gBindAtCalls++;
    if (gBindAtCalls <= 8 || (gBindAtCalls % 512) == 0)
        GMC_LOG("bind_at #%u: %llu bytes bus=%#llx -> mc=%#llx (gart_off=%#llx, %u PTEs "
                "from PT idx %llu)",
                (unsigned)gBindAtCalls,
                (unsigned long long)rounded, (unsigned long long)busAddr,
                (unsigned long long)(gmc.gart_start + gartOffset),
                (unsigned long long)gartOffset, (unsigned)numPTEs,
                (unsigned long long)firstPTE);
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// gmc_walker_error_name — GCVM_L2_PROTECTION_FAULT_STATUS_LO32.WALKER_ERROR
// [3:1], gc_12_0_0_sh_mask.h:9002-9025. Named so a fault log says WHICH level
// of the walk refused instead of printing a bare 1..7.
// ---------------------------------------------------------------------------
const char *gmc_walker_error_name(uint32_t we)
{
    switch (we & 7u) {
    case 0: return "none";
    case 1: return "RANGE";
    case 2: return "PDE0";
    case 3: return "PDE1";
    case 4: return "PDE2";
    case 5: return "TRANSLATE-FURTHER";
    case 6: return "NACK";
    default: return "DUMMY-PAGE";
    }
}

// ---------------------------------------------------------------------------
// gmc_log_vm_faults — decode L2 protection-fault status of both hubs (read-only).
// STATUS_LO32 fields (gc_12_0_0_sh_mask.h): MORE_FAULTS bit0, WALKER_ERROR bits1-3,
// PERMISSION_FAULTS bits4-7, MAPPING_ERROR bit8, CID bits9-17, RW bit18.
// ---------------------------------------------------------------------------
static void log_hub_faults(DeviceContext &dev, const char *name, IPBlock block,
                           uint32_t regStatusLo, uint32_t regStatusHi, uint32_t regAddrLo, uint32_t regAddrHi) {
    if (!dev.ip.isResolved(block)) { GMC_LOG("faults[%s]: hub base unresolved", name); return; }
    const uint32_t lo = RREG32(dev, SOC15_REG_OFFSET_BIDX(dev, block, 0, regStatusLo));
    const uint32_t hi = RREG32(dev, SOC15_REG_OFFSET_BIDX(dev, block, 0, regStatusHi));
    const uint32_t alo = RREG32(dev, SOC15_REG_OFFSET_BIDX(dev, block, 0, regAddrLo));
    const uint32_t ahi = RREG32(dev, SOC15_REG_OFFSET_BIDX(dev, block, 0, regAddrHi));
    const uint64_t faultPage = ((uint64_t)ahi << 32) | alo;
    GMC_LOG("faults[%s]: STATUS_LO32=0x%08x HI32=0x%08x ADDR=0x%llx (page<<12 = 0x%llx) | more=%u walker_err=%u perm=0x%x mapping_err=%u cid=%u rw=%s",
            name, lo, hi, (unsigned long long)faultPage, (unsigned long long)(faultPage << 12),
            lo & 1u, (lo >> 1) & 7u, (lo >> 4) & 0xfu, (lo >> 8) & 1u, (lo >> 9) & 0x1ffu, ((lo >> 18) & 1u) ? "write" : "read");
}

void gmc_log_vm_faults(DeviceContext &dev, const GMCContext &gmc) {
    (void)gmc;
    log_hub_faults(dev, "GCVM (gfxhub)",  IPBlock::GC,    0x15d0, 0x15d1, 0x15d2, 0x15d3);
    log_hub_faults(dev, "MMVM (mmhub)",   IPBlock::MMHUB, 0x04f0, 0x04f1, 0x04f2, 0x04f3);
}

// Linux gmc_v12_0_process_interrupt: after reading the status,
// WREG32_P(hub->vm_l2_pro_fault_cntl, 1, ~1) — CLEAR_PROTECTION_FAULT_STATUS_ADDR
// (bit 0). Pulsed here so the CNTL keeps its programmed value and the next
// fault latches fresh. Used to separate a test's faults from earlier ones.
void gmc_clear_vm_faults(DeviceContext &dev, const GMCContext &gmc) {
    const HubContext *hubs[2] = { &gmc.gfxhub, &gmc.mmhub };
    for (const HubContext *h : hubs) {
        if (!h->inited || !dev.ip.isResolved(h->ip)) continue;
        const uint32_t reg = SOC15_REG_OFFSET_BIDX(dev, h->ip, h->base_idx, h->vm_l2_protection_fault_cntl);
        const uint32_t v = RREG32(dev, reg);
        WREG32(dev, reg, v | 1u);
        WREG32(dev, reg, v & ~1u);
        GMC_LOG("faults: cleared L2 protection fault status (ip=%u CNTL@%#x kept %#010x)",
                (unsigned)h->ip, reg, v);
    }
}

//============================================================================
// gmc_vmfrag_selftest (0.0.193, boot-arg navi48-vmfrag-test=1) — INCREMENT 1 of
// the fourth Apple-vs-gfx12 encoding fix ( + the the reviewer report).
//
// THE QUESTION, asked before we touch one byte of Apple's live page table:
// does gfx12's GFXHUB walker accept an APPLE-SHAPED tree written in GFX12
// ENTRY ENCODING?  Apple-shaped means the geometry AMDGFX10VMM::init builds
// (0xbe1e101-0xbe1e188, CONFIRMED by the the reviewer pass):
//
//   CONTEXT PAGE_TABLE_DEPTH = 1, PAGE_TABLE_BLOCK_SIZE = 7
//   VA window [0x400000000, 0x2400000000)
//   root PDE  -> one entry per 256 MiB, block-fragment-size 4
//   L1  entry -> one per 64 KiB: either a pointer to a 16-entry sub-table of
//                4 KiB PTEs, or a 64 KiB leaf in its own right
//
// and gfx12 encoding means our own GART's proven bits (gmc_v12_0.c:456-484,
// amdgpu_vm.h:121-141): P = bit 63 marks a LEAF, BFS sits at 62:58, MTYPE at
// 55:54, and there is no TRANSLATE_FURTHER bit at all.
//
// Two legs, each a COPY_LINEAR inside an IB submitted under VMID 1 — VMID 1
// because CONTEXT1 is free on this machine (every queue we own is VMID 0, and
// Apple uses VMID 2), and an IB because the IB header is the only place an
// SDMA packet can name a VMID. The IB itself is fetched through the same
// table as its operands (: the CP faulted on the IB BASE under VMID 2), so
// each leg's IB lives inside the mapping that leg is testing:
//
//   (a) sub-table:  IB, source and destination are three 4 KiB PTEs in the
//                   16-entry sub-table hanging off L1[0xa] — 0xa is Apple's
//                   own index for the blit IB at VA 0x4000a0000.
//   (b) 64 KiB leaf: IB, source and destination are all inside the single
//                   L1[0xb] leaf, so nothing but the leaf entry is exercised.
//
// If (a) fails, it is re-run once with the INVERSE P polarity (pointer P=1 /
// PTE P=0) because the fragment-level rule for a non-root pointer is the one
// thing the report could not settle from the code.
//
// Everything is ours: our VRAM, our CONTEXT1, our SDMA0 QUEUE0. CONTEXT1's six
// registers are saved before and restored after, whatever happens, and Apple's
// arena is neither read nor written. Default off.
//============================================================================
static constexpr uint64_t kVMFragVAStart    = 0x400000000ull;  // Apple's VA window
static constexpr uint64_t kVMFragVAEnd      = 0x2400000000ull;
static constexpr uint32_t kVMFragL1Index    = 0xa;    // Apple's index for +0xa0000
static constexpr uint64_t kVMFragL1Off      = 0x1000; // L1 block inside our table
static constexpr uint64_t kVMFragSubOff     = 0x9000; // sub-table block
static constexpr uint32_t kVMFragTableBytes = 0x10000;
static constexpr uint32_t kVMFragDataBytes  = 0x20000;
static constexpr uint32_t kVMFragCopyBytes  = 4096;
static constexpr uint64_t kVMFragBFS4       = (4ull << 58);   // gfx12 PDE BFS 4
static constexpr uint64_t kVMFragFrag64K    = (4ull << 7);    // PTE FRAG 4 = 64 KiB

static inline uint64_t vmfrag_leaf(uint64_t pa, uint64_t frag) {
    return pa | PTEFlags::IS_PTE | PTEFlags::MTYPE_GFX12_UC | frag |
           PTEFlags::EXECUTABLE | PTEFlags::READABLE | PTEFlags::WRITEABLE |
           PTEFlags::VALID;
}

kern_return_t
gmc_vmfrag_selftest(DeviceContext &dev, GMCContext &gmc, SDMAInstance &inst,
                    VMFragTestResult *out, bool noValidLeg)
{
    VMFragTestResult t {};
    if (out) *out = t;

    const HubContext &h = gmc.gfxhub;
    if (!h.inited || !dev.ip.isResolved(h.ip)) {
        GMC_LOG("vmfrag: GFXHUB not ready — skipping"); return kIOReturnNotReady;
    }
    if (!inst.inited || !inst.enabled) {
        GMC_LOG("vmfrag: SDMA%u QUEUE0 is not running — skipping", inst.instance);
        return kIOReturnNotReady;
    }

    VRAMAllocation tbl {}, data {};
    if (!gmc.vram_alloc.alloc(kVMFragTableBytes, kAMDGPUGPUPageSize, &tbl)) {
        GMC_LOG("vmfrag: no VRAM for the table — skipping"); return kIOReturnNoMemory;
    }
    if (!gmc.vram_alloc.alloc(kVMFragDataBytes, 0x10000, &data)) {
        GMC_LOG("vmfrag: no VRAM for the data pages — skipping");
        gmc.vram_alloc.free(tbl); return kIOReturnNoMemory;
    }
    const uint64_t tbl_off  = tbl.gpu_va  - gmc.vram_start;
    const uint64_t data_off = data.gpu_va - gmc.vram_start;
    if (tbl_off + kVMFragTableBytes > dev.bar0Size ||
        data_off + kVMFragDataBytes > dev.bar0Size) {
        GMC_LOG("vmfrag: table vram+%#llx / data vram+%#llx outside BAR0 (%llu) "
                "— REFUSING (every entry and every check is a BAR0 access)",
                (unsigned long long)tbl_off, (unsigned long long)data_off,
                (unsigned long long)dev.bar0Size);
        gmc.vram_alloc.free(data); gmc.vram_alloc.free(tbl);
        return kIOReturnNoMemory;
    }
    if ((data.gpu_va & 0xFFFFull) != 0) {
        GMC_LOG("vmfrag: data page mc %#llx is not 64 KiB aligned — REFUSING (the "
                "64 KiB leaf's physical base must be leaf-aligned)",
                (unsigned long long)data.gpu_va);
        gmc.vram_alloc.free(data); gmc.vram_alloc.free(tbl);
        return kIOReturnNotAligned;
    }

    // Physical (device) addresses, exactly as gmc_v12_0_get_vm_pde computes
    // them for a VRAM-resident table: vram_base_offset + MC - vram_start.
    auto pa_of = [&](uint64_t mc) -> uint64_t {
        return gmc.vram_base_offset + mc - gmc.vram_start;
    };
    const uint64_t tbl_pa  = pa_of(tbl.gpu_va);
    const uint64_t data_pa = pa_of(data.gpu_va);
    const uint64_t sub_off = kVMFragSubOff + (uint64_t)(kVMFragL1Index % 32) * 0x80;

    // VAs. L1[0xa] -> the sub-table; L1[0xb] -> the 64 KiB leaf.
    const uint64_t sub_va  = kVMFragVAStart + ((uint64_t)kVMFragL1Index << 16);
    const uint64_t leaf_va = kVMFragVAStart + ((uint64_t)(kVMFragL1Index + 1) << 16);

    t.ran       = true;
    t.table_mc  = tbl.gpu_va;
    t.data_mc   = data.gpu_va;
    t.va_start  = kVMFragVAStart;
    t.sub_va    = sub_va;
    t.leaf_va   = leaf_va;

    // ---- build the table -------------------------------------------------
    auto build_table = [&](bool invertedPolarity) {
        bar0_memset_vram(dev, tbl_off, 0, kVMFragTableBytes);
        const uint64_t ptrBit  = invertedPolarity ? PTEFlags::IS_PTE : 0ull;
        const uint64_t pteBit  = invertedPolarity ? 0ull : PTEFlags::IS_PTE;
        // root[0]: BFS 4, P = 0 (a real directory), -> the L1 block.
        WBAR0_64(dev, tbl_off + 0,
                 (tbl_pa + kVMFragL1Off) | kVMFragBFS4 | PTEFlags::VALID);
        // L1[0xa]: pointer to the 16-entry sub-table.
        WBAR0_64(dev, tbl_off + kVMFragL1Off + (uint64_t)kVMFragL1Index * 8,
                 (tbl_pa + sub_off) | ptrBit | PTEFlags::VALID);
        // L1[0xb]: a 64 KiB leaf covering data[0 .. 64 KiB).
        WBAR0_64(dev, tbl_off + kVMFragL1Off + (uint64_t)(kVMFragL1Index + 1) * 8,
                 vmfrag_leaf(data_pa, kVMFragFrag64K));
        // sub[0..2]: 4 KiB PTEs -> data + 0x10000 / 0x11000 / 0x12000.
        for (uint32_t i = 0; i < 3; i++) {
            uint64_t e = vmfrag_leaf(data_pa + 0x10000 + (uint64_t)i * 0x1000, 0);
            if (!pteBit) e &= ~PTEFlags::IS_PTE;
            WBAR0_64(dev, tbl_off + sub_off + (uint64_t)i * 8, e);
        }
        amdgpu_hdp_flush(dev);
        GMC_LOG("vmfrag: table at mc %#llx (pa %#llx)%s — root[0]=%#018llx "
                "L1[%u]=%#018llx L1[%u]=%#018llx sub[0]=%#018llx sub[1]=%#018llx "
                "sub[2]=%#018llx (sub-table at +%#llx)",
                (unsigned long long)tbl.gpu_va, (unsigned long long)tbl_pa,
                invertedPolarity ? " INVERSE P POLARITY (pointer P=1 / PTE P=0)" : "",
                (unsigned long long)RBAR0_64(dev, tbl_off + 0),
                kVMFragL1Index,
                (unsigned long long)RBAR0_64(dev, tbl_off + kVMFragL1Off +
                                             (uint64_t)kVMFragL1Index * 8),
                kVMFragL1Index + 1,
                (unsigned long long)RBAR0_64(dev, tbl_off + kVMFragL1Off +
                                             (uint64_t)(kVMFragL1Index + 1) * 8),
                (unsigned long long)RBAR0_64(dev, tbl_off + sub_off + 0),
                (unsigned long long)RBAR0_64(dev, tbl_off + sub_off + 8),
                (unsigned long long)RBAR0_64(dev, tbl_off + sub_off + 16),
                (unsigned long long)sub_off);
    };

    // ---- CONTEXT1: save, program, and (at the end) restore ---------------
    const uint32_t cntl_reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_cntl);
    const uint32_t base_lo_reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx,
                                     h.ctx0_pt_base_lo) + h.ctx_addr_distance;
    const uint32_t base_hi_reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx,
                                     h.ctx0_pt_base_hi) + h.ctx_addr_distance;
    const uint32_t st_lo_reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_start_lo);
    const uint32_t st_hi_reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_start_hi);
    const uint32_t en_lo_reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_end_lo);
    const uint32_t en_hi_reg = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_end_hi);
    const uint32_t saved[7] = {
        RREG32(dev, cntl_reg),    RREG32(dev, base_lo_reg), RREG32(dev, base_hi_reg),
        RREG32(dev, st_lo_reg),   RREG32(dev, st_hi_reg),
        RREG32(dev, en_lo_reg),   RREG32(dev, en_hi_reg),
    };

    uint32_t cntl = RREG32(dev, cntl_reg);
    cntl = REG_SET_FIELD(cntl, MMVM_CONTEXT1_CNTL, ENABLE_CONTEXT, 1);
    cntl = REG_SET_FIELD(cntl, MMVM_CONTEXT1_CNTL, PAGE_TABLE_DEPTH,
                         kAppleCtxPageTableDepth);
    cntl = REG_SET_FIELD(cntl, MMVM_CONTEXT1_CNTL, PAGE_TABLE_BLOCK_SIZE,
                         kAppleCtxPageTableBlockSize);
    cntl = REG_SET_FIELD(cntl, MMVM_CONTEXT1_CNTL, RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 1);
    WREG32(dev, cntl_reg, cntl);
    WREG32(dev, base_lo_reg, (uint32_t)((tbl_pa | PTEFlags::VALID) & 0xFFFFFFFFu));
    WREG32(dev, base_hi_reg, (uint32_t)((tbl_pa | PTEFlags::VALID) >> 32));
    WREG32(dev, st_lo_reg, (uint32_t)(kVMFragVAStart >> 12));
    WREG32(dev, st_hi_reg, (uint32_t)(kVMFragVAStart >> 44));
    WREG32(dev, en_lo_reg, (uint32_t)((kVMFragVAEnd - 1) >> 12));
    WREG32(dev, en_hi_reg, (uint32_t)((kVMFragVAEnd - 1) >> 44));
    t.ctx1_cntl = RREG32(dev, cntl_reg);
    GMC_LOG("vmfrag: GFXHUB CONTEXT1 (VMID 1) CNTL@%#x = %#010x (was %#010x; want "
            "0x03fffd73) BASE %#010x:%#010x START %#010x END %#010x — VA window "
            "[%#llx, %#llx), sub-table VA %#llx, 64 KiB leaf VA %#llx",
            cntl_reg, t.ctx1_cntl, saved[0],
            RREG32(dev, base_hi_reg), RREG32(dev, base_lo_reg),
            RREG32(dev, st_lo_reg), RREG32(dev, en_lo_reg),
            (unsigned long long)kVMFragVAStart, (unsigned long long)kVMFragVAEnd,
            (unsigned long long)sub_va, (unsigned long long)leaf_va);

    // ---- one leg: fill the source through BAR0, copy, compare -----------
    auto run_leg = [&](const char *name, uint64_t ibVa, uint64_t ibOff,
                       uint64_t srcVa, uint64_t dstVa,
                       uint64_t srcOff, uint64_t dstOff, uint32_t pattern,
                       uint32_t tag, uint32_t *badOut) -> bool {
        bar0_memset_vram(dev, srcOff, pattern, kVMFragCopyBytes);
        bar0_memset_vram(dev, dstOff, 0xDEADBEEFu, kVMFragCopyBytes);
        amdgpu_hdp_flush(dev);
        (void)gmc_flush_gpu_tlb(dev, gmc, h, /*vmid*/ 1, /*type*/ 0);
        gmc_clear_vm_faults(dev, gmc);
        const kern_return_t r =
            sdma_ib_copy_linear_test(dev, inst, ibVa, ibOff, srcVa, dstVa,
                                     kVMFragCopyBytes, /*vmid*/ 1, tag, 200000ull);
        uint32_t bad = 0;
        for (uint32_t i = 0; i < kVMFragCopyBytes / 4; i++)
            if (RBAR0_32(dev, dstOff + (uint64_t)i * 4) != pattern) bad++;
        if (badOut) *badOut = bad;
        const uint32_t flo = RREG32(dev, SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, 0x15d0));
        const uint32_t alo = RREG32(dev, SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, 0x15d2));
        const bool ok = (r == kIOReturnSuccess) && (bad == 0);
        GMC_LOG("vmfrag %s: IB VA %#llx, COPY %#010x  VA %#llx -> VA %#llx  %s — "
                "kr=%#x, %u/%u dwords wrong; STATUS_LO32=%#010x walker=%u (%s) "
                "perm=%#x mapping=%u vmid=%u ADDR_LO=%#010x",
                name, (unsigned long long)ibVa, pattern,
                (unsigned long long)srcVa, (unsigned long long)dstVa,
                ok ? "PASSED" : "FAILED", r, bad, kVMFragCopyBytes / 4,
                flo, (flo >> 1) & 7u, gmc_walker_error_name(flo >> 1),
                (flo >> 4) & 0xfu, (flo >> 8) & 1u, (flo >> 20) & 0xfu, alo);
        t.fault_lo = flo;
        return ok;
    };

    // ---- leg (a): the 16-entry sub-table --------------------------------
    build_table(/*invertedPolarity*/ false);
    t.sub_ok = run_leg("(a) SUB-TABLE", sub_va + 0x2000, data_off + 0x12000,
                       sub_va + 0x0000, sub_va + 0x1000,
                       data_off + 0x10000, data_off + 0x11000,
                       0x5B7AB1E0u, 0xA1, &t.sub_bad);
    if (!t.sub_ok) {
        GMC_LOG("vmfrag: leg (a) failed with pointer P=0 / PTE P=1 — retrying ONCE "
                "with the inverse polarity, which is the one rule the the reviewer pass "
                "could not settle from Apple's code");
        build_table(/*invertedPolarity*/ true);
        t.inverted = true;
        t.sub_ok = run_leg("(a') SUB-TABLE inverse-P", sub_va + 0x2000,
                           data_off + 0x12000, sub_va + 0x0000, sub_va + 0x1000,
                           data_off + 0x10000, data_off + 0x11000,
                           0x5B7AB1E1u, 0xA2, &t.sub_bad);
        if (!t.sub_ok) build_table(/*invertedPolarity*/ false);   // leg (b) wants the normal one
    }

    // ---- leg (b): the 64 KiB leaf ---------------------------------------
    t.leaf_ok = run_leg("(b) 64 KiB LEAF", leaf_va + 0x2000, data_off + 0x2000,
                        leaf_va + 0x0000, leaf_va + 0x1000,
                        data_off + 0x0000, data_off + 0x1000,
                        0x64C0FFEEu, 0xB1, &t.leaf_bad);
    t.fault_walker = (t.fault_lo >> 1) & 7u;

    // ---- leg (c), M4-WS-VMID-VALID: the SAME 64 KiB leaf copy on a base WITHOUT the VALID bit ----
    // Only with navi48-vmfrag-novalid=1 (as well as navi48-vmfrag-test=1), and only after leg (b) passed WITH the bit, so a failure
    // here is the bit and nothing else. The one question: does gfx12 check bit 0 (AMDGPU_PTE_VALID, amdgpu_vm.h:57) of
    // GCVM_CONTEXTn_PAGE_TABLE_BASE_ADDR_LO32? Apple writes the bare base and our drain sets the bit on CONTEXT2 only (
    // changed three things at once, so this was never isolated). RETRY_PERMISSION_OR_INVALID_PAGE_FAULT is CLEARED for this leg
    // alone: with retry on, a refused walk retries forever and would wedge our own SDMA0 QUEUE0 for the rest of the boot; with it
    // off the fault is recorded in STATUS_LO32 and the access is answered from the default page (SUSPECTED, the reason the leg is
    // default-off). CONTEXT1 is restored below with everything else.
    if (noValidLeg && t.leaf_ok) {
        const uint32_t faultB = t.fault_lo;
        uint32_t c2 = cntl;
        c2 = REG_SET_FIELD(c2, MMVM_CONTEXT1_CNTL, RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 0);
        WREG32(dev, cntl_reg, c2);
        WREG32(dev, base_lo_reg, (uint32_t)(tbl_pa & 0xFFFFFFFFu) & ~(uint32_t)PTEFlags::VALID);
        WREG32(dev, base_hi_reg, (uint32_t)(tbl_pa >> 32));
        t.nv_base_lo = RREG32(dev, base_lo_reg);
        t.nv_ran = true;
        GMC_LOG("vmfrag (c): CONTEXT1 BASE %#010x:%#010x (bit 0 %s), CNTL %#010x (retry off for this leg only)",
                RREG32(dev, base_hi_reg), t.nv_base_lo, (t.nv_base_lo & 1u) ? "STILL SET - leg invalid" : "clear",
                RREG32(dev, cntl_reg));
        if ((t.nv_base_lo & 1u) == 0u)
            t.nv_ok = run_leg("(c) 64 KiB LEAF, BASE WITHOUT VALID", leaf_va + 0x2000, data_off + 0x2000,
                              leaf_va + 0x0000, leaf_va + 0x1000, data_off + 0x0000, data_off + 0x1000,
                              0x0DDBA5E0u, 0xC1, &t.nv_bad);
        t.nv_fault_lo = t.fault_lo;
        t.nv_fault_addr_lo = RREG32(dev, SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, 0x15d2));
        t.fault_lo = faultB;   // Navi48,VMFragTest keeps leg (b)'s status
        GMC_LOG("vmfrag (c): *** the bare base was %s *** (%u dw wrong; STATUS_LO32 %#010x walker %u (%s) vmid %u ADDR_LO %#010x)",
                t.nv_ok ? "ACCEPTED: gfx12 did not check bit 0 on this walk" : "REFUSED or the copy failed: bit 0 is checked",
                t.nv_bad, t.nv_fault_lo, (t.nv_fault_lo >> 1) & 7u, gmc_walker_error_name(t.nv_fault_lo >> 1),
                (t.nv_fault_lo >> 20) & 0xfu, t.nv_fault_addr_lo);
    }

    // ---- restore CONTEXT1, always ---------------------------------------
    WREG32(dev, cntl_reg,    saved[0]);
    WREG32(dev, base_lo_reg, saved[1]);
    WREG32(dev, base_hi_reg, saved[2]);
    WREG32(dev, st_lo_reg,   saved[3]);
    WREG32(dev, st_hi_reg,   saved[4]);
    WREG32(dev, en_lo_reg,   saved[5]);
    WREG32(dev, en_hi_reg,   saved[6]);
    amdgpu_hdp_flush(dev);
    (void)gmc_flush_gpu_tlb(dev, gmc, h, /*vmid*/ 1, /*type*/ 0);
    gmc_clear_vm_faults(dev, gmc);
    gmc.vram_alloc.free(data);
    gmc.vram_alloc.free(tbl);

    GMC_LOG("vmfrag: *** sub-table %s%s, 64 KiB leaf %s *** — CONTEXT1 restored to "
            "CNTL %#010x; Apple's arena was never touched",
            t.sub_ok ? "PASSED" : "FAILED", t.inverted ? " (inverse P polarity)" : "",
            t.leaf_ok ? "PASSED" : "FAILED", saved[0]);
    if (out) *out = t;
    return (t.sub_ok && t.leaf_ok) ? kIOReturnSuccess : kIOReturnError;
}

} // namespace amdgpu

