#!/bin/bash
# m6-stage1b-run.sh - M6 STAGE 1b RUN: FIRST GPU PIXELS ON THE MONB (instance 2: HUBP2 / OTG2 flipped by the Metal bundle through SCANX_*), the DP on instance 0 unchanged.
# RUN ON THE AIR; drives the PC over ssh (host alias `navi48`) ONLY when the brief grants PC operation (pc-protocol.md). `DRYRUN=1` prints every ssh command, runs none.
# UNTRACKED (not committed) until the reviewer says so. Derived from tools/pc/m6-stage1a-run.sh (the committed 1a kit incl. its four fixes: aux loads only after the nub publish; PXOUT overwrite order;
# only WindowServer's bundle lines are parsed; the review's M1/M2) and an internal design note "## Q2" item 13 + "Run criteria" 1-10; queue 292 (fresh boot may have the DP in 1080p).
# Needs: the PC deployed by m6-stage1b-deploy.sh (kext 0.0.661, aux 0.0.7, bundle 13 + Mesa dylib, variant ...-amfi-m6flip, latches navi48-m6=1 navi48-m6flip=1) on a FRESH BOOT
# (one run per boot; 0 console users; not armed; nothing published; the DP at its full-res raster).
#
# RECIPE
#   P0 prechecks (0 users, versions, both latches, fresh boot, aux 0.0.7 at its path, parser controls) + THE DP-RASTER CHECK: reg 0x4fea (OTG0_H_TOTAL) must be 0x0a9f; 0x0897 = the 1080p fallback of queue 292
#      -> the run REFUSES (exit 2) with "needs a warm restart" BEFORE touching anything.        P1 DP baseline (disp2 status 2)
#   P2 monitor B chain exactly as 1a: pclk otg2-1440-on, timing1440 2, phyd enable-1440, digd setup-1440, connect 2, SCDC lock, plane 2, show 2   -> CRC_A, underflow 0 BEFORE
#   P3 fbhold 2 (IRREVERSIBLE this boot; after it NO dmubsend of any kind), fbpublish 2        P4 nub publish -> aux 0.0.7 loaded -> pipeadopt -> pipeagdc 1 -> fbname 1 -> headless flag -> pipearm 1
#   P5 ONE WindowServer restart (pipereload + killall -9 in one ssh, at 0 users) + dtrace ep.d + the watcher + the CIKL backstop (as 1a).  (REVIEW CHECKS hook A sits before it.)
#   P5b PRE-FLIP re-confirm (see AMBIGUITY 1: the flip begins within seconds of the restart, so the "60 s before anything flips" window is the time until the first flip, K-snapshot taken at once
#      and every ~10 s; PREHOLD=1 buys the full 60 s with the kill file present and a SECOND WindowServer restart, OFF by default).
#   P6 FLIP: criterion 2 (CRC leaves CRC_A within 20 s of the restart; EARLIEST2 one of the 3 slots; FLIP_PENDING 0), criterion 1 (Acquire2 OK, 3 slots, MCs outside A/B and the DP's slots).
#   P7 WATCH (WATCH_MIN minutes after the flip, default 6 so that >= 5 min are AT REST): a fast sample every ~10 s (WS pid, users, HUBP2 underflow, ODM2 bit 10, VM fault, FLIP_PENDING, EARLIEST2, CRC, HUNG)
#      and a full K-snapshot every minute (DP stamps/s, DP presents/s, m6stat 0/1/3, m6xstat 0/1/2, bundle counters). (REVIEW CHECKS hook B runs in every full snapshot.)
#   P8 criterion 9 (USER_STEP=1 only, optional); criterion 7 kill-switch at the END (SKIP_KILL=1 leaves the GPU desktop live and skips criterion 7); P9 logs + verdicts + exit.
# ABORT (any HUBP2 underflow / ODM2 bit 10 / VM fault / HUNG / WindowServer pid change after the restart): quick capture (one ssh), THEN touch /private/tmp/n48m-noflip2, verify EARLIEST2 = A and CRC_A within 6 s,
#   then the full capture, then PARK. NO teardown verbs anywhere (no pipearm 0, disp2 off/planeoff, pipeagdc 0, dmubsend after fbhold, no second restart except PREHOLD=1's).
#
# ROLLBACK (documented, not automated): the 1a variant: ssh navi48 '~/navi48-staging/esp-config.sh stage17-native-1440-metal-disp-amfi-m6 && ~/navi48-staging/reboot-pc.sh' (0 users), wait-for-driver.sh 300.
# STOP RULES: PC unreachable after one more check -> "PC needs a manual reset"; two panics in a row on this build -> stop; a user logging in during the watch -> stop and report.
#
# PASS CRITERIA (spec "Run criteria") -> how this script measures each
#   R0  1a's K1-K6 re-confirmed BEFORE the first flip: AGDC nfb 2 (m6stat 1), 0x921 answered for a non-DP endpoint >= 1 (m6stat 1; E1 gives the monitor B its own timing), monitor B transactions > 0 and DP transactions growing
#       (m6stat 0), no ambiguous IDs / no no-framebuffer submits, DP stamps/s >= DP_MIN (m6stat 1 instance 0 diffed over 3 s), OTG0 COUNTING, the monitor B CRC == CRC_A with all underflow lines 0 while instance 2 is
#       NOT acquired (disp2 crc 2). K1's WS-log proof ("Unable to find display pipe" 0) and the ioreg display count are taken at the END (P9) because the log needs time to exist.
#   C1  Acquire2 OK with 3 slots: m6xstat 0 "acquired YES", m6xstat 1 three non-zero distinct slot MCs, none equal A or B (m6xstat 0) or any DP slot MC (bundle log "scanout: slot N ... MC 0x..."), Acquire2 refused 0.
#   C2  OTG2 CRC (disp2 crc 2) differs from CRC_A at a sample <= 20 s (PC clock) after the restart; at that sample EARLIEST2 (m6xstat 2) == one of the three slot MCs (m6xstat 1) and FLIP_PENDING 0 (<= 3 retries 2 s apart).
#   C3  at rest: two CRCs ~10 s apart equal (the fast samples' consecutive pairs), 0 CRC returns to CRC_A, "watchdog restores" (m6xstat 0) == 0 and the at-rest time (first equal pair -> last sample) >= 300 s.
#   C4  every fast sample (~10 s, i.e. more often than every minute): HUBP2 underflow 0, ODM2 bit 10 0, DCN_VM_FAULT_STATUS 0 (m6xstat 2) and HUNG 0 (sessstat 4): any non-zero ABORTS.
#   C5  REUSE_INUSE_REFUSED (m6xstat 0) and inst_mismatch (bundle 10 s "scanout (instance 2): counters" line) are both PARSED and reported; the target "0 actual reuse of an in-use slot" is NOT directly measurable
#       (see AMBIGUITY 4): reuse_inuse_refused > 0 is a WARNING (the bundle asked to reuse an in-use slot and was refused).
#   C6  DP unchanged: DP stamps/s (m6stat 1 instance 0, 3 s diff) >= DP_MIN every minute and >= 85% of minute 1; DP presents/s from the bundle's 10 s "m6:" line >= PRES_MIN from minute 2 on; OTG0 COUNTING and
#       DIG1_DIG_BE_CNTL vs the pre-arm value (a change is a WARNING) in disp2 status 2; the 15-row DP plane watch is the CLI's own and every disp2 op still prints it.
#   C7  kill switch at the END: touch /private/tmp/n48m-noflip2; within 6 s (PC clock) m6xstat 2 EARLIEST2 == A (m6xstat 0) AND disp2 crc 2 == CRC_A.
#   C9  USER_STEP=1 only: 'move the mouse onto the monitor B now'; m6stat/m6xstat + CRC every 2 s for 60 s; PASS = the CRC changes from its start value and ends stable (last 5 samples equal); else NOT-OBSERVED (not a fail).
#   C10 WindowServer pid stable after the restart, 0 HUNG, 0 new crash reports (find /Library/Logs/DiagnosticReports -newer a marker touched at P0, excluding nothing: a shutdown stall of an earlier boot cannot be newer).
#   (C8, the WindowServer kill, is NOT in this kit.)  P6b/P9 also take K1's final proofs (WS log, ioreg, K3 IDs 2-3 per pipe + 0 ambiguous, K2).
#
# AMBIGUITIES (for the reviewer):
#   1. "re-confirm 1a's K1-K6 for 60 s before anything flips": bundle 13 acquires instance 2 LAZILY at the first monitor B frame and reads /private/tmp/n48m-noflip2 ONCE at init (Navi48Device.m ~1139), so the flip comes
#      within seconds of the ONE restart and the window before it is shorter than 60 s. Default: R0 = the conditions on the last K-snapshot taken before the first flip (fast sample first, then a K-snapshot, then every ~10 s).
#      PREHOLD=1 gives the full 60 s (kill file present -> instance 2 stays OFF -> 1a's recipe for 60 s -> rm the file -> a SECOND restart) at the price of a second WindowServer restart (1a's rule was "never a second").
#   2. C3 "two CRCs 10 s apart equal": measured on consecutive fast samples >= 8 s apart (~10 s cadence); PASS needs >= 1 equal pair, 0 returns to CRC_A, 0 watchdog restores and >= 300 s from the first equal pair.
#      A monitor B desktop whose content keeps changing at rest (animation, clock) shows unequal pairs: they are reported, not gated.
#   3. C10 "0 new crash reports" = nothing newer than /tmp/n48-s1b-marker (touched at P0) in /Library/Logs/DiagnosticReports.
#   4. C5 "0 actual reuse of an in-use slot" is NOT directly measurable: REUSE_INUSE_REFUSED counts the refusals (the guard fired); an actual in-use overwrite would show as a flicker / CRC churn, not as a counter.
#   5. HUNG comes from `accel sessstat 4` ("open / DEAD slots / HUNG: a / b / c", navi48test.c:2304; page number SUSPECTED), not from a dedicated verb.
#   6. REVIEW "GPU-held code 4" aborts only once at least one present has been made: an acquired plane that has not been presented yet legitimately has EARLIEST2 = A (code 4 for A / B / unknown, contract item 11).
#   7. REVIEW "bundle cached table generation == kernel published generation within 2 s": the bundle prints its cached generation only in the 10 s counters line, so it is judged as "equal in two consecutive K-snapshots"
#      (abort on a miss in both). The kernel's published generation is m6xstat 0 "M6 table gen". The 'FLIP_PENDING stuck' check is 3 m6xstat 2 reads >= 100 ms apart (the CLI call time adds to the gap).
#   8. REVIEW "HUBP0 outside its own slot set" = the kext's row-14 rule inside the 15-row DP plane watch that every `disp2 crc 2` prints ("all 15 registers unchanged" / "DP DISTURBED"); there is no CLI read of HUBP0's address
#      against the DP slot set. "overlapping the monitor A pair": no monitor A plane exists on a 1b boot, so there is nothing to overlap (TODO-VERB: no verb lists it).
# TODO-VERB (no CLI verb exists; the kit uses the bundle's log instead): (a) the DP's slot MCs (bundle line "scanout: slot N: ... MC 0x..."); (b) the bundle-side inst_mismatch / drops / gpu_fail (bundle 10 s line
#   "scanout (instance 2): counters"); (c) a read of the bundle's table-refresh counters; (d) a kernel-side "actual in-use reuse" counter; (e) FREE VISIBLE VRAM before the bundle runs (only the bundle's own "free visible VRAM N MiB" log line at its acquire); (f) the bundle's cached table generation
#   (only the 10 s counters line); (g) the monitor A pair's MCs; (h) HUBP0's primary address vs the DP slot set. The GPU-HELD wording of disp2 status/crc goes to the kernel log only (N48LOG), not the CLI.
# LOGS: notes/logs/runs/m6-s1b/<timestamp>/ (gitignored): run.log, NN-*.txt per command, samples.tsv (fast), minutes.tsv (full), ws-pids.txt, summary.txt, capture-*.txt on a stop.
# Env: DRYRUN=1  WATCH_MIN=6  DP_MIN=40  PRES_MIN=20  USER_STEP=0  SKIP_KILL=0  PREHOLD=0  FLIP_S=20 (criterion 2's limit)  PRE_MAX_S=60  FAST_SLEEP=6  N48_HOST=navi48

set -u
set -o pipefail
ROOT=${N48_ROOT:-/path/to/navi48-checkout.nosync}
HOST=${N48_HOST:-navi48}
DRYRUN=${DRYRUN:-0}
WATCH_MIN=${WATCH_MIN:-6}
DP_MIN=${DP_MIN:-40}
PRES_MIN=${PRES_MIN:-20}
USER_STEP=${USER_STEP:-0}
SKIP_KILL=${SKIP_KILL:-0}
PREHOLD=${PREHOLD:-0}
FLIP_S=${FLIP_S:-20}
PRE_MAX_S=${PRE_MAX_S:-60}
FAST_SLEEP=${FAST_SLEEP:-6}
SSHO="-o BatchMode=yes -o ConnectTimeout=8"
TS=$(date '+%Y%m%d-%H%M%S')
RUN=$ROOT/notes/logs/runs/m6-s1b/$TS
[ "$DRYRUN" = 1 ] && { RUN=${DRYRUN_OUT:-/private/tmp/m6-s1b-run-dry-$TS}; WATCH_MIN=2; }
LOG=$RUN/run.log
mkdir -p "$RUN" || { echo "cannot create $RUN"; exit 1; }
T='~/navi48-staging/navi48test'
NUB='~/n48-metal/n48nub'
KILLFILE=/private/tmp/n48m-noflip2
CRC_RG=0x78097625; CRC_B=0x4bd14c15
DP_H_TOTAL_OK=0xa9f; DP_H_TOTAL_FALLBACK=0x897
WANT_KEXT=${WANT_KEXT:-0.0.661}; WANT_AUX=${WANT_AUX:-0.0.7}; WANT_BUNDLE=${WANT_BUNDLE:-13}
AUX_DIR=/Library/Extensions/Navi48Accel-$WANT_AUX.kext
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
FAILS=""; ABORTING=0
# stop <exit code> <reason>: capture + park + summary, never continues. (Before the restart / for user-login / unreachable: no kill file is touched.)
stop() {
  local code=$1; shift
  say; say "### STOP at $(stamp): $*"
  FAILS="$FAILS
  STOP: $*"
  if [ "$code" = 3 ]; then
    say "### PC needs a manual reset. Last command: see $LOG ; kext $WANT_KEXT, aux $WANT_AUX, bundle $WANT_BUNDLE, variant stage17-native-1440-metal-disp-amfi-m6flip"
  else
    capture
  fi
  summary_and_exit "$code"
}
# refuse <reason>: a precheck refusal BEFORE anything was written (exit 2): no capture needed beyond what the log already holds.
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

