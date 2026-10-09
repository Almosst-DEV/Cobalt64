// rootwrite_guards.h — the G1..G6 refusal predicate for MILESTONE 3 step 4
// increment (iii): the single 8-byte root PDE write into a live Apple VM context.
//
// WHY THIS IS A SEPARATE, DEPENDENCY-FREE HEADER.
//
// notes/an internal review note.4 divides the guards sharply:
//
//     G1-G3 fail SILENTLY and OFF-TARGET  (another process's GPU memory, no fault)
//     G4-G6 fail LOUDLY and ON-TARGET     (a clean VM fault at an address we chose)
//
// A guard whose failure is silent cannot be validated by running it on hardware and
// seeing nothing happen — "nothing happened" is exactly what a silently-wrong guard
// looks like. So the predicate is a PURE FUNCTION over an explicit input struct,
// with no kernel types, no globals and no I/O, and the host test
// tests/rootwrite_guards_test.cpp drives it through every REFUSAL path. The kext
// and the test compile the identical code: there is no second implementation to
// drift.
//
// The rule this encodes: an identity may key ONLY on something Apple
// cannot recycle underneath it. Apple reuses AMDHWVMContext addresses (r86) AND
// AMDAccelTask addresses (r87), sometimes in the same boot, so the vtable and task
// checks are used ONLY TO REJECT, never to confirm; what confirms is our own
// creation sequence number, which is ours and monotonic.
//
// Returns 0 (kRootWriteOk) to permit the write, else the number of the FIRST guard
// that refused. Guards are evaluated in order so the reported number is the
// earliest objection, which is the most informative one.

#ifndef NAVI48_ROOTWRITE_GUARDS_H
#define NAVI48_ROOTWRITE_GUARDS_H

#include <stdint.h>

enum RootWriteGuard {
    kRootWriteOk = 0,
    kRootWriteG1 = 1,   // the context is ours, live, identity intact, not already written
    kRootWriteG2 = 2,   // the root address is inside Apple's arena and 4 KiB aligned
    kRootWriteG3 = 3,   // root[0] is a live root PDE pointing INTO the arena
    kRootWriteG4 = 4,   // root[511] reads EXACTLY zero, both dwords
    kRootWriteG5 = 5,   // no pending Apple PTEPDE targets this root page, and (0.0.356) the
                        // root's own block-clear is WITNESSED as already executed and no
                        // decoded WRITE/COPY/PTEPDE in the scanned content covers slot 511
    kRootWriteG6 = 6,   // our own L1 block is built and verified THIS BOOT
    // 0.0.356: 8, NOT 7. "G7" is already the name of the withdrawal's own check
    // (rootwrite_withdraw_check below, review.4), and one number meaning two guards
    // is exactly the kind of string that lies to the next reader.
    kRootWriteG8 = 8,   // the caller MEASURED the gfx power state at write time and it read OFF
};

// Everything the predicate needs, gathered by the caller. Plain integers only:
// the point is that a host test can construct any of these states, including the
// ones that are hard or impossible to produce on demand on real hardware.
struct RootWriteInputs {
    // --- G1: identity. ctxSeq is OUR creation counter and is what actually
    // confirms; ctxState/ctxIdentityOk only ever veto. alreadyWrote makes the
    // write idempotent per context so a repeated verb cannot double-arm.
    uint32_t ctxState;        // 0 free, 1 live, 2 released (our own record)
    uint32_t ctxIdentityOk;   // ctx vtable AND ctx->0x28 task still match create
    uint32_t ctxSeq;          // our own creation sequence number; 0 = no record
    uint32_t alreadyWrote;    // non-zero once we have written for this context

    // --- G2: the root page address, read from ctx+0x98+0x20 while live.
    uint64_t root;
    uint64_t arenaBot;        // Apple's page-table arena, exclusive bounds
    uint64_t arenaTop;

