//
//  amdgpu_mes.h — MicroEngine Scheduler (MES) v12_1 for RDNA4 / gfx1201.
//
//  Ported from lemonade-sdk/mac-amdgpu (MIT) @ commit 3bdeed2:
//      dext/amdgpu/amdgpu_mes.h
//      dext/amdgpu/mes_v12_1.cpp
//      dext/MacAMDGPU.cpp  (the MES firmware-header parse in LoadFirmware)
//
//  which in turn ports a subset of:
//      drivers/gpu/drm/amd/amdgpu/mes_v12_0.c (gfx1201 reuses these
//          ops via mes_v12_1 alias)
//      drivers/gpu/drm/amd/amdgpu/amdgpu_mes.c
//      drivers/gpu/drm/amd/include/mes_v12_api_def.h
//      drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_offset.h
//      drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_sh_mask.h
//
//  Uni-MES note: on RDNA4 we run with enable_uni_mes = true so a
//  single firmware blob (`gc_12_0_1_uni_mes.bin`) drives both the
//  scheduler and KIQ duties; only pipe 0 (SCHED) is programmed.
//
//  ---------------------------------------------------------------
//  Deviations from the reference (see also mes_v12_1.cpp's header)
//  ---------------------------------------------------------------
//   1. DriverKit IOBufferMemoryDescriptor + IODMACommand pairs →
//      amdgpu::SysMem (PORTING.md). There is no IOMMU here, so the
//      SysMem bus address is the host physical address and is NOT a
//      GPU address: every MES buffer is additionally bound into the
//      GART (gmc_bind_existing) and `*_bus` holds the resulting MC
//      address — which is what the reference's `*_bus` meant to the
//      GPU. The CPU pointer (`*_cpu`) is the kernel VA.
//   2. Because of (1) the three entry points that ALLOCATE
//      GPU-visible memory take an extra `GMCContext &gmc`:
//          mes_alloc_storage, mes_set_hw_resources,
//          mes_set_hw_resources_1
//      Every other function keeps the reference's exact signature.
//   3. mes_init_full keeps the reference's 4-argument form (so the
//      ported amdgpu_init.cpp call site compiles unchanged) and adds
//      two DEFAULTED parameters carrying the uni_mes firmware bytes.
//      The reference loads MES microcode through the dext's
//      LoadFirmware external method (MacAMDGPU.cpp) before MESInit
//      and parses `mes_uc_start_addr` there; this kext has no such
//      selector, so the header parse (mes_parse_ucode_header) and an
//      optional PSP LOAD_IP_FW fallback (mes_load_firmware) live
//      here. If the bring-up ladder already loaded MES through
//      psp_load_non_psp_fw, mes_init_full only parses the header.
//   4. The register-field macros this module shares with the CP/GFX
//      MQD paths (CP_HQD_*, CP_MQD_*, GRBM_GFX_CNTL__*, and the
//      kCP_*_DEFAULT constants) already live in amdgpu_gfx.h in this
//      tree; we include that header instead of re-declaring them, and
//      #ifndef-guard the few we do declare so either include order
//      works. Values are byte-identical to the reference.
//   5. mes_release_storage() added (the dext never freed its MES
//      buffers because the dext owned them for its whole lifetime).
//
#pragma once

#include <stdint.h>
#include <stddef.h>   // 0.0.623: offsetof (MES_ResetQueue)
#include <IOKit/IOReturn.h>

#include "amdgpu_ip.h"
#include "amdgpu_regs.h"
#include "amdgpu_sysmem.h"
// Provides CP_HQD_*/CP_MQD_*/GRBM_GFX_CNTL__* field macros and the
// kCP_*_DEFAULT reset values (deviation 4).
#include "amdgpu_gfx.h"

namespace amdgpu {

struct GMCContext;  // forward
struct PSPContext;  // forward

constexpr uint32_t kMaxMESPipes        = 2;     // SCHED + KIQ
constexpr uint32_t kMES_EOP_SIZE       = 2048;  // upstream MES_EOP_SIZE
constexpr uint32_t kMES_MQD_SIZE       = 4096;  // v12_compute_mqd + slop
constexpr uint32_t kMES_RING_SIZE      = 64 * 1024;
constexpr uint32_t kMES_CMD_BUF_SIZE   = 16 * 1024;
constexpr bool     kEnableUniMES       = true;  // RDNA4 default

enum class MESPipe : uint32_t {
    Sched = 0,
    KIQ   = 1,
};

//------------------------------------------------------------------
// MES register table — GC IP-block offsets from gc_12_0_0_offset.h.
//------------------------------------------------------------------
namespace MESRegs {
    constexpr uint32_t GRBM_GFX_CNTL              = 0x0900;
    constexpr uint32_t CP_MES_PRGRM_CNTR_START    = 0x2800;
    constexpr uint32_t CP_MES_INTR_ROUTINE_START  = 0x2801;
    constexpr uint32_t CP_MES_CNTL                = 0x2807;
    constexpr uint32_t CP_MES_PIPE0_PRIORITY      = 0x2809;
    constexpr uint32_t CP_MES_INSTR_PNTR          = 0x2813;
    constexpr uint32_t CP_MES_MSCRATCH_HI         = 0x2814;
    constexpr uint32_t CP_MES_MSCRATCH_LO         = 0x2815;
    constexpr uint32_t CP_MES_PRGRM_CNTR_START_HI = 0x289d;
    constexpr uint32_t CP_MES_IC_BASE_LO          = 0x5850;
    constexpr uint32_t CP_MES_IC_BASE_HI          = 0x5851;
    constexpr uint32_t CP_MES_DC_BASE_LO          = 0x5854;
    constexpr uint32_t CP_MES_DC_BASE_HI          = 0x5855;

