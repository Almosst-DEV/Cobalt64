//
//  amdgpu_gmc.h — GMC v12 / MMHUB v4_1_0 / GFXHUB v12_0 interface.
//
//  Ported from lemonade-sdk/mac-amdgpu (MIT) @ commit 3bdeed2:
//      dext/amdgpu/amdgpu_gmc.h
//      dext/amdgpu/gmc_v12_0.cpp
//
//  which in turn ports a subset of:
//      drivers/gpu/drm/amd/amdgpu/gmc_v12_0.c
//      drivers/gpu/drm/amd/amdgpu/mmhub_v4_1_0.c
//      drivers/gpu/drm/amd/amdgpu/gfxhub_v12_0.c
//
//  Deviations from reference (see gmc_v12_0.cpp for the full list):
//    * DriverKit IOBufferMemoryDescriptor/IODMACommand → amdgpu::SysMem.
//    * VRAM byte offsets are relative to dev.vramBase (PORTING.md rule).
//    * `skipDisplayRisky` gate added (defaults false = reference behaviour).
//
#pragma once

#include <stdint.h>
#include <IOKit/IOReturn.h>

#include "amdgpu_ip.h"
#include "amdgpu_regs.h"
#include "amdgpu_sysmem.h"
#include "amdgpu_vram.h"
#include <IOKit/IOLocks.h>
#include "apple/gfx_tlb83.h"   // build 0.0.528: switch 83 (pure)

namespace amdgpu {

// ---- VRAM high-arena reserves: THESE TWO ARE COUPLED ------------------
//
// Read both before changing either. Navi48Ttl::getLocalMemoryInfo reports
// `reservedHiOffset = vramSizeBytes - kVramHiTailReserve` to Apple. Apple seeds
// Hardware->0x350 from it and AMDHardware::appendToReservedVRAMOffset (vtable
// slot 48) carves its page tables DOWNWARD from that value. So the top of
// Apple's region is exactly our reported hiTop, and it grows down into VRAM that
// our own vram_alloc_hi would otherwise own.
//
// Therefore the hi pool must stop BELOW Apple's carve, not at the same boundary:
//
//     vram_alloc_hi top = vramSizeBytes - kVramHiTailReserve - kAppleArenaCarveReserve
//     Apple's arena top = vramSizeBytes - kVramHiTailReserve      (what the TTL reports)
//
// History: the tail reserve was originally 512 MiB and Apple's
// tables sat at total-68 MiB, INSIDE it, so could correctly say "our
// allocator can never collide with it". lowered the reported hiTop to move
// Apple off the PSP TMR — which also moved Apple's region OUT of the reserve and
// into the hi pool, silently voiding that guarantee. The two numbers live here
// together so that cannot happen again.
//
// kVramHiTailReserve   — clears the SOS-managed PSP TMR (~0x3f860_0000 on this
//                        card; LOAD_IP_FW reported 0x83f860d000) and the on-die
//                        IP discovery table (~0x3faff0000).
// kAppleArenaCarveReserve — headroom for Apple's downward carve. One 68 MiB
//                        allocation is observed (17 SDMA CONST_FILLs,); that
//                        is measured behaviour, not a documented bound, so this
//                        is deliberately several times larger.
static constexpr uint64_t kVramHiTailReserve      = 512ull << 20;
static constexpr uint64_t kAppleArenaCarveReserve = 256ull << 20;
static constexpr uint64_t kVramHiTotalReserve     =
    kVramHiTailReserve + kAppleArenaCarveReserve;

// ---- GFXHUB VMID-0 two-level page table (option B', notes/re/
//      an internal review note Q3) ---------------------------------
//
// Apple's AMDRadeonX6000 forms VRAM addresses 0-BASED (its MQDs land at
// 0x0fffd000, its page-table arena at 0x3d6c00000). Our VRAM lives at MC
// 0x8000000000, so every one of those untranslated VMID-0 accesses misses.
//
// B' fixes it in the page tables instead of rewriting Apple's addresses: give
// GFXHUB CONTEXT0 a DEPTH=1 table whose PDB0 has 8 GiB leaves
// (PAGE_TABLE_BLOCK_SIZE=12 -> 2^(12+21)) and:
//   [0], [1]   PDE-as-PTE leaves -> VRAM physical 0 and 8 GiB  (the ALIAS:
//              MC [0, 16 GiB) now resolves to the same VRAM as MC
//              [0x8000000000, +16 GiB))
//   [64],[65]  the same two physical bases (IDENTITY for our own MC window,
//              so nothing this driver already does changes meaning)
//   [66]       a real PDE pointing at the existing flat GART PTB, so
//              MC 0x8400000000.. still resolves through the same 65536-entry
//              array and every gmc_bind_* index stays valid
// 66 * 2^33 == 0x8400000000 == gart_start and 64 * 2^33 == 0x8000000000 ==
// vram_start; gmc_pdb0_build refuses to enable if either identity fails.
//
// MMHUB is NEVER given this table — the VBIOS-programmed scanout translates
// through MMHUB and must not move.
struct PDB0SelfTest {
    bool     ran          { false };
    bool     alias_ok     { false };   // (a) CONST_FILL to a 0-based address
    bool     identity_ok  { false };   // (b) CONST_FILL to the MC address
    bool     gart_ok      { false };   // (c) CONST_FILL to a GART sysmem page
    bool     reverted     { false };   // CONTEXT0 put back on the flat table
    uint32_t alias_bad    { 0 };       // mismatching dwords
    uint32_t identity_bad { 0 };
    uint32_t gart_bad     { 0 };
    uint64_t alias_addr   { 0 };       // the 0-based address we filled
    uint64_t identity_addr{ 0 };       // the MC address we filled
    uint64_t gart_addr    { 0 };
    // GCVM_CONTEXT0_CNTL, PT_BASE_LO/HI, PT_START_LO/HI, PT_END_LO/HI
    uint32_t regs[7]      { 0, 0, 0, 0, 0, 0, 0 };
};

// Per-hub register offset table — MMHUB and GFXHUB have parallel
// register layouts so we keep a single struct shape and populate
// it differently per hub. Linux's `struct amdgpu_vmhub` is the
// upstream analogue.
//
// All offsets are SOC15-style relative-to-IP-base dword indices.
// To form a final BAR5 register offset:
//     dword_off = SOC15_REG_OFFSET(dev, IPBlock::<hub>, hub.<field>)
struct HubContext {
    bool      inited { false };
    bool      gart_enabled { false };   // set by gmc_*hub_gart_enable; gates TLB flushes on this hub

