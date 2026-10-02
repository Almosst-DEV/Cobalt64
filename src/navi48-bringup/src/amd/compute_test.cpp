//
//  compute_test.cpp — one compute dispatch on gfx1201 through the CP GFX ring.
//
//  Phase 3's finish line. See compute_test.h for what this proves.
//
//  ORIGIN / AUTHORITY
//  ------------------
//  Not a port of lemonade-sdk/mac-amdgpu: that reference stops at the first
//  PM4 submission and has no compute dispatch. The packet stream mirrors
//  Linux amdgpu's ring-side dispatch
//      drivers/gpu/drm/amd/amdgpu/gfx_v8_0.c  gfx_v8_0_do_edc_gpr_workarounds
//      drivers/gpu/drm/amd/amdgpu/gfx_v9_0.c  gfx_v9_0_do_edc_gpr_workarounds
//  (a run of PACKET3_SET_SH_REG writes into the COMPUTE_* SH registers, then
//  PACKET3_DISPATCH_DIRECT, then EVENT_WRITE CS_PARTIAL_FLUSH) and Mesa
//  radeonsi's si_emit_dispatch_packets, which dispatches compute on the
//  *graphics* queue exactly as we do here.
//
//  Every register offset and field mask below is quoted from
//      ref/linux-asic-reg/gc_12_0_0_offset.h
//      ref/linux-asic-reg/gc_12_0_0_sh_mask.h
//  with the line the value came from. All COMPUTE_* registers carry
//  reg<NAME>_BASE_IDX 0, so they are reached at GC[0] — which this card has
//  at absolute dword 0x1260 (hardware-confirmed, notes/logs/stage12-run2).
//
//  COMPUTE_PGM_RSRC1 / _RSRC2 / _RSRC3 are NOT hand-derived: LLVM computes
//  them from shaders/store_magic.s's .amdhsa_kernel block and
//  tools/build-shader.sh prints them into shaders/store_magic.kd.txt. The
//  three constants here must equal that file. The decode comments below say
//  what each field means and cite the sh_mask lines.
//
// ===========================================================================
//  FINDING — amdgpu_pm4.h's pm4_header() has the PACKET3 fields SWAPPED
// ===========================================================================
//  amdgpu_pm4.h (verbatim from the reference, which never successfully
//  submitted a packet) builds
//        (3 << 30) | (op << 16) | count
//  The hardware's PACKET3 header, per drivers/gpu/drm/amd/amdgpu/soc15d.h and
//  nvd.h, is
//        #define PACKET3(op, n) ((3 << 30) | (((op) & 0xFF) << 8) \
//                                          | (((n) & 0x3FFF) << 16))
//  i.e. TYPE[31:30], COUNT[29:16], IT_OPCODE[15:8]. This tree already spells
//  the correct form out in cp_v12_0.cpp:729 ("PACKET3 macro: 0xC0000000 |
//  (opcode << 8) | ((count & 0x3FFF) << 16)") and uses the correct literal
//  0xC0064900 for RELEASE_MEM count 6 in cp_kiq_smoke_test — so the tree
//  contains both the right encoding and the wrong helper.
//
//  pm4_header(RELEASE_MEM=0x49, 6) yields 0xC0490006, which the CP decodes as
//  opcode 0x00 (NOP) with COUNT 0x49 = "skip 74 dwords" — it would walk off
//  the end of the packet into the rest of the ring.
//
//  Consequences, and what this file does about them:
//    * This file encodes its own headers with pm4_type3() below. It does not
//      call pm4_header().
//    * cp_emit_eop_fence() (amdgpu_cp.h / cp_v12_0.cpp) uses pm4_header(), so
//      the stage-16 PM4Test's fence is affected too. Rather than depend on a
//      packet we believe is malformed, emit_eop_fence_fixed() below is a
//      functional clone of cp_emit_eop_fence — same ring, same
//      cp.fence_gpu_addr slot, same ++cp.fence_counter value, so
//      cp_kick_doorbell() and polling *cp.fence_cpu work unchanged — with the
//      header encoded per nvd.h. Flip kUseCpEmitEopFence to true once
//      amdgpu_pm4.h is fixed and the two become identical.
//    * Success here does NOT depend on the fence at all: the verdict is the
//      data the shader wrote. The fence is reported separately, so a run that
//      says "data PASSED, fence did not land" isolates the RELEASE_MEM
//      encoding, and "data FAILED, fence landed" isolates the dispatch.
//
//  A second, milder finding in the same header: amdgpu_pm4.h places
//  RELEASE_MEM's GCR_GL2_WB at bit 16 and GCR_SEQ at bit 20. Per nvd.h those
//  are GL2_US and GL2_INV; GL2_WB is bit 21 and SEQ bit 22. That only affects
//  which caches get flushed — which is why shaders/store_magic.s does its own
//  `global_wb scope:SCOPE_SYS` and stores at scope:SCOPE_SYS, so the readback
//  is correct no matter how the GCR bits land.
// ===========================================================================
//
//  Deviations worth stating explicitly:
//   1. COMPUTE_VMID (0x1bb4) is written 0. Linux does not write it for a ring
//      dispatch; we do because we need VMID 0 (the GFX ring runs VMID 0 —
//      cp_hqd_program writes CP_RB_VMID = 0 — and VMID 0's GFXHUB system
//      aperture is what makes our VRAM MC addresses pass through
//      untranslated). 0 is also the reset value, so this is belt and braces.
//      It also makes RSRC1..SE3 one contiguous nine-register SET_SH_REG run.
//   2. COMPUTE_START_X/Y/Z are not written: DISPATCH_INITIATOR
//      .FORCE_START_AT_000 = 1 forces them to zero. Same as Mesa.
//   3. COMPUTE_STATIC_THREAD_MGMT_SE4..SE7 are not written (Navi 48 has 4 SEs
//      — hardware-confirmed 8 SAs / 64 CUs — and Linux's compute MQD init only
//      programs SE0..SE3).
//   4. The two VRAM allocations are never freed. See compute_test.h.
//

#include <IOKit/IOLib.h>

#include "compute_test.h"
#include "amdgpu_cp.h"
#include "amdgpu_gmc.h"
#include "amdgpu_sysmem.h"
#include "amdgpu_log.h"

#define CS_LOG(fmt, ...) AMDGPU_LOG("compute", fmt, ##__VA_ARGS__)

