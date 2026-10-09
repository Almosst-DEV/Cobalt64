#!/bin/bash
# run-scenarios.sh - drive tools/pc/m6-stage2-run.sh against the FAKE PC (no ssh, no PC contact): a fake ssh executes the remote commands locally in a fake root, the REAL navi48test.c printf bodies
# print the CLI verbs (fakecli, built by build.sh), the clock is VIRTUAL (fake sleep / date), faults are injected by scenario config. One scenario per abort rule / refusal / verdict.
#   run-scenarios.sh [-k] [name ...]      (no names: every scenario; -k keeps the per-scenario work dirs)
# Every scenario work dir is made with mktemp under the scratch root and removed only after a prefix check.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=${N48_ROOT:-$(cd "$HERE/../../.." && pwd)}
KIT=${KIT:-$ROOT/tools/pc/m6-stage2-run.sh}
SCR=${SIM_SCRATCH:-/private/tmp/claude-501/m6s2-sim}
FAKECLI=${FAKECLI:-$SCR/fakecli}
KEEP=0; [ "${1:-}" = "-k" ] && { KEEP=1; shift; }
mkdir -p "$SCR"
[ -x "$FAKECLI" ] || "$HERE/build.sh" "$FAKECLI" >/dev/null || { echo "cannot build fakecli"; exit 1; }
PASSN=0; FAILN=0; FAILED=""

