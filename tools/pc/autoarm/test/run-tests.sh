#!/bin/bash
# run-tests.sh - fake-environment harness for n48-autoarm.sh / install.sh / uninstall.sh. host Mac only: no ssh, nothing contacts the PC.
# Fake navi48test / n48nub / stat / who / pgrep / killall / ioreg / sysctl / kmutil / launchctl / shutdown / nohup / sleep on PATH, a fake root for the /Library and /private/tmp paths.
# Every scenario asserts the exact command sequence the script issued (fake logs: $FAKE_DIR/cmds.log). Temporaries: one mktemp dir under /private/tmp/, removed at the end with a prefix check.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); SRC=$(cd "$HERE/.." && pwd)
BASE=/private/tmp; mkdir -p "$BASE"
W=$(mktemp -d "$BASE/autoarm-test.XXXXXX") || exit 1
case "$W" in "$BASE"/autoarm-test.*) ;; *) echo "bad temp dir $W"; exit 1 ;; esac
PASS=0; FAIL=0; SCN=""
ok() { PASS=$((PASS+1)); }
bad() { FAIL=$((FAIL+1)); echo "FAIL [$SCN] $*"; }
eq() { [ "$2" = "$3" ] && ok || bad "$1: got '$2' want '$3'"; }
BA_THREE="navi48-m6=1 navi48-m6flip=1 navi48-m6flip1=1 navi48-fb2=1 navi48-metal-disp=1 navi48-disp2=1 navi48-dmubcmd=1 keepsyms=1"
BA_TWO="navi48-m6=1 navi48-m6flip=1 navi48-fb2=1 navi48-metal-disp=1 navi48-disp2=1 navi48-dmubcmd=1 keepsyms=1"

# ---- golden command lists (what the script must issue, in order, filtered to CLI / NUB / killall / shutdown)
monb_chain() { cat <<'E'
CLI accel dmubsend pclk otg2-1440-on
CLI accel disp2 timing1440 2
CLI accel dmubsend phyd enable-1440
CLI accel dmubsend digd setup-1440
CLI accel disp2 connect 2
CLI accel scdcread 3 0x40 1
CLI accel disp2 plane 2
CLI accel disp2 show 2
CLI accel disp2 crc 2
E
}
mona_chain() { cat <<'E'
CLI accel dmubsend pclk otg1-on
CLI accel disp2 crc 2
CLI accel dmubsend phyc enable
CLI accel disp2 crc 2
CLI accel dmubsend digc setup
CLI accel disp2 crc 2
CLI accel disp2 timing 1
CLI accel disp2 crc 2
CLI accel disp2 connect 1
CLI accel disp2 crc 2
CLI accel disp2 plane 1
CLI accel disp2 crc 2
CLI accel disp2 show 1
CLI accel disp2 crc 2
CLI accel disp2 crc 1
CLI accel disp2 crc 1
CLI accel disp2 crc 1
CLI accel disp2 crc 2
E
}
golden_pre() { # golden_pre THREE|TWO : everything up to and including the WindowServer kill
  echo "CLI accel m6xstat 0"; echo "CLI reg 0x4fea"; echo "CLI accel m6stat 0"; echo "CLI accel m6stat 4"; echo "CLI accel m6xstat 2 0"
  [ "$1" = THREE ] && echo "CLI accel m6xstat 1 0"
  echo "NUB status"; echo "CLI accel disp2 status 2"; echo "CLI accel vramstat"
  monb_chain
  [ "$1" = THREE ] && mona_chain
  echo "CLI accel disp2 fbhold 2"; echo "CLI accel disp2 crc 2"
  if [ "$1" = THREE ]; then echo "CLI accel disp2 fbhold 1"; echo "CLI accel disp2 crc 1"; echo "CLI accel disp2 crc 2"; fi
  echo "CLI accel fbpublish 2"; echo "CLI accel disp2 crc 2"
  if [ "$1" = THREE ]; then echo "CLI accel fbpublish 1"; echo "CLI accel disp2 crc 1"; echo "CLI accel disp2 crc 2"; fi
  echo "NUB publish"; echo "CLI accel pipeadopt"; echo "CLI accel pipeagdc 1"; echo "CLI accel fbname 1"; echo "CLI accel pipearm 1"
  echo "CLI accel disp2 crc 2"; [ "$1" = THREE ] && echo "CLI accel disp2 crc 1"
  echo "CLI accel m6xstat 2 0"; [ "$1" = THREE ] && echo "CLI accel m6xstat 1 0"
  echo "CLI accel pipereload"; echo "FAKE killall -9 WindowServer"
}
golden_post() { # 3 health samples (HEALTH_S=9, step 3) + the allow-list
  local k; for k in 1 2 3; do echo "CLI accel sessstat 4"; echo "CLI accel m6xstat 2 0"; echo "CLI accel m6xstat 2 2"; [ "$1" = THREE ] && { echo "CLI accel m6xstat 1 0"; echo "CLI accel m6xstat 1 2"; }; done
  echo "CLI accel appallow add Maps"; echo "CLI accel appallow add Preview"
}

