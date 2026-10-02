// VERBATIM from lemonade-sdk/mac-amdgpu (MIT) dext/amdgpu/amdgpu_ip.h @ 3bdeed2 — platform-independent; keep in sync, do not fork.
//
//  amdgpu_ip.h — IP version pinning + IP base-address table.
//
//  In upstream Linux, IP base addresses come from the runtime IP
//  discovery table on the GPU (see amdgpu_discovery.c) because one
//  driver supports dozens of ASICs. We support exactly one ASIC
//  (Radeon AI PRO R9700, gfx1201) so we pin the versions at compile
//  time and either:
//      (a) hardcode the base offsets once we know them, or
//      (b) read them from the discovery binary on first init and
//          stash them in the global IPBaseTable.
//
//  Initial commit uses placeholder 0xFFFFFFFFu sentinels for any
//  base address we haven't read off real hardware yet. The PSP /
//  SMU / GFX bringup code asserts the bases are filled before use,
//  so a missing entry fails loud rather than reading register 0.
//

#pragma once

#include <stdint.h>

namespace amdgpu {

// IP versions on Radeon AI PRO R9700 (gfx1201) — confirmed from
// the devcoredump in qemu-vfio-apple/traces/:
//
//   HWIP: GC[1][0]:    v12.0.1.0.0   → gfx_v12_1
//   HWIP: HDP[2][0]:   v7.0.0.0.0
//   HWIP: SDMA0[3][0]: v7.0.1.0.0    → sdma_v7_1
//   HWIP: SDMA1[4][0]: v7.0.1.0.0
//
// PSP / SMU / MES inferred from linux-firmware blob versions
// (psp_14_0_3_*, smu_14_0_3*, gc_12_0_1_mes*):
//
//   PSP:  v14.0.3
//   SMU:  v14.0.3
//   MES:  v12.0.1
//   NBIO: v7.11
//
struct IPVersion {
    uint8_t major;
    uint8_t minor;
    uint8_t rev;
};

constexpr IPVersion kIP_GFX  = { 12, 0, 1 };
constexpr IPVersion kIP_GMC  = { 12, 0, 0 };
constexpr IPVersion kIP_SDMA = { 7,  0, 1 };
constexpr IPVersion kIP_PSP  = { 14, 0, 3 };
constexpr IPVersion kIP_SMU  = { 14, 0, 3 };
constexpr IPVersion kIP_MES  = { 12, 0, 1 };
constexpr IPVersion kIP_NBIO = { 7,  11, 0 };
constexpr IPVersion kIP_IH   = { 7,  0, 0 };
constexpr IPVersion kIP_HDP  = { 7,  0, 0 };

// Hardware IP enum mirrors Linux amdgpu_ip_block.h.
enum class IPBlock : uint8_t {
    GC = 0,    // graphics + compute
    HDP,       // host data path
    SDMA0,
    SDMA1,
    MP0,       // PSP lives here
    MP1,       // SMU lives here
    NBIO,
    OSSSYS,    // IH lives here
    GMC,
    MMHUB,     // owns regMMMC_VM_FB_LOCATION_BASE — needed to find vram_start
    Count,
};

// IP base addresses in BAR0/BAR5-relative dword offsets. Each IP
// block has UP TO kMaxBaseSegments (5) base addresses — one per
// SOC15 BASE_IDX. Upstream registers declare their BASE_IDX
// alongside their offset (e.g. `regMP1_SMN_C2PMSG_*_BASE_IDX 1`),
// so the correct address is `base[BLK][BASE_IDX] + reg_offset`.
//
// Practical examples on R9700:
//   MP0  / regMPASP_SMN_C2PMSG_*   BASE_IDX 0
//   MP1  / regMP1_SMN_C2PMSG_*     BASE_IDX 1   (SMU mailbox)
//   NBIO / regBIF_BX0_REMAP_HDP_*  BASE_IDX 5   (HDP flush remap)
//
// Sentinel: 0xFFFFFFFFu means "not yet read from discovery". A real
// IP base is never 0 on a PCIDriverKit-mapped BAR (SMN registers
// start in the 0x40000+ range after the SMUIO front-end block).
struct IPBaseTable {
    // Some IPs (esp. NBIO on RDNA4) declare BASE_IDX up to 5 — see
    // regBIF_BIF256_CI256_RC3X4_USB4_PCIE_MST_CTRL_3_BASE_IDX=5 in
    // nbio_7_11_0_offset.h. That means we need slot index 5, i.e. at
    // least 6 entries. Use 8 for some headroom.
    static constexpr int kMaxBaseSegments = 8;
    uint32_t base[(int)IPBlock::Count][kMaxBaseSegments];

    // Discovered IP version per block (major.minor.rev). Populated by
    // amdgpu_discovery.cpp during on-die discovery walk. Used by code
    // that needs to switch register tables / function tables based on
    // the chip's actual IP version (e.g. gc_12_0_0 vs gc_12_1_0 offsets,
    // nbif_v6_3_1 vs nbio_v7_11 funcs, _nbif_4_10 variant for 7.11.4).
    //
    // [[feedback_mac_amdgpu_per_ip_version_offsets]] — selecting offsets
    // at runtime is non-negotiable. Don't hardcode for one chip.
    IPVersion version[(int)IPBlock::Count];

    constexpr IPBaseTable() : base{}, version{} {
        for (int i = 0; i < (int)IPBlock::Count; i++) {
            for (int j = 0; j < kMaxBaseSegments; j++) {
                base[i][j] = 0xFFFFFFFFu;
            }
            version[i] = {0, 0, 0};
        }
    }

    // IIG's IONewZero zero-fills the parent struct without running
    // C++ ctors, so base[] arrives as 0 instead of 0xFFFFFFFFu.
    // Treat both as "unresolved".
    bool isResolved(IPBlock block) const {
        uint32_t b = base[(int)block][0];
        return b != 0xFFFFFFFFu && b != 0u;
    }
    bool isResolved(IPBlock block, int baseIdx) const {
        if (baseIdx < 0 || baseIdx >= kMaxBaseSegments) return false;
        uint32_t b = base[(int)block][baseIdx];
        return b != 0xFFFFFFFFu && b != 0u;
    }
    // Default BASE_IDX = 0 for backward compatibility with code that
    // hasn't been threaded with a baseIdx yet.
    uint32_t get(IPBlock block) const { return base[(int)block][0]; }
    void set(IPBlock block, uint32_t b) { base[(int)block][0] = b; }
    // Explicit BASE_IDX accessors for new code (NBIO HDP remap, SMU
    // MP1 mailbox, etc.). idx clamped to [0, kMaxBaseSegments).
    uint32_t getBase(IPBlock block, int baseIdx) const {
        if (baseIdx < 0 || baseIdx >= kMaxBaseSegments) return 0xFFFFFFFFu;
        return base[(int)block][baseIdx];
    }
    void setBase(IPBlock block, int baseIdx, uint32_t b) {
        if (baseIdx < 0 || baseIdx >= kMaxBaseSegments) return;
        base[(int)block][baseIdx] = b;
    }