    // --- G3: root[0], the entry Apple itself wrote.
    uint64_t rootEnt0;

    // --- G4: root[511], the slot we intend to write.
    uint64_t rootEnt511;

    // --- G5: destinations of PTEPDE packets still pending in Apple's SDMA rings
    // that land inside [root, root + 0x1000). Non-zero means Apple is about to
    // rewrite this page and would revert us (review.4 rates this benign, but
    // it is still a refusal: a write that is about to be undone is not a
    // measurement).
    uint32_t pendingPtepdeHits;

    // --- G6: our own mapping, from the ringmap state THIS BOOT.
    uint32_t ringMapBuilt;
    uint32_t ringMapMismatched;   // L1 read-back mismatches; must be 0
    uint32_t ringMapProbes;       // walker probes run; must be non-zero
    uint32_t ringMapProbeBad;     // walker probe mismatches; must be 0
    uint64_t l1Off;               // our L1 block's 0-based VRAM offset

    // ======================================================================
    // 0.0.356 — the rootwrite review's four REQUIRED changes, as
    // predicate inputs. Every new clause below can only ADD a refusal; the
    // property test in tests/rootwrite_guards_test.cpp proves that nothing the
    // pre-0.0.356 predicate refused is now permitted.

    // --- G1, the OWNER clause (review change 1). Set by a caller that selected the
    // context BY ITS OWNER (the `rootwrite 2|3` WindowServer mode) rather than by
    // "whichever live root equals CONTEXT2 right now". ownerOk is the caller's
    // runtime evidence: the creator pid recorded at createVMContext is alive NOW
    // under the recorded name, and it is the ONLY such live context with a root.
    uint32_t ownerRequired;
    uint32_t ownerOk;
    // --- G1, the VMID clause. The write path invalidates the GFXHUB TLB for VMID 2
    // only, so a caller that demands it must show CONTEXT2 names this root NOW.
    uint32_t ctx2Required;
    uint32_t ctx2Agrees;

    // --- G5, the BLOCK-CLEAR WITNESS (review change 3). measured the hazard
    // G5 never looked at: Apple's clearWithDMA (SDMA CONST_FILL) of a FRESHLY allocated
    // root page, queued inside the mapVA that allocated it, wipes slot 511 when it
    // finally executes. The witness that it HAS executed: our mapVA hook saw THIS root
    // page appear (rootAtTransition == root) and read root[0] as ZERO at that instant,
    // and root[0] is now a live arena PDE (G3). Only the PTEPDE that follows the clear
    // writes root[0]; a clear still pending would wipe Apple's own root[0] too, which a
    // working driver cannot allow - so "zero at allocation, live now" means the clear
    // is behind us. A root[0] that was ALREADY non-zero at allocation is stale VRAM
    // and proves nothing, so it refuses.
    uint32_t clearWitnessKnown;   // the hook saw the transition AND read root[0] then
    uint64_t rootAtTransition;    // the root page the hook saw appear
    uint64_t ent0AtTransition;    // root[0] as read at that instant
    // Decoded, length-KNOWN writes (WRITE, COPY, PTEPDE by the full reader vocabulary,
    // which also sees past the NOPs and opcodes the narrow table stops on) whose
    // destination covers slot 511. Apple has no reason ever to aim one there.
    uint32_t slotWriteHits;

    // --- G8, gfx power at the write (review change 4). Only a caller that MEASURED
    // it sets gfxPowerChecked; rootwrite_gfx_power_on() below makes the call.
    uint32_t gfxPowerChecked;
    uint32_t gfxPowerOn;
};

