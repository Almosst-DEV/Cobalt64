//
//  amdgpu_cp.h — Command Processor (PFP / ME / MEC) + GFX ring / KIQ
//  infrastructure for GFX12 (RDNA4), x86 kernel kext.
//
//  Origin: lemonade-sdk/mac-amdgpu (MIT) @ commit 3bdeed2,
//          dext/amdgpu/amdgpu_cp.h (arm64 DriverKit dext).
//  MIT License. See ../../NOTICE.
//
//  Upstream sources behind the reference:
//      drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c   (cp_gfx_resume, cp_gfx_enable,
//                                                cp_compute_enable,
//                                                cp_set_doorbell_range,
//                                                cp_gfx_start, cp_resume)
//      drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c
//
// ===========================================================================
//  Deviations from reference (dext/amdgpu/amdgpu_cp.h @ 3bdeed2)
// ===========================================================================
//  1. DriverKit members removed. `IOBufferMemoryDescriptor *x_buf` +
//     `IODMACommand *x_dma` pairs become one `amdgpu::SysMem x_sys` each
//     (PORTING.md rule 2). No <DriverKit/…> includes anywhere.
//
//  2. `*_bus` now means "GART MC address", not "DART iova". The reference
//     ran on Apple Silicon where IODMACommand::PrepareForDMA handed back a
//     DART-translated device address the CP could use directly. There is no
//     IOMMU here, so `SysMem::bus` is the raw host physical address and the
//     GPU cannot reach it without a GART mapping. cp_alloc_storage therefore
//     binds each buffer with gmc_bind_existing() and stores the returned MC
//     address in ring_bus / mqd_bus / wb_bus. Field names, meanings for the
//     hardware ("the address you program into CP_RB0_BASE") and every
//     consumer are unchanged. The raw bus address is kept in `*_sys.bus`.
//
//  3. `cp_kiq_smoke_test`'s `MESContext &mes` parameter became
//     `bool mes_kiq_armed` (same position, same role: the "MES SCHED has
//     armed the KIQ" gate). amdgpu_mes.h is not part of this port yet and
//     this header must not depend on it. When the MES module lands, its
//     caller passes `mes.pipe[0].inited && mes.pipe[0].enabled`.
//
//  4. All mask literals carry a `u` suffix (the reference's are plain ints)
//     so REG_SET_FIELD stays warning-free under -Wsign-conversion.
//
//  5. CPContext gained default member initializers, matching GMCContext /
//     GARTContext in this tree. The reference relied on IIG's IONewZero.
//     Callers should still zero the struct; `inited` is the idempotency gate.
//
//  6. NOT IN THE REFERENCE AT ALL: cp_config_gfx_rs64 / cp_gfx_start /
//     cp_ring_fetch_probe / cp_dump_state, plus the register and field
//     definitions they need. The reference's cp_v12_0.cpp never ported
//     gfx_v12_0_config_gfx_rs64, so the RS64 PFP/ME/MEC cores were being
//     unhalted with a zero program counter. CPContext gained the
//     rs64_* / pfp_pc / me_pc / mec_pc / fetch_proven fields for the
//     failure dump.
//
//  Memory layout (x86 / Navi48Bringup):
//      GFX ring buffer  — 16 KB, system memory, bound into GART.
//                         The CP fetches PM4 from here through GART
//                         translation; the CPU writes PM4 directly through
//                         the kernel VA (no BAR aperture needed).
//      KIQ MQD          — 4 KB, system memory, bound into GART. The CP
//                         reads it once at queue-create time.
//      Write-back page  — 16 KB, system memory, bound into GART. The CP
//                         writes rptr / wptr / fence values here; the host
//                         reads the same backing store through wb_cpu.
//      Smoke-test fence — 64 B in VRAM (gmc.vram_alloc), read back through
//                         the MM_INDEX/MM_DATA window. VRAM exactly as in
//                         the reference — see cp_kiq_smoke_test.
//

#pragma once

#include <stdint.h>
#include <IOKit/IOReturn.h>

#include "amdgpu_ip.h"
#include "amdgpu_regs.h"
#include "amdgpu_sysmem.h"
#include "amdgpu_vram.h"
#include "amdgpu_pm4.h"

namespace amdgpu {

struct GMCContext;   // forward — we ask it for GART bindings + VRAM

// GC register offsets for the CP HQD registers we touch.
// Sourced from gc_12_0_0_offset.h. Used by cp_hqd_program / cp_enable.
namespace CPRegs {
    // gc_12_0_0_offset.h: regCP_RB_WPTR_DELAY  = 0x0f61
    constexpr uint32_t CP_RB_WPTR_DELAY           = 0x0F61;
    // gc_12_0_0_offset.h: regCP_RB0_BASE = 0x1DE0
    constexpr uint32_t CP_RB0_BASE                = 0x1DE0;
    constexpr uint32_t CP_RB0_CNTL                = 0x1DE1;
    constexpr uint32_t CP_RB0_RPTR_ADDR           = 0x1DE3;
    constexpr uint32_t CP_RB0_RPTR_ADDR_HI        = 0x1DE4;
    // gc_12_0_0_offset.h: regCP_DEVICE_ID = 0x1deb
    constexpr uint32_t CP_DEVICE_ID               = 0x1DEB;
    constexpr uint32_t CP_RB_VMID                 = 0x1DF1;
    constexpr uint32_t CP_RB0_WPTR                = 0x1DF4;
    // gc_12_0_0_offset.h: regCP_RB0_WPTR_HI = 0x1df5
    constexpr uint32_t CP_RB0_WPTR_HI             = 0x1DF5;
    constexpr uint32_t CP_RB_DOORBELL_RANGE_LOWER = 0x1DFA;
    constexpr uint32_t CP_RB_DOORBELL_RANGE_UPPER = 0x1DFB;
    // gc_12_0_0_offset.h: regCP_MEC_DOORBELL_RANGE_{LOWER,UPPER} = 0x1dfc/0x1dfd
    constexpr uint32_t CP_MEC_DOORBELL_RANGE_LOWER = 0x1DFC;
    constexpr uint32_t CP_MEC_DOORBELL_RANGE_UPPER = 0x1DFD;
    // gc_12_0_0_offset.h: regCP_MAX_CONTEXT = 0x1e4e
    constexpr uint32_t CP_MAX_CONTEXT             = 0x1E4E;
    constexpr uint32_t CP_RB0_BASE_HI             = 0x1E51;
    // gc_12_0_0_offset.h: regCP_RB_WPTR_POLL_ADDR_LO/_HI = 0x1e8b/0x1e8c
    constexpr uint32_t CP_RB_WPTR_POLL_ADDR_LO    = 0x1E8B;
    constexpr uint32_t CP_RB_WPTR_POLL_ADDR_HI    = 0x1E8C;
    constexpr uint32_t CP_RB_DOORBELL_CONTROL     = 0x1E8D;
    // gc_12_0_0_offset.h: regCP_RB_ACTIVE = 0x1f40
    constexpr uint32_t CP_RB_ACTIVE               = 0x1F40;
    constexpr uint32_t CP_ME_CNTL                 = 0x0803;
    // gc_12_0_0_offset.h: regCP_MEC_RS64_CNTL = 0x2904, BASE_IDX 1.
    constexpr uint32_t CP_MEC_RS64_CNTL           = 0x2904;

    // ---- RS64 program-counter start registers (gfx_v12_0_config_gfx_rs64) ----
    // gc_12_0_0_offset.h:3578-3581, 3612-3613, 3674-3675 (BASE_IDX 0) and
    // 4946-4947, 5046-5047 (BASE_IDX 1 — the MEC pair sits in the other
    // segment, which is exactly the trap CP_MEC_RS64_CNTL already documents).
    constexpr uint32_t CP_PFP_PRGRM_CNTR_START        = 0x1E44;
    constexpr uint32_t CP_PFP_PRGRM_CNTR_START_HI     = 0x1E59;
    constexpr uint32_t CP_ME_PRGRM_CNTR_START         = 0x1E45;
    constexpr uint32_t CP_ME_PRGRM_CNTR_START_HI      = 0x1E79;
    constexpr uint32_t CP_MEC_RS64_PRGRM_CNTR_START   = 0x2900;
    constexpr uint32_t CP_MEC_RS64_PRGRM_CNTR_START_HI = 0x2938;