    // HQD registers (for mes_queue_init).
    constexpr uint32_t CP_MQD_BASE_ADDR             = 0x1fa9;
    constexpr uint32_t CP_MQD_BASE_ADDR_HI          = 0x1faa;
    constexpr uint32_t CP_HQD_ACTIVE                = 0x1fab;
    constexpr uint32_t CP_HQD_VMID                  = 0x1fac;
    constexpr uint32_t CP_HQD_PERSISTENT_STATE      = 0x1fad;
    constexpr uint32_t CP_HQD_PQ_BASE               = 0x1fb1;
    constexpr uint32_t CP_HQD_PQ_BASE_HI            = 0x1fb2;
    constexpr uint32_t CP_HQD_PQ_RPTR_REPORT_ADDR   = 0x1fb4;
    constexpr uint32_t CP_HQD_PQ_RPTR_REPORT_ADDR_HI= 0x1fb5;
    constexpr uint32_t CP_HQD_PQ_WPTR_POLL_ADDR     = 0x1fb6;
    constexpr uint32_t CP_HQD_PQ_WPTR_POLL_ADDR_HI  = 0x1fb7;
    constexpr uint32_t CP_HQD_PQ_DOORBELL_CONTROL   = 0x1fb8;
    constexpr uint32_t CP_HQD_PQ_CONTROL            = 0x1fba;
    constexpr uint32_t CP_HQD_EOP_BASE_ADDR         = 0x1fce;
    constexpr uint32_t CP_HQD_EOP_BASE_ADDR_HI      = 0x1fcf;
    constexpr uint32_t CP_HQD_EOP_CONTROL           = 0x1fd0;
    constexpr uint32_t CP_HQD_PQ_WPTR_LO            = 0x1fdf;
    constexpr uint32_t CP_HQD_PQ_WPTR_HI            = 0x1fe0;
    constexpr uint32_t CP_MQD_CONTROL               = 0x1fcb;
    // Aggregated doorbell + GFX gate registers.
    constexpr uint32_t CP_HQD_GFX_CONTROL           = 0x1e9f;
    // gc_12_0_0_offset.h: regCP_UNMAPPED_DOORBELL = 0x0880 (BASE_IDX=1).
    // mes_v12_0.c:867 reads/writes this to enable unmapped doorbell
    // handling so MES sees doorbell writes to queues it hasn't yet
    // mapped (the KFD-style "any process can ring any doorbell" model).
    constexpr uint32_t CP_UNMAPPED_DOORBELL         = 0x0880;
    constexpr uint32_t CP_MES_DOORBELL_CONTROL1     = 0x283c;
    constexpr uint32_t CP_MES_DOORBELL_CONTROL2     = 0x283d;
    constexpr uint32_t CP_MES_DOORBELL_CONTROL3     = 0x283e;
    constexpr uint32_t CP_MES_DOORBELL_CONTROL4     = 0x283f;
    constexpr uint32_t CP_MES_DOORBELL_CONTROL5     = 0x2840;
    // gc_12_0_0_offset.h: regRLC_CP_SCHEDULERS = 0x098a.
    // Written by mes_v12_0_kiq_setting (mes_v12_0.c:1728) to tell RLC
    // which CP queue is the KIQ — required before MES can serve as
    // the kernel-interface queue manager.
    constexpr uint32_t RLC_CP_SCHEDULERS            = 0x098A;
    // gc_12_0_0_offset.h: regCP_MES_MSCRATCH_HI/_LO = 0x2814/0x2815.
    // Written by mes_v12_0_enable (mes_v12_0.c:1100) when MES event
    // logging is on — provides MES with a buffer for its scratch
    // ring. Optional but harmless if event_log_size is 0; we wire it
    // up to a static pair of zero values so the registers aren't left
    // at reset garbage.
    constexpr uint32_t CP_MES_MSCRATCH_LO_OFFSET    = 0x2815;
    constexpr uint32_t CP_MES_MSCRATCH_HI_OFFSET    = 0x2814;
    // gc_12_0_0_offset.h: regCP_MES_GP3_LO = 0x2849.  MES copies its
    // version into this register at queue-init time; mes_v12_0.c:1506
    // reads it from CP_MES_GP3_LO into adev->mes.sched_version after
    // mes_v12_0_queue_init.
    constexpr uint32_t CP_MES_GP3_LO                = 0x2849;
}

//
// SOC15 BASE_IDX for each MESRegs entry, copied verbatim from
// gc_12_0_0_offset.h's reg<NAME>_BASE_IDX lines.
//
// The reference addressed every one of these at BASE_IDX 0 (plain
// SOC15_REG_OFFSET). In reality the split is:
//   * CP_MES_* (0x28xx / 0x585x), RLC_CP_SCHEDULERS (0x098a),
//     CP_UNMAPPED_DOORBELL (0x0880) and GRBM_GFX_CNTL (0x0900) are
//     BASE_IDX 1 — the reference's writes landed on unrelated registers.
//   * The CP_HQD_* / CP_MQD_* HQD window (0x1fxx) and CP_HQD_GFX_CONTROL
//     (0x1e9f) really are BASE_IDX 0.
// Hardware corroboration for the split: CP_MEC_RS64_CNTL (0x2904, the
// same 0x28xx-0x29xx block as CP_MES_CNTL) reads 0 at GC[0] and
// 0x40030000 at GC[1]; CP_RB0_CNTL (0x1de1, the same low block as the
// HQD registers) is BASE_IDX 0 and reads correctly there.
//
namespace MESRegBaseIdx {
    constexpr int GRBM_GFX_CNTL                 = 1;
    constexpr int CP_MES_PRGRM_CNTR_START       = 1;
    constexpr int CP_MES_INTR_ROUTINE_START     = 1;
    constexpr int CP_MES_CNTL                   = 1;
    constexpr int CP_MES_PIPE0_PRIORITY         = 1;
    constexpr int CP_MES_INSTR_PNTR             = 1;
    constexpr int CP_MES_MSCRATCH_HI            = 1;
    constexpr int CP_MES_MSCRATCH_LO            = 1;
    constexpr int CP_MES_PRGRM_CNTR_START_HI    = 1;
    constexpr int CP_MES_IC_BASE_LO             = 1;
    constexpr int CP_MES_IC_BASE_HI             = 1;
    constexpr int CP_MES_DC_BASE_LO             = 1;
    constexpr int CP_MES_DC_BASE_HI             = 1;
    constexpr int CP_MQD_BASE_ADDR              = 0;
    constexpr int CP_MQD_BASE_ADDR_HI           = 0;
    constexpr int CP_HQD_ACTIVE                 = 0;
    constexpr int CP_HQD_VMID                   = 0;
    constexpr int CP_HQD_PERSISTENT_STATE       = 0;
    constexpr int CP_HQD_PQ_BASE                = 0;
    constexpr int CP_HQD_PQ_BASE_HI             = 0;
    constexpr int CP_HQD_PQ_RPTR_REPORT_ADDR    = 0;
    constexpr int CP_HQD_PQ_RPTR_REPORT_ADDR_HI = 0;
    constexpr int CP_HQD_PQ_WPTR_POLL_ADDR      = 0;
    constexpr int CP_HQD_PQ_WPTR_POLL_ADDR_HI   = 0;
    constexpr int CP_HQD_PQ_DOORBELL_CONTROL    = 0;
    constexpr int CP_HQD_PQ_CONTROL             = 0;
    constexpr int CP_HQD_EOP_BASE_ADDR          = 0;
    constexpr int CP_HQD_EOP_BASE_ADDR_HI       = 0;
    constexpr int CP_HQD_EOP_CONTROL            = 0;
    constexpr int CP_HQD_PQ_WPTR_LO             = 0;
    constexpr int CP_HQD_PQ_WPTR_HI             = 0;
    constexpr int CP_MQD_CONTROL                = 0;
    constexpr int CP_HQD_GFX_CONTROL            = 0;
    constexpr int CP_UNMAPPED_DOORBELL          = 1;
    constexpr int CP_MES_DOORBELL_CONTROL1      = 1;
    constexpr int CP_MES_DOORBELL_CONTROL2      = 1;
    constexpr int CP_MES_DOORBELL_CONTROL3      = 1;
    constexpr int CP_MES_DOORBELL_CONTROL4      = 1;
    constexpr int CP_MES_DOORBELL_CONTROL5      = 1;
    constexpr int RLC_CP_SCHEDULERS             = 1;
    constexpr int CP_MES_MSCRATCH_LO_OFFSET     = 1;
    constexpr int CP_MES_MSCRATCH_HI_OFFSET     = 1;
    constexpr int CP_MES_GP3_LO                 = 1;
    // The five CP_MES_DOORBELL_CONTROLn registers are also walked through
    // a table in mes_v12_1.cpp; they share this BASE_IDX.
    constexpr int kMESDoorbellControlBaseIdx    = 1;
}

// Address one MESRegs entry at the SOC15 BASE_IDX gc_12_0_0_offset.h
// declares for it.
#define MES_REG(dev, name) \
    SOC15_REG_OFFSET_BIDX((dev), IPBlock::GC, \
                          MESRegBaseIdx::name, MESRegs::name)

// CP_UNMAPPED_DOORBELL fields per gc_12_0_0_sh_mask.h:14084-14093.
#ifndef CP_UNMAPPED_DOORBELL__ENABLE__SHIFT
#define CP_UNMAPPED_DOORBELL__ENABLE__SHIFT          0x0
#define CP_UNMAPPED_DOORBELL__ENABLE_MASK            0x00000001
#define CP_UNMAPPED_DOORBELL__PROC_LSB__SHIFT        0x8
#define CP_UNMAPPED_DOORBELL__PROC_LSB_MASK          0x00001F00
#endif

// CP_MES_DOORBELL_CONTROL1..5 share the same field layout —
// DOORBELL_OFFSET[27:2], DOORBELL_EN[30], DOORBELL_HIT[31].
#ifndef CP_MES_DOORBELL_CONTROL1__DOORBELL_OFFSET__SHIFT
#define CP_MES_DOORBELL_CONTROL1__DOORBELL_OFFSET__SHIFT 0x2
#define CP_MES_DOORBELL_CONTROL1__DOORBELL_OFFSET_MASK   0x0FFFFFFC
#define CP_MES_DOORBELL_CONTROL1__DOORBELL_EN__SHIFT     0x1e
#define CP_MES_DOORBELL_CONTROL1__DOORBELL_EN_MASK       0x40000000
#define CP_MES_DOORBELL_CONTROL1__DOORBELL_HIT_MASK      0x80000000
#endif

// CP_HQD_GFX_CONTROL.DB_UPDATED_MSG_EN — required to route doorbell
// updated messages to MES for gfx queues.
#ifndef CP_HQD_GFX_CONTROL__DB_UPDATED_MSG_EN__SHIFT
#define CP_HQD_GFX_CONTROL__DB_UPDATED_MSG_EN__SHIFT     0xf
#define CP_HQD_GFX_CONTROL__DB_UPDATED_MSG_EN_MASK       0x00008000
#endif

//
// HQD register field shift/mask defs and defaults — gfx12.
//
// Deviation 4: CP_HQD_PQ_CONTROL__*, CP_HQD_PQ_DOORBELL_CONTROL__*,
// CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE*, CP_HQD_VMID__VMID*,
// CP_MQD_CONTROL__VMID*, CP_HQD_EOP_CONTROL__EOP_SIZE* and the four
// kCP_*_DEFAULT constants come from amdgpu_gfx.h (included above),
// byte-identical to the reference's copies in this header. They are
// re-declared below only when that header did not supply them, so
// either include order works.
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

#ifndef CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT
#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT  0x2
#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK    0x0FFFFFFC
#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN__SHIFT      0x1e
#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN_MASK        0x40000000
#endif

#ifndef CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE__SHIFT
#define CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE__SHIFT 0x8
#define CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE_MASK   0x0003FF00
#endif

#ifndef CP_HQD_VMID__VMID__SHIFT
#define CP_HQD_VMID__VMID__SHIFT  0x0
#define CP_HQD_VMID__VMID_MASK    0x0000000F
#endif

#ifndef CP_MQD_CONTROL__VMID__SHIFT
#define CP_MQD_CONTROL__VMID__SHIFT  0x0
#define CP_MQD_CONTROL__VMID_MASK    0x0000000F
#endif

// Documented defaults from gfx_v12_0.c.
#ifndef kCP_HQD_PQ_CONTROL_DEFAULT_DECLARED
#define kCP_HQD_PQ_CONTROL_DEFAULT_DECLARED 1
constexpr uint32_t kCP_HQD_PQ_CONTROL_DEFAULT      = 0x00308509u;
constexpr uint32_t kCP_HQD_PERSISTENT_STATE_DEFAULT= 0x0be05501u;
constexpr uint32_t kCP_MQD_CONTROL_DEFAULT         = 0x00000100u;
constexpr uint32_t kCP_HQD_EOP_CONTROL_DEFAULT     = 0x00000006u;
#endif

// CP_MES_CNTL — fields from gc_12_0_0_sh_mask.h
#ifndef CP_MES_CNTL__MES_HALT__SHIFT
#define CP_MES_CNTL__MES_INVALIDATE_ICACHE__SHIFT 0x4
#define CP_MES_CNTL__MES_INVALIDATE_ICACHE_MASK   0x00000010
#define CP_MES_CNTL__MES_PIPE0_RESET__SHIFT       0x10
#define CP_MES_CNTL__MES_PIPE0_RESET_MASK         0x00010000
#define CP_MES_CNTL__MES_PIPE1_RESET__SHIFT       0x11
#define CP_MES_CNTL__MES_PIPE1_RESET_MASK         0x00020000
#define CP_MES_CNTL__MES_PIPE0_ACTIVE__SHIFT      0x1a
#define CP_MES_CNTL__MES_PIPE0_ACTIVE_MASK        0x04000000
#define CP_MES_CNTL__MES_PIPE1_ACTIVE__SHIFT      0x1b
#define CP_MES_CNTL__MES_PIPE1_ACTIVE_MASK        0x08000000
#define CP_MES_CNTL__MES_HALT__SHIFT              0x1e
#define CP_MES_CNTL__MES_HALT_MASK                0x40000000
#endif

// GRBM_GFX_CNTL — selects (ME, PIPE, QUEUE, VMID) for HQD writes.
// Also defined in amdgpu_gfx.h; guard so headers can be included in
// either order.
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

//------------------------------------------------------------------
// Per-pipe state.
//
// Deviation 1: every `*_bus` is the GART **MC address** the GPU uses
// (sysmem bound through gmc_bind_existing), `*_cpu` the kernel VA,
// and the SysMem handle that owns the pages sits alongside.
//------------------------------------------------------------------
struct MESInstance {
    bool inited  { false };
    bool enabled { false };