    uint32_t  ctx0_pt_base_lo  { 0 };  // GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 (or MMHUB equiv)
    uint32_t  ctx0_pt_base_hi  { 0 };
    uint32_t  ctx0_pt_start_lo { 0 };
    uint32_t  ctx0_pt_start_hi { 0 };
    uint32_t  ctx0_pt_end_lo   { 0 };
    uint32_t  ctx0_pt_end_hi   { 0 };
    uint32_t  ctx1_pt_start_lo { 0 };  // CONTEXT1 PT start/end — programmed
    uint32_t  ctx1_pt_start_hi { 0 };  // per-VMID in setup_vmid_config.
    uint32_t  ctx1_pt_end_lo   { 0 };
    uint32_t  ctx1_pt_end_hi   { 0 };
    uint32_t  ctx0_cntl        { 0 };
    uint32_t  ctx1_cntl        { 0 };
    uint32_t  vm_l2_cntl       { 0 };
    uint32_t  vm_l2_cntl2      { 0 };
    uint32_t  vm_l2_cntl3      { 0 };
    uint32_t  vm_l2_cntl4      { 0 };
    uint32_t  vm_l2_cntl5      { 0 };
    uint32_t  vm_l2_protection_fault_cntl              { 0 };
    uint32_t  vm_l2_protection_fault_cntl2             { 0 };
    uint32_t  vm_l2_protection_fault_default_addr_lo32 { 0 };
    uint32_t  vm_l2_protection_fault_default_addr_hi32 { 0 };
    // Identity aperture — closed during gart_enable
    uint32_t  vm_l2_ctx1_identity_aperture_low_lo32   { 0 };
    uint32_t  vm_l2_ctx1_identity_aperture_low_hi32   { 0 };
    uint32_t  vm_l2_ctx1_identity_aperture_high_lo32  { 0 };
    uint32_t  vm_l2_ctx1_identity_aperture_high_hi32  { 0 };
    uint32_t  vm_l2_ctx_identity_physical_offset_lo32 { 0 };
    uint32_t  vm_l2_ctx_identity_physical_offset_hi32 { 0 };
    uint32_t  vm_l1_tlb_cntl                    { 0 };
    uint32_t  vm_invalidate_eng0_req            { 0 };
    uint32_t  vm_invalidate_eng0_ack            { 0 };
    uint32_t  vm_invalidate_eng0_sem            { 0 };
    uint32_t  vm_invalidate_eng0_addr_range_lo32 { 0 };
    uint32_t  vm_invalidate_eng0_addr_range_hi32 { 0 };
    uint32_t  vm_system_aperture_low_addr       { 0 };
    uint32_t  vm_system_aperture_high_addr      { 0 };
    uint32_t  vm_system_aperture_default_addr_lo { 0 };
    uint32_t  vm_system_aperture_default_addr_hi { 0 };
    // AGP / FB location regs (MMHUB only really; GFXHUB shadows them).
    // NOTE: vm_fb_location_* / vm_fb_offset are READ-ONLY here. The
    // reference never writes them and neither do we — on this card the
    // UEFI console is scanning out of the VBIOS-programmed FB window.
    uint32_t  vm_agp_base          { 0 };
    uint32_t  vm_agp_top           { 0 };
    uint32_t  vm_agp_bot           { 0 };
    uint32_t  vm_fb_location_base  { 0 };
    uint32_t  vm_fb_location_top   { 0 };
    uint32_t  vm_fb_offset         { 0 };

