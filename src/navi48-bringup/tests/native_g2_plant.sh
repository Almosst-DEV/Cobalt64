#!/bin/zsh
# native_g2_plant.sh - planted breaks for tests/native_g2_test.cpp (kext 0.0.627, GPU-apps G2: several native sessions).
# For each plant: copy the real sources into a scratch tree, apply ONE break to the REAL code (the script first proves the break changed the text),
# compile the real test against the scratch tree and demand that it FAILS (a compile error counts). A plant the suite lets through is a hole: the
# script exits non-zero and says which.
#   run from anywhere:  src/navi48-bringup/tests/native_g2_plant.sh
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
K=src/navi48-bringup
SCR="${TMPDIR:-/tmp}/g2-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0

fresh() {   # the scratch tree: the kext sources, the plist, the test and the CLI
  rm -rf "$SCR"; mkdir -p "$SCR/$K/tests" "$SCR/tools/pc"
  cp -R "$ROOT/$K/src" "$SCR/$K/"
  cp "$ROOT/$K/Info.plist" "$SCR/$K/"
  cp "$ROOT/$K/tests/native_g2_test.cpp" "$SCR/$K/tests/"
  cp "$ROOT/tools/pc/navi48test.c" "$SCR/tools/pc/"
}
build_run() {   # -> sets OUT, returns 0 pass / 1 test failed / 2 compile failed
  OUT=$(cd "$SCR" && clang++ -std=c++17 -Wall -Wextra -Werror -O0 -I $K/src -I $K/src/amd $K/tests/native_g2_test.cpp -o "$SCR/t" 2>&1) || return 2
  OUT=$(cd "$SCR" && ./t . 2>&1) && return 0
  return 1
}
baseline() {
  fresh; total=$((total+1))
  build_run; local rc=$?
  if [ $rc -ne 0 ]; then echo "BASELINE: the unmodified scratch copy does not pass (rc $rc): $(echo "$OUT" | /usr/bin/grep -m1 -E 'error|^FAIL' | cut -c1-140)"; escaped=$((escaped+1)); return; fi
  echo "BASELINE: the unmodified scratch copy compiles and passes ($(echo "$OUT" | tail -1))"
}
plant() {   # plant <id> <file relative to repo root> <old text> <new text> <description>
  local id="$1" file="$2" old="$3" new="$4" desc="$5"
  fresh
  OLD="$old" NEW="$new" FILE="$SCR/$file" python3 - <<'PY'
import os,sys
p=os.environ['FILE']; s=open(p).read(); o=os.environ['OLD']; n=os.environ['NEW']
if s.count(o)!=1:
    print("PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %r" % (s.count(o), o[:70])); sys.exit(3)
open(p,'w').write(s.replace(o,n))
PY
  local rc=$?
  total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  build_run; rc=$?
  if [ $rc -eq 2 ]; then echo "PLANT $id: $desc: CAUGHT at compile time: $(echo "$OUT" | /usr/bin/grep -m1 -E 'error|static assertion' | cut -c1-140)"; return; fi
  if [ $rc -eq 0 ]; then echo "PLANT $id: $desc: *** ESCAPED (the suite passed) ***"; escaped=$((escaped+1)); return; fi
  echo "PLANT $id: $desc: CAUGHT by $(echo "$OUT" | /usr/bin/grep -c '^FAIL') check(s); first: $(echo "$OUT" | /usr/bin/grep -m1 '^FAIL' | cut -c1-120)"
}

baseline
P=$K/src/amd/native_g2_pure.h
E=$K/src/amd/native_s1c.cpp
N=$K/src/Navi48NativeClient.cpp
D=$K/src/amd/native_disp_pure.h

