#!/bin/bash
# m6-stage1a-deploy.sh - DEPLOY PLAN for M6 Stage 1a (bring-up 0.0.659 + aux 0.0.7 + Metal bundle 11 + latch navi48-m6=1).
# RUN ON THE AIR. Contacts the PC (host alias `navi48`) ONLY when the brief grants PC operation; follow
# the project PC-run protocol. `DRYRUN=1` prints every ssh/scp command and runs none of them.
# Written from: NATIVE-S7-MULTIMON.md "### Stage 1a", tools/native/navi48accel/INSTALL.md (0.0.7 step 4b, as written by the builder),
# tools/native/navi48metal/INSTALL.md (bundle 11), pc-protocol.md sections 2, 3, 8, queue 286 (Run B deploy plan).
#
# WHAT IT DOES, in order (every step logs; the FIRST failure stops the script, nothing is retried):
#   D0  host Mac preflight: the built artefacts exist, versions 0.0.659 / 0.0.7 / 11, signed, the build-proof tokens are INSIDE the binaries, the variant's boot-args
#       are the ms-apps ones + navi48-fb2=1 + navi48-m6=1.
#   D1  PC orientation (read-only): reachable, 0 console users, uptime, boot-args, loaded kexts, ESP kext, aux KC, newest DiagnosticReports.
#   D2  stage to ~/navi48-staging (old files MOVED aside into ~/navi48-staging/backup/, never deleted): the bring-up kext (build-armg), the aux kext, the CLI,
#       the variant plist, the bundle tar, the nub tool if missing.
#   D3  bring-up kext 0.0.659 onto the ESP (esp-kext.sh install); `esp-kext.sh show` must name 0.0.659 BEFORE any reboot.
#   D4  aux 0.0.7 at the NEW path /Library/Extensions/Navi48Accel-0.0.7.kext (step 4b): back up the aux KC, move every other bundle holding the id
#       com.navi48.accelprobe aside, copy, chown/chmod/xattr, `kmutil libraries`, ONE refused `kmutil load`. NEVER `kmutil install --update-all`.
#   D5  Metal bundle 11 into /Library/GPUBundles (the old one MOVED aside, no reboot needed for it).
#   D6  boot variant stage17-native-1440-metal-disp-amfi-m6 via esp-config.sh, after the stale-variant check (pc-protocol 3): every boot-arg live now must
#       be in the variant; the differences are printed and must be exactly the expected ones.
#   D7  reboot ONLY through reboot-pc.sh (gates: 0 users, no U-state), then the pc-protocol 8 stop rules (down within 180 s, wait-for-driver 300 s, boottime changed),
#       then the post-boot version checks (kext 0.0.659, aux 0.0.7 at the new path, bundle 11, boot-args with navi48-m6=1, m6stat latch ON, aux KC names 0.0.7).
#
# STOP RULES (pc-protocol 8): PC unreachable after one more check -> "PC needs a manual reset"; a reboot that does not take within bounds -> "PC needs a manual
# power-cycle"; reboot-pc.sh refusal (exit 3/4) -> STOP and report its snapshot; two panics in a row on this build -> stop (the script does not loop; it exits 1).
#
# ROLLBACK (documented here, NOT automated): reboot into the ms-apps variant on the previous build:
#   ssh navi48 '~/navi48-staging/esp-config.sh stage17-native-1440-metal-disp-amfi-ms-apps'      # boot-args without navi48-m6 / navi48-fb2
#   (the previous kext 0.0.658 is the file this script moved to ~/navi48-staging/backup/Navi48Bringup.kext.<ts>; esp-kext.sh install needs it staged as
#    ~/navi48-staging/Navi48Bringup.kext; the aux 0.0.6 dir is in ~/navi48-staging/backup/ - copy it back to a NEW path, e.g. /Library/Extensions/Navi48Accel-0.0.6b.kext,
#    never reuse a path; or leave 0.0.7: with navi48-m6 absent it behaves as 0.0.6.)  Kill switch at any time: boot-arg navi48-aux=0.
#   then ~/navi48-staging/reboot-pc.sh and, on the ms-apps boot, tools/pc/arm-gpu-desktop.sh (at 0 users).
#
# LOGS: notes/logs/runs/m6-s1a/<timestamp>-deploy/ (gitignored).
# Env: DRYRUN=1  N48_SRC=<tree holding the built artefacts, default the main repo>  N48_HOST=navi48  KEXT_BUILD=build-armg  SKIP_REBOOT=1 (stop after D6)

