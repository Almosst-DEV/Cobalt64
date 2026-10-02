#!/bin/zsh
# native_ws_open_plant.sh - planted breaks for tests/native_ws_open_test.cpp and tests/native_hostimport_test.cpp (build 0.0.612, milestone #11 step 11c).
# For each plant: copy the real sources into a scratch tree, apply the break (exact-once replacements; the script first proves each replaced text occurs exactly once),
# compile the named real test against the scratch tree and demand that it FAILS (a compile error, a failing check, a crash or a hang cut off by the alarm all count).
# A plant the suite lets through is a hole: the script exits non-zero and says which. The CONTROLS (no break) must pass.
#   run from anywhere:  src/navi48-bringup/tests/native_ws_open_plant.sh [first-plant-id last-plant-id]
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
K=src/navi48-bringup
SR=$K/src
SCR="${TMPDIR:-/tmp}/nwsopen-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0
FIRST=${1:-0}; LAST=${2:-9999}

fresh() {
  rm -rf "$SCR"; mkdir -p "$SCR/$K/tests" "$SCR/tools"
  cp -R "$ROOT/$SR" "$SCR/$SR"
  cp "$ROOT/$K/Makefile" "$ROOT/$K/Info.plist" "$SCR/$K/"
  cp "$ROOT/$K/tests/native_ws_open_test.cpp" "$ROOT/$K/tests/native_hostimport_test.cpp" "$SCR/$K/tests/"
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
  local t
  for t in native_ws_open_test native_hostimport_test; do
    fresh; total=$((total+1))
    local msg; msg=$(build_run $t); local brc=$?
    if [ $brc -eq 0 ]; then echo "CONTROL $t (no break): $msg"; else echo "CONTROL $t (no break): *** FAILED: $msg ***"; escaped=$((escaped+1)); fi
  done
}

OP=$SR/amd/native_open_policy_pure.h
HP=$SR/amd/native_hostimport_pure.h
MP=$SR/amd/native_metal_pure.h
E=$SR/amd/native_s1c.cpp
C=$SR/Navi48NativeClient.cpp
N=$SR/Navi48MetalNub.cpp
W=native_ws_open_test
H=native_hostimport_test
control
# ---- W2: the open policy (fail-open directions first) ----
plant 1 $W $OP "FAIL-OPEN: uid 88 is admitted without the boot-arg" '    if (!wsArg) return OpenDecision{ false, kReasonWsArgOff };' '    (void)wsArg;'
plant 2 $W $OP "FAIL-OPEN: uid 88 is admitted while a client is open" '    if (alreadyOpen) return OpenDecision{ false, kReasonWsAlreadyOpen };' '    (void)alreadyOpen;'
plant 3 $W $OP "FAIL-OPEN: any uid is admitted with the boot-arg" '    if (uid != kWindowServerUid) return OpenDecision{ false, kReasonNotPrivileged };' '    (void)uid;'
plant 4 $W $OP "the wrong uid (89)" 'constexpr uint32_t kWindowServerUid = 88u;' 'constexpr uint32_t kWindowServerUid = 89u;'
plant 5 $W $OP "root needs the boot-arg (OFF identity broken)" '    if (admin) return OpenDecision{ true, kReasonAdmin };' '    if (admin && wsArg) return OpenDecision{ true, kReasonAdmin };'
plant 6 $W $OP "root is refused when a client is open (a changed return code)" '    if (admin) return OpenDecision{ true, kReasonAdmin };' '    if (admin && !alreadyOpen) return OpenDecision{ true, kReasonAdmin };'
plant 7 $W $OP "any non-zero boot-arg value latches ON" 'return (present && value == 1u) ? kLatchOn : kLatchOff;' 'return (present && value != 0u) ? kLatchOn : kLatchOff;'
plant 8 $W $OP "an absent boot-arg latches ON" 'return (present && value == 1u) ? kLatchOn : kLatchOff;' 'return (value == 1u || !present) ? kLatchOn : kLatchOff;'
plant 9 $W $C "the client ignores the latched boot-arg" 'n48native::policy::latch_is_on(gMetalWsLatch)' 'true'
plant 10 $W $C "the client forgets to log a refusal" '		NCLOG("open refused: uid %u, reason: %s", uid, n48native::policy::open_reason_text(d.reason));
' ''
plant 11 $W $C "the client forgets to log an admit" '	NCLOG("open admitted: uid %u, reason: %s", uid, n48native::policy::open_reason_text(d.reason));
' ''
plant 12 $W $C "the boot-arg is re-read on every open" '	if (gMetalWsLatch == n48native::policy::kLatchUnset) latchBootArgs();' '	latchBootArgs();
	{ uint32_t again = 0; (void)PE_parse_boot_argn("navi48-metal-ws", &again, sizeof(again)); }'