    // Strides between per-VMID/per-engine register blocks. Upstream
    // computes these as `regCONTEXT1_X - regCONTEXT0_X` in the hub
    // init function. On RDNA4 these are:
    //   ctx_distance       = 1   (CONTEXT1_CNTL - CONTEXT0_CNTL)
    //   ctx_addr_distance  = 2   (CONTEXT1_PT_BASE_ADDR_LO32 - CONTEXT0_PT_BASE_ADDR_LO32)
    //   eng_distance       = 1   (INVALIDATE_ENG1_REQ - INVALIDATE_ENG0_REQ)
    //   eng_addr_distance  = 2   (INVALIDATE_ENG1_ADDR_RANGE_LO32 - INVALIDATE_ENG0_ADDR_RANGE_LO32)
    // Audit #4 F6: previously eng_distance was hard-coded to 4 — that's
    // wrong. The REQ/ACK/SEM stride is 1; only the ADDR_RANGE stride is 2.
    uint32_t  ctx_distance      { 0 };
    uint32_t  ctx_addr_distance { 0 };
    uint32_t  eng_distance      { 0 };
    uint32_t  eng_addr_distance { 0 };

    IPBlock   ip { IPBlock::MMHUB };  // which IP this hub lives in (GC for GFXHUB, MMHUB for MMHUB)

    // SOC15 BASE_IDX the whole register set above resolves through.
    // Set explicitly by {mmhub,gfxhub}_offsets_init from the asic headers
    // rather than defaulted at the call sites: every regGCVM_* / regGCMC_*
    // in gc_12_0_0_offset.h and every regMMVM_* / regMMMC_* in
    // mmhub_4_1_0_offset.h carries _BASE_IDX 0, so this is 0 for both hubs
    // — but it is now a stated fact, not an assumption baked into
    // SOC15_REG_OFFSET's default.
    int       base_idx { 0 };
};

// GMC controller state. Populated by gmc_init().
struct GMCContext {
    bool      inited { false };

    // Memory geometry. Unlike the reference (which hardcodes the R9700's
    // 32 GiB) we take real_vram_size from dev.vramSizeBytes
    // (RCC_CONFIG_MEMSIZE, 16304 MiB on the 9070 XT) — that is what
    // upstream gmc_v12_0_mc_init does via nbio.funcs->get_memsize.
    uint64_t  real_vram_size    { 0 };   // RCC_CONFIG_MEMSIZE
    uint64_t  visible_vram_size { 0 };   // BAR0 CPU aperture (256 MiB here)
    uint64_t  vram_start        { 0 };   // GPU/MC base of VRAM = dev.vramMcBase
    uint64_t  vram_end          { 0 };

    // GART aperture (system memory remapped into MC space).
    uint64_t  gart_size  { 0 };
    uint64_t  gart_start { 0 };
    uint64_t  gart_end   { 0 };

    // AGP/FB apertures
    uint64_t  fb_start  { 0 };
    uint64_t  fb_end    { 0 };
    uint64_t  agp_start { 0 };
    uint64_t  agp_end   { 0 };

