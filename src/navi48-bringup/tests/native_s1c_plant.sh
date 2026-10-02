#!/bin/zsh
# native_s1c_plant.sh - planted breaks for tests/native_s1c_test.cpp (build 0.0.601).
# For each plant: copy the real sources into a scratch tree, apply ONE break (the script first proves the break changed the text), compile
# the real test against the scratch tree and demand that it FAILS (a compile error from a static_assert counts). A plant the suite lets
# through is a hole: the script exits non-zero and says which.
#   run from anywhere:  src/navi48-bringup/tests/native_s1c_plant.sh
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
K=src/navi48-bringup
SCR="${TMPDIR:-/tmp}/n1c-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0

fresh() {   # the scratch tree: exactly the files native_s1c_test.cpp reads
  rm -rf "$SCR"; mkdir -p "$SCR/$K/src/amd" "$SCR/$K/tests"
  cp "$ROOT/$K/src/Navi48NativeABI.h" "$ROOT/$K/src/Navi48NativeClient.cpp" "$ROOT/$K/src/Navi48Bringup.cpp" "$ROOT/$K/src/Navi48UserClient.cpp" "$SCR/$K/src/"
  cp "$ROOT/$K/src/amd/"native_s1b_pure.h "$ROOT/$K/src/amd/"native_s1c_pure.h "$ROOT/$K/src/amd/"native_s1c.cpp "$ROOT/$K/src/amd/"cp_pm4_gfx12.h "$ROOT/$K/src/amd/"amdgpu_pm4.h "$SCR/$K/src/amd/"
  cp "$ROOT/$K/Makefile" "$ROOT/$K/Info.plist" "$SCR/$K/"
  cp "$ROOT/$K/tests/native_s1c_test.cpp" "$SCR/$K/tests/"
}
# BASELINE (review 0.0.612): the UNMODIFIED scratch copy must compile and PASS. Without this a broken scratch copy (a missing file, a stale header) would make every plant "CAUGHT at compile time".
baseline() {
  fresh; total=$((total+1))
  local out
  out=$(cd "$SCR" && clang++ -std=c++17 -Wall -Wextra -Werror -O0 -I $K/src -I $K/src/amd $K/tests/native_s1c_test.cpp -o "$SCR/t" 2>&1) || { echo "BASELINE: the unmodified scratch copy does not compile: $(echo "$out" | /usr/bin/grep -m1 -E 'error' | cut -c1-140)"; escaped=$((escaped+1)); return; }
  out=$(cd "$SCR" && ./t . 2>&1) || { echo "BASELINE: the unmodified scratch copy FAILS: $(echo "$out" | /usr/bin/grep -m1 '^FAIL' | cut -c1-120)"; escaped=$((escaped+1)); return; }
  echo "BASELINE: the unmodified scratch copy compiles and passes ($(echo "$out" | tail -1))"
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
  local out
  out=$(cd "$SCR" && clang++ -std=c++17 -Wall -Wextra -Werror -O0 -I $K/src -I $K/src/amd $K/tests/native_s1c_test.cpp -o "$SCR/t" 2>&1)
  if [ $? -ne 0 ]; then
    echo "PLANT $id: $desc: CAUGHT at compile time: $(echo "$out" | /usr/bin/grep -m1 -E 'error|static assertion' | cut -c1-140)"; return
  fi
  out=$(cd "$SCR" && ./t . 2>&1)
  if [ $? -eq 0 ]; then echo "PLANT $id: $desc: *** ESCAPED (the suite passed) ***"; escaped=$((escaped+1)); return; fi
  echo "PLANT $id: $desc: CAUGHT by $(echo "$out" | /usr/bin/grep -c '^FAIL') check(s); first: $(echo "$out" | /usr/bin/grep -m1 '^FAIL' | cut -c1-120)"
}