    SysMem    eop_mem  {};
    SysMem    mqd_mem  {};
    SysMem    ring_mem {};
    SysMem    cmd_mem  {};
    SysMem    wb_mem   {};

    uint64_t  eop_bus  { 0 };  void *eop_cpu  { nullptr };
    uint64_t  mqd_bus  { 0 };  void *mqd_cpu  { nullptr };
    uint64_t  ring_bus { 0 };  void *ring_cpu { nullptr };
    uint64_t  cmd_bus  { 0 };  void *cmd_cpu  { nullptr };
    uint64_t  wb_bus   { 0 };  void *wb_cpu   { nullptr };  // rptr/wptr shadow page

    // Stashed from the MES firmware header (mes_uc_start_addr_lo/hi)
    // when LoadFirmware processes the matching IP fw type. Set this
    // before mes_enable() — mes_enable programs CP_MES_PRGRM_CNTR_START
    // with `uc_start_addr >> 2`.
    uint64_t  uc_start_addr { 0 };

    // Per-pipe ring write-back state derived during queue_init. The
    // GPU writes the ring's read-pointer + (optional) write-pointer
    // poll shadow into wb_bus + wb_rptr_offset / wb_wptr_offset.
    uint64_t  ring_rptr_gpu_addr { 0 };   // wb base + 0
    uint64_t  ring_wptr_gpu_addr { 0 };   // wb base + 0x40
    uint32_t  ring_size_dwords   { 0 };
    uint32_t  doorbell_index     { 0 };   // BAR2 doorbell slot for this ring
};

struct MESContext {
    MESInstance pipe[kMaxMESPipes];
    bool        sched_ucode_loaded { false };  // RS64_MES (76) seen via LoadFirmware
    bool        kiq_ucode_loaded   { false };  // RS64_KIQ  (78) seen — N/A for uni
    bool        uni_mes_active     { false };

