// sdma_gcr.h — the SDMA GCR_REQ "graphics cache rinse" packet, and the pure arithmetic and refusal
// rules 0.0.416 (notes/design/SDMA-GCR.md;) drives it with. No kernel headers, no hardware:
// the kext (Navi48Bringup.cpp, DisplayPipeGuard.cpp), the host test (tests/sdma_gcr_test.cpp) and the
// PC test client (tools/pc/navi48test.c) compile this same file, so there is no second implementation
// that could drift from the one that runs.
//
// WHY THIS EXISTS. st6 proved the SDMA detile correct on the live geometry with our own buffers;
// on Apple's CB-written plane the MM window reads green where the SDMA copy delivers black. showed
// nothing in our SDMA path writes back or invalidates GL2, while Linux emits an SDMA GCR_REQ with
// GL2_WB|GL2_INV at the start of every SDMA IB (ref/linux-amdgpu/sdma_v7_0.c:296-317,
// sdma_v7_0_ring_emit_mem_sync). This is that packet.
//
// THE PACKET, verbatim from upstream Linux (read 2026-09-22 from
// https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h,
// the header sdma_v7_0.c includes). Names as upstream:
//   #define SDMA_OP_GCR_REQ  17
//   SDMA_PKT_GCR_REQ_HEADER op: mask 0xFF shift 0; sub_op: mask 0xFF shift 8
//   PAYLOAD1 (dw1) base_va_31_7:  mask 0x01FFFFFF shift 7
//   PAYLOAD2 (dw2) base_va_47_32: mask 0xFFFF shift 0;  gcr_control_15_0: mask 0xFFFF shift 16
//   PAYLOAD3 (dw3) gcr_control_18_16: mask 0x7 shift 0; limit_va_31_7:   mask 0x01FFFFFF shift 7
//   PAYLOAD4 (dw4) limit_va_47_32: mask 0xFFFF shift 0; vmid: mask 0xF shift 24
// Linux's gcr_cntl = GL2_INV | GL2_WB | GLM_INV | GL1_INV | GLV_INV | GLK_INV | GLI_INV(1) = 0xC3A1,
// base = limit = 0, VMID 0. The five dwords are therefore 0x00000011, 0x00000000, 0xC3A10000,
// 0x00000000, 0x00000000 — and tests/sdma_gcr_test.cpp asserts exactly those.
//
// NOTE this header deliberately OWNS the opcode and the field definitions: amdgpu_sdma.h does not
// duplicate them, because it cannot be compiled on the host and these must be host-testable. Nothing
// else emits the packet.
#ifndef N48_SDMA_GCR_H
#define N48_SDMA_GCR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------
// G1 — the constants (upstream names) and the ONE emitter.
// ---------------------------------------------------------------------------------------------------
#define SDMA_OP_GCR_REQ        17
#define SDMA_GCR_RANGE_IS_PA   (1 << 18)
#define SDMA_GCR_SEQ(x)        (((x) & 0x3) << 16)
#define SDMA_GCR_GL2_WB        (1 << 15)
#define SDMA_GCR_GL2_INV       (1 << 14)
#define SDMA_GCR_GL2_DISCARD   (1 << 13)
#define SDMA_GCR_GL2_RANGE(x)  (((x) & 0x3) << 11)
#define SDMA_GCR_GL2_US        (1 << 10)
#define SDMA_GCR_GL1_INV       (1 << 9)
#define SDMA_GCR_GLV_INV       (1 << 8)
#define SDMA_GCR_GLK_INV       (1 << 7)
#define SDMA_GCR_GLK_WB        (1 << 6)
#define SDMA_GCR_GLM_INV       (1 << 5)
#define SDMA_GCR_GLM_WB        (1 << 4)
#define SDMA_GCR_GL1_RANGE(x)  (((x) & 0x3) << 2)
#define SDMA_GCR_GLI_INV(x)    (((x) & 0x3) << 0)

// Linux's gcr_cntl for the memory sync it emits before every SDMA IB.
#define N48_SDMA_GCR_CNTL      (SDMA_GCR_GL2_WB | SDMA_GCR_GL2_INV | SDMA_GCR_GLM_INV | \
                                SDMA_GCR_GL1_INV | SDMA_GCR_GLV_INV | SDMA_GCR_GLK_INV | \
                                SDMA_GCR_GLI_INV(1))

#define N48_SDMA_GCR_REQ_DWORDS 5u

// The ONE emitter. Writes the five dwords with Linux's gcr_cntl (base = limit = 0, VMID 0) and
// returns the dword count. Nothing else in the tree builds these dwords.
static inline uint32_t n48_sdma_gcr_req(uint32_t *pkt)
{
    pkt[0] = (uint32_t)SDMA_OP_GCR_REQ;                                   // op 17, sub_op 0 -> 0x00000011
    pkt[1] = 0;                                                           // base_va_31_7 = 0 -> 0x00000000
    pkt[2] = (uint32_t)((N48_SDMA_GCR_CNTL & 0xffffu) << 16);             // gcr_control_15_0 -> 0xC3A10000
    pkt[3] = 0;                                                           // control_18_16 | limit_va_31_7
    pkt[4] = 0;                                                           // limit_va_47_32 | vmid
    return N48_SDMA_GCR_REQ_DWORDS;
}