collect_ep() {  # S4: fetch the ep.d output (never fatal)
  [ "$DRYRUN" = 1 ] && { say "--- DRYRUN: ssh $HOST 'cat /tmp/ep.out /tmp/ep.err' > $RUN/ep.out"; return 0; }
  ssh $SSHO "$HOST" 'echo "== /tmp/ep.out"; cat /tmp/ep.out 2>&1; echo "== /tmp/ep.err"; cat /tmp/ep.err 2>&1; echo "== dtrace running: $(pgrep -x dtrace | tr "\n" " ")"' >"$RUN/ep.out" 2>&1 && say "  ep.d output fetched: $(grep -c '^ep ' "$RUN/ep.out") 'ep' lines -> $RUN/ep.out"
}
quick_capture() {  # ONE ssh, the four reads that matter most (the ABORT path takes them BEFORE the kill file is touched)
  say "### quick capture (one ssh; read-only)"
  [ "$DRYRUN" = 1 ] && { say "--- DRYRUN: ssh $HOST 'm6xstat 0|1|2; disp2 crc 2; disp2 status 2; sessstat 4' > $RUN/capture-quick.txt"; return 0; }
  ssh $SSHO "$HOST" "date +%s; for k in 'm6xstat 0' 'm6xstat 1' 'm6xstat 2' 'disp2 crc 2' 'disp2 status 2' 'sessstat 4'; do echo \"== \$k\"; sudo -n $T accel \$k 2>&1; done; echo \"WS pid: \$(pgrep -x WindowServer)\"" >"$RUN/capture-quick.txt" 2>&1
}
capture() {  # best effort, each command separately, bounded
  say "### capturing state (the PC stays parked; read-only commands only)"
  reach || { say "### PC unreachable while capturing: PC needs a manual reset (one more check done)"; return; }
  local k
  for k in "m6stat 0" "m6stat 1" "m6stat 2" "m6stat 3" "m6xstat 0" "m6xstat 1" "m6xstat 2" "pipestat 0" "pipestat 1" "pipestat 2" "pipestat 3" "pipestat 4" "disp2 status 2" "disp2 crc 2" "sessstat 4" "status"; do
    N=$((N+1)); ssh $SSHO "$HOST" "sudo -n $T accel $k 2>&1" >"$RUN/capture-$(printf '%s' "$k" | tr ' ' '-').txt" 2>&1
  done
  ssh $SSHO "$HOST" "sudo -n $T log 2>&1 | tail -500" >"$RUN/capture-driverlog.txt" 2>&1
  ssh $SSHO "$HOST" 'echo "up: $(uptime)"; echo "WS pid: $(pgrep -x WindowServer)"; echo "console: $(stat -f %Su /dev/console)"; who; ls -lt /Library/Logs/DiagnosticReports | head -8; find /Library/Logs/DiagnosticReports -newer /tmp/n48-s1b-marker 2>/dev/null; ioreg -c IOFramebuffer -r -d1 -w0 | grep -E "^\+-o "; kmutil showloaded --list-only 2>/dev/null | grep -iE "navi48|rdna4|accelprobe"; ls -l /private/tmp/n48m-noflip2 2>&1; tail -20 /var/tmp/n48watch.log 2>/dev/null' >"$RUN/capture-system.txt" 2>&1
  ssh $SSHO "$HOST" 'sudo -n log show --info --last 20m --predicate "process == \"WindowServer\" OR eventMessage CONTAINS \"Navi48Metal\" OR eventMessage CONTAINS \"n48accel\" OR eventMessage CONTAINS \"scanout\"" --style compact 2>/dev/null | tail -400' >"$RUN/capture-logshow.txt" 2>&1
  collect_ep
  say "### capture files: $RUN/capture-*.txt"
}

# ---------------------------------------------------------------------------------------------- criteria state
R0=PENDING; C1=PENDING; C2=PENDING; C3=PENDING; C4=PENDING; C5=PENDING; C6=PENDING; C7=PENDING; C9=PENDING; C10=PENDING; K1=PENDING; K2=PENDING; K3=PENDING
R0w=""; C1w=""; C2w=""; C3w=""; C4w=""; C5w=""; C6w=""; C7w=""; C9w=""; C10w=""; K1w=""; K2w=""; K3w=""; WARN=""
summary_and_exit() {
  local code=$1 S=$RUN/summary.txt
  { echo "M6 Stage 1b run $TS  (dry-run=$DRYRUN, watch ${WATCH_MIN} min, PREHOLD=$PREHOLD, USER_STEP=$USER_STEP, SKIP_KILL=$SKIP_KILL)  exit code $code"
    echo "R0  1a K1-K6 re-confirmed before the first flip        : $R0 $R0w"
    echo "C1  Acquire2 OK, 3 slots, MCs outside A/B/DP slots      : $C1 $C1w"
    echo "C2  CRC leaves CRC_A <= ${FLIP_S} s, EARLIEST2 a slot, no pend : $C2 $C2w"
    echo "C3  at rest: CRCs equal, 0 watchdog restores >= 5 min   : $C3 $C3w"
    echo "C4  underflow / ODM2 bit 10 / VM fault / HUNG all 0     : $C4 $C4w"
    echo "C5  reuse_inuse_refused and inst_mismatch reported      : $C5 $C5w"
    echo "C6  DP unchanged (stamps, presents/s, plane watch)      : $C6 $C6w"
    echo "C7  kill switch: EARLIEST2 = A and CRC_A within 6 s     : $C7 $C7w"
    echo "C9  mouse on the monitor B: CRC changes then stable (optional): $C9 $C9w"
    echo "C10 WS pid stable, 0 HUNG, 0 new crash reports          : $C10 $C10w"
    echo "(final 1a proofs: K1 $K1 $K1w | K2 $K2 $K2w | K3 $K3 $K3w)"
    [ -n "$WARN" ] && printf 'WARNINGS:%s\n' "$WARN"
    [ -n "$FAILS" ] && printf 'FAILURES / STOPS:%s\n' "$FAILS"
    echo "Logs: $RUN"; } | tee -a "$LOG" >"$S"
  exit "$code"
}

