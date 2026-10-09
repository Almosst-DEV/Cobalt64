#!/bin/zsh
# native_g4_plant.sh - planted breaks for tests/native_g4_test.cpp (kext 0.0.640, GPU-apps G4: user applications, the allow-list, the budget report, the close-path mapping rule).
# For each plant: copy the real sources into a scratch tree, apply ONE break to the REAL code (the script first proves the break changed the text),
# compile the real test against the scratch tree and demand that it FAILS (a compile error counts). A plant the suite lets through is a hole: the
# script exits non-zero and says which.
#   run from anywhere:  src/navi48-bringup/tests/native_g4_plant.sh
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
K=src/navi48-bringup
SCR="${TMPDIR:-/tmp}/g4-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0

fresh() {   # the scratch tree: the kext sources, the plist, the test and the CLI
  rm -rf "$SCR"; mkdir -p "$SCR/$K/tests" "$SCR/tools/pc"
  cp -R "$ROOT/$K/src" "$SCR/$K/"
  cp "$ROOT/$K/Info.plist" "$SCR/$K/"
  cp "$ROOT/$K/tests/native_g4_test.cpp" "$SCR/$K/tests/"
  cp "$ROOT/tools/pc/navi48test.c" "$SCR/tools/pc/"
}
build_run() {   # -> sets OUT, returns 0 pass / 1 test failed / 2 compile failed
  OUT=$(cd "$SCR" && clang++ -std=c++17 -Wall -Wextra -Werror -O0 -I $K/src -I $K/src/amd $K/tests/native_g4_test.cpp -o "$SCR/t" 2>&1) || return 2
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
P=$K/src/amd/native_g4_pure.h
O=$K/src/amd/native_open_policy_pure.h
E=$K/src/amd/native_s1c.cpp
N=$K/src/Navi48NativeClient.cpp
H=$K/src/Navi48NativeClient.hpp
B=$K/src/Navi48Bringup.cpp
U=$K/src/Navi48UserClient.cpp
A=$K/src/Navi48NativeABI.h
KEY=$K/src/Navi48AppKey.h
G2=$K/src/amd/native_g2_pure.h
C=tools/pc/navi48test.c

# ---- the open policy: the third role ----
plant 1  $O '    return onAllowList ? OpenDecision{ true, kReasonApp } : OpenDecision{ false, kReasonAppNotAllowed };' '    (void)onAllowList; return OpenDecision{ true, kReasonApp };' "ROLE 1: every ordinary user is admitted as an APP (the allow-list is ignored)"
plant 2  $O '    if (d.admit || d.reason != kReasonNotPrivileged || !appsOn || uid < kAppMinUid) return d;' '    (void)appsOn; if (d.admit || d.reason != kReasonNotPrivileged || uid < kAppMinUid) return d;' "ROLE 2: the switches are ignored (a user can be an APP with navi48-apps OFF)"
plant 3  $O '    if (d.admit || d.reason != kReasonNotPrivileged || !appsOn || uid < kAppMinUid) return d;' '    (void)uid; if (d.admit || d.reason != kReasonNotPrivileged || !appsOn) return d;' "ROLE 3: a system uid (daemon) can be an APP"
plant 4  $O 'constexpr uint32_t kAppMinUid = 501u;' 'constexpr uint32_t kAppMinUid = 100u;' "ROLE 4: the first APP uid is 100"
plant 5  $O '    if (d.admit || d.reason != kReasonNotPrivileged || !appsOn || uid < kAppMinUid) return d;' '    if (d.admit || !appsOn || uid < kAppMinUid) return d;' "ROLE 5: a refusal that is not NotPrivileged (uid 88's) is re-judged as an APP candidate"
plant 6  $N '	appSession = d.reason == n48native::policy::kReasonApp;' '	appSession = false;' "ROLE 6: the client never records that it is an APP (its gate and its mark are skipped)"
plant 7  $N '		onAllow = amdgpu::n1c_app_allowed(comm, &struckOut);   // 0.0.641: also false for a listed name with 3 strikes (struckOut)' '		onAllow = true;' "ROLE 7: the client never asks the allow-list"
plant 8  $N '	if (appsOn && !admin && uid >= n48native::policy::kAppMinUid) {' '	if (appsOn && uid >= n48native::policy::kAppMinUid) {' "ROLE 8: the name lookup also runs for an administrator"
plant 9  $N '	const bool appsOn = amdgpu::n1c_apps_enabled();' '	const bool appsOn = true;' "ROLE 9: the client assumes the switches are ON"
plant 10 $N '		proc_name(proc_selfpid(), comm, (int)sizeof comm);   // the p_comm of the opening process (IOServiceOpen runs on its thread); the same pair Navi48Bringup.cpp already uses
' '' "ROLE 10: the opener name is never read"
# ---- the selector sets ----
plant 11 $O '           (sel >= (uint32_t)N48N_SEL_BOCREATE && sel <= (uint32_t)N48N_SEL_WAITSEQ) || sel == (uint32_t)N48N_SEL_BO_IMPORT_HOST;' '           (sel >= (uint32_t)N48N_SEL_READREGS && sel <= (uint32_t)N48N_SEL_WAITSEQ) || sel == (uint32_t)N48N_SEL_BO_IMPORT_HOST;' "SEL 11: an APP may call ReadRegs"
plant 12 $O '           (sel >= (uint32_t)N48N_SEL_BOCREATE && sel <= (uint32_t)N48N_SEL_WAITSEQ) || sel == (uint32_t)N48N_SEL_BO_IMPORT_HOST;' '           (sel >= (uint32_t)N48N_SEL_BOCREATE && sel <= (uint32_t)N48N_SEL_SCAN_RELEASE) || sel == (uint32_t)N48N_SEL_BO_IMPORT_HOST;' "SEL 12: an APP may call the scanout set"
plant 13 $O '           (sel >= (uint32_t)N48N_SEL_BOCREATE && sel <= (uint32_t)N48N_SEL_WAITSEQ) || sel == (uint32_t)N48N_SEL_BO_IMPORT_HOST;' '           (sel >= (uint32_t)N48N_SEL_BOCREATE && sel <= (uint32_t)N48N_SEL_WAITSEQ) || sel >= (uint32_t)N48N_SEL_BO_IMPORT_HOST;' "SEL 13: an APP may call every selector from 21 up (and unknown numbers)"
plant 14 $N '	if (appSession && !n48native::policy::selector_allowed_for_app(selector)) {' '	if (false && !n48native::policy::selector_allowed_for_app(selector)) {' "SEL 14: the client never applies the APP selector gate"
plant 15 $N '	if (appSession && !n48native::policy::selector_allowed_for_app(selector)) {' '	if (appSession && n48native::policy::selector_allowed_for_app(selector)) {' "SEL 15: the APP gate is inverted"
# ---- engine-level refusal (defence in depth) ----
plant 16 $E '    if (app_refused(ref, n48native::g4::kCallReadRegs)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): an APP session never reads registers
' '' "DEPTH 16: the engine's ReadRegs no longer refuses an APP session"
plant 17 $E '    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    if (flags != 0ull) return kIOReturnBadArgument;
    Session *s = sess_lock(ref, true);' '    if (flags != 0ull) return kIOReturnBadArgument;
    Session *s = sess_lock(ref, true);' "DEPTH 17: ScanoutAcquire no longer refuses an APP session"
plant 18 $P 'constexpr bool app_call_allowed(bool isApp, uint32_t call) { return !isApp || call == (uint32_t)kCallOther; }' 'constexpr bool app_call_allowed(bool isApp, uint32_t call) { (void)isApp; (void)call; return true; }' "DEPTH 18 (pure): an APP session passes every engine call"
plant 19 $E '    __atomic_store_n(&s->app, true, __ATOMIC_RELEASE);
' '' "DEPTH 19: n1c_mark_app marks nothing"
plant 20 $E '    if (!apps_on()) return kIOReturnNotPermitted;
    Session *s = sess_lock(ref, false);' '    Session *s = sess_lock(ref, false);' "DEPTH 20: n1c_mark_app marks even with the role disabled"
plant 21 $N '		const IOReturn mrc = amdgpu::n1c_mark_app(ref, uc->appKey);' '		const IOReturn mrc = kIOReturnSuccess;' "DEPTH 21: create() never marks the session as an APP"
# ---- the switches ----
plant 22 $P 'constexpr uint32_t latch_value(bool present, uint32_t value) { return (present && value == 1u) ? kLatchOn : kLatchOff; }' 'constexpr uint32_t latch_value(bool present, uint32_t value) { (void)present; (void)value; return kLatchOn; }' "SWITCH 22 (pure): navi48-apps latches ON whatever the boot-arg"
plant 23 $P 'constexpr bool apps_enabled(bool appsLatchOn, bool multiLatchOn) { return appsLatchOn && multiLatchOn; }' 'constexpr bool apps_enabled(bool appsLatchOn, bool multiLatchOn) { (void)multiLatchOn; return appsLatchOn; }' "SWITCH 23 (pure): the role does not need the multi-session latch"
plant 24 $E '    return n48native::g4::apps_enabled(n48native::g4::latch_is_on(l), multi_on());' '    return n48native::g4::latch_is_on(l);' "SWITCH 24 (kernel): apps_on() ignores navi48-multisession"
plant 25 $E '    if (!apps_on()) return false;
    const uint64_t key = n48a_comm_key(comm);' '    const uint64_t key = n48a_comm_key(comm);' "SWITCH 25: the allow-list answers with the role disabled"
plant 26 $B '		if (!n48native::g4::native_exempt(amdgpu::n1c_apps_enabled(), action, argScalar)) return kIOReturnBadArgument;' '		if (false) return kIOReturnBadArgument;' "SWITCH 26: accel 104 runs with the role disabled / any argument"
plant 27 $U '	if (n48native::g4::action_admitted(amdgpu::n1c_apps_enabled(), action)) {' '	if (n48native::g4::is_g4_action(action)) {' "SWITCH 27: the user client lets 104 past the old bound with the role disabled"
plant 28 $P 'constexpr bool action_admitted(bool appsEnabled, uint32_t a) { return appsEnabled && is_g4_action(a); }' 'constexpr bool action_admitted(bool appsEnabled, uint32_t a) { (void)appsEnabled; return is_g4_action(a); }' "SWITCH 28 (pure): action 104 exists without the role"
# ---- the allow-list ----
plant 29 $P '    if (al_contains(l, key)) return kAllowExists;
    // 0.0.642: a slot is claimed' '    // 0.0.642: a slot is claimed' "LIST 29: add does not notice a duplicate"
plant 30 $P '    return kAllowFull;
}
inline uint32_t al_add(' '    return kAllowOk;
}
inline uint32_t al_add(' "LIST 30: a full list pretends the add worked"
plant 31 $P '        if (__atomic_compare_exchange_n(&l[i], &e, 0ull, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) hit = true;' '        if (__atomic_compare_exchange_n(&l[i], &e, 0ull, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { hit = true; break; }' "LIST 31: remove clears only the first matching slot"
plant 32 $P '    if (!appsEnabled) { o[0] = kAllowOff; return kAllowOff; }' '    (void)appsEnabled;' "LIST 32: the verb works with the role disabled"
plant 33 $P '    return a == kActAppAllow && ((arg_op(arg) == N48A_OP_ADD || arg_op(arg) == N48A_OP_REMOVE) ? arg_payload(arg) != 0ull' '    return a == kActAppAllow && ((arg_op(arg) == N48A_OP_ADD || arg_op(arg) == N48A_OP_REMOVE) ? true' "LIST 33: a zero key is a legal argument"
plant 34 $P 'static_assert(kListPages == 4u' 'static_assert(kListPages == 3u' "LIST 34: the page count is wrong (compile)"
plant 35 $P 'constexpr uint32_t kMaxEntries = 32u;' 'constexpr uint32_t kMaxEntries = 64u;' "LIST 35: the list holds 64 entries (brief: max 32)"
plant 36 $P '    if (key == 0ull || (key & ~N48A_KEY_MASK) != 0ull) return kAllowBadArg;
    if (al_contains' '    if (key == 0ull) return kAllowBadArg;
    if (al_contains' "LIST 36: add accepts a key wider than 60 bits"
plant 37 $KEY '    for (unsigned i = 0; i < N48A_COMM_MAX && comm[i] != 0; i++)' '    for (unsigned i = 0; i < 32u && comm[i] != 0; i++)' "KEY 37: the name key covers 32 characters (p_comm has 16)"
plant 38 $KEY '#define N48A_KEY_BITS   60u' '#define N48A_KEY_BITS   61u' "KEY 38: the key is 61 bits (collides with the operation nibble)"
plant 39 $KEY '    return h != 0ull ? h : 1ull;   /* 0 is the empty slot */' '    return h;' "KEY 39: the key can be 0 (the empty slot)"
plant 40 $C 'n48a_arg(isAdd ? N48A_OP_ADD : N48A_OP_REMOVE, n48a_comm_key(arg2))' 'n48a_arg(isAdd ? N48A_OP_ADD : N48A_OP_REMOVE, n48a_comm_key(arg))' "KEY 40: the CLI hashes the wrong token"
# ---- the budget report ----
plant 41 $P '    if (!multi || isWs) return r;' '    (void)isWs; if (!multi) return r;' "BUDGET 41 (pure): WindowServer's session is told it has a budget"
plant 42 $P '    r.cap = cap; r.avail = room < poolFree ? room : poolFree;' '    (void)poolFree; r.cap = cap; r.avail = room;' "BUDGET 42 (pure): the pool's free bytes do not bound the available budget"
plant 43 $P '    reserved[N48N_INFO_R_BUDGET_CAP_LO] = (uint32_t)(b.cap & 0xFFFFFFFFull);   reserved[N48N_INFO_R_BUDGET_CAP_HI] = (uint32_t)(b.cap >> 32);' '    reserved[N48N_INFO_R_BUDGET_CAP_HI] = (uint32_t)(b.cap & 0xFFFFFFFFull);   reserved[N48N_INFO_R_BUDGET_CAP_LO] = (uint32_t)(b.cap >> 32);' "BUDGET 43 (pure): the cap's low and high words are swapped"
plant 44 $P '    const uint64_t room = othersHeld >= cap ? 0ull : cap - othersHeld;' '    const uint64_t room = cap - othersHeld;' "BUDGET 44 (pure): over the cap the available bytes underflow"
plant 45 $E 'n48native::g4::budget_report(multi_on(), s->ws, __atomic_load_n(&s->app, __ATOMIC_ACQUIRE),' 'n48native::g4::budget_report(multi_on(), false, __atomic_load_n(&s->app, __ATOMIC_ACQUIRE),' "BUDGET 45 (kernel): QueryInfo reports a budget to WindowServer's session"
plant 46 $E 'n48native::g4::budget_pack(n48native::g4::budget_report(' '(void)(n48native::g4::budget_report(' "BUDGET 46 (kernel): QueryInfo never packs the report"
plant 47 $E '__atomic_load_n(&gVramOthers, __ATOMIC_RELAXED), vram_total(), poolFree), o->reserved);' '0ull, vram_total(), poolFree), o->reserved);' "BUDGET 47 (kernel): QueryInfo reports an empty budget counter (always full headroom)"
plant 48 $A '#define N48N_INFO_R_BUDGET_AVAIL_LO 6u' '#define N48N_INFO_R_BUDGET_AVAIL_LO 5u' "BUDGET 48: the ABI index of the available word collides"
plant 49 $A '#define N48N_ABI_MINOR     12u ' '#define N48N_ABI_MINOR     9u ' "BUDGET 49: the ABI minor is not bumped"
# ---- R6 on the close path ----
plant 50 $P '    return !(appsOn && isVisVram && hasDescriptor && descriptorRefs > 1u);' '    return !(appsOn && isVisVram && hasDescriptor && descriptorRefs > 2u);' "CLOSE 50 (pure): one live mapping is not seen at close"
plant 51 $P '    return !(appsOn && isVisVram && hasDescriptor && descriptorRefs > 1u);' '    (void)appsOn; return !(isVisVram && hasDescriptor && descriptorRefs > 1u);' "CLOSE 51 (pure): the switch-off identity is lost (OFF leaks too)"
plant 52 $E '        else if (b.kind == kBoVis && !vramReturnOk) {' '        else if (false && b.kind == kBoVis && !vramReturnOk) {' "CLOSE 52 (kernel): a mapped BO is returned to the pool at close"
plant 53 $E '            gMappedLeakBytes += b.size; gMappedLeakBos++;' '            gMappedLeakBytes += b.size; gMappedLeakBos++; budget_release(b);' "CLOSE 53 (kernel): the leaked range is taken off the budget (its bytes stop being counted)"
plant 54 $E '        const bool vramReturnOk = n48native::g4::close_vram_return_ok(apps_on(), b.kind == kBoVis, b.vmd != nullptr, b.vmd != nullptr ? (uint32_t)b.vmd->getRetainCount() : 0u);
        if (b.vmd != nullptr) { b.vmd->release(); b.vmd = nullptr; }   // the descriptor is released once, before the range it names is freed' '        if (b.vmd != nullptr) { b.vmd->release(); b.vmd = nullptr; }   // the descriptor is released once, before the range it names is freed
        const bool vramReturnOk = n48native::g4::close_vram_return_ok(apps_on(), b.kind == kBoVis, b.vmd != nullptr, b.vmd != nullptr ? (uint32_t)b.vmd->getRetainCount() : 0u);' "CLOSE 54 (kernel): the references are read AFTER the closing session dropped its own (always looks unmapped)"
plant 55 $E '            gMappedLeakBytes += b.size; gMappedLeakBos++;' '            gMappedLeakBytes += b.size;' "CLOSE 55 (kernel): the leaked-BO count is never kept"
plant 56 $G2 '((uint64_t)(g.mappedLeakBos & 0xFFFFu) << 20)' '((uint64_t)(g.mappedLeakBos & 0xFFFFu) << 21)' "CLOSE 56 (pure): the stat page puts the leak count in the wrong bits"
plant 57 $E '        g.mappedLeakBos = gMappedLeakBos;   // 0.0.640 (G4): BOs leaked at close because a CPU mapping outlived the session (read without gCliLock: a diagnostic)
' '' "CLOSE 57 (kernel): sessstat 4 never reports the leaked BOs"
plant 58 $E '        const uint32_t mrc = hung_now() ? kOk : n48native::g2::bofree_mapping_rc(' '        const uint32_t mrc = kOk; (void)n48native::g2::bofree_mapping_rc(' "R6 58: BoFree of a mapped BO is no longer refused (0.0.629's rule lost)"
# ---- version ----
plant 59 $K/Info.plist '<key>CFBundleVersion</key>
	<string>0.0.664</string>' '<key>CFBundleVersion</key>
	<string>0.0.632</string>' "the version is not bumped"
plant 60 $K/src/amd/native_s1c.h 'kN1cKextBuild = 664;' 'kN1cKextBuild = 632;' "the native client build number is stale"

plant 61 $N '		if (gAppSelRefusedLogged < 16u && OSIncrementAtomic((volatile SInt32 *)&gAppSelRefusedLogged) < 16) NCLOG("selector %u refused: not permitted for an APP (user application) client", selector);
		return kIOReturnNotPrivileged;' '		if (gAppSelRefusedLogged < 16u && OSIncrementAtomic((volatile SInt32 *)&gAppSelRefusedLogged) < 16) NCLOG("selector %u refused: not permitted for an APP (user application) client", selector);
		return kIOReturnBadArgument;' "SEL 61: the APP gate refuses with BadArgument instead of NotPrivileged"
plant 62 $N '	const IOReturn rc = amdgpu::n1c_open(*owner->bringupContext(), uc->wsSession, &ref);' '	const IOReturn rc = amdgpu::n1c_open(*owner->bringupContext(), uc->wsSession || uc->appSession, &ref);' "ROLE 62: an APP session is opened as WindowServer's (no budget, close disarms the display pipe)"
# ---- 0.0.641 (the G4 review fixes) ----
# item 1: the close path
plant 63 $E 'close_vram_return_ok(apps_on(), b.kind == kBoVis' 'close_vram_return_ok(multi_on(), b.kind == kBoVis' "CLOSE 63 (kernel): the close rule is keyed on navi48-multisession alone again (the review's ungated change)"
plant 64 $P '    return (appsOn && isVisVram && hasDescriptor && descriptorRefs > 1u) ? (descriptorRefs - 1u < kUnmapMaxPerBo' '    (void)appsOn; return (isVisVram && hasDescriptor && descriptorRefs > 1u) ? (descriptorRefs - 1u < kUnmapMaxPerBo' "CLOSE 64 (pure): the unmap sweep also runs with apps OFF"
plant 65 $N 'n48disp_on_ws_client_closed(wsSession); amdgpu::n1c_unmap_for_close(amdgpu::N1cRef{ sessSlot, sessId }, this); amdgpu::n1c_close(amdgpu::N1cRef{ sessSlot, sessId }, "clientClose");' 'n48disp_on_ws_client_closed(wsSession); amdgpu::n1c_close(amdgpu::N1cRef{ sessSlot, sessId }, "clientClose");' "CLOSE 65 (kernel): clientClose never removes the CPU mappings (the leak the review found)"
plant 66 $N 'n48disp_on_ws_client_closed(wsSession); amdgpu::n1c_unmap_for_close(amdgpu::N1cRef{ sessSlot, sessId }, this); amdgpu::n1c_close(amdgpu::N1cRef{ sessSlot, sessId }, "stop");' 'n48disp_on_ws_client_closed(wsSession); amdgpu::n1c_close(amdgpu::N1cRef{ sessSlot, sessId }, "stop");' "CLOSE 66 (kernel): stop() never removes the CPU mappings"
plant 67 $E '    if (uc == nullptr || !apps_on()) return;' '    if (uc == nullptr) return;' "CLOSE 67 (kernel): the sweep runs with apps OFF"
plant 68 $E '        if (n != 0u) vmd->retain();   // our own reference while the lock is dropped (a racing BoFree may release the record'"'"'s)' '        (void)0;' "CLOSE 68 (kernel): the sweep keeps no reference of its own while the lock is dropped"
plant 69 $E '            m->release(); removed++;' '            removed++;' "CLOSE 69 (kernel): the sweep never releases the map it removed (the descriptor keeps its reference: the range still leaks)"
plant 70 $E '        if (n == 0u) continue;' '        IOLockLock(gCliLock); IOLockUnlock(gCliLock); if (n == 0u) continue;' "CLOSE 70 (kernel): the sweep takes the shared client lock"
plant 71 $P 'constexpr uint32_t kUnmapMaxPerBo = 256u;' 'constexpr uint32_t kUnmapMaxPerBo = 100000u;' "CLOSE 71 (pure): the sweep is no longer bounded"
# item 3: the log rate limit
plant 72 $N 'if (amdgpu::n1c_log_gate(&suppressed)) {' 'if (true) {' "LOG 72 (kernel): the 'open refused' line is not behind the gate"
plant 73 $P '    if (used < kLogPerSec) { *admit = true; return w + (1ull << 32); }' '    (void)used; { *admit = true; return w + (1ull << 32); }' "LOG 73 (pure): the gate never drops a line"
plant 74 $P '    if ((nowSec & 0xFFFFFFFFull) != sec) { *suppressedBefore = supp; *admit = true;' '    if ((nowSec & 0xFFFFFFFFull) != sec) { *admit = true;' "LOG 74 (pure): the dropped lines are never reported"
plant 75 $P 'constexpr uint32_t kLogPerSec = 5u;' 'constexpr uint32_t kLogPerSec = 50u;' "LOG 75 (pure): 50 lines per second"
plant 76 $E '        const uint64_t nw = n48native::g4::log_gate_step(w, sec, &admit, suppressed);' '        const uint64_t nw = w; admit = true; (void)sec; (void)suppressed;' "LOG 76 (kernel): the gate admits everything"
# item 4: the visible pool
plant 77 $P 'constexpr bool hi_first(bool appsOn, bool isWs, bool hiOk) { return appsOn && !isWs && hiOk; }' 'constexpr bool hi_first(bool appsOn, bool isWs, bool hiOk) { (void)appsOn; (void)isWs; (void)hiOk; return false; }' "VIS 77 (pure): a NO_CPU_ACCESS BO is never placed in the hi pool first"
plant 78 $P 'constexpr bool appvis_ok(bool appsOn, bool isWs, uint64_t held, uint64_t add, uint64_t cap) { return !appsOn || isWs || !s1c::would_exceed(held, add, cap); }' 'constexpr bool appvis_ok(bool appsOn, bool isWs, uint64_t held, uint64_t add, uint64_t cap) { (void)appsOn; (void)isWs; (void)held; (void)add; (void)cap; return true; }' "VIS 78 (pure): no visible-pool cap"
plant 79 $P 'constexpr uint32_t kAppVisDefaultMiB = 64u,' 'constexpr uint32_t kAppVisDefaultMiB = 32u,' "VIS 79 (pure): the default cap is 32 MiB"
plant 80 $P '(present && mib >= kAppVisMinMiB && mib <= kAppVisMaxMiB)' '(present && mib >= kAppVisMinMiB)' "VIS 80 (pure): an absurd navi48-appvis value is taken as the cap"
plant 81 $P 'constexpr bool hi_first(bool appsOn, bool isWs, bool hiOk) { return appsOn && !isWs && hiOk; }' 'constexpr bool hi_first(bool appsOn, bool isWs, bool hiOk) { (void)isWs; return appsOn && hiOk; }' "VIS 81 (pure): WindowServer too is placed hi first"
plant 82 $E '                if (!n48native::g4::appvis_ok(appsNow, s->ws, gVisOthers, p.size, appsNow ? appvis_cap() : 0ull)) {' '                if (false) {' "VIS 82 (kernel): bo_create never applies the visible cap"
plant 83 $E '                if (n48native::g4::appvis_counts(appsNow, s->ws, true)) { b.visCounted = 1u; gVisOthers += p.size; }' '                (void)appsNow;' "VIS 83 (kernel): visible placements of other sessions are never counted"
plant 84 $E '    if (b.visCounted != 0u) { gVisOthers = gVisOthers >= b.size ? gVisOthers - b.size : 0ull; b.visCounted = 0u; }   // 0.0.641' '    (void)0;' "VIS 84 (kernel): a freed visible BO stays counted forever"
plant 85 $E '            bool hiDone = n48native::g4::hi_first(appsNow, s->ws, p.hiOk) && ' '            bool hiDone = false && ' "VIS 85 (kernel): bo_create never tries the hi pool first"
plant 86 $E '        if (apps_on()) { g.appVisBytes = gVisOthers; g.appVisCapMiB = (uint32_t)(appvis_cap() >> 20); }' '        (void)0;' "VIS 86 (kernel): sessstat 4 reports neither the visible use nor the cap"
plant 87 $G2 'constexpr uint64_t appvis_bits(uint64_t visBytes, uint32_t capMiB) { return (((visBytes >> 10) & 0xFFFFFull) << 36) | ((uint64_t)(capMiB & 0xFFu) << 56); }' 'constexpr uint64_t appvis_bits(uint64_t visBytes, uint32_t capMiB) { return (((visBytes >> 10) & 0xFFFFFull) << 36) | ((uint64_t)(capMiB & 0xFFu) << 57); }' "VIS 87 (pure): the cap rides in the wrong bits of the stat word"
plant 88 $C '(out[5] >> 36) & 0xfffff, (out[5] >> 56));' '(out[5] >> 36) & 0xfffff, (out[5] >> 57));' "VIS 88 (CLI): navi48test decodes the cap from the wrong bits"
# item 5: WindowServer always gets a slot
plant 89 $G2 '        if (ws ? wsOpen : (!wsOpen && free_slots(state) <= 1u)) return OpenPick{ kExclusive, kNoSlot };' '        if (ws ? wsOpen : (!wsOpen && free_slots(state) <= 0u)) return OpenPick{ kExclusive, kNoSlot };' "SLOT 89 (pure): no slot is reserved for WindowServer"
plant 90 $G2 '        if (ws ? wsOpen : (!wsOpen && free_slots(state) <= 1u)) return OpenPick{ kExclusive, kNoSlot };' '        if (ws ? (openCount != 0u) : (!wsOpen && free_slots(state) <= 1u)) return OpenPick{ kExclusive, kNoSlot };' "SLOT 90 (pure): WindowServer is still refused whenever anything is open"
plant 91 $G2 '        if (ws ? wsOpen : (!wsOpen && free_slots(state) <= 1u)) return OpenPick{ kExclusive, kNoSlot };' '        if (ws ? wsOpen : (free_slots(state) <= 1u)) return OpenPick{ kExclusive, kNoSlot };' "SLOT 91 (pure): the reserve also binds when WindowServer already has its session"
plant 92 $G2 'constexpr bool ws_already_open(bool apps, bool anyOpen, bool wsOpen) { return apps ? wsOpen : anyOpen; }' 'constexpr bool ws_already_open(bool apps, bool anyOpen, bool wsOpen) { (void)apps; (void)wsOpen; return anyOpen; }' "SLOT 92 (pure): the open policy still refuses WindowServer whenever any client is open"
plant 93 $G2 'ws, bool g1Holds, bool apps = false, bool wsOpen = false) {' 'ws, bool g1Holds, bool apps = true, bool wsOpen = false) {' "SLOT 93 (pure): the new rules are ON by default (apps absent no longer identical)"
plant 94 $E 'const OpenPick pk = open_pick(multi, gSlotState, openNow, ws, g1_holds(), appsNow, wsOpen);' 'const OpenPick pk = open_pick(multi, gSlotState, openNow, ws, g1_holds(), false, wsOpen);' "SLOT 94 (kernel): n1c_open never passes the apps flag to open_pick"
plant 95 $E 'const bool appsNow = apps_on(), wsOpen = __atomic_load_n(&gSessWs, __ATOMIC_SEQ_CST) != 0u;' 'const bool appsNow = apps_on(), wsOpen = false;' "SLOT 95 (kernel): n1c_open never tells open_pick that WindowServer is open"
plant 96 $N 'n48native::g2::ws_already_open(appsOn, amdgpu::n1c_is_open(), amdgpu::n1c_ws_is_open())' 'amdgpu::n1c_is_open()' "SLOT 96 (kernel): the client class asks the open policy 'is anything open' with apps ON too"
plant 97 $O 'constexpr uint32_t kWindowServerUid = 88u;' 'constexpr uint32_t kWindowServerUid = 89u;' "SLOT 97: the WindowServer identity rule changed"
# item 6: the strike count
plant 98 $P 'constexpr bool strike_refused(uint32_t count) { return count >= kStrikeLimit; }' 'constexpr bool strike_refused(uint32_t count) { return count > kStrikeLimit; }' "STRIKE 98 (pure): a name is refused only at the fourth strike"
plant 99 $P 'constexpr bool app_admit(bool onAllowList, uint32_t strikes) { return onAllowList && !strike_refused(strikes); }' 'constexpr bool app_admit(bool onAllowList, uint32_t strikes) { (void)strikes; return onAllowList; }' "STRIKE 99 (pure): strikes never refuse an open"
plant 100 $E '    if (act == kStallRecover && __atomic_load_n(&gs->app, __ATOMIC_ACQUIRE)) {' '    if (false && __atomic_load_n(&gs->app, __ATOMIC_ACQUIRE)) {' "STRIKE 100 (kernel): an automatic recovery blamed on an APP session adds no strike"
plant 101 $E '    return n48native::g4::app_admit(listed, n48native::g4::strike_count(gStrikes, key));' '    return listed;' "STRIKE 101 (kernel): the open path ignores the strike table"
plant 102 $E '    s->appKey = key;' '    (void)key;' "STRIKE 102 (kernel): an APP session never records its name key (every strike lands on key 0)"
plant 103 $E 'const uint32_t rc = verb_run(apps_on(), gAppAllow, arg, o, &gStrikes);' 'const uint32_t rc = verb_run(apps_on(), gAppAllow, arg, o, nullptr);' "STRIKE 103 (kernel): the verb cannot show the strike table"
plant 104 $P '        if (__atomic_load_n(&t.key[i], __ATOMIC_ACQUIRE) == key || __atomic_compare_exchange_n(&t.key[i], &e, key, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) || e == key)
            return __atomic_add_fetch(&t.n[i], 1u, __ATOMIC_ACQ_REL);' '        if (__atomic_load_n(&t.key[i], __ATOMIC_ACQUIRE) == key || __atomic_compare_exchange_n(&t.key[i], &e, key, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) || e == key)
            return __atomic_add_fetch(&t.n[i], 2u, __ATOMIC_ACQ_REL);' "STRIKE 104 (pure): a strike counts double"
plant 105 $N '		onAllow = amdgpu::n1c_app_allowed(comm, &struckOut);   // 0.0.641: also false for a listed name with 3 strikes (struckOut)
		appKey = n48a_comm_key(comm);' '		onAllow = amdgpu::n1c_app_allowed(comm, &struckOut);   // 0.0.641: also false for a listed name with 3 strikes (struckOut)' "STRIKE 105 (kernel): the client never remembers the name key it opens create() with"
# item 7: wording
plant 106 $O 'case kReasonNotPrivileged: return "not root (euid != 0)";' 'case kReasonNotPrivileged: return "not an administrator";' "WORD 106: the refusal text says 'not an administrator' again"
# item 8 (0.0.650): the allow-list add under concurrency
plant 107 $P '            if (!__atomic_compare_exchange_n(&l[i], &e, key, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {' '            if (__atomic_load_n(&l[i], __ATOMIC_ACQUIRE) != 0ull ? (e = __atomic_load_n(&l[i], __ATOMIC_ACQUIRE), true) : (__atomic_store_n(&l[i], key, __ATOMIC_RELEASE), false)) {' "ALLOW 107 (pure): the slot is claimed with a load + plain store, not one compare-and-swap"
plant 108 $P '                if (j < i) { uint64_t k2 = key; __atomic_compare_exchange_n(&l[i], &k2, 0ull, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); return kAllowExists; }' '                if (j < i) { return kAllowExists; }' "ALLOW 108 (pure): the higher adder keeps its duplicate slot (no lowest-slot settle)"
plant 109 $P '                uint64_t k3 = key; __atomic_compare_exchange_n(&l[j], &k3, 0ull, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);' '                (void)j;' "ALLOW 109 (pure): the lower adder leaves the higher duplicate in place"
echo "native_g4_plant: $((total - 1)) plants (+ the baseline), $escaped escaped or did not apply"
[ $escaped -eq 0 ]
