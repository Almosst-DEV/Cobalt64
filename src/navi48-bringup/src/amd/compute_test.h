//
//  compute_test.h — Phase 3's finish line: one compute dispatch on the
//  Radeon RX 9070 XT (Navi 48, GC 12.0.1 / gfx1201).
//
//  A shader we wrote and assembled ourselves (shaders/store_magic.s, embedded
//  as src/fw/fw_shader_store_magic.c) is placed in VRAM, dispatched as PM4
//  through the already-working CP GFX ring (CPContext — the same ring the
//  stage-16 WRITE_DATA test uses), and the values it wrote are read back
//  through the GPU's own view of VRAM. When this passes, the shader cores of
//  the GPU have executed our code.
//
//  This is NOT a port of a mac-amdgpu module — the reference never got this
//  far (its frontier was the first PM4 submission). The PM4 stream mirrors
//  what Linux amdgpu does for a ring-side compute dispatch
//  (gfx_v8_0_do_edc_gpr_workarounds / gfx_v9_0_do_edc_gpr_workarounds: a run
//  of PACKET3_SET_SH_REG writes to the COMPUTE_* SH registers, then
//  PACKET3_DISPATCH_DIRECT, then an EVENT_WRITE CS_PARTIAL_FLUSH) and what
//  Mesa's radeonsi does when it dispatches compute on the graphics queue
//  (si_emit_dispatch_packets). Every register offset and field mask is taken
//  from ref/linux-asic-reg/gc_12_0_0_offset.h and gc_12_0_0_sh_mask.h — not
//  from the reference, which addressed some GC registers at the wrong
//  BASE_IDX (see amdgpu_cp.h's deviation 6).
//
//  Submit / poll / readback deliberately reuse the proven stage-16 code path:
//      cp_ring_write() -> cp_emit_eop_fence() -> cp_kick_doorbell()
//      -> poll *cp.fence_cpu with sysmem_rmb()
//      -> RVRAM32_via_mm() for the GPU-side view of the result.
//
#pragma once

#include <stdint.h>
#include <IOKit/IOReturn.h>

#include "amdgpu_regs.h"

namespace amdgpu {

struct GMCContext;
struct CPContext;

// The constant lane 0 of the shader writes. "called GPU" — lane i writes
// kComputeMagic + i, so the whole 32-lane wave leaves a checkable ramp.
constexpr uint32_t kComputeMagic = 0xCA11ED47u;

// The LLVM-compiled shader writes a DIFFERENT constant, because it is a
// different shader: shaders/store_magic_hsa.s line 34,
//
//     v_add_nc_u32_e32 v1, 0xca6aed47, v0
//
// against shaders/store_magic.s line 63,
//
//     v_add_nc_u32  v2, 0xca11ed47, v0
//
// This cost a hardware run. The hsaAbi switch selected the shader, the RSRC
// words and the kernarg page, but not the magic — so the dispatch worked
// perfectly, all 32 lanes wrote 0xca6aed47 + lane, and the checker compared
// them against 0xca11ed47 + lane and reported "32 of 32 lanes wrong". The
// diagnosis printed alongside it ("points at the RSRC encodings, the wave mode,
// or the work-item id") was confidently wrong in three directions at once.
//
// If a third shader variant is ever added, it needs an entry here too. Better:
// have tools/build-shader.sh extract the literal from the .s and emit it beside
// the firmware bytes, so the two cannot drift at all.
constexpr uint32_t kComputeMagicHsa = 0xCA6AED47u;

// Poison written into the result buffer before the dispatch, so "the shader
// never ran" is distinguishable from "the shader wrote zeros".
constexpr uint32_t kComputePoison = 0xDEADBEEFu;

// One wave32, one workgroup: COMPUTE_NUM_THREAD_X/Y/Z = 32/1/1 and
// DISPATCH_DIRECT 1,1,1. Every lane stores one dword.
constexpr uint32_t kComputeThreadsX = 32;

struct ComputeTestResult {
    // ---- the set the ladder reports -------------------------------------
    kern_return_t kr;              // overall verdict
    bool          fence_landed;    // did the EOP fence value materialise?
    uint64_t      elapsed_us;      // doorbell kick -> fence observed
    uint32_t      observed[4];     // first four result dwords, read via MM window
    uint32_t      expected[4];     // kComputeMagic + 0..3
    uint64_t      code_gpu_va;     // MC address of the shader blob in VRAM
    uint64_t      result_gpu_va;   // MC address of the result buffer in VRAM
    uint64_t      ib_gpu_va;       // MC address of the indirect buffer in VRAM
    bool          ib_test_passed;  // WRITE_DATA-in-an-IB (vmid 0) landed before the dispatch

