#!/bin/zsh
# native_s2d_plant.sh - planted breaks for tests/native_s2d_test.cpp (build 0.0.605, native S2d: the timed mode trial).
# For each plant: copy the real sources into a scratch tree, apply the break (one or several exact-once replacements; the script first proves each replaced text occurs exactly
# once), compile the real test against the scratch tree and demand that it FAILS (a compile error, a failing check, a crash or a hang cut off by the alarm all count). A plant the
# suite lets through is a hole: the script exits non-zero and says which. The CONTROL (no break) must pass.
#   run from anywhere:  src/navi48-bringup/tests/native_s2d_plant.sh [first-plant-id last-plant-id]
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
K=src/navi48-bringup
SR=$K/src
SCR="${TMPDIR:-/tmp}/n2d-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0
FIRST=${1:-0}; LAST=${2:-9999}

fresh() {
  rm -rf "$SCR"; mkdir -p "$SCR/$SR" "$SCR/$K/tests" "$SCR/tools/native" "$SCR/src/dcn41" "$SCR/notes/design"
  cp "$ROOT/$SR/"*.h "$ROOT/$SR/"*.hpp "$ROOT/$SR/"*.cpp "$SCR/$SR/"
  cp -R "$ROOT/$SR/amd" "$ROOT/$SR/dcn" "$ROOT/$SR/apple" "$SCR/$SR/"
  cp "$ROOT/$K/Makefile" "$ROOT/$K/Info.plist" "$SCR/$K/"
  cp "$ROOT/$K/tests/native_s2d_test.cpp" "$SCR/$K/tests/"
  cp "$ROOT/tools/native/n48mode.c" "$ROOT/tools/native/n48scan.c" "$ROOT/tools/native/build.sh" "$ROOT/tools/native/gen_modetrial_tables.py" "$SCR/tools/native/"
  cp "$ROOT/src/dcn41/"*.c "$ROOT/src/dcn41/"*.h "$SCR/src/dcn41/"
  cp "$ROOT/.gitignore" "$SCR/.gitignore"
}
build_run() {   # -> prints the verdict line; returns 0 if the suite PASSED, 1 if it failed, 2 if it did not compile
  local out
  if [ -n "${PLANT_DRY:-}" ]; then echo "(dry run: applicability only)"; return 1; fi   # PLANT_DRY=1 checks that every plant text is found exactly once, without compiling
  out=$(cd "$SCR" && clang++ -std=c++17 -Wall -Wextra -O0 -pthread -I $SR -I $SR/dcn -I src/dcn41 $K/tests/native_s2d_test.cpp -x c++ src/dcn41/dcn41_allow.c -o "$SCR/t" 2>&1)
  if [ $? -ne 0 ]; then echo "CAUGHT at compile time: $(echo "$out" | /usr/bin/grep -m1 -E 'error|static assertion' | cut -c1-140)"; return 2; fi
  out=$(cd "$SCR" && perl -e 'alarm 120; exec @ARGV' ./t . 2>&1)
  local trc=$?
  if [ $trc -eq 0 ]; then echo "the suite passed: $(echo "$out" | tail -1)"; return 0; fi
  if [ $trc -ge 128 ]; then echo "CAUGHT by a crash or hang (signal $((trc-128)))"; return 1; fi
  echo "CAUGHT by $(echo "$out" | /usr/bin/grep -c '^FAIL') check(s); first: $(echo "$out" | /usr/bin/grep -m1 '^FAIL' | cut -c1-120)"; return 1
}
plant() {   # plant <id> <file relative to repo root> <description> <old1> <new1> [<old2> <new2> ...]
  local id="$1" file="$2" desc="$3"; shift 3
  if [ "$id" -lt "$FIRST" ] || [ "$id" -gt "$LAST" ]; then return; fi
  fresh
  local pairs=""
  while [ $# -ge 2 ]; do pairs="${pairs}${1}"$'\x1e'"${2}"$'\x1d'; shift 2; done
  PAIRS="$pairs" FILE="$SCR/$file" python3 - <<'PY'
import os,sys
p=os.environ['FILE']; s=open(p).read()
for pair in os.environ['PAIRS'].split('\x1d'):
    if not pair: continue
    o,n=pair.split('\x1e')
    if s.count(o)!=1:
        print("PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %r" % (s.count(o), o[:80])); sys.exit(3)
    s=s.replace(o,n)
open(p,'w').write(s)
PY
  local rc=$?
  total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  local msg; msg=$(build_run); local brc=$?
  if [ $brc -eq 0 ]; then echo "PLANT $id: $desc: *** ESCAPED ($msg) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: $msg"; fi
}
control() {
  fresh; total=$((total+1))
  local msg; msg=$(build_run); local brc=$?
  if [ $brc -eq 0 ]; then echo "CONTROL (no break): $msg"; else echo "CONTROL (no break): *** FAILED: $msg ***"; escaped=$((escaped+1)); fi
}

E=$SR/dcn/navi48_modetrial_pure.h
D=$SR/dcn/navi48_dcn.cpp
T=$SR/dcn/navi48_modetrial_tables.h
control
# ---- the engine ----
plant 1 $E "the restore skips the register written FIRST" 'for (uint32_t k = st.nWritten; k-- > 0u; ) {' 'for (uint32_t k = st.nWritten; k-- > 1u; ) {'
plant 2 $E "the restore runs in FORWARD order" 'for (uint32_t k = st.nWritten; k-- > 0u; ) {' 'for (uint32_t k = 0; k < st.nWritten; k++) {'
plant 3 $E "ORDERING: the write index is recorded AFTER the write (a refused write is never restored)" '        const PlanEnt &e = st.plan[k];
        st.written[st.nWritten++] = e.idx;
        const bool ok = hw.wr(hw.ctx, e.abs, e.newv);' '        const PlanEnt &e = st.plan[k];
        const bool ok = hw.wr(hw.ctx, e.abs, e.newv);
        if (ok) st.written[st.nWritten++] = e.idx;'
plant 4 $E "clock sufficiency ignores DISPCLK" 'return d.ok && d.dentAgree && d.chgDone && d.dispKhz >= r.needDispKhz && d.dppKhz >= r.needDppKhz;' 'return d.ok && d.dentAgree && d.chgDone && d.dppKhz >= r.needDppKhz;'
plant 5 $E "clock sufficiency uses > instead of >= (the exact DML edge is refused)" 'd.dispKhz >= r.needDispKhz && d.dppKhz >= r.needDppKhz;' 'd.dispKhz > r.needDispKhz && d.dppKhz >= r.needDppKhz;'
plant 6 $E "the watchdog is never due" 'return nowUs >= deadlineUs || (nowUs > beatUs && nowUs - beatUs >= staleUs); }' 'return false; }'
plant 7 $E "the restore claim is not exclusive (both parties restore)" '    if (st.restoreClaimed) { hw.unlock(hw.ctx); return false; }
    st.restoreClaimed = true;' '    st.restoreClaimed = true;'
plant 8 $E "a failure verdict does not latch" 'constexpr bool is_failure(uint32_t v) { return v >= N48N_MODE_V_RATE && v <= N48N_MODE_V_HOLD; }' 'constexpr bool is_failure(uint32_t v) { return v > N48N_MODE_V_HOLD; }'
plant 9 $E "the latch is not consulted" '    if (p.latched) return N48N_MODE_D_LATCHED;
' ''
plant 10 $E "row 120 without a step-down is allowed" '    if (p.row == 120u && !p.step50Passed) return N48N_MODE_D_STEP_DOWN;
' ''
plant 11 $E "the rate tolerance is 100 %" 'constexpr Timing kProd = { 2000u, 500u, 3000u, 50u, 2000u, 3000u, 250u, 250u, 10u, 20u, 15000u, false };' 'constexpr Timing kProd = { 2000u, 500u, 3000u, 50u, 2000u, 3000u, 250u, 250u, 1000u, 20u, 15000u, false };'
plant 12 $E 'the DIO FIFO / steer / DIG check after the rate window is gone' '            smp.now();
            o.fifoBad = o.fifoBad || smp.fifo; o.streamLost' '            smp.now();
            o.streamLost'
plant 13 $E 'the stream check after the rate window is gone' 'smp.fifo; o.streamLost = o.streamLost || smp.stream; o.underflow = o.underflow || smp.uf; o.clockLost = o.clockLost || smp.clk;
            r->vtotal_reg[1]' 'smp.fifo; o.underflow = o.underflow || smp.uf; o.clockLost = o.clockLost || smp.clk;
            r->vtotal_reg[1]'
plant 14 $E "the frame-counter stall detector is gone" '        if (tLast - tAdv >= (uint64_t)tm.stallMs * 1000ull) { m.stalled = true; break; }
' ''
plant 15 $E "the rate windows are 1 s (under the 2 s the brief demands)" 'constexpr Timing kProd = { 2000u, 500u, 3000u, 50u, 2000u, 3000u, 250u, 250u, 10u, 20u, 15000u, false };' 'constexpr Timing kProd = { 2000u, 500u, 1000u, 50u, 2000u, 3000u, 250u, 250u, 10u, 20u, 15000u, false };'
plant 16 $E "the read-modify-write ignores the mask (whole-dword write)" 'constexpr uint32_t rmw(uint32_t cur, uint32_t mask, uint32_t val) { return (cur & ~mask) | (val & mask); }' 'constexpr uint32_t rmw(uint32_t cur, uint32_t mask, uint32_t val) { (void)cur; (void)mask; return val; }'
plant 17 $E "the post-restore register verification always passes" '    st.mismatch = bad;
    return bad;' '    st.mismatch = 0;
    return 0;'
plant 18 $E "the dwell bound is gone" '    if (p.dwellMs > kMaxDwellMs) return N48N_MODE_D_BAD_DWELL;
' ''
plant 19 $E "the busy flag does not exclude a second trial" '    if (__atomic_exchange_n(&st.busy, 1u, __ATOMIC_SEQ_CST) != 0u) { detail::deny(hw, st, r, N48N_MODE_D_BUSY); return; }   // not ours: leave it set' '    __atomic_store_n(&st.busy, 1u, __ATOMIC_SEQ_CST);'
plant 20 $E "the watchdog thread is never started" '        if (!hw.start_watchdog(hw.ctx, myId)) why = N48N_MODE_D_WATCHDOG;' '        (void)myId;'
plant 21 $E "ORDERING: the watchdog is started AFTER the writes" '        if (!hw.start_watchdog(hw.ctx, myId)) why = N48N_MODE_D_WATCHDOG;' '        (void)myId;' '    r->flags |= N48N_MODE_F_WROTE;
    if (anyFail()) failPhase = 2u;' '    r->flags |= N48N_MODE_F_WROTE;
    (void)hw.start_watchdog(hw.ctx, myId);
    if (anyFail()) failPhase = 2u;'
plant 22 $E "the baseline check always passes" '    p.baselineOk = liveOk && baseline_ok(st.live) && !health_bad(prc, steer0, dig0) && (prc & kDtoEnableBit) != 0u;   // a FIFO already unhealthy before the trial would make the post-restore check meaningless' '    p.baselineOk = true;'
plant 23 $E "the drift check always passes" '    p.driftFree = liveOk && st.goldenValid && drift_count(ri, st.live, st.golden, &firstDrift) == 0u && !st.blanked && !st.lockHeld;   // a stream left blanked or a lock left held by an earlier failure: run `dcnmode 0` first' '    p.driftFree = true;'
plant 24 $E "the clock check is not used by the gate" '    p.clocksOk = clkRead && clocks_ok(ri, clk);' '    p.clocksOk = true;'
plant 25 $E "the plane-acquired check is not used by the gate" '    p.planeAcquired = hw.plane_acquired(hw.ctx);' '    p.planeAcquired = false;'
plant 26 $E "the stream check is not used by the gate" '    p.streamOk = stream_active(dpStream) && dpOther == 0u;' '    p.streamOk = true;'
plant 27 $E "the DP stream of the wrong instance is accepted (another DP enabled)" '    p.streamOk = stream_active(dpStream) && dpOther == 0u;' '    p.streamOk = stream_active(dpStream);'
plant 28 $E "a golden copy may be taken after a write" '    if (st.everWrote) { hw.unlock(hw.ctx); return false; }' '    (void)st.everWrote;'
plant 29 $E "a second golden_take overwrites the first copy" '    if (st.goldenValid) { hw.unlock(hw.ctx); return true; }' '    (void)st.goldenValid;'
plant 30 $E "the abort request is honoured while idle (it would abort the NEXT trial)" 'inline void request_abort(const Hw &hw, State &st) { hw.lock(hw.ctx); if (__atomic_load_n(&st.busy, __ATOMIC_ACQUIRE) != 0u) st.abortReq = true; hw.unlock(hw.ctx); }' 'inline void request_abort(const Hw &hw, State &st) { hw.lock(hw.ctx); st.abortReq = true; hw.unlock(hw.ctx); }'
plant 31 $E "dcnmode 0 restores in FORWARD order" 'for (uint32_t k = kCapN; k-- > 0u; ) {
                    if (grp != kGrpAll && cap_group(k) != grp) continue;' 'for (uint32_t k = 0; k < kCapN; k++) {
                    if (grp != kGrpAll && cap_group(k) != grp) continue;'
plant 32 $E "dcnmode 0 rewrites registers that already match" '                if (cur != 0xFFFFFFFFu && (cur & kCap[k].mask) == (st.golden[k] & kCap[k].mask)) continue;' '                (void)0;'
plant 33 $E "the restore verify of the 60 Hz rate is not judged" '        rateAfterOk = !a.readFail && !a.stalled && rate_within(r->rate_after_mhz, expect_mhz(row60()), tol);' '        rateAfterOk = true;'
plant 34 $E "a failed restore is not a failure verdict" '    o.restoreBad = !restoreDone || !regsOk || !rateAfterOk || !stream_active(r->stream_after) || healthBad || repSeqBad || st.rep.ufAfterBad || repBlanked || repLockHeld;' '    o.restoreBad = false;'
plant 35 $E "the 50 Hz pass does not enable the 120 Hz row" '    if (r->verdict == N48N_MODE_V_PASS && row == 50u) st.step50Passed = true;' '    (void)row;'
# ---- the tables ----
plant 36 $T "row 50 also writes DLG registers" 'constexpr uint32_t kRow50N = 3u;
constexpr uint16_t kRow50Idx[kRow50N] = { 0, 1, 2 };' 'constexpr uint32_t kRow50N = 5u;
constexpr uint16_t kRow50Idx[kRow50N] = { 0, 1, 2, 30, 31 };'
plant 37 $T "the DTO phase register address points into the SMU mailbox" '{ 0x00000141u, 0xffffffffu, "DP_DTO0_PHASE" }' '{ 0x00016282u, 0xffffffffu, "DP_DTO0_PHASE" }'
plant 38 $T "H_TOTAL and V_TOTAL swap places in the write order" '"OTG0_OTG_H_TOTAL" }' '"@@" }' '"OTG0_OTG_V_TOTAL" }' '"OTG0_OTG_H_TOTAL" }' '"@@" }' '"OTG0_OTG_V_TOTAL" }'
plant 39 $T "the 60 Hz golden DTO phase is not the census value" 'constexpr uint32_t kGold60[kCapN] = {
    0x0e64ff60u' 'constexpr uint32_t kGold60[kCapN] = {
    0x0e64ff61u'
plant 40 $T "the 120 Hz row's DML need is understated (DISPCLK 272 MHz)" 'kRow120NeedDispKhz = 514285u' 'kRow120NeedDispKhz = 272000u'
plant 41 $T "the 50 Hz pixel clock is not the DTO's" 'kRow50PixHz = 201000000u, kRow50Modulo = 720000000u, kRow50Phase = 201000000u' 'kRow50PixHz = 201000000u, kRow50Modulo = 720000000u, kRow50Phase = 200000000u'
# ---- the kext glue ----
plant 42 $D "scanAcquire does not refuse while a trial runs" '		if (__atomic_load_n(&gMt.busy, __ATOMIC_SEQ_CST) != 0u && !n48mt::hold_allows_acquire(gMt, sess)) { N48LOG("n48scan: Acquire refused: a mode trial is running"); rc = kBusy; break; }   // 0.0.605; 0.0.609: except while a row-120 hold is UP and this session may take it (atomic words only, never the trial engine lock)
' ''
plant 43 $D "dcnmode 0 does not restore the full golden set" '		if (mt_emergency_restore("dcnmode 0")) {' '		if (false) {'
plant 44 $D "no golden copy at bind" '	mt_golden_at_bind();
	return 0;' '	return 0;'
plant 45 $D "the kext stop does not end a running trial" '	mt_shutdown();                           // build 0.0.605' '	// build 0.0.605'
plant 46 $D "the trial write bypasses the allowlist" '	if (!dcn41_allow_write(&gDcn.allow, abs, v, gDcn.tag)) {
		const char *why = nullptr;
		const int reason = dcn41_allow_classify(abs, gDcn.allow.mmio_dwords, nullptr, &why);
		N48LOG("mode-trial: REFUSED' '	if (false) {
		const char *why = nullptr;
		const int reason = dcn41_allow_classify(abs, gDcn.allow.mmio_dwords, nullptr, &why);
		N48LOG("mode-trial: REFUSED'
plant 47 $D "the trial gate accepts a native boot without S1b POSITIVE PASS" '	return s.gate == n48native::kGateOn && s.positivePass;
}
static bool mt_armed' '	return s.gate == n48native::kGateOn;
}
static bool mt_armed'
plant 48 $D "the trial accepts a second lit OTG" '	if (lit != 1u || mask != 1u) return false;' '	if (lit == 0u) return false;'
plant 50 $SR/dcn/navi48_scanout_pure.h "the native exemption is broadened to any argument" 'return arg == 0ull && (action == 74u || action == 76u || action == 77u);' 'return (action == 74u || action == 76u || action == 77u);'
plant 51 $SR/Navi48NativeClient.cpp "the selector shape is wrong (2 scalars)" 'shape(3, 0, 0, sizeof(n48n_mode_result))' 'shape(2, 0, 0, sizeof(n48n_mode_result))'
plant 52 $SR/amd/native_s1c.cpp "the selector body ignores non-zero flags" '    if ((flags & ~(uint64_t)N48N_MODE_TF_MASK) != 0ull || out == nullptr) return kIOReturnBadArgument;   // 0.0.606: the flags word carries N48N_MODE_TF_* (0.0.605 required 0)
    return (IOReturn)n48dcn::modeTrial(row, dwellMs, flags, out);' '    if (out == nullptr) return kIOReturnBadArgument;
    return (IOReturn)n48dcn::modeTrial(row, dwellMs, flags, out);'
plant 53 $SR/amd/native_s1c.h "the kext build number is stale" 'kN1cKextBuild = 664;' 'kN1cKextBuild = 606;'
plant 54 $SR/Navi48NativeABI.h "the ABI minor is not bumped" '#define N48N_ABI_MINOR     12u' '#define N48N_ABI_MINOR     4u'
plant 55 $SR/amd/smu_dal_pure.h "the DID table: divider of DID 0x40..0x5F is 4-stepped" 'did < 0x60u ? 64u + 2u * (did - 0x40u)' 'did < 0x60u ? 64u + 4u * (did - 0x40u)'
plant 56 tools/native/n48mode.c "the tool does not print the watch warning" '; WATCH THE MONITOR)' ')'

# ---- 0.0.605 review fixes ----
plant 57 $E "the restore transition is not judged (a stuck FIFO reports PASS)" '!stream_active(r->stream_after) || healthBad || repSeqBad' '!stream_active(r->stream_after) || repSeqBad'
plant 58 $E "DIO_ERROR_COUNT growth across the restore is not judged" '    const bool healthBad = health_bad(prcA, r->steer_after, digA) || r->dio_restore_errs != 0u;' '    const bool healthBad = health_bad(prcA, r->steer_after, digA);'
plant 59 $E "the steer FIFO overflow flag is not judged" 'constexpr bool steer_bad(uint32_t v) { return v == 0xFFFFFFFFu || (v & 0x10u) != 0u; }' 'constexpr bool steer_bad(uint32_t v) { return v == 0xFFFFFFFFu; }'
plant 60 $E "the DIG FIFO error bits are not judged" 'constexpr bool dig_bad(uint32_t v) { return v == 0xFFFFFFFFu || ((v >> 28) & 3u) != 0u; }' 'constexpr bool dig_bad(uint32_t v) { return v == 0xFFFFFFFFu; }'
plant 61 $E "row 120 is enabled in production" 'constexpr Timing kProd = { 2000u, 500u, 3000u, 50u, 2000u, 3000u, 250u, 250u, 10u, 20u, 15000u, false };' 'constexpr Timing kProd = { 2000u, 500u, 3000u, 50u, 2000u, 3000u, 250u, 250u, 10u, 20u, 15000u, true };'
plant 62 $E "the row-120 hard deny is not consulted" '    if (p.row == 120u && p.row120Off) return N48N_MODE_D_ROW120_OFF;
' ''
plant 63 $E "the abort request is not cleared after the restore, and the VERIFY window honours it" '    hw.lock(hw.ctx); if (st.abortReq) { o.aborted = true; st.abortReq = false; } hw.unlock(hw.ctx);
    // ---- VERIFY (7): the 60 Hz rate is back and the stream is up; the restore transition'"'"'s FIFO health is judged after it.
' '    // ---- VERIFY (7)
' 'measure(hw, st, tm, mms, false);' 'measure(hw, st, tm, mms, true);'
plant 64 $D "dcnmode 0 restores without an earlier trial write" '	if (!gMt.everWrote) {                  // 0.0.605 review' '	if (false) {                  // 0.0.605 review'
plant 65 $E "the DIO baseline is NOT refreshed after the settle (the write transition is judged)" 'Sampler smp{ &hw, &st, err_count(prcS), hw.now_us(hw.ctx) + samplePeriod, samplePeriod };' 'Sampler smp{ &hw, &st, err_count(prc), hw.now_us(hw.ctx) + samplePeriod, samplePeriod };'
plant 66 $SR/amd/smu_dal.cpp "the DAL step does not refuse while a trial runs" 'const bool busy = !gotBusy || n48dcn::modeTrialBusy();' 'const bool busy = !gotBusy;'
plant 67 $D "a running DAL step does not make the trial DENIED" 'return gDcn.flipHeld || gDcn.srcEnabled || amdgpu::dal_busy(); }' 'return gDcn.flipHeld || gDcn.srcEnabled; }'
plant 68 $E "the steer / DIG FIFO health is not part of the pre-trial baseline" '!health_bad(prc, steer0, dig0) &&' 'fifo_err(prc) == 0u &&'
plant 69 $D "modeTrialBusy always answers false" 'bool modeTrialBusy() { return __atomic_load_n(&gMt.busy, __ATOMIC_SEQ_CST) != 0u; }' 'bool modeTrialBusy() { return false; }'


# ---- 0.0.606 (native S2-120HZ: the underflow read P1, the DP1 resync P3, the OTG update lock P2, F1) ----
plant 70 $E 'P1: the OPTC clear sets INPUT_SOFT_RESET (b0)' 'constexpr uint32_t optc_clear_value(uint32_t cur) { return (cur & ~(kOptcSeen | kOptcDbPending | kOptcClear)) | kOptcClear; }' 'constexpr uint32_t optc_clear_value(uint32_t cur) { return (cur & ~(kOptcSeen | kOptcDbPending | kOptcClear)) | kOptcClear | kOptcSoftReset; }'
plant 71 $E 'P1: the soft-reset refusal is gone (OPTC cleared while INPUT_SOFT_RESET is set)' 'constexpr bool clear_refused(uint32_t optc) { return !unreadable(optc) && (optc & kOptcSoftReset) != 0u; }' 'constexpr bool clear_refused(uint32_t optc) { (void)optc; return false; }'
plant 72 $E 'P1: the HUBP UNDERFLOW_STATUS field is not an underflow' 'constexpr bool hubp_bad(uint32_t v) { return unreadable(v) || hubp_status(v) != 0u || hubp_timeout(v) != 0u; }' 'constexpr bool hubp_bad(uint32_t v) { return unreadable(v) || hubp_timeout(v) != 0u; }'
plant 73 $E 'P1: the HUBP TIMEOUT_STATUS field is not an underflow' 'constexpr bool hubp_bad(uint32_t v) { return unreadable(v) || hubp_status(v) != 0u || hubp_timeout(v) != 0u; }' 'constexpr bool hubp_bad(uint32_t v) { return unreadable(v) || hubp_status(v) != 0u; }'
plant 74 $E 'P1: OPTC OCCURRED_CURRENT (b13) is not an underflow' 'constexpr uint32_t kOptcSeen = kOptcOccurred | kOptcInt | kOptcCurrent;' 'constexpr uint32_t kOptcSeen = kOptcOccurred | kOptcInt;'
plant 75 $E 'P1: an unreadable underflow register reads as clean' 'constexpr bool unreadable(uint32_t v) { return v == 0xFFFFFFFFu; }' 'constexpr bool unreadable(uint32_t v) { (void)v; return false; }'
plant 76 $E 'P1: an underflow in the rate window is not folded into the verdict (and its phase)' '            o.fifoBad = o.fifoBad || smp.fifo; o.streamLost = o.streamLost || smp.stream; o.underflow = o.underflow || smp.uf; o.clockLost = o.clockLost || smp.clk;
            r->vtotal_reg[1]' '            o.fifoBad = o.fifoBad || smp.fifo; o.streamLost = o.streamLost || smp.stream; o.clockLost = o.clockLost || smp.clk;
            r->vtotal_reg[1]'
plant 77 $E 'P1: the dwell samples no underflow' 'if (smp.tick(now)) {' 'if (false) {'
plant 78 $E 'P1: the pre-trial underflow baseline is not checked' 'const bool ufBad0 = uf_is_bad(u0);' 'const bool ufBad0 = false;'
plant 79 $E 'P1: the settle-time (write transition) underflow is JUDGED' 'split ? kUfJudged : kUfSettle' 'kUfJudged'
plant 80 $E 'P1: an underflow after the restore is not judged' 'if (bad && kind == kUfAfter) p.ufAfterBad = true;' '(void)0;'
plant 81 $E 'P1: the restore entry does not clear the trial-phase status (it would be counted again after the restore)' '{ const UfRead u = uf_read(hw); if (uf_record(hw, st, u, kUfJudged)) (void)uf_clear_locked(hw, st); }' '{ const UfRead u = uf_read(hw); (void)uf_record(hw, st, u, kUfJudged); }'
plant 82 $E 'P1 precedence: UNDERFLOW ranks below the DIO FIFO and the rate' 'o.underflow ? N48N_MODE_V_UNDERFLOW : o.fifoBad ? N48N_MODE_V_FIFO : o.rateBad ? N48N_MODE_V_RATE' 'o.fifoBad ? N48N_MODE_V_FIFO : o.rateBad ? N48N_MODE_V_RATE : o.underflow ? N48N_MODE_V_UNDERFLOW'
plant 83 $E 'P1: the 0.0.606 verdicts do not latch' 'v <= N48N_MODE_V_HOLD; }   // latches' 'v <= N48N_MODE_V_ABORT; }   // latches'
plant 84 $E 'P1: a strobe that reads back set is left set' 'if (!uf::unreadable(u.hubp) && (u.hubp & (uf::kHubpClear | uf::kHubpTimeoutClear)) != 0u) {' 'if (false) {'
plant 85 $E 'P1: an unreadable register is written by the clear' '    if (uf::unreadable(u.hubp) || uf::unreadable(u.optc)) return false;
    bool wrote = false;' '    bool wrote = false;'
plant 86 $E 'P1: the UNREAD flag is never reported' 'if (p.ufUnread) r->flags |= N48N_MODE_F_UNDERFLOW_UNREAD;' '(void)0;'
plant 87 $E 'P1: the underflow baseline of a trial with lock rows is judged by the lock instead (deny reason dropped)' '    if (!p.ufOk) return N48N_MODE_D_UNDERFLOW;
' ''
plant 88 $E 'P3: STEER_FIFO_RESET is written BEFORE the STATUS wait (right after ENABLE = 0)' '        st.blanked = true;
        if (!wr_field(hw, kRegDp1Vid, kVidEnable, 0u, kVidStatus, &rc)) break;' '        st.blanked = true;
        { uint32_t rc0 = 0; (void)wr_field(hw, kRegDp1Steer, kSteerReset, kSteerReset, kSteerNever, &rc0); }
        if (!wr_field(hw, kRegDp1Vid, kVidEnable, 0u, kVidStatus, &rc)) break;'
plant 89 $E 'P3: the blank waits for nothing (no STATUS == 0 wait)' 'if (!poll_reg(hw, kRegDp1Vid, kVidStatus, 0u, kStatusPolls, kStatusPollUs)) { rc = N48N_MODE_SEQ_STATUS_STUCK; break; }   // wait STATUS == 0 FIRST' '(void)0;'
plant 90 $E 'P3: the blank writes DP0 instead of DP1 (compile-time: the register table pins DP1)' 'constexpr uint32_t kRegDp1Vid = 0x5706u; ' 'constexpr uint32_t kRegDp1Vid = 0x55E2u; '
plant 91 $E 'P3: the blank'"'"'s DIS_DEFER write goes to DP0' 'if (!wr_field(hw, kRegDp1Vid, kVidDisDefer, kVidDisDefer2, kVidStatus, &rc)) break;' 'if (!wr_field(hw, kRegDpStream[0], kVidDisDefer, kVidDisDefer2, kVidStatus, &rc)) break;'
plant 92 $E 'P3: PIXEL_PER_CYCLE is written (the level field mask spans it and the value sets it)' 'kDigLevelMask = 0x7Cu, kDigLevel7 = 0x1Cu,' 'kDigLevelMask = 0x37Cu, kDigLevel7 = 0x11Cu,'
plant 93 $E 'P3: the steer FIFO ACK (b6) is written' 'kSteerEnable = 0x1u, kSteerReset = 0x2u, kSteerNever = 0x74u;' 'kSteerEnable = 0x1u, kSteerReset = 0x42u, kSteerNever = 0x34u;'
plant 94 $E 'P3: the steer overflow flag (b4) is written back as read' 'kSteerNever = 0x74u;' 'kSteerNever = 0x64u;'
plant 95 $E 'P3: the DIG FIFO error bits and RESET_DONE are written back as read' 'kDigNever = 0x30100000u;' 'kDigNever = 0x0u;'
plant 96 $E 'P3: a blank that failed before touching anything is not retried' 'if (rc != N48N_MODE_SEQ_OK && rc != N48N_MODE_SEQ_NOT_ENABLED && rc != N48N_MODE_SEQ_UNREADABLE && !st.blanked) {' 'if (false) {'
plant 97 $E 'P3: a failed unblank is not retried' '    if (rc != N48N_MODE_SEQ_OK) {
        rc = unblank_locked(hw, st, true);' '    if (false) {
        rc = unblank_locked(hw, st, true);'
plant 98 $E 'P3: `blanked` is not recorded when the stream is stopped' '        st.blanked = true;
        if (!wr_field(hw, kRegDp1Vid, kVidEnable, 0u, kVidStatus, &rc)) break;' '        if (!wr_field(hw, kRegDp1Vid, kVidEnable, 0u, kVidStatus, &rc)) break;'
plant 99 $E 'P3: the unblank does not verify the stream is active before clearing `blanked`' 'if (!poll_reg(hw, kRegDp1Vid, kVidEnable | kVidStatus, kVidEnable | kVidStatus, kActivePolls, kActivePollUs)) { if (rc == N48N_MODE_SEQ_OK) rc = N48N_MODE_SEQ_NOT_ACTIVE; break; }' '(void)0;'
plant 100 $E 'P3: the restore does not unblank a blanked stream' 'if (wrap_unblank_locked(hw, st, true) != N48N_MODE_SEQ_OK) p.seqBad = true;' '(void)0;'
plant 101 $E 'P3: `dcnmode 0` (emergency_recover) never resyncs an unhealthy back end' 'o->rc = resync_locked(hw, st, true);' 'o->rc = 0u;'
plant 102 $E 'P3: the golden restore (dcnmode 0, kext stop) does not unblank' 'if (st.blanked) {                                  // a stream a failed trial left blanked comes back here (dcnmode 0, the kext stop)' 'if (false) {                                  // a stream a failed trial left blanked comes back here (dcnmode 0, the kext stop)'
plant 103 $E 'P3: the unblank skips the DIG FIFO reset' 'if (!wr_field(hw, kRegDigFifoCtrl0, kDigReset, kDigReset, kDigNever, &rc)) break;                            // DIG_FIFO_RESET = 1' '(void)0;'
plant 104 $E 'P3: the unblank skips the steer FIFO reset pulse' 'if (!wr_field(hw, kRegDp1Steer, kSteerReset, kSteerReset, kSteerNever, &rc)) break;                          // STEER_FIFO_RESET = 1' '(void)0;'
plant 105 $E 'P3: a failed resync does not reach the verdict' 'o.lockFail ? N48N_MODE_V_LOCK : o.resyncFail ? N48N_MODE_V_RESYNC :' 'o.lockFail ? N48N_MODE_V_LOCK :'
plant 106 $E 'P3: the wrapper does not unblank after the writes' 'if (wrap && wrap_unblank_locked(hw, st) != N48N_MODE_SEQ_OK) a.resyncFail = true;' '(void)wrap;'
plant 107 $E 'P3: the wrapper does not blank before the writes' 'if (wrap && wrap_blank_locked(hw, st) != N48N_MODE_SEQ_OK) { a.resyncFail = true; return a; }' '(void)wrap;'
plant 108 $E 'P3: the watchdog window does not carry the resync time' 'uint32_t wrapExtraMs = 4000u;' 'uint32_t wrapExtraMs = 0u;'
plant 109 $E 'P3: a passed row-1 resync is not remembered' 'if (r->verdict == N48N_MODE_V_PASS && row == 1u) st.resyncPassed = true;' '(void)0;'
plant 110 $E 'P2 / P3: row 2 and the wrapped row 50 need no passed resync' 'const bool needResync = row == 2u || row == 120u || (row == 50u && (tflags & N48N_MODE_TF_RESYNC) != 0u);' 'const bool needResync = false;'
plant 111 $E 'the restore is blind to a stream left blanked / a lock left held (verdict)' '|| repSeqBad || st.rep.ufAfterBad || repBlanked || repLockHeld;' '|| st.rep.ufAfterBad;'
plant 112 $E 'P2: the apply never unlocks' '        const uint32_t u = unlock_locked(hw, st);
        if (u != N48N_MODE_SEQ_OK) { p.lkRc = u; a.lockFail = true; }' '        const uint32_t u = N48N_MODE_SEQ_OK;
        if (u != N48N_MODE_SEQ_OK) { p.lkRc = u; a.lockFail = true; }'
plant 113 $E 'P2 ORDER: the unlock happens BEFORE the V_TOTAL write' '    for (uint32_t k = 0; k < st.nPlan && !st.restoreClaimed; k++) {' '    if (lockRow) (void)unlock_locked(hw, st);
    for (uint32_t k = 0; k < st.nPlan && !st.restoreClaimed; k++) {'
plant 114 $E 'P2 ORDER: the V_TOTAL write happens WITHOUT the lock' '
        if (rc == N48N_MODE_SEQ_OK) rc = lock_locked(hw, st);' '
        (void)0;'
plant 115 $E 'P2: the DRR mode is not written before the lock' '
        uint32_t rc = drr_set_locked(hw, st, kDrrModeStartOfFrame);' '
        uint32_t rc = N48N_MODE_SEQ_OK;'
plant 116 $E 'P2: the DRR mode is not restored' 'if (st.drrWritten) {                                            // DRR mode back to its golden value, LAST of the register restores' 'if (false) {                                            // DRR mode back to its golden value, LAST of the register restores'
plant 117 $E 'P2: the lock select is never written' 'if (!wr_field(hw, kRegGlobalCtrl2, kSelMask, kOtgInst << kSelShift, 0u, &rc)) { seq_end(hw, kSeqLock, rc, t0, 0u); return rc; }' '(void)0;'
plant 118 $E 'P2: the latch confirmation always passes' 'const uint32_t rc = seen ? N48N_MODE_SEQ_OK : N48N_MODE_SEQ_LATCH;' 'const uint32_t rc = N48N_MODE_SEQ_OK;'
plant 119 $E 'P2: the latch witness accepts any count (does not compare with the OLD maximum)' 'if (c > oldMax) { seen = true; break; }' 'if (c > 0u) { seen = true; break; }'
plant 120 $E 'P2: the pending wait is satisfied at once' 'if (db != 0xFFFFFFFFu && pipe != 0xFFFFFFFFu && (db & kDbPendingMask) == 0u && (pipe & kPipeRegPending) == 0u) { rc = N48N_MODE_SEQ_OK; break; }' 'rc = N48N_MODE_SEQ_OK; break;'
plant 121 $E 'P2: a stuck lock is not cleared at the restore entry' '    clear_stuck_lock_locked(hw, st);
    bool ranBlank = false;' '    bool ranBlank = false;'
plant 122 $E 'P2: `dcnmode 0` does not clear a stuck lock' '        clear_stuck_lock_locked(hw, st);                   // 0.0.606: a stuck OTG lock request first (design P2)' ''
plant 123 $E 'P2: the lock precondition ignores UPDATE_INSTANTLY' '(db & kDbInstantly) == 0u &&' 'true &&'
plant 124 $E 'P2: the lock select is not restored' '    if (st.selWritten) {
        uint32_t rc = N48N_MODE_SEQ_OK;' '    if (false) {
        uint32_t rc = N48N_MODE_SEQ_OK;'
plant 125 $E 'P2: the restore writes V_TOTAL without the lock' 'const bool useLock = st.lockOn && (split ? nOtg != 0u : st.nWritten != 0u);' 'const bool useLock = false;'
plant 126 $E 'P2: row 2'"'"'s rate tolerance is 1 %' 'uint32_t vtTolPermille = 6u;' 'uint32_t vtTolPermille = 10u;'
plant 127 $E 'P2: row 2 latches a different total (1600)' 'constexpr uint32_t kRow2VTotalReg = 1500u;' 'constexpr uint32_t kRow2VTotalReg = 1600u;'
plant 128 $E 'P2: a lock whose status never rises is left held' 'if (!got) { (void)unlock_locked(hw, st); rc = N48N_MODE_SEQ_LOCK_TIMEOUT; }' 'if (!got) { rc = N48N_MODE_SEQ_LOCK_TIMEOUT; }'
plant 129 $E 'P2: the golden of the DRR mode / lock select is never taken' 'for (uint32_t i = 0; i < kExtN; i++) st.ext[i] = x[i]; ' ''
plant 130 $E 'P2: the DRR mode restore writes mode 2 instead of the golden' 'const uint32_t d = drr_set_locked(hw, st, (st.ext[kExtDrr] & kDbDrrMask) >> kDbDrrShift);' 'const uint32_t d = drr_set_locked(hw, st, kDrrModeStartOfFrame);'
plant 131 $E 'P2 / P3: the unlock write clears the wrong field (writes the status bit)' 'if (!wr_field(hw, kRegMasterLock, kLockReq, 0u, kLockStatus, &rc)) break;                                    // MASTER_UPDATE_LOCK = 0' 'if (!wr_field(hw, kRegMasterLock, kLockReq, 0u, 0u, &rc)) break;                                    // MASTER_UPDATE_LOCK = 0'
plant 132 $D 'glue: dcnmode 0 restores without the resync (emergency_recover replaced by nothing)' '	n48mt::emergency_recover(kMtHw, gMt, &rc);' '	(void)rc;'
plant 133 $D 'glue: the delay of the sequences does nothing' 'static void mt_delay_us(void *, uint32_t us) { IODelay(us); }' 'static void mt_delay_us(void *, uint32_t us) { (void)us; }'
plant 134 $D 'glue: the selector drops the flags word' 'flags > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)flags, o);' '0u, o);'


# ---- 0.0.606 review SHOULD-FIXes ----
plant 135 $E 'review 2: the LAST unblank attempt is not forced past a timeout' 'rc = unblank_locked(hw, st, true);' 'rc = unblank_locked(hw, st, false);'
plant 136 $E 'review 2: `force` is ignored inside the unblank' 'inline uint32_t unblank_locked(const Hw &hw, State &st, bool force = false) {' 'inline uint32_t unblank_locked(const Hw &hw, State &st, bool force0 = false) {
    const bool force = false; (void)force0;'
plant 137 $E 'review 3: the golden restore writes the DRR mode back without waiting for the pending latch' 'if (e == kExtDrr) (void)wait_pending_locked(hw, st);   // a latch still pending must complete before the DRR mode goes back' ''
plant 138 $E 'review 4: no lock-hold witness' 'if (lockRow && !a.writeFailed) {                                // the V_TOTAL write must be HELD while blanked and locked' 'if (false) {                                // the V_TOTAL write must be HELD while blanked and locked'
plant 139 $E 'review 4: the lock-hold witness accepts anything' 'const bool ok = n != 0u && vmax <= oldMax && vmax > 100u;
    if (ok) st.rep.lockHeldProven = true;' 'const bool ok = true;
    if (ok) st.rep.lockHeldProven = true;'
plant 140 $E 'review 5: a stale `lockHeld` with the request at 0 is never cleared' 'if (st.lockHeld && poll_reg(hw, kRegMasterLock, kLockReq | kLockStatus, 0u, kLockPolls, kLockPollUs)) st.lockHeld = false;' '(void)0;'
plant 141 $E 'review 6: the underflow clear is written even when another reason denies the trial' 'if (why == 0u && ufBad0) {' 'if (ufBad0) {'
plant 142 $E 'review 1: row 2 measures 3 s' 'uint32_t vtMeasureMs = 8000u;' 'uint32_t vtMeasureMs = 3000u;'
plant 143 $E 'review 4: the hold witness failure does not fail the apply' 'if (h != N48N_MODE_SEQ_OK) { p.lkRc = h; a.lockFail = true; }' '(void)h;'

# ---- 0.0.607: row 120, the clock hold, the split order ----
plant 144 $E "P5: the DTO is written INSIDE the OTG lock (before the unlock)" '    write_group_locked(hw, st, kGrpOtg, r, a);                                                        // step 4' '    write_group_locked(hw, st, kGrpOtg, r, a);
    write_group_locked(hw, st, kGrpDto, r, a);'
plant 145 $E "P4: the release ignores restoreBad (the clocks come off after a restore that did not verify)" 'o.holdStuck = hw.hold_release(hw.ctx, o.restoreBad, &st.rep.hold) != n48dal::kRlOk;' 'o.holdStuck = hw.hold_release(hw.ctx, false, &st.rep.hold) != n48dal::kRlOk;'
plant 146 $E "P4: the WATCHDOG releases the clocks" '    hw.unlock(hw.ctx);
    if (byWatchdog) hw.log(hw.ctx, kLogWatchdog' '    hw.unlock(hw.ctx);
    if (byWatchdog && hw.hold_release != nullptr) (void)hw.hold_release(hw.ctx, false, &st.rep.hold);
    if (byWatchdog) hw.log(hw.ctx, kLogWatchdog'
plant 147 $E "P2: the OTG lock is never released after the row-120 writes (missing unlock)" 'const uint32_t u = unlock_locked(hw, st);                                                         // step 5' 'const uint32_t u = N48N_MODE_SEQ_OK;                                                            // step 5'
plant 148 $E "P4: the clock raise runs UNDER the engine lock (a DAL call under gMtLock)" 'const uint32_t hrc = hw.hold_raise(hw.ctx, ri.needDispKhz, ri.needDppKhz, &st.rep.hold, &after);' 'hw.lock(hw.ctx); const uint32_t hrc = hw.hold_raise(hw.ctx, ri.needDispKhz, ri.needDppKhz, &st.rep.hold, &after); hw.unlock(hw.ctx);'
plant 149 $E "P5: the restore writes the DTO AFTER the lock instead of before it" '            restore_group_locked(hw, st, kGrpDto);
            // 0.0.608: ONE OPTC read' '            // 0.0.608: ONE OPTC read' '        if (useLock) { const uint32_t l = lock_locked(hw, st); if (l != N48N_MODE_SEQ_OK) { p.lkRestoreRc = l; p.seqBad = true; } }
        restore_group_locked(hw, st, split ? kGrpOtg : kGrpAll);' '        if (useLock) { const uint32_t l = lock_locked(hw, st); if (l != N48N_MODE_SEQ_OK) { p.lkRestoreRc = l; p.seqBad = true; } }
        if (split) restore_group_locked(hw, st, kGrpDto);
        restore_group_locked(hw, st, split ? kGrpOtg : kGrpAll);'
plant 150 $E "P4: the DFS is not sampled during the trial" '        if (holdOn) { smp.clkOn = true;' '        if (false) { smp.clkOn = true;'
plant 151 $E "P4: row 120 runs WITHOUT raising the clocks" '    if (why == 0u && split) {
        n48dal::Decoded after{};' '    if (false) {
        n48dal::Decoded after{};'
plant 152 $E "P5: the row-2 lock-hold prerequisite is not enforced" '    if (p.needRow2 && !p.row2Proven) return N48N_MODE_D_ROW2;
' ''
plant 153 $E "P5: the MSA is written BEFORE the DTO" '        write_group_locked(hw, st, kGrpDto, r, a);                                                    // step 6: DTO (after the unlock)
        if (!a.writeFailed) write_group_locked(hw, st, kGrpMsa, r, a);                                // step 7: MSA' '        write_group_locked(hw, st, kGrpMsa, r, a);
        if (!a.writeFailed) write_group_locked(hw, st, kGrpDto, r, a);'
plant 154 $E "P4: dcnmode 0 releases a hold after a restore that did not verify" 'bool bad = regsStillDiffer != 0u || !healthy;' 'bool bad = false;'
plant 155 $E "P4: a lost clock is not a verdict" '        o.lockFail ? N48N_MODE_V_LOCK : o.resyncFail ? N48N_MODE_V_RESYNC : o.clockLost ? N48N_MODE_V_CLOCK_LOST :' '        o.lockFail ? N48N_MODE_V_LOCK : o.resyncFail ? N48N_MODE_V_RESYNC :'
plant 156 $E "P5: the row-2 proof is granted without the lock-hold witness" 'row == 2u && lockHeldProven && latchConfirmed; }' 'row == 2u; }'
plant 157 $E "P4: the hold precondition is not enforced by the trial" '        if (st.rep.hold.pre != n48dal::kHpOk) p.holdOk = false;
' ''
plant 158 $E "P4: emergency_release releases while a trial still runs" '    if (!idle || hw.hold_state == nullptr || hw.hold_release == nullptr) return n48dal::kRlNotHeld;' '    if (hw.hold_state == nullptr || hw.hold_release == nullptr) return n48dal::kRlNotHeld;'
plant 159 $E "P5: row 120 is not wrapped in the DP1 blank" 'constexpr bool row_wrap(uint32_t row, uint32_t tflags) { return row == 1u || row == 2u || row == 120u ||' 'constexpr bool row_wrap(uint32_t row, uint32_t tflags) { return row == 1u || row == 2u ||'
plant 160 $E "P5: the kext-stop / dcnmode 0 emergency restore writes the DTO group LAST after a row-120 trial" 'const uint32_t grp = st.ever120 ? (gi == 0u ? kGrpDto : gi == 1u ? kGrpOtg : kGrpMsa) : kGrpAll;' 'const uint32_t grp = st.ever120 ? (gi == 0u ? kGrpMsa : gi == 1u ? kGrpOtg : kGrpDto) : kGrpAll;'
plant 161 $E "P4: the release is issued BEFORE the restore verdict is computed" '    if (holdOn) o.holdStuck = hw.hold_release(hw.ctx, o.restoreBad, &st.rep.hold) != n48dal::kRlOk;' '    if (holdOn) o.holdStuck = hw.hold_release(hw.ctx, false, &st.rep.hold) != n48dal::kRlOk;'

plant 162 $E "P5: row 120 writes an OTG register BEFORE the write index is recorded" '        const PlanEnt &e = st.plan[k];
        if (cap_group(e.idx) != grp) continue;
        st.written[st.nWritten++] = e.idx;
        const bool ok = hw.wr(hw.ctx, e.abs, e.newv);' '        const PlanEnt &e = st.plan[k];
        if (cap_group(e.idx) != grp) continue;
        const bool ok = hw.wr(hw.ctx, e.abs, e.newv);
        if (ok) st.written[st.nWritten++] = e.idx;'
plant 163 $E "P5: row 120's OTG writes happen WITHOUT the lock" '
    if (rc == N48N_MODE_SEQ_OK) rc = lock_locked(hw, st);' '
    (void)0;'
plant 164 $E "P5: row 120 does not write the DRR mode before the lock" '
    uint32_t rc = drr_set_locked(hw, st, kDrrModeStartOfFrame);                                       // step 3' '
    uint32_t rc = N48N_MODE_SEQ_OK;                                                                   // step 3'
plant 165 $E "P5: row 120's restore writes V_TOTAL and the rest WITHOUT the lock" 'const bool useLock = st.lockOn && (split ? nOtg != 0u : st.nWritten != 0u);' 'const bool useLock = false;'
plant 166 $E "P5: the row-120 V_TOTAL latch is not confirmed" '            if (l != N48N_MODE_SEQ_OK) { p.lkRc = l; a.lockFail = true; } else { p.latchConfirmed = true; p.lkConfirmed++; }
        }
    }
    p.lkDbDuring' '            (void)l; p.latchConfirmed = true; p.lkConfirmed++;
        }
    }
    p.lkDbDuring'

plant 167 $T "M1: VTG0_CONTROL's write mask covers VTG0_ENABLE (bit 31) again" '{ 0x000039f0u, 0x7fff7fffu, "VTG0_CONTROL" }' '{ 0x000039f0u, 0xffff7fffu, "VTG0_CONTROL" }'
plant 168 $E "S4: an abort during the raise is not looked at" '    if (why == 0u && holdOn && aborted(hw, st)) why = N48N_MODE_D_ABORTED;' '    (void)0;'
plant 169 $E "S4: an abort during the raise is honoured but the hold is not released" '        if (holdOn) (void)hw.hold_release(hw.ctx, false, &st.rep.hold);   // nothing was written and the display is at 60 Hz: the hold comes straight off' '        (void)holdOn;'

plant 170 $T "c120-pre-1: OTG_H_TIMING_CNTL is back in row 120's write set" 'constexpr uint32_t kRow120N = 55u;' 'constexpr uint32_t kRow120N = 56u;' 'constexpr uint16_t kRow120Idx[kRow120N] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 19,' 'constexpr uint16_t kRow120Idx[kRow120N] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,'

# ---- 0.0.608: the blanked-transition underflow reads (row 120) ----
plant 171 $E "0.0.608: the apply's transition clear is removed" '(void)uf_record(hw, st, ut, kUfTransition); if (uf_is_bad(ut)) (void)uf_clear_locked(hw, st); }' '(void)uf_record(hw, st, ut, kUfTransition); }'
plant 172 $E "0.0.608: the restore's latch-transition clear is removed" '(void)uf_record(hw, st, ul, kUfTransition);
            if (uf_is_bad(ul)) (void)uf_clear_locked(hw, st);' '(void)uf_record(hw, st, ul, kUfTransition);'
plant 173 $E "0.0.608: the verify-window (after-restore) read is only REPORTED" '(void)uf_record(hw, st, ua, kUfAfter); }' '(void)uf_record(hw, st, ua, kUfTransition); }'
plant 174 $E "0.0.608: the restore's clear moves AFTER the unblank" '(void)uf_record(hw, st, ul, kUfTransition);
            if (uf_is_bad(ul)) (void)uf_clear_locked(hw, st);' '(void)uf_record(hw, st, ul, kUfTransition);' '        if (wrap_unblank_locked(hw, st, true) != N48N_MODE_SEQ_OK) p.seqBad = true;
    }' '        if (wrap_unblank_locked(hw, st, true) != N48N_MODE_SEQ_OK) p.seqBad = true;
        (void)uf_clear_locked(hw, st);
    }'
plant 175 $E "0.0.608: row 120's settle read is REPORTED again" 'split ? kUfJudged : kUfSettle' 'kUfSettle'
plant 176 $E "0.0.608: the apply's clear moves AFTER the unblank" '(void)uf_record(hw, st, ut, kUfTransition); if (uf_is_bad(ut)) (void)uf_clear_locked(hw, st); }' '(void)uf_record(hw, st, ut, kUfTransition); }' '        if (wrap_unblank_locked(hw, st) != N48N_MODE_SEQ_OK) a.resyncFail = true;                     // step 8' '        if (wrap_unblank_locked(hw, st) != N48N_MODE_SEQ_OK) a.resyncFail = true;                     // step 8
        (void)uf_clear_locked(hw, st);'
plant 177 $E "0.0.608: the DTO-step read also CLEARS (the attribution is lost)" 'p.ufOptcRsDto |= ud.optc; (void)uf_record(hw, st, ud, kUfTransition);' 'p.ufOptcRsDto |= ud.optc; (void)uf_record(hw, st, ud, kUfTransition); (void)uf_clear_locked(hw, st);'
plant 178 $E "0.0.608: the apply's transition read is JUDGED" '(void)uf_record(hw, st, ut, kUfTransition);' '(void)uf_record(hw, st, ut, kUfJudged);'
plant 179 $E "0.0.608: the restore's latch read is JUDGED" '(void)uf_record(hw, st, ul, kUfTransition);' '(void)uf_record(hw, st, ul, kUfJudged);'
plant 180 $E "0.0.608: the row-120 lock-hold witness is not run" '    if (!a.writeFailed) {                                                                             // 0.0.608: the lock-hold witness' '    if (false) {                                                                             // 0.0.608: the lock-hold witness'
plant 181 $E "0.0.608: a failed row-120 lock-hold witness fails the apply" 'p.lkHoldRc = lock_hold_witness_locked(hw, st, st.live[kIdxVTotal] & kVertMask, frameUs);' 'p.lkHoldRc = lock_hold_witness_locked(hw, st, st.live[kIdxVTotal] & kVertMask, frameUs); if (p.lkHoldRc != N48N_MODE_SEQ_OK) { p.lkRc = p.lkHoldRc; a.lockFail = true; }'
plant 182 $E "0.0.608: the restore's latch-transition read runs for every row, not only row 120 (OFF identity for rows 1 / 2 / 50)" '        if (split) {
            // 0.0.608: still blanked' '        if (true) {
            // 0.0.608: still blanked'
plant 183 $E "0.0.608: the DIO post-unblank base is 0 (the attribution counts from boot)" 'err_count(p.dioHoldBase)' '0u'
plant 184 $E "0.0.608: the DIO hold read is not stored" 'st.rep.dioHoldBase = prcH; st.rep.dioHoldErrs = err_count(prcH) - err_count(prc);' 'st.rep.dioHoldBase = prcH;'
plant 185 $E "0.0.608: the ABI 1.6 words are not copied into the result" 'r->dio_hold_errs = p.dioHoldErrs; r->dio_post_unblank_errs = p.dioPostUnblankErrs;' '(void)0;'

# ---- 0.0.609: the HELD mode (row 120) ----
plant 190 $E 'hold: the scanout put-back is removed from the restore' '    hold_begin_end(hw, st);   // 0.0.609: a hold: the scanout plane goes back to the console (and is verified) BEFORE the timing is restored; a no-op for every other trial
' ''
plant 191 $E 'hold: the scanout put-back runs UNDER the engine lock (moved after the lock statement)' '    hold_begin_end(hw, st);   // 0.0.609: a hold: the scanout plane goes back to the console (and is verified) BEFORE the timing is restored; a no-op for every other trial
' '' '    hw.lock(hw.ctx);
    if (st.restoreClaimed) { hw.unlock(hw.ctx); return false; }' '    hw.lock(hw.ctx);
    hold_begin_end(hw, st);
    if (st.restoreClaimed) { hw.unlock(hw.ctx); return false; }'
plant 192 $E 'hold: `ending` is never set' '    stz(st.ending, 1u);
    if (hw.scan_release == nullptr) return;' '    if (hw.scan_release == nullptr) return;'
plant 193 $E 'hold: a scanout restore that did not verify is not recorded' 'if (rc == 2u) { stz(st.scanBad, 1u); hw.lock(hw.ctx); st.rep.seqBad = true; hw.unlock(hw.ctx); }' '(void)rc;'
plant 194 $E 'hold: HELD is published for every hold trial (a failed one enters)' '        if (hc != nullptr && !anyFail() && (r->flags & N48N_MODE_F_RATE_TRIAL_OK) != 0u) {' '        if (hc != nullptr) {'
plant 195 $E 'hold: an underflow judged at the settle read does not stop the entry' 'holdEnter = !ufJudged;' 'holdEnter = true; (void)ufJudged;'
plant 196 $E 'hold: a hold that did not enter still dwells' '(hc == nullptr || holdEnter) && hw.now_us' 'true && hw.now_us'
plant 197 $E 'hold: Acquire is allowed while the end is under way' '    if (sess == 0u || ld(st.held) == 0u || ld(st.ending) != 0u || ld(st.releaseReq) != 0u || ld(st.ownerGone) != 0u) return false;' '    if (sess == 0u || ld(st.held) == 0u || ld(st.releaseReq) != 0u || ld(st.ownerGone) != 0u) return false;'
plant 198 $E 'hold: any session may Acquire' '    return o == sess || (o == 0u && ld(st.handoff) != 0u);' '    return true;'
plant 199 $E 'hold: a taker does not end the handoff' 'stz(st.handoff, 0u); __atomic_fetch_or(&st.holdFlags' '__atomic_fetch_or(&st.holdFlags'
plant 200 $E 'hold: the owner'\''s close never ends a non-handoff hold' '    else stz(st.ownerGone, 1u);' '    else (void)0;'
plant 201 $E 'hold: the issuing session'\''s close of a HANDOFF hold ends it' 'stz(st.ownerSeq, 0u); }
    else stz(st.ownerGone, 1u);' 'stz(st.ownerSeq, 0u); stz(st.ownerGone, 1u); }
    else stz(st.ownerGone, 1u);'
plant 202 $E 'hold: ModeRelease is not honoured' '    if (ld(st.releaseReq) != 0u) return kHeRelease;
' ''
plant 203 $E 'hold: the handoff window never expires' '    if (ld(st.ownerSeq) == 0u && ld(st.handoff) != 0u && hw.now_us(hw.ctx) >= __atomic_load_n(&st.handoffDeadlineUs, __ATOMIC_SEQ_CST)) return kHeNoTaker;
' ''
plant 204 $E 'hold: the max-time end reason is mislabelled' 'holdEnter ? kHeMaxTime : 0u' 'holdEnter ? kHeRelease : 0u'
plant 205 $E 'hold: the watchdog window has no hold slack' 'dwellMs + (hc != nullptr ? kHoldEndSlackMs : 0u)' 'dwellMs'
plant 206 $SR/Navi48NativeABI.h 'hold: the ABI max hold is 10 minutes' '#define N48N_MODE_MAX_HOLD_MS     300000u' '#define N48N_MODE_MAX_HOLD_MS     600000u'
plant 207 $E 'hold: hold_cfg_ok accepts any flags' '(c.flags & ~N48N_HOLD_F_MASK) == 0u' 'true'
plant 208 $E 'hold: a hold is allowed for any row' '(tflags == 0u || row != 120u) && (hc == nullptr || row == 120u);' '(tflags == 0u || row != 120u);'
plant 209 $E 'hold: holdOn is never cleared on the finished path' 'stz(st.held, 0u); stz(st.holdOn, 0u);   // 0.0.609: the result buffer' 'stz(st.held, 0u);   // 0.0.609: the result buffer'
plant 210 $E 'hold: busy drops BEFORE held / holdOn (a waiting ModeRelease could read a half-final result)' '    stz(st.held, 0u); stz(st.holdOn, 0u);   // 0.0.609: the result buffer is final BEFORE busy drops (a waiting ModeRelease reads it after runnerDone)
    __atomic_store_n(&st.busy, 0u, __ATOMIC_RELEASE);' '    __atomic_store_n(&st.busy, 0u, __ATOMIC_RELEASE);
    stz(st.held, 0u); stz(st.holdOn, 0u);'
plant 211 $E 'hold: hold_abort_scan does nothing (dcnmode 0 / the kext stop leave the plane for the hold thread)' 'inline void hold_abort_scan(const Hw &hw, State &st) { if (__atomic_load_n(&st.busy, __ATOMIC_ACQUIRE) != 0u) hold_begin_end(hw, st); }' 'inline void hold_abort_scan(const Hw &, State &) {}'
plant 212 $E 'hold: the runner does not free the hold slot' 'inline void hold_runner_done(State &st) { stz(st.runnerDone, 1u); stz(st.launching, 0u); stz(st.releaseReq, 0u); }' 'inline void hold_runner_done(State &st) { stz(st.runnerDone, 1u); stz(st.releaseReq, 0u); }'
plant 213 $E 'hold: the launch claim is not exclusive' 'inline bool hold_launch_claim(State &st) { uint32_t zero = 0u; return __atomic_compare_exchange_n(&st.launching, &zero, 1u, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }' 'inline bool hold_launch_claim(State &st) { stz(st.launching, 1u); return true; }'
plant 214 $E 'hold: a bad scanout restore is not reported in hold_flags' 'ld(st.scanBad) != 0u ? N48N_HOLD_FL_SCAN_BAD : 0u' '0u'
plant 215 $E 'hold: the watchdog'\''s claim does not end the hold thread'\''s dwell' '    return claimed ? kHeWatchdog : kHeNone;' '    (void)claimed; return kHeNone;'
plant 216 $E 'hold: ModeRelease is accepted when no hold runs' 'inline bool hold_request_release(State &st) { if (ld(st.launching) == 0u) return false; stz(st.releaseReq, 1u); return true; }   // latched from the claim on: a release that arrives before run_trial sets holdOn is NOT dropped' 'inline bool hold_request_release(State &st) { stz(st.releaseReq, 1u); return true; }'
plant 217 $E 'hold: the session hook ends a hold for ANY session'\''s close' '    if (ld(st.launching) == 0u || seq == 0u || ld(st.ownerSeq) != seq) return;' '    if (ld(st.launching) == 0u || seq == 0u) return;'
plant 218 $E 'hold: the take-over CAS is an unconditional store (a second taker displaces the owner)' '__atomic_compare_exchange_n(&st.ownerSeq, &zero, sess, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)' '(stz(st.ownerSeq, sess), true)'
plant 219 $D 'hold: scanAcquire ignores a hold (always refuses while busy)' '&& !n48mt::hold_allows_acquire(gMt, sess)' ''
plant 220 $D 'hold: the take-over is not recorded by scanAcquire' '		n48mt::hold_take_owner(gMt, sess);   // 0.0.609: a HANDOFF hold with no owner passes to this session (atomic words only); a no-op when no hold is up
' ''
plant 221 $D 'hold: the scanout put-back callback always says nothing to do' 'return o[0] != 1ull ? 2u : (acq ? 1u : 0u);' 'return 0u;'
plant 222 $D 'hold: dcnmode 0 does not put the scanout back first' '	n48mt::hold_abort_scan(kMtHw, gMt);    // 0.0.609: a running hold'\''s scanout plane goes back FIRST (no lock held here)
' ''
plant 223 $D 'hold: the kext stop does not put the scanout back first' '	n48mt::hold_abort_scan(kMtHw, gMt);    // 0.0.609: as in mt_emergency_restore
' ''
plant 224 $D 'hold: the kext stop does not wait for the hold thread' '(__atomic_load_n(&gMtWdAlive, __ATOMIC_ACQUIRE) != 0u || __atomic_load_n(&gMtHoldAlive, __ATOMIC_ACQUIRE) != 0u); i++)' '__atomic_load_n(&gMtWdAlive, __ATOMIC_ACQUIRE) != 0u; i++)'
plant 225 $D 'hold: the slot is prepared after the claim is replaced by prepare-before-claim' '	if (!n48mt::hold_launch_claim(gMt)) { mt_result_denied(o, N48N_MODE_D_BUSY); return n48scan::kOk; }   // one hold at a time, from the claim until its runner is done
	n48mt::hold_launch_prepare(gMt, sess, cfg);
' '	n48mt::hold_launch_prepare(gMt, sess, cfg);
	if (!n48mt::hold_launch_claim(gMt)) { mt_result_denied(o, N48N_MODE_D_BUSY); return n48scan::kOk; }   // one hold at a time, from the claim until its runner is done
'
plant 226 $D 'hold: the session hook is a no-op' 'void modeHoldSessionClosed(uint32_t sess) { n48mt::hold_session_closed(kMtHw, gMt, sess); }' 'void modeHoldSessionClosed(uint32_t) {}'
plant 227 $D 'hold: the runner does not free the hold slot' '	n48mt::hold_runner_done(gMt);                                   // the result buffer is final; the hold slot is free
' ''
plant 228 $D 'hold: the runner runs row 50 instead of 120' 'N48N_MODE_ROW_120, cfg.maxMs, 0u, &gMt.heldFinal, &cfg);' 'N48N_MODE_ROW_50, cfg.maxMs, 0u, &gMt.heldFinal, &cfg);'
plant 229 $D 'hold: the ModeRelease wait does not wait for the runner' '	if (n48mt::hold_request_release(gMt)) N48LOG("mode-hold: RELEASE requested");
	for (uint32_t i = 0; i < 4000u; i++) {' '	if (n48mt::hold_request_release(gMt)) N48LOG("mode-hold: RELEASE requested");
	for (uint32_t i = 0; i < 0u; i++) {'
plant 230 $SR/amd/native_s1c.cpp 'hold: the client'\''s close tells the hold BEFORE the console plane is back' '    if (!multi || __atomic_load_n(&gScanOwner, __ATOMIC_ACQUIRE) == s->id) { uint64_t tr[2]; scan_teardown(s, how, tr); if (multi) __atomic_store_n(&gScanOwner, 0u, __ATOMIC_RELEASE); }
    n48dcn::modeHoldSessionClosed(s->id);   // 0.0.609: a row-120 hold owned by this session ends (or, HANDOFF, passes on) - AFTER the plane is back; atomic words only
' '    n48dcn::modeHoldSessionClosed(s->id);   // 0.0.609: a row-120 hold owned by this session ends (or, HANDOFF, passes on) - AFTER the plane is back; atomic words only
    if (!multi || __atomic_load_n(&gScanOwner, __ATOMIC_ACQUIRE) == s->id) { uint64_t tr[2]; scan_teardown(s, how, tr); if (multi) __atomic_store_n(&gScanOwner, 0u, __ATOMIC_RELEASE); }
'
plant 231 $SR/amd/native_s1c.cpp 'hold: the client'\''s close never tells the hold' '    n48dcn::modeHoldSessionClosed(s->id);   // 0.0.609: a row-120 hold owned by this session ends (or, HANDOFF, passes on) - AFTER the plane is back; atomic words only
' ''
plant 232 $SR/amd/native_s1c.cpp 'hold: Acquire passes no session id' 'n48dcn::scanAcquire(out, s->id)' 'n48dcn::scanAcquire(out, 0u)'
plant 233 $SR/amd/native_s1c.cpp 'hold: n1c_mode_release takes the client lock' 'IOReturn n1c_mode_release(const N1cRef &ref, uint64_t flags, n48n_mode_result *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;' 'IOReturn n1c_mode_release(const N1cRef &ref, uint64_t flags, n48n_mode_result *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    IOLockLock(gCliLock); IOLockUnlock(gCliLock);'
plant 234 $SR/amd/native_s1c.cpp 'hold: every open gets the same session id' '__atomic_store_n(&s->id, ++gSessSeqCounter, __ATOMIC_SEQ_CST);' '__atomic_store_n(&s->id, 1u, __ATOMIC_SEQ_CST);'
plant 235 $SR/Navi48NativeClient.cpp 'hold: selector 17 is dispatched without a shape check' 'case N48N_SEL_MODE_HOLD:
		if (!shape(2, 0, 0, sizeof(n48n_mode_result))) return kIOReturnBadArgument;' 'case N48N_SEL_MODE_HOLD:'
plant 236 tools/native/n48mode.c 'hold: n48mode --run keeps its (exclusive) session open while the command runs' '        IOServiceClose(c);
        usleep(300000);' '        usleep(300000);'
plant 237 tools/native/n48scan.c 'hold: n48scan does not judge the final hold verdict' '&& rateOk && holdOk;' '&& rateOk;'

# ---- 0.0.609 review fixes A / B / C ----
plant 238 $D 'hold: mt_scan_release is not forced (an earlier failed put-back reads as nothing to do)' 'scan_restore("row-120 hold ending", true, 0u, o);' 'scan_restore("row-120 hold ending", false, 0u, o);'
plant 239 $D 'hold: an unverified put-back is reported only when the plane was acquired' 'return o[0] != 1ull ? 2u : (acq ? 1u : 0u);' 'return !acq ? 0u : (o[0] == 1ull ? 1u : 2u);'
plant 240 $E 'hold: a release latched before holdOn is dropped (request needs holdOn)' 'if (ld(st.launching) == 0u) return false; stz(st.releaseReq, 1u); return true; }' 'if (ld(st.holdOn) == 0u) return false; stz(st.releaseReq, 1u); return true; }'
plant 241 $E 'hold: hold_launch_prepare clears a latched release' 'stz(st.ownerGone, 0u);   /* releaseReq is NOT' 'stz(st.ownerGone, 0u); stz(st.releaseReq, 0u);   /* releaseReq is NOT'
plant 242 tools/native/n48scan.c 'hold: n48scan --hold120 asks for 120 s' 'const uint64_t hin[2] = { 60000u, 0 };' 'const uint64_t hin[2] = { 120000u, 0 };'

echo "planted breaks run: $total, escaped: $escaped"
[ $escaped -eq 0 ]
