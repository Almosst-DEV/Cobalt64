#!/bin/zsh
# native_s2a_plant.sh - planted breaks for tests/native_s2a_test.cpp (build 0.0.603, native S2a).
# For each plant: copy the real sources into a scratch tree, apply ONE break (the script first proves the break changed the text exactly once),
# compile the real test against the scratch tree and demand that it FAILS (a compile error from a static_assert counts). A plant the suite lets
# through is a hole: the script exits non-zero and says which.
#   run from anywhere:  src/navi48-bringup/tests/native_s2a_plant.sh
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
K=src/navi48-bringup
SR=$K/src
SCR="${TMPDIR:-/tmp}/n2a-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0

plant() {   # plant <id> <file relative to repo root> <old text> <new text> <description>
  local id="$1" file="$2" old="$3" new="$4" desc="$5"
  rm -rf "$SCR"; mkdir -p "$SCR/$SR/dcn" "$SCR/$SR/amd" "$SCR/$K/tests" "$SCR/tools/native"
  cp "$ROOT/$SR/Navi48NativeABI.h" "$ROOT/$SR/Navi48NativeClient.cpp" "$ROOT/$SR/Navi48Bringup.cpp" "$SCR/$SR/"
  cp "$ROOT/$SR/dcn/"navi48_scanout_pure.h "$ROOT/$SR/dcn/"navi48_dcn.cpp "$ROOT/$SR/dcn/"navi48_dcn.hpp "$SCR/$SR/dcn/"
  cp "$ROOT/$SR/amd/"native_s1c.cpp "$ROOT/$SR/amd/"native_s1c.h "$SCR/$SR/amd/"
  cp "$ROOT/$K/Makefile" "$ROOT/$K/Info.plist" "$SCR/$K/"
  cp "$ROOT/$K/tests/native_s2a_test.cpp" "$SCR/$K/tests/"
  cp "$ROOT/tools/native/n48scan.c" "$SCR/tools/native/"
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
  local out
  out=$(cd "$SCR" && clang++ -std=c++17 -Wall -Wextra -Werror -O0 -I $SR -I $SR/dcn $K/tests/native_s2a_test.cpp -o "$SCR/t" 2>&1)
  if [ $? -ne 0 ]; then
    echo "PLANT $id: $desc: CAUGHT at compile time: $(echo "$out" | /usr/bin/grep -m1 -E 'error|static assertion' | cut -c1-140)"; return
  fi
  out=$(cd "$SCR" && ./t . 2>&1)
  if [ $? -eq 0 ]; then echo "PLANT $id: $desc: *** ESCAPED (the suite passed) ***"; escaped=$((escaped+1)); return; fi
  echo "PLANT $id: $desc: CAUGHT by $(echo "$out" | /usr/bin/grep -c '^FAIL') check(s); first: $(echo "$out" | /usr/bin/grep -m1 '^FAIL' | cut -c1-120)"
}