    // ---- RS64 instruction- / data-cache base registers ----
    // Every one of these is BASE_IDX 1. gc_12_0_0_offset.h:6196-6217
    // (IC bases), 6248-6255 + 5184-5187 (GFX RS64 DC), 6260-6267 +
    // 4966-4969 (MEC DC), 5176-5177 (CPC IC OP).
    // Linux writes them only in the AMDGPU_FW_LOAD_DIRECT loaders
    // (gfx_v12_0.c:2443-2505 PFP, 2587-2650 ME, 2902-2930 MEC), pointing
    // at the driver's own VRAM copy. Under PSP load the PSP is supposed to
    // have set them; on this card it does not — 0.0.14 showed PFP/ME start
    // and immediately fetch at VA page 0 — so cp_config_rs64_caches
    // programs them from the PSP's per-fw_type tmr_fw_addr.
    constexpr uint32_t CP_PFP_IC_BASE_LO          = 0x5840;
    constexpr uint32_t CP_PFP_IC_BASE_HI          = 0x5841;
    constexpr uint32_t CP_PFP_IC_BASE_CNTL        = 0x5842;
    constexpr uint32_t CP_PFP_IC_OP_CNTL          = 0x5843;
    constexpr uint32_t CP_ME_IC_BASE_LO           = 0x5844;
    constexpr uint32_t CP_ME_IC_BASE_HI           = 0x5845;
    constexpr uint32_t CP_ME_IC_BASE_CNTL         = 0x5846;
    constexpr uint32_t CP_ME_IC_OP_CNTL           = 0x5847;
    constexpr uint32_t CP_CPC_IC_BASE_LO          = 0x584C;
    constexpr uint32_t CP_CPC_IC_BASE_HI          = 0x584D;
    constexpr uint32_t CP_CPC_IC_BASE_CNTL        = 0x584E;
    constexpr uint32_t CP_CPC_IC_OP_CNTL          = 0x297A;
    constexpr uint32_t CP_GFX_RS64_DC_BASE0_LO    = 0x5863;
    constexpr uint32_t CP_GFX_RS64_DC_BASE1_LO    = 0x5864;
    constexpr uint32_t CP_GFX_RS64_DC_BASE0_HI    = 0x5865;
    constexpr uint32_t CP_GFX_RS64_DC_BASE1_HI    = 0x5866;
    constexpr uint32_t CP_GFX_RS64_DC_BASE_CNTL   = 0x2A08;
    constexpr uint32_t CP_GFX_RS64_DC_OP_CNTL     = 0x2A09;
    // regCP_MEC_MDBASE_{LO,HI} and regCP_MEC_DC_BASE_{LO,HI} are the SAME
    // register (0x5870 / 0x5871 under both names); it is per-pipe, selected
    // through GRBM_GFX_CNTL. gfx_v12_0.c:2924 uses the MDBASE spelling.
    constexpr uint32_t CP_MEC_MDBASE_LO           = 0x5870;
    constexpr uint32_t CP_MEC_MDBASE_HI           = 0x5871;
    constexpr uint32_t CP_MEC_DC_BASE_CNTL        = 0x290B;
    constexpr uint32_t CP_MEC_DC_OP_CNTL          = 0x290C;

    // ---- read-only diagnostics ----
    // gc_12_0_0_offset.h:2130-2131 regCP_STAT = 0x0f40 BASE_IDX 0
    constexpr uint32_t CP_STAT                    = 0x0F40;
    // gc_12_0_0_offset.h:2148-2149 regCP_RB0_RPTR = 0x0f60 BASE_IDX 0
    // (regCP_RB_RPTR is the same register under its generic name).
    constexpr uint32_t CP_RB0_RPTR                = 0x0F60;