// gc_12_0_0_sh_mask.h (in-tree, ref/linux-asic-reg): RLC_GPM_STAT (GC BASE_IDX 1, dword
// 0x4e6c) GFX_POWER_STATUS bit 1, GFX_CLOCK_STATUS bit 2; RLC_PG_CNTL (GC BASE_IDX 1,
// dword 0x4c43) GFX_POWER_GATING_ENABLE bit 0. SUSPECTED, not in tree: 1 = powered /
// clocked (gfx_v8_0's enter_rlc_safe_mode waits for BOTH bits to read 1 after asking
// the RLC to hold gfx on). The call therefore refuses only when the RLC says power
// gating is ENABLED and the power-status bit reads 0 - never on a status bit alone -
// and a dead read (all ones) is not "on".
#define N48_RLC_GPM_STAT_GFX_POWER   (1u << 1)
#define N48_RLC_GPM_STAT_GFX_CLOCK   (1u << 2)
#define N48_RLC_PG_CNTL_GFX_PG_EN    (1u << 0)
static inline uint32_t rootwrite_gfx_power_on(uint32_t gpmStat, uint32_t pgCntl)
{
    if (gpmStat == 0xFFFFFFFFu || pgCntl == 0xFFFFFFFFu) return 0u;   // dead read
    if (!(pgCntl & N48_RLC_PG_CNTL_GFX_PG_EN)) return 1u;             // RLC power gating is off
    return (gpmStat & N48_RLC_GPM_STAT_GFX_POWER) ? 1u : 0u;
}

// gfx12 page-table entry bits used below (amdgpu_ip.h PTEFlags, and the gfx12
// PDE layout recorded at AppleHardwareHook.cpp's PTEPDE encoding block:
// P at 63, BFS at 62:58, MTYPE at 55:54).
#define N48_PTE_VALID_BIT   (1ull << 0)
#define N48_PTE_SYSTEM_BIT  (1ull << 1)
#define N48_PTE_P_BIT       (1ull << 63)
#define N48_PTE_ADDR_MASK   0x0000FFFFFFFFF000ull

