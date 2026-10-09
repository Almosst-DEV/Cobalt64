#!/bin/bash
# test-parsers.sh - the kit's parsers against (a) the REAL CLI printf bodies (fakecli = a copy of tools/pc/navi48test.c over a simulated kernel) in known states and (b) REAL captured output of run
# notes/logs/runs/m6-s1b/20261008-123836 (0.0.663 CLI, instance 2). Sources the kit as a library (M6S2_LIB=1 DRYRUN=1): nothing runs, no ssh.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=${N48_ROOT:-$(cd "$HERE/../../.." && pwd)}
SCR=${SIM_SCRATCH:-/private/tmp/claude-501/m6s2-sim}; mkdir -p "$SCR"
FAKECLI=${FAKECLI:-$SCR/fakecli}; [ -x "$FAKECLI" ] || "$HERE/build.sh" "$FAKECLI" >/dev/null || exit 1
W=$(mktemp -d "$SCR/parsers.XXXXXX"); export SIM_DIR=$W/sd SIM_FR=$W/fr
mkdir -p "$SIM_DIR/cfg" "$SIM_DIR/kv" "$SIM_FR/private/tmp"
PASSN=0; FAILN=0
eq() { if [ "$2" = "$3" ]; then PASSN=$((PASSN+1)); else FAILN=$((FAILN+1)); printf 'FAIL  %-44s got [%s] want [%s]\n' "$1" "$2" "$3"; fi; }
c()  { printf '%s\n' "$2" >"$SIM_DIR/cfg/$1"; }
at() { echo $(( ($1) * 1000 )) >"$SIM_DIR/vclock_ms"; }           # virtual seconds since the sim's start; the restart happens at 100 s
restart() { echo $(( 1791470000 + 100 )) >"$SIM_DIR/kv/restart_epoch"; echo 0 >"$SIM_DIR/kv/rst_kill1"; echo 0 >"$SIM_DIR/kv/rst_kill2"; }
out() { local f=$W/o.$$.$RANDOM; "$FAKECLI" accel "$@" >"$f" 2>&1; echo "$f"; }
export DRYRUN=1 DRYRUN_OUT=$W/dry M6S2_LIB=1 STAGE=2b
# shellcheck disable=SC1090
. "$ROOT/tools/pc/m6-stage2-run.sh" >/dev/null 2>&1 || { echo "cannot source the kit as a library"; exit 1; }
set +e