    // ---- GFX HQD / MQD register block (gfx_v12_0_gfx_mqd_init targets) ----
    // gc_12_0_0_offset.h:3680-3731 — all BASE_IDX 0, per-queue behind
    // GRBM_GFX_CNTL. This is the block the CP firmware loads from a
    // v12_gfx_mqd when the MES maps the kernel GFX queue (ADD_QUEUE with
    // map_legacy_kq=1); MQD dwords 128.. mirror it one-to-one
    // (cp_mqd_base_addr <-> 0x1E7E ... cp_gfx_mqd_control <-> 0x1E9A).
    constexpr uint32_t CP_GFX_MQD_BASE_ADDR       = 0x1E7E;
    constexpr uint32_t CP_GFX_MQD_BASE_ADDR_HI    = 0x1E7F;
    constexpr uint32_t CP_GFX_HQD_ACTIVE          = 0x1E80;
    constexpr uint32_t CP_GFX_HQD_VMID            = 0x1E81;
    constexpr uint32_t CP_GFX_HQD_QUEUE_PRIORITY  = 0x1E84;
    constexpr uint32_t CP_GFX_HQD_QUANTUM         = 0x1E85;
    constexpr uint32_t CP_GFX_HQD_BASE            = 0x1E86;
    constexpr uint32_t CP_GFX_HQD_BASE_HI         = 0x1E87;
    constexpr uint32_t CP_GFX_HQD_RPTR            = 0x1E88;
    constexpr uint32_t CP_GFX_HQD_RPTR_ADDR       = 0x1E89;
    constexpr uint32_t CP_GFX_HQD_RPTR_ADDR_HI    = 0x1E8A;
    constexpr uint32_t CP_GFX_HQD_OFFSET          = 0x1E8E;
    constexpr uint32_t CP_GFX_HQD_CNTL            = 0x1E8F;
    constexpr uint32_t CP_GFX_HQD_CSMD_RPTR       = 0x1E90;
    constexpr uint32_t CP_GFX_HQD_WPTR            = 0x1E91;
    constexpr uint32_t CP_GFX_HQD_WPTR_HI         = 0x1E92;
    constexpr uint32_t CP_GFX_HQD_MAPPED          = 0x1E94;
    constexpr uint32_t CP_GFX_HQD_QUE_MGR_CONTROL = 0x1E95;
    constexpr uint32_t CP_GFX_HQD_HQ_STATUS0      = 0x1E98;
    constexpr uint32_t CP_GFX_HQD_HQ_CONTROL0     = 0x1E99;
    constexpr uint32_t CP_GFX_MQD_CONTROL         = 0x1E9A;
    // gc_12_0_0_offset.h:3528-3533 — CP interrupt control/status, BASE_IDX 0.
    constexpr uint32_t CP_INT_CNTL_RING0          = 0x1E0A;
    constexpr uint32_t CP_INT_STATUS_RING0        = 0x1E0D;
    // gc_12_0_0_offset.h:4348-4349 regSCRATCH_REG0 = 0x2040, BASE_IDX 1 —
    // the register gfx_v12_0_ring_test_ring writes through SET_UCONFIG_REG.
    constexpr uint32_t SCRATCH_REG0               = 0x2040;
    // ---- compute HQD block (gc_12_0_0_offset.h:3842-3953, all BASE_IDX 0) ----
    // Per-queue behind GRBM_GFX_CNTL(me, pipe, queue), the compute mirror of
    // the CP_GFX_HQD_* block above.
    constexpr uint32_t CP_MQD_BASE_ADDR            = 0x1FA9;
    constexpr uint32_t CP_HQD_ACTIVE               = 0x1FAB;
    constexpr uint32_t CP_HQD_PQ_BASE              = 0x1FB1;
    constexpr uint32_t CP_HQD_PQ_RPTR              = 0x1FB3;
    constexpr uint32_t CP_HQD_PQ_DOORBELL_CONTROL  = 0x1FB8;
    constexpr uint32_t CP_HQD_PQ_CONTROL           = 0x1FBA;
    constexpr uint32_t CP_HQD_PQ_WPTR_LO           = 0x1FDF;
}

//
// SOC15 BASE_IDX for each CPRegs entry, copied verbatim from
// gc_12_0_0_offset.h's reg<NAME>_BASE_IDX lines.
//
// The reference addressed all of these at BASE_IDX 0 (plain
// SOC15_REG_OFFSET). Two of them are BASE_IDX 1 and were therefore
// landing on unrelated registers:
//   * CP_MEC_RS64_CNTL (0x2904) — hardware reads 0 at GC[0] and
//     0x40030000 at GC[1]; a write at GC[0] did not stick.
//   * CP_ME_CNTL (0x0803) — the GC[0] write "read back correct", i.e.
//     it was writing and reading some *other* register while PFP/ME
//     stayed halted. This is the dangerous one.
// Everything else in the 0x1dXX / 0x1eXX / 0x1fXX CP window really is
// BASE_IDX 0 (CP_RB0_CNTL 0x1de1 BASE_IDX 0 is the hardware-confirmed
// anchor), so those addresses are unchanged.
//
namespace CPRegBaseIdx {
    constexpr int CP_RB_WPTR_DELAY            = 0;
    constexpr int CP_RB0_BASE                 = 0;
    constexpr int CP_RB0_CNTL                 = 0;
    constexpr int CP_RB0_RPTR_ADDR            = 0;
    constexpr int CP_RB0_RPTR_ADDR_HI         = 0;
    constexpr int CP_DEVICE_ID                = 0;
    constexpr int CP_RB_VMID                  = 0;
    constexpr int CP_RB0_WPTR                 = 0;
    constexpr int CP_RB0_WPTR_HI              = 0;
    constexpr int CP_RB_DOORBELL_RANGE_LOWER  = 0;
    constexpr int CP_RB_DOORBELL_RANGE_UPPER  = 0;
    constexpr int CP_MEC_DOORBELL_RANGE_LOWER = 0;
    constexpr int CP_MEC_DOORBELL_RANGE_UPPER = 0;
    constexpr int CP_MAX_CONTEXT              = 0;
    constexpr int CP_RB0_BASE_HI              = 0;
    constexpr int CP_RB_WPTR_POLL_ADDR_LO     = 0;
    constexpr int CP_RB_WPTR_POLL_ADDR_HI     = 0;
    constexpr int CP_RB_DOORBELL_CONTROL      = 0;
    constexpr int CP_RB_ACTIVE                = 0;
    constexpr int CP_ME_CNTL                  = 1;  // regCP_ME_CNTL_BASE_IDX 1
    constexpr int CP_MEC_RS64_CNTL            = 1;  // regCP_MEC_RS64_CNTL_BASE_IDX 1
    // RS64 program-counter start. PFP/ME live in GC[0]; the MEC pair lives
    // in GC[1] alongside CP_MEC_RS64_CNTL.
    constexpr int CP_PFP_PRGRM_CNTR_START         = 0;  // ..._BASE_IDX 0
    constexpr int CP_PFP_PRGRM_CNTR_START_HI      = 0;  // ..._BASE_IDX 0
    constexpr int CP_ME_PRGRM_CNTR_START          = 0;  // ..._BASE_IDX 0
    constexpr int CP_ME_PRGRM_CNTR_START_HI       = 0;  // ..._BASE_IDX 0
    constexpr int CP_MEC_RS64_PRGRM_CNTR_START    = 1;  // ..._BASE_IDX 1
    constexpr int CP_MEC_RS64_PRGRM_CNTR_START_HI = 1;  // ..._BASE_IDX 1
    // RS64 IC / DC bases — all BASE_IDX 1 without exception.
    constexpr int CP_PFP_IC_BASE_LO           = 1;
    constexpr int CP_PFP_IC_BASE_HI           = 1;
    constexpr int CP_PFP_IC_BASE_CNTL         = 1;
    constexpr int CP_PFP_IC_OP_CNTL           = 1;
    constexpr int CP_ME_IC_BASE_LO            = 1;
    constexpr int CP_ME_IC_BASE_HI            = 1;
    constexpr int CP_ME_IC_BASE_CNTL          = 1;
    constexpr int CP_ME_IC_OP_CNTL            = 1;
    constexpr int CP_CPC_IC_BASE_LO           = 1;
    constexpr int CP_CPC_IC_BASE_HI           = 1;
    constexpr int CP_CPC_IC_BASE_CNTL         = 1;
    constexpr int CP_CPC_IC_OP_CNTL           = 1;
    constexpr int CP_GFX_RS64_DC_BASE0_LO     = 1;
    constexpr int CP_GFX_RS64_DC_BASE1_LO     = 1;
    constexpr int CP_GFX_RS64_DC_BASE0_HI     = 1;
    constexpr int CP_GFX_RS64_DC_BASE1_HI     = 1;
    constexpr int CP_GFX_RS64_DC_BASE_CNTL    = 1;
    constexpr int CP_GFX_RS64_DC_OP_CNTL      = 1;
    constexpr int CP_MEC_MDBASE_LO            = 1;
    constexpr int CP_MEC_MDBASE_HI            = 1;
    constexpr int CP_MEC_DC_BASE_CNTL         = 1;
    constexpr int CP_MEC_DC_OP_CNTL           = 1;
    constexpr int CP_STAT                     = 0;  // regCP_STAT_BASE_IDX 0
    constexpr int CP_RB0_RPTR                 = 0;  // regCP_RB0_RPTR_BASE_IDX 0
    // GFX HQD / MQD block — gc_12_0_0_offset.h:3680-3731, all BASE_IDX 0.
    constexpr int CP_GFX_MQD_BASE_ADDR        = 0;
    constexpr int CP_GFX_MQD_BASE_ADDR_HI     = 0;
    constexpr int CP_GFX_HQD_ACTIVE           = 0;
    constexpr int CP_GFX_HQD_VMID             = 0;
    constexpr int CP_GFX_HQD_QUEUE_PRIORITY   = 0;
    constexpr int CP_GFX_HQD_QUANTUM          = 0;
    constexpr int CP_GFX_HQD_BASE             = 0;
    constexpr int CP_GFX_HQD_BASE_HI          = 0;
    constexpr int CP_GFX_HQD_RPTR             = 0;
    constexpr int CP_GFX_HQD_RPTR_ADDR        = 0;
    constexpr int CP_GFX_HQD_RPTR_ADDR_HI     = 0;
    constexpr int CP_GFX_HQD_OFFSET           = 0;
    constexpr int CP_GFX_HQD_CNTL             = 0;
    constexpr int CP_GFX_HQD_CSMD_RPTR        = 0;
    constexpr int CP_GFX_HQD_WPTR             = 0;
    constexpr int CP_GFX_HQD_WPTR_HI          = 0;
    constexpr int CP_GFX_HQD_MAPPED           = 0;
    constexpr int CP_GFX_HQD_QUE_MGR_CONTROL  = 0;
    constexpr int CP_GFX_HQD_HQ_STATUS0       = 0;
    constexpr int CP_GFX_HQD_HQ_CONTROL0      = 0;
    constexpr int CP_GFX_MQD_CONTROL          = 0;
    constexpr int CP_INT_CNTL_RING0           = 0;
    constexpr int CP_INT_STATUS_RING0         = 0;
    constexpr int SCRATCH_REG0                = 1;  // regSCRATCH_REG0_BASE_IDX 1
    constexpr int CP_MQD_BASE_ADDR            = 0;
    constexpr int CP_HQD_ACTIVE               = 0;
    constexpr int CP_HQD_PQ_BASE              = 0;
    constexpr int CP_HQD_PQ_RPTR              = 0;
    constexpr int CP_HQD_PQ_DOORBELL_CONTROL  = 0;
    constexpr int CP_HQD_PQ_CONTROL           = 0;
    constexpr int CP_HQD_PQ_WPTR_LO           = 0;
}

// CP_ME_CNTL bit positions per gc_12_0_0_sh_mask.h:13868-13889.
//   PFP_HALT__SHIFT = 0x1a (26) → MASK 0x04000000
//   ME_HALT__SHIFT  = 0x1c (28) → MASK 0x10000000
#define CP_ME_CNTL__PFP_HALT__SHIFT 0x1a
#define CP_ME_CNTL__PFP_HALT_MASK   0x04000000u
#define CP_ME_CNTL__ME_HALT__SHIFT  0x1c
#define CP_ME_CNTL__ME_HALT_MASK    0x10000000u

// CP_ME_CNTL RS64 per-pipe reset bits — gc_12_0_0_sh_mask.h:13862-13865 and
// 13881-13884. gfx_v12_0_config_gfx_rs64 (gfx_v12_0.c:2168-2199) pulses
// these after writing each engine's PRGRM_CNTR_START so the RS64 core
// re-fetches its entry point.
#define CP_ME_CNTL__PFP_PIPE0_RESET__SHIFT 0x12
#define CP_ME_CNTL__PFP_PIPE0_RESET_MASK   0x00040000u
#define CP_ME_CNTL__PFP_PIPE1_RESET__SHIFT 0x13
#define CP_ME_CNTL__PFP_PIPE1_RESET_MASK   0x00080000u
#define CP_ME_CNTL__ME_PIPE0_RESET__SHIFT  0x14
#define CP_ME_CNTL__ME_PIPE0_RESET_MASK    0x00100000u
#define CP_ME_CNTL__ME_PIPE1_RESET__SHIFT  0x15
#define CP_ME_CNTL__ME_PIPE1_RESET_MASK    0x00200000u

// PRGRM_CNTR_START IP_START fields — gc_12_0_0_sh_mask.h:12324-12328,
// 12451-12452, 12602-12603, 15877-15878, 16075-16076. The _HI half is
// 30 bits wide, the low half a full 32.
#define CP_PFP_PRGRM_CNTR_START__IP_START__SHIFT          0x0
#define CP_PFP_PRGRM_CNTR_START__IP_START_MASK            0xFFFFFFFFu
#define CP_PFP_PRGRM_CNTR_START_HI__IP_START__SHIFT       0x0
#define CP_PFP_PRGRM_CNTR_START_HI__IP_START_MASK         0x3FFFFFFFu
#define CP_ME_PRGRM_CNTR_START__IP_START__SHIFT           0x0
#define CP_ME_PRGRM_CNTR_START__IP_START_MASK             0xFFFFFFFFu
#define CP_ME_PRGRM_CNTR_START_HI__IP_START__SHIFT        0x0
#define CP_ME_PRGRM_CNTR_START_HI__IP_START_MASK          0x3FFFFFFFu
#define CP_MEC_RS64_PRGRM_CNTR_START__IP_START__SHIFT     0x0
#define CP_MEC_RS64_PRGRM_CNTR_START__IP_START_MASK       0xFFFFFFFFu
#define CP_MEC_RS64_PRGRM_CNTR_START_HI__IP_START__SHIFT  0x0
#define CP_MEC_RS64_PRGRM_CNTR_START_HI__IP_START_MASK    0x3FFFFFFFu

// ---- RS64 instruction-cache base / op control fields ----
// gc_12_0_0_sh_mask.h:19488-19504 (PFP), 19512-19528 (ME),
// 19536-19547 + 16398-16409 (CPC).
#define CP_PFP_IC_BASE_CNTL__VMID__SHIFT                 0x0
#define CP_PFP_IC_BASE_CNTL__VMID_MASK                   0x0000000Fu
#define CP_PFP_IC_BASE_CNTL__EXE_DISABLE__SHIFT          0x17
#define CP_PFP_IC_BASE_CNTL__EXE_DISABLE_MASK            0x00800000u
#define CP_PFP_IC_BASE_CNTL__CACHE_POLICY__SHIFT         0x18
#define CP_PFP_IC_BASE_CNTL__CACHE_POLICY_MASK           0x03000000u
#define CP_PFP_IC_OP_CNTL__INVALIDATE_CACHE__SHIFT           0x0
#define CP_PFP_IC_OP_CNTL__INVALIDATE_CACHE_MASK             0x00000001u
#define CP_PFP_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE__SHIFT  0x1
#define CP_PFP_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE_MASK    0x00000002u
#define CP_PFP_IC_OP_CNTL__PRIME_ICACHE__SHIFT               0x4
#define CP_PFP_IC_OP_CNTL__PRIME_ICACHE_MASK                 0x00000010u
#define CP_PFP_IC_OP_CNTL__ICACHE_PRIMED__SHIFT              0x5
#define CP_PFP_IC_OP_CNTL__ICACHE_PRIMED_MASK                0x00000020u

#define CP_ME_IC_BASE_CNTL__VMID__SHIFT                  0x0
#define CP_ME_IC_BASE_CNTL__VMID_MASK                    0x0000000Fu
#define CP_ME_IC_BASE_CNTL__EXE_DISABLE__SHIFT           0x17
#define CP_ME_IC_BASE_CNTL__EXE_DISABLE_MASK             0x00800000u
#define CP_ME_IC_BASE_CNTL__CACHE_POLICY__SHIFT          0x18
#define CP_ME_IC_BASE_CNTL__CACHE_POLICY_MASK            0x03000000u
#define CP_ME_IC_OP_CNTL__INVALIDATE_CACHE__SHIFT            0x0
#define CP_ME_IC_OP_CNTL__INVALIDATE_CACHE_MASK              0x00000001u
#define CP_ME_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE__SHIFT   0x1
#define CP_ME_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE_MASK     0x00000002u
#define CP_ME_IC_OP_CNTL__PRIME_ICACHE__SHIFT                0x4
#define CP_ME_IC_OP_CNTL__PRIME_ICACHE_MASK                  0x00000010u
#define CP_ME_IC_OP_CNTL__ICACHE_PRIMED__SHIFT               0x5
#define CP_ME_IC_OP_CNTL__ICACHE_PRIMED_MASK                 0x00000020u

#define CP_CPC_IC_BASE_CNTL__VMID__SHIFT                 0x0
#define CP_CPC_IC_BASE_CNTL__VMID_MASK                   0x0000000Fu
#define CP_CPC_IC_BASE_CNTL__EXE_DISABLE__SHIFT          0x17
#define CP_CPC_IC_BASE_CNTL__EXE_DISABLE_MASK            0x00800000u
#define CP_CPC_IC_BASE_CNTL__CACHE_POLICY__SHIFT         0x18
#define CP_CPC_IC_BASE_CNTL__CACHE_POLICY_MASK           0x03000000u
#define CP_CPC_IC_OP_CNTL__INVALIDATE_CACHE__SHIFT           0x0
#define CP_CPC_IC_OP_CNTL__INVALIDATE_CACHE_MASK             0x00000001u
#define CP_CPC_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE__SHIFT  0x1
#define CP_CPC_IC_OP_CNTL__INVALIDATE_CACHE_COMPLETE_MASK    0x00000002u

// ---- RS64 data-cache base / op control fields ----
// gc_12_0_0_sh_mask.h:16420-16432 (GFX RS64 DC), 15929-15939 (MEC DC).
#define CP_GFX_RS64_DC_BASE_CNTL__VMID__SHIFT            0x0
#define CP_GFX_RS64_DC_BASE_CNTL__VMID_MASK              0x0000000Fu
#define CP_GFX_RS64_DC_BASE_CNTL__CACHE_POLICY__SHIFT    0x18
#define CP_GFX_RS64_DC_BASE_CNTL__CACHE_POLICY_MASK      0x03000000u
#define CP_GFX_RS64_DC_OP_CNTL__INVALIDATE_DCACHE__SHIFT           0x0
#define CP_GFX_RS64_DC_OP_CNTL__INVALIDATE_DCACHE_MASK             0x00000001u
#define CP_GFX_RS64_DC_OP_CNTL__INVALIDATE_DCACHE_COMPLETE__SHIFT  0x1
#define CP_GFX_RS64_DC_OP_CNTL__INVALIDATE_DCACHE_COMPLETE_MASK    0x00000002u

#define CP_MEC_DC_BASE_CNTL__VMID__SHIFT                 0x0
#define CP_MEC_DC_BASE_CNTL__VMID_MASK                   0x0000000Fu
#define CP_MEC_DC_BASE_CNTL__CACHE_POLICY__SHIFT         0x18
#define CP_MEC_DC_BASE_CNTL__CACHE_POLICY_MASK           0x03000000u
#define CP_MEC_DC_OP_CNTL__INVALIDATE_DCACHE__SHIFT           0x0
#define CP_MEC_DC_OP_CNTL__INVALIDATE_DCACHE_MASK             0x00000001u
#define CP_MEC_DC_OP_CNTL__INVALIDATE_DCACHE_COMPLETE__SHIFT  0x1
#define CP_MEC_DC_OP_CNTL__INVALIDATE_DCACHE_COMPLETE_MASK    0x00000002u

// CP_MEC_RS64_CNTL bit positions per gc_12_0_0_sh_mask.h:15886-15909.
#define CP_MEC_RS64_CNTL__MEC_INVALIDATE_ICACHE__SHIFT 0x4
#define CP_MEC_RS64_CNTL__MEC_INVALIDATE_ICACHE_MASK   0x00000010u
#define CP_MEC_RS64_CNTL__MEC_PIPE0_RESET__SHIFT       0x10
#define CP_MEC_RS64_CNTL__MEC_PIPE0_RESET_MASK         0x00010000u
#define CP_MEC_RS64_CNTL__MEC_PIPE1_RESET__SHIFT       0x11
#define CP_MEC_RS64_CNTL__MEC_PIPE1_RESET_MASK         0x00020000u
#define CP_MEC_RS64_CNTL__MEC_PIPE2_RESET__SHIFT       0x12
#define CP_MEC_RS64_CNTL__MEC_PIPE2_RESET_MASK         0x00040000u
#define CP_MEC_RS64_CNTL__MEC_PIPE3_RESET__SHIFT       0x13
#define CP_MEC_RS64_CNTL__MEC_PIPE3_RESET_MASK         0x00080000u
#define CP_MEC_RS64_CNTL__MEC_PIPE0_ACTIVE__SHIFT      0x1a
#define CP_MEC_RS64_CNTL__MEC_PIPE0_ACTIVE_MASK        0x04000000u
#define CP_MEC_RS64_CNTL__MEC_PIPE1_ACTIVE__SHIFT      0x1b
#define CP_MEC_RS64_CNTL__MEC_PIPE1_ACTIVE_MASK        0x08000000u
#define CP_MEC_RS64_CNTL__MEC_PIPE2_ACTIVE__SHIFT      0x1c
#define CP_MEC_RS64_CNTL__MEC_PIPE2_ACTIVE_MASK        0x10000000u
#define CP_MEC_RS64_CNTL__MEC_PIPE3_ACTIVE__SHIFT      0x1d
#define CP_MEC_RS64_CNTL__MEC_PIPE3_ACTIVE_MASK        0x20000000u
#define CP_MEC_RS64_CNTL__MEC_HALT__SHIFT              0x1e
#define CP_MEC_RS64_CNTL__MEC_HALT_MASK                0x40000000u

// CP_RB0_CNTL fields — gfx_v12_0.c:2735 writes RB_BUFSZ and RB_BLKSZ.
#define CP_RB0_CNTL__RB_BUFSZ__SHIFT 0x0
#define CP_RB0_CNTL__RB_BUFSZ_MASK   0x0000003Fu
#define CP_RB0_CNTL__RB_BLKSZ__SHIFT 0x8
#define CP_RB0_CNTL__RB_BLKSZ_MASK   0x00003F00u

// CP_INT_CNTL_RING0 fields (gc_12_0_0_sh_mask.h:11832-11862). gfx12 enables
// TIME_STAMP_INT_ENABLE + GENERIC0_INT_ENABLE together for an EOP interrupt
// (gfx_v12_0_set_gfx_eop_interrupt_state); the fault bits are ours, so a
// privileged-register or bad-opcode error also reaches the host.
#define CP_INT_CNTL_RING0__CMP_BUSY_INT_ENABLE__SHIFT     0x12
#define CP_INT_CNTL_RING0__CNTX_BUSY_INT_ENABLE__SHIFT    0x13
#define CP_INT_CNTL_RING0__PRIV_INSTR_INT_ENABLE__SHIFT   0x16
#define CP_INT_CNTL_RING0__PRIV_REG_INT_ENABLE__SHIFT     0x17
#define CP_INT_CNTL_RING0__OPCODE_ERROR_INT_ENABLE__SHIFT 0x18
#define CP_INT_CNTL_RING0__TIME_STAMP_INT_ENABLE__SHIFT   0x1a
#define CP_INT_CNTL_RING0__GENERIC0_INT_ENABLE__SHIFT     0x1f
#define CP_INT_CNTL_RING0__PRIV_INSTR_INT_ENABLE_MASK     0x00400000u
#define CP_INT_CNTL_RING0__PRIV_REG_INT_ENABLE_MASK       0x00800000u
#define CP_INT_CNTL_RING0__OPCODE_ERROR_INT_ENABLE_MASK   0x01000000u
#define CP_INT_CNTL_RING0__TIME_STAMP_INT_ENABLE_MASK     0x04000000u
#define CP_INT_CNTL_RING0__GENERIC0_INT_ENABLE_MASK       0x80000000u

// CP_RB_DOORBELL_CONTROL field shifts (upstream gfx_v12_0.c uses
// REG_SET_FIELD on these). DOORBELL_OFFSET[27:2], DOORBELL_EN[30].
#define CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT 0x2
#define CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK   0x0FFFFFFCu
#define CP_RB_DOORBELL_CONTROL__DOORBELL_EN__SHIFT     0x1e
#define CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK       0x40000000u

// The CP_GFX_MQD_CONTROL / CP_GFX_HQD_{VMID,QUEUE_PRIORITY,QUANTUM,CNTL}
// field masks gfx_v12_0_gfx_mqd_init uses live in amdgpu_gfx.h (which
// cp_v12_0.cpp includes) — not duplicated here.

// v12_gfx_mqd (drivers/gpu/drm/amd/include/v12_structs.h): 512 dwords.
// Only the dwords gfx_v12_0_gfx_mqd_init writes are named; the rest stay
// zero, exactly as upstream's memset + init leaves them. Offsets are the
// "// offset: N" annotations in v12_structs.h.
namespace GfxMqd {
    constexpr uint32_t kDwords                    = 512;
    constexpr uint32_t shadow_base_lo             = 0;
    constexpr uint32_t shadow_base_hi             = 1;
    constexpr uint32_t fw_work_area_base_lo       = 4;
    constexpr uint32_t fw_work_area_base_hi       = 5;
    constexpr uint32_t cp_mqd_base_addr           = 128;
    constexpr uint32_t cp_mqd_base_addr_hi        = 129;
    constexpr uint32_t cp_gfx_hqd_active          = 130;
    constexpr uint32_t cp_gfx_hqd_vmid            = 131;
    constexpr uint32_t cp_gfx_hqd_queue_priority  = 134;
    constexpr uint32_t cp_gfx_hqd_quantum         = 135;
    constexpr uint32_t cp_gfx_hqd_base            = 136;
    constexpr uint32_t cp_gfx_hqd_base_hi         = 137;
    constexpr uint32_t cp_gfx_hqd_rptr            = 138;
    constexpr uint32_t cp_gfx_hqd_rptr_addr       = 139;
    constexpr uint32_t cp_gfx_hqd_rptr_addr_hi    = 140;
    constexpr uint32_t cp_rb_wptr_poll_addr_lo    = 141;
    constexpr uint32_t cp_rb_wptr_poll_addr_hi    = 142;
    constexpr uint32_t cp_rb_doorbell_control     = 143;
    constexpr uint32_t cp_gfx_hqd_offset          = 144;
    constexpr uint32_t cp_gfx_hqd_cntl            = 145;
    constexpr uint32_t cp_gfx_hqd_csmd_rptr       = 148;
    constexpr uint32_t cp_gfx_hqd_wptr            = 149;
    constexpr uint32_t cp_gfx_hqd_wptr_hi         = 150;
    constexpr uint32_t cp_gfx_hqd_mapped          = 156;
    constexpr uint32_t cp_gfx_hqd_que_mgr_control = 157;
    constexpr uint32_t cp_gfx_hqd_hq_status0      = 160;
    constexpr uint32_t cp_gfx_hqd_hq_control0     = 161;
    constexpr uint32_t cp_gfx_mqd_control         = 162;
    constexpr uint32_t fence_address_lo           = 510;
    constexpr uint32_t fence_address_hi           = 511;
    // Register defaults the init starts from — gfx_v12_0.c:55-61.
    constexpr uint32_t kDefault_CP_GFX_MQD_CONTROL        = 0x00000100;
    constexpr uint32_t kDefault_CP_GFX_HQD_VMID           = 0x00000000;
    constexpr uint32_t kDefault_CP_GFX_HQD_QUEUE_PRIORITY = 0x00000000;
    constexpr uint32_t kDefault_CP_GFX_HQD_QUANTUM        = 0x00000a01;
    constexpr uint32_t kDefault_CP_GFX_HQD_CNTL           = 0x00f00000;
    constexpr uint32_t kDefault_CP_RB_DOORBELL_CONTROL    = 0x00000000;
    constexpr uint32_t kDefault_CP_GFX_HQD_RPTR           = 0x00000000;
}

// CP_RB_DOORBELL_RANGE_LOWER.DOORBELL_RANGE_LOWER + RANGE_UPPER mask.
// gc_12_0_0_sh_mask.h: both fields are [11:2], MASK 0x00000FFC — NOT the
// 0x0FFFFFFC the reference used (that is CP_RB_DOORBELL_CONTROL's
// DOORBELL_OFFSET layout, copied onto the wrong register). This matters
// for a value, not just an address: upstream's cp_gfx_set_doorbell writes
// the RANGE_UPPER *mask itself* into the register, so the reference was
// programming 0x0FFFFFFC where the hardware field tops out at 0x00000FFC.
#define CP_RB_DOORBELL_RANGE_LOWER__DOORBELL_RANGE_LOWER__SHIFT 0x2
#define CP_RB_DOORBELL_RANGE_LOWER__DOORBELL_RANGE_LOWER_MASK   0x00000FFCu
#define CP_RB_DOORBELL_RANGE_UPPER__DOORBELL_RANGE_UPPER__SHIFT 0x2
#define CP_RB_DOORBELL_RANGE_UPPER__DOORBELL_RANGE_UPPER_MASK   0x00000FFCu

// CP_RB_RPTR_ADDR_HI bit mask (upstream gfx_v12_0.c:2748 uses the
// `RB_RPTR_ADDR_HI` field mask to keep only the low 16 bits).
#define CP_RB_RPTR_ADDR_HI__RB_RPTR_ADDR_HI_MASK 0x0000FFFFu

// Ring / MQD / write-back sizes. Same values as the reference.
constexpr uint32_t kCPRingDefaultBytes = 16 * 1024;
constexpr uint32_t kCPMQDBytes         = 4 * 1024;
constexpr uint32_t kCPWBPageBytes      = 16 * 1024;

// Write-back layout (host + GPU agree on these offsets within
// the 16 KB write-back page):
constexpr uint32_t kCPWBOffsetRptr  = 0x000;
constexpr uint32_t kCPWBOffsetWptr  = 0x040;
constexpr uint32_t kCPWBOffsetFence = 0x080;   // 8 B, qword-aligned

// GFX12 doorbell stride: one u64 slot per doorbell index.
// cp_kick_doorbell writes at BAR2 + doorbell_index * kCPDoorbellStride.
constexpr uint64_t kCPDoorbellStride = 4;   // dword-indexed aperture: byte offset = doorbell_index * 4 (Linux WDOORBELL64); was 8

struct CPContext {
    bool              inited           { false };