    // VM manager parameters (mirrors upstream adev->vm_manager).
    uint32_t  num_level        { 0 };   // 3 for GFX12 (4-level PTs)
    uint32_t  block_size       { 0 };   // 9 — log2 of entries per block
    uint64_t  max_pfn          { 0 };   // (1 << 48) / 4 KB for GFX12
    uint64_t  vram_base_offset { 0 };   // MMMC_VM_FB_OFFSET << 24 (reads 0 on this card)

    // Free-list allocator over the visible VRAM aperture, pinned to
    // [dev.vramBase + kGMCVRAMAllocOffset, dev.vramLimit) in MC space.
    VRAMBumpAllocator vram_alloc;

    // Resources allocated at gart_init time.
    //   - GART page table: VRAM-resident (upstream amdgpu_gart_table_vram_alloc).
    //   - dummy_page / mem_scratch: DMA-able system memory (was
    //     IOBufferMemoryDescriptor + IODMACommand in the reference).
    SysMem    dummy_page  {};
    SysMem    mem_scratch {};

    uint64_t  gart_pt_bus         { 0 };  // MC address of the GART page table (in VRAM)
    void     *gart_pt_cpu         { nullptr };  // always null — CPU writes go via BAR0
    uint64_t  gart_pt_size        { 0 };  // page table size in bytes
    uint64_t  gart_pt_vram_offset { 0 };  // absolute VRAM byte offset of the PT
    uint64_t  dummy_page_bus      { 0 };  // for protection-fault redirect
    uint64_t  mem_scratch_bus     { 0 };  // default aperture address

    // Bump allocator for sysmem buffers bound into GART (e.g. firmware
    // staging). Offsets are within [gart_start, gart_end). Reset to
    // gart_bump_start at gmc_init time.
    uint64_t  gart_bump_offset { 0 };

    //. Apple's accelerator allocates from the BOTTOM of this same
    // aperture, and page 0 carries its per-channel writeback frame: the
    // completion dword at +0x00, the tail writeback at +0x10 and the COND_EXE
    // gate at +0x1c. Our bump allocator also began at 0 and took page 0 for the
    // CP ring, so gmc_bind_at REFUSED Apple's mirror of it, the GPU's FENCE
    // landed inside our ring, and Apple's completion was never written.
    //
    // gart_bump_start is the FLOOR of the region this driver has handed out.
    // The overlap guard in gmc_bind_at tests against [start, offset) rather than
    // "anything below offset", which is what makes a non-zero start usable at all.
    uint64_t  gart_bump_start  { 0 };

    // ---- GFXHUB VMID-0 PDB0 (option B') ------------------------------
    // pdb0_want is set from boot-arg navi48-pdb0 BEFORE the ladder reaches
    // CPInit (where gmc_gfxhub_gart_enable runs). Everything else here is
    // filled in by gmc_pdb0_build. Default OFF: with pdb0_want false not one
    // register write or VRAM byte below changes.
    bool      pdb0_want        { false };
    bool      pdb0_active      { false };  // built AND CONTEXT0 programmed for it
    uint64_t  pdb0_vram_offset { 0 };      // absolute VRAM byte offset of the PDB0
    uint64_t  pdb0_mc          { 0 };      // its MC address
    uint64_t  pdb0_pa          { 0 };      // vram_base_offset + mc - vram_start
    uint32_t  pdb0_block_size  { 12 };     // GCVM_CONTEXT0_CNTL.PAGE_TABLE_BLOCK_SIZE
    uint32_t  pdb0_idx_ident   { 0 };      // 64  (vram_start  >> 33)
    uint32_t  pdb0_idx_gart    { 0 };      // 66  (gart_start  >> 33)
    uint32_t  pdb0_vram_leaves { 0 };      // 2   (16304 MiB over 8 GiB leaves)

    // Set from boot-arg navi48-gart-high before the ladder runs. Off by default:
    // relocating the region moves the CP ring, MQD, WB and MES buffers, which is
    // working bring-up (stage 17, compute:passed).
    bool      gart_high_bump   { false };