# ---- the brief's five ----
plant 1  $E '    const uint64_t own = __atomic_load_n(&s->emitted, __ATOMIC_ACQUIRE);' '    const uint64_t own = emitted_now();' "BREAK 1: the own wait waits on the GLOBAL emitted count instead of the session's own last seqno"
plant 2  $P 'constexpr uint8_t slot_after_close(bool parked, bool ownIdle) { return (parked && ownIdle) ? (uint8_t)kSlotFree : (uint8_t)kSlotDead; }' 'constexpr uint8_t slot_after_close(bool parked, bool ownIdle) { (void)parked; (void)ownIdle; return (uint8_t)kSlotFree; }' "BREAK 2 (pure): a VMID is handed out again while its work may still be in use"
plant 3  $E '    gSlotState[s->slot] = slot_after_close(parked, ownIdle);' '    gSlotState[s->slot] = kSlotFree;' "BREAK 2 (kernel): close frees the slot (VMID) even after a leak"
plant 4  $P '    for (uint32_t i = 0; i < kMaxSessions; i++) if (state[i] == kSlotFree) return OpenPick{ kOk, i };' '    for (uint32_t i = 0; i < kMaxSessions; i++) if (state[i] != kSlotOpen) return OpenPick{ kOk, i };' "BREAK 2 (pure): open_pick reuses a DEAD slot"
plant 5  $E '    __atomic_store_n(&gs->poisoned, 1u, __ATOMIC_SEQ_CST);
' '' "BREAK 3: the recovery never poisons the guilty session"
plant 6  $E '    if (s != nullptr && sess_poisoned(s)) return n48native::g2::poison_rc(true);
' '' "BREAK 3: a poisoned session passes the GPU gate (its submits are accepted)"
plant 7  $P 'constexpr uint32_t poison_rc(bool poisoned) { return poisoned ? kAborted : kOk; }' 'constexpr uint32_t poison_rc(bool poisoned) { (void)poisoned; return kOk; }' "BREAK 3 (pure): poisoning returns success"
plant 8  $P '    if (!multi) return (openCount == 0u && state[0] == kSlotFree) ? OpenPick{ kOk, 0u } : OpenPick{ kExclusive, kNoSlot };' '    if (!multi) return (state[0] == kSlotFree || state[1] == kSlotFree) ? OpenPick{ kOk, state[0] == kSlotFree && openCount == 0u ? 0u : 1u } : OpenPick{ kExclusive, kNoSlot };' "BREAK 4 (pure): the OFF path admits a second session"
plant 9  $E '    if (!multi && !OSCompareAndSwap(0, 1, &gOpenFlag)) {' '    if (!multi && !OSCompareAndSwap(0, 1, &gOpenFlag) && false) {' "BREAK 4 (kernel): the OFF compare-and-swap no longer refuses a second open"
plant 10 $N '	if (opened) { n48disp_on_ws_client_closed(wsSession); amdgpu::n1c_unmap_for_close(amdgpu::N1cRef{ sessSlot, sessId }, this); amdgpu::n1c_close(amdgpu::N1cRef{ sessSlot, sessId }, "clientClose");' '	if (opened) { n48disp_on_ws_client_closed(!adminClient); amdgpu::n1c_close(amdgpu::N1cRef{ sessSlot, sessId }, "clientClose");' "BREAK 5: the close-disarm is keyed on 'not admin' again"
plant 11 $D 'constexpr bool ws_close_disarms(bool latchOn, bool closingIsWsSession) { return latchOn && closingIsWsSession; }' 'constexpr bool ws_close_disarms(bool latchOn, bool closingIsWsSession) { return latchOn && !closingIsWsSession; }' "BREAK 5 (pure): the rule disarms on the OTHER sessions' close"
# ---- attribution and the decision ----
plant 12 $P '    const uint64_t seq = retired + 1ull;
    const OwnerEnt &x' '    const uint64_t seq = retired;
    const OwnerEnt &x' "the guilty seqno is the last RETIRED one (off by one)"