    // GFX ring (sysmem, bound into GART)
    SysMem            ring_sys         {};            // was ring_buf + ring_dma
    uint64_t          ring_bus         { 0 };         // GART MC address
    void             *ring_cpu         { nullptr };   // CPU-side write target
    uint32_t          ring_size_dwords { 0 };
    uint32_t          ring_ptr_mask    { 0 };         // ring_size_dwords - 1

    // KIQ MQD (sysmem, bound into GART)
    SysMem            mqd_sys          {};            // was mqd_buf + mqd_dma
    uint64_t          mqd_bus          { 0 };         // GART MC address
    void             *mqd_cpu          { nullptr };

    // Write-back page (sysmem, bound into GART)
    SysMem            wb_sys           {};            // was wb_buf + wb_dma
    uint64_t          wb_bus           { 0 };         // GART MC address
    void             *wb_cpu           { nullptr };

    // Convenience pointers into the WB page (CPU-side):
    volatile uint32_t *rptr_cpu        { nullptr };
    volatile uint32_t *wptr_cpu        { nullptr };
    volatile uint64_t *fence_cpu       { nullptr };

    // GPU-side (GART MC) addresses derived from wb_bus
    uint64_t  rptr_gpu_addr  { 0 };
    uint64_t  wptr_gpu_addr  { 0 };
    uint64_t  fence_gpu_addr { 0 };

