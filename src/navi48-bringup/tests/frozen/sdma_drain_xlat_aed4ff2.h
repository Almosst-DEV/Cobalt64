// FROZEN COPY of src/navi48-bringup/src/apple/sdma_drain_xlat.h at aed4ff2 (git show aed4ff2:<path>), for tests/ws_valid_test.cpp's
// inert-without-COMMIT proof. Only the include guard is renamed; included inside namespace head { }. Do not edit.
// sdma_drain_xlat.h — the SDMA packet translation Apple's page-table stream needs on gfx1201, as PURE
// functions over a buffer (no kernel types, no globals), so tests/sdma_drain_xlat_test.cpp compiles the
// IDENTICAL code the kext runs.
//
// 0.0.269: MOVED here verbatim from AppleHardwareHook.cpp - the PTEPDE entry re-encoding
// (0.0.193,) and the GCVM register renumbering guard - so the xlatregs verb and the new drain
// share ONE implementation. Only `static` became `static inline`. Then NEW: the drain's single-pass IB translation
// and its ring-frame walk.
//
// WHY THE DRAIN EXISTS: `sdmamap` makes Apple's SDMA rings run on hardware, but `xlatregs` and
// `neuterpoll` translate only the IBs present when they run, and both refuse a whole IB on an opcode their
// table lacks. Every page-table IB Apple writes after its startup batch carries opcode 16 (GPUVM_INV), and a new
// client's VM-program frame carries a register-form POLL_REGMEM with retry count 0xfff that waits forever on a
// gfx10-numbered register. So each submission is translated here before its doorbell.

#ifndef NAVI48_SDMA_DRAIN_XLAT_AED4FF2_H
#define NAVI48_SDMA_DRAIN_XLAT_AED4FF2_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// 0.0.193 ( + the the reviewer pass) — THE PTEPDE ENTRY ENCODING.
//
// Apple's AMDGFX10VMM writes gfx10-format page-table entries:
//   PDE: BFS at 63:59, TRANSLATE_FURTHER at 56, no P bit
//   PTE: MTYPE at 49:48, PDE_PTE at 54, NOALLOC at 58, no IS_PTE
// gfx12 (gmc_v12_0.c:456-484, amdgpu_vm.h:121-141) puts the SAME ideas
// somewhere else:
//   PDE: P at 63, BFS at 62:58, A at 56, MTYPE at 55:54, no TF at all
//   PTE: P at 63 (this entry IS a leaf), D 58, G 57, T 56, MTYPE 55:54
// Read by a gfx12 walker, Apple's root PDE's bit 61 lands in BFS (4 -> 8) and
// every leaf has P = 0, so the walker treats leaves as directories. That is
// what the walk into Apple's arena at 0x3d6c00000 hits today.
//
// Only the FLAGS dwords of a PTEPDE packet are rewritten; the address dwords,
// the increment and the count are Apple's and stay Apple's. Every rewrite is
// keyed on a value only an unconverted Apple entry can carry, so a second pass
// over the same IB is a no-op even if the per-IB record were lost.
static constexpr uint32_t kPteHiRootG10  = 0x20000000u; // BFS 4 at 63:59 (gfx10)
static constexpr uint32_t kPteHiRootG12  = 0x10000000u; // BFS 4 at 62:58 (gfx12)
static constexpr uint32_t kPteHiTfG10    = 0x01000000u; // TRANSLATE_FURTHER, bit 56
static constexpr uint32_t kPteHiIsPte    = 0x80000000u; // gfx12 P, bit 63
static constexpr uint32_t kPteLoRWX      = 0x00000070u; // EXEC|READ|WRITE, bits 4-6
// bits 48,49 (gfx10 MTYPE), 50 (reserved on gfx12) and 54 (gfx10 PDE_PTE, which
// is gfx12's MTYPE low bit) — every bit the gfx10 encoding uses in that region.
static constexpr uint32_t kPteHiG10MtypeMask = 0x00470000u;
static constexpr uint32_t kPteHiG12MtypeShift = 22;     // hi bit 22 = entry bit 54
// Apple's gfx10 MTYPE -> the gfx12 MTYPE this driver has PROVEN on this
// silicon. DELIBERATE CHOICE, the one place this hook does not copy Apple:
// Apple's UC is gfx10 MTYPE 3, but gfx12's MTYPE field is two bits wide
// (AMDGPU_PTE_MTYPE_GFX12_MASK = 3ULL << 54) and every PTE our GART writes uses
// PTEFlags::MTYPE_GFX12_UC = 2 << 54 (amdgpu_ip.h:324) — the value that made
// PSP's own walker read our pages correctly. So 3 (gfx10 UC) maps to 2 (gfx12
// UC, proven here) rather than to a literal 3, and 0 (NC) stays 0.
static constexpr uint32_t kAppleMtypeToGfx12[4] = { 0u, 1u, 2u, 2u };