baseline
P=$K/src/amd/native_s1c_pure.h
E=$K/src/amd/native_s1c.cpp
plant 1  $P 'return (rptr - wptr - 1u) & mask; }' 'return (rptr - wptr) & mask; }' "ring_free loses its -1 (a full ring reads as empty)"
plant 2  $P 'return need <= 0xFFFFu && freeDw >= need + kRingSlack; }' 'return need <= 0xFFFFu && freeDw >= need; }' "ring_has_space drops the 16-dword slack"
plant 3  $P '    if (hasFence) k += emit_release_mem(o + k, fenceAddr, seq);
    k += emit_release_mem(o + k, seqAddr, seq);' '    k += emit_release_mem(o + k, seqAddr, seq);
    if (hasFence) k += emit_release_mem(o + k, fenceAddr, seq);' "the kernel seqno is emitted BEFORE the user fence"
plant 4  $P '((vmflags & N48N_VM_PAGE_EXECUTABLE) ? kPteExec : 0ull) |' '0ull |' "leaf_flags drops the EXEC bit"
plant 5  $P 'start < t[i].start + t[i].size && t[i].start < start + size' 'start < t[i].start + t[i].size && t[i].start <= start + size' "va_overlap treats touching ranges as overlapping"
plant 6  $P 'now - h.tProgress >= kHangNs;' 'now - h.tProgress > kHangNs;' "hang_expired off by one at exactly 2 s"
plant 7  $P '    if (size != kCsHdr + kCsIb * v->nIbs) return kBadArg;' '' "cs_parse stops checking the exact size (over-read)"
plant 8  $P '    if (!va_canonical(va)) return VaReq{ kBadArg, 0 };' '' "gemva_check accepts a non-canonical VA"
plant 9  $P '    if (target > emitted) return kBadArg;' '' "wait_resolve accepts a future seqno"
plant 10 $P '    if ((flags & ~kGemAccepted) != 0ull) return Place{ kUnsupported, kPlaceNone, size, align, false, false };' '' "place_decide accepts refused/unknown GEM flags"
plant 11 $P '    if (retired != h.lastRetired) { h.lastRetired = retired; h.tProgress = now; }' '    if (false) { h.lastRetired = retired; h.tProgress = now; }' "the hang clock no longer restarts on progress"
plant 12 $P 'if (hit < 0 || (t[hit].flags & kIbNeedFlags) != kIbNeedFlags) return false;   // an IB fetch needs READ and EXEC' 'if (hit < 0 || (t[hit].flags & N48N_VM_PAGE_EXECUTABLE) == 0u) return false;' "IB coverage drops the READ requirement (EXEC alone covers)"
plant 13 $P '            m.wr(ptb, idx_ptb(v) + (uint32_t)k, pte);' '            m.wr(ptb, idx_ptb(v), pte);' "pt_map writes every leaf at the first index"
plant 14 $P '(system ? (kPteSystem | kPteSnooped) : 0ull);' '(system ? (kPteSystem) : 0ull);' "a GTT leaf loses SNOOPED"
plant 15 $P 'constexpr uint32_t kWbOffsetNativeSeq = 0x0C0u;' 'constexpr uint32_t kWbOffsetNativeSeq = 0x080u;' "the native seqno slot collides with the legacy fence slot"
plant 16 $P '{ return hungBefore ? kAborted :' '{ return hungBefore ? kTimeout :' "a call after the latch returns Timeout instead of Aborted"
plant 18 $E '        if (ring_has_space(ring_free64(r, gWc, mask), n)) break;' '        break;' "ring_emit stops checking free space"
plant 19 $E '    __atomic_store_n(&gSess.emitted, seq, __ATOMIC_RELEASE);
' '' "emitted is never advanced before the doorbell"
plant 20 $E 'if (!OSCompareAndSwap(0, 1, &gOpenFlag)) {' 'if (false) {' "a second native open is no longer refused"
plant 21 $E ' || !va_exec_covered(s->maps, kMaxMaps, st, by[i])) { bad = true; break; }' ') { bad = true; break; }' "Submit no longer checks that the IB is EXEC-mapped"
plant 22 $E '    if (!native_s1b_latched()) return false;' '    return false;' "the legacy refusals never fire"
plant 23 $E '        if (hung_now()) bo_release(h, kRelLeak);                // HUNG: drop the handle, leak the memory and the PTEs, succeed' '        if (false) bo_release(h, kRelLeak);' "BoFree under HUNG frees memory the GPU may still read"
plant 24 $E '    if (!leak && !idle_wait()) leak = true;            // a timeout here latched the hang' '' "close no longer waits for idle before freeing"
plant 25 $E '        if (program_root(gPark.pa) != kOk) leak = true;' '        (void)0;' "close frees memory without parking CONTEXT8"
plant 26 $K/src/Navi48NativeClient.cpp '	if (args->structureInputDescriptor != nullptr || args->structureOutputDescriptor != nullptr) return kIOReturnBadArgument;' '' "out-of-line struct input is no longer refused"
plant 27 $K/src/Navi48Bringup.cpp '	if (type == N48N_UC_TYPE) return IOAccelNavi48NativeClient::create(this, owningTask, securityID, type, properties, handler);' '' "newUserClient no longer creates the native client"
plant 28 $K/src/Navi48UserClient.cpp '	if (amdgpu::n1c_refuse_legacy(amdgpu::kN1cSiteWriteVRAM)) return kIOReturnNotPermitted;   // NATIVE S1c (0.0.601): refused once the native VM self-test ran
' '' "legacy WriteVRAM is no longer refused after the native latch"
plant 30 $P 'constexpr uint64_t retired_clamped(uint64_t slot, uint64_t emitted) { return slot > emitted ? emitted : slot; }' 'constexpr uint64_t retired_clamped(uint64_t slot, uint64_t emitted) { (void)emitted; return slot; }' "a corrupt seqno slot can retire work that was never queued"
plant 31 $P 'constexpr uint64_t wait_timeout_ns(uint64_t t) { return t > kWaitCapNs ? kWaitCapNs : t; }' 'constexpr uint64_t wait_timeout_ns(uint64_t t) { return t; }' "the 2 s wait cap is gone"
plant 32 $P '        if (s[i].used && s[i].handle == handle && s[i].page == page) { *isNew = false; return (int)i; }' '        if (false && s[i].used && s[i].handle == handle && s[i].page == page) { *isNew = false; return (int)i; }' "a fence target never reuses its window slot (the window drains)"
plant 33 $P '    if ((s >> 47) != ((s + size - 1ull) >> 47)) return VaReq{ kBadArg, 0 };      // never straddle the sign-extension seam' '' "a mapping may straddle the sign-extension seam"
plant 34 $P 'm.wr(tbl, idx_ptb(v) + (uint32_t)k, 0ull);' 'm.wr(tbl, idx_ptb(v) + (uint32_t)k, kPteValid);' "unmap leaves a leaf valid"
plant 35 $P '        if ((fl & kIbUnsupportedFlags) != 0u) return kUnsupported;' '' "a CE / SECURE IB flag is accepted"
plant 36 $P 'inline void hang_kick(Hang &h, uint64_t retired, uint64_t emittedBefore, uint64_t now) { if (retired >= emittedBefore) h.tProgress = now; }' 'inline void hang_kick(Hang &h, uint64_t retired, uint64_t emittedBefore, uint64_t now) { (void)retired; (void)emittedBefore; h.tProgress = now; }' "a kick on a busy ring restarts the hang clock (a stuck ring is never detected)"
plant 37 $P '    if (size != kCsHdr + kCsIb * v->nIbs) return kBadArg;
    if (rd64(p + 24) != 0ull) return kBadArg;' '    if (size != kCsHdr + kCsIb * v->nIbs) return kBadArg;' "the reserved qword of the Submit header is not checked"