    // Software wptr — what the host has committed but not yet kicked.
    uint32_t  wptr           { 0 };
    uint32_t  fence_counter  { 0 };

    // Doorbell index (dword index into the BAR2 doorbell map).
    uint32_t  doorbell_index { 0 };

    // ---- RS64 image locations inside the PSP TMR ----
    // MC addresses the PSP reported as `tmr_fw_addr` for each LOAD_IP_FW.
    // 0 = unknown (the ladder did not supply it), in which case
    // cp_config_rs64_caches leaves that cache base alone.
    // Filled by the ladder from psp.tmr_fw_addr_by_type[]:
    //   pfp_ic  = 87 GFX_FW_TYPE_RS64_PFP        (ucode)
    //   pfp_dc  = 90 GFX_FW_TYPE_RS64_PFP_P0_STACK
    //   me_ic   = 88 GFX_FW_TYPE_RS64_ME
    //   me_dc   = 92 GFX_FW_TYPE_RS64_ME_P0_STACK
    //   mec_ic  = 89 GFX_FW_TYPE_RS64_MEC
    //   mec_dc0 = 94 GFX_FW_TYPE_RS64_MEC_P0_STACK
    //   mec_dc1 = 95 GFX_FW_TYPE_RS64_MEC_P1_STACK
    uint64_t tmr_pfp_ic  { 0 };
    uint64_t tmr_pfp_dc  { 0 };
    uint64_t tmr_me_ic   { 0 };
    uint64_t tmr_me_dc   { 0 };
    uint64_t tmr_mec_ic  { 0 };
    uint64_t tmr_mec_dc0 { 0 };
    uint64_t tmr_mec_dc1 { 0 };