# ---- before the restart: both instances un-acquired
echo 0 >"$SIM_DIR/vclock_ms"; c stage2b 1; c pool_big 1; c flip_at_2 14; c flip_at_1 18
f=$(out m6xstat 1 0); parse_x0 "$f" 1
eq "pre i1 X_LATCH" "$X_LATCH" "navi48-m6 ON, navi48-m6flip ON, navi48-m6flip1 ON"; eq "pre i1 X_ACQ" "$X_ACQ" no; eq "pre i1 X_OFFLINE" "$X_OFFLINE" 0; eq "pre i1 X_A (prints 0 until acquired)" "$X_A" 0; eq "pre i1 X_RESTORES" "$X_RESTORES" 0; eq "pre i1 X_WDOG" "$X_WDOG" 0; eq "pre i1 X_REUSE" "$X_REUSE" 0
f=$(out m6xstat 2 0); parse_x0 "$f" 2
eq "pre i2 X_LATCH" "$X_LATCH" "navi48-m6 ON, navi48-m6flip ON"; eq "pre i2 X_ACQ" "$X_ACQ" no
c latch1_off 1; f=$(out m6xstat 1 0); parse_x0 "$f" 1; eq "latch1 OFF: X_OFFLINE" "$X_OFFLINE" 1; c latch1_off 0
# ---- the baseline planes (m6xstat <i> 2) before the restart
f=$(out m6xstat 1 2); parse_x2 "$f" 1
eq "i1 p2 X_UF" "$X_UF" 0; eq "i1 p2 X_BLANK" "$X_BLANK" 0; eq "i1 p2 X_VTG" "$X_VTG" 1; eq "i1 p2 X_VMF" "$X_VMF" 00000000; eq "i1 p2 X_ODM10" "$X_ODM10" 0; eq "i1 p2 X_FP" "$X_FP" 0; eq "i1 p2 X_VIEW" "$X_VIEW" 4380780
eq "i1 p2 X_PITCH" "$X_PITCH" 77f; eq "i1 p2 X_SCFG" "$X_SCFG" 8; eq "i1 p2 X_SCTL" "$X_SCTL" 0; eq "i1 p2 X_VMID" "$X_VMID" 0; eq "i1 p2 X_STAGE/CUR/PIN" "$X_STAGE/$X_CUR/$X_PIN" 6/0/1
eq "i1 p2 X_PROG=X_EARLY=A" "$X_PROG/$X_EARLY" "8004360000/8004360000"; eq "i1 p2 X_PA2/PB2" "$X_PA2/$X_PB2" "8004360000/8004b50000"; [ -n "$X_OTGFC" ] && eq "i1 p2 OTG frame counter parsed" 1 1 || eq "i1 p2 OTG frame counter parsed" "$X_OTGFC" "nonempty"
f=$(out m6xstat 2 2); parse_x2 "$f" 2
eq "i2 p2 X_VTG/PITCH/VIEW" "$X_VTG/$X_PITCH/$X_VIEW" "2/9ff/5a00a00"; eq "i2 p2 X_PROG" "$X_PROG" 8002740000
# ---- the crc output of both instances, MONB WATCH mask 0 / 0x40 / 0x100, the CLI's own DISTURBED abort text
f=$(out disp2 crc 1); parse_crc "$f" 1
eq "i1 crc RG/B" "$CRC_RG_NOW/$CRC_B_NOW" "0xe7a0b345/0xb196d20f"; eq "i1 crc UF_OK" "$UF_OK" 1; eq "i1 crc FIFO_ERR" "$FIFO_ERR" 0; eq "i1 crc MONBW" "$MONBW" 0; eq "i1 crc DPW_OK" "$DPW_OK" 1; eq "i1 crc A/B" "$CRC_PA/$CRC_PB" "8004360000/8004b50000"; eq "i1 crc DP console" "$DPRULE_HI:$DPRULE_LO" "80:01900000"
f=$(out disp2 crc 2); parse_crc "$f" 2
eq "i2 crc RG/B" "$CRC_RG_NOW/$CRC_B_NOW" "0x78097625/0x4bd14c15"; eq "i2 crc MONBW (a monitor B op has no monitor B watch)" "$MONBW" ""; eq "i2 crc UF_OK/FIFO" "$UF_OK/$FIFO_ERR" "1/0"
c fifoerr 2; f=$(out disp2 crc 1); parse_crc "$f" 1; eq "i1 crc FIFO_ERR 2" "$FIFO_ERR" 2; c fifoerr 0
# ---- after the restart: acquired, slots, content, faults
restart; at 100; echo "restart at 100 s"  >/dev/null
at 130   # 30 s after the restart: monitor B (14) and monitor A (18) acquired, content settled
f=$(out m6xstat 1 0); parse_x0 "$f" 1
eq "acq i1 X_ACQ" "$X_ACQ" YES; eq "acq i1 X_A/X_B" "$X_A/$X_B" "8004360000/8004b50000"; eq "acq i1 X_ACQREF" "$X_ACQREF" 0; eq "acq i1 X_TGEN" "$X_TGEN" 8; eq "acq i1 X_GH4" "$X_GH4" 0; eq "acq i1 X_WDOG/RESTORES/RESFAIL" "$X_WDOG/$X_RESTORES/$X_RESFAIL" "0/0/0"; eq "acq i1 X_RFAILED" "$X_RFAILED" no
eq "acq i1 X_GPUHELD in 0x1k" "$(in_set "$X_GPUHELD" 0x10 0x11 0x12 && echo ok || echo "$X_GPUHELD")" ok; [ "$X_PRES" -gt 0 ] && eq "acq i1 presents > 0" 1 1 || eq "acq i1 presents > 0" "$X_PRES" ">0"
f=$(out m6xstat 1 1); parse_x1 "$f" 1
eq "acq i1 slots" "$X_S0 $X_S1 $X_S2" "800bb40000 800c340000 800cb40000"; eq "acq i1 X_WREF/WFAIL/UNTAG/GEOM" "$X_WREF/$X_WFAIL/$X_UNTAG/$X_GEOM" "0/0/0/0"; eq "acq i1 front/pend" "$X_FRONT/$X_PEND" "ffffffff/ffffffff"
f=$(out m6xstat 2 1); parse_x1 "$f" 2; eq "acq i2 slots" "$X_S0 $X_S1 $X_S2" "8008e80000 8009cc0000 800ab00000"
f=$(out m6xstat 1 2); parse_x2 "$f" 1; eq "acq i1 EARLIEST in own slots" "$(in_set "$X_EARLY" 800bb40000 800c340000 800cb40000 && echo ok || echo "$X_EARLY")" ok; eq "acq i1 FP" "$X_FP" 0
f=$(out disp2 crc 1); parse_crc "$f" 1; eq "acq i1 crc off CRC_A" "$([ "$CRC_RG_NOW" != 0xe7a0b345 ] && echo off)" off
# the monitor A crc op: monitor B-watch DISTURBED paths
c monbw_row_at 10; c monbw_row 6; f=$(out disp2 crc 1); parse_crc "$f" 1; eq "monbw row6 MONBW" "$MONBW" 0x40; eq "monbw row6: the CLI abort text must not clear DPW_OK" "$DPW_OK" 1; eq "monbw row6: CRC still parsed" "$([ -n "$CRC_RG_NOW" ] && echo y)" y
c monbw_row 8; f=$(out disp2 crc 1); parse_crc "$f" 1; eq "monbw row8 MONBW" "$(printf '%x' $((MONBW & 0x100)))" 100; c monbw_row_at 1e9
c uf_at_1 10; f=$(out disp2 crc 1); parse_crc "$f" 1; eq "uf crc: UF_OK 0" "$UF_OK" 0; eq "uf crc: ODM_OCC" "$ODM_OCC" 1; f=$(out m6xstat 1 2); parse_x2 "$f" 1; eq "uf p2: X_UF/X_ODM10" "$X_UF/$X_ODM10" "1/1"; c uf_at_1 1e9
c vmf_at 10; f=$(out m6xstat 1 2); parse_x2 "$f" 1; eq "vmf p2 X_VMF" "$X_VMF" 00000001; f=$(out disp2 crc 1); parse_crc "$f" 1; eq "vmf crc UF_OK 0" "$UF_OK" 0; c vmf_at 1e9
c fp_stuck_at_2 10; f=$(out m6xstat 2 2); parse_x2 "$f" 2; eq "fp p2 X_FP" "$X_FP" 1; eq "fp p2 prog != early" "$([ "$X_PROG" != "$X_EARLY" ] && echo y)" y; c fp_stuck_at_2 1e9
c gh4_at_1 10; f=$(out m6xstat 1 0); parse_x0 "$f" 1; eq "gh4 X_GH4" "$X_GH4" 1; c gh4_at_1 1e9
# ---- m6stat 0 1 2 4
f=$(out m6stat 0); f1=$(out m6stat 1); f2=$(out m6stat 2); f4=$(out m6stat 4); parse_m6 "$f" "$f1" "$f2" "$f4"
eq "m6stat txn>0 on all three" "$([ "$M_TXN0" -gt 0 ] && [ "$M_TXN1" -gt 0 ] && [ "$M_TXN2" -gt 0 ] && echo y)" y; eq "m6stat IDs" "$M_IDS0/$M_IDS1/$M_IDS2" "2/3/3"; eq "m6stat amb/nofb/amb2" "$M_AMB/$M_NOFB/$M_AMB2" "0/0/0"; eq "m6stat 1 AGDC nfb" "$M_AGN" 3
eq "m6stat 1 stamps>0 refused" "$([ "$M_STAMP1" -gt 0 ] && echo y)/$M_REF0/$M_REF1/$M_REF2" "y/0/0/0"; eq "m6stat 4 n921 (instance 1, 2)" "$M_N921_1/$M_N921_2" "6/6"; eq "m6stat 4 bitmask" "$M_EPMASK" 7; eq "m6stat 4 nfb/out/last" "$M_NFB4/$M_EPOUT/$M_LASTEP4" "3/0/2"
c epmask 3; c nfb 2; c epout 1; f4=$(out m6stat 4); f1=$(out m6stat 1); parse_m6 "$f" "$f1" "$f2" "$f4"; eq "m6stat 4 broken: mask/nfb/out" "$M_EPMASK/$M_NFB4/$M_EPOUT" "3/2/1"; eq "m6stat 1 broken nfb" "$M_AGN" 2
# ---- vramstat: 256 MiB pool, 1 GiB pool, and 0.0.664's extra ReBAR line
c pool_big 0; at 99; f=$(out vramstat); parse_vram "$f"; eq "vram small total/free/mapped" "$V_TOTAL/$V_FREE/$V_MAPPED" "217/97/256"; eq "vram small free bytes" "$V_FREEB" 101711872
at 130; f=$(out vramstat); parse_vram "$f"; eq "vram small after both acquires (monitor B first, monitor A refused)" "$V_FREE" 54
c pool_big 1; f=$(out vramstat); parse_vram "$f"; eq "vram big total/mapped" "$V_TOTAL/$V_MAPPED" "985/1024"
{ cat "$f"; echo "  ReBAR BAR0  : current 256 MiB, supported-sizes mask 0x7f00: 256 MiB 512 MiB 1024 MiB  (smallest 256 MiB = ResizeAppleGpuBars code 8)"; } >"$f.664"; parse_vram "$f.664"; eq "vram with the 0.0.664 ReBAR line" "$V_TOTAL/$V_FREE" "985/$V_FREE"
# ---- the bundle's lines (generated by sim.c from the Navi48Device.m N48LOG formats)
at 160; r=$((1791470000+100)); "$FAKECLI" --genlog $((r+0)) $((r+60)) >"$W/blog.txt"
for i in 1 2; do parse_bundlec "$W/blog.txt" $i; parse_bundlelog "$W/blog.txt" $i; done
eq "bundle c1 presents/wdog/mism/geom" "$([ -n "$B1_PRES" ] && echo y)/$B1_WDOG/$B1_MISMATCH/$B1_GEOMMIS" "y/0/0/0"; eq "bundle c2 mism" "$B2_MISMATCH" 3; eq "bundle cached" "$B1_CACHED/$B2_CACHED" "8/8"; eq "bundle TOD parsed" "$([ -n "$B2_TOD" ] && echo y)" y
eq "bundle log init i1/i2" "$BL1_INIT/$BL2_INIT" "ENABLED/ENABLED"; eq "bundle log kill file" "$BL1_KILLFILE/$BL2_KILLFILE" "absent/absent"; eq "bundle log ACQUIRED" "$BL1_ACQUIRED/$BL2_ACQUIRED" "1/1"; eq "bundle log pool free/need i1" "$BL1_FREE/$BL1_NEED" "857/55"; eq "bundle log slots i1" "$BL1_SLOTS" "800bb40000 800c340000 800cb40000 "
c pool_big 0; "$FAKECLI" --genlog $((r+0)) $((r+60)) >"$W/blog2.txt"; for i in 1 2; do parse_bundlelog "$W/blog2.txt" $i; done
eq "small pool: monitor A refused (monitor B first)" "$BL1_POOLREF/$BL1_ACQUIRED/$BL2_ACQUIRED/$BL1_FAILCLOSED" "1/0/1/1"; eq "small pool refusal numbers" "$BL1_FREE/$BL1_NEED" "54/55"
c pool_big 1; c geomword_at_1 25; "$FAKECLI" --genlog $((r+0)) $((r+60)) >"$W/blog3.txt"; parse_bundlelog "$W/blog3.txt" 1; eq "geometry-word line counted" "$BL1_GEOMWORD" 1
# a pre-flip 2a world: kill file present at the restart
echo 1 >"$SIM_DIR/kv/rst_kill1"; c force_init_enabled 0; "$FAKECLI" --genlog $((r+0)) $((r+60)) >"$W/blog4.txt"; parse_bundlelog "$W/blog4.txt" 1; eq "2a bundle init i1" "$BL1_INIT/$BL1_KILLFILE" "OFF/PRESENT"
# ---- ioreg lines of REAL run B (notes/logs/runs/runB-658/runB658.log:687,699) and the 1b run
printf '+-o Navi48Framebuffer  <class Navi48Framebuffer, id 0x100000681, registered, matched, active, busy 0 (1 ms), retain 18>\n+-o Navi48Framebuffer  <class Navi48Framebuffer, id 0x100000689, registered, matched, active, busy 0 (0 ms), retain 18>\n' >"$W/fb.txt"
parse_fbids "$W/fb.txt"; eq "ioreg ids (real Run B lines)" "$FB_N/$FB_IDS" "2/0x100000681 0x100000689 "
printf '+-o Navi48Framebuffer  <class Navi48Framebuffer, id 0x100000729, matching, active, busy 1>\n' >"$W/fb2.txt"; parse_fbids "$W/fb2.txt"; eq "an unregistered node is not counted" "$FB_N" 0
# ---- REAL captured output (run 20261008-123836: 0.0.663 CLI, the monitor B) through the generalized parsers
R=$ROOT/notes/logs/runs/m6-s1b/20261008-123836
if [ -f "$R/45-snap-flip02.txt.x0" ]; then
  parse_x0 "$R/45-snap-flip02.txt.x0" 2; eq "REAL x0 X_ACQ/ACQUIRES/GEN" "$X_ACQ/$X_ACQUIRES/$X_GEN" "YES/1/1"; eq "REAL x0 A/B" "$X_A/$X_B" "8002740000/8003550000"; eq "REAL x0 presents/latched/reuse" "$X_PRES/$X_LATCHED/$X_REUSE" "760/744/0"; eq "REAL x0 TGEN/ACQREF/WDOG" "$X_TGEN/$X_ACQREF/$X_WDOG" "8/0/0"; eq "REAL x0 GPUHELD" "$X_GPUHELD" 0x21; eq "REAL x0 X_LATCH" "$X_LATCH" "navi48-m6 ON, navi48-m6flip ON"
  parse_x1 "$R/45-snap-flip02.txt.x1" 2; eq "REAL x1 slots" "$X_S0 $X_S1 $X_S2" "8007f80000 8008dc0000 8009c00000"; eq "REAL x1 refusals" "$X_WREF/$X_WFAIL/$X_UNTAG/$X_GEOM" "0/0/0/0"; eq "REAL x1 front/pend" "$X_FRONT/$X_PEND" "22/20"
  parse_x2 "$R/45-snap-flip02.txt.x2" 2; eq "REAL x2 UF/VTG/FP" "$X_UF/$X_VTG/$X_FP" "0/2/1"; eq "REAL x2 prog/early" "$X_PROG/$X_EARLY" "8008dc0000/8007f80000"; eq "REAL x2 pitch/scfg/view" "$X_PITCH/$X_SCFG/$X_VIEW" "9ff/8/5a00a00"; eq "REAL x2 stage/cur/pin/A/B" "$X_STAGE/$X_CUR/$X_PIN/$X_PA2/$X_PB2" "6/0/1/8002740000/8003550000"
  parse_crc "$R/45-snap-flip02.txt.crc" 2; eq "REAL crc2 RG/B" "$CRC_RG_NOW/$CRC_B_NOW" "$(sed -n 's/.*OTG2 CRC *: RG \(0x[0-9a-f]*\) *B \(0x[0-9a-f]*\).*/\1\/\2/p' "$R/45-snap-flip02.txt.crc" | head -1)"; eq "REAL crc2 UF_OK" "$UF_OK" 1; eq "REAL crc2 DPW_OK" "$DPW_OK" 1; eq "REAL crc2 A/B" "$CRC_PA/$CRC_PB" "8002740000/8003550000"
  parse_m6 "$R/45-snap-flip02.txt.m0" "$R/45-snap-flip02.txt.m1" "$R/45-snap-flip02.txt.m2"; eq "REAL m6stat: txn0/2 present" "$([ "$M_TXN0" -gt 0 ] && [ "$M_TXN2" -gt 0 ] && echo y)" y; eq "REAL m6stat 1 AGDC nfb (2-entry list)" "$M_AGN" 2; eq "REAL m6stat monitor A line (instance 1) parses" "$([ -n "$M_TXN1" ] && echo y)" y
else echo "(real fixtures of run 20261008-123836 not present: skipped)"; fi
case "$W" in "$SCR"/parsers.*) rm -r "$W" ;; esac
echo "== parser tests: $PASSN passed, $FAILN failed"
[ $FAILN = 0 ]