# ---- scenario plumbing
setup() { # setup <name> [bootargs]: fresh fake root + fake dir; exports the env the script sees
  SCN=$1; SD=$W/$1; mkdir -p "$SD/fd" "$SD/tmp" "$SD/fr/Library/Application Support/Navi48/autoarm" "$SD/fr/Library/Logs"
  echo 5001 >"$SD/fd/wspid"; echo root >"$SD/fd/console"; : >"$SD/fd/who"
  SUPD="$SD/fr/Library/Application Support/Navi48"; LOGF="$SD/fr/Library/Logs/Navi48/autoarm.log"; CMDS="$SD/fd/cmds.log"
  cp "$SRC/n48-autoarm-watch.sh" "$SUPD/autoarm/"
  ENVV=(AA_LIB="$SD/fr/Library" AA_TMP="$SD/tmp" AA_CLI="$HERE/fake-navi48test" AA_NUB="$HERE/fake-n48nub" AA_PATH="$HERE/fakebin:/usr/bin:/bin" AA_SETTLE_S=0 AA_HEALTH_S=9 AA_HEALTH_STEP=3 AA_DRIVER_WAIT_S=15 AA_REBOOT_CMD="$HERE/fakebin/shutdown -r now" FAKE_DIR="$SD/fd" FAKE_BOOTARGS="${2:-$BA_THREE}")
}
runit() { # runit [extra env...] : runs the script, sets EXIT
  env "${ENVV[@]}" "$@" /bin/bash "$SRC/n48-autoarm.sh" >"$SD/stdout.txt" 2>&1; EXIT=$?
}
cmdlist() { /usr/bin/grep -E '^(CLI|NUB|FAKE killall|FAKE shutdown)' "$CMDS" 2>/dev/null; }
n_of() { cmdlist | /usr/bin/grep -c -- "$1"; }
last_cli() { cmdlist | tail -1; }
no_teardown_after_hold() { # nothing after the first fbhold may be a teardown / dmub / off verb
  cmdlist | awk '/fbhold/{f=1;next} f' | /usr/bin/grep -E 'dmubsend|planeoff|disp2 off|pipearm 0|pipeagdc 0|pipereload 1| off' | wc -l | tr -d ' '
}
killfiles() { local s=""; [ -e "$SD/tmp/n48m-noflip1" ] && s="$s 1"; [ -e "$SD/tmp/n48m-noflip2" ] && s="$s 2"; echo "${s# }"; }
logline() { /usr/bin/grep -c -- "$1" "$LOGF"; }