# ---------------------------------------------------------------------------------------------- PARSERS (every one is tested on synthetic text built from the real printf formats: see the DRYRUN notes in the brief report)
# All take a FILE (or stdin when the argument is "-") holding the verb's raw output and set globals. An unparsable field is left EMPTY (callers treat empty as a failure).
# m6xstat 0 -> X_LATCH ("navi48-m6 ON, navi48-m6flip ON" line verbatim), X_ACQ X_RESTORING X_WANT X_RFAILED (YES|no), X_GEN X_ACQUIRES, X_A X_B (hex, no 0x), X_PRES X_LATCHED X_REPL X_REFUSED X_POLL X_REUSE,
#   X_WDOG X_RESTORES X_RESFAIL X_ACQREF X_LASTREASON X_IDLE X_TGEN, X_GPUHELD (slot 0x2k or "" ), X_LASTADDR
parse_x0() {
  local t; t=$(cat "$1")
  X_LATCH=$(printf '%s\n' "$t" | sed -n 's/^accel m6xstat 0: \(navi48-m6 [A-Z]*, navi48-m6flip [A-Z]*\).*/\1/p' | head -1)
  X_ACQ=$(printf '%s\n' "$t" | sed -n 's/^  instance 2: acquired \([A-Za-z]*\),.*/\1/p' | head -1)
  X_RESTORING=$(printf '%s\n' "$t" | sed -n 's/^  instance 2: .*restoring \([A-Za-z]*\),.*/\1/p' | head -1)
  X_WANT=$(printf '%s\n' "$t" | sed -n 's/^  instance 2: .*wantRestore \([A-Za-z]*\),.*/\1/p' | head -1)
  X_RFAILED=$(printf '%s\n' "$t" | sed -n 's/^  instance 2: .*restoreFailed \([A-Za-z]*\),.*/\1/p' | head -1)
  X_GEN=$(printf '%s\n' "$t" | sed -n 's/^  instance 2: .*; generation \([0-9]*\), acquires \([0-9]*\).*/\1/p' | head -1)
  X_ACQUIRES=$(printf '%s\n' "$t" | sed -n 's/^  instance 2: .*; generation \([0-9]*\), acquires \([0-9]*\).*/\2/p' | head -1)
  X_A=$(printf '%s\n' "$t" | sed -n 's/^  A (console) 0x\([0-9a-f]*\)  B 0x\([0-9a-f]*\).*/\1/p' | head -1)
  X_B=$(printf '%s\n' "$t" | sed -n 's/^  A (console) 0x\([0-9a-f]*\)  B 0x\([0-9a-f]*\).*/\2/p' | head -1)
  local l; l=$(printf '%s\n' "$t" | sed -n 's/^  presents \([0-9]*\), latched \([0-9]*\), replaced \([0-9]*\), refused \([0-9]*\), latched-by-poll \([0-9]*\), REUSE_INUSE_REFUSED \([0-9]*\).*/\1 \2 \3 \4 \5 \6/p' | head -1)
  read -r X_PRES X_LATCHED X_REPL X_REFUSED X_POLL X_REUSE <<<"$l"
  l=$(printf '%s\n' "$t" | sed -n 's/^  watchdog restores \([0-9]*\), restores \([0-9]*\) (failed \([0-9]*\)); Acquire2\{0,1\} refused \([0-9]*\) (last reason \([0-9]*\):.*); idle \([0-9]*\) ms; M6 table gen \([0-9]*\).*/\1 \2 \3 \4 \5 \6 \7/p' | head -1)
  read -r X_WDOG X_RESTORES X_RESFAIL X_ACQREF X_LASTREASON X_IDLE X_TGEN <<<"$l"
  X_GPUHELD=$(printf '%s\n' "$t" | sed -n 's/^  \.\.\. slot \(0x[0-9a-f]*\).*/\1/p' | head -1)
  X_GH4=0; printf '%s\n' "$t" | /usr/bin/grep -q 'GPU-HELD: EARLIEST_INUSE2 is not a registered slot' && X_GH4=1
  X_LASTADDR=$(printf '%s\n' "$t" | sed -n 's/^  last restore [0-9]* us; last address written 0x\([0-9a-f]*\).*/\1/p' | head -1)
}
# m6xstat 1 -> X_S0 X_S1 X_S2 (slot MCs, hex no 0x), X_SP0..2 (presents), X_WREF X_WFAIL X_UNTAG X_GEOM, X_FRONT X_PEND (slot ids, hex no 0x; ffffffff = none)
parse_x1() {
  local t i v; t=$(cat "$1")
  for i in 0 1 2; do
    v=$(printf '%s\n' "$t" | sed -n "s/^  slot 0x2$i: MC 0x\([0-9a-f]*\), presents \([0-9]*\), latches \([0-9]*\).*/\1 \2/p" | head -1)
    eval "X_S$i=\${v%% *}; X_SP$i=\${v#* }"; [ -n "$v" ] || eval "X_S$i=''; X_SP$i=''"
  done
  local l; l=$(printf '%s\n' "$t" | sed -n 's/^  writer refusals \([0-9]*\), write failures \([0-9]*\); untagged presents refused \([0-9]*\); geometry refusals \([0-9]*\).*/\1 \2 \3 \4/p' | head -1)
  read -r X_WREF X_WFAIL X_UNTAG X_GEOM <<<"$l"
  l=$(printf '%s\n' "$t" | sed -n 's/.*front slot 0x\([0-9a-f]*\), pending slot 0x\([0-9a-f]*\) (0xffffffff = none); last address written.*/\1 \2/p' | head -1)
  read -r X_FRONT X_PEND <<<"$l"
}
# m6xstat 2 -> X_UF (underflow 0..7) X_BLANK X_VTG X_VMF (hex, no 0x) X_ODM10 X_OTGFC, X_FP (FLIP_PENDING) X_PROG X_EARLY (hex addresses, no 0x), X_STAGE X_CUR
parse_x2() {
  local t l; t=$(cat "$1")
  l=$(printf '%s\n' "$t" | sed -n 's/^  HUBP2_DCHUBP_CNTL 0x[0-9a-f]* (underflow \([0-9]*\), BLANK_EN \([0-9]*\), VTG_SEL \([0-9]*\)); DCN_VM_FAULT_STATUS 0x\([0-9a-f]*\); ODM2 OPTC_INPUT_GLOBAL_CONTROL 0x[0-9a-f]* (bit 10 \([0-9]*\)); OTG2 frame counter \([0-9]*\).*/\1 \2 \3 \4 \5 \6/p' | head -1)
  read -r X_UF X_BLANK X_VTG X_VMF X_ODM10 X_OTGFC <<<"$l"
  l=$(printf '%s\n' "$t" | sed -n 's/^  FLIP_CONTROL 0x[0-9a-f]* (FLIP_PENDING \([0-9]*\)); viewport 0x[0-9a-f]*; programmed 0x\([0-9a-f]*\):0x\([0-9a-f]*\); EARLIEST_INUSE2 0x\([0-9a-f]*\):0x\([0-9a-f]*\);.*/\1 \2 \3 \4 \5/p' | head -1)
  local fp ph pl eh el; read -r fp ph pl eh el <<<"$l"
  X_FCR=$(printf '%s\n' "$t" | sed -n 's/^  FLIP_CONTROL 0x\([0-9a-f]*\) (FLIP_PENDING.*/\1/p' | head -1)
  l=$(printf '%s\n' "$t" | sed -n 's/.*; pitch 0x\([0-9a-f]*\); SURFACE_CONFIG 0x\([0-9a-f]*\); SURFACE_CONTROL 0x\([0-9a-f]*\); VMID_SETTINGS_0 0x\([0-9a-f]*\); OTG2_CONTROL.*/\1 \2 \3 \4/p' | head -1)
  read -r X_PITCH X_SCFG X_SCTL X_VMID <<<"$l"
  X_FP=$fp; X_PROG=""; X_EARLY=""
  [ -n "$ph" ] && X_PROG=$(addr "$ph" "$pl"); [ -n "$eh" ] && X_EARLY=$(addr "$eh" "$el")
  l=$(printf '%s\n' "$t" | sed -n 's/^  disp2 plane state of instance 2: stage \([0-9]*\) (6 = HELD), cur \([0-9]*\) (0 = A), pair pinned \([0-9]*\);.*/\1 \2/p' | head -1)
  read -r X_STAGE X_CUR <<<"$l"
}
# disp2 crc 2 -> CRC_RG_NOW CRC_B_NOW UF_LINE ODM_OCC VMF UF_OK (the CLI's own gate lines: kept as in 1a; the HUBP2/ODM2/VM health GATE of 1b is m6xstat 2)
parse_crc() {
  local t; t=$(cat "$1")
  CRC_RG_NOW=$(printf '%s' "$t" | sed -n 's/.*OTG2 CRC *: RG \(0x[0-9a-f]*\) *B \(0x[0-9a-f]*\).*/\1/p' | head -1)
  CRC_B_NOW=$(printf '%s' "$t" | sed -n 's/.*OTG2 CRC *: RG \(0x[0-9a-f]*\) *B \(0x[0-9a-f]*\).*/\2/p' | head -1)
  UF_LINE=$(printf '%s' "$t" | /usr/bin/grep 'HUBP2_DCHUBP_CNTL' | head -1 | sed -n 's/.*\(underflow(30:28) [0-9]*  *SEG_ALLOC_ERR [0-9]*  *TIMEOUT(23:20) [0-9a-fx]*\).*/\1/p')
  ODM_OCC=$(printf '%s' "$t" | sed -n 's/.*underflow occurred(10) \([0-9]\).*/\1/p' | head -1)
  VMF=$(printf '%s' "$t" | sed -n 's/.*DCN_VM_FAULT_STATUS *: *\([0-9a-fx]*\).*/\1/p' | head -1)
  DPW_OK=0; printf '%s' "$t" | /usr/bin/grep -q 'DP plane watch .*all 15 registers unchanged' && ! printf '%s' "$t" | /usr/bin/grep -q 'DP DISTURBED' && DPW_OK=1
  UF_OK=0
  [ "$UF_LINE" = "underflow(30:28) 0  SEG_ALLOC_ERR 0  TIMEOUT(23:20) 0" ] && [ "$ODM_OCC" = 0 ] && { [ "$VMF" = 0000000000 ] || [ "$VMF" = 0x0 ] || [ "$VMF" = 0 ]; } && UF_OK=1
}
# sessstat 4 -> HUNG (0|1; empty = unparsable). SUSPECTED page number: n48d2 / G2 page 4 prints "open / DEAD slots / HUNG: a / b / c" (navi48test.c:2304).
parse_hung() { HUNG=$(sed -n 's/^  open \/ DEAD slots \/ HUNG: [0-9]* \/ [0-9]* \/ \([0-9]*\).*/\1/p' "$1" | head -1); }
# m6stat 0 -> M_TXN0 M_TXN2 M_IDS0 M_IDS2 M_AMB M_NOFB M_PIPES ; m6stat 1 -> M_AGN M_NONDP M_LASTEP M_STAMP0 (instance 0 stamps) M_STAMP2 ; page 2 -> M_AMB2 (count of AMBIGUOUS)
parse_m6() { # parse_m6 <m6stat0 file> <m6stat1 file> <m6stat2 file>
  M_TXN0=$(sed -n 's/^ *instance 0 .*: \([0-9][0-9]*\) transactions, \([0-9][0-9]*\) surface IDs seen.*/\1/p' "$1" | head -1)
  M_IDS0=$(sed -n 's/^ *instance 0 .*: \([0-9][0-9]*\) transactions, \([0-9][0-9]*\) surface IDs seen.*/\2/p' "$1" | head -1)
  M_TXN2=$(sed -n 's/^ *instance 2 .*: \([0-9][0-9]*\) transactions, \([0-9][0-9]*\) surface IDs seen.*/\1/p' "$1" | head -1)
  M_IDS2=$(sed -n 's/^ *instance 2 .*: \([0-9][0-9]*\) transactions, \([0-9][0-9]*\) surface IDs seen.*/\2/p' "$1" | head -1)
  M_AMB=$(sed -n 's/^ *ambiguous IDs (seen on two pipes) *: \([0-9]*\).*/\1/p' "$1" | head -1)
  M_NOFB=$(sed -n 's/^ *submits on a pipe with no known framebuffer: \([0-9]*\).*/\1/p' "$1" | head -1)
  M_PIPES=$(sed -n 's/.* \([0-9]*\) pipes recorded.*/\1/p' "$1" | head -1)
  M_AGN=$(sed -n 's/.*AGDC: \([0-9]*\) framebuffers in the last 0x980 reply.*/\1/p' "$2" | head -1)
  M_NONDP=$(sed -n 's/.*0x921 answered for a non-DP endpoint \([0-9]*\) times.*/\1/p' "$2" | head -1)
  M_LASTEP=$(sed -n 's/.*last endpoint dword \([0-9]*\).*/\1/p' "$2" | head -1)
  M_STAMP0=$(sed -n 's/^ *instance 0 .*: \([0-9][0-9]*\) vblank stamps written.*/\1/p' "$2" | head -1)
  M_STAMP2=$(sed -n 's/^ *instance 2 .*: \([0-9][0-9]*\) vblank stamps written.*/\1/p' "$2" | head -1)
  M_AMB2=$(/usr/bin/grep -c 'AMBIGUOUS' "$3")
}
# the bundle's lines: DP presents/s from the latest "m6:" line vs the newest line >= 50 s earlier ("RATE unknown_delta secs" or "none"); instance-2 counters -> B_MISMATCH B_GPUFAIL B_DROPS B_WDOG B_PRES2
bundle_rate() { # bundle_rate <file with the m6: lines>
  awk '{ split($2,a,":"); T[NR]=a[1]*3600+a[2]*60+a[3]; v=$0; sub(/.*presented to instance 0 /,"",v); V[NR]=v+0; u=$0; sub(/.*unknown /,"",u); U[NR]=u+0 }
    END { for (k=NR-1; k>=1; k--) if (T[NR]-T[k] >= 50) { printf "%d %d %d\n", (V[NR]-V[k])/(T[NR]-T[k]), U[NR]-U[k], T[NR]-T[k]; exit } print "none" }' "$1"
}
parse_bundle2() { # parse_bundle2 <file with "scanout (instance 2): counters" lines> (the LAST line)
  local l; l=$(/usr/bin/grep 'scanout (instance 2): counters' "$1" | tail -1)
  B_PRES2=$(printf '%s' "$l" | sed -n 's/.*counters: presents \([0-9]*\) (kernel presents.*/\1/p')
  B_WDOG=$(printf '%s' "$l" | sed -n 's/.*watchdog_restores \([0-9]*\)).*/\1/p')
  B_DROPS=$(printf '%s' "$l" | sed -n 's/.* drops \([0-9]*\) gpu_fail.*/\1/p')
  B_GPUFAIL=$(printf '%s' "$l" | sed -n 's/.* gpu_fail \([0-9]*\) stale_skipped.*/\1/p')
  B_MISMATCH=$(printf '%s' "$l" | sed -n 's/.*; planned [0-9]* inst_mismatch \([0-9]*\) stale_retry.*/\1/p')
  B_CACHED=$(printf '%s' "$l" | sed -n 's/.*table gen [0-9]* (cached \([0-9]*\),.*/\1/p')
  B_PLAN=$(printf '%s' "$l" | sed -n 's/.*; planned \([0-9]*\) inst_mismatch.*/\1/p')
}
# DP raster: reg[0x4fea] = 0x00000897 -> RASTER_HEX (no leading zeros, lowercase)
parse_reg() { RASTER_HEX=$(sed -n 's/^reg\[0x4fea\] = 0x\([0-9a-f]*\).*/\1/p' "$1" | head -1); [ -n "$RASTER_HEX" ] && RASTER_HEX=$(hexn "$RASTER_HEX"); }
# the DP side line of disp2 status 2 -> DP_OTG0 DP_BE
parse_dpside() { local l; l=$(sed -n 's/.*DP side: OTG0 \([A-Z]*\).*DIG1_DIG_BE_CNTL \(0x[0-9a-f]*\).*/\1 \2/p' "$1" | head -1); DP_OTG0=${l%% *}; DP_BE=${l#* }; }

# ---------------------------------------------------------------------------------------------- remote snapshot scripts (one ssh each)
# FAST: WS pid, users, PC clock, m6xstat 0 / 2, disp2 crc 2, HUNG. ~4 CLI calls.
fast_remote() {
  cat <<EOS
echo "@@T"; date +%s
echo "@@WS"; pgrep -x WindowServer
echo "@@USERS"; stat -f %Su /dev/console; who
echo "@@X0"; sudo -n $T accel m6xstat 0 2>&1
echo "@@X2"; sudo -n $T accel m6xstat 2 2>&1
echo "@@CRC"; sudo -n $T accel disp2 crc 2 2>&1
echo "@@HUNG"; sudo -n $T accel sessstat 4 2>&1
EOS
}
# FULL (K-snapshot): everything the 1a minute snapshot read + m6stat 3 + m6xstat 0/1/2 + the bundle's m6: and instance-2 counter lines + the newest report + the marker find.
snap_remote() {
  cat <<EOS
echo "@@T"; date +%s
echo "@@WS"; pgrep -x WindowServer
echo "@@USERS"; stat -f %Su /dev/console; who
echo "@@STAMPA"; sudo -n $T accel m6stat 1 2>&1 | grep '^ *instance 0 '
sleep 3
echo "@@STAMPB"; sudo -n $T accel m6stat 1 2>&1 | grep '^ *instance 0 '
echo "@@X0"; sudo -n $T accel m6xstat 0 2>&1
echo "@@X1"; sudo -n $T accel m6xstat 1 2>&1
echo "@@X2"; sudo -n $T accel m6xstat 2 2>&1
echo "@@CRC"; sudo -n $T accel disp2 crc 2 2>&1
echo "@@T2"; date +%s
echo "@@STATUS"; sudo -n $T accel disp2 status 2 2>&1
echo "@@M6_0"; sudo -n $T accel m6stat 0 2>&1
echo "@@M6_1"; sudo -n $T accel m6stat 1 2>&1
echo "@@M6_2"; sudo -n $T accel m6stat 2 2>&1
echo "@@M6_3"; sudo -n $T accel m6stat 3 2>&1
echo "@@HUNG"; sudo -n $T accel sessstat 4 2>&1
echo "@@BUNDLE"; sudo -n log show --info --last 4m --predicate 'eventMessage CONTAINS "m6: surfaces for instance" OR eventMessage CONTAINS "scanout (instance 2): counters" OR eventMessage CONTAINS "free visible VRAM"' --style compact 2>/dev/null | grep 'WindowServer\[' | tail -40
echo "@@REPORTS"; find /Library/Logs/DiagnosticReports -newer /tmp/n48-s1b-marker 2>/dev/null
EOS
}

# ---------------------------------------------------------------------------------------------- P0 prechecks
say "### M6 Stage 1b RUN $TS dry-run=$DRYRUN host=$HOST watch=${WATCH_MIN}min DP_MIN=$DP_MIN PREHOLD=$PREHOLD USER_STEP=$USER_STEP SKIP_KILL=$SKIP_KILL out=$RUN"
say "### host Mac date: $(date)"
say; say "=== P0 prechecks"
reach || stop 3 "PC ($HOST) not reachable"
px pre-system 'echo "up: $(uptime)"; echo "boottime: $(sysctl -n kern.boottime)"; echo "boot-args: $(sysctl -n kern.bootargs)"; echo "console: $(stat -f %Su /dev/console)"; echo "who:"; who; echo "WS: $(pgrep -x WindowServer)"; echo "loaded:"; kmutil showloaded --list-only 2>/dev/null | grep -iE "navi48|rdna4|accelprobe"; echo "bundle: $(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" /Library/GPUBundles/Navi48Metal.bundle/Contents/Info.plist 2>&1)"; echo "ESP: $(~/navi48-staging/esp-kext.sh show 2>&1 | grep "injected kext")"; echo "nub:"; sudo -n ~/n48-metal/n48nub status 2>&1; echo "noflip2: $(ls /private/tmp/n48m-noflip2 2>&1)"; echo "newest reports:"; ls -lt /Library/Logs/DiagnosticReports 2>&1 | head -3' || stop 3 "system read failed"
need "console user is root (0 users)" has '^console: root$'
need "who lists nobody" bash -c '! printf "%s\n" "$1" | sed -n "/^who:/,/^WS:/p" | sed "1d;\$d" | /usr/bin/grep -q .' _ "$PXOUT"
need "boot-args carry navi48-m6=1 AND navi48-m6flip=1" bash -c 'for t in navi48-m6=1 navi48-m6flip=1; do printf "%s" "$1" | /usr/bin/grep -qE -- "(^| )$t( |$)" || exit 1; done' _ "$PXOUT"
need "boot-args carry navi48-fb2=1 navi48-metal-disp=1 navi48-disp2=1 navi48-dmubcmd=1" bash -c 'for t in navi48-fb2=1 navi48-metal-disp=1 navi48-disp2=1 navi48-dmubcmd=1; do printf "%s" "$1" | /usr/bin/grep -q -- "$t" || exit 1; done' _ "$PXOUT"
need "ESP names $WANT_KEXT" has "injected kext: $WANT_KEXT"
need "bring-up kext $WANT_KEXT loaded" has "navi48.*$WANT_KEXT|bringup.*$WANT_KEXT"
need "bundle $WANT_BUNDLE installed" has "^bundle: $WANT_BUNDLE\$"
need "the Metal nub is NOT published yet (fresh boot: one run per boot)" has 'NO Navi48MetalNub'
need "the kill file $KILLFILE is absent (instance 2 may be enabled)" has "^noflip2: ls: .*No such file"
WS0=$(printf '%s' "$PXOUT" | sed -n 's/^WS: //p')
PRE_REPORT=$(printf '%s' "$PXOUT" | sed -n '/^newest reports:/,$p' | sed -n '2p')
say "  WindowServer pid at start: $WS0 ; newest report: $PRE_REPORT"
echo "$(date +%s) start $WS0" >"$RUN/ws-pids.txt"
# THE DP-RASTER CHECK FIRST (queue 292): a fresh user boot can come up with the DP in the 1080p fallback; the disp2 plane precheck then refuses with OTG0_H_TOTAL 0x4fea = 0x897. Detect it HERE, write nothing.
px dp-raster "sudo -n $T reg 0x4fea 2>&1" || stop 1 "reg 0x4fea unreadable"
if [ "$DRYRUN" != 1 ]; then
  printf '%s\n' "$PXOUT" >"$RUN/dp-raster.txt"; parse_reg "$RUN/dp-raster.txt"
  say "  DP OTG0_H_TOTAL (0x4fea) = 0x${RASTER_HEX:-?}   (full-res 1440p60 raster: $DP_H_TOTAL_OK ; the 1080p fallback of queue 292: $DP_H_TOTAL_FALLBACK)"
  [ -n "$RASTER_HEX" ] || stop 1 "could not parse reg 0x4fea ('$PXOUT')"
  [ "0x$RASTER_HEX" = "$DP_H_TOTAL_FALLBACK" ] && refuse "the DP came up in the 1080p FALLBACK (OTG0_H_TOTAL 0x4fea = $DP_H_TOTAL_FALLBACK, not $DP_H_TOTAL_OK): the PC needs a warm restart (queue 292: a warm restart or booting Windows fixes it). The monitor B chain was NOT run."
  [ "0x$RASTER_HEX" = "$DP_H_TOTAL_OK" ] || refuse "the DP raster OTG0_H_TOTAL is 0x$RASTER_HEX, neither the 1440p value $DP_H_TOTAL_OK nor the known 1080p fallback: the PC needs a warm restart / a look before the monitor B chain. Nothing was written."
else say "  DRYRUN would require: DP OTG0_H_TOTAL == $DP_H_TOTAL_OK, and REFUSE with 'needs a warm restart' on $DP_H_TOTAL_FALLBACK"; fi
# marker for criterion 10 (a file in /tmp on the PC; harmless)
px marker 'touch /tmp/n48-s1b-marker; ls -l /tmp/n48-s1b-marker' || stop 1 "marker touch failed"
# the aux matches on the Metal nub, so it is NOT loaded before P4's nub publish: here only the aux KC and the aux bundle's path/version are checked
px auxkc "kmutil inspect -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>&1 | grep -i accelprobe; echo version: \$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' $AUX_DIR/Contents/Info.plist 2>&1)" || stop 3 "aux KC read failed"
need "aux KC carries com.navi48.accelprobe $WANT_AUX" has "accelprobe.*$WANT_AUX|$WANT_AUX.*accelprobe"
need "aux $WANT_AUX is still at $AUX_DIR" has "^version: $WANT_AUX\$"
# m6stat / m6xstat baselines (also the positive controls of their parsers)
px pre-m6stat "sudo -n $T accel m6stat 0 2>&1; sudo -n $T accel m6stat 1 2>&1; sudo -n $T accel m6stat 2 2>&1" || stop 1 "m6stat unreadable"
need "m6stat: navi48-m6 latch ON" has 'navi48-m6 latch ON'
need "m6stat: nothing adopted / armed yet (fresh boot)" has 'pipe adopted no; armed no'
need "m6stat: 0 pipes recorded before the recipe" has ' 0 pipes recorded'
need "m6stat parser control: instance 0 line present" has 'instance 0 \(DP, OTG0\) +: [0-9]+ transactions'
px pre-m6xstat "sudo -n $T accel m6xstat 0 2>&1; sudo -n $T accel m6xstat 1 2>&1; sudo -n $T accel m6xstat 2 2>&1" || stop 1 "m6xstat unreadable (the CLI predates action 107?)"
need "m6xstat: navi48-m6 ON, navi48-m6flip ON (instance 2 reachable)" has '^accel m6xstat 0: navi48-m6 ON, navi48-m6flip ON$'
if [ "$DRYRUN" != 1 ]; then
  printf '%s\n' "$PXOUT" >"$RUN/pre-x.txt"; parse_x0 "$RUN/pre-x.txt"
  say "  m6xstat 0 baseline: acquired=$X_ACQ restoring=$X_RESTORING acquires=$X_ACQUIRES A=0x$X_A B=0x$X_B watchdog=$X_WDOG reuse_refused=$X_REUSE"
  [ "$X_ACQ" = no ] && [ -n "$X_A" ] && [ -n "$X_WDOG" ] && [ -n "$X_REUSE" ] || stop 1 "m6xstat 0 parser control failed or instance 2 is already acquired (acq='$X_ACQ' A='$X_A')"
fi
# DP baseline (positive control: the DP-side line must parse)
px pre-dp "sudo -n $T accel disp2 status 2 2>&1" || stop 1 "disp2 status 2 failed"
if [ "$DRYRUN" != 1 ]; then printf '%s\n' "$PXOUT" >"$RUN/pre-dp.txt"; parse_dpside "$RUN/pre-dp.txt"; DP0="$DP_OTG0 $DP_BE"; else DP0=""; fi
say "  DP baseline (OTG0 state, DIG1_DIG_BE_CNTL): '$DP0'"
need "DP baseline parsed and OTG0 COUNTING" bash -c '[ "${1%% *}" = COUNTING ]' _ "$DP0"
DP_BE0=${DP0#* }
echo "$(date +%s)" >"$RUN/t0"

# ---------------------------------------------------------------------------------------------- P2 monitor B chain (exactly 1a)
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
  if [ "$DRYRUN" != 1 ]; then parse_crc "$PXF"; fi
  say "  CRC RG=${CRC_RG_NOW:-?} B=${CRC_B_NOW:-?} ; '${UF_LINE:-?}' ; ODM2 occurred=${ODM_OCC:-?} ; VM fault=${VMF:-?} -> underflow-clean=${UF_OK:-?}"
}
crc_read crc-A
need "monitor B CRC_A exact ($CRC_RG / $CRC_B) BEFORE the holds" bash -c '[ "$1" = "$3" ] && [ "$2" = "$4" ]' _ "${CRC_RG_NOW:-}" "${CRC_B_NOW:-}" "$CRC_RG" "$CRC_B"
need "monitor B underflow lines all 0 BEFORE the holds" [ "${UF_OK:-0}" = 1 ]

# ---------------------------------------------------------------------------------------------- P3 hold + publish
say; say "=== P3 fbhold 2 (IRREVERSIBLE this boot: after it NO dmubsend, no teardown) and fbpublish 2"
px fbhold "sudo -n $T accel disp2 fbhold 2 2>&1" || stop 1 "fbhold 2 failed"
need "fbhold 2: status 0" has 'fbhold 2: status 0 \(OK\)'
crc_read crc-after-hold
need "monitor B CRC_A after the hold" bash -c '[ "$1" = "$3" ] && [ "$2" = "$4" ]' _ "${CRC_RG_NOW:-}" "${CRC_B_NOW:-}" "$CRC_RG" "$CRC_B"
px fbpublish "sudo -n $T accel fbpublish 2 2>&1" || stop 1 "fbpublish 2 failed"
need "fbpublish 2: status 0 (the nub is published, Navi48DisplayIndex 1)" has 'fbpublish 2: status 0 \(OK'
nap 5
px fbnode 'ioreg -c IOFramebuffer -r -d1 -w0 | grep -E "^\+-o "; echo "Navi48Framebuffer nodes: $(ioreg -c Navi48Framebuffer -r -d1 -w0 | grep -cE "^\+-o Navi48Framebuffer")"' || true
need "1 Navi48Framebuffer node exists (the aux framebuffer started on the monitor B's nub)" has 'Navi48Framebuffer nodes: 1'
crc_read crc-after-publish
need "monitor B CRC_A after fbpublish" bash -c '[ "$1" = "$3" ] && [ "$2" = "$4" ]' _ "${CRC_RG_NOW:-}" "${CRC_B_NOW:-}" "$CRC_RG" "$CRC_B"

# ---------------------------------------------------------------------------------------------- P4 nub, adopt, agdc, fbname, arm
say; say "=== P4 nub publish -> aux loaded -> pipeadopt (every pipe) -> pipeagdc 1 (nfb = 2 from the published nubs) -> fbname 1 -> headless flag -> pipearm 1   (lines mirror arm-gpu-desktop.sh)"
px nubpublish "sudo -n $NUB publish 2>&1" || stop 1 "n48nub publish failed"
need "nub publish: 0 (Success)" has 'publish: 0 \(Success\)'
nap 3; px auxloaded "kmutil showloaded --list-only 2>/dev/null | grep -iE accelprobe" || true
need "aux $WANT_AUX loaded after the nub publish (kmutil showloaded)" has "accelprobe.*\\($WANT_AUX\\)"
nap 2
px pipeadopt "sudo -n $T accel pipeadopt 2>&1" || stop 1 "pipeadopt failed (a non-OK status is a CLEAN refusal: the pipe check is per pipe, not pipe 0 only)"
need "pipeadopt: status 0 (OK) (accepted every pipe)" has 'status +: 0 \(OK\)'
px pipeagdc "sudo -n $T accel pipeagdc 1 2>&1" || stop 1 "pipeagdc failed"
need "pipeagdc: agdc status 0 (PUBLISHED)" has 'agdc status +: 0 \(PUBLISHED\)'
need "pipeagdc not REFUSED (the display nub's framebuffer has started)" bash -c '! printf "%s" "$1" | /usr/bin/grep -q REFUSED' _ "$PXOUT"
px fbname "sudo -n $T accel fbname 1 2>&1" || stop 1 "fbname failed"
need "fbname: class name NOW AMDRDNA4" has 'class name NOW +: AMDRDNA4'
px headless 'sudo -n touch /private/tmp/n48m-headless-no; sudo -n chmod 644 /private/tmp/n48m-headless-no; ls -l /private/tmp/n48m-headless-no' || stop 1 "headless flag failed"
px pipearm "sudo -n $T accel pipearm 1 2>&1 | grep 0xccf" || stop 1 "pipearm failed"
need "pipearm: accel+0xccf now 1" has '0xccf now +: 1'
# last look before the restart: monitor B CRC_A, DP counting, instance 2 NOT acquired, nothing yet routed
crc_read crc-before-restart
need "monitor B CRC_A before the WindowServer restart" bash -c '[ "$1" = "$3" ] && [ "$2" = "$4" ]' _ "${CRC_RG_NOW:-}" "${CRC_B_NOW:-}" "$CRC_RG" "$CRC_B"
px pre-restart-state "sudo -n $T accel m6stat 0 2>&1; sudo -n $T accel m6xstat 0 2>&1; sudo -n $T accel pipestat 4 2>&1 | grep 'stamps written'; pgrep -x WindowServer; stat -f %Su /dev/console; who" || true
WSB=$(printf '%s' "$PXOUT" | /usr/bin/grep -E '^[0-9]+$' | head -1)
need "still 0 users right before the restart (console root, no who lines)" bash -c 'printf "%s\n" "$1" | /usr/bin/grep -qx root && ! printf "%s\n" "$1" | /usr/bin/grep -qE "^[a-z_][a-z0-9_]* +(console|ttys[0-9]+) "' _ "$PXOUT"
need "instance 2 not acquired before the restart (m6xstat 0)" has 'instance 2: acquired no,'

# ======================================================================== REVIEW CHECKS (hook A: BEFORE the first flip) ========================================================================
# From the Opus safety review of 0.0.661 ("Run-time checks the kit must make", NATIVE-S7-MULTIMON.md last section). This section DEFINES every review check; hook A (review_checks_a) runs it before the WindowServer
# restart through `stop 1`; the same plane baseline (rc_every, mode pre) re-runs on EVERY sample until the first flip ("again right before the first flip where possible"); hook B (review_checks_b, rc_every mode post) runs
# after the flip. Every ABORT goes through flip_abort (quick capture -> touch the kill file -> require EARLIEST2 = programmed = A within 6 s -> else "reboot without freeing anything"); NEVER a teardown verb.
SLOTLEN=14745600; A_HI=""; RC_GENMISS=0; DPCON=""; DPSLOTS=""; LEAK_BASE=0; RC_SLOTS_KNOWN=0
in_set() { local x=$1; shift; local y; for y in "$@"; do [ "$x" = "$y" ] && return 0; done; return 1; }
ov() { [ $((0x$1)) -lt $((0x$2 + SLOTLEN)) ] && [ $((0x$2)) -lt $((0x$1 + SLOTLEN)) ]; }   # two SLOTLEN-byte ranges overlap
rc_fail() { RC_BAD="$RC_BAD $1;"; }
rc_plane_pre() { # acquired = no: the plane must be exactly the 1a baseline (A, nothing pending, geometry)
  { [ -z "${X_A:-}" ] || [ "$X_A" = 0 ]; } && [ -n "${A_HELD:-}" ] && X_A=$A_HELD   # before Acquire2 m6xstat prints A 0x0: use the HELD plane's A read in hook A
  [ "${X_UF:-x}" = 0 ] || rc_fail "HUBP2 underflow ${X_UF:-?}"; [ "${X_BLANK:-x}" = 0 ] || rc_fail "BLANK_EN ${X_BLANK:-?}"; [ "${X_VTG:-x}" = 2 ] || rc_fail "VTG_SEL ${X_VTG:-?} (want 2)"
  [ -n "$X_VMF" ] && [ $((0x$X_VMF)) = 0 ] || rc_fail "VM fault 0x${X_VMF:-?}"; [ "${X_ODM10:-x}" = 0 ] || rc_fail "ODM2 bit 10 ${X_ODM10:-?}"
  [ -n "$X_FCR" ] && [ $(( 0x$X_FCR & 0x103 )) = 0 ] || rc_fail "FLIP_CONTROL 0x${X_FCR:-?} bits 0/1/8 not all 0"
  [ -n "$X_A" ] && [ "$X_PROG" = "$X_A" ] && [ "$X_EARLY" = "$X_A" ] || rc_fail "programmed 0x${X_PROG:-?} / EARLIEST 0x${X_EARLY:-?} != A 0x${X_A:-?}"
  [ "${X_PITCH:-x}" = 9ff ] || rc_fail "pitch 0x${X_PITCH:-?} (want 0x9ff)"; [ "${X_SCFG:-x}" = 8 ] || rc_fail "SURFACE_CONFIG 0x${X_SCFG:-?} (want 8)"
  [ "${X_SCTL:-x}" = 0 ] || rc_fail "SURFACE_CONTROL 0x${X_SCTL:-?}"; [ "${X_VMID:-x}" = 0 ] || rc_fail "VMID_SETTINGS_0 0x${X_VMID:-?}"
}
rc_fp_stuck() { # FLIP_PENDING stuck across 3 reads 100 ms apart (one ssh); 0 = stuck
  [ "$DRYRUN" = 1 ] && return 1
  # stuck = pending in all 3 reads AND the same programmed/EARLIEST each time (no progress). A monitor B flipping every frame
  # (the login window can live on the monitor B, run 20261008-104515) reads pending most of the time while it IS latching.
  local l n u; l=$(ssh $SSHO "$HOST" "for i in 1 2 3; do sudo -n $T accel m6xstat 2 2>&1 | grep FLIP_CONTROL; sleep 0.1; done" </dev/null 2>/dev/null)
  n=$(printf '%s\n' "$l" | /usr/bin/grep -c 'FLIP_PENDING 1'); u=$(printf '%s\n' "$l" | /usr/bin/grep 'FLIP_PENDING 1' | sort -u | wc -l | tr -d ' ')
  [ "${n:-0}" = 3 ] && [ "${u:-0}" = 1 ]
}
rc_every() { # every sample (fast or full): parse_x0 / parse_x2 / parse_crc must be fresh
  [ "$DRYRUN" = 1 ] && return 0
  RC_BAD=""
  # (all modes) restore / watchdog / DP plane watch
  [ "${X_WDOG:-x}" = 0 ] || rc_fail "watchdog restores ${X_WDOG:-?} (> 0 at rest)"
  [ "${X_RESFAIL:-x}" = 0 ] || rc_fail "restore failures ${X_RESFAIL:-?}"; [ "${X_RFAILED:-x}" = no ] || rc_fail "restoreFailed ${X_RFAILED:-?}"
  [ -z "$CRC_RG_NOW" ] || [ "$DPW_OK" = 1 ] || rc_fail "the DP plane watch in disp2 crc 2 is not 'all 15 registers unchanged' (HUBP0 outside its own slot set is the kext's row-14 rule)"
  if [ "$X_ACQ" = no ]; then rc_plane_pre
  elif [ "$X_ACQ" = YES ]; then
    [ -n "$X_A" ] && A_HI=$(( 0x$X_A >> 32 ))
    [ -n "$X_PROG" ] && [ "$(( 0x$X_PROG >> 32 ))" = "$A_HI" ] || rc_fail "programmed address HIGH dword != A's (programmed 0x${X_PROG:-?}, A 0x${X_A:-?})"
    [ -n "$X_EARLY" ] && [ "$(( 0x$X_EARLY >> 32 ))" = "$A_HI" ] || rc_fail "EARLIEST_INUSE2 HIGH dword != A's (0x${X_EARLY:-?})"
    if [ "$RC_SLOTS_KNOWN" = 1 ]; then
      in_set "$X_PROG" "$X_A" "$RC_S0" "$RC_S1" "$RC_S2" || rc_fail "programmed address 0x${X_PROG:-?} is outside {A, slots}"
      in_set "$X_EARLY" "$X_A" "$RC_S0" "$RC_S1" "$RC_S2" || rc_fail "EARLIEST_INUSE2 0x${X_EARLY:-?} is outside {A, slots}"
    fi
    [ "${X_UF:-x}" = 0 ] || rc_fail "HUBP2 underflow ${X_UF:-?}"; [ -n "$X_VMF" ] && [ $((0x$X_VMF)) = 0 ] || rc_fail "VM fault 0x${X_VMF:-?}"; [ "${X_ODM10:-x}" = 0 ] || rc_fail "ODM2 bit 10 ${X_ODM10:-?}"
    # GPU-held code 4 (EARLIEST not a registered slot) is only a fault once a present has been made (acquired with nothing presented yet legitimately sits on A: AMBIGUITY 6)
    [ "$X_GH4" = 0 ] || [ "${X_PRES:-0}" = 0 ] || rc_fail "GPU-HELD code 4 (EARLIEST_INUSE2 is not a registered slot) with $X_PRES presents made"
    if [ "${X_FP:-0}" != 0 ] && rc_fp_stuck; then rc_fail "FLIP_PENDING stuck across 3 reads 100 ms apart"; fi
  fi
  [ -z "$RC_BAD" ] || flip_abort "REVIEW CHECK failed:$RC_BAD (acq=$X_ACQ, +${S_SINCE:-?}s)"
}
review_checks_b() { # after every FULL K-snapshot once instance 2 may be acquired: slots, refusals, table generation, free VRAM
  [ "$DRYRUN" = 1 ] && return 0
  RC_BAD=""
  if [ "$X_ACQ" = YES ]; then
    local nm v w
    A_HI=$(( 0x$X_A >> 32 ))
    [ "$A_HI" = 128 ] || WARN="$WARN
  A's HIGH dword is 0x$(printf '%x' "$A_HI"), not 0x80 as the review assumed (the checks use A's own HIGH)"
    for nm in S0 S1 S2; do
      eval "v=\$X_$nm"
      if [ -z "$v" ]; then rc_fail "slot $nm MC not parsed"; continue; fi
      [ $(( 0x$v & 0xFFFF )) = 0 ] || rc_fail "slot $nm 0x$v is not 64 KiB aligned"
      [ "$(( 0x$v >> 32 ))" = "$A_HI" ] || rc_fail "slot $nm 0x$v HIGH dword != A's"
      ov "$v" "$X_A" && rc_fail "slot $nm 0x$v overlaps A 0x$X_A"
      ov "$v" "$X_B" && rc_fail "slot $nm 0x$v overlaps B 0x$X_B"
      [ -z "$DPCON" ] || { ov "$v" "$DPCON" && rc_fail "slot $nm 0x$v overlaps the DP console 0x$DPCON"; }
      for w in $DPSLOTS; do ov "$v" "$w" && rc_fail "slot $nm 0x$v overlaps the DP slot 0x$w"; done
    done
    ov "$X_S0" "$X_S1" && rc_fail "slots 0 and 1 overlap"; ov "$X_S0" "$X_S2" && rc_fail "slots 0 and 2 overlap"; ov "$X_S1" "$X_S2" && rc_fail "slots 1 and 2 overlap"
    # the monitor A pair: no monitor A plane exists on a 1b boot (instance 1 is not driven): nothing to overlap; TODO-VERB: no verb lists the monitor A pair's MCs
    RC_S0=$X_S0; RC_S1=$X_S1; RC_S2=$X_S2; RC_SLOTS_KNOWN=1
    [ "${X_WREF:-x}" = 0 ] || rc_fail "writer refusals ${X_WREF:-?}"; [ "${X_WFAIL:-x}" = 0 ] || rc_fail "write failures ${X_WFAIL:-?}"
    [ "${X_UNTAG:-x}" = 0 ] || rc_fail "untagged presents refused ${X_UNTAG:-?}"; [ "${X_GEOM:-x}" = 0 ] || rc_fail "geometry refusals ${X_GEOM:-?}"
    # the bundle's cached table generation vs the kernel's published one (M1): the bundle prints it every 10 s, so "within 2 s" is judged as "equal in two consecutive K-snapshots" (AMBIGUITY 7)
    if [ -n "$B_CACHED" ] && [ -n "$X_TGEN" ]; then
      if [ "$B_CACHED" = "$X_TGEN" ]; then RC_GENMISS=0
      else RC_GENMISS=$((RC_GENMISS+1)); say "  REVIEW: bundle cached table gen $B_CACHED != kernel published gen $X_TGEN (miss $RC_GENMISS)"; [ "$RC_GENMISS" -lt 2 ] || rc_fail "the bundle's cached table generation $B_CACHED != the kernel's published $X_TGEN across 2 K-snapshots (M1: a publish the bundle never sees)"; fi
    else WARN="$WARN
  table generation not comparable at +${S_SINCE:-?}s (bundle cached '${B_CACHED:-?}', kernel '${X_TGEN:-?}': no counters line in the last 4 min?)"; fi
    local fv; fv=$(/usr/bin/grep -h 'free visible VRAM' "$SF.bundle" 2>/dev/null | tail -1 | sed -n 's/.*free visible VRAM \([0-9]*\) MiB.*/\1/p'); say "  REVIEW: free visible VRAM (the bundle's last log line, kernel's figure): ${fv:-not in the last 4 min of bundle log (logged at acquire only)} MiB"
  fi
  [ -z "$RC_BAD" ] || flip_abort "REVIEW CHECK (K-snapshot) failed:$RC_BAD (+${S_SINCE:-?}s)"
}
review_checks_kill() { # AFTER the kill-switch leg (criterion 7): restore VERIFIED and counted, restore-failed 0, CRC_A exact, no LEAKED since P0
  [ "$DRYRUN" = 1 ] && { say "  DRYRUN: review_checks_kill: m6xstat 0 restores incremented (was \${RESTORES_BEFORE}) and restoreFailed 0; disp2 crc 2 == CRC_A; bundle log 'release rc .. restore of A verified 1'; driver-log LEAKED count == P0 baseline"; return 0; }
  sleep 2
  px rck "sudo -n $T accel m6xstat 0 2>&1; sudo -n $T accel disp2 crc 2 2>&1; echo LEAKED-NOW: \$(sudo -n $T log 2>&1 | grep -c LEAKED); sudo -n log show --info --last 3m --predicate 'eventMessage CONTAINS \"scanout (instance 2): release\"' --style compact 2>/dev/null | grep 'WindowServer\\['" || true
  printf '%s\n' "$PXOUT" >"$RUN/rck.txt"; parse_x0 "$RUN/rck.txt"; parse_crc "$RUN/rck.txt"
  local w="" ln; ln=$(sed -n 's/^LEAKED-NOW: //p' "$RUN/rck.txt" | head -1)
  [ -n "${RESTORES_BEFORE:-}" ] && [ -n "$X_RESTORES" ] && [ "$X_RESTORES" -gt "$RESTORES_BEFORE" ] || w="$w restore count did not increment (${RESTORES_BEFORE:-?} -> ${X_RESTORES:-?});"
  /usr/bin/grep -q 'restore of A verified 1' "$RUN/rck.txt" || w="$w no bundle line 'release rc 0, restore of A verified 1';"
  [ "${X_RESFAIL:-x}" = 0 ] && [ "${X_RFAILED:-x}" = no ] || w="$w restore failures ${X_RESFAIL:-?} / restoreFailed ${X_RFAILED:-?};"
  [ "$CRC_RG_NOW" = "$CRC_RG" ] && [ "$CRC_B_NOW" = "$CRC_B" ] || w="$w CRC ${CRC_RG_NOW:-?}/${CRC_B_NOW:-?} != CRC_A;"
  [ "${ln:-x}" = "$LEAK_BASE" ] || w="$w the kernel log has LEAKED lines: $ln now vs $LEAK_BASE at P0;"
  RCK=$([ -z "$w" ] && echo PASS || echo FAIL); RCKw="$w (restores ${RESTORES_BEFORE:-?} -> ${X_RESTORES:-?}, restoreFailed ${X_RFAILED:-?}, LEAKED $ln vs $LEAK_BASE at P0)"
  say "  after-kill review checks: $RCK $RCKw"
}
review_checks_a() { # HOOK A: before the WindowServer restart (nothing has flipped; a failure is a plain `stop 1`)
  say; say "=== REVIEW CHECKS A (before the restart): kext 0.0.661, both latches, instance 2 not acquired, HELD, CRC_A, plane baseline, OTG2 frame count moving"
  px rca-kext 'kmutil showloaded --list-only 2>/dev/null | grep -iE "bringup"; sysctl -n kern.bootargs' || true
  need "kext 0.0.661 is the loaded bring-up kext" has "navi48.bringup.*\\($WANT_KEXT\\)|bringup.*$WANT_KEXT"
  need "both latches in the live boot-args" bash -c 'printf "%s" "$1" | /usr/bin/grep -qE "(^| )navi48-m6=1( |$)" && printf "%s" "$1" | /usr/bin/grep -qE "(^| )navi48-m6flip=1( |$)"' _ "$PXOUT"
  px rca-x0 "sudo -n $T accel m6xstat 0 2>&1" || stop 1 "m6xstat 0 failed"
  px rca-x2a "sudo -n $T accel m6xstat 2 2>&1; sudo -n $T accel m6xstat 0 2>&1" || stop 1 "m6xstat 2 failed"
  if [ "$DRYRUN" != 1 ]; then printf '%s\n' "$PXOUT" >"$RUN/rca-a.txt"; parse_x2 "$RUN/rca-a.txt"; parse_x0 "$RUN/rca-a.txt"; FC1=$X_OTGFC; fi
  need "m6xstat 0: latch bits = 3 ('navi48-m6 ON, navi48-m6flip ON')" bash -c '[ "$1" = "navi48-m6 ON, navi48-m6flip ON" ]' _ "${X_LATCH:-}"
  need "m6xstat 0: acquired = no" bash -c '[ "$1" = no ]' _ "${X_ACQ:-}"
  nap 1
  px rca-x2b "sudo -n $T accel m6xstat 2 2>&1; sudo -n $T accel disp2 crc 2 2>&1" || stop 1 "m6xstat 2 (second read) failed"
  if [ "$DRYRUN" != 1 ]; then printf '%s\n' "$PXOUT" >"$RUN/rca-b.txt"; parse_x2 "$RUN/rca-b.txt"; FC2=$X_OTGFC; RC_BAD=""
    # before Acquire2 the scanx state does not know A yet (m6xstat prints A 0x0, run 20261007-2143): take the HELD plane's A from disp2 crc 2 ("EARLIEST_INUSE ... (A 0x..., B ...)")
    if [ -z "${X_A:-}" ] || [ "$X_A" = 0 ]; then X_A=$(sed -n 's/.*(A 0x\([0-9a-f]*\), B 0x.*/\1/p' "$RUN/rca-b.txt" | head -1); A_HELD=$X_A; say "  A taken from disp2 crc 2 (scanx not acquired yet): 0x${X_A:-?}"; [ -n "$X_A" ] || stop 1 "REVIEW CHECK A: cannot read buffer A from disp2 crc 2"; fi
    rc_plane_pre
    say "  baseline: underflow=$X_UF BLANK_EN=$X_BLANK VTG_SEL=$X_VTG VMF=0x$X_VMF ODM10=$X_ODM10 FLIP_CONTROL=0x$X_FCR pend=$X_FP programmed=0x$X_PROG EARLIEST=0x$X_EARLY A=0x$X_A pitch=0x$X_PITCH cfg=0x$X_SCFG ctl=0x$X_SCTL vmid=0x$X_VMID stage=$X_STAGE cur=$X_CUR OTG2 frames $FC1 -> $FC2"
    [ -z "$RC_BAD" ] || stop 1 "REVIEW CHECK A: plane baseline wrong:$RC_BAD"
    [ -n "$FC1" ] && [ -n "$FC2" ] && [ "$FC1" != "$FC2" ] || stop 1 "REVIEW CHECK A: the OTG2 frame counter is not moving ($FC1 -> $FC2)"
    [ "$X_STAGE" = 6 ] && [ "$X_CUR" = 0 ] || stop 1 "REVIEW CHECK A: instance 2's plane is not HELD on A (stage '$X_STAGE' cur '$X_CUR')"
  else say "  DRYRUN would require: plane baseline (underflow 0, BLANK_EN 0, VTG_SEL 2, VM fault 0, ODM2 bit 10 0, FLIP_CONTROL bits 0/1/8 0, programmed = EARLIEST = A, pitch 0x9ff, SURFACE_CONFIG 8, SURFACE_CONTROL 0, VMID 0, stage 6 cur 0) and OTG2 frame counter moving"; fi
  px rca-status2 "sudo -n $T accel disp2 status 2 2>&1" || stop 1 "disp2 status 2 failed"
  need "disp2 status 2: instance 2's plane is HELD" has "instance 2's plane is HELD"
  crc_read rca-crc
  need "monitor B CRC = CRC_A" bash -c '[ "$1" = "$3" ] && [ "$2" = "$4" ]' _ "${CRC_RG_NOW:-}" "${CRC_B_NOW:-}" "$CRC_RG" "$CRC_B"
  need "the DP plane watch in disp2 crc 2 is unchanged (HUBP0 rule)" bash -c '[ "$1" = 1 ]' _ "${DPW_OK:-0}"
  px rca-leak "sudo -n $T log 2>&1 | grep -c LEAKED" || true
  LEAK_BASE=$(printf '%s' "$PXOUT" | /usr/bin/grep -E '^[0-9]+$' | tail -1); say "  kernel-log LEAKED lines at P0 (baseline for the after-kill check): ${LEAK_BASE:-0}"; LEAK_BASE=${LEAK_BASE:-0}
  say "  TODO-VERB: free visible VRAM has no CLI verb before the bundle runs (scan_query.vram_vis_free is read by the bundle only); it is logged by the bundle at its acquire ('free visible VRAM N MiB') and the kit reports it after the flip."
}
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
if [ "$DRYRUN" = 1 ]; then say "--- DRYRUN: ssh $HOST 'cat > /tmp/ep.d' < ep.d (the Stage 0a probe, Q1 T1) ; ssh $HOST 'cat > /tmp/n48watch.sh' < n48watch.sh (the watcher from arm-gpu-desktop.sh; placed BEFORE the restart so the post-restart window is not spent on it)"
else
  ssh $SSHO "$HOST" 'cat > /tmp/ep.d' <"$RUN/ep.d" || say "  (S4) could not place ep.d: continuing without the probe"
  ssh $SSHO "$HOST" 'cat > /tmp/n48watch.sh' <"$RUN/n48watch.sh" || stop 1 "could not place the watcher"
fi
px epstart "sudo -n sh -c 'nohup dtrace -Z -q -s /tmp/ep.d -W WindowServer -o /tmp/ep.out >/tmp/ep.err 2>&1 </dev/null &'; sleep 6; if pgrep -x dtrace >/dev/null; then echo 'ep.d: dtrace is waiting for the WindowServer exec'; else echo 'ep.d: dtrace is NOT running (could not attach); the run continues without it'; cat /tmp/ep.err; fi" || say "  (S4) dtrace start failed: continuing without the probe"

RESTART_EPOCH=0; WS1=""; WSNOW=""
ws_restart() { # ws_restart <label> : the ONE restart (PREHOLD=1 sends a second); sets RESTART_EPOCH (PC clock) WS1; any failure stops (no second attempt of the SAME restart)
  local lab=$1
  px pcdate "date '+%Y-%m-%d %H:%M:%S'" || true
  RESTART_AT=$(printf '%s' "$PXOUT" | head -1)
  say "  PC clock at the restart ($lab): $RESTART_AT ; WindowServer pid before: ${WSB:-?}"
  px restart-$lab "who | grep -q . && { echo 'users logged in - refusing'; exit 9; }; r=\$(sudo -n $T accel pipereload 2>&1); echo \"\$r\" | grep -i 'restart window'; echo \"\$r\" | grep -q 'OPEN' || { echo 'restart window NOT open - not killing'; exit 8; }; sudo -n killall -9 WindowServer; echo killed-epoch=\$(date +%s)" || stop 1 "the restart command failed or refused (exit 9 = a user is logged in); NO second attempt"
  need "restart window was OPEN" has 'restart window +: OPEN'
  RESTART_EPOCH=$(printf '%s' "$PXOUT" | sed -n 's/^killed-epoch=\([0-9]*\).*/\1/p' | head -1)
  echo "$(date +%s) killed ${WSB:-?} ($lab)" >>"$RUN/ws-pids.txt"
  # no fixed nap: poll the pid every second (the 20 s criterion starts at the kill)
  WS1=""; local k=0
  while [ "$DRYRUN" != 1 ] && [ $k -lt 60 ]; do
    ssh $SSHO "$HOST" 'pgrep -x WindowServer' >"$RUN/wspid.tmp" 2>/dev/null; WS1=$(head -1 "$RUN/wspid.tmp")
    [ -n "$WS1" ] && [ "$WS1" != "${WSB:-}" ] && break; k=$((k+1)); sleep 1
  done
  say "  WindowServer pid after the restart ($lab): '$WS1' (was ${WSB:-?}) after ~${k}+ polls; kill epoch (PC) ${RESTART_EPOCH:-?}"
  need "WindowServer came back with a NEW pid" bash -c '[ -n "$1" ] && [ "$1" != "$2" ]' _ "$WS1" "${WSB:-}"
  echo "$(date +%s) restarted $WS1 ($lab)" >>"$RUN/ws-pids.txt"
  WSB=$WS1
  px watcher "pgrep -f n48watch.sh >/dev/null && echo 'watcher already running' || { sudo -n nohup bash /tmp/n48watch.sh >/dev/null 2>&1 </dev/null & sleep 1; }; sudo -n launchctl asuser 501 launchctl setenv CI_USE_MTL_DAG_FOR_CIKL_SRC 0 || echo 'note: launchctl asuser 501 setenv failed (no user domain yet?): the CIKL backstop is NOT in place'" || true
}

# ---------------------------------------------------------------------------------------------- fast / full sampling helpers
printf 'phase\tt_pc\tsec_since_restart\tws\tacq\tcrc_rg\tcrc_b\tuf\tvmf\todm10\tflip_pend\tearliest2\tprog2\ta\tgpuheld\twdog\treuse\thung\tcrc_is_A\n' >"$RUN/samples.tsv"
printf 'min\ttime\tws\tdp_stamps_s\tcrc_rg\tcrc_b\tacq\ttxn0\ttxn2\tids0\tids2\tambig\tnofb\tagdc_nfb\tnondp921\totg0\tdig1_be\tdell_stamps\tusers\tdp_presents_s\tdp_unknown\tlast_ep\tinst_mismatch\treuse_refused\twdog\tpresents2\tgpu_fail\tdrops\n' >"$RUN/minutes.tsv"
PHASE=pre; SF=""; SNAP_N=0
FASTN=0
fast_sample() { # fast_sample <phase> : one ssh; parses into the X_*/CRC_* globals; appends a samples.tsv row; sets S_T (PC epoch), S_SINCE
  PHASE=$1; N=$((N+1)); FASTN=$((FASTN+1)); SF=$(printf '%s/%02d-fast-%s%03d.txt' "$RUN" "$N" "$PHASE" "$FASTN")
  if [ "$DRYRUN" = 1 ]; then say "--- [$N] DRYRUN: ssh $HOST '<fast: date; pgrep WS; console+who; m6xstat 0; m6xstat 2; disp2 crc 2; sessstat 4>' -> $SF"; : >"$SF"; S_T=0; S_SINCE=0; return 0; fi
  if ! fast_remote | ssh $SSHO "$HOST" 'bash -s' >"$SF" 2>&1; then
    if reach; then say "  (the fast ssh returned nonzero but the PC answers; using what arrived)"; else stop 3 "PC unreachable during the sampling ($PHASE), and one more check failed"; fi
  fi
  S_T=$(sect T "$SF" | head -1); S_SINCE=$(( ${S_T:-0} - ${RESTART_EPOCH:-0} ))
  WSNOW=$(sect WS "$SF" | head -1); USERS_NOW=$(sect USERS "$SF" | tr '\n' ' ')
  sect X0 "$SF" >"$SF.x0"; sect X2 "$SF" >"$SF.x2"; sect CRC "$SF" >"$SF.crc"; sect HUNG "$SF" >"$SF.hung"
  parse_x0 "$SF.x0"; parse_x2 "$SF.x2"; parse_crc "$SF.crc"; parse_hung "$SF.hung"
  CRC_IS_A=0; [ "$CRC_RG_NOW" = "$CRC_RG" ] && [ "$CRC_B_NOW" = "$CRC_B" ] && CRC_IS_A=1
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$PHASE" "$S_T" "$S_SINCE" "$WSNOW" "$X_ACQ" "$CRC_RG_NOW" "$CRC_B_NOW" "$X_UF" "$X_VMF" "$X_ODM10" "$X_FP" "$X_EARLY" "$X_PROG" "$X_A" "$X_GPUHELD" "$X_WDOG" "$X_REUSE" "$HUNG" "$CRC_IS_A" >>"$RUN/samples.tsv"
  say "  [$PHASE +${S_SINCE}s] WS $WSNOW acq=$X_ACQ CRC ${CRC_RG_NOW:-?}/${CRC_B_NOW:-?}$([ "$CRC_IS_A" = 1 ] && echo '(=A)') uf=$X_UF vmf=0x$X_VMF odm10=$X_ODM10 pend=$X_FP early2=0x$X_EARLY wdog=$X_WDOG reuse=$X_REUSE hung=$HUNG"
}
health_abort() { # called after every sample once instance 2 has had the chance to flip: ABORT on any underflow / ODM2 bit 10 / VM fault / HUNG / WS pid change
  [ "$DRYRUN" = 1 ] && return 0
  local bad=""
  [ -n "$WSNOW" ] && [ "$WSNOW" = "${WS1:-}" ] || bad="$bad WindowServer pid '$WSNOW' != '${WS1:-}' (a crash or relaunch);"
  [ -n "$X_UF" ] && [ "$X_UF" = 0 ] || bad="$bad HUBP2 underflow '${X_UF:-?}';"
  [ -n "$X_ODM10" ] && [ "$X_ODM10" = 0 ] || bad="$bad ODM2 bit 10 '${X_ODM10:-?}';"
  [ -n "$X_VMF" ] && [ "$((0x$X_VMF))" = 0 ] || bad="$bad DCN_VM_FAULT_STATUS 0x${X_VMF:-?};"
  [ -n "$HUNG" ] && [ "$HUNG" = 0 ] || bad="$bad HUNG '${HUNG:-unparsable}';"
  [ -z "$bad" ] || { C4=FAIL; C4w="($bad at +${S_SINCE}s)"; flip_abort "health abort:$bad"; }
  rc_every
  # a console user appearing is NOT a flip abort: park as 1a does
  # run 20261007-2145 stopped falsely on '_windowserver ': WindowServer's own account owns the console for a moment after the restart; that is not a login. A login = any other owner, or any `who` line.
  case "$USERS_NOW" in "root "|"_windowserver ") ;; *) stop 1 "a console user appeared ('$USERS_NOW'): user logged in; the PC is left as is" ;; esac
}
kill_and_verify() { # touch the kill file and verify EARLIEST2 = A and CRC_A within 6 s (PC clock); sets KILL_OK (1/0) KILL_SECS
  KILL_OK=0; KILL_SECS=""
  px killfile "sudo -n touch $KILLFILE; sudo -n chmod 644 $KILLFILE; ls -l $KILLFILE; echo touched-epoch=\$(date +%s)" || true
  local te; te=$(printf '%s' "$PXOUT" | sed -n 's/^touched-epoch=\([0-9]*\).*/\1/p' | head -1)
  N=$((N+1)); local KF; KF=$(printf '%s/%02d-killpoll.txt' "$RUN" "$N")
  if [ "$DRYRUN" = 1 ]; then say "--- [$N] DRYRUN: ssh $HOST '<14 x: date; m6xstat 2 EARLIEST; m6xstat 0 A; disp2 crc 2; sleep .3>' -> $KF ; judged: first block with EARLIEST2 == A and CRC == CRC_A, epoch <= touch + 6"; return 0; fi
  ssh $SSHO "$HOST" "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14; do echo \"@@S\$i \$(date +%s)\"; sudo -n $T accel m6xstat 2 2>&1 | sed 's/^/K /'; sudo -n $T accel m6xstat 0 2>&1 | sed 's/^/K /'; sudo -n $T accel disp2 crc 2 2>&1 | grep 'OTG2 CRC' | sed 's/^/K /'; sleep 0.3; done" >"$KF" 2>&1
  local i ts blk
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14; do
    ts=$(sed -n "s/^@@S$i \([0-9]*\).*/\1/p" "$KF" | head -1); [ -n "$ts" ] || continue
    awk -v n="@@S$i " 'index($0,n)==1{f=1;next} /^@@S/{f=0} f' "$KF" | sed 's/^K //' >"$KF.blk"
    parse_x2 "$KF.blk"; parse_x0 "$KF.blk"; parse_crc "$KF.blk"
    if [ -n "$X_EARLY" ] && [ -n "$X_A" ] && [ "$X_EARLY" = "$X_A" ] && [ "$X_PROG" = "$X_A" ] && [ "$CRC_RG_NOW" = "$CRC_RG" ] && [ "$CRC_B_NOW" = "$CRC_B" ]; then
      KILL_SECS=$(( ts - ${te:-$ts} )); [ "$KILL_SECS" -le 6 ] && KILL_OK=1; break
    fi
  done
  say "  kill-file verification: EARLIEST2 = A and CRC_A first seen $([ -n "$KILL_SECS" ] && echo "at touch+${KILL_SECS}s" || echo 'NEVER within the 14 polls') -> $([ "$KILL_OK" = 1 ] && echo OK || echo 'NOT within 6 s')"
}
flip_abort() { # ABORT path: quick capture FIRST, then the kill file, verification, the full capture, park
  [ "$ABORTING" = 1 ] && return; ABORTING=1
  say; say "### ABORT at $(stamp): $*"
  FAILS="$FAILS
  ABORT: $*"
  quick_capture
  kill_and_verify
  if [ "$KILL_OK" = 1 ]; then say "### the kill file took: EARLIEST2 = programmed = A and CRC_A within ${KILL_SECS}s"
  else FAILS="$FAILS
  the kill file did NOT restore A + CRC_A within 6 s: REBOOT WITHOUT FREEING ANYTHING (no teardown verb, no BO free, no second restart)"; say "### the kill file did NOT restore A + CRC_A within 6 s: capturing m6xstat 0-2 and STOPPING. REBOOT WITHOUT FREEING ANYTHING; send no teardown verb."; fi
  capture
  say "### PC PARKED (no teardown verbs sent; $KILLFILE present so the bundle keeps instance 2 OFF)."
  summary_and_exit 1
}

