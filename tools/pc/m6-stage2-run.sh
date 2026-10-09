#!/bin/bash
# m6-stage2-run.sh - M6 STAGE 2 RUN: THE MONA (instance 1: HUBP1 / OTG1, HDMI 1080p60) AS THE THIRD GPU-COMPOSITED DISPLAY, beside the DP (instance 0) and the monitor B (instance 2).
# RUN ON THE AIR; drives the PC over ssh (host alias `navi48`) ONLY when the brief grants PC operation (pc-protocol.md). `DRYRUN=1` prints every ssh command, runs none.
# UNTRACKED (not committed) until the reviewer says so. Derived from tools/pc/m6-stage1b-run.sh (the proven 1b kit, incl. every false alarm fixed after real runs: aux loads only after the nub publish; PXOUT overwrite order;
# only WindowServer's bundle lines; review M1/M2; A from `disp2 crc` before Acquire; the _windowserver console owner right after the restart; WANT_* env pins; FLIP_PENDING stuck = the SAME programmed/EARLIEST over 3 reads;
# the 0.0.662+ CLI's 'Acquire refused' wording; the login window may live on ANY display so floors are judged PER DISPLAY and an idle display is not a fault), an internal design note "## Stage 2 build contract" section 4
# (R0 / K1-K7 / C1'-C10 / C7' swap detector), the "## Stage 2 safety review" section "Run-time checks for the kit", and notes/
#
# DEPLOY NOTE (the reviewer deploys; there is no deploy script): kernel 0.0.663 or 0.0.664 (WANT_KEXT_RE, default 0\.0\.66[34]; the Stage 2 flip needs 0.0.662+, `vramstat` needs 0.0.663+), aux kext 0.0.7 at
#   /Library/Extensions/Navi48Accel-0.0.7.kext (no change), Metal bundle 19 (WANT_BUNDLE, default 19: it carries the R-B1 / R-S2 fixes of bundle 17), CLI navi48test of the 0.0.663+ build (m6xstat <inst> <page>, m6stat 4, vramstat),
#   variant stage17-native-1440-metal-disp-amfi-m6flip3 (or its -rebar0 / -rebar1g siblings): boot-args navi48-m6=1 navi48-m6flip=1 navi48-m6flip1=1 + the 1b set (navi48-fb2=1 navi48-metal-disp=1 navi48-disp2=1 navi48-dmubcmd=1),
#   on a FRESH BOOT (one run per boot; 0 console users; not armed; nothing published; the DP at its full-res 1440p raster; /private/tmp/n48m-noflip1 and -noflip2 ABSENT; /private/tmp/n48m-noinproc is NOT needed).
#   Two boots: first STAGE=2a (this kit touches noflip1 itself before the WindowServer start: the monitor A stays on IOPresentment / the bars, never flips; K1-K7 for THREE displays), later STAGE=2b (noflip1 absent: the monitor A flips).
#   STAGE=2b REFUSES (exit 2, nothing written) unless the installed bundle binary carries the R-B1 fix token ('bpr %lu, %s; plan' - the new copy-gate log text; the review's blocker B1).
#
# RECIPE (Stage 2 contract section 4)
#   P0  prechecks: 0 users, kext / aux / bundle versions, ALL THREE latches, fresh boot, kill files, the R-B1 token, bundle LC_UUID, vramstat baseline (pool size class), the 0x4fea DP-raster check (REFUSE on the 1080p fallback)
#   P1  DP baseline.  P2 the monitor B chain (exactly 1b) -> monitor B CRC_A; then the MONA chain (dmubsend pclk otg1-on, phyc enable, digc setup; disp2 timing 1, connect 1, plane 1, show 1), the monitor B CRC re-read after EVERY monitor A op,
#       monitor A CRC_A1 MEASURED on this boot (3 equal reads; e7a0b345 is only compared as information). BOTH chains run BEFORE any hold (after fbhold 2 no dmubsend of any kind).
#   P3  fbhold 2, fbhold 1, fbpublish 2, then WAIT until the monitor B's Navi48Framebuffer is registered in ioreg (review S1), fbpublish 1, wait for the second node; BOTH registry entry IDs recorded, the monitor B's must be the LOWER.
#   P4  nub publish -> aux loaded -> 2 Navi48Framebuffer nodes -> pipeadopt (pipes ours / all = 3 / 3) -> pipeagdc -> fbname -> headless flag -> [2a: touch noflip1] -> pipearm; HOOK A (review checks); ONE WS restart with ep.d.
#   P5b pre-flip K-snapshots (R0 + K1..K7 counters), P6 the flips (C1' / C2' per instance), P7 watch, P8 C9' (USER_STEP=1) and the kill-switch legs C7', P9 final proofs (K1 K2 K3 K5 K7), verdicts.
#
# ABORT (per-instance kill files /private/tmp/n48m-noflip1 and -noflip2): the 1b list per instance (HUBPn underflow / ODMn bit 10 / write refusals+failures / restore failures / HIGH dword != A's / address or EARLIEST outside
#   {own A, own slots} or EQUAL to the OTHER display's slot or A/B / FLIP_PENDING stuck / GPU-HELD code 4 / watchdog restores > 0 / geometry-word line / inst_geom_mismatch rising after startup / the monitor B stalling during the monitor A's acquire /
#   any LEAKED line / a monitor B-watch row other than EARLIEST_INUSE2); a VM fault (DCN_VM_FAULT_STATUS is global) or HUNG or a WindowServer pid change touches BOTH kill files. The ABORT path: quick capture FIRST, then touch the
#   kill file(s) of the affected instance(s), require EARLIEST = programmed = A and the CRC back to CRC_A within 6 s (PC clock), else "REBOOT WITHOUT FREEING ANYTHING"; then the full capture and PARK. NO teardown verbs anywhere.
#   N1: a pool-refused instance logs ONE VERIFIED restore of A (restores 1, failed 0): EXPECTED, judged, never a fault.
#
# ROLLBACK (documented, not automated): ssh navi48 '~/navi48-staging/esp-config.sh stage17-native-1440-metal-disp-amfi-m6flip && ~/navi48-staging/reboot-pc.sh' (0 users), wait-for-driver.sh 300.
# STOP RULES: PC unreachable after one more check -> "PC needs a manual reset"; two panics in a row on this build -> stop; a user logging in during the watch -> stop and report.
#
# PASS CRITERIA -> HOW THIS SCRIPT MEASURES EACH (the final P10 section prints each verdict with its reason)
#   R0  K1..K7 re-confirmed BEFORE the first flip (the last pre-flip K-snapshot; falls back to the flip snapshot): AGDC nfb 3 (m6stat 1), m6stat 4 bitmask exactly 0x7 {0,1,2}, out-of-range endpoints 0, n921 >= 1 for the monitor A and the
#       monitor B (m6stat 4), transactions > 0 on all three pipes, no ambiguous IDs / no-framebuffer submits, both CRCs exact (monitor B CRC_A pinned, monitor A CRC_A1 measured), all HUBP1/HUBP2 underflow lines 0 and DIG2/DIG3 FIFO error 0 (disp2 crc 1|2).
#   K1  WS log 'Unable to find display pipe' == 0 and 'compositor activated' == 3; ioreg: 3 IOFramebuffer (RDNA4FB + 2 Navi48Framebuffer).  K2 transactions > 0 on all three pipes (m6stat 0).  K3 IDs 2..IDS_MAX per pipe (WARN above 3), 0 ambiguous.
#   K4/K5 floors judged PER DISPLAY (m6stat 1 stamps/s over 3 s, bundle 'm6:' DP presents/s): an ACTIVE display (baseline >= ACT_MIN) must stay above max(STAMP_MIN,50% of baseline) (2 consecutive K-snapshots -> abort); an IDLE display has
#       no rate floor. monitor A stamps written > 0 and refused 0 (m6stat 1).  K6 = R0's CRC / underflow / FIFO part.  K7 = nfb 3 + endpoints {0,1,2} (live bitmask AND ep.out) + n921 + out-of-range 0 + inst_geom_mismatch 0 + system_profiler sizes.
#   C1' each ENABLED HDMI instance acquired with 3 slots (m6xstat <i> 0/1), all slot MCs pairwise disjoint and outside A1/B1/A2/B2, the DP console and the DP slots; free VRAM logged (vramstat before the restart, in every K-snapshot, right after
#       each acquire, plus the bundle's 'free visible VRAM' line). POOL class from vramstat (visible total < 400 MiB = SMALL): SMALL -> at least one enabled HDMI display acquires and every other one is a clean POOL refusal (N1);
#       BIG -> every enabled one must acquire. Which one was refused is reported.
#   C2' per flipping instance: the OTG<i> CRC leaves its CRC_A within FLIP_S s of the restart (PC clock), EARLIEST<i> is one of ITS slots, FLIP_PENDING not stuck. 2a: the monitor A must stay on A (CRC_A1 at EVERY sample, never acquired, bundle says OFF + kill file PRESENT).
#   C3' at rest, per flipping instance: >= 1 equal CRC pair, 0 returns to its CRC_A, 0 watchdog restores, >= REST_MIN_S (300) s at rest; static instances (2a monitor A, pool-refused): CRC == CRC_A at every sample.
#   C4' every fast sample (~FAST_SLEEP+4 s) and every minute: HUBP1/HUBP2 underflow, ODM1/ODM2 bit 10, the global VM fault and HUNG all 0 (any non-zero aborts; VM fault / HUNG touch BOTH kill files).
#   C5' per-instance counters PARSED and reported: REUSE_INUSE_REFUSED (m6xstat), inst_mismatch / inst_geom_mismatch (bundle counters line); inst_geom_mismatch rising after startup aborts, a non-zero final value fails K7.
#   C6' the DP is unchanged (OTG0 COUNTING, the 15-row plane watch in every crc, DIG1_DIG_BE_CNTL change = WARNING, per-display floors above) and the monitor B keeps flipping through the monitor A's acquire (monitor B presents rate across the acquire).
#   C7' swap detector (END of the watch, SKIP_KILL=1 skips): 2b leg 1 touch noflip1: within 6 s EARLIEST1 = A1 and the monitor A CRC == CRC_A1 WHILE EARLIEST2 stays one of the monitor B's slots and the monitor B CRC is NOT CRC_A2; the monitor A restore count
#       rises by 1 (VERIFIED), the monitor B's does not move; leg 2 touch noflip2: the monitor B returns to CRC_A2 (EARLIEST2 = A2) within 6 s, monitor B restores +1, the monitor A's unchanged; restoreFailed 0, CRC exact, LEAKED lines == the P0 baseline.
#       2a runs only leg 2.
#   C9' USER_STEP=1 only: 'move the mouse onto the MONA now': 30 samples of both CRCs every 2 s; PASS = the monitor A CRC changes from its start and its last 5 are equal while the monitor B CRC takes <= 3 values and its last 5 are equal; else NOT-OBSERVED.
#   C10 WindowServer pid stable after the restart, 0 HUNG, 0 new crash reports (find /Library/Logs/DiagnosticReports -newer the P0 marker).
#   S1  (review S1) the monitor B's Navi48Framebuffer registry entry ID is LOWER than the monitor A's (the monitor B published and registered first).
#
# AMBIGUITIES (for the reviewer)
#   1. 'DP/monitor B/monitor A floors per display': the 1b DP floor aborted falsely when the login window lived on the monitor B. Here a display is ACTIVE only if its minute-1 rate >= ACT_MIN; the floor abort needs 2 consecutive K-snapshots
#      below max(STAMP_MIN, 50% of baseline); a drop below 85% is a WARNING. If NO display is active the floors are vacuous (WARNING). This is weaker than the review's 'abort on the DP below its floors' on purpose.
#   2. `disp2 crc 1` is an monitor A plane op: the kext takes a monitor B watch (9 rows incl. EARLIEST_INUSE2) before/after it. While the monitor B is GPU-flipping, row 6 (EARLIEST_INUSE2) legitimately changes between the two snapshots -> the CLI prints
#      'monitor B watch: CHANGED' / 'THE MONB WAS DISTURBED', exits 4 and tries `planeoff 1` (the kernel REFUSES it: the plane is HELD). The kit reads the CLI's own 'changed mask' and treats ONLY row 6 (mask 0x40) with the monitor B ACQUIRED as benign
#      (counted, reported); row 8 (0x100, OTG2 not counting), rows 0-5, 7, or row 6 with the monitor B not acquired abort BOTH instances. SUSPECTED benign: the kernel returns the CRC in the same output (CONFIRMED by reading navi48_disp2_flow.h:775).
#   3. K3 'IDs 2-3 per pipe': the monitor B has shown 4 IDs on real 0.0.663 runs (queue 329); the kit allows up to IDS_MAX=4 (WARN above 3).
#   4. CRC_A1 is measured per boot (review); 'e7a0b345/b196d20f' is compared only as information (a mismatch is a WARNING).
#   5. K7's system_profiler leg: a WRONG size (a swap signature) fails K7; fewer than 3 displays listed is a WARNING (Run B's review: the pass criterion is WindowServer drawing, not system_profiler).
#   6. 'monitor B stalling during the monitor A's acquire': judged from the monitor B instance's kernel presents counter (m6xstat 2 0): if the monitor B presented >= 2/s over the 30 s before the monitor A's acquire and < 25% of that over the 12 s after -> abort the MONA.
#   7. The DP's own inst_geom_mismatch counter (the tentative-DP-plan size gate of R-B1) is not logged anywhere; only the two HDMI counters are (TODO-VERB d).
#   8. C9' 'monitor B stays steady' = <= 3 distinct monitor B CRC values and its last 5 equal.
#   9. The 1b kit's GPU-HELD code-4 pattern ('EARLIEST_INUSE2 is not a registered slot') no longer matches the 0.0.662+ CLI wording ('EARLIEST_INUSE is not a registered slot'); this kit uses the new wording. The 1b kit's
#      counters-line parsers (', monitor B' name, inst_geom_mismatch field) were also stale; this kit parses 'scanout (instance N, Name): counters:'.
# TODO-VERB (no CLI verb exists; the kit uses the bundle's log or does without): (a) the DP / monitor A / monitor B slot MCs (bundle 'scanout: slot N: ... MC' / 'scanout (instance N, Name): slot k: ... MC' lines); (b) bundle-side
#   inst_mismatch / drops / gpu_fail (bundle 10 s counters line); (c) table-refresh counters; (d) the DP's inst_geom_mismatch and a kernel-side 'actual in-use reuse' counter; (e) per-acquire free VRAM from the kernel (the kit samples
#   `accel vramstat` after each acquire is DETECTED, so the figure can include a few hundred ms of drift); (f) the bundle's cached table generation (10 s counters line only); (g) the DP console MC before the restart (read from the
#   'a RULE' row of any disp2 crc output = HUBP0's live primary address); (h) a per-instance 'refused by the POOL' counter (read from the bundle log line 'does not cover'); (i) a live `ep.out` (the dtrace file is fetched at the end).
# LOGS: notes/logs/runs/m6-s2/<timestamp>/ (gitignored): run.log, NN-*.txt per command, samples.tsv (fast), minutes.tsv (full), ws-pids.txt, fb-ids.txt, summary.txt, capture-*.txt on a stop.
# Env: DRYRUN=1  STAGE=2a|2b (default 2a)  WATCH_MIN=6  REST_MIN_S=300  STAMP_MIN=20  ACT_MIN=15  PRES_MIN=15  IDS_MAX=4  USER_STEP=0  SKIP_KILL=0  FLIP_S=20  PRE_MAX_S=60  MONA_WAIT_S=90  FAST_SLEEP=6  POOL=auto|small|big
#      WANT_KEXT_RE='0\.0\.66[34]'  WANT_AUX=0.0.7  WANT_BUNDLE=19  WANT_BUNDLE_UUID=''  N48_HOST=navi48  N48_ROOT=...  DRYRUN_OUT=<dir>

set -u
set -o pipefail
ROOT=${N48_ROOT:-/path/to/navi48-checkout.nosync}
HOST=${N48_HOST:-navi48}
DRYRUN=${DRYRUN:-0}
STAGE=${STAGE:-2a}
WATCH_MIN=${WATCH_MIN:-6}
REST_MIN_S=${REST_MIN_S:-300}
STAMP_MIN=${STAMP_MIN:-20}
ACT_MIN=${ACT_MIN:-15}
PRES_MIN=${PRES_MIN:-15}
IDS_MAX=${IDS_MAX:-4}
USER_STEP=${USER_STEP:-0}
SKIP_KILL=${SKIP_KILL:-0}
FLIP_S=${FLIP_S:-20}
PRE_MAX_S=${PRE_MAX_S:-60}
MONA_WAIT_S=${MONA_WAIT_S:-90}
FAST_SLEEP=${FAST_SLEEP:-6}
POOL=${POOL:-auto}
case "$STAGE" in 2a|2b) ;; *) echo "STAGE must be 2a or 2b (got '$STAGE')"; exit 2 ;; esac
SSHO="-o BatchMode=yes -o ConnectTimeout=8"
TS=$(date '+%Y%m%d-%H%M%S')
RUN=$ROOT/notes/logs/runs/m6-s2/$TS
if [ "$DRYRUN" = 1 ]; then
  WATCH_MIN=2
  if [ -n "${DRYRUN_OUT:-}" ]; then RUN=$DRYRUN_OUT; else RUN=$(mktemp -d "${TMPDIR:-/tmp}/m6-s2-dry.XXXXXX") || { echo "cannot make the dry-run dir"; exit 1; }; fi
fi
LOG=$RUN/run.log
mkdir -p "$RUN" || { echo "cannot create $RUN"; exit 1; }
T='~/navi48-staging/navi48test'
NUB='~/n48-metal/n48nub'
KILL1=/private/tmp/n48m-noflip1
KILL2=/private/tmp/n48m-noflip2
CRC_RG2=0x78097625; CRC_B2=0x4bd14c15          # the monitor B's CRC_A: constant across boots (queue 285 / Run B / 1b)
CRC_RG1_INFO=0xe7a0b345; CRC_B1_INFO=0xb196d20f  # the monitor A's CRC_A of Run A / Run B: INFORMATION only (the kit measures CRC_A1 on this boot)
DP_H_TOTAL_OK=0xa9f; DP_H_TOTAL_FALLBACK=0x897
WANT_KEXT_RE=${WANT_KEXT_RE:-0\\.0\\.66[34]}; WANT_AUX=${WANT_AUX:-0.0.7}; WANT_BUNDLE=${WANT_BUNDLE:-19}; WANT_BUNDLE_UUID=${WANT_BUNDLE_UUID:-}
AUX_DIR=/Library/Extensions/Navi48Accel-$WANT_AUX.kext
BUNDLE_BIN=/Library/GPUBundles/Navi48Metal.bundle/Contents/MacOS/Navi48Metal
B1_TOKEN='bpr %lu, %s; plan'          # only bundle >= 17 carries this copy-gate log text (R-B1)
LEN1=8323072; LEN2=14745600; LENDP=14745600   # allocation sizes: monitor A A/B/slot, monitor B A/B/slot, DP console/slot
N=0
TAB=$'\t'