namespace amdgpu {

// The shader blob, generated by tools/build-shader.sh from
// shaders/store_magic.s and compiled from src/fw/fw_shader_store_magic.c
// (C translation unit -> C linkage).
extern "C" {
extern const uint8_t fw_shader_store_magic[];
extern const size_t  fw_shader_store_magic_size;
// The same kernel produced by LLVM from shaders/llvm/lean.ll instead of by hand,
// so it uses the standard HSA kernarg convention. Built by
// tools/build-shader.sh store_magic_hsa. See notes/B1-COMPILER-BRIDGE.md.
extern const uint8_t fw_shader_store_magic_hsa[];
extern const size_t  fw_shader_store_magic_hsa_size;
}

namespace {

//====================================================================
// PM4
//====================================================================

// PACKET3 header, per soc15d.h / nvd.h:
//   TYPE[31:30] = 3, COUNT[29:16] = payload dwords - 1, IT_OPCODE[15:8].
// See the FINDING block at the top of this file.
inline uint32_t pm4_type3(uint32_t op, uint32_t count_minus_1)
{
    return (3u << 30) | ((count_minus_1 & 0x3FFFu) << 16) | ((op & 0xFFu) << 8);
}

// PACKET3 header bit 1 is SHADER_TYPE: 0 = graphics, 1 = compute. nvd.h's
// PACKET3() macro does not expose it, but every AMD user-mode driver sets it
// on compute work submitted to the graphics queue — Mesa PKT3_SHADER_TYPE_S(1)
// on DISPATCH_DIRECT/INDIRECT, PAL ShaderCompute on the dispatch AND the
// compute SET_SH_REGs, libdrm's amdgpu tests PACKET3_COMPUTE(op, n) =
// PACKET3(op, n) | (1 << 1) on everything compute. Stage 17 run 1 sent the
// whole dispatch graphics-typed: the CP consumed it and the SH registers took
// the values, but the wave came up under VMID 6 and took an execute fault on
// its own code page (IH: GFX/UTCL2 fault, vmid 6, READ|EXE at the shader
// address). This is one half of the fix; the INDIRECT_BUFFER below is the other.
inline uint32_t pm4_type3_cs(uint32_t op, uint32_t count_minus_1)
{
    return pm4_type3(op, count_minus_1) | (1u << 1);
}

// IT opcodes — nvd.h / soc15d.h (stable across SOC15..GFX12).
constexpr uint32_t kOpDispatchDirect  = 0x15;
constexpr uint32_t kOpContextControl  = 0x28;   // nvd.h:94
constexpr uint32_t kOpWriteData       = 0x37;
constexpr uint32_t kOpIndirectBuffer  = 0x3F;   // nvd.h:224
constexpr uint32_t kOpEventWrite      = 0x46;
constexpr uint32_t kOpReleaseMem      = 0x49;
constexpr uint32_t kOpSetShReg        = 0x76;
constexpr uint32_t kOpSetShRegIndex   = 0x9B;   // nvd.h:576

// nvd.h: PACKET3_SET_SH_REG's ordinal-1 register index is the ABSOLUTE
// register dword offset minus this. With GC[0] = 0x1260 on this card,
// COMPUTE_PGM_LO (0x1bac) becomes 0x1260 + 0x1bac - 0x2c00 = 0x20c, which is
// also what Mesa computes as (R_00B830_COMPUTE_PGM_LO - 0xB000) >> 2 — two
// independent derivations agreeing is the check that this constant is right.
constexpr uint32_t kPacket3SetShRegStart = 0x00002C00u;

// A one-dword NOP. PACKET3_NOP (opcode 0x10) with COUNT = 0x3FFF is the PM4
// special case that occupies a single dword (amdgpu's ring->funcs->nop for
// every gfx ring: PACKET3(PACKET3_NOP, 0x3FFF) == 0xFFFF1000). The earlier
// 0xFFFF0000 here carried opcode 0x00 — see cp_pm4_gfx12.h.
constexpr uint32_t kNop1Dword = 0xFFFF1000u;

// Indirect-buffer submission (gfx_v12_0_ring_emit_ib_gfx): the IB lives in
// VRAM, the INDIRECT_BUFFER control dword is length_dw | (vmid << 24), and
// kernel work runs with VMID 0 (amdgpu_ring_test_ib: job == NULL -> vmid 0).
constexpr uint64_t kIbBytes     = 4096;
constexpr uint32_t kIbVmid      = 0;
constexpr uint32_t kIbTestMagic = 0x1B7E57EDu;   // "IB tested"

// EVENT_WRITE payload: EVENT_TYPE[5:0] | EVENT_INDEX[11:8].
// CS_PARTIAL_FLUSH = 7 with EVENT_INDEX 4 — the "wait for the compute
// dispatch to drain" event both amdgpu and Mesa emit right after
// DISPATCH_DIRECT.
constexpr uint32_t kEventCsPartialFlush = 7u | (4u << 8);   // 0x00000407

// RELEASE_MEM ordinal 1 (nvd.h field positions):
//   EVENT_TYPE[5:0]   = CACHE_FLUSH_AND_INV_TS_EVENT (0x14)
//   EVENT_INDEX[11:8] = 5
//   GCR_GL2_WB (1<<21), GCR_SEQ (1<<22)
//   CACHE_POLICY[26:25] = 3 (BYPASS) so the fence dword is coherent for the
//                         CPU's read of the write-back page.
// Deliberately conservative: no GL2_INV/GL2_DISCARD/GLM bits, so that even if
// one of these positions is off by one the worst case is a cache that did not
// get flushed — never a discarded dirty line.
constexpr uint32_t kReleaseMemDw1 =
      (0x14u)            // EVENT_TYPE  = CACHE_FLUSH_AND_INV_TS_EVENT
    | (5u << 8)          // EVENT_INDEX = 5
    | (1u << 21)         // GCR_GL2_WB
    | (1u << 22)         // GCR_SEQ
    | (3u << 25);        // CACHE_POLICY = BYPASS
// RELEASE_MEM ordinal 2: DATA_SEL[31:29] = 2 (64-bit), INT_SEL[25:24] = 2
// (send interrupt, so the IH ring also witnesses it), DST_SEL[17:16] = 0
// (memory controller).
constexpr uint32_t kReleaseMemDw2 = (2u << 29) | (2u << 24) | (0u << 16);

//====================================================================
// GC register offsets — gc_12_0_0_offset.h, all with _BASE_IDX 0
//====================================================================
namespace Reg {
constexpr uint32_t COMPUTE_DISPATCH_INITIATOR   = 0x1BA0;  // offset.h:2242
constexpr uint32_t COMPUTE_NUM_THREAD_X         = 0x1BA7;  // offset.h:2256
constexpr uint32_t COMPUTE_NUM_THREAD_Y         = 0x1BA8;  // offset.h:2258
constexpr uint32_t COMPUTE_NUM_THREAD_Z         = 0x1BA9;  // offset.h:2260
constexpr uint32_t COMPUTE_PGM_LO               = 0x1BAC;  // offset.h:2266
constexpr uint32_t COMPUTE_PGM_HI               = 0x1BAD;  // offset.h:2268
constexpr uint32_t COMPUTE_PGM_RSRC1            = 0x1BB2;  // offset.h:2278
constexpr uint32_t COMPUTE_PGM_RSRC2            = 0x1BB3;  // offset.h:2280
constexpr uint32_t COMPUTE_VMID                 = 0x1BB4;
constexpr uint32_t COMPUTE_RESOURCE_LIMITS      = 0x1BB5;  // offset.h:2284
constexpr uint32_t COMPUTE_STATIC_THREAD_MGMT_SE0 = 0x1BB6; // offset.h:2288
constexpr uint32_t COMPUTE_STATIC_THREAD_MGMT_SE1 = 0x1BB7; // offset.h:2292
constexpr uint32_t COMPUTE_TMPRING_SIZE         = 0x1BB8;  // offset.h:2294
constexpr uint32_t COMPUTE_STATIC_THREAD_MGMT_SE2 = 0x1BB9; // offset.h:2298
constexpr uint32_t COMPUTE_STATIC_THREAD_MGMT_SE3 = 0x1BBA; // offset.h:2302
constexpr uint32_t COMPUTE_PGM_RSRC3            = 0x1BC8;  // offset.h:2330
constexpr uint32_t COMPUTE_USER_DATA_0          = 0x1BE0;  // offset.h:2360
constexpr uint32_t COMPUTE_USER_DATA_1          = 0x1BE1;  // offset.h:2362
// Read-only witnesses for the failure dump.
constexpr uint32_t GRBM_STATUS                  = 0x0DA4;  // offset.h:2002, BASE_IDX 0
constexpr uint32_t CP_STAT                      = 0x0F40;  // offset.h:2130, BASE_IDX 0
}

// Each PACKET3_SET_SH_REG below writes a RUN of consecutive registers from one
// starting offset. Prove the runs really are consecutive, and in the order the
// value arrays assume, at compile time — a transposed pair here would silently
// program COMPUTE_TMPRING_SIZE with a CU mask.
static_assert(Reg::COMPUTE_PGM_HI == Reg::COMPUTE_PGM_LO + 1, "PGM_LO/HI run");
static_assert(Reg::COMPUTE_PGM_RSRC2             == Reg::COMPUTE_PGM_RSRC1 + 1, "RSRC run +1");
static_assert(Reg::COMPUTE_VMID                  == Reg::COMPUTE_PGM_RSRC1 + 2, "RSRC run +2");
static_assert(Reg::COMPUTE_RESOURCE_LIMITS       == Reg::COMPUTE_PGM_RSRC1 + 3, "RSRC run +3");
static_assert(Reg::COMPUTE_STATIC_THREAD_MGMT_SE0 == Reg::COMPUTE_PGM_RSRC1 + 4, "RSRC run +4");
static_assert(Reg::COMPUTE_STATIC_THREAD_MGMT_SE1 == Reg::COMPUTE_PGM_RSRC1 + 5, "RSRC run +5");
// TMPRING_SIZE really does sit between SE1 and SE2 in gc_12_0_0_offset.h.
static_assert(Reg::COMPUTE_TMPRING_SIZE          == Reg::COMPUTE_PGM_RSRC1 + 6, "RSRC run +6");
static_assert(Reg::COMPUTE_STATIC_THREAD_MGMT_SE2 == Reg::COMPUTE_PGM_RSRC1 + 7, "RSRC run +7");
static_assert(Reg::COMPUTE_STATIC_THREAD_MGMT_SE3 == Reg::COMPUTE_PGM_RSRC1 + 8, "RSRC run +8");
static_assert(Reg::COMPUTE_NUM_THREAD_Y == Reg::COMPUTE_NUM_THREAD_X + 1, "NUM_THREAD run");
static_assert(Reg::COMPUTE_NUM_THREAD_Z == Reg::COMPUTE_NUM_THREAD_X + 2, "NUM_THREAD run");
static_assert(Reg::COMPUTE_USER_DATA_1  == Reg::COMPUTE_USER_DATA_0 + 1,  "USER_DATA run");

//====================================================================
// Shader resource descriptors
//
// Authoritative source: shaders/store_magic.kd.txt, the HSA kernel descriptor
// LLVM produced for shaders/store_magic.s (bytes 48-51 / 52-55 / 44-47).
// tools/build-shader.sh prints them on every rebuild — if they ever disagree
// with these three constants, the build script's output is right and these
// are stale.
//====================================================================

// COMPUTE_PGM_RSRC1 = 0x600F0000. Fields per gc_12_0_0_sh_mask.h:7447-7474:
//   VGPRS        [5:0]   0     -> 1 allocation granule. The gfx12 wave32 VGPR
//                                 granule is 8 and the shader uses v0..v2, so
//                                 ceil(3/8) - 1 = 0. (Under-allocating here is
//                                 fatal; 8 VGPRs is what the wave gets.)
//   SGPRS        [9:6]   0     -> ignored on gfx10+: the hardware allocates a
//                                 fixed SGPR file per wave. LLVM emits 0.
//   PRIORITY     [11:10] 0
//   FLOAT_MODE   [19:12] 0xF0  -> FP_ROUND = 0 (round-nearest-even),
//                                 FP_DENORM = 0xF (denormals enabled). The
//                                 shader does no float math; this is LLVM's
//                                 default and is inert here.
//   PRIV/WG_RR_EN/DEBUG_MODE/DISABLE_PERF/BULKY/CDBG_USER/FP16_OVFL  0
//   WGP_MODE     [29]    1     -> WGP (dual-CU) mode. NOTE: on gfx10+ the
//                                 wave *size* is NOT in RSRC1 — wave32 is
//                                 selected by COMPUTE_DISPATCH_INITIATOR
//                                 .CS_W32_EN (sh_mask.h:7363). WGP_MODE only
//                                 chooses WGP vs CU resource allocation, and 1
//                                 is the gfx10+ compiler default.
//   MEM_ORDERED  [30]    1
//   FWD_PROGRESS [31]    0
constexpr uint32_t kComputePgmRsrc1 = 0x600F0000u;

// COMPUTE_PGM_RSRC2 = 0x00000084. Fields per gc_12_0_0_sh_mask.h:7476-7499:
//   SCRATCH_EN     [0]     0  -> no scratch/private segment (TMPRING_SIZE = 0)
//   USER_SGPR      [5:1]   2  -> s[0:1] preloaded from COMPUTE_USER_DATA_0/1
//                                = the 64-bit result-buffer MC address
//   DYNAMIC_VGPR   [6]     0
//   TGID_X_EN      [7]     1  -> s2 = workgroup id X (unused by the shader,
//                                but this is what LLVM declares and it costs
//                                one SGPR)
//   TGID_Y/Z_EN    [8],[9] 0
//   TG_SIZE_EN     [10]    0
//   TIDIG_COMP_CNT [12:11] 0  -> only work-item id X is delivered; on gfx11+
//                                it arrives packed in v0 bits [9:0]
//   LDS_SIZE       [23:15] 0  -> no LDS
constexpr uint32_t kComputePgmRsrc2 = 0x00000084u;

// COMPUTE_PGM_RSRC3 = 0. Fields per gc_12_0_0_sh_mask.h:7615-7622:
// SHARED_VGPR_CNT 0, INST_PREF_SIZE 0 (no instruction prefetch beyond the
// blob — deliberate: the 56-byte shader plus the s_endpgm fill below is all
// that is initialised), GLG_EN 0, IMAGE_OP 0.
constexpr uint32_t kComputePgmRsrc3 = 0x00000000u;

// The LLVM-compiled variant's descriptor, from shaders/store_magic_hsa.kd.txt.
// RSRC1 differs from the hand-written one only in bit 31 (forward progress);
// RSRC2 only in TGID_Y/Z_EN; RSRC3 carries the instruction-prefetch size that
// goes with LLVM's s_code_end padding. USER_SGPR is 2 and TIDIG_COMP_CNT is 0
// in both, which is why the same dispatch can run either.
constexpr uint32_t kHsaPgmRsrc1 = 0xe00f0000u;
constexpr uint32_t kHsaPgmRsrc2 = 0x00000384u;
constexpr uint32_t kHsaPgmRsrc3 = 0x00000010u;

// COMPUTE_DISPATCH_INITIATOR — gc_12_0_0_sh_mask.h:7351-7384. Same set Mesa's
// si_emit_dispatch_packets uses:
//   COMPUTE_SHADER_EN   [0]  launch it
//   FORCE_START_AT_000  [2]  start at (0,0,0); makes COMPUTE_START_X/Y/Z moot
//   ORDER_MODE          [6]  allow waves to launch out of order (GFX7+)
//   CS_W32_EN           [15] wave32 — must match .amdhsa_wavefront_size32 1
constexpr uint32_t kDispatchInitiator = (1u << 0) | (1u << 2) | (1u << 6) | (1u << 15);

// COMPUTE_RESOURCE_LIMITS = 0 (no WAVES_PER_SH / TG_PER_CU / CU_GROUP_COUNT
// limits), COMPUTE_TMPRING_SIZE = 0 (no scratch ring),
// COMPUTE_STATIC_THREAD_MGMT_SEn = all CUs enabled (SA0_CU_EN | SA1_CU_EN,
// sh_mask.h:7520-7523).
constexpr uint32_t kResourceLimits  = 0x00000000u;
constexpr uint32_t kTmpringSize     = 0x00000000u;
constexpr uint32_t kAllCUsEnabled   = 0xFFFFFFFFu;

//====================================================================
// Knobs
//====================================================================
// Set true to call cp_emit_eop_fence() instead of emit_eop_fence_fixed().
// Only do that once amdgpu_pm4.h's pm4_header() is fixed — see the FINDING
// block at the top of this file.
constexpr bool kUseCpEmitEopFence = true;

constexpr uint64_t kTimeoutUs        = 2000000;  // 2 s, as PM4Test uses
constexpr uint64_t kFastPollUs       = 5000;     // first 5 ms at 1 us steps
constexpr uint64_t kResultBytes      = 4096;
constexpr uint64_t kCodeAlign        = 256;      // COMPUTE_PGM_LO is addr >> 8
constexpr uint32_t kEndpgmFillDwords = 128;      // 512 B of s_endpgm behind the
                                                 // shader, so a stray fetch
                                                 // lands on a legal end
constexpr uint32_t kSEndpgm          = 0xBFB00000u;   // s_endpgm, gfx12

//====================================================================
// Helpers
//====================================================================

// Absolute BAR5 dword index of a GC[0] register.
inline uint32_t gc0(const DeviceContext &dev, uint32_t reg)
{
    return SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, 0, reg);
}

// The value PACKET3_SET_SH_REG carries for `reg`.
inline uint32_t sh_index(const DeviceContext &dev, uint32_t reg)
{
    return gc0(dev, reg) - kPacket3SetShRegStart;
}

// Emit one PACKET3_SET_SH_REG covering `n` CONSECUTIVE registers starting at
// GC-relative offset `first_reg`. Payload = 1 index dword + n value dwords,
// so COUNT = n. Logs every register it writes, by name and value.
uint32_t emit_set_sh_reg(const DeviceContext &dev, uint32_t *pkt, uint32_t at,
                         uint32_t first_reg, const uint32_t *vals, uint32_t n,
                         const char *const *names)
{
    const uint32_t idx = sh_index(dev, first_reg);
    pkt[at++] = pm4_type3_cs(kOpSetShReg, n);
    pkt[at++] = idx;
    for (uint32_t i = 0; i < n; i++) {
        pkt[at++] = vals[i];
        CS_LOG("  SET_SH_REG(cs) %s gc_off=%#06x abs_dw=%#07x sh_idx=%#05x <- %#010x",
               names[i], first_reg + i, gc0(dev, first_reg + i), idx + i, vals[i]);
    }
    return at;
}

// PACKET3_SET_SH_REG_INDEX with INDEX = 3 — how gfx10+ writes the
// COMPUTE_STATIC_THREAD_MGMT_SE* CU masks (Mesa radeon_set_sh_reg_idx3,
// libdrm's 0x30000216 / 0x30000219). The index rides in bits [31:28] of the
// register-offset dword; the CP resolves the mask against the CUs that exist.
uint32_t emit_set_sh_reg_index3(const DeviceContext &dev, uint32_t *pkt, uint32_t at,
                                uint32_t first_reg, const uint32_t *vals, uint32_t n,
                                const char *const *names)
{
    const uint32_t idx = sh_index(dev, first_reg);
    pkt[at++] = pm4_type3_cs(kOpSetShRegIndex, n);
    pkt[at++] = idx | (3u << 28);
    for (uint32_t i = 0; i < n; i++) {
        pkt[at++] = vals[i];
        CS_LOG("  SET_SH_REG_INDEX(cs, idx 3) %s gc_off=%#06x abs_dw=%#07x sh_idx=%#05x <- %#010x",
               names[i], first_reg + i, gc0(dev, first_reg + i), idx + i, vals[i]);
    }
    return at;
}

// GRBM_STATUS with its busy bits spelled out (gc_12_0_0_sh_mask.h:5932-5952).
// SPI_BUSY with CP_BUSY/ME busy after a dispatch = waves launched and stuck.
void log_grbm_status(const DeviceContext &dev, const char *when)
{
    const uint32_t v = RREG32(dev, gc0(dev, Reg::GRBM_STATUS));
    CS_LOG("GRBM_STATUS (%s) = %#010x:%s%s%s%s%s%s%s%s%s%s%s%s  CP_STAT=%#010x", when, v,
           (v & 0x80000000u) ? " GUI_ACTIVE" : "", (v & 0x40000000u) ? " CB_BUSY" : "",
           (v & 0x20000000u) ? " CP_BUSY" : "",    (v & 0x10000000u) ? " CP_COHERENCY_BUSY" : "",
           (v & 0x08000000u) ? " ANY_ACTIVE" : "", (v & 0x04000000u) ? " DB_BUSY" : "",
           (v & 0x02000000u) ? " PA_BUSY" : "",    (v & 0x01000000u) ? " SC_BUSY" : "",
           (v & 0x00800000u) ? " BCI_BUSY" : "",   (v & 0x00400000u) ? " SPI_BUSY" : "",
           (v & 0x00200000u) ? " GE_BUSY" : "",    (v & 0x00004000u) ? " TA_BUSY" : "",
           RREG32(dev, gc0(dev, Reg::CP_STAT)));
}

// Functional clone of cp_emit_eop_fence() with a correctly encoded PACKET3
// header (see the FINDING block). Same ring, same fence slot, same counter —
// cp_kick_doorbell() and *cp.fence_cpu polling behave identically.
uint32_t emit_eop_fence_fixed(CPContext &cp)
{
    if (!cp.inited) return 0;

    const uint32_t fence = ++cp.fence_counter;
    uint32_t pkt[9];
    uint32_t n = 0;

    pkt[n++] = kNop1Dword;
    pkt[n++] = pm4_type3(kOpReleaseMem, 6);   // 7 payload dwords - 1
    pkt[n++] = kReleaseMemDw1;
    pkt[n++] = kReleaseMemDw2;
    pkt[n++] = (uint32_t)(cp.fence_gpu_addr & 0xFFFFFFF8u);   // qword aligned
    pkt[n++] = (uint32_t)(cp.fence_gpu_addr >> 32);
    pkt[n++] = fence;   // value lo
    pkt[n++] = 0;       // value hi

    CS_LOG("EOP fence (locally encoded): value=%u slot_mc=%#llx "
           "hdr=%#010x dw1=%#010x dw2=%#010x",
           fence, (unsigned long long)cp.fence_gpu_addr,
           pm4_type3(kOpReleaseMem, 6), kReleaseMemDw1, kReleaseMemDw2);

    if (cp_ring_write(cp, pkt, n) != n) {
        CS_LOG("EOP fence: ring_write refused %u dwords", n);
        return 0;
    }
    return fence;
}

void dump_packets(const uint32_t *pkt, uint32_t n, const char *what)
{
    CS_LOG("%s: %u dwords", what, n);
    for (uint32_t i = 0; i < n; i += 4) {
        const uint32_t a = pkt[i];
        const uint32_t b = (i + 1 < n) ? pkt[i + 1] : 0;
        const uint32_t c = (i + 2 < n) ? pkt[i + 2] : 0;
        const uint32_t d = (i + 3 < n) ? pkt[i + 3] : 0;
        CS_LOG("  [%02u] %08x %08x %08x %08x", i, a, b, c, d);
    }
}

// Read back the COMPUTE_* SH registers. If the CP consumed our SET_SH_REG
// packets these hold what we sent; if they hold something else the packets
// never landed and the problem is upstream of the shader.
void dump_sh_readback(const DeviceContext &dev, const char *when)
{
    CS_LOG("SH register readback (%s): PGM_LO=%#010x PGM_HI=%#010x "
           "RSRC1=%#010x RSRC2=%#010x RSRC3=%#010x", when,
           RREG32(dev, gc0(dev, Reg::COMPUTE_PGM_LO)),
           RREG32(dev, gc0(dev, Reg::COMPUTE_PGM_HI)),
           RREG32(dev, gc0(dev, Reg::COMPUTE_PGM_RSRC1)),
           RREG32(dev, gc0(dev, Reg::COMPUTE_PGM_RSRC2)),
           RREG32(dev, gc0(dev, Reg::COMPUTE_PGM_RSRC3)));
    CS_LOG("SH register readback (%s): NUM_THREAD_X/Y/Z=%u/%u/%u "
           "USER_DATA_0=%#010x USER_DATA_1=%#010x VMID=%#010x "
           "DISPATCH_INITIATOR=%#010x", when,
           RREG32(dev, gc0(dev, Reg::COMPUTE_NUM_THREAD_X)),
           RREG32(dev, gc0(dev, Reg::COMPUTE_NUM_THREAD_Y)),
           RREG32(dev, gc0(dev, Reg::COMPUTE_NUM_THREAD_Z)),
           RREG32(dev, gc0(dev, Reg::COMPUTE_USER_DATA_0)),
           RREG32(dev, gc0(dev, Reg::COMPUTE_USER_DATA_1)),
           RREG32(dev, gc0(dev, Reg::COMPUTE_VMID)),
           RREG32(dev, gc0(dev, Reg::COMPUTE_DISPATCH_INITIATOR)));
}

} // anonymous namespace