    // Set once cp_config_rs64_caches has run (whether or not it had to
    // write anything).
    bool     caches_configured { false };

    // ---- RS64 program-counter configuration (cp_config_gfx_rs64) ----
    // Set from the PFP / ME / MEC gfx_firmware_header_v2_0 headers so the
    // failure dump can show what we asked the RS64 cores to start at.
    bool      rs64_configured { false };
    uint32_t  pfp_pc          { 0 };   // CP_PFP_PRGRM_CNTR_START value
    uint32_t  pfp_pc_hi       { 0 };
    uint32_t  me_pc           { 0 };
    uint32_t  me_pc_hi        { 0 };
    uint32_t  mec_pc          { 0 };
    uint32_t  mec_pc_hi       { 0 };

    // Ring-fetch probe result (cp_ring_fetch_probe): the first hardware
    // evidence that the CP front-end consumes PM4 at all.
    bool      fetch_proven    { false };
    uint32_t  probe_rptr      { 0 };   // CP_RB0_RPTR after the probe

    // ---- MES-mapped kernel GFX queue (gfx_v12_0_cp_async_gfx_ring_resume) ----
    // The GFX12 driver never programs CP_RB0_* itself on real hardware: it
    // fills a v12_gfx_mqd (in mqd_cpu here) and the MES maps it with
    // ADD_QUEUE(map_legacy_kq=1). 0.0.16.
    bool      gfx_mqd_inited   { false };  // cp_gfx_mqd_init filled mqd_cpu
    bool      kgq_mapped       { false };  // MES acked ADD_QUEUE for me0/pipe0/queue0
    bool      ring_test_passed { false };  // cp_ring_test_scratch: SET_UCONFIG_REG landed
    uint32_t  ring_test_value  { 0 };      // SCRATCH_REG0 at the end of that test
};

// Allocate the GFX ring + MQD + write-back page in system memory and bind
// all three into GART. Idempotent (gated on cp.inited). Requires gmc_init
// to have run (gmc.inited && gmc.gart_pt_bus != 0) so PTEs can be written.
kern_return_t cp_alloc_storage(DeviceContext &dev,
                               GMCContext &gmc, CPContext &cp);

// Free everything cp_alloc_storage allocated. The GART slots stay "used"
// (the GART bump allocator has no free) until the page table is re-zeroed.
void cp_release_storage(CPContext &cp);

// Append PM4 dwords to the ring at the current software wptr.
// Wraps modulo the ring size. Returns the number of dwords written
// (or 0 if `dwords` would overflow the ring). Does NOT kick the
// doorbell — caller does that after all packets are staged.
uint32_t cp_ring_write(CPContext &cp, const uint32_t *src,
                       uint32_t dwords);

// Build a NOP+RELEASE_MEM packet pair into the ring. Returns the
// fence value the EOP write will deposit at fence_gpu_addr; caller
// should kick the doorbell then poll *fence_cpu for that value.
uint32_t cp_emit_eop_fence(CPContext &cp);

// Program the GFX ring's HQD registers (CP_RB0_BASE/BASE_HI/CNTL,
// RPTR_ADDR/HI, WPTR/WPTR_HI, WPTR_POLL_ADDR_LO/HI, VMID, RB_ACTIVE,
// doorbell control + range, CP_MAX_CONTEXT, CP_DEVICE_ID). Mirrors
// gfx_v12_0_cp_gfx_resume's register sequence in upstream's exact order.
// Caller must have populated CPContext via cp_alloc_storage and set
// cp.doorbell_index.
kern_return_t cp_hqd_program(const DeviceContext &dev, CPContext &cp);

// Read, log and (where needed) program the RS64 instruction- and
// data-cache base registers for PFP, ME and MEC.
//
// Hardware finding (build 0.0.14): with PRGRM_CNTR_START programmed, the
// PFP/ME start and immediately fault — GCVM fault status 0x0d33, client 6
// (CPG), walker+mapping error on a read, four IH fault IVs all at VA page
// 0, and the CP re-halts itself (CP_ME_CNTL 0x0100a000 -> 0x1500a000,
// CP_STAT 0x80021000). A PC of byte 0x3000 faulting at page 0 means the
// instruction-cache base is 0: the PSP did NOT program CP_PFP_IC_BASE /
// CP_ME_IC_BASE / CP_CPC_IC_BASE (nor the DC bases) in this flow.
//
// Linux only writes these on AMDGPU_FW_LOAD_DIRECT (gfx_v12_0.c:2443-2505
// PFP, 2587-2650 ME, 2902-2930 MEC), where they point at the driver's own
// VRAM copy of the ucode. Here they point at the PSP's TMR copies, whose
// MC addresses the caller puts in the tmr_* fields above.
//
// Every base is READ and LOGGED first. A base that already reads nonzero
// is left exactly as it is (someone else programmed it) and logged as
// such; only a base that reads 0 and has a TMR address available is
// written. Safe to call with all tmr_* zero: it degrades to a pure
// register survey. Must run BEFORE cp_config_gfx_rs64 and before any
// unhalt, because programming an IC base invalidates that core's L1 I$.
kern_return_t cp_config_rs64_caches(DeviceContext &dev, CPContext &cp);

// Port of gfx_v12_0_config_gfx_rs64 (gfx_v12_0.c:2142), the PSP-load
// branch that gfx_v12_0_hw_init runs at line 3814 — BEFORE gfxhub_enable,
// constants_init, rlc_resume and cp_resume.
//
// Without it the RS64 PFP / ME / MEC cores are released from halt with a
// program counter of 0 and start executing at address 0. That is the
// signature seen on this card: the GFX ring never advances (CP_RB0_RPTR
// stays 0) and the GFX hub logs VM faults at VA 0x0 the moment the CP is
// enabled.
//
// Per pipe it writes CP_{PFP,ME,MEC_RS64}_PRGRM_CNTR_START(_HI) from the
// firmware's own gfx_firmware_header_v2_0 (fw_get(FwId::GC_PFP/GC_ME/
// GC_MEC)), selecting the pipe through GRBM_GFX_CNTL exactly as
// soc24_grbm_select does, then pulses the per-pipe reset bits in
// CP_ME_CNTL (PFP, then ME) and CP_MEC_RS64_CNTL (MEC pipes 0..3).
//
// It does NOT touch CP_*_IC_BASE_*: Linux programs those only on the
// AMDGPU_FW_LOAD_DIRECT path (gfx_v12_0.c:2443, 2587, 2902-2926), where
// the driver owns the ucode copy. Under PSP load the PSP has already
// placed the images and told the CP where they are.
kern_return_t cp_config_gfx_rs64(DeviceContext &dev, CPContext &cp);

// Port of gfx_v12_0_cp_gfx_start (gfx_v12_0.c:2699). On GFX12 this is
// three steps and no packets: CP_MAX_CONTEXT = max_hw_contexts - 1,
// CP_DEVICE_ID = 1, then (on the !amdgpu_async_gfx_ring path this port
// mirrors) cp_gfx_enable(true). GFX11's clear-state preamble
// (PREAMBLE_CNTL / CONTEXT_CONTROL / SET_CONTEXT_REG, gfx_v11_0.c:3677)
// was removed for GFX12 — see the comment on the implementation.
// `legacy_unhalt` selects the !amdgpu_async_gfx_ring branch (cp_gfx_enable
// inside cp_gfx_start). On the default MES-mapped path the CP was already
// unhalted at CPInit (gfx_v12_0_cp_resume:3546-3549) and this only writes
// the two registers — after the MES has mapped the queue, as upstream's
// cp_async_gfx_ring_resume does.
kern_return_t cp_gfx_start(DeviceContext &dev, CPContext &cp,
                           bool legacy_unhalt);

// Diagnostic, not in upstream: push a block of single-dword PM4 NOPs into
// the GFX ring, ring the doorbell, and poll CP_RB0_RPTR / the write-back
// rptr until it reaches the software wptr. This is the narrowest possible
// proof that the CP front-end fetches from the ring — a NOP needs no
// context state, no shader, no VMID and touches no memory but the ring
// itself. Logs rptr/wptr before and after plus the full CP state dump on
// timeout. Sets cp.fetch_proven.
kern_return_t cp_ring_fetch_probe(const DeviceContext &dev, CPContext &cp,
                                  uint32_t nops, uint64_t timeout_us);

// Dump every CP register that says something about why the ring is not
// moving: CP_RB0_RPTR / _WPTR(_HI), the write-back rptr/wptr, CP_ME_CNTL,
// CP_MEC_RS64_CNTL, CP_STAT, GRBM_STATUS, CP_RB0_CNTL, CP_RB_ACTIVE,
// CP_RB_DOORBELL_CONTROL and the three PRGRM_CNTR_START pairs.
void cp_dump_state(const DeviceContext &dev, const CPContext &cp,
                   const char *where);

// Dump the per-queue GFX HQD / MQD register block (me0/pipe0/queue0):
// CP_GFX_HQD_ACTIVE/MAPPED/BASE/CNTL/RPTR/WPTR/OFFSET/CSMD_RPTR/HQ_STATUS0,
// CP_GFX_MQD_BASE_ADDR(_HI), CP_GFX_MQD_CONTROL, CP_RB_DOORBELL_CONTROL.
// Read before and after the MES ADD_QUEUE so the log shows what the
// firmware loaded from our MQD.
void cp_dump_gfx_hqd(const DeviceContext &dev, const char *where);

struct MESContext;   // amdgpu_mes.h — only cp_map_gfx_kgq_mes needs it

// gfx_v12_0_kgq_init_queue -> gfx_v12_0_gfx_mqd_init: fill the v12_gfx_mqd
// in cp.mqd_cpu (GART sysmem, MC address cp.mqd_bus) for the kernel GFX
// ring — hqd base = ring, rptr report / wptr poll = the WB slots, doorbell
// = cp.doorbell_index, VMID 0, PRIV_STATE 1, queue active. Publishes it
// (sfence + HDP flush). Does not touch the hardware.
kern_return_t cp_gfx_mqd_init(const DeviceContext &dev, CPContext &cp);

// amdgpu_gfx_enable_kgq -> amdgpu_mes_map_legacy_queue ->
// mes_v12_0_map_legacy_queue: MES ADD_QUEUE { queue_type GFX, pipe 0,
// queue 0, doorbell_offset, mqd_addr, wptr_addr, map_legacy_kq = 1 } on
// the uni_mes SCHED pipe. On ack the MES/CP firmware has loaded the GFX
// HQD registers from the MQD and the ring accepts doorbells. Sets
// cp.kgq_mapped. Requires MESInit to have run (mes.pipe[0] enabled).
kern_return_t cp_map_gfx_kgq_mes(const DeviceContext &dev, CPContext &cp,
                                 MESContext &mes);

// 0.0.178 "gfxmap" — the SAME v12_gfx_mqd, built for a ring somebody ELSE
// owns (Apple's GFX ring at 0x8400020000, 0x80000 bytes), into OUR MQD buffer
// (cp.mqd_cpu / cp.mqd_bus). Every field is derived exactly as cp_gfx_mqd_init
// derives it; only the ring base/size, the rptr-report address, the wptr-poll
// address and the doorbell index come from the caller, because those four are
// the only things that belong to Apple's ring rather than to ours.
//
// It deliberately does NOT set cp.gfx_mqd_inited or cp.kgq_mapped: after this
// runs, that buffer no longer describes our kernel GFX queue, and anything that
// believed it did would be wrong.
struct CPExternalGfxQueue {
    // ---- in
    uint64_t ring_gpu        { 0 };   // GART MC address, 256-byte aligned
    uint32_t ring_bytes      { 0 };   // power of two, >= 32 bytes
    uint64_t rptr_report_gpu { 0 };   // where the CP writes its rptr back
    uint64_t wptr_poll_gpu   { 0 };   // the wptr write-back the doorbell mirrors
    uint32_t doorbell_index  { 0 };   // DWORD index into the BAR2 aperture
    // ---- out (what was actually written, for the log and the scalars)
    uint32_t rb_bufsz        { 0 };
    uint32_t hqd_cntl        { 0 };
    uint32_t doorbell_control{ 0 };
};
kern_return_t cp_gfx_mqd_init_external(const DeviceContext &dev, CPContext &cp,
                                       CPExternalGfxQueue &q);

// gfx_v12_0_ring_test_ring: SCRATCH_REG0 <- 0xCAFEDEAD by MMIO, then one
// PACKET3_SET_UCONFIG_REG(SCRATCH_REG0, 0xDEADBEEF) through the ring, then
// poll the register. Needs no memory write from the CP at all, so it
// separates "the CP executes PM4" from "the CP can reach our sysmem".
// Sets cp.ring_test_passed / ring_test_value.
kern_return_t cp_ring_test_scratch(const DeviceContext &dev, CPContext &cp,
                                   uint64_t timeout_us);

// gfx_v12_0_set_gfx_eop_interrupt_state(me 0, pipe 0): CP_INT_CNTL_RING0
// TIME_STAMP_INT_ENABLE + GENERIC0_INT_ENABLE, so a RELEASE_MEM with
// INT_SEL != 0 raises src_id 181 on the GFX client. Also enables the three
// command-stream error interrupts (bad opcode, privileged register,
// privileged instruction) — upstream arms those from its own irq sources,
// and they are how a malformed packet announces itself instead of hanging.
kern_return_t cp_set_eop_interrupt(const DeviceContext &dev, bool enable);

// Submit an indirect buffer on the GFX ring the way gfx_v12_0 does for every
// job: CONTEXT_CONTROL (load_enable) + INDIRECT_BUFFER{addr, len | vmid<<24}
// + an EOP fence, then the doorbell. `ib_gpu_va` must be dword-aligned and
// already visible to the GPU (HDP flushed). Returns the fence value the
// RELEASE_MEM will deposit at cp.fence_gpu_addr.
//
// This is the same sequence compute_test.cpp proved at stage 17, factored out
// so the user client and the self-tests share one submission path.
kern_return_t cp_submit_ib(const DeviceContext &dev, CPContext &cp,
                           uint64_t ib_gpu_va, uint32_t length_dw,
                           uint32_t vmid, uint32_t *out_fence);

// Poll cp.fence_cpu until it reaches `fence`. Returns kIOReturnTimeout and
// leaves *observed at the last value seen if it does not.
kern_return_t cp_wait_fence(const CPContext &cp, uint32_t fence,
                            uint64_t timeout_us, uint64_t *observed,
                            uint64_t *elapsed_us);

// Toggle CP_ME_CNTL.{ME_HALT,PFP_HALT}. After cp_enable(true) the
// CP can fetch + execute from the GFX ring; before, the ring is
// dormant. Mirrors upstream gfx_v12_0_cp_gfx_enable
// (gfx_v12_0.c:2332) — both halts must drop together.
kern_return_t cp_enable(const DeviceContext &dev, bool enable);

// Toggle CP_MEC_RS64_CNTL — bring all 4 MEC pipes in/out of reset
// and active, and drop MEC_HALT. Mirrors upstream
// gfx_v12_0_cp_compute_enable (gfx_v12_0.c:2778).
kern_return_t cp_compute_enable(const DeviceContext &dev, bool enable);

// Program CP_RB_DOORBELL_RANGE_{LOWER,UPPER} for GFX and
// CP_MEC_DOORBELL_RANGE_{LOWER,UPPER} for compute. Mirrors upstream
// gfx_v12_0_cp_set_doorbell_range (gfx_v12_0.c:2954).
kern_return_t cp_set_doorbell_range(const DeviceContext &dev,
                                    const CPContext &cp,
                                    uint32_t mec_first_doorbell,
                                    uint32_t mec_last_doorbell);

// Publish the software wptr into the write-back page and ring the GFX
// ring's doorbell at BAR2 + doorbell_index * 8 (GFX12 stride).
kern_return_t cp_kick_doorbell(const DeviceContext &dev,
                               const CPContext &cp);

// End-to-end test: emit NOP+RELEASE_MEM, kick doorbell, poll fence.
// Returns kIOReturnSuccess if the fence value materialised at
// *fence_cpu within timeout_us microseconds. Sanity-checks the entire
// submit path once HQD + CP enable have run.
kern_return_t cp_submit_eop_test(const DeviceContext &dev,
                                 CPContext &cp,
                                 uint64_t timeout_us,
                                 uint32_t *outFence);

// KIQ PM4 NOP + RELEASE_MEM smoke test.
//
// Builds a tiny PM4 packet sequence (PACKET3_NOP + PACKET3_RELEASE_MEM)
// targeting a VRAM-resident 64-byte fence slot. CP firmware should
// process both and write `expected_fence_value` into the fence slot.
//
// Pre-fills the fence slot with 0xCAFEBABE so a "no write" outcome is
// distinguishable from a transient zero.
//
// `mes_kiq_armed` replaces the reference's `MESContext &mes` parameter
// (deviation 3 at the top of this file): pass true once the MES SCHED
// pipe has armed the KIQ, false to have the call refuse with
// kIOReturnNotReady exactly as the reference does.
//
// Returns kIOReturnSuccess if the fence write was observed within
// `timeout_us`, kIOReturnTimeout otherwise, kIOReturnNotReady if the
// CP / KIQ state isn't initialized.
//
// Out scalars:
//   *out_elapsed_us     — wall-clock from kick to observed fence
//   *out_fence_gpu_va   — GPU MC address of the fence dword
//   *out_observed_fence — last value read from the fence slot
kern_return_t cp_kiq_smoke_test(DeviceContext &dev,
                                CPContext &cp,
                                bool mes_kiq_armed,
                                GMCContext &gmc,
                                uint32_t expected_fence_value,
                                uint32_t timeout_us,
                                uint64_t *out_elapsed_us,
                                uint64_t *out_fence_gpu_va,
                                uint32_t *out_observed_fence);

//==================================================================
//  Kernel compute queue (KCQ)
//==================================================================
//
// A second ring, owned by the MEC rather than the graphics front-end. Same
// creation story as the kernel GFX queue that stage 16 proved: fill a
// v12_compute_mqd, hand it to the MES with ADD_QUEUE(map_legacy_kq), submit
// through a doorbell. It matters because compute work belongs on the compute
// pipes — the graphics ring serialises it behind graphics state — and because
// every later user-facing queue is this shape, not the GFX one.
//
// Upstream: gfx_v12_0_kcq_resume -> gfx_v12_0_kcq_init_queue ->
// gfx_v12_0_compute_mqd_init (gfx_v12_0.c:3143), then
// amdgpu_gfx_enable_kcq -> amdgpu_gfx_mes_enable_kcq ->
// amdgpu_mes_map_legacy_queue with queue_type COMPUTE.
struct ComputeQueue {
    bool      inited { false };
    bool      mapped { false };     // MES acked ADD_QUEUE