plant 13 $W $C "the uid is a constant" 'kauth_cred_getuid(kauth_cred_get())' '88'
plant 14 $W $C "privileged is set before the decision" '	if (!d.admit) {
		NCLOG' '	privileged = true;
	if (!d.admit) {
		NCLOG'
plant 15 $W $C "the class is not IOAccel-prefixed" 'OSDefineMetaClassAndStructors(IOAccelNavi48NativeClient, IOUserClient)' 'OSDefineMetaClassAndStructors(Navi48NativeClient, IOUserClient)'
plant 16 $W $SR/Navi48Bringup.cpp "newUserClient creates the old class" 'return IOAccelNavi48NativeClient::create(this, owningTask' 'return Navi48NativeClient::create(this, owningTask'
plant 17 $W $SR/Navi48Bringup.cpp "the boot-arg is never latched at start" '	IOAccelNavi48NativeClient::latchBootArgs();   // 0.0.612: boot-arg navi48-metal-ws is read ONCE here' '	// (no latch)   // 0.0.612: boot-arg navi48-metal-ws is read ONCE here'
# ---- W3: the Ready property ----
plant 20 $W $MP "Ready is 1 after a latch (publish ignores the sticky state)" 'return (stickyNo || hungNow) ? kReadyNo : kReadyYes;' 'return hungNow ? kReadyNo : kReadyYes;'
plant 21 $W $MP "the sticky state clears" 'return stickyNo || hangEvent;' 'return hangEvent;'
plant 22 $W $N "hungLatched writes 1" 'nub->setProperty("Navi48,Ready", (unsigned long long)n48metal::kReadyNo, 32);' 'nub->setProperty("Navi48,Ready", (unsigned long long)n48metal::kReadyYes, 32);'
plant 23 $W $N "hungLatched forgets the sticky word" '	__atomic_store_n(&gReadyNo, n48metal::ready_sticky_after(__atomic_load_n(&gReadyNo, __ATOMIC_ACQUIRE) != 0u, true) ? 1u : 0u, __ATOMIC_RELEASE);
' ''
plant 24 $W $N "publish always writes Ready = 1" '	nub->setProperty("Navi48,Ready", ready);' '	nub->setProperty("Navi48,Ready", abi);'
plant 25 $W $E "hang_poll latches but does not announce" '    const bool det = hang_detect(gHang, re, em, now);
    IOLockUnlock(gHangLock);
    if (det) hang_announce(em, re);' '    const bool det = hang_detect(gHang, re, em, now);
    IOLockUnlock(gHangLock);
    if (det) hang_log(em, re);'
plant 26 $W $E "hang_from_wait latches but does not announce" '    const bool det = hang_latch_wait(gHang);
    IOLockUnlock(gHangLock);
    if (det) hang_announce(em, re);' '    const bool det = hang_latch_wait(gHang);
    IOLockUnlock(gHangLock);
    if (det) hang_log(em, re);'
plant 27 $W $E "hang_announce forgets the property" '    hang_log(emitted, retired);
    Navi48MetalNub::hungLatched();' '    hang_log(emitted, retired);'
plant 28 $W $E "the announce runs BEFORE the latch (unconditionally)" '    const bool det = hang_detect(gHang, re, em, now);
    IOLockUnlock(gHangLock);
    if (det) hang_announce(em, re);' '    hang_announce(em, re);
    const bool det = hang_detect(gHang, re, em, now);
    IOLockUnlock(gHangLock);'
# ---- W4: BoImportHost ----
plant 40 $H $HP "the per-BO cap is 128 MiB" 'constexpr uint64_t kImportMaxBo   = 64ull << 20;' 'constexpr uint64_t kImportMaxBo   = 128ull << 20;'
plant 41 $H $HP "the client cap is 512 MiB (the GTT cap)" 'constexpr uint64_t kImportCap     = 2048ull << 20;' 'constexpr uint64_t kImportCap     = 512ull << 20;'
plant 42 $H $HP "the host va alignment is not checked" '    if (((hostVa | size) & (kPage - 1ull)) != 0ull) return ImportChk{ kBadArg, 0, 0 };' '    if ((size & (kPage - 1ull)) != 0ull) return ImportChk{ kBadArg, 0, 0 };'
plant 43 $H $HP "size 0 is accepted" '    if (size == 0ull || hostVa == 0ull) return ImportChk{ kBadArg, 0, 0 };' '    if (hostVa == 0ull) return ImportChk{ kBadArg, 0, 0 };'
plant 44 $H $HP "the cap check is off by one page" '    if (would_exceed(importedNow, size, kImportCap)) return ImportChk{ kNoMemory, 0, 0 };' '    if (would_exceed(importedNow, size + kPage, kImportCap)) return ImportChk{ kNoMemory, 0, 0 };'
plant 45 $H $HP "the cap is not enforced" '    if (would_exceed(importedNow, size, kImportCap)) return ImportChk{ kNoMemory, 0, 0 };' '    (void)importedNow;'
plant 46 $H $HP "a free does not return the room" 'constexpr uint64_t import_after_free(uint64_t importedNow, uint64_t size) { return importedNow >= size ? importedNow - size : 0ull; }' 'constexpr uint64_t import_after_free(uint64_t importedNow, uint64_t size) { (void)size; return importedNow; }'
plant 47 $H $HP "an over-free wraps" 'return importedNow >= size ? importedNow - size : 0ull; }' 'return importedNow - size; }'
plant 48 $H $HP "kernel-half host addresses are accepted" '    if (hostVa >= kImportUserMax || size > kImportUserMax - hostVa) return ImportChk{ kBadArg, 0, 0 };   // no wrap, user half only' '    (void)kImportUserMax;'
plant 49 $H $HP "flags are accepted without a GPU VA" '        if (flags != 0ull) return ImportChk{ kBadArg, 0, 0 };                        // flags mean nothing without a VA to map at' '        (void)flags;'
plant 50 $H $HP "the page collector accepts an unaligned physical address" '        if (pa == 0ull || len < kPage || (pa & (kPage - 1ull)) != 0ull) return kNoMemory;' '        if (pa == 0ull || len < kPage) return kNoMemory;'
plant 51 $H $HP "the page collector accepts a short run" '        if (pa == 0ull || len < kPage || (pa & (kPage - 1ull)) != 0ull) return kNoMemory;' '        if (pa == 0ull || (pa & (kPage - 1ull)) != 0ull) return kNoMemory;'
plant 52 $H $HP "the scattered mapper maps consecutive pages (the pt_map shape)" 'm.wr(ptb, idx_ptb(v) + (uint32_t)k, pte_encode(pagePa[p + k], leafFlagsIn));' 'm.wr(ptb, idx_ptb(v) + (uint32_t)k, pte_encode(pagePa[0] + (p + k) * kPage, leafFlagsIn));'
plant 53 $H $HP "the scattered mapper does not validate first (a partial mapping on a bad page)" '    for (uint64_t i = 0; i < pages; i++) if (pte_encode(pagePa[i], leafFlagsIn) == 0ull || (pagePa[i] & (kPage - 1ull)) != 0ull) return kBadArg;' '    (void)0;'
plant 54 $H $HP "the HUNG leak releases the pages" 'constexpr bool host_may_release(uint32_t mode) { return mode == kHostRelNormal || mode == kHostRelClosing; }' 'constexpr bool host_may_release(uint32_t mode) { return mode <= kHostRelClosing; }'
plant 55 $H $E "BoFree leaks the import (never completes it)" '        else if (b.kind == kBoHost && host_may_release(kHostRelNormal) && memOk) host_release(b);' '        else if (b.kind == kBoHost && !host_may_release(kHostRelNormal) && memOk) host_release(b);'
plant 56 $H $E "close does not free the imports" '        else if (b.kind == kBoHost && host_may_release(kHostRelClosing)) host_release(b);' '        else if (b.kind == kBoHost && !host_may_release(kHostRelClosing)) host_release(b);'
plant 57 $H $E "host_release releases before it completes" '    if (b.hmd != nullptr) { b.hmd->complete(); b.hmd->release(); b.hmd = nullptr; }' '    if (b.hmd != nullptr) { b.hmd->release(); b.hmd->complete(); b.hmd = nullptr; }'
plant 58 $H $E "the leak branch completes the pages" '        for (uint32_t i = 0; i < N48N_FENCE_SLOTS; i++) if (s->fslot[i].used && s->fslot[i].handle == h) s->fslot[i].used = 0;
    }' '        for (uint32_t i = 0; i < N48N_FENCE_SLOTS; i++) if (s->fslot[i].used && s->fslot[i].handle == h) s->fslot[i].used = 0;
        if (b.hmd) b.hmd->complete();
    }'