    // IP version accessors. Discovery populates these from the on-die
    // binary's major/minor/revision fields. A version of {0,0,0} means
    // "unknown" (discovery hasn't run or block not present).
    IPVersion getVersion(IPBlock block) const { return version[(int)block]; }
    void setVersion(IPBlock block, IPVersion v) { version[(int)block] = v; }
    bool isVersion(IPBlock block, uint8_t maj, uint8_t min, uint8_t rev) const {
        const IPVersion &v = version[(int)block];
        return v.major == maj && v.minor == min && v.rev == rev;
    }
    // Inclusive range check on (major, minor, rev) — useful for
    // matching across a family of chip revisions.
    bool isVersionRange(IPBlock block,
                        uint8_t lo_maj, uint8_t lo_min, uint8_t lo_rev,
                        uint8_t hi_maj, uint8_t hi_min, uint8_t hi_rev) const {
        const IPVersion &v = version[(int)block];
        const uint32_t pack = (uint32_t)v.major << 16 |
                              (uint32_t)v.minor << 8 | v.rev;
        const uint32_t lo = (uint32_t)lo_maj << 16 |
                            (uint32_t)lo_min << 8 | lo_rev;
        const uint32_t hi = (uint32_t)hi_maj << 16 |
                            (uint32_t)hi_min << 8 | hi_rev;
        return pack >= lo && pack <= hi;
    }
};

// SOC15 register offsets from upstream Linux —
// drivers/gpu/drm/amd/include/asic_reg/mp/mp_14_0_2_offset.h:
//
//   regMPASP_SMN_C2PMSG_35  = 0x0063  (bootloader cmd / status)
//   regMPASP_SMN_C2PMSG_36  = 0x0064  (binary fw_pri_mc_addr >> 20)
//   regMPASP_SMN_C2PMSG_64  = 0x0080  (ring create cmd)
//   regMPASP_SMN_C2PMSG_69  = 0x0085  (ring low addr)
//   regMPASP_SMN_C2PMSG_70  = 0x0086  (ring high addr)
//   regMPASP_SMN_C2PMSG_71  = 0x0087  (ring size)
//   regMPASP_SMN_C2PMSG_81  = 0x0091  (SOS sign-of-life)
//   regMPASP_SMN_C2PMSG_101 = 0x00A5  (ring destroy, SR-IOV)
//   regMPASP_SMN_C2PMSG_102 = 0x00A6  (SR-IOV ring low)
//   regMPASP_SMN_C2PMSG_103 = 0x00A7  (SR-IOV ring high)
//
// All added to the MP0 IP base.
namespace MP0Regs {
    constexpr uint32_t C2PMSG_35  = 0x0063;
    constexpr uint32_t C2PMSG_36  = 0x0064;
    constexpr uint32_t C2PMSG_64  = 0x0080;
    constexpr uint32_t C2PMSG_67  = 0x0083;  // PSP ring wptr (non-SR-IOV)
    constexpr uint32_t C2PMSG_69  = 0x0085;
    constexpr uint32_t C2PMSG_70  = 0x0086;
    constexpr uint32_t C2PMSG_71  = 0x0087;
    constexpr uint32_t C2PMSG_81  = 0x0091;
    constexpr uint32_t C2PMSG_101 = 0x00A5;
    constexpr uint32_t C2PMSG_102 = 0x00A6;
    constexpr uint32_t C2PMSG_103 = 0x00A7;
}

// SMU (MP1) mailbox registers from mp_14_0_2_offset.h. SMU is the
// PMFW on RDNA4 — handles clocks, voltages, power management, link
// training. Mailbox protocol (smu_cmn_send_smc_msg_with_param):
//   1. WREG32(C2PMSG_90, 0)            — clear response slot
//   2. WREG32(C2PMSG_82, param)        — parameter
//   3. WREG32(C2PMSG_66, msg_id)       — kicks SMU
//   4. poll C2PMSG_90 != 0             — response = status code
//   5. RREG32(C2PMSG_82)               — read return value
namespace MP1Regs {
    constexpr uint32_t C2PMSG_66 = 0x0082;  // message id (host → SMU)
    constexpr uint32_t C2PMSG_82 = 0x0092;  // parameter / return value
    constexpr uint32_t C2PMSG_90 = 0x009A;  // response (SMU → host)
}

// MMHUB registers — owns the VRAM/framebuffer location in MC space.
// Used to compute the GPU-MC address of any VRAM offset so PSP /
// other firmware can DMA-read from it via the GMC internal path.
//
// Offsets from upstream asic_reg/mmhub/mmhub_4_1_0_offset.h (RDNA4).
// Older NBIO families (4_1_0, etc) use the same offset but different
// IP BASE_IDX — we use BASE_IDX 0 which is what discovery reports.
//
// `regMMMC_VM_FB_LOCATION_BASE & 0x00FFFFFF` << 24 = vram_start MC addr.
namespace MMHUBRegs {
    constexpr uint32_t MMMC_VM_FB_LOCATION_BASE = 0x0554;
    constexpr uint32_t MMMC_VM_FB_LOCATION_TOP  = 0x0555;
    // FB_OFFSET is in the LOW MMHUB-cfg block at 0x04c7, NOT colocated
    // with FB_LOCATION. We previously had 0x0556 which is actually
    // regMMMC_VM_AGP_TOP — a different register entirely.
    constexpr uint32_t MMMC_VM_FB_OFFSET        = 0x04c7;
    constexpr uint32_t kFBBaseMask = 0x00FFFFFFu;  // low 24 bits
    constexpr uint32_t kFBBaseShift = 24;          // <<24 to get MC addr

