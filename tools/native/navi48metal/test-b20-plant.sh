#!/bin/zsh
# test-b20-plant.sh - planted breaks for bundle build 20 (the HDMI rubberband fix, an internal design note "Q3. Fix contract"): F1 (known scanout surfaces), F2 (per-slot supersede marks), F3 (re-copy with the
# noted seq), F4 (the per-instance 10 s line), the version, and the diff guard (display mode: nothing else changed; apps mode: the display code equals base + the accepted delta). Same discipline as test-b19-plant.sh: copy the
# REAL files into a scratch tree (tools/native/navi48metal + the two kernel headers the pins read), apply ONE break (the script proves the text was there exactly once), run the suites and demand at least one FAILS (a compile
# error counts). A plant the tests let through is a hole: exit non-zero.
#   usage: test-b20-plant.sh           env: TMPDIR (where the scratch tree goes)        Fast mode: C compiles at -O0, no sanitizers.
set -u
D="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(git -C "$D" rev-parse --show-toplevel)"
SCR="$(mktemp -d "${TMPDIR:-/tmp}/b20-plant.XXXXXX")"
case "$SCR" in */b20-plant.*) ;; *) echo "bad scratch dir $SCR"; exit 2;; esac
trap 'rm -rf -- "$SCR"' EXIT
escaped=0; total=0
W="$SCR/c/tools/native/navi48metal"
cfresh() {
  rm -rf -- "$SCR/c"; mkdir -p "$W" "$SCR/c/src/navi48-bringup/src/amd"
  cp "$D"/n48_*.h "$D"/Navi48Device.m "$D"/Info.plist "$D"/INSTALL.md "$D"/test-dispflip.c "$D"/test-m6x.c "$D"/test-m6route.c "$W/"
  cp "$ROOT/src/navi48-bringup/src/Navi48NativeABI.h" "$SCR/c/src/navi48-bringup/src/"
  cp "$ROOT/src/navi48-bringup/src/amd/native_m6_pure.h" "$SCR/c/src/navi48-bringup/src/amd/"
}
one() {   # one <name> <extra cc flags>: compile and run one suite in the scratch tree
  local n="$1"; shift
  ( cd "$W" && cc -O0 -Wall -Wextra -Werror "$@" -o "t-$n" "test-$n.c" ) >"$SCR/cc.out" 2>&1 || { OUT="test-$n: compile error: $(/usr/bin/grep -m1 error "$SCR/cc.out" | cut -c1-110)"; return 1; }
  ( cd "$W" && "./t-$n" . ) >"$SCR/o0" 2>&1 || { OUT="test-$n: $(/usr/bin/grep -m1 '^FAIL' "$SCR/o0" | cut -c1-120)"; return 1; }
  return 0
}
c_test() {
  OUT=""
  one dispflip || return 1
  one m6x -D_GNU_SOURCE || return 1
  one m6route -D_GNU_SOURCE || return 1
  return 0
}
apply() {   # apply <file> <old> <new>  -> exit 3 if the text is not there exactly once
  OLD="$2" NEW="$3" FILE="$1" python3 - <<'PY'
import os,sys
p=os.environ['FILE']; s=open(p).read(); o=os.environ['OLD']; n=os.environ['NEW']
if s.count(o)!=1:
    print("PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %r" % (s.count(o), o[:80])); sys.exit(3)
open(p,'w').write(s.replace(o,n))
PY
}
cplant() {   # cplant <id> <file in the scratch dir> <old> <new> <description>
  local id="$1" file="$2" old="$3" new="$4" desc="$5"
  cfresh; apply "$W/$file" "$old" "$new"; local rc=$?; total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  if c_test; then echo "PLANT $id: $desc: *** ESCAPED (the tests passed) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: CAUGHT by $OUT"; fi
}
gplant() {   # gplant <id> <mode> <old> <new> <description>: a break in Navi48Device.m that the DIFF GUARD in <mode> (display | apps, default bases) must refuse
  local id="$1" mode="$2" old="$3" new="$4" desc="$5"
  cfresh; apply "$W/Navi48Device.m" "$old" "$new"; local rc=$?; total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  if ( cd "$D" && N48_DG_NEW="$W/Navi48Device.m" ./test-b17-diffguard.sh "$mode" >"$SCR/g.out" 2>&1 ); then echo "PLANT $id: $desc: *** ESCAPED (the $mode guard passed) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: CAUGHT by the $mode diff guard: $(/usr/bin/grep -m1 -E 'FAIL:|NOT ALLOWED|CHANGED:|hunk inside' "$SCR/g.out" | cut -c1-110)"; fi
}
cfresh; total=$((total+1)); if c_test; then echo "BASELINE (C): the unmodified scratch copy passes"; else echo "BASELINE (C): FAILED: $OUT"; escaped=$((escaped+1)); fi
for m in display apps; do total=$((total+1)); if ( cd "$D" && ./test-b17-diffguard.sh $m >"$SCR/g.out" 2>&1 ); then echo "BASELINE (guard $m): the real tree passes (default base 4eb7cab3)"; else echo "BASELINE (guard $m): FAILED"; tail -3 "$SCR/g.out"; escaped=$((escaped+1)); fi; done
M=Navi48Device.m; H=n48_dispflip.h; X=n48_m6x.h
# ---- the contract's three named breaks: F1 reverted, F2 reverted, a fresh seq in R3
cplant 1  $H "(known && sid) || n48df_is_scan(s, sid)" "n48df_is_scan(s, sid)" "F1 reverted in the header: a known scanout surface needs its table entry again (the saturation bug)"
cplant 2  $M "n48df_account_sid(am, mask, sid, N48S.m6.latch && !tent)" "n48df_account_sid(am, mask, sid, 0)" "F1 reverted at the caller: known is never passed"
cplant 3  $H "if (s->sub_seq[j] < seq) { s->sup_n[j]++; s->made[slot] |= (uint8_t)(1u << j); }" "if (0) { s->sup_n[j]++; s->made[slot] |= (uint8_t)(1u << j); }" "F2 reverted: a later submission supersedes nothing"
cplant 4  $M "const uint64_t seq = x->recopy_seq;" "const uint64_t seq = atomic_fetch_add(&N48S.seq, 1) + 1;" "F3 reverted: the re-copy takes a fresh newest seq (the backward jump)"
# ---- F1 further breaks
cplant 5  $H "(known && sid) || n48df_is_scan(s, sid)" "known || n48df_is_scan(s, sid)" "F1: an unknown surface (sid 0) is presented when known"
cplant 6  $M "N48S.m6.latch && !tent)" "N48S.m6.latch)" "F1: a TENTATIVE frame counts as known (an unknown surface becomes presentable)"
cplant 7  $H "if ((m & N48DF_W_DRAW_FINAL) && !known) n48df_scan_set(s, sid);" "if (m & N48DF_W_DRAW_FINAL) n48df_scan_set(s, sid);" "F1: known surfaces still fill the proxy flag table (the table can saturate again)"
cplant 8  $H "if ((m & N48DF_W_DRAW_FINAL) && !known) n48df_scan_set(s, sid);" "if ((m & N48DF_W_DRAW_FINAL) && !known) (void)0;" "F1: the proxy flag is never set (tentative / latch-off composites are refused)"
# ---- F2 further breaks
cplant 9  $H "else if (s->sub_seq[j] > seq) { s->sup_n[slot]++; s->made[j] |= (uint8_t)(1u << slot); }" "else if (0) { s->sup_n[slot]++; s->made[j] |= (uint8_t)(1u << slot); }" "F2: an old seq submitted after a newer one is not itself superseded"
cplant 10 $H "if (vkres != 0) { n48df_unsupersede(s, slot); return n48df_complete_seq(s, slot, vkres, seq); }" "if (vkres != 0) { return n48df_complete_seq(s, slot, vkres, seq); }" "F2: a GPU-failed superseder keeps its marks (the earlier frame is never shown)"
cplant 11 $H "s->sub_sid[slot] = 0; s->sub_seq[slot] = 0; s->sup_n[slot] = 0; s->made[slot] = 0;" "s->sub_sid[slot] = 0; s->sub_seq[slot] = 0; s->made[slot] = 0;" "F2: a released slot keeps its superseded count (the next frame in that slot is skipped)"
cplant 12 $H "    for (int k = 0; k < N48DF_MAX_SLOTS; k++) s->made[k] &= (uint8_t)~(1u << slot);
" "" "F2: a released slot stays in other slots' made[] (a later failure takes a mark off the slot's next frame)"
cplant 13 $H "if (!n48df_is_superseded(s, slot)) w = n48df_hold_ns(on, hms, tcommit, now);" "w = n48df_hold_ns(on, hms, tcommit, now);" "F2: the present hold waits for a frame that is already superseded"
cplant 14 $M "if (_dsid) n48df_submit(dsm, _dslot, _dsid, _dseq);" "if (_dsid) n48df_submit(dsm, -1, _dsid, _dseq);" "F2: the submit-time bookkeeping loses the slot (nothing is ever marked)"
cplant 15 $H "if (slot < 0 || slot >= N48DF_MAX_SLOTS || !s->inflight[slot]) { s->surf_untracked++; return; }" "if (slot < 0 || slot >= N48DF_MAX_SLOTS || !s->inflight[slot]) { return; }" "F4: an untracked submission is not counted"
# ---- F3 further breaks
cplant 16 $X "x->recopy_sid = sid; x->recopy_seq = seq;" "x->recopy_sid = sid;" "F3: the note does not remember the frame's seq (the re-copy would present with 0 or a stale value)"
cplant 17 $M "n48x_recopy_note(&cx->xs, sid, seq);" "n48x_recopy_note(&cx->xs, sid, 0);" "F3: the caller notes seq 0"
# ---- F4 further breaks
cplant 18 $M "n48x_rubber_summary_locked(xi); } }" "} }" "F4: the per-instance 10 s line is never logged"
cplant 19 $H "if (s->nscan >= N48DF_MAX_SURF) { s->scan_full_refused++; return 0; }" "if (s->nscan >= N48DF_MAX_SURF) { return 0; }" "F4: scan_full_refused is never counted"
cplant 20 $M "n48df_nonscan_used(&X->sm), N48DF_MAX_SURF, n48df_marked_count(&X->sm)" "0, N48DF_MAX_SURF, 0" "F4: the occupancy fields are constant"
# ---- version
cplant 21 Info.plist "<key>CFBundleVersion</key>
	<string>20</string>" "<key>CFBundleVersion</key>
	<string>19</string>" "the bundle is still build 19"