// ---------------------------------------------------------------------------------------------------
// G3 — the ring-space arithmetic. The tiled copy is ONE 14-dword COPY_TILED_SUB_WINDOW plus a 4-dword
// FENCE (18 dwords, which is the "ring advancing 18 dwords per copy" st6's S2 log showed); with the
// GCR_REQ in front it is 23. The linear copy is 8 + 4 = 12, or 17 with the GCR_REQ. The kext's
// scanout_sdma_tiled_copy / scanout_sdma_copy_linear_gcr size their packet buffers and their
// `k != dwords` checks from these, and a host test pins both numbers.
// ---------------------------------------------------------------------------------------------------
#define N48_SDMA_TILED_SUB_WINDOW_DWORDS 14u
#define N48_SDMA_COPY_LINEAR_DWORDS      8u
#define N48_SDMA_FENCE_DWORDS            4u

static inline uint32_t n48_sdma_tiled_copy_dwords(int gcr)
{
    return N48_SDMA_TILED_SUB_WINDOW_DWORDS + N48_SDMA_FENCE_DWORDS +
           (gcr ? N48_SDMA_GCR_REQ_DWORDS : 0u);
}
static inline uint32_t n48_sdma_linear_copy_dwords(int gcr)
{
    return N48_SDMA_COPY_LINEAR_DWORDS + N48_SDMA_FENCE_DWORDS +
           (gcr ? N48_SDMA_GCR_REQ_DWORDS : 0u);
}

// ---------------------------------------------------------------------------------------------------
// G2 — the scalar the `accel scanout 7` verb carries, and the source-window refusal rules.
//
// navi48_scanout_control receives ONE 64-bit scalar over the ABI, so mode 7 packs the mode, the
// optional GCR flag and the 4-KiB-aligned VRAM offset into it:
//     bits 0..6   mode (7)
//     bit  7      the GCR flag
//     bits 8..11  unused (the offset is 4-KiB aligned, so its low 12 bits are zero)
//     bits 12..63 the source VRAM offset
// Modes 0..6 keep their scalar = the mode, so every pre-0.0.416 call is byte-identical.
// ---------------------------------------------------------------------------------------------------
#define N48_SCANOUT_MODE_MASK 0x7fu
#define N48_SCANOUT_GCR_FLAG  0x80u

// D2 (0.0.417): the return type is uint64_t. The ABI carries ONE 64-bit scalar, and the
// 0.0.416 uint32_t returned a truncated offset, so the CLI could not send an offset >= 4 GiB. Modes 0..6
// are unaffected (their scalar is the mode); a host test pins an offset above 4 GiB.
static inline uint64_t n48_scanout7_scalar(uint64_t vramOff, int gcr)
{
    return (vramOff & ~0xfffull) | 7ull | (gcr ? (uint64_t)N48_SCANOUT_GCR_FLAG : 0ull);
}
static inline uint64_t n48_scanout7_off(uint64_t arg)
{
    return arg & ~0xfffull;
}
static inline int n48_scanout7_gcr(uint64_t arg)
{
    return (arg & N48_SCANOUT_GCR_FLAG) != 0;
}

// G2's window refusals (0 = usable):
enum {
    kN48GcrSrcOk      = 0,
    kN48GcrSrcAlign   = 1,   // the source offset is not 4 KiB aligned
    kN48GcrSrcRange   = 2,   // empty, or the window leaves the card's VRAM
    kN48GcrSrcOverlap = 3,   // the window overlaps one of our own allocator pools
};

// `vramOff`/`bytes` is the window to read; `vramSize` the card's VRAM bytes; the two `alloc*` pairs
// are our own pools. A zero-size pool is ignored. Refuses rather than clamp: the caller names the
// reason. This is the same refusal discipline as n48_scanout_geom_check in scanout_copy.h.
static inline uint32_t n48_gcr_src_reason(uint64_t vramOff, uint64_t bytes, uint64_t vramSize,
                                          uint64_t allocBase, uint64_t allocSize,
                                          uint64_t allocHiBase, uint64_t allocHiSize)
{
    if (bytes == 0) return kN48GcrSrcRange;
    if (vramOff & 0xfffull) return kN48GcrSrcAlign;
    if (vramOff > vramSize || bytes > vramSize - vramOff) return kN48GcrSrcRange;
    if (allocSize && vramOff < allocBase + allocSize && allocBase < vramOff + bytes)
        return kN48GcrSrcOverlap;
    if (allocHiSize && vramOff < allocHiBase + allocHiSize && allocHiBase < vramOff + bytes)
        return kN48GcrSrcOverlap;
    return kN48GcrSrcOk;
}

#ifdef __cplusplus
}
#endif

#endif // N48_SDMA_GCR_H
