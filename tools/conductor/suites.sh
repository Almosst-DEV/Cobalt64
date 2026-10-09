#!/bin/bash
# suites.sh: run every host suite in src/navi48-bringup/tests with the compile line from each
# file's own header, sequentially, and print one line per suite plus a totals line.
#
# usage: tools/conductor/suites.sh <tree-root> <out-dir>
#   <tree-root>  the checkout or worktree to test (for example /path/to/navi48-checkout)
#   <out-dir>    where binaries (<out-dir>/bin) and logs (<out-dir>/logs) go; use a scratch dir,
#                never the tree itself
#
# Not covered here (run them separately, as every build brief lists them):
#   make -C src/xlat12 test; make -C src/xlat12 kextcheck; python3 tools/dcn41/build.py kextcheck
# Covered here since 0.0.513: the generated-rows check (emit_ws.py --check src/xlat12 --adopted) and completeness.py.
#
# The host Mac has 16 GiB and has panicked under heavy parallel load: this script is strictly sequential and
# refuses to start under 25% free memory (set SUITES_IGNORE_MEMORY=1 to override after checking).
# A test file that is not in the list below is printed as UNLISTED so a new suite is never skipped
# silently: add its compile line from its header.

if [ $# -ne 2 ]; then
  echo "usage: $0 <tree-root> <out-dir>" >&2
  exit 2
fi
WT=$(cd "$1" 2>/dev/null && pwd) || { echo "no such tree: $1" >&2; exit 2; }
OUT=$2
mkdir -p "$OUT/bin" "$OUT/logs" || exit 2
OUT=$(cd "$OUT" && pwd)
B=$OUT/bin
L=$OUT/logs

case "$OUT/" in
  "$WT"/*) echo "refusing: <out-dir> is inside the tree ($OUT)" >&2; exit 2 ;;
esac

free=$(memory_pressure -Q 2>/dev/null | awk '/free percentage/ {gsub("%","",$NF); print $NF}')
if [ -n "${free:-}" ] && [ "$free" -lt 25 ] && [ "${SUITES_IGNORE_MEMORY:-0}" != 1 ]; then
  echo "refusing: memory free ${free}% < 25% (see the air-memory rule)" >&2
  exit 3
fi

cd "$WT" || exit 2
A=src/navi48-bringup/src/apple
T=src/navi48-bringup/tests
AHH=$A/AppleHardwareHook.cpp
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
W="-std=c++17 -Wall -Wextra -Werror -O1"
Wn="-std=c++17 -Wall -Wextra -O1"

NRUN=0; NBAD=0; NBUILD=0
LISTED=" "
run() { # name, compile..., --, runargs...
  local name=$1; shift
  local cc=()
  while [ "$1" != "--" ]; do cc+=("$1"); shift; done
  shift
  LISTED="$LISTED$name "
  NRUN=$((NRUN + 1))
  if ! "${cc[@]}" -o "$B/$name" > "$L/$name.build" 2>&1; then
    echo "$name: BUILD FAILED"
    tail -5 "$L/$name.build"
    NBUILD=$((NBUILD + 1))
    return
  fi
  "$B/$name" "$@" > "$L/$name.out" 2>&1
  local rc=$?
  [ $rc -ne 0 ] && NBAD=$((NBAD + 1))
  echo "$name: rc=$rc :: $(tail -1 "$L/$name.out" | cut -c1-200)"
}

run ctx_latch clang++ $W $SAN -I $A $T/ctx_latch_test.cpp --
run dcn_liveraster clang++ $W $SAN -I $A -I src/navi48-bringup/src/dcn -I src/dcn41 -x c++ $T/dcn_liveraster_test.cpp src/dcn41/dcn41_core.c src/dcn41/dcn41_otg.c -- src/navi48-bringup/src/dcn/navi48_dcn.cpp src/navi48-bringup/src/Navi48Bringup.cpp $A/DisplayPipeGuard.cpp
run display_pipe_guard clang++ $Wn -I $A $T/display_pipe_guard_test.cpp --
run f3_reader clang++ $W $SAN -I $A $T/f3_reader_test.cpp --
run fbname_scan clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -I src/navi48-bringup/src/dcn $T/fbname_scan_test.cpp --
run gfx_admit112 clang++ $W $SAN -I $A -I src/xlat12 -I src/xlat12/tests -x c++ $T/gfx_admit112_test.cpp src/xlat12/xlat12.c src/xlat12/xlat12_ib.c -- $AHH src/xlat12/xlat12_ib.c   # build 0.0.554: switch 112 admit stale
run gfx_capture_scan clang++ $Wn -fsanitize=address,undefined -I $A $T/gfx_capture_scan_test.cpp --
run gfx_classa clang++ $W $SAN -I $A $T/gfx_classa_test.cpp --
run gfx_clock88 clang++ $W $SAN -I $A -I src/xlat12 $T/gfx_clock88_test.cpp -- src/navi48-bringup/src/Navi48Bringup.cpp $AHH $A/Navi48AccelPeer.cpp
run gfx_commit clang++ $W $SAN -I $A $T/gfx_commit_test.cpp -- $AHH
run gfx_copyguard clang++ $W $SAN -I $A -I src/xlat12 -x c++ $T/gfx_copyguard_test.cpp -- src/navi48-bringup/src/Navi48Bringup.cpp $AHH $A/Navi48AccelPeer.cpp $A/Navi48Ttl.hpp
run gfx_cycle515 clang++ $W $SAN -I $A $T/gfx_cycle515_test.cpp -- $AHH
run gfx_cycle80 clang++ $W $SAN -I $A -I $T $T/gfx_cycle80_test.cpp -- $AHH src/navi48-bringup/src/Navi48Bringup.cpp
run gfx_present73 clang++ $W $SAN -I $A $T/gfx_present73_test.cpp -- $AHH $A/DisplayPipeGuard.cpp $A/Navi48AccelPeer.cpp
run gfx_walknop75 clang++ $W $SAN -I $A $T/gfx_walknop75_test.cpp -- $AHH $A/gfx_commit.h
run gfx_spill clang++ $W $SAN -I $A -I src/xlat12 -x c++ $T/gfx_spill_test.cpp src/xlat12/xlat12_ib.c src/xlat12/xlat12.c -- $AHH
run gfx_rnforgive clang++ $W $SAN -I $A $T/gfx_rnforgive_test.cpp -- $AHH
run gfx_cgredo clang++ $W $SAN -I $A -I src/xlat12 -x c++ $T/gfx_cgredo_test.cpp src/xlat12/xlat12_ib.c src/xlat12/xlat12.c -- $AHH src/navi48-bringup/src/Navi48Bringup.cpp
run gfx_flipmode clang++ $W $SAN -I $A $T/gfx_flipmode_test.cpp -- $A/DisplayPipeGuard.cpp src/navi48-bringup/src/Navi48Bringup.cpp $AHH $A/gfx_commit.h src/navi48-bringup/src/dcn/navi48_dcn.cpp
run gfx_judge_unbounded clang++ $W $SAN -I $A $T/gfx_judge_unbounded_test.cpp -- $AHH
run gfx_dep clang++ $W $SAN -I $A -I src/xlat12 -I src/xlat12/tests -x c++ $T/gfx_dep_test.cpp src/xlat12/xlat12.c src/xlat12/xlat12_ib.c -- $AHH
run gfx_desc_port clang++ $W $SAN -I $A $T/gfx_desc_port_test.cpp -- $AHH
run gfx_descsw clang++ $W $SAN -I $A -I src/xlat12 $T/gfx_descsw_test.cpp -- $AHH
run gfx_e1 clang++ $W $SAN -I $A $T/gfx_e1_test.cpp --
run gfx_f84 clang++ $W $SAN -I $A -I src/xlat12 -x c++ $T/gfx_f84_test.cpp src/xlat12/xlat12.c src/xlat12/xlat12_ib.c --
run gfx_fence828 clang++ $W $SAN -I $A $T/gfx_fence828_test.cpp --
run gfx_fillset clang++ $W $SAN -I $A $T/gfx_fillset_test.cpp --
run gfx_fs85 clang++ $W $SAN -I $A -I $T $T/gfx_fs85_test.cpp -- $AHH $A/gfx_commit.h $A/gfx_fs85.h
run gfx_fslearn clang++ $W $SAN -I $A -I $T $T/gfx_fslearn_test.cpp -- $AHH $A/gfx_fillset.h   # build 0.0.548: switch 106 LEARNED / SHADOW (the learned fill set of switch 33)
run gfx_p86 clang++ $W $SAN -I $A $T/gfx_p86_test.cpp -- $AHH $A/Navi48AccelPeer.cpp src/navi48-bringup/src/Navi48Bringup.cpp src/xlat12/xlat12_ib.c src/xlat12/xlat12_dtable_rows.inc src/xlat12/xlat12_shader_ids.h
run gfx_p94 clang++ $W $SAN -I $A $T/gfx_p94_test.cpp -- $AHH $A/DisplayPipeGuard.cpp src/navi48-bringup/src/Navi48Bringup.cpp $A/gfx_commit.h $A/gfx_p94.h $A/gfx_p95.h   # build 0.0.538: switches 94 and 95
run gfx_flightring clang++ $W $SAN -I $A $T/gfx_flightring_test.cpp -- $AHH
run gfx_forgive clang++ $W $SAN -I $A $T/gfx_forgive_test.cpp --
run gfx_hw72 clang++ $W $SAN -I $A $T/gfx_hw72_test.cpp -- $AHH   # build 0.0.532: switch 72 M3 (T27)
run gfx_hg88 clang++ $W $SAN -I $A $T/gfx_hg88_test.cpp -- $AHH $A/Navi48AccelPeer.cpp $A/gfx_commit.h   # build 0.0.533: switch 88 (T33)
run gfx_fc89 clang++ $W $SAN -I $A $T/gfx_fc89_test.cpp -- $AHH src/navi48-bringup/src/Navi48Bringup.cpp $A/gfx_commit.h   # build 0.0.534: switches 89 and 90 (T34)
run gfx_nclear clang++ $W $SAN -I $A -I src/xlat12 -I $T -x c++ $T/gfx_nclear_test.cpp src/xlat12/xlat12_ib.c src/xlat12/xlat12.c -- $AHH   # build 0.0.535: switch 91 + tex531's draw state
run gfx_rect92 clang++ $W $SAN -I $A -I src/xlat12 -I src/shadercache -I $T -x c++ $T/gfx_rect92_test.cpp src/xlat12/xlat12_ib.c src/xlat12/xlat12.c src/shadercache/shadercache.c -- $AHH $A/Navi48AccelPeer.cpp src/xlat12/xlat12_ib.c   # build 0.0.536: switch 92 (RECT_2D + the v3 VS image)
run gfx_pws93 clang++ $W $SAN -I $A -I src/xlat12 -I $T -x c++ $T/gfx_pws93_test.cpp src/xlat12/xlat12_ib.c src/xlat12/xlat12.c -- $AHH src/xlat12/xlat12_ib.c   # build 0.0.537: switch 93 (Apple's barrier waits: PWS)
run gfx_perf540 clang++ $W $SAN -I $A $T/gfx_perf540_test.cpp -- $AHH src/navi48-bringup/src/Navi48Bringup.cpp src/navi48-bringup/src/amd/n48log.cpp $A/Navi48Ttl.hpp   # build 0.0.540: switch 96 (perf540)
run gfx_tc97 clang++ $W $SAN -I $A -I src/xlat12 -x c++ $T/gfx_tc97_test.cpp src/xlat12/xlat12_ib.c src/xlat12/xlat12.c -- $AHH src/xlat12/xlat12_ib.c   # build 0.0.540 item 5: switch 97 (the verifier's table cache)
run gfx_wc98 clang++ $W $SAN -I $A $T/gfx_wc98_test.cpp -- $AHH src/navi48-bringup/src/Navi48Bringup.cpp   # build 0.0.541: switch 98 (the provenance-ask walk cache)
run gfx_wo99 clang++ $W $SAN -I $A -I $T $T/gfx_wo99_test.cpp -- $AHH $A/gfx_dep.h $A/gfx_cp_build.h   # build 0.0.541 item 6: switch 99 (the witness-overflow relaxation)
run gfx_long543 clang++ $W $SAN -I $A $T/gfx_long543_test.cpp -- $AHH   # build 0.0.543 item D: switch 102 (the longer continuous N/T)
run gfx_hm100 clang++ $W $SAN -I $A $T/gfx_hm100_test.cpp -- $AHH   # build 0.0.543 item A: switch 100 (the per-call host-page map cache)
run gfx_stale103 clang++ $W $SAN -I $A $T/gfx_stale103_test.cpp -- $AHH $A/gfx_commit.h   # build 0.0.544: switch 103 (the stale-input gate + instruments)
run gfx_late540 clang++ $W $SAN -I $A $T/gfx_late540_test.cpp -- $AHH   # build 0.0.540 item 6: late-phase evidence (capture window, gate reason, segment, tex531 late)
run gfx_keystone clang++ $W $SAN -I $A $T/gfx_keystone_test.cpp -- $AHH
run gfx_ks81 clang++ $W $SAN -I $A $T/gfx_ks81_test.cpp -- $AHH src/navi48-bringup/src/Navi48Bringup.cpp $A/gfx_commit.h
run gfx_lutfill clang++ $W $SAN -I $A $T/gfx_lutfill_test.cpp --
run gfx_memdst clang++ $W $SAN -I $A -I src/xlat12 -I src/xlat12/tests -x c++ $T/gfx_memdst_test.cpp src/xlat12/xlat12.c src/xlat12/xlat12_ib.c src/xlat12/xlat12_headless.c --
run gfx_mib clang++ $W $SAN -I $A -I src/xlat12 -x c++ $T/gfx_mib_test.cpp src/xlat12/xlat12_ib.c src/xlat12/xlat12.c -- $AHH
run gfx_mmprio clang++ $W $SAN -I $A $T/gfx_mmprio_test.cpp -- src/navi48-bringup/src/Navi48Bringup.cpp $AHH $A/gfx_desc_port.h src/xlat12/xlat12_ib.c
run gfx_neuter clang++ $Wn -I $A $T/gfx_neuter_test.cpp --
run gfx_pgmid clang++ $W $SAN -I $A -I src/xlat12 -x c++ $T/gfx_pgmid_test.cpp src/xlat12/xlat12.c src/xlat12/xlat12_ib.c -- $AHH
run gfx_probe_plan clang++ $Wn -fsanitize=address,undefined -I $A $T/gfx_probe_plan_test.cpp --
run gfx_ramtop clang++ $W $SAN -I $A $T/gfx_ramtop_test.cpp --
run gfx_rasterarm clang++ $W $SAN -I $A -I src/xlat12 $T/gfx_rasterarm_test.cpp -- $AHH
run gfx_rasterarm_pd clang++ $W $SAN -I $A -I src/xlat12 $T/gfx_rasterarm_pd_test.cpp -- $AHH
run gfx_reloc clang++ $W $SAN -I $A -I src/xlat12 $T/gfx_reloc_test.cpp --
run gfx_ringidle clang++ $W $SAN -I $A $T/gfx_ringidle_test.cpp -- src/xlat12/xlat12_ib.c $AHH
run gfx_rpunmapq clang++ $W $SAN -pthread -I $A $T/gfx_rpunmapq_test.cpp --
run gfx_sk82 clang++ $W $SAN -I $A -I src/xlat12 -I $T $T/gfx_sk82_test.cpp -- $A/Navi48AccelPeer.cpp src/navi48-bringup/src/Navi48Bringup.cpp $AHH $A/gfx_commit.h
run gfx_tlb83 clang++ $W $SAN -I $A $T/gfx_tlb83_test.cpp -- src/navi48-bringup/src/amd/gmc_v12_0.cpp src/navi48-bringup/src/amd/amdgpu_regs.h src/navi48-bringup/src/Navi48Bringup.cpp $AHH $A/gfx_commit.h src/navi48-bringup/src/amd
run gfx_cg84 clang++ $W $SAN -I $A -I src/xlat12 -I $T $T/gfx_cg84_test.cpp -- src/navi48-bringup/src/Navi48Bringup.cpp $AHH $A/Navi48AccelPeer.cpp $A/gfx_copyguard.h $A/gfx_commit.h
run gfx_src_decide clang++ $W $SAN -I $A $T/gfx_src_decide_test.cpp --
run gfx_t0src clang++ $W $SAN -I $A $T/gfx_t0src_test.cpp -- $AHH $A/Navi48AccelPeer.cpp $A/gfx_t0src.h
run gfx_tgtsample clang++ $W $SAN -I $A $T/gfx_tgtsample_test.cpp --
run gfx_xlat_verdict clang++ $Wn -fsanitize=address,undefined -I $A $T/gfx_xlat_verdict_test.cpp --
run pairing_policy clang++ $Wn -I $A $T/pairing_policy_test.cpp --
run pipeshim_verify clang++ $W -I $A $T/pipeshim_verify_test.cpp --
run rootwrite_guards clang++ $Wn -I $A $T/rootwrite_guards_test.cpp --
# N48_AUX_ROOT (optional): the tree whose tools/native/navi48accel holds the aux kext, for the ops-header identity check (default: this tree; set it while the aux kext 0.0.3 is not yet merged here)
run native_metal clang++ $W $SAN -I src/navi48-bringup/src -I src/navi48-bringup/src/amd $T/native_metal_test.cpp -- . "${N48_AUX_ROOT:-.}"   # build 0.0.610: the Metal nub + ops table (milestone #9); the plant script is tests/native_metal_plant.sh
run native_disp clang++ $W $SAN -I src/navi48-bringup/src -I src/navi48-bringup/src/amd $T/native_disp_test.cpp -- . "${N48_AUX_ROOT:-.}"   # build 0.0.613: the display pipe (#11 11h.2); the plant script is tests/native_disp_plant.sh
run native_agdc clang++ $W $SAN -I src/navi48-bringup/src -I src/navi48-bringup/src/amd $T/native_agdc_test.cpp -- .   # build 0.0.614: the native AGDC service (#11 11h.3, accel action 88); the plant script is tests/native_agdc_plant.sh
run native_rebar clang++ $W $SAN -I src/navi48-bringup/src -I src/navi48-bringup/src/amd $T/native_rebar_test.cpp -- .   # build 0.0.663: Resizable BAR planning, the census, the latch union, the aperture points, vramstat (REBAR.md section 3); the plant script is tests/native_rebar_plant.sh
if clang -std=c11 -O1 -c src/shadercache/shadercache.c -o "$B/shadercache.o" 2> "$L/sc.build"; then
  run sc_census clang++ $Wn -I $A -I src/shadercache $T/sc_census_test.cpp "$B/shadercache.o" --
else
  LISTED="${LISTED}sc_census "; NRUN=$((NRUN + 1)); NBUILD=$((NBUILD + 1))
  echo "sc_census: BUILD FAILED (shadercache.o)"; tail -5 "$L/sc.build"
fi
run scanout_copy clang++ $Wn -I $A $T/scanout_copy_test.cpp --
run scanout_selftest clang++ $W -I $A $T/scanout_selftest_test.cpp --
run scanout_full clang++ $W $SAN -I $A -I src/navi48-bringup/src/dcn -I src/dcn41 -x c++ $T/scanout_full_test.cpp src/dcn41/dcn41_core.c src/dcn41/dcn41_otg.c -- src/navi48-bringup/src/Navi48Bringup.cpp src/navi48-bringup/src/dcn/navi48_dcn.cpp src/navi48-bringup/src/Navi48UserClient.cpp   # build 0.0.542: `accel scanout full`
run sdma_dcc clang++ $W -I $A $T/sdma_dcc_test.cpp --
run sdma_drain_xlat clang++ $Wn -I $A $T/sdma_drain_xlat_test.cpp --
run sdma_gcr clang++ $W -I $A $T/sdma_gcr_test.cpp --
run test_gfx12_descriptors c++ -std=c++17 -O1 $T/test_gfx12_descriptors.cpp --
run ws_ident clang++ $W $SAN -I $A $T/ws_ident_test.cpp --
run ws_resprov clang++ $W $SAN -I $A $T/ws_resprov_test.cpp --
run ws_valid clang++ $W $SAN -I $A $T/ws_valid_test.cpp --
run gfx_capture_synth clang++ $Wn -I $A $T/gfx_capture_synth.cpp -- "$B/synth.bin"
python3 tools/m4-xlat/capdecode.py --synth-check "$B/synth.bin" > "$L/synthcheck.out" 2>&1
rc=$?
NRUN=$((NRUN + 1)); [ $rc -ne 0 ] && NBAD=$((NBAD + 1))
echo "capdecode synth-check: rc=$rc :: $(tail -1 "$L/synthcheck.out" | cut -c1-200)"
# build 0.0.536 (switch 92): the v3 RectPosTexFast_VS images from source (assembly, the ISA checker, the one-instruction diff)
python3 tools/test_rect92_vs.py > "$L/rect92_vs.out" 2>&1
rc=$?
NRUN=$((NRUN + 1)); [ $rc -ne 0 ] && NBAD=$((NBAD + 1))
echo "rect92_vs: rc=$rc :: $(tail -1 "$L/rect92_vs.out" | cut -c1-200)"
# build 0.0.537 (switch 93): the whole captured corpus (run11v/y/z, read from the main checkout's notes/logs) - OFF identity
# against the frozen 12e011c7 translator, never a refusal, the converted counts against an independent census, the ordering.
python3 tools/test_pws93_corpus.py > "$L/pws93_corpus.out" 2>&1
rc=$?
NRUN=$((NRUN + 1)); [ $rc -ne 0 ] && NBAD=$((NBAD + 1))
echo "pws93_corpus: rc=$rc :: $(tail -1 "$L/pws93_corpus.out" | cut -c1-200)"
# build 0.0.540 item 5 (switch 97): the verifier's table cache over the whole captured corpus (run11v/y/z/ac/ad/af): ON == OFF
# (status, length, err_op / err_reg, whole-output FNV) in 7 flag modes, the real stages (modes 1, 2, decide44's full recipe) line for
# line, and the verifier >= 10x faster with the cache.
python3 tools/test_tc97_corpus.py > "$L/tc97_corpus.out" 2>&1
rc=$?
NRUN=$((NRUN + 1)); [ $rc -ne 0 ] && NBAD=$((NBAD + 1))
echo "tc97_corpus: rc=$rc :: $(tail -1 "$L/tc97_corpus.out" | cut -c1-200)"
# build 0.0.542: the `accel scanout full` decoder (tools/runkit/scanout2png.py) - the 64KB_2D table against the kext's own C
# equation, round trips (tiled from the C equation), a C-written header read back field for field, the crops, compare.
python3 tools/runkit/test_scanout2png.py > "$L/scanout2png.out" 2>&1
rc=$?
NRUN=$((NRUN + 1)); [ $rc -ne 0 ] && NBAD=$((NBAD + 1))
echo "scanout2png: rc=$rc :: $(tail -1 "$L/scanout2png.out" | cut -c1-200)"

# build 0.0.513 ( ORDER (1),): the kext's row tables ARE the generated rows (tools/auto-ws/emit_ws.py
# --policy adopt; the committed set is src/xlat12/xlat12_adopt_rows.json). A hand edit that drifts from it - a changed value
# (DIFFER), a dropped or unparseable generated row (ADDED, with --adopted), a row kind the generator stopped giving
# (MISSING-FROM-AUTO) - fails here; so does a gap in the generated set or a tree gap on an identity it covers (completeness.py).
# Rows OUTSIDE auto (m2 tests, SecurityAgent, driver-internal, SkyLight UberCompositeVertex) are reported, not judged.
python3 tools/auto-ws/emit_ws.py --check src/xlat12 --adopted > "$L/rows_check.out" 2>&1
rc=$?
NRUN=$((NRUN + 1)); [ $rc -ne 0 ] && NBAD=$((NBAD + 1))
echo "rows_check (emit_ws --check --adopted): rc=$rc :: $(tail -1 "$L/rows_check.out" | cut -c1-200)"
python3 tools/auto-ws/completeness.py --rows src/xlat12/xlat12_adopt_rows.json --tree . --strict-covered > "$L/rows_complete.out" 2>&1
rc=$?
NRUN=$((NRUN + 1)); [ $rc -ne 0 ] && NBAD=$((NBAD + 1))
echo "rows_complete (completeness --strict-covered): rc=$rc :: $(grep '^== tree' "$L/rows_complete.out" | tail -1 | cut -c1-200)"

# Any test source this list does not run.
NUNL=0
for f in $T/*_test.cpp $T/test_*.cpp $T/*_synth.cpp; do
  [ -e "$f" ] || continue
  b=$(basename "$f" .cpp)
  n=${b%_test}
  case "$LISTED" in
    *" $n "*|*" $b "*) ;;
    *) echo "UNLISTED: $f (not run; add its compile line from its header)"; NUNL=$((NUNL + 1)) ;;
  esac
done

echo "SUITES: $NRUN run, $NBAD rc!=0, $NBUILD build failed, $NUNL unlisted (tree $WT; logs $L)"
[ $NBAD -eq 0 ] && [ $NBUILD -eq 0 ] && [ $NUNL -eq 0 ]