set -u
set -o pipefail
ROOT=${N48_ROOT:-/path/to/navi48-checkout.nosync}
SRC=${N48_SRC:-$ROOT}
HOST=${N48_HOST:-navi48}
DRYRUN=${DRYRUN:-0}
KEXT_BUILD=${KEXT_BUILD:-build-armg}
VARIANT=stage17-native-1440-metal-disp-amfi-m6
WANT_KEXT=0.0.659
WANT_AUX=0.0.7
WANT_BUNDLE=11
AUX_DIR=/Library/Extensions/Navi48Accel-$WANT_AUX.kext
TS=$(date '+%Y%m%d-%H%M%S')
OUT=$ROOT/notes/logs/runs/m6-s1a/$TS-deploy
LOG=$OUT/deploy.log
SSHO="-o BatchMode=yes -o ConnectTimeout=8"
STEPNO=0

if [ "$DRYRUN" = 1 ]; then OUT=${DRYRUN_OUT:-/private/tmp/m6-s1a-deploy-dry-$TS}; LOG=$OUT/deploy.log; fi
mkdir -p "$OUT" || { echo "cannot create $OUT"; exit 1; }

say() { printf '%s\n' "$*" | tee -a "$LOG"; }
stamp() { date '+%H:%M:%S'; }
die() { say "### STOP at $(stamp): $*"; say "### nothing further was sent to the PC; log: $LOG"; exit "${2:-1}"; }
# dryfail: a check that cannot be evaluated in a dry run (no PC output, or an artefact not built yet) is reported, not fatal.
chk() { # chk <description> <command...>   (command run only when not DRYRUN for remote-dependent checks; use chkl for local ones)
  local d=$1; shift
  if "$@"; then say "  ok: $d"; else die "check failed: $d"; fi
}
chkl() { # local check: in DRYRUN a failure is a WARNING (the builder may still be building)
  local d=$1; shift
  if "$@"; then say "  ok: $d"; elif [ "$DRYRUN" = 1 ]; then say "  DRYRUN-WARN (would STOP): $d"; else die "check failed: $d"; fi
}
# px: run a command on the PC. Output -> $OUT/NN-label.txt and the log. rc returned. DRYRUN prints the command only.
px() { # px <label> <remote command string>
  STEPNO=$((STEPNO+1)); local f; f=$(printf '%s/%02d-%s.txt' "$OUT" "$STEPNO" "$1"); local c=$2
  say "--- [$STEPNO] $(stamp) ssh $HOST: $c"
  if [ "$DRYRUN" = 1 ]; then say "    DRYRUN: not run"; : >"$f"; return 0; fi
  # shellcheck disable=SC2086
  ssh $SSHO "$HOST" "$c" >"$f" 2>&1; local rc=$?
  sed 's/^/    | /' "$f" | tee -a "$LOG"
  say "    rc=$rc"
  return $rc
}
pxo() { # like px but the output is also left in $PXOUT
  px "$@"; local rc=$?; PXOUT=$(cat "$(printf '%s/%02d-%s.txt' "$OUT" "$STEPNO" "$1")" 2>/dev/null); return $rc
}
scp_to() { # scp_to <local path> <remote path> [-r]
  STEPNO=$((STEPNO+1)); local flag=${3:-}
  say "--- [$STEPNO] $(stamp) scp $flag $1 -> $HOST:$2"
  if [ "$DRYRUN" = 1 ]; then say "    DRYRUN: not run"; return 0; fi
  # shellcheck disable=SC2086
  scp -q $SSHO $flag "$1" "$HOST:$2" || die "scp of $1 failed"
}
reach() { [ "$DRYRUN" = 1 ] && return 0; ssh $SSHO "$HOST" true 2>/dev/null; }
nap() { [ "$DRYRUN" = 1 ] && return 0; sleep "$1"; }