P=$SR/dcn/navi48_scanout_pure.h
D=$SR/dcn/navi48_dcn.cpp
E=$SR/amd/native_s1c.cpp
# ---- the pure half ----
plant 1  $P 'if (offset > bo.size || bytes > bo.size - offset) return kBadArg;' 'if (offset > bo.size) return kBadArg;' "bounds: the buffer may run past the end of the BO"
plant 2  $P '|| ((end - 1ull) >> 32) != (fb.consoleMc >> 32)) return kBadArg;' ') return kBadArg;' "bounds: only the START must share the console's HIGH dword (a buffer may straddle a HIGH boundary)"
plant 3  $P 'if ((start & (kBufAlign - 1ull)) != 0ull) return kBadArg;' ';' "bounds: no 64 KiB alignment check"
plant 4  $P 'if (!bo.vis) return kBadArg;' ';' "bounds: a hi-pool / GTT BO is accepted"
plant 5  $P 'if (fb.hi == 0ull || start < fb.lo || end > fb.hi) return kBadArg;' 'if (fb.hi == 0ull || start < fb.lo) return kBadArg;' "bounds: the top of the frame-buffer window is not enforced"
plant 6  $P 'if (fb.consoleBytes != 0ull && start < fb.consoleMc + fb.consoleBytes && fb.consoleMc < end) return kBadArg;' ';' "bounds: a buffer over the console is accepted"
plant 7  $P 'if (pitchBytes != livePitchBytes || height != liveHeight) return kBadArg;' 'if (false && (pitchBytes != livePitchBytes || height != liveHeight)) return kBadArg;' "bounds: the stated pitch / height need not match the live plane"
plant 8  $P '(earliestMc != t.s[slot].mc && t.front != slot &&' '((earliestMc & 0ull) == 0ull && t.front != slot &&' "reuse: a slot the hardware is still fetching (EARLIEST_INUSE) counts as reusable"
plant 9  $P 'earliestMc != t.s[slot].mc && t.front != slot &&' 'earliestMc != t.s[slot].mc &&' "reuse: the FRONT slot counts as reusable while EARLIEST_INUSE lags"
plant 10 $P '&& !(t.pendActive && t.pendSlot == slot)));' '));' "reuse: the PENDING slot counts as reusable"
plant 11 $P ': (!earliestValid ? false' ': (!earliestValid ? true' "reuse: an unreadable EARLIEST_INUSE counts as reusable"
plant 12 $P 'if (!slot_reusable(t, i, earliestMc, earliestValid)) return UnpinPlan{ 0u, true };' 'if (false && !slot_reusable(t, i, earliestMc, earliestValid)) return UnpinPlan{ 0u, true };' "BoFree-while-front: a shown or pending slot is just dropped, no console restore first"
plant 13 $P 'if (alwaysFull) return UnpinPlan{ 0u, true };' 'if (alwaysFull && false) return UnpinPlan{ 0u, true };' "leak / close: no forced console restore for an idle slot"
plant 14 $P 'if (m.attempts == 0u) { m.attempts = 1u; m.polls = 0u; return kActProgram; }' 'if (m.attempts == 0u) { m.attempts = 1u; m.verified = true; return kActDone; }' "RELEASE WITHOUT RESTORE: the state machine reports done before it ever programs the console"
plant 15 $P 'plane == consoleMc && earliest == consoleMc && !hwPending;' 'plane == consoleMc && (earliest & 0ull) == 0ull && !hwPending;' "restore_verified ignores EARLIEST_INUSE"
plant 16 $P 'plane == consoleMc && earliest == consoleMc && !hwPending;' 'plane == consoleMc && earliest == consoleMc && !(hwPending && false);' "restore_verified ignores the pending flag"
plant 17 $P 'if (m.attempts < kRestoreAttempts)' 'if (false)' "restore: no second attempt"
plant 18 $P 'if (d >= (kFcMask + 1u) / 2u) return e.v;' ';' "frame counter: a backward step becomes a 2^24-frame jump"
plant 19 $P 'raw &= kFcMask;' ';' "frame counter: the top raw bits are not masked"
plant 20 $P 'constexpr uint32_t kStormLimit    = 1000u;' 'constexpr uint32_t kStormLimit    = 100000u;' "storm guard: the limit is 100x too high"
plant 21 $P 'if (s.n == 0u || nowNs < s.winStart || nowNs - s.winStart >= kStormWindowNs) { s.winStart = nowNs; s.n = 0u; }' 'if (s.n == 0u) { s.winStart = nowNs; s.n = 0u; }' "storm guard: the window never resets (a slow drip trips it: the cumulative cap again)"
plant 22 $P 'return arg == 0ull && (action == 74u || action == 76u || action == 77u);' 'return (arg * 0ull == 0ull) && (action == 74u || action == 76u || action == 77u);' "EXEMPTION MATCHES THE ACTION ONLY (dcnflip 1002, dcnmode 1..130 admitted)"
plant 23 $P '(action == 74u || action == 76u || action == 77u);' '(action == 74u || action == 75u || action == 76u || action == 77u);' "the exemption also admits dcnvbl 0"
plant 24 $P 'if (!t.pendActive || rawFlipPending) return false;' 'if (!t.pendActive || (rawFlipPending && false)) return false;' "latch: counted while the flip bit is still set"
plant 25 $P 'if (t.pendActive) t.replaced++;' ';' "a Present over a still-pending one is not counted as replaced"
plant 26 $P 'if (dccEn) return kGeomDcc;' 'if (dccEn && false) return kGeomDcc;' "geometry: PRIMARY_SURFACE_DCC_EN is accepted"
plant 27 $P 'if (ranges_overlap(mc, bytes, t.s[i].mc, t.s[i].bytes)) return kBadArg;' ';' "slots: two slots may alias the same memory"
plant 28 $P 'for (uint32_t i = 0; i < kMaxSlots; i++) if (t.s[i].used && t.s[i].mc == mc) return true;
    return false;' 'return true;' "the exact flip-target set admits any address"
