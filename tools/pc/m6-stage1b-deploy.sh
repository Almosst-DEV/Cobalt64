#!/bin/bash
# m6-stage1b-deploy.sh - DEPLOY PLAN for M6 Stage 1b (first GPU pixels on the monitor B): bring-up 0.0.661 + Metal bundle 13 (with the Mesa dylib inside the bundle tar) + variant
# stage17-native-1440-metal-disp-amfi-m6flip (latches navi48-m6=1 navi48-m6flip=1). The aux kext stays 0.0.7 (already installed at /Library/Extensions/Navi48Accel-0.0.7.kext): NO aux step, only a check.
# RUN ON THE AIR. Contacts the PC (host alias `navi48`) ONLY when the brief grants PC operation; follow .claude/skills/navi48-brief-agent/pc-protocol.md. `DRYRUN=1` prints every ssh/scp command and runs none.
# Derived from tools/pc/m6-stage1a-deploy.sh (committed cd79ca1d..87bf2d52) and the build contract an internal design note "## Q2" item 13.
# UNTRACKED (not committed) until the reviewer says so.
#
# WHAT IT DOES, in order (every step logs; the FIRST failure stops the script, nothing is retried):
#   D0  host Mac preflight: artefacts exist, versions 0.0.661 / bundle 13, signed, build-proof tokens INSIDE the binaries (kext 'navi48-m6flip' + 'GPU-HELD', CLI 'm6xstat', bundle 'n48m-noflip2',
#       the Mesa dylib in the bundle tar exports _radv_darwin_n48n_call and _radv_darwin_n48n_bo_handle (nm)), the variant's boot-args are the 1a variant's + navi48-m6flip=1 (nothing else).
#   D1  PC orientation (read-only): reachable, 0 console users, uptime, boot-args, loaded kexts, ESP kext, aux KC, newest DiagnosticReports.
#   D2  stage to ~/navi48-staging (old files MOVED aside into ~/navi48-staging/backup/, never deleted): the bring-up kext (build-armg), the CLI, the variant plist, the bundle tar. (No aux, no nub tool unless missing.)
#   D3  bring-up kext 0.0.661 onto the ESP (esp-kext.sh install); `esp-kext.sh show` must name 0.0.661 BEFORE any reboot.
#   D4  aux CHECK ONLY: /Library/Extensions/Navi48Accel-0.0.7.kext exists with CFBundleVersion 0.0.7 and is the only bundle holding com.navi48.accelprobe; the aux KC names it. Nothing is changed.
#   D5  Metal bundle 13 into /Library/GPUBundles (the old one MOVED aside, no reboot needed for it); the installed dylib's sha256 must equal the host Mac's.
#   D6  boot variant stage17-native-1440-metal-disp-amfi-m6flip via esp-config.sh, after the stale-variant check (pc-protocol 3): the live boot-args (the 1a m6 variant) must all be in the variant; the ONLY addition must be navi48-m6flip=1.
#   D7  reboot ONLY through reboot-pc.sh (gates: 0 users, no U-state), then the pc-protocol 8 stop rules (down within 180 s, wait-for-driver 300 s, boottime changed), then the post-boot checks
#       (kext 0.0.661, aux 0.0.7 at its path, bundle 13, boot-args with navi48-m6flip=1, m6stat latch ON, m6xstat 'navi48-m6 ON, navi48-m6flip ON').
#
# STOP RULES (pc-protocol 8): PC unreachable after one more check -> "PC needs a manual reset"; a reboot that does not take within bounds -> "PC needs a manual power-cycle"; reboot-pc.sh refusal (exit 3/4)
# -> STOP and report its snapshot; two panics in a row on this build -> stop (the script does not loop; it exits 1).
#
# ROLLBACK (documented here, NOT automated): reboot into the 1a variant on the 1a build:
#   ssh navi48 '~/navi48-staging/esp-config.sh stage17-native-1440-metal-disp-amfi-m6'          # boot-args without navi48-m6flip
#   (the previous kext 0.0.659/0.0.660 is the file this script moved to ~/navi48-staging/backup/Navi48Bringup.kext.<ts>; esp-kext.sh install needs it staged as ~/navi48-staging/Navi48Bringup.kext;
#    the previous bundle is ~/navi48-staging/backup/Navi48Metal.bundle.pre13.<ts>.)  With navi48-m6flip absent every instance-2 selector returns Unsupported and 0.0.661 behaves as 0.0.660.
#   then ~/navi48-staging/reboot-pc.sh (0 users; it gates).
#
# LOGS: notes/logs/runs/m6-s1b/<timestamp>-deploy/ (gitignored: notes/logs/runs).
# Env: DRYRUN=1  N48_SRC=<tree holding the built artefacts, default the main repo; the brief uses ~/navi48-native/wt-s1>  N48_HOST=navi48  KEXT_BUILD=build-armg  SKIP_REBOOT=1 (stop after D6)