say "### M6 Stage 1a DEPLOY $TS  dry-run=$DRYRUN  src=$SRC  host=$HOST  out=$OUT"
say "### host Mac: $(sw_vers -productVersion 2>/dev/null) $(uname -m); free disk: $(df -h "$ROOT" | tail -1 | awk '{print $4}')"

# ------------------------------------------------------------------ D0 host Mac preflight
say; say "=== D0 host Mac preflight (artefacts, versions, proofs INSIDE the binaries)"
KEXT=$SRC/src/navi48-bringup/$KEXT_BUILD/Navi48Bringup.kext
AUX=$SRC/tools/native/navi48accel/build/Navi48Accel.kext
BUNDLE_TAR=$SRC/tools/native/navi48metal/build/Navi48Metal.bundle.tar
CLI=$SRC/tools/pc/navi48test
NUB=$SRC/tools/native/n48nub; [ -x "$NUB" ] || NUB=$ROOT/tools/native/n48nub
VARIANT_PLIST=$SRC/variants/configs/$VARIANT.plist
LOCAL_VARIANT=$ROOT/variants/configs/$VARIANT.plist
say "  kext   $KEXT"; say "  aux    $AUX"; say "  bundle $BUNDLE_TAR"; say "  CLI    $CLI"; say "  nub    $NUB"; say "  config $VARIANT_PLIST"
pver() { /usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$1" 2>/dev/null; }
chkl "bring-up kext built at $KEXT_BUILD" test -d "$KEXT"
chkl "bring-up kext CFBundleVersion is $WANT_KEXT (got '$(pver "$KEXT/Contents/Info.plist")')" test "$(pver "$KEXT/Contents/Info.plist")" = "$WANT_KEXT"
chkl "bring-up kext is signed" test -f "$KEXT/Contents/_CodeSignature/CodeResources"
# A token only the 0.0.659 code contains (the m6stat verb's kernel half logs 'm6stat page'; the latch name is navi48-m6). /usr/bin/grep, never the interactive grep (ugrep wrapper).
n_kext=$(strings -a "$KEXT/Contents/MacOS/Navi48Bringup" 2>/dev/null | /usr/bin/grep -c 'm6stat page')
chkl "build proof: 'm6stat page' appears $n_kext times INSIDE the kext binary (want >= 1)" test "${n_kext:-0}" -ge 1
n_kext2=$(strings -a "$KEXT/Contents/MacOS/Navi48Bringup" 2>/dev/null | /usr/bin/grep -c 'navi48-m6')
chkl "build proof: boot-arg token 'navi48-m6' appears $n_kext2 times in the kext binary (want >= 1)" test "${n_kext2:-0}" -ge 1
chkl "aux kext CFBundleVersion is $WANT_AUX (got '$(pver "$AUX/Contents/Info.plist")')" test "$(pver "$AUX/Contents/Info.plist")" = "$WANT_AUX"
chkl "aux kext is signed" test -f "$AUX/Contents/_CodeSignature/CodeResources"
n_aux=$(strings -a "$AUX/Contents/MacOS/Navi48Accel" 2>/dev/null | /usr/bin/grep -c 'navi48-m6')
chkl "build proof: 'navi48-m6' appears $n_aux times in the aux binary (want >= 1; the R1 lift)" test "${n_aux:-0}" -ge 1
chkl "bundle tar exists" test -f "$BUNDLE_TAR"
BT=$(mktemp -d /private/tmp/m6s1a-bt.XXXXXX) || die "mktemp failed"
case "$BT" in /private/tmp/m6s1a-bt.*) ;; *) die "unexpected temp dir $BT" ;; esac
if [ -f "$BUNDLE_TAR" ] && tar -xf "$BUNDLE_TAR" -C "$BT" 2>/dev/null; then
  chkl "bundle CFBundleVersion is $WANT_BUNDLE (got '$(pver "$BT/Navi48Metal.bundle/Contents/Info.plist")')" test "$(pver "$BT/Navi48Metal.bundle/Contents/Info.plist")" = "$WANT_BUNDLE"
  n_b=$(strings -a "$BT"/Navi48Metal.bundle/Contents/MacOS/* 2>/dev/null | /usr/bin/grep -c 'Navi48,M6Surf')
  chkl "build proof: 'Navi48,M6Surf' appears $n_b times in the bundle binary (want >= 1)" test "${n_b:-0}" -ge 1
else say "  DRYRUN/UNPACK: bundle tar not unpacked (missing or unreadable)"; [ "$DRYRUN" = 1 ] || die "bundle tar unreadable"; fi
# prefix-checked removal of OUR temp dir only (reported):
case "$BT" in /private/tmp/m6s1a-bt.*) rm -rf "$BT"; say "  [rm] removed own temp dir $BT" ;; esac
chkl "CLI binary exists" test -x "$CLI"
n_cli=$(strings -a "$CLI" 2>/dev/null | /usr/bin/grep -c 'm6stat')
chkl "build proof: 'm6stat' appears $n_cli times in the CLI (want >= 1)" test "${n_cli:-0}" -ge 1
chkl "nub tool exists" test -x "$NUB"
chkl "variant plist exists ($VARIANT_PLIST)" test -f "$VARIANT_PLIST"
va=$(plutil -extract NVRAM.Add.7C436110-AB2A-4BBB-A880-FE41995C9F82.boot-args raw -o - "$VARIANT_PLIST" 2>/dev/null)
msapps=$(plutil -extract NVRAM.Add.7C436110-AB2A-4BBB-A880-FE41995C9F82.boot-args raw -o - "$ROOT/variants/configs/stage17-native-1440-metal-disp-amfi-ms-apps.plist" 2>/dev/null)
say "  variant boot-args: $va"
chkl "variant boot-args == ms-apps boot-args + ' navi48-fb2=1 navi48-m6=1' (nothing else)" test "$va" = "$msapps navi48-fb2=1 navi48-m6=1"

# ------------------------------------------------------------------ D1 PC orientation
say; say "=== D1 PC orientation (read-only)"
reach || die "PC ($HOST) not reachable - is it on and booted into macOS? (one check only; stop rule 8)" 3
pxo orient 'echo "up: $(uptime)"; echo "boottime: $(sysctl -n kern.boottime)"; echo "boot-args: $(sysctl -n kern.bootargs)"; echo "console: $(stat -f %Su /dev/console)"; echo "who:"; who; echo "loaded:"; kmutil showloaded --list-only 2>/dev/null | grep -iE "navi48|rdna4|accelprobe"; echo "ESP:"; ~/navi48-staging/esp-kext.sh show 2>&1; echo "esp-config:"; ~/navi48-staging/esp-config.sh show 2>&1; echo "extensions holding the aux id:"; grep -l com.navi48.accelprobe /Library/Extensions/*/Contents/Info.plist 2>/dev/null; echo "GPUBundles:"; ls -l /Library/GPUBundles 2>&1; echo "panics:"; ls -lt /Library/Logs/DiagnosticReports 2>&1 | head -5' || die "orientation failed"
if [ "$DRYRUN" != 1 ]; then
  echo "$PXOUT" | /usr/bin/grep -q '^console: root$' || die "a console user is logged in (not root): user logged in - ask them to log out"
  echo "$PXOUT" | sed -n '/^who:/,/^loaded:/p' | sed '1d;$d' | /usr/bin/grep -q . && die "who lists a logged-in user: user logged in - ask them to log out"
  say "  ok: 0 console users"
fi
PRE_BOOT=$(echo "$PXOUT" | sed -n 's/^boottime: //p')
PRE_PANIC=$(echo "$PXOUT" | sed -n '/^panics:/,$p' | sed -n '2p')
say "  recorded: boottime='$PRE_BOOT' newest report='$PRE_PANIC'"

# ------------------------------------------------------------------ D2 stage
say; say "=== D2 stage to ~/navi48-staging (old files MOVED aside, never deleted)"
px mkbackup 'mkdir -p ~/navi48-staging/backup ~/navi48-staging/configs ~/n48-metal' || die "mkdir failed"
px aside "set -e; cd ~/navi48-staging; for f in Navi48Bringup.kext Navi48Accel.kext navi48test; do if [ -e \$f ]; then mv \$f backup/\$f.$TS; echo moved \$f; fi; done" || die "moving old staged files aside failed"
scp_to "$KEXT" '~/navi48-staging/' -r
scp_to "$AUX" '~/navi48-staging/' -r
scp_to "$CLI" '~/navi48-staging/navi48test'
px chmodcli 'chmod +x ~/navi48-staging/navi48test' || die "chmod failed"
scp_to "$VARIANT_PLIST" "~/navi48-staging/configs/$VARIANT.plist"
scp_to "$BUNDLE_TAR" '~/n48-metal/Navi48Metal.bundle.tar'
pxo nubtool 'if [ -x ~/n48-metal/n48nub ]; then echo "n48nub present"; else echo "n48nub MISSING"; fi' || die "nub check failed"
if echo "$PXOUT" | grep -q MISSING; then scp_to "$NUB" '~/n48-metal/n48nub'; px chmodnub 'chmod +x ~/n48-metal/n48nub' || die "chmod nub failed"; fi
pxo stagedver "/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' ~/navi48-staging/Navi48Bringup.kext/Contents/Info.plist; /usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' ~/navi48-staging/Navi48Accel.kext/Contents/Info.plist; codesign --verify --strict ~/navi48-staging/Navi48Bringup.kext 2>&1 && echo kext-codesign-ok; strings -a ~/navi48-staging/Navi48Bringup.kext/Contents/MacOS/Navi48Bringup | grep -c 'm6stat page'" || die "staged version read failed"
[ "$DRYRUN" = 1 ] || { echo "$PXOUT" | sed -n 1p | grep -qx "$WANT_KEXT" && echo "$PXOUT" | sed -n 2p | grep -qx "$WANT_AUX" && echo "$PXOUT" | grep -q kext-codesign-ok || die "staged files on the PC are not 0.0.659 / 0.0.7 / signed"; }

# ------------------------------------------------------------------ D3 ESP
say; say "=== D3 bring-up kext onto the ESP (OpenCore injects it)"
px espinstall 'sudo -n ~/navi48-staging/esp-kext.sh install' || die "esp-kext.sh install failed (ESP full? OpenCore logs fill the 200 MB ESP: archive, never delete, to ~/esp-oc-logs-archive on the PC)"
pxo espshow '~/navi48-staging/esp-kext.sh show' || die "esp-kext.sh show failed"
[ "$DRYRUN" = 1 ] || echo "$PXOUT" | grep -q "injected kext: $WANT_KEXT" || die "ESP does not name $WANT_KEXT - NOT rebooting on an unverified ESP"
say "  ESP names $WANT_KEXT (version gate, pc-protocol 2)"

# ------------------------------------------------------------------ D4 aux 0.0.7 at a NEW path
say; say "=== D4 aux $WANT_AUX at the NEW path $AUX_DIR (step 4b; one refused kmutil load; NEVER kmutil install --update-all)"
pxo auxpre "test ! -e $AUX_DIR && echo 'new path is free' || echo 'NEW PATH ALREADY EXISTS'; grep -l com.navi48.accelprobe /Library/Extensions/*/Contents/Info.plist 2>/dev/null" || true
[ "$DRYRUN" = 1 ] || { echo "$PXOUT" | grep -q 'new path is free' || die "$AUX_DIR already exists - a path is never reused (same-path swaps are ignored at boot)"; }
px auxkc "set -e; sudo -n mkdir -p ~/navi48-staging/backup/auxkc-pre-$WANT_AUX; sudo -n cp -Rp /Library/KernelCollections/AuxiliaryKernelExtensions.kc ~/navi48-staging/backup/auxkc-pre-$WANT_AUX/; ls -l ~/navi48-staging/backup/auxkc-pre-$WANT_AUX/" || die "aux KC backup failed"
# Move every bundle that holds the id aside (exactly ONE bundle with the id may remain in /Library/Extensions: the new one).
px auxmove "set -e; for p in \$(grep -l com.navi48.accelprobe /Library/Extensions/*/Contents/Info.plist 2>/dev/null | sed 's#/Contents/Info.plist##'); do echo moving \$p; sudo -n mv \$p ~/navi48-staging/backup/\$(basename \$p).aux-pre-$WANT_AUX.$TS; done; echo remaining:; grep -l com.navi48.accelprobe /Library/Extensions/*/Contents/Info.plist 2>/dev/null || echo '(none)'" || die "moving the old aux bundle aside failed"
px auxcopy "set -e; sudo -n cp -R ~/navi48-staging/Navi48Accel.kext $AUX_DIR; sudo -n chown -R root:wheel $AUX_DIR; sudo -n chmod -R go-w $AUX_DIR; sudo -n xattr -cr $AUX_DIR; /usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' $AUX_DIR/Contents/Info.plist" || die "aux copy failed"
pxo auxlibs "kmutil libraries -p $AUX_DIR -a x86_64 2>&1 | head -30" || die "kmutil libraries failed"
[ "$DRYRUN" = 1 ] || { ! echo "$PXOUT" | grep -qiE 'unresolved|not found|error' || die "kmutil libraries reports unresolved symbols/libraries (see the file above)"; }
say "  ONE kmutil load: 'not approved' is EXPECTED (it only creates this boot's history row). It is never retried."
px auxload "sudo -n kmutil load -p $AUX_DIR 2>&1; echo kmutil-load-rc=\$?" || true     # nonzero is expected
say "  (the aux KC is rebuilt at the reboot below; same id as before, new path: no Allow click expected)"

# ------------------------------------------------------------------ D5 bundle 11
say; say "=== D5 Metal bundle $WANT_BUNDLE into /Library/GPUBundles (old one moved aside; no reboot needed for it)"
px bundle "set -e; cd ~/n48-metal; mkdir ~/n48-metal/unpack.$TS; tar -xf Navi48Metal.bundle.tar -C ~/n48-metal/unpack.$TS; sudo -n mkdir -p /Library/GPUBundles; if [ -d /Library/GPUBundles/Navi48Metal.bundle ]; then sudo -n mv /Library/GPUBundles/Navi48Metal.bundle ~/navi48-staging/backup/Navi48Metal.bundle.pre11.$TS; fi; sudo -n cp -R ~/n48-metal/unpack.$TS/Navi48Metal.bundle /Library/GPUBundles/Navi48Metal.bundle; sudo -n chown -R root:wheel /Library/GPUBundles/Navi48Metal.bundle; sudo -n xattr -cr /Library/GPUBundles/Navi48Metal.bundle; codesign --verify --strict -v /Library/GPUBundles/Navi48Metal.bundle 2>&1; /usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' /Library/GPUBundles/Navi48Metal.bundle/Contents/Info.plist" || die "bundle install failed"
# (no PC-side deletion anywhere in this script: the unpack directory ~/n48-metal/unpack.<ts> is left in place for the reviewer)
[ "$DRYRUN" = 1 ] || { tail -1 "$(printf '%s/%02d-bundle.txt' "$OUT" "$STEPNO")" | grep -qx "$WANT_BUNDLE" || die "installed bundle is not CFBundleVersion $WANT_BUNDLE"; }

# ------------------------------------------------------------------ D6 variant
say; say "=== D6 boot variant $VARIANT (stale-variant check first, pc-protocol 3)"
pxo variantcheck "echo LIVE:; sysctl -n kern.bootargs; echo TARGET:; plutil -extract NVRAM.Add.7C436110-AB2A-4BBB-A880-FE41995C9F82.boot-args raw -o - ~/navi48-staging/configs/$VARIANT.plist" || die "variant read failed"
if [ "$DRYRUN" != 1 ]; then
  live=$(echo "$PXOUT" | sed -n '/^LIVE:/{n;p;}'); tgt=$(echo "$PXOUT" | sed -n '/^TARGET:/{n;p;}')
  say "  live  : $live"; say "  target: $tgt"
  missing=""; for t in $live; do case " $tgt " in *" $t "*) ;; *) missing="$missing $t" ;; esac; done
  added=""; for t in $tgt; do case " $live " in *" $t "*) ;; *) added="$added $t" ;; esac; done
  say "  in live NVRAM but NOT in the target (must be empty): '$missing'"
  say "  in the target but not live (expected: navi48-m6=1 and, if the PC is not on an fb2 boot, navi48-fb2=1, navi48-metal-ws=1, navi48-multisession=1, navi48-apps=1): '$added'"
  [ -z "$missing" ] || die "the target variant drops live boot-args:$missing - a stale/wrong baseline (pc-protocol 3); fix the variant, do not reboot"
  case "$added" in *navi48-m6=1*) ;; *) die "the target variant does not add navi48-m6=1" ;; esac