static inline uint32_t rootwrite_guard_check(const struct RootWriteInputs *in)
{
    if (!in) return kRootWriteG1;

    // --- G1. Live, ours, identity intact, not already written.: the vtable
    // and task checks REJECT only; ctxSeq is what confirms, because Apple recycles
    // both pointers and a lookup keyed on either can bind to the wrong record.
    if (in->ctxSeq == 0)        return kRootWriteG1;
    if (in->ctxState != 1)      return kRootWriteG1;   // not live (free or released)
    if (!in->ctxIdentityOk)     return kRootWriteG1;   // recycled underneath us
    if (in->alreadyWrote)       return kRootWriteG1;   // idempotent per context
    // 0.0.356: selected by owner -> the owner evidence must hold NOW; bound to
    // VMID 2's invalidate -> CONTEXT2 must name this root NOW. Both only veto.
    if (in->ownerRequired && !in->ownerOk)   return kRootWriteG1;
    if (in->ctx2Required && !in->ctx2Agrees) return kRootWriteG1;

    // --- G2. The root must be a 4 KiB-aligned page inside Apple's arena. Outside
    // the arena is review.4's worst row: vram_alloc_hi or the PSP TMR tail.
    if (in->arenaTop <= in->arenaBot)                  return kRootWriteG2;
    if (in->root == 0)                                 return kRootWriteG2;
    if (in->root & 0xFFFull)                           return kRootWriteG2;
    if (in->root < in->arenaBot)                       return kRootWriteG2;
    if (in->root >= in->arenaTop)                      return kRootWriteG2;

    // --- G3. root[0] must look like the live root PDE Apple wrote: VALID, a
    // directory (P clear), VRAM (sys clear), and — the clause that actually
    // carries this guard — a target INSIDE the arena.
    //
    // is the reason the range clause is load-bearing: VRAM IS NOT ZEROED AT
    // BOOT, so on stale contents the VALID bit alone is worthless; a recycled leaf
    // page can easily have bit 0 set in its first entry. Only "points back into
    // the 68 MiB arena" distinguishes a real root table from stale image data.
    if (!(in->rootEnt0 & N48_PTE_VALID_BIT))           return kRootWriteG3;
    if (in->rootEnt0 & N48_PTE_P_BIT)                  return kRootWriteG3;  // a leaf, not a root
    if (in->rootEnt0 & N48_PTE_SYSTEM_BIT)             return kRootWriteG3;  // sysmem, not VRAM
    {
        const uint64_t t = in->rootEnt0 & N48_PTE_ADDR_MASK;
        if (t < in->arenaBot || t >= in->arenaTop)     return kRootWriteG3;
    }

    // --- G4. The slot we want must be EXACTLY zero, both dwords. Review: if
    // Apple ever claimed 511 the cost of this guard being strict is a refused
    // write, never corruption.
    if (in->rootEnt511 != 0)                           return kRootWriteG4;

    // --- G5. Apple must not have a queued PTEPDE aimed at this page.
    if (in->pendingPtepdeHits != 0)                    return kRootWriteG5;
    // 0.0.356: ...nor any decoded write whose range covers our slot...
    if (in->slotWriteHits != 0)                        return kRootWriteG5;
    // ...and the page's own deferred block-clear must be WITNESSED as executed. This is
    // the clause that makes an arm at the mapVA transition itself impossible: there,
    // root[0] now == root[0] at the transition, so either it is zero (G3 refuses above)
    // or it was non-zero at allocation (stale, refused here) - which is precisely the
    // r96 case, where an arm at the transition was wiped by the clear it raced.
    if (!in->clearWitnessKnown)                        return kRootWriteG5;
    if (in->rootAtTransition != in->root)              return kRootWriteG5;
    if (in->ent0AtTransition != 0)                     return kRootWriteG5;

    // --- G6. Our own L1 block must be built and PROVEN this boot, or the entry
    // would point at an unbuilt or half-built table.
    if (!in->ringMapBuilt)                             return kRootWriteG6;
    if (in->ringMapMismatched != 0)                    return kRootWriteG6;
    if (in->ringMapProbes == 0)                        return kRootWriteG6;
    if (in->ringMapProbeBad != 0)                      return kRootWriteG6;
    if (in->l1Off == 0)                                return kRootWriteG6;
    if (in->l1Off & 0xFFFull)                          return kRootWriteG6;

    // --- G8 (0.0.356,). Measured gfx power, when the caller measured it.
    if (in->gfxPowerChecked && !in->gfxPowerOn)        return kRootWriteG8;

    return kRootWriteOk;
}

// ---------------------------------------------------------------------------
// 0.0.253 — G7 (WITHDRAW) and the RE-ARM predicate, as pure functions.
//
// These live here, beside G1-G6, for exactly the reason the header exists at all.
// Review.4 puts G7 in the SILENT, OFF-TARGET class: if it is wrong we zero eight
// bytes of a page that has already changed owner, nothing faults, and another
// process's mapping is quietly destroyed. A hardware run cannot tell that apart
// from success, so the only way to validate it is to drive it through every refusal
// in a host test — which requires it to be a pure function over an explicit struct,
// not logic inlined in a hook that needs a live Apple context to execute.
//
// r89 measured why the re-arm predicate is load-bearing rather than
// decorative: 11 slot-37 fires for the armed contexts, 1 of which freed the root and
// **10 of which left it alive**. Withdrawing on every unmap is safe by construction,
// but it means the re-arm path runs on almost every call, so it must be exactly as
// hard to get wrong as the withdrawal.

enum RootWithdrawVerdict {
    kRootWithdrawOk        = 0,  // clear it: ours, in the page we wrote, still unchanged
    kRootWithdrawNotArmed  = 1,  // we never wrote for this context, or already withdrew
    kRootWithdrawNoRead    = 2,  // the slot could not be read back - refuse, do not guess
    kRootWithdrawPageMoved = 3,  // the context's root is no longer the page we wrote into
    kRootWithdrawNotOurs   = 4,  // G7 proper: the slot no longer holds EXACTLY our value
};

