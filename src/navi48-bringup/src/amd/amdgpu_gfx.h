// Adapted from lemonade-sdk/mac-amdgpu (MIT) dext/amdgpu/amdgpu_gfx.h @ 3bdeed2.
// Was VERBATIM; forked for the Navi48Bringup kext. Changes vs the reference:
//   1. Navi 48 (RX 9070 XT) caps: max_sh_per_se 1 -> 2. The reference's
//      "4 SE x 1 SA x 8 CU/SA" describes 32 CUs; Navi 48 is 4 SE x 2 SA x
//      8 CU/SA = 64 CUs (32 WGPs). With max_sh_per_se = 1 get_cu_info only
//      walks half the shader arrays and under-counts CUs by 2x.
//   2. Added GFXRegBaseIdx — the SOC15 BASE_IDX for each GC register the
//      module touches, in one table (the reference hardcodes BASE_IDX 0 at
//      all 13 call sites). See the comment on that namespace.
//   3. Added the four kCP_*_DEFAULT constants the reference's gfx_v12_0.cpp
//      got from amdgpu_mes.h — that header is not part of this port.
//   4. GFXConfig gained audit/log fields (active_sa_bitmap,
//      global_active_rb_bitmap, num_active_wgps) and upstream's
//      gpu_early_init sc_* FIFO sizes.
//   5. Declared gfx_gpu_early_init (the reference inlines it into
//      gfx_constants_init).
//
//  amdgpu_gfx.h — GFX top-level configuration.
//
//  Covers gfx_v12_0_constants_init — the GRBM/SH_MEM-config writes the CP
//  expects to find programmed before it'll accept queue submissions —
//  plus setup_rb, get_cu_info, init_compute_vmid and the MQD builders.
//
//  Source: drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c:1806
//          (gfx_v12_0_constants_init)
//

#pragma once

#include <stdint.h>
#include "amdgpu_ip.h"
#include "amdgpu_regs.h"

