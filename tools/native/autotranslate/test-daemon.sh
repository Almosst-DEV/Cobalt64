#!/bin/bash
# test-daemon.sh - host test of the hardened translate daemon (C1) on the host Mac, with the arm64 metal2vulkan (the PC runs x86_64; same scripts).
#   usage: test-daemon.sh [--sandbox]   (--sandbox also runs one translation under sandbox-exec with n48-translate.sb)
# Needs: ~/navi48-native/metal2vulkan/target/release/metal2vulkan (arm64), an AIR sample from ~/navi48-native/c0work/air, /opt/homebrew/bin/spirv-val (arm64, only to prove the real-validator wiring).
set -u
D="$(cd "$(dirname "$0")" && pwd)"
# Scratch rules: ONE mktemp dir (under $N48_TEST_TMP, else $TMPDIR, else /private/tmp), prefix-checked before anything is removed; every symlink the test makes points INSIDE it; no other path is ever removed.
W=$(mktemp -d "${N48_TEST_TMP:-${TMPDIR:-/private/tmp}}/n48daemon.XXXXXX") || exit 2; W=$(cd "$W" && pwd -P)
case "$W" in */n48daemon.??????) ;; *) echo "test-daemon: unexpected scratch dir '$W'"; exit 2;; esac
cleanup() { case "$W" in */n48daemon.??????) [ -d "$W" ] && rm -rf -- "$W";; esac; }; trap cleanup EXIT
M2V=${N48_TEST_M2V:-$HOME/navi48-native/metal2vulkan/target/release/metal2vulkan}
AIR=${N48_TEST_AIR:-$HOME/navi48-native/c0work/air/__System__Applications__Chess.app__Contents__Resources__default/0002_fsChess.fragment.air}
fails=0; runs=0
check() { runs=$((runs+1)); if eval "$2"; then :; else fails=$((fails+1)); echo "FAIL: $1"; fi; }
H=$W/inst; mkdir -p "$H"; cp "$D"/{pc-translate.py,pc-translate.sh,n48-llvm-dis,n48-spirv-val-stub,overrides.txt,n48-translate.sb} "$H/"; cp "$M2V" "$H/metal2vulkan"
UID_=$(id -u); S=$W/src; mkdir -p "$S/$UID_" "$S/99999" "$S/abc"
sha=$(shasum -a 256 "$AIR" | cut -c1-64); cp "$AIR" "$S/$UID_/$sha.air"
# a second valid-looking name for each hostile file
printf 'BC\xc0\xde%s' "$(head -c 200 /dev/urandom | base64)" > "$W/mal.bin"; msha=$(shasum -a 256 "$W/mal.bin" | cut -c1-64); cp "$W/mal.bin" "$S/$UID_/$msha.air"
head -c 100 /dev/urandom > "$W/junk.bin"; jsha=$(shasum -a 256 "$W/junk.bin" | cut -c1-64); cp "$W/junk.bin" "$S/$UID_/$jsha.air"
head -c 5000000 /dev/zero > "$W/big.bin"; bsha=$(shasum -a 256 "$W/big.bin" | cut -c1-64); cp "$W/big.bin" "$S/$UID_/$bsha.air"
: > "$S/$UID_/$(printf '0%.0s' $(seq 64)).air"
mkdir -p "$W/linktarget"; cp "$AIR" "$W/linktarget/real.air"; ln -s "$W/linktarget/real.air" "$S/$UID_/$(printf 'a%.0s' $(seq 64)).air"
AIR2=$(dirname "$AIR")/0001_vsChess.vertex.air; AIR3=$(dirname "$AIR")/0004_fsChessHintArrow.fragment.air
cp "$AIR" "$S/$UID_/not-a-sha.air"; cp "$AIR3" "$S/$UID_/$(printf 'g%.0s' $(seq 64)).air"
sha2=$(shasum -a 256 "$AIR2" | cut -c1-64); sha3=$(shasum -a 256 "$AIR3" | cut -c1-64)
cp "$AIR2" "$S/99999/$sha2.air"; cp "$AIR2" "$S/abc/$sha2.air"
run() { N48_SRC=$S N48_OUT=$W/out N48_STATE=$W/state N48_BUNDLE=$W/nobundle N48_HERE=$H N48_ONCE=1 N48_POLL=0.1 N48_NO_SANDBOX=${SB-1} bash "$H/pc-translate.sh"; }
SB=1 run
OUT=$W/out; DONE=$W/state/done; LOG=$W/state/translate.log
check "the good AIR was translated (spv + meta installed)" "[ -f $OUT/$sha.spv ] && [ -f $OUT/$sha.meta.json ]"
check "the good AIR is recorded ok under the current key" "grep -Eq '^$sha [0-9a-f]{12} ok\$' $DONE"
check "a malformed AIR (BC magic, garbage) is recorded FAIL and installs nothing" "grep -Eq '^$msha [0-9a-f]{12} FAIL\$' $DONE && [ ! -f $OUT/$msha.spv ]"
check "random junk is rejected before any translator runs (magic check)" "grep -Eq '^$jsha [0-9a-f]{12} FAIL\$' $DONE && grep -q 'not LLVM bitcode' $LOG"
check "a 5 MB input is rejected by the size cap" "grep -q \"$bsha uid=$UID_ REJECT size 5000000\" $LOG && ! grep -q \"$bsha.*FAIL translate\" $LOG"
check "an empty file is rejected" "grep -q 'REJECT size 0' $LOG"
check "a symlinked .air is never followed or recorded" "! grep -q '^aaaaaaaa' $DONE"
check "a non-sha file name is ignored" "! grep -q 'not-a-sha' $DONE $LOG"
check "a subdirectory whose name is not its owner's uid (99999) is ignored" "! grep -q 'uid=99999' $LOG"
check "a non-numeric subdirectory is ignored" "! grep -q 'uid=abc' $LOG"
check "a non-hex 64-character name is ignored" "! grep -q 'gggggggg' $DONE && ! grep -q \"$sha3\" $DONE"
check "files in the foreign / non-numeric subdirectories were never translated" "! grep -q \"^$sha2 \" $DONE && [ ! -f $OUT/$sha2.spv ]"
# a symlinked uid directory: its own fixture (the link is named with our uid, the target is elsewhere)
mkdir -p "$W/realdir" "$W/src2"; cp "$AIR2" "$W/realdir/$sha2.air"; ln -s "$W/realdir" "$W/src2/$UID_"
N48_SRC=$W/src2 N48_OUT=$W/out2 N48_STATE=$W/state2 N48_BUNDLE=$W/nobundle N48_HERE=$H N48_ONCE=1 N48_POLL=0.1 N48_NO_SANDBOX=1 bash "$H/pc-translate.sh"
check "a symlinked uid directory is not followed" "[ ! -f $W/out2/$sha2.spv ] && ! grep -q \"^$sha2 \" $W/state2/done"
# a fake translator that hogs 300 MB and then hangs for 30 s: the watchdog and the wall clock must kill it (its whole process group)
printf '#!/bin/sh\n/usr/bin/python3 -c "b=bytearray(300*1024*1024); import time; time.sleep(30)" &\nsleep 30\n' > "$H/slow-m2v"; chmod +x "$H/slow-m2v"
t0=$(date +%s); out=$(python3 $H/pc-translate.py $AIR --out $W/out4 --m2v $H/slow-m2v --max-rss-kb 100000 2>&1); t1=$(date +%s)
check "a translator (and its child) over the memory cap is killed within seconds and installs nothing" "echo \"\$out\" | grep -q 'FAIL memory' && [ $((t1-t0)) -lt 10 ] && [ ! -f $W/out4/$sha.spv ]"
t0=$(date +%s); out=$(python3 $H/pc-translate.py $AIR --out $W/out5 --m2v $H/slow-m2v --timeout 1 2>&1); t1=$(date +%s)
check "a hung translator is killed at the wall-clock limit (1 s here; 20 s in the daemon)" "echo \"\$out\" | grep -q 'FAIL timeout' && [ $((t1-t0)) -lt 10 ] && [ ! -f $W/out5/$sha.spv ]"
check "...and no orphan from the fake translator is left" "! ps -axo command= | /usr/bin/grep -q '[b]ytearray(300'"
# the translator itself refuses an oversize input when run directly (defence in depth)
check "pc-translate.py refuses an oversize input itself" "python3 $H/pc-translate.py $W/big.bin --out $W/out3 --m2v $H/metal2vulkan 2>&1 | grep -q 'FAIL input size'"
check "the daemon survived every hostile input (one pass completed, log has 'start')" "grep -q ' start (' $LOG"
n1=$(wc -l < $DONE)
SB=1 run
check "a second pass does nothing (everything decided under the same key)" "[ \$(wc -l < $DONE) -eq $n1 ]"
# versioned key: a changed translator retries the FAIL but keeps the ok
echo '# changed' >> "$H/overrides.txt"; SB=1 run
check "a changed translator key retries the old FAIL (new FAIL line under a new key)" "[ \$(grep -c \"^$msha \" $DONE) -eq 2 ]"
check "... and ALSO re-translates the old ok (a correctness fix replaces ok outputs; detail in the stub section below)" "[ \$(grep -c \"^$sha \" $DONE) -eq 2 ]"
# real validator wiring (arm64 homebrew spirv-val, only proves the plumbing)
if [ -x /opt/homebrew/bin/spirv-val ]; then
  printf '#!/bin/sh\nexec /opt/homebrew/bin/spirv-val "$@"\n' > "$H/spirv-val"; chmod +x "$H/spirv-val"; rm -f $OUT/$sha.spv $OUT/$sha.meta.json; : > $DONE; SB=1 run
  check "with spirv-val next to the scripts the log names it as the validator" "grep -q 'validator=$H/spirv-val' $LOG"
  check "...and the good AIR still translates with it" "[ -f $OUT/$sha.spv ]"
  printf '#!/bin/sh\necho planted-invalid >&2\nexit 1\n' > "$H/spirv-val"; rm -f $OUT/$sha.spv $OUT/$sha.meta.json; : > $DONE; SB=1 run
  check "PLANT: a validator that rejects makes the translation FAIL and installs nothing (the validator really runs)" "grep -Eq '^$sha [0-9a-f]{12} FAIL\$' $DONE && [ ! -f $OUT/$sha.spv ]"
  rm -f "$H/spirv-val"