struct RootWithdrawInputs {
    uint32_t wrote;            // we wrote an entry for this context
    uint32_t withdrawn;        // and have not already taken it back
    uint32_t slotRead;         // the MM-window read of root[511] succeeded
    uint64_t rootNow;          // ctx+0x98+0x20 as it reads at this instant
    uint64_t rootPageAtWrite;  // the page we wrote into, cached at write time
    uint64_t slot511;          // what root[511] reads now
    uint64_t rootWritten;      // the EXACT entry we put there
};

// Returns 0 to permit the clear, else the first objection. Every refusal leaves the
// page untouched, which is always the safe outcome: the cost of refusing wrongly is
// that our own mapping outlives its page, which G4 then catches on the next arm.
static inline uint32_t rootwrite_withdraw_check(const struct RootWithdrawInputs *in)
{
    if (!in)                                    return kRootWithdrawNotArmed;
    if (!in->wrote || in->withdrawn)            return kRootWithdrawNotArmed;
    if (!in->slotRead)                          return kRootWithdrawNoRead;
    // The page must still be the one we wrote into. Apple recycles these pages
    //, so a changed root means the cached address now belongs to
    // somebody else and writing at it is review.4's G7 row.
    if (in->rootNow == 0)                       return kRootWithdrawPageMoved;
    if (in->rootNow != in->rootPageAtWrite)     return kRootWithdrawPageMoved;
    if (in->rootWritten == 0)                   return kRootWithdrawNotArmed;
    // G7 proper. Not "is it valid", not "does it look like ours" - EXACTLY the 64
    // bits we wrote. Anything else and the entry is not ours to clear.
    if (in->slot511 != in->rootWritten)         return kRootWithdrawNotOurs;
    return kRootWithdrawOk;
}

enum RootRearmVerdict {
    kRootRearmOk           = 0,  // put it back: the root survived and the slot is free
    kRootRearmNotWithdrawn = 1,  // nothing of ours is currently down
    kRootRearmRootFreed    = 2,  // ctx+0x98+0x20 reads 0: Apple freed it, do NOTHING
    kRootRearmRootMoved    = 3,  // the root changed address across the call
    kRootRearmSlotTaken    = 4,  // slot 511 is no longer the zero we left behind
    kRootRearmNoRead       = 5,  // could not read the slot back
};

struct RootRearmInputs {
    uint32_t withdrawn;        // we took our entry down at this call's entry
    uint32_t slotRead;         // the MM-window read of root[511] succeeded
    uint64_t rootAtEntry;      // ctx+0x98+0x20 as it read when the call began
    uint64_t rootAtReturn;     // and as it reads now
    uint64_t rootPageAtWrite;  // the page we originally wrote into
    uint64_t slot511;          // what root[511] reads now
};

// The decision memo's key insight, encoded: rootAtReturn == 0 is APPLE'S OWN signal
// that it freed the root inside this call (be1252a zeroes the field downstream of the
// arena return). Observed after the fact, it needs no predicate at entry - and when it
// is set, re-arming would write into a page that is already back in Apple's allocator.
// So a freed root is a REFUSAL, not an error.
static inline uint32_t rootwrite_rearm_check(const struct RootRearmInputs *in)
{
    if (!in)                                      return kRootRearmNotWithdrawn;
    if (!in->withdrawn)                           return kRootRearmNotWithdrawn;
    if (in->rootAtReturn == 0)                    return kRootRearmRootFreed;
    if (in->rootAtEntry != in->rootAtReturn)      return kRootRearmRootMoved;
    if (in->rootAtReturn != in->rootPageAtWrite)  return kRootRearmRootMoved;
    if (!in->slotRead)                            return kRootRearmNoRead;
    // We zeroed this slot on the way in. If it is not zero now, something else wrote
    // it during Apple's call and it is no longer ours to reclaim.
    if (in->slot511 != 0)                         return kRootRearmSlotTaken;
    return kRootRearmOk;
}