namespace amdgpu {

// GC register offsets that belong to top-level GFX init (as opposed
// to the CP HQD registers in amdgpu_cp.h). All offsets sourced from
// drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_offset.h.
namespace GFXRegs {
    constexpr uint32_t GRBM_CNTL        = 0x0DA0;
    constexpr uint32_t GRBM_GFX_CNTL    = 0x0900;
    // gc_12_0_0_offset.h: regGRBM_GFX_INDEX = 0x2200.
    constexpr uint32_t GRBM_GFX_INDEX   = 0x2200;
    constexpr uint32_t SH_MEM_BASES     = 0x09E3;
    constexpr uint32_t SH_MEM_CONFIG    = 0x09E4;
    // gc_12_0_0_offset.h: regSPI_GDBG_PER_VMID_CNTL = 0x1f72.
    constexpr uint32_t SPI_GDBG_PER_VMID_CNTL = 0x1F72;
    // RB / SA harvest map registers. gfx_v12_0_get_rb_active_bitmap
    // and gfx_v12_0_get_sa_active_bitmap read these via SE-broadcast
    // GRBM_GFX_INDEX writes.
    //   regCC_RB_BACKEND_DISABLE / regCC_GC_SHADER_ARRAY_CONFIG and
    //   regGRBM_CC_GC_SA_UNIT_DISABLE — used by setup_rb.
    constexpr uint32_t CC_RB_BACKEND_DISABLE       = 0x13DD; // gc_12_0_0_offset.h
    constexpr uint32_t CC_GC_SHADER_ARRAY_CONFIG   = 0x100F;
    // gc_12_0_0_offset.h: regGRBM_CC_GC_SA_UNIT_DISABLE = 0x0fe9.
    // The reference had 0x0cfd, which is not a register on gc_12_0_0 at all
    // (nothing in the header carries that offset) — reads returned whatever
    // the hole decoded to and the SA harvest mask was garbage.
    constexpr uint32_t GRBM_CC_GC_SA_UNIT_DISABLE  = 0x0FE9;
    // RDNA4 CU-harvest registers used by get_cu_info.
    //   regGC_USER_SHADER_ARRAY_CONFIG = 0x5b90
    //   regGRBM_GC_USER_SA_UNIT_DISABLE = 0x5b92
    //   regGC_USER_RB_BACKEND_DISABLE = 0x5b94
    constexpr uint32_t GC_USER_SHADER_ARRAY_CONFIG = 0x5B90;
    constexpr uint32_t GRBM_GC_USER_SA_UNIT_DISABLE = 0x5B92;
    constexpr uint32_t GC_USER_RB_BACKEND_DISABLE  = 0x5B94;
    // regGB_ADDR_CONFIG = 0x13de — used by get_gb_addr_config.
    constexpr uint32_t GB_ADDR_CONFIG              = 0x13DE;
}

//
// SOC15 BASE_IDX for each GC register above.
//
// Upstream's RREG32_SOC15(GC, 0, regX) resolves to
// `reg_offset[GC][0][regX_BASE_IDX] + regX`, i.e. every GC register carries
// its own segment index, published in gc_12_0_0_offset.h as
// `reg<NAME>_BASE_IDX`. The reference's gfx_v12_0.cpp hardcoded BASE_IDX 0
// at all 13 call sites via SOC15_REG_OFFSET(); its rlc_v12_0.cpp, by
// contrast, was explicit (CP_STAT at GC[0], RLC_RLCS_BOOTLOAD_STATUS at
// GC[1]) — the same class of mistake was live in this file.
//
// Every value below is now copied verbatim from gc_12_0_0_offset.h's
// reg<NAME>_BASE_IDX line; the header is the authority, not the reference.
// Hardware confirmation on this card: GRBM_GFX_INDEX reads 0xffffffff at
// GC[0] and the 0xe0000000 reset pattern at GC[1]; GB_ADDR_CONFIG reads
// 0x08200545 at GC[0]; GRBM_STATUS reads 0x382c at GC[0].
//
// VERIFY-ON-HARDWARE: gfx_constants_init still logs GRBM_GFX_INDEX and
// GB_ADDR_CONFIG read at BOTH base indices, with absolute dword addresses,
// before it writes anything. See gfx_probe_gc_bases().
//
namespace GFXRegBaseIdx {
    constexpr int GRBM_CNTL                   = 0;  // regGRBM_CNTL_BASE_IDX 0
    constexpr int GRBM_GFX_CNTL               = 1;  // regGRBM_GFX_CNTL_BASE_IDX 1
    constexpr int GRBM_GFX_INDEX              = 1;  // regGRBM_GFX_INDEX_BASE_IDX 1
    constexpr int SH_MEM_BASES                = 1;  // regSH_MEM_BASES_BASE_IDX 1
    constexpr int SH_MEM_CONFIG               = 1;  // regSH_MEM_CONFIG_BASE_IDX 1
    constexpr int SPI_GDBG_PER_VMID_CNTL      = 0;  // ..._BASE_IDX 0
    constexpr int CC_RB_BACKEND_DISABLE       = 0;  // ..._BASE_IDX 0
    constexpr int CC_GC_SHADER_ARRAY_CONFIG   = 0;  // ..._BASE_IDX 0
    constexpr int GRBM_CC_GC_SA_UNIT_DISABLE  = 0;  // ..._BASE_IDX 0
    constexpr int GC_USER_SHADER_ARRAY_CONFIG = 1;  // ..._BASE_IDX 1
    constexpr int GRBM_GC_USER_SA_UNIT_DISABLE = 1; // ..._BASE_IDX 1
    constexpr int GC_USER_RB_BACKEND_DISABLE  = 1;  // ..._BASE_IDX 1
    constexpr int GB_ADDR_CONFIG              = 0;  // regGB_ADDR_CONFIG_BASE_IDX 0
    // The alternate segment the probe reads for comparison (the one that is
    // NOT GRBM_GFX_INDEX's own BASE_IDX).
    constexpr int kProbeAlternate             = 0;
}

// GRBM_STATUS — read-only, used by the probe as a second witness that the
// GC[0] segment decodes. gc_12_0_0_offset.h: regGRBM_STATUS = 0x0da4,
// regGRBM_STATUS_BASE_IDX 0. Hardware: 0x382c.
namespace GFXRegsRO {
    constexpr uint32_t GRBM_STATUS = 0x0DA4;
}
constexpr int kGFXRegBaseIdx_GRBM_STATUS = 0;

// GRBM_GFX_INDEX reset value: INSTANCE_BROADCAST_WRITES |
// SA_BROADCAST_WRITES | SE_BROADCAST_WRITES, i.e. 0xE0000000. A posted,
// idle GPU reads this back; it is the signature the base-index probe
// looks for.
constexpr uint32_t kGRBM_GFX_INDEX_BROADCAST_ALL = 0xE0000000u;

// GRBM_GFX_CNTL field shift/mask defs. Sourced from
// gc_12_0_0_sh_mask.h. (Mirrored in amdgpu_mes.h for code paths
// that don't pull in amdgpu_gfx.h — keep both in sync.)
#ifndef GRBM_GFX_CNTL__PIPEID__SHIFT
#define GRBM_GFX_CNTL__PIPEID__SHIFT  0x0
#define GRBM_GFX_CNTL__PIPEID_MASK    0x00000003
#define GRBM_GFX_CNTL__MEID__SHIFT    0x2
#define GRBM_GFX_CNTL__MEID_MASK      0x0000000C
#define GRBM_GFX_CNTL__VMID__SHIFT    0x4
#define GRBM_GFX_CNTL__VMID_MASK      0x000000F0
#define GRBM_GFX_CNTL__QUEUEID__SHIFT 0x8
#define GRBM_GFX_CNTL__QUEUEID_MASK   0x00000700
#endif

// GRBM_GFX_INDEX field shifts — used by setup_rb / get_cu_info to
// select an (SE, SA, INSTANCE) tuple before reading the per-SE
// harvest registers. Mirrors gc_12_0_0_sh_mask.h:15024-15035.
#define GRBM_GFX_INDEX__INSTANCE_INDEX__SHIFT             0x0
#define GRBM_GFX_INDEX__INSTANCE_INDEX_MASK               0x0000007F
#define GRBM_GFX_INDEX__SA_INDEX__SHIFT                   0x8
#define GRBM_GFX_INDEX__SA_INDEX_MASK                     0x00000300
#define GRBM_GFX_INDEX__SE_INDEX__SHIFT                   0x10
#define GRBM_GFX_INDEX__SE_INDEX_MASK                     0x000F0000
#define GRBM_GFX_INDEX__SA_BROADCAST_WRITES__SHIFT        0x1d
#define GRBM_GFX_INDEX__SA_BROADCAST_WRITES_MASK          0x20000000
#define GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES__SHIFT  0x1e
#define GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK    0x40000000
#define GRBM_GFX_INDEX__SE_BROADCAST_WRITES__SHIFT        0x1f
#define GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK          0x80000000

// SH_MEM_BASES — for per-VMID context aperture base register
// writes. gc_12_0_0_sh_mask.h:32587-32590.
#define SH_MEM_BASES__PRIVATE_BASE__SHIFT 0x0
#define SH_MEM_BASES__PRIVATE_BASE_MASK   0x0000FFFF
#define SH_MEM_BASES__SHARED_BASE__SHIFT  0x10
#define SH_MEM_BASES__SHARED_BASE_MASK    0xFFFF0000

// SPI_GDBG_PER_VMID_CNTL — gc_12_0_0_sh_mask.h:27237-27244.
#define SPI_GDBG_PER_VMID_CNTL__STALL_VMID__SHIFT      0x0
#define SPI_GDBG_PER_VMID_CNTL__STALL_VMID_MASK        0x00000001
#define SPI_GDBG_PER_VMID_CNTL__TRAP_EN__SHIFT         0x3
#define SPI_GDBG_PER_VMID_CNTL__TRAP_EN_MASK           0x00000008

// GB_ADDR_CONFIG — gc_12_0_0_sh_mask.h:25748-25759. Subset of fields
// read by get_gb_addr_config (gfx_v12_0.c:3575).
#define GB_ADDR_CONFIG__NUM_PIPES__SHIFT             0x0
#define GB_ADDR_CONFIG__NUM_PIPES_MASK               0x00000007
#define GB_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE__SHIFT  0x3
#define GB_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE_MASK    0x00000038
#define GB_ADDR_CONFIG__MAX_COMPRESSED_FRAGS__SHIFT  0x6
#define GB_ADDR_CONFIG__MAX_COMPRESSED_FRAGS_MASK    0x000000C0
#define GB_ADDR_CONFIG__NUM_PKRS__SHIFT              0x8
#define GB_ADDR_CONFIG__NUM_PKRS_MASK                0x00000700
#define GB_ADDR_CONFIG__NUM_SHADER_ENGINES__SHIFT    0x13
#define GB_ADDR_CONFIG__NUM_SHADER_ENGINES_MASK      0x00780000
#define GB_ADDR_CONFIG__NUM_RB_PER_SE__SHIFT         0x1a
#define GB_ADDR_CONFIG__NUM_RB_PER_SE_MASK           0x0C000000

// CC_GC_SHADER_ARRAY_CONFIG.INACTIVE_WGPS — used by get_cu_info.
#define CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS__SHIFT 0x10
#define CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS_MASK   0xFFFF0000

// GRBM_CC_GC_SA_UNIT_DISABLE.SA_DISABLE — used by get_sa_active_bitmap.
#define GRBM_CC_GC_SA_UNIT_DISABLE__SA_DISABLE__SHIFT 0x8
#define GRBM_CC_GC_SA_UNIT_DISABLE__SA_DISABLE_MASK   0x00FFFF00

// CC_RB_BACKEND_DISABLE.BACKEND_DISABLE — used by
// get_rb_active_bitmap. gc_12_0_0_sh_mask.h:25744-25746.
#define CC_RB_BACKEND_DISABLE__BACKEND_DISABLE__SHIFT 0x4
#define CC_RB_BACKEND_DISABLE__BACKEND_DISABLE_MASK   0x000000F0

// gc_12_0_0_sh_mask.h:38379-38380.
#define GC_USER_RB_BACKEND_DISABLE__BACKEND_DISABLE__SHIFT 0x4
#define GC_USER_RB_BACKEND_DISABLE__BACKEND_DISABLE_MASK   0x000000F0

// gc_12_0_0_sh_mask.h:19402-19403.
#define GRBM_GC_USER_SA_UNIT_DISABLE__SA_DISABLE__SHIFT 0x8
#define GRBM_GC_USER_SA_UNIT_DISABLE__SA_DISABLE_MASK   0x00FFFF00

// gc_12_0_0_sh_mask.h:38370-38371.
#define GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_WGPS__SHIFT 0x10
#define GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_WGPS_MASK   0xFFFF0000

// GRBM_CNTL.READ_TIMEOUT — bits [11:0]. Upstream writes 0xFF as the
// safe default. Larger values give more tolerance for slow MMIO
// returns; 0xFF works on every gen.
constexpr uint32_t kGRBM_CNTL_READ_TIMEOUT_MASK  = 0x00000FFFu;
constexpr uint32_t kGRBM_CNTL_READ_TIMEOUT_VALUE = 0x000000FFu;

// SH_MEM_CONFIG composition — mirrors upstream's DEFAULT_SH_MEM_CONFIG:
//   (SH_MEM_ADDRESS_MODE_64        << ADDRESS_MODE__SHIFT      = 0 << 0)
//   (SH_MEM_ALIGNMENT_MODE_UNALIGNED << ALIGNMENT_MODE__SHIFT = 3 << 2)
//   (3                             << INITIAL_INST_PREFETCH__SHIFT = 3 << 0xE)
//
// Pre-computed: 0xC | 0xC000 = 0xC00C.
constexpr uint32_t kSH_MEM_ADDRESS_MODE_64_SHIFT          = 0x0;
constexpr uint32_t kSH_MEM_ALIGNMENT_MODE_UNALIGNED_SHIFT = 0x2;
constexpr uint32_t kSH_MEM_INITIAL_INST_PREFETCH_SHIFT    = 0xE;
constexpr uint32_t kSH_MEM_ADDRESS_MODE_64                = 0x0;
constexpr uint32_t kSH_MEM_ALIGNMENT_MODE_UNALIGNED       = 0x3;
constexpr uint32_t kSH_MEM_INITIAL_INST_PREFETCH          = 0x3;

constexpr uint32_t kDefaultSHMemConfig =
      (kSH_MEM_ADDRESS_MODE_64           << kSH_MEM_ADDRESS_MODE_64_SHIFT)
    | (kSH_MEM_ALIGNMENT_MODE_UNALIGNED  << kSH_MEM_ALIGNMENT_MODE_UNALIGNED_SHIFT)
    | (kSH_MEM_INITIAL_INST_PREFETCH     << kSH_MEM_INITIAL_INST_PREFETCH_SHIFT);

// GFXConfig — runtime device-config state populated by setup_rb /
// get_cu_info / get_gb_addr_config. Mirrors a subset of upstream
// adev->gfx.config.
struct GFXConfig {
    bool      inited;
    // gb_addr_config-derived fields.
    uint32_t  gb_addr_config_raw;
    uint32_t  num_pipes;             // = 1 << NUM_PIPES
    uint32_t  num_shader_engines;    // = 1 << NUM_SHADER_ENGINES
    uint32_t  num_rb_per_se;         // = 1 << NUM_RB_PER_SE
    uint32_t  num_pkrs;              // = 1 << NUM_PKRS
    uint32_t  pipe_interleave_size;  // = 1 << (8 + PIPE_INTERLEAVE_SIZE)
    uint32_t  max_compressed_frags;  // = 1 << MAX_COMPRESSED_FRAGS