    // ---- extra diagnostics (cheap, and the log is all we get on HW) -----
    uint32_t      fence_expected;      // value cp_emit_eop_fence promised
    uint64_t      fence_observed;      // last value seen at *cp.fence_cpu
    uint32_t      lanes_checked;       // kComputeThreadsX
    uint32_t      lanes_mismatched;    // 0 == every lane wrote what it should
    uint32_t      dispatch_initiator;  // the COMPUTE_DISPATCH_INITIATOR we sent
    uint32_t      packet_dwords;       // PM4 dwords staged (before the fence)
    uint32_t      shader_bytes;        // size of the embedded blob
};

//
// Run the dispatch. Requires the ladder to have reached at least CPInit
// (stage 12) with a working GFX ring — in practice run it after stage 16
// (PM4Test), which proves the same ring end to end with WRITE_DATA.
//
// Preconditions checked and logged rather than assumed:
//   cp.inited, cp.fence_cpu, dev.bar2 (doorbell), dev.bar0 (VRAM aperture),
//   gmc.inited + gmc.vram_alloc initialised, GC IP base resolved.
//
// Allocates two VRAM buffers from gmc.vram_alloc (shader code, 16 KiB
// granule; result page, 4 KiB requested) and deliberately does NOT free
// them: if the dispatch timed out the GPU may still write into them later,
// and this allocator is bring-up scratch that is never reclaimed anyway.
//
// `out` is always fully filled in (including on early failure) when non-null.
// The return value is the same as out->kr.
//
// hsaAbi selects which shader is dispatched, and therefore which kernel-argument
// convention the dispatch must set up:
//
//   false (default) — shaders/store_magic.s, hand-written. s[0:1] IS the result
//                     buffer's address. This is the proven stage-17 path and is
//                     byte-for-byte unchanged by the HSA work.
//   true            — shaders/store_magic_hsa.s, compiled by LLVM from
//                     shaders/llvm/lean.ll. s[0:1] is a POINTER to a kernarg
//                     buffer whose first 8 bytes hold the result address, which
//                     is the standard convention and the only one that
//                     generalises past a single argument. Selected by the
//                     boot-arg navi48-hsa-abi=1. See notes/B1-COMPILER-BRIDGE.md.
kern_return_t compute_dispatch_test(DeviceContext &dev,
                                    GMCContext &gmc,
                                    CPContext &cp,
                                    ComputeTestResult *out,
                                    bool hsaAbi = false);

// NATIVE S1b (0.0.600): the hand-written shader's bytes, and compute_dispatch_test's PM4 stream (steps 5 (a)-(k), hsaAbi = false)
// for a caller-chosen code VA and result VA. `pkt` needs room for 64 dwords; the return is the dword count (0 = refused). No
// hardware access; the existing compute_dispatch_test is untouched.
const uint8_t *compute_store_magic_code(uint32_t *size_out);
uint32_t compute_build_va_dispatch_ib(const DeviceContext &dev, uint32_t *pkt, uint32_t max_dw,
                                      uint64_t code_va, uint64_t res_va);

} // namespace amdgpu