say() { printf '%s\n' "$*" | tee -a "$LOG"; }
stamp() { date '+%H:%M:%S'; }
nap() { [ "$DRYRUN" = 1 ] && return 0; sleep "$1"; }
reach() { [ "$DRYRUN" = 1 ] && return 0; ssh $SSHO "$HOST" true 2>/dev/null; }
# px <label> <remote cmd>: output to $RUN/NN-label.txt (+ log); sets PXF (file) and PXOUT; returns the ssh rc. DRYRUN prints the command only.
px() {
  N=$((N+1)); PXF=$(printf '%s/%02d-%s.txt' "$RUN" "$N" "$1"); local c=$2
  say "--- [$N] $(stamp) ssh $HOST: $c"
  if [ "$DRYRUN" = 1 ]; then say "    DRYRUN: not run"; : >"$PXF"; PXOUT=""; return 0; fi
  ssh $SSHO "$HOST" "$c" >"$PXF" 2>&1; local rc=$?
  PXOUT=$(cat "$PXF"); sed 's/^/    | /' "$PXF" | tee -a "$LOG"; say "    rc=$rc"
  return $rc
}
FAILS=""; ABORTING=0; WARN=""
# stop <exit code> <reason>: capture + park + summary, never continues. (Before the restart / for user-login / unreachable: no kill file is touched.)
stop() {
  local code=$1; shift
  say; say "### STOP at $(stamp): $*"
  FAILS="$FAILS
  STOP: $*"
  if [ "$code" = 3 ]; then
    say "### PC needs a manual reset. Last command: see $LOG ; kext /$WANT_KEXT_RE/, aux $WANT_AUX, bundle $WANT_BUNDLE, stage $STAGE"
  else
    capture
  fi
  summary_and_exit "$code"
}
# refuse <reason>: a precheck refusal BEFORE anything was written (exit 2).
refuse() {
  say; say "### REFUSED at $(stamp): $*"; FAILS="$FAILS
  REFUSED: $*"; say "### nothing was written to the PC; nothing to park."; summary_and_exit 2
}
# need <description> <test command...>: a hard requirement (not evaluated in a dry run).
need() {
  local d=$1; shift
  if [ "$DRYRUN" = 1 ]; then say "  DRYRUN would require: $d"; return 0; fi
  if "$@"; then say "  ok: $d"; else stop 1 "requirement failed: $d"; fi
}
has() { printf '%s' "$PXOUT" | /usr/bin/grep -qE -- "$1"; }
sect() { awk -v n="@@$1" '$0==n{f=1;next} /^@@/{f=0} f' "$2"; }   # text of a @@NAME section of a snapshot file
addr() { printf '%x' $(( (0x${1:-0} << 32) | 0x${2:-0} )); }       # HI LO (hex digits) -> the 64-bit address as lowercase hex without 0x
hexn() { printf '%x' $((0x${1#0x})) 2>/dev/null; }                 # normalise a hex string (no leading zeros)
gv() { eval "printf '%s' \"\${$1-}\""; }                           # value of the variable named $1
sv() { printf -v "$1" '%s' "$2"; }                                 # set the variable named $1
# per-instance pin tables (instance 1 = monitor A, 2 = monitor B)
exp_pitch() { [ "$1" = 1 ] && echo 77f || echo 9ff; }
exp_vtg()   { [ "$1" = 1 ] && echo 1 || echo 2; }
exp_view()  { [ "$1" = 1 ] && echo 4380780 || echo 5a00a00; }
inst_len()  { [ "$1" = 1 ] && echo $LEN1 || echo $LEN2; }
inst_name() { [ "$1" = 1 ] && echo monitor A || echo monitor B; }
kill_of()   { [ "$1" = 1 ] && echo $KILL1 || echo $KILL2; }

collect_ep() {  # S4: fetch the ep.d output (never fatal)
  [ "$DRYRUN" = 1 ] && { say "--- DRYRUN: ssh $HOST 'cat /tmp/ep.out /tmp/ep.err' > $RUN/ep.out"; return 0; }
  ssh $SSHO "$HOST" 'echo "== /tmp/ep.out"; cat /tmp/ep.out 2>&1; echo "== /tmp/ep.err"; cat /tmp/ep.err 2>&1; echo "== dtrace running: $(pgrep -x dtrace | tr "\n" " ")"' >"$RUN/ep.out" 2>&1 && say "  ep.d output fetched: $(grep -c '^ep ' "$RUN/ep.out") 'ep' lines -> $RUN/ep.out"
}
quick_capture() {  # ONE ssh, the reads that matter most (the ABORT path takes them BEFORE a kill file is touched)
  say "### quick capture (one ssh; read-only)"
  [ "$DRYRUN" = 1 ] && { say "--- DRYRUN: ssh $HOST 'm6xstat 1|2 0|1|2; disp2 crc 1|2; disp2 status 1|2; sessstat 4; vramstat' > $RUN/capture-quick.txt"; return 0; }
  ssh $SSHO "$HOST" "date +%s; for k in 'm6xstat 1 all' 'm6xstat 2 all' 'disp2 crc 1' 'disp2 crc 2' 'disp2 status 1' 'disp2 status 2' 'sessstat 4' 'vramstat'; do echo \"== \$k\"; sudo -n $T accel \$k 2>&1; done; echo \"WS pid: \$(pgrep -x WindowServer)\"" >"$RUN/capture-quick.txt" 2>&1
}
capture() {  # best effort, each command separately, bounded
  say "### capturing state (the PC stays parked; read-only commands only)"
  reach || { say "### PC unreachable while capturing: PC needs a manual reset (one more check done)"; return; }
  local k
  for k in "m6stat 0" "m6stat 1" "m6stat 2" "m6stat 3" "m6stat 4" "m6xstat 1 all" "m6xstat 2 all" "vramstat" "pipestat 0" "pipestat 1" "pipestat 2" "pipestat 3" "pipestat 4" "disp2 status 1" "disp2 status 2" "disp2 crc 1" "disp2 crc 2" "sessstat 4" "status"; do
    N=$((N+1)); ssh $SSHO "$HOST" "sudo -n $T accel $k 2>&1" >"$RUN/capture-$(printf '%s' "$k" | tr ' ' '-').txt" 2>&1
  done
  ssh $SSHO "$HOST" "sudo -n $T log 2>&1 | tail -600" >"$RUN/capture-driverlog.txt" 2>&1
  ssh $SSHO "$HOST" 'echo "up: $(uptime)"; echo "WS pid: $(pgrep -x WindowServer)"; echo "console: $(stat -f %Su /dev/console)"; who; ls -lt /Library/Logs/DiagnosticReports | head -8; find /Library/Logs/DiagnosticReports -newer /tmp/n48-s2-marker 2>/dev/null; ioreg -c IOFramebuffer -r -d1 -w0 | grep -E "^\+-o "; kmutil showloaded --list-only 2>/dev/null | grep -iE "navi48|rdna4|accelprobe"; ls -l /private/tmp/n48m-noflip1 /private/tmp/n48m-noflip2 2>&1; tail -20 /var/tmp/n48watch.log 2>/dev/null' >"$RUN/capture-system.txt" 2>&1
  ssh $SSHO "$HOST" 'sudo -n log show --info --last 20m --predicate "process == \"WindowServer\" OR eventMessage CONTAINS \"Navi48Metal\" OR eventMessage CONTAINS \"n48accel\" OR eventMessage CONTAINS \"scanout\"" --style compact 2>/dev/null | tail -500' >"$RUN/capture-logshow.txt" 2>&1
  collect_ep
  say "### capture files: $RUN/capture-*.txt"
}

# ---------------------------------------------------------------------------------------------- criteria state
R0=PENDING; C1p=PENDING; C2p=PENDING; C3p=PENDING; C4p=PENDING; C5p=PENDING; C6p=PENDING; C7p=PENDING; C9p=PENDING; C10=PENDING; K1=PENDING; K2=PENDING; K3=PENDING; K5=PENDING; K7=PENDING; S1=PENDING; POOLV=PENDING
R0w=""; C1pw=""; C2pw=""; C3pw=""; C4pw=""; C5pw=""; C6pw=""; C7pw=""; C9pw=""; C10w=""; K1w=""; K2w=""; K3w=""; K5w=""; K7w=""; S1w=""; POOLVw=""
summary_and_exit() {
  local code=$1 S=$RUN/summary.txt
  { echo "M6 Stage $STAGE run $TS  (dry-run=$DRYRUN, watch ${WATCH_MIN} min, USER_STEP=$USER_STEP, SKIP_KILL=$SKIP_KILL, pool=${POOLCLASS:-?})  exit code $code"
    echo "R0  K1-K7 re-confirmed before the first flip            : $R0 $R0w"
    echo "C1' each enabled HDMI instance acquired (3 slots, disjoint) / clean pool refusal : $C1p $C1pw"
    echo "POOL pool class vs outcome (SMALL: >=1 acquires; BIG: all)  : $POOLV $POOLVw"
    echo "C2' CRC leaves CRC_A <= ${FLIP_S} s; 2a: the monitor A stays on A      : $C2p $C2pw"
    echo "C3' at rest: equal CRCs, 0 restores, >= ${REST_MIN_S} s (static = CRC_A) : $C3p $C3pw"
    echo "C4' underflow / ODM bit 10 / VM fault / HUNG all 0           : $C4p $C4pw"
    echo "C5' reuse_inuse_refused / inst_mismatch / inst_geom_mismatch  : $C5p $C5pw"
    echo "C6' DP unchanged, floors per display, monitor B flips through the monitor A acquire : $C6p $C6pw"
    echo "C7' swap detector: kill legs (noflip1 then noflip2)           : $C7p $C7pw"
    echo "C9' mouse on the monitor A: monitor A CRC changes, monitor B steady (optional): $C9p $C9pw"
    echo "C10 WS pid stable, 0 HUNG, 0 new crash reports                : $C10 $C10w"
    echo "S1  monitor B Navi48Framebuffer registered first (lower entry ID)  : $S1 $S1w"
    echo "(final proofs: K1 $K1 $K1w | K2 $K2 $K2w | K3 $K3 $K3w | K5 $K5 $K5w | K7 $K7 $K7w)"
    [ -n "$WARN" ] && printf 'WARNINGS:%s\n' "$WARN"
    [ -n "$FAILS" ] && printf 'FAILURES / STOPS:%s\n' "$FAILS"
    echo "Logs: $RUN"; } | tee -a "$LOG" >"$S"
  exit "$code"
}

# ---------------------------------------------------------------------------------------------- PARSERS
# Each takes a FILE (the verb's raw output) and, for the per-instance ones, the INSTANCE (1 = monitor A, 2 = monitor B); it RESETS its own field group first, so an absent field reads EMPTY (callers treat empty as a failure).
# The working fields are the globals X_* / CRC_*; `stash <inst> <group>...` copies a group into P<inst>_*, `use <inst>` loads every group of an instance back into the working globals.
V0="X_LATCH X_OFFLINE X_ACQ X_RESTORING X_WANT X_RFAILED X_GEN X_ACQUIRES X_A X_B X_PRES X_LATCHED X_REPL X_REFUSED X_POLL X_REUSE X_WDOG X_RESTORES X_RESFAIL X_ACQREF X_LASTREASON X_IDLE X_TGEN X_GPUHELD X_GH4 X_LASTADDR"
V1="X_S0 X_S1 X_S2 X_SP0 X_SP1 X_SP2 X_WREF X_WFAIL X_UNTAG X_GEOM X_FRONT X_PEND"
V2="X_UF X_BLANK X_VTG X_VMF X_ODM10 X_OTGFC X_FCR X_FP X_PROG X_EARLY X_PITCH X_SCFG X_SCTL X_VMID X_VIEW X_STAGE X_CUR X_PIN X_PA2 X_PB2"
VC="CRC_RG_NOW CRC_B_NOW UF_LINE ODM_OCC VMF UF_OK DPW_OK FIFO_ERR MONBW CRC_PA CRC_PB DPRULE_HI DPRULE_LO"
clr() { local v; for v in "$@"; do eval "$v="; done; }
stash() { local i=$1 g v; shift; for g in "$@"; do for v in $g; do eval "P${i}_$v=\${$v-}"; done; done; }
use() { local i=$1 v; for v in $V0 $V1 $V2 $VC; do eval "$v=\${P${i}_$v-}"; done; }

# m6xstat <inst> 0 -> X_LATCH ("navi48-m6 ON, navi48-m6flip ON[, navi48-m6flip1 ON]" verbatim), X_OFFLINE (1 = the CLI says the instance is OFF: nothing was read), X_ACQ X_RESTORING X_WANT X_RFAILED (YES|no), X_GEN X_ACQUIRES,
#   X_A X_B (hex, no 0x), X_PRES X_LATCHED X_REPL X_REFUSED X_POLL X_REUSE, X_WDOG X_RESTORES X_RESFAIL X_ACQREF X_LASTREASON X_IDLE X_TGEN, X_GPUHELD (slot 0x1k / 0x2k or ""), X_GH4, X_LASTADDR
parse_x0() {
  local t i=$2; t=$(cat "$1"); clr $V0
  if [ "$i" = 1 ]; then
    X_LATCH=$(printf '%s\n' "$t" | sed -n 's/^accel m6xstat inst1 page [0-9]: \(navi48-m6 [A-Z]*, navi48-m6flip [A-Z]*, navi48-m6flip1 [A-Z]*\).*/\1/p' | head -1)
  else
    X_LATCH=$(printf '%s\n' "$t" | sed -n 's/^accel m6xstat [0-9]: \(navi48-m6 [A-Z]*, navi48-m6flip [A-Z]*\).*/\1/p' | head -1)
  fi
  X_OFFLINE=0; printf '%s\n' "$t" | /usr/bin/grep -q "(instance $i is OFF: nothing was read)" && X_OFFLINE=1
  X_ACQ=$(printf '%s\n' "$t" | sed -n "s/^  instance $i: acquired \([A-Za-z]*\),.*/\1/p" | head -1)
  X_RESTORING=$(printf '%s\n' "$t" | sed -n "s/^  instance $i: .*restoring \([A-Za-z]*\),.*/\1/p" | head -1)
  X_WANT=$(printf '%s\n' "$t" | sed -n "s/^  instance $i: .*wantRestore \([A-Za-z]*\),.*/\1/p" | head -1)
  X_RFAILED=$(printf '%s\n' "$t" | sed -n "s/^  instance $i: .*restoreFailed \([A-Za-z]*\),.*/\1/p" | head -1)
  X_GEN=$(printf '%s\n' "$t" | sed -n "s/^  instance $i: .*; generation \([0-9]*\), acquires \([0-9]*\).*/\1/p" | head -1)
  X_ACQUIRES=$(printf '%s\n' "$t" | sed -n "s/^  instance $i: .*; generation \([0-9]*\), acquires \([0-9]*\).*/\2/p" | head -1)
  X_A=$(printf '%s\n' "$t" | sed -n 's/^  A (console) 0x\([0-9a-f]*\)  B 0x\([0-9a-f]*\).*/\1/p' | head -1)
  X_B=$(printf '%s\n' "$t" | sed -n 's/^  A (console) 0x\([0-9a-f]*\)  B 0x\([0-9a-f]*\).*/\2/p' | head -1)
  local l; l=$(printf '%s\n' "$t" | sed -n 's/^  presents \([0-9]*\), latched \([0-9]*\), replaced \([0-9]*\), refused \([0-9]*\), latched-by-poll \([0-9]*\), REUSE_INUSE_REFUSED \([0-9]*\).*/\1 \2 \3 \4 \5 \6/p' | head -1)
  read -r X_PRES X_LATCHED X_REPL X_REFUSED X_POLL X_REUSE <<<"$l"
  l=$(printf '%s\n' "$t" | sed -n 's/^  watchdog restores \([0-9]*\), restores \([0-9]*\) (failed \([0-9]*\)); Acquire2\{0,1\} refused \([0-9]*\) (last reason \([0-9]*\):.*); idle \([0-9]*\) ms; M6 table gen \([0-9]*\).*/\1 \2 \3 \4 \5 \6 \7/p' | head -1)
  read -r X_WDOG X_RESTORES X_RESFAIL X_ACQREF X_LASTREASON X_IDLE X_TGEN <<<"$l"
  X_GPUHELD=$(printf '%s\n' "$t" | sed -n 's/^  \.\.\. slot \(0x[0-9a-f]*\).*/\1/p' | head -1)
  X_GH4=0; printf '%s\n' "$t" | /usr/bin/grep -q 'GPU-HELD: EARLIEST_INUSE is not a registered slot' && X_GH4=1
  X_LASTADDR=$(printf '%s\n' "$t" | sed -n 's/^  last restore [0-9]* us; last address written 0x\([0-9a-f]*\).*/\1/p' | head -1)
}
# m6xstat <inst> 1 -> X_S0 X_S1 X_S2 (slot MCs, hex no 0x), X_SP0..2 (presents), X_WREF X_WFAIL X_UNTAG X_GEOM, X_FRONT X_PEND (slot ids, hex no 0x; ffffffff = none)
parse_x1() {
  local t i=$2 k v th; t=$(cat "$1"); clr $V1
  th=$i
  for k in 0 1 2; do
    v=$(printf '%s\n' "$t" | sed -n "s/^  slot 0x$th$k: MC 0x\([0-9a-f]*\), presents \([0-9]*\), latches \([0-9]*\).*/\1 \2/p" | head -1)
    eval "X_S$k=\${v%% *}; X_SP$k=\${v#* }"; [ -n "$v" ] || eval "X_S$k=''; X_SP$k=''"
  done
  local l; l=$(printf '%s\n' "$t" | sed -n 's/^  writer refusals \([0-9]*\), write failures \([0-9]*\); untagged presents refused \([0-9]*\); geometry refusals \([0-9]*\).*/\1 \2 \3 \4/p' | head -1)
  read -r X_WREF X_WFAIL X_UNTAG X_GEOM <<<"$l"
  l=$(printf '%s\n' "$t" | sed -n 's/.*front slot 0x\([0-9a-f]*\), pending slot 0x\([0-9a-f]*\) (0xffffffff = none); last address written.*/\1 \2/p' | head -1)
  read -r X_FRONT X_PEND <<<"$l"
}
# m6xstat <inst> 2 -> X_UF (underflow) X_BLANK X_VTG X_VMF (hex) X_ODM10 X_OTGFC, X_FCR X_FP X_VIEW X_PROG X_EARLY (hex addresses, no 0x) X_PITCH X_SCFG X_SCTL X_VMID, X_STAGE X_CUR X_PIN X_PA2 X_PB2
parse_x2() {
  local t l i=$2; t=$(cat "$1"); clr $V2
  l=$(printf '%s\n' "$t" | sed -n "s/^  HUBP${i}_DCHUBP_CNTL 0x[0-9a-f]* (underflow \([0-9]*\), BLANK_EN \([0-9]*\), VTG_SEL \([0-9]*\)); DCN_VM_FAULT_STATUS 0x\([0-9a-f]*\); ODM${i} OPTC_INPUT_GLOBAL_CONTROL 0x[0-9a-f]* (bit 10 \([0-9]*\)); OTG${i} frame counter \([0-9]*\).*/\1 \2 \3 \4 \5 \6/p" | head -1)
  read -r X_UF X_BLANK X_VTG X_VMF X_ODM10 X_OTGFC <<<"$l"
  l=$(printf '%s\n' "$t" | sed -n "s/^  FLIP_CONTROL 0x[0-9a-f]* (FLIP_PENDING \([0-9]*\)); viewport 0x\([0-9a-f]*\); programmed 0x\([0-9a-f]*\):0x\([0-9a-f]*\); EARLIEST_INUSE${i} 0x\([0-9a-f]*\):0x\([0-9a-f]*\);.*/\1 \2 \3 \4 \5 \6/p" | head -1)
  local fp vw ph pl eh el; read -r fp vw ph pl eh el <<<"$l"
  X_FCR=$(printf '%s\n' "$t" | sed -n 's/^  FLIP_CONTROL 0x\([0-9a-f]*\) (FLIP_PENDING.*/\1/p' | head -1)
  l=$(printf '%s\n' "$t" | sed -n "s/.*; pitch 0x\([0-9a-f]*\); SURFACE_CONFIG 0x\([0-9a-f]*\); SURFACE_CONTROL 0x\([0-9a-f]*\); VMID_SETTINGS_0 0x\([0-9a-f]*\); OTG${i}_CONTROL.*/\1 \2 \3 \4/p" | head -1)
  read -r X_PITCH X_SCFG X_SCTL X_VMID <<<"$l"
  X_FP=$fp; X_VIEW=$(hexn "${vw:-}"); X_PROG=""; X_EARLY=""
  [ -n "$ph" ] && X_PROG=$(addr "$ph" "$pl"); [ -n "$eh" ] && X_EARLY=$(addr "$eh" "$el")
  l=$(printf '%s\n' "$t" | sed -n "s/^  disp2 plane state of instance $i: stage \([0-9]*\) (6 = HELD), cur \([0-9]*\) (0 = A), pair pinned \([0-9]*\); A 0x\([0-9a-f]*\) B 0x\([0-9a-f]*\).*/\1 \2 \3 \4 \5/p" | head -1)
  read -r X_STAGE X_CUR X_PIN X_PA2 X_PB2 <<<"$l"
}
# disp2 crc <inst> -> CRC_RG_NOW CRC_B_NOW, UF_LINE ODM_OCC VMF (hex), UF_OK, DPW_OK, FIFO_ERR (DIGn_FIFO_CTRL0 error 29:28), MONBW (the monitor A's monitor B-watch change mask, "" for the monitor B's op), CRC_PA CRC_PB (A / B from the EARLIEST line), DPRULE_HI/LO (HUBP0's live primary after the op)
parse_crc() {
  local t i=$2; t=$(cat "$1"); clr $VC
  CRC_RG_NOW=$(printf '%s' "$t" | sed -n "s/.*OTG${i} CRC *: RG \(0x[0-9a-f]*\) *B \(0x[0-9a-f]*\).*/\1/p" | head -1)
  CRC_B_NOW=$(printf '%s' "$t" | sed -n "s/.*OTG${i} CRC *: RG \(0x[0-9a-f]*\) *B \(0x[0-9a-f]*\).*/\2/p" | head -1)
  UF_LINE=$(printf '%s' "$t" | /usr/bin/grep "HUBP${i}_DCHUBP_CNTL" | head -1 | sed -n 's/.*\(underflow(30:28) [0-9]*  *SEG_ALLOC_ERR [0-9]*  *TIMEOUT(23:20) [0-9a-fx]*\).*/\1/p')
  ODM_OCC=$(printf '%s' "$t" | sed -n 's/.*underflow occurred(10) \([0-9]\).*/\1/p' | head -1)
  VMF=$(printf '%s' "$t" | sed -n 's/.*DCN_VM_FAULT_STATUS *: *\([0-9a-fx]*\).*/\1/p' | head -1)
  FIFO_ERR=$(printf '%s' "$t" | sed -n 's/.*DIG[0-9]_FIFO_CTRL0 0x[0-9a-f]* (error 29:28 \([0-9]*\)).*/\1/p' | head -1)
  MONBW=$(printf '%s' "$t" | sed -n 's/.*monitor B watch (kext) *: .*(changed mask \(0[x0-9a-f]*\)).*/\1/p' | head -1)
  CRC_PA=$(printf '%s' "$t" | sed -n 's/.*(A 0x\([0-9a-f]*\), B 0x\([0-9a-f]*\)).*/\1/p' | head -1)
  CRC_PB=$(printf '%s' "$t" | sed -n 's/.*(A 0x\([0-9a-f]*\), B 0x\([0-9a-f]*\)).*/\2/p' | head -1)
  local rl; rl=$(printf '%s' "$t" | sed -n 's/^  DP .*: 0x\([0-9a-f]*\):\(0x\)\{0,1\}\([0-9a-f]*\) -> 0x\([0-9a-f]*\):\(0x\)\{0,1\}\([0-9a-f]*\)  (a RULE.*/\4 \6/p' | head -1)
  read -r DPRULE_HI DPRULE_LO <<<"$rl"
  # the CLI's own abort text for an monitor A op that tripped the MONB watch also contains 'DP DISTURBED' (it is the generic abort line): only the DP-watch report's own markers count here
  DPW_OK=0; printf '%s' "$t" | /usr/bin/grep -q 'DP plane watch .*all 15 registers unchanged' && ! printf '%s' "$t" | /usr/bin/grep -qE 'kext DP change mask|kext DP-plane mask|DP plane registers changed|\*\*\* CHANGED \*\*\*|\*\*\* DP DISTURBED \(' && DPW_OK=1
  UF_OK=0
  [ "$UF_LINE" = "underflow(30:28) 0  SEG_ALLOC_ERR 0  TIMEOUT(23:20) 0" ] && [ "$ODM_OCC" = 0 ] && { [ "$VMF" = 0000000000 ] || [ "$VMF" = 0x0 ] || [ "$VMF" = 0 ]; } && UF_OK=1
}
crc_is_a() { [ -n "$(gv CRC_RG_NOW)" ] && [ "$(gv CRC_RG_NOW)" = "$(gv CRC_A_RG_$1)" ] && [ "$(gv CRC_B_NOW)" = "$(gv CRC_A_B_$1)" ]; }
# sessstat 4 -> HUNG (0|1; empty = unparsable). SUSPECTED page number: "open / DEAD slots / HUNG: a / b / c" (navi48test.c).
parse_hung() { HUNG=$(sed -n 's/^  open \/ DEAD slots \/ HUNG: [0-9]* \/ [0-9]* \/ \([0-9]*\).*/\1/p' "$1" | head -1); }
# m6stat 0/1/2 (+4) -> M_TXN0..2 M_IDS0..2 M_AMB M_NOFB M_PIPES; M_AGN M_NONDP M_LASTEP M_STAMP0..2 M_REF0..2; M_AMB2; page 4: M_N921_0..2 M_N711_0..2 M_EPMASK M_NFB4 M_EPOUT M_LASTEP4
parse_m6() { # parse_m6 <m6stat0 file> <m6stat1 file> <m6stat2 file> [<m6stat4 file>]
  local k
  for k in 0 1 2; do
    sv M_TXN$k "$(sed -n "s/^ *instance $k .*: \([0-9][0-9]*\) transactions, \([0-9][0-9]*\) surface IDs seen.*/\1/p" "$1" | head -1)"
    sv M_IDS$k "$(sed -n "s/^ *instance $k .*: \([0-9][0-9]*\) transactions, \([0-9][0-9]*\) surface IDs seen.*/\2/p" "$1" | head -1)"
    sv M_STAMP$k "$(sed -n "s/^ *instance $k .*: \([0-9][0-9]*\) vblank stamps written, \([0-9][0-9]*\) refused.*/\1/p" "$2" | head -1)"
    sv M_REF$k "$(sed -n "s/^ *instance $k .*: \([0-9][0-9]*\) vblank stamps written, \([0-9][0-9]*\) refused.*/\2/p" "$2" | head -1)"
  done
  M_AMB=$(sed -n 's/^ *ambiguous IDs (seen on two pipes) *: \([0-9]*\).*/\1/p' "$1" | head -1)
  M_NOFB=$(sed -n 's/^ *submits on a pipe with no known framebuffer: \([0-9]*\).*/\1/p' "$1" | head -1)
  M_PIPES=$(sed -n 's/.* \([0-9]*\) pipes recorded.*/\1/p' "$1" | head -1)
  M_AGN=$(sed -n 's/.*AGDC: \([0-9]*\) framebuffers in the last 0x980 reply.*/\1/p' "$2" | head -1)
  M_NONDP=$(sed -n 's/.*0x921 answered for a non-DP endpoint \([0-9]*\) times.*/\1/p' "$2" | head -1)
  M_LASTEP=$(sed -n 's/.*last endpoint dword \([0-9]*\).*/\1/p' "$2" | head -1)
  M_AMB2=$(/usr/bin/grep -c 'AMBIGUOUS' "$3")
  if [ -n "${4:-}" ]; then
    for k in 0 1 2; do
      sv M_N921_$k "$(sed -n "s/^ *instance $k .*: AGDC 0x921 answered \([0-9]*\) times, 0x711 \([0-9]*\) times.*/\1/p" "$4" | head -1)"
      sv M_N711_$k "$(sed -n "s/^ *instance $k .*: AGDC 0x921 answered \([0-9]*\) times, 0x711 \([0-9]*\) times.*/\2/p" "$4" | head -1)"
    done
    M_EPMASK=$(sed -n 's/.*endpoint dwords seen (bitmask 0x\([0-9a-f]*\):.*/\1/p' "$4" | head -1)
    M_NFB4=$(sed -n 's/.*nfb of the last 0x980 reply \([0-9]*\); endpoints outside the list \([0-9]*\) (last endpoint dword \([0-9]*\)).*/\1/p' "$4" | head -1)
    M_EPOUT=$(sed -n 's/.*nfb of the last 0x980 reply \([0-9]*\); endpoints outside the list \([0-9]*\) (last endpoint dword \([0-9]*\)).*/\2/p' "$4" | head -1)
    M_LASTEP4=$(sed -n 's/.*nfb of the last 0x980 reply \([0-9]*\); endpoints outside the list \([0-9]*\) (last endpoint dword \([0-9]*\)).*/\3/p' "$4" | head -1)
  fi
}
# vramstat -> V_MAPPED V_TOTAL V_FREE (MiB) V_FREEB (bytes); empty when the allocator is not up
parse_vram() {
  V_MAPPED=$(sed -n 's/^accel vramstat: BAR0 phys 0x[0-9a-f]*, mapped \([0-9]*\) MiB .*/\1/p' "$1" | head -1)
  V_TOTAL=$(sed -n 's/^  visible pool: [0-9]* bytes total (\([0-9]*\) MiB), \([0-9]*\) bytes free (\([0-9]*\) MiB).*/\1/p' "$1" | head -1)
  V_FREEB=$(sed -n 's/^  visible pool: [0-9]* bytes total (\([0-9]*\) MiB), \([0-9]*\) bytes free (\([0-9]*\) MiB).*/\2/p' "$1" | head -1)
  V_FREE=$(sed -n 's/^  visible pool: [0-9]* bytes total (\([0-9]*\) MiB), \([0-9]*\) bytes free (\([0-9]*\) MiB).*/\3/p' "$1" | head -1)
}
# the bundle's lines: DP presents/s from the latest "m6:" line vs the newest line >= 50 s earlier ("RATE unknown_delta secs" or "none")
bundle_rate() { # bundle_rate <file with the m6: lines>
  awk '{ split($2,a,":"); T[NR]=a[1]*3600+a[2]*60+a[3]; v=$0; sub(/.*presented to instance 0 /,"",v); V[NR]=v+0; u=$0; sub(/.*unknown /,"",u); U[NR]=u+0 }
    END { for (k=NR-1; k>=1; k--) if (T[NR]-T[k] >= 50) { printf "%d %d %d\n", (V[NR]-V[k])/(T[NR]-T[k]), U[NR]-U[k], T[NR]-T[k]; exit } print "none" }' "$1"
}
# "scanout (instance N, Name): counters: ..." (the LAST such line of the file) -> B<N>_PRES B<N>_WDOG B<N>_DROPS B<N>_GPUFAIL B<N>_MISMATCH B<N>_GEOMMIS B<N>_CACHED B<N>_PLAN (empty when no line)
parse_bundlec() { # parse_bundlec <file> <inst>
  local i=$2 l; l=$(/usr/bin/grep "scanout (instance $i, [A-Za-z]*): counters" "$1" | tail -1)
  sv B${i}_PRES "$(printf '%s' "$l" | sed -n 's/.*counters: presents \([0-9]*\) (kernel presents.*/\1/p')"
  sv B${i}_WDOG "$(printf '%s' "$l" | sed -n 's/.*watchdog_restores \([0-9]*\)).*/\1/p')"
  sv B${i}_DROPS "$(printf '%s' "$l" | sed -n 's/.* drops \([0-9]*\) gpu_fail.*/\1/p')"
  sv B${i}_GPUFAIL "$(printf '%s' "$l" | sed -n 's/.* gpu_fail \([0-9]*\) stale_skipped.*/\1/p')"
  sv B${i}_MISMATCH "$(printf '%s' "$l" | sed -n 's/.*; planned [0-9]* inst_mismatch \([0-9]*\) inst_geom_mismatch \([0-9]*\) stale_retry.*/\1/p')"
  sv B${i}_GEOMMIS "$(printf '%s' "$l" | sed -n 's/.*; planned [0-9]* inst_mismatch \([0-9]*\) inst_geom_mismatch \([0-9]*\) stale_retry.*/\2/p')"
  sv B${i}_PLAN "$(printf '%s' "$l" | sed -n 's/.*; planned \([0-9]*\) inst_mismatch.*/\1/p')"
  sv B${i}_CACHED "$(printf '%s' "$l" | sed -n 's/.*table gen [0-9]* (cached \([0-9]*\),.*/\1/p')"
  sv B${i}_TOD "$(printf '%s' "$l" | awk '{split($2,a,":"); if (NF>1) print int(a[1]*3600+a[2]*60+a[3])}')"
}
# the bundle's acquire / pool / init lines (any window) -> for inst I: BL<I>_INIT (ENABLED|OFF|""), BL<I>_KILLFILE (PRESENT|absent|""), BL<I>_ACQUIRED (count), BL<I>_FREE / BL<I>_NEED (MiB, the LAST pool line), BL<I>_POOLREF (count of 'does not cover'),
#   BL<I>_POOLOK (count of 'covers'), BL<I>_FAILCLOSED (count), BL<I>_GEOMWORD (count of the plane-geometry-word refusal), BL<I>_KILLSW (count of 'kill switch ... exists'), BL<I>_SLOTS (space separated MCs, no 0x)
parse_bundlelog() { # parse_bundlelog <file> <inst>
  local f=$1 i=$2 l
  l=$(/usr/bin/grep "scanout: instance $i (the [A-Za-z]*):" "$f" | tail -1)
  sv BL${i}_INIT "$(printf '%s' "$l" | sed -nE 's/.*scanout: instance [12] \(the [A-Za-z]*\): (ENABLED|OFF).*/\1/p')"
  sv BL${i}_KILLFILE "$(printf '%s' "$l" | sed -nE 's/.*kill file (PRESENT|absent);.*/\1/p')"
  sv BL${i}_ACQUIRED "$(/usr/bin/grep -c "scanout (instance $i, [A-Za-z]*) ACQUIRED" "$f")"
  l=$(/usr/bin/grep "scanout (instance $i, [A-Za-z]*): free visible VRAM [0-9]* MiB covers" "$f" | tail -1)
  sv BL${i}_POOLOK "$(/usr/bin/grep -c "scanout (instance $i, [A-Za-z]*): free visible VRAM [0-9]* MiB covers" "$f")"
  if [ -n "$l" ]; then
    sv BL${i}_FREE "$(printf '%s' "$l" | sed -n 's/.*free visible VRAM \([0-9]*\) MiB covers.*(need \([0-9]*\) MiB).*/\1/p')"
    sv BL${i}_NEED "$(printf '%s' "$l" | sed -n 's/.*free visible VRAM \([0-9]*\) MiB covers.*(need \([0-9]*\) MiB).*/\2/p')"
  fi
  sv BL${i}_POOLREF "$(/usr/bin/grep -c "scanout (instance $i, [A-Za-z]*): the free visible VRAM (" "$f")"
  l=$(/usr/bin/grep "scanout (instance $i, [A-Za-z]*): the free visible VRAM (" "$f" | tail -1)
  if [ -n "$l" ]; then
    sv BL${i}_FREE "$(printf '%s' "$l" | sed -n 's/.*the free visible VRAM (\([0-9]*\) MiB, the kernel.*(need \([0-9]*\) MiB: verdict.*/\1/p')"
    sv BL${i}_NEED "$(printf '%s' "$l" | sed -n 's/.*the free visible VRAM (\([0-9]*\) MiB, the kernel.*(need \([0-9]*\) MiB: verdict.*/\2/p')"
  fi
  sv BL${i}_FAILCLOSED "$(/usr/bin/grep -c "scanout (instance $i, [A-Za-z]*): FAIL CLOSED" "$f")"
  sv BL${i}_GEOMWORD "$(/usr/bin/grep -c "scanout (instance $i, [A-Za-z]*): the kernel's plane geometry word" "$f")"
  sv BL${i}_KILLSW "$(/usr/bin/grep -c "scanout (instance $i, [A-Za-z]*): kill switch" "$f")"
  sv BL${i}_SLOTS "$(/usr/bin/grep "scanout (instance $i, [A-Za-z]*): slot [0-9]*:" "$f" | sed -n 's/.*MC 0x\([0-9a-f]*\) (64 KiB.*/\1/p' | sort -u | tr '\n' ' ')"
}
# DP raster: reg[0x4fea] = 0x00000897 -> RASTER_HEX (no leading zeros, lowercase)
parse_reg() { RASTER_HEX=$(sed -n 's/^reg\[0x4fea\] = 0x\([0-9a-f]*\).*/\1/p' "$1" | head -1); [ -n "$RASTER_HEX" ] && RASTER_HEX=$(hexn "$RASTER_HEX"); }
# the DP side line of disp2 status -> DP_OTG0 DP_BE
parse_dpside() { local l; l=$(sed -n 's/.*DP side: OTG0 \([A-Z]*\).*DIG1_DIG_BE_CNTL \(0x[0-9a-f]*\).*/\1 \2/p' "$1" | head -1); DP_OTG0=${l%% *}; DP_BE=${l#* }; }
# ioreg lines of Navi48Framebuffer nodes -> FB_IDS (space separated 0x ids of REGISTERED nodes, in ioreg order)
parse_fbids() { FB_IDS=$(sed -n 's/^+-o Navi48Framebuffer  *<class Navi48Framebuffer, id \(0x[0-9a-f]*\), registered.*/\1/p' "$1" | tr '\n' ' '); FB_N=$(printf '%s' "$FB_IDS" | wc -w | tr -d ' '); }

# ---------------------------------------------------------------------------------------------- remote snapshot scripts (one ssh each)
# FAST: WS pid, users, PC clock, m6xstat <i> 0 / 2 for BOTH HDMI instances, disp2 crc 2 / 1, HUNG.
fast_remote() {
  cat <<EOS
echo "@@T"; date +%s
echo "@@WS"; pgrep -x WindowServer
echo "@@USERS"; stat -f %Su /dev/console; who
echo "@@X0_2"; sudo -n $T accel m6xstat 2 0 2>&1
echo "@@X2_2"; sudo -n $T accel m6xstat 2 2 2>&1
echo "@@X0_1"; sudo -n $T accel m6xstat 1 0 2>&1
echo "@@X2_1"; sudo -n $T accel m6xstat 1 2 2>&1
echo "@@CRC2"; sudo -n $T accel disp2 crc 2 2>&1
echo "@@CRC1"; sudo -n $T accel disp2 crc 1 2>&1
echo "@@HUNG"; sudo -n $T accel sessstat 4 2>&1
EOS
}
# FULL (K-snapshot): the fast reads + m6xstat 1 pages for both instances + vramstat + m6stat 0|1|2|3|4 + DP stamps 3 s apart for all three OTGs + the bundle's m6:/counters/acquire lines + LEAKED count + new reports.
snap_remote() {
  cat <<EOS
echo "@@T"; date +%s
echo "@@WS"; pgrep -x WindowServer
echo "@@USERS"; stat -f %Su /dev/console; who
echo "@@STAMPA"; sudo -n $T accel m6stat 1 2>&1 | grep '^ *instance [012] '
sleep 3
echo "@@STAMPB"; sudo -n $T accel m6stat 1 2>&1 | grep '^ *instance [012] '
echo "@@VRAM"; sudo -n $T accel vramstat 2>&1
echo "@@X0_2"; sudo -n $T accel m6xstat 2 0 2>&1
echo "@@X1_2"; sudo -n $T accel m6xstat 2 1 2>&1
echo "@@X2_2"; sudo -n $T accel m6xstat 2 2 2>&1
echo "@@X0_1"; sudo -n $T accel m6xstat 1 0 2>&1
echo "@@X1_1"; sudo -n $T accel m6xstat 1 1 2>&1
echo "@@X2_1"; sudo -n $T accel m6xstat 1 2 2>&1
echo "@@CRC2"; sudo -n $T accel disp2 crc 2 2>&1
echo "@@CRC1"; sudo -n $T accel disp2 crc 1 2>&1
echo "@@T2"; date +%s
echo "@@STATUS"; sudo -n $T accel disp2 status 2 2>&1
echo "@@STATUS1"; sudo -n $T accel disp2 status 1 2>&1
echo "@@M6_0"; sudo -n $T accel m6stat 0 2>&1
echo "@@M6_1"; sudo -n $T accel m6stat 1 2>&1
echo "@@M6_2"; sudo -n $T accel m6stat 2 2>&1
echo "@@M6_3"; sudo -n $T accel m6stat 3 2>&1
echo "@@M6_4"; sudo -n $T accel m6stat 4 2>&1
echo "@@HUNG"; sudo -n $T accel sessstat 4 2>&1
echo "@@LEAK"; sudo -n $T log 2>&1 | grep -c LEAKED
echo "@@BUNDLE"; sudo -n log show --info --last 4m --predicate 'eventMessage CONTAINS "m6: surfaces for instance" OR eventMessage CONTAINS "scanout (instance" OR eventMessage CONTAINS "scanout: instance"' --style compact 2>/dev/null | grep 'WindowServer\['
echo "@@REPORTS"; find /Library/Logs/DiagnosticReports -newer /tmp/n48-s2-marker 2>/dev/null
EOS
}

# ======================================================================== state
SLOTLEN=0; A_HI=""; RC_GENMISS=0; DPCON=""; DPSLOTS=""; LEAK_BASE=0; LEAK_NOW=0
A_HELD_1=""; A_HELD_2=""; B_HELD_1=""; B_HELD_2=""; CRC_A_RG_1=""; CRC_A_B_1=""; CRC_A_RG_2=$CRC_RG2; CRC_A_B_2=$CRC_B2
RC_SLOTS_KNOWN_1=0; RC_SLOTS_KNOWN_2=0; RC_S0_1=""; RC_S1_1=""; RC_S2_1=""; RC_S0_2=""; RC_S1_2=""; RC_S2_2=""
OFFA_SINCE_1=""; OFFA_SINCE_2=""; ACQ_T_1=""; ACQ_T_2=""; GEOMBASE_1=""; GEOMBASE_2=""
EVER_POOLREF_1=0; EVER_POOLREF_2=0; EVER_GEOMWORD_1=0; EVER_GEOMWORD_2=0; EVER_FAILCLOSED_1=0; EVER_FAILCLOSED_2=0; EVER_ACQLOG_1=0; EVER_ACQLOG_2=0
FREE_LAST_1=""; FREE_LAST_2=""; NEED_LAST_1=""; NEED_LAST_2=""; MONBW6_N=0
S_T=0; S_SINCE=0; WSNOW=""; USERS_NOW=""; WS1=""; WSB=""; RESTART_EPOCH=0; RESTART_AT=""
VRAM_LOG=""
in_set() { local x=$1; shift; local y; for y in "$@"; do [ "$x" = "$y" ] && return 0; done; return 1; }
ov2() { [ $((0x$1)) -lt $((0x$3 + $4)) ] && [ $((0x$3)) -lt $((0x$1 + $2)) ]; }   # two ranges (start len start len) overlap
rc_fail() { RC_BAD="$RC_BAD $1;"; }
# the instance's baseline when NOT acquired (HELD on A, nothing pending); loaded instance in X_*; $1 = the instance
rc_plane_pre() {
  local i=$1
  { [ -z "${X_A:-}" ] || [ "$X_A" = 0 ]; } && [ -n "$(gv A_HELD_$i)" ] && X_A=$(gv A_HELD_$i)
  [ "${X_UF:-x}" = 0 ] || rc_fail "HUBP$i underflow ${X_UF:-?}"; [ "${X_BLANK:-x}" = 0 ] || rc_fail "BLANK_EN ${X_BLANK:-?}"; [ "${X_VTG:-x}" = "$(exp_vtg $i)" ] || rc_fail "VTG_SEL ${X_VTG:-?} (want $(exp_vtg $i))"
  [ -n "$X_VMF" ] && [ $((0x$X_VMF)) = 0 ] || rc_fail "VM fault 0x${X_VMF:-?}"; [ "${X_ODM10:-x}" = 0 ] || rc_fail "ODM$i bit 10 ${X_ODM10:-?}"
  [ -n "$X_FCR" ] && [ $(( 0x$X_FCR & 0x103 )) = 0 ] || rc_fail "FLIP_CONTROL 0x${X_FCR:-?} bits 0/1/8 not all 0"
  [ -n "$X_A" ] && [ "$X_PROG" = "$X_A" ] && [ "$X_EARLY" = "$X_A" ] || rc_fail "programmed 0x${X_PROG:-?} / EARLIEST 0x${X_EARLY:-?} != A$i 0x${X_A:-?}"
  [ "${X_PITCH:-x}" = "$(exp_pitch $i)" ] || rc_fail "pitch 0x${X_PITCH:-?} (want 0x$(exp_pitch $i))"; [ "${X_SCFG:-x}" = 8 ] || rc_fail "SURFACE_CONFIG 0x${X_SCFG:-?} (want 8)"
  [ "${X_SCTL:-x}" = 0 ] || rc_fail "SURFACE_CONTROL 0x${X_SCTL:-?}"; [ "${X_VMID:-x}" = 0 ] || rc_fail "VMID_SETTINGS_0 0x${X_VMID:-?}"
}
rc_fp_stuck() { # FLIP_PENDING stuck across 3 reads 100 ms apart (one ssh); 0 = stuck. $1 = instance
  [ "$DRYRUN" = 1 ] && return 1
  # stuck = pending in all 3 reads AND the same programmed/EARLIEST each time (no progress). A display flipping every frame reads pending most of the time while it IS latching.
  local l n u; l=$(ssh $SSHO "$HOST" "for i in 1 2 3; do sudo -n $T accel m6xstat $1 2 2>&1 | grep FLIP_CONTROL; sleep 0.1; done" </dev/null 2>/dev/null)
  n=$(printf '%s\n' "$l" | /usr/bin/grep -c 'FLIP_PENDING 1'); u=$(printf '%s\n' "$l" | /usr/bin/grep 'FLIP_PENDING 1' | sort -u | wc -l | tr -d ' ')
  [ "${n:-0}" = 3 ] && [ "${u:-0}" = 1 ]
}
# ---------------------------------------------------------------------------------------------- the abort path
kill_and_verify() { # kill_and_verify "<targets>" ["<watched-only instances>"]: touch each target's kill file; require EARLIEST = programmed = A and CRC = CRC_A within 6 s (PC clock) for EVERY target;
                    # sets KILL_OK KILL_SECS and, from the LAST poll block, KV_OTH_OK_<j> (a watched instance: EARLIEST in ITS slots and its CRC not CRC_A_j) and KV_OTH_EARLY_<j>
  local tg=$1 oth=${2:-} i cmd="" lst
  KILL_OK=0; KILL_SECS=""
  for i in $tg; do cmd="$cmd sudo -n touch $(kill_of $i); sudo -n chmod 644 $(kill_of $i); ls -l $(kill_of $i);"; done
  px killfile "$cmd echo touched-epoch=\$(date +%s)" || true
  local te; te=$(printf '%s' "$PXOUT" | sed -n 's/^touched-epoch=\([0-9]*\).*/\1/p' | head -1)
  N=$((N+1)); local KF; KF=$(printf '%s/%02d-killpoll.txt' "$RUN" "$N")
  lst="$tg $oth"
  if [ "$DRYRUN" = 1 ]; then say "--- [$N] DRYRUN: ssh $HOST '<14 x: date; for each of ($lst): m6xstat <i> 2; m6xstat <i> 0; disp2 crc <i> (OTG line); sleep .3>' -> $KF ; judged: first block where EVERY target ($tg) has EARLIEST == programmed == A and CRC == its CRC_A, epoch <= touch + 6"; return 0; fi
  local body=""
  for i in $lst; do body="$body sudo -n $T accel m6xstat $i 2 2>&1 | sed 's/^/I$i /'; sudo -n $T accel m6xstat $i 0 2>&1 | sed 's/^/I$i /'; sudo -n $T accel disp2 crc $i 2>&1 | grep 'OTG$i CRC' | sed 's/^/I$i /';"; done
  ssh $SSHO "$HOST" "for n in 1 2 3 4 5 6 7 8 9 10 11 12 13 14; do echo \"@@S\$n \$(date +%s)\"; $body sleep 0.3; done" >"$KF" 2>&1
  local n ts ok a
  for n in 1 2 3 4 5 6 7 8 9 10 11 12 13 14; do
    ts=$(sed -n "s/^@@S$n \([0-9]*\).*/\1/p" "$KF" | head -1); [ -n "$ts" ] || continue
    awk -v n="@@S$n " 'index($0,n)==1{f=1;next} /^@@S/{f=0} f' "$KF" >"$KF.blk"
    ok=1
    for i in $tg; do
      /usr/bin/grep "^I$i " "$KF.blk" | sed "s/^I$i //" >"$KF.blk$i"
      parse_x2 "$KF.blk$i" $i; parse_x0 "$KF.blk$i" $i; parse_crc "$KF.blk$i" $i
      a=$X_A; { [ -z "$a" ] || [ "$a" = 0 ]; } && a=$(gv A_HELD_$i)
      if [ -n "$X_EARLY" ] && [ -n "$a" ] && [ "$X_EARLY" = "$a" ] && [ "$X_PROG" = "$a" ] && crc_is_a $i; then :; else ok=0; fi
    done
    if [ "$ok" = 1 ]; then KILL_SECS=$(( ts - ${te:-$ts} )); [ "$KILL_SECS" -le 6 ] && KILL_OK=1; break; fi
  done
  # the watched-only instances, from the LAST block that carries them
  for i in $oth; do
    /usr/bin/grep "^I$i " "$KF.blk" | sed "s/^I$i //" >"$KF.blk$i"
    parse_x2 "$KF.blk$i" $i; parse_x0 "$KF.blk$i" $i; parse_crc "$KF.blk$i" $i
    local s0=$(gv RC_S0_$i) s1=$(gv RC_S1_$i) s2=$(gv RC_S2_$i) inslot=0
    [ -n "$X_EARLY" ] && in_set "$X_EARLY" "$s0" "$s1" "$s2" && inslot=1
    sv KV_OTH_EARLY_$i "${X_EARLY:-?}"; sv KV_OTH_INSLOT_$i $inslot; sv KV_OTH_NOTA_$i $(crc_is_a $i && echo 0 || echo 1)
  done
  say "  kill-file verification ($tg): EARLIEST = programmed = A and CRC_A first seen $([ -n "$KILL_SECS" ] && echo "at touch+${KILL_SECS}s" || echo 'NEVER within the 14 polls') -> $([ "$KILL_OK" = 1 ] && echo OK || echo 'NOT within 6 s')"
}
flip_abort() { # flip_abort "<targets>" reason...: quick capture FIRST, then the targets' kill files, verification, the full capture, park
  local tg; tg=$(echo $1); shift
  [ "$ABORTING" = 1 ] && return; ABORTING=1
  say; say "### ABORT at $(stamp) (kill file(s) of instance(s): $tg): $*"
  FAILS="$FAILS
  ABORT: $*"
  quick_capture
  kill_and_verify "$tg" ""
  if [ "$KILL_OK" = 1 ]; then say "### the kill file(s) took: EARLIEST = programmed = A and CRC_A within ${KILL_SECS}s for instance(s) $tg"
  else FAILS="$FAILS
  the kill file(s) did NOT restore A + CRC_A within 6 s: REBOOT WITHOUT FREEING ANYTHING (no teardown verb, no BO free, no second restart)"; say "### the kill file(s) did NOT restore A + CRC_A within 6 s: capturing and STOPPING. REBOOT WITHOUT FREEING ANYTHING; send no teardown verb."; fi
  capture
  say "### PC PARKED (no teardown verbs sent; kill file(s) for instance(s) $tg present so the bundle keeps them OFF)."
  summary_and_exit 1
}

# ======================================================================== REVIEW CHECKS (the Stage 2 safety review, "Run-time checks for the kit") ========================================================================
# rc_every <inst> (every sample, fast or full) and review_checks_b (every full K-snapshot) DEFINE hook B; hook A (review_checks_a) runs before the WindowServer restart. Every ABORT goes through flip_abort with the
# affected instance(s): quick capture -> that instance's kill file -> EARLIEST = programmed = A + its CRC_A within 6 s -> else "reboot without freeing anything"; NEVER a teardown verb.
refresh_inst() { # refresh_inst <inst>: re-read m6xstat <inst> 0 / 2 NOW (the acquire may have straddled the sample's two reads) into P<inst>_*
  local i=$1 f0="$RUN/refresh-$i-x0.txt" f2="$RUN/refresh-$i-x2.txt"
  ssh $SSHO "$HOST" "sudo -n $T accel m6xstat $i 0 2>&1" >"$f0" 2>&1; ssh $SSHO "$HOST" "sudo -n $T accel m6xstat $i 2 2>&1" >"$f2" 2>&1
  parse_x0 "$f0" $i; stash $i "$V0"; parse_x2 "$f2" $i; stash $i "$V2"
  if [ "${2:-}" = full ]; then local f1="$RUN/refresh-$i-x1.txt"; ssh $SSHO "$HOST" "sudo -n $T accel m6xstat $i 1 2>&1" >"$f1" 2>&1; parse_x1 "$f1" $i; stash $i "$V1"; fi
}
rc_core() { # rc_core <inst>: the state-dependent review checks of instance <inst> on its P<inst>_* fields; sets RC_BAD
  local i=$1 o a; if [ "$i" = 1 ]; then o=2; else o=1; fi
  use $i; RC_BAD=""
  [ "${X_OFFLINE:-0}" = 0 ] || rc_fail "m6xstat says instance $i is OFF (latch missing?)"
  [ -n "$X_ACQ" ] || rc_fail "m6xstat $i 0 unparsable"
  [ "${X_WDOG:-x}" = 0 ] || rc_fail "watchdog restores ${X_WDOG:-?} (> 0 at rest)"
  [ "${X_RESFAIL:-x}" = 0 ] || rc_fail "restore failures ${X_RESFAIL:-?}"; [ "${X_RFAILED:-x}" = no ] || rc_fail "restoreFailed ${X_RFAILED:-?}"
  [ -z "$CRC_RG_NOW" ] || [ "$DPW_OK" = 1 ] || rc_fail "the DP plane watch in disp2 crc $i is not 'all 15 registers unchanged' (HUBP0 outside its own slot set is the kext's row-14 rule)"
  if [ "$X_ACQ" = no ]; then rc_plane_pre $i
  elif [ "$X_ACQ" = YES ]; then
    [ -n "$X_A" ] && A_HI=$(( 0x$X_A >> 32 ))
    [ -n "$X_PROG" ] && [ "$(( 0x$X_PROG >> 32 ))" = "$A_HI" ] || rc_fail "programmed address HIGH dword != A$i's (programmed 0x${X_PROG:-?}, A 0x${X_A:-?})"
    [ -n "$X_EARLY" ] && [ "$(( 0x$X_EARLY >> 32 ))" = "$A_HI" ] || rc_fail "EARLIEST_INUSE$i HIGH dword != A$i's (0x${X_EARLY:-?})"
    if [ "$(gv RC_SLOTS_KNOWN_$i)" = 1 ]; then
      in_set "$X_PROG" "$X_A" "$(gv RC_S0_$i)" "$(gv RC_S1_$i)" "$(gv RC_S2_$i)" || rc_fail "programmed address 0x${X_PROG:-?} is outside {A$i, its slots}"
      in_set "$X_EARLY" "$X_A" "$(gv RC_S0_$i)" "$(gv RC_S1_$i)" "$(gv RC_S2_$i)" || rc_fail "EARLIEST_INUSE$i 0x${X_EARLY:-?} is outside {A$i, its slots}"
    fi
    # never the OTHER display's A / B / slots (the swap signature)
    local oa ob; oa=$(gv P${o}_X_A); { [ -n "$oa" ] && [ "$oa" != 0 ]; } || oa=$(gv A_HELD_$o); ob=$(gv P${o}_X_B); { [ -n "$ob" ] && [ "$ob" != 0 ]; } || ob=$(gv B_HELD_$o)
    for a in "$oa" "$ob" "$(gv RC_S0_$o)" "$(gv RC_S1_$o)" "$(gv RC_S2_$o)"; do
      [ -n "$a" ] && [ "$a" != 0 ] || continue
      [ "$X_PROG" != "$a" ] || rc_fail "the plane address 0x$X_PROG of instance $i EQUALS the OTHER display's ($o) A / B / slot"
      [ "$X_EARLY" != "$a" ] || rc_fail "EARLIEST_INUSE$i 0x$X_EARLY EQUALS the OTHER display's ($o) A / B / slot"
    done
    [ "${X_UF:-x}" = 0 ] || rc_fail "HUBP$i underflow ${X_UF:-?}"; [ -n "$X_VMF" ] && [ $((0x$X_VMF)) = 0 ] || rc_fail "VM fault 0x${X_VMF:-?}"; [ "${X_ODM10:-x}" = 0 ] || rc_fail "ODM$i bit 10 ${X_ODM10:-?}"
    # GPU-held code 4 (EARLIEST not a registered slot) is only a fault once a present has been made (acquired with nothing presented yet legitimately sits on A)
    [ "$X_GH4" = 0 ] || [ "${X_PRES:-0}" = 0 ] || rc_fail "GPU-HELD code 4 (EARLIEST_INUSE$i is not a registered slot) with $X_PRES presents made"
    if [ "${X_FP:-0}" != 0 ] && rc_fp_stuck $i; then rc_fail "FLIP_PENDING$i stuck across 3 reads 100 ms apart"; fi
  fi
}
rc_every() { # rc_every <inst>: every sample (fast or full); the instance's P<inst>_* fields must be fresh (x0 / x2 / crc of this sample)
  [ "$DRYRUN" = 1 ] && return 0
  local i=$1; RC_TG="$i"
  # the monitor B watch of an MONA crc op (AMBIGUITY 2): ONLY row 6 (EARLIEST_INUSE2, mask 0x40) with the monitor B ACQUIRED is benign
  use $i; local monbbad=""
  if [ "$i" = 1 ] && [ -n "$MONBW" ] && [ "$MONBW" != 0 ]; then
    local m=$((MONBW))
    if [ $(( m & ~0x40 )) != 0 ]; then monbbad="monitor B watch changed mask $MONBW across the monitor A's crc op (rows other than EARLIEST_INUSE2: clocks / mux / DET / underflow / OTG2 counting)"; RC_TG="1 2"
    elif [ "$(gv P2_X_ACQ)" = YES ]; then MONBW6_N=$((MONBW6_N+1))
    else
      refresh_inst 2; use 1       # the monitor B may have acquired between this sample's m6xstat 2 0 read and the monitor A's crc op: look once more
      if [ "$(gv P2_X_ACQ)" = YES ]; then MONBW6_N=$((MONBW6_N+1)); else monbbad="monitor B EARLIEST_INUSE2 moved across the monitor A's crc op while the monitor B is NOT acquired (mask $MONBW)"; RC_TG="1 2"; fi
    fi
  fi
  rc_core $i
  if [ -n "$RC_BAD" ] && [ "$X_ACQ" = no ]; then   # the acquire may have straddled the sample's two reads (m6xstat 0 said no, m6xstat 2 already shows a slot): look again once
    say "  (instance $i: baseline mismatch while acquired=no at +${S_SINCE}s:$RC_BAD re-reading once in case the acquire straddled the sample)"
    refresh_inst $i; rc_core $i
  fi
  [ -z "$monbbad" ] || RC_BAD="$RC_BAD $monbbad;"
  [ -z "$RC_BAD" ] || flip_abort "$RC_TG" "REVIEW CHECK failed (instance $i, acq=$X_ACQ, +${S_SINCE:-?}s):$RC_BAD"
}
# every slot / pair known so far, as "name start len" lines -> disjoint_check
disjoint_check() { # sets DJ_BAD (text) when two known ranges overlap
  local lines="" i s v nm
  for i in 1 2; do
    for nm in A B; do v=$(gv P${i}_X_$nm); { [ -n "$v" ] && [ "$v" != 0 ]; } || v=$(gv ${nm}_HELD_$i); [ -n "$v" ] && [ "$v" != 0 ] && lines="$lines
$nm$i $v $(inst_len $i)"; done
    if [ "$(gv RC_SLOTS_KNOWN_$i)" = 1 ]; then for s in 0 1 2; do lines="$lines
slot$i.$s $(gv RC_S${s}_$i) $(inst_len $i)"; done; fi
  done
  [ -n "$DPCON" ] && lines="$lines
DPconsole $DPCON $LENDP"
  local k=0; for s in $DPSLOTS; do lines="$lines
DPslot$k $s $LENDP"; k=$((k+1)); done
  DJ_BAD=""
  printf '%s\n' "$lines" | /usr/bin/grep -v '^$' >"$RUN/dj.tmp"
  local n1 a1 l1 n2 a2 l2 c1=0 c2
  while read -r n1 a1 l1; do
    c1=$((c1+1)); c2=0
    while read -r n2 a2 l2; do c2=$((c2+1)); [ $c2 -gt $c1 ] || continue
      ov2 "$a1" "$l1" "$a2" "$l2" && DJ_BAD="$DJ_BAD $n1(0x$a1)x$n2(0x$a2);"
    done <"$RUN/dj.tmp"
  done <"$RUN/dj.tmp"
}
review_checks_b() { # after every FULL K-snapshot once an HDMI instance may be acquired
  [ "$DRYRUN" = 1 ] && return 0
  local i v nm bad_tg="" msg
  for i in 1 2; do
    use $i; RC_BAD=""
    if [ "$X_ACQ" = YES ] && { [ -z "$X_S0" ] || [ "$((0x${X_S0:-0}))" = 0 ]; }; then   # the acquire straddled the snapshot's reads (m6xstat 0 saw it, m6xstat 1 did not): look again once
      say "  (instance $i: acquired but the slot page was read before the acquire at +${S_SINCE}s; re-reading m6xstat $i 0|1|2 once)"; refresh_inst $i full; use $i
    fi
    if [ "$X_ACQ" = YES ]; then
      [ -n "$X_A" ] && A_HI=$(( 0x$X_A >> 32 ))
      [ "$A_HI" = 128 ] || WARN="$WARN
  A$i's HIGH dword is 0x$(printf '%x' "$A_HI"), not 0x80 as the review assumed (the checks use A's own HIGH)"
      for nm in S0 S1 S2; do
        eval "v=\$X_$nm"
        if [ -z "$v" ]; then rc_fail "instance $i slot $nm MC not parsed"; continue; fi
        [ $(( 0x$v & 0xFFFF )) = 0 ] || rc_fail "slot $nm 0x$v is not 64 KiB aligned"
        [ "$(( 0x$v >> 32 ))" = "$A_HI" ] || rc_fail "slot $nm 0x$v HIGH dword != A$i's"
      done
      [ "${X_WREF:-x}" = 0 ] || rc_fail "writer refusals ${X_WREF:-?}"; [ "${X_WFAIL:-x}" = 0 ] || rc_fail "write failures ${X_WFAIL:-?}"
      [ "${X_UNTAG:-x}" = 0 ] || rc_fail "untagged presents refused ${X_UNTAG:-?}"; [ "${X_GEOM:-x}" = 0 ] || rc_fail "geometry refusals ${X_GEOM:-?}"
      if [ -z "$RC_BAD" ]; then sv RC_S0_$i "$X_S0"; sv RC_S1_$i "$X_S1"; sv RC_S2_$i "$X_S2"; sv RC_SLOTS_KNOWN_$i 1; fi
    fi
    [ -z "$RC_BAD" ] || flip_abort "$i" "REVIEW CHECK (K-snapshot) failed for instance $i:$RC_BAD (+${S_SINCE:-?}s)"
  done
  # all nine slots + both pairs + the DP console / slots pairwise disjoint
  disjoint_check
  [ -z "$DJ_BAD" ] || flip_abort "1 2" "slot / pair OVERLAP:$DJ_BAD (+${S_SINCE:-?}s)"
  # the bundle's cached table generation vs the kernel's published one (M1): equal in two consecutive K-snapshots; any instance's counters line carries it
  local bc="" kg=""; for i in 2 1; do [ -n "$(gv B${i}_CACHED)" ] && bc=$(gv B${i}_CACHED); done; kg=$(gv P2_X_TGEN); [ -n "$kg" ] || kg=$(gv P1_X_TGEN)
  if [ -n "$bc" ] && [ -n "$kg" ]; then
    if [ "$bc" = "$kg" ]; then RC_GENMISS=0
    else RC_GENMISS=$((RC_GENMISS+1)); say "  REVIEW: bundle cached table gen $bc != kernel published gen $kg (miss $RC_GENMISS)"; [ "$RC_GENMISS" -lt 2 ] || flip_abort "1 2" "the bundle's cached table generation $bc != the kernel's published $kg across 2 K-snapshots (M1: a publish the bundle never sees)"; fi
  else WARN="$WARN
  table generation not comparable at +${S_SINCE:-?}s (bundle cached '${bc:-?}', kernel '${kg:-?}': no counters line in the last 4 min?)"; fi
  # LEAKED lines (any new one since the P0 baseline) abort both
  [ "${LEAK_NOW:-0}" -le "${LEAK_BASE:-0}" ] 2>/dev/null || flip_abort "1 2" "a LEAKED line appeared in the kernel log ($LEAK_NOW now vs $LEAK_BASE at hook A)"
  # bundle log evidence per instance: the geometry-word refusal aborts that instance; inst_geom_mismatch rising after startup aborts it
  for i in 1 2; do
    [ "$(gv EVER_GEOMWORD_$i)" = 0 ] || flip_abort "$i" "the bundle logged 'the kernel's plane geometry word ... is NOT WxH' for instance $i"
    v=$(gv B${i}_GEOMMIS)
    if [ -n "$v" ] && [ "${S_SINCE:-0}" -ge 15 ]; then
      if [ -z "$(gv GEOMBASE_$i)" ]; then sv GEOMBASE_$i "$v"; elif [ "$v" -gt "$(gv GEOMBASE_$i)" ]; then flip_abort "$i" "inst_geom_mismatch of instance $i is still RISING after startup ($(gv GEOMBASE_$i) -> $v at +${S_SINCE}s): a surface of the wrong size is being planned to it (instance swap?)"; fi
    fi
  done
  monb_stall_check
  msg=""; for i in 1 2; do msg="$msg free-VRAM-line-$i: $(gv FREE_LAST_$i)/$(gv NEED_LAST_$i) MiB;"; done
  say "  REVIEW: kernel free visible VRAM ${V_FREE:-?} MiB of ${V_TOTAL:-?}; bundle pool lines (free/need):$msg"
}
# the monitor B must keep flipping through the monitor A's acquire: its kernel presents counter (m6xstat 2 0) across the monitor A's first acquire (AMBIGUITY 6)
MONB_STALL_DONE=0
monb_hist() { [ -n "$(gv P2_X_PRES)" ] && printf '%s %s\n' "$S_T" "$(gv P2_X_PRES)" >>"$RUN/monbhist.tsv"; }
monb_stall_check() {
  [ "$DRYRUN" = 1 ] && return 0
  [ "$MONB_STALL_DONE" = 0 ] && [ -n "$ACQ_T_1" ] && [ "$(gv P2_X_ACQ)" = YES ] && [ "$S_T" -ge $((ACQ_T_1 + 12)) ] || return 0
  MONB_STALL_DONE=1
  local r; r=$(awk -v a="$ACQ_T_1" -v d="${ACQ_T_2:-0}" '{ lb=a-40; if (d>lb) lb=d; if ($1>=lb && $1<=a-1) { if (!b1++) {t1=$1;p1=$2}; t1e=$1;p1e=$2 } if ($1>=a-1) { if (!b2++) {t2=$1;p2=$2}; t2e=$1;p2e=$2 } }
    END { if (b1<2 || b2<2 || t1e-t1<4 || t2e-t2<4) { print "none"; exit } r1=(p1e-p1)/(t1e-t1); r2=(p2e-p2)/(t2e-t2); printf "%.2f %.2f\n", r1, r2 }' "$RUN/monbhist.tsv" 2>/dev/null)
  say "  monitor B presents/s before / after the monitor A's acquire: ${r:-none}"
  case "$r" in none|"") WARN="$WARN
  monitor B stall check: not enough monitor B presents samples (>= 4 s of the monitor B acquired BEFORE the monitor A's acquire, and 4 s after) around the monitor A's acquire: not evaluable (the monitor B may have acquired just before the monitor A, or be idle)" ;;
    *) set -- $r; if awk -v a="$1" -v b="$2" 'BEGIN{exit !(a>=2 && b < a*0.25)}'; then flip_abort "1" "the monitor B STALLED during the monitor A's acquire (monitor B presents/s $1 before, $2 after)"; fi ;;
  esac
}
review_checks_kill() { # AFTER a kill-switch leg: that instance's restore VERIFIED and counted, restore-failed 0, its CRC_A exact, the other instance's counters unmoved, no LEAKED since hook A
  # $1 = the instance killed, $2 = the other. Sets RCK (PASS|FAIL) RCKw. Needs RES_BEFORE_<k> / OTH_BEFORE_<o> from the leg's "before" reads.
  local k=$1 o=$2
  if [ "$DRYRUN" = 1 ]; then say "  DRYRUN: review_checks_kill $k: m6xstat $k 0 restores incremented (was RES_BEFORE) and restoreFailed 0; disp2 crc $k == CRC_A$k; instance $o's restores / watchdog unchanged; bundle line 'release rc .. restore of A verified 1'; LEAKED count == the hook-A baseline"; RCK="(dry run)"; RCKw=""; return 0; fi
  sleep 2
  px rck$k-xk "sudo -n $T accel m6xstat $k 0 2>&1" || true; parse_x0 "$PXF" $k; local kres=$X_RESTORES kfail=$X_RESFAIL krf=$X_RFAILED kwd=$X_WDOG
  px rck$k-xo "sudo -n $T accel m6xstat $o 0 2>&1" || true; parse_x0 "$PXF" $o; local ores=$X_RESTORES owd=$X_WDOG
  px rck$k-crc "sudo -n $T accel disp2 crc $k 2>&1" || true; parse_crc "$PXF" $k
  px rck$k-log "echo LEAKED-NOW: \$(sudo -n $T log 2>&1 | grep -c LEAKED); sudo -n log show --info --last 3m --predicate 'eventMessage CONTAINS \"scanout (instance $k\"' --style compact 2>/dev/null | grep 'WindowServer\\['" || true
  local w="" ln rb ob; ln=$(printf '%s\n' "$PXOUT" | sed -n 's/^LEAKED-NOW: //p' | head -1)
  rb=$(gv RES_BEFORE_$k); ob=$(gv OTH_BEFORE_$o)
  [ -n "$rb" ] && [ -n "$kres" ] && [ "$kres" -gt "$rb" ] || w="$w the killed instance $k's restore count did not increment (${rb:-?} -> ${kres:-?});"
  [ "${kfail:-x}" = 0 ] && [ "${krf:-x}" = no ] || w="$w instance $k restore failures ${kfail:-?} / restoreFailed ${krf:-?};"
  [ "${kwd:-0}" = 0 ] || w="$w instance $k watchdog restores ${kwd};"
  [ -n "$ob" ] && [ "$ores" = "$ob" ] || w="$w the OTHER instance $o's restore count moved (${ob:-?} -> ${ores:-?});"
  printf '%s\n' "$PXOUT" | /usr/bin/grep -q 'restore of A verified 1' || WARN="$WARN
  after the kill leg of instance $k: no bundle line 'release rc 0, restore of A verified 1' in the last 3 min (log lag?)"
  crc_is_a $k || w="$w the instance-$k CRC ${CRC_RG_NOW:-?}/${CRC_B_NOW:-?} != its CRC_A;"
  [ "${ln:-x}" = "$LEAK_BASE" ] || w="$w the kernel log has LEAKED lines: ${ln:-?} now vs $LEAK_BASE at hook A;"
  RCK=$([ -z "$w" ] && echo PASS || echo FAIL); RCKw="$w (instance $k restores ${rb:-?} -> ${kres:-?}, restoreFailed ${krf:-?}; other instance $o restores ${ob:-?} -> ${ores:-?}; LEAKED ${ln:-?} vs $LEAK_BASE)"
  say "  after-kill review checks (instance $k): $RCK $RCKw"
}

# ---------------------------------------------------------------------------------------------- fast / full sampling helpers
printf 'phase\tt_pc\tsec_since_restart\tws\tacq2\tcrc2_rg\tcrc2_b\tuf2\todm10_2\tfp2\tearly2\tprog2\tacq1\tcrc1_rg\tcrc1_b\tuf1\todm10_1\tfp1\tearly1\tprog1\tvmf\twdog2\twdog1\treuse2\treuse1\thung\tcrc2_isA\tcrc1_isA\n' >"$RUN/samples.tsv"
printf 'min\ttime\tws\tstamps0\tstamps1\tstamps2\tcrc2_rg\tcrc2_b\tcrc1_rg\tcrc1_b\tacq2\tacq1\ttxn0\ttxn1\ttxn2\tids0\tids1\tids2\tambig\tnofb\tagdc_nfb\tn921_1\tn921_2\totg0\tdig1_be\tusers\tdp_presents_s\tdp_unknown\tpresents2\tpresents1\tgeommis1\tgeommis2\tmism1\tmism2\tfree_mib\n' >"$RUN/minutes.tsv"
: >"$RUN/monbhist.tsv"
PHASE=pre; SF=""; SNAP_N=0; FASTN=0; HUNG=""
parse_fast_inst() { # parse_fast_inst <file>: X0 / X2 / CRC of both instances into P1_* / P2_*
  local f=$1 i
  for i in 1 2; do
    sect X0_$i "$f" >"$f.x0_$i"; sect X2_$i "$f" >"$f.x2_$i"; sect CRC$i "$f" >"$f.crc$i"
    parse_x0 "$f.x0_$i" $i; stash $i "$V0"; parse_x2 "$f.x2_$i" $i; stash $i "$V2"; parse_crc "$f.crc$i" $i; stash $i "$VC"
  done
}
note_flip() { # after a sample: first sight of the acquire / of the CRC leaving CRC_A, per instance (S_T / S_SINCE are fresh)
  local i
  for i in 1 2; do
    use $i
    if [ "$X_ACQ" = YES ] && [ -z "$(gv ACQ_T_$i)" ]; then sv ACQ_T_$i "$S_T"; sv ACQSINCE_$i "$S_SINCE"; fi
    if [ -n "$CRC_RG_NOW" ] && ! crc_is_a $i && [ -z "$(gv OFFA_SINCE_$i)" ]; then sv OFFA_SINCE_$i "$S_SINCE"; fi
    # the OTG must keep counting (two identical frame counters in a row = the display lost its timing)
    if [ -n "$X_OTGFC" ]; then
      if [ "$X_OTGFC" = "$(gv LASTFC_$i)" ]; then sv FCSAME_$i $(( $(gv FCSAME_$i) + 1 )); else sv FCSAME_$i 0; fi
      sv LASTFC_$i "$X_OTGFC"
    fi
  done
}
FCSAME_1=0; FCSAME_2=0; LASTFC_1=""; LASTFC_2=""
fast_sample() { # fast_sample <phase> : one ssh; parses into P1_* / P2_*; appends a samples.tsv row; sets S_T (PC epoch), S_SINCE
  PHASE=$1; N=$((N+1)); FASTN=$((FASTN+1)); SF=$(printf '%s/%02d-fast-%s%03d.txt' "$RUN" "$N" "$PHASE" "$FASTN")
  if [ "$DRYRUN" = 1 ]; then say "--- [$N] DRYRUN: ssh $HOST '<fast: date; pgrep WS; console+who; m6xstat 2 0|2; m6xstat 1 0|2; disp2 crc 2; disp2 crc 1; sessstat 4>' -> $SF"; : >"$SF"; S_T=0; S_SINCE=0; return 0; fi
  if ! fast_remote | ssh $SSHO "$HOST" 'bash -s' >"$SF" 2>&1; then
    if reach; then say "  (the fast ssh returned nonzero but the PC answers; using what arrived)"; else stop 3 "PC unreachable during the sampling ($PHASE), and one more check failed"; fi
  fi
  S_T=$(sect T "$SF" | head -1); S_SINCE=$(( ${S_T:-0} - ${RESTART_EPOCH:-0} ))
  WSNOW=$(sect WS "$SF" | head -1); USERS_NOW=$(sect USERS "$SF" | tr '\n' ' ')
  sect HUNG "$SF" >"$SF.hung"; parse_hung "$SF.hung"
  parse_fast_inst "$SF"; note_flip; monb_hist
  local i row=""
  for i in 2 1; do use $i; local ia=0; crc_is_a $i && ia=1; row="$row$TAB$X_ACQ$TAB$CRC_RG_NOW$TAB$CRC_B_NOW$TAB$X_UF$TAB$X_ODM10$TAB$X_FP$TAB$X_EARLY$TAB$X_PROG"; sv CIA_$i $ia; done
  use 2; local v2=$X_VMF w2=$X_WDOG r2=$X_REUSE; use 1
  printf '%s\t%s\t%s\t%s%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$PHASE" "$S_T" "$S_SINCE" "$WSNOW" "$row" "$v2" "$w2" "$X_WDOG" "$r2" "$X_REUSE" "$HUNG" "$CIA_2" "$CIA_1" >>"$RUN/samples.tsv"
  say "  [$PHASE +${S_SINCE}s] WS $WSNOW | monitor B acq=$(gv P2_X_ACQ) CRC $(gv P2_CRC_RG_NOW)/$(gv P2_CRC_B_NOW)$([ "$CIA_2" = 1 ] && echo '(=A)') uf=$(gv P2_X_UF) pend=$(gv P2_X_FP) early=0x$(gv P2_X_EARLY) | monitor A acq=$(gv P1_X_ACQ) CRC $(gv P1_CRC_RG_NOW)/$(gv P1_CRC_B_NOW)$([ "$CIA_1" = 1 ] && echo '(=A)') uf=$(gv P1_X_UF) pend=$(gv P1_X_FP) early=0x$(gv P1_X_EARLY) | vmf=0x$(gv P2_X_VMF)/0x$(gv P1_X_VMF) hung=$HUNG"
}
health_abort() { # called after every sample once the HDMI instances have had the chance to flip: ABORT on any underflow / ODM bit 10 / VM fault / HUNG / WS pid change; then the per-instance review checks
  [ "$DRYRUN" = 1 ] && return 0
  local bad="" i badtg="" glob=0
  [ -n "$WSNOW" ] && [ "$WSNOW" = "${WS1:-}" ] || { bad="$bad WindowServer pid '$WSNOW' != '${WS1:-}' (a crash or relaunch);"; glob=1; }
  [ -n "$HUNG" ] && [ "$HUNG" = 0 ] || { bad="$bad HUNG '${HUNG:-unparsable}';"; glob=1; }
  for i in 1 2; do
    use $i
    [ -n "$X_VMF" ] && [ "$((0x$X_VMF))" = 0 ] || { bad="$bad DCN_VM_FAULT_STATUS 0x${X_VMF:-?} (read with instance $i; the register is GLOBAL);"; glob=1; }
    [ -n "$X_UF" ] && [ "$X_UF" = 0 ] || { bad="$bad HUBP$i underflow '${X_UF:-?}';"; badtg="$badtg $i"; }
    [ -n "$X_ODM10" ] && [ "$X_ODM10" = 0 ] || { bad="$bad ODM$i bit 10 '${X_ODM10:-?}';"; badtg="$badtg $i"; }
    [ "$(gv FCSAME_$i)" -lt 2 ] || { bad="$bad OTG$i frame counter did not move across 3 samples ($X_OTGFC);"; badtg="$badtg $i"; }
  done
  if [ -n "$bad" ]; then
    C4p=FAIL; C4pw="($bad at +${S_SINCE}s)"
    if [ "$glob" = 1 ]; then flip_abort "1 2" "health abort (global):$bad"; else flip_abort "$(printf '%s\n' $badtg | sort -u | tr '\n' ' ')" "health abort:$bad"; fi
  fi
  rc_every 1; rc_every 2
  # a console user appearing is NOT a flip abort: park as 1a/1b do. run 20261007-2145 stopped falsely on '_windowserver ': WindowServer's own account owns the console for a moment after the restart. A login = any other owner, or any `who` line.
  case "$USERS_NOW" in "root "|"_windowserver ") ;; *) stop 1 "a console user appeared ('$USERS_NOW'): user logged in; the PC is left as is" ;; esac
}

# ---- the K-snapshot parser/evaluator used pre-flip and per minute --------------------------------
LAST_DPRATE=""; PRES=""; PUNK=""; DPRATE=0; RATE0=""; RATE1=""; RATE2=""; DP_OTG0=""; DP_BE=""; DP_BE0=""; LAST_BE=""; NEWREPORTS=""
V_FREE=""; V_TOTAL=""; V_MAPPED=""; V_FREEB=""
for k in 0 1 2; do sv BASE_$k ""; sv LOW_$k 0; sv ACTIVE_$k ""; done
for k in 1 2; do sv LASTP_$k ""; sv LASTPTOD_$k ""; sv PRATE_$k ""; done
PRE_LAST_OK=""; PRE_LAST_W=""; PRE_LAST_AT=""; KPRE_OK=""; KPRE_W=""
ksnap() { # ksnap <phase> <min> : one full snapshot (~10 s); fills P1_* / P2_* (all groups), the M_* / V_* / B* globals and appends a minutes.tsv row
  local ph=$1 m=$2 i k; PHASE=$ph; N=$((N+1)); SNAP_N=$((SNAP_N+1)); SF=$(printf '%s/%02d-snap-%s%02d.txt' "$RUN" "$N" "$ph" "$m")
  say "--- [$N] $(stamp) K-snapshot $ph/$m (one ssh) -> $SF"
  if [ "$DRYRUN" = 1 ]; then say "    DRYRUN: ssh $HOST '<snapshot: date; pgrep WS; console+who; m6stat 1 x2 3 s apart; vramstat; m6xstat 2|1 0|1|2; disp2 crc 2|1; disp2 status 2|1; m6stat 0|1|2|3|4; sessstat 4; LEAKED count; bundle log lines; new reports>'"; return 0; fi
  if ! snap_remote | ssh $SSHO "$HOST" 'bash -s' >"$SF" 2>&1; then
    if reach; then say "  (the snapshot ssh returned nonzero but the PC answers; using what arrived)"; else stop 3 "PC unreachable during the K-snapshot ($ph/$m), and one more check failed"; fi
  fi
  S_T=$(sect T2 "$SF" | head -1); [ -n "$S_T" ] || S_T=$(sect T "$SF" | head -1); S_SINCE=$(( ${S_T:-0} - ${RESTART_EPOCH:-0} ))   # T2 = the clock right after the CRC reads (the flip evidence), not the start of the snapshot
  WSNOW=$(sect WS "$SF" | head -1); USERS_NOW=$(sect USERS "$SF" | tr '\n' ' ')
  local a b
  for k in 0 1 2; do
    a=$(sect STAMPA "$SF" | sed -n "s/^ *instance $k .*: \([0-9][0-9]*\) vblank stamps written.*/\1/p" | head -1); b=$(sect STAMPB "$SF" | sed -n "s/^ *instance $k .*: \([0-9][0-9]*\) vblank stamps written.*/\1/p" | head -1)
    if [ -n "$a" ] && [ -n "$b" ]; then sv RATE$k $(( (b - a) / 3 )); else sv RATE$k ""; fi
  done
  DPRATE=$RATE0
  for i in 1 2; do
    sect X0_$i "$SF" >"$SF.x0_$i"; sect X1_$i "$SF" >"$SF.x1_$i"; sect X2_$i "$SF" >"$SF.x2_$i"; sect CRC$i "$SF" >"$SF.crc$i"
    parse_x0 "$SF.x0_$i" $i; stash $i "$V0"; parse_x1 "$SF.x1_$i" $i; stash $i "$V1"; parse_x2 "$SF.x2_$i" $i; stash $i "$V2"; parse_crc "$SF.crc$i" $i; stash $i "$VC"
  done
  sect HUNG "$SF" >"$SF.hung"; parse_hung "$SF.hung"; sect STATUS "$SF" >"$SF.st"; parse_dpside "$SF.st"; sect STATUS1 "$SF" >"$SF.st1"
  sect M6_0 "$SF" >"$SF.m0"; sect M6_1 "$SF" >"$SF.m1"; sect M6_2 "$SF" >"$SF.m2"; sect M6_3 "$SF" >"$SF.m3"; sect M6_4 "$SF" >"$SF.m4"; sect BUNDLE "$SF" >"$SF.bundle"; sect REPORTS "$SF" >"$SF.reports"; sect VRAM "$SF" >"$SF.vram"
  parse_m6 "$SF.m0" "$SF.m1" "$SF.m2" "$SF.m4"; parse_vram "$SF.vram"
  LEAK_NOW=$(sect LEAK "$SF" | head -1); LEAK_NOW=${LEAK_NOW:-0}
  /usr/bin/grep 'm6: surfaces for instance' "$SF.bundle" >"$SF.bundle.m6"
  local pr; pr=$(bundle_rate "$SF.bundle.m6"); PRES=""; PUNK=""; [ -n "$pr" ] && [ "$pr" != none ] && { PRES=${pr%% *}; PUNK=$(echo "$pr" | awk '{print $2}'); }
  for i in 1 2; do
    parse_bundlec "$SF.bundle" $i; parse_bundlelog "$SF.bundle" $i
    # sticky evidence (the 4-minute log window forgets): refusals / acquire lines / geometry-word lines / fail-closed
    [ "$(gv BL${i}_POOLREF)" -gt 0 ] 2>/dev/null && sv EVER_POOLREF_$i 1
    [ "$(gv BL${i}_GEOMWORD)" -gt 0 ] 2>/dev/null && sv EVER_GEOMWORD_$i 1
    [ "$(gv BL${i}_FAILCLOSED)" -gt 0 ] 2>/dev/null && sv EVER_FAILCLOSED_$i 1
    [ "$(gv BL${i}_ACQUIRED)" -gt 0 ] 2>/dev/null && sv EVER_ACQLOG_$i 1
    [ -n "$(gv BL${i}_FREE)" ] && { sv FREE_LAST_$i "$(gv BL${i}_FREE)"; sv NEED_LAST_$i "$(gv BL${i}_NEED)"; }
    [ -n "$(gv BL${i}_INIT)" ] && { sv INIT_$i "$(gv BL${i}_INIT)"; sv KILLFILE_$i "$(gv BL${i}_KILLFILE)"; }
    # HDMI presents/s from the bundle's own counters line (its own timestamp)
    local pp tod lp lt; pp=$(gv B${i}_PRES); tod=$(gv B${i}_TOD); lp=$(gv LASTP_$i); lt=$(gv LASTPTOD_$i)
    if [ -n "$pp" ] && [ -n "$tod" ]; then
      if [ -n "$lp" ] && [ "$tod" -gt "$lt" ]; then sv PRATE_$i $(( (pp - lp) / (tod - lt) )); fi
      if [ -z "$lp" ] || [ "$tod" -gt "$lt" ]; then sv LASTP_$i "$pp"; sv LASTPTOD_$i "$tod"; fi
    fi
  done
  note_flip; monb_hist
  NEWREPORTS=$(cat "$SF.reports" | tr '\n' ' ')
  local ia
  CIA_1=0; use 1; crc_is_a 1 && CIA_1=1; CIA_2=0; use 2; crc_is_a 2 && CIA_2=1
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$ph$m" "$(stamp)" "$WSNOW" "${RATE0:-}" "${RATE1:-}" "${RATE2:-}" "$(gv P2_CRC_RG_NOW)" "$(gv P2_CRC_B_NOW)" "$(gv P1_CRC_RG_NOW)" "$(gv P1_CRC_B_NOW)" "$(gv P2_X_ACQ)" "$(gv P1_X_ACQ)" \
    "${M_TXN0:-}" "${M_TXN1:-}" "${M_TXN2:-}" "${M_IDS0:-}" "${M_IDS1:-}" "${M_IDS2:-}" "${M_AMB:-}" "${M_NOFB:-}" "${M_AGN:-}" "${M_N921_1:-}" "${M_N921_2:-}" "${DP_OTG0:-}" "${DP_BE:-}" "$([ "$USERS_NOW" = "root " ] && echo 0 || echo 1)" "${PRES:-n/a}" "${PUNK:-n/a}" "$(gv B2_PRES)" "$(gv B1_PRES)" "$(gv B1_GEOMMIS)" "$(gv B2_GEOMMIS)" "$(gv B1_MISMATCH)" "$(gv B2_MISMATCH)" "${V_FREE:-?}" >>"$RUN/minutes.tsv"
  say "  $ph/$m +${S_SINCE}s: WS $WSNOW | monitor B acq=$(gv P2_X_ACQ) CRC $(gv P2_CRC_RG_NOW)/$(gv P2_CRC_B_NOW)$([ "$CIA_2" = 1 ] && echo '(=A)') | monitor A acq=$(gv P1_X_ACQ) CRC $(gv P1_CRC_RG_NOW)/$(gv P1_CRC_B_NOW)$([ "$CIA_1" = 1 ] && echo '(=A)') | stamps/s DP ${RATE0:-?} monitor A ${RATE1:-?} monitor B ${RATE2:-?} DP presents/s ${PRES:-n/a} OTG0 ${DP_OTG0:-?} | txn ${M_TXN0:-?}/${M_TXN1:-?}/${M_TXN2:-?} IDs ${M_IDS0:-?}/${M_IDS1:-?}/${M_IDS2:-?} amb ${M_AMB:-?} nofb ${M_NOFB:-?} | AGDC nfb ${M_AGN:-?} n921 ${M_N921_1:-?}/${M_N921_2:-?} ep 0x${M_EPMASK:-?} | free VRAM ${V_FREE:-?}/${V_TOTAL:-?} MiB | geom-mismatch ${B1_GEOMMIS:-?}/${B2_GEOMMIS:-?} reuse ${P1_X_REUSE:-?}/${P2_X_REUSE:-?} wdog ${P1_X_WDOG:-?}/${P2_X_WDOG:-?}"
}
ksnap_checks() { # the DP-side hard checks of a K-snapshot (OTG0 COUNTING, DIG1 change, report appearance) - after health_abort
  [ "$DRYRUN" = 1 ] && return 0
  [ "$DP_OTG0" = COUNTING ] || { C6p=FAIL; C6pw="$C6pw (OTG0 '$DP_OTG0' at +${S_SINCE}s)"; flip_abort "1 2" "the DP's OTG0 is not COUNTING ('$DP_OTG0') at +${S_SINCE}s"; }
  [ "$DP_BE" = "$LAST_BE" ] || WARN="$WARN
  DIG1_DIG_BE_CNTL $LAST_BE -> $DP_BE at +${S_SINCE}s (DP side register changed; arming may legitimately change it, queue 285; judge by eye)"
  LAST_BE=$DP_BE
  [ -z "${NEWREPORTS// /}" ] || WARN="$WARN
  new DiagnosticReports since the P0 marker at +${S_SINCE}s: $NEWREPORTS"
}
# per-display floors (K4/K5, AMBIGUITY 1): an ACTIVE display (minute-1 rate >= ACT_MIN) must stay above max(STAMP_MIN, 50% of its baseline) - 2 consecutive snapshots abort; <85% is a warning; an IDLE display has no floor.
# arg: the watch minute number. DP presents/s (bundle) judged the same way against PRES_MIN from minute 2.
FLOOR_ANY_ACTIVE=0
floors() {
  [ "$DRYRUN" = 1 ] && return 0
  local k r b fl nm
  for k in 0 1 2; do
    r=$(gv RATE$k); [ -n "$r" ] || { WARN="$WARN
  minute $1: no stamp reading for display $k"; continue; }
    b=$(gv BASE_$k); if [ -z "$b" ]; then sv BASE_$k "$r"; b=$r; if [ "$r" -ge "$ACT_MIN" ]; then sv ACTIVE_$k 1; FLOOR_ANY_ACTIVE=1; else sv ACTIVE_$k 0; fi; fi
    case $k in 0) nm=DP;; 1) nm=monitor A;; 2) nm=monitor B;; esac
    if [ "$(gv ACTIVE_$k)" = 1 ]; then
      fl=$(( b / 2 )); [ "$fl" -ge "$STAMP_MIN" ] || fl=$STAMP_MIN
      if [ "$r" -lt "$fl" ]; then sv LOW_$k $(( $(gv LOW_$k) + 1 )); else sv LOW_$k 0; fi
      [ $(( r * 100 )) -ge $(( b * 85 )) ] || FLOORWARN="$FLOORWARN
  minute $1: $nm stamps/s $r < 85% of its minute-1 baseline $b"
      if [ "$(gv LOW_$k)" -ge 2 ]; then C6p=FAIL; C6pw="$C6pw $nm stamps/s $r < floor $fl twice running at minute $1 (baseline $b);"; flip_abort "$([ $k = 0 ] && echo '1 2' || echo $k)" "display $nm (active, baseline $b stamps/s) fell below its floor $fl twice running: $r stamps/s at watch minute $1"; fi
    fi
  done
  if [ "$1" -ge 2 ]; then
    if [ -z "$PRES" ]; then WARN="$WARN
  minute $1: no bundle 'm6:' counter lines >= 50 s apart (DP presents/s not readable)"
    else
      [ -n "$(gv BASEP_0)" ] || { sv BASEP_0 "$PRES"; if [ "$PRES" -ge "$ACT_MIN" ]; then sv ACTP_0 1; else sv ACTP_0 0; fi; }
      if [ "$(gv ACTP_0)" = 1 ] && [ "$PRES" -lt "$PRES_MIN" ]; then sv LOWP_0 $(( $(gv LOWP_0) + 1 )); else sv LOWP_0 0; fi
      [ "$(gv LOWP_0)" -lt 2 ] || { C6p=FAIL; C6pw="$C6pw DP presents/s $PRES < $PRES_MIN twice running;"; flip_abort "1 2" "DP frames delivered collapsed: $PRES presents/s at watch minute $1 twice running (floor $PRES_MIN; baseline $(gv BASEP_0))"; }
    fi
  fi
}
FLOORWARN=""; BASEP_0=""; ACTP_0=""; LOWP_0=0
# R0 on a K-snapshot taken while NO HDMI instance was acquired and both CRCs were exact: sets KPRE_OK (1/0) KPRE_W. Covers K1 (counters side), K2, K3, K6, K7 (live part).
pre_k_eval() {
  local w="" i
  [ "${M_AGN:-0}" = 3 ] || w="$w AGDC nfb ${M_AGN:-?} (want 3);"
  [ "${M_NFB4:-0}" = 3 ] || w="$w m6stat 4 nfb ${M_NFB4:-?} (want 3);"
  [ "${M_EPMASK:-x}" = 7 ] || w="$w endpoint bitmask 0x${M_EPMASK:-?} (want exactly 0x7 = {0,1,2});"
  [ "${M_EPOUT:-x}" = 0 ] || w="$w endpoints outside the list ${M_EPOUT:-?};"
  for i in 1 2; do [ "$(gv M_N921_$i)" -ge 1 ] 2>/dev/null || w="$w 0x921 answered $(gv M_N921_$i) times for instance $i (want >= 1);"; done
  for i in 0 1 2; do [ "$(gv M_TXN$i)" -gt 0 ] 2>/dev/null || w="$w transactions on instance $i: $(gv M_TXN$i) (want > 0);"; done
  [ "${M_AMB:-1}" = 0 ] || w="$w ambiguous ${M_AMB:-?};"
  [ "${M_NOFB:-1}" = 0 ] || w="$w no-framebuffer submits ${M_NOFB:-?};"
  [ "${M_AMB2:-1}" = 0 ] 2>/dev/null || w="$w page 2 AMBIGUOUS;"
  [ "$DP_OTG0" = COUNTING ] || w="$w OTG0 '$DP_OTG0';"
  [ "${CIA_2:-0}" = 1 ] || w="$w monitor B CRC $(gv P2_CRC_RG_NOW)/$(gv P2_CRC_B_NOW) != CRC_A2;"
  [ "${CIA_1:-0}" = 1 ] || w="$w monitor A CRC $(gv P1_CRC_RG_NOW)/$(gv P1_CRC_B_NOW) != CRC_A1;"
  for i in 1 2; do use $i
    [ "$UF_OK" = 1 ] || w="$w HUBP$i/ODM$i underflow lines not all 0;"
    [ "${FIFO_ERR:-x}" = 0 ] || w="$w DIG$((i+1)) FIFO error '${FIFO_ERR:-?}' (want 0);"
  done
  local any=0; for i in 0 1 2; do [ "$(gv RATE$i)" -ge "$ACT_MIN" ] 2>/dev/null && any=1; done
  [ "$any" = 1 ] || WARN="$WARN
  R0 snapshot at +${S_SINCE}s: no display shows >= $ACT_MIN stamps/s (stamps/s DP ${RATE0:-?} monitor A ${RATE1:-?} monitor B ${RATE2:-?}): the desktop may be static; floors are vacuous"
  KPRE_W="$w (K-snapshot at +${S_SINCE}s: AGDC nfb ${M_AGN:-?}, ep mask 0x${M_EPMASK:-?}, n921 ${M_N921_1:-?}/${M_N921_2:-?}, txn ${M_TXN0:-?}/${M_TXN1:-?}/${M_TXN2:-?}, IDs ${M_IDS0:-?}/${M_IDS1:-?}/${M_IDS2:-?}, stamps/s ${RATE0:-?}/${RATE1:-?}/${RATE2:-?})"
  [ -z "$w" ] && KPRE_OK=1 || KPRE_OK=0
}

# the parser tests source this file as a library: everything above is definitions only
if [ "${M6S2_LIB:-}" = 1 ]; then return 0 2>/dev/null || exit 0; fi

# ---------------------------------------------------------------------------------------------- P0 prechecks
say "### M6 Stage $STAGE RUN $TS dry-run=$DRYRUN host=$HOST watch=${WATCH_MIN}min STAMP_MIN=$STAMP_MIN ACT_MIN=$ACT_MIN USER_STEP=$USER_STEP SKIP_KILL=$SKIP_KILL pool=$POOL out=$RUN"
say "### host Mac date: $(date)"
say "### $([ "$STAGE" = 2a ] && echo 'STAGE 2a: /private/tmp/n48m-noflip1 is touched before the WindowServer start; the monitor A stays on the bars (IOPresentment), never flips; the monitor B flips as in 1b.' || echo 'STAGE 2b: noflip1 ABSENT; the monitor A flips too. Needs the R-B1 fix in the installed bundle.')"
say; say "=== P0 prechecks"
reach || stop 3 "PC ($HOST) not reachable"
px pre-system 'echo "up: $(uptime)"; echo "boottime: $(sysctl -n kern.boottime)"; echo "boot-args: $(sysctl -n kern.bootargs)"; echo "console: $(stat -f %Su /dev/console)"; echo "who:"; who; echo "WS: $(pgrep -x WindowServer)"; echo "loaded:"; kmutil showloaded --list-only 2>/dev/null | grep -iE "navi48|rdna4|accelprobe"; echo "bundle: $(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" /Library/GPUBundles/Navi48Metal.bundle/Contents/Info.plist 2>&1)"; echo "b1token: $(/usr/bin/grep -ac "'"$B1_TOKEN"'" '"$BUNDLE_BIN"' 2>&1)"; echo "geomtoken: $(/usr/bin/grep -ac inst_geom_mismatch '"$BUNDLE_BIN"' 2>&1)"; echo "noflip1token: $(/usr/bin/grep -ac n48m-noflip1 '"$BUNDLE_BIN"' 2>&1)"; echo "uuid: $(otool -l '"$BUNDLE_BIN"' 2>/dev/null | grep -A2 LC_UUID | grep uuid | tr -s " ")"; echo "ESP: $(~/navi48-staging/esp-kext.sh show 2>&1 | grep "injected kext")"; echo "nub:"; sudo -n ~/n48-metal/n48nub status 2>&1; echo "noflip1: $(ls /private/tmp/n48m-noflip1 2>&1)"; echo "noflip2: $(ls /private/tmp/n48m-noflip2 2>&1)"; echo "newest reports:"; ls -lt /Library/Logs/DiagnosticReports 2>&1 | head -3' || stop 3 "system read failed"
need "console user is root (0 users)" has '^console: root$'
need "who lists nobody" bash -c '! printf "%s\n" "$1" | sed -n "/^who:/,/^WS:/p" | sed "1d;\$d" | /usr/bin/grep -q .' _ "$PXOUT"
need "boot-args carry ALL THREE latches navi48-m6=1 navi48-m6flip=1 navi48-m6flip1=1" bash -c 'for t in navi48-m6=1 navi48-m6flip=1 navi48-m6flip1=1; do printf "%s" "$1" | /usr/bin/grep -qE -- "(^| )$t( |$)" || exit 1; done' _ "$PXOUT"
need "boot-args carry navi48-fb2=1 navi48-metal-disp=1 navi48-disp2=1 navi48-dmubcmd=1" bash -c 'for t in navi48-fb2=1 navi48-metal-disp=1 navi48-disp2=1 navi48-dmubcmd=1; do printf "%s" "$1" | /usr/bin/grep -q -- "$t" || exit 1; done' _ "$PXOUT"
need "ESP names a kext matching /$WANT_KEXT_RE/" has "injected kext: $WANT_KEXT_RE"
need "bring-up kext /$WANT_KEXT_RE/ loaded" has "navi48.*$WANT_KEXT_RE|bringup.*$WANT_KEXT_RE"
need "bundle $WANT_BUNDLE installed" has "^bundle: $WANT_BUNDLE\$"
need "the Metal nub is NOT published yet (fresh boot: one run per boot)" has 'NO Navi48MetalNub'
need "the kill file $KILL2 is absent (the monitor B may be enabled)" has "^noflip2: ls: .*No such file"
need "the kill file $KILL1 is absent at P0 ($([ "$STAGE" = 2a ] && echo '2a touches it itself before the WS start' || echo '2b: the monitor A may be enabled'))" has "^noflip1: ls: .*No such file"
if [ "$DRYRUN" != 1 ]; then
  B1N=$(printf '%s' "$PXOUT" | sed -n 's/^b1token: //p' | head -1); GEON=$(printf '%s' "$PXOUT" | sed -n 's/^geomtoken: //p' | head -1); NF1N=$(printf '%s' "$PXOUT" | sed -n 's/^noflip1token: //p' | head -1)
  BUUID=$(printf '%s' "$PXOUT" | sed -n 's/^uuid: *//p' | head -1)
  say "  installed bundle binary: R-B1 token '$B1_TOKEN' x${B1N:-?} ; inst_geom_mismatch x${GEON:-?} ; n48m-noflip1 x${NF1N:-?} ; LC_UUID ${BUUID:-?}"
  { [ "${GEON:-0}" -ge 1 ] 2>/dev/null && [ "${NF1N:-0}" -ge 1 ] 2>/dev/null; } || stop 1 "the installed bundle binary lacks the Stage 2 strings (inst_geom_mismatch x${GEON:-?}, n48m-noflip1 x${NF1N:-?}): it is bundle < 15"
  if [ "${B1N:-0}" -ge 1 ] 2>/dev/null; then say "  R-B1 fix present in the installed bundle"
  elif [ "$STAGE" = 2b ]; then refuse "STAGE=2b needs the R-B1 fix (review blocker B1: an monitor A-sized surface planned tentatively to the DP is copied at the DP's size): the token '$B1_TOKEN' is NOT in $BUNDLE_BIN. Install bundle >= 17 (bundle 19 carries it) or run STAGE=2a."
  else WARN="$WARN
  R-B1 token missing from the installed bundle: Stage 2a is safe as built (the monitor A is OFF) but 2b would be refused"; fi
  [ -z "$WANT_BUNDLE_UUID" ] || need "bundle LC_UUID contains $WANT_BUNDLE_UUID" bash -c 'printf "%s" "$1" | /usr/bin/grep -qi -- "$2"' _ "$BUUID" "$WANT_BUNDLE_UUID"
fi
WS0=$(printf '%s' "$PXOUT" | sed -n 's/^WS: //p')
PRE_REPORT=$(printf '%s' "$PXOUT" | sed -n '/^newest reports:/,$p' | sed -n '2p')
say "  WindowServer pid at start: $WS0 ; newest report: $PRE_REPORT"
echo "$(date +%s) start $WS0" >"$RUN/ws-pids.txt"
# THE DP-RASTER CHECK FIRST (queue 292): a fresh user boot can come up with the DP in the 1080p fallback; detect it HERE, write nothing.
px dp-raster "sudo -n $T reg 0x4fea 2>&1" || stop 1 "reg 0x4fea unreadable"
if [ "$DRYRUN" != 1 ]; then
  printf '%s\n' "$PXOUT" >"$RUN/dp-raster.txt"; parse_reg "$RUN/dp-raster.txt"
  say "  DP OTG0_H_TOTAL (0x4fea) = 0x${RASTER_HEX:-?}   (full-res 1440p60 raster: $DP_H_TOTAL_OK ; the 1080p fallback of queue 292: $DP_H_TOTAL_FALLBACK)"
  [ -n "$RASTER_HEX" ] || stop 1 "could not parse reg 0x4fea ('$PXOUT')"
  [ "0x$RASTER_HEX" = "$DP_H_TOTAL_FALLBACK" ] && refuse "the DP came up in the 1080p FALLBACK (OTG0_H_TOTAL 0x4fea = $DP_H_TOTAL_FALLBACK, not $DP_H_TOTAL_OK): the PC needs a warm restart (queue 292). The monitor B chain was NOT run."
  [ "0x$RASTER_HEX" = "$DP_H_TOTAL_OK" ] || refuse "the DP raster OTG0_H_TOTAL is 0x$RASTER_HEX, neither the 1440p value $DP_H_TOTAL_OK nor the known 1080p fallback: the PC needs a warm restart / a look before the monitor B chain. Nothing was written."
else say "  DRYRUN would require: DP OTG0_H_TOTAL == $DP_H_TOTAL_OK, and REFUSE with 'needs a warm restart' on $DP_H_TOTAL_FALLBACK"; fi
px marker 'touch /tmp/n48-s2-marker; ls -l /tmp/n48-s2-marker' || stop 1 "marker touch failed"
# the aux matches on the Metal nub, so it is NOT loaded before P4's nub publish: here only the aux KC and the aux bundle's path/version are checked
px auxkc "kmutil inspect -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>&1 | grep -i accelprobe; echo version: \$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' $AUX_DIR/Contents/Info.plist 2>&1)" || stop 3 "aux KC read failed"
need "aux KC carries com.navi48.accelprobe $WANT_AUX" has "accelprobe.*$WANT_AUX|$WANT_AUX.*accelprobe"
need "aux $WANT_AUX is still at $AUX_DIR" has "^version: $WANT_AUX\$"
px pre-m6stat "sudo -n $T accel m6stat 0 2>&1; sudo -n $T accel m6stat 1 2>&1; sudo -n $T accel m6stat 2 2>&1; sudo -n $T accel m6stat 4 2>&1" || stop 1 "m6stat unreadable"
need "m6stat: navi48-m6 latch ON" has 'navi48-m6 latch ON'
need "m6stat: nothing adopted / armed yet (fresh boot)" has 'pipe adopted no; armed no'
need "m6stat: 0 pipes recorded before the recipe" has ' 0 pipes recorded'
need "m6stat parser control: instance 0 line present" has 'instance 0 \(DP, OTG0\) +: [0-9]+ transactions'
need "m6stat 4 exists in this CLI (per-instance AGDC counters)" has 'endpoint dwords seen \(bitmask 0x'
px pre-m6xstat "sudo -n $T accel m6xstat 2 0 2>&1; sudo -n $T accel m6xstat 1 0 2>&1" || stop 1 "m6xstat unreadable (the CLI predates action 107 / the 0.0.662 instance argument?)"
need "m6xstat 2: navi48-m6 ON, navi48-m6flip ON (the monitor B reachable)" has '^accel m6xstat 0: navi48-m6 ON, navi48-m6flip ON$'
need "m6xstat 1: all THREE latches ON (the monitor A reachable)" has '^accel m6xstat inst1 page 0: navi48-m6 ON, navi48-m6flip ON, navi48-m6flip1 ON$'
if [ "$DRYRUN" != 1 ]; then
  printf '%s\n' "$PXOUT" >"$RUN/pre-x.txt"
  awk '/^accel m6xstat inst1/{f=1} !f{print > "'"$RUN"'/pre-x2.txt"} f{print > "'"$RUN"'/pre-x1.txt"}' "$RUN/pre-x.txt"
  for i in 1 2; do parse_x0 "$RUN/pre-x$i.txt" $i; stash $i "$V0"
    say "  m6xstat $i 0 baseline: acquired=$X_ACQ acquires=$X_ACQUIRES A=0x$X_A B=0x$X_B watchdog=$X_WDOG reuse_refused=$X_REUSE restores=$X_RESTORES"
    [ "$X_ACQ" = no ] && [ -n "$X_WDOG" ] && [ -n "$X_REUSE" ] || stop 1 "m6xstat $i 0 parser control failed or instance $i is already acquired (acq='$X_ACQ')"
  done
fi
# the pool: `accel vramstat` (action 108) before anything runs
px pre-vram "sudo -n $T accel vramstat 2>&1" || stop 1 "vramstat failed (the CLI / kernel predate 0.0.663?)"
if [ "$DRYRUN" != 1 ]; then
  printf '%s\n' "$PXOUT" >"$RUN/pre-vram.txt"; parse_vram "$RUN/pre-vram.txt"
  [ -n "$V_FREE" ] && [ -n "$V_TOTAL" ] || stop 1 "vramstat: the allocator is not up or the output is unparsable ('$PXOUT')"
  case "$POOL" in small) POOLCLASS=SMALL ;; big) POOLCLASS=BIG ;; *) if [ "$V_TOTAL" -lt 400 ]; then POOLCLASS=SMALL; else POOLCLASS=BIG; fi ;; esac
  say "  POOL at P0: BAR0 mapped ${V_MAPPED:-?} MiB, visible pool ${V_TOTAL} MiB total, ${V_FREE} MiB free -> class $POOLCLASS ($([ "$POOLCLASS" = SMALL ] && echo 'expect ONE HDMI display to be refused at the first acquire when both are enabled; report which' || echo 'both HDMI displays must acquire'))"
  VRAM_P0=$V_FREE