    // Navi 48 (gfx1201) caps, set by gfx_gpu_early_init. Upstream takes
    // max_shader_engines / max_sh_per_se / max_backends_per_se /
    // max_cu_per_sh from the on-die GC discovery table
    // (amdgpu_discovery.c:amdgpu_discovery_get_gfx_info) and only
    // max_hw_contexts + the sc_* FIFO sizes from
    // gfx_v12_0_gpu_early_init's IP_VERSION(12, 0, 1) branch.
    //
    //   4 SE x 2 SA/SE x 4 WGP/SA x 2 CU/WGP = 64 CUs  (RX 9070 XT, full die)
    //   4 SE x 4 RB/SE                       = 16 RBs
    uint32_t  max_shader_engines;     // 4
    uint32_t  max_sh_per_se;          // 2  (reference said 1 — see file header)
    uint32_t  max_backends_per_se;    // 4
    uint32_t  max_cu_per_sh;          // 8  (4 WGPs x 2 CUs)
    uint32_t  max_hw_contexts;        // 8

    // gfx_v12_0_gpu_early_init IP_VERSION(12, 0, 1) constants. Nothing in
    // the bring-up path consumes these yet; kept so the port is complete
    // and so they show up in the log.
    uint32_t  sc_prim_fifo_size_frontend;   // 0x20
    uint32_t  sc_prim_fifo_size_backend;    // 0x100
    uint32_t  sc_hiz_tile_fifo_size;        // 0
    uint32_t  sc_earlyz_tile_fifo_size;     // 0x4C0

