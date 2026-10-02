// ws_valid.h — THE VALID BIT OF WINDOWSERVER'S PAGE-TABLE BASE, SET ONLY AT COMMIT (M4-WS-VMID-VALID, notes/M4-WS-VMID-VALID.md).
// Pure C, host-tested by tests/ws_valid_test.cpp (with planted defects); the kext compiles the SAME header.
//
// THE GAP (hp4, ; project notes). GCVM_CONTEXTn_PAGE_TABLE_BASE_ADDR_LO32/HI32 hold a whole PDE:
// the registers' only field is PAGE_DIRECTORY_ENTRY_LO32/HI32 (ref/linux-asic-reg/gc_12_0_0_sh_mask.h:10722-10726, CONTEXT3), and
// upstream writes `root | AMDGPU_PTE_VALID` (amdgpu_gmc_pd_addr; AMDGPU_PTE_VALID = 1 << 0, ref/linux-amdgpu/amdgpu_vm.h:57).
// Apple writes the BARE base; the SDMA drain ORs bit 0 into CONTEXT2's only (sdma_drain_xlat.h pass 2, 0.0.193). WindowServer
// landed on VMID 3 in hp4 and its base read `0x3:0xd6c0a000`, bit 0 CLEAR. If gfx12 checks bit 0, a committed WindowServer IB
// cannot be fetched (the CP parks AT the packet, as in). SUSPECTED, never isolated ( changed three things at once).
//
// THE RULE, AND ITS SCOPE. Bit 0 is set on ONE base only: the one of WindowServer's RECORDED VMID (ws_ident.h), and only while
// COMMIT is armed. Every other VMID keeps Apple's bare base on purpose - it is an accidental second safety net: an IB that escaped
// the neuter parks on its fetch instead of running gfx10 writes (the review). CONTEXT2's 0.0.193 rule is HEAD behaviour and stays.
//   n48_wsv_decide     the direct write (the kext's frame path, under the ws_ident lock and the SDMA drain lock)
//   n48_wsv_readback   the write is accepted only if LO reads back as written and HI is untouched
//   n48_wsv_keep_applies  the SDMA drain's keep-rule: a later Apple re-program of THAT VMID to THE bound root keeps bit 0
//   n48_wsv_base_valid the committed-frame exemption's new condition (gfx_neuter.h, N48_GFXN_EX_BASE)
// Nothing here touches hardware; every function is total over its inputs.
#ifndef N48_WS_VALID_H
#define N48_WS_VALID_H

#include <stdint.h>
#include "ws_ident.h"

#define N48_WSV_ARM_COMMIT  2u            /* == gfx_src_decide.h N48_SD_ARM_COMMIT; AppleHardwareHook.cpp static_asserts it */
#define N48_WSV_CNTL        0x03fffd73u   /* Apple's geometry on every client context (hp3/hp4: 16 of 16), rule 32 */
#define N48_WSV_VALID_BIT   1u            /* AMDGPU_PTE_VALID */

/* What the direct write decides, in the order it is checked. */
enum {
    N48_WSV_INERT = 0,      /* COMMIT not armed: nothing is evaluated, nothing is written (every run of this build) */
    N48_WSV_WRITE,          /* every condition holds and bit 0 is clear: write LO | 1, HI untouched */
    N48_WSV_ALREADY,        /* every condition holds and bit 0 is already set: nothing to write */
    N48_WSV_NOT_BOUND,      /* WindowServer is not BOUND by owner (ws_ident.h) */
    N48_WSV_NOT_JUDGED,     /* this frame was not judged WindowServer's */
    N48_WSV_VMID,           /* the frame's VMID is 0, >15, unrecorded, or not the one recorded for the binding */
    N48_WSV_CNTL_BAD,       /* that VMID's CNTL is not Apple's proven 0x03fffd73 */
    N48_WSV_ROOT,           /* HI:LO does not name the bound root (or the root is 0) */
    N48_WSV_SHAPE,          /* HI:LO carries bits other than the root and bit 0: not a base Apple wrote - never touched */
    N48_WSV_BUSY,           /* the SDMA drain lock was not free, or an SDMA ring that carried a base write is not proven idle */
    N48_WSV_REASONS
};

typedef struct {
    uint32_t arm;           /* the COMMIT arm level, read once (gXdArm) */
    uint32_t ws_state;      /* N48_WS_* */
    uint32_t ws_vmid;       /* the VMID recorded for the binding (0 = none yet) */
    uint64_t ws_root;       /* the bound root (masked) */
    uint32_t frame_vmid;    /* the submission's VMID field */
    uint32_t verdict;       /* N48_WSF_* of this frame */
    uint32_t cntl;          /* GCVM_CONTEXT<frame_vmid>_CNTL, read now */
    uint32_t lo, hi;        /* GCVM_CONTEXT<frame_vmid>_PAGE_TABLE_BASE_ADDR_LO32 / HI32, read now */
    uint32_t sdma_idle;     /* 1 only when the caller holds the SDMA drain lock and proved every base-writing ring consumed */
} n48_wsv_in;