struct XlatPteStats {
    uint32_t roots { 0 }, l1ptrs { 0 }, leaves { 0 };
    uint32_t unmaps { 0 }, already { 0 }, foreign { 0 }, unknown { 0 };
    uint32_t mtype[4] {};
};

// Rewrite ONE PTEPDE packet's flag dwords in place. Layout (the reviewer, cross-checked
// against sdma_v7_0.c:1119-1135 and the r19 dump):
//   [0] header  [1][2] dst lo/hi  [3][4] flags lo/hi  [5][6] value lo/hi
//   [7] increment  [8] 0  [9] count-1
// Returns true when a dword changed.
static inline bool ptepde_to_gfx12(uint32_t *p, uint64_t arenaBot, uint64_t arenaTop,
                            XlatPteStats &st)
{
    const uint64_t dst = ((uint64_t)p[2] << 32) | p[1];
    if (dst < arenaBot || dst >= arenaTop) { st.foreign++; return false; }

    const uint64_t value = ((uint64_t)p[6] << 32) | p[5];
    const uint32_t lo    = p[3];
    uint32_t       hi    = p[4];
    const uint32_t incr  = p[7];
    const uint32_t count = p[9] + 1u;
    const bool valueInArena = (value >= arenaBot && value < arenaTop);

    if (lo == 0 && hi == 0)          { st.unmaps++;  return false; }  // unmap
    if (hi & kPteHiIsPte)            { st.already++; return false; }  // leaf, done
    if (lo == 1 && hi == kPteHiRootG12) { st.already++; return false; }  // root, done
    if (lo == 1 && hi == 0)          { st.already++; return false; }  // L1 ptr, done

    // Root PDE: VALID | BFS 4, one entry, no increment, pointing inside the
    // arena at the next level (getPDEValue 0xbe1ea60).
    if (lo == 1 && hi == kPteHiRootG10 && incr == 0 && count == 1 && valueInArena) {
        p[4] = kPteHiRootG12;
        st.roots++;
        return true;
    }
    // L1 -> 16-entry sub-table: VALID | TRANSLATE_FURTHER. gfx12 has no TF; a
    // directory pointer is simply an entry with P clear.
    if (lo == 1 && hi == kPteHiTfG10 && valueInArena) {
        p[4] = 0;
        st.l1ptrs++;
        return true;
    }
    // Leaf: anything carrying EXEC/READ/WRITE (getPTEValue 0xbe1eaae). Observed
    // flag words: 0x77/0x177/0x1f7 sysmem, 0x1f1/0x71/0xf1 VRAM 4 KiB, 0x271
    // with increment 0x10000 for a 64 KiB leaf at L1.
    if (lo & kPteLoRWX) {
        const uint32_t m = (hi >> 16) & 3u;
        st.mtype[m]++;
        hi |= kPteHiIsPte;
        hi &= ~kPteHiG10MtypeMask;
        hi |= kAppleMtypeToGfx12[m] << kPteHiG12MtypeShift;
        p[4] = hi;
        st.leaves++;
        return true;
    }
    st.unknown++;
    return false;
}

static constexpr uint32_t kGcvmShift  = 0x28;
static constexpr uint32_t kGcvmT0Lo = 0x1624, kGcvmT0Hi = 0x1633;  // CONTEXT0..15_CNTL
static constexpr uint32_t kGcvmT1Lo = 0x1635, kGcvmT1Hi = 0x16ee;  // ENG0_SEM..CTX15_PT_END_HI32
static constexpr uint32_t kGcmcXLo  = 0x1614, kGcmcXHi  = 0x161b;  // FB_LOCATION_BASE..MX_L1_TLB_CNTL
// regGCVM_CONTEXT2_PAGE_TABLE_BASE_ADDR_LO32, gc_12_0_0_offset.h:3114-3115 —
// the same constant the 0.0.191 SRBM self-test targets (sdma_v7_0.cpp).
static constexpr uint32_t kGcvmCtx2PtBaseLo = 0x1693;

