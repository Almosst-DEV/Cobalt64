#!/bin/zsh
# native_sdmaorder_plant.sh - planted breaks for tests/native_sdmaorder_test.cpp (build 0.0.657, GitHub issue #1: the SDMA0_DCC_CNTL clear lands before the ladder's own SDMA copies).
# For each plant: copy the real sources into a scratch tree, apply the break (exact-once replacements; the script first proves each replaced text occurs exactly once),
# compile the named real test against the scratch tree and demand that it FAILS (a compile error, a failing check, a crash or a hang cut off by the alarm all count).
# A plant the suite lets through is a hole: the script exits non-zero and says which. The CONTROLS (no break) must pass.
#   run from anywhere:  src/navi48-bringup/tests/native_sdmaorder_plant.sh [first-plant-id last-plant-id]
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
K=src/navi48-bringup
SR=$K/src
SCR="${TMPDIR:-/tmp}/nsdmao-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0
FIRST=${1:-0}; LAST=${2:-9999}

fresh() {
  rm -rf "$SCR"; mkdir -p "$SCR/$K/tests" "$SCR/tools"
  cp -R "$ROOT/$SR" "$SCR/$SR"
  cp "$ROOT/$K/Makefile" "$ROOT/$K/Info.plist" "$SCR/$K/"
  cp "$ROOT/$K/tests/native_sdmaorder_test.cpp" "$SCR/$K/tests/"
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
  local msg; msg=$(build_run native_sdmaorder_test); local brc=$?
  if [ $brc -eq 0 ]; then echo "CONTROL native_sdmaorder_test (no break): $msg"; else echo "CONTROL native_sdmaorder_test (no break): *** FAILED: $msg ***"; escaped=$((escaped+1)); fi
}
INIT=$SR/amd/amdgpu_init.cpp
B=$SR/Navi48Bringup.cpp
T=native_sdmaorder_test
CALL='        navi48_sdmadcc_default();
'
control
# ---- the order (the point of the build) ----
plant 1 $T $INIT "the call moves AFTER the sweep (the original order bug: copies run on the boot default)" "$CALL" '' '        run_pdb0_selftest(ctx);
' "$CALL"'        run_pdb0_selftest(ctx);
'
plant 2 $T $INIT "the call moves between the copy test and the sweep" "$CALL" '' '        constexpr bool kSweepEnabled = true;' "$CALL"'        constexpr bool kSweepEnabled = true;'
plant 3 $T $INIT "the call moves to the end of the stage's copy block (after the sweep and the self-tests)" "$CALL" '' '        if (ctx.sdmaQ1TestWant) {' "$CALL"'        if (ctx.sdmaQ1TestWant) {'
plant 4 $T $INIT "the call is removed from the stage" "$CALL" ''
plant 5 $T $INIT "the call comes BEFORE sdma_init_full (SDMA not up)" "$CALL" '' '        r = sdma_init_full(dev, ctx.psp, ctx.gmc, ctx.sdma);' "$CALL"'        r = sdma_init_full(dev, ctx.psp, ctx.gmc, ctx.sdma);'
plant 6 $T $INIT "the call comes before sdma_init_full's failure return" "$CALL" '' '        if (r != kIOReturnSuccess) return r;
        // 0.0.657' "$CALL"'        if (r != kIOReturnSuccess) return r;
        // 0.0.657'
plant 7 $T $INIT "the stage calls it twice" "$CALL" "$CALL$CALL"
plant 8 $T $INIT "the declaration is dropped" 'void navi48_sdmadcc_default(void);
' ''
# ---- the once-per-boot flag ----
plant 9 $T $B "the already-done check is removed (a second write in runStages)" '	if (gSdmaDccDefaultDone) {
		N48LOG("sdmadcc: DEFAULT already handled earlier this boot (right after sdma_init_full, before the copy test) - no second write");
		return;
	}
' ''
plant 10 $T $B "the already-done check no longer returns" '- no second write");
		return;' '- no second write");'
plant 11 $T $B "the flag is not set after the write" '	navi48_reg_write32(reg0, target);
	gSdmaDccDefaultDone = true;' '	navi48_reg_write32(reg0, target);'
plant 12 $T $B "the opt-out path does not set the flag" '		gSdmaDccDefaultDone = true;
		N48LOG("sdmadcc: DEFAULT SKIPPED - navi48-sdmadcc=0' '		N48LOG("sdmadcc: DEFAULT SKIPPED - navi48-sdmadcc=0'
plant 13 $T $B "the no-DeviceContext skip sets the flag (the later call would never retry)" 'if (!gBringup.dev) { N48LOG("sdmadcc: DEFAULT SKIPPED - no DeviceContext"); return; }' 'if (!gBringup.dev) { gSdmaDccDefaultDone = true; N48LOG("sdmadcc: DEFAULT SKIPPED - no DeviceContext"); return; }'
plant 14 $T $B "the GC-base skip sets the flag" 'if (!gc) { N48LOG("sdmadcc: DEFAULT SKIPPED - GC BASE_IDX 0 did not resolve"); return; }' 'if (!gc) { gSdmaDccDefaultDone = true; N48LOG("sdmadcc: DEFAULT SKIPPED - GC BASE_IDX 0 did not resolve"); return; }'
plant 15 $T $B "the flag is set only after the readback (a second entry could slip in between)" '	navi48_reg_write32(reg0, target);
	gSdmaDccDefaultDone = true;
	const uint32_t rb = navi48_reg_read32(reg0);' '	navi48_reg_write32(reg0, target);
	const uint32_t rb = navi48_reg_read32(reg0);
	gSdmaDccDefaultDone = true;'
plant 16 $T $B "navi48-sdmadcc=0 no longer opts out" 'if (!n48_sdma_dcc_default_on(present, ba)) {
		gSdmaDccDefaultDone = true;' 'if (false) {
		gSdmaDccDefaultDone = true;'
plant 17 $T $B "the runStages call is dropped" '		navi48_sdmadcc_default();
	}
	publishPDB0On(this, ctx);' '	}
	publishPDB0On(this, ctx);'
plant 18 $T $B "a second write is added to the default function" '	gSdmaDccDefaultDone = true;
	const uint32_t rb' '	gSdmaDccDefaultDone = true;
	navi48_reg_write32(reg0, target);
	const uint32_t rb'
echo "plants: $total run, $escaped escaped/failed"
[ "$escaped" -eq 0 ]