set -u
set -o pipefail
ROOT=${N48_ROOT:-/path/to/navi48-checkout.nosync}
SRC=${N48_SRC:-$ROOT}
HOST=${N48_HOST:-navi48}
DRYRUN=${DRYRUN:-0}
KEXT_BUILD=${KEXT_BUILD:-build-armg}
VARIANT=stage17-native-1440-metal-disp-amfi-m6flip
VARIANT_1A=stage17-native-1440-metal-disp-amfi-m6
WANT_KEXT=0.0.661
WANT_AUX=0.0.7
WANT_BUNDLE=13
AUX_DIR=/Library/Extensions/Navi48Accel-$WANT_AUX.kext
TS=$(date '+%Y%m%d-%H%M%S')
OUT=$ROOT/notes/logs/runs/m6-s1b/$TS-deploy
LOG=$OUT/deploy.log
SSHO="-o BatchMode=yes -o ConnectTimeout=8"
STEPNO=0

if [ "$DRYRUN" = 1 ]; then OUT=${DRYRUN_OUT:-/private/tmp/claude-501/m6-s1b-deploy-dry-$TS}; LOG=$OUT/deploy.log; fi
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

say "### M6 Stage 1b DEPLOY $TS  dry-run=$DRYRUN  src=$SRC  host=$HOST  out=$OUT"
say "### host Mac: $(sw_vers -productVersion 2>/dev/null) $(uname -m); free disk: $(df -h "$ROOT" | tail -1 | awk '{print $4}')"

