//
//  native_g2_pure.h - the PURE half of GPU-apps stage G2 (kext 0.0.627): several native client sessions at once (an internal design note section 1
//  and section 4, G2). No kernel header is included: tests/native_g2_test.cpp compiles this file and drives the very decision functions the kext calls
//  (amd/native_s1c.cpp, Navi48NativeClient.cpp).
//
//  DEFAULT OFF: boot-arg navi48-multisession=1, latched once (latch_value). With it absent EVERY decision below collapses to the 0.0.626 behaviour:
//  one session (slot 0, VMID 8), a second open refused with ExclusiveAccess, no VRAM budget, a stall always latches HUNG, the stat verb (103) does not exist.
//
//  With it ON:
//    * up to kMaxSessions sessions; slot i runs on VMID 8 + i (8 for the first / WindowServer, 9..11 for the others). uid-88 (WindowServer) opens exactly as
//      before (only when NO session is open; 0.0.641, user-application role ON: whenever a slot is free, with the last free slot reserved for it - see open_pick); administrators (root) may open the extra sessions. A G1 test context holding the GPU refuses every open.
//    * a slot whose close could not prove its VMID idle (HUNG, a timed-out own wait, a park that did not flush) is DEAD for the boot: its VMID is never
//      handed out again (it may still be in use by the GPU).
//    * waits are per session: retired >= this session's own last seqno (the ring is in order, so that is sufficient).
//    * every seqno records its owner; on a stall the OLDEST unretired seqno names the guilty session. If that session is a user session (Hello done) that is
//      NOT WindowServer's, and no MES frame ever timed out, and no G1 test holds the GPU, the kernel runs the G1 recovery (MES RESET) on its own, proves it
//      with a kernel-only probe fence, and then POISONS the guilty session (its submits are refused, its waits return Aborted) while the others continue.
//      Everything else (WindowServer guilty, owner unknown, MES timed out, the RESET not acknowledged, the probe not retiring) is today's HUNG latch.
//    * non-WindowServer sessions together may hold at most 1/kOthersVramDiv (25 %) of the VRAM pools; WindowServer's own allocations are not limited.
//
#pragma once
#include <stdint.h>

#include "native_s1c_pure.h"   // the return codes, would_exceed, kNativeVmid / vmid_ok (via native_s1b_pure.h)