    // MES scheduler firmware version. Read from CP_MES_GP3_LO after
    // mes_v12_0_queue_init by upstream (mes_v12_0.c:1506). Only the
    // low 16 bits matter (AMDGPU_MES_VERSION_MASK = 0xffff). Used to
    // gate SET_HW_RESOURCES_1: upstream sends that frame only when
    // sched_version >= 0x4b (mes_v12_0_hw_init:1859).
    uint32_t  sched_version { 0 };
    uint32_t  kiq_version   { 0 };

    // SET_HW_RESOURCES_1 cleaner-shader fence buffer. Allocated lazily
    // by mes_set_hw_resources_1.  Matches mes->resource_1_gpu_addr in
    // upstream (mes_v12_0.c:721).
    SysMem    resource_1_mem {};
    uint64_t  resource_1_bus { 0 };

    // Lazy-allocated by mes_set_hw_resources on first call. Lifetime
    // is tied to the MESContext (i.e. the driver instance).
    SysMem    sch_ctx_mem       {};
    SysMem    status_fence_mem  {};
    uint64_t  sch_ctx_bus       { 0 };
    uint64_t  status_fence_bus  { 0 };
};

constexpr uint32_t kMES_VERSION_MASK     = 0x0000FFFFu;
constexpr uint32_t kMES_Resource1Bytes   = 4096;
constexpr uint32_t kMES_HwResources1MinSchedVersion = 0x4b;

//------------------------------------------------------------------
// Helper — GRBM_GFX_CNTL select. Mirrors soc21_grbm_select(adev,
// me, pipe, queue, vmid). me=3 picks MES; me=0/pipe=0/queue=0/vmid=0
// is the "deselect" state used after the GRBM-protected writes are
// done.
//------------------------------------------------------------------
static inline void
mes_grbm_select(const DeviceContext &dev, uint32_t me, uint32_t pipe,
                uint32_t queue, uint32_t vmid)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return;
    // gc_12_0_0_offset.h: regGRBM_GFX_CNTL = 0x0900, BASE_IDX 1 (the
    // reference used BASE_IDX 0 here).
    uint32_t reg = MES_REG(dev, GRBM_GFX_CNTL);
    uint32_t v = 0;
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, PIPEID,  pipe);
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, MEID,    me);
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, QUEUEID, queue);
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, VMID,    vmid);
    WREG32(dev, reg, v);
}

//------------------------------------------------------------------
// API
//------------------------------------------------------------------

// Allocate EOP/MQD/ring/cmd_buf for one pipe. Idempotent.
// `gmc` is needed to bind the sysmem pages into GART (deviation 2);
// gmc.inited must be true and gmc_init must have run.
kern_return_t mes_alloc_storage(DeviceContext &dev, GMCContext &gmc,
                                MESInstance &inst);

// Release everything mes_alloc_storage / the lazy allocators took.
// GART slots are bump-allocated and are NOT reclaimed (see
// gmc_bind_existing) — the PTEs simply stop being referenced.
void mes_release_storage(MESContext &mes);

// CP_MES_CNTL enable/disable for uni_mes pipe 0. Writes
// PRGRM_CNTR_START/_HI from inst.uc_start_addr (caller must have
// set this before calling, else mes_enable returns NotReady).
kern_return_t mes_enable(const DeviceContext &dev,
                         MESContext &mes, bool enable);

// Stash the uc_start_addr that was just parsed out of the MES
// firmware header. Called from LoadFirmware after a successful
// psp_load_ip_fw for RS64_MES / RS64_KIQ / uni_mes.
//
// `pipe` selects which MESInstance to set on. `uc_start_addr` is
// the absolute value from `mes_uc_start_addr_lo | (hi << 32)`
// (NOT pre-shifted; mes_enable does the >> 2 itself).
kern_return_t mes_set_uc_start_addr(MESContext &mes, MESPipe pipe,
                                    uint64_t uc_start_addr);

