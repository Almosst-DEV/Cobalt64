#!/bin/zsh
# plant.sh - planted breaks for tests/host_test.cpp (Navi48Accel aux kext, 0.0.7). For each plant: copy the real sources into a scratch tree, apply the break (exact-once
# replacements; the script first proves each replaced text occurs exactly once), compile the real test against the scratch tree and demand that it FAILS. A plant the suite
# lets through is a hole: the script exits non-zero and says which. The CONTROL (no break) must pass.
#   tools/native/navi48accel/tests/plant.sh [first last]     (BRING_ROOT=<bring-up tree root> to compare the two Navi48MetalOps.h copies; default: the tree this script lives in)
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BRING="${BRING_ROOT:-$(cd "$HERE/../../.." && pwd)}"
SCR="${TMPDIR:-/tmp}/n48accel-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0
FIRST=${1:-0}; LAST=${2:-9999}

fresh() {
  rm -rf "$SCR"; mkdir -p "$SCR/tests"
  cp -R "$HERE/src" "$SCR/src"; cp -R "$HERE/gates" "$SCR/gates"; mkdir -p "$SCR/build/gen"; cp "$HERE/build/gen/n48_tramp.inc" "$SCR/build/gen/" 2>/dev/null
  cp "$HERE/Info.plist" "$HERE/Makefile" "$SCR/"
  cp "$HERE/tests/host_test.cpp" "$SCR/tests/"
  cp "$HERE/INSTALL.md" "$SCR/"; mkdir -p "$SCR/ioaccel"; cp "$HERE/../ioaccel-layout/vtables/IOFramebuffer.tsv" "$SCR/ioaccel/"     # aux 0.0.4: the INSTALL.md pins and the KC vtable listing the framebuffer tests read
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
plant 29 $S "ops are fetched with waitForFunction=true" 'callPlatformFunction(fn, /*waitForFunction=*/false, (void *)&o, nullptr, nullptr, nullptr);
	fn->release();
	if (rc != kIOReturnSuccess) { N48_LOG' 'callPlatformFunction(fn, /*waitForFunction=*/true, (void *)&o, nullptr, nullptr, nullptr);
	fn->release();
	if (rc != kIOReturnSuccess) { N48_LOG'
plant 119 $S "the display ops are fetched with waitForFunction=true" 'callPlatformFunction(fn, /*waitForFunction=*/false, (void *)&o, nullptr, nullptr, nullptr);
	fn->release();
	return rc == kIOReturnSuccess ? o : nullptr;' 'callPlatformFunction(fn, /*waitForFunction=*/true, (void *)&o, nullptr, nullptr, nullptr);
	fn->release();
	return rc == kIOReturnSuccess ? o : nullptr;'
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
	<string>0.0.7</string>' '<key>CFBundleVersion</key>
	<string>0.0.3</string>'
plant 44 src/kmod_info.c "kmod version differs from the plist" 'KMOD_EXPLICIT_DECL(com.navi48.accelprobe, "0.0.7", _start, _stop)' 'KMOD_EXPLICIT_DECL(com.navi48.accelprobe, "0.0.3", _start, _stop)'
plant 45 Info.plist "another bundle id (would need a new Allow approval)" '<string>com.navi48.accelprobe</string>
	<key>CFBundleInfoDictionaryVersion</key>' '<string>com.navi48.accelprobe2</string>
	<key>CFBundleInfoDictionaryVersion</key>'
plant 46 Info.plist "IOMatchCategory changes" '<key>IOMatchCategory</key>
			<string>IOAccelerator</string>' '<key>IOMatchCategory</key>
			<string>IOFramebuffer</string>'
plant 47 Info.plist "the kext is required at boot" '<key>OSBundleLibraries</key>' '<key>OSBundleRequired</key>
	<string>Local-Root</string>
	<key>OSBundleLibraries</key>'
plant 48 src/Navi48MetalOps.h "the ops header differs from the bring-up kext's copy" '#define N48_METAL_ABI         3u' '#define N48_METAL_ABI         3u
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
plant 81 gates/leaves.py "the expected gate total is not raised for the pipe and the framebuffer" "EXPECT_SLOTS = 2720" "EXPECT_SLOTS = 2064"
plant 82 gates/leaves.py "slot 277 is generated instead of hand written (its declaration is a placeholder)" "HAND = {'Navi48EventMachine': [35], 'Navi48DisplayPipe': [277]}" "HAND = {'Navi48EventMachine': [35]}"

# ---- aux 0.0.4 (M5): Navi48Framebuffer ----
plant 83 $S 'Navi48Framebuffer::start has no kill switch' 'bool Navi48Framebuffer::start(IOService *provider) {
	N48_AUX_ENTER(false);
' 'bool Navi48Framebuffer::start(IOService *provider) {
'
plant 84 $S 'the framebuffer starts with navi48-metal-ws present' 'fb_start_verdict(n48_aux_on(), metalWs, isNub, layoutOk, ov, rc, sv)' 'fb_start_verdict(n48_aux_on(), false, isNub, layoutOk, ov, rc, sv)'
plant 85 $S 'the framebuffer starts under any provider' 'fb_start_verdict(n48_aux_on(), metalWs, isNub, layoutOk, ov, rc, sv)' 'fb_start_verdict(n48_aux_on(), metalWs, true, layoutOk, ov, rc, sv)'
plant 86 $S 'the framebuffer ignores the ops verdict' 'fb_start_verdict(n48_aux_on(), metalWs, isNub, layoutOk, ov, rc, sv)' 'fb_start_verdict(n48_aux_on(), metalWs, isNub, layoutOk, (uint32_t)N48_DOV_OK, rc, sv)'
plant 87 $S 'the framebuffer does not validate the snapshot' 'const uint32_t sv = rc == 0 ? fb_snap_for_index(&s, pidx) : (uint32_t)N48_DSV_NULL;' 'const uint32_t sv = (uint32_t)N48_DSV_OK;'
plant 88 $S 'the framebuffer starts even when the verdict refuses' '	if (v != kFbStartOk) return false;
' '	if (false) return false;
'
plant 89 $S 'getApertureRange serves any aperture' 'fb_aperture(&snap, aperture == kIOFBSystemAperture, &phys, &len)' 'fb_aperture(&snap, true, &phys, &len)'
plant 90 $S 'isConsoleDevice is overridden (the monitor B would become the console)' '	bool start(IOService *provider) override;
	IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;' '	bool start(IOService *provider) override;
	bool isConsoleDevice() override { return true; }
	IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;'
plant 91 $S 'the framebuffer declares a new virtual' '	void initForPM();                 // RDNA4FB' '	virtual void initForPM();                 // RDNA4FB'
plant 92 $S 'initForPM lacks the system-sleep veto' '	for (auto &state : powerStates) state.capabilityFlags |= kIOPMPreventSystemSleep;
' ''
plant 93 $S 'setAttribute(power) is left to super' '	if (!fb_power_attr((uint32_t)attribute)) return IOFramebuffer::setAttribute(attribute, value);
' '	return IOFramebuffer::setAttribute(attribute, value);
'
plant 94 $P 'the cursor attribute reports a hardware cursor' 'if (attribute == kFbAttrCursor) { r.rc = kFbRcSuccess; r.hasValue = true; r.value = 0u; }' 'if (attribute == kFbAttrCursor) { r.rc = kFbRcSuccess; r.hasValue = true; r.value = 1u; }'
plant 95 $P 'the pixel masks are swapped (BGR)' 'o->masks[0] = 0x00FF0000u; o->masks[1] = 0x0000FF00u; o->masks[2] = 0x000000FFu;' 'o->masks[0] = 0x000000FFu; o->masks[1] = 0x0000FF00u; o->masks[2] = 0x00FF0000u;'
plant 96 $P 'the pixel format string is BGR' 'static const char fmt[] = "--------RRRRRRRRGGGGGGGGBBBBBBBB";' 'static const char fmt[] = "--------BBBBBBBBGGGGGGGGRRRRRRRR";'
plant 97 $P 'the row bytes are width x 3' 'o->bytesPerRow = s->pitchBytes;' 'o->bytesPerRow = s->w * 3u;'
plant 98 $P 'setDisplayMode accepts depth 1' 'return mode == kFbModeId && depth == 0u; }' 'return mode == kFbModeId && depth <= 1u; }'
plant 99 $P 'any aperture is served' 'if (!s || !phys || !len || !systemAperture || s->aperPhys == 0u || s->aperLen == 0u) return false;' 'if (!s || !phys || !len || s->aperPhys == 0u || s->aperLen == 0u) return false;'
plant 100 $P 'start verdict: the provider is judged before navi48-metal-ws' '    if (metalWsPresent) return kFbStartMetalWs;
    if (!providerIsNub) return kFbStartNotNub;' '    if (!providerIsNub) return kFbStartNotNub;
    if (metalWsPresent) return kFbStartMetalWs;'
plant 101 $P 'start verdict ignores navi48-metal-ws' '    if (metalWsPresent) return kFbStartMetalWs;
' ''
plant 102 $P 'start verdict ignores the kill switch' '    if (!auxOn) return kFbStartAuxOff;
' ''
plant 103 $P 'start verdict ignores a refused snapshot hook' 'if (snapRc != 0 || snapVerdict != N48_DSV_OK) return kFbStartSnapshot;' 'if (snapVerdict != N48_DSV_OK) return kFbStartSnapshot;'
plant 104 $P 'HLDDC sense succeeds without an EDID' 'r.rc = (s && s->edidLen != 0u) ? kFbRcSuccess : kFbRcUnsupported;' 'r.rc = kFbRcSuccess;'
plant 105 $P 'the DDC blocks are 0-based' '*src = s->edid + (uint64_t)(blockNumber - 1u) * kFbDdcBlock;' '*src = s->edid + (uint64_t)blockNumber * kFbDdcBlock;'
plant 106 $P 'the mode reports 60.00 Hz' 'o->refresh1616 = s->refresh1616;' 'o->refresh1616 = 60u << 16;'
plant 107 Info.plist 'the framebuffer personality has no IOPropertyMatch' '			<key>IOPropertyMatch</key>
			<dict>
				<key>Navi48DisplayIndex</key>
				<integer>1</integer>
			</dict>
' ''
plant 108 Info.plist 'the framebuffer personality matches a PCI device' '			<key>IOProviderClass</key>
			<string>Navi48DisplayNub</string>
			<key>IOPropertyMatch</key>
			<dict>
				<key>Navi48DisplayIndex</key>
				<integer>1</integer>' '			<key>IOProviderClass</key>
			<string>IOPCIDevice</string>
			<key>IOPropertyMatch</key>
			<dict>
				<key>Navi48DisplayIndex</key>
				<integer>1</integer>'
plant 168 Info.plist 'the monitor A personality matches a PCI device' '			<key>IOProviderClass</key>
			<string>Navi48DisplayNub</string>
			<key>IOPropertyMatch</key>
			<dict>
				<key>Navi48DisplayIndex</key>
				<integer>2</integer>' '			<key>IOProviderClass</key>
			<string>IOPCIDevice</string>
			<key>IOPropertyMatch</key>
			<dict>
				<key>Navi48DisplayIndex</key>
				<integer>2</integer>'
plant 111 INSTALL.md 'INSTALL.md recommends kmutil install --update-all' 'NEVER `kmutil install --update-all` (see step 4b).' 'Run `kmutil install --update-all` (see step 4b).'
plant 112 gates/leaves.py 'the framebuffer'\''s override list forgets getDDCBlock' 'FB_EXTRA = [184, 310, 311, 319, 322, 325, 326, 330, 331, 332, 344, 345]' 'FB_EXTRA = [184, 310, 311, 319, 322, 325, 326, 330, 331, 332, 344]'
plant 114 gates/leaves.py 'the framebuffer leaf is not gated' '    ('\''Navi48Framebuffer'\'',   '\''IOFramebuffer'\'',            FB_EXTRA),
' ''
plant 115 $S 'getDDCBlock serves any block type' 'blockType == kIODDCBlockTypeEDID' 'true'
plant 116 $S 'setDisplayMode accepts everything' '	return (depth >= 0 && fb_mode_ok((uint32_t)displayMode, (uint32_t)depth)) ? kIOReturnSuccess : kIOReturnUnsupported;      // only (1, 0)' '	return kIOReturnSuccess;'
plant 117 $S 'enableController does not register power management' '	initForPM();
	if (dops && dops->trace) dops->trace(N48_DTR_ENABLE' '	if (dops && dops->trace) dops->trace(N48_DTR_ENABLE'
plant 118 $S 'getStartupDisplayMode answers mode 0' '	if (displayMode) *displayMode = (IODisplayModeID)kFbModeId;      // the base getter returns 0xE00002C7' '	if (displayMode) *displayMode = 0;      // the base getter returns 0xE00002C7'

plant 120 $S "the framebuffer starts without running the layout gate (on an fb2 boot no accelerator probe ever runs it)" 'const bool layoutOk = !metalWs && isNub && n48_layout_gate();' 'const bool layoutOk = !metalWs && isNub;'
plant 121 $S "the framebuffer ignores the layout gate's verdict" 'fb_start_verdict(n48_aux_on(), metalWs, isNub, layoutOk, ov, rc, sv)' 'fb_start_verdict(n48_aux_on(), metalWs, isNub, true, ov, rc, sv)'
plant 122 $P "start verdict ignores the layout gate" '    if (!layoutOk) return kFbStartLayout;
' ''
plant 123 $P "start verdict judges the layout gate before the provider" '    if (!providerIsNub) return kFbStartNotNub;
    if (!layoutOk) return kFbStartLayout;' '    if (!layoutOk) return kFbStartLayout;
    if (!providerIsNub) return kFbStartNotNub;'

# ---- aux 0.0.5 (G6): the 'N48N' user client on the accelerator (slot 239) ----
L=gates/leaves.py
plant 130 $S "AN 'N48N' OPEN FALLS THROUGH TO THE FAMILY when the plan says unsupported (the family would hand out a generic IOAccelContext2)" '	if (plan != kNucNative) return kIOReturnUnsupported;' '	if (plan != kNucNative) return IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, handler);'
plant 131 $P "nuc_plan: an unusable 'N48N' (kill switch, gate, device, table) is the family's" '    if (!auxOn || !gatePass || !haveDevice || !native_open_usable(o)) return kNucUnsupported;' '    if (!auxOn || !gatePass || !haveDevice || !native_open_usable(o)) return kNucFamily;'
plant 132 $S "the kill switch lets an 'N48N' open through to the family" 'N48_AUX_ENTER(type == N48_METAL_UC_N48N ? kIOReturnUnsupported : IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, handler));' 'N48_AUX_ENTER(IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, handler));'
plant 133 $P "AN IOReturn IS READ AS A BOOL (nuc_result): any non-zero refusal counts as handled" 'return rc != kIoSuccess ? rc : (haveClient ? kIoSuccess : kIoInternalError);' 'return rc ? kIoSuccess : (haveClient ? kIoSuccess : kIoInternalError);'
plant 134 $S "AN IOReturn IS READ AS A BOOL (the aux returns 0 = failed / 1 = handled)" '	return (IOReturn)out;' '	return (IOReturn)(out != 0 ? 0 : 1);'
plant 135 $P "native_open_usable ignores the capability bit" '    return o && o->abi >= 3u && o->size >= N48_METAL_OPS_V3 && cap_has(o, N48_CAP_NATIVE_OPEN) && o->native_open != nullptr;' '    return o && o->abi >= 3u && o->size >= N48_METAL_OPS_V3 && o->native_open != nullptr;'
plant 136 $P "native_open_usable does not check the table size (reads past a 144-byte table)" '    return o && o->abi >= 3u && o->size >= N48_METAL_OPS_V3 && cap_has(o, N48_CAP_NATIVE_OPEN) && o->native_open != nullptr;' '    return o && o->abi >= 3u && cap_has(o, N48_CAP_NATIVE_OPEN) && o->native_open != nullptr;'
plant 137 $P "native_open_usable ignores the ABI" '    return o && o->abi >= 3u && o->size >= N48_METAL_OPS_V3 && cap_has(o, N48_CAP_NATIVE_OPEN) && o->native_open != nullptr;' '    return o && o->size >= N48_METAL_OPS_V3 && cap_has(o, N48_CAP_NATIVE_OPEN) && o->native_open != nullptr;'
plant 138 $S "the hook is called without a live device" 'nuc_plan(type, true, gGateState == 1, gOps, ctx != nullptr && ctx == gCtx)' 'nuc_plan(type, true, gGateState == 1, gOps, true)'
plant 139 $S "the hook is called although the layout gate did not pass" 'nuc_plan(type, true, gGateState == 1, gOps, ctx != nullptr && ctx == gCtx)' 'nuc_plan(type, true, true, gOps, ctx != nullptr && ctx == gCtx)'
plant 140 $S "the hook gets no accelerator identity" 'gOps->native_open(ctx, this, (void *)owningTask' 'gOps->native_open(ctx, nullptr, (void *)owningTask'
plant 141 $S "*handler is written on a refusal" '	if (out == kIoSuccess) *handler = uc;
	else if (uc) { uc->release(); uc = nullptr; }' '	*handler = uc;'
plant 142 $L "slot 239 is not in the expected override list" "[183, 184, 18, 322, 348, 329, 239])" "[183, 184, 18, 322, 348, 329])"
plant 143 $P "the type constant is not N48N" 'if (type != N48_METAL_UC_N48N) return kNucFamily;' 'if (type != 0x4E34384Fu) return kNucFamily;'
plant 144 Info.plist "the short version stays 0.0.4" '<key>CFBundleShortVersionString</key>
	<string>0.0.7</string>' '<key>CFBundleShortVersionString</key>
	<string>0.0.4</string>'
plant 145 INSTALL.md "the install path is the old one" '/Library/Extensions/Navi48Accel-0.0.5.kext && sudo chown' '/Library/Extensions/Navi48Accel-0.0.4.kext && sudo chown'
plant 146 INSTALL.md "the 0.0.5 section suggests kmutil install --update-all" '    Kill switch at any time: boot-arg `navi48-aux=0` (an '"'"'N48N'"'"' open on the accelerator then returns kIOReturnUnsupported).' '    sudo kmutil install --update-all'
plant 147 src/Navi48MetalOps.h "the ops header drifts from the bring-up copy (a changed offset comment)" '/* 144 */
};' '/* 148 */
};'
plant 148 $S "newUserClient forgets to set the trace (the open leaves no line)" '	n48_trace(N48_TR_NATIVE_OPEN, (uint64_t)(uint32_t)out, (uint64_t)(uintptr_t)uc);
' ''
plant 149 $S "a NULL handler is dereferenced after the plan said native" '	if (!handler) return kIOReturnBadArgument;
	IOUserClient *uc = nullptr;' '	IOUserClient *uc = nullptr;'

# ---- aux 0.0.6 (Run B): the monitor A as display index 2 (a second personality of the same class; ops ABI 2; the per-index geometry)
plant 150 $S 'the framebuffer does not read its nub index (the snapshot is judged for any index)' 'const uint32_t sv = rc == 0 ? fb_snap_for_index(&s, pidx) : (uint32_t)N48_DSV_NULL;' 'const uint32_t sv = rc == 0 ? n48disp_snap_valid(&s) : (uint32_t)N48_DSV_NULL;'
plant 151 $S 'the ops check ignores the index (an ABI-1 bring-up kext could serve the monitor A)' 'layoutOk ? n48disp_ops_check_for(o, pidx)' 'layoutOk ? n48disp_ops_check(o)'
plant 152 src/n48accel_pure.h 'the snapshot may carry another index than its nub' 'return s->index == providerIndex ? (uint32_t)N48_DSV_OK : (uint32_t)N48_DSV_INDEX;' 'return (uint32_t)N48_DSV_OK;'
plant 153 src/n48accel_pure.h 'any provider index is accepted' 'value == N48_DISP_INDEX || value == N48_DISPA_INDEX' 'value != 0'
plant 154 src/Navi48DisplayOps.h 'the shared validator accepts any aperture length' 's->pixHz != g.pixHz || s->aperLen != g.aperLen) return N48_DSV_GEOMETRY;' 's->pixHz != g.pixHz) return N48_DSV_GEOMETRY;'
plant 155 src/Navi48DisplayOps.h 'the monitor A geometry has another refresh (not exactly 60 Hz)' '#define N48_DISPA_REFRESH1616 0x3C0000u' '#define N48_DISPA_REFRESH1616 0x3BFFFFu'
plant 156 src/Navi48DisplayOps.h 'the monitor A geometry keeps the monitor B DDC line' 'g->otg = N48_DISPA_OTG; g->ddcLine = N48_DISPA_DDC_LINE;' 'g->otg = N48_DISPA_OTG; g->ddcLine = N48_DISP_DDC_LINE;'
plant 157 src/Navi48DisplayOps.h 'an ABI-1 table is accepted for index 2' 'return o->abi >= N48_DISP_ABI_MULTI ? N48_DOV_OK : N48_DOV_ABI;' 'return N48_DOV_OK;'
plant 158 src/Navi48DisplayOps.h 'the monitor B row is changed (the pitch)' 'g->w = N48_DISP_W; g->h = N48_DISP_H; g->pitchBytes = N48_DISP_PITCH;' 'g->w = N48_DISP_W; g->h = N48_DISP_H; g->pitchBytes = N48_DISPA_PITCH;'
plant 159 Info.plist 'the monitor A personality matches index 1 (two framebuffers on one nub)' '<key>Navi48DisplayIndex</key>
				<integer>2</integer>' '<key>Navi48DisplayIndex</key>
				<integer>1</integer>'
plant 160 Info.plist 'the monitor A personality is missing' '<key>Navi48Framebuffer2</key>' '<key>Navi48FramebufferX</key>'
plant 161 Info.plist 'the monitor A personality is another class' '<key>Navi48Framebuffer2</key>
		<dict>
			<key>CFBundleIdentifier</key>
			<string>com.navi48.accelprobe</string>
			<key>IOClass</key>
			<string>Navi48Framebuffer</string>' '<key>Navi48Framebuffer2</key>
		<dict>
			<key>CFBundleIdentifier</key>
			<string>com.navi48.accelprobe</string>
			<key>IOClass</key>
			<string>Navi48Accelerator</string>'
plant 162 INSTALL.md 'the 0.0.6 install path is the old one' '/Library/Extensions/Navi48Accel-0.0.6.kext && sudo chown' '/Library/Extensions/Navi48Accel-0.0.5.kext && sudo chown'
plant 163 INSTALL.md 'the 0.0.6 section suggests kmutil install --update-all' '    Kill switch at any time: boot-arg `navi48-aux=0` (both framebuffers refuse to start). The framebuffers also refuse to start with boot-arg navi48-metal-ws present.' '    sudo kmutil install --update-all'
plant 164 $S 'the framebuffer class hard-wires the monitor B mode' 'info->nominalWidth = m.w;' 'info->nominalWidth = 2560;'
plant 165 $S 'the framebuffer class hard-wires the monitor B aperture' 'IODeviceMemory::withRange((IOPhysicalAddress)phys, (IOPhysicalLength)len);      // a fresh instance' 'IODeviceMemory::withRange((IOPhysicalAddress)phys, (IOPhysicalLength)14745600ull);      // a fresh instance'
plant 166 src/n48accel_pure.h 'the pixel format hard-wires the monitor B pitch' 'o->bytesPerRow = s->pitchBytes;' 'o->bytesPerRow = 10240u;'
plant 167 src/Navi48DisplayOps.h 'the aux copy of the ops header differs from the bring-up one' '#define N48_DISPA_PITCH       7680u' '#define N48_DISPA_PITCH       7681u'

# ---- aux 0.0.7 (M6 Stage 1a, R1): the navi48-m6 latch lifts the metal-ws refusal and only that
plant 168 src/n48accel_pure.h 'the metal-ws refusal ignores the M6 latch' 'constexpr bool fb_metal_ws_blocks(bool metalWsPresent, bool m6On) { return metalWsPresent && !m6On; }' 'constexpr bool fb_metal_ws_blocks(bool metalWsPresent, bool m6On) { (void)m6On; return metalWsPresent; }'
plant 169 src/n48accel_pure.h 'the M6 latch is on for any value' 'constexpr bool m6_on(bool present, uint32_t value) { return present && value == 1u; }' 'constexpr bool m6_on(bool present, uint32_t value) { (void)value; return present; }'
plant 170 $S 'start judges metal-ws without the M6 latch' 'const bool metalWs = fb_metal_ws_blocks(metalWsArg, m6On);' 'const bool metalWs = metalWsArg; (void)m6On;'
plant 171 $S 'start never refuses on metal-ws (latch or not)' 'const bool metalWs = fb_metal_ws_blocks(metalWsArg, m6On);' 'const bool metalWs = false; (void)metalWsArg; (void)m6On;'
plant 172 $S 'the M6 latch is read under a wrong name' 'PE_parse_boot_argn("navi48-m6", &m6v, sizeof(m6v))' 'PE_parse_boot_argn("navi48-m6x", &m6v, sizeof(m6v))'
plant 173 src/n48accel_pure.h 'the metal-ws refusal is inverted under the latch' 'return metalWsPresent && !m6On; }' 'return metalWsPresent && m6On; }'
plant 174 INSTALL.md 'the 0.0.7 install path is the old one' '/Library/Extensions/Navi48Accel-0.0.7.kext && sudo chown' '/Library/Extensions/Navi48Accel-0.0.6.kext && sudo chown'
plant 175 src/kmod_info.c 'kmod_info still says 0.0.6' '"0.0.7"' '"0.0.6"'
plant 176 $S 'the M6 latch is read twice in start' 'const bool metalWs = fb_metal_ws_blocks(metalWsArg, m6On);' 'const bool metalWs = fb_metal_ws_blocks(metalWsArg, m6On); uint32_t m6w = 0; (void)PE_parse_boot_argn("navi48-m6", &m6w, sizeof(m6w));'
plant 177 INSTALL.md 'the 0.0.7 section suggests kmutil install --update-all' '    Kill switch at any time: boot-arg `navi48-aux=0` (both framebuffers refuse to start). With navi48-m6=1 they START' '    sudo kmutil install --update-all
    Kill switch at any time: boot-arg `navi48-aux=0` (both framebuffers refuse to start). With navi48-m6=1 they START'

echo "plants: $total run, $escaped escaped/failed"
[ "$escaped" -eq 0 ]