    // Shader-array harvest map (set by setup_rb; kept for audit/logging).
    uint32_t  active_sa_bitmap;
    uint32_t  global_active_rb_bitmap;

    // Render-backend harvest map (set by setup_rb).
    uint32_t  active_rb_bitmap;
    uint32_t  num_rbs;

    // CU harvest map (set by get_cu_info). active_cu_bitmap[se][sh].
    // For Navi 48 (gfx1201): max_shader_engines = 4, max_sh_per_se = 2,
    // so all 8 shader arrays fit the 4x2 array exactly.
    uint32_t  active_cu_bitmap[4][2];
    uint32_t  num_active_cus;
    uint32_t  num_active_wgps;

    // Set true once gfx_probe_gc_bases() has run (it logs GC[0] vs GC[1]).
    bool      gc_bases_probed;
};

//
// Port of gfx_v12_0_gpu_early_init (gfx_v12_0.c:1640, IP_VERSION(12, 0, 1)
// branch) — fills the static per-ASIC caps in GFXConfig. No MMIO except
// the GB_ADDR_CONFIG read that upstream does in the same function, which
// we keep in gfx_get_gb_addr_config. Called by gfx_constants_init; exposed
// so a caller that only needs the caps can get them without touching the
// GPU's GRBM state.
//
kern_return_t gfx_gpu_early_init(const DeviceContext &dev, GFXConfig &cfg);

//
// Port of gfx_v12_0_constants_init (gfx_v12_0.c:1806).
//
// Programs the bring-up subset that the CP and shader cores expect
// to find configured before they fetch packets:
//   - gpu_early_init (static per-ASIC caps, no MMIO)
//   - GRBM_CNTL.READ_TIMEOUT = 0xFF
//   - get_gb_addr_config (GB_ADDR_CONFIG decode)
//   - setup_rb (harvest map from SA_UNIT_DISABLE + CC_RB_BACKEND_DISABLE)
//   - get_cu_info (CU harvest map from CC_GC_SHADER_ARRAY_CONFIG)
//   - per-VMID SH_MEM_CONFIG/_BASES loop (VMIDs 0..7)
//   - init_compute_vmid (SPI_GDBG_PER_VMID_CNTL.TRAP_EN for VMIDs 8..15)
//
// Audit-7 #8.
//
kern_return_t gfx_constants_init(const DeviceContext &dev,
                                 GFXConfig &cfg);
// Convenience overload — uses an internal static GFXConfig for
// callers that don't track config state (e.g. our cp_init_full).
// Should be migrated to the explicit form once we have a real
// device-wide GFXContext.
kern_return_t gfx_constants_init(const DeviceContext &dev);

//
// Port of get_gb_addr_config (gfx_v12_0.c:3575). Reads regGB_ADDR_CONFIG
// once and decodes the harvest/pipe/se fields into the GFXConfig
// struct. Required before setup_rb (which reads max_backends_per_se
// / max_sh_per_se from the cfg).
//
kern_return_t gfx_get_gb_addr_config(const DeviceContext &dev,
                                     GFXConfig &cfg);

//
// Port of gfx_v12_0_setup_rb (gfx_v12_0.c:1731). Build active_rb_bitmap
// from per-SE harvest registers.  Audit-7 #8.
//
kern_return_t gfx_setup_rb(const DeviceContext &dev, GFXConfig &cfg);

//
// Port of gfx_v12_0_get_cu_info (the v2 implementation, gfx_v12_0.c:5737).
// Populates active_cu_bitmap[se][sh] + num_active_cus.  Audit-7 #8.
//
kern_return_t gfx_get_cu_info(const DeviceContext &dev, GFXConfig &cfg);

//
// Port of gfx_v12_0_init_compute_vmid (gfx_v12_0.c:1766). For each
// KFD VMID, GRBM-select it and program SH_MEM_CONFIG, SH_MEM_BASES,
// and SPI_GDBG_PER_VMID_CNTL.TRAP_EN = 1.  Audit-7 #8.
//
kern_return_t gfx_init_compute_vmid(const DeviceContext &dev);

//==================================================================
// MQD construction
//
// Audit-7 #9. Previously only MES SCHED's pipe MQD was built. We
// also need:
//   - KIQ MQD   (v12_compute_mqd, ME=1)
//   - KCQ MQD   (v12_compute_mqd, ME=1)
//   - GFX MQD   (v12_gfx_mqd,     ME=0)
//
// Layouts mirror upstream v12_structs.h (include/v12_structs.h).
// We only mirror the dword offsets actually written by upstream's
// {kiq,kcq,gfx}_mqd_init paths — the rest of the MQD page stays 0.
//
// The MQDs are populated at queue-create time; this header just
// exposes the offsets + the build helpers. Storage allocation
// lives alongside whichever queue manager creates the ring.
//==================================================================

// MQD page size for all three queue types (matches MES_MQD_SIZE).
constexpr uint32_t kGFX_MQDPageBytes = 4096;

// Doorbell indexes upstream stitches in for these queues (kept here
// so the code that wires queue-create has stable defaults).
constexpr uint32_t kGFX_KIQ_DoorbellIndex     = 0x21;
constexpr uint32_t kGFX_KCQ_DoorbellBase      = 0x30;  // 0x30..
constexpr uint32_t kGFX_KGQ_DoorbellIndex     = 0x22;

// v12_compute_mqd dword offsets — used by KIQ + KCQ. From
// include/v12_structs.h:674.. (we add what gfx_v12_0_compute_mqd_init
// writes — gfx_v12_0.c:3143).
namespace ComputeMqdOff {
    // CORRECTED against include/v12_structs.h. The previous table
    // had the whole low block wrong (header at 80, the CU masks at 82-85, the
    // priorities at 169/170, ib_control at 168, dequeue_request at 172) — those
    // look like v12_gfx_mqd offsets pasted into the compute struct. Every value
    // below is the "// offset: N" annotation on the matching field of
    // struct v12_compute_mqd. The high block (128-183) was already right.
    constexpr uint32_t header                            = 0;
    constexpr uint32_t compute_pipelinestat_enable       = 11;
    constexpr uint32_t compute_static_thread_mgmt_se0    = 23;
    constexpr uint32_t compute_static_thread_mgmt_se1    = 24;
    constexpr uint32_t compute_static_thread_mgmt_se2    = 26;
    constexpr uint32_t compute_static_thread_mgmt_se3    = 27;
    constexpr uint32_t compute_misc_reserved             = 32;
    constexpr uint32_t cp_mqd_base_addr_lo               = 128;
    constexpr uint32_t cp_mqd_base_addr_hi               = 129;
    constexpr uint32_t cp_hqd_active                     = 130;
    constexpr uint32_t cp_hqd_vmid                       = 131;
    constexpr uint32_t cp_hqd_persistent_state           = 132;
    constexpr uint32_t cp_hqd_pipe_priority              = 133;
    constexpr uint32_t cp_hqd_queue_priority             = 134;
    constexpr uint32_t cp_hqd_quantum                    = 135;
    constexpr uint32_t cp_hqd_pq_base_lo                 = 136;
    constexpr uint32_t cp_hqd_pq_base_hi                 = 137;
    constexpr uint32_t cp_hqd_pq_rptr                    = 138;
    constexpr uint32_t cp_hqd_pq_rptr_report_addr_lo     = 139;
    constexpr uint32_t cp_hqd_pq_rptr_report_addr_hi     = 140;
    constexpr uint32_t cp_hqd_pq_wptr_poll_addr_lo       = 141;
    constexpr uint32_t cp_hqd_pq_wptr_poll_addr_hi       = 142;
    constexpr uint32_t cp_hqd_pq_doorbell_control        = 143;
    constexpr uint32_t cp_hqd_pq_control                 = 145;
    constexpr uint32_t cp_hqd_ib_control                 = 149;
    constexpr uint32_t cp_hqd_dequeue_request            = 152;
    constexpr uint32_t cp_mqd_control                    = 162;
    constexpr uint32_t cp_hqd_eop_base_addr_lo           = 165;
    constexpr uint32_t cp_hqd_eop_base_addr_hi           = 166;
    constexpr uint32_t cp_hqd_eop_control                = 167;
    constexpr uint32_t cp_hqd_pq_wptr_lo                 = 182;
    constexpr uint32_t cp_hqd_pq_wptr_hi                 = 183;
    constexpr uint32_t fence_address_lo                  = 446;
    constexpr uint32_t fence_address_hi                  = 447;
    constexpr uint32_t kDwords                           = 512;
    // gfx_v12_0.c:51 — each compute queue gets a 2 KiB end-of-pipe buffer.
    constexpr uint32_t kMecHpdBytes                      = 2048;
    // Register defaults gfx_v12_0_compute_mqd_init starts from (gfx_v12_0.c:39-46).
    constexpr uint32_t kDefault_CP_HQD_EOP_CONTROL       = 0x00000006;
    constexpr uint32_t kDefault_CP_HQD_PQ_DOORBELL_CTRL  = 0x00000000;
    constexpr uint32_t kDefault_CP_MQD_CONTROL           = 0x00000100;
    constexpr uint32_t kDefault_CP_HQD_PQ_CONTROL        = 0x00308509;
    constexpr uint32_t kDefault_CP_HQD_PQ_RPTR           = 0x00000000;
    constexpr uint32_t kDefault_CP_HQD_PERSISTENT_STATE  = 0x0be05501;
    constexpr uint32_t kDefault_CP_HQD_IB_CONTROL        = 0x00300000;
}

// v12_gfx_mqd dword offsets — used by GFX kgq (kernel gfx queue).
// From include/v12_structs.h:27 and gfx_v12_0_gfx_mqd_init
// (gfx_v12_0.c:2969).
namespace GfxMqdOff {
    constexpr uint32_t shadow_base_lo            = 0;
    constexpr uint32_t shadow_base_hi            = 1;
    constexpr uint32_t fw_work_area_base_lo      = 4;
    constexpr uint32_t fw_work_area_base_hi      = 5;
    constexpr uint32_t cp_mqd_base_addr          = 128;
    constexpr uint32_t cp_mqd_base_addr_hi       = 129;
    constexpr uint32_t cp_gfx_hqd_active         = 130;
    constexpr uint32_t cp_gfx_hqd_vmid           = 131;
    constexpr uint32_t cp_gfx_hqd_queue_priority = 134;
    constexpr uint32_t cp_gfx_hqd_quantum        = 135;
    constexpr uint32_t cp_gfx_hqd_base           = 136;
    constexpr uint32_t cp_gfx_hqd_base_hi        = 137;
    constexpr uint32_t cp_gfx_hqd_rptr           = 138;
    constexpr uint32_t cp_gfx_hqd_rptr_addr      = 139;
    constexpr uint32_t cp_gfx_hqd_rptr_addr_hi   = 140;
    constexpr uint32_t cp_rb_wptr_poll_addr_lo   = 141;
    constexpr uint32_t cp_rb_wptr_poll_addr_hi   = 142;
    constexpr uint32_t cp_rb_doorbell_control    = 143;
    constexpr uint32_t cp_gfx_hqd_cntl           = 145;
    constexpr uint32_t cp_gfx_hqd_wptr           = 149;
    constexpr uint32_t cp_gfx_hqd_wptr_hi        = 150;
    constexpr uint32_t cp_gfx_mqd_control        = 162;
}

// Default register values used by the MQD builders (sourced from
// gc_11_0_0_default.h — gfx12 inherits the same reset defaults per
// gfx_v12_0.c which uses these regCP_* names without a gen-specific
// override).
constexpr uint32_t kRegCP_GFX_HQD_CNTL_DEFAULT          = 0x00a00000u;
constexpr uint32_t kRegCP_GFX_MQD_CONTROL_DEFAULT       = 0x00000100u;
constexpr uint32_t kRegCP_GFX_HQD_QUANTUM_DEFAULT       = 0x00000a01u;
constexpr uint32_t kRegCP_GFX_HQD_QUEUE_PRIORITY_DEFAULT= 0x00000000u;
constexpr uint32_t kRegCP_GFX_HQD_RPTR_DEFAULT          = 0x00000000u;
constexpr uint32_t kRegCP_GFX_HQD_VMID_DEFAULT          = 0x00000000u;
constexpr uint32_t kRegCP_HQD_PQ_DOORBELL_CONTROL_DEFAULT = 0x00000000u;
constexpr uint32_t kRegCP_HQD_IB_CONTROL_DEFAULT        = 0x00300000u;
constexpr uint32_t kRegCP_HQD_IQ_TIMER_DEFAULT          = 0x00000000u;
constexpr uint32_t kRegCP_HQD_QUANTUM_DEFAULT           = 0x00000000u;
constexpr uint32_t kRegCP_HQD_PQ_RPTR_DEFAULT           = 0x00000000u;

// Field shifts/masks for gfx-mqd fields — gc_12_0_0_sh_mask.h.
#define CP_GFX_HQD_CNTL__RB_BUFSZ__SHIFT     0x0
#define CP_GFX_HQD_CNTL__RB_BUFSZ_MASK       0x0000003F
#define CP_GFX_HQD_CNTL__RB_BLKSZ__SHIFT     0x8
#define CP_GFX_HQD_CNTL__RB_BLKSZ_MASK       0x00003F00
#define CP_GFX_HQD_CNTL__TMZ_MATCH__SHIFT    0x7
#define CP_GFX_HQD_CNTL__TMZ_MATCH_MASK      0x00000080
#define CP_GFX_HQD_CNTL__RB_NON_PRIV__SHIFT  0xf
#define CP_GFX_HQD_CNTL__RB_NON_PRIV_MASK    0x00008000

#define CP_GFX_MQD_CONTROL__VMID__SHIFT          0x0
#define CP_GFX_MQD_CONTROL__VMID_MASK            0x0000000F
#define CP_GFX_MQD_CONTROL__PRIV_STATE__SHIFT    0x8
#define CP_GFX_MQD_CONTROL__PRIV_STATE_MASK      0x00000100
#define CP_GFX_MQD_CONTROL__CACHE_POLICY__SHIFT  0x18
#define CP_GFX_MQD_CONTROL__CACHE_POLICY_MASK    0x03000000

#define CP_GFX_HQD_VMID__VMID__SHIFT             0x0
#define CP_GFX_HQD_VMID__VMID_MASK               0x0000000F

#define CP_GFX_HQD_QUEUE_PRIORITY__PRIORITY_LEVEL__SHIFT 0x0
#define CP_GFX_HQD_QUEUE_PRIORITY__PRIORITY_LEVEL_MASK   0x0000000F

#define CP_GFX_HQD_QUANTUM__QUANTUM_EN__SHIFT 0x0
#define CP_GFX_HQD_QUANTUM__QUANTUM_EN_MASK   0x00000001

// CP_HQD_PQ_DOORBELL_CONTROL fields needed by compute mqd init.
// Same layout as CP_HQD_PQ_DOORBELL_CONTROL elsewhere — re-declare
// with #ifndef so amdgpu_mes.h's earlier defs don't collide.
#ifndef CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT
#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT 0x2
#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK   0x0FFFFFFC
#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN__SHIFT     0x1e
#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN_MASK       0x40000000
#endif
// DOORBELL_SOURCE / DOORBELL_HIT are also touched by the compute
// MQD path (gfx_v12_0.c:3177-3180).
#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_SOURCE__SHIFT 0x1c
#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_SOURCE_MASK   0x10000000
#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_HIT__SHIFT    0x1f
#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_HIT_MASK      0x80000000

// CP_HQD_PERSISTENT_STATE PRELOAD_SIZE — also already in mes.h, guard.
#ifndef CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE__SHIFT
#define CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE__SHIFT 0x8
#define CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE_MASK   0x0003FF00
#endif

// CP_HQD_IB_CONTROL fields — compute MQD init writes MIN_IB_AVAIL_SIZE.
#define CP_HQD_IB_CONTROL__MIN_IB_AVAIL_SIZE__SHIFT 0x14
#define CP_HQD_IB_CONTROL__MIN_IB_AVAIL_SIZE_MASK   0x00300000

// CP_HQD_PQ_CONTROL queue-control fields — amdgpu_mes.h provides
// most; add the few it doesn't (TMZ etc.).  Re-guarded.
#ifndef CP_HQD_PQ_CONTROL__QUEUE_SIZE__SHIFT
#define CP_HQD_PQ_CONTROL__QUEUE_SIZE__SHIFT       0x0
#define CP_HQD_PQ_CONTROL__QUEUE_SIZE_MASK         0x0000003F
#define CP_HQD_PQ_CONTROL__RPTR_BLOCK_SIZE__SHIFT  0x8
#define CP_HQD_PQ_CONTROL__RPTR_BLOCK_SIZE_MASK    0x00003F00
#define CP_HQD_PQ_CONTROL__NO_UPDATE_RPTR__SHIFT   0x1b
#define CP_HQD_PQ_CONTROL__NO_UPDATE_RPTR_MASK     0x08000000
#define CP_HQD_PQ_CONTROL__UNORD_DISPATCH__SHIFT   0x1c
#define CP_HQD_PQ_CONTROL__UNORD_DISPATCH_MASK     0x10000000
#define CP_HQD_PQ_CONTROL__TUNNEL_DISPATCH__SHIFT  0x1d
#define CP_HQD_PQ_CONTROL__TUNNEL_DISPATCH_MASK    0x20000000
#define CP_HQD_PQ_CONTROL__PRIV_STATE__SHIFT       0x1e
#define CP_HQD_PQ_CONTROL__PRIV_STATE_MASK         0x40000000
#define CP_HQD_PQ_CONTROL__KMD_QUEUE__SHIFT        0x1f
#define CP_HQD_PQ_CONTROL__KMD_QUEUE_MASK          0x80000000
#endif

#define CP_HQD_PQ_CONTROL__TMZ__SHIFT              0x16
#define CP_HQD_PQ_CONTROL__TMZ_MASK                0x00400000

// CP_HQD_EOP_CONTROL.EOP_SIZE — bits [5:0].
#define CP_HQD_EOP_CONTROL__EOP_SIZE__SHIFT 0x0
#define CP_HQD_EOP_CONTROL__EOP_SIZE_MASK   0x0000003F

#ifndef CP_HQD_VMID__VMID__SHIFT
#define CP_HQD_VMID__VMID__SHIFT  0x0
#define CP_HQD_VMID__VMID_MASK    0x0000000F
#endif

#ifndef CP_MQD_CONTROL__VMID__SHIFT
// CP_HQD_QUANTUM — gc_12_0_0_sh_mask.h:13222-13229.
#define CP_HQD_QUANTUM__QUANTUM_EN__SHIFT       0x0
#define CP_HQD_QUANTUM__QUANTUM_EN_MASK         0x00000001
#define CP_HQD_QUANTUM__QUANTUM_SCALE__SHIFT    0x4
#define CP_HQD_QUANTUM__QUANTUM_SCALE_MASK      0x00000010
#define CP_HQD_QUANTUM__QUANTUM_DURATION__SHIFT 0x8
#define CP_HQD_QUANTUM__QUANTUM_DURATION_MASK   0x00003F00

#define CP_MQD_CONTROL__VMID__SHIFT  0x0
#define CP_MQD_CONTROL__VMID_MASK    0x0000000F
#endif

// Cached compute-MQD reset defaults. The reference's gfx_v12_0.cpp gets
// these by including amdgpu_mes.h (amdgpu_mes.h:180-183); that header is
// not part of this port, so they live here. Values are byte-identical to
// the reference. The guard lets a future amdgpu_mes.h port declare them
// without a duplicate-definition error.
#ifndef kCP_HQD_PQ_CONTROL_DEFAULT_DECLARED
#define kCP_HQD_PQ_CONTROL_DEFAULT_DECLARED 1
// Documented defaults from gfx_v12_0.c.
constexpr uint32_t kCP_HQD_PQ_CONTROL_DEFAULT      = 0x00308509u;
constexpr uint32_t kCP_HQD_PERSISTENT_STATE_DEFAULT= 0x0be05501u;
constexpr uint32_t kCP_MQD_CONTROL_DEFAULT         = 0x00000100u;
constexpr uint32_t kCP_HQD_EOP_CONTROL_DEFAULT     = 0x00000006u;
#endif

// MQD-build inputs — caller provides addresses, we write the dwords.
struct GFXMqdProps {
    uint64_t mqd_gpu_addr;        // GPU bus address of the MQD page itself
    uint64_t hqd_base_gpu_addr;   // GPU bus address of the ring buffer
    uint64_t rptr_gpu_addr;       // GPU bus address of rptr write-back slot
    uint64_t wptr_gpu_addr;       // GPU bus address of wptr poll slot
    uint64_t eop_gpu_addr;        // EOP buffer (compute/KIQ); ignored for GFX
    uint32_t queue_size_bytes;    // ring buffer size in BYTES
    uint32_t eop_size_bytes;      // EOP buffer size in bytes (compute only)
    uint32_t doorbell_index;
    bool     use_doorbell;
    bool     kernel_queue;        // kernel-mode vs user-mode
};

//
// Build a v12_compute_mqd into `mqd_cpu` for a KIQ ring (ME=1, pipe=0).
// Mirrors gfx_v12_0_compute_mqd_init (gfx_v12_0.c:3143).
// Audit-7 #9.
//
kern_return_t gfx_build_compute_mqd(uint32_t *mqd_cpu,
                                    const GFXMqdProps &p);

//
// Build a v12_gfx_mqd into `mqd_cpu` for a GFX kgq ring (ME=0, pipe=0).
// Mirrors gfx_v12_0_gfx_mqd_init (gfx_v12_0.c:2969).  Audit-7 #9.
//
kern_return_t gfx_build_gfx_mqd(uint32_t *mqd_cpu,
                                const GFXMqdProps &p);

//==================================================================
// gfxhub gart_enable re-run helper.
//
// Audit-7 #10. Upstream gfx_v12_0_hw_init (gfx_v12_0.c:3697) calls
// gfx_v12_0_gfxhub_enable AFTER RLC autoload completes — the RLC
// firmware reset writes can revert GFXHUB context regs, so the
// driver re-runs gart_enable + the HDP flush + TLB flush. Our
// implementation lives in the GFXInit stage handler in
// amdgpu_init.cpp (it needs BringupContext.gmc).
//==================================================================

} // namespace amdgpu