// Deviation 3 — port of the MES branch of MacAMDGPU.cpp's LoadFirmware
// (MacAMDGPU.cpp:2011-2024): reads mes_firmware_header_v1_0 out of a
// `gc_<v>_uni_mes.bin` (or `_mes.bin`) blob and forwards
// mes_uc_start_addr_lo|hi to mes_set_uc_start_addr.
kern_return_t mes_parse_ucode_header(MESContext &mes, MESPipe pipe,
                                     const uint8_t *bin, uint64_t size);

// Deviation 3 — push the MES microcode + data through the PSP.
// Mirrors amdgpu_ucode_extract's extract_mes payload split
// (CP_MES / CP_MES_DATA for the SCHED pipe, CP_MES_KIQ /
// MES_KIQ_STACK for the KIQ pipe of a uni_mes blob) plus
// psp_load_non_psp_fw step 6. Also parses the header. The PSP GPCOM
// ring must be up and the TMR set up. No-op success if the caller
// already loaded MES (mes.sched_ucode_loaded).
kern_return_t mes_load_firmware(DeviceContext &dev, PSPContext &psp,
                                MESContext &mes,
                                const uint8_t *bin, uint64_t size);

//------------------------------------------------------------------
// MES API wire format — abridged subset of mes_v12_api_def.h
//------------------------------------------------------------------
constexpr uint32_t kMES_API_FRAME_DWORDS = 64;          // every msg is 64 dw
constexpr uint32_t kMES_API_TYPE_SCHEDULER = 1;

// MES_SCH_API_OPCODE subset (the ones we care about for first PM4):
namespace MESSchOp {
    constexpr uint32_t SET_HW_RSRC               = 0;
    constexpr uint32_t SET_SCHEDULING_CONFIG     = 1;
    constexpr uint32_t ADD_QUEUE                 = 2;
    constexpr uint32_t REMOVE_QUEUE              = 3;
    constexpr uint32_t RESET                     = 8;   // 0.0.623 (G1): MES_SCH_API_RESET (mes_v12_api_def.h enum MES_SCH_API_OPCODE)
    constexpr uint32_t QUERY_SCHEDULER_STATUS    = 11;
    constexpr uint32_t SET_HW_RSRC_1             = 19;
}

// MES_API_HEADER bit layout — type[3:0], opcode[11:4], dwsize[19:12],
// reserved[31:20].
static inline uint32_t
mes_api_header(uint32_t type, uint32_t opcode, uint32_t dwsize)
{
    return (type   & 0xFu)
         | ((opcode & 0xFFu) << 4)
         | ((dwsize & 0xFFu) << 12);
}

//------------------------------------------------------------------
// Per-call API status footprint that lives inside every MES message.
// The dext sets `fence_addr` to a 64-bit GPU-side WB slot and
// `fence_value` to a per-call sequence value (0.0.626: it was the constant 1;
// upstream mes_v12_0 increments it); MES writes that value into the slot once
// the API completes. (Failure encoding lives in the high 32 bits — see
// upstream comment in mes_v12_api_def.h.)
//------------------------------------------------------------------
struct MES_API_Status {
    uint64_t fence_addr;
    uint64_t fence_value;
};

//------------------------------------------------------------------
// mes_submit_pkt — port of mes_v12_0_submit_pkt_and_poll_completion.
//
// `pkt` must point to a buffer of exactly kMES_API_FRAME_DWORDS
// dwords. `api_status_off_dw` is the dword offset *inside* `pkt`
// where the embedded MES_API_Status starts. We patch fence_addr +
// fence_value into that slot, write the whole frame into the
// scheduler ring, append a QUERY_SCHEDULER_STATUS that chains a
// second fence on the ring's own fence area, kick the ring's
// doorbell, then poll the status slot.
//
// Returns kIOReturnSuccess if the status slot latches lower-32 == THIS
// frame's own fence value (0.0.626: a per-call sequence in 1..0x7fffffff,
// never the constant 1) within timeout_us. A late completion of an
// EARLIER frame carries an earlier value, so it can never be read as the
// ack of this one.
//------------------------------------------------------------------
kern_return_t mes_submit_pkt(const DeviceContext &dev, MESContext &mes,
                             MESPipe pipe,
                             const uint32_t *pkt,
                             uint32_t api_status_off_dw,
                             uint64_t timeout_us);

// 0.0.626 (G1 review F3): true once ANY mes_submit_pkt in this boot returned kIOReturnTimeout (0xe00002d6): the firmware may still own what that
// frame touched. Sticky for the boot; the G1 recovery verbs refuse every further MES method when it is set.
bool mes_timeout_seen();

// Convenience wrapper — sends MES_SCH_API_QUERY_SCHEDULER_STATUS to
// the given pipe. Returns kIOReturnSuccess on a successful echo.
kern_return_t mes_query_sched_status(const DeviceContext &dev,
                                     MESContext &mes, MESPipe pipe);

// SET_HW_RESOURCES bus addresses (scheduler context + status fence)
// live alongside the regular MES storage. Allocated lazily by
// mes_set_hw_resources on first call, kept alive thereafter.
constexpr uint32_t kMES_SchCtxBytes      = 4096;
constexpr uint32_t kMES_StatusFenceBytes = 4096;

// Doorbell offsets handed to MES for the 5 priority levels now come from
// DeviceContext::doorbell.index.mes_aggregated[] (amdgpu_ip.h), so there is
// one place that owns the BAR2 layout. This constant is the fallback used
// only if the doorbell module never ran.
constexpr uint32_t kMES_AggregatedDoorbellsBase = 0x1A0;

//------------------------------------------------------------------
// Wire-format structs for the MES API messages.
//
// Natural alignment matches upstream's Linux/x86_64 layout exactly
// (alignof(uint64_t) == 8 on both platforms), so as long as we
// keep field order + types verbatim from mes_v12_api_def.h the
// resulting byte layout is bit-compatible. The trailing
// `padding[…]` arrays round each frame up to API_FRAME_SIZE_IN_DWORDS.
//------------------------------------------------------------------

constexpr uint32_t kMES_PriorityLevels = 5;  // AMD_PRIORITY_NUM_LEVELS

struct MES_Header_Wire {
    uint32_t u32All;            // type[3:0] | opcode[11:4] | dwsize[19:12]
};

struct MES_SetHwResources {
    MES_Header_Wire header;
    uint32_t vmid_mask_mmhub;
    uint32_t vmid_mask_gfxhub;
    uint32_t gds_size;
    uint32_t paging_vmid;
    uint32_t compute_hqd_mask[8];
    uint32_t gfx_hqd_mask[2];
    uint32_t sdma_hqd_mask[2];
    uint32_t aggregated_doorbells[5];
    uint64_t g_sch_ctx_gpu_mc_ptr;
    uint64_t query_status_fence_gpu_mc_ptr;
    uint32_t gc_base[8];
    uint32_t mmhub_base[8];
    uint32_t osssys_base[8];
    MES_API_Status api_status;
    uint32_t flags;
    uint32_t oversubscription_timer;
    uint64_t doorbell_info;
    uint64_t event_intr_history_gpu_mc_ptr;
    uint64_t timestamp;
    uint32_t os_tdr_timeout_in_sec;
    uint32_t pad[1];  // bring total to 64 dw = 256 bytes
};
static_assert(sizeof(MES_SetHwResources) == 64 * 4,
              "MES_SetHwResources must be 64 dwords");

