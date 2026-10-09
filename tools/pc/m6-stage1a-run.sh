#!/bin/bash
# m6-stage1a-run.sh - M6 STAGE 1a RUN: two displays (DP + monitor B) on IOPresentment, pipes complete, NO monitor B pixels from the GPU.
# RUN ON THE AIR; drives the PC over ssh (host alias `navi48`) ONLY when the brief grants PC operation (pc-protocol.md). `DRYRUN=1` prints every ssh command, runs none.
# Spec: an internal design note "### Stage 1a". Procedures mirrored from queue 285 (Stage 0b), 287 (Run B), arm-gpu-desktop.sh (the arm lines), display chain from Run B's log
# (notes/logs/runs/runB-658/runB658.log). Needs: the PC deployed by m6-stage1a-deploy.sh (kext 0.0.659, aux 0.0.7, bundle 11, variant ...-amfi-m6, latch navi48-m6=1) on a FRESH BOOT
# (one run per boot; 0 console users; not armed; nothing published).
#
# RECIPE (the spec's order, with the monitor B chain first):
#   P0 prechecks (0 users, versions, latch, fresh boot, positive control for every parser)       P1 DP baseline (disp2 status 2)
#   P2 monitor B chain: pclk otg2-1440-on, timing1440 2, phyd enable-1440, digd setup-1440, connect 2, SCDC lock, plane 2, show 2   -> CRC_A, underflow 0 BEFORE
#   P3 fbhold 2 (IRREVERSIBLE this boot; after it NO dmubsend of any kind), fbpublish 2 (the monitor B's nub; the aux framebuffer starts; 1 Navi48Framebuffer node)
#   P4 nub publish -> pipeadopt (every pipe accepted; status 0) -> pipeagdc 1 (nfb comes from the published nubs = 2) -> fbname 1 -> headless flag -> pipearm 1
#   P5 ONE WindowServer restart (pipereload + killall -9 in one ssh, at 0 users) + the watcher + the CIKL backstop (as arm-gpu-desktop.sh)
#   P6 timed watch (WATCH_MIN minutes, default 10): per minute monitor B CRC + underflow, DP stamps/s, WS pid, 0 users, m6stat 0/1/2, DP side; then the bundle log and the WS log
#   P7 PASS/FAIL per criterion (K1..K6 below), logs, exit 0 (PASS) / 1 (FAIL) / 2 (precheck refused) / 3 (PC unreachable).
# ON ANY FAILURE: stop, capture (m6stat 0/1/2, pipestat 0-4, disp2 status 2, crc, driver log, WS + bundle logs, ioreg, DiagnosticReports), LEAVE THE PC PARKED. This script has NO
# teardown verbs (no pipearm 0, no disp2 off/planeoff, no pipeagdc 0, no killall except the ONE restart in P5): the review has not approved any.
#
# ROLLBACK (documented, not automated): reboot into the ms-apps variant and re-arm:
#   ssh navi48 '~/navi48-staging/esp-config.sh stage17-native-1440-metal-disp-amfi-ms-apps && ~/navi48-staging/reboot-pc.sh'   (0 users; reboot-pc.sh gates)
#   tools/pc/wait-for-driver.sh 300 ; then ON THE PC at 0 users: bash arm-gpu-desktop.sh (copy tools/pc/arm-gpu-desktop.sh to ~/ first; never on an fb2/m6 boot).
# STOP RULES: PC unreachable after one more check -> "PC needs a manual reset"; two panics in a row on this build -> stop; a user logging in during the watch -> stop and report.
#
# PASS CRITERIA (spec) -> how this script measures each:
#   K1 WindowServer lists 2 displays, both on IOPresentment : (N3) "AGDC list built" = m6stat page 1 "AGDC: 2 framebuffers in the last 0x980 reply" is a PRECONDITION only (AGDC::start's own 0x980 sets it; it does not prove
#      WS asked). The proof is the WS log (0 "Unable to find display pipe") + both pipes' transactions > 0 + the ioreg display count (1 Navi48Framebuffer node; the IOFramebuffer node total is printed, != 2 is a WARNING). Formerly: AGDC AND
#      m6stat page 0 transactions > 0 on instance 0 AND instance 2 (both pipes submitted) AND 1 Navi48Framebuffer node + the RDNA4FB node in ioreg AND the WindowServer log
#      since the restart has 0 "Unable to find display pipe" lines. (system_profiler is NOT used: queue 287.) "pipes recorded" is printed; != 2 is a WARNING, not a verdict.
#   K2 per-pipe transaction counts > 0 : m6stat page 0 transactions for instance 0 and instance 2 > 0 at the last sample; instance 0 must also still be growing between minute 1 and the last.
#   K3 3 surface IDs per pipe, 0 ambiguous : m6stat page 0 "surface IDs seen" == 3 for instance 0 and 2, "ambiguous IDs" == 0, "submits on a pipe with no known framebuffer" == 0,
#      and no "(AMBIGUOUS)" in page 2. (Evaluated at the LAST sample: the IDs may take minutes to be learned.)
#   K4 (M2) ALSO gates DP FRAMES DELIVERED: the delta of "presented to instance 0" in the bundle's 10 s "m6:" log line between the latest line and the newest line >= 50 s earlier, from minute 2 on, must be
#      >= PRES_MIN presents/s (20; env PRES_MIN) every minute (stamps are written even when nothing is presented, so K5 alone cannot see a frozen DP); a stopped DP stops the run, captures and parks. The line has no
#      DP-specific refusal counter; the delta of "unknown at the present" is recorded per minute in minutes.tsv.
#   S4 (not gating): ep.d (NATIVE-S7-MULTIMON.md Stage 0a) is started with dtrace -Z -W WindowServer before the ONE restart; its output /tmp/ep.out (+ /tmp/ep.err) is fetched to $RUN/ep.out at the end and in every
#      stop path; m6stat page 1's "last endpoint dword" is recorded every minute. If dtrace cannot attach the run continues and says so.
#   K4 the bundle frame counter counts monitor B surfaces : WindowServer's "m6: surfaces for instance 1/2: A/B" line (10 s counter log, Navi48Device.m n48_m6_report): B (monitor B) >= 1 in the
#      last line, its "(kernel table N IDs: instance 0 X, ambiguous 0 ...)" consistent, and "presented to instance 0" > 0.
#   K5 DP pixels and rate unchanged : (M1) the PER-INSTANCE counter m6stat page 1 "instance 0 (DP, OTG0) : N vblank stamps written" diffed over 3 s every minute (pipestat 4's "stamps written" is the GLOBAL
#      counter: with M6 every pipe's perform stamps it, so the monitor B inflates it; it is printed for information only): >= DP_MIN (40/s; Stage 0b measured 46-51) in EVERY minute and no minute below 85% of
#      minute 1's; disp2 status 2 "DP side: OTG0 COUNTING" at every sample; DIG1_DIG_BE_CNTL vs the pre-arm value (a change is a WARNING: arming may change it, queue 285).
#   K6 the monitor B CRC still CRC_A : disp2 crc 2 every minute == RG 0x78097625 B 0x4bd14c15, HUBP2 underflow(30:28) 0, SEG_ALLOC_ERR 0, TIMEOUT 0, ODM2 underflow-occurred 0, VM fault 0.
# Extras recorded, not gating: monitor B OTG2 stamps written (m6stat page 1), refused/guard reasons, WS pid timeline, panics.
#
# LOGS: notes/logs/runs/m6-s1a/<timestamp>/ (gitignored): run.log (everything), NN-*.txt per command, minutes.tsv, ws-pids.txt, summary.txt, capture-*.txt on failure.
# Env: DRYRUN=1  WATCH_MIN=10  DP_MIN=40  PRES_MIN=20  N48_HOST=navi48