plant 13 $P '    if (!multi || !guiltyKnown || guiltySlot >= kMaxSessions || !userSession || guiltyIsWs || mesTimedOut || g1Holds) return kStallLatch;' '    if (!multi || !guiltyKnown || guiltySlot >= kMaxSessions || !userSession || (guiltyIsWs && false) || mesTimedOut || g1Holds) return kStallLatch;' "WindowServer's own hang is auto-recovered"
plant 14 $P '    if (!multi || !guiltyKnown || guiltySlot >= kMaxSessions || !userSession || guiltyIsWs || mesTimedOut || g1Holds) return kStallLatch;' '    if ((!multi && false) || !guiltyKnown || guiltySlot >= kMaxSessions || !userSession || guiltyIsWs || mesTimedOut || g1Holds) return kStallLatch;' "the switch OFF still auto-recovers"
plant 15 $E '    const uint32_t act = stall_action_budgeted(multi_on(), g.known, g.slot, user, ws, mes_timeout_seen(), g1_holds(), tried);' '    const uint32_t act = stall_action_budgeted(multi_on(), g.known, g.slot, user, false, mes_timeout_seen(), g1_holds(), tried);' "the kernel never tells the decision the guilty session is WindowServer's"
plant 16 $E '    if (r == kIOReturnSuccess) {
        uint32_t dw[8];' '    if (true) {
        uint32_t dw[8];' "the probe runs (and may 'recover') after an UNACKED RESET"
plant 17 $E '    n48native::g2::owner_note(gOwners, seq, ownerSlot, ownerId, startWc);
' '' "the ring no longer records the owner of a seqno"
# ---- VMIDs, waits, budget ----
plant 18 $E 'va, by, nIbs, s->vmid, hasFence,
                                    fenceAddr, gCtx->cp.wb_bus + kWbOffsetNativeSeq, seq);
    if (n == 0u) { IOLockUnlock(gRingLock); return' 'va, by, nIbs, kNativeVmid, hasFence,
                                    fenceAddr, gCtx->cp.wb_bus + kWbOffsetNativeSeq, seq);
    if (n == 0u) { IOLockUnlock(gRingLock); return' "every session's IB runs on VMID 8"
plant 19 $E '    const uint32_t rrc = n48native::g2::own_wait_resolve(target, em, __atomic_load_n(&s->emitted, __ATOMIC_ACQUIRE), &res);' '    const uint32_t rrc = n48native::g2::own_wait_resolve(target, em, em, &res);' "WaitSeq LAST waits for every session's work"
plant 20 $P '    return !multi || isWs || !s1c::would_exceed(othersHeld, add, others_vram_cap(vramTotal));' '    return !multi || (isWs && false) || !s1c::would_exceed(othersHeld, add, others_vram_cap(vramTotal));' "WindowServer is charged against the apps' budget"
plant 21 $E '            if (!n48native::g2::vram_budget_ok(multi, s->ws, gVramOthers, p.size, vram_total())) {' '            if (false && !n48native::g2::vram_budget_ok(multi, s->ws, gVramOthers, p.size, vram_total())) {' "BoCreate ignores the VRAM budget"
plant 22 $P 'constexpr uint32_t latch_value(bool present, uint32_t value) { return (present && value == 1u) ? kLatchOn : kLatchOff; }' 'constexpr uint32_t latch_value(bool present, uint32_t value) { (void)present; (void)value; return kLatchOn; }' "the switch defaults ON"
plant 23 $E '        if (!stalled && el >= kWaitCapNs) {
            if (!multi_on()) { hang_from_wait(); return false; }   // switch OFF: exactly the 0.0.626 idle wait (latch, not idle)' '        if (!stalled && el >= kWaitCapNs) {' "the OFF own wait no longer latches HUNG at 2 s"
plant 24 $E '    if (!multi || __atomic_load_n(&gScanOwner, __ATOMIC_ACQUIRE) == s->id) { uint64_t tr[2]; scan_teardown(s, how, tr);' '    if (true) { uint64_t tr[2]; scan_teardown(s, how, tr);' "any session's close takes the scanout plane back (WindowServer's included)"
# ---- 0.0.629: the G2 review fixes R1..R6 ----
plant 26 $E '    IOLockUnlock(sl);   // R1: through the local' '    IOLockUnlock(s->lock);   // R1: through the local' "R1: close re-reads the Session's lock after its last gCliLock release"
plant 27 $E '        n48native::g2::sess_wipe_keep_lock(s);' '        bzero(s, sizeof(Session));' "R1: open wipes the whole record, lock included"
plant 28 $P '    __builtin_memset(b, 0, (unsigned long)(l - b));' '    __builtin_memset(b, 0, (unsigned long)(e - b));' "R1 (pure): the keep-lock wipe zeroes the lock field too"
plant 29 $E '        mine = hang_expired(gHang, re, em, now);   // R2' '        mine = true;   // R2' "R2: a ring wait's 2 s blames the oldest seqno's owner regardless of progress"
plant 30 $P 'busy ? (uint32_t)kBoundBusy : hangExpired ? (uint32_t)kBoundStalled : (uint32_t)kBoundNoBlame;' 'busy ? (uint32_t)kBoundBusy : hangExpired ? (uint32_t)kBoundStalled : (uint32_t)kBoundStalled;' "R2 (pure): no-progress is not required for blame"
plant 31 $E '            if (stall_from_wait() == n48native::g2::kBoundNoBlame) {' '            if (stall_from_wait() == 99u) {' "R2: own_wait ignores the no-blame verdict (keeps waiting / blames later)"
plant 32 $E '            if (noBlame) {' '            if (false) {' "R2: BoFree drops (leaks) the BO after a no-blame Timeout"
plant 33 $P 'constexpr bool recovery_budget_left(uint64_t attemptsSoFar) { return attemptsSoFar < kMaxAutoRecoveries; }' 'constexpr bool recovery_budget_left(uint64_t attemptsSoFar) { (void)attemptsSoFar; return true; }' "R3 (pure): no automatic-recovery budget"
plant 34 $E '    const uint64_t tried = gG2.recoveries + gG2.failed;' '    const uint64_t tried = 0ull;' "R3: the kernel never feeds the attempts to the budget"
plant 35 $P 'return !multi || isWs || !s1c::would_exceed(allHeld, add, kSysGlobalCap); }' 'return !multi || isWs || allHeld == 0ull || !s1c::would_exceed(allHeld, add, kSysGlobalCap); }' "R4 (pure): the global cap is not enforced once memory is held"
plant 36 $E '            if (!n48native::g2::sys_budget_ok(multi, s->ws, gSysHeld, p.size)) {' '            if (false) {' "R4: BoCreate (GTT) ignores the global cap"
plant 37 $E '        if (!n48native::g2::sys_budget_ok(multi_on(), s->ws, gSysHeld, size)) {' '        if (false) {' "R4: import ignores the global cap"
plant 38 $E '        if (b.kind == kBoGtt) { sysmem_free(b.sm); sys_release(b); }' '        if (b.kind == kBoGtt) sysmem_free(b.sm);' "R4: a closing session's GTT pages stay counted forever"
plant 39 $E 'aLo = hub_rd(0x15d2), aHi' 'aLo = hub_rd(0x15d4), aHi' "R5: the wrong fault-address register"
plant 40 $E '        g.faultPrev = gFaultPrev; gFaultPrev = g.faultNow;' '        g.faultPrev = g.faultNow;' "R5: the previous sessstat word is never kept (a change cannot be seen)"
plant 41 $E '        s->faultAtOpen = fault_snap("open", slot);   // R5: read only' '' "R5: no fault word at session open"
plant 42 $P 'o[11] = g.faultNow; o[12] = g.faultPrev;' 'o[11] = g.faultNow; o[12] = 0;' "R5 (pure): the global page drops the previous word"
plant 43 $E '        if (mrc != kOk) {' '        if (false) {' "R6: BoFree frees a CPU-mapped visible-VRAM BO"
plant 44 $P 'descriptorRefs > 1u) ? kBusy : kOk;' 'descriptorRefs > 2u) ? kBusy : kOk;' "R6 (pure): one live mapping is not seen"
plant 45 $E '    if (!multi_on()) { hang_from_wait(); return n48native::g2::kBoundStalled; }' '    if (!multi_on()) { return n48native::g2::kBoundNoBlame; }' "R2: the switch OFF no longer latches at the ring-wait bound"
plant 25 $K/Info.plist '<key>CFBundleVersion</key>
	<string>0.0.664</string>' '<key>CFBundleVersion</key>
	<string>0.0.626</string>' "the version is not bumped"

echo "native_g2_plant: $((total - 1)) plants (+ the baseline), $escaped escaped or did not apply"
[ $escaped -eq 0 ]