# ------------------------------------------------------------------ D0 host Mac preflight
say; say "=== D0 host Mac preflight (artefacts, versions, proofs INSIDE the binaries)"
KEXT=$SRC/src/navi48-bringup/$KEXT_BUILD/Navi48Bringup.kext
BUNDLE_TAR=$SRC/tools/native/navi48metal/build/Navi48Metal.bundle.tar
CLI=$SRC/tools/pc/navi48test
NUB=$SRC/tools/native/n48nub; [ -x "$NUB" ] || NUB=$ROOT/tools/native/n48nub
VARIANT_PLIST=$SRC/variants/configs/$VARIANT.plist
LOCAL_VARIANT=$ROOT/variants/configs/$VARIANT.plist
say "  kext   $KEXT"; say "  aux    (unchanged, 0.0.7: checked on the PC only)"; say "  bundle $BUNDLE_TAR"; say "  CLI    $CLI"; say "  nub    $NUB"; say "  config $VARIANT_PLIST"
pver() { /usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$1" 2>/dev/null; }
chkl "bring-up kext built at $KEXT_BUILD" test -d "$KEXT"
chkl "bring-up kext CFBundleVersion is $WANT_KEXT (got '$(pver "$KEXT/Contents/Info.plist")')" test "$(pver "$KEXT/Contents/Info.plist")" = "$WANT_KEXT"
chkl "bring-up kext is signed" test -f "$KEXT/Contents/_CodeSignature/CodeResources"
# Tokens only the 0.0.661 code contains: 'navi48-m6flip' (the second latch) and 'GPU-HELD' (disp2 status/crc report GPU-held, contract item 11). /usr/bin/grep, never the interactive grep (ugrep wrapper).
n_kext=$(strings -a "$KEXT/Contents/MacOS/Navi48Bringup" 2>/dev/null | /usr/bin/grep -c 'navi48-m6flip')
chkl "build proof: 'navi48-m6flip' appears $n_kext times INSIDE the kext binary (want >= 1)" test "${n_kext:-0}" -ge 1
n_kext2=$(strings -a "$KEXT/Contents/MacOS/Navi48Bringup" 2>/dev/null | /usr/bin/grep -c 'GPU-HELD')
chkl "build proof: 'GPU-HELD' appears $n_kext2 times in the kext binary (want >= 1)" test "${n_kext2:-0}" -ge 1
n_kext3=$(strings -a "$KEXT/Contents/MacOS/Navi48Bringup" 2>/dev/null | /usr/bin/grep -c 'm6stat page')
chkl "build proof: 'm6stat page' (0.0.659+ code) appears $n_kext3 times in the kext binary (want >= 1)" test "${n_kext3:-0}" -ge 1
chkl "bundle tar exists" test -f "$BUNDLE_TAR"
BT=$(mktemp -d /private/tmp/claude-501/m6s1b-bt.XXXXXX) || die "mktemp failed"
case "$BT" in /private/tmp/claude-501/m6s1b-bt.*) ;; *) die "unexpected temp dir $BT" ;; esac
if [ -f "$BUNDLE_TAR" ] && tar -xf "$BUNDLE_TAR" -C "$BT" 2>/dev/null; then
  chkl "bundle CFBundleVersion is $WANT_BUNDLE (got '$(pver "$BT/Navi48Metal.bundle/Contents/Info.plist")')" test "$(pver "$BT/Navi48Metal.bundle/Contents/Info.plist")" = "$WANT_BUNDLE"
  n_b=$(strings -a "$BT"/Navi48Metal.bundle/Contents/MacOS/* 2>/dev/null | /usr/bin/grep -c 'n48m-noflip2')
  chkl "build proof: 'n48m-noflip2' (bundle 13's kill-switch file) appears $n_b times in the bundle binary (want >= 1)" test "${n_b:-0}" -ge 1
  DYLIB=$BT/Navi48Metal.bundle/Contents/Resources/libvulkan_radeon.dylib
  chkl "Mesa dylib is inside the bundle tar" test -f "$DYLIB"
  n_d=$(nm -gU "$DYLIB" 2>/dev/null | /usr/bin/grep -cE ' T _radv_darwin_n48n_(call|bo_handle)$')
  chkl "build proof: the dylib exports _radv_darwin_n48n_call AND _radv_darwin_n48n_bo_handle ($n_d of 2; nm, because strings does not list Mach-O symbol names here)" test "${n_d:-0}" -eq 2
  DYLIB_SHA=$(shasum -a 256 "$DYLIB" 2>/dev/null | awk '{print $1}')
  say "  host Mac dylib sha256: $DYLIB_SHA (D5 compares the PC's installed copy with it)"
else say "  DRYRUN/UNPACK: bundle tar not unpacked (missing or unreadable)"; [ "$DRYRUN" = 1 ] || die "bundle tar unreadable"; fi
# prefix-checked removal of OUR temp dir only (reported):
case "$BT" in /private/tmp/claude-501/m6s1b-bt.*) rm -rf "$BT"; say "  [rm] removed own temp dir $BT" ;; esac
chkl "CLI binary exists" test -x "$CLI"
n_cli=$(strings -a "$CLI" 2>/dev/null | /usr/bin/grep -c 'm6xstat')
chkl "build proof: 'm6xstat' appears $n_cli times in the CLI (want >= 1; the new verb, action 107)" test "${n_cli:-0}" -ge 1
chkl "nub tool exists" test -x "$NUB"
chkl "variant plist exists ($VARIANT_PLIST)" test -f "$VARIANT_PLIST"
va=$(plutil -extract NVRAM.Add.7C436110-AB2A-4BBB-A880-FE41995C9F82.boot-args raw -o - "$VARIANT_PLIST" 2>/dev/null)
v1a=$(plutil -extract NVRAM.Add.7C436110-AB2A-4BBB-A880-FE41995C9F82.boot-args raw -o - "$ROOT/variants/configs/$VARIANT_1A.plist" 2>/dev/null)
say "  variant boot-args: $va"
chkl "variant boot-args == the 1a variant's ($VARIANT_1A) + ' navi48-m6flip=1' (nothing else)" test -n "$v1a" -a "$va" = "$v1a navi48-m6flip=1"

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
px aside "set -e; cd ~/navi48-staging; for f in Navi48Bringup.kext navi48test; do if [ -e \$f ]; then mv \$f backup/\$f.$TS; echo moved \$f; fi; done" || die "moving old staged files aside failed"
scp_to "$KEXT" '~/navi48-staging/' -r
scp_to "$CLI" '~/navi48-staging/navi48test'
px chmodcli 'chmod +x ~/navi48-staging/navi48test' || die "chmod failed"
scp_to "$VARIANT_PLIST" "~/navi48-staging/configs/$VARIANT.plist"
scp_to "$BUNDLE_TAR" '~/n48-metal/Navi48Metal.bundle.tar'
pxo nubtool 'if [ -x ~/n48-metal/n48nub ]; then echo "n48nub present"; else echo "n48nub MISSING"; fi' || die "nub check failed"
if echo "$PXOUT" | grep -q MISSING; then scp_to "$NUB" '~/n48-metal/n48nub'; px chmodnub 'chmod +x ~/n48-metal/n48nub' || die "chmod nub failed"; fi
pxo stagedver "/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' ~/navi48-staging/Navi48Bringup.kext/Contents/Info.plist; codesign --verify --strict ~/navi48-staging/Navi48Bringup.kext 2>&1 && echo kext-codesign-ok; strings -a ~/navi48-staging/Navi48Bringup.kext/Contents/MacOS/Navi48Bringup | grep -c 'navi48-m6flip'" || die "staged version read failed"
[ "$DRYRUN" = 1 ] || { echo "$PXOUT" | sed -n 1p | grep -qx "$WANT_KEXT" && echo "$PXOUT" | grep -q kext-codesign-ok && [ "$(echo "$PXOUT" | tail -1)" -ge 1 ] || die "staged kext on the PC is not $WANT_KEXT / signed / carrying navi48-m6flip"; }

# ------------------------------------------------------------------ D3 ESP
say; say "=== D3 bring-up kext onto the ESP (OpenCore injects it)"
px espinstall 'sudo -n ~/navi48-staging/esp-kext.sh install' || die "esp-kext.sh install failed (ESP full? OpenCore logs fill the 200 MB ESP: archive, never delete, to ~/esp-oc-logs-archive on the PC)"
pxo espshow '~/navi48-staging/esp-kext.sh show' || die "esp-kext.sh show failed"
[ "$DRYRUN" = 1 ] || echo "$PXOUT" | grep -q "injected kext: $WANT_KEXT" || die "ESP does not name $WANT_KEXT - NOT rebooting on an unverified ESP"
say "  ESP names $WANT_KEXT (version gate, pc-protocol 2)"

# ------------------------------------------------------------------ D4 aux CHECK ONLY
say; say "=== D4 aux $WANT_AUX is still installed at $AUX_DIR (CHECK ONLY: nothing is changed, no kmutil)"
pxo auxcheck "echo version: \$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' $AUX_DIR/Contents/Info.plist 2>&1); echo holders:; grep -l com.navi48.accelprobe /Library/Extensions/*/Contents/Info.plist 2>/dev/null; echo auxkc:; kmutil inspect -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>&1 | grep -i accelprobe" || die "aux check failed"
if [ "$DRYRUN" != 1 ]; then
  echo "$PXOUT" | /usr/bin/grep -qx "version: $WANT_AUX" || die "the aux at $AUX_DIR is not $WANT_AUX (read above): the 1a deploy has not been done on this PC, or the path differs"
  [ "$(echo "$PXOUT" | sed -n '/^holders:/,/^auxkc:/p' | /usr/bin/grep -c 'Info.plist')" = 1 ] || die "not exactly ONE bundle in /Library/Extensions holds com.navi48.accelprobe (see above)"
  echo "$PXOUT" | sed -n '/^holders:/,/^auxkc:/p' | /usr/bin/grep -q "Navi48Accel-$WANT_AUX.kext" || die "the one holder is not Navi48Accel-$WANT_AUX.kext"
  echo "$PXOUT" | sed -n '/^auxkc:/,$p' | /usr/bin/grep -qE "accelprobe.*$WANT_AUX|$WANT_AUX.*accelprobe" || die "the aux KC does not carry accelprobe $WANT_AUX"
fi
say "  ok: aux $WANT_AUX at its path, the only holder, named by the aux KC"

# ------------------------------------------------------------------ D5 bundle 13 (+ the Mesa dylib inside it)
say; say "=== D5 Metal bundle $WANT_BUNDLE (with the Mesa dylib) into /Library/GPUBundles (old one moved aside; no reboot needed for it)"
px bundle "set -e; cd ~/n48-metal; mkdir ~/n48-metal/unpack.$TS; tar -xf Navi48Metal.bundle.tar -C ~/n48-metal/unpack.$TS; sudo -n mkdir -p /Library/GPUBundles; if [ -d /Library/GPUBundles/Navi48Metal.bundle ]; then sudo -n mv /Library/GPUBundles/Navi48Metal.bundle ~/navi48-staging/backup/Navi48Metal.bundle.pre13.$TS; fi; sudo -n cp -R ~/n48-metal/unpack.$TS/Navi48Metal.bundle /Library/GPUBundles/Navi48Metal.bundle; sudo -n chown -R root:wheel /Library/GPUBundles/Navi48Metal.bundle; sudo -n xattr -cr /Library/GPUBundles/Navi48Metal.bundle; codesign --verify --strict -v /Library/GPUBundles/Navi48Metal.bundle 2>&1; /usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' /Library/GPUBundles/Navi48Metal.bundle/Contents/Info.plist" || die "bundle install failed"
# (no PC-side deletion anywhere in this script: the unpack directory ~/n48-metal/unpack.<ts> is left in place for the reviewer)
[ "$DRYRUN" = 1 ] || { tail -1 "$(printf '%s/%02d-bundle.txt' "$OUT" "$STEPNO")" | grep -qx "$WANT_BUNDLE" || die "installed bundle is not CFBundleVersion $WANT_BUNDLE"; }
pxo dylibsha 'shasum -a 256 /Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/libvulkan_radeon.dylib' || die "dylib sha read failed"
[ "$DRYRUN" = 1 ] || { [ -n "${DYLIB_SHA:-}" ] && echo "$PXOUT" | /usr/bin/grep -q "^$DYLIB_SHA " || die "the installed Mesa dylib's sha256 differs from the host Mac's ($DYLIB_SHA): the dylib swap did not take"; }
say "  ok: the installed Mesa dylib is the host Mac's (sha256 $DYLIB_SHA)"

# ------------------------------------------------------------------ D6 variant
say; say "=== D6 boot variant $VARIANT (stale-variant check first, pc-protocol 3)"
pxo variantcheck "echo LIVE:; sysctl -n kern.bootargs; echo TARGET:; plutil -extract NVRAM.Add.7C436110-AB2A-4BBB-A880-FE41995C9F82.boot-args raw -o - ~/navi48-staging/configs/$VARIANT.plist" || die "variant read failed"
if [ "$DRYRUN" != 1 ]; then
  live=$(echo "$PXOUT" | sed -n '/^LIVE:/{n;p;}'); tgt=$(echo "$PXOUT" | sed -n '/^TARGET:/{n;p;}')
  say "  live  : $live"; say "  target: $tgt"
  missing=""; for t in $live; do case " $tgt " in *" $t "*) ;; *) missing="$missing $t" ;; esac; done
  added=""; for t in $tgt; do case " $live " in *" $t "*) ;; *) added="$added $t" ;; esac; done
  say "  in live NVRAM but NOT in the target (must be empty): '$missing'"
  say "  in the target but not live (expected: exactly navi48-m6flip=1, the PC being on the 1a m6 boot): '$added'"
  [ -z "$missing" ] || die "the target variant drops live boot-args:$missing - a stale/wrong baseline (pc-protocol 3); fix the variant, do not reboot"
  case "$added" in *navi48-m6flip=1*) ;; *) die "the target variant does not add navi48-m6flip=1" ;; esac
  [ "$(echo $added)" = "navi48-m6flip=1" ] || die "the target adds more than navi48-m6flip=1 ('$added'): the PC is not on the 1a m6 boot, or the variant is wrong - fix it, do not reboot"