else POOLCLASS=${POOL/auto/?}; say "  DRYRUN would classify the pool from vramstat: visible total < 400 MiB = SMALL (one HDMI display refused), else BIG (both must acquire); POOL=$POOL"; fi
# DP baseline (positive control: the DP-side line must parse)
px pre-dp "sudo -n $T accel disp2 status 2 2>&1" || stop 1 "disp2 status 2 failed"
if [ "$DRYRUN" != 1 ]; then printf '%s\n' "$PXOUT" >"$RUN/pre-dp.txt"; parse_dpside "$RUN/pre-dp.txt"; DP0="$DP_OTG0 $DP_BE"; else DP0=""; fi
say "  DP baseline (OTG0 state, DIG1_DIG_BE_CNTL): '$DP0'"
need "DP baseline parsed and OTG0 COUNTING" bash -c '[ "${1%% *}" = COUNTING ]' _ "$DP0"
DP_BE0=${DP0#* }; LAST_BE=$DP_BE0
echo "$(date +%s)" >"$RUN/t0"

# ---------------------------------------------------------------------------------------------- P2 the monitor B chain (exactly 1b), then the MONA chain; both BEFORE any hold
crc_read() { # crc_read <label> <inst> -> parses disp2 crc <inst> into the working CRC_* fields (and stashes them)
  px "$1" "sudo -n $T accel disp2 crc $2 2>&1"; local rc=$?
  # the monitor A's crc op can end in rc 4 (monitor B watch, AMBIGUITY 2) with the CRC printed: only an unreadable output is fatal here
  if [ "$DRYRUN" != 1 ]; then parse_crc "$PXF" $2; stash $2 "$VC"; [ -n "$CRC_RG_NOW" ] || stop 1 "$1: disp2 crc $2 printed no CRC line (rc $rc)"; fi
  say "  CRC$2 RG=${CRC_RG_NOW:-?} B=${CRC_B_NOW:-?} ; '${UF_LINE:-?}' ; ODM$2 occurred=${ODM_OCC:-?} ; VM fault=${VMF:-?} ; FIFO error=${FIFO_ERR:-?} ; monitor B-watch mask=${MONBW:-n/a} -> underflow-clean=${UF_OK:-?} DP-watch-clean=${DPW_OK:-?}"
}
same_as_a2() { [ "${CRC_RG_NOW:-}" = "$CRC_RG2" ] && [ "${CRC_B_NOW:-}" = "$CRC_B2" ]; }
say; say "=== P2 monitor B chain (Run B / runA657 order; every op: status 0 and the kext's DP plane watch unchanged; first failure stops)"
monbop() { # monbop <label> <accel args...>
  local lab=$1; shift
  px "$lab" "sudo -n $T accel $* 2>&1" || stop 1 "$lab: ssh/CLI failed"
  need "$lab: status 0 (OK)" has 'status +: 0 \(OK\)|status 0 \(OK\)'
  need "$lab: DP plane watch unchanged" has 'DP plane watch .*all 15 registers unchanged'
  need "$lab: no DISTURBED / VM FAULT text" bash -c '! printf "%s" "$1" | /usr/bin/grep -qE "DISTURBED|VM FAULT"' _ "$PXOUT"
}
monbop dmub-pclk dmubsend pclk otg2-1440-on
monbop disp2-timing1440 disp2 timing1440 2
monbop dmub-phyd dmubsend phyd enable-1440
monbop dmub-digd dmubsend digd setup-1440
monbop disp2-connect disp2 connect 2
nap 3
px scdc "sudo -n $T accel scdcread 3 0x40 1 2>&1" || stop 1 "scdcread failed"
need "SCDC: the sink LOCKS onto our TMDS" has 'LOCKS onto our TMDS'
monbop disp2-plane disp2 plane 2
monbop disp2-show disp2 show 2
crc_read crc-A2 2
need "monitor B CRC_A2 exact ($CRC_RG2 / $CRC_B2) BEFORE the holds" same_as_a2
need "monitor B underflow lines all 0 BEFORE the holds" [ "${UF_OK:-0}" = 1 ]

say; say "=== P2b the MONA chain (Run A / Run B order: dmubsend pclk otg1-on, phyc enable, digc setup; disp2 timing 1, connect 1, plane 1, show 1). After EVERY monitor A op: status 0, DP watch unchanged, 'monitor B watch ... unchanged', and the monitor B CRC still CRC_A2."
monaop() { # monaop <label> <accel args...>
  local lab=$1; shift
  local verb=$1
  px "$lab" "sudo -n $T accel $* 2>&1" || stop 1 "$lab: ssh/CLI failed"
  need "$lab: status 0 (OK)" has 'status +: 0 \(OK\)|status 0 \(OK\)'
  need "$lab: DP plane watch unchanged" has 'DP plane watch .*all 15 registers unchanged'
  need "$lab: no DISTURBED / VM FAULT text" bash -c '! printf "%s" "$1" | /usr/bin/grep -qE "DISTURBED|VM FAULT"' _ "$PXOUT"
  need "$lab: the monitor B was not touched (no 'monitor B watch: CHANGED' / 'THE MONB WAS DISTURBED')" bash -c '! printf "%s" "$1" | /usr/bin/grep -qE "monitor B watch: CHANGED|THE MONB WAS DISTURBED|MONB WATCH tripped|MONB DISTURBED"' _ "$PXOUT"
  [ "$verb" = disp2 ] && need "$lab: the kext's monitor B watch reports unchanged" has 'monitor B watch: unchanged|all nine rows unchanged'
  crc_read "monbcrc-after-$lab" 2
  need "monitor B CRC_A2 still exact after $lab" same_as_a2
  need "monitor B underflow lines still 0 after $lab" [ "${UF_OK:-0}" = 1 ]
}
monaop mona-pclk dmubsend pclk otg1-on
monaop mona-phyc dmubsend phyc enable
monaop mona-digc dmubsend digc setup
monaop mona-timing disp2 timing 1
monaop mona-connect disp2 connect 1
nap 2
monaop mona-plane disp2 plane 1
monaop mona-show disp2 show 1
# CRC_A1 MEASURED on this boot: three equal reads (e7a0b345 is only compared as information)
crc_read crc-A1a 1; nap 1; crc_read crc-A1b 1; nap 1; crc_read crc-A1c 1
if [ "$DRYRUN" != 1 ]; then
  CRC_A_RG_1=$(gv P1_CRC_RG_NOW); CRC_A_B_1=$(gv P1_CRC_B_NOW)
  F1=$(ls "$RUN"/*-crc-A1a.txt); F2=$(ls "$RUN"/*-crc-A1b.txt); F3=$(ls "$RUN"/*-crc-A1c.txt)
  x1=$(sed -n 's/.*OTG1 CRC *: \(.*\)/\1/p' "$F1" | head -1); x2=$(sed -n 's/.*OTG1 CRC *: \(.*\)/\1/p' "$F2" | head -1); x3=$(sed -n 's/.*OTG1 CRC *: \(.*\)/\1/p' "$F3" | head -1)
  [ -n "$x1" ] && [ "$x1" = "$x2" ] && [ "$x2" = "$x3" ] || stop 1 "the monitor A's CRC is not stable across 3 reads ('$x1' / '$x2' / '$x3'): CRC_A1 cannot be measured"
  say "  CRC_A1 (measured this boot) = $CRC_A_RG_1 / $CRC_A_B_1"
  [ "$CRC_A_RG_1" = "$CRC_RG1_INFO" ] && [ "$CRC_A_B_1" = "$CRC_B1_INFO" ] && say "  = the Run A / Run B value $CRC_RG1_INFO / $CRC_B1_INFO (information)" || WARN="$WARN
  monitor A CRC_A1 measured $CRC_A_RG_1/$CRC_A_B_1 differs from the Run A / Run B value $CRC_RG1_INFO/$CRC_B1_INFO (information only: the reference is THIS boot's reading)"
  need "monitor A underflow lines all 0 and DIG2 FIFO error 0 (CRC_A1 read)" bash -c '[ "$1" = 1 ] && [ "$2" = 0 ]' _ "${UF_OK:-0}" "${FIFO_ERR:-x}"
fi
crc_read crc-A2-after-chain 2
need "monitor B CRC_A2 exact after the whole monitor A chain" same_as_a2

# ---------------------------------------------------------------------------------------------- P3 holds + publishes (the monitor B's framebuffer must be REGISTERED before fbpublish 1: review S1)
say; say "=== P3 fbhold 2, fbhold 1 (each IRREVERSIBLE this boot: after the first NO dmubsend, no teardown), then fbpublish 2 -> WAIT for the monitor B's Navi48Framebuffer -> fbpublish 1"
px fbhold2 "sudo -n $T accel disp2 fbhold 2 2>&1" || stop 1 "fbhold 2 failed"
need "fbhold 2: status 0" has 'fbhold 2: status 0 \(OK\)'
crc_read crc-after-hold2 2
need "monitor B CRC_A2 after fbhold 2" same_as_a2
px fbhold1 "sudo -n $T accel disp2 fbhold 1 2>&1" || stop 1 "fbhold 1 failed"
need "fbhold 1: status 0" has 'fbhold 1: status 0 \(OK\)'
need "fbhold 1: the monitor B watch reported unchanged" has 'monitor B watch: unchanged|all nine rows unchanged'
crc_read crc-after-hold1 1
if [ "$DRYRUN" != 1 ]; then [ "$(gv P1_CRC_RG_NOW)" = "$CRC_A_RG_1" ] && [ "$(gv P1_CRC_B_NOW)" = "$CRC_A_B_1" ] || stop 1 "monitor A CRC left CRC_A1 after fbhold 1"; fi
crc_read crc-after-hold1-monb 2
need "monitor B CRC_A2 after fbhold 1" same_as_a2
fbnodes_wait() { # fbnodes_wait <count> : poll ioreg until <count> REGISTERED Navi48Framebuffer nodes exist (max 40 s); sets FB_IDS FB_N
  local want=$1 k=0
  if [ "$DRYRUN" = 1 ]; then say "--- DRYRUN: ssh $HOST 'ioreg -c Navi48Framebuffer -r -d1 -w0 | grep -E \"^\\+-o Navi48Framebuffer\"' polled every 2 s (max 40 s) until $want REGISTERED node(s)"; FB_IDS=""; FB_N=0; return 0; fi
  while [ $k -lt 20 ]; do
    ssh $SSHO "$HOST" 'ioreg -c Navi48Framebuffer -r -d1 -w0 | grep -E "^\+-o Navi48Framebuffer"' >"$RUN/fbnodes.tmp" 2>&1; parse_fbids "$RUN/fbnodes.tmp"
    [ "${FB_N:-0}" -ge "$want" ] && break; k=$((k+1)); sleep 2
  done
  say "  ioreg: ${FB_N:-0} registered Navi48Framebuffer node(s) after ~$((k*2)) s: ids ${FB_IDS:-none}"
  cp "$RUN/fbnodes.tmp" "$RUN/fbnodes-$want.txt"
  [ "${FB_N:-0}" = "$want" ] || stop 1 "expected $want registered Navi48Framebuffer node(s), saw ${FB_N:-0} ('${FB_IDS}')"
}
px fbpublish2 "sudo -n $T accel fbpublish 2 2>&1" || stop 1 "fbpublish 2 failed"
need "fbpublish 2: status 0 (the nub is published, Navi48DisplayIndex 1)" has 'fbpublish 2: status 0 \(OK'
fbnodes_wait 1
MONB_FBID=${FB_IDS%% *}
say "  the monitor B's Navi48Framebuffer is registered: entry ID $MONB_FBID (review S1: the monitor A is published only now)"
crc_read crc-after-publish2 2
need "monitor B CRC_A2 after fbpublish 2" same_as_a2
px fbpublish1 "sudo -n $T accel fbpublish 1 2>&1" || stop 1 "fbpublish 1 failed"
need "fbpublish 1: status 0 (the nub is published, Navi48DisplayIndex 2)" has 'fbpublish 1: status 0 \(OK'
fbnodes_wait 2
if [ "$DRYRUN" != 1 ]; then
  MONA_FBID=""; for x in $FB_IDS; do [ "$x" = "$MONB_FBID" ] || MONA_FBID=$x; done
  printf 'monb %s\nmona %s\n' "$MONB_FBID" "$MONA_FBID" >"$RUN/fb-ids.txt"
  say "  registry entry IDs: monitor B $MONB_FBID, monitor A ${MONA_FBID:-?}"
  if [ -n "$MONA_FBID" ] && [ $((MONB_FBID)) -lt $((MONA_FBID)) ]; then S1=PASS; S1w="(monitor B $MONB_FBID < monitor A $MONA_FBID)"; else S1=FAIL; S1w="(monitor B ${MONB_FBID:-?} vs monitor A ${MONA_FBID:-?}: the monitor B must register first)"; fi
  [ "$S1" = PASS ] || stop 1 "S1: the monitor B's Navi48Framebuffer entry ID is not lower than the monitor A's: $S1w (endpoint order = registration order is the assumption of the three-entry AGDC list)"
else S1="(dry run)"; fi
crc_read crc-after-publish1 1
if [ "$DRYRUN" != 1 ]; then [ "$(gv P1_CRC_RG_NOW)" = "$CRC_A_RG_1" ] && [ "$(gv P1_CRC_B_NOW)" = "$CRC_A_B_1" ] || stop 1 "monitor A CRC left CRC_A1 after fbpublish 1"; fi
crc_read crc-after-publish1-monb 2
need "monitor B CRC_A2 after fbpublish 1" same_as_a2

# ---------------------------------------------------------------------------------------------- P4 nub, aux, adopt, agdc, fbname, arm
say; say "=== P4 nub publish -> aux loaded -> 2 Navi48Framebuffer nodes -> pipeadopt (3 pipes) -> pipeagdc (nfb 3 after the restart) -> fbname -> headless flag -> [2a: noflip1] -> pipearm   (lines mirror arm-gpu-desktop.sh)"
px nubpublish "sudo -n $NUB publish 2>&1" || stop 1 "n48nub publish failed"
need "nub publish: 0 (Success)" has 'publish: 0 \(Success\)'
nap 3; px auxloaded "kmutil showloaded --list-only 2>/dev/null | grep -iE accelprobe" || true
need "aux $WANT_AUX loaded after the nub publish (kmutil showloaded)" has "accelprobe.*\\($WANT_AUX\\)"
nap 2
fbnodes_wait 2
if [ "$DRYRUN" != 1 ]; then
  for x in $MONB_FBID $MONA_FBID; do printf '%s\n' "$FB_IDS" | /usr/bin/grep -q -- "$x" || stop 1 "after the aux load the Navi48Framebuffer entry $x is gone / re-registered (ids now: $FB_IDS)"; done
fi
px pipeadopt "sudo -n $T accel pipeadopt 2>&1" || stop 1 "pipeadopt failed (a non-OK status is a CLEAN refusal: the pipe check is per pipe)"
need "pipeadopt: status 0 (OK) (accepted every pipe)" has 'status +: 0 \(OK\)'
need "pipeadopt: 3 pipes (ours / all = 3 / 3)" has 'pipes ours / all, display-machine starts / PCI substituted: 3 / 3,'
px pipeagdc "sudo -n $T accel pipeagdc 1 2>&1" || stop 1 "pipeagdc failed"
need "pipeagdc: agdc status 0 (PUBLISHED)" has 'agdc status +: 0 \(PUBLISHED\)'
need "pipeagdc not REFUSED (the display nubs' framebuffers have started)" bash -c '! printf "%s" "$1" | /usr/bin/grep -q REFUSED' _ "$PXOUT"
px fbname "sudo -n $T accel fbname 1 2>&1" || stop 1 "fbname failed"
need "fbname: class name NOW AMDRDNA4" has 'class name NOW +: AMDRDNA4'
px headless 'sudo -n touch /private/tmp/n48m-headless-no; sudo -n chmod 644 /private/tmp/n48m-headless-no; ls -l /private/tmp/n48m-headless-no' || stop 1 "headless flag failed"
if [ "$STAGE" = 2a ]; then
  px noflip1-touch "sudo -n touch $KILL1; sudo -n chmod 644 $KILL1; ls -l $KILL1" || stop 1 "could not touch $KILL1 (STAGE 2a)"
  need "STAGE 2a: $KILL1 present BEFORE the WindowServer start (the bundle reads it at init)" has "$KILL1"
fi
px pipearm "sudo -n $T accel pipearm 1 2>&1 | grep 0xccf" || stop 1 "pipearm failed"
need "pipearm: accel+0xccf now 1" has '0xccf now +: 1'
# last look before the restart: both CRCs exact, DP counting, nothing acquired
crc_read crc-before-restart2 2
need "monitor B CRC_A2 before the WindowServer restart" same_as_a2
crc_read crc-before-restart1 1
if [ "$DRYRUN" != 1 ]; then [ "$(gv P1_CRC_RG_NOW)" = "$CRC_A_RG_1" ] && [ "$(gv P1_CRC_B_NOW)" = "$CRC_A_B_1" ] || stop 1 "monitor A CRC left CRC_A1 before the restart"; fi
px pre-restart-state "sudo -n $T accel m6stat 0 2>&1; sudo -n $T accel m6xstat 2 0 2>&1; sudo -n $T accel m6xstat 1 0 2>&1; sudo -n $T accel pipestat 4 2>&1 | grep 'stamps written'; pgrep -x WindowServer; stat -f %Su /dev/console; who" || true
WSB=$(printf '%s' "$PXOUT" | /usr/bin/grep -E '^[0-9]+$' | head -1)
need "still 0 users right before the restart (console root, no who lines)" bash -c 'printf "%s\n" "$1" | /usr/bin/grep -qx root && ! printf "%s\n" "$1" | /usr/bin/grep -qE "^[a-z_][a-z0-9_]* +(console|ttys[0-9]+) "' _ "$PXOUT"
need "instance 2 not acquired before the restart (m6xstat)" has 'instance 2: acquired no,'
need "instance 1 not acquired before the restart (m6xstat)" has 'instance 1: acquired no,'

# ======================================================================== HOOK A (before the WindowServer restart)
review_checks_a() {
  say; say "=== REVIEW CHECKS A (before the restart): kext, three latches, both instances not acquired and HELD, CRC_A1 measured, plane baselines for BOTH instances, OTG frame counters moving, A1/B1/A2/B2/DP console disjoint, the monitor A chain ran before fbhold 2, the monitor B registered before fbpublish 1"
  px rca-kext 'kmutil showloaded --list-only 2>/dev/null | grep -iE "bringup"; sysctl -n kern.bootargs' || true
  need "the loaded bring-up kext matches /$WANT_KEXT_RE/" has "navi48.bringup.*\\($WANT_KEXT_RE\\)|bringup.*$WANT_KEXT_RE"
  need "all three latches in the live boot-args" bash -c 'for t in navi48-m6=1 navi48-m6flip=1 navi48-m6flip1=1; do printf "%s" "$1" | /usr/bin/grep -qE "(^| )$t( |$)" || exit 1; done' _ "$PXOUT"
  px rca-x0 "sudo -n $T accel m6xstat 1 0 2>&1; sudo -n $T accel m6xstat 2 0 2>&1" || stop 1 "m6xstat 0 failed"
  need "m6xstat 1 0: m6flip1 ON (all 3 latches) and acquired = no" bash -c 'printf "%s" "$1" | /usr/bin/grep -q "^accel m6xstat inst1 page 0: navi48-m6 ON, navi48-m6flip ON, navi48-m6flip1 ON$" && printf "%s" "$1" | /usr/bin/grep -q "instance 1: acquired no,"' _ "$PXOUT"
  need "m6xstat 2 0: latches ON and acquired = no" bash -c 'printf "%s" "$1" | /usr/bin/grep -q "^accel m6xstat 0: navi48-m6 ON, navi48-m6flip ON$" && printf "%s" "$1" | /usr/bin/grep -q "instance 2: acquired no,"' _ "$PXOUT"
  local i
  if [ "$DRYRUN" = 1 ]; then
    for i in 1 2; do px rca-i${i}-p2a "sudo -n $T accel m6xstat $i 2 2>&1"; done; nap 1
    for i in 1 2; do px rca-i${i}-p2b "sudo -n $T accel m6xstat $i 2 2>&1"; done
    px rca-status1 "sudo -n $T accel disp2 status 1 2>&1"; px rca-status2 "sudo -n $T accel disp2 status 2 2>&1"
    crc_read rca-crc1 1; crc_read rca-crc2 2
    say "  DRYRUN would require, per instance: plane baseline (underflow 0, BLANK_EN 0, VTG_SEL 1 monitor A / 2 monitor B, VM fault 0, ODM bit 10 0, FLIP_CONTROL bits 0/1/8 0, programmed = EARLIEST = A (this boot's A from disp2 crc), pitch 0x77f monitor A / 0x9ff monitor B, SURFACE_CONFIG 8, SURFACE_CONTROL 0, VMID 0, viewport 0x04380780 monitor A, stage 6 cur 0 pinned 1), OTG frame counter moving between two reads, HELD, A1/B1/A2/B2/DP console pairwise disjoint, CRC_A1 stable"
    px rca-leak "sudo -n $T log 2>&1 | grep -c LEAKED"; px rca-vram "sudo -n $T accel vramstat 2>&1"
    return 0
  fi
  local FC1_1 FC1_2 FC2_1 FC2_2
  for i in 1 2; do px rca-i${i}-p2a "sudo -n $T accel m6xstat $i 2 2>&1" || stop 1 "m6xstat $i 2 failed"; parse_x2 "$PXF" $i; stash $i "$V2"; sv FC1_$i "$X_OTGFC"; done
  nap 1
  for i in 1 2; do px rca-i${i}-p2b "sudo -n $T accel m6xstat $i 2 2>&1" || stop 1 "m6xstat $i 2 (second read) failed"; parse_x2 "$PXF" $i; stash $i "$V2"; sv FC2_$i "$X_OTGFC"; done
  px rca-status1 "sudo -n $T accel disp2 status 1 2>&1" || stop 1 "disp2 status 1 failed"
  need "disp2 status 1: instance 1's plane is HELD" has "instance 1's plane is HELD"
  px rca-status2 "sudo -n $T accel disp2 status 2 2>&1" || stop 1 "disp2 status 2 failed"
  need "disp2 status 2: instance 2's plane is HELD" has "instance 2's plane is HELD"
  crc_read rca-crc1 1; A_HELD_1=$(gv P1_CRC_PA); B_HELD_1=$(gv P1_CRC_PB)
  crc_read rca-crc2 2; A_HELD_2=$(gv P2_CRC_PA); B_HELD_2=$(gv P2_CRC_PB); DPCON=""
  [ -n "$DPRULE_HI" ] && [ -n "$DPRULE_LO" ] && DPCON=$(addr "$DPRULE_HI" "$DPRULE_LO")
  say "  A1 0x$A_HELD_1 B1 0x$B_HELD_1 ; A2 0x$A_HELD_2 B2 0x$B_HELD_2 ; DP console (HUBP0's live primary) ${DPCON:+0x}${DPCON:-?}"
  [ -n "$A_HELD_1" ] && [ -n "$A_HELD_2" ] && [ -n "$B_HELD_1" ] && [ -n "$B_HELD_2" ] || stop 1 "REVIEW CHECK A: A / B of the HELD planes not readable from disp2 crc"
  [ -n "$DPCON" ] || stop 1 "REVIEW CHECK A: the DP console MC (HUBP0 primary, the 'a RULE' row of disp2 crc) is not readable"
  for i in 1 2; do
    use $i; RC_BAD=""
    [ "$(gv P${i}_X_PA2)" = 0 ] || [ -z "$(gv P${i}_X_PA2)" ] || [ "$(gv P${i}_X_PA2)" = "$(gv A_HELD_$i)" ] || stop 1 "REVIEW CHECK A: m6xstat $i 2's A 0x$(gv P${i}_X_PA2) differs from disp2 crc $i's A 0x$(gv A_HELD_$i)"
    X_A=$(gv A_HELD_$i); rc_plane_pre $i
    local fa fb; fa=$(gv FC1_$i); fb=$(gv FC2_$i)
    [ "${X_STAGE:-x}" = 6 ] && [ "${X_CUR:-x}" = 0 ] && [ "${X_PIN:-x}" = 1 ] || rc_fail "instance $i's plane is not HELD on A and pinned (stage '${X_STAGE:-?}' cur '${X_CUR:-?}' pinned '${X_PIN:-?}')"
    [ -n "$fa" ] && [ -n "$fb" ] && [ "$fa" != "$fb" ] || rc_fail "the OTG$i frame counter is not moving ($fa -> $fb)"
    if [ "$i" = 1 ]; then [ "${X_VIEW:-x}" = 4380780 ] || rc_fail "viewport 0x${X_VIEW:-?} (want 0x04380780 = 1920x1080)"
    else [ "${X_VIEW:-x}" = 5a00a00 ] || WARN="$WARN
  monitor B viewport reads 0x${X_VIEW:-?}, expected 0x05a00a00 (2560x1440) - information"; fi
    say "  baseline instance $i: underflow=$X_UF BLANK_EN=$X_BLANK VTG_SEL=$X_VTG VMF=0x$X_VMF ODM$i-10=$X_ODM10 FLIP_CONTROL=0x$X_FCR pend=$X_FP programmed=0x$X_PROG EARLIEST=0x$X_EARLY A=0x$X_A pitch=0x$X_PITCH cfg=0x$X_SCFG ctl=0x$X_SCTL vmid=0x$X_VMID viewport=0x$X_VIEW stage=$X_STAGE cur=$X_CUR pinned=$X_PIN OTG$i frames $fa -> $fb"
    [ -z "$RC_BAD" ] || stop 1 "REVIEW CHECK A: instance $i plane baseline wrong:$RC_BAD"
  done
  disjoint_check
  say "  A1/B1/A2/B2 + DP console pairwise: $([ -z "$DJ_BAD" ] && echo disjoint || echo "OVERLAP:$DJ_BAD")"
  [ -z "$DJ_BAD" ] || stop 1 "REVIEW CHECK A: A1/B1/A2/B2/DP console overlap:$DJ_BAD"
  need "monitor B CRC = CRC_A2 (hook A)" same_as_a2_p
  need "monitor A CRC = this boot's CRC_A1 (hook A)" bash -c '[ "$1" = "$3" ] && [ "$2" = "$4" ]' _ "$(gv P1_CRC_RG_NOW)" "$(gv P1_CRC_B_NOW)" "$CRC_A_RG_1" "$CRC_A_B_1"
  need "the DP plane watch in disp2 crc 1 / 2 is unchanged (HUBP0 rule)" bash -c '[ "$1" = 1 ] && [ "$2" = 1 ]' _ "$(gv P1_DPW_OK)" "$(gv P2_DPW_OK)"
  px rca-leak "sudo -n $T log 2>&1 | grep -c LEAKED" || true
  LEAK_BASE=$(printf '%s' "$PXOUT" | /usr/bin/grep -E '^[0-9]+$' | tail -1); say "  kernel-log LEAKED lines at hook A (baseline for the abort rule and the after-kill checks): ${LEAK_BASE:-0}"; LEAK_BASE=${LEAK_BASE:-0}
  px rca-vram "sudo -n $T accel vramstat 2>&1" || true
  parse_vram "$PXF"; say "  FREE VISIBLE VRAM right before the restart: ${V_FREE:-?} MiB free of ${V_TOTAL:-?} MiB (P0 had ${VRAM_P0:-?}; the monitor A's and monitor B's A/B pairs are allocated now)"; VRAM_PRE=$V_FREE
  say "  hook A order facts: the monitor A chain ran before fbhold 2 (P2b < P3); the monitor B's Navi48Framebuffer was registered (id $MONB_FBID) before fbpublish 1 (monitor A id ${MONA_FBID:-?})."
}
same_as_a2_p() { [ "$(gv P2_CRC_RG_NOW)" = "$CRC_RG2" ] && [ "$(gv P2_CRC_B_NOW)" = "$CRC_B2" ]; }
review_checks_a

# ---------------------------------------------------------------------------------------------- P5 ONE WindowServer restart
say; say "=== P5 ONE WindowServer restart (pipereload opens the 15 s window; the kill follows in the SAME ssh at 0 users)"
cat >"$RUN/ep.d" <<'EPD'
pid$target::*GatherAGDCLogicalDeviceCapabilities*:entry {
  p = *(uint64_t*)copyin(arg0+0x20,8);
  printf("ep %u conn %u\n", *(uint32_t*)copyin(p+0x50,4), *(uint32_t*)copyin(p+0x20,4));
}
EPD
cat >"$RUN/n48watch.sh" <<'W'
#!/bin/bash
LOG=/var/tmp/n48watch.log; P=$(pgrep -x WindowServer); echo "v2 start $(date) ws=$P" >> $LOG
while true; do
  Q=$(pgrep -x WindowServer)
  if [ -n "$Q" ] && [ "$Q" != "$P" ]; then rm -f /private/tmp/n48m-headless-no /private/tmp/n48m-starts; echo "$(date) ws $P -> $Q: flag+counter removed" >> $LOG; P=$Q; fi
  sleep 0.5
done
W
if [ "$DRYRUN" = 1 ]; then say "--- DRYRUN: ssh $HOST 'cat > /tmp/ep.d' < ep.d (the Stage 0a endpoint probe) ; ssh $HOST 'cat > /tmp/n48watch.sh' < n48watch.sh (the watcher from arm-gpu-desktop.sh; placed BEFORE the restart)"
else
  ssh $SSHO "$HOST" 'cat > /tmp/ep.d' <"$RUN/ep.d" || say "  (S4) could not place ep.d: continuing without the probe (K7's ep.out leg will be NOT-OBSERVED)"
  ssh $SSHO "$HOST" 'cat > /tmp/n48watch.sh' <"$RUN/n48watch.sh" || stop 1 "could not place the watcher"
fi
px epstart "sudo -n sh -c 'nohup dtrace -Z -q -s /tmp/ep.d -W WindowServer -o /tmp/ep.out >/tmp/ep.err 2>&1 </dev/null &'; sleep 6; if pgrep -x dtrace >/dev/null; then echo 'ep.d: dtrace is waiting for the WindowServer exec'; else echo 'ep.d: dtrace is NOT running (could not attach); the run continues without it'; cat /tmp/ep.err; fi" || say "  (S4) dtrace start failed: continuing without the probe"
ws_restart() { # the ONE restart; sets RESTART_EPOCH (PC clock) WS1; any failure stops (no second attempt)
  px pcdate "date '+%Y-%m-%d %H:%M:%S'" || true
  RESTART_AT=$(printf '%s' "$PXOUT" | head -1)
  say "  PC clock at the restart: $RESTART_AT ; WindowServer pid before: ${WSB:-?}"
  px restart "who | grep -q . && { echo 'users logged in - refusing'; exit 9; }; r=\$(sudo -n $T accel pipereload 2>&1); echo \"\$r\" | grep -i 'restart window'; echo \"\$r\" | grep -q 'OPEN' || { echo 'restart window NOT open - not killing'; exit 8; }; sudo -n killall -9 WindowServer; echo killed-epoch=\$(date +%s)" || stop 1 "the restart command failed or refused (exit 9 = a user is logged in); NO second attempt"
  need "restart window was OPEN" has 'restart window +: OPEN'
  RESTART_EPOCH=$(printf '%s' "$PXOUT" | sed -n 's/^killed-epoch=\([0-9]*\).*/\1/p' | head -1)
  echo "$(date +%s) killed ${WSB:-?}" >>"$RUN/ws-pids.txt"
  WS1=""; local k=0
  while [ "$DRYRUN" != 1 ] && [ $k -lt 60 ]; do
    ssh $SSHO "$HOST" 'pgrep -x WindowServer' >"$RUN/wspid.tmp" 2>/dev/null; WS1=$(head -1 "$RUN/wspid.tmp")
    [ -n "$WS1" ] && [ "$WS1" != "${WSB:-}" ] && break; k=$((k+1)); sleep 1
  done
  say "  WindowServer pid after the restart: '$WS1' (was ${WSB:-?}) after ~${k}+ polls; kill epoch (PC) ${RESTART_EPOCH:-?}"
  need "WindowServer came back with a NEW pid" bash -c '[ -n "$1" ] && [ "$1" != "$2" ]' _ "$WS1" "${WSB:-}"
  echo "$(date +%s) restarted $WS1" >>"$RUN/ws-pids.txt"
  WSB=$WS1
  px watcher "pgrep -f n48watch.sh >/dev/null && echo 'watcher already running' || { sudo -n nohup bash /tmp/n48watch.sh >/dev/null 2>&1 </dev/null & sleep 1; }; sudo -n launchctl asuser 501 launchctl setenv CI_USE_MTL_DAG_FOR_CIKL_SRC 0 || echo 'note: launchctl asuser 501 setenv failed (no user domain yet?): the CIKL backstop is NOT in place'" || true
}
ws_restart

# ---------------------------------------------------------------------------------------------- P5b pre-flip
EXPECT="2"; [ "$STAGE" = 2b ] && EXPECT="2 1"
anyflip() { # 0 when an enabled HDMI instance is acquired or its CRC left CRC_A (2a: the monitor B only; an monitor A flip in 2a aborts in rc_every)
  local i
  for i in $EXPECT; do use $i; [ "$X_ACQ" = YES ] && return 0; [ -n "$CRC_RG_NOW" ] && ! crc_is_a $i && return 0; done
  return 1
}
stage2a_guard() { # 2a: the monitor A must stay un-acquired and on CRC_A1 at EVERY sample
  [ "$STAGE" = 2a ] || return 0; [ "$DRYRUN" = 1 ] && return 0
  use 1
  if [ "$X_ACQ" != no ] || { [ -n "$CRC_RG_NOW" ] && ! crc_is_a 1; }; then flip_abort "1" "STAGE 2a: the monitor A moved (acquired '$X_ACQ', CRC ${CRC_RG_NOW:-?}/${CRC_B_NOW:-?} vs CRC_A1 $CRC_A_RG_1/$CRC_A_B_1) although $KILL1 was present at the WindowServer start"; fi
}
vram_after_acquire() { # right after an instance's first acquire is DETECTED: one vramstat (TODO-VERB e: no per-acquire kernel figure)
  [ "$DRYRUN" = 1 ] && return 0
  local i
  for i in 1 2; do
    if [ -n "$(gv ACQ_T_$i)" ] && [ -z "$(gv VRAMACQ_DONE_$i)" ]; then
      sv VRAMACQ_DONE_$i 1; px vram-after-acq$i "sudo -n $T accel vramstat 2>&1" || true
      parse_vram "$PXF"; sv VRAM_AFTER_$i "$V_FREE"
      say "  FREE VISIBLE VRAM right after instance $i's acquire (+$(gv ACQSINCE_$i)s): ${V_FREE:-?} MiB (before the restart ${VRAM_PRE:-?} MiB)"
    fi
  done
}
say; say "=== P5b PRE-FLIP re-confirm of R0 (K1-K7 counters, both CRCs exact, underflow / FIFO 0): a fast sample at once, then a K-snapshot, then fast samples every ~${FAST_SLEEP} s until an enabled HDMI instance acquires / its CRC leaves CRC_A, a K-snapshot every ~10 s meanwhile"
FLIPPED=0; FLIP_SINCE=""
KPRE_TAKEN=0; LAST_K=0; FASTLOOP=0; PRE_STARTED=$(date +%s)
while :; do
  FASTLOOP=$((FASTLOOP+1))
  if [ "$FASTLOOP" -ge 2 ] && { [ $KPRE_TAKEN = 0 ] || [ $(( $(date +%s) - LAST_K )) -ge 10 ]; }; then
    ksnap pre $FASTLOOP; LAST_K=$(date +%s); KPRE_TAKEN=1
    [ "$DRYRUN" = 1 ] || { health_abort; stage2a_guard; review_checks_b; vram_after_acquire; }
    if [ "$DRYRUN" != 1 ] && [ "$(gv P1_X_ACQ)" = no ] && [ "$(gv P2_X_ACQ)" = no ] && [ "$CIA_1" = 1 ] && [ "$CIA_2" = 1 ]; then pre_k_eval; PRE_LAST_OK=$KPRE_OK; PRE_LAST_W=$KPRE_W; PRE_LAST_AT=$S_SINCE; fi
    if [ "$DRYRUN" != 1 ] && anyflip; then FLIPPED=1; FLIP_SINCE=$S_SINCE; break; fi
  else
    fast_sample pre
    [ "$DRYRUN" = 1 ] || { health_abort; stage2a_guard; vram_after_acquire; }
    if [ "$DRYRUN" != 1 ] && anyflip; then FLIPPED=1; FLIP_SINCE=$S_SINCE; break; fi
  fi
  [ "$DRYRUN" = 1 ] && break
  if [ $(( $(date +%s) - PRE_STARTED )) -ge "$PRE_MAX_S" ]; then break; fi
  sleep "$FAST_SLEEP"
done
if [ "$DRYRUN" = 1 ]; then say "  DRYRUN: the flip loop is not executed (it ends at the first sample showing an enabled HDMI instance acquired / its CRC != CRC_A, or after ${PRE_MAX_S} s)"
else
  say "  first flip detected: $([ $FLIPPED = 1 ] && echo "YES at +${FLIP_SINCE}s after the kill (PC clock)" || echo "NO within ${PRE_MAX_S} s")"
  # R0: evaluated on the LAST K-snapshot with nothing acquired and both CRCs exact (the pre-flip state)
  if [ -n "${PRE_LAST_OK:-}" ]; then R0=$([ "$PRE_LAST_OK" = 1 ] && echo PASS || echo FAIL); R0w="$PRE_LAST_W (the last K-snapshot before the first flip, at +${PRE_LAST_AT}s)"
  else R0="NOT-EVALUATED"; R0w="(no K-snapshot with nothing acquired and both CRCs exact existed before the first flip: it is re-read at the flip snapshot below)"; WARN="$WARN
  R0: no K-snapshot was taken before the first flip (flip +${FLIP_SINCE:-?}s); R0 falls back to the flip snapshot"; fi
  [ "$FLIPPED" = 1 ] || { C2p=FAIL; C2pw="(neither the monitor B's CRC left CRC_A nor an instance acquired for ${PRE_MAX_S} s after the restart: no frame reached an HDMI instance - Q1 (b)/(c)? or the bundle's instance is OFF: see the bundle log)"; stop 1 "no flip within ${PRE_MAX_S} s of the restart: C2' FAIL"; }
fi

pre_k_eval_flip() { # R0's fallback: the counters at the FLIP snapshot (transactions exist because the instances flipped; AGDC / endpoints must be there)
  local w="" i
  [ "${M_AGN:-0}" = 3 ] || w="$w AGDC nfb ${M_AGN:-?} (want 3);"; [ "${M_EPMASK:-x}" = 7 ] || w="$w endpoint bitmask 0x${M_EPMASK:-?} (want 0x7);"; [ "${M_EPOUT:-x}" = 0 ] || w="$w endpoints outside the list ${M_EPOUT:-?};"
  for i in 1 2; do [ "$(gv M_N921_$i)" -ge 1 ] 2>/dev/null || w="$w n921[$i] $(gv M_N921_$i);"; done
  for i in 0 1 2; do [ "$(gv M_TXN$i)" -gt 0 ] 2>/dev/null || w="$w txn[$i] $(gv M_TXN$i);"; done
  [ "${M_AMB:-1}" = 0 ] || w="$w ambiguous;"; [ "${M_NOFB:-1}" = 0 ] || w="$w nofb;"
  R0=$([ -z "$w" ] && echo PASS-AT-FLIP || echo FAIL); R0w="$w (evaluated at the FLIP snapshot, not before it; the CRC / underflow parts of R0 were checked at every pre-flip sample by health_abort)"
}
# ---------------------------------------------------------------------------------------------- P6 the flips: C1' / C2' per enabled instance (+ the pool verdict)
say; say "=== P6 the flips: keep sampling until every ENABLED HDMI instance ($EXPECT) is resolved (acquired, or a clean pool refusal) or ${MONA_WAIT_S} s after the restart; then C1' (3 slots, disjoint, outside A/B/DP) and C2' per instance"
OUTCOME_1="OFF-BY-KILLFILE"; OUTCOME_2=""; C1R_1=""; C1R_2=""; C2R_1=""; C2R_2=""; C1W_1=""; C1W_2=""; C2W_1=""; C2W_2=""
if [ "$DRYRUN" = 1 ]; then
  say "  DRYRUN: settle loop = K-snapshots (health_abort, stage2a_guard, review_checks_b, vram after each acquire) until each enabled instance is ACQUIRED or has a 'does not cover' pool line, or ${MONA_WAIT_S} s after the restart"
  say "  DRYRUN: C2' per acquired instance: first sample off its CRC_A at <= $FLIP_S s (PC clock), EARLIEST<i> in ITS 3 slots (m6xstat <i> 1), FLIP_PENDING not stuck (3 reads, same programmed/EARLIEST); 2a: the monitor A on CRC_A1 at every sample, never acquired, bundle 'scanout: instance 1 (the monitor A): OFF ... kill file PRESENT'"
  say "  DRYRUN: C1' per acquired instance: acquired YES, 3 distinct non-zero 64 KiB-aligned slot MCs with A's HIGH dword, none equal/overlapping A1/B1/A2/B2, the DP console or the DP slots (bundle 'scanout: slot N: ... MC'), the 9 slots pairwise disjoint, Acquire refused 0; a pool-refused instance: restores 1 VERIFIED, EARLIEST = A, CRC = its CRC_A, no LEAKED line"
  px acqlog "sudo -n log show --info --start '<restart>' --predicate '<scanout lines>' --style compact | grep WindowServer"
else
  DEADLINE=$(( RESTART_EPOCH + MONA_WAIT_S )); r=0
  while :; do
    r=$((r+1)); ksnap flip $r; health_abort; stage2a_guard; review_checks_b; vram_after_acquire
    resolved=1
    for i in $EXPECT; do
      use $i
      if [ "$X_ACQ" = YES ]; then :
      elif [ "$(gv EVER_POOLREF_$i)" = 1 ]; then :
      else resolved=0; fi
    done
    [ "$resolved" = 1 ] && break
    [ "${S_T:-0}" -ge "$DEADLINE" ] && break
    [ "$r" -ge 40 ] && break
    sleep 2
  done
  # let FLIP_PENDING settle on the acquired ones (up to 3 more K-snapshots, 2 s apart) as 1b does
  r2=0; while [ $r2 -lt 3 ]; do
    pend=0; for i in $EXPECT; do use $i; [ "$X_ACQ" = YES ] && [ "${X_FP:-0}" != 0 ] && pend=1; done
    [ $pend = 0 ] && break; r2=$((r2+1)); sleep 2; ksnap flip $((r+r2)); health_abort; stage2a_guard; review_checks_b; vram_after_acquire
  done
  # the DP's slots / console and every instance's bundle lines SINCE the restart (the 4-minute windows forget)
  px acqlog "sudo -n log show --info --start '$RESTART_AT' --predicate 'eventMessage CONTAINS \"scanout: query\" OR eventMessage CONTAINS \"scanout: slot\" OR eventMessage CONTAINS \"scanout (instance\" OR eventMessage CONTAINS \"scanout ACQUIRED\" OR eventMessage CONTAINS \"scanout: instance\"' --style compact 2>/dev/null | grep 'WindowServer\\[' | tail -80" || true
  printf '%s\n' "$PXOUT" >"$RUN/acqlog.txt"
  for i in 1 2; do
    parse_bundlelog "$RUN/acqlog.txt" $i
    [ "$(gv BL${i}_POOLREF)" -gt 0 ] 2>/dev/null && sv EVER_POOLREF_$i 1
    [ "$(gv BL${i}_GEOMWORD)" -gt 0 ] 2>/dev/null && sv EVER_GEOMWORD_$i 1
    [ "$(gv BL${i}_FAILCLOSED)" -gt 0 ] 2>/dev/null && sv EVER_FAILCLOSED_$i 1
    [ -n "$(gv BL${i}_FREE)" ] && { sv FREE_LAST_$i "$(gv BL${i}_FREE)"; sv NEED_LAST_$i "$(gv BL${i}_NEED)"; }
    [ -n "$(gv BL${i}_INIT)" ] && { sv INIT_$i "$(gv BL${i}_INIT)"; sv KILLFILE_$i "$(gv BL${i}_KILLFILE)"; }
    sv ACQLOGN_$i "$(gv BL${i}_ACQUIRED)"
    say "  bundle (instance $i): init '$(gv INIT_$i)' kill file '$(gv KILLFILE_$i)'; ACQUIRED lines $(gv ACQLOGN_$i); pool line free/need $(gv FREE_LAST_$i)/$(gv NEED_LAST_$i) MiB; pool refusals $(gv BL${i}_POOLREF); FAIL CLOSED $(gv BL${i}_FAILCLOSED); geometry-word refusals $(gv BL${i}_GEOMWORD); slots $(gv BL${i}_SLOTS)"
  done
  DPMC=$(printf '%s\n' "$PXOUT" | /usr/bin/grep 'scanout: slot' | sed -n 's/.*MC 0x\([0-9a-f]*\) (64 KiB.*/\1/p' | tr '\n' ' ')
  DPCONL=$(printf '%s\n' "$PXOUT" | /usr/bin/grep 'scanout: query' | sed -n 's/.*console MC 0x\([0-9a-f]*\).*/\1/p' | head -1)
  say "  DP slot MCs from the bundle log: ${DPMC:-NONE FOUND}; DP console MC (bundle): ${DPCONL:-n/a} (hook A read 0x${DPCON:-?} from HUBP0)"
  DPSLOTS=$DPMC
  [ -z "$DPCONL" ] || [ "$DPCONL" = "$DPCON" ] || WARN="$WARN
  DP console MC: bundle says 0x$DPCONL, HUBP0 read 0x${DPCON:-?} before the restart"
  [ -n "${DPCONL}" ] && DPCON=$DPCONL
  # ---- classify each instance and judge C1' / C2'
  for i in $EXPECT; do
    use $i; w=""; w2=""; nm=$(inst_name $i)
    if [ "$X_ACQ" = YES ]; then OUTCOME=ACQUIRED
    elif [ "$(gv EVER_POOLREF_$i)" = 1 ]; then OUTCOME=POOLREF
    else OUTCOME=NONE; fi
    sv OUTCOME_$i "$OUTCOME"
    case "$OUTCOME" in
    ACQUIRED)
      [ -n "$X_S0" ] && [ -n "$X_S1" ] && [ -n "$X_S2" ] || w="$w fewer than 3 slot MCs parsed;"
      [ "$X_S0" != "$X_S1" ] && [ "$X_S0" != "$X_S2" ] && [ "$X_S1" != "$X_S2" ] || w="$w slot MCs not distinct;"
      [ "$((0x${X_S0:-0}))" != 0 ] && [ "$((0x${X_S1:-0}))" != 0 ] && [ "$((0x${X_S2:-0}))" != 0 ] || w="$w a slot MC is 0;"
      for s in "$X_S0" "$X_S1" "$X_S2"; do [ "$s" != "$X_A" ] && [ "$s" != "$X_B" ] || w="$w slot 0x$s equals A (0x$X_A) or B (0x$X_B);"; done
      [ -n "${DPMC// /}" ] || w="$w the DP's slot MCs could not be read from the bundle log (cannot prove the $nm slots are outside them);"
      for s in "$X_S0" "$X_S1" "$X_S2"; do for d in $DPMC; do [ "$s" != "$d" ] || w="$w $nm slot 0x$s equals DP slot 0x$d;"; done; done
      [ "${X_ACQREF:-1}" = 0 ] || w="$w Acquire refused ${X_ACQREF:-?} time(s) (last reason ${X_LASTREASON:-?});"
      bs=$(gv BL${i}_SLOTS); ks="$X_S0 $X_S1 $X_S2"
      [ "$(printf '%s\n' $bs | sort | tr '\n' ' ')" = "$(printf '%s\n' $ks | sort | tr '\n' ' ')" ] || WARN="$WARN
  $nm slot MCs: kernel (m6xstat) '$ks' vs bundle log '$bs' differ (information: the bundle logs the BO MC, the kernel the registered MC)"
      [ "$(gv ACQLOGN_$i)" -ge 1 ] 2>/dev/null || WARN="$WARN
  $nm: no bundle line 'scanout (instance $i, $nm) ACQUIRED' in the log since the restart"
      f=$(gv FREE_LAST_$i); nd=$(gv NEED_LAST_$i); { [ -z "$f" ] || [ "$f" -ge "$nd" ]; } || w="$w the bundle's pool line says free $f < need $nd MiB but the instance was acquired;"
      # C2'
      of=$(gv OFFA_SINCE_$i)
      [ -n "$of" ] || w2="$w2 the CRC never left CRC_A$i;"
      [ -z "$of" ] || [ "$of" -le "$FLIP_S" ] || w2="$w2 the first sample off CRC_A$i was at +${of}s > ${FLIP_S} s;"
      crc_is_a $i && w2="$w2 the CRC is back at CRC_A$i in the settle snapshot;"
      in_set "$X_EARLY" "$X_S0" "$X_S1" "$X_S2" || w2="$w2 EARLIEST_INUSE$i 0x${X_EARLY:-?} is not one of its slots (0x$X_S0 0x$X_S1 0x$X_S2);"
      [ "${X_FP:-1}" = 0 ] || { rc_fp_stuck $i && w2="$w2 FLIP_PENDING$i stuck (3 reads, same programmed/EARLIEST);"; }
      sv C1W_$i "$w (slots 0x$X_S0 0x$X_S1 0x$X_S2; A 0x$X_A B 0x$X_B; DP slots ${DPMC:-?})"
      sv C2W_$i "$w2 (first off CRC_A$i at +${of:-?}s; EARLIEST 0x${X_EARLY:-?}; CRC ${CRC_RG_NOW:-?}/${CRC_B_NOW:-?}; acquired at +$(gv ACQSINCE_$i)s)"
      sv C1R_$i "$([ -z "$w" ] && echo PASS || echo FAIL)"; sv C2R_$i "$([ -z "$w2" ] && echo PASS || echo FAIL)"
      ;;
    POOLREF)
      # N1: a pool-refused instance logs ONE VERIFIED restore of A (the kernel Acquire succeeded before the bundle's verdict): expected, judged
      [ "${X_RESTORES:-x}" = 1 ] || w="$w restores ${X_RESTORES:-?} (N1 expects exactly 1 VERIFIED restore of A after a pool refusal);"
      [ "${X_RESFAIL:-x}" = 0 ] && [ "${X_RFAILED:-x}" = no ] || w="$w restore failures ${X_RESFAIL:-?} / restoreFailed ${X_RFAILED:-?};"
      RC_BAD=""; rc_plane_pre $i; [ -z "$RC_BAD" ] || w="$w the refused instance's plane is not back on A:$RC_BAD"
      crc_is_a $i || w="$w the CRC ${CRC_RG_NOW:-?}/${CRC_B_NOW:-?} != CRC_A$i;"
      f=$(gv FREE_LAST_$i); nd=$(gv NEED_LAST_$i); { [ -n "$f" ] && [ "$f" -lt "$nd" ]; } || w="$w the bundle's pool line does not show free < need (free '${f:-?}' need '${nd:-?}');"
      sv C1W_$i "$w (pool-REFUSED: free $f MiB < need $nd MiB; restores ${X_RESTORES:-?}; $nm stays on the bars A)"
      sv C1R_$i "$([ -z "$w" ] && echo POOLREFUSED || echo FAIL)"; sv C2R_$i "N/A"; sv C2W_$i "(pool-refused: static on A)"
      ;;
    *)
      sv C1W_$i "(instance $i was neither acquired nor pool-refused ${MONA_WAIT_S} s after the restart: no frame reached it, or it failed another way: FAIL CLOSED lines $(gv EVER_FAILCLOSED_$i), acquire refusals ${X_ACQREF:-?} last reason ${X_LASTREASON:-?})"
      sv C1R_$i FAIL; sv C2R_$i FAIL; sv C2W_$i "(never flipped)"
      ;;
    esac
    say "  C1' instance $i ($nm): $(gv OUTCOME_$i) -> $(gv C1R_$i) $(gv C1W_$i)"; say "  C2' instance $i ($nm): $(gv C2R_$i) $(gv C2W_$i)"
  done
  # the cross-instance disjointness over everything known now (also a hook-B check at every snapshot)
  disjoint_check; [ -z "$DJ_BAD" ] || { for i in $EXPECT; do sv C1R_$i FAIL; sv C1W_$i "$(gv C1W_$i) OVERLAP:$DJ_BAD"; done; }
  # ---- 2a: the monitor A must be OFF in the bundle and quiet; judged here and at every sample by stage2a_guard
  if [ "$STAGE" = 2a ]; then
    w=""
    [ "$(gv INIT_1)" = OFF ] && [ "$(gv KILLFILE_1)" = PRESENT ] || w="$w the bundle's init line for instance 1 says '$(gv INIT_1)' / kill file '$(gv KILLFILE_1)' (want OFF / PRESENT);"
    use 1; [ "$X_ACQ" = no ] || w="$w the monitor A is acquired ('$X_ACQ');"; crc_is_a 1 || w="$w the monitor A CRC left CRC_A1;"
    [ "$(gv BL1_ACQUIRED)" = 0 ] || w="$w the bundle logged an monitor A ACQUIRED line;"
    sv C1R_1 "$([ -z "$w" ] && echo STAYSOFF || echo FAIL)"; sv C1W_1 "$w (2a: the monitor A stays on the bars; init '$(gv INIT_1)', kill file '$(gv KILLFILE_1)')"; sv C2R_1 "$([ -z "$w" ] && echo PASS || echo FAIL)"; sv C2W_1 "$w (2a: monitor A on CRC_A1 at every sample so far)"
    say "  2a monitor A: $(gv C2R_1) $(gv C2W_1)"
  fi
  # ---- pool verdict (POOL class from vramstat at P0)
  n_acq=0; n_ref=0; n_none=0; refwho=""; w=""
  for i in $EXPECT; do case "$(gv OUTCOME_$i)" in ACQUIRED) n_acq=$((n_acq+1));; POOLREF) n_ref=$((n_ref+1)); refwho="$refwho $(inst_name $i)";; *) n_none=$((n_none+1));; esac; done
  n_exp=$(printf '%s\n' $EXPECT | wc -l | tr -d ' ')
  if [ "$POOLCLASS" = SMALL ]; then
    [ $n_acq -ge 1 ] && [ $n_none -eq 0 ] || w="$w SMALL pool: acquired $n_acq refused $n_ref none $n_none (want >= 1 acquired and every other a clean pool refusal);"
  else
    [ $n_acq -eq $n_exp ] || w="$w BIG pool: every enabled instance must acquire (acquired $n_acq of $n_exp; pool-refused:${refwho:- none}; none $n_none);"
  fi
  POOLV=$([ -z "$w" ] && echo PASS || echo FAIL)
  POOLVw="$w (pool class $POOLCLASS: visible total ${V_TOTAL:-?} MiB; free before the restart ${VRAM_PRE:-?} MiB; free after the acquires monitor A ${VRAM_AFTER_1:-n/a} monitor B ${VRAM_AFTER_2:-n/a} MiB; acquired $n_acq, pool-refused:${refwho:- none}; bundle pool lines free/need monitor A $(gv FREE_LAST_1)/$(gv NEED_LAST_1) monitor B $(gv FREE_LAST_2)/$(gv NEED_LAST_2) MiB)"
  say "  POOL $POOLV $POOLVw"
  # ---- C1'/C2' roll-up
  w=""; w2=""
  for i in $EXPECT; do
    case "$(gv C1R_$i)" in PASS|POOLREFUSED) ;; *) w="$w instance $i: $(gv C1W_$i);" ;; esac
    case "$(gv C2R_$i)" in PASS|N/A) ;; *) w2="$w2 instance $i: $(gv C2W_$i);" ;; esac
  done
  [ "$STAGE" != 2a ] || { [ "$(gv C2R_1)" = PASS ] || w2="$w2 monitor A (2a): $(gv C2W_1);"; }
  C1p=$([ -z "$w" ] && echo PASS || echo FAIL); C1pw="$w [monitor B: $(gv OUTCOME_2) $(gv C1R_2); monitor A: $([ "$STAGE" = 2a ] && echo "$(gv C1R_1) (2a)" || echo "$(gv OUTCOME_1) $(gv C1R_1)")]"
  C2p=$([ -z "$w2" ] && echo PASS || echo FAIL); C2pw="$w2 [monitor B $(gv C2R_2) first off CRC_A at +$(gv OFFA_SINCE_2)s; monitor A $(gv C2R_1) first off CRC_A at +$(gv OFFA_SINCE_1)s]"
  say "  C1' $C1p $C1pw"; say "  C2' $C2p $C2pw"
  # R0 fallback when no pre-flip snapshot existed
  if [ "$R0" = NOT-EVALUATED ]; then pre_k_eval_flip; fi
  [ "$R0" != FAIL ] || { FAILS="$FAILS
  R0 failed: $R0w"; }
fi

# ---------------------------------------------------------------------------------------------- P7 watch after the flips
STATIC_1=0; STATIC_2=0
if [ "$DRYRUN" != 1 ]; then
  [ "$STAGE" = 2a ] && STATIC_1=1; [ "$(gv OUTCOME_1)" = ACQUIRED ] || STATIC_1=1; [ "$(gv OUTCOME_2)" = ACQUIRED ] || STATIC_2=1
fi
for i in 1 2; do for v in EQ NEQ ARET STATBAD NSAMP; do sv ${v}_$i 0; done; sv PREV_CRC_$i ""; sv PREV_T_$i 0; sv REST_START_$i ""; sv LASTWD_$i 0; done
LAST_T=0; MINN=0
book_inst() { # book_inst <inst>: C3' bookkeeping on the instance's freshest sample (P<inst>_*, S_T fresh)
  local i=$1 cur prev pt; use $i
  sv NSAMP_$i $(( $(gv NSAMP_$i) + 1 ))
  if [ "$(gv STATIC_$i)" = 1 ]; then crc_is_a $i || sv STATBAD_$i $(( $(gv STATBAD_$i) + 1 )); return 0; fi
  [ -n "$(gv OFFA_SINCE_$i)" ] || return 0     # not flipped yet: nothing at rest to judge
  cur="$CRC_RG_NOW/$CRC_B_NOW"
  crc_is_a $i && sv ARET_$i $(( $(gv ARET_$i) + 1 ))
  [ -n "${X_WDOG:-}" ] && [ "$X_WDOG" != 0 ] && sv LASTWD_$i "$X_WDOG"
  prev=$(gv PREV_CRC_$i); pt=$(gv PREV_T_$i)
  if [ -n "$prev" ] && [ $(( S_T - pt )) -ge 8 ]; then
    if [ "$cur" = "$prev" ]; then sv EQ_$i $(( $(gv EQ_$i) + 1 )); [ -n "$(gv REST_START_$i)" ] || sv REST_START_$i "$pt"; else sv NEQ_$i $(( $(gv NEQ_$i) + 1 )); fi
    sv PREV_CRC_$i "$cur"; sv PREV_T_$i "$S_T"
  elif [ -z "$prev" ]; then sv PREV_CRC_$i "$cur"; sv PREV_T_$i "$S_T"; fi
}
say; say "=== P7 watch: ${WATCH_MIN} min after the flips (>= ${REST_MIN_S} s at rest needed for C3'); fast sample every ~$((FAST_SLEEP+4)) s, a full K-snapshot about every minute"
WATCH_START=$(date +%s); WATCH_S=$((WATCH_MIN*60)); LAST_FULL=0
if [ "$DRYRUN" = 1 ]; then say "  DRYRUN: the loop is not executed; per fast sample: health_abort (underflow / ODM bit 10 / VM fault / HUNG / WS pid change / OTG frame counter -> flip_abort of the affected instance(s)), rc_every per instance, CRC pair bookkeeping per instance (equal pairs, returns to CRC_A, restores); per K-snapshot: OTG0 COUNTING, per-display stamp floors, DP presents/s, review_checks_b (9 slots disjoint, cross-instance addresses, generations, LEAKED, geometry-word, inst_geom_mismatch rising, monitor B stall)"; fi
while [ "$DRYRUN" != 1 ] && [ $(( $(date +%s) - WATCH_START )) -lt "$WATCH_S" ]; do
  if [ $(( $(date +%s) - LAST_FULL )) -ge 60 ]; then
    MINN=$((MINN+1)); ksnap watch $MINN; LAST_FULL=$(date +%s); health_abort; stage2a_guard; ksnap_checks; floors $MINN; review_checks_b; vram_after_acquire
  else
    fast_sample watch; health_abort; stage2a_guard; vram_after_acquire
  fi
  book_inst 1; book_inst 2
  LAST_T=$S_T
  sleep "$FAST_SLEEP"
done

# ---------------------------------------------------------------------------------------------- P8 criterion 9' (optional) and the kill-switch legs C7' (END of the watch)
if [ "$USER_STEP" = 1 ]; then
  say; say "=== P8 criterion 9' (USER_STEP=1): ************  MOVE THE MOUSE ONTO THE MONA NOW  ************  sampling both CRCs every 2 s for 60 s"
  if [ "$DRYRUN" = 1 ]; then say "  DRYRUN: 30 samples: disp2 crc 1 and disp2 crc 2; PASS = the monitor A CRC changes from the first sample and its last 5 are equal while the monitor B CRC takes <= 3 values and its last 5 are equal"
  else
    sleep 3; U1_FIRST=""; U1_CHANGED=0; U1_LAST5=""; U2_SET=""; U2_LAST5=""; us=0
    while [ $us -lt 30 ]; do us=$((us+1))
      N=$((N+1)); UFL=$(printf '%s/%02d-ustep%02d.txt' "$RUN" "$N" "$us")
      ssh $SSHO "$HOST" "date +%s; sudo -n $T accel disp2 crc 1 2>&1 | grep 'OTG1 CRC'; sudo -n $T accel disp2 crc 2 2>&1 | grep 'OTG2 CRC'" >"$UFL" 2>&1
      c1=$(sed -n 's/.*OTG1 CRC *: RG \(0x[0-9a-f]*\) *B \(0x[0-9a-f]*\).*/\1\/\2/p' "$UFL" | head -1); c2=$(sed -n 's/.*OTG2 CRC *: RG \(0x[0-9a-f]*\) *B \(0x[0-9a-f]*\).*/\1\/\2/p' "$UFL" | head -1)
      [ -n "$U1_FIRST" ] || U1_FIRST=$c1; [ "$c1" = "$U1_FIRST" ] || U1_CHANGED=1
      U1_LAST5="$U1_LAST5 $c1"; U1_LAST5=$(printf '%s\n' $U1_LAST5 | tail -5 | tr '\n' ' '); U2_LAST5="$U2_LAST5 $c2"; U2_LAST5=$(printf '%s\n' $U2_LAST5 | tail -5 | tr '\n' ' '); U2_SET="$U2_SET $c2"
      say "  ustep $us: monitor A CRC ${c1:-?}  monitor B CRC ${c2:-?}"
      sleep 2
    done
    u1s=0; [ "$(printf '%s\n' $U1_LAST5 | sort -u | wc -l | tr -d ' ')" = 1 ] && [ "$(printf '%s\n' $U1_LAST5 | wc -l | tr -d ' ')" = 5 ] && u1s=1
    u2s=0; [ "$(printf '%s\n' $U2_LAST5 | sort -u | wc -l | tr -d ' ')" = 1 ] && [ "$(printf '%s\n' $U2_LAST5 | wc -l | tr -d ' ')" = 5 ] && u2s=1
    u2n=$(printf '%s\n' $U2_SET | sort -u | wc -l | tr -d ' ')
    if [ "$U1_CHANGED" = 1 ] && [ "$u1s" = 1 ] && [ "$u2s" = 1 ] && [ "$u2n" -le 3 ]; then C9p=PASS; C9pw="(the monitor A CRC changed and ended stable; the monitor B took $u2n value(s) and ended stable)"
    else C9p=NOT-OBSERVED; C9pw="(monitor A changed=$U1_CHANGED stable-at-end=$u1s; monitor B values=$u2n stable-at-end=$u2s: the pointer may not have reached the monitor A; not a failure of the stage)"; fi
  fi
else C9p="SKIPPED (USER_STEP=0)"; fi

leg() { # leg <killed inst> <other inst> <watch the other 0|1>: one kill-switch leg of C7'
  local k=$1 o=$2 wo=$3 w="" nmk nmo; nmk=$(inst_name $k); nmo=$(inst_name $o)
  say "  --- leg: touch $(kill_of $k) ($nmk): within 6 s EARLIEST$k = A$k and the $nmk CRC == CRC_A$k$([ "$wo" = 1 ] && echo "; WHILE EARLIEST$o stays one of the $nmo's slots and the $nmo CRC is NOT CRC_A$o"); the $nmk restore count +1, the $nmo's unchanged"
  if [ "$DRYRUN" = 1 ]; then px legpre$k "sudo -n $T accel m6xstat $k 0 2>&1"; px legpre$o "sudo -n $T accel m6xstat $o 0 2>&1"; kill_and_verify "$k" "$([ "$wo" = 1 ] && echo $o)"; review_checks_kill $k $o; return 0; fi
  px legpre$k "sudo -n $T accel m6xstat $k 0 2>&1" || true; parse_x0 "$PXF" $k; sv RES_BEFORE_$k "$X_RESTORES"
  px legpre$o "sudo -n $T accel m6xstat $o 0 2>&1" || true; parse_x0 "$PXF" $o; sv OTH_BEFORE_$o "$X_RESTORES"
  kill_and_verify "$k" "$([ "$wo" = 1 ] && echo $o)"
  review_checks_kill $k $o
  [ "$KILL_OK" = 1 ] || w="$w the $nmk did not return to A$k + CRC_A$k within 6 s (touch+${KILL_SECS:-never}s);"
  [ "${RCK:-FAIL}" = PASS ] || w="$w after-kill checks: ${RCKw};"
  if [ "$wo" = 1 ]; then
    [ "$(gv KV_OTH_INSLOT_$o)" = 1 ] || w="$w EARLIEST$o ($(gv KV_OTH_EARLY_$o)) is NOT one of the $nmo's slots after the $nmk leg (the $nmo stopped flipping: an instance swap or a shared fault?);"
    [ "$(gv KV_OTH_NOTA_$o)" = 1 ] || w="$w the $nmo CRC is back at CRC_A$o after the $nmk leg;"
  fi
  LEGW="$LEGW $nmk-leg: $([ -z "$w" ] && echo PASS || echo "FAIL$w") (touch+${KILL_SECS:-?}s);"
  [ -z "$w" ] || LEGBAD=1
}
LEGW=""; LEGBAD=0; KILL_RESULT_NOTE=""
if [ "$SKIP_KILL" = 1 ]; then C7p="SKIPPED (SKIP_KILL=1: the GPU desktop stays live on every display)"
else
  say; say "=== C7' the kill-switch legs at the END of the watch. AFTER THEM the killed display shows its bars A and its instance stays OFF until a reboot."
  if [ "$STAGE" = 2b ]; then
    if [ "$STATIC_1" = 1 ]; then LEGW="$LEGW monitor A-leg: N/A (the monitor A was pool-refused / not acquired);"; else leg 1 2 "$([ "$STATIC_2" = 1 ] && echo 0 || echo 1)"; fi
  else LEGW="$LEGW monitor A-leg: N/A (STAGE 2a: $KILL1 has been present since the start);"; fi
  if [ "$STATIC_2" = 1 ]; then LEGW="$LEGW monitor B-leg: N/A (the monitor B was pool-refused / not acquired);"; else leg 2 1 0; fi
  C7p=$([ "$LEGBAD" = 0 ] && echo PASS || { [ "$DRYRUN" = 1 ] && echo '(dry run)' || echo FAIL; }); C7pw="($LEGW)"
  KILL_RESULT_NOTE="the kill switches WERE used: the killed display(s) show the console buffer A; those instances are OFF for this WindowServer process"
fi

# ---------------------------------------------------------------------------------------------- P9 logs + final proofs (K1 K2 K3 K5 K7), verdicts
say; say "=== P9 logs since the restart (WindowServer log, bundle counters, displays in ioreg, m6stat final, crash reports, system_profiler)"
px wslog "sudo -n log show --info --start '$RESTART_AT' --predicate 'process == \"WindowServer\"' --style compact >/tmp/n48-s2-wslog.txt 2>&1; grep -E 'Unable to find display pipe|compositor activated' /tmp/n48-s2-wslog.txt | sed 's/^/WS: /'; echo \"nopipe-lines: \$(grep -c 'Unable to find display pipe' /tmp/n48-s2-wslog.txt)\"; wc -l /tmp/n48-s2-wslog.txt" || true
NOPIPE=$(printf '%s' "$PXOUT" | sed -n 's/^nopipe-lines: //p' | head -1)
ACT=$(printf '%s' "$PXOUT" | /usr/bin/grep -c 'compositor activated')
px bundlelog "sudo -n log show --info --start '$RESTART_AT' --predicate 'eventMessage CONTAINS \"scanout (instance\" OR eventMessage CONTAINS \"m6: surfaces\" OR eventMessage CONTAINS \"scanout: instance\"' --style compact 2>/dev/null | grep 'WindowServer\\[' | tail -120" || true
printf '%s\n' "$PXOUT" >"$RUN/bundlelog-final.txt"
FBC=""
if [ "$DRYRUN" != 1 ]; then for i in 1 2; do parse_bundlec "$RUN/bundlelog-final.txt" $i; parse_bundlelog "$RUN/bundlelog-final.txt" $i; done; fi
for i in 2 1; do say "  last instance-$i counters: presents $(gv B${i}_PRES) planned $(gv B${i}_PLAN) inst_mismatch $(gv B${i}_MISMATCH) inst_geom_mismatch $(gv B${i}_GEOMMIS) watchdog_restores $(gv B${i}_WDOG) gpu_fail $(gv B${i}_GPUFAIL) drops $(gv B${i}_DROPS)"; done
say "  bundle log lines naming a FAIL CLOSED / OFF: $(/usr/bin/grep -c 'FAIL CLOSED' "$RUN/bundlelog-final.txt")"
px displays 'ioreg -c IOFramebuffer -r -d1 -w0 | grep -E "^\+-o "; echo "Navi48Framebuffer nodes: $(ioreg -c Navi48Framebuffer -r -d1 -w0 | grep -cE "^\+-o Navi48Framebuffer")"' || true
FBNODES=$(printf '%s' "$PXOUT" | sed -n 's/^Navi48Framebuffer nodes: //p'); FBTOTAL=$(printf '%s' "$PXOUT" | /usr/bin/grep -cE '^\+-o ')
px fin-m6stat0 "sudo -n $T accel m6stat 0 2>&1" || true; cp "$PXF" "$RUN/fin.m0"
px fin-m6stat1 "sudo -n $T accel m6stat 1 2>&1" || true; cp "$PXF" "$RUN/fin.m1"
px fin-m6stat2 "sudo -n $T accel m6stat 2 2>&1" || true; cp "$PXF" "$RUN/fin.m2"
px fin-m6stat4 "sudo -n $T accel m6stat 4 2>&1" || true; cp "$PXF" "$RUN/fin.m4"
px fin-x2 "sudo -n $T accel m6xstat 2 0 2>&1; sudo -n $T accel m6xstat 2 1 2>&1; sudo -n $T accel m6xstat 2 2 2>&1" || true; cp "$PXF" "$RUN/fin.x2"
px fin-x1 "sudo -n $T accel m6xstat 1 0 2>&1; sudo -n $T accel m6xstat 1 1 2>&1; sudo -n $T accel m6xstat 1 2 2>&1" || true; cp "$PXF" "$RUN/fin.x1"
px finalstate "sudo -n $T accel sessstat 4 2>&1; pgrep -x WindowServer; find /Library/Logs/DiagnosticReports -newer /tmp/n48-s2-marker 2>/dev/null; echo \"noflip1: \$(ls $KILL1 2>&1)\"; echo \"noflip2: \$(ls $KILL2 2>&1)\"; echo LEAKED-FINAL: \$(sudo -n $T log 2>&1 | grep -c LEAKED)" || true
if [ "$DRYRUN" != 1 ]; then
  printf '%s\n' "$PXOUT" >"$RUN/final.txt"; parse_hung "$RUN/final.txt"
  FINAL_WS=$(printf '%s\n' "$PXOUT" | /usr/bin/grep -E '^[0-9]+$' | head -1)
  FINAL_REPORTS=$(printf '%s\n' "$PXOUT" | /usr/bin/grep 'DiagnosticReports' | tr '\n' ' ')
  FINAL_LEAK=$(sed -n 's/^LEAKED-FINAL: //p' "$RUN/final.txt" | head -1)
  parse_m6 "$RUN/fin.m0" "$RUN/fin.m1" "$RUN/fin.m2" "$RUN/fin.m4"
fi
px sysprof 'system_profiler SPDisplaysDataType 2>&1 | grep -E "Resolution|Displays:|Online|Main Display|Display Type"' || true
SPRES=$(printf '%s\n' "$PXOUT" | sed -n 's/.*Resolution: *\([0-9]*\) x \([0-9]*\).*/\1x\2/p' | tr '\n' ' ')
px post-driverlog "sudo -n $T log 2>&1 | tail -250" || true
collect_ep

say; say "=== P10 verdicts"
if [ "$DRYRUN" = 1 ]; then
  R0="(dry run)"; C1p="(dry run)"; C2p="(dry run)"; C3p="(dry run)"; C4p="(dry run)"; C5p="(dry run)"; C6p="(dry run)"; C10="(dry run)"; POOLV="(dry run)"; K1="(dry run)"; K2="(dry run)"; K3="(dry run)"; K5="(dry run)"; K7="(dry run)"
  say "  DRYRUN: no verdicts are computed; the rules are in the header (R0, K1-K7, C1'-C10, S1, POOL)"
  summary_and_exit 0
fi
# ---- C3' per instance
w=""; for i in 2 1; do
  nm=$(inst_name $i)
  if [ "$(gv STATIC_$i)" = 1 ]; then
    [ "$(gv STATBAD_$i)" = 0 ] && [ "$(gv NSAMP_$i)" -ge 1 ] || w="$w $nm (static on A): CRC left CRC_A$i at $(gv STATBAD_$i) of $(gv NSAMP_$i) samples;"
    [ "$(gv P${i}_X_WDOG)" = 0 ] || w="$w $nm watchdog restores $(gv P${i}_X_WDOG);"
  else
    rs=0; [ -n "$(gv REST_START_$i)" ] && rs=$(( LAST_T - $(gv REST_START_$i) ))
    [ "$(gv EQ_$i)" -ge 1 ] || w="$w $nm: no two CRCs ~10 s apart were equal (equal pairs $(gv EQ_$i), unequal $(gv NEQ_$i));"
    [ "$(gv ARET_$i)" = 0 ] || w="$w $nm: the CRC returned to CRC_A$i at $(gv ARET_$i) sample(s) after the flip;"
    [ "$(gv P${i}_X_WDOG)" = 0 ] && [ "$(gv LASTWD_$i)" = 0 ] || w="$w $nm: watchdog restores $(gv P${i}_X_WDOG) (max seen $(gv LASTWD_$i));"
    [ "$(gv B${i}_WDOG)" = 0 ] || [ -z "$(gv B${i}_WDOG)" ] || w="$w $nm: the bundle counter shows watchdog_restores $(gv B${i}_WDOG);"
    [ "$rs" -ge "$REST_MIN_S" ] || w="$w $nm: only ${rs} s at rest (< $REST_MIN_S);"
  fi
done
C3p=$([ -z "$w" ] && echo PASS || echo FAIL); C3pw="$w [monitor B: $([ "$STATIC_2" = 1 ] && echo static || echo "equal pairs $(gv EQ_2) unequal $(gv NEQ_2)"); monitor A: $([ "$STATIC_1" = 1 ] && echo "static, off-A samples $(gv STATBAD_1)/$(gv NSAMP_1)" || echo "equal pairs $(gv EQ_1) unequal $(gv NEQ_1)")]"
# ---- C4' (aborts already stopped the run; here the final reads of the last sample)
if [ "$C4p" = PENDING ]; then
  w=""; for i in 1 2; do [ "$(gv P${i}_X_UF)" = 0 ] || w="$w HUBP$i underflow $(gv P${i}_X_UF) in the last sample;"; done; [ "${HUNG:-x}" = 0 ] || w="$w HUNG '${HUNG:-?}';"
  C4p=$([ -z "$w" ] && echo PASS || echo FAIL); C4pw="$w (every fast sample ~$((FAST_SLEEP+4)) s + every minute: HUBP1/2 underflow, ODM1/2 bit 10, VM fault, HUNG, OTG counters; samples.tsv)"
fi
# ---- C5'
w=""; for i in 1 2; do
  [ -n "$(gv P${i}_X_REUSE)" ] || w="$w instance $i reuse_inuse_refused not parsed;"
  if [ "$(gv STATIC_$i)" = 0 ]; then [ -n "$(gv B${i}_MISMATCH)" ] && [ -n "$(gv B${i}_GEOMMIS)" ] || w="$w instance $i inst_mismatch / inst_geom_mismatch not parsed (no counters line);"; fi
  [ "$(gv P${i}_X_REUSE)" = 0 ] || [ -z "$(gv P${i}_X_REUSE)" ] || WARN="$WARN
  instance $i REUSE_INUSE_REFUSED = $(gv P${i}_X_REUSE) (the bundle asked to reuse a slot the hardware still fetched; the kernel refused: no in-use slot was overwritten, but the bundle should not ask)"
done
C5p=$([ -z "$w" ] && echo PASS || echo FAIL); C5pw="$w (REUSE_INUSE_REFUSED monitor A $(gv P1_X_REUSE) monitor B $(gv P2_X_REUSE); inst_mismatch monitor A $(gv B1_MISMATCH) monitor B $(gv B2_MISMATCH); inst_geom_mismatch monitor A $(gv B1_GEOMMIS) monitor B $(gv B2_GEOMMIS); presents monitor A $(gv B1_PRES) monitor B $(gv B2_PRES); actual in-use reuse is not directly measurable)"
# ---- C6'
if [ "$C6p" = PENDING ]; then C6p=PASS; fi
[ "$FLOOR_ANY_ACTIVE" = 1 ] || WARN="$WARN
  no display was ACTIVE (>= $ACT_MIN stamps/s) at watch minute 1: the per-display floors were vacuous (stamps/s baselines DP $(gv BASE_0) monitor A $(gv BASE_1) monitor B $(gv BASE_2))"
[ -z "$FLOORWARN" ] || WARN="$WARN$FLOORWARN"
C6pw="$C6pw (baselines stamps/s DP $(gv BASE_0)$([ "$(gv ACTIVE_0)" = 1 ] && echo ' active' || echo ' idle') monitor A $(gv BASE_1)$([ "$(gv ACTIVE_1)" = 1 ] && echo ' active' || echo ' idle') monitor B $(gv BASE_2)$([ "$(gv ACTIVE_2)" = 1 ] && echo ' active' || echo ' idle'); DP presents/s baseline ${BASEP_0:-n/a}; monitor B-watch row-6 benign events $MONBW6_N; monitor B stall check $([ "$MONB_STALL_DONE" = 1 ] && echo done || echo 'not evaluated (monitor A never acquired or the monitor B idle)'); DP pixels are NOT CRC-checked: judge by eye)"
# ---- C10
w=""; [ "${FINAL_WS:-}" = "$WS1" ] || w="$w WindowServer pid ${FINAL_WS:-?} != $WS1;"; [ "${HUNG:-x}" = 0 ] || w="$w HUNG '${HUNG:-?}';"; [ -z "${FINAL_REPORTS// /}" ] || w="$w new crash reports since the marker: $FINAL_REPORTS;"
C10=$([ -z "$w" ] && echo PASS || echo FAIL); C10w="$w (WS pid $WS1 -> ${FINAL_WS:-?})"
# ---- K1 K2 K3 K5 K7
w=""
[ "${NOPIPE:-1}" = 0 ] || w="$w WS logged 'Unable to find display pipe' ${NOPIPE:-?} times;"
[ "${ACT:-0}" = 3 ] || w="$w 'compositor activated' lines ${ACT:-?} (want 3);"
[ "${FBNODES:-0}" = 2 ] || w="$w Navi48Framebuffer nodes ${FBNODES:-?} (want 2);"
[ "${FBTOTAL:-0}" = 3 ] || w="$w ioreg IOFramebuffer nodes ${FBTOTAL:-?} (want 3);"
K1=$([ -z "$w" ] && echo PASS || echo FAIL); K1w="$w (WS log nopipe ${NOPIPE:-?}; 'compositor activated' ${ACT:-?}; IOFramebuffer ${FBTOTAL:-?}; AGDC nfb ${M_AGN:-?})"
w=""; for i in 0 1 2; do [ "$(gv M_TXN$i)" -gt 0 ] 2>/dev/null || w="$w instance $i has $(gv M_TXN$i) transactions;"; done
K2=$([ -z "$w" ] && echo PASS || echo FAIL); K2w="$w (transactions DP ${M_TXN0:-?} monitor A ${M_TXN1:-?} monitor B ${M_TXN2:-?})"
w=""; for i in 0 1 2; do v=$(gv M_IDS$i); { [ "${v:-0}" -ge 2 ] && [ "${v:-0}" -le "$IDS_MAX" ]; } 2>/dev/null || w="$w instance $i IDs ${v:-?} (want 2-$IDS_MAX);"; [ "${v:-0}" -le 3 ] 2>/dev/null || WARN="$WARN
  instance $i learned $v surface IDs (> 3; the spec says 2-3, real runs show 4 on the monitor B)"; done
[ "${M_AMB:-1}" = 0 ] && [ "${M_NOFB:-1}" = 0 ] && [ "${M_AMB2:-1}" = 0 ] || w="$w ambiguous ${M_AMB:-?}/${M_AMB2:-?} or no-fb ${M_NOFB:-?};"
K3=$([ -z "$w" ] && echo PASS || echo FAIL); K3w="$w (IDs DP ${M_IDS0:-?} monitor A ${M_IDS1:-?} monitor B ${M_IDS2:-?}; bound 2-$IDS_MAX)"
w=""; [ "${M_STAMP1:-0}" -gt 0 ] 2>/dev/null || w="$w monitor A stamps written ${M_STAMP1:-?} (want > 0);"; [ "${M_REF1:-1}" = 0 ] || w="$w monitor A stamps refused ${M_REF1:-?} (want 0);"
[ "${M_REF0:-0}" = 0 ] && [ "${M_REF2:-0}" = 0 ] || WARN="$WARN
  vblank stamps refused: DP ${M_REF0:-?} monitor B ${M_REF2:-?} (information)"
K5=$([ -z "$w" ] && echo PASS || echo FAIL); K5w="$w (stamps written DP ${M_STAMP0:-?} monitor A ${M_STAMP1:-?} monitor B ${M_STAMP2:-?}; refused monitor A ${M_REF1:-?})"
w=""
[ "${M_AGN:-0}" = 3 ] && [ "${M_NFB4:-0}" = 3 ] || w="$w nfb m6stat1 ${M_AGN:-?} / m6stat4 ${M_NFB4:-?} (want 3);"
[ "${M_EPMASK:-x}" = 7 ] || w="$w endpoint bitmask 0x${M_EPMASK:-?} (want exactly 0x7);"
[ "${M_EPOUT:-x}" = 0 ] || w="$w endpoints outside the list ${M_EPOUT:-?};"
for i in 1 2; do [ "$(gv M_N921_$i)" -ge 1 ] 2>/dev/null || w="$w 0x921 answered $(gv M_N921_$i) times for instance $i;"; done
for i in 1 2; do [ "$(gv B${i}_GEOMMIS)" = 0 ] || [ -z "$(gv B${i}_GEOMMIS)" ] || w="$w inst_geom_mismatch of instance $i = $(gv B${i}_GEOMMIS) (want 0);"; done
EPS=$(sed -n 's/^ep \([0-9]*\) conn.*/\1/p' "$RUN/ep.out" 2>/dev/null | sort -un | tr '\n' ' ')
if [ -n "$EPS" ]; then [ "$EPS" = "0 1 2 " ] || w="$w ep.out endpoints seen '$EPS' (want exactly '0 1 2');"; else WARN="$WARN
  K7: ep.out holds no 'ep N conn M' lines (the dtrace probe did not run or saw nothing): the live m6stat 4 bitmask was used instead"; fi
n1920=$(printf '%s\n' $SPRES | /usr/bin/grep -c '^1920x1080$'); n2560=$(printf '%s\n' $SPRES | /usr/bin/grep -c '^2560x1440$'); nsp=$(printf '%s\n' $SPRES | wc -l | tr -d ' ')
if [ "$nsp" -ge 3 ]; then
  { [ "$n1920" = 1 ] && [ "$n2560" -ge 2 ]; } || w="$w system_profiler sizes '$SPRES' (want one 1920x1080 = the monitor A and two 2560x1440 = the DP and the monitor B: a wrong size is a SWAP signature);"
else WARN="$WARN
  K7: system_profiler listed $nsp display(s) ('$SPRES'), not 3 (Run B's review: the criterion is WindowServer drawing, not system_profiler)"; fi
K7=$([ -z "$w" ] && echo PASS || echo FAIL); K7w="$w (nfb ${M_AGN:-?}/${M_NFB4:-?}; endpoint bitmask 0x${M_EPMASK:-?}; ep.out '${EPS:-none}'; n921 monitor A $(gv M_N921_1) monitor B $(gv M_N921_2); out-of-range ${M_EPOUT:-?}; inst_geom_mismatch monitor A $(gv B1_GEOMMIS) monitor B $(gv B2_GEOMMIS); system_profiler '${SPRES:-none}')"
[ "${FINAL_LEAK:-0}" = "$LEAK_BASE" ] || WARN="$WARN
  LEAKED lines in the kernel log: ${FINAL_LEAK:-?} at the end vs $LEAK_BASE at hook A"
say "  R0 $R0 $R0w"; say "  C1' $C1p $C1pw"; say "  POOL $POOLV $POOLVw"; say "  C2' $C2p $C2pw"; say "  C3' $C3p $C3pw"; say "  C4' $C4p $C4pw"; say "  C5' $C5p $C5pw"; say "  C6' $C6p $C6pw"; say "  C7' $C7p $C7pw"; say "  C9' $C9p $C9pw"; say "  C10 $C10 $C10w"; say "  S1 $S1 $S1w"
say "  K1 $K1 $K1w"; say "  K2 $K2 $K2w"; say "  K3 $K3 $K3w"; say "  K5 $K5 $K5w"; say "  K7 $K7 $K7w"
ALL=PASS
for v in "$R0" "$C1p" "$POOLV" "$C2p" "$C3p" "$C4p" "$C5p" "$C6p" "$C7p" "$C10" "$S1" "$K1" "$K2" "$K3" "$K5" "$K7"; do case "$v" in PASS|PASS-AT-FLIP|SKIPPED*) ;; *) ALL=FAIL ;; esac; done
if [ "$ALL" = PASS ]; then
  say "### STAGE $STAGE: PASS (R0, K1-K7, C1'-C7', C10, S1, POOL$([ "$C9p" = PASS ] && echo ", C9'")). $([ -n "$KILL_RESULT_NOTE" ] && echo "$KILL_RESULT_NOTE." || echo 'PC left PARKED with the GPU desktop live on every display.') 0 users. No teardown sent."; summary_and_exit 0
else FAILS="$FAILS
  one or more criteria FAILED (see above); the evidence is in $RUN"; capture; say "### STAGE $STAGE: FAIL. PC left PARKED (no teardown verbs)."; summary_and_exit 1; fi