    // GART setup registers from mmhub_4_1_0_offset.h (RDNA4 NBIO 7_11
    // family). All BASE_IDX 0 — same IP base as MMMC_VM_FB_LOCATION_BASE.
    // Used by gart_enable to point the GPU's GMC at our page table and
    // define the GART aperture in MC space.
    constexpr uint32_t MMVM_CONTEXT0_CNTL                       = 0x0564;
    constexpr uint32_t MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32  = 0x05cf;
    constexpr uint32_t MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32  = 0x05d0;
    constexpr uint32_t MMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32 = 0x05ef;
    constexpr uint32_t MMVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32 = 0x05f0;
    constexpr uint32_t MMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32   = 0x060f;
    constexpr uint32_t MMVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32   = 0x0610;
    // AGP/system-aperture/L2 register offsets — all per
    // mmhub_4_1_0_offset.h. Previous values for AGP_BASE/BOT/TOP were
    // wrong (had them at 0x055c/0x055d/0x055e); upstream is
    // 0x0558/0x0557/0x0556 — note the BOT/TOP/BASE non-sequential
    // ordering. SYSTEM_APERTURE_DEFAULT_{LSB,MSB} are in the LOW
    // MMHUB-cfg block, NOT colocated with the rest.
    constexpr uint32_t MMMC_VM_AGP_TOP                          = 0x0556;
    constexpr uint32_t MMMC_VM_AGP_BOT                          = 0x0557;
    constexpr uint32_t MMMC_VM_AGP_BASE                         = 0x0558;
    constexpr uint32_t MMMC_VM_SYSTEM_APERTURE_LOW_ADDR         = 0x0559;
    constexpr uint32_t MMMC_VM_SYSTEM_APERTURE_HIGH_ADDR        = 0x055a;
    constexpr uint32_t MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB = 0x04c8;
    constexpr uint32_t MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB = 0x04c9;
    constexpr uint32_t MMMC_VM_MX_L1_TLB_CNTL                   = 0x055b;
    constexpr uint32_t MMVM_L2_CNTL                             = 0x04e4;
    constexpr uint32_t MMVM_L2_CNTL2                            = 0x04e5;
    constexpr uint32_t MMVM_L2_CNTL3                            = 0x04e6;
    constexpr uint32_t MMVM_L2_CNTL4                            = 0x04fd;
    constexpr uint32_t MMVM_L2_CNTL5                            = 0x0503;