// SET_HW_RESOURCES flag bit positions (mirrors upstream packed
// bitfield in mes_v12_api_def.h:275-298). Layout (LSB→MSB):
//   bit  0  : disable_reset
//   bit  1  : use_different_vmid_compute
//   bit  2  : disable_mes_log
//   bit  3  : apply_mmhub_pgvm_invalidate_ack_loss_wa
//   bit  4  : apply_grbm_remote_register_dummy_read_wa
//   bit  5  : second_gfx_pipe_enabled
//   bit  6  : enable_level_process_quantum_check
//   bit  7  : legacy_sch_mode
//   bit  8  : disable_add_queue_wptr_mc_addr
//   bit  9  : enable_mes_event_int_logging
//   bit 10  : enable_reg_active_poll
//   bit 11  : use_disable_queue_in_legacy_uq_preemption
//   bit 12  : send_write_data
//   bit 13  : os_tdr_timeout_override
//   bit 14  : use_rs64mem_for_proc_gang_ctx
//   bit 15  : halt_on_misaligned_access
//   bit 16  : use_add_queue_unmap_flag_addr
//   bit 17  : enable_mes_sch_stb_log
//   bit 18  : limit_single_process
//   bit 19..20 : unmapped_doorbell_handling (2 bits) — upstream
//                writes value 1 (basic version) per mes_v12_0.c:792
//   bit 21  : enable_mes_fence_int
//   bit 22  : enable_lr_compute_wa
constexpr uint32_t kSetHwRsrcFlag_disable_reset                      = 1u <<  0;
constexpr uint32_t kSetHwRsrcFlag_use_different_vmid_compute         = 1u <<  1;
constexpr uint32_t kSetHwRsrcFlag_disable_mes_log                    = 1u <<  2;
constexpr uint32_t kSetHwRsrcFlag_enable_level_process_quantum_check = 1u <<  6;
constexpr uint32_t kSetHwRsrcFlag_enable_reg_active_poll             = 1u << 10;
constexpr uint32_t kSetHwRsrcFlag_unmapped_doorbell_handling_BASIC   = 1u << 19;

struct MES_AddQueue {
    MES_Header_Wire header;
    uint32_t process_id;
    uint64_t page_table_base_addr;
    uint64_t process_va_start;
    uint64_t process_va_end;
    uint64_t process_quantum;
    uint64_t process_context_addr;
    uint64_t gang_quantum;
    uint64_t gang_context_addr;
    uint32_t inprocess_gang_priority;
    uint32_t gang_global_priority_level;
    uint32_t doorbell_offset;
    uint32_t _pad0;             // align mqd_addr to 8
    uint64_t mqd_addr;
    uint64_t wptr_addr;
    uint64_t h_context;
    uint64_t h_queue;
    uint32_t queue_type;
    uint32_t gds_base;          // mes_v12_api_def.h:357 — was MISSING.
    uint32_t gds_size;          // union with kfd_queue_size
    uint32_t gws_base;
    uint32_t gws_size;
    uint32_t oa_mask;
    // Natural u64 alignment pads to 144 here. trap_handler_addr at
    // offset 144 (dw 36) — matches upstream's offset (compiler also
    // inserts the same 4-byte pad after `oa_mask` upstream).
    uint64_t trap_handler_addr;
    uint32_t vm_context_cntl;
    uint32_t flags;             // packed bitfield
    // api_status (u64 first field) is naturally aligned — vm_context_cntl
    // + flags = 8 bytes; sum-since-trap_handler_addr u64 = 16 bytes; the
    // running offset is 8-aligned.
    MES_API_Status api_status;
    uint64_t tma_addr;
    uint32_t sch_id;
    uint32_t _pad2;             // align timestamp to 8
    uint64_t timestamp;
    uint32_t process_context_array_index;
    uint32_t gang_context_array_index;
    uint32_t pipe_id;
    uint32_t queue_id;
    uint32_t alignment_mode_setting;
    uint32_t full_sh_mem_config_data;
    // Tail pad rounds to 64 dwords (256 bytes). After full_sh_mem_config_data
    // the running offset is 216; we need 40 more bytes = 10 dwords.
    uint32_t pad[10];
};
static_assert(sizeof(MES_AddQueue) == 64 * 4,
              "MES_AddQueue must be 64 dwords");

// MES_AddQueue flags bitfield encoding (bit position).
// Mirrors mes_v12_api_def.h:369-385 packed bitfield order:
//   bit  0    : paging
//   bits 1..4 : debug_vmid (4 bits)
//   bit  5    : program_gds
//   bit  6    : is_gang_suspended
//   bit  7    : is_tmz_queue
//   bit  8    : map_kiq_utility_queue
//   bit  9    : is_kfd_process
//   bit 10    : trap_en
//   bit 11    : is_aql_queue
//   bit 12    : skip_process_ctx_clear
//   bit 13    : map_legacy_kq
//   bit 14    : exclusively_scheduled
//   bit 15    : is_long_running
//   bit 16    : is_dwm_queue
constexpr uint32_t kAddQueueFlag_paging                = 1u <<  0;
constexpr uint32_t kAddQueueFlag_program_gds           = 1u <<  5;
constexpr uint32_t kAddQueueFlag_is_gang_suspended     = 1u <<  6;
constexpr uint32_t kAddQueueFlag_is_tmz_queue          = 1u <<  7;
constexpr uint32_t kAddQueueFlag_map_kiq_utility_queue = 1u <<  8;
constexpr uint32_t kAddQueueFlag_is_kfd_process        = 1u <<  9;
constexpr uint32_t kAddQueueFlag_trap_en               = 1u << 10;
constexpr uint32_t kAddQueueFlag_is_aql_queue          = 1u << 11;
constexpr uint32_t kAddQueueFlag_skip_process_ctx_clear= 1u << 12;
constexpr uint32_t kAddQueueFlag_map_legacy_kq         = 1u << 13;
constexpr uint32_t kAddQueueFlag_exclusively_scheduled = 1u << 14;
constexpr uint32_t kAddQueueFlag_is_long_running       = 1u << 15;
constexpr uint32_t kAddQueueFlag_is_dwm_queue          = 1u << 16;