plant 29 $P 'constexpr uint64_t kIdleNs        = 5000000000ull;' 'constexpr uint64_t kIdleNs        = 50000000000ull;' "the idle deadline is 50 s"
plant 30 $P 'constexpr bool lock_order_ok(uint32_t held, uint32_t taking) { return taking > held; }' 'constexpr bool lock_order_ok(uint32_t held, uint32_t taking) { return taking != held; }' "the lock-order predicate admits the inversion"
# ---- the kext sources (source pins) ----
plant 31 $E 'if (b.pinMask != 0u && scan_unpin(h, b, mode)) b.pinLeak = 1u;' ';' "BoFree / release of a pinned BO frees or leaks without restoring the console"
plant 32 $E '{ uint64_t tr[2]; scan_teardown(s, how, tr); }' '{ }' "the N48N close (and death) does not restore the console"
plant 33 $E 'n48dcn::scanBoGone(b.pinMask, b.pinGen, mode != kRelNormal, r)' 'n48dcn::scanBoGone(b.pinMask, b.pinGen, false, r)' "HUNG leak / close no longer forces the console restore first"
plant 34 $SR/Navi48Bringup.cpp 'if (!n48scan::accel_exempt(action, argScalar) && !n48disp::native_exempt(n48disp_latched_on(), action, argScalar)) {
	if (action != 0 && amdgpu::native_s1b_refuse(0)) return kIOReturnNotPermitted;
	}' 'if (action != 0 && amdgpu::native_s1b_refuse(0)) return kIOReturnNotPermitted;
	if (!n48scan::accel_exempt(action, argScalar) && !n48disp::native_exempt(n48disp_latched_on(), action, argScalar)) {
	}' "the exemption sits AFTER the native refusal (it never runs)"
plant 35 $D 'const int pr = dcn41_hubp_program_flip(&gDcn.d, 0u, consoleMc, 0u, false, false);' 'const int pr = 0;' "RELEASE WITHOUT RESTORE (kext source): the restore no longer programs the console"
plant 36 $D 'gScan.vupdates++;' 'gScan.vupdates++; IOSleep(1);' "the interrupt handler sleeps under the scanout lock"
plant 37 $D 'IOLockUnlock(l);
		IOSleep(1);' 'IOSleep(1);' "the restore sleeps with the scanout lock held"
plant 38 $D '__atomic_load_n(&gScan.active, __ATOMIC_ACQUIRE) == 0u &&
	    gDcn.dceEntries - gDcn.entriesAtEnable > kDceStormCap' 'gDcn.dceEntries - gDcn.entriesAtEnable > kDceStormCap' "the cumulative storm cap still trips while the plane is taken"
plant 39 $D 'if (!gDcn.goldenValid && amdgpu::native_s1b_state().gate == n48native::kGateOn) {' 'if (false) {' "no dcnmode golden copy at bind"
plant 40 $D 'scanEscape("explicit dcnflip 0");' ';' "dcnflip 0 does nothing on a native boot"
plant 41 $SR/amd/native_s1c.h 'kN1cKextBuild = 620;' 'kN1cKextBuild = 601;' "Hello reports a stale kext build"
plant 42 $E 'IOLockLock(gCliLock);
    if (!sess_hello()) { IOLockUnlock(gCliLock); return kIOReturnNotReady; }   // closed while we waited for the lock
    const IOReturn rc = (IOReturn)n48dcn::scanAcquire(out, __atomic_load_n(&gSessSeq, __ATOMIC_SEQ_CST));   // 0.0.609: the session id lets a row-120 hold be Acquired
    IOLockUnlock(gCliLock);' 'const IOReturn rc = (IOReturn)n48dcn::scanAcquire(out, 0u);' "Acquire without the client lock (a racing close could leave the plane taken)"