# ---- 1. happy paths
setup happy3; runit; eq "exit" $EXIT 0; eq "command sequence (3-display)" "$( { golden_pre THREE; golden_post THREE; } | diff - <(cmdlist) >/dev/null && echo same || { golden_pre THREE; golden_post THREE; } | diff - <(cmdlist) | head -5)" same
eq "one WS restart" "$(n_of 'FAKE killall')" 1; eq "no reboot" "$(n_of 'FAKE shutdown')" 0; eq "no kill files" "$(killfiles)" ""
eq "headless flag created" "$([ -e "$SD/tmp/n48m-headless-no" ] && echo y)" y; eq "watcher started" "$(/usr/bin/grep -c 'FAKE nohup' "$CMDS")" 1
eq "RESULT line" "$(logline 'RESULT: ARMED (THREE-display)')" 1
setup happy2 "$BA_TWO"; runit; eq "exit" $EXIT 0; eq "command sequence (2-display)" "$( { golden_pre TWO; golden_post TWO; } | diff - <(cmdlist) >/dev/null && echo same || { golden_pre TWO; golden_post TWO; } | diff - <(cmdlist) | head -5)" same
eq "no monitor A verb at all" "$(n_of 'disp2 .* 1$\|otg1\|phyc\|digc\|fbhold 1\|fbpublish 1\|m6xstat 1')" 0
# custom allow-list: comments, blanks, an invalid line
setup apps; printf '# c\nMaps\n\n  Safari  \nbad;name\nPreview # x\n' >"$SUPD/apps.txt"; runit; eq "exit" $EXIT 0
eq "allow-list adds" "$(cmdlist | /usr/bin/grep appallow | tr '\n' '|')" "CLI accel appallow add Maps|CLI accel appallow add Safari|CLI accel appallow add Preview|"
# driver ready only after 2 polls
setup delay; runit FAKE_DRIVER_DELAY=2; eq "exit" $EXIT 0; eq "polls before ready" "$(cmdlist | awk '/m6xstat 0/&&!d{c++} /reg 0x4fea/{d=1} END{print c}')" 3

# ---- 2. kill switch, once per boot, log rotation
setup killsw; touch "$SUPD/autoarm-off"; runit; eq "exit" $EXIT 10; eq "no command at all" "$(cat "$CMDS" 2>/dev/null | wc -l | tr -d ' ')" 0; eq "no tmp files" "$(ls "$SD/tmp" | wc -l | tr -d ' ')" 0
setup twice; runit; eq "first run" $EXIT 0; runit; eq "second run same boot refuses" $EXIT 10; eq "one restart only" "$(n_of 'FAKE killall')" 1
setup twice2; runit; runit FAKE_BOOTSEC=1791479999 AA_FORCE=; eq "new boot runs again (then refuses: nub present)" $EXIT 7
setup rotate; for k in 1 2 3 4 5 6 7 8; do runit AA_FORCE=1 FAKE_FAIL_VERB="accel reg 0x4fea" ; done; eq "log files kept" "$(ls "$SD/fr/Library/Logs/Navi48" | tr '\n' ' ')" "autoarm.log autoarm.log.1 autoarm.log.2 autoarm.log.3 autoarm.log.4 autoarm.log.5 "