# ---------------------------------------------------------------------------------------------- scenario DSL
c()  { printf '%s\n' "$2" >"$SD/cfg/$1"; }                 # c <key> <value>: scenario config read by sim.c / the shims
E()  { ENVS="$ENVS $1"; }                                   # E VAR=value: environment for the kit
expect_rc()  { X_RC=$1; }
expect_log() { X_LOG="$X_LOG
$1"; }                                                      # a regex that must appear in run.log
expect_nolog() { X_NOLOG="$X_NOLOG
$1"; }                                                      # a regex that must NOT appear in run.log
expect_kill() { X_KILL="$X_KILL $1"; }                     # a kill file number that must exist at the end (1 or 2)
expect_nokill() { X_NOKILL="$X_NOKILL $1"; }

world() {   # world <stage>: a fresh fake PC
    W=$(mktemp -d "$SCR/scen.XXXXXX"); SD=$W/sd; FR=$W/fr
    mkdir -p "$SD/cfg" "$SD/kv" "$FR/private/tmp" "$FR/tmp" "$FR/var/tmp" "$FR/Library/GPUBundles/Navi48Metal.bundle/Contents/MacOS" "$FR/Library/Extensions/Navi48Accel-0.0.7.kext/Contents" "$FR/Library/Logs/DiagnosticReports" "$W/root"
    echo 0 >"$SD/vclock_ms"
    /usr/bin/plutil -create xml1 "$FR/Library/GPUBundles/Navi48Metal.bundle/Contents/Info.plist" && /usr/libexec/PlistBuddy -c "Add :CFBundleVersion string 19" "$FR/Library/GPUBundles/Navi48Metal.bundle/Contents/Info.plist" >/dev/null
    /usr/bin/plutil -create xml1 "$FR/Library/Extensions/Navi48Accel-0.0.7.kext/Contents/Info.plist" && /usr/libexec/PlistBuddy -c "Add :CFBundleVersion string 0.0.7" "$FR/Library/Extensions/Navi48Accel-0.0.7.kext/Contents/Info.plist" >/dev/null
    printf 'scanout (instance %%d, %%s): counters: presents\nbpr %%lu, %%s; plan %%ux%%u pitch %%u\ninst_geom_mismatch\nn48m-noflip1\n' >"$FR/Library/GPUBundles/Navi48Metal.bundle/Contents/MacOS/Navi48Metal"
    ENVS=""; X_RC=0; X_LOG=""; X_NOLOG=""; X_KILL=""; X_NOKILL=""
    [ "$1" = 2b ] && c stage2b 1
    c flip_at_2 14; c flip_at_1 18; E FLIP_S=25
    return 0
}
run_kit() {   # run_kit <name> <stage>
    local name=$1 stage=$2 out=$W/kit.out
    ( export SIM_DIR=$SD SIM_FR=$FR SIM_FAKECLI=$FAKECLI SIM_HOME=$HERE/home SIM_BIN=$HERE/bin SIM_RBIN=$HERE/rbin
      export PATH="$HERE/bin:$PATH"
      env N48_ROOT=$W/root STAGE=$stage FAST_SLEEP=6 WATCH_MIN=${WATCH_MIN:-3} REST_MIN_S=${REST_MIN_S:-20} PRE_MAX_S=60 MONA_WAIT_S=40 $ENVS bash "$KIT" ) >"$out" 2>&1
    local rc=$?
    local log; log=$(ls -d "$W"/root/notes/logs/runs/m6-s2/*/ 2>/dev/null | head -1)
    [ -f "${log}summary.txt" ] && cat "${log}summary.txt" >>"$out"      # the summary (warnings, verdicts) goes to summary.txt / run.log, not to stdout
    local ok=1 why=""
    [ "$rc" = "$X_RC" ] || { ok=0; why="$why rc $rc != $X_RC;"; }
    local p
    while IFS= read -r p; do [ -n "$p" ] || continue; /usr/bin/grep -Eq -- "$p" "$out" || { ok=0; why="$why missing /$p/;"; }; done <<<"$X_LOG"
    while IFS= read -r p; do [ -n "$p" ] || continue; /usr/bin/grep -Eq -- "$p" "$out" && { ok=0; why="$why unexpected /$p/;"; }; done <<<"$X_NOLOG"
    for p in $X_KILL; do [ -e "$FR/private/tmp/n48m-noflip$p" ] || { ok=0; why="$why kill file $p absent;"; }; done
    for p in $X_NOKILL; do [ ! -e "$FR/private/tmp/n48m-noflip$p" ] || { ok=0; why="$why kill file $p PRESENT;"; }; done
    if [ $ok = 1 ]; then PASSN=$((PASSN+1)); printf 'PASS  %-34s rc=%s\n' "$name" "$rc"; else FAILN=$((FAILN+1)); FAILED="$FAILED $name"; printf 'FAIL  %-34s rc=%s %s\n        (work dir kept: %s)\n' "$name" "$rc" "$why" "$W"; KEEP=1; fi
    if [ $KEEP = 0 ]; then case "$W" in "$SCR"/scen.*) rm -r "$W" ;; esac; fi
}

# ---------------------------------------------------------------------------------------------- scenarios
# mk <name> <stage> <big|small>: a world with the default timeline (monitor B flips at +14 s, monitor A at +18 s); go: run the kit
mk() { NAME=$1; STG=$2; world "$2"; [ "${3:-big}" = big ] && c pool_big 1; return 0; }
go() { run_kit "$NAME" "$STG"; }
nob1() { printf 'scanout (instance %%d, %%s): counters: presents\ninst_geom_mismatch\nn48m-noflip1\n' >"$FR/Library/GPUBundles/Navi48Metal.bundle/Contents/MacOS/Navi48Metal"; }
# ---- passes
sc_pass_2b_big()            { mk pass_2b_big 2b big;   expect_rc 0; expect_log 'STAGE 2b: PASS'; expect_log 'R0 PASS '; expect_log 'POOL PASS  \(pool class BIG'; expect_log 'C7. PASS \( monitor A-leg: PASS'; expect_kill 1; expect_kill 2; go; }
sc_pass_2a()                { mk pass_2a 2a small;     expect_rc 0; expect_log 'STAGE 2a: PASS'; expect_log 'monitor A-leg: N/A \(STAGE 2a'; expect_log '2a monitor A: PASS'; expect_kill 1; expect_kill 2; go; }
sc_pass_2b_small_monb_first() { mk pass_2b_small_monbfirst 2b small; expect_rc 0; expect_log 'STAGE 2b: PASS'; expect_log 'pool-refused: monitor A'; expect_log 'monitor A-leg: N/A'; expect_log 'POOL PASS  \(pool class SMALL'; go; }
sc_pass_2b_small_mona_first() { mk pass_2b_small_monafirst 2b small; c flip_at_1 14; c flip_at_2 18; expect_rc 0; expect_log 'STAGE 2b: PASS'; expect_log 'pool-refused: monitor B'; expect_log 'monitor B-leg: N/A'; go; }
sc_pass_idle_dp()           { mk pass_idle_dp 2b big; c srate0 2; expect_rc 0; expect_log 'STAGE 2b: PASS'; expect_log 'DP [0-9]* idle'; go; }
sc_pass_all_idle_warns()    { mk pass_all_idle 2b big; c srate0 0; c srate1 0; c srate2 0; expect_rc 0; expect_log 'STAGE 2b: PASS'; expect_log 'no display was ACTIVE'; go; }
sc_pass_sysprof_two_warns() { mk pass_sysprof_two 2b big; c sysprof two; expect_rc 0; expect_log 'system_profiler listed 2 display'; go; }
sc_pass_crca1_other_warns() { mk pass_crca1_other 2b big; c crc_a1_other 1; expect_rc 0; expect_log 'monitor A CRC_A1 measured'; go; }
sc_pass_ids4_warns()        { mk pass_ids4 2b big; c ids2 4; expect_rc 0; expect_log 'learned 4 surface IDs'; go; }
sc_pass_skipkill()          { mk pass_skipkill 2b big; E SKIP_KILL=1; expect_rc 0; expect_log "C7' SKIPPED"; expect_nokill 1; expect_nokill 2; go; }
sc_pass_userstep()          { mk pass_userstep 2b big; E USER_STEP=1; expect_rc 0; expect_log "C9' NOT-OBSERVED"; go; }
# ---- refusals / stops before anything flips
sc_refuse_2b_no_b1()        { mk refuse_2b_no_b1 2b big; nob1; expect_rc 2; expect_log 'REFUSED .*needs the R-B1 fix'; expect_nokill 1; expect_nokill 2; expect_nolog 'fbhold 2: status'; go; }
sc_pass_2a_no_b1_warns()    { mk pass_2a_no_b1 2a small; nob1; expect_rc 0; expect_log 'R-B1 token missing'; go; }
sc_refuse_dp_fallback()     { mk refuse_dp_fallback 2b big; c raster_fallback 1; expect_rc 2; expect_log '1080p FALLBACK'; expect_nolog 'P2 monitor B chain \(Run B.*\n.*dmub-pclk'; go; }
sc_stop_unreachable_p0()    { mk stop_unreach_p0 2b big; c unreach_abs 0; expect_rc 3; expect_log 'PC needs a manual reset'; go; }
sc_stop_missing_latch()     { mk stop_missing_latch 2b big; c bootargs 'navi48-m6=1 navi48-m6flip=1 navi48-fb2=1 navi48-metal-disp=1 navi48-disp2=1 navi48-dmubcmd=1'; expect_rc 1; expect_log 'requirement failed: boot-args carry ALL THREE latches'; go; }
sc_stop_latch1_off_in_cli() { mk stop_latch1_off 2b big; c latch1_off 1; expect_rc 1; expect_log 'requirement failed: m6xstat 1: all THREE latches ON'; go; }
sc_stop_uuid_pin()          { mk stop_uuid_pin 2b big; E WANT_BUNDLE_UUID=DEADBEEF; expect_rc 1; expect_log 'requirement failed: bundle LC_UUID contains'; go; }
sc_stop_mona_chain_disturbs_monb() { mk stop_chain_disturb 2b big; c disturb_chain 1; expect_rc 1; expect_log 'mona-timing: ssh/CLI failed'; go; }
sc_stop_pipes2()            { mk stop_pipes2 2b big; c pipes_adopt 2; expect_rc 1; expect_log 'requirement failed: pipeadopt: 3 pipes'; go; }
sc_stop_s1_order()          { mk stop_s1_order 2b big; c fbid_swap 1; expect_rc 1; expect_log 'S1: the monitor B.s Navi48Framebuffer entry ID is not lower'; go; }
sc_stop_fbnodes_never()     { mk stop_fbnodes 2b big; c fb_delay 9999; expect_rc 1; expect_log 'expected 1 registered Navi48Framebuffer'; go; }
sc_stop_unreachable_after_restart() { mk stop_unreach_restart 2b big; c unreach_after_restart 30; expect_rc 3; expect_log 'PC unreachable during the sampling'; go; }
sc_stop_login()             { mk stop_login 2b big; c login_at 40; expect_rc 1; expect_log 'a console user appeared'; expect_nokill 1; expect_nokill 2; go; }
# ---- verdict failures that do not abort
sc_fail_r0_nfb2()           { mk fail_r0_nfb2 2b big; c nfb 2; expect_rc 1; expect_log 'R0 FAIL'; go; }
sc_fail_k7_epmask()         { mk fail_k7_epmask 2b big; c epmask 3; expect_rc 1; expect_log 'K7 FAIL'; expect_log 'endpoint bitmask 0x3'; go; }
sc_fail_k7_sysprof_swap()   { mk fail_k7_swap 2b big; c sysprof swap; expect_rc 1; expect_log 'K7 FAIL'; expect_log 'SWAP signature'; go; }
sc_fail_k2_mona_no_txn()    { mk fail_k2 2b big; c txn1_zero 1; expect_rc 1; expect_log 'K2 FAIL'; go; }
sc_fail_k1_nopipe()         { mk fail_k1_nopipe 2b big; c nopipe_lines 1; expect_rc 1; expect_log 'K1 FAIL'; go; }
sc_fail_k1_compositor2()    { mk fail_k1_comp 2b big; c compositor_lines 2; expect_rc 1; expect_log "compositor activated' lines 2"; go; }
sc_fail_k5_mona_stamp_refused() { mk fail_k5 2b big; c stamp_ref1 3; expect_rc 1; expect_log 'K5 FAIL'; go; }
sc_fail_pool_both_refused() { mk fail_pool_both 2b small; c poolref_1 1; c poolref_2 1; expect_rc 1; expect_log 'no flip within'; go; }
sc_fail_pool_big_one_refused() { mk fail_pool_big1 2b big; c poolref_1 1; expect_rc 1; expect_log 'POOL FAIL'; expect_log 'BIG pool: every enabled instance must acquire'; go; }
sc_fail_mona_never_acquires() { mk fail_mona_never 2b big; c flip_at_1 100000; expect_rc 1; expect_log "C1' FAIL"; expect_log 'neither acquired nor pool-refused'; go; }
sc_fail_c3_short_rest()     { mk fail_c3_rest 2b big; REST_MIN_S=300; E REST_MIN_S=300; expect_rc 1; expect_log "C3' FAIL"; expect_log 'at rest'; go; }
sc_fail_2a_wrong_init()     { mk fail_2a_wrong_init 2a small; c force_init_enabled 1; expect_rc 1; go; }
# ---- the abort list: each exits 1 through flip_abort and touches ONLY the affected instance's kill file (both for the global ones)
sc_abort_uf1()              { mk abort_uf1 2b big; c uf_at_1 40; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1\)'; expect_log 'HUBP1 underflow'; expect_kill 1; expect_nokill 2; go; }
sc_abort_uf2()              { mk abort_uf2 2b big; c uf_at_2 40; expect_rc 1; expect_log '### ABORT .*instance\(s\): 2\)'; expect_log 'HUBP2 underflow'; expect_kill 2; expect_nokill 1; go; }
sc_abort_vmf()              { mk abort_vmf 2b big; c vmf_at 40; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1 2'; expect_log 'DCN_VM_FAULT_STATUS'; expect_kill 1; expect_kill 2; go; }
sc_abort_hung()             { mk abort_hung 2b big; c hung_at 40; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1 2'; expect_log "HUNG '1'"; expect_kill 1; expect_kill 2; go; }
sc_abort_wspid()            { mk abort_wspid 2b big; c wspid_change_at 40; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1 2'; expect_log 'WindowServer pid'; expect_kill 1; expect_kill 2; go; }
sc_abort_writer_refusal1()  { mk abort_wref1 2b big; c wref_at_1 40; WATCH_MIN=4; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1\)'; expect_log 'writer refusals 1'; expect_kill 1; expect_nokill 2; go; }
sc_abort_write_failure2()   { mk abort_wfail2 2b big; c wfail_at_2 40; WATCH_MIN=4; expect_rc 1; expect_log '### ABORT .*instance\(s\): 2\)'; expect_log 'write failures 1'; expect_kill 2; expect_nokill 1; go; }
sc_abort_restore_failed1()  { mk abort_resfail1 2b big; c resfail_at_1 40; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1\)'; expect_log 'restore failures 1'; expect_kill 1; expect_nokill 2; go; }
sc_abort_high_dword2()      { mk abort_high2 2b big; c hihigh_at_2 40; expect_rc 1; expect_log '### ABORT .*instance\(s\): 2\)'; expect_log 'HIGH dword'; expect_kill 2; go; }
sc_abort_outside_set1()     { mk abort_outside1 2b big; c outside_at_1 40; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1\)'; expect_log 'outside \{A1, its slots\}'; expect_kill 1; expect_nokill 2; go; }
sc_abort_other_display_slot() { mk abort_swap1 2b big; c swap_addr_at_1 40; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1\)'; expect_log "EQUALS the OTHER display"; expect_kill 1; go; }
sc_abort_flip_pending_stuck2() { mk abort_fp2 2b big; c fp_stuck_at_2 40; expect_rc 1; expect_log '### ABORT .*instance\(s\): 2\)'; expect_log 'FLIP_PENDING2 stuck'; expect_kill 2; go; }
sc_abort_gpu_held_code4()   { mk abort_gh4 2b big; c gh4_at_1 40; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1\)'; expect_log 'GPU-HELD code 4'; expect_kill 1; go; }
sc_abort_watchdog2()        { mk abort_wdog2 2b big; c wdog_at_2 40; expect_rc 1; expect_log '### ABORT .*instance\(s\): 2\)'; expect_log 'watchdog restores 1'; expect_kill 2; go; }
sc_abort_otg_stuck1()       { mk abort_otg1 2b big; c otgstuck_1 1; c otgstuck_at_1 40; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1\)'; expect_log 'OTG1 frame counter did not move'; expect_kill 1; go; }
sc_abort_dp_floor()         { mk abort_dpfloor 2b big; c srate0_drop_at 70; WATCH_MIN=5; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1 2'; expect_log 'fell below its floor'; expect_kill 1; expect_kill 2; go; }
sc_abort_dp_otg0_stopped()  { mk abort_otg0 2b big; c otg0_stop_at 40; expect_rc 1; expect_log "OTG0 is not COUNTING"; expect_kill 1; expect_kill 2; go; }
sc_abort_geometry_word1()   { mk abort_geomword1 2b big; c geomword_at_1 24; WATCH_MIN=4; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1\)'; expect_log "plane geometry word"; expect_kill 1; expect_nokill 2; go; }
sc_abort_geom_mismatch_rising() { mk abort_georise1 2b big; c georise_at_1 45; WATCH_MIN=4; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1\)'; expect_log 'inst_geom_mismatch of instance 1 is still RISING'; expect_kill 1; go; }
sc_abort_monb_stall()       { mk abort_monbstall 2b big; c flip_at_1 26; c monbstall_at 27; WATCH_MIN=4; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1\)'; expect_log 'monitor B STALLED'; expect_kill 1; expect_nokill 2; go; }
sc_abort_leaked()           { mk abort_leak 2b big; c leak_at 40; WATCH_MIN=4; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1 2'; expect_log 'LEAKED'; expect_kill 1; expect_kill 2; go; }
sc_abort_monbwatch_row4()   { mk abort_monbw4 2b big; c monbw_row_at 40; c monbw_row 4; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1 2'; expect_log 'monitor B watch changed mask'; expect_kill 1; expect_kill 2; go; }
sc_abort_monbwatch_row8()   { mk abort_monbw8 2b big; c monbw_row_at 40; c monbw_row 8; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1 2'; expect_log 'monitor B watch changed mask'; go; }
sc_abort_monbwatch_row6_monb_not_acquired() { mk abort_monbw6 2b big; c poolref_2 1; c monbw_row_at 40; c monbw_row 6; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1 2'; expect_log 'NOT acquired'; go; }
sc_abort_table_generation() { mk abort_tgen 2b big; c tgen_other 1; WATCH_MIN=4; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1 2'; expect_log 'cached table generation'; expect_kill 1; expect_kill 2; go; }
sc_abort_slot_overlap()     { mk abort_overlap 2b big; c overlap_slot 1; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1 2'; expect_log 'OVERLAP'; go; }
sc_abort_2a_mona_moves()    { mk abort_2a_moves 2a small; c crc1_leave_at 30; expect_rc 1; expect_log '### ABORT .*instance\(s\): 1\)'; expect_log 'STAGE 2a: the monitor A moved'; expect_kill 1; expect_nokill 2; go; }
sc_abort_kill_does_not_restore() { mk abort_killfail 2b big; c uf_at_2 40; c kill_ignore_2 1; expect_rc 1; expect_log 'REBOOT WITHOUT FREEING ANYTHING'; expect_kill 2; go; }
main() {
    local names=("$@") n
    if [ ${#names[@]} -eq 0 ]; then names=($(declare -F | awk '{print $3}' | /usr/bin/grep '^sc_' | sed 's/^sc_//')); fi
    for n in "${names[@]}"; do "sc_$n"; done
    echo "== $PASSN passed, $FAILN failed${FAILED:+:$FAILED}"
    [ $FAILN = 0 ]
}
main "$@"