plant 38 $E '    const uint64_t wptr64 = wc;   // the FULL 64-bit counter, never masked' '    const uint64_t wptr64 = wc & mask;' "the doorbell / wptr shadow get the MASKED counter (it decreases at every wrap)"
plant 39 $E '    if (!regs_allowed(off, count) || (off + count) * 4ull > gCtx->dev->rmmioSize) return kIOReturnBadArgument;   // allowlist 0x263e..0x2641 only' '    if ((off + count) * 4ull > gCtx->dev->rmmioSize) return kIOReturnBadArgument;' "ReadRegs reads any register"
plant 40 $P 'off >= kRegsAllowFirst && off <= kRegsAllowLast && off + count - 1ull <= kRegsAllowLast;' 'off >= kRegsAllowFirst && off <= kRegsAllowLast;' "the ReadRegs allowlist lets a read run past 0x2641"
plant 41 $P 'constexpr uint32_t ring_idx(uint64_t wc, uint32_t mask) { return (uint32_t)(wc & mask); }' 'constexpr uint32_t ring_idx(uint64_t wc, uint32_t mask) { return (uint32_t)(wc % (mask + 2u)); }' "the ring index is computed wrongly across the wrap"
plant 42 $E '    for (uint32_t i = 0; i < s->nblk; i++) gPtFree[gPtFreeN++] = s->blk[i];' '    for (uint32_t i = 0; i < s->nblk; i++) gCtx->gmc.vram_alloc.free(gPt[s->blk[i]]);' "page-table blocks go back to vram_alloc (a stale user mapping could alias a page table)"
plant 43 $E 'static Session gSess;          // STATIC and never freed: a selector racing a close can never touch freed memory' 'static Session gSess; static void *gSessHeap = IOMallocAligned(4096, 4096);' "the session stops being a plain static (lifetime pin)"
plant 44 $E '    if (!sess_hello()) { IOLockUnlock(gCliLock); return kIOReturnNotReady; }   // closed while we waited for the lock
    Session *s = gS;
    IOReturn rc = kIOReturnSuccess;
    do {
        if (in->operation == N48N_VA_OP_UNMAP && hung_now()) {' '    Session *s = gS;
    IOReturn rc = kIOReturnSuccess;
    do {
        if (in->operation == N48N_VA_OP_UNMAP && hung_now()) {' "GemVa forgets to re-check the session under the lock"
plant 45 $E '            if (b.vmd == nullptr) {' '            if (true) {' "clientMemoryForType creates a NEW descriptor on every call (unmap can never find the mapping)"
plant 46 $E '        if (b.vmd != nullptr) { b.vmd->release(); b.vmd = nullptr; }   // the descriptor is released once, before the range it names is freed
        if (b.kind == kBoGtt) sysmem_free(b.sm);' '        if (b.vmd != nullptr) { b.vmd->release(); b.vmd = nullptr; }   // the descriptor is released once, before the range it names is freed
        if (b.vmd != nullptr) { b.vmd->release(); b.vmd = nullptr; }
        if (b.kind == kBoGtt) sysmem_free(b.sm);' "close releases the descriptor twice"
plant 47 $E '        if (slotCleared) { amdgpu_hdp_flush(*gCtx->dev); memOk = memory_may_free_after_flush(flush_vmid(0)) && memOk; }
        if (b.vmd != nullptr) { b.vmd->release(); b.vmd = nullptr; }   // the descriptor is released once, before the range it names is freed
' '        if (slotCleared) { amdgpu_hdp_flush(*gCtx->dev); memOk = memory_may_free_after_flush(flush_vmid(0)) && memOk; }
' "BoFree never releases the descriptor (leak)"
# control: a plant whose text does not exist must be REPORTED as not applied (so a typo in a plant can never read as "caught").
ctl=$(plant 99 $P 'this text does not exist anywhere in the header' 'x' "(control)" 2>&1)
if echo "$ctl" | /usr/bin/grep -q "THE PLANT ITSELF DID NOT APPLY"; then echo "control: an unappliable plant is reported as unapplied (ok)"; else echo "control FAILED: $ctl"; escaped=$((escaped+1)); fi
echo "planted: $total, escaped or unapplied: $escaped"
[ "$escaped" -eq 0 ]
