#!/bin/bash
# n48-autoarm.sh - Navi48 PC AUTO-ARM. Runs ON THE PC as root (LaunchDaemon com.navi48.autoarm, once per boot; no ssh, no sudo).
# Brings up the GPU desktop (DP + monitor B, + monitor A when the boot-args carry navi48-m6flip1=1) unattended at the login window: the recipe of tools/pc/m6-stage2-run.sh (STAGE 2b) / m6-stage1b-run.sh
# (two-display) and tools/pc/arm-gpu-desktop.sh, WITHOUT the long watch, and FAIL-SAFE:
#   * the FIRST failure of any step stops the script and leaves the CPU-rendered desktop; after a hold NO teardown / dmubsend / planeoff / pipearm 0 verb is ever sent (the script has none);
#   * a user logging in (console owner not root/_windowserver, or any `who` line) at ANY point before the WindowServer restart stops it; nothing further is sent;
#   * exactly ONE WindowServer restart, never retried; the arm is a one-way step of the bring-up kext for this boot.
# Exit codes: 0 armed + healthy | 10 nothing done (kill switch / already ran this boot) | 1 a recipe step failed | 2 driver / CLI not ready | 3 a user logged in | 4 DP raster not full-res (needs a warm restart)
#             5 automatic warm reboot issued | 6 armed but the 30 s health check failed (kill files touched) | 7 PC not in the fresh-boot state | 8 boot-args do not carry the latches
# Environment overrides (tests only): AA_LIB AA_TMP AA_PCHOME AA_CLI AA_NUB AA_WATCH AA_PATH AA_FORCE AA_SETTLE_S AA_DRIVER_WAIT_S AA_HEALTH_S AA_HEALTH_STEP AA_CRC_RG2 AA_CRC_B2 AA_REBOOT_CMD
set -u
PATH=${AA_PATH:-/usr/bin:/bin:/usr/sbin:/sbin}; export PATH
GREP=${AA_GREP:-/usr/bin/grep}
LIB=${AA_LIB:-/Library}
SUP="$LIB/Application Support/Navi48"
LOGDIR="$LIB/Logs/Navi48"; LOG="$LOGDIR/autoarm.log"
STATE="$SUP/autoarm-state"
TMPD=${AA_TMP:-/private/tmp}
PCHOME=${AA_PCHOME:-/Users/testuser}
T=${AA_CLI:-$PCHOME/navi48-staging/navi48test}
NUB=${AA_NUB:-$PCHOME/n48-metal/n48nub}
WATCH=${AA_WATCH:-$SUP/autoarm/n48-autoarm-watch.sh}
KILL1=$TMPD/n48m-noflip1; KILL2=$TMPD/n48m-noflip2; HEADLESS=$TMPD/n48m-headless-no
SETTLE_S=${AA_SETTLE_S:-15}; DRIVER_WAIT_S=${AA_DRIVER_WAIT_S:-180}; HEALTH_S=${AA_HEALTH_S:-30}; HEALTH_STEP=${AA_HEALTH_STEP:-3}
CRC_RG2=${AA_CRC_RG2:-}; CRC_B2=${AA_CRC_B2:-}      # the monitor B's CRC_A of the static test pattern. Unset (default): measured on THIS boot at the first monb_exact (after 'disp2 show 2') and pinned for the rest of the run; set both to require a value you measured earlier
RASTER_OK=$((0xa9f)); RASTER_FALLBACK=$((0x897))                      # DP OTG0_H_TOTAL (reg 0x4fea): full-res 1440p60 / the 1080p fallback of queue 292
REBOOT_CMD=${AA_REBOOT_CMD:-/sbin/shutdown -r now}
PHASE=pre            # pre = before the WindowServer restart (user check before every CLI call); post = after
CMDN=0; OUT=""; RC=0
M_HELD=none; M_PUB=none; M_ARMED=no; M_RESTARTED=no; MODE=""

