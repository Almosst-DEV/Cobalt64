#!/bin/zsh
# native_g6_plant.sh - planted breaks for tests/native_g6_test.cpp (build 0.0.656, G6: sandboxed apps reach the N48N client through the IOAccelerator; an internal design note "G6 design" section 4 step 1).
# For each plant: copy the real sources into a scratch tree, apply the break (exact-once replacements; the script first proves each replaced text occurs exactly once),
# compile the named real test against the scratch tree and demand that it FAILS (a compile error, a failing check, a crash or a hang cut off by the alarm all count).
# A plant the suite lets through is a hole: the script exits non-zero and says which. The CONTROLS (no break) must pass.
#   run from anywhere:  src/navi48-bringup/tests/native_g6_plant.sh [first-plant-id last-plant-id]
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
K=src/navi48-bringup
SR=$K/src
SCR="${TMPDIR:-/tmp}/ng6-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0
FIRST=${1:-0}; LAST=${2:-9999}

fresh() {
  rm -rf "$SCR"; mkdir -p "$SCR/$K/tests" "$SCR/tools"
  cp -R "$ROOT/$SR" "$SCR/$SR"
  cp "$ROOT/$K/Makefile" "$ROOT/$K/Info.plist" "$SCR/$K/"
  cp "$ROOT/$K/tests/native_g6_test.cpp" "$SCR/$K/tests/"
  cp -R "$ROOT/tools/native" "$SCR/tools/native"
}
build_run() {   # $1 = test name; prints the verdict line; returns 0 if the suite PASSED, 1 if it failed, 2 if it did not compile
  local t="$1" out
  out=$(cd "$SCR" && clang++ -std=c++17 -Wall -Wextra -O0 -I $SR -I $SR/amd $K/tests/$t.cpp -o "$SCR/t" 2>&1)
  if [ $? -ne 0 ]; then echo "CAUGHT at compile time: $(echo "$out" | /usr/bin/grep -m1 -E 'error|static assertion' | cut -c1-140)"; return 2; fi
  out=$(cd "$SCR" && perl -e 'alarm 120; exec @ARGV' ./t . 2>&1)
  local trc=$?
  if [ $trc -eq 0 ]; then echo "the suite passed: $(echo "$out" | tail -1)"; return 0; fi
  if [ $trc -ge 128 ]; then echo "CAUGHT by a crash or hang (signal $((trc-128)))"; return 1; fi
  echo "CAUGHT by $(echo "$out" | /usr/bin/grep -c '^FAIL') check(s); first: $(echo "$out" | /usr/bin/grep -m1 '^FAIL' | cut -c1-120)"; return 1
}
plant() {   # plant <id> <test> <file relative to repo root> <description> <old1> <new1> [<old2> <new2> ...]
  local id="$1" test="$2" file="$3" desc="$4"; shift 4
  if [ "$id" -lt "$FIRST" ] || [ "$id" -gt "$LAST" ]; then return; fi
  fresh
  local pairs=""
  while [ $# -ge 2 ]; do pairs="${pairs}${1}"$'\x1e'"${2}"$'\x1d'; shift 2; done
  PAIRS="$pairs" FILE="$SCR/$file" python3 - <<'PY'
import os,sys
p=os.environ['FILE']; s=open(p).read()
for pair in os.environ['PAIRS'].split('\x1d'):
    if not pair: continue
    o,n=pair.split('\x1e')
    if s.count(o)!=1:
        print("PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %r" % (s.count(o), o[:80])); sys.exit(3)
    s=s.replace(o,n)
open(p,'w').write(s)
PY
  local rc=$?
  total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  local msg; msg=$(build_run $test); local brc=$?
  if [ $brc -eq 0 ]; then echo "PLANT $id: $desc: *** ESCAPED ($msg) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: $msg"; fi
}
control() {
  fresh; total=$((total+1))
  local msg; msg=$(build_run native_g6_test); local brc=$?
  if [ $brc -eq 0 ]; then echo "CONTROL native_g6_test (no break): $msg"; else echo "CONTROL native_g6_test (no break): *** FAILED: $msg ***"; escaped=$((escaped+1)); fi
}
OP=$SR/amd/native_open_policy_pure.h
MP=$SR/amd/native_metal_pure.h
C=$SR/Navi48NativeClient.cpp
CH=$SR/Navi48NativeClient.hpp
N=$SR/Navi48MetalNub.cpp
B=$SR/Navi48Bringup.cpp
O=$SR/Navi48MetalOps.h
T=native_g6_test
control
# ---- the route x selector rule (the accelerator route must be the APP set ONLY) ----
plant 1 $T $OP "FAIL-OPEN: the accelerator route reaches selector 19 / 20 (Metal nub publish / withdraw: the client would terminate its own provider)" 'constexpr bool route_selector_ok(Route r, uint32_t sel) { return r == kRouteBringup || selector_allowed_for_app(sel); }' 'constexpr bool route_selector_ok(Route r, uint32_t sel) { return r == kRouteBringup || selector_allowed_for_app(sel) || sel == 19u || sel == 20u; }'
plant 2 $T $OP "FAIL-OPEN: the accelerator route is the WindowServer set (scanout, ReadRegs)" 'constexpr bool route_selector_ok(Route r, uint32_t sel) { return r == kRouteBringup || selector_allowed_for_app(sel); }' 'constexpr bool route_selector_ok(Route r, uint32_t sel) { return r == kRouteBringup || selector_allowed_for_windowserver(sel); }'
plant 3 $T $OP "FAIL-OPEN: an administrator on the accelerator route gets every selector" 'constexpr bool route_selector_ok(Route r, uint32_t sel) { return r == kRouteBringup || selector_allowed_for_app(sel); }' 'constexpr bool route_selector_ok(Route r, uint32_t sel) { (void)sel; return true; }'
plant 4 $T $OP "the Bringup route loses a selector (WindowServer's scanout)" 'constexpr bool route_selector_ok(Route r, uint32_t sel) { return r == kRouteBringup || selector_allowed_for_app(sel); }' 'constexpr bool route_selector_ok(Route r, uint32_t sel) { return selector_allowed_for_app(sel); }'
plant 5 $T $OP "the accelerator route drops selector 21 (host import)" 'constexpr bool route_selector_ok(Route r, uint32_t sel) { return r == kRouteBringup || selector_allowed_for_app(sel); }' 'constexpr bool route_selector_ok(Route r, uint32_t sel) { return r == kRouteBringup || (selector_allowed_for_app(sel) && sel != 21u); }'
plant 6 $T $OP "the composition forgets the route" '    return route_selector_ok(r, sel) && selector_allowed(admin, sel) && (!app || selector_allowed_for_app(sel));' '    return selector_allowed(admin, sel) && (!app || selector_allowed_for_app(sel));'
plant 7 $T $OP "an unknown route value is valid" 'constexpr bool route_valid(uint32_t r) { return r == (uint32_t)kRouteBringup || r == (uint32_t)kRouteAccelerator; }' 'constexpr bool route_valid(uint32_t r) { return r <= 3u; }'
# ---- the provider rule ----
plant 8 $T $OP "FAIL-OPEN: the accelerator route also accepts the owner as its provider" '    return r == kRouteBringup ? providerIsOwner : r == kRouteAccelerator ? providerIsRegisteredAccelerator : false;' '    return r == kRouteBringup ? providerIsOwner : r == kRouteAccelerator ? (providerIsRegisteredAccelerator || providerIsOwner) : false;'
plant 9 $T $OP "FAIL-OPEN: the Bringup route accepts the accelerator" '    return r == kRouteBringup ? providerIsOwner : r == kRouteAccelerator ? providerIsRegisteredAccelerator : false;' '    return r == kRouteBringup ? (providerIsOwner || providerIsRegisteredAccelerator) : r == kRouteAccelerator ? providerIsRegisteredAccelerator : false;'
plant 10 $T $OP "an unknown route accepts the accelerator" '    return r == kRouteBringup ? providerIsOwner : r == kRouteAccelerator ? providerIsRegisteredAccelerator : false;' '    return r == kRouteBringup ? providerIsOwner : providerIsRegisteredAccelerator;'
# ---- the client ----
plant 11 $T $C "THE CLIENT ATTACHES TO Navi48Bringup on the accelerator route (attach(owner))" '	if (!uc->attach(attachTo)) {' '	if (!uc->attach(owner)) {'
plant 12 $T $C "the client is started on the owner" '	if (!uc->start(attachTo))  { uc->detach(attachTo);' '	if (!uc->start(owner))  { uc->detach(attachTo);'
plant 13 $T $C "a failed start detaches from the wrong service" 'uc->detach(attachTo); amdgpu::n1c_close(ref, "start failed")' 'uc->detach(owner); amdgpu::n1c_close(ref, "start failed")'
plant 14 $T $C "FAIL-OPEN: the route check is removed from externalMethod (the accelerator route reaches 19 / 20)" '	if (!n48native::policy::route_selector_ok(route, selector)) {' '	if (false) {'
plant 15 $T $C "the route check comes after the role checks" '	if (!n48native::policy::route_selector_ok(route, selector)) {' '	if (!n48native::policy::selector_allowed(adminClient, selector) ? false : !n48native::policy::route_selector_ok(route, selector)) {'
plant 16 $T $C "the route refusal is a different code (an IOReturn read as success)" 'NCLOG("selector %u refused: not permitted on the accelerator route (APP selector set only)", selector);
		return kIOReturnNotPrivileged;' 'NCLOG("selector %u refused: not permitted on the accelerator route (APP selector set only)", selector);
		return kIOReturnSuccess;'
plant 17 $T $C "start() accepts any provider" '	if (!n48native::policy::start_provider_ok(route, isOwner, isAccel)) return false;' '	(void)isOwner; (void)isAccel;'
plant 18 $T $C "start() takes the accelerator identity from something else" 'provider == Navi48MetalNub::registeredAccelerator();' 'provider != nullptr;'
plant 19 $T $C "the route is stored after attach (start() would see the default)" '	uc->owner = owner;
	uc->route = route;' '	uc->owner = owner;'
plant 20 $T $C "create accepts an accelerator route with the owner as attachTo" '	if (route == n48native::policy::kRouteAccelerator && static_cast<IOService *>(owner) == attachTo) return kIOReturnBadArgument;' ''
plant 21 $T $C "create does not validate the route" '	if (!n48native::policy::route_valid((uint32_t)route)) return kIOReturnBadArgument;' ''
plant 22 $T $CH "the route defaults to the accelerator (a client built without create would be on the wrong route)" 'n48native::policy::Route route { n48native::policy::kRouteBringup };' 'n48native::policy::Route route { n48native::policy::kRouteAccelerator };'
plant 23 $T $CH "the client class derives from IOAccelerationUserClient (WebKit denies it by name)" 'class IOAccelNavi48NativeClient : public IOUserClient {' 'class IOAccelNavi48NativeClient : public IOAccelerationUserClient {'
plant 24 $T $B "WindowServer's Bringup entry creates its client on the accelerator route" 'IOAccelNavi48NativeClient::create(this, this, n48native::policy::kRouteBringup, owningTask' 'IOAccelNavi48NativeClient::create(this, this, n48native::policy::kRouteAccelerator, owningTask'
# ---- the ops hook ----
plant 25 $T $N "the hook attaches the client to Navi48Bringup (the owner) on the Bringup route" 'IOAccelNavi48NativeClient::create(owner, static_cast<IOService *>(accel), n48native::policy::kRouteAccelerator,' 'IOAccelNavi48NativeClient::create(owner, owner, n48native::policy::kRouteBringup,'
plant 26 $T $N "the hook creates the client on the accelerator route but attached to the wrong service" 'IOAccelNavi48NativeClient::create(owner, static_cast<IOService *>(accel), n48native::policy::kRouteAccelerator,' 'IOAccelNavi48NativeClient::create(owner, nub, n48native::policy::kRouteAccelerator,'
plant 27 $T $N "AN IOReturn IS READ AS A BOOL: the hook answers 1 = handled" '	return (int32_t)rc;
}' '	return rc == kIOReturnSuccess;
}'
plant 28 $T $N "the hook ignores the accelerator identity" '	const bool isAccel = live && accel != nullptr && accel == d->accel;' '	const bool isAccel = live;'
plant 29 $T $N "the hook ignores the device identity" '	const bool live = d != nullptr && d == __atomic_load_n(&gDev, __ATOMIC_ACQUIRE) && d->magic == kDevMagic;' '	const bool live = d != nullptr;'
plant 30 $T $N "the hook holds gLock across create (lock-order inversion with hungLatched)" '	if (gLock) { IOLockLock(gLock); nub = gNub; if (nub) nub->retain(); IOLockUnlock(gLock); }' '	if (gLock) { IOLockLock(gLock); nub = gNub; if (nub) nub->retain(); }'
plant 31 $T $N "the hook writes *handler on a refusal" '		if (rc == kIOReturnSuccess) { if (uc) *handler = uc; else rc = kIOReturnInternalError; }' '		*handler = uc; if (rc == kIOReturnSuccess && !uc) rc = kIOReturnInternalError;'
plant 32 $T $N "the OFF table lacks native_open" '	0u, 0u, nullptr, nullptr,
	op_native_open,
};' '	0u, 0u, nullptr, nullptr,
	nullptr,
};'
plant 33 $T $N "the display-ON table lacks native_open" '	N48_DISP_F_ON, 0u, op_disp_hook, op_pci_device,
	op_native_open,
};' '	N48_DISP_F_ON, 0u, op_disp_hook, op_pci_device,
	nullptr,
};'
plant 34 $T $N "the tables are chosen by ABI instead of the latch (OFF would publish the display-ON table)" 'ops_shape(n48disp_latched_on()).dispFlags != 0u ? &gOps : &gOpsOff;' 'ops_shape(n48disp_latched_on()).abi >= 3u ? &gOps : &gOpsOff;'
plant 35 $T $N "device_open does not remember the accelerator" 'd->accel = accel;' 'd->accel = nullptr;'
plant 36 $T $N "device_open accepts no accelerator" 'if (!nub || !accel || gDev) return nullptr;' 'if (!nub || gDev) return nullptr;'
plant 37 $T $MP "the capability bit is missing from the published caps" 'constexpr uint64_t kOpsCaps = N48_CAP_VHOOK | N48_CAP_NATIVE_OPEN;' 'constexpr uint64_t kOpsCaps = N48_CAP_VHOOK;'
plant 38 $T $MP "native_open_verdict: a wrong accelerator is accepted" 'return !typeIsN48N ? kUnsupported : !deviceLive ? kNotReady : !accelIsRegistered ? kBadArg : !nubPublished ? kNotReady : kOk;' 'return !typeIsN48N ? kUnsupported : !deviceLive ? kNotReady : !nubPublished ? kNotReady : kOk;'
plant 39 $T $MP "native_open_verdict: no nub is accepted" 'return !typeIsN48N ? kUnsupported : !deviceLive ? kNotReady : !accelIsRegistered ? kBadArg : !nubPublished ? kNotReady : kOk;' 'return !typeIsN48N ? kUnsupported : !deviceLive ? kNotReady : !accelIsRegistered ? kBadArg : kOk;'
plant 40 $T $MP "native_open_verdict: another type is accepted" 'return !typeIsN48N ? kUnsupported : !deviceLive' 'return !deviceLive'
# ---- the ops header ----
plant 41 $T $O "native_open moves (inserted before disp_flags)" '    int32_t (*native_open)(void *ctx, void *accel, void *task, void *security_id, uint32_t type, void **handler);   /* 144 */' '    int32_t (*native_open)(void *ctx, void *accel, void *task, void *security_id, uint32_t type, void **handler);   /* 144 */
    uint64_t extra_member;'
plant 42 $T $O "the capability bit collides with VHOOK" '#define N48_CAP_NATIVE_OPEN   (1ull << 1)' '#define N48_CAP_NATIVE_OPEN   (1ull << 0)'
plant 43 $T $O "the ops ABI stays 2" '#define N48_METAL_ABI         3u ' '#define N48_METAL_ABI         2u '
plant 44 $T $O "the type constant is not N48N" '#define N48_METAL_UC_N48N     0x4E34384Eu ' '#define N48_METAL_UC_N48N     0x4E34384Fu '
plant 45 $T $SR/amd/native_s1c.h "Hello reports the old build" 'constexpr uint32_t kN1cKextBuild = 664;' 'constexpr uint32_t kN1cKextBuild = 654;'
echo "plants: $total run, $escaped escaped/failed"
[ "$escaped" -eq 0 ]