fi
check "the stub is the validator when no spirv-val is installed" "grep -q 'validator=$H/n48-spirv-val-stub' $LOG"
# ---- key change with a stub translator whose output embeds its version (item 256) ----
H2=$W/inst2; mkdir -p "$H2"; cp "$D"/{pc-translate.py,pc-translate.sh,n48-llvm-dis,n48-spirv-val-stub,overrides.txt,n48-translate.sb} "$H2/"
mkstub() {  # $1 version: a metal2vulkan stand-in; spv = SPIR-V magic + the version padded to 20 bytes
  printf '#!/bin/sh\nprintf "\\003\\002\\043\\007%%-20s" "%s" > "$2"\nprintf "{\\"stage\\":\\"fragment\\",\\"bindings\\":[]}" > "$4"\n' "$1" > "$H2/metal2vulkan"; chmod +x "$H2/metal2vulkan"
}
S2=$W/src2b; mkdir -p "$S2/$UID_"; cp "$AIR" "$S2/$UID_/$sha.air"; cp "$AIR2" "$S2/$UID_/$sha2.air"
O2=$W/out2b; ST2=$W/state2b; D2=$ST2/done; L2=$ST2/translate.log
run2() { N48_SRC=$S2 N48_OUT=$O2 N48_STATE=$ST2 N48_BUNDLE=$W/nobundle N48_HERE=$H2 N48_ONCE=1 N48_POLL=0.1 N48_NO_SANDBOX=1 bash "$H2/pc-translate.sh"; }
mkstub v1; run2
k1=$(awk -v s=$sha '$1==s{print $2}' $D2 | tail -1)
check "stub v1: both inputs translated, outputs embed v1" "grep -q 'v1  ' $O2/$sha.spv && grep -q 'v1  ' $O2/$sha2.spv && grep -Eq '^$sha [0-9a-f]{12} ok\$' $D2"
ino1=$(stat -f %i $O2/$sha.spv); rm -f -- "$S2/$UID_/$sha2.air"   # sha2's AIR vanishes (/tmp cleared)
mkstub v2; run2
k2=$(awk -v s=$sha '$1==s{print $2}' $D2 | tail -1)
check "(1) an ok under key K1 is re-translated under K2 and the output changes" "[ '$k1' != '$k2' ] && grep -q 'v2  ' $O2/$sha.spv && ! grep -q 'v1  ' $O2/$sha.spv && grep -q \"^$sha $k2 ok\\\$\" $D2"
check "(2) the old output (spv and meta) is in the backup dir of the old key" "grep -q 'v1  ' $ST2/backup/$k1/$sha.spv && [ -f $ST2/backup/$k1/$sha.meta.json ]"
check "(3a) an entry without AIR keeps its old output" "grep -q 'v1  ' $O2/$sha2.spv && [ -f $O2/$sha2.meta.json ]"
run2; run2
check "(3b) ...and logs 'stale-kept (no AIR)' exactly once for it (not on later passes)" "[ \$(grep -c \"$sha2 stale-kept (no AIR)\" $L2) -eq 1 ]"
check "(3c) the entry with an AIR is not logged stale" "! grep -q \"$sha stale-kept\" $L2"
mt_meta=$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns)" $O2/$sha.meta.json); mt_spv=$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns)" $O2/$sha.spv)
check "(4a) atomicity: the meta was written before the spv" "[ $mt_meta -lt $mt_spv ]"
check "(4b) atomicity: the replacement is a rename (new inode, no temp left)" "[ \$(stat -f %i $O2/$sha.spv) != $ino1 ] && [ -z \"\$(ls $O2 | grep '\.tmp\$')\" ]"
mkstub v3; run2; mkstub v4; run2
check "backups are bounded: only the last 2 key dirs remain, the oldest pruned" "[ \$(ls $ST2/backup | wc -l) -eq 2 ] && [ ! -d $ST2/backup/$k1 ]"
check "the newest output is v4 and decided under the newest key" "grep -q 'v4  ' $O2/$sha.spv"
# a new translator that FAILs for a previously-ok input: the old output leaves OUT, the backup keeps it
printf '#!/bin/sh\necho boom >&2\nexit 1\n' > "$H2/metal2vulkan"; run2
check "a failing new translation drops the old output from OUT (backup kept)" "[ ! -f $O2/$sha.spv ] && [ \$(ls $ST2/backup/*/$sha.spv | wc -l) -ge 1 ]"
if [ "${1:-}" = "--sandbox" ]; then
  rm -f $OUT/$sha.spv $OUT/$sha.meta.json; : > $DONE; : > $LOG; SB= run
  check "SANDBOXED: the good AIR translates under sandbox-exec" "[ -f $OUT/$sha.spv ] && grep -Eq '^$sha [0-9a-f]{12} ok\$' $DONE"
  check "SANDBOXED: a malformed AIR fails without crashing the daemon" "grep -Eq '^$msha [0-9a-f]{12} FAIL\$' $DONE"
fi
echo "test-daemon: $runs checks, $fails failed"; [ $fails -eq 0 ]