# ---------------------------------------------------------------------------------------------- P5 (continued) restart + P5b pre-flip
RESTART_AT=""
if [ "$PREHOLD" = 1 ]; then
  say; say "=== PREHOLD=1: the kill file is touched BEFORE the first restart so instance 2 stays OFF (bundle 13 reads it ONCE at init) and 1a's K1-K6 are re-confirmed for 60 s; a SECOND WindowServer restart follows (AMBIGUITY 1)"
  px prehold-touch "sudo -n touch $KILLFILE; sudo -n chmod 644 $KILLFILE; ls -l $KILLFILE" || stop 1 "could not touch the kill file"
fi
ws_restart first

# ---- the K-snapshot parser/evaluator used pre-flip and per minute --------------------------------
LAST_DPRATE=""; DP_FIRST=""; DP_BAD=0; TXN0_PREV=""; PRES_BAD=0; LAST_PRES=""; PRES_MINW=""
KPRE_OK=""; KPRE_W=""; KPRE_N=0
ksnap() { # ksnap <phase> <min> : one full snapshot (~8 s); sets the M_* / X_* / DP_* globals and appends a minutes.tsv row; returns after the hard checks
  local ph=$1 m=$2; PHASE=$ph; N=$((N+1)); SNAP_N=$((SNAP_N+1)); SF=$(printf '%s/%02d-snap-%s%02d.txt' "$RUN" "$N" "$ph" "$m")
  say "--- [$N] $(stamp) K-snapshot $ph/$m (one ssh) -> $SF"
  if [ "$DRYRUN" = 1 ]; then say "    DRYRUN: ssh $HOST '<snapshot: date; pgrep WS; console+who; m6stat 1 x2 3 s apart; m6xstat 0|1|2; disp2 crc 2; disp2 status 2; m6stat 0|1|2|3; sessstat 4; bundle log lines; new reports>'"; return 0; fi
  if ! snap_remote | ssh $SSHO "$HOST" 'bash -s' >"$SF" 2>&1; then
    if reach; then say "  (the snapshot ssh returned nonzero but the PC answers; using what arrived)"; else stop 3 "PC unreachable during the K-snapshot ($ph/$m), and one more check failed"; fi
  fi
  S_T=$(sect T2 "$SF" | head -1); [ -n "$S_T" ] || S_T=$(sect T "$SF" | head -1); S_SINCE=$(( ${S_T:-0} - ${RESTART_EPOCH:-0} ))   # T2 = the clock right after the CRC read (the flip evidence), not the start of the snapshot
  WSNOW=$(sect WS "$SF" | head -1); USERS_NOW=$(sect USERS "$SF" | tr '\n' ' ')
  local a b
  a=$(sect STAMPA "$SF" | sed -n 's/^ *instance 0 .*: \([0-9][0-9]*\) vblank stamps written.*/\1/p' | head -1); b=$(sect STAMPB "$SF" | sed -n 's/^ *instance 0 .*: \([0-9][0-9]*\) vblank stamps written.*/\1/p' | head -1)
  DPRATE=""; [ -n "$a" ] && [ -n "$b" ] && DPRATE=$(( (b - a) / 3 ))
  sect X0 "$SF" >"$SF.x0"; sect X1 "$SF" >"$SF.x1"; sect X2 "$SF" >"$SF.x2"; sect CRC "$SF" >"$SF.crc"; sect HUNG "$SF" >"$SF.hung"; sect STATUS "$SF" >"$SF.st"
  sect M6_0 "$SF" >"$SF.m0"; sect M6_1 "$SF" >"$SF.m1"; sect M6_2 "$SF" >"$SF.m2"; sect M6_3 "$SF" >"$SF.m3"; sect BUNDLE "$SF" >"$SF.bundle"; sect REPORTS "$SF" >"$SF.reports"
  parse_x0 "$SF.x0"; parse_x1 "$SF.x1"; parse_x2 "$SF.x2"; parse_crc "$SF.crc"; parse_hung "$SF.hung"; parse_dpside "$SF.st"; parse_m6 "$SF.m0" "$SF.m1" "$SF.m2"
  /usr/bin/grep 'm6: surfaces for instance' "$SF.bundle" >"$SF.bundle.m6"; /usr/bin/grep 'scanout (instance 2): counters' "$SF.bundle" >"$SF.bundle.x"
  local pr; pr=$(bundle_rate "$SF.bundle.m6"); PRES=""; PUNK=""; [ -n "$pr" ] && [ "$pr" != none ] && { PRES=${pr%% *}; PUNK=$(echo "$pr" | awk '{print $2}'); }
  parse_bundle2 "$SF.bundle.x"
  CRC_IS_A=0; [ "$CRC_RG_NOW" = "$CRC_RG" ] && [ "$CRC_B_NOW" = "$CRC_B" ] && CRC_IS_A=1
  NEWREPORTS=$(cat "$SF.reports" | tr '\n' ' ')
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$ph$m" "$(stamp)" "$WSNOW" "${DPRATE:-}" "$CRC_RG_NOW" "$CRC_B_NOW" "$X_ACQ" "$M_TXN0" "$M_TXN2" "$M_IDS0" "$M_IDS2" "$M_AMB" "$M_NOFB" "$M_AGN" "$M_NONDP" "$DP_OTG0" "$DP_BE" "$M_STAMP2" "$([ "$USERS_NOW" = "root " ] && echo 0 || echo 1)" "${PRES:-n/a}" "${PUNK:-n/a}" "${M_LASTEP:-?}" "${B_MISMATCH:-}" "${X_REUSE:-}" "${X_WDOG:-}" "${B_PRES2:-}" "${B_GPUFAIL:-}" "${B_DROPS:-}" >>"$RUN/minutes.tsv"
  say "  $ph/$m +${S_SINCE}s: WS $WSNOW | acq=$X_ACQ CRC ${CRC_RG_NOW:-?}/${CRC_B_NOW:-?}$([ "$CRC_IS_A" = 1 ] && echo '(=A)') | DP stamps/s ${DPRATE:-?} presents/s ${PRES:-n/a} OTG0 ${DP_OTG0:-?} | txn DP ${M_TXN0:-?} monitor B ${M_TXN2:-?} IDs ${M_IDS0:-?}/${M_IDS2:-?} amb ${M_AMB:-?} nofb ${M_NOFB:-?} | AGDC nfb ${M_AGN:-?} 921-nonDP ${M_NONDP:-?} | inst_mismatch ${B_MISMATCH:-?} reuse_refused ${X_REUSE:-?} wdog ${X_WDOG:-?} presents2 ${B_PRES2:-?}"
}
ksnap_checks() { # the DP-side hard checks of a K-snapshot (OTG0 COUNTING, report appearance) - after health_abort
  [ "$DRYRUN" = 1 ] && return 0
  [ "$DP_OTG0" = COUNTING ] || { C6=FAIL; C6w="(OTG0 '$DP_OTG0')"; flip_abort "the DP's OTG0 is not COUNTING ('$DP_OTG0') at +${S_SINCE}s"; }
  [ "$DP_BE" = "$LAST_BE" ] || WARN="$WARN
  DIG1_DIG_BE_CNTL $LAST_BE -> $DP_BE at +${S_SINCE}s (DP side register changed; arming may legitimately change it, queue 285; judge by eye)"
  LAST_BE=$DP_BE
  [ -z "${NEWREPORTS// /}" ] || WARN="$WARN
  new DiagnosticReports since the P0 marker at +${S_SINCE}s: $NEWREPORTS"
}
LAST_BE="$DP_BE0"
pre_k_eval() { # R0 on a K-snapshot taken while instance 2 was NOT acquired: sets KPRE_OK (1/0) KPRE_W
  local w=""
  [ "${M_AGN:-0}" = 2 ] || w="$w AGDC nfb ${M_AGN:-?} (want 2);"
  [ "${M_NONDP:-0}" -ge 1 ] 2>/dev/null || w="$w 0x921 answered for a non-DP endpoint ${M_NONDP:-?} (want >= 1);"
  [ "${M_TXN2:-0}" -gt 0 ] 2>/dev/null || w="$w monitor B transactions ${M_TXN2:-?} (want > 0);"
  [ "${M_TXN0:-0}" -gt 0 ] 2>/dev/null || w="$w DP transactions ${M_TXN0:-?} (want > 0);"
  [ "${M_AMB:-1}" = 0 ] || w="$w ambiguous ${M_AMB:-?};"
  [ "${M_NOFB:-1}" = 0 ] || w="$w no-framebuffer submits ${M_NOFB:-?};"
  [ "$M_AMB2" = 0 ] 2>/dev/null || w="$w page 2 AMBIGUOUS;"
  [ "${DPRATE:-0}" -ge "$DP_MIN" ] 2>/dev/null || w="$w DP stamps/s ${DPRATE:-?} < $DP_MIN;"
  [ "$DP_OTG0" = COUNTING ] || w="$w OTG0 '$DP_OTG0';"
  [ "$CRC_IS_A" = 1 ] || w="$w monitor B CRC ${CRC_RG_NOW:-?}/${CRC_B_NOW:-?} != CRC_A;"
  [ "$UF_OK" = 1 ] || w="$w monitor B underflow lines not all 0;"
  KPRE_N=$((KPRE_N+1)); KPRE_W="$w (K-snapshot at +${S_SINCE}s: AGDC nfb ${M_AGN:-?}, 921-nonDP ${M_NONDP:-?}, txn DP ${M_TXN0:-?} monitor B ${M_TXN2:-?}, IDs ${M_IDS0:-?}/${M_IDS2:-?}, DP stamps/s ${DPRATE:-?}, CRC_A=$CRC_IS_A)"
  [ -z "$w" ] && KPRE_OK=1 || KPRE_OK=0
}

