#!/bin/zsh
# native_s2_dal_plant.sh - planted breaks for tests/native_s2_dal_test.cpp (build 0.0.604, native S2-DISPCLK).
# For each plant: copy the real sources into a scratch tree, apply ONE break (the script first proves the break changed the text exactly once),
# compile the real test (which itself compiles the real smu_dal.cpp and smu_v14_0.cpp) against the scratch tree and demand that it FAILS (a compile
# error, a failing check, a crash or a hang cut off by the 60 s alarm all count). A plant the suite lets through is a hole: the script exits
# non-zero and says which.
#   run from anywhere:  src/navi48-bringup/tests/native_s2_dal_plant.sh [first-plant-id last-plant-id]
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
K=src/navi48-bringup
SR=$K/src
SCR="${TMPDIR:-/tmp}/n2dal-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0
FIRST=${1:-0}; LAST=${2:-9999}

plant() {   # plant <id> <file relative to repo root> <old text> <new text> <description>
  local id="$1" file="$2" old="$3" new="$4" desc="$5"
  if [ "$id" -lt "$FIRST" ] || [ "$id" -gt "$LAST" ]; then return; fi
  rm -rf "$SCR"; mkdir -p "$SCR/$SR" "$SCR/$K/tests" "$SCR/tools/native" "$SCR/notes/design"
  cp "$ROOT/$SR/"*.h "$ROOT/$SR/"*.hpp "$ROOT/$SR/"*.cpp "$SCR/$SR/"
  cp -R "$ROOT/$SR/amd" "$ROOT/$SR/dcn" "$ROOT/$SR/apple" "$SCR/$SR/"
  cp "$ROOT/$K/Makefile" "$ROOT/$K/Info.plist" "$SCR/$K/"
  cp -R "$ROOT/$K/tests/dalshim" "$SCR/$K/tests/"
  cp "$ROOT/$K/tests/native_s2_dal_test.cpp" "$SCR/$K/tests/"
  cp "$ROOT/tools/native/n48dal.c" "$ROOT/tools/native/build.sh" "$SCR/tools/native/"
  cp "$ROOT/an internal design note" "$SCR/notes/design/"
  OLD="$old" NEW="$new" FILE="$SCR/$file" python3 - <<'PY'
import os,sys
p=os.environ['FILE']; s=open(p).read(); o=os.environ['OLD']; n=os.environ['NEW']
if s.count(o)!=1:
    print("PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %r" % (s.count(o), o[:70])); sys.exit(3)
open(p,'w').write(s.replace(o,n))
PY
  local rc=$?
  total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  local out
  out=$(cd "$SCR" && clang++ -std=c++17 -Wall -Wextra -O0 -pthread '-D__asm__=0;' '-D__volatile__(...)=' -I $K/tests/dalshim -I $SR -I $SR/amd \
        $K/tests/native_s2_dal_test.cpp $SR/amd/smu_dal.cpp $SR/amd/smu_v14_0.cpp -o "$SCR/t" 2>&1)
  if [ $? -ne 0 ]; then
    echo "PLANT $id: $desc: CAUGHT at compile time: $(echo "$out" | /usr/bin/grep -m1 -E 'error|static assertion' | cut -c1-140)"; return
  fi
  out=$(cd "$SCR" && perl -e 'alarm 60; exec @ARGV' ./t . 2>&1)
  local trc=$?
  if [ $trc -eq 0 ]; then echo "PLANT $id: $desc: *** ESCAPED (the suite passed) ***"; escaped=$((escaped+1)); return; fi
  if [ $trc -ge 128 ]; then echo "PLANT $id: $desc: CAUGHT by a crash or hang (signal $((trc-128)))"; return; fi
  echo "PLANT $id: $desc: CAUGHT by $(echo "$out" | /usr/bin/grep -c '^FAIL') check(s); first: $(echo "$out" | /usr/bin/grep -m1 '^FAIL' | cut -c1-120)"
}