    // Registers needed for the rest of mmhub_v4_1_0_gart_enable.
    constexpr uint32_t MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB_AT_4C8 = 0x04c8;
    constexpr uint32_t MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB_AT_4C9 = 0x04c9;
    constexpr uint32_t MMVM_L2_PROTECTION_FAULT_CNTL2           = 0x04ed;
    constexpr uint32_t MMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32 = 0x04f4;
    constexpr uint32_t MMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32 = 0x04f5;
    constexpr uint32_t MMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32  = 0x04f7;
    constexpr uint32_t MMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32  = 0x04f8;
    constexpr uint32_t MMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32 = 0x04f9;
    constexpr uint32_t MMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_HI32 = 0x04fa;
    constexpr uint32_t MMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_LO32     = 0x04fb;
    constexpr uint32_t MMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32     = 0x04fc;
    // Note: MMMC_VM_SYSTEM_APERTURE_{LOW,HIGH,DEFAULT_ADDR_*} have two
    // different offset locations in the header. The ones used by
    // mmhub_v4_1_0_init_system_aperture_regs are AT_4C8/AT_4C9. The
    // similar-named regs at 0x559/0x55a/0x55f/0x560 are for a different
    // context and not touched by init_system_aperture_regs.
    //
    // VMID config + invalidation engine — per-context regs accessed
    // via base + i * ctx_distance. Upstream uses ctx_distance =
    // MMVM_CONTEXT1_CNTL - MMVM_CONTEXT0_CNTL.
    constexpr uint32_t MMVM_CONTEXT1_CNTL                       = 0x0565;
    constexpr uint32_t MMVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32 = 0x05f1;
    constexpr uint32_t MMVM_CONTEXT1_PAGE_TABLE_START_ADDR_HI32 = 0x05f2;
    constexpr uint32_t MMVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32   = 0x0611;
    constexpr uint32_t MMVM_CONTEXT1_PAGE_TABLE_END_ADDR_HI32   = 0x0612;
    // Invalidate engine 0 (the only engine we use for CONTEXT0).
    // Upstream eng_distance = 1 (REQ/ACK/SEM are stride-1 dwords),
    // eng_addr_distance = 2 (ADDR_RANGE_LO/HI are 2 dwords apart per
    // engine). REQ/ACK/SEM offsets per mmhub_4_1_0_offset.h:
    constexpr uint32_t MMVM_INVALIDATE_ENG0_SEM                 = 0x0575;
    constexpr uint32_t MMVM_INVALIDATE_ENG0_REQ                 = 0x0587;
    constexpr uint32_t MMVM_INVALIDATE_ENG0_ACK                 = 0x0599;
    constexpr uint32_t MMVM_INVALIDATE_ENG0_ADDR_RANGE_LO32     = 0x05ab;
    constexpr uint32_t MMVM_INVALIDATE_ENG0_ADDR_RANGE_HI32     = 0x05ac;
}

// PTE flag bits — drivers/gpu/drm/amd/amdgpu/amdgpu_vm.h.
// PTEs are 64-bit: high bits = host physical address (page-aligned),
// low bits = flags. For GART-mapped sysmem we use VALID|SYSTEM|R|W.
namespace PTEFlags {
    constexpr uint64_t VALID     = (1ULL << 0);
    constexpr uint64_t SYSTEM    = (1ULL << 1);  // sysmem (not VRAM)
    constexpr uint64_t SNOOPED   = (1ULL << 2);
    constexpr uint64_t TMZ       = (1ULL << 3);
    constexpr uint64_t EXECUTABLE = (1ULL << 4);
    constexpr uint64_t READABLE  = (1ULL << 5);
    constexpr uint64_t WRITEABLE = (1ULL << 6);
    constexpr uint64_t FRAG_4K   = 0;
    // GFX12 / RDNA4 PTE bit. With PAGE_TABLE_DEPTH=0 (single-level
    // page table), GMC distinguishes PTEs from sub-PDE pointers by
    // bit 63. Every PTE we write MUST have this set, otherwise GMC
    // walks the entry as if it points to another PDE level and faults
    // on the resulting garbage address. amdgpu_vm.h:133.
    constexpr uint64_t IS_PTE    = (1ULL << 63);
    // GFX12 / RDNA4 MTYPE field is at bits 54-55 per upstream
    // amdgpu_vm.h:125: #define AMDGPU_PTE_MTYPE_GFX12_SHIFT(mtype)
    // ((uint64_t)(mtype) << 54). MTYPE=2 = Uncached (UC) per upstream
    // AMDGPU_PTE_MTYPE_GFX12(0ULL, MTYPE_UC) at gmc_v12_0.c:799.
    //
    // Prior to this fix we had `(2ULL << 57)` which planted MTYPE in
    // bits 58-59 (above the actual MTYPE field). PSP's PT walker then
    // saw MTYPE=0 (NC=cached) plus a stray set bit at 58, which fed
    // scrambled bytes into the signature check and made every
    // GART-bound LOAD_IP_FW reject with resp=0x11.
    constexpr uint64_t MTYPE_GFX12_UC = (2ULL << 54);
    // Standard sysmem mapping for PSP-readable buffers. Adds IS_PTE
    // (mandatory on GFX12) + EXECUTABLE + MTYPE_GFX12_UC to match
    // upstream gart_pte_flags defaults (amdgpu_vm.h:97-106,
    // gmc_v12_0.c:799).
    constexpr uint64_t SYSMEM_RW = VALID | SYSTEM | SNOOPED |
                                   EXECUTABLE | READABLE | WRITEABLE |
                                   IS_PTE | MTYPE_GFX12_UC;
}

// AMDGPU GPU page size is fixed at 4 KB regardless of CPU page size.
// Apple Silicon CPU is 16 KB pages so each CPU page maps 4 GPU PTEs.
constexpr uint32_t kAMDGPUGPUPageSize  = 4096;
constexpr uint32_t kAMDGPUGPUPageShift = 12;

// PPSMC messages — drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/
// smu_v14_0_2_ppsmc.h. Tiny subset; expand as we wire up features.
//
// v0.1.20 added the feature-enable + driver-table-location + RunDcBtc
// messages needed to mirror upstream smu_smc_hw_setup (amdgpu_smu.c:1662)
// so PMFW transitions out of bootup-idle and unblocks the IMU autoload
// state machine (BOOTLOAD_STATUS poll in gfx_v12_0_hw_init).
namespace PPSMC {
    constexpr uint32_t TestMessage                = 0x01;
    constexpr uint32_t GetSmuVersion              = 0x02;
    constexpr uint32_t GetDriverIfVersion         = 0x03;
    constexpr uint32_t SetAllowedFeaturesMaskLow  = 0x04;
    constexpr uint32_t SetAllowedFeaturesMaskHigh = 0x05;
    constexpr uint32_t EnableAllSmuFeatures       = 0x06;
    constexpr uint32_t DisableAllSmuFeatures      = 0x07;
    constexpr uint32_t GetRunningSmuFeaturesLow   = 0x0C;
    constexpr uint32_t GetRunningSmuFeaturesHigh  = 0x0D;
    constexpr uint32_t SetDriverDramAddrHigh      = 0x0E;
    constexpr uint32_t SetDriverDramAddrLow       = 0x0F;
    constexpr uint32_t SetToolsDramAddrHigh       = 0x10;
    constexpr uint32_t SetToolsDramAddrLow        = 0x11;
    constexpr uint32_t TransferTableSmu2Dram      = 0x12;
    constexpr uint32_t TransferTableDram2Smu      = 0x13;
    // v0.1.29 — pptable defaulting. PMFW falls back to the IFWI-baked
    // default powerplay table on this message; gives us a real fan
    // curve + populates the chip-specific allowed-feature mask.
    constexpr uint32_t UseDefaultPPTable          = 0x14;
    // v0.1.29 — per-state GFXCLK soft-clamp messages. Encoding is
    // (clk_id << 16) | freq_mhz; clk_id=0 == PPCLK_GFXCLK on v14.
    // See smu_v14_0_set_soft_freq_limited_range (smu_v14_0.c:1099).
    constexpr uint32_t SetSoftMinByFreq           = 0x19;
    constexpr uint32_t SetSoftMaxByFreq           = 0x1A;
    // v0.1.29 — AC/DC source notify. Upstream smu_smc_hw_setup sends
    // this after RunDcBtc. Param: 0=DC, 1=AC. Opcode per upstream
    // smu_v14_0_2_ppsmc.h (#define PPSMC_MSG_NotifyPowerSource 0x35).
    constexpr uint32_t NotifyPowerSource          = 0x35;
    constexpr uint32_t RunDcBtc                   = 0x36;
}

// PPCLK enum (subset) — drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/
// smu14_driver_if_v14_0.h:456. clk_id is encoded in the high 16 bits
// of the PPSMC SetSoftMin/Max param.
namespace PPCLK {
    constexpr uint32_t GFXCLK = 0;
    constexpr uint32_t SOCCLK = 1;
}

// Linux SMU mailbox response codes — smu_msg_v1_decode_response().
namespace SMUResp {
    constexpr uint32_t OK              = 0x01;
    constexpr uint32_t Failed          = 0xFF;
    constexpr uint32_t UnknownCmd      = 0xFE;
    constexpr uint32_t CmdRejectedPrereq = 0xFD;
    constexpr uint32_t CmdRejectedBusy = 0xFC;
}

// PSP bootloader commands —
// drivers/gpu/drm/amd/amdgpu/amdgpu_psp.h (enum psp_bootloader_cmd).
namespace PSPBootloaderCmd {
    constexpr uint32_t LoadKeyDatabase  = 0x80000;
    constexpr uint32_t LoadTosSPLTable  = 0x10000000;
    constexpr uint32_t LoadSysDrv       = 0x10000;
    constexpr uint32_t LoadSocDrv       = 0xB0000;
    constexpr uint32_t LoadIntfDrv      = 0xD0000;
    constexpr uint32_t LoadHADDrv       = 0xC0000;
    constexpr uint32_t LoadRASDrv       = 0xE0000;
    constexpr uint32_t LoadIPKeyMgrDrv  = 0x0F0000;  // upstream PSP_BL__LOAD_IPKEYMGRDRV
    constexpr uint32_t LoadSOSDrv       = 0x20000;
}

constexpr uint32_t kPSPBootloaderReadyBit = 0x80000000u;
constexpr uint32_t kPSPFwPriBufSize       = 1024u * 1024u;  // PSP_1_MEG

// ============================================================
// Bootstrap registers — absolute BAR0 dword offsets that don't
// require any IP base to be resolved.
//
// These are the chicken-and-egg solver: amdgpu_discovery.c reads
// them via plain RREG32(mm...) (no SOC15 wrapper) to bootstrap the
// IP base table. On RDNA4 / gfx1201 the same legacy absolute
// aliases still resolve to the right physical registers.
//
// Sources:
//   drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c:140-148
//
// mmRCC_CONFIG_MEMSIZE  : VRAM size in MB.
// mmDRIVER_SCRATCH_0    : TMR offset LO (PSP-written; sysmem-TMR override)
// mmDRIVER_SCRATCH_1    : TMR offset HI
// mmDRIVER_SCRATCH_2    : TMR size in bytes (0 if no sysmem override)
//
// Discovery binary location (per amdgpu_discovery_get_tmr_info):
//   if DRIVER_SCRATCH_2 != 0:
//       offset = (DRIVER_SCRATCH_1 << 32) | DRIVER_SCRATCH_0
//       size   = DRIVER_SCRATCH_2
//   else:
//       offset = (vram_size_mb << 20) - DISCOVERY_TMR_OFFSET
//       size   = DISCOVERY_TMR_SIZE
// ============================================================
namespace BootstrapRegs {
    // BAR0-absolute dword offsets — these are the upstream "legacy
    // aliases" that work pre-IP-discovery. Same offset across NBIO
    // 6_1 / 7_0 / 7_4 / 7_11. They become valid only AFTER IFWI
    // init completes (poll MP0_C2PMSG_33 bit 31).
    constexpr uint32_t RCC_CONFIG_MEMSIZE = 0x0DE3;
    constexpr uint32_t DRIVER_SCRATCH_0   = 0x0094;
    constexpr uint32_t DRIVER_SCRATCH_1   = 0x0095;
    constexpr uint32_t DRIVER_SCRATCH_2   = 0x0096;