# ---------------------------------------------------------------------------------------------- logging
mkdir -p "$LOGDIR" 2>/dev/null
log() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >>"$LOG" 2>/dev/null; }
rotate_log() { # keep the current log plus 5 older ones
  [ -s "$LOG" ] || return 0
  local i=5; rm -f "$LOG.5"
  while [ $i -gt 1 ]; do [ -f "$LOG.$((i-1))" ] && mv -f "$LOG.$((i-1))" "$LOG.$i"; i=$((i-1)); done
  mv -f "$LOG" "$LOG.1"
}
log_out() { [ -n "$OUT" ] || return 0; printf '%s\n' "$OUT" | head -40 | sed 's/^/    | /' >>"$LOG" 2>/dev/null; [ "$(printf '%s\n' "$OUT" | wc -l)" -le 40 ] || log "    | ... ($(printf '%s\n' "$OUT" | wc -l | tr -d ' ') lines, first 40 shown)"; }
finish() { # finish <exit code> <text>
  local code=$1; shift
  log "RESULT: $* (exit $code)"
  log "STATE: mode=${MODE:-?} held=$M_HELD published=$M_PUB armed=$M_ARMED ws_restarted=$M_RESTARTED"
  exit "$code"
}
fail() { finish 1 "STOP, recipe step failed after CLI command #$CMDN: $* -- no further commands sent; the CPU desktop is left as is (no teardown verbs exist in this script)"; }

# ---------------------------------------------------------------------------------------------- users / CLI helpers
users_ok() { # 0 when the console belongs to root/_windowserver and `who` is empty
  local c w; c=$(stat -f %Su /dev/console 2>/dev/null); w=$(who 2>/dev/null)
  USERS_WHY=""
  case "$c" in root|_windowserver) ;; *) USERS_WHY="console owner '$c'"; return 1 ;; esac
  [ -z "$w" ] || { USERS_WHY="who: $(printf '%s' "$w" | head -1)"; return 1; }
  return 0
}
guard_users() { [ "$PHASE" = pre ] || return 0; users_ok || finish 3 "a user is logged in ($USERS_WHY) before the WindowServer restart: STOP, nothing further sent"; }
cli() { # cli <args to navi48test...>; sets OUT RC; logs the command and its output
  guard_users
  CMDN=$((CMDN+1)); log "CLI[$CMDN]: navi48test $*"
  OUT=$("$T" "$@" 2>&1); RC=$?
  log_out; log "    rc=$RC"
  return $RC
}
chk() { printf '%s\n' "$OUT" | $GREP -qE -- "$2" || fail "$1 (output lacks /$2/)"; }
chk_not() { printf '%s\n' "$OUT" | $GREP -qE -- "$2" && fail "$1 (output has /$2/)"; return 0; }
# step <description> <ERE that must match> <args...>: the CLI must exit 0, print the ERE and no "FAILED (0x"
step() { local d=$1 re=$2; shift 2; cli "$@" || fail "$d: exit code $RC"; chk_not "$d" 'FAILED \(0x'; chk "$d" "$re"; }
nap() { sleep "$1"; }
hexval() { printf '%d' "$(( $1 ))" 2>/dev/null; }

# ---------------------------------------------------------------------------------------------- recipe pieces
ST_OK='status +: 0 \(OK\)|status 0 \(OK\)'
DPW_OK='DP plane watch .*all 15 registers unchanged'
monbop() { cli accel "$@" || fail "accel $*: exit code $RC"; chk_not "accel $*" 'FAILED \(0x'; chk "accel $*: status 0" "$ST_OK"; chk "accel $*: DP plane watch" "$DPW_OK"; chk_not "accel $*" 'DISTURBED|VM FAULT'; }
monaop() { monbop "$@"; chk_not "accel $*: monitor B touched" 'monitor B watch: CHANGED|THE MONB WAS DISTURBED|MONB WATCH tripped|MONB DISTURBED'
  [ "$1" = disp2 ] && chk "accel $*: monitor B watch" 'monitor B watch: unchanged|all nine rows unchanged'; return 0; }