say; say "=== P5b PRE-FLIP re-confirm of 1a's K1-K6 (+ AGDC 0x921 for the monitor B, monitor B transactions): a fast sample at once, then a K-snapshot, then fast samples every ~${FAST_SLEEP} s until instance 2 acquires / the CRC leaves CRC_A, a K-snapshot every ~10 s meanwhile"
FLIPPED=0; FLIP_SINCE=""
if [ "$PREHOLD" = 1 ]; then
  # 60 s with the kill file present: instance 2 is OFF in this bundle process, so this is 1a's full recipe; then the file goes, a second restart follows
  pm=0; while [ $pm -lt 6 ]; do pm=$((pm+1)); ksnap prehold $pm; health_abort; ksnap_checks; [ "$DRYRUN" = 1 ] || pre_k_eval
    [ "$DRYRUN" = 1 ] || { [ "$X_ACQ" = no ] || flip_abort "instance 2 acquired although the kill file was present (PREHOLD)"; }
    nap 8; done
  R0=$([ "${KPRE_OK:-0}" = 1 ] && echo PASS || echo FAIL); R0w="PREHOLD 60 s: ${KPRE_W:-}"
  [ "$DRYRUN" = 1 ] || [ "$R0" = PASS ] || { FAILS="$FAILS
  R0 failed in the PREHOLD window: ${KPRE_W:-}"; stop 1 "1a's K1-K6 did not re-confirm in the 60 s PREHOLD window:${KPRE_W:-}"; }
  px prehold-rm "sudo -n rm -f $KILLFILE; ls -l $KILLFILE 2>&1" || stop 1 "could not remove the kill file"
  ws_restart second
fi
RESTART_FOR_FLIP=${RESTART_EPOCH:-0}
KPRE_TAKEN=0; LAST_K=0; FASTLOOP=0; PRE_STARTED=$(date +%s)   # (after a PREHOLD second restart the clock restarts too)
while :; do
  FASTLOOP=$((FASTLOOP+1))
  if [ "$FASTLOOP" -ge 2 ] && { [ $KPRE_TAKEN = 0 ] || [ $(( $(date +%s) - LAST_K )) -ge 10 ]; }; then
    ksnap pre $FASTLOOP; LAST_K=$(date +%s); KPRE_TAKEN=1
    [ "$DRYRUN" = 1 ] || { health_abort; review_checks_b; }
    if [ "$DRYRUN" != 1 ] && [ "$X_ACQ" = no ] && [ "$CRC_IS_A" = 1 ]; then pre_k_eval; PRE_LAST_OK=$KPRE_OK; PRE_LAST_W=$KPRE_W; PRE_LAST_AT=$S_SINCE; fi
    if [ "$DRYRUN" != 1 ] && { [ "$X_ACQ" = YES ] || [ "$CRC_IS_A" = 0 ]; }; then FLIPPED=1; FLIP_SINCE=$S_SINCE; break; fi
  else
    fast_sample pre
    [ "$DRYRUN" = 1 ] || health_abort
    if [ "$DRYRUN" != 1 ] && { [ "$X_ACQ" = YES ] || [ "$CRC_IS_A" = 0 ]; }; then FLIPPED=1; FLIP_SINCE=$S_SINCE; break; fi
  fi
  [ "$DRYRUN" = 1 ] && break
  if [ $(( $(date +%s) - PRE_STARTED )) -ge "$PRE_MAX_S" ]; then break; fi
  sleep "$FAST_SLEEP"
done
if [ "$DRYRUN" = 1 ]; then say "  DRYRUN: the flip loop is not executed (it ends at the first sample showing acquired=YES or CRC != CRC_A, or after ${PRE_MAX_S} s)"
else
  say "  flip detected: $([ $FLIPPED = 1 ] && echo "YES at +${FLIP_SINCE}s after the kill (PC clock)" || echo "NO within ${PRE_MAX_S} s")"
  # R0: evaluated on the LAST K-snapshot with instance 2 not acquired and the CRC == CRC_A (the pre-flip state); under PREHOLD it was decided above
  if [ "$PREHOLD" != 1 ]; then
    if [ -n "${PRE_LAST_OK:-}" ]; then R0=$([ "$PRE_LAST_OK" = 1 ] && echo PASS || echo FAIL); R0w="$PRE_LAST_W (the last K-snapshot before the first flip, at +${PRE_LAST_AT}s)"
    else R0="NOT-EVALUATED"; R0w="(the first K-snapshot already showed acquired=YES or CRC != CRC_A: the flip came before any pre-flip snapshot; the same conditions are re-read at the flip snapshot below)"; WARN="$WARN
  R0: no K-snapshot was taken before the first flip (flip +${FLIP_SINCE:-?}s); R0 falls back to the flip snapshot"; fi
  fi
  [ "$FLIPPED" = 1 ] || { C2=FAIL; C2w="(CRC never left CRC_A and acquired stayed no for ${PRE_MAX_S} s after the restart: no monitor B frame reached instance 2 - Q1 (b)/(c)? or the bundle's instance 2 is OFF: see the bundle log)"; stop 1 "no flip within ${PRE_MAX_S} s of the restart: C2 FAIL"; }
fi

# ---------------------------------------------------------------------------------------------- P6 FLIP: criteria 2 and 1
say; say "=== P6 the flip: criterion 2 (CRC leaves CRC_A within ${FLIP_S} s of the restart; EARLIEST2 a slot; FLIP_PENDING 0) and criterion 1 (Acquire2 OK, 3 slots, MCs outside A/B/DP slots)"
if [ "$DRYRUN" = 1 ]; then
  say "  DRYRUN: C2 judged on the first fast sample with CRC != CRC_A: seconds since the kill (PC clock) <= $FLIP_S; EARLIEST2 (m6xstat 2) in {m6xstat 1 slot MCs}; FLIP_PENDING 0 within 3 retries 2 s apart"
  say "  DRYRUN: C1 judged on a K-snapshot after the flip: acquired YES, 3 distinct non-zero slot MCs, none == A or B (m6xstat 0) or a DP slot MC (bundle log), Acquire2 refused 0 (m6xstat 0)"
else
  # settle: take fresh data (up to 3 retries 2 s apart) until FLIP_PENDING is 0
  r=0; while [ $r -lt 4 ]; do ksnap flip $((r+1)); health_abort; review_checks_b; [ "${X_FP:-1}" = 0 ] && break; r=$((r+1)); sleep 2; done
  FLIP_CRC_AT=$FLIP_SINCE
  slots=" $X_S0 $X_S1 $X_S2 "
  in_slots=0; case "$slots" in *" $X_EARLY "*) [ -n "$X_EARLY" ] && in_slots=1 ;; esac
  w=""
  [ "$FLIP_CRC_AT" -le "$FLIP_S" ] || w="$w the first sample off CRC_A was at +${FLIP_CRC_AT}s > ${FLIP_S} s;"
  [ "$CRC_IS_A" = 0 ] || w="$w the CRC is back at CRC_A in the settle snapshot;"
  [ "$in_slots" = 1 ] || w="$w EARLIEST2 0x${X_EARLY:-?} is not one of the slots (0x$X_S0 0x$X_S1 0x$X_S2);"
  [ "${X_FP:-1}" = 0 ] || { rc_fp_stuck && w="$w FLIP_PENDING stuck (3 reads, same programmed/EARLIEST);"; }   # only a STUCK flip fails C2: a monitor B flipping every frame reads pending while latching
  C2=$([ -z "$w" ] && echo PASS || echo FAIL); C2w="$w (first off CRC_A at +${FLIP_CRC_AT}s PC clock; EARLIEST2 0x${X_EARLY:-?}; slots 0x$X_S0 0x$X_S1 0x$X_S2; CRC ${CRC_RG_NOW:-?}/${CRC_B_NOW:-?})"
  # criterion 1
  w=""
  [ "$X_ACQ" = YES ] || w="$w acquired '$X_ACQ';"
  [ -n "$X_S0" ] && [ -n "$X_S1" ] && [ -n "$X_S2" ] || w="$w fewer than 3 slot MCs parsed;"
  [ "$X_S0" != "$X_S1" ] && [ "$X_S0" != "$X_S2" ] && [ "$X_S1" != "$X_S2" ] || w="$w slot MCs not distinct;"
  [ "$((0x${X_S0:-0}))" != 0 ] && [ "$((0x${X_S1:-0}))" != 0 ] && [ "$((0x${X_S2:-0}))" != 0 ] || w="$w a slot MC is 0;"
  for s in "$X_S0" "$X_S1" "$X_S2"; do [ "$s" != "$X_A" ] && [ "$s" != "$X_B" ] || w="$w slot 0x$s equals A (0x$X_A) or B (0x$X_B);"; done
  # the DP's slot MCs from the bundle: "scanout: slot N: memory type ..., MC 0x...." (NOT the instance-2 lines); fetched once here
  px dpslots "sudo -n log show --info --start '$RESTART_AT' --predicate 'eventMessage CONTAINS \"scanout: query\" OR eventMessage CONTAINS \"scanout: slot\" OR eventMessage CONTAINS \"scanout (instance 2): slot\" OR eventMessage CONTAINS \"scanout (instance 2) ACQUIRED\" OR eventMessage CONTAINS \"scanout ACQUIRED\"' --style compact 2>/dev/null | grep 'WindowServer\\[' | tail -20" || true
  DPMC=$(printf '%s\n' "$PXOUT" | /usr/bin/grep 'scanout: slot' | /usr/bin/grep -v 'instance 2' | sed -n 's/.*MC 0x\([0-9a-f]*\) (64 KiB.*/\1/p' | tr '\n' ' ')
  say "  DP slot MCs from the bundle log: ${DPMC:-NONE FOUND}; DP console MC: $(printf '%s\n' "$PXOUT" | /usr/bin/grep 'scanout: query' | sed -n 's/.*console MC 0x\([0-9a-f]*\).*/\1/p' | head -1)"
  [ -n "${DPMC// /}" ] || w="$w the DP's slot MCs could not be read from the bundle log (cannot prove the monitor B slots are outside them);"
  for s in "$X_S0" "$X_S1" "$X_S2"; do for d in $DPMC; do [ "$s" != "$d" ] || w="$w monitor B slot 0x$s equals DP slot 0x$d;"; done; done
  [ "${X_ACQREF:-1}" = 0 ] || w="$w Acquire2 refused ${X_ACQREF:-?} time(s) (last reason ${X_LASTREASON:-?});"
  DPCONL=$(printf '%s\n' "$PXOUT" | /usr/bin/grep 'scanout: query' | sed -n 's/.*console MC 0x\([0-9a-f]*\).*/\1/p' | head -1); DPCON=$DPCONL; DPSLOTS=$DPMC
  RC_S0=$X_S0; RC_S1=$X_S1; RC_S2=$X_S2; RC_SLOTS_KNOWN=1
  C1=$([ -z "$w" ] && echo PASS || echo FAIL); C1w="$w (monitor B slots 0x$X_S0 0x$X_S1 0x$X_S2; A 0x$X_A B 0x$X_B; DP slots ${DPMC:-?})"
  say "  C2 $C2 $C2w"; say "  C1 $C1 $C1w"
  # R0 fallback when no pre-flip snapshot existed: the same conditions at the flip snapshot (the monitor B has transactions because it flipped; AGDC / 0x921 must be there)
  if [ "$R0" = NOT-EVALUATED ]; then
    w=""; [ "${M_AGN:-0}" = 2 ] || w="$w AGDC nfb ${M_AGN:-?};"; [ "${M_NONDP:-0}" -ge 1 ] 2>/dev/null || w="$w 0x921 non-DP ${M_NONDP:-?};"; [ "${M_TXN2:-0}" -gt 0 ] 2>/dev/null || w="$w monitor B txn ${M_TXN2:-?};"; [ "${M_AMB:-1}" = 0 ] || w="$w ambiguous;"; [ "${M_NOFB:-1}" = 0 ] || w="$w nofb;"
    R0=$([ -z "$w" ] && echo PASS-AT-FLIP || echo FAIL); R0w="$w (evaluated at the FLIP snapshot, not before it)"
  fi
  [ "$R0" != FAIL ] || { FAILS="$FAILS
  R0 failed: $R0w"; }
fi

# ======================================================================== REVIEW CHECKS (hook B: AFTER the first flip) ========================================================================
# review_checks_b / rc_every are DEFINED in hook A's section above (they must exist before the pre-flip loop uses them); they are CALLED: rc_every from health_abort (every fast and full sample), review_checks_b after every full
# K-snapshot (pre-flip, settle and watch), review_checks_kill after criterion 7. The reviewer can append more checks to review_checks_b.
# ================================================================================================================================================================================================

# ---------------------------------------------------------------------------------------------- P7 watch after the flip
say; say "=== P7 watch: ${WATCH_MIN} min after the flip (>= 5 min at rest needed for C3); fast sample every ~$((FAST_SLEEP+4)) s, a full K-snapshot about every minute"
REST_START=""; PREV_CRC=""; PREV_T=0; EQ_PAIRS=0; NEQ_PAIRS=0; A_RETURNS=0; LAST_X_WDOG=0; WATCH_START=$(date +%s); WATCH_S=$((WATCH_MIN*60)); MINN=0; LAST_FULL=0
if [ "$DRYRUN" = 1 ]; then say "  DRYRUN: the loop is not executed; per fast sample: health_abort (underflow / ODM2 bit 10 / VM fault / HUNG / WS pid change -> flip_abort), CRC pair bookkeeping (equal pairs, returns to CRC_A), watchdog restores; per K-snapshot: DP stamps/s >= $DP_MIN (and >= 85% of minute 1), DP presents/s >= $PRES_MIN from minute 2, OTG0 COUNTING, review_checks_b"; fi
while [ "$DRYRUN" != 1 ] && [ $(( $(date +%s) - WATCH_START )) -lt "$WATCH_S" ]; do
  if [ $(( $(date +%s) - LAST_FULL )) -ge 60 ]; then
    MINN=$((MINN+1)); ksnap watch $MINN; LAST_FULL=$(date +%s); health_abort; ksnap_checks
    # C6: DP floors
    if [ -n "$DPRATE" ]; then
      [ -n "$DP_FIRST" ] || DP_FIRST=$DPRATE
      [ "$DPRATE" -ge "$DP_MIN" ] || { DP_BAD=1; C6=FAIL; C6w="$C6w minute $MINN: $DPRATE stamps/s < $DP_MIN;"; flip_abort "the DP's stamps/s $DPRATE fell below the floor $DP_MIN at watch minute $MINN (review: abort on the DP below its floors)"; }
      [ "$DPRATE" -ge $(( DP_FIRST * 85 / 100 )) ] || { DP_BAD=1; C6w="$C6w minute $MINN: $DPRATE stamps/s < 85% of minute 1 ($DP_FIRST);"; }
    else DP_BAD=1; C6w="$C6w minute $MINN: no stamp reading;"; fi
    if [ "$MINN" -ge 2 ]; then
      if [ -z "$PRES" ]; then C6=FAIL; C6w="$C6w minute $MINN: no DP presents reading;"; flip_abort "no bundle 'm6:' counter lines >= 50 s apart at watch minute $MINN (the bundle stopped logging?)"
      elif [ "$PRES" -lt "$PRES_MIN" ]; then C6=FAIL; C6w="$C6w minute $MINN: DP presents/s $PRES < $PRES_MIN;"; flip_abort "DP frames delivered collapsed: $PRES presents/s at watch minute $MINN (floor $PRES_MIN)"; fi
      LAST_PRES=$PRES
    fi
    review_checks_b
  else
    fast_sample watch; health_abort
  fi
  # CRC / restore bookkeeping on whichever sample just ran (S_T, CRC_IS_A, X_WDOG are fresh)
  if [ "$CRC_IS_A" = 1 ]; then A_RETURNS=$((A_RETURNS+1)); fi
  if [ -n "${X_WDOG:-}" ] && [ "$X_WDOG" != 0 ]; then LAST_X_WDOG=$X_WDOG; fi
  cur="$CRC_RG_NOW/$CRC_B_NOW"
  if [ -n "$PREV_CRC" ] && [ $(( S_T - PREV_T )) -ge 8 ]; then
    if [ "$cur" = "$PREV_CRC" ]; then EQ_PAIRS=$((EQ_PAIRS+1)); [ -n "$REST_START" ] || REST_START=$PREV_T; else NEQ_PAIRS=$((NEQ_PAIRS+1)); fi
    PREV_CRC=$cur; PREV_T=$S_T
  elif [ -z "$PREV_CRC" ]; then PREV_CRC=$cur; PREV_T=$S_T; fi
  LAST_T=$S_T
  # the fast loop sleeps; a K-snapshot is itself ~8 s
  sleep "$FAST_SLEEP"
done

# ---------------------------------------------------------------------------------------------- P8 criterion 9 (optional) and criterion 7 (END of the watch)
if [ "$USER_STEP" = 1 ]; then
  say; say "=== P8 criterion 9 (USER_STEP=1): ************  MOVE THE MOUSE ONTO THE MONB NOW  ************  sampling m6stat/m6xstat + CRC every 2 s for 60 s"
  if [ "$DRYRUN" = 1 ]; then say "  DRYRUN: 30 samples: m6stat 0 (monitor B transactions), m6xstat 0 (presents2), disp2 crc 2; PASS = CRC changes from the first sample and the last 5 are equal"
  else
    sleep 3; U_FIRST=""; U_CHANGED=0; U_LAST5=""; us=0
    while [ $us -lt 30 ]; do us=$((us+1))
      N=$((N+1)); UF=$(printf '%s/%02d-ustep%02d.txt' "$RUN" "$N" "$us")
      ssh $SSHO "$HOST" "date +%s; sudo -n $T accel m6stat 0 2>&1 | grep 'instance 2 '; sudo -n $T accel m6xstat 0 2>&1 | grep -E 'presents [0-9]+, latched'; sudo -n $T accel disp2 crc 2 2>&1 | grep 'OTG2 CRC'" >"$UF" 2>&1
      parse_crc "$UF"; c="$CRC_RG_NOW/$CRC_B_NOW"; tx=$(sed -n 's/^ *instance 2 .*: \([0-9][0-9]*\) transactions.*/\1/p' "$UF" | head -1); pr=$(sed -n 's/^  presents \([0-9]*\),.*/\1/p' "$UF" | head -1)
      [ -n "$U_FIRST" ] || U_FIRST=$c; [ "$c" = "$U_FIRST" ] || U_CHANGED=1
      U_LAST5="$U_LAST5 $c"; U_LAST5=$(printf '%s\n' $U_LAST5 | tail -5 | tr '\n' ' ')
      say "  ustep $us: monitor B txn ${tx:-?} presents2 ${pr:-?} CRC $c"
      sleep 2
    done
    U_STABLE=0; [ "$(printf '%s\n' $U_LAST5 | sort -u | wc -l | tr -d ' ')" = 1 ] && [ "$(printf '%s\n' $U_LAST5 | wc -l | tr -d ' ')" = 5 ] && U_STABLE=1
    if [ "$U_CHANGED" = 1 ] && [ "$U_STABLE" = 1 ]; then C9=PASS; C9w="(the CRC changed from its start value and the last 5 samples are equal)"
    else C9=NOT-OBSERVED; C9w="(changed=$U_CHANGED stable-at-end=$U_STABLE: the pointer may not have reached the monitor B; not a failure of the stage)"; fi
  fi
else C9="SKIPPED (USER_STEP=0)"; fi

KILL_RESULT_NOTE=""
if [ "$SKIP_KILL" = 1 ]; then C7="SKIPPED (SKIP_KILL=1: the GPU desktop stays live on both screens)"
else
  say; say "=== criterion 7: the kill switch at the END of the watch (touch $KILLFILE; EARLIEST2 = A and CRC_A within 6 s). AFTER THIS the monitor B shows A (the bars) and instance 2 stays OFF until a reboot."
  RESTORES_BEFORE=${X_RESTORES:-}
  kill_and_verify
  review_checks_kill
  C7=$([ "$KILL_OK" = 1 ] && [ "${RCK:-PASS}" = PASS ] && echo PASS || { [ "$DRYRUN" = 1 ] && echo '(dry run)' || echo FAIL; }); C7w="(EARLIEST2 = programmed = A and CRC_A first seen at touch+${KILL_SECS:-?}s, limit 6 s; after-kill review checks: ${RCK:-n/a} ${RCKw:-})"
  KILL_RESULT_NOTE="the kill switch WAS used: the monitor B shows the console buffer A; instance 2 is OFF for this WindowServer process"
fi

# ---------------------------------------------------------------------------------------------- P9 logs + verdicts
say; say "=== P9 logs since the restart (WindowServer log, bundle counters, displays in ioreg, crash reports)"
px wslog "sudo -n log show --info --start '$RESTART_AT' --predicate 'process == \"WindowServer\"' --style compact >/tmp/n48-s1b-wslog.txt 2>&1; grep -E 'Unable to find display pipe|compositor activated' /tmp/n48-s1b-wslog.txt | sed 's/^/WS: /'; echo \"nopipe-lines: \$(grep -c 'Unable to find display pipe' /tmp/n48-s1b-wslog.txt)\"; wc -l /tmp/n48-s1b-wslog.txt" || true
NOPIPE=$(printf '%s' "$PXOUT" | sed -n 's/^nopipe-lines: //p' | head -1)
ACT=$(printf '%s' "$PXOUT" | /usr/bin/grep -c 'compositor activated')
px bundlelog "sudo -n log show --info --start '$RESTART_AT' --predicate 'eventMessage CONTAINS \"scanout (instance 2)\" OR eventMessage CONTAINS \"m6: surfaces\"' --style compact 2>/dev/null | grep 'WindowServer\\[' | tail -40" || true
printf '%s\n' "$PXOUT" >"$RUN/bundlelog-final.txt"
M6L=$(printf '%s\n' "$PXOUT" | /usr/bin/grep 'm6: surfaces for instance' | tail -1)
parse_bundle2 "$RUN/bundlelog-final.txt"
say "  last instance-2 counters: presents ${B_PRES2:-?} planned ${B_PLAN:-?} inst_mismatch ${B_MISMATCH:-?} watchdog_restores ${B_WDOG:-?} gpu_fail ${B_GPUFAIL:-?} drops ${B_DROPS:-?}"
say "  bundle log lines naming a FAIL CLOSED / OFF: $(/usr/bin/grep -c 'FAIL CLOSED' "$RUN/bundlelog-final.txt")"
px displays 'ioreg -c IOFramebuffer -r -d1 -w0 | grep -E "^\+-o "; echo "Navi48Framebuffer nodes: $(ioreg -c Navi48Framebuffer -r -d1 -w0 | grep -cE "^\+-o Navi48Framebuffer")"' || true
FBNODES=$(printf '%s' "$PXOUT" | sed -n 's/^Navi48Framebuffer nodes: //p'); FBTOTAL=$(printf '%s' "$PXOUT" | /usr/bin/grep -cE '^\+-o ')
px finalstate "sudo -n $T accel m6xstat 0 2>&1; sudo -n $T accel m6xstat 1 2>&1; sudo -n $T accel m6xstat 2 2>&1; sudo -n $T accel sessstat 4 2>&1; pgrep -x WindowServer; find /Library/Logs/DiagnosticReports -newer /tmp/n48-s1b-marker 2>/dev/null; echo \"noflip2: \$(ls $KILLFILE 2>&1)\"" || true
if [ "$DRYRUN" != 1 ]; then
  printf '%s\n' "$PXOUT" >"$RUN/final.txt"; parse_x0 "$RUN/final.txt"; parse_hung "$RUN/final.txt"
  FINAL_WS=$(printf '%s\n' "$PXOUT" | /usr/bin/grep -E '^[0-9]+$' | head -1)
  FINAL_REPORTS=$(printf '%s\n' "$PXOUT" | /usr/bin/grep 'DiagnosticReports' | tr '\n' ' ')
fi
px post-driverlog "sudo -n $T log 2>&1 | tail -200" || true
collect_ep

say; say "=== P10 verdicts"
if [ "$DRYRUN" = 1 ]; then
  R0="(dry run)"; C1="(dry run)"; C2="(dry run)"; C3="(dry run)"; C4="(dry run)"; C5="(dry run)"; C6="(dry run)"; C10="(dry run)"
  say "  DRYRUN: no verdicts are computed; the rules are in the header (R0, C1..C10)"
  summary_and_exit 0
fi
# C3
w=""; REST_S=0; [ -n "$REST_START" ] && REST_S=$(( LAST_T - REST_START ))
[ "$EQ_PAIRS" -ge 1 ] || w="$w no two CRCs ~10 s apart were equal (equal pairs $EQ_PAIRS, unequal $NEQ_PAIRS);"
[ "$A_RETURNS" = 0 ] || w="$w the CRC returned to CRC_A at $A_RETURNS sample(s) after the flip (a restore, or A is on screen);"
[ "${X_WDOG:-x}" = 0 ] && [ "${LAST_X_WDOG:-0}" = 0 ] || w="$w watchdog restores ${X_WDOG:-?} (max seen $LAST_X_WDOG);"
[ "${B_WDOG:-0}" = 0 ] || w="$w the bundle counter shows watchdog_restores ${B_WDOG:-?};"
[ "$REST_S" -ge 300 ] || w="$w only ${REST_S} s at rest (< 300);"
C3=$([ -z "$w" ] && echo PASS || echo FAIL); C3w="$w (equal pairs $EQ_PAIRS, unequal $NEQ_PAIRS, at rest ${REST_S} s from the first equal pair, restores ${X_WDOG:-?})"
# C4 (aborts already stopped the run; here the final reads)
if [ "$C4" = PENDING ]; then
  w=""; [ "${X_UF:-x}" = 0 ] || w="$w HUBP2 underflow ${X_UF:-?} in the last fast sample;"; [ "${HUNG:-x}" = 0 ] || w="$w HUNG '${HUNG:-?}';"
  C4=$([ -z "$w" ] && echo PASS || echo FAIL); C4w="$w (every fast sample ~$((FAST_SLEEP+4)) s + every minute: underflow / ODM2 bit 10 / VM fault / HUNG all 0; samples.tsv)"
fi
# C5
w=""; [ -n "${X_REUSE:-}" ] || w="$w reuse_inuse_refused not parsed;"; [ -n "${B_MISMATCH:-}" ] || w="$w inst_mismatch not parsed;"
[ "${X_REUSE:-0}" = 0 ] || WARN="$WARN
  REUSE_INUSE_REFUSED = $X_REUSE (the bundle asked to reuse a slot the hardware still fetched, and the kernel refused: no in-use slot was overwritten by a present, but the bundle should not ask)"
C5=$([ -z "$w" ] && echo PASS || echo FAIL); C5w="$w (REUSE_INUSE_REFUSED ${X_REUSE:-?}; inst_mismatch ${B_MISMATCH:-?}; presents ${B_PRES2:-?} planned ${B_PLAN:-?}; actual in-use reuse is not directly measurable: AMBIGUITY 4)"
# C6
if [ "$C6" = PENDING ]; then if [ "$DP_BAD" = 0 ] && [ -n "$DP_FIRST" ]; then C6=PASS; else C6=FAIL; fi; fi
C6w="$C6w (minute-1 DP stamps/s ${DP_FIRST:-?}, floor $DP_MIN; last presents/s ${LAST_PRES:-?}, floor $PRES_MIN from minute 2; DP pixels are NOT CRC-checked: judge by eye)"
# C10
w=""; [ "${FINAL_WS:-}" = "$WS1" ] || w="$w WindowServer pid ${FINAL_WS:-?} != $WS1;"; [ "${HUNG:-x}" = 0 ] || w="$w HUNG '${HUNG:-?}';"; [ -z "${FINAL_REPORTS// /}" ] || w="$w new crash reports since the marker: $FINAL_REPORTS;"
C10=$([ -z "$w" ] && echo PASS || echo FAIL); C10w="$w (WS pid $WS1 -> ${FINAL_WS:-?})"
# final 1a proofs
k1=PASS; w=""
[ "${NOPIPE:-1}" = 0 ] || { k1=FAIL; w="$w WS logged 'Unable to find display pipe' ${NOPIPE:-?} times;"; }
[ "${FBNODES:-0}" = 1 ] || { k1=FAIL; w="$w Navi48Framebuffer nodes ${FBNODES:-?};"; }
[ "${FBTOTAL:-0}" = 2 ] || WARN="$WARN
  ioreg lists ${FBTOTAL:-?} IOFramebuffer nodes (expected 2)"
K1=$k1; K1w="$w (WS log nopipe ${NOPIPE:-?}; 'compositor activated' $ACT; AGDC nfb ${M_AGN:-?})"
k2=PASS; w=""; [ "${M_TXN0:-0}" -gt 0 ] && [ "${M_TXN2:-0}" -gt 0 ] || { k2=FAIL; w="$w a pipe has 0 transactions (DP ${M_TXN0:-?}, monitor B ${M_TXN2:-?});"; }; K2=$k2; K2w="$w (DP ${M_TXN0:-?}, monitor B ${M_TXN2:-?})"
k3=PASS; w=""; [ "${M_IDS0:-0}" -ge 2 ] && [ "${M_IDS0:-0}" -le 3 ] || { k3=FAIL; w="$w DP IDs ${M_IDS0:-?} (want 2-3);"; }; [ "${M_IDS2:-0}" -ge 2 ] && [ "${M_IDS2:-0}" -le 3 ] || { k3=FAIL; w="$w monitor B IDs ${M_IDS2:-?} (want 2-3);"; }
[ "${M_AMB:-1}" = 0 ] && [ "${M_NOFB:-1}" = 0 ] && [ "${M_AMB2:-1}" = 0 ] || { k3=FAIL; w="$w ambiguous ${M_AMB:-?}/${M_AMB2:-?} or no-fb ${M_NOFB:-?};"; }; K3=$k3; K3w="$w (IDs DP ${M_IDS0:-?} monitor B ${M_IDS2:-?}; spec amended: 2-3 per pipe)"
[ "$K1$K2$K3" = PASSPASSPASS ] || WARN="$WARN
  final 1a proofs: K1 $K1 K2 $K2 K3 $K3 (see their lines)"
say "  R0 $R0 $R0w"; say "  C1 $C1 $C1w"; say "  C2 $C2 $C2w"; say "  C3 $C3 $C3w"; say "  C4 $C4 $C4w"; say "  C5 $C5 $C5w"; say "  C6 $C6 $C6w"; say "  C7 $C7 $C7w"; say "  C9 $C9 $C9w"; say "  C10 $C10 $C10w"
ALL=PASS
for v in "$R0" "$C1" "$C2" "$C3" "$C4" "$C5" "$C6" "$C7" "$C10"; do case "$v" in PASS|PASS-AT-FLIP|SKIPPED*) ;; *) ALL=FAIL ;; esac; done
if [ "$ALL" = PASS ]; then
  say "### STAGE 1b: PASS (R0, C1-C7, C10$([ "$C9" = PASS ] && echo ', C9')). $([ -n "$KILL_RESULT_NOTE" ] && echo "$KILL_RESULT_NOTE." || echo 'PC left PARKED with the GPU desktop live on both screens.') 0 users. No teardown sent."; summary_and_exit 0
else FAILS="$FAILS
  one or more criteria FAILED (see above); the evidence is in $RUN"; capture; say "### STAGE 1b: FAIL. PC left PARKED (no teardown verbs)."; summary_and_exit 1; fi