    // ---- Navi48Bringup addition (not in the reference) ----------------
    // On this machine the UEFI/GOP console is LIVE in VRAM [0, 0x7e9000)
    // and the kernel keeps drawing into it. DCN translates through its own
    // DCN_VM_* registers (which we never touch), but display traffic still
    // crosses MMHUB. Setting this true skips the three MMHUB writes that
    // could plausibly disturb an in-flight scanout — the AGP triple,
    // MMMC_VM_MX_L1_TLB_CNTL, and the MMVM_L2_CNTL* cache block (incl. its
    // live invalidate). GFXHUB is never gated (the display does not use it).
    // Defaults FALSE = full reference behaviour. See gmc_v12_0.cpp.
    bool      skipDisplayRisky { false };

    // Hub register offset tables.
    // Device-only VRAM, above the BAR0 aperture. `vram_alloc` covers the part
    // of VRAM the CPU can reach through BAR0 (256 MiB here, of which ~168 MiB
    // is left after the console framebuffer, the PSP's staging areas, the GART
    // page table and fw_buf). `vram_alloc_hi` covers everything above that, up
    // to a reserved tail — which is where the PSP put the firmware TMR
    // (~0x3f8_00000 and up on this card) and where the on-die IP discovery
    // table lives. The CPU reaches this pool only through the MM_INDEX window
    // (vram_memcpy / RVRAM32_via_mm); the GPU reaches it normally.
    VRAMBumpAllocator vram_alloc_hi {};
    uint64_t      vram_hi_base { 0 };   // VRAM byte offset where the hi pool starts
    uint64_t      vram_hi_size { 0 };