static inline bool gcvm_translatable(uint32_t ipSrc) {
    const uint32_t t = ipSrc + kGcvmShift;
    const bool tgtOk = (t >= kGcvmT0Lo && t <= kGcvmT0Hi) ||
                       (t >= kGcvmT1Lo && t <= kGcvmT1Hi);
    if (!tgtOk) return false;
    if (ipSrc >= kGcmcXLo && ipSrc <= kGcmcXHi) return false;   // never redirect MC apertures
    return true;
}

// ===========================================================================
// 0.0.269 — THE DRAIN'S IB PASS.
//
// Strides, each from Apple's OWN emitter in AMDRadeonX6000 (addresses unslid), or from the
// reference where Apple's matches it:
//   0  NOP         1 + count (header bits 29:16)                 (sdma_v7_0.c; Apple fills with 1-dword NOPs)
//   5  FENCE       4   writeProfilingCommand   @0xbe2359f  movl $0x30005,(%rsi)
//   6  TRAP        1
//   8  POLL_REGMEM 6   writeWritePTEPDECommand @0xbe2368e  movl $0x80000008,0x28(%rsi) .. 0x3c
//   11 CONST_FILL  5   writeConstantFillCommand @0xbe235ed movl $0x8000000b ... 0x10(%rsi)
//   12 PTEPDE      10  writeWritePTEPDECommand @0xbe23651  movl $0xc,(%rsi) .. 0x24(%rsi)
//   13 TIMESTAMP   3   writeGetGlobalTimestampCommand @0xbe236cc movl $0x20d,(%rsi) ; movl $0x3,%eax
//                      (NOT 4: the read-only decoder sdma_pkt_stride in AppleHardwareHook.cpp says 4 and is wrong
//                      for Apple's form; the drain does not use it)
//   14 REG_WRITE   3
//   16 GPUVM_INV   4   writeVMInvalidateCommand @0xbe234d8 movl $0x10,(%r12) ; @0xbe2350c movl $0x4,%r15d
// Any other opcode REFUSES the IB before a single dword is changed (pass 1 validates, pass 2 writes).
static constexpr uint32_t kDxMaxDwords = 65536u;

static inline uint32_t sdma_drain_ib_stride(uint32_t h)
{
    switch (h & 0xFFu) {
    case 0:  return 1u + ((h >> 16) & 0x3FFFu);
    case 5:  return 4u;
    case 6:  return 1u;
    case 8:  return 6u;
    case 11: return 5u;
    case 12: return 10u;
    case 13: return 3u;
    case 14: return 3u;
    case 16: return 4u;
    default: return 0u;
    }
}

struct SdmaDrainIbStats {
    uint32_t packets;           // pass-1 packets
    uint32_t nops, fences, traps, memPolls, regPolls, fills, ptepdes, timestamps, regWrites, gpuvmInv;
    uint32_t regWritesConverted;   // Apple SRBM form -> v7 form, GCVM renumbered (+0x28)
    uint32_t regWritesAlreadyV7;   // header already 0x0000000e
    uint32_t regWritesLeft;        // Apple form, not a translatable GCVM register (a harmless no-op on SDMA 7,)
    uint32_t regWritesMcExcluded;  // an MC aperture source, deliberately left alone (0.0.52)
    uint32_t ctx2BasesValidated;   // CONTEXT2 PT base given its VALID bit
    uint32_t pollsShifted;         // register-form polls renumbered
    uint32_t pollsNeutered;        // register-form polls with a live mask -> ref 0, mask 0
    uint32_t pollsAlreadyNeutered; // mask already 0 (translated before) - left alone
    uint32_t pollsKept;            // 0.0.358: TLB-invalidate ACK polls renumbered and KEPT (ref/mask left as Apple wrote them)
    uint32_t pollsAlreadyKept;     // 0.0.358: a poll this pass kept before, seen again - left alone
    XlatPteStats pte;              // ptepde_to_gfx12's own counts
    uint32_t refusedOp;            // opcode that refused (valid when refused)
    uint32_t refusedAt;            // dword index
    uint32_t refusedHeader;
    uint8_t  refused;              // 1 unknown opcode, 2 packet runs past the IB end, 3 bad argument
};