control() {
  rm -rf "$SCR"; mkdir -p "$SCR/$SR" "$SCR/$K/tests" "$SCR/tools/native" "$SCR/notes/design"
  cp "$ROOT/$SR/"*.h "$ROOT/$SR/"*.hpp "$ROOT/$SR/"*.cpp "$SCR/$SR/"
  cp -R "$ROOT/$SR/amd" "$ROOT/$SR/dcn" "$ROOT/$SR/apple" "$SCR/$SR/"
  cp "$ROOT/$K/Makefile" "$ROOT/$K/Info.plist" "$SCR/$K/"
  cp -R "$ROOT/$K/tests/dalshim" "$SCR/$K/tests/"
  cp "$ROOT/$K/tests/native_s2_dal_test.cpp" "$SCR/$K/tests/"
  cp "$ROOT/tools/native/n48dal.c" "$ROOT/tools/native/build.sh" "$SCR/tools/native/"
  cp "$ROOT/an internal design note" "$SCR/notes/design/"
  local out
  out=$(cd "$SCR" && clang++ -std=c++17 -Wall -Wextra -O0 -pthread '-D__asm__=0;' '-D__volatile__(...)=' -I $K/tests/dalshim -I $SR -I $SR/amd \
        $K/tests/native_s2_dal_test.cpp $SR/amd/smu_dal.cpp $SR/amd/smu_v14_0.cpp -o "$SCR/t" 2>&1) || { echo "CONTROL: the unplanted tree does not compile: $out"; exit 2; }
  out=$(cd "$SCR" && perl -e 'alarm 60; exec @ARGV' ./t . 2>&1) || { echo "CONTROL: the unplanted tree does not pass: $(echo "$out" | /usr/bin/grep '^FAIL' | head -3)"; exit 2; }
  echo "CONTROL: the unplanted copy compiles and passes ($(echo "$out" | /usr/bin/grep 'native_s2_dal_test: [0-9]* checks' | tail -1))"
}
control