    // MP0 PSP bootloader-ready handshake register. Upstream amdgpu uses
    // mmMP0_SMN_C2PMSG_35 bit 31 as the authoritative "PSP bootloader
    // is alive" signal; psp_v14_0_wait_for_bootloader / psp_v*_wait_for_*
    // all poll C2PMSG_35 specifically. C2PMSG_33 is not used by upstream
    // as a ready gate on RDNA4 (mp_14_0 has dropped its use).
    //
    // Both are pre-IP-base "legacy alias" dword offsets and read identically
    // regardless of IP base resolution. We use C2PMSG_35 here so the
    // IFWI poll matches the gate PSP itself drives.
    // Offset matches mp_11_5_0 / mp_13_0_* / mp_14_0_* ASIC headers
    // (regMPASP_SMN_C2PMSG_35 = 0x0063).
    //
    // Audit #9 #2: was previously C2PMSG_33 (0x0061) — wrong register.
    constexpr uint32_t MP0_C2PMSG_35      = 0x0063;
    constexpr uint32_t kIFWIReadyMask     = 0x80000000u;
    constexpr uint32_t kIFWIReadyValue    = 0x80000000u;
    constexpr uint64_t kIFWITimeoutMs     = 2000;

    // MP0 SOS sign-of-life. Upstream soc24_need_reset_on_init checks
    // this register: if non-zero, sOS / driver were already loaded
    // (warm reboot / kernel re-load) and we need to FLR; if zero,
    // the card is cold-booted and FLR is unnecessary.
    // Same dword offset across mp_11_5_0 / mp_13_0_* / mp_14_0_*
    // (regMPASP_SMN_C2PMSG_81 = 0x0091).
    // Audit #9 #3.
    constexpr uint32_t MP0_C2PMSG_81      = 0x0091;
}

// NBIO registers — RDNA4 NBIO 7_11 family.
// Offsets from upstream asic_reg/nbio/nbio_7_11_0_offset.h.
// All added to the NBIO IP base (discovered via on-die discovery).
namespace NBIORegs {
    // SMN indirect-access register pair (port of nbio_v7_11_get_pcie_*
    // _offset, nbio_v7_11.c:229/234). Both at BASE_IDX 0.
    //
    // The nbio_7_11_0 offset header defines TWO variants:
    //   BIF_BX1_PCIE_INDEX2 = 0x800e  (PCIe Function 0 — our PF)
    //   BIF_BX1_PCIE_INDEX2 = 0x000e  (PCIe Function 1)
    //
    // Linux upstream uses BX1 (0x000E). v0.1.3 probe confirmed that
    // BX1 at BAR5 byte 0x38 actually holds the written value (low 2
    // bits get masked off as expected for a dword-address register),
    // while BX0 at byte 0x20038 returns 0xFFFFFFFF (not mapped).
    constexpr uint32_t BIF_BX1_PCIE_INDEX2               = 0x000E;
    constexpr uint32_t BIF_BX1_PCIE_DATA2                = 0x000F;

    // PCIe-PORT indirect-access register pair — port of
    // nbif_v6_3_1_get_pcie_port_{index,data}_offset (nbif_v6_3_1.c:322/332)
    // and nbio_v7_11_get_pcie_port_{index,data}_offset (nbio_v7_11.c:239/244).
    // Both NBIF v6.3.1 and NBIO v7.11 resolve through the same RSMU_INDEX/
    // DATA pair at NBIO BASE_IDX 1 (offset 0x0000/0x0001). The nbif_6_3_1
    // header names it BX_PF0; the nbio_7_11 header names it BX_PF1 — same
    // physical register, different SR-IOV "function" prefix.
    //
    // For IP_VERSION(7, 11, 4) chips, nbif_v6_3_1 switches to BIF_BX0_PCIE_
    // INDEX/DATA (offset 0x000C/0x000D BASE_IDX 1). Add both so the runtime
    // branch in nbif_v6_3_1_get_pcie_port_index_offset can pick correctly.
    constexpr uint32_t BIF_BX_PF0_RSMU_INDEX             = 0x0000;
    constexpr uint32_t BIF_BX_PF0_RSMU_DATA              = 0x0001;
    constexpr uint32_t BIF_BX0_PCIE_INDEX                = 0x000C;
    constexpr uint32_t BIF_BX0_PCIE_DATA                 = 0x000D;
    // Legacy alias retained for prior call sites (PCIE_PORT_RREG32 helper).
    constexpr uint32_t BIF_BX_PF1_RSMU_INDEX             = 0x0000;
    constexpr uint32_t BIF_BX_PF1_RSMU_DATA              = 0x0001;

