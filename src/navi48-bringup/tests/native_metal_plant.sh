#!/bin/zsh
# native_metal_plant.sh - planted breaks for tests/native_metal_test.cpp (build 0.0.610, extended 0.0.611, milestone #9 route A: the Metal nub and its ops table).
# For each plant: copy the real sources into a scratch tree, apply the break (one or several exact-once replacements; the script first proves each replaced text occurs
# exactly once), compile the real test against the scratch tree and demand that it FAILS (a compile error, a failing check, a crash or a hang cut off by the alarm all count).
# A plant the suite lets through is a hole: the script exits non-zero and says which. The CONTROL (no break) must pass.
#   run from anywhere:  src/navi48-bringup/tests/native_metal_plant.sh [first-plant-id last-plant-id]
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
K=src/navi48-bringup
SR=$K/src
SCR="${TMPDIR:-/tmp}/nmetal-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0
FIRST=${1:-0}; LAST=${2:-9999}

fresh() {
  rm -rf "$SCR"; mkdir -p "$SCR/$K/tests" "$SCR/notes/design" "$SCR/tools/native"
  cp -R "$ROOT/$SR" "$SCR/$SR"
  cp "$ROOT/$K/Makefile" "$ROOT/$K/Info.plist" "$SCR/$K/"
  cp "$ROOT/$K/tests/native_metal_test.cpp" "$SCR/$K/tests/"
  cp "$ROOT/an internal design note" "$SCR/notes/design/"
  cp "$ROOT/tools/native/n48nub.c" "$ROOT/tools/native/build.sh" "$SCR/tools/native/"
}
build_run() {   # -> prints the verdict line; returns 0 if the suite PASSED, 1 if it failed, 2 if it did not compile
  local out
  if [ -n "${PLANT_DRY:-}" ]; then echo "(dry run: applicability only)"; return 1; fi
  out=$(cd "$SCR" && clang++ -std=c++17 -Wall -Wextra -O0 -I $SR -I $SR/amd $K/tests/native_metal_test.cpp -o "$SCR/t" 2>&1)
  if [ $? -ne 0 ]; then echo "CAUGHT at compile time: $(echo "$out" | /usr/bin/grep -m1 -E 'error|static assertion' | cut -c1-140)"; return 2; fi
  out=$(cd "$SCR" && perl -e 'alarm 120; exec @ARGV' ./t . 2>&1)
  local trc=$?
  if [ $trc -eq 0 ]; then echo "the suite passed: $(echo "$out" | tail -1)"; return 0; fi
  if [ $trc -ge 128 ]; then echo "CAUGHT by a crash or hang (signal $((trc-128)))"; return 1; fi
  echo "CAUGHT by $(echo "$out" | /usr/bin/grep -c '^FAIL') check(s); first: $(echo "$out" | /usr/bin/grep -m1 '^FAIL' | cut -c1-120)"; return 1
}
plant() {   # plant <id> <file relative to repo root> <description> <old1> <new1> [<old2> <new2> ...]
  local id="$1" file="$2" desc="$3"; shift 3
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
  local msg; msg=$(build_run); local brc=$?
  if [ $brc -eq 0 ]; then echo "PLANT $id: $desc: *** ESCAPED ($msg) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: $msg"; fi
}
control() {
  fresh; total=$((total+1))
  local msg; msg=$(build_run); local brc=$?
  if [ $brc -eq 0 ]; then echo "CONTROL (no break): $msg"; else echo "CONTROL (no break): *** FAILED: $msg ***"; escaped=$((escaped+1)); fi
}

