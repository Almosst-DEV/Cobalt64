#!/bin/zsh
# plant.sh - planted breaks for tests/host_test.cpp (Navi48Accel aux kext 0.0.3). For each plant: copy the real sources into a scratch tree, apply the break (exact-once
# replacements; the script first proves each replaced text occurs exactly once), compile the real test against the scratch tree and demand that it FAILS. A plant the suite
# lets through is a hole: the script exits non-zero and says which. The CONTROL (no break) must pass.
#   tools/native/navi48accel/tests/plant.sh [first last]     (BRING_ROOT=<bring-up tree root> to compare the two Navi48MetalOps.h copies; default ~/navi48-native/wt-s1)
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BRING="${BRING_ROOT:-$HOME/navi48-native/wt-s1}"
SCR="${TMPDIR:-/tmp}/n48accel-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0
FIRST=${1:-0}; LAST=${2:-9999}

fresh() {
  rm -rf "$SCR"; mkdir -p "$SCR/tests"
  cp -R "$HERE/src" "$SCR/src"; cp -R "$HERE/gates" "$SCR/gates"; mkdir -p "$SCR/build/gen"; cp "$HERE/build/gen/n48_tramp.inc" "$SCR/build/gen/" 2>/dev/null
  cp "$HERE/Info.plist" "$HERE/Makefile" "$SCR/"
  cp "$HERE/tests/host_test.cpp" "$SCR/tests/"
}
build_run() {
  local out
  if [ -n "${PLANT_DRY:-}" ]; then echo "(dry run: applicability only)"; return 1; fi
  out=$(cd "$SCR" && clang++ -std=c++17 -Wall -Wextra -O0 -I src tests/host_test.cpp -o "$SCR/t" 2>&1)
  if [ $? -ne 0 ]; then echo "CAUGHT at compile time: $(echo "$out" | /usr/bin/grep -m1 -E 'error' | cut -c1-140)"; return 2; fi
  out=$(cd "$SCR" && perl -e 'alarm 120; exec @ARGV' ./t . "$BRING" 2>&1)
  local trc=$?
  if [ $trc -eq 0 ]; then echo "the suite passed: $(echo "$out" | tail -1)"; return 0; fi
  if [ $trc -ge 128 ]; then echo "CAUGHT by a crash or hang (signal $((trc-128)))"; return 1; fi
  echo "CAUGHT by $(echo "$out" | /usr/bin/grep -c '^FAIL') check(s); first: $(echo "$out" | /usr/bin/grep -m1 '^FAIL' | cut -c1-130)"; return 1
}
plant() {   # plant <id> <file relative to the aux dir> <description> <old1> <new1> [<old2> <new2> ...]
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
S=src/Navi48Accel.cpp
P=src/n48accel_pure.h
control
# ---- kill switch ----
plant 1 $S "ORDERING: the kill switch is checked AFTER the nub check in probe" '	N48_AUX_ENTER(nullptr);
	if (!n48_provider_is_nub(provider)) return nullptr;      // nub only: never a PCI device' '	if (!n48_provider_is_nub(provider)) return nullptr;      // nub only: never a PCI device
	N48_AUX_ENTER(nullptr);'
plant 2 $S "newEventMachine has no kill switch" '	N48_AUX_ENTER(nullptr);
	return OSTypeAlloc(Navi48EventMachine);' '	return OSTypeAlloc(Navi48EventMachine);'
plant 3 $S "populateAccelConfig has no kill switch" '	N48_AUX_ENTER_V();
	uint8_t *c = (uint8_t *)cfg;' '	uint8_t *c = (uint8_t *)cfg;'
plant 4 $P "the kill switch is inverted" 'return !(argPresent && argValue == 0u);' 'return (argPresent && argValue == 0u);'
plant 5 $S "the kill switch reads another boot-arg" 'PE_parse_boot_argn("navi48-aux",' 'PE_parse_boot_argn("navi48-auxx",'
plant 6 $S "a second boot-arg is read" '	return aux_enabled(present, v);' '	uint32_t w = 0; (void)PE_parse_boot_argn("navi48-other", &w, sizeof(w));
	return aux_enabled(present, v);'
plant 7 $S "the createKernelGPUTask factory has no kill switch" '	N48_AUX_ENTER(nullptr);
	Navi48Task *t = OSTypeAlloc(Navi48Task);
	if (!t) return nullptr;
	if (!t->setup(this, 1u))' '	Navi48Task *t = OSTypeAlloc(Navi48Task);
	if (!t) return nullptr;
	if (!t->setup(this, 1u))'
# ---- the layout gate (fail-open directions) ----
plant 8 $P "FAIL-OPEN: an inherited slot that differs from the family is accepted" '} else if (ours[i] != fam[i]) {' '} else if (false) {'
plant 9 $P "FAIL-OPEN: an override outside our text is accepted" '            if (ours[i] < textLo || ours[i] >= textHi) { r.v = kGateOverrideOutside;' '            if (false) { r.v = kGateOverrideOutside;'
plant 10 $P "FAIL-OPEN: the zero terminator is not required (an extra virtual passes)" '    if (ours[nslots] != 0) { r.v = kGateNoTerminator;' '    if (false) { r.v = kGateNoTerminator;'
plant 11 $P "FAIL-OPEN: a NULL family slot is not refused" '        if (fam[i] == 0) { r.v = kGateFamilyNull; r.slot = i; return r; }
' ''
plant 12 $P "an override equal to the family slot is accepted" '            if (ours[i] == fam[i]) { r.v = kGateOverrideIsFamily;' '            if (false) { r.v = kGateOverrideIsFamily;'
plant 13 $P "FAIL-OPEN: zero slots is a vacuous PASS" '    if (nslots == 0) { r.v = kGateEmpty; return r; }
' ''
plant 14 $P "the gate stops after the first slot" '    for (uint32_t i = 0; i < nslots; ++i) {' '    for (uint32_t i = 0; i < 1 && i < nslots; ++i) {'
plant 15 $P "the class size check accepts any size" 'inline bool size_ok(uint32_t have, uint32_t want) { return have == want; }' 'inline bool size_ok(uint32_t have, uint32_t want) { return have != 0 || want == 0; }'
plant 16 $S "probe does not run the layout gate" '	if (!n48_layout_gate()) return nullptr;                  // fail closed' '	(void)n48_layout_gate;'
plant 17 $S "start does not require the gate" '	if (!gOps || !n48_provider_is_nub(provider) || gGateState != 1) return false;' '	if (!gOps || !n48_provider_is_nub(provider)) return false;'
plant 18 $S "probe accepts any provider (a PCI device)" '	if (!n48_provider_is_nub(provider)) return nullptr;      // nub only: never a PCI device
' ''
plant 19 $S "the gate result is not cached fail-closed (a FAIL is retried as unchecked)" '	OSCompareAndSwap(0, ok ? 1 : 2, &gGateState);' '	OSCompareAndSwap(0, 1, &gGateState);'
# ---- ops ----
plant 20 $P "the ops ABI is not checked" 'if (o->abi < N48_METAL_ABI_MIN) return kOpsAbi;' ';'
plant 21 $P "a too-small ops table is accepted" '    if (o->size < mySize) return kOpsSize;' '    if (o->size < 8) return kOpsSize;'
plant 22 $P "the task_window hook may be missing" ' || !o->stamp_va || !o->task_window) return kOpsMissing;' ' || !o->stamp_va) return kOpsMissing;'
plant 23 $P "the magic is not checked" '    if (o->magic != N48_METAL_OPS_MAGIC) return kOpsMagic;
' ''
plant 24 $P "FAIL-OPEN: a refused config is kept" 'inline bool config_keep(bool opsOk, int rc) { return opsOk && rc == 0; }' 'inline bool config_keep(bool opsOk, int rc) { (void)rc; return opsOk; }'
plant 25 $P "FAIL-OPEN: the factory mask is ignored" 'inline bool factory_allowed(bool opsOk, uint32_t mask, uint32_t bit) { return opsOk && (mask & bit) != 0u; }' 'inline bool factory_allowed(bool opsOk, uint32_t mask, uint32_t bit) { (void)mask; (void)bit; return opsOk; }'
plant 26 $P "the memory-map hook result is ignored" 'inline bool mm_result(bool opsOk, bool haveHook, int rc) { return opsOk && haveHook && rc == 0; }' 'inline bool mm_result(bool opsOk, bool haveHook, int rc) { (void)rc; return opsOk && haveHook; }'
plant 28 $S "the sys-memory factory ignores the bring-up kext's mask" '	return ok ? OSTypeAlloc(Navi48SysMemory) : nullptr;' '	return OSTypeAlloc(Navi48SysMemory);'
plant 29 $S "ops are fetched with waitForFunction=true" 'callPlatformFunction(fn, /*waitForFunction=*/false,' 'callPlatformFunction(fn, /*waitForFunction=*/true,'
plant 30 $S "an unacceptable ops table is kept" '	if (v != kOpsOk) { N48_LOG("ops: refused (verdict %u)", (unsigned)v); return nullptr; }' '	if (v != kOpsOk) { N48_LOG("ops: refused (verdict %u)", (unsigned)v); }'
# ---- event machine ----
plant 31 $P "the stamp base is set even when Fast2 init failed" '    if (!superInitOk) return p;
' ''
plant 32 $P "the stamp base is set without a stamp VA" '    if (!haveStampVA) return p;
' ''
plant 33 $S "ORDERING: Fast2::init runs AFTER the stamp base" '	const bool sup = IOAccelEventMachineFast2::init(accel, n, timeout);          // FIRST (MUST-FIX 2)' '	const bool sup = true;' '	n48_trace(N48_TR_EM_INIT, p.ok, sup);' '	IOAccelEventMachineFast2::init(accel, n, timeout); n48_trace(N48_TR_EM_INIT, p.ok, sup);'
plant 34 $S "the accelerator is not identity-checked in the event machine" '	Navi48Accelerator *na = OSDynamicCast(Navi48Accelerator, accel);' '	Navi48Accelerator *na = (Navi48Accelerator *)accel;'
# ---- thin ----
plant 35 $S "a config literal creeps into the aux kext" '// ---- state (one accelerator per boot)' '// the config stores 0x480000 here would be a decision that belongs in the bring-up kext
// ---- state (one accelerator per boot)'
plant 36 $S "the aux kext allocates the stamp page itself" '// ---- ops acquisition' '// IOBufferMemoryDescriptor::inTaskWithPhysicalMask here would be a decision that belongs in the bring-up kext
// ---- ops acquisition'
plant 37 $S "a leaf class introduces a virtual" 'class Navi48Task : public IOAccelTask {
	OSDeclareDefaultStructors(Navi48Task)
public:' 'class Navi48Task : public IOAccelTask {
	OSDeclareDefaultStructors(Navi48Task)
public:
	virtual void extra();'
plant 38 $S "the aux kext writes a register" '	uint8_t *cfg = (uint8_t *)this + 0xc88;' '	uint8_t *cfg = (uint8_t *)this + 0xc88; (void)"WREG32";'
plant 39 $S "the aux kext gains a PCI call" '	if (!o) return nullptr;
	// Static config check' '	if (!o) return nullptr;
	(void)"configWrite";
	// Static config check'
plant 40 $S "a log line per event" 'static inline void n48_trace(' 'static inline void n48_log_all() { N48_LOG("a"); N48_LOG("b"); N48_LOG("c"); N48_LOG("d"); N48_LOG("e"); N48_LOG("f"); }
static inline void n48_trace('
# ---- personality / identity ----
plant 41 tests/../Info.plist "the personality matches a PCI device" '<string>Navi48MetalNub</string>' '<string>IOPCIDevice</string>'
plant 42 Info.plist "an IOResources personality (runs at every boot)" '<key>IOProbeScore</key>' '<key>IOResourceMatch</key>
			<string>IOKit</string>
			<key>IOProbeScore</key>'
plant 43 Info.plist "the version is not bumped (one field)" '<key>CFBundleVersion</key>
	<string>0.0.3</string>' '<key>CFBundleVersion</key>
	<string>0.0.2</string>'
plant 44 src/kmod_info.c "kmod version differs from the plist" 'KMOD_EXPLICIT_DECL(com.navi48.accelprobe, "0.0.3", _start, _stop)' 'KMOD_EXPLICIT_DECL(com.navi48.accelprobe, "0.0.2", _start, _stop)'
plant 45 Info.plist "another bundle id (would need a new Allow approval)" '<string>com.navi48.accelprobe</string>
	<key>CFBundleInfoDictionaryVersion</key>' '<string>com.navi48.accelprobe2</string>
	<key>CFBundleInfoDictionaryVersion</key>'
plant 46 Info.plist "IOMatchCategory changes" '<key>IOMatchCategory</key>
			<string>IOAccelerator</string>' '<key>IOMatchCategory</key>
			<string>IOFramebuffer</string>'
plant 47 Info.plist "the kext is required at boot" '<key>OSBundleLibraries</key>' '<key>OSBundleRequired</key>
	<string>Local-Root</string>
	<key>OSBundleLibraries</key>'
plant 48 src/Navi48MetalOps.h "the ops header differs from the bring-up kext's copy" '#define N48_METAL_ABI         2u' '#define N48_METAL_ABI         2u
/* drift */'
plant 49 Makefile "make does not run the gates" 'python3 gates/gate_link.py $(EXEC) $(BUILD)/accel.o
	python3 gates/layout_gate_host.py $(EXEC)' 'true'
plant 50 Makefile "make does not run the host twin" '	python3 gates/layout_gate_host.py $(EXEC)' '	true'

plant 51 $S "populateAccelConfig blanks the name to NULL again (get_name would fault)" 'c[i] = (uint8_t)(np >> (8 * i));' 'c[i] = 0;'
plant 52 $S "a refused config does not tear the device down (start would go on)" '		if (ctx && gOps) { gOps->device_close(ctx); }
' ''
plant 53 $P "a NEWER ops abi is refused (the bring-up kext could not grow the table)" 'if (o->abi < N48_METAL_ABI_MIN) return kOpsAbi;' 'if (o->abi != N48_METAL_ABI_MIN) return kOpsAbi;'
plant 54 $S "the hook is used without its cap bit" '!gOps->vhook || !cap_has(gOps, N48_CAP_VHOOK)' '!gOps->vhook'
plant 55 $S "any non-zero hook return counts as handled" 'gOps->vhook(gCtx, cls, slot, self, a, n, r) == 1' 'gOps->vhook(gCtx, cls, slot, self, a, n, r) != 0'
plant 56 $P "the gate window ignores kmod_info" 'if (kaddr != 0 && ksize != 0 && anchor >= kaddr' 'if (false && anchor >= kaddr'
plant 57 $P "the family vtable length is not checked" '    if (fam[nslots] != 0) { r.v = kGateFamilyLonger;' '    if (false) { r.v = kGateFamilyLonger;'
plant 58 gates/gen_tramp.py "generated trampolines lose the kill switch" "ks = 'N48_AUX_ENTER((%s)%s);' % (ret, mode)" "ks = ''"
plant 59 gates/leaves.py "the shared user client has no base default (externalMethod would return 0)" "{266: 'base'}" "{266: 'zero'}"
plant 60 $S "the command queue subclass is always created" 'n48_fact(this, N48_FACT_CMDQUEUE, 348)' 'true'
plant 61 $S "newSharedUserClient has no family fallback" '(IOAccelSharedUserClient2 *)IOGraphicsAccelerator2::newSharedUserClient()' 'nullptr'
plant 62 $S "free() forgets the global device context" 'if (gCtx == ctx) gCtx = nullptr; ctx = nullptr; stampMD = nullptr; stampVA = nullptr; }   // idempotent' 'ctx = nullptr; stampMD = nullptr; stampVA = nullptr; }   // idempotent'

# ---- aux 0.0.3: the display pipe (A1-A6; the decisions A7 of the host test) ----
plant 63 $P "an OLD ops table (abi 1) is treated as display ON" 'return o && o->abi >= 2u && o->size >= N48_METAL_OPS_V2 &&' 'return o && o->size >= N48_METAL_OPS_V2 &&'
plant 64 $P "a 120-byte ops table is read past its end (the size is not checked)" 'return o && o->abi >= 2u && o->size >= N48_METAL_OPS_V2 &&' 'return o && o->abi >= 2u &&'
plant 65 $P "display ON without the flag the bring-up kext latched" '(o->disp_flags & N48_DISP_F_ON) != 0u && o->disp_hook != nullptr;' 'o->disp_hook != nullptr;'
plant 66 $P "display ON without a display hook (a NULL call)" '(o->disp_flags & N48_DISP_F_ON) != 0u && o->disp_hook != nullptr;' '(o->disp_flags & N48_DISP_F_ON) != 0u;'
plant 67 $P "the display-machine walk starts at the PCI device even when the argument is not the nub" 'return (dispOn && providerIsNub && pci) ? pci : provider;' 'return (dispOn && pci) ? pci : provider;'
plant 68 $P "the display-machine walk is redirected with display OFF" 'return (dispOn && providerIsNub && pci) ? pci : provider;' 'return (providerIsNub && pci) ? pci : provider;'
plant 69 $P "a NULL PCI device is substituted for the nub" 'return (dispOn && providerIsNub && pci) ? pci : provider;' 'return (dispOn && providerIsNub) ? pci : provider;'
plant 70 $P "newDisplayPipe picks our pipe although the allocation failed (would return NULL)" 'return (dispOn && allocated) ? kPipeOurs : kPipeFamily;' 'return dispOn ? kPipeOurs : kPipeFamily;'
plant 71 $P "newDisplayPipe picks our pipe with display OFF" 'return (dispOn && allocated) ? kPipeOurs : kPipeFamily;' 'return allocated ? kPipeOurs : kPipeFamily;'
plant 72 $S "newDisplayPipe has no family fallback (a failed allocation returns NULL)" '	IOAccelDisplayPipe *p = ours ? (IOAccelDisplayPipe *)mine : IOGraphicsAccelerator2::newDisplayPipe();' '	IOAccelDisplayPipe *p = (IOAccelDisplayPipe *)mine;'
plant 73 $S "newDisplayPipe has no kill switch" '	N48_AUX_ENTER(IOGraphicsAccelerator2::newDisplayPipe());
' ''
plant 74 $S "the display hook is asked with display OFF" '	if (!gCtx || !disp_enabled(gOps)) return false;
	return gOps->disp_hook(' '	if (!gCtx) return false;
	return gOps->disp_hook('
plant 75 $S "the display trampolines ask the generic vhook" 'return gOps->disp_hook(gCtx, cls, slot, self, a, n, r) == 1;' 'return gOps->vhook(gCtx, cls, slot, self, a, n, r) == 1;'
plant 76 $S "display-machine start has no kill switch" '	N48_AUX_ENTER(IOAccelDisplayMachine::start(provider));
' ''
plant 77 $S "display-machine start substitutes the PCI device for any provider" 'dm_walk_provider(on, n48_provider_is_nub((IOService *)(void *)provider), (void *)provider, pci)' 'dm_walk_provider(on, true, (void *)provider, pci)'
plant 78 $S "slot 277 has no kill switch" '	N48_AUX_ENTER((uint64_t)0);
	uint64_t a[6] = { (uint64_t)(uintptr_t)txn' '	uint64_t a[6] = { (uint64_t)(uintptr_t)txn'
plant 79 $S "slot 277 falls back to the wrong family slot" 'n48_ztvfam_IOAccelDisplayPipe[2 + 277]' 'n48_ztvfam_IOAccelDisplayPipe[2 + 276]'
plant 80 gates/leaves.py "the display trampolines are generated against the generic vhook" "{267: 'base', 278: 'base', 279: 'base'}, 'n48_disp_vhook')" "{267: 'base', 278: 'base', 279: 'base'})"
plant 81 gates/leaves.py "the expected gate total is not raised for the pipe" "EXPECT_SLOTS = 2370" "EXPECT_SLOTS = 2064"
plant 82 gates/leaves.py "slot 277 is generated instead of hand written (its declaration is a placeholder)" "HAND = {'Navi48EventMachine': [35], 'Navi48DisplayPipe': [277]}" "HAND = {'Navi48EventMachine': [35]}"

echo "plants: $total run, $escaped escaped/failed"
[ "$escaped" -eq 0 ]