// MESAPI__REMOVE_QUEUE (mes_v12_api_def.h). Natural alignment matches
// upstream's x86_64 layout; see the note above MES_AddQueue.
struct MES_RemoveQueue {
    MES_Header_Wire header;
    uint32_t doorbell_offset;
    uint64_t gang_context_addr;
    uint32_t flags;             // packed bitfield, see kRemoveQueueFlag_*
    uint32_t _pad0;             // align api_status to 8
    MES_API_Status api_status;
    uint32_t pipe_id;
    uint32_t queue_id;
    uint64_t tf_addr;
    uint32_t tf_data;
    uint32_t queue_type;
    uint64_t timestamp;
    uint32_t gang_context_array_index;
    uint32_t pad[45];           // to 64 dwords
};
static_assert(sizeof(MES_RemoveQueue) == 64 * 4,
              "MES_RemoveQueue must be 64 dwords");

// REMOVE_QUEUE flags (bit positions, mes_v12_api_def.h):
//   bit 0 reserved01, 1 unmap_kiq_utility_queue, 2 preempt_legacy_gfx_queue,
//   bit 3 unmap_legacy_queue, 4 remove_queue_after_reset
constexpr uint32_t kRemoveQueueFlag_unmap_kiq_utility_queue = 1u << 1;
constexpr uint32_t kRemoveQueueFlag_preempt_legacy_gfx      = 1u << 2;
constexpr uint32_t kRemoveQueueFlag_unmap_legacy_queue      = 1u << 3;
constexpr uint32_t kRemoveQueueFlag_remove_after_reset      = 1u << 4;

// 0.0.623 (G1, hang recovery): MESAPI__RESET (mes_v12_api_def.h:544-618). Only the legacy-GFX form is built: reset_legacy_gfx = 1 with the
// low-priority (_lp) queue fields, exactly what mes_v12_0_reset_hw_queue sends when input->legacy_gfx (the path gfx_v12_0_reset_kgq takes:
// gfx_v12_0.c:1608 use_mmio_for_reset = false for the ME). The offsets below were CONFIRMED by compiling the upstream header with clang and
// printing offsetof: header 0, gang_context_addr 8, doorbell_offset 16, doorbell_offset_addr 24, queue_type 32, pipe_id_lp 36,
// queue_id_lp 40, vmid_id_lp 44, mqd_mc_addr_lp 48, doorbell_offset_lp 56, wptr_addr_lp 64, pipe_id_hp 72, api_status 112, active_vmids 128,
// timestamp 136, size 256; reset_legacy_gfx is bit 3 of dword 1.
struct MES_ResetQueue {
    MES_Header_Wire header;
    uint32_t flags;                 // packed bitfield, see kResetFlag_*
    uint64_t gang_context_addr;
    uint32_t doorbell_offset;       // valid only with reset_queue_only
    uint32_t _pad0;
    uint64_t doorbell_offset_addr;
    uint32_t queue_type;
    uint32_t pipe_id_lp;
    uint32_t queue_id_lp;
    uint32_t vmid_id_lp;
    uint64_t mqd_mc_addr_lp;
    uint32_t doorbell_offset_lp;
    uint32_t _pad1;
    uint64_t wptr_addr_lp;
    uint32_t pipe_id_hp;
    uint32_t queue_id_hp;
    uint32_t vmid_id_hp;
    uint32_t _pad2;
    uint64_t mqd_mc_addr_hp;
    uint32_t doorbell_offset_hp;
    uint32_t _pad3;
    uint64_t wptr_addr_hp;
    MES_API_Status api_status;
    uint32_t active_vmids;
    uint32_t _pad4;
    uint64_t timestamp;
    uint32_t gang_context_array_index;
    uint32_t connected_queue_index;
    uint32_t connected_queue_index_p1;
    uint32_t pad[25];               // to 64 dwords
};
static_assert(sizeof(MES_ResetQueue) == 64 * 4, "MES_ResetQueue must be 64 dwords");
static_assert(offsetof(MES_ResetQueue, flags) == 4 && offsetof(MES_ResetQueue, gang_context_addr) == 8 &&
              offsetof(MES_ResetQueue, doorbell_offset) == 16 && offsetof(MES_ResetQueue, doorbell_offset_addr) == 24 &&
              offsetof(MES_ResetQueue, queue_type) == 32 && offsetof(MES_ResetQueue, pipe_id_lp) == 36 &&
              offsetof(MES_ResetQueue, queue_id_lp) == 40 && offsetof(MES_ResetQueue, vmid_id_lp) == 44 &&
              offsetof(MES_ResetQueue, mqd_mc_addr_lp) == 48 && offsetof(MES_ResetQueue, doorbell_offset_lp) == 56 &&
              offsetof(MES_ResetQueue, wptr_addr_lp) == 64 && offsetof(MES_ResetQueue, pipe_id_hp) == 72 &&
              offsetof(MES_ResetQueue, api_status) == 112 && offsetof(MES_ResetQueue, active_vmids) == 128 &&
              offsetof(MES_ResetQueue, timestamp) == 136 && offsetof(MES_ResetQueue, gang_context_array_index) == 144 &&
              offsetof(MES_ResetQueue, connected_queue_index_p1) == 152,
              "MES_ResetQueue: the upstream MESAPI__RESET offsets (measured with clang on mes_v12_api_def.h)");
// RESET flags (bit positions, mes_v12_api_def.h:550-560): 0 reset_queue_only, 1 hang_detect_then_reset, 2 hang_detect_only, 3 reset_legacy_gfx,
// 4 use_connected_queue_index, 5 use_connected_queue_index_p1.
constexpr uint32_t kResetFlag_reset_legacy_gfx = 1u << 3;
// mes_v12_0_unmap_legacy_queue (mes_v12_0.c:762-766): remove_queue_after_reset is set only for a reset-time unmap and only from sched version 0x5a.
constexpr uint32_t kMES_RemoveAfterResetMinVersion = 0x5au;

// MES queue types (mirrors enum MES_QUEUE_TYPE).
constexpr uint32_t kMESQueueType_GFX     = 0;
constexpr uint32_t kMESQueueType_COMPUTE = 1;
constexpr uint32_t kMESQueueType_SDMA    = 2;

//------------------------------------------------------------------
// Inputs for the high-level helpers.
//------------------------------------------------------------------
struct MESSetHwResourcesInput {
    uint32_t vmid_mask_mmhub;
    uint32_t vmid_mask_gfxhub;
    uint32_t compute_hqd_mask[8];
    uint32_t gfx_hqd_mask[2];
    uint32_t sdma_hqd_mask[2];
    uint32_t aggregated_doorbells[kMES_PriorityLevels];
    // gc_base / mmhub_base / osssys_base copied from DeviceContext.
};