fi
px esconfig "sudo -n ~/navi48-staging/esp-config.sh $VARIANT" || die "esp-config.sh failed"
px esconfigshow '~/navi48-staging/esp-config.sh show' || die "esp-config show failed"
[ "$DRYRUN" = 1 ] || grep -q 'navi48-m6flip=1' "$(printf '%s/%02d-esconfigshow.txt' "$OUT" "$STEPNO")" || die "live ESP config does not carry navi48-m6flip=1 after esp-config.sh"

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
pxo postboot 'echo "boottime: $(sysctl -n kern.boottime)"; echo "boot-args: $(sysctl -n kern.bootargs)"; echo "console: $(stat -f %Su /dev/console)"; echo "who:"; who; echo "loaded:"; kmutil showloaded --list-only 2>/dev/null | grep -iE "navi48|rdna4|accelprobe"; echo "auxkc:"; kmutil inspect -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>&1 | grep -i accelprobe; echo "bundle:"; /usr/libexec/PlistBuddy -c "Print :CFBundleVersion" /Library/GPUBundles/Navi48Metal.bundle/Contents/Info.plist; echo "ESP:"; ~/navi48-staging/esp-kext.sh show 2>&1; echo "m6stat:"; sudo -n ~/navi48-staging/navi48test accel m6stat 0 2>&1; echo "m6xstat:"; sudo -n ~/navi48-staging/navi48test accel m6xstat 0 2>&1; echo "panics:"; ls -lt /Library/Logs/DiagnosticReports 2>&1 | head -5' || die "post-boot read failed"
if [ "$DRYRUN" != 1 ]; then
  post_boot=$(echo "$PXOUT" | sed -n 's/^boottime: //p')
  [ -n "$post_boot" ] && [ "$post_boot" != "$PRE_BOOT" ] || die "kern.boottime did not change ($PRE_BOOT): this is not a new boot"
  echo "$PXOUT" | /usr/bin/grep -q 'navi48-m6=1' || die "kern.bootargs does not carry navi48-m6=1"
  echo "$PXOUT" | /usr/bin/grep -q 'navi48-m6flip=1' || die "kern.bootargs does not carry navi48-m6flip=1 - the m6flip variant is not live"
  echo "$PXOUT" | /usr/bin/grep -qE "com.navi48.bringup.*$WANT_KEXT|Navi48Bringup.*$WANT_KEXT" || die "kmutil showloaded does not show the bring-up kext at $WANT_KEXT (known skew: kmod_info.c shows 0.0.15 - see navi48-deploy reference)"
  echo "$PXOUT" | /usr/bin/grep -q "$WANT_AUX" || die "no accelprobe $WANT_AUX in showloaded / aux KC"
  echo "$PXOUT" | /usr/bin/grep -q "Navi48Accel-$WANT_AUX.kext" || die "the aux KC does not name the new path Navi48Accel-$WANT_AUX.kext"
  echo "$PXOUT" | sed -n '/^bundle:/{n;p;}' | /usr/bin/grep -qx "$WANT_BUNDLE" || die "bundle version on the PC is not $WANT_BUNDLE"
  echo "$PXOUT" | /usr/bin/grep -q 'navi48-m6 latch ON' || die "m6stat does not report 'navi48-m6 latch ON' (kext and CLI disagree or the latch did not take)"
  echo "$PXOUT" | /usr/bin/grep -qE '^accel m6xstat 0: navi48-m6 ON, navi48-m6flip ON$' || die "m6xstat 0 does not report 'navi48-m6 ON, navi48-m6flip ON' (the second latch did not take, or the CLI predates action 107)"
  newest=$(echo "$PXOUT" | sed -n '/^panics:/,$p' | sed -n '2p')
  [ "$newest" = "$PRE_PANIC" ] || say "  WARNING: the newest DiagnosticReports entry changed across the reboot: '$newest' (was '$PRE_PANIC'): read it before the run (pc-protocol 7)"
fi
say; say "### DEPLOY COMPLETE $(stamp): kext $WANT_KEXT (ESP), aux $WANT_AUX (unchanged) at $AUX_DIR, bundle $WANT_BUNDLE, variant $VARIANT, latches navi48-m6=1 navi48-m6flip=1. PC is at the login window, 0 users, NOT armed."
say "### next: tools/pc/m6-stage1b-run.sh"
exit 0