fi
px esconfig "sudo -n ~/navi48-staging/esp-config.sh $VARIANT" || die "esp-config.sh failed"
px esconfigshow '~/navi48-staging/esp-config.sh show' || die "esp-config show failed"
[ "$DRYRUN" = 1 ] || grep -q 'navi48-m6=1' "$(printf '%s/%02d-esconfigshow.txt' "$OUT" "$STEPNO")" || die "live ESP config does not carry navi48-m6=1 after esp-config.sh"

if [ "${SKIP_REBOOT:-0}" = 1 ]; then say "### SKIP_REBOOT=1: stopping after D6; nothing rebooted"; exit 0; fi

# ------------------------------------------------------------------ D7 reboot + checks
say; say "=== D7 reboot through reboot-pc.sh only (gates), then the stop rules, then the post-boot checks"
px rebootcheck '~/navi48-staging/reboot-pc.sh --check' || die "reboot-pc.sh --check refused (exit 3 = a user is logged in, exit 4 = U-state process): STOP and report the snapshot above; nothing was rebooted" 4
pxo espshow2 '~/navi48-staging/esp-kext.sh show' || die "esp show failed"
[ "$DRYRUN" = 1 ] || echo "$PXOUT" | grep -q "injected kext: $WANT_KEXT" || die "ESP no longer names $WANT_KEXT - not rebooting"
say "  issuing the reboot at $(stamp) (graceful; reboot-pc.sh repeats its gates)"
px reboot '~/navi48-staging/reboot-pc.sh' || say "  (the ssh session dropping with a nonzero rc is normal for a reboot; the checks below decide)"
# down within 180 s
if [ "$DRYRUN" != 1 ]; then
  n=0; while [ $n -lt 180 ]; do ssh $SSHO "$HOST" true 2>/dev/null || break; sleep 5; n=$((n+5)); done
  if [ $n -ge 180 ]; then die "the PC still answers ssh 180 s after the reboot command: the reboot did not take. PC needs a manual power-cycle (send nothing more; no second reboot, no --quick)" 5; fi
  say "  ssh went away after ~${n}s"