// ---------------------------------------------------------------------------
// 0.0.358 — THE TLB-INVALIDATE ACK WAIT, MADE REAL.
//
// Apple's VM-program IB ends with the textbook set-up-the-context-then-invalidate sequence: REG_WRITE
// GCVM_INVALIDATE_ENG6_REQ <- 0x00990004 (VMID 2), then a register-form POLL_REGMEM on GCVM_INVALIDATE_ENG6_ACK with
// ref = mask = 1 << vmid, func 3 (equal), DW5 0x0fff0004. The drain renumbers both correctly (+0x28,) and then,
// since 0.0.269, ZEROED ref and mask: the SDMA fence can then signal before the invalidation completes - a stale-
// TRANSLATION hazard for a reused VA range, and it hides whether the translated REQUEST works at all
// (notes/M4-X9-COMPLETE.mdc: 3 per run, 6 of 6 runs, one of them WindowServer's own before its first frame).
//
// UPSTREAM DOES EXACTLY THIS WAIT on this engine generation: ref/linux-amdgpu/gmc_v12_0.c:418-421
// (gmc_v12_0_emit_flush_gpu_tlb: amdgpu_ring_emit_reg_write_reg_wait(ring, vm_inv_eng0_req + eng,
// vm_inv_eng0_ack + eng, req, 1 << vmid)), emitted on SDMA by ref/linux-amdgpu/sdma_v7_0.c:1214-1236
// (sdma_v7_0_ring_emit_reg_wait: POLL_REGMEM FUNC(3), reg << 2, ref, mask, RETRY_COUNT(0xfff)); the GFXHUB takes no
// invalidate semaphore (gmc_v12_0.c:190-195, MMHUB only). Apple's REQ value is a valid gfx12 request: PER_VMID bit 2,
// FLUSH_TYPE 1, INVALIDATE_L2_PTES, L2_PDE0, L1_PTES (amdgpu_field_defs.h:169-184, the gfx12 layout; the gfx12 REQ builder
// is ref/linux-amdgpu/gfxhub_v12_0.c:61-79).
//
// KEPT ONLY IN THAT SHAPE, else neutered exactly as before (so every other poll's bytes are unchanged): the poll's
// renumbered register is a gfx12 GCVM_INVALIDATE_ENGn_ACK (0x1659 + n, n 0..17; eng0_ack 0x1659 and eng0_req 0x1647
// are gmc_v12_0.cpp:595-596), func is 3, ref == mask != 0, the mask is per-VMID bits only, and EVERY bit of it was
// REQUESTED on the same engine by a REG_WRITE earlier in the SAME IB. The last clause is what stops the wait from ever
// being on an acknowledgement nobody asked for. It does NOT make the wait bounded: if the request does not land, the SDMA
// ring parks in the poll, as it did in before the REG_WRITE form was fixed - one boot, and the reason the kext can
// switch it off at run time (`gfxneuter 9 | M << 8`).
//
// Idempotence, which the drain relies on: a KEPT poll is already gfx12-numbered and its mask is non-zero, so the old rule
// would shift it a second time. It is recognised instead as: register already a gfx12 ENGn_ACK, func 3, ref == mask, and
// every mask bit requested by a REG_WRITE earlier in this IB that is ALREADY in v7 form (header 0x0000000e) on that
// engine's gfx12 REQ. Apple's own stream never carries a v7-form REG_WRITE, so a first-pass poll cannot match it, and
// before 0.0.358 no output of this function carried a live-mask poll after one (every poll was neutered), so with
// `flags` 0 the only IBs whose output differs from 0.0.357's are ones holding a poll 0.0.358 kept - which 0.0.357's rule
// would have shifted a second time onto the wrong register.
static constexpr uint32_t kGcvmInvReqLo = 0x1647u, kGcvmInvAckLo = 0x1659u, kGcvmInvEngines = 18u;
enum : uint32_t { kDxKeepInvAck = 1u };

static inline int sdma_inv_eng(uint32_t ip, uint32_t lo)
{
    return (ip >= lo && ip < lo + kGcvmInvEngines) ? (int)(ip - lo) : -1;
}