P=$SR/amd/smu_dal_pure.h
D=$SR/amd/smu_dal.cpp
S=$SR/amd/smu_v14_0.cpp
# ---- the pure half: the table, the allowlist, the rules ----
plant 1  $P '64u + 2u * (did - 0x40u)' '64u + 4u * (did - 0x40u)' "DID table: range 2 step wrong (0x41 is no longer 272.7 MHz)"
plant 2  $P '128u + 4u * (did - 0x60u)' '32u + 4u * (did - 0x60u)' "DID table: range 3 start wrong (0x64 is no longer 125 MHz)"
plant 3  $P 'constexpr uint32_t kMinMhz = 250u;' 'constexpr uint32_t kMinMhz = 249u;' "the floor is 249 MHz"
plant 4  $P 'constexpr uint32_t kCapLevel4Mhz = 600u;' 'constexpr uint32_t kCapLevel4Mhz = 601u;' "the level-4 cap is 601 MHz"
plant 5  $P 'constexpr uint32_t kCapLevel23Mhz = 320u;' 'constexpr uint32_t kCapLevel23Mhz = 321u;' "the level-2/3 cap is 321 MHz"
plant 6  $P 'constexpr bool clk_ok(uint32_t clk) { return clk == kClkDispclk || clk == kClkDppclk; }' 'constexpr bool clk_ok(uint32_t clk) { return clk <= kClkDppclk; }' "clk 0 (and 1..5) accepted"
plant 7  $P 'return mhz <= mx ? kAllow : kRefAboveMax;' 'return kAllow;' "a hard-min above the E1b DPM max is allowed"
plant 8  $P 'if (mx == 0u) return kRefNoMax;' ';' "a hard-min is allowed before E1b recorded a max"
plant 9  $P 'if (level < 2u) return kRefLevel;                    // levels 2..4 only' ';' "0x9 allowed at level 1"
plant 10 $P 'default: return kRefMsg;                                 // 0xA, 0x5 / 0x6 / 0x8, 0xD..0x1C, everything else' 'default: return kAllow;' "any other message id (0x5, 0x6, 0x8, 0xA, 0xD..) is allowed"
plant 11 $P 'constexpr uint32_t resp_pre_check(uint32_t resp) { return (resp == 0u || resp == 0xFFFFFFFFu) ? kRefRespZero : kAllow; }' 'constexpr uint32_t resp_pre_check(uint32_t resp) { return (resp == 0xFFFFFFFFu) ? kRefRespZero : kAllow; }' "RESP == 0 before a send is admitted (pure rule)"
plant 12 $P 'return (idx <= 15u || idx == kIndexAll) ? kAllow : kRefParam;' 'return kAllow;' "0xB accepts any index"
plant 13 $P 'return param == 0u ? kAllow : kRefParam;' 'return kAllow;' "0x2 / 0x3 / 0x4 / 0x15 accept a parameter"
plant 14 $P 'return (param & 0xFFFFu) == 0u ? kAllow : kRefParam;' 'return kAllow;' "0xC accepts low bits"
plant 15 $P 'constexpr uint32_t norm_mhz(uint32_t v) { return v >= 20000u ? v / 1000u : v; }' 'constexpr uint32_t norm_mhz(uint32_t v) { return v; }' "kHz replies are not normalised"
plant 16 $P 'Plan{ 2u, { { kClkDispclk, 530u }, { kClkDppclk, 530u } }' 'Plan{ 2u, { { kClkDppclk, 530u }, { kClkDispclk, 530u } }' "E4 raises DPPCLK first (Linux raises DISPCLK first)"
plant 17 $P 'if (now.dispKhz < base.dispKhz || now.dppKhz < base.dppKhz) return kRegressed;' ';' "a clock below its start is not REGRESSED"
plant 18 $P 'if (!now.dentAgree || !now.chgDone) return kGlitch;' 'if (!now.dentAgree) return kGlitch;' "CHG_DONE is not checked"
plant 19 $P 'if ((!p.dispMoves && now.dispDid != base.dispDid) || (!p.dppMoves && now.dppDid != base.dppDid)) return kGlitch;' ';' "a clock that must not move may move"
plant 20 $P 'if (now.ok && ((p.dispMoves && now.dispKhz < p.dispKhzMin) || (p.dppMoves && now.dppKhz < p.dppKhzMin))) return kRegressed;' ';' "the dwell does not notice a dropped floor"
plant 21 $P '(!l.stopped && is_stop(verdict)) ? Latch{ true, verdict, step } : l;' '(false && is_stop(verdict)) ? Latch{ true, verdict, step } : l;' "the latch never sets"
plant 22 $P 'return v == kRefused || v == kTimeout || v == kRegressed || v == kGlitch;' 'return v == kRefused || v == kTimeout || v == kRegressed;' "GLITCH does not latch"
plant 23 $P '    if (stopped) return kDenyLatched;
' ';
' "the pre-gate ignores the latch"
plant 24 $P '    if (step != kE1b && !e1bOk) return kDenyNoE1b;
' ';
' "E2..E4 run without E1b"
plant 25 $P '    if (level < step_min_level(step)) return kDenyLevel;
' ';
' "any level runs any step"
plant 26 $P 'return a * 100ull >= b * 98ull && a * 100ull <= b * 102ull;' 'return a * 100ull >= b * 80ull && a * 100ull <= b * 120ull;' "the frame-rate tolerance is +-20 %"
plant 27 $P 'constexpr uint32_t kStallMs = 250u;' 'constexpr uint32_t kStallMs = 2500u;' "the stall bound is 2.5 s"
plant 28 $P 'constexpr bool e1b_enables(uint32_t levels6, uint32_t max6, uint32_t max7) { return levels6 > 1u && max6 != 0u && max7 != 0u; }' 'constexpr bool e1b_enables(uint32_t levels6, uint32_t max6, uint32_t max7) { return max6 != 0u && max7 != 0u; }' "one DISPCLK level still enables E2"
plant 29 $P 'constexpr uint32_t kE1bPassMaxMhz = 540u' 'constexpr uint32_t kE1bPassMaxMhz = 500u' "E1b passes with a DISPCLK max of 500"
plant 30 $P 'return d.ok && d.dentAgree && d.chgDone && d.dispKhz >= kMinMhz' 'return d.ok && d.chgDone && d.dispKhz >= kMinMhz' "the baseline does not need DENTIST to agree"
plant 31 $P '| (uint64_t)(pllReq >> 16)) * 100000ull >> 16);' '| 0ull) * 100000ull >> 16);' "the VCO ignores the PLL fraction"
plant 32 $P 'dentist_disp_w(dentist) == dfs0 && dentist_dpp_w(dentist) == dfs1' 'dentist_disp_w(dentist) == dfs0' "DENTIST agreement ignores DPPCLK"
# ---- the sender and the runner (real code) ----
plant 33 $D 'if (resp_pre_check(pre) != kAllow) {' 'if (false) {' "SEND WHILE RESP == 0: the pre-check is gone"
plant 34 $D 'const uint32_t pre = RREG32(dev, kRegResp);' 'const uint32_t pre = 1u;' "the pre-check reads a constant instead of RESP"
plant 35 $D '    WREG32(dev, kRegArg, param);
    WREG32(dev, kRegMsg, msg);' '    WREG32(dev, kRegMsg, msg);
    WREG32(dev, kRegArg, param);' "MSG is written before ARG"