    SysMem    ring_sys {};  uint64_t ring_bus { 0 };  void *ring_cpu { nullptr };
    SysMem    mqd_sys  {};  uint64_t mqd_bus  { 0 };  void *mqd_cpu  { nullptr };
    SysMem    eop_sys  {};  uint64_t eop_bus  { 0 };
    SysMem    wb_sys   {};  uint64_t wb_bus   { 0 };  void *wb_cpu   { nullptr };

    uint32_t  ring_size_dwords { 0 };
    uint32_t  ring_ptr_mask    { 0 };
    volatile uint32_t *rptr_cpu  { nullptr };
    volatile uint32_t *wptr_cpu  { nullptr };
    volatile uint64_t *fence_cpu { nullptr };
    uint64_t  rptr_gpu_addr  { 0 };
    uint64_t  wptr_gpu_addr  { 0 };
    uint64_t  fence_gpu_addr { 0 };

    uint32_t  wptr { 0 };
    uint32_t  fence_counter { 0 };
    uint32_t  doorbell_index { 0 };
    // Which HQD this queue occupies. me is always 1 (the MEC); Linux's KCQs
    // start at pipe 0 queue 0 and walk outward.
    uint32_t  me { 1 }, pipe { 0 }, queue { 0 };
};

// Allocate ring / MQD / EOP / write-back and bind them into the GART. The
// doorbell comes from the map's compute block, which the MEC doorbell window
// already covers (CP_MEC_DOORBELL_RANGE = kiq..userqueue_end).
kern_return_t cp_compute_queue_alloc(DeviceContext &dev, GMCContext &gmc,
                                     ComputeQueue &cq, uint32_t pipe, uint32_t queue);

// gfx_v12_0_compute_mqd_init: fill the v12_compute_mqd in cq.mqd_cpu.
// hqd_active stays 0 — for a MES-mapped queue upstream sets it only for KIQ
// (amdgpu_ring_to_mqd_prop: prop->hqd_active = type == KIQ).
kern_return_t cp_compute_mqd_init(const DeviceContext &dev, ComputeQueue &cq);

// amdgpu_mes_map_legacy_queue with queue_type COMPUTE. Sets cq.mapped.
kern_return_t cp_compute_queue_map_mes(const DeviceContext &dev, ComputeQueue &cq,
                                       MESContext &mes);

// Stage dwords, append an EOP fence, kick the doorbell. Mirrors
// cp_submit_ib/cp_wait_fence on the GFX ring.
kern_return_t cp_compute_submit_ib(const DeviceContext &dev, ComputeQueue &cq,
                                   uint64_t ib_gpu_va, uint32_t length_dw,
                                   uint32_t vmid, uint32_t *out_fence);
kern_return_t cp_compute_wait_fence(const ComputeQueue &cq, uint32_t fence,
                                    uint64_t timeout_us, uint64_t *observed,
                                    uint64_t *elapsed_us);
void cp_compute_queue_release(GMCContext &gmc, ComputeQueue &cq);
void cp_dump_compute_hqd(const DeviceContext &dev, const ComputeQueue &cq,
                         const char *where);

// Top-level CPInit stage entry — alloc + GART-bind storage, then (if the
// GC IP base is resolved) halt CP, RS64 caches + program counters, run
// gfx_constants_init, program the doorbell ranges, enable MEC, and unhalt
// PFP/ME. `legacy_rb = false` (default path, 0.0.16) leaves the GFX queue
// unmapped for cp_gfx_mqd_init + cp_map_gfx_kgq_mes after MESInit, exactly
// like gfx_v12_0_cp_resume with amdgpu_async_gfx_ring=1. `legacy_rb = true`
// (navi48-cp-legacy-rb=1) keeps the CP_RB0_* register path + fetch probe
// that stage 16 runs 1-3 used. Idempotent.
kern_return_t cp_init_full(DeviceContext &dev,
                           GMCContext &gmc, CPContext &cp,
                           bool legacy_rb);

} // namespace amdgpu