// Translate one Apple SDMA IB in place. Returns 1 changed, 0 nothing to change, -1 REFUSED (buf untouched).
// Idempotent by content: a converted REG_WRITE has header 0x0000000e, a converted PTEPDE is recognised by its
// gfx12 flags, and a neutered poll has mask 0 and is not renumbered again - so running this twice over the
// same bytes changes nothing the second time. `flags` 0 is the 0.0.269-0.0.357 behaviour, byte for byte, on every IB
// that does not already hold a poll kept by `kDxKeepInvAck` (see above).
static inline int sdma_drain_translate_ib_ex(uint32_t *buf, uint32_t n, uint32_t gcBase,
                                             uint64_t arenaBot, uint64_t arenaTop, SdmaDrainIbStats *st, uint32_t flags)
{
    if (!buf || !st || n == 0 || n > kDxMaxDwords || gcBase == 0) { if (st) st->refused = 3; return -1; }
    // pass 1: every packet known, and the walk lands exactly on the end
    for (uint32_t k = 0; k < n; ) {
        const uint32_t h = buf[k];
        const uint32_t s = sdma_drain_ib_stride(h);
        if (s == 0) { st->refused = 1; st->refusedOp = h & 0xFFu; st->refusedAt = k; st->refusedHeader = h; return -1; }
        if (k + s > n) { st->refused = 2; st->refusedOp = h & 0xFFu; st->refusedAt = k; st->refusedHeader = h; return -1; }
        st->packets++;
        switch (h & 0xFFu) {
        case 0:  st->nops++; break;
        case 5:  st->fences++; break;
        case 6:  st->traps++; break;
        case 8:  if ((h >> 31) & 1u) st->memPolls++; else st->regPolls++; break;
        case 11: st->fills++; break;
        case 12: st->ptepdes++; break;
        case 13: st->timestamps++; break;
        case 14: st->regWrites++; break;
        case 16: st->gpuvmInv++; break;
        default: break;
        }
        k += s;
    }
    // pass 2: the three transforms xlatregs + neuterpoll apply, in one pass
    bool changed = false;
    const bool keepAck = (flags & kDxKeepInvAck) != 0u;
    uint32_t reqNew[kGcvmInvEngines] = { 0 }, reqV7[kGcvmInvEngines] = { 0 };   // per-VMID bits REQUESTED earlier in this IB
    for (uint32_t k = 0; k < n; ) {
        const uint32_t h = buf[k];
        const uint32_t s = sdma_drain_ib_stride(h);
        const uint32_t op = h & 0xFFu;
        if (op == 14) {
            if (h == 0x0000000eu) {
                st->regWritesAlreadyV7++;
                const uint32_t abs7 = buf[k + 1] >> 2;
                const int e = abs7 > gcBase ? sdma_inv_eng(abs7 - gcBase, kGcvmInvReqLo) : -1;
                if (e >= 0) reqV7[e] |= buf[k + 2] & 0xFFFFu;
            } else {
                const uint32_t abs = buf[k + 1];
                bool done = false;
                if (abs > gcBase) {
                    const uint32_t ip = abs - gcBase;
                    if (gcvm_translatable(ip)) {
                        buf[k]     = 0x0000000eu;
                        buf[k + 1] = (abs + kGcvmShift) << 2;
                        const int e = sdma_inv_eng(ip + kGcvmShift, kGcvmInvReqLo);
                        if (e >= 0) reqNew[e] |= buf[k + 2] & 0xFFFFu;
                        if (ip + kGcvmShift == kGcvmCtx2PtBaseLo && (buf[k + 2] & 1u) == 0) {
                            buf[k + 2] |= 1u;
                            st->ctx2BasesValidated++;
                        }
                        st->regWritesConverted++;
                        changed = done = true;
                    } else if (ip >= kGcmcXLo && ip <= kGcmcXHi) {
                        st->regWritesMcExcluded++;
                        done = true;
                    }
                }
                if (!done) st->regWritesLeft++;
            }
        } else if (op == 12) {
            if (ptepde_to_gfx12(&buf[k], arenaBot, arenaTop, st->pte)) changed = true;
        } else if (op == 8 && !((h >> 31) & 1u)) {
            const uint32_t ref = buf[k + 3], mask = buf[k + 4], func = (h >> 28) & 7u;
            const bool ackShape = func == 3u && ref == mask && mask != 0u && (mask & ~0xFFFFu) == 0u;
            const uint32_t abs = buf[k + 1] >> 2;
            // Recognising OUR kept poll does not depend on `flags`: a reused IB translated with the wait kept and then
            // re-walked after `gfxneuter 9 | 2 << 8` switched it off must still not be shifted a second time.
            const int eNow = abs > gcBase ? sdma_inv_eng(abs - gcBase, kGcvmInvAckLo) : -1;
            if (buf[k + 4] == 0) {
                st->pollsAlreadyNeutered++;
            } else if (eNow >= 0 && ackShape && (mask & ~reqV7[eNow]) == 0u) {
                st->pollsAlreadyKept++;          // ours, from an earlier pass: already gfx12-numbered, left alone
            } else {
                bool shifted = false;
                if (abs > gcBase && gcvm_translatable(abs - gcBase)) {
                    buf[k + 1] = (abs + kGcvmShift) << 2;
                    st->pollsShifted++;
                    shifted = true;
                }
                const int e = (keepAck && shifted) ? sdma_inv_eng(abs + kGcvmShift - gcBase, kGcvmInvAckLo) : -1;
                if (e >= 0 && ackShape && (mask & ~reqNew[e]) == 0u) {
                    st->pollsKept++;             // the wait upstream emits: ref and mask stay Apple's
                } else {
                    buf[k + 3] = 0;     // ref
                    buf[k + 4] = 0;     // mask
                    st->pollsNeutered++;
                }
                changed = true;
            }
        }
        k += s;
    }
    return changed ? 1 : 0;
}