plant 36 $D '    SmuSeq seq;
    if (!seq.held) {' '    struct { bool held = true; } seq;
    if (!seq.held) {' "THE DAL SENDER TAKES NO SHARED LOCK"
plant 37 $D 'if (now_us() - t0 >= kRespWaitUs) break;' ';' "the RESP wait is unbounded"
plant 38 $D 'return s.gate == n48native::kGateOn && s.positivePass;' 'return s.gate == n48native::kGateOn;' "the gate does not need S1b POSITIVE PASS"
plant 39 $D 'return (v >= 1u && v <= kMaxLevel) ? v : 0u;' 'return v;' "the boot-arg level is not range-checked"
plant 40 $D '    if (why != kAllow) {' '    if (false) {' "THE SENDER DOES NOT CHECK THE ALLOWLIST"
plant 41 $D 'gDal.max[0] = mx[0]; gDal.max[1] = mx[1]; gDal.e1bOk = true;' 'gDal.max[0] = 0xFFFFu; gDal.max[1] = 0xFFFFu; gDal.e1bOk = true;' "E1b records an unbounded max (cap above the E1b max)"
plant 42 $D 'gDal.latch = latch_apply(gDal.latch, step, gRun.verdict);' ';' "THE LATCH IS NEVER APPLIED"
plant 43 $D 'gDal.e1bOk, gDal.latch.stopped, busy)' 'gDal.e1bOk, false, busy)' "the pre-gate is given latch = false"
plant 44 $D 'if (gRun.timedOut && gRun.hardMinSent) {' 'if (false) {' "a restore is attempted after the mailbox stopped answering"
plant 45 $D 'for (uint32_t i = plan.nRaise; i-- > 0u; ) order[n++] = plan.raise[i].clk;' 'for (uint32_t i = 0; i < plan.nRaise; i++) order[n++] = plan.raise[i].clk;' "the restore runs in raise order, not reversed"
plant 46 $D 'if ((mask & (1u << clk)) == 0u || (done & (1u << clk)) != 0u) continue;' 'if ((done & (1u << clk)) != 0u) continue;' "the restore covers clocks that were never raised nor low"
plant 47 $D '} else if (mask != 0u && !gRun.timedOut) {' '} else if (false) {' "NO RESTORE AT ALL after a raise"
plant 48 $D 'while (gRun.verdict == kVNone && now_us() - ts < (uint64_t)plan.dwellMs * 1000ull) {' 'while (false) {' "no dwell"
plant 49 $D 'if (now_us() - tAdv >= (uint64_t)kStallMs * 1000ull) {' 'if (false) {' "no frame-counter stall check"
plant 50 $D 'if (gRun.verdict == kVNone && !rate_ok(' 'if (false && !rate_ok(' "no frame-rate check"
plant 51 $D 'if (!baseline_ok(b0)) {' 'if (false) {' "no baseline check"
plant 52 $D 'const bool gotBusy = owner_cas(kOwnerIdle, kOwnerDal);' 'const bool gotBusy = true;' "no busy flag"
plant 53 $D 'if (now_us() - t0 >= kHardMinPollUs) {' 'if (false) {' "the ReturnHardMinStatus poll is unbounded"
plant 54 $D 'if (isRaise) gRun.raisedMask |= 1u << clk;' ';' "a raise is never recorded as raised"
plant 55 $D '(gQ.flags & N48N_SCANQ_LIT) == 0u ||' 'false ||' "the OTG-lit pre-flight is gone"
plant 56 $D 'if (!a.ok || a.dispDid != b0.dispDid || a.dppDid != b0.dppDid) {' 'if (false) {' "E1b does not notice a clock that moved during the queries"
plant 57 $D 'if (gRun.verdict == kVNone) gRun.verdict = kGlitch;' 'if (gRun.verdict == kVNone) gRun.verdict = kPass;' "a path that decides nothing passes"
plant 58 $D 'out->flags = N48N_DAL_F_UNDERFLOW_UNREAD | N48N_DAL_F_VUPDATE_UNCOUNTED;' 'out->flags = 0;' "the underflow / VUPDATE gaps are not reported"
plant 59 $D 'if ((arg & (1u << clk)) != 0u) break;' 'break;' "the hard-min completion bit is not waited for"
plant 60 $D 'kRestoreMhz, false' 'kMinMhz, false' "the restore asks for 250 MHz, not 272"
plant 96 $SR/amd/smu_dal_pure.h 'constexpr uint32_t kRestoreMhz = 272u;' 'constexpr uint32_t kRestoreMhz = 273u;' "F1: the restore asks for 273 MHz (the CEILING of the boot clock; the read can never be AT_START)"
plant 61 $D 'WREG32(dev, kRegResp, 0u);' 'WREG32(dev, kRegResp, 0u); WREG32(dev, kRegArg, 0u);' "a fourth register write"
plant 62 $D 'if (isRaise) gRun.r->grant_reply' 'gRun.r->grant_reply' "the restore overwrites the raise's grant reply"
plant 63 $D 'const uint32_t rc9 = dal_msg(kMsgSetHardMinByFreq,' 'const uint32_t rc9 = dal_msg(kMsgSetHardMinByFreq + 1u,' "the hard-min message id is wrong"
# ---- the shared lock and the glue ----
plant 64 $S '    SmuSeq seq;   // the shared mailbox lock (held = false only when IOLockAlloc failed: then the message goes out unlocked, as before 0.0.604)' ';' "THE PPSMC PATH TAKES NO SHARED LOCK"
plant 65 $S 'if (__atomic_load_n(&gSmuOwner, __ATOMIC_ACQUIRE) == me) { gSmuDepth++; return true; }' ';' "the shared lock is not recursive (deadlock)"
plant 66 $SR/Navi48UserClient.cpp '	amdgpu::SmuSeq smuSeq;   // 0.0.604: the whole table-transfer sequence under the shared mailbox lock (PPSMC and DAL)' ';' "doMetrics does not hold the shared lock"
plant 67 $SR/Navi48UserClient.cpp '	amdgpu::SmuSeq smuSeq;   // 0.0.604: both clock messages of the power state under the shared mailbox lock' ';' "doPowerState does not hold the shared lock"
plant 68 $SR/amd/native_s1c.cpp '    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    if (flags != 0ull || step < N48N_DAL_STEP_E1B' '    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    if (flags != 0ull || step < N48N_DAL_STEP_E1B' "selector 15 does not need Hello"
plant 69 $SR/Navi48NativeClient.cpp 'if (!shape(2, 0, 0, sizeof(n48n_dal_result))) return kIOReturnBadArgument;' 'if (!shape(2, 0, 0, 0)) return kIOReturnBadArgument;' "selector 15's output shape is not checked"
plant 70 $SR/Navi48NativeABI.h '#define N48N_ABI_MINOR     12u ' '#define N48N_ABI_MINOR     1u ' "the ABI minor is not raised"
plant 71 $K/Info.plist '<key>CFBundleVersion</key>
	<string>0.0.664</string>' '<key>CFBundleVersion</key>
	<string>0.0.603</string>' "Info.plist version is stale"
plant 72 an internal design note '## ABI 1.2 addendum' '## ABI 1.2 notes' "the contract addendum is missing"
plant 73 tools/native/n48dal.c '{ N48N_ABI_VERSION, N48N_HELLO_F_MINOR }' '{ N48N_ABI_VERSION, 0 }' "the tool does not ask for the minor"
plant 74 $SR/Navi48Bringup.cpp 'n48dcn::scanShutdown();' 'n48dcn::scanShutdown(); amdgpu::smu_dal_send(*mDev, 2, 0, nullptr);' "the interrupt handler's file calls the DAL sender"
plant 75 $SR/Navi48NativeABI.h '#define N48N_DAL_V_GLITCH    5u' '#define N48N_DAL_V_GLITCH    6u' "the ABI verdict value differs from the kernel's"
plant 76 $SR/amd/native_s1c.cpp '    return dal_run_step(*gCtx->dev, (uint32_t)step, out);' '    IOLockLock(gCliLock); IOReturn r = dal_run_step(*gCtx->dev, (uint32_t)step, out); IOLockUnlock(gCliLock); return r;' "selector 15 holds the client lock across the step"
# ---- the 0.0.604 review fixes ----
plant 77 $P 'constexpr uint32_t level_count(uint32_t r) { return (r & 0x80000000u) != 0u ? 2u : (r & 0xFFu); }' 'constexpr uint32_t level_count(uint32_t r) { return (r & 0xFFu); }' "E1b: the fine-grained bit 31 of the level count is ignored"
plant 78 $P 'constexpr uint32_t level_mhz(uint32_t r) { return r & 0xFFFFu; }' 'constexpr uint32_t level_mhz(uint32_t r) { return r; }' "E1b: per-level replies are not masked to 16 bits"
plant 79 $D 'const uint32_t mhz = level_mhz(arg);' 'const uint32_t mhz = norm_mhz(arg);' "E1b guesses kHz on 0xB replies"
plant 80 $D 'const uint32_t cnt = level_count(arg);' 'const uint32_t cnt = arg;' "E1b takes the whole reply as the level count"
plant 81 $D '        if (clk == kClkDispclk) {
            uint32_t resp = 0;' '        if (true) {
            uint32_t resp = 0;' "E1b sends GetDcModeMaxDpmFreq for DPPCLK too"
plant 82 $D '    if (soft) return 0u;' ';' "a non-OK GetDcModeMaxDpmFreq reply stops the step"
plant 83 $P 'return raisedMask | ((now.ok && now.dispKhz < base.dispKhz) ? (1u << kClkDispclk) : 0u) | ((now.ok && now.dppKhz < base.dppKhz) ? (1u << kClkDppclk) : 0u);' 'return raisedMask;' "restore_mask ignores clocks below their start"
plant 84 $D 'const uint32_t mask = restore_mask(gRun.raisedMask, b0, zb);' 'const uint32_t mask = gRun.raisedMask;' "the restore covers only the raised clocks"
plant 85 $D 'gRun.verdict = kTimeout; gRun.r->flags |= N48N_DAL_F_POLL_TIMEOUT;' 'gRun.verdict = kTimeout; gRun.timedOut = true; gRun.r->flags |= N48N_DAL_F_POLL_TIMEOUT;' "a poll timeout is treated as a mailbox timeout (no restore)"
plant 86 $D '            return 2u;' '            return 1u;' "a poll timeout is a plain message failure"
plant 87 $D '    } else if (mask != 0u && !gRun.timedOut) {' '    } else if (mask != 0u) {' "a restore is sent into a dead mailbox"
plant 88 $P 'constexpr uint32_t kRespWaitUs = 2000u * 1000u;' 'constexpr uint32_t kRespWaitUs = 500u * 1000u;' "the reply bound is 500 ms"
plant 89 $D 'r->flags |= at_start(b0, z) ? N48N_DAL_F_AT_START : N48N_DAL_F_ABOVE_START;' 'r->flags |= N48N_DAL_F_AT_START;' "the restore always reports AT_START"
plant 90 $D 'r->flags |= at_start(b0, z) ? N48N_DAL_F_AT_START : N48N_DAL_F_ABOVE_START;' 'r->flags |= N48N_DAL_F_ABOVE_START;' "the restore always reports ABOVE_START"
plant 91 $D '(gQ.flags & N48N_SCANQ_ACQUIRED) != 0u) {' 'false) {' "E2..E4 run while the scanout plane is acquired"
plant 92 $P 'constexpr bool at_start(const Decoded &base, const Decoded &now) { return now.ok && now.dispDid == base.dispDid && now.dppDid == base.dppDid; }' 'constexpr bool at_start(const Decoded &base, const Decoded &now) { return now.ok; }' "at_start is true for any readable clock"
plant 93 an internal design note 'NOT implemented**' 'implemented**' "the addendum claims the temperature stop rule"
plant 94 $P 'kDenyFrames = 9, kDenyAcquired = 10 };' 'kDenyFrames = 9, kDenyAcquired = 9 };' "the acquired denial shares the frames code"
# ---- 0.0.607: the clock hold (P4) ----
plant 95  $P 'if (restoreBad) {
        if (!hold_cas_state(c, kEvStuck))' 'if (false) {
        if (!hold_cas_state(c, kEvStuck))' "P4: the release ignores restoreBad"
plant 96  $P '    if (!hold_cas_state(c, kEvRelease)) { rep.relRc = kRlNotHeld; return kRlNotHeld; }      // someone else already claimed the release' '' "P4: the release claim is not a compare-and-swap (a second release runs)"
plant 97  $P 'if (!hold_cas_owner(c, kOwnerIdle, kOwnerHold)) {' 'if (false) {' "P4: the raise does not claim the mailbox owner word"
plant 98  $P 'if (c.ackedMask == 0u && !mailboxLost) {' 'if (true) {' "P4: a failed raise is never unwound"
plant 99  $P 'restored_ok(c.base, z) && hold_off_the_need(c, z, (1u << kClkDispclk) | (1u << kClkDppclk))' 'restored_ok(c.base, z)' "P4: a release that leaves the clocks at 545 MHz verifies"
plant 100 $P 'const uint32_t order[2] = { kClkDispclk, kClkDppclk };   // Linux' 'const uint32_t order[2] = { kClkDppclk, kClkDispclk };   // Linux' "P4: the raise sends DPPCLK first"
plant 101 $P '    const uint32_t order[2] = { kClkDppclk, kClkDispclk };
    for (uint32_t k = 0; k < 2u; k++) {
        bool acked = false;
        const uint32_t hr = io.hard_min(io.ctx, order[k], kRestoreMhz, &acked);
        rep.nMsg++;
        hold_lg(io, kHlHardMin' '    const uint32_t order[2] = { kClkDispclk, kClkDppclk };
    for (uint32_t k = 0; k < 2u; k++) {
        bool acked = false;
        const uint32_t hr = io.hard_min(io.ctx, order[k], kRestoreMhz, &acked);
        rep.nMsg++;
        hold_lg(io, kHlHardMin' "P4: the release sends DISPCLK first"
plant 102 $P '        if (io.latch_stop != nullptr) io.latch_stop(io.ctx);
        hold_lg(io, kHlStuck, 1u' '        hold_lg(io, kHlStuck, 1u' "P4: a STUCK hold (restoreBad) does not latch the DAL"
plant 103 $P '        (void)hold_cas_owner(c, kOwnerHold, kOwnerIdle);
        rep.flags |= kHfReleased;' '        rep.flags |= kHfReleased;' "P4: a verified release does not free the mailbox owner"
plant 104 $P 'level != kMaxLevel ? kHpLevel : !e1bOk ? kHpNoE1b :' 'level != kMaxLevel ? kHpLevel :' "P4: the precondition does not need E1b on this boot"
plant 105 $D 'return __atomic_load_n(&gDalOwner, __ATOMIC_SEQ_CST) == kOwnerDal;' 'return __atomic_load_n(&gDalOwner, __ATOMIC_SEQ_CST) != 0u;' "P4: dal_busy() counts the clock hold (the trial would deny itself)"
plant 106 $P 'return d.ok && d.dentAgree && d.chgDone && d.dispKhz >= needDispKhz && d.dppKhz >= needDppKhz;' 'return d.ok && d.dentAgree && d.chgDone && d.dispKhz >= needDispKhz;' "P4: the hold's DFS check ignores DPPCLK"
plant 107 $D '        if (now_us() - t0 >= kHardMinPollUs) return kHmPollTimeout;
        IOSleep(1);
    }
}
static bool hold_io_decode' '        IOSleep(1);
    }
}
static bool hold_io_decode' "P4: the hold's 0x15 poll is unbounded"
plant 108 $D 'const bool gotBusy = owner_cas(kOwnerIdle, kOwnerDal);' 'const bool gotBusy = (__atomic_store_n(&gDalOwner, 1u, __ATOMIC_SEQ_CST), true);' "P4: a DAL step takes the mailbox over from a running hold"
plant 109 $P '        const uint32_t hr = io.hard_min(io.ctx, order[k], kRestoreMhz, &acked);
        rep.nMsg++;
        hold_lg(io, kHlHardMin' '        const uint32_t hr = io.hard_min(io.ctx, order[k], kHoldMhz, &acked);
        rep.nMsg++;
        hold_lg(io, kHlHardMin' "P4: the release re-requests 530 MHz instead of the 272 floor"
plant 110 $P '    for (uint32_t k = 0; rc == kHrOk && k < 2u; k++) {
        bool acked = false;
        const uint32_t hr = io.hard_min(io.ctx, order[k], kHoldMhz, &acked);' '    for (uint32_t k = 0; rc == kHrOk && k < 1u; k++) {
        bool acked = false;
        const uint32_t hr = io.hard_min(io.ctx, order[k], kHoldMhz, &acked);' "P4: the raise sends only DISPCLK"
echo "native_s2_dal_plant: $total plants, $escaped escaped"
[ $escaped -eq 0 ]