    // ---- NBIF v6.3.1 S2A doorbell entry routing table ------------------
    //
    // NBIF v6.3.1 (used by R9700-class chips per discovery hw_id=108
    // version 6.3.1) routes doorbell BAR2 traffic through eight "Slave-
    // to-AXI" (S2A) entries, one per consumer block. Each entry packs
    // ENABLE, AWID (the AXI write ID the consumer listens for), RANGE_
    // OFFSET, RANGE_SIZE, FENCE_ENABLE, 64BIT_SUPPORT_DIS, DROP_EN, NEED_
    // DEDUCT_RANGE_OFFSET, and AWADDR_31_28_VALUE (bits 31:28 of the
    // generated AXI address). Layout per nbif_6_3_1_sh_mask.h:11313+.
    //
    // For IP_VERSION(7, 11, 4) the same fields live at the _nbif_4_10
    // register variants (BASE_IDX 3, far outside Apple's 2 MB BAR5 — must
    // be reached through PCIE_PORT_RREG/WREG indirect). For base
    // 6.3.1 / 7.11.0-3, the entries live at BASE_IDX 2 and are reachable
    // via direct MMIO.
    //
    // ENTRY purpose (from nbif_v6_3_1.c):
    //   0 — GFX/HQD                  (gc_doorbell_init writes 0x30000007)
    //   1 — IH (ih_doorbell_range, PORT1_AWID=0x0, AWADDR=0x0)
    //   2 — SDMA  (sdma_doorbell_range, PORT2_AWID=0xe, AWADDR=0x3)
    //   3 — MES/compute fence       (gc_doorbell_init writes 0x3000000d)
    //   4 — VCN0  (vcn_doorbell_range)
    //   5 — VCN1
    //   6 — VPE (unused on R9700)
    //   7 — JPEG (unused on R9700)
    constexpr uint32_t GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL = 0x01CB;
    constexpr uint32_t GDC_S2A0_S2A_DOORBELL_ENTRY_1_CTRL = 0x01CC;
    constexpr uint32_t GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL = 0x01CD;
    constexpr uint32_t GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL = 0x01CE;
    constexpr uint32_t GDC_S2A0_S2A_DOORBELL_ENTRY_4_CTRL = 0x01CF;
    constexpr uint32_t GDC_S2A0_S2A_DOORBELL_ENTRY_5_CTRL = 0x01D0;
    // _nbif_4_10 variants for IP_VERSION(7, 11, 4) at BASE_IDX 3.
    constexpr uint32_t GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL_nbif_4_10 = 0x4F0AEB;
    constexpr uint32_t GDC_S2A0_S2A_DOORBELL_ENTRY_1_CTRL_nbif_4_10 = 0x4F0AED;
    constexpr uint32_t GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL_nbif_4_10 = 0x4F0AEF;
    constexpr uint32_t GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL_nbif_4_10 = 0x4F0AF1;
    constexpr uint32_t GDC_S2A0_S2A_DOORBELL_ENTRY_4_CTRL_nbif_4_10 = 0x4F0AF3;
    constexpr uint32_t GDC_S2A0_S2A_DOORBELL_ENTRY_5_CTRL_nbif_4_10 = 0x4F0AF5;

    // S2A_DOORBELL_PORTn_* field layout (identical across all 8 ports).
    // From nbif_6_3_1_sh_mask.h:11313-11330.
    constexpr uint32_t kS2A_DOORBELL_PORT_ENABLE_SHIFT             = 0x0;
    constexpr uint32_t kS2A_DOORBELL_PORT_ENABLE_MASK              = 0x00000001u;
    constexpr uint32_t kS2A_DOORBELL_PORT_AWID_SHIFT               = 0x1;
    constexpr uint32_t kS2A_DOORBELL_PORT_AWID_MASK                = 0x0000003Eu;
    constexpr uint32_t kS2A_DOORBELL_PORT_FENCE_ENABLE_SHIFT       = 0x6;
    constexpr uint32_t kS2A_DOORBELL_PORT_FENCE_ENABLE_MASK        = 0x00000040u;
    constexpr uint32_t kS2A_DOORBELL_PORT_RANGE_OFFSET_SHIFT       = 0x7;
    constexpr uint32_t kS2A_DOORBELL_PORT_RANGE_OFFSET_MASK        = 0x0001FF80u;
    constexpr uint32_t kS2A_DOORBELL_PORT_RANGE_SIZE_SHIFT         = 0x11;
    constexpr uint32_t kS2A_DOORBELL_PORT_RANGE_SIZE_MASK          = 0x01FE0000u;
    constexpr uint32_t kS2A_DOORBELL_PORT_64BIT_SUPPORT_DIS_SHIFT  = 0x19;
    constexpr uint32_t kS2A_DOORBELL_PORT_64BIT_SUPPORT_DIS_MASK   = 0x02000000u;
    constexpr uint32_t kS2A_DOORBELL_PORT_NEED_DEDUCT_OFFSET_SHIFT = 0x1A;
    constexpr uint32_t kS2A_DOORBELL_PORT_NEED_DEDUCT_OFFSET_MASK  = 0x04000000u;
    constexpr uint32_t kS2A_DOORBELL_PORT_DROP_EN_SHIFT            = 0x1B;
    constexpr uint32_t kS2A_DOORBELL_PORT_DROP_EN_MASK             = 0x08000000u;
    constexpr uint32_t kS2A_DOORBELL_PORT_AWADDR_31_28_VALUE_SHIFT = 0x1C;
    constexpr uint32_t kS2A_DOORBELL_PORT_AWADDR_31_28_VALUE_MASK  = 0xF0000000u;

    // regRCC_DEV0_EPF0_RCC_DOORBELL_APER_EN — enables doorbell aperture.
    // Bit 0 = BIF_DOORBELL_APER_EN. Port of nbif_v6_3_1_enable_doorbell_
    // aperture (nbif_v6_3_1.c:203). NBIF 6.3.1 keeps the same offset
    // (0x00C0) and BASE_IDX (2) as NBIO 7.11.
    constexpr uint32_t RCC_DOORBELL_APER_EN              = 0x00C0;
    constexpr uint32_t kRCC_DOORBELL_APER_EN_BIT         = 0x00000001u;

    // regBIF_BX0_INTERRUPT_CNTL / _CNTL2 — IH dummy-page + interrupt cfg.
    // Port of nbif_v6_3_1_ih_control (nbif_v6_3_1.c:272). NBIF uses BX0
    // here (not BX1 like NBIO 7.11). Same offsets, BASE_IDX 2.
    constexpr uint32_t INTERRUPT_CNTL                    = 0x00F1;
    constexpr uint32_t INTERRUPT_CNTL2                   = 0x00F2;
    constexpr uint32_t kINTERRUPT_CNTL_DUMMY_RD_OVERRIDE_MASK = 0x00000001u;
    constexpr uint32_t kINTERRUPT_CNTL_REQ_NONSNOOP_EN_MASK   = 0x00000008u;