namespace n48native {
namespace g2 {

using s1c::kOk; using s1c::kExclusive; using s1c::kAborted; using s1c::kBadArg; using s1c::kNoMemory;

// ---- the switch ---------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kLatchUnset = 0u, kLatchOff = 1u, kLatchOn = 2u;
constexpr uint32_t latch_value(bool present, uint32_t value) { return (present && value == 1u) ? kLatchOn : kLatchOff; }
constexpr bool latch_is_on(uint32_t latch) { return latch == kLatchOn; }

// ---- slots and VMIDs ------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMaxSessions = 4u;
constexpr uint32_t vmid_of_slot(uint32_t slot) { return kNativeVmid + slot; }
static_assert(vmid_of_slot(0) == 8u && vmid_of_slot(kMaxSessions - 1u) == 11u, "VMID 8 for slot 0, 9..11 for the others");
static_assert(vmid_ok(vmid_of_slot(0)) && vmid_ok(vmid_of_slot(1)) && vmid_ok(vmid_of_slot(2)) && vmid_ok(vmid_of_slot(3)), "every session VMID is outside the MES mask (8..15)");
// How many slots a boot may use: 1 with the switch OFF (exactly the 0.0.626 single session), kMaxSessions with it ON.
constexpr uint32_t slots_in_use(bool multi) { return multi ? kMaxSessions : 1u; }

enum SlotState : uint8_t { kSlotFree = 0u, kSlotOpen = 1u, kSlotDead = 2u };
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
struct OpenPick { uint32_t rc; uint32_t slot; };
// The slot an open gets. state[] has kMaxSessions entries. openCount = sessions open now. ws = the caller was admitted by the uid-88 rule. g1Holds = a G1 test
// context holds the GPU. OFF: slot 0 only, and only when nothing is open (the caller's compare-and-swap has already made that true; this re-states it).
// ON: never while a G1 test holds; WindowServer only when nothing is open (exactly the open policy's kReasonWsAlreadyOpen, re-checked here under the lock
// because the policy's view can be stale); otherwise the lowest FREE slot (a DEAD one is never reused); none free = ExclusiveAccess.
// 0.0.641 (G4 review): with the user-application role ON (apps = navi48-apps AND navi48-multisession) a WindowServer open is admitted whenever a slot is free and no WindowServer session is
// open (wsOpen), whatever else is open (an app that outlives a WindowServer restart no longer denies WindowServer its device), and the LAST free slot is reserved for it: a non-WindowServer
// open (apps, root tools, a G1 test context) is refused while no WindowServer session is open and only one free slot is left. apps = false (the default): exactly the 0.0.640 / 0.0.632 rule above.
inline uint32_t free_slots(const uint8_t *state) { uint32_t n = 0; for (uint32_t i = 0; i < kMaxSessions; i++) if (state[i] == kSlotFree) n++; return n; }
inline OpenPick open_pick(bool multi, const uint8_t *state, uint32_t openCount, bool ws, bool g1Holds, bool apps = false, bool wsOpen = false) {
    if (!multi) return (openCount == 0u && state[0] == kSlotFree) ? OpenPick{ kOk, 0u } : OpenPick{ kExclusive, kNoSlot };
    if (g1Holds) return OpenPick{ kExclusive, kNoSlot };
    if (apps) {
        if (ws ? wsOpen : (!wsOpen && free_slots(state) <= 1u)) return OpenPick{ kExclusive, kNoSlot };
    } else if (ws && openCount != 0u) return OpenPick{ kExclusive, kNoSlot };
    for (uint32_t i = 0; i < kMaxSessions; i++) if (state[i] == kSlotFree) return OpenPick{ kOk, i };
    return OpenPick{ kExclusive, kNoSlot };
}
// The open policy's "a WindowServer client is already open" input (native_open_policy_pure.h open_decision's alreadyOpen). OFF: any native client (as 0.0.640). apps ON: only a WindowServer session.
constexpr bool ws_already_open(bool apps, bool anyOpen, bool wsOpen) { return apps ? wsOpen : anyOpen; }
// The slot's state after its close: FREE (its VMID may be handed out again) only when its context was parked AND its own work was proven retired; else DEAD.
constexpr uint8_t slot_after_close(bool parked, bool ownIdle) { return (parked && ownIdle) ? (uint8_t)kSlotFree : (uint8_t)kSlotDead; }

// ---- per-session waits -------------------------------------------------------------------------------------------------------------------------------
// The ring runs in order: once the global retired seqno reaches this session's own last seqno, every submission of this session has retired.
constexpr bool own_idle(uint64_t retired, uint64_t ownLast) { return retired >= ownLast; }
// WaitSeq's target. LAST = this session's own last seqno (0.0.626: the global emitted count, which with one session is the same number). A seqno above the
// global emitted count was never issued: BadArg. Any issued seqno is accepted (in-order retirement makes another session's seqno a harmless target).
inline uint32_t own_wait_resolve(uint64_t target, uint64_t globalEmitted, uint64_t ownLast, uint64_t *resolved) {
    if (target == N48N_SEQ_LAST) { *resolved = ownLast; return kOk; }
    if (target > globalEmitted) return kBadArg;
    *resolved = target;
    return kOk;
}

// ---- seqno ownership and the guilty session -----------------------------------------------------------------------------------------------------------
constexpr uint32_t kOwnerN = 1024u;          // > the most submissions the 4096-dword ring can hold (each is >= 15 dwords: <= 273 in flight)
constexpr uint8_t kOwnerKernel = 0xFEu;      // a kernel-only probe fence (the recovery's), never a session
struct OwnerEnt { uint64_t seq; uint64_t startWc; uint32_t sessId; uint8_t slot; };
struct OwnerLog { OwnerEnt e[kOwnerN]; };
inline void owner_note(OwnerLog &l, uint64_t seq, uint8_t slot, uint32_t sessId, uint64_t startWc) { l.e[seq % kOwnerN] = OwnerEnt{ seq, startWc, sessId, slot }; }
struct Guilty { bool known; uint8_t slot; uint32_t sessId; uint64_t seq; uint64_t startWc; };
// The owner of the OLDEST unretired seqno (retired + 1). Unknown when nothing is outstanding, when more than kOwnerN are outstanding (the entry was
// overwritten), or when the entry does not carry that very seqno.
inline Guilty guilty_of(const OwnerLog &l, uint64_t retired, uint64_t emitted) {
    if (retired >= emitted || emitted - retired > (uint64_t)kOwnerN) return Guilty{ false, 0u, 0u, 0ull, 0ull };
    const uint64_t seq = retired + 1ull;
    const OwnerEnt &x = l.e[seq % kOwnerN];
    if (x.seq != seq) return Guilty{ false, 0u, 0u, seq, 0ull };
    return Guilty{ true, x.slot, x.sessId, seq, x.startWc };
}

// ---- what a stall does ----------------------------------------------------------------------------------------------------------------------------------
enum StallAct : uint32_t { kStallLatch = 0u, kStallRecover = 1u };
// multi: the switch. guilty: guilty_of's verdict. userSession: the guilty slot is open, carries the same session id, and its client said Hello (a G1 test
// context never does). guiltyIsWs: that session was admitted by the uid-88 rule. mesTimedOut: mes_timeout_seen(). g1Holds: a G1 test context holds the GPU.
// The kernel recovers on its own ONLY for a non-WindowServer user session; everything else latches HUNG exactly as before.
constexpr uint32_t stall_action(bool multi, bool guiltyKnown, uint8_t guiltySlot, bool userSession, bool guiltyIsWs, bool mesTimedOut, bool g1Holds) {
    if (!multi || !guiltyKnown || guiltySlot >= kMaxSessions || !userSession || guiltyIsWs || mesTimedOut || g1Holds) return kStallLatch;
    return kStallRecover;
}
// 0.0.629 (G2 review fix R3): at most kMaxAutoRecoveries automatic recoveries per boot (attempts: acknowledged or not). After that every stall latches HUNG.
constexpr uint64_t kMaxAutoRecoveries = 3ull;
constexpr bool recovery_budget_left(uint64_t attemptsSoFar) { return attemptsSoFar < kMaxAutoRecoveries; }
// The full decision the kernel's stall_handle takes: stall_action AND the per-boot budget.
constexpr uint32_t stall_action_budgeted(bool multi, bool guiltyKnown, uint8_t guiltySlot, bool userSession, bool guiltyIsWs, bool mesTimedOut, bool g1Holds, uint64_t attemptsSoFar) {
    return recovery_budget_left(attemptsSoFar) ? stall_action(multi, guiltyKnown, guiltySlot, userSession, guiltyIsWs, mesTimedOut, g1Holds) : (uint32_t)kStallLatch;
}
// 0.0.629 (G2 review fix R2): a RING wait (an own-seqno wait or the ring-space wait) reached its 2 s bound, counted from the start of THAT wait. With the switch ON
// nobody is blamed for that alone: the stall path runs only when the hang clock (hang_expired: no change of the retired seqno for kHangNs, the clock restarted by
// every observed advance and by the kick of an idle ring) has expired. busy = HUNG is latched or another call already runs a recovery (the waiter then keeps
// waiting up to the recovery's bound). Otherwise the waiter returns Timeout and NOBODY is reset or poisoned.
enum BoundVerdict : uint32_t { kBoundNoBlame = 0u, kBoundStalled = 1u, kBoundBusy = 2u };
constexpr uint32_t bound_verdict(bool busy, bool hangExpired) { return busy ? (uint32_t)kBoundBusy : hangExpired ? (uint32_t)kBoundStalled : (uint32_t)kBoundNoBlame; }
// The recovery counts only when the MES acknowledged the RESET, the kernel probe fence was emitted, and it retired within the verify bound.
constexpr bool recovery_ok(bool resetAcked, bool probeEmitted, bool probeRetired) { return resetAcked && probeEmitted && probeRetired; }
constexpr uint64_t kVerifyNs = 1000000000ull;   // the probe is one RELEASE_MEM: it retires in microseconds on a healthy queue (G1 measured 32..101 us)
// After a recovery the guilty session is poisoned: every later Submit is refused and every Wait (and every GPU-dependent call) returns Aborted.
constexpr uint32_t poison_rc(bool poisoned) { return poisoned ? kAborted : kOk; }
// Ctx QUERY_STATE2 for a session: RESET | GUILTY when the GPU is HUNG (0.0.626) or this session was poisoned.
constexpr uint64_t ctx_state2(bool hung, bool poisoned) { return (hung || poisoned) ? (uint64_t)(N48N_CTX_QUERY2_RESET | N48N_CTX_QUERY2_GUILTY) : 0ull; }
// Submissions of OTHER sessions that were queued behind the guilty one when the RESET was sent ("collateral"): their seqnos retire with the probe, but
// whether the CP executed or discarded them is not known (SUSPECTED either way; there is no replay). Counted for the stat page.
constexpr uint64_t collateral(uint64_t guiltySeq, uint64_t emittedAtReset) { return emittedAtReset > guiltySeq ? emittedAtReset - guiltySeq : 0ull; }

// ---- the global VRAM budget -------------------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kOthersVramDiv = 4ull;   // non-WindowServer sessions together: <= 25 % of the VRAM pools
constexpr uint64_t others_vram_cap(uint64_t vramTotal) { return vramTotal / kOthersVramDiv; }
// OFF, or a WindowServer session: no budget (0.0.626 had none). Otherwise othersHeld + add must stay within the cap.
constexpr bool vram_budget_ok(bool multi, bool isWs, uint64_t othersHeld, uint64_t add, uint64_t vramTotal) {
    return !multi || isWs || !s1c::would_exceed(othersHeld, add, others_vram_cap(vramTotal));
}

// ---- 0.0.629 (G2 review fix R4): the global system-memory cap -------------------------------------------------------------------------------------------------
// GTT + host-import bytes of ALL sessions together (switch ON) stay within kSysGlobalCap. WindowServer's session keeps only its per-session caps (kGttCap,
// kImportCap) and is never refused by this cap, but what it holds counts towards the total; a NEW allocation of any other session that would take the total past
// the cap is refused (NoMemory). OFF: no global cap (one session: 512 MiB GTT + 4 GiB imports, below it anyway).
constexpr uint64_t kSysGlobalCap = 6ull << 30;
constexpr bool sys_budget_ok(bool multi, bool isWs, uint64_t allHeld, uint64_t add) { return !multi || isWs || !s1c::would_exceed(allHeld, add, kSysGlobalCap); }

// ---- 0.0.629 (G2 review fix R6): a CPU mapping must not outlive BoFree -----------------------------------------------------------------------------------------
// A visible-VRAM BO's CPU mapping is made from its ONE descriptor (Bo::vmd); every live user mapping (IOMemoryMap) holds a reference on it, the BO record holds
// one. With the switch ON, BoFree of such a BO while the descriptor has more than the record's own reference is REFUSED with Busy (nothing freed, the handle stays
// valid): the range would go back to the shared pool while another process could still read and write it. OFF: as 0.0.626 (no check). GTT BOs are not refused
// (their pages belong to the descriptor, which the mapping keeps alive), nor host imports (the caller's own memory).
constexpr uint32_t kBusy = 0xe00002d5u;   // kIOReturnBusy
constexpr uint32_t bofree_mapping_rc(bool multi, bool isVisVram, bool hasDescriptor, uint32_t descriptorRefs) {
    return (multi && isVisVram && hasDescriptor && descriptorRefs > 1u) ? kBusy : kOk;
}

// ---- 0.0.629 (G2 review fix R1): the open / close race ----------------------------------------------------------------------------------------------------------
// n1c_open re-initialises a session record under gCliLock but NOT under that session's lock (taking it there would invert the lock order), while a closing thread
// may still hold that lock. So the wipe never writes the record's lock field (the lock is allocated once with the slot and never replaced), and n1c_close
// releases the lock through a LOCAL copy taken before it last drops gCliLock. S is the session record type (the kernel's Session; a model in the tests).
template <class S> inline void sess_wipe_keep_lock(S *s) {
    unsigned char *const b = reinterpret_cast<unsigned char *>(s);
    unsigned char *const l = reinterpret_cast<unsigned char *>(&s->lock);
    unsigned char *const e = b + sizeof(S);
    __builtin_memset(b, 0, (unsigned long)(l - b));
    __builtin_memset(l + sizeof(s->lock), 0, (unsigned long)(e - (l + sizeof(s->lock))));
}

// ---- the stat verb (accel 103 `sessstat <0..4>`) -----------------------------------------------------------------------------------------------------------
constexpr uint32_t kActSessStat = 103u;
constexpr bool is_g2_action(uint32_t a) { return a == kActSessStat; }
constexpr bool action_admitted(bool latchOn, uint32_t a) { return latchOn && is_g2_action(a); }
constexpr uint32_t kStatGlobal = kMaxSessions;    // page 4: the global page; 0..3: one session each
constexpr bool args_ok(uint32_t a, uint64_t arg) { return a == kActSessStat && arg <= (uint64_t)kStatGlobal; }
constexpr bool native_exempt(bool latchOn, uint32_t a, uint64_t arg) { return action_admitted(latchOn, a) && args_ok(a, arg); }
constexpr uint32_t kOutN = 13u;
enum StatCode : uint32_t { kStatOk = 0u, kStatOff = 1u, kStatBadArg = 2u };
// 0.0.629 (G2 review fix R5): the GCVM L2 protection-fault registers, READ ONLY (never written, never cleared: the S1b self-test's VMID-8 fault stays latched
// until something clears it, so only a CHANGE tells a new fault). fault_word packs STATUS_LO32 (GFXHUB dword 0x15d0) | ADDR_LO32 (0x15d2) << 32, the two
// registers native_s1b.cpp's fault leg reads. Session page o[11]: the word at that session's open. Global page o[11]: the word now; o[12]: the word at the
// PREVIOUS sessstat 4 call (0 before the first). A new fault = o[11] differs from both.
constexpr uint64_t fault_word(uint32_t statusLo32, uint32_t addrLo32) { return (uint64_t)statusLo32 | ((uint64_t)addrLo32 << 32); }
constexpr uint32_t fault_word_vmid(uint64_t w) { return (uint32_t)((w >> 20) & 0xFu); }   // GCVM_L2_PROTECTION_FAULT_STATUS_LO32.VMID (bits 23:20, native_s1b_pure.h fault_status_vmid)
constexpr bool fault_changed(uint64_t before, uint64_t now) { return before != now; }
struct SessView { uint8_t state; bool ws, hello, poisoned; uint32_t vmid, id; uint64_t vram, gtt, imported, ptBytes, lastSeq; uint32_t nBos; uint64_t faultAtOpen; bool app = false; };
struct GlobalView { uint32_t open; bool hung; uint64_t emitted, retired, othersVram, othersCap, recoveries, lastGuiltySeq, collateralTotal; uint32_t lastGuiltySlot, dead; uint64_t faultNow, faultPrev; uint32_t mappedLeakBos = 0; uint64_t appVisBytes = 0; uint32_t appVisCapMiB = 0; };
// 0.0.641 (G4 review): sessstat 4 o[2] bits 36..55 = visible-VRAM KiB held by non-WindowServer sessions (user applications' pool, apps ON), bits 56..63 = its cap in MiB (navi48-appvis, default 64). Both 0 with apps OFF.
constexpr uint64_t appvis_bits(uint64_t visBytes, uint32_t capMiB) { return (((visBytes >> 10) & 0xFFFFFull) << 36) | ((uint64_t)(capMiB & 0xFFu) << 56); }
constexpr uint64_t sess_flags(const SessView &v) { return (uint64_t)v.state | (v.ws ? 0x100ull : 0ull) | (v.hello ? 0x200ull : 0ull) | (v.poisoned ? 0x400ull : 0ull) | (v.app ? 0x800ull : 0ull); }   /* 0.0.640 (G4): 0x800 = an allow-listed user application (APP) */
inline void sess_out(uint32_t slot, const SessView &v, uint64_t *o) {
    o[0] = kStatOk; o[1] = slot; o[2] = sess_flags(v); o[3] = v.vmid; o[4] = v.id; o[5] = v.vram; o[6] = v.gtt; o[7] = v.imported; o[8] = v.ptBytes;
    o[9] = v.lastSeq; o[10] = v.nBos; o[11] = v.faultAtOpen; o[12] = 0;
}
inline void global_out(const GlobalView &g, uint64_t *o) {
    o[0] = kStatOk; o[1] = kStatGlobal; o[2] = (uint64_t)g.open | ((uint64_t)g.dead << 8) | (g.hung ? 0x10000ull : 0ull) | ((uint64_t)(g.mappedLeakBos & 0xFFFFu) << 20) | appvis_bits(g.appVisBytes, g.appVisCapMiB);   /* 0.0.640 (G4): bits 20..35 = BOs leaked at close by a surviving CPU mapping (0 until it happens) */ o[3] = g.emitted; o[4] = g.retired;
    o[5] = g.othersVram; o[6] = g.othersCap; o[7] = g.recoveries; o[8] = g.lastGuiltySlot; o[9] = g.lastGuiltySeq; o[10] = g.collateralTotal; o[11] = g.faultNow; o[12] = g.faultPrev;
}

} // namespace g2
} // namespace n48native
