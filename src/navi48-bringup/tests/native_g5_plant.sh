#!/bin/zsh
# native_g5_plant.sh - planted breaks for tests/native_g5_test.cpp (kext 0.0.650, GPU-apps G5 Stage 1: the application credit).
# For each plant: copy the real sources into a scratch tree, apply ONE break to the REAL code (the script first proves the break changed the text),
# compile the real test against the scratch tree and demand that it FAILS (a compile error counts). A plant the suite lets through is a hole: the
# script exits non-zero and says which.
#   run from anywhere:  src/navi48-bringup/tests/native_g5_plant.sh
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
K=src/navi48-bringup
SCR="$(mktemp -d "${TMPDIR:-/tmp}/g5-plant.XXXXXX")"
case "$SCR" in */g5-plant.??????) ;; *) echo "bad scratch dir"; exit 2 ;; esac
trap 'find "$SCR" -mindepth 1 -delete 2>/dev/null; rmdir "$SCR" 2>/dev/null' EXIT
escaped=0; total=0

fresh() {   # the scratch tree: the kext sources, the plist, the test and the CLI
  find "$SCR" -mindepth 1 -delete; mkdir -p "$SCR/$K/tests" "$SCR/tools/pc"
  cp -R "$ROOT/$K/src" "$SCR/$K/"
  cp "$ROOT/$K/Info.plist" "$SCR/$K/"
  cp "$ROOT/$K/tests/native_g5_test.cpp" "$SCR/$K/tests/"
  cp "$ROOT/tools/pc/navi48test.c" "$SCR/tools/pc/"
}
build_run() {   # -> sets OUT, returns 0 pass / 1 test failed / 2 compile failed
  OUT=$(cd "$SCR" && clang++ -std=c++17 -Wall -Wextra -Werror -O0 -I $K/src -I $K/src/amd $K/tests/native_g5_test.cpp -o "$SCR/t" 2>&1) || return 2
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
P=$K/src/amd/native_g5_pure.h
E=$K/src/amd/native_s1c.cpp
H=$K/src/amd/native_s1c.h
B=$K/src/Navi48Bringup.cpp
C=tools/pc/navi48test.c
PL=$K/Info.plist
# ---- the pure rules ----
plant 1  $P '    if (!on || isWs) return kAdmit;' '    (void)on; (void)isWs; return kAdmit;' "PURE 1: admit always returns Admit (the gate is removed)"
plant 2  $P '        if (e.seq == q) {' '        if (e.seq != q) continue;
        {' "PURE 2: app_in_flight skips an entry that does not carry its seqno (fail OPEN)"
plant 3  $P '    if (wsActive && inUse >= K) return kWait;' '    if (wsActive && inUse > K) return kWait;' "PURE 3: the credit is off by one (>= becomes >)"
plant 4  $P '    if (!on || isWs) return kAdmit;' '    (void)isWs; if (!on) return kAdmit;' "PURE 4: the isWs early return is dropped (WindowServer can be made to wait)"
plant 5  $P '    if (!on || isWs) return kAdmit;' '    (void)on; if (isWs) return kAdmit;' "PURE 5 (= spec 8): on=false still gates"
plant 6  $P '    if (!s1c::ring_has_space(freeDw, needDw + reserveDw)) return kWait;' '    (void)reserveDw; if (!s1c::ring_has_space(freeDw, needDw)) return kWait;' "PURE 6: the ring reserve is ignored"
plant 7  $P 'constexpr uint32_t kReserveDw  = 1024u;' 'constexpr uint32_t kReserveDw  = 256u;' "PURE 7: the reserve is 256 dwords, not 25 %"
plant 8  $P '    if (olderWaiter) return kWait;' '    (void)olderWaiter;' "PURE 8: first come first served is ignored"
plant 9  $P '    if (wsActive && inUse >= K) return kWait;' '    (void)wsActive; if (inUse >= K) return kWait;' "PURE 9: the credit also binds when WindowServer is idle"
plant 10 $P '    return wsOpen && lastWsNs != 0ull && now >= lastWsNs && now - lastWsNs < windowNs;' '    return wsOpen && lastWsNs != 0ull && (now < lastWsNs || now - lastWsNs < windowNs);' "PURE 10: ws_active accepts a clock that went backwards"
plant 11 $P '    return wsOpen && lastWsNs != 0ull && now >= lastWsNs && now - lastWsNs < windowNs;' '    return wsOpen && lastWsNs != 0ull && now >= lastWsNs && now - lastWsNs <= windowNs;' "PURE 11: ws_active boundary (< becomes <=)"
plant 12 $P '    return wsOpen && lastWsNs != 0ull && now >= lastWsNs && now - lastWsNs < windowNs;' '    (void)wsOpen; return lastWsNs != 0ull && now >= lastWsNs && now - lastWsNs < windowNs;' "PURE 12: ws_active ignores whether a WindowServer session is open"
plant 13 $P '    if (emitted - retired > (uint64_t)g2::kOwnerN) return cap;
    uint32_t n = 0u;
    for (uint64_t q = retired + 1ull; q <= emitted; q++) {
        const g2::OwnerEnt &e = l.e[q % g2::kOwnerN];
        if (e.seq == q) {' '    if (emitted - retired > (uint64_t)g2::kOwnerN) return 0u;
    uint32_t n = 0u;
    for (uint64_t q = retired + 1ull; q <= emitted; q++) {
        const g2::OwnerEnt &e = l.e[q % g2::kOwnerN];
        if (e.seq == q) {' "PURE 13: app_in_flight returns 0 (not the cap) when the log wrapped"
plant 14 $P '            if (e.slot == g2::kOwnerKernel) continue;
            if (wsId != 0u && e.sessId == wsId) continue;' '            if (wsId != 0u && e.sessId == wsId) continue;' "PURE 14: a kernel probe fence counts as an app job"
plant 15 $P '            if (e.slot == g2::kOwnerKernel) continue;
            if (wsId != 0u && e.sessId == wsId) continue;' '            if (e.slot == g2::kOwnerKernel) continue;
            if (wsId != 0u && e.slot == 0u) continue;' "PURE 15 (= spec 7): WindowServer is identified by slot 0 instead of its session id"
plant 16 $P '            if (e.slot == g2::kOwnerKernel) continue;
            if (wsId != 0u && e.sessId == wsId) continue;' '            (void)wsId; if (e.slot == g2::kOwnerKernel) continue;' "PURE 16: WindowServer's own jobs count against the credit"
plant 17 $P '        if (t < mine || (t == mine && j < mySlot)) return true;' '        if (t < mine || (t == mine && j > mySlot)) return true;' "PURE 17: older_waiter tie goes to the higher slot"
plant 18 $P '        if (t == 0ull || t > now || now - t > staleNs) continue;' '        (void)staleNs; if (t == 0ull || t > now) continue;' "PURE 18: a leaked since[] entry never heals (stale entries are not ignored)"
plant 19 $P '        if (t == 0ull || t > now || now - t > staleNs) continue;' '        if (t == 0ull || (t <= now && now - t > staleNs)) continue;' "PURE 19: a since[] entry in the future is honoured"
plant 20 $P '    const uint64_t mine = (mySlot < kMaxSessions && since[mySlot] != 0ull) ? since[mySlot] : now;' '    const uint64_t mine = 0ull;' "PURE 20: a slot not waiting yet counts as the oldest"
plant 21 $P 'inline void since_mark(uint64_t *since, uint32_t slot, uint64_t now) { if (slot < kMaxSessions && since[slot] == 0ull) since[slot] = now; }' 'inline void since_mark(uint64_t *since, uint32_t slot, uint64_t now) { if (slot < kMaxSessions) since[slot] = now; }' "PURE 21: since_mark restarts the clock on every try (a waiter never gets older)"
plant 22 $P '    const uint32_t inUse = (a.on && !a.isWs && act) ? app_in_flight(log, a.retired, a.emitted, a.wsId, a.K) : 0u;' '    const uint32_t inUse = 0u; (void)log;' "PURE 22: the composite never counts the jobs in flight"
plant 23 $P '    const bool act = ws_active(a.wsOpen, a.lastWsNs, a.now, kWsWindowNs);' '    const bool act = false;' "PURE 23: the composite thinks WindowServer is never active"
plant 24 $P '    const bool older = (a.on && !a.isWs) ? older_waiter(a.since, a.slot, a.now, kSinceStaleNs) : false;' '    const bool older = false;' "PURE 24: the composite never looks for an older waiter"
plant 25 $P '    return elapsedNs >= kG5WaitBoundNs ? (uint32_t)kGiveUp : (!stallDone && elapsedNs >= s1c::kWaitCapNs) ? (uint32_t)kStallNow : (uint32_t)kKeepWaiting;' '    return elapsedNs > kG5WaitBoundNs ? (uint32_t)kGiveUp : (!stallDone && elapsedNs >= s1c::kWaitCapNs) ? (uint32_t)kStallNow : (uint32_t)kKeepWaiting;' "PURE 25: the wait bound is off by one"
plant 26 $P '    return elapsedNs >= kG5WaitBoundNs ? (uint32_t)kGiveUp : (!stallDone && elapsedNs >= s1c::kWaitCapNs) ? (uint32_t)kStallNow : (uint32_t)kKeepWaiting;' '    (void)stallDone; return elapsedNs >= kG5WaitBoundNs ? (uint32_t)kGiveUp : (elapsedNs >= s1c::kWaitCapNs) ? (uint32_t)kStallNow : (uint32_t)kKeepWaiting;' "PURE 26: the stall path runs on every loop, not once"
plant 27 $P 'constexpr bool on_effective(bool g5LatchOn, bool multiLatchOn) { return g5LatchOn && multiLatchOn; }' 'constexpr bool on_effective(bool g5LatchOn, bool multiLatchOn) { (void)multiLatchOn; return g5LatchOn; }' "PURE 27: navi48-g5 is effective without multisession"
plant 28 $P 'constexpr uint32_t credit_value(bool present, uint32_t v) { return (present && v >= 1u && v <= kCreditMax) ? v : kCreditDefault; }' 'constexpr uint32_t credit_value(bool present, uint32_t v) { return (present && v >= 1u) ? v : kCreditDefault; }' "PURE 28: navi48-g5credit is not clamped to 4"
plant 29 $P 'constexpr bool args_ok(bool g5on, uint32_t a, uint64_t arg) { return g5on && a == g2::kActSessStat && arg == (uint64_t)kStatPage; }' 'constexpr bool args_ok(bool g5on, uint32_t a, uint64_t arg) { (void)g5on; return a == g2::kActSessStat && arg == (uint64_t)kStatPage; }' "PURE 29: sessstat 5 exists with G5 OFF"
plant 30 $P '    if (waitedNs != 0ull) { s.delayed++;' '    if (true) { s.delayed++;' "PURE 30: every admit counts as delayed"
plant 31 $P 'o[4] = v.appInFlight; o[5] = v.wsOutstanding;' 'o[4] = v.wsOutstanding; o[5] = v.appInFlight;' "PURE 31: the stat page swaps o[4] and o[5]"
plant 32 $P 'constexpr uint64_t kSinceStaleNs = kG5WaitBoundNs + 1000000000ull;' 'constexpr uint64_t kSinceStaleNs = 1000000000ull;' "PURE 32: since[] entries go stale after 1 s (a legitimate 6 s wait would be forgotten)"
# ---- the kernel ----
plant 40 $E '        if (admission(gOwners, gG5In) == kAdmit) {' '        if (true) {' "KERNEL 40: ring_submit_app_g5 never asks the admission"
plant 41 $E '            since_clear(gG5Since, s->slot); waiting = false;
' '            since_clear(gG5Since, s->slot); waiting = false;
            IOLockUnlock(gRingLock); IOLockLock(gRingLock);
' "KERNEL 41: the admission verdict and the seqno are taken in two holds of gRingLock"
plant 42 $E '        since_mark(gG5Since, s->slot, now); waiting = true; t0 = gG5Since[s->slot];
        IOLockUnlock(gRingLock);' '        since_mark(gG5Since, s->slot, now); waiting = true; t0 = gG5Since[s->slot];
        IOSleep(1);
        IOLockUnlock(gRingLock);' "KERNEL 42 (= spec 5): the sleep runs inside the lock"
plant 43 $E '        since_clear(gG5Since, s->slot);
        if (timedOut) stat_timeout(gG5);' '        if (timedOut) stat_timeout(gG5);' "KERNEL 43 (= spec 6): the exit does not clear gG5Since (a timed-out waiter leaks its entry)"
plant 44 $E '        else if (s->ws) { __atomic_store_n(&gWsLastNs,' '        else if (s->slot == 0u) { __atomic_store_n(&gWsLastNs,' "KERNEL 44 (= spec 7): n1c_submit identifies WindowServer by slot 0"
plant 45 $E '        if (!g5_on()) er = ring_submit(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut);   // G5 OFF: exactly 0.0.642' '        if (false) er = ring_submit(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut);   // G5 OFF: exactly 0.0.642' "KERNEL 45: with G5 OFF the app path is still taken"
plant 46 $E '        else er = ring_submit_app_g5(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut);' '        else er = ring_submit(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut);' "KERNEL 46: apps never reach the admission (n1c_submit calls ring_submit for them)"
plant 47 $E '        else if (s->ws) { __atomic_store_n(&gWsLastNs, now_ns(), __ATOMIC_RELEASE); er = ring_submit(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut); }' '        else if (s->ws) { er = ring_submit_app_g5(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut); }' "KERNEL 47: WindowServer goes through the app admission (and is never stamped)"
plant 48 $E '        if (step == kGiveUp) { timedOut = true; rc = kTimeout; break; }' '        if (step == kGiveUp) { timedOut = true; rc = kOk; break; }' "KERNEL 48: the 8 s bound returns success without a seqno"
plant 49 $E '        if (hung_now()) { rc = hang_rc(true, false); break; }
        if (sess_poisoned(s)) { rc = n48native::g2::poison_rc(true); break; }
        const uint64_t el' '        const uint64_t el' "KERNEL 49: a waiting app ignores HUNG and the poison"
plant 50 $E '        if (step == kStallNow) { stallDone = true; (void)stall_from_wait(); continue; }' '        if (step == kStallNow) { stallDone = true; continue; }' "KERNEL 50: the stall path (and so G2's recovery of a wedged app that holds the credit) never runs"
plant 51 $E 'gG5In = n48native::g5::AdmitIn{ true, false, wsId != 0u,' 'gG5In = n48native::g5::AdmitIn{ true, true, wsId != 0u,' "KERNEL 51: the app path tells the admission the caller is WindowServer"
plant 52 $E '            if (er == kOk) stat_admit(gG5, waitedNs);' '            (void)waitedNs;' "KERNEL 52: the admission counters are never updated"
plant 53 $E '    if (s->ws) __atomic_store_n(&gWsId, 0u, __ATOMIC_SEQ_CST);     // 0.0.650 (G5)
' '' "KERNEL 53: WindowServer's session id is not cleared at close"
plant 54 $E '        if (ws) __atomic_store_n(&gWsId, s->id, __ATOMIC_SEQ_CST);   // 0.0.650 (G5): WindowServer is found by its session id, never by its slot
' '' "KERNEL 54: WindowServer's session id is never recorded"
plant 55 $E '    return n48native::g5::on_effective(n48native::g2::latch_is_on(l), multi_on());' '    return n48native::g2::latch_is_on(l);' "KERNEL 55: g5_on() ignores navi48-multisession"
plant 56 $E '        gG5Credit = n48native::g5::credit_value(present, v);' '        gG5Credit = v;' "KERNEL 56: navi48-g5credit is used unclamped"
plant 57 $E '        v.appInFlight = app_in_flight(gOwners, re, em, wsId, n48native::g2::kOwnerN);
        v.wsOutstanding = ws_outstanding(gOwners, re, em, wsId);
        v.st = gG5;
        IOLockUnlock(gRingLock);' '        IOLockUnlock(gRingLock);
        v.appInFlight = app_in_flight(gOwners, re, em, wsId, n48native::g2::kOwnerN);
        v.wsOutstanding = ws_outstanding(gOwners, re, em, wsId);
        v.st = gG5;' "KERNEL 57: the stat page reads the owner log and counters after dropping gRingLock"
plant 59 $E 'constexpr uint64_t kRecoverWaitNs = 6000000000ull;' 'constexpr uint64_t kRecoverWaitNs = 1000000000ull;' "KERNEL 59: the kernel's recovery bound no longer matches the 8 s app bound"
plant 61 $B '		const bool g5page = n48native::g5::args_ok(amdgpu::n1c_g5_latched_on(), action, argScalar);' '		const bool g5page = n48native::g5::args_ok(true, action, argScalar);' "KERNEL 61: sessstat 5 is served with G5 OFF"
plant 62 $B '		if (!g5page && !n48native::g2::native_exempt(amdgpu::n1c_multi_latched_on(), action, argScalar)) return kIOReturnBadArgument;' '		if (false) return kIOReturnBadArgument;' "KERNEL 62: sessstat accepts any page"
plant 64 $E '            const uint32_t n = build_submit(gSubmitBuf, (uint32_t)(sizeof(gSubmitBuf) / sizeof(gSubmitBuf[0])), va, by, nIbs, s->vmid, hasFence,' '            const uint32_t n = build_submit(gSubmitBuf, (uint32_t)(sizeof(gSubmitBuf) / sizeof(gSubmitBuf[0])), va, by, nIbs, kNativeVmid, hasFence,' "KERNEL 64: the app path builds every IB on VMID 8"
plant 65 $E '        IOLockLock(gRingLock);
        g5_fill_in_locked(s, K, need);' '        g5_fill_in_locked(s, K, need);
        IOLockLock(gRingLock);' "KERNEL 65: the admission inputs are read before gRingLock is taken (a stale snapshot)"
plant 63 $E '        if (el < 5000000ull) IODelay(1); else IOSleep(1);
    }
    if (waiting) {' '    }
    if (waiting) {' "KERNEL 63: the wait loop spins without sleeping"
# ---- the CLI, the version ----
plant 70 $C 'hangstat [0..8]|sessstat [0..5]|' 'hangstat [0..8]|sessstat [0..4]|' "CLI 70: the usage does not offer page 5"
plant 71 $C '        } else if (c == 0 && out[4] == 5) {' '        } else if (c == 0 && out[4] == 55) {' "CLI 71: page 5 is not printed"
plant 72 $PL '<key>CFBundleVersion</key>
	<string>0.0.664</string>' '<key>CFBundleVersion</key>
	<string>0.0.642</string>' "VERSION 72: Info.plist is not bumped"
plant 73 $H 'kN1cKextBuild = 664;' 'kN1cKextBuild = 642;' "VERSION 73: the native client build number is stale"
echo "native_g5_plant: $((total - 1)) plants (+ the baseline), $escaped escaped or did not apply"
[ $escaped -eq 0 ]