struct MESAddQueueInput {
    uint32_t process_id;
    uint64_t mqd_addr;
    uint64_t wptr_addr;          // GPU bus address of wptr shadow
    uint32_t doorbell_offset;
    uint32_t queue_type;         // kMESQueueType_*
    uint32_t pipe_id;
    uint32_t queue_id;
    uint32_t inprocess_gang_priority;       // 0..4 (AMD_PRIORITY_LEVEL_*)
    uint32_t gang_global_priority_level;    // 0..4
    uint64_t gang_context_addr;             // 4 KB sysmem allocated by user
    uint64_t process_context_addr;          // 4 KB sysmem allocated by user
    uint64_t page_table_base_addr;          // GART PD base — VMID 0 path
    uint32_t flags;                         // bitwise-OR of kAddQueueFlag_*
};

// Build + submit a SET_HW_RESOURCES message on the SCHED pipe.
// Lazy-allocates the scheduler context + query-status fence buffers
// on first call and stashes them on MESContext. `gmc` is needed for
// the GART bind (deviation 2).
kern_return_t mes_set_hw_resources(DeviceContext &dev, GMCContext &gmc,
                                   MESContext &mes,
                                   const MESSetHwResourcesInput &in);

// Build + submit an ADD_QUEUE message. Pipes always SCHED.
kern_return_t mes_add_hw_queue(const DeviceContext &dev, MESContext &mes,
                               const MESAddQueueInput &in);

// mes_v12_0_unmap_legacy_queue with action == UNMAP_LATENCY (the plain
// teardown case): REMOVE_QUEUE { doorbell_offset, pipe_id, queue_id,
// queue_type, unmap_legacy_queue = 1 } on the SCHED pipe. Call before
// halting the CP so the scheduler does not keep a mapped queue pointing at
// memory we are about to free.
kern_return_t mes_remove_hw_queue(const DeviceContext &dev, MESContext &mes,
                                  uint32_t queue_type, uint32_t pipe_id,
                                  uint32_t queue_id, uint32_t doorbell_offset);

// 0.0.623 (G1): mes_remove_hw_queue with the REMOVE_QUEUE flags word given (kRemoveQueueFlag_*). mes_remove_hw_queue is this with
// kRemoveQueueFlag_unmap_legacy_queue; G1's remap adds kRemoveQueueFlag_remove_after_reset after an acked RESET (mes_v12_0.c:762-766).
kern_return_t mes_remove_hw_queue_flags(const DeviceContext &dev, MESContext &mes,
                                        uint32_t queue_type, uint32_t pipe_id,
                                        uint32_t queue_id, uint32_t doorbell_offset,
                                        uint32_t flags);
// 0.0.623 (G1): mes_v12_0_reset_hw_queue for a legacy (kernel) GFX queue: RESET { queue_type GFX, reset_legacy_gfx = 1, pipe_id_lp, queue_id_lp,
// vmid_id_lp, mqd_mc_addr_lp, doorbell_offset_lp, wptr_addr_lp } on the SCHED pipe, bounded at 2 s by mes_submit_pkt. Sends nothing but the frame.
kern_return_t mes_reset_legacy_gfx_queue(const DeviceContext &dev, MESContext &mes,
                                         uint32_t pipe_id, uint32_t queue_id, uint32_t vmid,
                                         uint64_t mqd_addr, uint32_t doorbell_offset,
                                         uint64_t wptr_addr);

// Program CP_MES_DOORBELL_CONTROL{1..5} with the 5 aggregated
// doorbell offsets + CP_HQD_GFX_CONTROL.DB_UPDATED_MSG_EN.
// Doorbell offsets are bytes (already shifted via OFFSET field write).
kern_return_t mes_init_aggregated_doorbell(const DeviceContext &dev,
                                           const uint32_t doorbells[5]);

// Port of mes_v12_0_kiq_setting (mes_v12_0.c:1728). Writes RLC_CP_SCHEDULERS
// with the (me, pipe, queue) encoding for the KIQ ring + scheduler-active
// bit so RLC routes IRQs to the correct queue.
kern_return_t mes_kiq_setting(const DeviceContext &dev,
                              uint32_t me, uint32_t pipe, uint32_t queue);

// Port of mes_v12_0_enable_unmapped_doorbell_handling (mes_v12_0.c:863).
// Sets CP_UNMAPPED_DOORBELL.PROC_LSB = 0xd + ENABLE = 1 so the CP
// forwards doorbell writes to queues MES hasn't yet mapped.
kern_return_t mes_enable_unmapped_doorbell_handling(const DeviceContext &dev,
                                                    bool enable);

// Port of mes_v12_0_set_hw_resources_1 (mes_v12_0.c:711). Sent
// AFTER set_hw_resources, gated on sched_version >= 0x4b. Lazily
// allocates the cleaner_shader_fence buffer (hence `gmc`).
kern_return_t mes_set_hw_resources_1(DeviceContext &dev, GMCContext &gmc,
                                     MESContext &mes);

// Read MES scheduler version from CP_MES_GP3_LO after mes_enable
// (mes_v12_0.c:1505). Stashed on mes.sched_version.
kern_return_t mes_read_sched_version(const DeviceContext &dev,
                                     MESContext &mes, MESPipe pipe);

// Program the MES SCHED pipe's HQD registers. Mirrors upstream's
// mes_v12_0_queue_init_register + the field defaults from
// mes_v12_0_mqd_init. Caller must have run mes_alloc_storage on
// the matching pipe. Does NOT depend on the MES microcode being
// loaded — the writes happen via GRBM_GFX_CNTL select to the MES
// pipe and program the queue state for when MES is later activated.
//
// We also stash the same values into the MQD memory at the
// upstream v12_compute_mqd byte offsets so that MES, once
// running, sees a consistent picture if it re-loads context.
kern_return_t mes_queue_init(const DeviceContext &dev,
                             MESContext &mes, MESPipe pipe);

// MESInit stage entry — alloc storage for every pipe we plan to
// drive, then (if microcode is already loaded) call mes_enable.
//
// `mes_fw` / `mes_fw_size` are the uni_mes .bin bytes (deviation 3).
// Pass nullptr/0 to let mes_init_full look the blob up itself via
// fw_get(FwId::GC_UNI_MES).
kern_return_t mes_init_full(DeviceContext &dev, PSPContext &psp,
                            GMCContext &gmc, MESContext &mes,
                            const uint8_t *mes_fw = nullptr,
                            uint64_t mes_fw_size = 0);

} // namespace amdgpu
