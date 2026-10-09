#!/bin/zsh
# native_g1_plant.sh - planted breaks for tests/native_g1_test.cpp (kext 0.0.623, GPU-apps G1: hang recovery).
# For each plant: copy the real sources into a scratch tree, apply ONE break to the REAL code (the script first proves the break changed the text),
# compile the real test against the scratch tree and demand that it FAILS (a compile error counts). A plant the suite lets through is a hole: the
# script exits non-zero and says which.
#   run from anywhere:  src/navi48-bringup/tests/native_g1_plant.sh
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
K=src/navi48-bringup
SCR="${TMPDIR:-/tmp}/g1-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0

fresh() {   # the scratch tree: exactly the files native_g1_test.cpp reads
  rm -rf "$SCR"; mkdir -p "$SCR/$K/src/amd" "$SCR/$K/tests"
  cp "$ROOT/$K/src/Navi48Bringup.cpp" "$ROOT/$K/src/Navi48UserClient.cpp" "$ROOT/$K/src/Navi48NativeClient.cpp" "$SCR/$K/src/"
  cp "$ROOT/$K/src/amd/"native_g1_pure.h "$ROOT/$K/src/amd/"native_s1c.cpp "$ROOT/$K/src/amd/"native_s1c.h "$ROOT/$K/src/amd/"mes_v12_1.cpp "$ROOT/$K/src/amd/"amdgpu_mes.h "$SCR/$K/src/amd/"
  cp "$ROOT/$K/Info.plist" "$SCR/$K/"
  cp "$ROOT/$K/tests/native_g1_test.cpp" "$SCR/$K/tests/"
}
build_run() {   # -> sets OUT, returns 0 pass / 1 test failed / 2 compile failed
  OUT=$(cd "$SCR" && clang++ -std=c++17 -Wall -Wextra -Werror -O0 -I $K/src -I $K/src/amd $K/tests/native_g1_test.cpp -o "$SCR/t" 2>&1) || return 2
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
P=$K/src/amd/native_g1_pure.h
E=$K/src/amd/native_s1c.cpp
B=$K/src/Navi48Bringup.cpp
U=$K/src/Navi48UserClient.cpp
N=$K/src/Navi48NativeClient.cpp
M=$K/src/amd/mes_v12_1.cpp
MH=$K/src/amd/amdgpu_mes.h

# ---- the latch is cleared only after verification ----
plant 1  $P '        const uint64_t t1 = e.now_ns();
        const ProbeSub ps = e.submit_probe(firstIdx + i);' '        e.clear_latch();
        const uint64_t t1 = e.now_ns();
        const ProbeSub ps = e.submit_probe(firstIdx + i);' "BREAK A: HUNG is cleared BEFORE the probe is verified"
plant 2  $P 'constexpr bool may_clear(uint32_t attStatus, bool retired, bool dataOk) { return attStatus == kAttVerified && retired && dataOk; }' 'constexpr bool may_clear(uint32_t attStatus, bool retired, bool dataOk) { (void)attStatus; (void)dataOk; return retired; }' "may_clear accepts a retired probe whose data is wrong"
plant 3  $P '            a.dataOk = a.retired && e.probe_data() == ps.magic;' '            a.dataOk = a.retired;' "the probe's readback is never checked"
plant 4  $P '            e.clear_latch();
            r.cleared = true;
            r.outcome = kRecRecovered;
            e.close_test("g1 hangrecover: recovered");' '            e.close_test("g1 hangrecover: recovered");
            e.clear_latch();
            r.cleared = true;
            r.outcome = kRecRecovered;' "the context is closed before HUNG is cleared (the close would leak as HUNG)"
plant 5  $E '        } else {
            G1Env e{ &ctx };
            const RecoverRep r = recover_run(e, (uint32_t)arg, gG1.attempts);' '        } else {
            gHang.hung = false;
            G1Env e{ &ctx };
            const RecoverRep r = recover_run(e, (uint32_t)arg, gG1.attempts);' "the kernel clears HUNG before running any method"
plant 6  $P '            if ((method == kMethAuto && a.method == kMethMesReset) || a.actRc == kMesTimeoutRc) break;
            continue;' '' "an unacked method still probes (and may clear)"
# ---- hangtest with a WindowServer session ----
plant 7  $P '    if (sessionIsWs) return kTestWsSession;' '    if (sessionIsWs && false) return kTestWsSession;' "BREAK B (pure): hangtest no longer refuses a WindowServer session by name"
plant 8  $E '        const bool ws = open && __atomic_load_n(&gSessWs, __ATOMIC_SEQ_CST) != 0u;' '        const bool ws = false;' "BREAK B (kernel): hangtest never sees the WindowServer note"
plant 9  $N '	const IOReturn rc = amdgpu::n1c_open(*owner->bringupContext(), uc->wsSession, &ref);' '	const IOReturn rc = amdgpu::n1c_open(*owner->bringupContext(), false, &ref);' "the native client never notes a WindowServer session (0.0.627: the note is the open's ws argument)"
plant 10 $E '    if (s->ws) __atomic_store_n(&gSessWs, 0u, __ATOMIC_SEQ_CST);   // 0.0.623 (G1): no WindowServer session any more
' '' "n1c_close leaves a stale WindowServer note"
plant 11 $P '    if (sessionOpen) return kTestSessionOpen;' '    if (sessionOpen && false) return kTestSessionOpen;' "hangtest runs while an administrator session is open"
plant 12 $P '    if (hung) return kTestHung;' '    if (hung && false) return kTestHung;' "hangtest runs while HUNG"
plant 13 $E '        const bool open = __atomic_load_n(&gOpenFlag, __ATOMIC_SEQ_CST) != 0u;' '        const bool open = false;' "the kernel never tells hangtest a session is open"
# ---- unbounded waits ----
plant 14 $P '        if (el >= boundNs) return pred();' '        if (el >= boundNs && false) return pred();' "BREAK C (pure): wait_until loses its bound"
plant 15 $E '    void latch_hang() {
' '    void latch_hang() {
        for (;;) { if (retired() >= gG1.hungSeq) break; IOSleep(1); }
' "BREAK C (kernel): an unbounded wait in the G1 block"
plant 16 $P '    if (wait_until(e, [&]() { return e.retired() >= ps.seq; }, kHangConfirmNs)) {' '    if (wait_until(e, [&]() { return e.retired() >= ps.seq; }, ~0ull)) {' "the hang confirmation has no bound"
plant 17 $P '            a.retired = wait_until(e, [&]() { return e.retired() >= ps.seq; }, kVerifyNs);' '            a.retired = wait_until(e, [&]() { return e.retired() >= ps.seq; }, 100ull * kVerifyNs);' "the verification wait is 100x its bound"
# ---- order and plan ----
plant 18 $P '    e.latch_hang();
    t.outcome = kTestHeld;' '    t.outcome = kTestHeld;' "hangtest never latches HUNG"
plant 19 $P '    t.seq = ps.seq;
    if (wait_until(' '    t.seq = ps.seq;
    e.latch_hang();
    if (wait_until(' "hangtest latches HUNG before the bounded confirmation"
plant 20 $P '    return method == kMethAuto ? Plan{ 2u, { kMethMesReset, kMethRemap } }' '    return method == kMethAuto ? Plan{ 2u, { kMethRemap, kMethMesReset } }' "auto tries the more invasive remap first"
plant 21 $P '    return method == kMethAuto ? Plan{ 2u, { kMethMesReset, kMethRemap } }' '    return method == kMethAuto ? Plan{ 2u, { kMethRelease, kMethRemap } }' "auto includes the release control"
plant 22 $P '        const ProbeSub ps = e.submit_probe(firstIdx + i);' '        const ProbeSub ps = e.submit_probe(i + 0u * firstIdx);' "attempt numbers restart at 0 (probe magics repeat)"
plant 23 $P '        if (a.method == kMethMesReset && a.actRc == 0u) resetAcked = true;' '        if (a.method == kMethMesReset && a.actRc == 0u) resetAcked = false;' "an ACKED reset is not reported to remap as acked"
plant 24 $P '    if (!testHolds) return kRecNoTest;' '    if (!testHolds && false) return kRecNoTest;' "hangrecover runs on a hang no test context holds"
plant 25 $P '    if (attempts + planLen > kMaxAttempts) return kRecTooMany;' '    if (attempts + planLen > kMaxAttempts && false) return kRecTooMany;' "the attempt budget is gone"
# ---- containment in the kernel block ----
plant 26 $E '            if (r != kIOReturnSuccess) return (uint32_t)r;    // the queue'"'"'s state is unknown: no re-init, no ADD_QUEUE' '            (void)0;    // the queue'"'"'s state is unknown: no re-init, no ADD_QUEUE' "remap re-initialises and re-adds the queue after an UNACKED unmap"
plant 27 $E '            IOLockLock(gCliLock);
            g1_scrub_ring();
            IOLockUnlock(gCliLock);
' '' "mesreset no longer scrubs the ring outside the live range"
plant 28 $E '    uint32_t n = 0;
    if (gWc - gG1.liveStart <= (uint64_t)cp.ring_size_dwords) {' '    uint32_t n = 0;
    WREG32(*gCtx->dev, 0x0F40u, 0u);
    if (gWc - gG1.liveStart <= (uint64_t)cp.ring_size_dwords) {' "a register write appears in the G1 block"
plant 29 $E '    void latch_hang() {
' '    void latch_hang() {
        gHang.hung = true;
' "the hang is latched without the client path (no announce)"
# ---- admission ----
plant 30 $P 'constexpr uint32_t latch_value(bool present, uint32_t value) { return (present && value == 1u) ? kLatchOn : kLatchOff; }' 'constexpr uint32_t latch_value(bool present, uint32_t value) { (void)present; (void)value; return kLatchOn; }' "the switch defaults ON"
plant 31 $B '		if (!n48native::g1::native_exempt(amdgpu::n1c_g1_latched_on(), action, argScalar)) return kIOReturnBadArgument;
' '' "accelExperiment runs the G1 verbs without the latch / argument gate"
plant 32 $U '	if (!n48native::g1::action_admitted(amdgpu::n1c_g1_latched_on(), action) && !n48native::g2::action_admitted(amdgpu::n1c_multi_latched_on(), action)) {' '	if (!n48native::g1::is_g1_action(action) && !n48native::g2::action_admitted(amdgpu::n1c_multi_latched_on(), action)) {' "the user client opens 100..102 with the switch OFF"
plant 33 $P '    return (a == kActHangTest && arg == (uint64_t)kModeWaitMem) ||' '    return (a == kActHangTest && arg <= 2ull) ||' "hangtest accepts unknown modes"
# ---- MES frames and the version ----
plant 34 $MH 'constexpr uint32_t kResetFlag_reset_legacy_gfx = 1u << 3;' 'constexpr uint32_t kResetFlag_reset_legacy_gfx = 1u << 2;' "the RESET flag is hang_detect_only (bit 2), not reset_legacy_gfx"
plant 35 $M '    return mes_remove_hw_queue_flags(dev, mes, queue_type, pipe_id, queue_id, doorbell_offset,
                                     kRemoveQueueFlag_unmap_legacy_queue);' '    return mes_remove_hw_queue_flags(dev, mes, queue_type, pipe_id, queue_id, doorbell_offset,
                                     kRemoveQueueFlag_preempt_legacy_gfx);' "the teardown REMOVE_QUEUE frame changes"
plant 36 $K/Info.plist '<key>CFBundleVersion</key>
	<string>0.0.664</string>' '<key>CFBundleVersion</key>
	<string>0.0.621</string>' "the version is not bumped"
plant 37 $P '    o[4] = kWaitRefNever;' '    o[4] = 0u;' "the WAIT_REG_MEM reference is the zero the poll dword holds (the test IB would not hang)"

# ---- 0.0.626: the G1 review's F1..F3 ----
plant 38 $M '    const uint64_t fence_value = fence_seq;' '    const uint64_t fence_value = 1;' "F1: the fence value is the constant 1 again"
plant 39 $M '        if ((v & 0xFFFFFFFFull) == fence_value) {' '        if ((v & 0xFFFFFFFFull) == 1) {' "F1: the wait compares against a literal 1, not this frame's value"
plant 40 $M '& 0x7FFFFFFFu; } while (fence_seq == 0u);' '& 0x7FFFFFFFu; } while (0);' "F1: the sequence may be 0 (the cleared slot) after a wrap"
plant 41 $M '    do { fence_seq = __atomic_add_fetch(&gMesFenceSeq, 1u, __ATOMIC_SEQ_CST) & 0x7FFFFFFFu; } while (fence_seq == 0u);' '    do { fence_seq = (gMesFenceSeq) & 0x7FFFFFFFu; } while (fence_seq == 0u);' "F1: the sequence is not incremented"
plant 42 $P '(method == kMethAuto && a.method == kMethMesReset) || ' '' "F2: auto goes on to remap after a failed RESET"
plant 43 $P '    if (method == kMethRemap && !resetAcked) return kRecRemapNeedsReset;
' '    (void)resetAcked;
' "F2: a standalone remap is allowed without an acked RESET"
plant 44 $E '            gG1.resetAcked = (r == kIOReturnSuccess);' '            if (r == kIOReturnSuccess) gG1.resetAcked = true;' "F2: resetAcked stays true after a later failed RESET"
plant 45 $E 'p.n, mes_timeout_seen(), gG1.resetAcked)' 'p.n, mes_timeout_seen(), true)' "F2: the decision is told the RESET was always acked"
plant 46 $P '    if (mesTimedOut && uses_mes(method)) return kRecMesTimedOut;
' '    (void)mesTimedOut;
' "F3: MES methods are allowed after a MES timeout"
plant 47 $P 'return method == kMethAuto || method == kMethMesReset ||' 'return method == kMethMesReset ||' "F3: auto is not treated as a MES method"
plant 48 $M '    __atomic_store_n(&gMesTimeoutSeen, 1u, __ATOMIC_SEQ_CST);   // 0.0.626 (F3): sticky for the boot
' '' "F3: a MES timeout does not set the sticky flag"
plant 49 $E 'p.n, mes_timeout_seen(), gG1.resetAcked)' 'p.n, false, gG1.resetAcked)' "F3: the decision is not told about the timeout"
plant 50 $P '|| a.actRc == kMesTimeoutRc) break;' ') break;' "F3: a MES timeout in a standalone method does not end the run"
plant 51 $E 'and they clear the HUNG latch after a verified probe. REGISTER WRITES' 'and they clear the HUNG latch after a verified probe. No MMIO register write. REGISTER WRITES' "the false 'No MMIO register write' claim is back"

echo "PLANTS: $total run (baseline included), $escaped escaped or broken"
[ $escaped -eq 0 ]