plant 59 $H $E "a host BO can be a fence target" '    if (b.kind == kBoHost) return kNotPermitted;   // 0.0.612: an imported range of the caller'"'"'s memory is never a kernel-written fence target
' ''
plant 60 $H $E "a host BO can be CPU-mapped (bogus VRAM range)" '        if (b.kind == kBoHi || b.kind == kBoHost) rc = kIOReturnNotPermitted;' '        if (b.kind == kBoHi) rc = kIOReturnNotPermitted;'
plant 61 $H $E "a host BO can be a scanout slot" '        if (b.kind == kBoHost) { rc = kIOReturnNotPermitted; break; }   // 0.0.612: system pages of the caller are never a scanout slot (HUBP flips VRAM only)
' ''
plant 62 $H $E "the authoritative cap check under the lock is dropped" 'import_check(hostVa, size, flags, gpuVa, s->importedBytes)' 'import_check(hostVa, size, flags, gpuVa, 0)'
plant 63 $H $E "the import is not counted" '        s->importedBytes += size;
' ''
plant 64 $H $E "a free is not counted" '    else if (b.kind == kBoHost) s->importedBytes = import_after_free(s->importedBytes, b.size);' '    else if (b.kind == kBoHost) { }'
plant 65 $H $E "the import maps without SYSTEM|SNOOPED" 'leaf_flags((uint32_t)flags, false, true)' 'leaf_flags((uint32_t)flags, false, false)'
plant 66 $H $E "GemVa maps a host BO as VRAM (not SYSTEM)" '            const bool sys = b.kind == kBoGtt || host;' '            const bool sys = b.kind == kBoGtt;'
plant 67 $H $E "GemVa maps a host BO with the contiguous mapper" 'host ? pt_map_pages(s->rootPa, q.stripped, in->map_size / kPage, b.hpages + in->offset_in_bo / kPage, leaf_flags(in->flags, b.uc, sys), mem)
                                    : pt_map(' 'false ? pt_map_pages(s->rootPa, q.stripped, in->map_size / kPage, b.hpages + in->offset_in_bo / kPage, leaf_flags(in->flags, b.uc, sys), mem)
                                    : pt_map('
plant 68 $H $C "selector 21 checks the wrong shape" 'if (!shape(4, 4, 0, 0)) return kIOReturnBadArgument;
		// 0.0.612 (review item B)' 'if (!shape(3, 4, 0, 0)) return kIOReturnBadArgument;
		// 0.0.612 (review item B)'
plant 69 $H $SR/Navi48NativeABI.h "the selector number moves" 'N48N_SEL_BO_IMPORT_HOST = 21,' 'N48N_SEL_BO_IMPORT_HOST = 22,'
plant 70 $H $E "the descriptor is the kernel's, not the caller's" 'kIODirectionInOut, task)' 'kIODirectionInOut, kernel_task)'
plant 71 $H $E "prepare() moves under the client lock" '    IOLockLock(gCliLock);
    do {
        if (!sess_hello()) { rc = kIOReturnNotReady; break; }   // closed while we waited for the lock
        if (!session_unchanged(' '    IOLockLock(gCliLock);
    (void)md->prepare();
    do {
        if (!sess_hello()) { rc = kIOReturnNotReady; break; }   // closed while we waited for the lock
        if (!session_unchanged('
plant 72 $H $E "a failed import leaks the descriptor" '        md->complete(); md->release();                            // never mapped: safe even when HUNG' '        (void)md;                                                  // never mapped: safe even when HUNG'
plant 73 $H $E "the import has a big local page array" '    uint64_t *pages = static_cast<uint64_t *>(IOMalloc((vm_size_t)(pre.pages * sizeof(uint64_t))));' '    uint64_t pagesLocal[16384]; uint64_t *pages = pagesLocal; (void)IOMalloc;'
plant 74 $H $E "the import writes a register" '        const ImportChk c = import_check(hostVa, size, flags, gpuVa, s->importedBytes);' '        WREG32(*gCtx->dev, 0, 0);
        const ImportChk c = import_check(hostVa, size, flags, gpuVa, s->importedBytes);'
plant 75 $H $E "close releases before the park" '    if (!leak) {
        // No client owns a tree from here: park CONTEXT8 on the kernel'"'"'s zeroed page, then free everything.
        if (program_root(gPark.pa) != kOk) leak = true;
    }
    if (!leak) {
        for (uint32_t h = 1; h < N48N_MAX_BOS; h++) if (s->boUsed[h]) bo_release(h, kRelClosing);' '    if (!leak) {
        for (uint32_t h = 1; h < N48N_MAX_BOS; h++) if (s->boUsed[h]) bo_release(h, kRelClosing);
        if (program_root(gPark.pa) != kOk) leak = true;
    }
    if (!leak) {'

# ---- review round on 0.0.612: A device pages, B owning task, C session change, D flush failure, E selector reach ----
BR=$SR/Navi48Bringup.cpp
HK=$SR/apple/AppleHardwareHook.cpp
plant 80 $H $HP "A: the BAR test is gone" '        if (pa < end && pe > bars[i].base) return kPageInBar;' '        (void)end;'
plant 81 $H $HP "A: the last page of a BAR is let through (end inclusive)" 'if (pa < end && pe > bars[i].base) return kPageInBar;' 'if (pa < end - kPage && pe > bars[i].base) return kPageInBar;'
plant 82 $H $HP "A: the BAR size is clamped to the 256 MiB mapped window" 'const uint64_t be = bars[i].base + bars[i].size;' 'const uint64_t be = bars[i].base + (bars[i].size < (256ull << 20) ? bars[i].size : (256ull << 20));'
plant 83 $H $HP "A: nothing latched fails OPEN" '    if (bars == nullptr || nbars == 0u) return kPageNoBars;' '    if (bars == nullptr) return kPageNoBars;'
plant 84 $H $HP "A: the page AT the top of DRAM is allowed" 'if (dramTop != 0ull && pe > dramTop) return kPageAboveDram;' 'if (dramTop != 0ull && pa > dramTop) return kPageAboveDram;'
plant 85 $H $HP "A: an unknown top (0) refuses every page" 'if (dramTop != 0ull && pe > dramTop) return kPageAboveDram;' 'if (pe > dramTop) return kPageAboveDram;'
plant 86 $H $HP "A: the list check skips the last page" '    for (uint64_t i = 0; i < n; i++) if (!import_page_allowed(pagePa[i], bars, nbars, dramTop)) return i;' '    for (uint64_t i = 0; i + 1 < n; i++) if (!import_page_allowed(pagePa[i], bars, nbars, dramTop)) return i;'
plant 87 $H $HP "A: a wrapping page address is accepted" '    if (pe < pa) return kPageInBar;                          // wraps the address space: never DRAM' '    (void)0;'
plant 88 $H $E "A: the engine never applies the device-page refusal" '        if (bad < pre.pages) {' '        if (false) {'
plant 89 $H $E "A: the engine ignores the DRAM top" 'const uint64_t dramTop = ::n48::hw_hook_ramtop_derived_or_zero();' 'const uint64_t dramTop = 0;'
plant 90 $H $E "A: an unlatched BAR list is passed as the latched (empty) array, failing open" 'import_first_refused(pages, pre.pages, barsOk ? gBars : nullptr, barsOk ? gNBars : 0u, dramTop)' 'import_first_refused(pages, pre.pages, gBars, gNBars, dramTop)'
plant 91 $H $BR "A: the BARs are never latched at start" 'latchPciBars(pciDevice);   // 0.0.612 (review item A): every BAR' '(void)0;   // 0.0.612 (review item A): every BAR'
plant 92 $H $BR "A: the latch clamps every BAR to the 256 MiB window" 'const uint64_t len = m->getLength();' 'const uint64_t len = m->getLength() > (256ull << 20) ? (256ull << 20) : m->getLength();'
plant 93 $H $BR "A: the expansion ROM is not read" 'kIOPCIConfigBaseAddress4, kIOPCIConfigBaseAddress5, kIOPCIConfigExpansionROMBase };' 'kIOPCIConfigBaseAddress4, kIOPCIConfigBaseAddress5 };'
plant 94 $H $E "A: the BAR list can be latched again (a later writer wins)" '    if (!OSCompareAndSwap(0, 2, &gBarsState)) return;          // first writer wins; 2 = being written' '    (void)gBarsState;'
plant 95 $H $HK "A: the DRAM-top accessor trusts a map that failed to parse" 'gRamTopRes.reason == N48_RT_OK) ? gRamTopDerived : 0ull;' 'true) ? gRamTopDerived : 0ull;'
plant 96 $H $BR "A: the BAR-0 upper-half register is not read (a 64-bit BAR pair)" 'static const UInt8 kRegs[] = { kIOPCIConfigBaseAddress0, kIOPCIConfigBaseAddress1, kIOPCIConfigBaseAddress2,' 'static const UInt8 kRegs[] = { kIOPCIConfigBaseAddress0, kIOPCIConfigBaseAddress2,'
plant 100 $W $C "B: selector 21 does not check the calling task" 'if (!n48native::policy::import_caller_ok(current_task(), task)) {' 'if (false) {'
plant 101 $W $OP "B: a null owner matches a null caller" 'return owner != nullptr && cur == owner;' 'return cur == owner;'
plant 102 $W $OP "B: any caller passes when an owner exists" 'return owner != nullptr && cur == owner;' 'return owner != nullptr;'
plant 103 $W $C "B: the refusal is logged every time (not once)" 'OSCompareAndSwap(0, 1, &gImportCallerLogged)' 'true'
plant 104 $W $C "B: a wrong caller gets BadArgument (not NotPermitted)" '			return kIOReturnNotPermitted;
		}
		return amdgpu::n1c_bo_import_host' '			return kIOReturnBadArgument;
		}
		return amdgpu::n1c_bo_import_host'
plant 110 $H $E "C: the session sequence is not re-checked under the lock" 'if (!session_unchanged(seq0, __atomic_load_n(&gSessSeq, __ATOMIC_SEQ_CST))) { rc = kIOReturnNotReady; break; }' '(void)seq0;'
plant 111 $H $E "C: the sequence is read under the lock, not at entry" 'const uint32_t seq0 = __atomic_load_n(&gSessSeq, __ATOMIC_SEQ_CST);   // review item C: the session this import belongs to, read BEFORE anything is wired' 'uint32_t seq0 = 0;' '    IOLockLock(gCliLock);
    do {
        if (!sess_hello()) { rc = kIOReturnNotReady; break; }   // closed while we waited for the lock
        if (!session_unchanged(' '    IOLockLock(gCliLock);
    seq0 = __atomic_load_n(&gSessSeq, __ATOMIC_SEQ_CST);
    do {
        if (!sess_hello()) { rc = kIOReturnNotReady; break; }   // closed while we waited for the lock
        if (!session_unchanged('
plant 112 $H $HP "C: no session at entry (0) counts as unchanged" 'return seqAtEntry != 0u && seqAtEntry == seqNow;' 'return seqAtEntry == seqNow;'
plant 113 $H $HP "C: any non-zero entry sequence counts as unchanged" 'return seqAtEntry != 0u && seqAtEntry == seqNow;' 'return seqAtEntry != 0u;'
plant 114 $H $E "C: the check runs after the record (too late)" '        if (!session_unchanged(seq0, __atomic_load_n(&gSessSeq, __ATOMIC_SEQ_CST))) { rc = kIOReturnNotReady; break; }   // review item C: a different (or no) session now: these pages are the old client'"'"'s
' '' '        out[0] = h; out[1] = size; out[2] = mapped ? va_canonicalize(c.gpuStripped) : 0ull; out[3] = N48N_PLACED_HOST_IMPORT;' '        if (!session_unchanged(seq0, __atomic_load_n(&gSessSeq, __ATOMIC_SEQ_CST))) { rc = kIOReturnNotReady; break; }
        out[0] = h; out[1] = size; out[2] = mapped ? va_canonicalize(c.gpuStripped) : 0ull; out[3] = N48N_PLACED_HOST_IMPORT;'
plant 120 $H $E "D: a failed VMID-8 TLB flush still frees / releases the memory" 'memOk = memory_may_free_after_flush(flush_vmid(kNativeVmid)) && memOk;' '(void)flush_vmid(kNativeVmid);'
plant 121 $H $E "D: a failed flush still completes and releases the host pages" '&& memOk) host_release(b);' ') host_release(b);'
plant 122 $H $E "D: a failed flush still frees the GTT pages" 'if (b.kind == kBoGtt) { if (memOk) sysmem_free(b.sm); }' 'if (b.kind == kBoGtt) sysmem_free(b.sm);'
plant 123 $H $HP "D: the pure verdict always frees" 'constexpr bool memory_may_free_after_flush(uint32_t flushRc) { return flushRc == 0u; }' 'constexpr bool memory_may_free_after_flush(uint32_t flushRc) { (void)flushRc; return true; }'
plant 124 $H $E "D: a failed GART (fence slot) flush is ignored" 'memOk = memory_may_free_after_flush(flush_vmid(0)) && memOk;' '(void)flush_vmid(0);'
plant 130 $W $OP "E: a uid-88 client may call the nub publish / withdraw (0..20)" 'sel <= (uint32_t)N48N_SEL_SCAN_RELEASE ||' 'sel <= (uint32_t)N48N_SEL_METAL_NUB_WITHDRAW ||'
plant 131 $W $OP "E: a uid-88 client may call the DAL step (0..15)" 'sel <= (uint32_t)N48N_SEL_SCAN_RELEASE ||' 'sel <= (uint32_t)N48N_SEL_DAL_STEP ||'
plant 132 $W $OP "E: a uid-88 client may not import (21 dropped)" ' || sel == (uint32_t)N48N_SEL_BO_IMPORT_HOST;' ';'
plant 133 $W $OP "E: an administrator loses the selectors outside the WindowServer set" 'return admin || selector_allowed_for_windowserver(sel);' 'return selector_allowed_for_windowserver(sel);'
plant 134 $W $OP "E: every client reaches every selector" 'return admin || selector_allowed_for_windowserver(sel);' 'return true;'
plant 135 $W $C "E: the client never applies the selector gate" 'if (!n48native::policy::selector_allowed(adminClient, selector)) {' 'if (false) {'
plant 136 $W $C "E: a uid-88 client is recorded as an administrator" 'adminClient = d.reason == n48native::policy::kReasonAdmin;' 'adminClient = true;'
plant 137 $W $C "E: the gate returns BadArgument, not NotPrivileged" '		return kIOReturnNotPrivileged;
	}
	switch (selector) {' '		return kIOReturnBadArgument;
	}
	switch (selector) {'
plant 138 $W $C "E: the gate refuses without a log line" '		if (gSelRefusedLogged < 16u && OSIncrementAtomic((volatile SInt32 *)&gSelRefusedLogged) < 16) NCLOG("selector %u refused: not permitted for a uid-88 (non-administrator) client", selector);
' ''
plant 139 $W $SR/Navi48NativeClient.hpp "E: adminClient defaults to true in the header" 'bool           adminClient { false };' 'bool           adminClient { true };'

# ---- 0.0.620: the import cap is 2 GiB ----
plant 140 $H $HP "0.0.620: the cap is the old 256 MiB again" 'constexpr uint64_t kImportCap     = 2048ull << 20;' 'constexpr uint64_t kImportCap     = 256ull << 20;'
plant 141 $H $HP "0.0.620: the cap check still compares against the old 256 MiB constant" '    if (would_exceed(importedNow, size, kImportCap)) return ImportChk{ kNoMemory, 0, 0 };' '    if (would_exceed(importedNow, size, 256ull << 20)) return ImportChk{ kNoMemory, 0, 0 };'
plant 142 $H $HP "0.0.620: the cap check refuses the exact fit (off by one)" '    if (would_exceed(importedNow, size, kImportCap)) return ImportChk{ kNoMemory, 0, 0 };' '    if (would_exceed(importedNow, size, kImportCap - 1ull)) return ImportChk{ kNoMemory, 0, 0 };'
plant 143 $H $SR/amd/native_s1c_pure.h "0.0.620: would_exceed refuses the exact fit (used >= cap - add)" 'return add > cap || used > cap - add; }' 'return add > cap || used >= cap - add; }'
plant 144 $H $SR/amd/native_s1c_pure.h "0.0.620: would_exceed is used + add > cap (the sum wraps for a wild total)" 'return add > cap || used > cap - add; }' 'return used + add > cap; }'
plant 145 $H $SR/amd/native_s1c_pure.h "0.0.620: would_exceed admits one byte over (used > cap - add + 1)" 'return add > cap || used > cap - add; }' 'return add > cap || used > cap - add + 1ull; }'
plant 146 $H $E "0.0.620: the advisory cap refusal is not logged" '    if (pre.rc == kNoMemory) N1C_LOG("import refused: per-client cap of %llu MiB reached (' '    if (false) N1C_LOG("import refused: per-client cap of %llu MiB reached ('
plant 147 $H $E "0.0.620: the authoritative cap refusal is not logged" '        if (c.rc == kNoMemory) N1C_LOG("import refused: per-client cap of %llu MiB reached under the lock (' '        if (false) N1C_LOG("import refused: per-client cap of %llu MiB reached under the lock ('
plant 148 $H $E "0.0.620: the logged cap is a hard-coded 256" '(unsigned long long)(kImportCap >> 20), (unsigned long long)(__atomic_load_n(&gSess.importedBytes' '(unsigned long long)256, (unsigned long long)(__atomic_load_n(&gSess.importedBytes'
plant 149 $H $SR/Navi48NativeABI.h "0.0.620: the ABI header still carries the 256 MiB cap" '#define N48N_IMPORT_CAP          (2048ull << 20)' '#define N48N_IMPORT_CAP          (256ull << 20)'
plant 150 $H $HP "0.0.620: the per-BO page list grows with the cap (kImportMaxPages from the cap)" 'constexpr uint32_t kImportMaxPages = (uint32_t)(kImportMaxBo / kPage);' 'constexpr uint32_t kImportMaxPages = (uint32_t)(kImportCap / kPage);'
echo "plants: $total run, $escaped escaped/failed"
[ "$escaped" -eq 0 ]