# ---- 3. users
setup user0; echo testuser >"$SD/fd/console"; runit; eq "exit (user at start)" $EXIT 3; eq "no CLI verb sent" "$(cmdlist | wc -l | tr -d ' ')" 0
setup user0b; echo "someone console  Oct 8" >"$SD/fd/who"; runit; eq "exit (who line)" $EXIT 3
setup userwin; echo "_windowserver" >"$SD/fd/console"; runit; eq "_windowserver console owner is fine" $EXIT 0
# a user logging in after each pre-restart CLI step: the script stops before the NEXT command
setup usermid_probe; runit; cmdlist | /usr/bin/grep -n . >/dev/null
GP=$(golden_pre THREE | sed '$d'); NP=$(printf '%s\n' "$GP" | wc -l | tr -d ' ')
i=0; while IFS= read -r line; do
  i=$((i+1)); case "$line" in "NUB "*) continue ;; "CLI accel m6xstat 0") [ $i = 1 ] && continue ;; esac
  v=${line#CLI }; nth=$(printf '%s\n' "$GP" | head -$i | /usr/bin/grep -cxF -- "$line")
  [ "$nth" = 1 ] || continue      # FAKE_USER_AFTER_VERB fires on the first match of the verb
  setup "user_after_$i"
  runit FAKE_USER_AFTER_VERB="$v"
  eq "user after '$v': exit" $EXIT 3
  eq "user after '$v': last command" "$(last_cli)" "$line"
  eq "user after '$v': no WS restart / reboot" "$(n_of 'FAKE killall')$(n_of 'FAKE shutdown')" 00
done <<<"$GP"
# the user arrives after pipearm 1 / pipereload: the restart is NOT sent
setup user_reload; runit FAKE_USER_AFTER_VERB="accel pipereload"; eq "exit" $EXIT 3; eq "no killall" "$(n_of 'FAKE killall')" 0; eq "last command pipereload" "$(last_cli)" "CLI accel pipereload"

# ---- 4. DP raster / warm restart
setup fb1; runit FAKE_RASTER=0x897; eq "exit (fallback, no opt-in)" $EXIT 4; eq "commands" "$(cmdlist | tr '\n' '|')" "CLI accel m6xstat 0|CLI reg 0x4fea|"; eq "'needs a warm restart' logged" "$(logline 'needs a warm restart')" 1; eq "no reboot" "$(n_of 'FAKE shutdown')" 0
setup fb2; touch "$SUPD/autoarm-reboot-on-fallback"; runit FAKE_RASTER=0x897; eq "exit (fallback + opt-in)" $EXIT 5; eq "one reboot" "$(n_of 'FAKE shutdown')" 1; eq "commands" "$(cmdlist | tr '\n' '|')" "CLI accel m6xstat 0|CLI reg 0x4fea|FAKE shutdown -r now|"
runit FAKE_RASTER=0x897 FAKE_BOOTSEC=1791480100; eq "next boot still fallback: no second reboot" "$(n_of 'FAKE shutdown')" 1; eq "exit 4" $EXIT 4
runit FAKE_RASTER=0xa9f FAKE_BOOTSEC=1791480200 FAKE_NUB_PRESENT=1; eq "good raster clears the counter" "$([ -e "$SUPD/autoarm-state/auto-reboot-done" ] && echo present || echo cleared)" cleared
runit FAKE_RASTER=0x897 FAKE_BOOTSEC=1791480300; eq "a later fallback cycle may reboot once again" "$(n_of 'FAKE shutdown')" 2
setup fb3; touch "$SUPD/autoarm-reboot-on-fallback"; runit FAKE_RASTER=0x123; eq "unknown raster: no reboot, exit 4" "$EXIT$(n_of 'FAKE shutdown')" 40
setup fb4; echo testuser >"$SD/fd/console"; touch "$SUPD/autoarm-reboot-on-fallback"; runit FAKE_RASTER=0x897; eq "user logged in: no reboot" "$EXIT$(n_of 'FAKE shutdown')" 30

# ---- 5. every verb failing in every mode: stop right there, nothing after it, no WS restart, no reboot, no teardown
GALL=$(golden_pre THREE | sed '$d')      # up to (not incl.) killall
total=$(printf '%s\n' "$GALL" | wc -l | tr -d ' ')
i=0; while IFS= read -r line; do
  i=$((i+1))
  [ $i = 1 ] && continue                 # the driver poll has its own scenarios
  case "$line" in "CLI accel vramstat"|"NUB status") continue ;; esac     # informational / expected-nonzero
  if [ "${line%% *}" = NUB ]; then key="nub ${line#NUB }"; else key=${line#CLI }; fi
  nth=$(printf '%s\n' "$GALL" | head -$i | /usr/bin/grep -cxF -- "$line")
  for mode in rc badout empty; do
    setup "vf_${i}_$mode"
    runit FAKE_FAIL_VERB="$key" FAKE_FAIL_MODE=$mode FAKE_FAIL_NTH=$nth
    want=1
    [ "$(last_cli)" = "$line" ] || bad "verb #$i '$line' mode $mode: last command is '$(last_cli)'"
    [ $EXIT = $want ] || bad "verb #$i '$line' mode $mode: exit $EXIT (want $want)"
    [ "$(n_of 'FAKE killall')$(n_of 'FAKE shutdown')" = 00 ] || bad "verb #$i '$line' mode $mode: WS restart / reboot issued"
    [ "$(no_teardown_after_hold)" = 0 ] || bad "verb #$i '$line' mode $mode: teardown/dmub verb after the hold"
    [ -z "$(killfiles)" ] || bad "verb #$i '$line' mode $mode: kill files touched before the restart"
    ok
  done
done <<<"$GALL"
echo "  (every-verb-failure matrix: $total positions x 3 modes)"
# same matrix, 2-display mode, rc only
GT=$(golden_pre TWO | sed '$d'); i=0; while IFS= read -r line; do
  i=$((i+1)); [ $i = 1 ] && continue
  case "$line" in "CLI accel vramstat"|"NUB status") continue ;; esac
  if [ "${line%% *}" = NUB ]; then key="nub ${line#NUB }"; else key=${line#CLI }; fi
  nth=$(printf '%s\n' "$GT" | head -$i | /usr/bin/grep -cxF -- "$line")
  setup "vf2_$i" "$BA_TWO"; runit FAKE_FAIL_VERB="$key" FAKE_FAIL_MODE=rc FAKE_FAIL_NTH=$nth
  [ "$(last_cli)" = "$line" ] && [ $EXIT = 1 ] && [ "$(n_of 'FAKE killall')" = 0 ] || bad "2-display verb #$i '$line': last '$(last_cli)' exit $EXIT"
  ok
done <<<"$GT"

# ---- 6. driver / environment
setup nodrv; runit FAKE_DRIVER_DOWN=1; eq "exit (driver never ready)" $EXIT 2; eq "only m6xstat 0 polls" "$(cmdlist | sort -u | tr '\n' '|')" "CLI accel m6xstat 0|"; eq "3 polls (15 s / 5 s)" "$(n_of 'm6xstat 0')" 3
setup noflag1 "navi48-m6=1 navi48-fb2=1 navi48-metal-disp=1 navi48-disp2=1 navi48-dmubcmd=1"; runit; eq "missing m6flip latch: exit 8" $EXIT 8; eq "nothing but the driver poll" "$(cmdlist | tr '\n' '|')" "CLI accel m6xstat 0|"
setup nodisp2 "navi48-m6=1 navi48-m6flip=1"; runit; eq "missing 1b set: exit 8" $EXIT 8
setup nubthere; runit FAKE_NUB_PRESENT=1; eq "nub already published: exit 7" $EXIT 7; eq "no write verb" "$(n_of 'dmubsend\|fbhold\|pipearm')" 0
setup killpre; touch "$SD/tmp/n48m-noflip2"; runit; eq "kill file present at start: exit 7" $EXIT 7; eq "no write verb" "$(n_of 'dmubsend\|fbhold\|pipearm')" 0
setup nocli; runit AA_CLI="$SD/nonexistent"; eq "CLI missing: exit 2" $EXIT 2
setup nodisp; runit; eq ok $EXIT 0
setup nodisp_tmp; rm -f "$SD/fd/wspid"; runit; eq "no WindowServer: exit 7" $EXIT 7
setup dpbad; runit FAKE_FAIL_VERB="accel disp2 status 2" FAKE_FAIL_MODE=badout; eq "DP not counting: exit 1" $EXIT 1
setup nodes; runit FAKE_NODES_STUCK=1; eq "framebuffer nodes never appear: exit 1" $EXIT 1; eq "last verb fbpublish 2" "$(last_cli)" "CLI accel fbpublish 2"
setup monafirst; runit FAKE_MONA_FIRST=1; eq "S1: monitor A registered first: exit 1" $EXIT 1; eq "last verb fbpublish 1" "$(last_cli)" "CLI accel fbpublish 1"
setup drift; runit FAKE_MONA_DRIFT=1; eq "monitor A CRC drift after fbhold 1: exit 1" $EXIT 1; eq "last verb crc 1 right after fbhold 1" "$(last_cli)" "CLI accel disp2 crc 1"; eq "no teardown" "$(no_teardown_after_hold)" 0

# ---- 7. health check failures: kill files, one restart, no teardown, no allow-list
hc() { # hc <name> <expected kill files> <env...>
  local n=$1 exp=$2; shift 2; setup "hc_$n"; runit "$@"
  eq "$n: exit 6" $EXIT 6; eq "$n: kill files" "$(killfiles)" "$exp"; eq "$n: exactly one WS restart" "$(n_of 'FAKE killall')" 1; eq "$n: no reboot" "$(n_of 'FAKE shutdown')" 0
  eq "$n: only read verbs after the restart" "$(cmdlist | awk '/FAKE killall/{f=1;next} f' | /usr/bin/grep -cvE 'CLI accel (sessstat 4|m6xstat [12] [02])$')" 0
  eq "$n: no allow-list" "$(n_of appallow)" 0; eq "$n: logged" "$(logline 'health check FAILED')" 1
}
hc uf_monb 2 FAKE_UF_INST=2
hc uf_mona 1 FAKE_UF_INST=1
hc vmfault "1 2" FAKE_VMF=1
hc hung "1 2" FAKE_HUNG=1
hc wschange "1 2" FAKE_WS_CHANGE=1
setup hc_noreturn; runit FAKE_WS_NO_RETURN=1; eq "WS does not return: exit 6" $EXIT 6; eq "both kill files" "$(killfiles)" "1 2"; eq "one restart, no CLI after it" "$(cmdlist | awk '/FAKE killall/{f=1;next} f' | wc -l | tr -d ' ')" 0
setup hc_noacq; runit FAKE_NO_ACQ=1; eq "nothing acquired (login window idle / pool refusal) is not a fault" "$EXIT$(killfiles)" 0
setup hc_ufnotacq; runit FAKE_NO_ACQ=1 FAKE_UF_INST=2; eq "underflow on a NOT acquired instance is not judged" "$EXIT" 0
setup hc_user; runit FAKE_USER_AFTER_VERB="accel m6xstat 1 2" FAKE_WS_CHANGE=1; eq "a user logging in during the health check ends the pid judgement" "$EXIT$(killfiles)" 0
setup hc_hung_off; runit FAKE_HUNG=; eq ok $EXIT 0

# ---- 8. install / uninstall in a fake root
IR=$W/instroot; mkdir -p "$IR/Library/Application Support" "$IR/Library/LaunchDaemons" "$IR/Library/Logs"
printf 'old\n' >"$IR/Library/LaunchDaemons/other.plist"
find "$IR" | sort >"$W/inst.before"
SCN=install; AA_ROOT=$IR /bin/bash "$SRC/install.sh" >"$W/inst.out" 2>&1; eq "install exit" $? 0
eq "scripts 755" "$(stat -f %Lp "$IR/Library/Application Support/Navi48/autoarm/n48-autoarm.sh" "$IR/Library/Application Support/Navi48/autoarm/n48-autoarm-watch.sh" | tr '\n' ' ')" "755 755 "
eq "plist 644" "$(stat -f %Lp "$IR/Library/LaunchDaemons/com.navi48.autoarm.plist")" 644
eq "apps.txt default" "$(/usr/bin/grep -v '^#' "$IR/Library/Application Support/Navi48/apps.txt" | tr '\n' ' ')" "Maps Preview "
eq "manifest exists" "$([ -f "$IR/Library/Application Support/Navi48/autoarm/install.manifest" ] && echo y)" y
AA_ROOT=$IR /bin/bash "$SRC/install.sh" >/dev/null 2>&1; eq "second install refuses" $? 2
AA_ROOT=$IR /bin/bash "$SRC/uninstall.sh" >"$W/uninst.out" 2>&1; eq "uninstall exit" $? 0
find "$IR" | sort >"$W/inst.after"; eq "tree identical to before the install" "$(diff "$W/inst.before" "$W/inst.after" >/dev/null && echo same || diff "$W/inst.before" "$W/inst.after")" same
# uninstall keeps an edited apps.txt and anything it did not add
AA_ROOT=$IR /bin/bash "$SRC/install.sh" >/dev/null 2>&1; echo Safari >>"$IR/Library/Application Support/Navi48/apps.txt"; echo x >"$IR/Library/Application Support/Navi48/autoarm-off"; echo log >"$IR/Library/Logs/Navi48/autoarm.log"
AA_ROOT=$IR /bin/bash "$SRC/uninstall.sh" >"$W/uninst2.out" 2>&1
eq "edited apps.txt kept" "$([ -f "$IR/Library/Application Support/Navi48/apps.txt" ] && echo kept)" kept
eq "kill switch + log kept" "$([ -f "$IR/Library/Application Support/Navi48/autoarm-off" ] && [ -f "$IR/Library/Logs/Navi48/autoarm.log" ] && echo kept)" kept
eq "scripts and plist gone" "$(ls "$IR/Library/LaunchDaemons/com.navi48.autoarm.plist" "$IR/Library/Application Support/Navi48/autoarm" 2>/dev/null | wc -l | tr -d ' ')" 0
plutil -lint "$SRC/com.navi48.autoarm.plist" >/dev/null && ok || bad "plist lint"

echo "== $PASS checks passed, $FAIL failed"
case "$W" in "$BASE"/autoarm-test.*) rm -r "$W"; echo "removed temp dir $W (prefix-checked rm -r)" ;; esac
[ $FAIL = 0 ]