// The 0.0.269-0.0.357 entry point: every register poll with a live mask neutered. Byte-identical to before.
static inline int sdma_drain_translate_ib(uint32_t *buf, uint32_t n, uint32_t gcBase,
                                          uint64_t arenaBot, uint64_t arenaTop, SdmaDrainIbStats *st)
{
    return sdma_drain_translate_ib_ex(buf, n, gcBase, arenaBot, arenaTop, st, 0u);
}

// ---- the ring-frame walk -----------------------------------------------------
// Apple's SDMA ring frame, as `ringib` dumped chan 14 on an early run:
//   [0000] 00000009 0000019c 00000084 00000001 0000007b   COND_EXE (5)
//   [0005] 00000000 x5                                     NOP
//   [000a] 80000004 00800000 00000084 00000055 00000000 00000000   INDIRECT (6): IB 0x8400800000, 0x55 dwords
//   [002b] 00030005 00000180 00000084 00000001             FENCE (4)
//   [002f] 00000006                                        TRAP (1)
// The walk records every INDIRECT; an opcode it does not know STOPS it (reported with its position and header),
// and the caller decides what to do with the rest.
struct SdmaDrainRingIb { uint64_t ib; uint32_t dwords; uint32_t at; };

static inline uint32_t sdma_drain_ring_stride(uint32_t h)
{
    switch (h & 0xFFu) {
    case 0:  return 1u + ((h >> 16) & 0x3FFFu);
    case 4:  return 6u;
    case 5:  return 4u;
    case 6:  return 1u;
    case 9:  return 5u;
    case 13: return 3u;
    default: return 0u;
    }
}

// Returns the number of INDIRECTs recorded (at most `max`; `*more` counts any beyond). `*stopAt` is n when the
// walk reached the end exactly, else the dword where it stopped; `*stopHeader` that dword.
static inline uint32_t sdma_drain_ring_walk(const uint32_t *d, uint32_t n, SdmaDrainRingIb *out, uint32_t max,
                                            uint32_t *more, uint32_t *stopAt, uint32_t *stopHeader)
{
    uint32_t cnt = 0, extra = 0, k = 0;
    if (stopHeader) *stopHeader = 0;
    while (d && k < n) {
        const uint32_t h = d[k];
        const uint32_t s = sdma_drain_ring_stride(h);
        if (s == 0 || k + s > n) { if (stopHeader) *stopHeader = h; break; }
        if ((h & 0xFFu) == 4) {
            const uint64_t ib = ((uint64_t)d[k + 2] << 32) | d[k + 1];
            const uint32_t dw = d[k + 3];
            if (ib != 0 && dw != 0) {
                if (cnt < max && out) { out[cnt].ib = ib; out[cnt].dwords = dw; out[cnt].at = k; cnt++; }
                else extra++;
            }
        }
        k += s;
    }
    if (more) *more = extra;
    if (stopAt) *stopAt = (d && k >= n) ? n : k;
    return cnt;
}

#endif // NAVI48_SDMA_DRAIN_XLAT_AED4FF2_H
