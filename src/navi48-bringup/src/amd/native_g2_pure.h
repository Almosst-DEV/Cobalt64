//
//  native_g2_pure.h - the PURE half of GPU-apps stage G2 (kext 0.0.627; 0.0.669: the POOLED-HANDLE model): several native client sessions at once,
//  each with its own address space and its own budgets. No kernel header is included: tests/native_g2_test.cpp compiles this file and drives the very decision
//  functions the kext calls (amd/native_s1c.cpp, Navi48NativeClient.cpp).
//
//  DEFAULT OFF: boot-arg navi48-multisession=1, latched once (latch_value). With it absent EVERY decision below collapses to the 0.0.626 behaviour:
//  one session, VMID 8, a second open refused with ExclusiveAccess, no VRAM budget, a stall always latches HUNG, the stat verb (103) does not exist.
//
//  With it ON:
//    * UP TO kMaxClients CLIENTS (0.0.669; it was kMaxSessions = 8 until 0.0.668, because a client WAS a VMID). Each client has its OWN page-table root /
//      address space (its VM context) from its open to its close, whatever happens to the VMID.
//    * VMIDs are a POOL of kVmidPoolSize HANDLES (VMIDs 8..15, the hardware's 8 native contexts). A handle is assigned when the client's work has to run in
//      the GPU (vmid_pick: a free handle, else one stolen from a client whose work is provably retired - Apple's assignVMID / waitForVMID split), released
//      when that client closes or is preempted, and re-assigned to the same client later: its tree is untouched in between, so a re-assignment is one
//      CONTEXT<n> write plus one TLB flush. "8" now bounds HOW MANY ADDRESS SPACES ARE LIVE IN THE GPU AT ONCE - the hardware's account - and NOT how many
//      clients may exist. uid-88 (WindowServer) opens exactly as before (only when NO session is open; 0.0.641, user-application role ON: whenever a record
//      is free, with the last free record reserved for it - see open_pick); administrators (root) may open the extra sessions. A G1 test context holding the
//      GPU refuses every open.
//    * a client whose close could NOT prove its own work retired (HUNG, a timed-out own wait, a park that did not flush) is QUARANTINED: its record and its
//      VMID handle are HELD (the GPU may still be reading that tree) but they are NOT lost. The RECLAIM pass (reclaim_pick, run by open and by the residency
//      path) hands them back the moment own_idle proves the work retired, or after the G1-style recovery has proven it with its probe. 0.0.627's permanent
//      "DEAD for the boot" - which is what made capacity shrink monotonically across a long uptime - no longer exists.
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

using s1c::kOk; using s1c::kExclusive; using s1c::kAborted; using s1c::kBadArg; using s1c::kNoMemory; using s1c::kNotReady;

// ---- the switch ---------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kLatchUnset = 0u, kLatchOff = 1u, kLatchOn = 2u;
constexpr uint32_t latch_value(bool present, uint32_t value) { return (present && value == 1u) ? kLatchOn : kLatchOff; }
constexpr bool latch_is_on(uint32_t latch) { return latch == kLatchOn; }

// ---- the VMID POOL and the CLIENT TABLE (0.0.669: Apple-shaped pooled handles) ---------------------------------------------------------------------
// These two are SEPARATE things, and that is the whole point of this revision: "how many clients at once" is not capped by the hardware's
// 8 native VMIDs, because a client holds one only while it has work to run:
//
//   * kVmidPoolSize = 8 POOL HANDLES, VMIDs 8..15 (16 VMIDs in all; VMID 0 is kernel-only and MES owns 1..7 through the 0xFE mask: native_s1b_pure.h).
//     A handle is a HANDLE ON A LIVE ADDRESS SPACE, not an identity. It is ASSIGNED when a client's work has to run in the GPU (vmid_pick: the lowest free
//     handle, else one STOLEN from a client whose own work is provably retired - the assignVMID / waitForVMID split of the Apple driver), RELEASED when that
//     client closes or is preempted (program_root parks the context, the handle goes back to the pool), and re-assigned to the SAME client later - its
//     page-table root is untouched in between, so a re-assignment is one CONTEXT<n> write plus one TLB flush. "8" therefore bounds THE NUMBER OF ADDRESS
//     SPACES LIVE IN THE GPU AT ONCE (the hardware's account) and NOT the number of clients.
//
//   * kMaxClients = 64 records in the CLIENT TABLE. A record is KERNEL MEMORY (sizeof(Session); native_s1c.cpp static_asserts the whole table against
//     kClientKernelBudget), allocated on first use and RECYCLED through a free list, so it is not a GPU resource at all. What limits one client is its own
//     VRAM / GTT / page-table / import budget (G2 R4/R6, G4); what limits all of them together is the global system-memory cap (6 GiB, sys_budget_ok) and
//     the shared page-table block reserve. 64 is a MEMORY bound, not a GPU one: kMaxClients * sizeof(Session) has to stay inside kClientKernelBudget.
constexpr uint32_t kVmidPoolSize = 8u;               // the pool: the address spaces that can be live in the GPU at once (the HARDWARE maximum, VMIDs 8..15)
constexpr uint32_t kMaxSessions  = kVmidPoolSize;    // the 0.0.627 name, kept: it always meant "the pool", and the pool is what the stat pages walk
constexpr uint32_t kMaxClients   = 64u;              // the client table: bounded by MEMORY, not by the GPU (kClientKernelBudget / sizeof(Session))
constexpr uint64_t kClientKernelBudget = 64ull << 20;   // the client table's kernel-memory ceiling (static_asserted in native_s1c.cpp)
constexpr uint32_t vmid_of_handle(uint32_t h) { return kNativeVmid + h; }   // pool handle -> VMID
static_assert(vmid_of_handle(0u) == 8u && vmid_of_handle(kVmidPoolSize - 1u) == 15u, "VMID 8 for pool handle 0, 9..15 for the others");
constexpr bool all_pool_vmids_ok() { for (uint32_t i = 0; i < kVmidPoolSize; i++) if (!vmid_ok(vmid_of_handle(i))) return false; return true; }
static_assert(all_pool_vmids_ok(), "every pool VMID is in 8..15 and outside the MES mask");
static_assert(kMaxClients <= 0xFDu, "a client index must stay clear of kOwnerKernel (0xFE) in the seqno owner log");
static_assert(kVmidPoolSize <= kMaxClients, "there can never be more live address spaces than there are clients");