P=$SR/amd/native_metal_pure.h
N=$SR/Navi48MetalNub.cpp
C=$SR/Navi48NativeClient.cpp
B=$SR/Navi48Bringup.cpp
control
# ---- the gate (fail-open directions) ----
plant 1 $P "FAIL-OPEN: the boot-arg is not required to publish" 'if (!g.bootarg) return kUnsupported;' '(void)0;'
plant 2 $P "FAIL-OPEN: S1b POSITIVE PASS is not required" 'if (!g.s1bGateOn || !g.s1bRan || !g.s1bPositive || g.s1bStopped) return kNotReady;' 'if (!g.s1bGateOn || !g.s1bRan || g.s1bStopped) return kNotReady;'
plant 3 $P "FAIL-OPEN: a HUNG GPU may publish" '    if (g.hung) return kNotReady;
' ''
plant 4 $P "FAIL-OPEN: publish without Hello" 'inline uint32_t publish_verdict(const GateIn &g, State st) {
    if (!g.hello) return kNotReady;' 'inline uint32_t publish_verdict(const GateIn &g, State st) {'
plant 5 $P "FAIL-OPEN: the native S1b gate need not be on" 'if (!g.s1bGateOn || !g.s1bRan || !g.s1bPositive || g.s1bStopped) return kNotReady;' 'if (!g.s1bRan || !g.s1bPositive || g.s1bStopped) return kNotReady;'
plant 6 $P "FAIL-OPEN: a latched S1b stop does not block" 'if (!g.s1bGateOn || !g.s1bRan || !g.s1bPositive || g.s1bStopped) return kNotReady;' 'if (!g.s1bGateOn || !g.s1bRan || !g.s1bPositive) return kNotReady;'
plant 7 $P "a second nub may be published over the first" '    if (st == kPublished) return kExclusive;
' ''
plant 8 $P "a nub may be published while the old one is still terminating" '    if (st == kTerminating) return kBusy;
    return kOk;
}
// Withdraw' '    return kOk;
}
// Withdraw'
plant 9 $P "NUB AT BOOT: publish_at_boot() says yes" 'inline bool publish_at_boot() { return false; }' 'inline bool publish_at_boot() { return true; }'
plant 10 $P "withdraw needs the boot-arg (cleanup becomes impossible)" '    if (g.flags != 0) return kBadArg;
    if (st == kPublished) return kOk;' '    if (g.flags != 0) return kBadArg;
    if (!g.bootarg) return kUnsupported;
    if (st == kPublished) return kOk;'
plant 11 $P "withdraw with no nub reports success" '    if (st == kTerminating) return kBusy;
    return kNotFound;' '    if (st == kTerminating) return kBusy;
    return kOk;'