    HubContext mmhub;     // MMHUB v4_1_0
    HubContext gfxhub;    // GFXHUB v12_0
};

// mc_init — port of gmc_v12_0_mc_init (gmc_v12_0.c:727). Fills in
// real_vram_size / visible_vram_size / vram_start / vram_end / fb_* /
// agp_* / gart_* and the VM-manager parameters. vram_start comes from
// the live MMHUB FB_LOCATION_BASE (dev.vramMcBase) — never reprogrammed.
kern_return_t gmc_mc_init(DeviceContext &dev, GMCContext &gmc);

// vram_alloc_init — initialize the VRAM allocator over the visible
// window above our fixed bring-up reservations. Must run after mc_init.
kern_return_t gmc_vram_alloc_init(DeviceContext &dev, GMCContext &gmc);

// Populate MMHUB v4_1_0 hub offsets. No MMIO — pure offset table init.
// Source: mmhub_v4_1_0_init (drivers/gpu/drm/amd/amdgpu/mmhub_v4_1_0.c:464)
kern_return_t gmc_mmhub_offsets_init(GMCContext &gmc);

// Populate GFXHUB v12_0 hub offsets. No MMIO.
// Source: gfxhub_v12_0_init (drivers/gpu/drm/amd/amdgpu/gfxhub_v12_0.c)
kern_return_t gmc_gfxhub_offsets_init(GMCContext &gmc);

// Zero the VRAM-resident GART page table and allocate dummy_page +
// mem_scratch in system memory. Mirrors amdgpu_gart_table_vram_alloc +
// the dummy_page / mem_scratch allocations Linux does in
// gmc_v12_0_sw_init.
kern_return_t gmc_alloc_resources(DeviceContext &dev, GMCContext &gmc);

// Free everything alloc_resources allocated.
void gmc_release_resources(GMCContext &gmc);

// Hub-level gart_enable: actual register programming. Each is a
// port of upstream mmhub_v4_1_0_gart_enable / gfxhub_v12_0_gart_enable
// composed of the sub-functions Linux structures it into:
//   init_gart_aperture_regs   — CONTEXT0 PT base/start/end
//   init_system_aperture_regs — AGP, system aperture, default page
//   init_tlb_regs             — L1 TLB on, MTYPE_UC, advanced model
//   init_cache_regs           — L2 cache fields
//   enable_system_domain      — CONTEXT0 enable
//   disable_identity_aperture — close the identity range
//   setup_vmid_config         — VMID 1..14 contexts
//   program_invalidation      — invalidation engines 0..17
kern_return_t gmc_mmhub_gart_enable(DeviceContext &dev, GMCContext &gmc);
kern_return_t gmc_gfxhub_gart_enable(DeviceContext &dev, GMCContext &gmc);

// ---- GFXHUB VMID-0 PDB0 (option B') ------------------------------------
//
// gmc_pdb0_build — validate the geometry, zero the 4 KiB PDB0 page in VRAM and
// write its 5 entries. Called from gmc_gfxhub_gart_enable BEFORE CONTEXT0 is
// programmed, and only when gmc.pdb0_want is set. On success sets
// gmc.pdb0_active; on ANY geometry mismatch it logs the reason, leaves
// pdb0_active false and returns an error — the caller then programs the flat
// single-level table exactly as before. Never touches MMHUB.
kern_return_t gmc_pdb0_build(DeviceContext &dev, GMCContext &gmc);

// gmc_pdb0_revert — put GFXHUB CONTEXT0 back on the flat GART page table
// (DEPTH=0, BLOCK_SIZE=0, BASE = the GART PTB, START = gart_start) and flush
// HDP + the GFXHUB TLB. Used by the self-test when the identity or GART leg
// fails, so the machine stays usable. Clears gmc.pdb0_active.
kern_return_t gmc_pdb0_revert(DeviceContext &dev, GMCContext &gmc);

// gmc_pdb0_read_regs — read GCVM_CONTEXT0_CNTL (0x1624), PAGE_TABLE_BASE_ADDR
// LO32/HI32 (0x168f/0x1690), START LO32/HI32 (0x16af/0x16b0) and END LO32/HI32
// (0x16cf/0x16d0) into out[7]. Read-only.
void gmc_pdb0_read_regs(const DeviceContext &dev, const GMCContext &gmc,
                        uint32_t out[7]);

// HDP flush — port of amdgpu_device_flush_hdp / amdgpu_hdp_generic_flush
// (amdgpu_hdp.c:48-54). Used after writing PTEs or after gart_enable so
// the GPU sees fresh state.
kern_return_t gmc_hdp_flush(DeviceContext &dev);

// flush_gpu_tlb — uses the invalidation engines hub_program_invalidation
// armed. Writes a request to VM_INVALIDATE_ENG17_REQ, polls ACK.
// vmid: 0..15 (0 = system, 1..14 = user, 15 = all)
// flush_type: 0 = legacy, 1 = light-weight, 2 = heavyweight
kern_return_t gmc_flush_gpu_tlb(DeviceContext &dev, const GMCContext &gmc,
                                const HubContext &hub,
                                uint32_t vmid, uint32_t flush_type);

// build 0.0.528 (; apple/gfx_tlb83.h) — SWITCH 83, gmc_flush_gpu_tlb's ack wait. gTlb83On: the mode (OFF at boot;
// written only by navi48_tlb83_switch, Navi48Bringup.cpp). gTlb83Lock: the LEAF lock taken around the request write and the ack
// wait while ON (allocated in Navi48Bringup::start, never freed). gTlb83S: the counters (item 2) and the `acked` line cap (item 3).
extern volatile uint32_t gTlb83On;
extern IOLock *gTlb83Lock;
extern n48_tlb83_stats gTlb83S;

// set_fault_enable_default — port of mmhub_v4_1_0_set_fault_enable_default
// + gfxhub_v12_0_set_fault_enable_default. Programs
// MMVM_L2_PROTECTION_FAULT_CNTL / GCVM_L2_PROTECTION_FAULT_CNTL. Called
// after gart_enable with value=true.
kern_return_t gmc_set_fault_enable_default(DeviceContext &dev,
                                           const HubContext &hub, bool value);

// Top-level GMCInit stage entry (the GMC hw_init) — runs the chain.
kern_return_t gmc_init(DeviceContext &dev, GMCContext &gmc);

// Diagnostics: read (do not clear) both hubs' L2 protection-fault status/address registers
// (gc_12_0_0: GCVM_L2_PROTECTION_FAULT_STATUS_LO32 0x15d0..ADDR_HI32 0x15d3; mmhub_4_1_0: MMVM 0x04f0..0x04f3).
void gmc_log_vm_faults(DeviceContext &dev, const GMCContext &gmc);
// Pulse CLEAR_PROTECTION_FAULT_STATUS_ADDR on both hubs (Linux WREG32_P(cntl, 1, ~1)).
void gmc_clear_vm_faults(DeviceContext &dev, const GMCContext &gmc);
// WALKER_ERROR[3:1] of GCVM_L2_PROTECTION_FAULT_STATUS_LO32 as a name
// (gc_12_0_0_sh_mask.h:9002-9025): none/RANGE/PDE0/PDE1/PDE2/TRANSLATE-FURTHER/
// NACK/DUMMY-PAGE. The status is FIRST-FAULT LATCHED — clear it before a
// measurement or you are reading an older fault.
const char *gmc_walker_error_name(uint32_t we);

// 0.0.193 — the Apple-shaped-table-in-gfx12-encoding self-test
// (boot-arg navi48-vmfrag-test=1). See gmc_v12_0.cpp for the full argument.
// Builds a DEPTH 1 / BLOCK_SIZE 7 tree of our own in our own VRAM, programs
// GFXHUB CONTEXT1 (VMID 1, which nothing else on this machine uses), copies
// through a 16-entry sub-table and through a 64 KiB leaf with SDMA IBs under
// VMID 1, restores CONTEXT1 and frees everything. Apple's arena is untouched.
struct SDMAInstance;
struct VMFragTestResult {
    bool     ran       { false };
    bool     sub_ok    { false };   // leg (a): 4 KiB PTE via a 16-entry sub-table
    bool     leaf_ok   { false };   // leg (b): the 64 KiB leaf at L1
    bool     inverted  { false };   // leg (a) needed the inverse P polarity
    uint32_t sub_bad   { 0 };
    uint32_t leaf_bad  { 0 };
    uint64_t table_mc  { 0 };
    uint64_t data_mc   { 0 };
    uint64_t va_start  { 0 };
    uint64_t sub_va    { 0 };
    uint64_t leaf_va   { 0 };
    uint32_t ctx1_cntl { 0 };       // what CONTEXT1_CNTL read back (want 0x03fffd73)
    uint32_t fault_lo  { 0 };       // STATUS_LO32 after the last leg
    uint32_t fault_walker { 0 };
    // M4-WS-VMID-VALID: leg (c), ONLY with navi48-vmfrag-novalid=1 as well - the 64 KiB leaf copy again, with CONTEXT1's base
    // written WITHOUT the VALID bit. Does gfx12's walker check bit 0 of the page-table base? Our own IB, no Apple client.
    bool     nv_ran    { false };
    bool     nv_ok     { false };   // the copy completed: gfx12 did NOT refuse the bare base
    uint32_t nv_bad    { 0 };
    uint32_t nv_base_lo{ 0 };       // CONTEXT1 BASE LO as read back for the leg (bit 0 must read 0)
    uint32_t nv_fault_lo { 0 };     // STATUS_LO32 after leg (c)
    uint32_t nv_fault_addr_lo { 0 };
};
// noValidLeg: leg (c) above. Default false: navi48-vmfrag-test=1 alone runs exactly the 0.0.193 legs.
kern_return_t gmc_vmfrag_selftest(DeviceContext &dev, GMCContext &gmc,
                                  SDMAInstance &inst, VMFragTestResult *out,
                                  bool noValidLeg = false);

// gmc_bind_existing — write GART PTEs for an existing DMA-able bus
// address range into the VRAM-resident page table. Returns the GART
// MC address a GPU IP (e.g. PSP) uses to reach the bound buffer. PTE
// writes use the BAR0 aperture (CPU → MMIO → VRAM). Bumps the allocator.
kern_return_t gmc_bind_existing(DeviceContext &dev, GMCContext &gmc,
                                uint64_t busAddr, uint64_t sizeBytes,
                                uint64_t *outMcAddr);

// gmc_bind_at — the same GFX12 PTE write, but at a GART offset the CALLER
// chooses instead of one taken from the bump allocator.
//
// This exists for Apple's accelerator. It runs its own allocator inside the
// aperture our TTL reports, so it picks its own offsets and then expects them to
// be mapped: measured, it placed an SDMA ring at GART offset
// 0x220000 and its command buffers around 0x800000. gmc_bind_existing cannot
// serve that — it hands out the next bump offset, not the one asked for.
//
// The caller is responsible for not overlapping ranges this driver allocated
// itself; the bump allocator and these fixed bindings share one page table.
// Overlap is refused rather than silently corrupting the other owner's PTEs.
kern_return_t gmc_bind_at(DeviceContext &dev, GMCContext &gmc,
                          uint64_t gartOffset, uint64_t busAddr,
                          uint64_t sizeBytes);

// 0.0.368: how many of those overlap refusals happened this boot.
// RULE E1 (apple/gfx_e1.h) requires ZERO before X9 v2's EARLY observer may go
// live: one refusal means something of Apple's tried to map over the pages this
// driver owns, and "only this kext writes the clear-state page" stops being an
// observation. Latched, never cleared.
uint64_t gmc_bind_at_overlap_refusals(void);

} // namespace amdgpu