set -u
set -o pipefail
ROOT=${N48_ROOT:-/path/to/navi48-checkout.nosync}
HOST=${N48_HOST:-navi48}
DRYRUN=${DRYRUN:-0}
WATCH_MIN=${WATCH_MIN:-10}
DP_MIN=${DP_MIN:-40}
PRES_MIN=${PRES_MIN:-20}
SSHO="-o BatchMode=yes -o ConnectTimeout=8"
TS=$(date '+%Y%m%d-%H%M%S')
RUN=$ROOT/notes/logs/runs/m6-s1a/$TS
[ "$DRYRUN" = 1 ] && { RUN=${DRYRUN_OUT:-/private/tmp/m6-s1a-run-dry-$TS}; WATCH_MIN=2; }
LOG=$RUN/run.log
mkdir -p "$RUN" || { echo "cannot create $RUN"; exit 1; }
T='~/navi48-staging/navi48test'
NUB='~/n48-metal/n48nub'
CRC_RG=0x78097625; CRC_B=0x4bd14c15
WANT_KEXT=0.0.659; WANT_AUX=0.0.7; WANT_BUNDLE=11
N=0

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
FAILS=""
# stop <exit code> <reason>: capture + park + summary, never continues.
stop() {
  local code=$1; shift
  say; say "### STOP at $(stamp): $*"
  FAILS="$FAILS
  STOP: $*"
  if [ "$code" = 3 ]; then
    say "### PC needs a manual reset. Last command: see $LOG ; kext $WANT_KEXT, aux $WANT_AUX, bundle $WANT_BUNDLE, variant stage17-native-1440-metal-disp-amfi-m6"
  else
    capture
  fi
  summary_and_exit "$code"
}
# need <description> <test command...>: a hard requirement (not evaluated in a dry run).
need() {
  local d=$1; shift
  if [ "$DRYRUN" = 1 ]; then say "  DRYRUN would require: $d"; return 0; fi
  if "$@"; then say "  ok: $d"; else stop 1 "requirement failed: $d"; fi
}
has() { printf '%s' "$PXOUT" | /usr/bin/grep -qE -- "$1"; }
sect() { awk -v n="@@$1" '$0==n{f=1;next} /^@@/{f=0} f' "$2"; }   # text of a @@NAME section of a snapshot file
num() { sed -n "$1" | head -1; }

collect_ep() {  # S4: fetch the ep.d output (never fatal)
  [ "$DRYRUN" = 1 ] && { say "--- DRYRUN: ssh $HOST 'cat /tmp/ep.out /tmp/ep.err' > $RUN/ep.out"; return 0; }
  ssh $SSHO "$HOST" 'echo "== /tmp/ep.out"; cat /tmp/ep.out 2>&1; echo "== /tmp/ep.err"; cat /tmp/ep.err 2>&1; echo "== dtrace running: $(pgrep -x dtrace | tr "\n" " ")"' >"$RUN/ep.out" 2>&1 && say "  ep.d output fetched: $(grep -c '^ep ' "$RUN/ep.out") 'ep' lines -> $RUN/ep.out"
}
capture() {  # best effort, each command separately, bounded
  say "### capturing state (the PC stays parked; read-only commands only)"
  reach || { say "### PC unreachable while capturing: PC needs a manual reset (one more check done)"; return; }
  local k
  for k in "m6stat 0" "m6stat 1" "m6stat 2" "pipestat 0" "pipestat 1" "pipestat 2" "pipestat 3" "pipestat 4" "disp2 status 2" "disp2 crc 2" "status"; do
    N=$((N+1)); ssh $SSHO "$HOST" "sudo -n $T accel $k 2>&1" >"$RUN/capture-$(printf '%s' "$k" | tr ' ' '-').txt" 2>&1
  done
  ssh $SSHO "$HOST" "sudo -n $T log 2>&1 | tail -400" >"$RUN/capture-driverlog.txt" 2>&1
  ssh $SSHO "$HOST" 'echo "up: $(uptime)"; echo "WS pid: $(pgrep -x WindowServer)"; echo "console: $(stat -f %Su /dev/console)"; who; ls -lt /Library/Logs/DiagnosticReports | head -8; ioreg -c IOFramebuffer -r -d1 -w0 | grep -E "^\+-o "; kmutil showloaded --list-only 2>/dev/null | grep -iE "navi48|rdna4|accelprobe"; tail -20 /var/tmp/n48watch.log 2>/dev/null' >"$RUN/capture-system.txt" 2>&1
  ssh $SSHO "$HOST" 'sudo -n log show --info --last 20m --predicate "process == \"WindowServer\" OR eventMessage CONTAINS \"Navi48Metal\" OR eventMessage CONTAINS \"n48accel\"" --style compact 2>/dev/null | tail -300' >"$RUN/capture-logshow.txt" 2>&1
  collect_ep
  say "### capture files: $RUN/capture-*.txt"
}

# ---------------------------------------------------------------------------------------------- criteria state
K1=PENDING; K2=PENDING; K3=PENDING; K4=PENDING; K5=PENDING; K6=PENDING
K1w=""; K2w=""; K3w=""; K4w=""; K5w=""; K6w=""; WARN=""
summary_and_exit() {
  local code=$1 S=$RUN/summary.txt
  { echo "M6 Stage 1a run $TS  (dry-run=$DRYRUN, watch ${WATCH_MIN} min)  exit code $code"
    echo "K1 WindowServer lists 2 displays on IOPresentment : $K1 $K1w"
    echo "K2 per-pipe transaction counts > 0                : $K2 $K2w"
    echo "K3 3 surface IDs per pipe, 0 ambiguous            : $K3 $K3w"
    echo "K4 bundle frame counter counts monitor B surfaces      : $K4 $K4w"
    echo "K5 DP pixels / rate unchanged                     : $K5 $K5w"
    echo "(K1 'AGDC list built' is a precondition; K4 includes DP presents/s; minutes.tsv has dp_presents_s and last_ep per minute; ep.d output: $RUN/ep.out)"
    echo "K6 monitor B CRC still CRC_A, underflow 0              : $K6 $K6w"
    [ -n "$WARN" ] && printf 'WARNINGS:%s\n' "$WARN"
    [ -n "$FAILS" ] && printf 'FAILURES / STOPS:%s\n' "$FAILS"
    echo "PC left PARKED (no teardown verbs were sent). Logs: $RUN"; } | tee -a "$LOG" >"$S"
  exit "$code"
}