plant 12 $P "the nub state never returns to Off after free()" 'inline State sm_after_free(State) { return kOff; }' 'inline State sm_after_free(State s) { return s; }'
# ---- IOAccelConfig ----
plant 13 $P "WRONG CONFIG BYTE: +0x08 is 0x480001" '{ kOffF0,     4, 0x480000ull },' '{ kOffF0,     4, 0x480001ull },'
plant 14 $P "WRONG CONFIG BYTE: +0x47 stays 1 (type-4 display pipe enabled on a headless nub)" '{ kOffPipeUC, 1, 0ull },' '{ kOffPipeUC, 1, 1ull },'
plant 15 $P "WRONG CONFIG BYTE: the +0x30 store is 16 bits wide (+0x32 keeps the family default)" '{ kOffLim30,  4, 0x40004000ull },' '{ kOffLim30,  2, 0x4000ull },'
plant 16 $P "the static check is gone (populate always says ok)" '    if (v != kCfgOk) return (int)(10u + (uint32_t)v);                // nothing copied back' ''
plant 17 $P "the fill zeroes the whole structure first (memset: the family defaults are lost)" 'inline void config_fill(uint8_t *cfg, uint64_t namePtr) {
' 'inline void config_fill(uint8_t *cfg, uint64_t namePtr) {
    for (uint32_t i = 0; i < kCfgBytes; ++i) cfg[i] = 0;
'
plant 18 $P "the +0x54 IOSurface limit is not checked" '    if (ld_le(cfg + kOffLim54, 4) == 0) return kCfgLimit54;
' ''
plant 19 $P "the +0x30 IOSurface limit is not checked" '    if (ld_le(cfg + kOffLim30, 2) == 0) return kCfgLimit30;
' ''
plant 20 $P "a store is missing (+0x5c)" '    { kOffF5c,    4, 4ull },
' ''
plant 21 $P "a store has the wrong offset (+0x64 -> +0x60)" '{ kOffF64,    4, 2ull },' '{ 0x60,        4, 2ull },'
plant 22 $P "the stamp page may lie above the 2^52 mask (and the 4 KiB alignment check via the mask is gone)" 'len >= kStampBytes && (phys & 0xfffull) == 0 && (phys & ~kStampPhysMask) == 0; }' 'len >= kStampBytes && (phys & 0xfffull) == 0; }'
plant 23 $P "the optional factories are ON by default" 'return argPresent ? (argValue & N48_FACT_ALL) : 0u;' 'return argPresent ? (argValue & N48_FACT_ALL) : N48_FACT_SYSMEMORY;'
plant 24 $P "banned string in the pure header" '// ---- the stamp page ----' '// pipe+0x2'"80"' is banned
// ---- the stamp page ----'
# ---- the kext glue ----
plant 25 $B "NUB AT BOOT: Navi48Bringup publishes the nub itself" '	Navi48MetalNub::shutdown();' '	Navi48MetalNub::selectorPublish(this, 0, nullptr);
	Navi48MetalNub::shutdown();'
plant 26 $N "ORDERING: the nub is allocated BEFORE the gate verdict" '	IOLockLock(gLock);
	const n48metal::GateIn g = gate_now(flags, true);' '	IOLockLock(gLock);
	Navi48MetalNub *early = OSTypeAlloc(Navi48MetalNub); if (early) early->release();
	const n48metal::GateIn g = gate_now(flags, true);'
plant 27 $N "ORDERING: the nub is registered before it is attached" 'if (nub->attach(owner)) {' 'if ((nub->registerService(), nub->attach(owner))) {'
plant 28 $N "the publish gate does not ask for the boot-arg" 'gate_now(flags, true)' 'gate_now(flags, false)'
plant 29 $N "the boot-arg counts for any non-zero value" 'v == 1u;' 'v != 0u;'
plant 30 $N "the gate ignores the S1b POSITIVE flag" 'g.s1bPositive = s.positivePass;' 'g.s1bPositive = true;'
plant 31 $N "the gate ignores HUNG" 'g.hung = amdgpu::n1c_hung();' 'g.hung = false;'
plant 32 $N "the gate does not read the Hello state" 'g.hello = amdgpu::n1c_hello_done();' 'g.hello = true;'
plant 33 $N "the populate hook is not the pure config_populate" 'n48metal::config_populate(cfg, bytes, (uint64_t)(uintptr_t)kAccelName)' '0'
plant 34 $N "the ops table carries the wrong build" 'const N48MetalOps gOps = {
	N48_METAL_OPS_MAGIC, N48_METAL_ABI, (uint32_t)sizeof(N48MetalOps), 664u,' 'const N48MetalOps gOps = {
	N48_METAL_OPS_MAGIC, N48_METAL_ABI, (uint32_t)sizeof(N48MetalOps), 610u,'
plant 35 $N "the memory-map hook claims success" 'return (int)kIOReturnUnsupported;' 'return 0;'
plant 36 $N "the nub source touches a register" '	MNLOG("device_close");' '	MNLOG("device_close"); (void)"WREG32";'
plant 37 $N "device_close is not identity-checked (a stale ctx frees the live device)" 'if (!d || d != gDev || d->magic != kDevMagic) return;' 'if (!d) return;'
plant 38 $C "the publish selector accepts the wrong shape" 'if (!shape(1, 4, 0, 0)) return kIOReturnBadArgument;
		return Navi48MetalNub::selectorPublish' 'if (!shape(1, 3, 0, 0)) return kIOReturnBadArgument;
		return Navi48MetalNub::selectorPublish'
plant 39 $C "the withdraw selector is not dispatched" 'case N48N_SEL_METAL_NUB_WITHDRAW:' 'case 20 + 100:'
plant 40 $SR/amd/native_s1c.h "the kext build is not bumped" 'kN1cKextBuild = 664;' 'kN1cKextBuild = 610;'
plant 41 $K/Info.plist "the plist is not bumped (one occurrence)" '<key>CFBundleShortVersionString</key>
	<string>0.0.664</string>' '<key>CFBundleShortVersionString</key>
	<string>0.0.610</string>'
plant 42 $SR/Navi48NativeABI.h "the ABI minor is not bumped" '#define N48N_ABI_MINOR     12u ' '#define N48N_ABI_MINOR     7u '
plant 43 $SR/Navi48NativeABI.h "the withdraw selector moves" 'N48N_SEL_METAL_NUB_WITHDRAW = 20,' 'N48N_SEL_METAL_NUB_WITHDRAW = 21,'
plant 44 $SR/Navi48MetalOps.h "the ops table grows a member in the middle (offsets move)" '    uint32_t reserved0;    /*  20  0 */' '    uint32_t reserved0;    /*  20  0 */
    uint32_t sneaky;'
plant 45 an internal design note "the ABI addendum is missing" '## ABI 1.8 addendum' '## ABI addendum'
plant 46 $SR/Navi48NativeClient.cpp "a third caller publishes from the client (case duplicated outside the guard)" 'return Navi48MetalNub::selectorWithdraw(si[0], so);' 'Navi48MetalNub::selectorPublish(owner, 0, so); return Navi48MetalNub::selectorWithdraw(si[0], so);'

plant 49 $N "DEADLOCK: the failed-publish path releases the nub while holding the lock (free() takes it)" '		IOLockUnlock(gLock);
		if (nub) nub->release();                                                  // AFTER the unlock: the last release runs free(), which takes gLock' '		if (nub) nub->release();
		IOLockUnlock(gLock);'
plant 47 tools/native/build.sh "build.sh forgets to build n48nub" 'n48nub:n48nub.c' 'n48mode:n48mode.c'
plant 48 tools/native/n48nub.c "the tool publishes without saying Hello" 'if (hello(&c)) return 1;
        const uint64_t in[1] = { 0 }; uint64_t o[4]' 'if (n48n_open(&c)) return 1;
        const uint64_t in[1] = { 0 }; uint64_t o[4]'
plant 50 $N "the nub is not retained across registerService() (a racing withdraw frees it)" '	nub->retain();' '	(void)nub;'
plant 51 $SR/Navi48MetalOps.h "the ops header drops the generic hook member" '    uint64_t caps;' '    uint64_t caps_;'
plant 52 $P "the caps constant loses the hook bit" 'constexpr uint64_t kOpsCaps = N48_CAP_VHOOK | N48_CAP_NATIVE_OPEN;' 'constexpr uint64_t kOpsCaps = N48_CAP_NATIVE_OPEN;'

# ---- 0.0.611: config atomicity, vhook rate limit, vhook output-parameter writes ----
plant 53 $P "config_populate fills the CALLER's struct in place (a refusal leaves our values there)" '    config_fill(work, namePtr);
    const ConfigVerdict v = config_check(work);' '    config_fill(cfg, namePtr);
    const ConfigVerdict v = config_check(cfg);'
plant 54 $P "config_populate copies the filled copy back even when the check refused" '    if (v != kCfgOk) return (int)(10u + (uint32_t)v);                // nothing copied back' '    if (v != kCfgOk) { for (uint32_t i = 0; i < kCfgBytes; ++i) cfg[i] = work[i]; return (int)(10u + (uint32_t)v); }'
plant 55 $P "vhook logging is not rate limited (every call logs)" 'return n <= kVhookLogFirst ? kVhLogFull : (n % kVhookLogEvery == 0u ? kVhLogCount : kVhLogNone);' 'return kVhLogFull;'
plant 56 $P "vhook logging never emits the periodic count line" '(n % kVhookLogEvery == 0u ? kVhLogCount : kVhLogNone)' 'kVhLogNone'
plant 57 $P "the call counter is per class only (slots share it)" '| (slot & 0xffffu);' '| 0u;'
plant 58 $P "an overflowing table logs every call (the shared bucket is not counted)" '    return __atomic_add_fetch(&t.n[kVhookSlots - 1u], 1u, __ATOMIC_RELAXED);' '    return 1u;'
plant 59 $P "getPhysicalSegment does not write *len = 0" '{ *(volatile uint64_t *)(uintptr_t)args[1] = 0; *ret = 0; return 1; }' '{ *ret = 0; return 1; }'
plant 60 $P "allocPhysical returns false" '{ *ret = 1; return 1; }' '{ *ret = 0; return 1; }'
plant 61 $P "getLevelOffset / getBackingLevelOffset do not write *int = 0" '{ *(volatile uint32_t *)(uintptr_t)args[2] = 0; *ret = 0; return 1; }' '{ *ret = 0; return 1; }'
plant 62 $P "calculateIOSurfaceDeviceCacheVRAMBytes leaves the second output unwritten" '*(volatile uint64_t *)(uintptr_t)args[0] = 0; *(volatile uint64_t *)(uintptr_t)args[1] = 0; *ret = 0; return 1;' '*(volatile uint64_t *)(uintptr_t)args[0] = 0; *ret = 0; return 1;'
plant 63 $P "the output pointers need not be in the kernel half" 'return p != 0 && (p % align) == 0 && p >= kmin; }' 'return p != 0 && (p % align) == 0; }'
plant 64 $P "the output pointers need not be aligned" 'return p != 0 && (p % align) == 0 && p >= kmin; }' 'return p != 0 && p >= kmin; }'
plant 65 $P "the VidMemory handling ignores its factory bit" 'cls == N48_VC_VIDMEMORY && (mask & N48_FACT_VIDMEMORY) != 0u' 'cls == N48_VC_VIDMEMORY'
plant 66 $P "the Resource handling ignores its factory bit" 'cls == N48_VC_RESOURCE && (mask & N48_FACT_RESOURCE) != 0u' 'cls == N48_VC_RESOURCE'
plant 67 $P "slot 62: the second pointer is not checked before the first is written" 'vhook_ptr_ok(args[1], 8u, kmin) && args[0] != args[1]' 'true'
plant 68 $P "the VidMemory mask bit is the Resource bit" '(mask & N48_FACT_VIDMEMORY) != 0u' '(mask & N48_FACT_RESOURCE) != 0u'
plant 69 $P "another class (event machine) is handled" '    if (cls == N48_VC_RESOURCE && (mask & N48_FACT_RESOURCE) != 0u) {' '    if (cls == N48_VC_EVENTMACHINE) { *ret = 0; return 1; }
    if (cls == N48_VC_RESOURCE && (mask & N48_FACT_RESOURCE) != 0u) {'
plant 70 $P "slot 43 is handled with the wrong argument count" 'slot == 43u && nargs == 2u' 'slot == 43u'
plant 71 $N "op_vhook logs unconditionally (an extra MNLOG outside the rate limit)" '	if (k == n48metal::kVhLogFull) MNLOG(' '	MNLOG("vhook every call");
	if (k == n48metal::kVhLogFull) MNLOG('
plant 72 $N "op_vhook reads the boot-arg on every call (the hot path)" '	if (cls == N48_VC_VIDMEMORY || cls == N48_VC_RESOURCE) mask = op_factory_mask(nullptr);' '	mask = op_factory_mask(nullptr);'
plant 73 $N "op_vhook ignores the pure handler" 'n48metal::vhook_handle(mask, cls, slot, args, nargs, ret, n48metal::kKernelMin)' '0'
plant 74 $N "op_vhook passes a zero kernel base (a user pointer is written)" 'n48metal::vhook_handle(mask, cls, slot, args, nargs, ret, n48metal::kKernelMin)' 'n48metal::vhook_handle(mask, cls, slot, args, nargs, ret, 0)'

# ---- review round: the total count of Navi48MetalNub references in native_s1c.cpp is pinned at 2 (include + hungLatched) ----
plant 90 $SR/amd/native_s1c.cpp "native_s1c.cpp gains a third reference to the nub (a new use)" '    Navi48MetalNub::hungLatched();' '    Navi48MetalNub::hungLatched(); (void)sizeof(Navi48MetalNub);'
plant 91 $SR/amd/native_s1c.cpp "native_s1c.cpp drops the hungLatched call (Ready would never go to 0)" '    Navi48MetalNub::hungLatched();' '    (void)0;'

echo "plants: $total run, $escaped escaped/failed"
[ "$escaped" -eq 0 ]