    // Doorbell SELFRING aperture — port of nbif_v6_3_1_enable_doorbell_
    // selfring_aperture (nbif_v6_3_1.c:210). soc24_common_hw_init enables
    // this BEFORE the regular doorbell aperture. Without it, host->engine
    // BAR2 doorbell traffic is decoded but never routed.
    //
    // NBIF v6.3.1 uses BX_PF0 prefix; NBIO 7.11 uses BX_PF1. Offsets and
    // BASE_IDX 2 are identical between the two header families, so we
    // keep the BX_PF1 alias for the v7.11 caller and add BX_PF0 names for
    // the v6.3.1 caller.
    constexpr uint32_t BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_HIGH = 0x00F3;
    constexpr uint32_t BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_LOW  = 0x00F4;
    constexpr uint32_t BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL      = 0x00F5;
    // Aliases for the existing v7.11 helpers — same physical regs.
    constexpr uint32_t BIF_BX_PF1_DOORBELL_SELFRING_GPA_APER_BASE_HIGH = 0x00F3;
    constexpr uint32_t BIF_BX_PF1_DOORBELL_SELFRING_GPA_APER_BASE_LOW  = 0x00F4;
    constexpr uint32_t BIF_BX_PF1_DOORBELL_SELFRING_GPA_APER_CNTL      = 0x00F5;
    // CNTL field shifts/masks (nbif_6_3_1_sh_mask.h equivalent to
    // nbio_7_11_0_sh_mask.h:55934-55939).
    constexpr uint32_t kDOORBELL_SELFRING_GPA_APER_EN_SHIFT    = 0x0;
    constexpr uint32_t kDOORBELL_SELFRING_GPA_APER_EN_MASK     = 0x00000001u;
    constexpr uint32_t kDOORBELL_SELFRING_GPA_APER_MODE_SHIFT  = 0x1;
    constexpr uint32_t kDOORBELL_SELFRING_GPA_APER_MODE_MASK   = 0x00000002u;
    constexpr uint32_t kDOORBELL_SELFRING_GPA_APER_SIZE_SHIFT  = 0x8;
    constexpr uint32_t kDOORBELL_SELFRING_GPA_APER_SIZE_MASK   = 0x000FFF00u;

    // HDP remap regs — port of nbif_v6_3_1_remap_hdp_registers
    // (nbif_v6_3_1.c:60). NBIF 6.3.1 places these at offset 0x012d/0x012e
    // BASE_IDX 2 (within Apple's 2 MB BAR5 — direct MMIO), distinct from
    // NBIO 7.11 which uses offset 0x8e4d/0x8e4e at BASE_IDX 5 (outside,
    // requires SMN indirect). Keep the v7.11 constants here too for the
    // alternate version branch.
    constexpr uint32_t BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL  = 0x012D;
    constexpr uint32_t BIF_BX0_REMAP_HDP_REG_FLUSH_CNTL  = 0x012E;
    constexpr uint32_t v7_11_REMAP_HDP_MEM_FLUSH_CNTL    = 0x8E4D;
    constexpr uint32_t v7_11_REMAP_HDP_REG_FLUSH_CNTL    = 0x8E4E;

    // regRCC_DEV0_EPF2_STRAP2 — programmed by nbif_v6_3_1_init_registers
    // (nbif_v6_3_1.c:355) to clear STRAP_NO_SOFT_RESET_DEV0_F2. This
    // allows the F2 endpoint to take soft-reset signals; without the
    // clear, certain RAS-triggered resets are silently masked.
    constexpr uint32_t RCC_DEV0_EPF2_STRAP2              = 0x009A;
    constexpr uint32_t kSTRAP_NO_SOFT_RESET_DEV0_F2_MASK = 0x00000002u;

    // regBIF_BIF256_CI256_RC3X4_USB4_PCIE_MST_CTRL_3 — PCIe master ctrl
    // for the GPU's PCIe root port. nbio_v7_11_init_registers sets the
    // SWUS_MAX_READ_REQUEST_SIZE_MODE bit so the GPU can issue 4 KB MRRs
    // (default is 512 B). Offset 0x4201C6, BASE_IDX 5.
    constexpr uint32_t PCIE_MST_CTRL_3                   = 0x4201C6;
    constexpr uint32_t kSWUS_MAX_READ_REQUEST_SIZE_MODE_MASK = 0x08000000u; // bit 27
    constexpr uint32_t kSWUS_MAX_READ_REQUEST_SIZE_MODE_SHIFT = 27;
    constexpr uint32_t kSWUS_MAX_READ_REQUEST_SIZE_PRIV_MASK = 0x30000000u; // bits 28-29
    constexpr uint32_t kSWUS_MAX_READ_REQUEST_SIZE_PRIV_SHIFT = 28;