say "### M6 Stage 1a RUN $TS dry-run=$DRYRUN host=$HOST watch=${WATCH_MIN}min DP_MIN=$DP_MIN out=$RUN"
say "### host Mac date: $(date)"

# ---------------------------------------------------------------------------------------------- P0 prechecks
say; say "=== P0 prechecks"
reach || stop 3 "PC ($HOST) not reachable"
px pre-system 'echo "up: $(uptime)"; echo "boottime: $(sysctl -n kern.boottime)"; echo "boot-args: $(sysctl -n kern.bootargs)"; echo "console: $(stat -f %Su /dev/console)"; echo "who:"; who; echo "WS: $(pgrep -x WindowServer)"; echo "loaded:"; kmutil showloaded --list-only 2>/dev/null | grep -iE "navi48|rdna4|accelprobe"; echo "bundle: $(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" /Library/GPUBundles/Navi48Metal.bundle/Contents/Info.plist 2>&1)"; echo "ESP: $(~/navi48-staging/esp-kext.sh show 2>&1 | grep "injected kext")"; echo "nub:"; sudo -n ~/n48-metal/n48nub status 2>&1; echo "newest reports:"; ls -lt /Library/Logs/DiagnosticReports 2>&1 | head -3' || stop 3 "system read failed"
need "console user is root (0 users)" has '^console: root$'
need "who lists nobody" bash -c '! printf "%s\n" "$1" | sed -n "/^who:/,/^WS:/p" | sed "1d;\$d" | /usr/bin/grep -q .' _ "$PXOUT"
need "boot-args carry navi48-m6=1" has 'navi48-m6=1'
need "boot-args carry navi48-fb2=1 navi48-metal-disp=1 navi48-disp2=1 navi48-dmubcmd=1" bash -c 'for t in navi48-fb2=1 navi48-metal-disp=1 navi48-disp2=1 navi48-dmubcmd=1; do printf "%s" "$1" | /usr/bin/grep -q -- "$t" || exit 1; done' _ "$PXOUT"
need "ESP names $WANT_KEXT" has "injected kext: $WANT_KEXT"
need "bring-up kext $WANT_KEXT loaded" has "navi48.*$WANT_KEXT|bringup.*$WANT_KEXT"
need "bundle $WANT_BUNDLE installed" has "^bundle: $WANT_BUNDLE\$"
need "the Metal nub is NOT published yet (fresh boot: one run per boot)" has 'NO Navi48MetalNub'
WS0=$(printf '%s' "$PXOUT" | sed -n 's/^WS: //p')
PRE_REPORT=$(printf '%s' "$PXOUT" | sed -n '/^newest reports:/,$p' | sed -n '2p')
say "  WindowServer pid at start: $WS0 ; newest report: $PRE_REPORT"
echo "$(date +%s) start $WS0" >"$RUN/ws-pids.txt"
# (after every check that reads pre-system's PXOUT) the aux kext matches on the Metal nub, so it is NOT loaded before P4's nub publish (0.0.658 boots the same): here only the aux KC is checked; the load is checked after the publish
px auxkc "kmutil inspect -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>&1 | grep -i accelprobe" || stop 3 "aux KC read failed"
need "aux KC carries com.navi48.accelprobe $WANT_AUX" has "accelprobe.*$WANT_AUX|$WANT_AUX.*accelprobe"
# m6stat baseline (also the positive control of its parser) and pipestat
px pre-m6stat "sudo -n $T accel m6stat 0 2>&1; sudo -n $T accel m6stat 1 2>&1; sudo -n $T accel m6stat 2 2>&1" || stop 1 "m6stat unreadable"
need "m6stat: navi48-m6 latch ON" has 'navi48-m6 latch ON'
need "m6stat: nothing adopted / armed yet (fresh boot)" has 'pipe adopted no; armed no'
need "m6stat: 0 pipes recorded before the recipe" has ' 0 pipes recorded'
need "m6stat parser control: instance 0 line present" has 'instance 0 \(DP, OTG0\) +: [0-9]+ transactions'
# DP baseline (positive control: the DP-side line must parse)
px pre-dp "sudo -n $T accel disp2 status 2 2>&1" || stop 1 "disp2 status 2 failed"
DP0=$(printf '%s' "$PXOUT" | sed -n 's/.*DP side: OTG0 \([A-Z]*\).*DIG1_DIG_BE_CNTL \(0x[0-9a-f]*\).*/\1 \2/p' | head -1)
say "  DP baseline (OTG0 state, DIG1_DIG_BE_CNTL): '$DP0'"
need "DP baseline parsed and OTG0 COUNTING" bash -c '[ "${1%% *}" = COUNTING ]' _ "$DP0"
DP_BE0=${DP0#* }
echo "$(date +%s)" >"$RUN/t0"

# ---------------------------------------------------------------------------------------------- P2 monitor B chain
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

crc_read() { # crc_read <label>  -> sets CRC_RG_NOW CRC_B_NOW UF_OK from a fresh disp2 crc 2
  px "$1" "sudo -n $T accel disp2 crc 2 2>&1" || stop 1 "$1: disp2 crc 2 failed"
  CRC_RG_NOW=$(printf '%s' "$PXOUT" | sed -n 's/.*OTG2 CRC *: RG \(0x[0-9a-f]*\) *B \(0x[0-9a-f]*\).*/\1/p' | head -1)
  CRC_B_NOW=$(printf '%s' "$PXOUT" | sed -n 's/.*OTG2 CRC *: RG \(0x[0-9a-f]*\) *B \(0x[0-9a-f]*\).*/\2/p' | head -1)
  UF_LINE=$(printf '%s' "$PXOUT" | /usr/bin/grep 'HUBP2_DCHUBP_CNTL' | head -1 | sed -n 's/.*\(underflow(30:28) [0-9]*  *SEG_ALLOC_ERR [0-9]*  *TIMEOUT(23:20) [0-9a-fx]*\).*/\1/p')
  ODM_OCC=$(printf '%s' "$PXOUT" | sed -n 's/.*underflow occurred(10) \([0-9]\).*/\1/p' | head -1)
  VMF=$(printf '%s' "$PXOUT" | sed -n 's/.*DCN_VM_FAULT_STATUS *: *\([0-9a-fx]*\).*/\1/p' | head -1)
  UF_OK=0
  [ "$UF_LINE" = "underflow(30:28) 0  SEG_ALLOC_ERR 0  TIMEOUT(23:20) 0" ] && [ "$ODM_OCC" = 0 ] && { [ "$VMF" = 0000000000 ] || [ "$VMF" = 0x0 ] || [ "$VMF" = 0 ]; } && UF_OK=1
  say "  CRC RG=$CRC_RG_NOW B=$CRC_B_NOW ; '$UF_LINE' ; ODM2 occurred=$ODM_OCC ; VM fault=$VMF -> underflow-clean=$UF_OK"
}
crc_read crc-A
need "monitor B CRC_A exact ($CRC_RG / $CRC_B) BEFORE the holds" bash -c '[ "$1" = "$3" ] && [ "$2" = "$4" ]' _ "$CRC_RG_NOW" "$CRC_B_NOW" "$CRC_RG" "$CRC_B"
need "monitor B underflow lines all 0 BEFORE the holds" [ "$UF_OK" = 1 ]
CRCA_OK=1

# ---------------------------------------------------------------------------------------------- P3 hold + publish
say; say "=== P3 fbhold 2 (IRREVERSIBLE this boot: after it NO dmubsend, no teardown) and fbpublish 2"
px fbhold "sudo -n $T accel disp2 fbhold 2 2>&1" || stop 1 "fbhold 2 failed"
need "fbhold 2: status 0" has 'fbhold 2: status 0 \(OK\)'
crc_read crc-after-hold
need "monitor B CRC_A after the hold" bash -c '[ "$1" = "$3" ] && [ "$2" = "$4" ]' _ "$CRC_RG_NOW" "$CRC_B_NOW" "$CRC_RG" "$CRC_B"
px fbpublish "sudo -n $T accel fbpublish 2 2>&1" || stop 1 "fbpublish 2 failed"
need "fbpublish 2: status 0 (the nub is published, Navi48DisplayIndex 1)" has 'fbpublish 2: status 0 \(OK'
nap 5
px fbnode 'ioreg -c IOFramebuffer -r -d1 -w0 | grep -E "^\+-o "; echo "Navi48Framebuffer nodes: $(ioreg -c Navi48Framebuffer -r -d1 -w0 | grep -cE "^\+-o Navi48Framebuffer")"' || true
need "1 Navi48Framebuffer node exists (the aux framebuffer started on the monitor B's nub)" has 'Navi48Framebuffer nodes: 1'
crc_read crc-after-publish
need "monitor B CRC_A after fbpublish" bash -c '[ "$1" = "$3" ] && [ "$2" = "$4" ]' _ "$CRC_RG_NOW" "$CRC_B_NOW" "$CRC_RG" "$CRC_B"

# ---------------------------------------------------------------------------------------------- P4 nub, adopt, agdc, fbname, arm
say; say "=== P4 nub publish -> pipeadopt (every pipe) -> pipeagdc 1 (nfb = 2 from the published nubs) -> fbname 1 -> headless flag -> pipearm 1   (lines mirror arm-gpu-desktop.sh)"
px nubpublish "sudo -n $NUB publish 2>&1" || stop 1 "n48nub publish failed"
need "nub publish: 0 (Success)" has 'publish: 0 \(Success\)'
nap 3; px auxloaded "kmutil showloaded --list-only 2>/dev/null | grep -iE accelprobe" || true
need "aux $WANT_AUX loaded after the nub publish (kmutil showloaded)" has "accelprobe.*\\($WANT_AUX\\)"
nap 2
px pipeadopt "sudo -n $T accel pipeadopt 2>&1" || stop 1 "pipeadopt failed (a non-OK status is a CLEAN refusal: the pipe check is per pipe, not pipe 0 only)"
need "pipeadopt: status 0 (OK) (accepted every pipe)" has 'status +: 0 \(OK\)'
px pre-arm-m6 "sudo -n $T accel m6stat 0 2>&1" || true
say "  after adopt (informational): $(printf '%s' "$PXOUT" | /usr/bin/grep 'm6stat 0:' | head -1)"
px pipeagdc "sudo -n $T accel pipeagdc 1 2>&1" || stop 1 "pipeagdc failed"
need "pipeagdc: agdc status 0 (PUBLISHED)" has 'agdc status +: 0 \(PUBLISHED\)'
need "pipeagdc not REFUSED (the display nub's framebuffer has started)" bash -c '! printf "%s" "$1" | /usr/bin/grep -q REFUSED' _ "$PXOUT"
px fbname "sudo -n $T accel fbname 1 2>&1" || stop 1 "fbname failed"
need "fbname: class name NOW AMDRDNA4" has 'class name NOW +: AMDRDNA4'
px headless 'sudo -n touch /private/tmp/n48m-headless-no; sudo -n chmod 644 /private/tmp/n48m-headless-no; ls -l /private/tmp/n48m-headless-no' || stop 1 "headless flag failed"
px pipearm "sudo -n $T accel pipearm 1 2>&1 | grep 0xccf" || stop 1 "pipearm failed"
need "pipearm: accel+0xccf now 1" has '0xccf now +: 1'
# last look before the restart: monitor B CRC_A, DP counting, nothing yet routed
crc_read crc-before-restart
need "monitor B CRC_A before the WindowServer restart" bash -c '[ "$1" = "$3" ] && [ "$2" = "$4" ]' _ "$CRC_RG_NOW" "$CRC_B_NOW" "$CRC_RG" "$CRC_B"
px pre-restart-state "sudo -n $T accel m6stat 0 2>&1; sudo -n $T accel pipestat 4 2>&1 | grep 'stamps written'; pgrep -x WindowServer; stat -f %Su /dev/console; who" || true
WSB=$(printf '%s' "$PXOUT" | /usr/bin/grep -E '^[0-9]+$' | head -1)
need "still 0 users right before the restart (console root, no who lines)" bash -c 'printf "%s\n" "$1" | /usr/bin/grep -qx root && ! printf "%s\n" "$1" | /usr/bin/grep -qE "^[a-z_][a-z0-9_]* +(console|ttys[0-9]+) "' _ "$PXOUT"

# ---------------------------------------------------------------------------------------------- P5 ONE WindowServer restart
say; say "=== P5 ONE WindowServer restart (pipereload opens the 15 s window; the kill follows in the SAME ssh at 0 users)"
cat >"$RUN/ep.d" <<'EPD'
pid$target::*GatherAGDCLogicalDeviceCapabilities*:entry {
  p = *(uint64_t*)copyin(arg0+0x20,8);
  printf("ep %u conn %u\n", *(uint32_t*)copyin(p+0x50,4), *(uint32_t*)copyin(p+0x20,4));
}
EPD
if [ "$DRYRUN" = 1 ]; then say "--- DRYRUN: ssh $HOST 'cat > /tmp/ep.d' < ep.d (the Stage 0a probe)"; else ssh $SSHO "$HOST" 'cat > /tmp/ep.d' <"$RUN/ep.d" || say "  (S4) could not place ep.d: continuing without the probe"; fi
px epstart "sudo -n sh -c 'nohup dtrace -Z -q -s /tmp/ep.d -W WindowServer -o /tmp/ep.out >/tmp/ep.err 2>&1 </dev/null &'; sleep 6; if pgrep -x dtrace >/dev/null; then echo 'ep.d: dtrace is waiting for the WindowServer exec'; else echo 'ep.d: dtrace is NOT running (could not attach); the run continues without it'; cat /tmp/ep.err; fi" || say "  (S4) dtrace start failed: continuing without the probe"
px pcdate "date '+%Y-%m-%d %H:%M:%S'" || true
RESTART_AT=$(printf '%s' "$PXOUT" | head -1)
say "  PC clock at the restart: $RESTART_AT ; WindowServer pid before: $WSB"
px restart "who | grep -q . && { echo 'users logged in - refusing'; exit 9; }; r=\$(sudo -n $T accel pipereload 2>&1); echo \"\$r\" | grep -i 'restart window'; echo \"\$r\" | grep -q 'OPEN' || { echo 'restart window NOT open - not killing'; exit 8; }; sudo -n killall -9 WindowServer; echo killed-at-\$(date +%T)" || stop 1 "the restart command failed or refused (exit 9 = a user is logged in); NO second attempt"
need "restart window was OPEN" has 'restart window +: OPEN'
echo "$(date +%s) killed $WSB" >>"$RUN/ws-pids.txt"
nap 15
WS1=""; k=0
while [ "$DRYRUN" != 1 ] && [ $k -lt 12 ]; do
  ssh $SSHO "$HOST" 'pgrep -x WindowServer' >"$RUN/wspid.tmp" 2>/dev/null; WS1=$(head -1 "$RUN/wspid.tmp")
  [ -n "$WS1" ] && [ "$WS1" != "$WSB" ] && break; k=$((k+1)); sleep 5
done
say "  WindowServer pid after the restart: '$WS1' (was $WSB)"
need "WindowServer came back with a NEW pid" bash -c '[ -n "$1" ] && [ "$1" != "$2" ]' _ "$WS1" "$WSB"
echo "$(date +%s) restarted $WS1" >>"$RUN/ws-pids.txt"
# the watcher and the CIKL backstop exactly as arm-gpu-desktop.sh
cat >"$RUN/n48watch.sh" <<'W'
#!/bin/bash
LOG=/var/tmp/n48watch.log; P=$(pgrep -x WindowServer); echo "v2 start $(date) ws=$P" >> $LOG
while true; do
  Q=$(pgrep -x WindowServer)
  if [ -n "$Q" ] && [ "$Q" != "$P" ]; then rm -f /private/tmp/n48m-headless-no /private/tmp/n48m-starts; echo "$(date) ws $P -> $Q: flag+counter removed" >> $LOG; P=$Q; fi
  sleep 0.5
done
W
if [ "$DRYRUN" = 1 ]; then say "--- DRYRUN: ssh $HOST 'cat > /tmp/n48watch.sh' < n48watch.sh (the watcher from arm-gpu-desktop.sh)"; else ssh $SSHO "$HOST" 'cat > /tmp/n48watch.sh' <"$RUN/n48watch.sh" || stop 1 "could not place the watcher"; fi
px watcher "sudo -n nohup bash /tmp/n48watch.sh >/dev/null 2>&1 </dev/null & sleep 1; sudo -n launchctl asuser 501 launchctl setenv CI_USE_MTL_DAG_FOR_CIKL_SRC 0 || echo 'note: launchctl asuser 501 setenv failed (no user domain yet?): the CIKL backstop is NOT in place'" || true

# ---------------------------------------------------------------------------------------------- P6 watch
say; say "=== P6 watch: ${WATCH_MIN} minutes, one snapshot per minute (monitor B CRC + underflow, DP stamps/s, WS pid, users, m6stat 0/1/2, DP side)"
printf 'min\ttime\tws\tdp_stamps_s\tcrc_rg\tcrc_b\tuf_ok\ttxn0\ttxn2\tids0\tids2\tambig\tnofb\tagdc_nfb\totg0\tdig1_be\tdell_stamps\tusers\tdp_presents_s\tdp_unknown_refusals\tlast_ep\tglobal_stamps_s_info\n' >"$RUN/minutes.tsv"
PRES_LAST=""; PRES_BAD=0; PRES_MINW=""; DP_FIRST=""; DP_BAD=0; TXN0_FIRST=""; LAST_TXN0=0; LAST_TXN2=0; LAST_IDS0=0; LAST_IDS2=0; LAST_AMB=0; LAST_AMB2=0; LAST_NOFB=0; LAST_AG=0; LAST_BE="$DP_BE0"; PIPES_REC=""; MONB_STAMPS=0
PANICNOW=""
snap_remote() {
  cat <<EOS
echo "@@WS"; pgrep -x WindowServer
echo "@@USERS"; stat -f %Su /dev/console; who
echo "@@STAMPA"; sudo -n $T accel m6stat 1 2>&1 | grep '^ *instance 0 '
echo "@@PIPEA"; sudo -n $T accel pipestat 4 2>&1 | grep 'stamps written'
sleep 3
echo "@@STAMPB"; sudo -n $T accel m6stat 1 2>&1 | grep '^ *instance 0 '
echo "@@PIPEB"; sudo -n $T accel pipestat 4 2>&1 | grep 'stamps written'
echo "@@CRC"; sudo -n $T accel disp2 crc 2 2>&1
echo "@@STATUS"; sudo -n $T accel disp2 status 2 2>&1
echo "@@M6_0"; sudo -n $T accel m6stat 0 2>&1
echo "@@M6_1"; sudo -n $T accel m6stat 1 2>&1
echo "@@M6_2"; sudo -n $T accel m6stat 2 2>&1
echo "@@BUNDLE"; sudo -n log show --info --last 4m --predicate 'eventMessage CONTAINS "m6: surfaces for instance"' --style compact 2>/dev/null | grep 'WindowServer\[.*m6: surfaces for instance' | tail -12
echo "@@REPORTS"; ls -lt /Library/Logs/DiagnosticReports 2>&1 | head -2 | tail -1
EOS
}
m=0
while [ $m -lt "$WATCH_MIN" ]; do
  m=$((m+1)); nap 45
  N=$((N+1)); SF=$(printf '%s/%02d-snap-min%02d.txt' "$RUN" "$N" "$m")
  say "--- [$N] $(stamp) minute $m snapshot (one ssh) -> $SF"
  if [ "$DRYRUN" = 1 ]; then say "    DRYRUN: ssh $HOST '<snapshot script: pgrep WS; console+who; pipestat 4 x2 3 s apart; disp2 crc 2; disp2 status 2; m6stat 0|1|2; newest report>'"; continue; fi
  if ! snap_remote | ssh $SSHO "$HOST" 'bash -s' >"$SF" 2>&1; then
    if reach; then say "  (the snapshot ssh returned nonzero but the PC answers; using what arrived)"; else stop 3 "PC unreachable during the watch (minute $m), and one more check failed"; fi
  fi
  ws=$(sect WS "$SF" | head -1); users=$(sect USERS "$SF" | tr '\n' ' ')
  a=$(sect STAMPA "$SF" | sed -n 's/^ *instance 0 .*: \([0-9][0-9]*\) vblank stamps written.*/\1/p' | head -1); b=$(sect STAMPB "$SF" | sed -n 's/^ *instance 0 .*: \([0-9][0-9]*\) vblank stamps written.*/\1/p' | head -1)
  rate=""; [ -n "$a" ] && [ -n "$b" ] && rate=$(( (b - a) / 3 ))
  ga=$(sect PIPEA "$SF" | sed -n 's/.*: *\([0-9][0-9]*\)$/\1/p'); gb=$(sect PIPEB "$SF" | sed -n 's/.*: *\([0-9][0-9]*\)$/\1/p')
  grate=""; [ -n "$ga" ] && [ -n "$gb" ] && grate=$(( (gb - ga) / 3 ))
  # M2: DP presents/s from the bundle's 10 s counter line: the latest line vs the newest line >= 50 s earlier
  sect BUNDLE "$SF" >"$SF.bundle"
  pr=$(awk '{ split($2,a,":"); T[NR]=a[1]*3600+a[2]*60+a[3]; v=$0; sub(/.*presented to instance 0 /,"",v); V[NR]=v+0; u=$0; sub(/.*unknown /,"",u); U[NR]=u+0 }
    END { for (k=NR-1; k>=1; k--) if (T[NR]-T[k] >= 50) { printf "%d %d %d\n", (V[NR]-V[k])/(T[NR]-T[k]), U[NR]-U[k], T[NR]-T[k]; exit } print "none" }' "$SF.bundle")
  pres=""; punk=""; if [ -n "$pr" ] && [ "$pr" != none ]; then pres=${pr%% *}; punk=$(echo "$pr" | awk '{print $2}'); fi
  sect CRC "$SF" >"$SF.crc"; PXOUT=$(cat "$SF.crc")
  CRC_RG_NOW=$(printf '%s' "$PXOUT" | sed -n 's/.*OTG2 CRC *: RG \(0x[0-9a-f]*\) *B \(0x[0-9a-f]*\).*/\1/p' | head -1)
  CRC_B_NOW=$(printf '%s' "$PXOUT" | sed -n 's/.*OTG2 CRC *: RG \(0x[0-9a-f]*\) *B \(0x[0-9a-f]*\).*/\2/p' | head -1)
  UF_LINE=$(printf '%s' "$PXOUT" | /usr/bin/grep 'HUBP2_DCHUBP_CNTL' | head -1 | sed -n 's/.*\(underflow(30:28) [0-9]*  *SEG_ALLOC_ERR [0-9]*  *TIMEOUT(23:20) [0-9a-fx]*\).*/\1/p')
  ODM_OCC=$(printf '%s' "$PXOUT" | sed -n 's/.*underflow occurred(10) \([0-9]\).*/\1/p' | head -1)
  VMF=$(printf '%s' "$PXOUT" | sed -n 's/.*DCN_VM_FAULT_STATUS *: *\([0-9a-fx]*\).*/\1/p' | head -1)
  ufok=0; [ "$UF_LINE" = "underflow(30:28) 0  SEG_ALLOC_ERR 0  TIMEOUT(23:20) 0" ] && [ "$ODM_OCC" = 0 ] && { [ "$VMF" = 0000000000 ] || [ "$VMF" = 0x0 ] || [ "$VMF" = 0 ]; } && ufok=1
  st=$(sect STATUS "$SF" | sed -n 's/.*DP side: OTG0 \([A-Z]*\).*DIG1_DIG_BE_CNTL \(0x[0-9a-f]*\).*/\1 \2/p' | head -1); otg0=${st%% *}; be=${st#* }
  sect M6_0 "$SF" >"$SF.m0"; sect M6_1 "$SF" >"$SF.m1"; sect M6_2 "$SF" >"$SF.m2"
  lastep=$(sed -n 's/.*last endpoint dword \([0-9]*\).*/\1/p' "$SF.m1" | head -1)
  inst() { sed -n "s/^ *instance $1 .*: \([0-9][0-9]*\) transactions, \([0-9][0-9]*\) surface IDs seen.*/\\$2/p" "$SF.m0" | head -1; }
  txn0=$(inst 0 1); ids0=$(inst 0 2); txn2=$(inst 2 1); ids2=$(inst 2 2)
  amb=$(sed -n 's/^ *ambiguous IDs (seen on two pipes) *: \([0-9]*\).*/\1/p' "$SF.m0" | head -1)
  nofb=$(sed -n 's/^ *submits on a pipe with no known framebuffer: \([0-9]*\).*/\1/p' "$SF.m0" | head -1)
  agn=$(sed -n 's/.*AGDC: \([0-9]*\) framebuffers in the last 0x980 reply.*/\1/p' "$SF.m1" | head -1)
  dst=$(sed -n 's/^ *instance 2 .*: \([0-9][0-9]*\) vblank stamps written.*/\1/p' "$SF.m1" | head -1)
  pipes=$(sed -n 's/.* \([0-9]*\) pipes recorded.*/\1/p' "$SF.m0" | head -1)
  amb2=$(/usr/bin/grep -c 'AMBIGUOUS' "$SF.m2")
  nuser=0; sect USERS "$SF" | sed '1d' | /usr/bin/grep -q . && nuser=1; [ "$(sect USERS "$SF" | head -1)" = root ] || nuser=1
  rep=$(sect REPORTS "$SF" | head -1)
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$m" "$(stamp)" "$ws" "$rate" "$CRC_RG_NOW" "$CRC_B_NOW" "$ufok" "$txn0" "$txn2" "$ids0" "$ids2" "$amb" "$nofb" "$agn" "$otg0" "$be" "$dst" "$nuser" "${pres:-n/a}" "${punk:-n/a}" "${lastep:-?}" "${grate:-?}" >>"$RUN/minutes.tsv"
  say "  min $m: WS $ws | DP stamps/s (m6stat inst 0) ${rate:-?} [global pipestat 4, info: ${grate:-?}] | DP presents/s ${pres:-n/a} (unknown-at-present delta ${punk:-n/a}) | last endpoint dword ${lastep:-?} | monitor B CRC ${CRC_RG_NOW:-?}/${CRC_B_NOW:-?} uf-clean=$ufok | txn DP ${txn0:-?} monitor B ${txn2:-?} | IDs DP ${ids0:-?} monitor B ${ids2:-?} ambiguous ${amb:-?} no-fb ${nofb:-?} | AGDC nfb ${agn:-?} | OTG0 ${otg0:-?} DIG1_BE ${be:-?} | monitor B stamps ${dst:-?} | pipes recorded ${pipes:-?} | users-flag $nuser"
  echo "$(date +%s) min$m $ws" >>"$RUN/ws-pids.txt"
  [ -n "$pipes" ] && PIPES_REC=$pipes
  # hard stops during the watch (park + capture)
  [ "$nuser" = 0 ] || stop 1 "a console user appeared during the watch (minute $m): '$users' - user logged in; the PC is left as is"
  [ "$ws" = "$WS1" ] || stop 1 "WindowServer pid changed after the restart: $WS1 -> '$ws' (minute $m): a crash or relaunch; no second restart is ever sent"
  [ "$CRC_RG_NOW" = "$CRC_RG" ] && [ "$CRC_B_NOW" = "$CRC_B" ] || { K6="FAIL"; K6w="(minute $m: CRC $CRC_RG_NOW/$CRC_B_NOW, want $CRC_RG/$CRC_B)"; stop 1 "the monitor B CRC left CRC_A at minute $m ($CRC_RG_NOW/$CRC_B_NOW): pixels reached the monitor B, or the plane moved"; }
  [ "$ufok" = 1 ] || { K6="FAIL"; K6w="(minute $m: underflow line '$UF_LINE' ODM $ODM_OCC VMF $VMF)"; stop 1 "monitor B underflow / fault at minute $m: '$UF_LINE' ODM occurred $ODM_OCC VM fault $VMF"; }
  [ "$otg0" = COUNTING ] || { K5="FAIL"; K5w="(minute $m: OTG0 '$otg0')"; stop 1 "the DP's OTG0 is not COUNTING at minute $m ('$otg0')"; }
  [ "$be" = "$LAST_BE" ] || WARN="$WARN
  minute $m: DIG1_DIG_BE_CNTL $LAST_BE -> $be (DP side register changed; arming may legitimately change it, queue 285; judge by eye)"
  LAST_BE=$be
  if [ -n "$rate" ]; then
    [ -n "$DP_FIRST" ] || DP_FIRST=$rate
    [ "$rate" -ge "$DP_MIN" ] || { DP_BAD=1; K5w="$K5w minute $m: $rate stamps/s < $DP_MIN;"; }
    [ "$rate" -ge $(( DP_FIRST * 85 / 100 )) ] || { DP_BAD=1; K5w="$K5w minute $m: $rate stamps/s < 85% of minute 1 ($DP_FIRST);"; }
  else DP_BAD=1; K5w="$K5w minute $m: no stamp reading;"; fi
  if [ "$m" -ge 2 ]; then
    if [ -z "$pres" ]; then K4="FAIL"; K4w="(minute $m: no DP presents reading)"; stop 1 "no bundle 'm6:' counter lines >= 50 s apart to measure DP presents/s at minute $m (the bundle does not see the latch, or WindowServer stopped logging)"
    elif [ "$pres" -lt "$PRES_MIN" ]; then K4="FAIL"; K4w="(minute $m: DP presents/s $pres < $PRES_MIN)"; stop 1 "DP frames delivered collapsed: $pres presents/s at minute $m (floor $PRES_MIN) while stamps/s was ${rate:-?}: a frozen DP"; fi
    [ -n "$PRES_LAST" ] || PRES_LAST=$pres
  fi
  [ -n "$TXN0_FIRST" ] || TXN0_FIRST=${txn0:-0}
  LAST_TXN0=${txn0:-0}; LAST_TXN2=${txn2:-0}; LAST_IDS0=${ids0:-0}; LAST_IDS2=${ids2:-0}; LAST_AMB=${amb:-0}; LAST_NOFB=${nofb:-0}; LAST_AG=${agn:-0}; MONB_STAMPS=${dst:-0}; LAST_AMB2=$amb2
  PANICNOW=$rep
done
collect_ep
K6=${K6/PENDING/PASS}; [ "$DRYRUN" = 1 ] && K6="(dry run: not evaluated)"

# ---------------------------------------------------------------------------------------------- P6b logs
say; say "=== P6b logs since the restart (WindowServer log, the bundle's 10 s counter log, the displays in ioreg)"
px wslog "sudo -n log show --info --start '$RESTART_AT' --predicate 'process == \"WindowServer\"' --style compact >/tmp/n48-s1a-wslog.txt 2>&1; grep -E 'Unable to find display pipe|compositor activated' /tmp/n48-s1a-wslog.txt | sed 's/^/WS: /'; echo \"nopipe-lines: \$(grep -c 'Unable to find display pipe' /tmp/n48-s1a-wslog.txt)\"; wc -l /tmp/n48-s1a-wslog.txt" || true
NOPIPE=$(printf '%s' "$PXOUT" | sed -n 's/^nopipe-lines: //p' | head -1)
ACT=$(printf '%s' "$PXOUT" | /usr/bin/grep -c 'compositor activated')
px bundlelog "sudo -n log show --info --start '$RESTART_AT' --predicate 'eventMessage CONTAINS \"m6: \"' --style compact 2>/dev/null | tail -8" || true
M6L=$(printf '%s' "$PXOUT" | /usr/bin/grep 'WindowServer\[.*m6: surfaces for instance' | tail -1)
say "  last bundle line: $M6L"
B_MONB=$(printf '%s' "$M6L" | sed -n 's/.*instance 1\/2: [0-9]*\/\([0-9]*\).*/\1/p')
B_I0=$(printf '%s' "$M6L" | sed -n 's/.*instance 0 \([0-9]*\),.*/\1/p')
B_AMB=$(printf '%s' "$M6L" | sed -n 's/.*ambiguous \([0-9]*\);.*/\1/p')
B_PRES=$(printf '%s' "$M6L" | sed -n 's/.*presented to instance 0 \([0-9]*\).*/\1/p')
px displays 'ioreg -c IOFramebuffer -r -d1 -w0 | grep -E "^\+-o "; echo "Navi48Framebuffer nodes: $(ioreg -c Navi48Framebuffer -r -d1 -w0 | grep -cE "^\+-o Navi48Framebuffer")"; ioreg -l -w0 | grep -cE "RDNA4FB|AMDRDNA4" ' || true
FBNODES=$(printf '%s' "$PXOUT" | sed -n 's/^Navi48Framebuffer nodes: //p')
FBTOTAL=$(printf '%s' "$PXOUT" | /usr/bin/grep -cE '^\+-o ')
px post-driverlog "sudo -n $T log 2>&1 | tail -150" || true

# ---------------------------------------------------------------------------------------------- P7 verdicts
say; say "=== P7 verdicts"
if [ "$DRYRUN" = 1 ]; then
  K1="(dry run)"; K2="(dry run)"; K3="(dry run)"; K4="(dry run)"; K5="(dry run)"
  say "  DRYRUN: no verdicts are computed; the rules are in the header (K1..K6)"
  summary_and_exit 0
fi
# K1
k1=PASS; w=""
[ "${LAST_AG:-0}" = 2 ] || { k1=FAIL; w="$w AGDC list built = ${LAST_AG:-?} (precondition, want 2);"; }
[ "${FBTOTAL:-0}" = 2 ] || WARN="$WARN
  ioreg lists ${FBTOTAL:-?} IOFramebuffer nodes (expected 2: RDNA4FB/AMDRDNA4 + Navi48Framebuffer; fbname may hide the renamed one from -c IOFramebuffer)"
[ "${LAST_TXN0:-0}" -gt 0 ] && [ "${LAST_TXN2:-0}" -gt 0 ] || { k1=FAIL; w="$w a pipe never submitted (DP ${LAST_TXN0:-?}, monitor B ${LAST_TXN2:-?});"; }
[ "${FBNODES:-0}" = 1 ] || { k1=FAIL; w="$w Navi48Framebuffer nodes=${FBNODES:-?} (want 1);"; }
[ "${NOPIPE:-1}" = 0 ] || { k1=FAIL; w="$w WindowServer logged 'Unable to find display pipe' ${NOPIPE:-?} times;"; }
[ "${PIPES_REC:-}" = 2 ] || WARN="$WARN
  m6stat 'pipes recorded' = '${PIPES_REC:-?}' (expected 2; its exact meaning is not documented: informational)"
K1=$k1; K1w="$w (proof: WS log 'Unable to find display pipe' ${NOPIPE:-?}; AGDC list built ${LAST_AG:-?} [precondition only]; ioreg IOFramebuffer nodes ${FBTOTAL:-?}; txn DP ${LAST_TXN0:-?} monitor B ${LAST_TXN2:-?}; 'compositor activated' lines $ACT)"
# K2
k2=PASS; w=""
[ "${LAST_TXN0:-0}" -gt 0 ] || { k2=FAIL; w="$w DP txn 0;"; }
[ "${LAST_TXN2:-0}" -gt 0 ] || { k2=FAIL; w="$w monitor B txn 0;"; }
[ "${LAST_TXN0:-0}" -gt "${TXN0_FIRST:-0}" ] || { k2=FAIL; w="$w DP txn not growing (${TXN0_FIRST:-?} -> ${LAST_TXN0:-?});"; }
K2=$k2; K2w="$w (DP ${LAST_TXN0:-?}, monitor B ${LAST_TXN2:-?})"
# K3
k3=PASS; w=""
[ "${LAST_IDS0:-0}" = 3 ] || { k3=FAIL; w="$w DP IDs ${LAST_IDS0:-?} (want 3);"; }
[ "${LAST_IDS2:-0}" = 3 ] || { k3=FAIL; w="$w monitor B IDs ${LAST_IDS2:-?} (want 3);"; }
[ "${LAST_AMB:-1}" = 0 ] || { k3=FAIL; w="$w ambiguous ${LAST_AMB:-?};"; }
[ "${LAST_AMB2:-1}" = 0 ] || { k3=FAIL; w="$w page 2 shows AMBIGUOUS;"; }
[ "${LAST_NOFB:-1}" = 0 ] || { k3=FAIL; w="$w submits on a pipe with no known framebuffer ${LAST_NOFB:-?};"; }
K3=$k3; K3w="$w (DP ${LAST_IDS0:-?}, monitor B ${LAST_IDS2:-?}, ambiguous ${LAST_AMB:-?})"
# K4
k4=PASS; w=""
[ -n "$M6L" ] || { k4=FAIL; w="$w no 'm6: surfaces for instance' line from the bundle since the restart (latch not seen by the bundle?);"; }
[ "${B_MONB:-0}" -ge 1 ] || { k4=FAIL; w="$w bundle counts ${B_MONB:-?} monitor B surfaces;"; }
[ "${B_AMB:-1}" = 0 ] || { k4=FAIL; w="$w bundle ambiguous ${B_AMB:-?};"; }
[ "${B_PRES:-0}" -gt 0 ] || { k4=FAIL; w="$w presented to instance 0 = ${B_PRES:-?};"; }
[ "$K4" = FAIL ] && k4=FAIL
K4=$k4; K4w="$K4w$w (DP presents/s at minute 2: ${PRES_LAST:-?}, floor $PRES_MIN, gated every minute from minute 2; bundle: monitor B surfaces ${B_MONB:-?}, instance 0 ${B_I0:-?}, presented to instance 0 ${B_PRES:-?})"
# K5
if [ "$DP_BAD" = 0 ] && [ -n "$DP_FIRST" ]; then k5=PASS; else k5=FAIL; fi
[ "$K5" = FAIL ] && k5=FAIL
K5=$k5; K5w="$K5w (minute 1: ${DP_FIRST:-?} stamps/s (per-instance m6stat counter); floor $DP_MIN; DP pixels are NOT CRC-checked: judge by eye / see ambiguities)"
# K6 was decided inside the loop (a FAIL there stops the run)
K6w="${K6w:-(every minute: CRC_A exact, underflow lines 0; monitor B OTG2 stamps written $MONB_STAMPS)}"
newest=$PANICNOW
[ "$newest" = "$PRE_REPORT" ] || WARN="$WARN
  newest DiagnosticReports entry changed during the run: '$newest' (was '$PRE_REPORT'): read it"
say "  K1 $K1 $K1w"; say "  K2 $K2 $K2w"; say "  K3 $K3 $K3w"; say "  K4 $K4 $K4w"; say "  K5 $K5 $K5w"; say "  K6 $K6 $K6w"
ALL=PASS
for v in "$K1" "$K2" "$K3" "$K4" "$K5" "$K6"; do [ "$v" = PASS ] || ALL=FAIL; done
if [ "$ALL" = PASS ]; then say "### STAGE 1a: PASS (all six criteria). PC left armed with two displays on IOPresentment, 0 users. No teardown sent."; summary_and_exit 0
else FAILS="$FAILS
  one or more criteria FAILED (see above); the evidence is in $RUN"; capture; say "### STAGE 1a: FAIL. PC left PARKED (armed, not torn down)."; summary_and_exit 1; fi