crc_parse() { # crc_parse <inst>: from $OUT
  local i=$1
  CRC_RG=$(printf '%s' "$OUT" | sed -n "s/.*OTG${i} CRC *: RG \(0x[0-9a-f]*\) *B \(0x[0-9a-f]*\).*/\1/p" | head -1)
  CRC_B=$(printf '%s' "$OUT" | sed -n "s/.*OTG${i} CRC *: RG \(0x[0-9a-f]*\) *B \(0x[0-9a-f]*\).*/\2/p" | head -1)
  UF_LINE=$(printf '%s' "$OUT" | $GREP "HUBP${i}_DCHUBP_CNTL" | head -1 | sed -n 's/.*\(underflow(30:28) [0-9]*  *SEG_ALLOC_ERR [0-9]*  *TIMEOUT(23:20) [0-9a-fx]*\).*/\1/p')
  ODM_OCC=$(printf '%s' "$OUT" | sed -n 's/.*underflow occurred(10) \([0-9]\).*/\1/p' | head -1)
  VMF=$(printf '%s' "$OUT" | sed -n 's/.*DCN_VM_FAULT_STATUS *: *\([0-9a-fx]*\).*/\1/p' | head -1)
  FIFO_ERR=$(printf '%s' "$OUT" | sed -n 's/.*DIG[0-9]_FIFO_CTRL0 0x[0-9a-f]* (error 29:28 \([0-9]*\)).*/\1/p' | head -1)
}
zero_hex() { local v=${1#0x}; [ -n "$v" ] && [ -z "${v//0/}" ]; }
crc_read() { # crc_read <inst>: one `disp2 crc`, exit 0, no DISTURBED / VM FAULT / monitor B watch tripped, a CRC line printed, underflow / ODM / VM-fault clean
  local i=$1
  cli accel disp2 crc "$i" || fail "accel disp2 crc $i: exit code $RC"
  chk_not "disp2 crc $i" 'FAILED \(0x|DISTURBED|VM FAULT|monitor B watch: CHANGED'
  crc_parse "$i"
  [ -n "$CRC_RG" ] && [ -n "$CRC_B" ] || fail "disp2 crc $i printed no OTG$i CRC line"
  [ "$UF_LINE" = "underflow(30:28) 0  SEG_ALLOC_ERR 0  TIMEOUT(23:20) 0" ] || fail "disp2 crc $i: HUBP$i underflow line is '$UF_LINE'"
  [ "$ODM_OCC" = 0 ] || fail "disp2 crc $i: ODM$i underflow occurred(10) is '$ODM_OCC'"
  zero_hex "$VMF" || fail "disp2 crc $i: DCN_VM_FAULT_STATUS is '$VMF'"
}
monb_exact() { crc_read 2
  if [ -z "$CRC_RG2" ] || [ -z "$CRC_B2" ]; then CRC_RG2=$CRC_RG; CRC_B2=$CRC_B; log "monitor B CRC_A (measured this boot, $1) = $CRC_RG2 / $CRC_B2"; return 0; fi
  [ "$CRC_RG" = "$CRC_RG2" ] && [ "$CRC_B" = "$CRC_B2" ] || fail "monitor B CRC $CRC_RG/$CRC_B is not the pinned CRC_A2 $CRC_RG2/$CRC_B2 ($1)"; }
mona_exact() { crc_read 1; [ "$CRC_RG" = "$A1_RG" ] && [ "$CRC_B" = "$A1_B" ] || fail "monitor A CRC $CRC_RG/$CRC_B left CRC_A1 $A1_RG/$A1_B ($1)"; }
fb_wait() { # fb_wait <n>: poll ioreg until <n> REGISTERED Navi48Framebuffer nodes exist (max 40 s); sets FB_IDS
  local want=$1 k=0 raw
  while [ $k -lt 20 ]; do
    guard_users
    raw=$(ioreg -c Navi48Framebuffer -r -d1 -w0 2>/dev/null | $GREP -E '^\+-o Navi48Framebuffer')
    FB_IDS=$(printf '%s\n' "$raw" | sed -n 's/^+-o Navi48Framebuffer  *<class Navi48Framebuffer, id \(0x[0-9a-f]*\), registered.*/\1/p' | tr '\n' ' ')
    FB_N=$(printf '%s' "$FB_IDS" | wc -w | tr -d ' ')
    [ "${FB_N:-0}" -ge "$want" ] && break
    k=$((k+1)); nap 2
  done
  log "  ioreg: ${FB_N:-0} registered Navi48Framebuffer node(s) after ~$((k*2)) s: ids ${FB_IDS:-none}"
  [ "${FB_N:-0}" = "$want" ] || fail "expected $want registered Navi48Framebuffer node(s), saw ${FB_N:-0}"
}
touch_kill() { local f; for f in "$@"; do touch "$f" 2>/dev/null && chmod 644 "$f" 2>/dev/null; log "  kill file touched: $f"; done; }

# ---------------------------------------------------------------------------------------------- 0. start: log, kill switch, once per boot
rotate_log
log "=== n48-autoarm start (pid $$) $(date)"
if [ -e "$SUP/autoarm-off" ]; then finish 10 "kill switch $SUP/autoarm-off exists: doing nothing"; fi
BOOTSEC=$(sysctl -n kern.boottime 2>/dev/null | sed -n 's/^{ sec = \([0-9]*\),.*/\1/p')
mkdir -p "$STATE" 2>/dev/null
if [ -z "${AA_FORCE:-}" ] && [ -n "$BOOTSEC" ] && [ "$(cat "$STATE/last-boot" 2>/dev/null)" = "$BOOTSEC" ]; then finish 10 "already ran in this boot (boot $BOOTSEC); set AA_FORCE=1 for a manual re-run"; fi
[ -n "$BOOTSEC" ] && printf '%s\n' "$BOOTSEC" >"$STATE/last-boot"

# ---------------------------------------------------------------------------------------------- 1. wait for the driver, WindowServer, settle
[ -x "$T" ] || finish 2 "CLI $T missing or not executable: nothing done"
k=0; max=$((DRIVER_WAIT_S/5)); [ $max -ge 1 ] || max=1
while :; do
  users_ok || finish 3 "a user is logged in ($USERS_WHY) while waiting for the driver: STOP, nothing sent"
  OUT=$("$T" accel m6xstat 0 2>&1); RC=$?
  if [ $RC -eq 0 ] && printf '%s\n' "$OUT" | $GREP -qE '^accel m6xstat 0: navi48-m6 (ON|OFF)'; then log "driver answers (accel m6xstat 0) after ~$((k*5)) s: $(printf '%s' "$OUT" | head -1)"; break; fi
  k=$((k+1)); [ $((k % 6)) -eq 1 ] && log "driver not ready yet (poll $k, rc=$RC): $(printf '%s' "$OUT" | head -1)"
  [ $k -ge $max ] && finish 2 "the driver never answered accel m6xstat 0 within ${DRIVER_WAIT_S} s: nothing done"
  nap 5
done
k=0; while [ $k -lt 45 ]; do pgrep -x WindowServer >/dev/null 2>&1 && break; k=$((k+1)); nap 2; done
WSB=$(pgrep -x WindowServer 2>/dev/null | head -1)
[ -n "$WSB" ] || finish 7 "no WindowServer after ~90 s: the login window is not up; nothing done"
log "WindowServer pid $WSB; settling ${SETTLE_S} s"
nap "$SETTLE_S"
guard_users

# ---------------------------------------------------------------------------------------------- 2. P0 prechecks (read-only)
BOOTARGS=$(sysctl -n kern.bootargs 2>/dev/null); log "boot-args: $BOOTARGS"
for t in navi48-m6=1 navi48-m6flip=1 navi48-fb2=1 navi48-metal-disp=1 navi48-disp2=1 navi48-dmubcmd=1; do
  printf '%s' "$BOOTARGS" | $GREP -qE -- "(^| )$t( |\$)" || finish 8 "boot-args lack $t: not the auto-arm variant; nothing done"
done
if printf '%s' "$BOOTARGS" | $GREP -qE -- '(^| )navi48-m6flip1=1( |$)'; then MODE=THREE; else MODE=TWO; fi
log "mode: $MODE-display ($([ $MODE = THREE ] && echo 'DP + monitor B + monitor A' || echo 'DP + monitor B'))"
for f in "$KILL1" "$KILL2"; do [ ! -e "$f" ] || finish 7 "kill file $f is present at start: not arming (remove it to allow the flips)"; done
# THE DP-RASTER CHECK FIRST (queue 292): a fresh boot can come up with the DP in the 1080p fallback; detect it here, write nothing.
cli reg 0x4fea || finish 1 "reg 0x4fea unreadable (rc=$RC): nothing written"
RASTER=$(printf '%s' "$OUT" | sed -n 's/^reg\[0x4fea\] = \(0x[0-9a-f]*\).*/\1/p' | head -1)
[ -n "$RASTER" ] || finish 1 "could not parse reg 0x4fea ('$OUT'): nothing written"
log "DP OTG0_H_TOTAL (0x4fea) = $RASTER (full-res 0xa9f; 1080p fallback 0x897)"
if [ "$(hexval "$RASTER")" != "$RASTER_OK" ]; then
  if [ -e "$SUP/autoarm-reboot-on-fallback" ]; then      # opt-in: ONE automatic warm reboot per boot cycle
    if [ "$(hexval "$RASTER")" = "$RASTER_FALLBACK" ] && [ ! -e "$STATE/auto-reboot-done" ]; then
      printf '%s\n' "$BOOTSEC" >"$STATE/auto-reboot-done"; sync
      log "DP raster is the 1080p fallback: needs a warm restart; doing the ONE automatic warm reboot ($REBOOT_CMD); marker $STATE/auto-reboot-done"
      $REBOOT_CMD >>"$LOG" 2>&1
      finish 5 "automatic warm reboot issued (DP in the 1080p fallback)"
    fi
  fi
  finish 4 "DP raster $RASTER is not full-res 0xa9f: needs a warm restart$([ -e "$STATE/auto-reboot-done" ] && echo ' (the ONE automatic reboot of this cycle was already used; not rebooting again)'); the monitor B chain was NOT run"
fi
rm -f "$STATE/auto-reboot-done"      # a good raster ends the reboot cycle: a later fallback may use its one reboot again
step "m6stat 0" 'navi48-m6 latch ON' accel m6stat 0
printf '%s\n' "$OUT" | $GREP -qE 'pipe adopted no; armed no' && printf '%s\n' "$OUT" | $GREP -qE ' 0 pipes recorded' || finish 7 "not a fresh boot: the pipe is already adopted / armed / has recorded pipes (someone ran the recipe by hand?): nothing done"
step "m6stat 4" 'endpoint dwords seen \(bitmask 0x' accel m6stat 4
step "m6xstat 2 0" '^accel m6xstat 0: navi48-m6 ON, navi48-m6flip ON$' accel m6xstat 2 0
chk "monitor B not acquired yet" 'instance 2: acquired no,'
if [ $MODE = THREE ]; then
  step "m6xstat 1 0" '^accel m6xstat inst1 page 0: navi48-m6 ON, navi48-m6flip ON, navi48-m6flip1 ON$' accel m6xstat 1 0
  chk "monitor A not acquired yet" 'instance 1: acquired no,'
fi
guard_users; CMDN=$((CMDN+1)); log "CLI[$CMDN]: n48nub status"; OUT=$("$NUB" status 2>&1); RC=$?; log_out
printf '%s\n' "$OUT" | $GREP -q 'NO Navi48MetalNub' || finish 7 "the Metal nub is already published (not a fresh boot / someone armed by hand): nothing done"
step "DP baseline" 'DP side: OTG0 COUNTING' accel disp2 status 2
cli accel vramstat; log "  (vramstat is informational only, rc=$RC)"

# ---------------------------------------------------------------------------------------------- 3. P2 the monitor B chain, P2b the monitor A chain (both BEFORE any hold)
log "=== monitor B chain"
monbop dmubsend pclk otg2-1440-on
monbop disp2 timing1440 2
monbop dmubsend phyd enable-1440
monbop dmubsend digd setup-1440
monbop disp2 connect 2
nap 3
step "SCDC" 'LOCKS onto our TMDS' accel scdcread 3 0x40 1
monbop disp2 plane 2
monbop disp2 show 2
monb_exact "after the monitor B chain"
if [ $MODE = THREE ]; then
  log "=== monitor A chain"
  monaop dmubsend pclk otg1-on;   monb_exact "after mona pclk"
  monaop dmubsend phyc enable;    monb_exact "after mona phyc"
  monaop dmubsend digc setup;     monb_exact "after mona digc"
  monaop disp2 timing 1;          monb_exact "after mona timing"
  monaop disp2 connect 1;         monb_exact "after mona connect"
  nap 2
  monaop disp2 plane 1;           monb_exact "after mona plane"
  monaop disp2 show 1;            monb_exact "after mona show"
  # CRC_A1 is MEASURED on this boot: three equal reads, FIFO error 0
  crc_read 1; X1="$CRC_RG/$CRC_B"; FIFO1=$FIFO_ERR; nap 1; crc_read 1; X2="$CRC_RG/$CRC_B"; nap 1; crc_read 1; X3="$CRC_RG/$CRC_B"
  [ "$X1" = "$X2" ] && [ "$X2" = "$X3" ] || fail "the monitor A CRC is not stable across 3 reads ($X1 $X2 $X3): CRC_A1 cannot be measured"
  [ "${FIFO_ERR:-0}" = 0 ] || fail "monitor A DIG2 FIFO error $FIFO_ERR"
  A1_RG=$CRC_RG; A1_B=$CRC_B; log "CRC_A1 (measured this boot) = $A1_RG / $A1_B"
  monb_exact "after the whole monitor A chain"
fi

# ---------------------------------------------------------------------------------------------- 4. P3 holds + publishes (IRREVERSIBLE this boot from here: no teardown, no dmubsend)
log "=== holds + publishes"
step "fbhold 2" 'fbhold 2: status 0 \(OK\)' accel disp2 fbhold 2; M_HELD=monb
monb_exact "after fbhold 2"
if [ $MODE = THREE ]; then
  step "fbhold 1" 'fbhold 1: status 0 \(OK\)' accel disp2 fbhold 1; M_HELD=monb+mona
  chk "fbhold 1: monitor B watch" 'monitor B watch: unchanged|all nine rows unchanged'
  mona_exact "after fbhold 1"; monb_exact "after fbhold 1 (monitor B)"
fi
step "fbpublish 2" 'fbpublish 2: status 0 \(OK' accel fbpublish 2; M_PUB=monb
fb_wait 1; MONB_FBID=${FB_IDS%% *}; log "  the monitor B's Navi48Framebuffer is registered: $MONB_FBID"
monb_exact "after fbpublish 2"
if [ $MODE = THREE ]; then
  step "fbpublish 1" 'fbpublish 1: status 0 \(OK' accel fbpublish 1; M_PUB=monb+mona
  fb_wait 2
  MONA_FBID=""; for x in $FB_IDS; do [ "$x" = "$MONB_FBID" ] || MONA_FBID=$x; done
  [ -n "$MONA_FBID" ] && [ $((MONB_FBID)) -lt $((MONA_FBID)) ] || fail "S1: the monitor B's registry entry ID ($MONB_FBID) is not lower than the monitor A's (${MONA_FBID:-?})"
  mona_exact "after fbpublish 1"; monb_exact "after fbpublish 1 (monitor B)"
fi

# ---------------------------------------------------------------------------------------------- 5. P4 nub, aux, adopt, agdc, fbname, headless flag, arm  (mirrors arm-gpu-desktop.sh)
log "=== nub / adopt / agdc / fbname / arm"
CMDN=$((CMDN+1)); guard_users; log "CLI[$CMDN]: n48nub publish"; OUT=$("$NUB" publish 2>&1); RC=$?; log_out; log "    rc=$RC"
[ $RC -eq 0 ] || fail "n48nub publish: exit code $RC"
chk "n48nub publish" 'publish: 0 \(Success\)'
k=0; while [ $k -lt 8 ]; do kmutil showloaded --list-only 2>/dev/null | $GREP -qiE accelprobe && break; k=$((k+1)); nap 2; done
kmutil showloaded --list-only 2>/dev/null | $GREP -qiE accelprobe || fail "the aux kext (accelprobe) is not loaded ~16 s after the nub publish"
log "  aux accelprobe loaded"
nap 2
if [ $MODE = THREE ]; then fb_wait 2; else fb_wait 1; fi
step "pipeadopt" "$ST_OK" accel pipeadopt
PA=$(printf '%s' "$OUT" | sed -n 's/.*pipes ours \/ all, display-machine starts \/ PCI substituted: \([0-9]*\) \/ \([0-9]*\),.*/\1 \2/p' | head -1)
[ -n "$PA" ] && [ "${PA% *}" = "${PA#* }" ] || fail "pipeadopt: 'pipes ours / all' is '${PA:-missing}' (want equal)"
[ $MODE != THREE ] || [ "${PA% *}" = 3 ] || fail "pipeadopt: pipes ours / all = $PA (three-display mode wants 3 / 3)"
step "pipeagdc 1" 'agdc status +: 0 \(PUBLISHED\)' accel pipeagdc 1
chk_not "pipeagdc" 'REFUSED'
step "fbname 1" 'class name NOW +: AMDRDNA4' accel fbname 1
guard_users; touch "$HEADLESS" && chmod 644 "$HEADLESS" || fail "could not create $HEADLESS"; log "  headless flag $HEADLESS created"
step "pipearm 1" '0xccf now +: 1' accel pipearm 1; M_ARMED=yes
# last look before the restart: both CRCs exact, instances not acquired, still 0 users
monb_exact "before the WindowServer restart"
[ $MODE != THREE ] || mona_exact "before the WindowServer restart"
step "m6xstat 2 0 (pre-restart)" 'instance 2: acquired no,' accel m6xstat 2 0
[ $MODE != THREE ] || step "m6xstat 1 0 (pre-restart)" 'instance 1: acquired no,' accel m6xstat 1 0

# ---------------------------------------------------------------------------------------------- 6. the ONE WindowServer restart (0 users only), watcher, CI backstop
log "=== WindowServer restart (the ONE)"
guard_users
WSB=$(pgrep -x WindowServer 2>/dev/null | head -1)
cli accel pipereload || fail "pipereload: exit code $RC"
chk "pipereload" 'restart window +: OPEN'
users_ok || { M_ARMED=yes; finish 3 "a user logged in ($USERS_WHY) between the arm and the restart: NOT killing WindowServer (the restart window closes by itself); armed state left as is"; }
KILL_EPOCH=$(date +%s)
killall -9 WindowServer >>"$LOG" 2>&1; log "  killall -9 WindowServer rc=$? (epoch $KILL_EPOCH, pid before ${WSB:-?})"
M_RESTARTED=yes; PHASE=post
WS1=""; k=0
while [ $k -lt 60 ]; do WS1=$(pgrep -x WindowServer 2>/dev/null | head -1); [ -n "$WS1" ] && [ "$WS1" != "${WSB:-}" ] && break; k=$((k+1)); sleep 1; done
if [ -z "$WS1" ] || [ "$WS1" = "${WSB:-}" ]; then
  touch_kill "$KILL1" "$KILL2"
  finish 6 "WindowServer did not come back with a new pid within 60 s (pid '${WS1:-none}'); kill files touched; no second restart is attempted"
fi
log "  WindowServer pid after the restart: $WS1 (was ${WSB:-?}) after ~${k} polls"
if [ -x "$WATCH" ]; then
  if pgrep -f n48-autoarm-watch.sh >/dev/null 2>&1; then log "  watcher already running"; else nohup "$WATCH" >/dev/null 2>&1 </dev/null & log "  watcher started: $WATCH"; fi
else log "  WARNING: watcher $WATCH missing: the headless flag / counter will NOT be removed on a later WindowServer pid change"; fi
( launchctl asuser 501 launchctl setenv CI_USE_MTL_DAG_FOR_CIKL_SRC 0 >>"$LOG" 2>&1 || log "  note: launchctl asuser 501 setenv failed (no user domain yet?): the CIKL backstop is NOT in place" ) &

# ---------------------------------------------------------------------------------------------- 7. short health check (read-only verbs only)
log "=== health check (${HEALTH_S} s, every ${HEALTH_STEP} s)"
INSTS="2"; [ $MODE = THREE ] && INSTS="2 1"
ACQ_2=""; ACQ_1=""; BAD_G=""; BAD_I=""; USER_SEEN=0; HUNG_OK=1; first=1
n=$((HEALTH_S/HEALTH_STEP)); [ $n -ge 1 ] || n=1; k=0
while [ $k -lt $n ]; do
  k=$((k+1)); nap "$HEALTH_STEP"
  WSNOW=$(pgrep -x WindowServer 2>/dev/null | head -1)
  if ! users_ok; then [ $USER_SEEN = 1 ] || log "  a user logged in during the health check ($USERS_WHY): WindowServer pid changes are no longer judged"; USER_SEEN=1; fi
  [ "$WSNOW" = "$WS1" ] || [ $USER_SEEN = 1 ] || BAD_G="$BAD_G WindowServer pid '$WSNOW' != '$WS1';"
  cli accel sessstat 4; HUNG=$(printf '%s\n' "$OUT" | sed -n 's/^  open \/ DEAD slots \/ HUNG: [0-9]* \/ [0-9]* \/ \([0-9]*\).*/\1/p' | head -1)
  if [ -z "$HUNG" ]; then [ $first = 1 ] && { HUNG_OK=0; log "  WARNING: sessstat 4 HUNG unparsable: the HUNG check is off"; }; elif [ "$HUNG" != 0 ]; then BAD_G="$BAD_G HUNG $HUNG;"; fi
  for i in $INSTS; do
    cli accel m6xstat $i 0; a=$(printf '%s\n' "$OUT" | sed -n "s/^  instance $i: acquired \([A-Za-z]*\),.*/\1/p" | head -1)
    [ "$a" = YES ] && eval "ACQ_$i=YES"
    cli accel m6xstat $i 2
    uf=$(printf '%s\n' "$OUT" | sed -n "s/^  HUBP${i}_DCHUBP_CNTL 0x[0-9a-f]* (underflow \([0-9]*\),.*/\1/p" | head -1)
    vmf=$(printf '%s\n' "$OUT" | sed -n 's/.*DCN_VM_FAULT_STATUS 0x\([0-9a-f]*\);.*/\1/p' | head -1)
    odm=$(printf '%s\n' "$OUT" | sed -n "s/.*ODM${i} OPTC_INPUT_GLOBAL_CONTROL 0x[0-9a-f]* (bit 10 \([0-9]*\)).*/\1/p" | head -1)
    [ -n "$vmf" ] && zero_hex "$vmf" || BAD_G="$BAD_G VM fault '${vmf:-unreadable}' (instance $i read);"
    eval "acq=\${ACQ_$i}"
    if [ "$acq" = YES ]; then
      [ "$uf" = 0 ] || BAD_I="$BAD_I $i"; [ "$odm" = 0 ] || BAD_I="$BAD_I $i"
      [ "$uf" = 0 ] && [ "$odm" = 0 ] || BAD_G="$BAD_G instance $i (acquired) underflow '${uf:-?}' ODM bit 10 '${odm:-?}';"
    elif [ "$uf" != 0 ]; then log "  note: instance $i not acquired, underflow '${uf:-?}' (not judged)"; fi
  done
  first=0
  [ -z "$BAD_G" ] || break
done
log "health: acquired monitor B=${ACQ_2:-no} monitor A=${ACQ_1:-n/a}; users appeared=$USER_SEEN"
if [ -n "$BAD_G" ]; then
  # global faults (VM fault / HUNG / pid change) touch BOTH kill files; an underflow on an acquired instance touches that instance's file
  case "$BAD_G" in *"VM fault"*|*HUNG*|*"WindowServer pid"*) touch_kill "$KILL1" "$KILL2" ;; *)
    for i in $(printf '%s\n' $BAD_I | sort -u); do if [ "$i" = 1 ]; then touch_kill "$KILL1"; else touch_kill "$KILL2"; fi; done ;;
  esac
  finish 6 "armed but the health check FAILED:$BAD_G kill files touched; no teardown, no second restart"
fi

# ---------------------------------------------------------------------------------------------- 8. app allow-list (best effort; only after a healthy arm)
APPS="$SUP/apps.txt"; NAMES=""
if [ -f "$APPS" ]; then NAMES=$(sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' "$APPS" | $GREP -E '^[A-Za-z0-9 ._+-]{1,64}$'); else NAMES=$(printf 'Maps\nPreview\n'); fi
printf '%s\n' "$NAMES" | while IFS= read -r nm; do
  [ -n "$nm" ] || continue
  CMDN=$((CMDN+1)); log "CLI: navi48test accel appallow add $nm"
  r=$("$T" accel appallow add "$nm" 2>&1); log "    | $(printf '%s' "$r" | head -2 | tr '\n' ' ') (rc=$?)"
done
finish 0 "ARMED ($MODE-display): WindowServer $WSB -> $WS1, health OK, monitor B acquired=${ACQ_2:-no}${ACQ_1:+ monitor A acquired=$ACQ_1}"