    // regRCC_DEV0_EPF5_STRAP4 — root-complex strap for the GPU's EPF5
    // function. nbio_v7_11_init_registers clears bit 23. Per
    // discussions in upstream commits this strap gates the IMU/RLC
    // boot path; without the clear, the autoload state machine never
    // takes the GC out of reset. Offset 0xD284, BASE_IDX 5.
    constexpr uint32_t RCC_DEV0_EPF5_STRAP4              = 0xD284;
    constexpr uint32_t kRCC_DEV0_EPF5_STRAP4_BIT23       = 0x00800000u;
}

// Upstream constants from amdgpu_discovery.h:
//     DISCOVERY_TMR_OFFSET = (64 << 10)   = 64 KB
//     DISCOVERY_TMR_SIZE   = (10 << 10)   = 10 KB (actual binary)
// We allocate 64 KB to round to the AS page size (16 KB) with room.
constexpr uint64_t kDiscoveryTMROffset = 0x10000;    // 64 KB (matches upstream)
constexpr uint32_t kDiscoveryTMRSize   = 0x10000;    // 64 KB allocation
                                                     // (binary itself ≈ 10 KB)

// **** Apple Silicon page size is 16 KB, not 4 KB. ****
//
// All DMA-mappable buffers handed to the GPU through DART must be
// aligned to 16 KB or DART will reject the mapping (qemu-vfio-apple
// has explicit "rejecting DMA mapping" logging for the multi-segment
// fallback case that fires when this is wrong). Use kASPageSize for
// alignment everywhere, even when the GPU side only needs 4 KB
// granularity — the cost is a few extra zero bytes per allocation,
// the failure mode otherwise is a hard refusal at PrepareForDMA.
constexpr uint64_t kASPageSize = 16384;

// PSP GFX command frame flags — from
// drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h.
constexpr uint32_t kPSPGfxCmdStatusMask   = 0x0000FFFFu;
constexpr uint32_t kPSPGfxCmdResponseMask = 0x80000000u;
constexpr uint32_t kPSPGfxFlagResponse    = 0x80000000u;

// Combined response handshake. C2PMSG_64 should latch
// (val & MASK) == FLAG once the bootloader finishes.
constexpr uint32_t kPSPMboxRespFlag = kPSPGfxFlagResponse;
constexpr uint32_t kPSPMboxRespMask = kPSPGfxCmdResponseMask
                                    | kPSPGfxCmdStatusMask;

// psp_ring_type — only KM is used outside SR-IOV.
// PSP_RING_TYPE__KM = 2 per upstream amdgpu_psp.h:116. Kernel-mode
// (formerly GPCOM) ring. When shifted by 16 and written to C2PMSG_64
// it produces GFX_CTRL_CMD_ID_INIT_GPCOM_RING = 0x20000. *DO NOT* set
// this to 1 — that's PSP_RING_TYPE__UM (user-mode / SR-IOV RBI ring),
// which when shifted by 16 maps to GFX_CTRL_CMD_ID_INIT_RBI_RING
// (0x10000). PSP accepts the RBI init and responds OK, but never
// honors C2PMSG_67 wptr doorbells for it — every subsequent ring
// submit is silently dropped. Burned 6 days chasing this with KM=1.
constexpr uint32_t kPSPRingTypeKM = 2;
constexpr uint32_t kPSPRingTypeUM = 1;

// PSP's protocol uses a 4 KB ring (matches Linux psp_ring_init).
// On AS we still allocate the underlying buffer at 16 KB alignment
// + size to satisfy the page granularity, and tell PSP that the
// usable area is 4 KB via C2PMSG_71. The trailing 12 KB is unused.
constexpr uint32_t kPSPKMRingSize    = 0x1000;
constexpr uint32_t kPSPKMRingBufSize = 16384;

// ============================================================
// Doorbell index map + state — mirrors upstream
// amdgpu_doorbell_index (amdgpu_drv.h) and amdgpu_doorbell_mgr.
//
// On RDNA4 / gfx1201 the doorbell BAR is BAR2. Each ring/IP
// block is assigned a dword index into the BAR2 aperture.
// The doorbell aperture must be enabled by NBIO bif init
// (nbio_v7_11_enable_doorbell_aperture) before writes to
// BAR2 + (index * 4) reach the target ring.
//
// Upstream values from amdgpu_doorbell_index (amdgpu_drv.c):
//   gfx_ring0_doorbell_index     = 0
//   gfx_ring1_doorbell_index     = 1
//   sdma0_doorbell_index         = 2
//   sdma1_doorbell_index         = 3
//   ih_doorbell_index            = 4
//   mes_ring0_doorbell_index     = 5
//   compute_doorbell_index       = 6
//   max_assignment               = 6 (or 8 with MES)
//
// We add MES entries (mes_ring0=7, compute=8) for RDNA4.
// ============================================================
// Doorbell index map. Values are the units each consumer expects, which is
// NOT uniform in upstream amdgpu and must not be "tidied":
//
//   * CP (KIQ / MEC / MES / GFX) and the CP_*_DOORBELL_RANGE registers use
//     these numbers as DWORD indices directly (gfx_v12_0.c:2957-2966,
//     ring->doorbell_index = adev->doorbell_index.gfx_ring0).
//   * SDMA shifts left by one first: sdma_v7_0.c:1325 assigns
//     ring->doorbell_index = adev->doorbell_index.sdma_engine[i] << 1,
//     so sdma_engine[0] = 0x100 means DWORD 0x200 in the BAR2 aperture.
//   * The IH does the same shift when it uses a doorbell at all (we poll).
//
// Layout is AMDGPU_NAVI10_DOORBELL_ASSIGNMENT (amdgpu_doorbell.h:186-237),
// which is what the hardware's two doorbell windows are shaped for: one
// contiguous compute block the MEC range covers, then GFX, then the
// non-CP engines above 0xFF.
struct DoorbellIndex {
    // ---- compute block: CP_MEC_DOORBELL_RANGE covers [kiq .. userqueue_end]
    uint32_t kiq             = 0x000;
    uint32_t hiq             = 0x001;
    uint32_t diq             = 0x002;
    uint32_t mec_ring[8]     = {0x003, 0x004, 0x005, 0x006,
                                0x007, 0x008, 0x009, 0x00A};
    uint32_t mes_ring0       = 0x00B;
    uint32_t mes_ring1       = 0x00C;
    uint32_t userqueue_start = 0x00D;
    uint32_t userqueue_end   = 0x08A;
    // ---- graphics block: CP_RB_DOORBELL_RANGE covers
    //      [gfx_ring0 .. gfx_userqueue_end]
    uint32_t gfx_ring0           = 0x08B;
    uint32_t gfx_ring1           = 0x08C;
    uint32_t gfx_userqueue_start = 0x08D;
    uint32_t gfx_userqueue_end   = 0x0FF;
    // ---- non-CP engines (SDMA values are QWORD indices, see above)
    uint32_t sdma_engine[4]      = {0x100, 0x10A, 0x114, 0x11E};
    uint32_t sdma_doorbell_range = 20;   // per engine, matches upstream
    uint32_t ih                  = 0x178;
    // ---- MES aggregated doorbells (one per AMD_PRIORITY level).
    // amdgpu_mes.c:55-56 hands MES five doorbells spaced two DWORDs apart
    // out of the process-doorbell pool, i.e. above every kernel assignment.
    // Ours sit above VPE (0x190), clear of SDMA's DWORD range 0x200-0x24F.
    uint32_t mes_aggregated[5]   = {0x1A0, 0x1A2, 0x1A4, 0x1A6, 0x1A8};
    uint32_t max_assignment      = 0x1A8;
    // The pre-0.0.18 ad-hoc map, kept so navi48-doorbell-legacy=1 can A/B a
    // regression against the build that first passed stages 16 and 17.
    bool     legacy_map          = false;
};

// Doorbell state — mirrors upstream amdgpu_doorbell.
struct DoorbellState {
    uint64_t base = 0;               // BAR2 base address (0 on AS — accessed via MemoryRead/Write)
    uint64_t size = 0;               // BAR2 size in bytes
    uint32_t num_kernel_doorbells = 0;  // min(size/4, max_assignment + 1) + 0x400 (Vega+ compat)
    DoorbellIndex index;             // ASIC-specific doorbell index map
};

} // namespace amdgpu