//====================================================================
// compute_dispatch_test
//====================================================================
kern_return_t
compute_dispatch_test(DeviceContext &dev, GMCContext &gmc, CPContext &cp,
                      ComputeTestResult *out, bool hsaAbi)
{
    ComputeTestResult local {};
    ComputeTestResult &r = out ? *out : local;
    r = ComputeTestResult {};

    // Which shader, and therefore which argument convention. Everything below
    // goes through these, so the default path is exactly what it was.
    const uint8_t *sh_code  = hsaAbi ? fw_shader_store_magic_hsa      : fw_shader_store_magic;
    const size_t   sh_size  = hsaAbi ? fw_shader_store_magic_hsa_size : fw_shader_store_magic_size;
    const uint32_t sh_rsrc1 = hsaAbi ? kHsaPgmRsrc1 : kComputePgmRsrc1;
    const uint32_t sh_rsrc2 = hsaAbi ? kHsaPgmRsrc2 : kComputePgmRsrc2;
    const uint32_t sh_rsrc3 = hsaAbi ? kHsaPgmRsrc3 : kComputePgmRsrc3;
    // The magic is part of the shader, so it switches with it.
    const uint32_t sh_magic = hsaAbi ? kComputeMagicHsa : kComputeMagic;
    CS_LOG("shader: %s (%llu bytes), RSRC1=%#010x RSRC2=%#010x RSRC3=%#010x, "
           "s[0:1] = %s",
           hsaAbi ? "store_magic_hsa (LLVM-compiled)" : "store_magic (hand-written)",
           (unsigned long long)sh_size, sh_rsrc1, sh_rsrc2, sh_rsrc3,
           hsaAbi ? "POINTER TO a kernarg buffer" : "the result buffer address");
    r.kr               = kIOReturnNotReady;
    r.lanes_checked    = kComputeThreadsX;
    r.dispatch_initiator = kDispatchInitiator;
    r.shader_bytes     = (uint32_t)sh_size;
    for (uint32_t i = 0; i < 4; i++) {
        r.expected[i] = sh_magic + i;
        r.observed[i] = 0;
    }

    //----------------------------------------------------------------
    // 0. Preconditions — each failure names exactly what was missing.
    //----------------------------------------------------------------
    if (!cp.inited || cp.ring_cpu == nullptr || cp.fence_cpu == nullptr) {
        CS_LOG("refused: CP GFX ring not initialised (inited=%d ring_cpu=%p fence_cpu=%p) "
               "— run the ladder to CPInit (stage 12) first",
               cp.inited, cp.ring_cpu, (void *)cp.fence_cpu);
        r.kr = kIOReturnNotReady; return kIOReturnNotReady;
    }
    if (dev.bar2 == nullptr) {
        CS_LOG("refused: BAR2 (doorbells) not mapped");
        r.kr = kIOReturnNotAttached; return kIOReturnNotAttached;
    }
    if (dev.bar0 == nullptr) {
        CS_LOG("refused: BAR0 (VRAM aperture) not mapped");
        r.kr = kIOReturnNotAttached; return kIOReturnNotAttached;
    }
    if (!gmc.inited || !gmc.vram_alloc.is_inited()) {
        CS_LOG("refused: GMC/VRAM allocator not ready (gmc.inited=%d alloc=%d)",
               gmc.inited, gmc.vram_alloc.is_inited());
        r.kr = kIOReturnNotReady; return kIOReturnNotReady;
    }
    if (!dev.ip.isResolved(IPBlock::GC, 0)) {
        CS_LOG("refused: GC IP base segment 0 unresolved — every COMPUTE_* register "
               "carries reg<NAME>_BASE_IDX 0");
        r.kr = kIOReturnNoDevice; return kIOReturnNoDevice;
    }
    if (sh_size == 0 || sh_size > 65536) {
        CS_LOG("refused: embedded shader blob is %llu bytes — run tools/build-shader.sh",
               (unsigned long long)sh_size);
        r.kr = kIOReturnNoResources; return kIOReturnNoResources;
    }
    {
        const uint32_t abs_lo = gc0(dev, Reg::COMPUTE_DISPATCH_INITIATOR);
        if (abs_lo < kPacket3SetShRegStart) {
            CS_LOG("refused: GC[0]=%#x puts COMPUTE_DISPATCH_INITIATOR at abs dword %#x, "
                   "below PACKET3_SET_SH_REG_START %#x — SET_SH_REG index would underflow",
                   dev.ip.getBase(IPBlock::GC, 0), abs_lo, kPacket3SetShRegStart);
            r.kr = kIOReturnBadArgument; return kIOReturnBadArgument;
        }
    }

    CS_LOG("=== compute dispatch test: GC[0]=%#x vram_start=%#llx "
           "shader=%llu bytes doorbell=%u ===",
           dev.ip.getBase(IPBlock::GC, 0),
           (unsigned long long)gmc.vram_start,
           (unsigned long long)sh_size,
           cp.doorbell_index);

    //----------------------------------------------------------------
    // 1. Shader code in VRAM.
    //----------------------------------------------------------------
    VRAMAllocation code {};
    if (!gmc.vram_alloc.alloc(sh_size + kEndpgmFillDwords * 4,
                              kCodeAlign, &code)) {
        CS_LOG("VRAM alloc for shader code failed (%llu free)",
               (unsigned long long)gmc.vram_alloc.bytes_free());
        r.kr = kIOReturnNoMemory; return kIOReturnNoMemory;
    }
    const uint64_t code_off = code.gpu_va - gmc.vram_start;
    r.code_gpu_va = code.gpu_va;

    // Fill with s_endpgm first: a stray instruction fetch past the blob (or a
    // COMPUTE_PGM_LO off by a granule) then ends the wave instead of executing
    // uninitialised VRAM.
    bar0_memset_vram(dev, code_off, kSEndpgm, kEndpgmFillDwords * 4);
    bar0_memcpy_to_vram(dev, code_off, sh_code, sh_size);
    (void)gmc_hdp_flush(dev);

    // Verify the copy through the GPU's own view of VRAM, not the CPU's.
    {
        bool code_ok = true;
        const uint32_t ndw = (uint32_t)(sh_size / 4);
        for (uint32_t i = 0; i < ndw; i++) {
            uint32_t want = 0;
            const uint8_t *p = sh_code + i * 4;
            want = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            const uint32_t got = RVRAM32_via_mm(dev, code_off + i * 4);
            if (got != want) {
                CS_LOG("shader copy MISMATCH at dword %u: vram=%#010x expected=%#010x",
                       i, got, want);
                code_ok = false;
            }
        }
        CS_LOG("shader staged at vram+%#llx (mc %#llx, %u dwords, PGM_LO=%#010x "
               "PGM_HI=%#010x) — GPU-side verify %s",
               (unsigned long long)code_off, (unsigned long long)code.gpu_va, ndw,
               (uint32_t)((code.gpu_va >> 8) & 0xFFFFFFFFu),
               (uint32_t)((code.gpu_va >> 40) & 0xFFu),
               code_ok ? "OK" : "FAILED");
        if (!code_ok) { r.kr = kIOReturnIOError; return kIOReturnIOError; }
    }

    //----------------------------------------------------------------
    // 2. Result buffer in VRAM, poisoned.
    //----------------------------------------------------------------
    VRAMAllocation res {};
    if (!gmc.vram_alloc.alloc(kResultBytes, 4096, &res)) {
        CS_LOG("VRAM alloc for the result buffer failed");
        r.kr = kIOReturnNoMemory; return kIOReturnNoMemory;
    }
    const uint64_t res_off = res.gpu_va - gmc.vram_start;
    r.result_gpu_va = res.gpu_va;

    //----------------------------------------------------------------
    // 2b. Kernel-argument buffer, HSA path only.
    //
    // LLVM's kernel does `s_load_b64 s[0:1], s[0:1], 0x0`: the user SGPRs hold a
    // POINTER to the arguments, not the argument. So allocate a page, put the
    // result buffer's MC address in its first 8 bytes, and hand the dispatch
    // that page instead. This is the standard convention and the only one that
    // generalises past a single argument.
    //----------------------------------------------------------------
    VRAMAllocation karg {};
    uint64_t karg_va = 0;
    if (hsaAbi) {
        if (!gmc.vram_alloc.alloc(4096, 4096, &karg)) {
            CS_LOG("VRAM alloc for the kernarg buffer failed");
            r.kr = kIOReturnNoMemory; return kIOReturnNoMemory;
        }
        const uint64_t karg_off = karg.gpu_va - gmc.vram_start;
        karg_va = karg.gpu_va;
        const uint32_t a_lo = (uint32_t)(res.gpu_va & 0xFFFFFFFFu);
        const uint32_t a_hi = (uint32_t)(res.gpu_va >> 32);
        bar0_memset_vram(dev, karg_off, 0, 4096);
        bar0_memcpy_to_vram(dev, karg_off,     &a_lo, 4);
        bar0_memcpy_to_vram(dev, karg_off + 4, &a_hi, 4);
        (void)gmc_hdp_flush(dev);
        CS_LOG("kernarg buffer at vram+%#llx (mc %#llx): [0..7] = result mc %#llx, "
               "GPU-side readback %#010x %#010x",
               (unsigned long long)karg_off, (unsigned long long)karg_va,
               (unsigned long long)res.gpu_va,
               RVRAM32_via_mm(dev, karg_off), RVRAM32_via_mm(dev, karg_off + 4));
    }

    bar0_memset_vram(dev, res_off, kComputePoison, kResultBytes);
    (void)gmc_hdp_flush(dev);
    {
        const uint32_t p0 = RVRAM32_via_mm(dev, res_off);
        const uint32_t p1 = RVRAM32_via_mm(dev, res_off + 4);
        CS_LOG("result buffer at vram+%#llx (mc %#llx, %llu bytes) poisoned with %#010x "
               "— GPU-side readback %#010x %#010x %s",
               (unsigned long long)res_off, (unsigned long long)res.gpu_va,
               (unsigned long long)kResultBytes, kComputePoison, p0, p1,
               (p0 == kComputePoison && p1 == kComputePoison) ? "OK" : "UNEXPECTED");
    }

    //----------------------------------------------------------------
    // 3. Indirect buffer in VRAM + the ring-side submission helper.
    //
    // Linux never puts shader work directly in the kernel ring. Every job is
    //     CONTEXT_CONTROL + INDIRECT_BUFFER{addr, length_dw | vmid << 24} + fence
    // (gfx_v12_0_ring_emit_cntxcntl, gfx_v12_0_ring_emit_ib_gfx, then
    // gfx_v12_0_ring_emit_fence_gfx), and the INDIRECT_BUFFER packet is what
    // binds the work to a VMID — kernel IBs use VMID 0 (amdgpu_ring_test_ib).
    // Stage 17 run 1 dispatched straight from the ring: the wave ran under
    // VMID 6 — nothing of ours, just the CP's never-initialised IB-VMID state —
    // and its instruction fetch faulted (IH fault IV: vmid 6, READ|EXE at the
    // shader page). So: first amdgpu_ring_test_ib's WRITE_DATA-in-an-IB, then
    // the dispatch the same way.
    //----------------------------------------------------------------
    VRAMAllocation ib {};
    if (!gmc.vram_alloc.alloc(kIbBytes, 4096, &ib)) {
        CS_LOG("VRAM alloc for the indirect buffer failed");
        r.kr = kIOReturnNoMemory; return kIOReturnNoMemory;
    }
    const uint64_t ib_off = ib.gpu_va - gmc.vram_start;
    r.ib_gpu_va = ib.gpu_va;

    // Copy `n` dwords into the IB (BAR0 write, HDP flush, GPU-side verify),
    // then ring: CONTEXT_CONTROL, INDIRECT_BUFFER (vmid 0), EOP fence, doorbell.
    auto submit_ib = [&](const uint32_t *dw, uint32_t n, const char *what,
                         uint32_t *fence_out) -> kern_return_t {
        if (n == 0 || (uint64_t)n * 4 > kIbBytes) return kIOReturnBadArgument;
        bar0_memcpy_to_vram(dev, ib_off, dw, n * 4);
        (void)gmc_hdp_flush(dev);
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t got = RVRAM32_via_mm(dev, ib_off + i * 4);
            if (got != dw[i]) {
                CS_LOG("%s: IB copy MISMATCH at dword %u: vram=%#010x expected=%#010x",
                       what, i, got, dw[i]);
                return kIOReturnIOError;
            }
        }
        dump_packets(dw, n, what);

        uint32_t ring[7];
        uint32_t k = 0;
        // gfx_v12_0_ring_emit_cntxcntl(flags = 0): load_enable, nothing else.
        ring[k++] = pm4_type3(kOpContextControl, 1);
        ring[k++] = 0x80000000u;
        ring[k++] = 0u;
        // gfx_v12_0_ring_emit_ib_gfx: PACKET3(INDIRECT_BUFFER, 2), addr lo/hi,
        // control = length_dw | (vmid << 24).
        ring[k++] = pm4_type3(kOpIndirectBuffer, 2);
        ring[k++] = (uint32_t)(ib.gpu_va & 0xFFFFFFFFu);
        ring[k++] = (uint32_t)(ib.gpu_va >> 32);
        ring[k++] = (n & 0xFFFFFu) | (kIbVmid << 24);
        CS_LOG("%s: ring <- CONTEXT_CONTROL %#010x %#010x | INDIRECT_BUFFER mc=%#llx "
               "len=%u vmid=%u control=%#010x",
               what, ring[1], ring[2], (unsigned long long)ib.gpu_va, n, kIbVmid, ring[6]);
        if (cp_ring_write(cp, ring, k) != k) {
            CS_LOG("%s: cp_ring_write refused %u dwords", what, k);
            return kIOReturnNoResources;
        }
        const uint32_t fence = kUseCpEmitEopFence ? cp_emit_eop_fence(cp)
                                                  : emit_eop_fence_fixed(cp);
        if (fence == 0) return kIOReturnInternalError;
        *fence_out = fence;
        CS_LOG("%s: submitting — wptr -> %u, fence=%u (slot mc=%#llx), doorbell %u",
               what, cp.wptr, fence, (unsigned long long)cp.fence_gpu_addr,
               cp.doorbell_index);
        return cp_kick_doorbell(dev, cp);
    };

    // Poll the fence slot and a VRAM dword. elapsed_us is a loop-step
    // estimate (IODelay/IOSleep accounting), the same approximation PM4Test
    // uses; the VRAM dword sits behind the MM_INDEX window (three MMIO
    // accesses per look) so it is sampled every kDataPollEvery fast steps.
    auto wait_for = [&](uint32_t fence, uint64_t vram_off, uint32_t want,
                        bool *fence_landed, bool *value_landed,
                        uint64_t *fence_us_out, uint64_t *value_us_out,
                        uint64_t *fence_obs_out) -> uint64_t {
        uint64_t elapsed = 0, fence_us = 0, value_us = 0, fence_obs = 0;
        bool fl = false, vl = false;
        constexpr uint32_t kDataPollEvery = 64;
        uint32_t iter = 0;
        while (elapsed < kTimeoutUs) {
            if (!fl) {
                sysmem_rmb();
                fence_obs = *cp.fence_cpu;
                if ((fence_obs & 0xFFFFFFFFu) == fence) { fl = true; fence_us = elapsed; }
            }
            const bool look = fl || elapsed >= kFastPollUs || (iter % kDataPollEvery) == 0;
            if (!vl && look) {
                if (RVRAM32_via_mm(dev, vram_off) == want) { vl = true; value_us = elapsed; }
            }
            if (fl && vl) break;
            iter++;
            if (elapsed < kFastPollUs) { IODelay(1);  elapsed += 1; }
            else                       { IOSleep(1);  elapsed += 1000; }
        }
        sysmem_rmb();
        fence_obs = *cp.fence_cpu;
        if (!fl && (fence_obs & 0xFFFFFFFFu) == fence) { fl = true; fence_us = elapsed; }
        *fence_landed = fl; *value_landed = vl;
        *fence_us_out = fence_us; *value_us_out = value_us; *fence_obs_out = fence_obs;
        return elapsed;
    };

    //----------------------------------------------------------------
    // 4. IB smoke test — gfx_v12_0_ring_test_ib: an IB holding one
    //    WRITE_DATA(DST_SEL 5 memory, WR_CONFIRM) of a magic to a VRAM dword,
    //    submitted through INDIRECT_BUFFER with VMID 0. Proves the IB path
    //    before any shader is involved.
    //----------------------------------------------------------------
    VRAMAllocation ibt {};
    if (!gmc.vram_alloc.alloc(4096, 4096, &ibt)) {
        CS_LOG("VRAM alloc for the IB-test target failed");
        r.kr = kIOReturnNoMemory; return kIOReturnNoMemory;
    }
    const uint64_t ibt_off = ibt.gpu_va - gmc.vram_start;
    bar0_memset_vram(dev, ibt_off, kComputePoison, 64);
    (void)gmc_hdp_flush(dev);
    {
        uint32_t t[5];
        t[0] = pm4_type3(kOpWriteData, 3);
        t[1] = pm4_write_data_control(kPM4WriteDataEngineME, kPM4WriteDataDstSelMemory, true);
        t[2] = (uint32_t)(ibt.gpu_va & 0xFFFFFFFFu);
        t[3] = (uint32_t)(ibt.gpu_va >> 32);
        t[4] = kIbTestMagic;
        uint32_t fence = 0;
        kern_return_t kr = submit_ib(t, 5, "IB test (WRITE_DATA inside an INDIRECT_BUFFER, vmid 0)", &fence);
        if (kr != kIOReturnSuccess) { CS_LOG("IB test: submit failed %#x", kr); r.kr = kr; return kr; }
        bool fl = false, vl = false;
        uint64_t fus = 0, vus = 0, fobs = 0;
        (void)wait_for(fence, ibt_off, kIbTestMagic, &fl, &vl, &fus, &vus, &fobs);
        r.ib_test_passed = fl && vl;
        CS_LOG("IB test: %s — fence %s (want %u, slot holds %#llx) after %llu us; target dword "
               "%#010x (want %#010x) after %llu us; ring rptr=%u wptr=%u",
               r.ib_test_passed ? "PASSED — the CP executes indirect buffers under VMID 0" : "FAILED",
               fl ? "landed" : "did NOT land", fence, (unsigned long long)fobs,
               (unsigned long long)fus, RVRAM32_via_mm(dev, ibt_off), kIbTestMagic,
               (unsigned long long)vus, cp.rptr_cpu ? *cp.rptr_cpu : 0xFFFFFFFFu, cp.wptr);
        if (!r.ib_test_passed) {
            CS_LOG("=== FAILED before any shader ran: INDIRECT_BUFFER (vmid 0) did not execute. ===");
            log_grbm_status(dev, "after IB test failure");
            gmc_log_vm_faults(dev, gmc);
            r.kr = fl ? kIOReturnIOError : kIOReturnTimeout;
            return r.kr;
        }
    }

    //----------------------------------------------------------------
    // 5. The dispatch IB. Compute-typed PACKET3 headers throughout, CU masks
    //    through SET_SH_REG_INDEX index 3, no COMPUTE_VMID write (privileged;
    //    the VMID now comes from the INDIRECT_BUFFER packet).
    //----------------------------------------------------------------
    const uint32_t pgm_lo = (uint32_t)((code.gpu_va >> 8) & 0xFFFFFFFFu);
    const uint32_t pgm_hi = (uint32_t)((code.gpu_va >> 40) & 0xFFu);  // PGM_HI.DATA is [7:0]
    // s[0:1]: the result buffer itself on the hand-written path, the kernarg
    // buffer that POINTS at it on the LLVM path.
    const uint64_t usr_va = hsaAbi ? karg_va : res.gpu_va;
    const uint32_t usr_lo = (uint32_t)(usr_va & 0xFFFFFFFFu);
    const uint32_t usr_hi = (uint32_t)(usr_va >> 32);

    uint32_t pkt[64];
    uint32_t n = 0;

    CS_LOG("--- PM4 register writes (dispatch IB, compute-typed headers) ---");

    // (a) COMPUTE_PGM_LO/HI — the shader address, >> 8.
    {
        const uint32_t v[2] = { pgm_lo, pgm_hi };
        static const char *const names[2] = { "COMPUTE_PGM_LO", "COMPUTE_PGM_HI" };
        n = emit_set_sh_reg(dev, pkt, n, Reg::COMPUTE_PGM_LO, v, 2, names);
    }
    // (b) RSRC1, RSRC2 (0x1bb2, 0x1bb3). COMPUTE_VMID (0x1bb4) deliberately skipped.
    {
        const uint32_t v[2] = { sh_rsrc1, sh_rsrc2 };
        static const char *const names[2] = { "COMPUTE_PGM_RSRC1", "COMPUTE_PGM_RSRC2" };
        n = emit_set_sh_reg(dev, pkt, n, Reg::COMPUTE_PGM_RSRC1, v, 2, names);
    }
    // (c) RESOURCE_LIMITS (0x1bb5) = 0.
    {
        const uint32_t v[1] = { kResourceLimits };
        static const char *const names[1] = { "COMPUTE_RESOURCE_LIMITS" };
        n = emit_set_sh_reg(dev, pkt, n, Reg::COMPUTE_RESOURCE_LIMITS, v, 1, names);
    }
    // (d) CU masks SE0/SE1 (0x1bb6-7) via SET_SH_REG_INDEX index 3.
    {
        const uint32_t v[2] = { kAllCUsEnabled, kAllCUsEnabled };
        static const char *const names[2] = { "COMPUTE_STATIC_THREAD_MGMT_SE0", "COMPUTE_STATIC_THREAD_MGMT_SE1" };
        n = emit_set_sh_reg_index3(dev, pkt, n, Reg::COMPUTE_STATIC_THREAD_MGMT_SE0, v, 2, names);
    }
    // (e) TMPRING_SIZE (0x1bb8) = 0 — no scratch.
    {
        const uint32_t v[1] = { kTmpringSize };
        static const char *const names[1] = { "COMPUTE_TMPRING_SIZE" };
        n = emit_set_sh_reg(dev, pkt, n, Reg::COMPUTE_TMPRING_SIZE, v, 1, names);
    }
    // (f) CU masks SE2/SE3 (0x1bb9-a) via SET_SH_REG_INDEX index 3.
    {
        const uint32_t v[2] = { kAllCUsEnabled, kAllCUsEnabled };
        static const char *const names[2] = { "COMPUTE_STATIC_THREAD_MGMT_SE2", "COMPUTE_STATIC_THREAD_MGMT_SE3" };
        n = emit_set_sh_reg_index3(dev, pkt, n, Reg::COMPUTE_STATIC_THREAD_MGMT_SE2, v, 2, names);
    }
    // (g) RSRC3 (0x1bc8).
    {
        const uint32_t v[1] = { sh_rsrc3 };
        static const char *const names[1] = { "COMPUTE_PGM_RSRC3" };
        n = emit_set_sh_reg(dev, pkt, n, Reg::COMPUTE_PGM_RSRC3, v, 1, names);
    }
    // (h) Workgroup size: one wave32 (32/1/1).
    {
        const uint32_t v[3] = { kComputeThreadsX, 1u, 1u };
        static const char *const names[3] = {
            "COMPUTE_NUM_THREAD_X", "COMPUTE_NUM_THREAD_Y", "COMPUTE_NUM_THREAD_Z" };
        n = emit_set_sh_reg(dev, pkt, n, Reg::COMPUTE_NUM_THREAD_X, v, 3, names);
    }
    // (i) The kernel argument: s[0:1] = the result buffer's MC address.
    {
        const uint32_t v[2] = { usr_lo, usr_hi };
        static const char *const names[2] = { "COMPUTE_USER_DATA_0", "COMPUTE_USER_DATA_1" };
        n = emit_set_sh_reg(dev, pkt, n, Reg::COMPUTE_USER_DATA_0, v, 2, names);
    }
    // (j) DISPATCH_DIRECT, compute-typed (Mesa: PKT3(PKT3_DISPATCH_DIRECT, 3) |
    //     PKT3_SHADER_TYPE_S(1)): dim 1,1,1 + the initiator.
    pkt[n++] = pm4_type3_cs(kOpDispatchDirect, 3);
    pkt[n++] = 1u;                    // COMPUTE_DIM_X  (workgroups)
    pkt[n++] = 1u;                    // COMPUTE_DIM_Y
    pkt[n++] = 1u;                    // COMPUTE_DIM_Z
    pkt[n++] = kDispatchInitiator;    // COMPUTE_DISPATCH_INITIATOR
    CS_LOG("  DISPATCH_DIRECT(cs)          dim=1,1,1 initiator=%#010x "
           "(COMPUTE_SHADER_EN|FORCE_START_AT_000|ORDER_MODE|CS_W32_EN)",
           kDispatchInitiator);
    // (k) CS_PARTIAL_FLUSH — wait for the dispatch to drain before the IB ends.
    pkt[n++] = pm4_type3(kOpEventWrite, 0);
    pkt[n++] = kEventCsPartialFlush;
    CS_LOG("  EVENT_WRITE                  %#010x (EVENT_TYPE=CS_PARTIAL_FLUSH(7), "
           "EVENT_INDEX=4)", kEventCsPartialFlush);

    r.packet_dwords = n;

    //----------------------------------------------------------------
    // 5b. Submit the dispatch IB and wait.
    //----------------------------------------------------------------
    dump_sh_readback(dev, "before submit");

    const uint32_t wptr_before = cp.wptr;
    uint32_t fence = 0;
    {
        kern_return_t kr = submit_ib(pkt, n, "dispatch IB", &fence);
        if (kr != kIOReturnSuccess) { CS_LOG("dispatch IB: submit failed %#x", kr); r.kr = kr; return kr; }
    }
    r.fence_expected = fence;
    CS_LOG("dispatch submitted: wptr %u -> %u, fence=%u", wptr_before, cp.wptr, fence);

    bool fence_landed = false, data_landed = false;
    uint64_t fence_us = 0, data_us = 0, fence_obs = 0;
    const uint64_t elapsed = wait_for(fence, res_off, sh_magic, &fence_landed, &data_landed,
                                      &fence_us, &data_us, &fence_obs);

    r.fence_landed   = fence_landed;
    r.fence_observed = fence_obs;
    r.elapsed_us     = data_landed ? data_us : (fence_landed ? fence_us : elapsed);

    CS_LOG("wait done: fence %s (want %u, slot holds %#llx) after %llu us; "
           "result[0] %s after %llu us; ring rptr=%u wptr=%u",
           fence_landed ? "LANDED" : "DID NOT LAND", fence,
           (unsigned long long)fence_obs, (unsigned long long)fence_us,
           data_landed ? "appeared" : "did not appear",
           (unsigned long long)data_us,
           cp.rptr_cpu ? *cp.rptr_cpu : 0xFFFFFFFFu, cp.wptr);

    //----------------------------------------------------------------
    // 6. Read back every lane through the GPU's view of VRAM.
    //----------------------------------------------------------------
    uint32_t got[kComputeThreadsX];
    uint32_t mismatched = 0;
    for (uint32_t i = 0; i < kComputeThreadsX; i++) {
        got[i] = RVRAM32_via_mm(dev, res_off + i * 4);
        if (got[i] != sh_magic + i) mismatched++;
    }
    for (uint32_t i = 0; i < 4; i++) r.observed[i] = got[i];
    r.lanes_mismatched = mismatched;

    for (uint32_t i = 0; i < kComputeThreadsX; i += 4) {
        CS_LOG("  result[%02u..%02u] = %08x %08x %08x %08x   (want %08x %08x %08x %08x)",
               i, i + 3, got[i], got[i + 1], got[i + 2], got[i + 3],
               sh_magic + i, sh_magic + i + 1,
               sh_magic + i + 2, sh_magic + i + 3);
    }
    // One dword past the wave must still be poison — proves we are not just
    // reading something that happens to look right.
    CS_LOG("  result[32] = %08x (must still be the poison %#010x)",
           RVRAM32_via_mm(dev, res_off + kComputeThreadsX * 4), kComputePoison);

    dump_sh_readback(dev, "after submit");

    //----------------------------------------------------------------
    // 7. Verdict.
    //----------------------------------------------------------------
    const bool passed = (mismatched == 0);
    r.kr = passed ? kIOReturnSuccess : (fence_landed ? kIOReturnIOError : kIOReturnTimeout);

    if (passed && fence_landed) {
        CS_LOG("=== PASSED — the GPU's shader cores executed our code. "
               "32/32 lanes wrote %#010x+lane into VRAM at mc %#llx in %llu us ===",
               sh_magic, (unsigned long long)res.gpu_va,
               (unsigned long long)r.elapsed_us);
    } else if (passed) {
        CS_LOG("=== PASSED (data) but the EOP FENCE NEVER LANDED. The dispatch ran and "
               "all 32 lanes wrote correctly, so the fault is in RELEASE_MEM, not in the "
               "dispatch — check amdgpu_pm4.h's pm4_header()/GCR bits and the write-back "
               "page at mc %#llx ===", (unsigned long long)cp.fence_gpu_addr);
    } else {
        CS_LOG("=== FAILED — %u of %u lanes wrong. Fence %s. ===",
               mismatched, kComputeThreadsX, fence_landed ? "landed" : "did NOT land");
        if (got[0] == kComputePoison) {
            CS_LOG("diagnosis hint: result[0] is still the poison, so NOTHING was written. "
                   "The IB test passed, so the CP executes IBs; either the dispatch never "
                   "reached the SPI (check the SH readback above), or the wave faulted — "
                   "check the VM fault dump and the decoded IH entries below (a fault IV "
                   "with vmid != 0 means the dispatch is still not bound to VMID 0; one at "
                   "the result address means the store path).");
        } else {
            CS_LOG("diagnosis hint: result[0] changed from the poison, so the shader DID "
                   "run — the values are wrong, which points at the RSRC encodings, the "
                   "wave mode (CS_W32_EN vs the shader's wave32), or the work-item id.");
        }
        log_grbm_status(dev, "after the dispatch failure");
        gmc_log_vm_faults(dev, gmc);
    }

    CS_LOG("summary: kr=%#x ib_test=%d fence_landed=%d elapsed=%llu us ib_mc=%#llx code_mc=%#llx result_mc=%#llx "
           "observed=%08x %08x %08x %08x expected=%08x %08x %08x %08x",
           r.kr, r.ib_test_passed ? 1 : 0, r.fence_landed, (unsigned long long)r.elapsed_us,
           (unsigned long long)r.ib_gpu_va,
           (unsigned long long)r.code_gpu_va, (unsigned long long)r.result_gpu_va,
           r.observed[0], r.observed[1], r.observed[2], r.observed[3],
           r.expected[0], r.expected[1], r.expected[2], r.expected[3]);

    return r.kr;
}