else say "  DRYRUN: would wait up to 180 s for ssh to drop"; fi
say "  waiting for the DRIVER (not sshd): tools/pc/wait-for-driver.sh 300"
if [ "$DRYRUN" = 1 ]; then say "    DRYRUN: $ROOT/tools/pc/wait-for-driver.sh 300"; else
  "$ROOT/tools/pc/wait-for-driver.sh" 300 2>&1 | tee -a "$LOG"; wrc=${PIPESTATUS[0]}
  if [ "$wrc" != 0 ]; then
    if reach; then die "the PC answers ssh but the driver never became ready: investigate (esp-kext.sh show, boot-args, kmutil showloaded, newest DiagnosticReports); do NOT reboot again" 6
    else die "PC needs a manual reset (unreachable after wait-for-driver timeout and one more check). Last command sent: reboot-pc.sh; kext $WANT_KEXT; variant $VARIANT" 3; fi
  fi
fi
pxo postboot 'echo "boottime: $(sysctl -n kern.boottime)"; echo "boot-args: $(sysctl -n kern.bootargs)"; echo "console: $(stat -f %Su /dev/console)"; echo "who:"; who; echo "loaded:"; kmutil showloaded --list-only 2>/dev/null | grep -iE "navi48|rdna4|accelprobe"; echo "auxkc:"; kmutil inspect -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>&1 | grep -i accelprobe; echo "bundle:"; /usr/libexec/PlistBuddy -c "Print :CFBundleVersion" /Library/GPUBundles/Navi48Metal.bundle/Contents/Info.plist; echo "ESP:"; ~/navi48-staging/esp-kext.sh show 2>&1; echo "m6stat:"; sudo -n ~/navi48-staging/navi48test accel m6stat 0 2>&1; echo "panics:"; ls -lt /Library/Logs/DiagnosticReports 2>&1 | head -5' || die "post-boot read failed"
if [ "$DRYRUN" != 1 ]; then
  post_boot=$(echo "$PXOUT" | sed -n 's/^boottime: //p')
  [ -n "$post_boot" ] && [ "$post_boot" != "$PRE_BOOT" ] || die "kern.boottime did not change ($PRE_BOOT): this is not a new boot"
  echo "$PXOUT" | /usr/bin/grep -q 'navi48-m6=1' || die "kern.bootargs does not carry navi48-m6=1 - the m6 variant is not live"
  echo "$PXOUT" | /usr/bin/grep -qE "com.navi48.bringup.*$WANT_KEXT|Navi48Bringup.*$WANT_KEXT" || die "kmutil showloaded does not show the bring-up kext at $WANT_KEXT (known skew: kmod_info.c shows 0.0.15 - see navi48-deploy reference)"
  echo "$PXOUT" | /usr/bin/grep -q "$WANT_AUX" || die "no accelprobe $WANT_AUX in showloaded / aux KC"
  echo "$PXOUT" | /usr/bin/grep -q "Navi48Accel-$WANT_AUX.kext" || die "the aux KC does not name the new path Navi48Accel-$WANT_AUX.kext"
  echo "$PXOUT" | sed -n '/^bundle:/{n;p;}' | /usr/bin/grep -qx "$WANT_BUNDLE" || die "bundle version on the PC is not $WANT_BUNDLE"
  echo "$PXOUT" | /usr/bin/grep -q 'navi48-m6 latch ON' || die "m6stat does not report 'navi48-m6 latch ON' (kext and CLI disagree or the latch did not take)"
  newest=$(echo "$PXOUT" | sed -n '/^panics:/,$p' | sed -n '2p')
  [ "$newest" = "$PRE_PANIC" ] || say "  WARNING: the newest DiagnosticReports entry changed across the reboot: '$newest' (was '$PRE_PANIC'): read it before the run (pc-protocol 7)"
fi
say; say "### DEPLOY COMPLETE $(stamp): kext $WANT_KEXT (ESP), aux $WANT_AUX at $AUX_DIR, bundle $WANT_BUNDLE, variant $VARIANT, latch navi48-m6=1. PC is at the login window, 0 users, NOT armed."
say "### next: tools/pc/m6-stage1a-run.sh"
exit 0