constexpr uint32_t kNoSlot   = 0xFFFFFFFFu;   // no handle / no record
constexpr uint32_t kNoClient = 0xFFFFFFFFu;   // the pool entry is FREE (no client holds that handle)

// The state of a CLIENT TABLE RECORD (0.0.669: this replaces the old three-state "slot", and the difference matters).
enum CliState : uint8_t {
    kCliFree        = 0u,   // the record is on the free list: the next open may take it
    kCliOpen        = 1u,   // a client owns it
    kCliQuarantined = 2u,   // its close could NOT prove its own work retired (or its park failed): the record AND its VMID handle are HELD, because the GPU may
                            // still be reading that tree. 0.0.627 called this state DEAD and kept it for the whole boot, so every unproven close cost one of the
                            // 8 handles for good - the monotonic capacity leak this revision removes. Here the RECLAIM pass (reclaim_pick) hands the record and
                            // its handle back the moment own_idle proves the work retired (or the G1-style recovery's probe has proven it).
};
struct CliPick { uint32_t rc; uint32_t cli; };
// The record an open gets. state[] has `n` entries (the caller's table size); openCount = clients open now; ws = the caller was admitted by the uid-88 rule;
// g1Holds = a G1 test context holds the GPU. OFF: record 0 only, and only when nothing is open (the caller's compare-and-swap has already made that true).
// ON: never while a G1 test holds; WindowServer only when nothing is open (exactly the open policy's kReasonWsAlreadyOpen, re-checked here under the lock
// because the policy's view can be stale); otherwise the lowest FREE record - a QUARANTINED one is skipped, the reclaim pass runs first; none free =
// ExclusiveAccess.
// 0.0.641 (G4 review): with the user-application role ON (apps = navi48-apps AND navi48-multisession) a WindowServer open is admitted whenever a record is
// free and no WindowServer session is open (wsOpen), whatever else is open, and the LAST free record is reserved for it: a non-WindowServer open (apps, root
// tools, a G1 test context) is refused while no WindowServer session is open and only one free record is left. apps = false (the default): exactly the
// 0.0.640 / 0.0.632 rule above.
inline uint32_t free_slots(const uint8_t *state, uint32_t n) { uint32_t k = 0; for (uint32_t i = 0; i < n; i++) if (state[i] == kCliFree) k++; return k; }
inline CliPick open_pick(bool multi, const uint8_t *state, uint32_t n, uint32_t openCount, bool ws, bool g1Holds, bool apps = false, bool wsOpen = false) {
    if (!multi) return (openCount == 0u && n > 0u && state[0] == kCliFree) ? CliPick{ kOk, 0u } : CliPick{ kExclusive, kNoSlot };
    if (g1Holds) return CliPick{ kExclusive, kNoSlot };
    if (apps) {
        if (ws ? wsOpen : (!wsOpen && free_slots(state, n) <= 1u)) return CliPick{ kExclusive, kNoSlot };
    } else if (ws && openCount != 0u) return CliPick{ kExclusive, kNoSlot };
    for (uint32_t i = 0; i < n; i++) if (state[i] == kCliFree) return CliPick{ kOk, i };
    return CliPick{ kExclusive, kNoSlot };
}
// The open policy's "a WindowServer client is already open" input (native_open_policy_pure.h open_decision's alreadyOpen). OFF: any native client (as 0.0.640). apps ON: only a WindowServer session.
constexpr bool ws_already_open(bool apps, bool anyOpen, bool wsOpen) { return apps ? wsOpen : anyOpen; }
// The record's state after its close: FREE - the record goes back to the free list AND its VMID handle back to the pool - only when its context was parked AND
// its own work was proven retired; QUARANTINED otherwise. 0.0.669: a quarantine is TEMPORARY (reclaim_pick), where 0.0.627's DEAD was permanent.
constexpr uint8_t cli_after_close(bool parked, bool ownIdle) { return (parked && ownIdle) ? (uint8_t)kCliFree : (uint8_t)kCliQuarantined; }
// THE RECLAIM PASS (0.0.669). A quarantined record is HELD, never lost: idle[] parallels state[] (idle[i] = own_idle(globalRetired, that client's own last
// seqno), computed by the kernel for the quarantined entries) and the first record whose work HAS retired is given back - its VMID parked and released, its
// record returned to the free list. THIS is what makes the available resources non-monotonic (there is no "never handed out again this boot" any more).
// Returns the record to reclaim, or kNoSlot.
inline uint32_t reclaim_pick(const uint8_t *state, const bool *idle, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (state[i] == kCliQuarantined && idle[i]) return i;
    return kNoSlot;
}
// ---- the VMID pool (0.0.669) -------------------------------------------------------------------------------------------------------------------
struct VmidPick { uint32_t rc; uint32_t handle; uint32_t victim; };
// Assign a pool handle to a client that needs one. owner[i] = the client index holding handle i, or kNoClient. idle[i] = that client's own work is provably
// retired (own_idle(globalRetired, client's own last seqno)). usable = a bitmask of handles that carry the native context geometry (ctx_prepare runs once per
// handle at boot; a handle that failed it is never handed out). The order is the Apple driver's assignVMID followed by waitForVMID: the lowest FREE usable
// handle; else STEAL the handle of a usable entry whose client is idle - that client simply re-acquires at its next Submit, its tree / BOs / mappings are
// untouched; else NotReady, and the CALLER decides whether to wait (bounded) and retry: a bound that expires is a stall the hang clock judges, never a
// permanent loss. victim = the client whose handle is being taken (kNoClient when a free handle was used).
inline VmidPick vmid_pick(const uint32_t *owner, const bool *idle, uint32_t usable) {
    for (uint32_t i = 0; i < kVmidPoolSize; i++) if (((usable >> i) & 1u) != 0u && owner[i] == kNoClient) return VmidPick{ kOk, i, kNoClient };
    for (uint32_t i = 0; i < kVmidPoolSize; i++) if (((usable >> i) & 1u) != 0u && idle[i]) return VmidPick{ kOk, i, owner[i] };
    return VmidPick{ kNotReady, kNoSlot, kNoClient };
}

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
struct OwnerEnt { uint64_t seq; uint64_t startWc; uint32_t sessId; uint8_t slot; };   // slot = the CLIENT index (0.0.669; it was a session slot before)
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
// multi: the switch. guilty: guilty_of's verdict. userSession: the guilty CLIENT record is open, carries the same session id, and its client said Hello (a G1
// test context never does). guiltyIsWs: that session was admitted by the uid-88 rule. mesTimedOut: mes_timeout_seen(). g1Holds: a G1 test context holds the
// GPU. The kernel recovers on its own ONLY for a non-WindowServer user session; everything else latches HUNG exactly as before.
constexpr uint32_t stall_action(bool multi, bool guiltyKnown, uint8_t guiltySlot, bool userSession, bool guiltyIsWs, bool mesTimedOut, bool g1Holds) {
    if (!multi || !guiltyKnown || guiltySlot >= kMaxClients || !userSession || guiltyIsWs || mesTimedOut || g1Holds) return kStallLatch;
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
// 0.0.669: page 0..kVmidPoolSize-1 is ONE POOL HANDLE each (VMID 8 + the page number: the handle's owner, or FREE); page kStatGlobal is the global page;
// page kStatClients is the NEW client-table page. The old pages keep their numbers and their global page's layout, so a reader that only knows
// `sessstat 0..8` still works - the client table is added, never folded into an existing page.
constexpr uint32_t kStatGlobal  = kMaxSessions;          // page kVmidPoolSize: the global page (the 0.0.627 number, unchanged)
constexpr uint32_t kStatClients = kMaxSessions + 1u;     // 0.0.669: the client table
constexpr bool args_ok(uint32_t a, uint64_t arg) { return a == kActSessStat && arg <= (uint64_t)kStatClients; }
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
// One POOL HANDLE's page (0.0.669). vmid = 8 + the page number; free = no client holds that handle; cli / id / the counters describe the client that does.
struct SessView { uint8_t state; bool ws, hello, poisoned, free = false; bool app = false; uint32_t vmid, id, cli; uint64_t vram, gtt, imported, ptBytes, lastSeq; uint32_t nBos; uint64_t faultAtOpen; };
struct GlobalView { uint32_t open; bool hung; uint64_t emitted, retired, othersVram, othersCap, recoveries, lastGuiltySeq, collateralTotal; uint32_t lastGuiltySlot, quarantined, records, freeList, handlesFree, handlesHeld; uint64_t faultNow, faultPrev; uint32_t mappedLeakBos = 0; uint64_t appVisBytes = 0; uint32_t appVisCapMiB = 0; };
// 0.0.641 (G4 review): sessstat 4 o[2] bits 36..55 = visible-VRAM KiB held by non-WindowServer sessions (user applications' pool, apps ON), bits 56..63 = its cap in MiB (navi48-appvis, default 64). Both 0 with apps OFF.
constexpr uint64_t appvis_bits(uint64_t visBytes, uint32_t capMiB) { return (((visBytes >> 10) & 0xFFFFFull) << 36) | ((uint64_t)(capMiB & 0xFFu) << 56); }
// flag bits: 0..1 the record state, 0x100 WindowServer, 0x200 Hello, 0x400 poisoned, 0x800 APP, 0x1000 (0.0.669) THE POOL HANDLE IS FREE (no client holds it)
constexpr uint64_t kSessFlagFree = 0x1000ull;
constexpr uint64_t sess_flags(const SessView &v) { return (uint64_t)v.state | (v.ws ? 0x100ull : 0ull) | (v.hello ? 0x200ull : 0ull) | (v.poisoned ? 0x400ull : 0ull) | (v.app ? 0x800ull : 0ull) | (v.free ? kSessFlagFree : 0ull); }   /* 0.0.640 (G4): 0x800 = an allow-listed user application (APP) */
inline void sess_out(uint32_t page, const SessView &v, uint64_t *o) {
    o[0] = kStatOk; o[1] = page; o[2] = sess_flags(v); o[3] = v.vmid; o[4] = v.id; o[5] = v.vram; o[6] = v.gtt; o[7] = v.imported; o[8] = v.ptBytes;
    o[9] = v.lastSeq; o[10] = v.nBos; o[11] = v.faultAtOpen; o[12] = v.free ? (uint64_t)kNoClient : (uint64_t)v.cli;
}
// 0.0.669: the global page. Its layout is UNCHANGED (a reader that knows `sessstat 8` keeps working); bits 8..15 are now the QUARANTINED count, which is
// exactly what the 0.0.627 "dead" count meant - records held back from the table - except that a quarantine ends by itself (reclaim_pick) where DEAD was
// permanent. The pool's own accounting is reported on the NEW client page (cli_out), never folded into this one.
inline void global_out(const GlobalView &g, uint64_t *o) {
    o[0] = kStatOk; o[1] = kStatGlobal; o[2] = (uint64_t)g.open | ((uint64_t)g.quarantined << 8) | (g.hung ? 0x10000ull : 0ull) | ((uint64_t)(g.mappedLeakBos & 0xFFFFu) << 20) | appvis_bits(g.appVisBytes, g.appVisCapMiB);   /* 0.0.640 (G4): bits 20..35 = BOs leaked at close by a surviving CPU mapping (0 until it happens) */ o[3] = g.emitted; o[4] = g.retired;
    o[5] = g.othersVram; o[6] = g.othersCap; o[7] = g.recoveries; o[8] = g.lastGuiltySlot; o[9] = g.lastGuiltySeq; o[10] = g.collateralTotal; o[11] = g.faultNow; o[12] = g.faultPrev;
}
// 0.0.669: the NEW client-table page (kStatClients), so nothing had to move on the pages above. o[2] = open | quarantined << 8 | records allocated << 16 |
// kMaxClients << 24; o[3] = the free-list length (records on it, i.e. reusable now); o[4] = kVmidPoolSize (the live-address-space limit); o[5] = the pool
// handles free right now; o[6] = the pool handles held (quarantined included: a held handle is not lost, but it is not available either); o[7] = the
// quarantined records. Everything else 0.
inline void cli_out(const GlobalView &g, uint64_t *o) {
    for (uint32_t i = 0; i < kOutN; i++) o[i] = 0;
    o[0] = kStatOk; o[1] = kStatClients;
    o[2] = (uint64_t)g.open | ((uint64_t)g.quarantined << 8) | ((uint64_t)g.records << 16) | ((uint64_t)kMaxClients << 24);
    o[3] = g.freeList; o[4] = kVmidPoolSize; o[5] = g.handlesFree; o[6] = g.handlesHeld; o[7] = g.quarantined;
}

} // namespace g2
} // namespace n48native