//====================================================================
// NATIVE S1b (0.0.600, navi48-native=1): the same dispatch, built for a VMID other than 0.
//
// compute_dispatch_test above is NOT changed. These two functions only expose its blob and its packet stream so the native
// self-test can put the dispatch IB, the shader and the result page behind a user VMID's page table and prove the shader FETCH
// and the store through it. Same registers, same values, same order as steps 5 (a)-(k) of compute_dispatch_test with the
// hand-written shader (hsaAbi = false); the only inputs are two virtual addresses. No hardware access: the caller stages the
// bytes and submits.
//====================================================================
const uint8_t *compute_store_magic_code(uint32_t *size_out)
{
    if (size_out) *size_out = (uint32_t)fw_shader_store_magic_size;
    return fw_shader_store_magic;
}

uint32_t compute_build_va_dispatch_ib(const DeviceContext &dev, uint32_t *pkt, uint32_t max_dw,
                                      uint64_t code_va, uint64_t res_va)
{
    if (pkt == nullptr || max_dw < 64) return 0;
    if (gc0(dev, Reg::COMPUTE_DISPATCH_INITIATOR) < kPacket3SetShRegStart) return 0;   // the guard compute_dispatch_test applies
    uint32_t n = 0;
    {   // (a) PGM_LO/HI: the shader address >> 8 (PGM_HI is [7:0] = VA bits 47:40)
        const uint32_t v[2] = { (uint32_t)((code_va >> 8) & 0xFFFFFFFFu), (uint32_t)((code_va >> 40) & 0xFFu) };
        static const char *const names[2] = { "COMPUTE_PGM_LO", "COMPUTE_PGM_HI" };
        n = emit_set_sh_reg(dev, pkt, n, Reg::COMPUTE_PGM_LO, v, 2, names);
    }
    {   // (b) RSRC1, RSRC2
        const uint32_t v[2] = { kComputePgmRsrc1, kComputePgmRsrc2 };
        static const char *const names[2] = { "COMPUTE_PGM_RSRC1", "COMPUTE_PGM_RSRC2" };
        n = emit_set_sh_reg(dev, pkt, n, Reg::COMPUTE_PGM_RSRC1, v, 2, names);
    }
    {   // (c) RESOURCE_LIMITS
        const uint32_t v[1] = { kResourceLimits };
        static const char *const names[1] = { "COMPUTE_RESOURCE_LIMITS" };
        n = emit_set_sh_reg(dev, pkt, n, Reg::COMPUTE_RESOURCE_LIMITS, v, 1, names);
    }
    {   // (d) CU masks SE0/SE1, index 3
        const uint32_t v[2] = { kAllCUsEnabled, kAllCUsEnabled };
        static const char *const names[2] = { "COMPUTE_STATIC_THREAD_MGMT_SE0", "COMPUTE_STATIC_THREAD_MGMT_SE1" };
        n = emit_set_sh_reg_index3(dev, pkt, n, Reg::COMPUTE_STATIC_THREAD_MGMT_SE0, v, 2, names);
    }
    {   // (e) TMPRING_SIZE
        const uint32_t v[1] = { kTmpringSize };
        static const char *const names[1] = { "COMPUTE_TMPRING_SIZE" };
        n = emit_set_sh_reg(dev, pkt, n, Reg::COMPUTE_TMPRING_SIZE, v, 1, names);
    }
    {   // (f) CU masks SE2/SE3, index 3
        const uint32_t v[2] = { kAllCUsEnabled, kAllCUsEnabled };
        static const char *const names[2] = { "COMPUTE_STATIC_THREAD_MGMT_SE2", "COMPUTE_STATIC_THREAD_MGMT_SE3" };
        n = emit_set_sh_reg_index3(dev, pkt, n, Reg::COMPUTE_STATIC_THREAD_MGMT_SE2, v, 2, names);
    }
    {   // (g) RSRC3
        const uint32_t v[1] = { kComputePgmRsrc3 };
        static const char *const names[1] = { "COMPUTE_PGM_RSRC3" };
        n = emit_set_sh_reg(dev, pkt, n, Reg::COMPUTE_PGM_RSRC3, v, 1, names);
    }
    {   // (h) one wave32
        const uint32_t v[3] = { kComputeThreadsX, 1u, 1u };
        static const char *const names[3] = { "COMPUTE_NUM_THREAD_X", "COMPUTE_NUM_THREAD_Y", "COMPUTE_NUM_THREAD_Z" };
        n = emit_set_sh_reg(dev, pkt, n, Reg::COMPUTE_NUM_THREAD_X, v, 3, names);
    }
    {   // (i) s[0:1] = the result buffer's VIRTUAL address
        const uint32_t v[2] = { (uint32_t)(res_va & 0xFFFFFFFFu), (uint32_t)(res_va >> 32) };
        static const char *const names[2] = { "COMPUTE_USER_DATA_0", "COMPUTE_USER_DATA_1" };
        n = emit_set_sh_reg(dev, pkt, n, Reg::COMPUTE_USER_DATA_0, v, 2, names);
    }
    pkt[n++] = pm4_type3_cs(kOpDispatchDirect, 3);   // (j)
    pkt[n++] = 1u; pkt[n++] = 1u; pkt[n++] = 1u;
    pkt[n++] = kDispatchInitiator;
    pkt[n++] = pm4_type3(kOpEventWrite, 0);          // (k) CS_PARTIAL_FLUSH
    pkt[n++] = kEventCsPartialFlush;
    return n;
}

} // namespace amdgpu