# ---- diff guard, display mode: nothing else changed
gplant 22 display "static BOOL n48s_testmode(void) { const char *e = getenv(\"N48M_TEST_DISPFLIP\"); return n48_allow() && e && !strcmp(e, \"1\"); }" "static BOOL n48s_testmode(void) { const char *e = getenv(\"N48M_TEST_DISPFLIP\"); return n48_allow() && e && !strcmp(e, \"2\"); }" "a one-line scanout function outside the allowed set is edited"
gplant 23 display "snprintf(m, sizeof m, \"scanout_status rc %d\", rc);" "snprintf(m, sizeof m, \"scanout_status RC %d\", rc);" "a multi-line scanout function outside the allowed set (n48s_status_locked) is edited"
gplant 24 display "- (void)n48PoolDone { [self n48UCRelease]; [self n48OccResolve]; if (_pser) n48_fence_close(_pser); }" "- (void)n48PoolDone { [self n48UCRelease]; if (_pser) n48_fence_close(_pser); }" "an app-fix line (n48PoolDone) changes"
# ---- diff guard, apps mode: the display code equals base + the accepted delta
gplant 25 apps "n48df_account_sid(am, mask, sid, N48S.m6.latch && !tent)" "n48df_account_sid(am, mask, sid, 0)" "the apps guard also refuses a revert of F1 (the display code must equal base + delta)"
gplant 26 apps "static BOOL n48s_testmode(void) { const char *e = getenv(\"N48M_TEST_DISPFLIP\"); return n48_allow() && e && !strcmp(e, \"1\"); }" "static BOOL n48s_testmode(void) { const char *e = getenv(\"N48M_TEST_DISPFLIP\"); return n48_allow() && e && !strcmp(e, \"2\"); }" "the apps guard refuses an edit of a scanout function"
echo "test-b20-plant: $total planted/baseline runs, $escaped escaped"
[ $escaped -eq 0 ]