// ---------------------------------------------------------------------------
// 0.0.257 — the ARM TRIGGER, as a pure predicate.
//
// r91 measured why the arm cannot be placed from a step list: the root page
// does not exist until Apple's lazy allocation runs, and the register that names it
// is programmed later still, by SDMA packets our own takeover releases. The arm
// therefore moves inside the kext, onto AMDHWVMContext::mapVA (vtable slot 36), and
// fires on the instant `ctx->0x98+0x20` first becomes non-zero - the call that
// created the root is the first moment there is anything to arm at all.
//
// The condition is a TIMING one, so it is factored out here for the same reason G7
// and the re-arm are: a host test can drive every refusal, where hardware can only
// ever show one path. Getting this wrong is not loud - arming on the wrong call
// means writing into a page that is not the context's root, which is review.4's
// silent, off-target class.
//
// NOTE the deliberate omission: this does NOT compare the root against the CONTEXT2
// register. At mapVA time CONTEXT2 is frequently still zero (r91 read 0 through step
// 20), so requiring agreement here would refuse exactly when we need to arm. The
// agreement is not abandoned - it is re-established later by observation, in the
// render path's liveness guard (0.0.256), which reads CONTEXT2 and requires
// root[511] to hold our PDE before the rings are driven from our mapping.

enum RootArmTrigger {
    kRootArmOk            = 0,  // this call created the root: evaluate G1-G6 and arm
    kRootArmNotOurs       = 1,  // no live record of this context, or identity failed
    kRootArmAlreadyArmed  = 2,  // we already wrote for this context (idempotent)
    kRootArmNoTransition  = 3,  // the root did not appear during this call
};

struct RootArmTriggerInputs {
    uint32_t ctxLive;      // our own record says this context is live
    uint32_t identityOk;   // the root read back behind the identity check
    uint32_t alreadyWrote; // G1's idempotence, checked before anything is read
    uint64_t rootBefore;   // ctx->0x98+0x20 as it read at mapVA ENTRY
    uint64_t rootAfter;    // and at mapVA RETURN
};

static inline uint32_t rootwrite_arm_trigger(const struct RootArmTriggerInputs *in)
{
    if (!in)                        return kRootArmNotOurs;
    if (!in->ctxLive)               return kRootArmNotOurs;
    if (!in->identityOk)            return kRootArmNotOurs;
    if (in->alreadyWrote)           return kRootArmAlreadyArmed;
    // The transition, and ONLY the transition. A root that was already there is not
    // ours to claim on this call: something else allocated it and we have no evidence
    // about what has happened to it since.
    if (in->rootBefore != 0)        return kRootArmNoTransition;
    if (in->rootAfter == 0)         return kRootArmNoTransition;
    return kRootArmOk;
}

// ---------------------------------------------------------------------------
// 0.0.259 — RE-ADOPTION of a stale entry, by REGION and never by VALUE.
//
// r93 refused to arm because root[511] already held exactly the PDE it was about to
// write: our own entry from r92, which no teardown ever withdrew and which survived
// the reboot because VRAM is not zeroed and the carve is deterministic. G4 was right
// to refuse, and the tempting exemption - "permit it when the slot already equals what
// we would write" - stays REJECTED: our L1 address is stable across boots, so that
// value is predictable and a different client's recycled page could legitimately hold
// it.
//
// notes/M3-ENTRY-LIFETIME.md replaces it with a question that is decidable rather
// than probabilistic: does this entry's ADDRESS FIELD name a page inside the region
// THIS BOOT reserved? Three structural facts make that proof of ownership:
//   * Apple's page-table blocks come only from its own arena (mapVMPT's allocator,
//     isInVMReservedPool's +0x4400000 bound);
//   * Apple's client pages come only from vram_alloc_hi, which our carve sits above
//     and which navi48_vram_apple_dest_check refuses for our region;
//   * our reservation lies in the 188 MiB gap between them, with 64 MiB of clearance.
// So an entry naming our tail cannot be Apple's: it is ours, or stale bytes. Both are
// safe to overwrite. This is the same argument G3 already trusts in the opposite
// direction ("a target INSIDE the arena").
//
// HOOK PATH ONLY. The mapVA arm fires on the zero -> non-zero transition, which means
// Apple's lazy allocator handed this page to this context inside that very call - so
// the page is provably a freshly allocated root block, not another client's live leaf.
// That evidence does not exist at the `rootwrite` verb, whose target selection merely
// picks a live context agreeing with CONTEXT2. `armTransition` carries it, and without
// it this predicate always refuses.