static inline uint64_t n48_wsv_raw(uint32_t lo, uint32_t hi) { return ((uint64_t)hi << 32) | lo; }

/* The base is exactly `root`, optionally with bit 0: no other bit of HI:LO is set. */
static inline uint32_t n48_wsv_shape_ok(uint32_t lo, uint32_t hi)
{
    return (n48_wsv_raw(lo, hi) & ~(N48_WS_ROOT_MASK | (uint64_t)N48_WSV_VALID_BIT)) == 0u ? 1u : 0u;
}

static inline uint32_t n48_wsv_decide(const n48_wsv_in *in, uint32_t *new_lo)
{
    if (new_lo) *new_lo = in ? in->lo : 0u;
    if (!in || in->arm != N48_WSV_ARM_COMMIT) return N48_WSV_INERT;
    if (in->ws_state != N48_WS_BOUND) return N48_WSV_NOT_BOUND;
    if (in->verdict != N48_WSF_JUDGE) return N48_WSV_NOT_JUDGED;
    if (in->frame_vmid == 0u || in->frame_vmid > 15u || in->ws_vmid == 0u || in->frame_vmid != in->ws_vmid) return N48_WSV_VMID;
    if (in->cntl != N48_WSV_CNTL) return N48_WSV_CNTL_BAD;
    if (in->ws_root == 0u || (in->ws_root & ~N48_WS_ROOT_MASK) != 0u ||
        (n48_wsv_raw(in->lo, in->hi) & N48_WS_ROOT_MASK) != in->ws_root)
        return N48_WSV_ROOT;
    if (!n48_wsv_shape_ok(in->lo, in->hi)) return N48_WSV_SHAPE;
    if (in->lo & N48_WSV_VALID_BIT) return N48_WSV_ALREADY;
    if (in->sdma_idle != 1u) return N48_WSV_BUSY;
    if (new_lo) *new_lo = in->lo | N48_WSV_VALID_BIT;
    return N48_WSV_WRITE;
}

/* The write stands only if LO reads back exactly as written and HI reads back as it was before the write. */
static inline uint32_t n48_wsv_readback(uint32_t written_lo, uint32_t hi_before, uint32_t rd_lo, uint32_t rd_hi)
{
    return (rd_lo == written_lo && rd_hi == hi_before) ? 1u : 0u;
}

/* The drain's keep-rule. `armed` is 1 only while COMMIT is armed AND WindowServer is BOUND with a RECORDED VMID; the kext takes it
 * under the ws_ident lock at each SDMA submission (never waiting: a busy lock means armed 0, i.e. HEAD's output). */
typedef struct {
    uint32_t armed;
    uint32_t vmid;          /* WindowServer's recorded VMID */
    uint64_t root;          /* the bound root (masked) */
} n48_wsv_keep;

/* A translated REG_WRITE of `lo` to CONTEXT<ctx>_PAGE_TABLE_BASE_ADDR_LO32, whose HI32 is written later in the SAME IB as `hi`
 * (hi_found 0: no HI write follows): keep bit 0 only if it is WindowServer's recorded VMID and HI:LO is exactly the bound root. */
static inline uint32_t n48_wsv_keep_applies(const n48_wsv_keep *k, uint32_t ctx, uint32_t lo, uint32_t hi_found, uint32_t hi)
{
    if (!k || k->armed != 1u) return 0u;
    if (k->vmid == 0u || k->vmid > 15u || ctx != k->vmid) return 0u;
    if (hi_found != 1u) return 0u;
    if (k->root == 0u || (k->root & ~N48_WS_ROOT_MASK) != 0u) return 0u;
    if ((n48_wsv_raw(lo, hi) & N48_WS_ROOT_MASK) != k->root) return 0u;
    return n48_wsv_shape_ok(lo, hi);
}

/* The committed-frame exemption's condition: the base of the token's VMID, read at the walk, has bit 0 set, names the bound
 * root, carries nothing else, and no validation of it is left with an unacknowledged TLB invalidate. */
static inline uint32_t n48_wsv_base_valid(uint32_t lo, uint32_t hi, uint64_t ws_root, uint32_t flush_unacked)
{
    if (flush_unacked) return 0u;
    if (!(lo & N48_WSV_VALID_BIT)) return 0u;
    if (ws_root == 0u || (n48_wsv_raw(lo, hi) & N48_WS_ROOT_MASK) != ws_root) return 0u;
    return n48_wsv_shape_ok(lo, hi);
}

static inline const char *n48_wsv_name(uint32_t r)
{
    static const char *const n[N48_WSV_REASONS] = {
        "inert (COMMIT not armed)", "WRITE", "already valid", "refused: WindowServer not bound",
        "refused: frame not judged WindowServer's", "refused: VMID not the recorded one", "refused: CNTL is not 0x03fffd73",
        "refused: base does not name the bound root", "refused: base carries other bits",
        "refused: SDMA not proven idle (drain lock busy or a base-writing ring not consumed)" };
    return r < N48_WSV_REASONS ? n[r] : "?";
}

#endif