plant 43 $D 'else if (n48scan::idle_expired(now_ns(), gScan.lastActivityNs)) why = "idle watchdog: 5 s without a scanout call";' ';' "the 5 s idle watchdog is gone"
plant 44 $D 'if (mc == 0ull || !flip_target_ok(gScan.tbl, mc)) {' 'if (mc == 0ull) {' "Present skips the exact flip-target set"
plant 45 $D 'const uint32_t gw = geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc);' 'const uint32_t gw = geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, false);' "Acquire does not refuse a DCC plane"
plant 46 $E '(flags & ~(uint64_t)N48N_HELLO_F_MINOR) != 0ull' 'false' "Hello accepts any flags"
plant 47 $SR/Navi48NativeClient.cpp 'if (!shape(2, 3, 0, 0)) return kIOReturnBadArgument;' ';' "ScanoutPresent does not check its shape"
plant 48 $D '(void)scan_poll_locked(false);                      // resolve the previous Present first: a latch that the IRQ has not delivered yet is not "replaced"' ';' "Present counts a replace without first resolving the previous latch"
plant 49 $D 'if (gScan.wantRestore) why = gScan.wantWhy ? gScan.wantWhy : "requested";' ';' "the watchdog ignores the restores the IRQ handler requests"
plant 50 $D 'gScan.wantRestore = true; gScan.wantWhy = "IRQ storm guard (rate)";' ';' "the rate guard trips but never asks for the console restore"
# ---- the 0.0.603 review fixes, and the lock order inverted in the source ----
plant 51 $E 'if (in->reserved0 != 0u) return kIOReturnBadArgument;
    IOLockLock(gCliLock);' 'if (in->reserved0 != 0u) return kIOReturnBadArgument;
    { uint64_t o2x[2]; (void)n48dcn::scanRegister(false, 0, 0, 0, 0, 0, 0, 0, o2x); }
    IOLockLock(gCliLock);' "LOCK ORDER: Register reaches the DCN lock BEFORE taking gCliLock"
plant 52 $D 'uint32_t scanGeneration() { return gScan.gen; }' 'uint32_t scanGeneration() { IOLockLock(gCliLock); return gScan.gen; }' "LOCK ORDER: the DCN layer takes gCliLock (after its own lock in the interrupt / restore paths)"
plant 53 $E 'return (IOReturn)n48dcn::scanStatus(out);' 'IOLockLock(gCliLock); return (IOReturn)n48dcn::scanStatus(out);' "LOCK ORDER: Status takes gCliLock (a lock-free selector would now wait behind a long restore)"
plant 54 $E 'IOLockLock(gCliLock);
    if (!sess_hello()) { IOLockUnlock(gCliLock); return kIOReturnNotReady; }   // closed while we waited for the lock
    uint64_t r[2] = { 1, 0 };
    scan_teardown(gS, "ScanoutRelease", r);' 'uint64_t r[2] = { 1, 0 };
    scan_teardown(gS, "ScanoutRelease", r);
    IOLockLock(gCliLock);
    if (!sess_hello()) { IOLockUnlock(gCliLock); return kIOReturnNotReady; }   // closed while we waited for the lock' "LOCK ORDER: Release runs the teardown (DCN lock) before taking gCliLock"
plant 55 $P 'return vmid == 0u && !tmz && flipType == 0u && viewportStart == 0u;' 'return vmid == 0u && !tmz && flipType == 0u && (viewportStart & 0ull) == 0u;' "the restore-exactness check ignores PRI_VIEWPORT_START"
plant 56 $D 'return r[0] != 0ull ? 1u : 2u;' 'return 1u;' "an unverified restore is reported as fine (the BO would be freed while possibly scanned)"
plant 57 $E 'if (b.pinLeak != 0u) mode = kRelLeak;' ';' "a BO pinned across an unverified restore is freed instead of leaked"
plant 58 $D 'if (!restore_exact(ex.vmid, ex.tmz, ex.flipType, ex.vpStart)) {' 'if (false) {' "Acquire does not require VMID / TMZ / FLIP_TYPE / viewport start = 0"
plant 59 $D 'else if (geomBad) why = "live plane geometry changed";' ';' "the watchdog no longer checks the live geometry"
plant 60 $SR/Navi48Bringup.cpp 'n48dcn::scanShutdown();' ';' "Navi48Bringup::stop does not release the scanout / wait for the watchdog"
plant 61 $D 'IOLockLock(bindLock);' ';' "bind() is not serialised"
plant 62 tools/native/n48scan.c 'st.latched >= 1 && rateOk' 'rateOk' "the tool PASSes with zero latches"
plant 63 tools/native/n48scan.c 'deliv >= 0.95 * vrate' '1' "the tool has no rate-based flip PASS"
plant 64 $D '__atomic_fetch_add(&gScan.watchdogAlive, 1u, __ATOMIC_ACQ_REL);
		const kern_return_t kr' 'const kern_return_t kr' "the watchdog-alive counter is not raised before the thread starts"
echo "native_s2a_plant: $total plants, $escaped escaped"
[ $escaped -eq 0 ]