enum RootAdoptVerdict {
    kAdoptRefuse    = 0,   // not ours: G4 refuses exactly as it does today
    kAdoptStaleOurs = 1,   // a PDE naming a page inside THIS boot's reservation
};

struct RootAdoptInputs {
    uint64_t slot511;                // what root[511] reads now
    uint64_t reservedLo, reservedHi; // THIS boot's ringmap carve, exclusive top
    uint32_t ringMapBuilt;           // G6's precondition must already hold
    uint32_t armTransition;          // the mapVA zero -> non-zero transition fired
};

static inline uint32_t rootwrite_adopt_check(const struct RootAdoptInputs *in)
{
    if (!in)                                   return kAdoptRefuse;
    // Hook path only. The verb can never reach this.
    if (!in->armTransition)                    return kAdoptRefuse;
    if (!in->ringMapBuilt)                     return kAdoptRefuse;
    if (in->reservedHi <= in->reservedLo)      return kAdoptRefuse;
    // It must decode as a DIRECTORY PDE of exactly the shape we write. A stale entry
    // that is not that shape is not ours to reason about.
    if (!(in->slot511 & N48_PTE_VALID_BIT))    return kAdoptRefuse;
    if (in->slot511 & N48_PTE_P_BIT)           return kAdoptRefuse;  // a 64 KiB leaf
    if (in->slot511 & N48_PTE_SYSTEM_BIT)      return kAdoptRefuse;  // sysmem
    if (((in->slot511 >> 58) & 0x1full) != 4ull) return kAdoptRefuse; // BFS must be 4
    // THE CLAUSE THAT CARRIES THIS PREDICATE: the target is inside OUR reservation.
    {
        const uint64_t t = in->slot511 & N48_PTE_ADDR_MASK;
        if (t < in->reservedLo || t >= in->reservedHi) return kAdoptRefuse;
    }
    return kAdoptStaleOurs;
}

// The root PDE this increment writes: our L1 block as a DIRECTORY pointer.
//
// CONFIRMED twice, independently:
//   * our own gfx12 self-test, gmc_v12_0.cpp, writes its root as
//     `(tbl_pa + kVMFragL1Off) | kVMFragBFS4 | PTEFlags::VALID` with
//     kVMFragBFS4 = (4ull << 58) and bit 63 CLEAR — the encoding proved on
//     this silicon ("64K leaf ok (0 dw wrong)");
//   * Apple's own live root[0] read 0x10000003d6c01001 (r80,), which decodes
//     as address 0x3d6c01000 | (4 << 58) | VALID, P = 0.
// Both agree, so this is Apple's encoding and ours at once. P MUST stay clear:
// the walker treats a P-set entry as a 64 KiB leaf (see vmib's decode), and a
// leaf here would map the ring VA to the L1 block itself.
#define N48_ROOT_PDE_BFS4  (4ull << 58)

static inline uint64_t rootwrite_root_pde(uint64_t l1Off)
{
    return (l1Off & N48_PTE_ADDR_MASK) | N48_ROOT_PDE_BFS4 | N48_PTE_VALID_BIT;
}

#endif // NAVI48_ROOTWRITE_GUARDS_H
