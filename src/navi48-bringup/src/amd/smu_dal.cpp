//
//  smu_dal.cpp - native step S2-DISPCLK (kext 0.0.604). See smu_dal.h for the shape, smu_dal_pure.h for every decision (allowlist, DID -> MHz,
//  verdict rules, the stop latch; host-tested by tests/native_s2_dal_test.cpp, which drives the same pure functions) and
//  an internal design note for the experiment.
//
//  What this file WRITES: the three DAL mailbox dwords (RESP 0x16274, ARG 0x16273, MSG 0x1628A) and nothing else - no clock register, no
//  display register. Everything it reads (CLK0 PLL_REQ / DFS_CNTL x2, DENTIST_DISPCLK_CNTL, the OTG frame counter through the scanout status
//  code) is read-only. It never sends a message the allowlist (allow_check) refuses, never sends with RESP == 0, never spins without an IOSleep,
//  and never runs from the interrupt handler or under a display lock (its only caller is the N48N selector 15 on a user-client thread).
//
#include <string.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>

#include "smu_dal.h"
#include "smu_dal_pure.h"
#include "amdgpu_smu.h"
#include "amdgpu_log.h"
#include "native_s1b.h"
#include "../dcn/navi48_dcn.hpp"

#define DAL_LOG(fmt, ...) AMDGPU_LOG("dal", fmt, ##__VA_ARGS__)

namespace amdgpu {

using namespace n48dal;

// Per-boot state: what E1b recorded (the DPM maxima the allowlist bounds a hard-min by) and the sticky stop latch. Zero-initialised: nothing recorded, not stopped.
struct DalState { bool e1bOk; uint32_t max[2]; Latch latch; };
static DalState gDal;
// 0.0.607: the mailbox OWNER word (was gBusy). 0 idle, 1 a DAL step runs, 2 the mode trial's clock hold (P4) holds the mailbox. dal_busy() is owner 1 ONLY, so the mode trial that owns a hold
// is not denied by its own hold (F9: mt_in_use() is true while dal_busy()); a DAL step and a hold refuse each other through the compare-and-swaps below.
static uint32_t gDalOwner;
static bool owner_cas(uint32_t from, uint32_t to) { return __atomic_compare_exchange_n(&gDalOwner, &from, to, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
static n48n_scan_status gSt;             // file-scope: the stack stays small; used only while gDalOwner is 1
static n48n_scan_query gQ;

static uint64_t now_us() {
    uint64_t t = 0, ns = 0;
    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &ns);
    return ns / 1000ull;
}
static uint32_t us32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v; }

// ---- the gate --------------------------------------------------------------------------------------------------------------------------
static bool native_gate_ok() {
    const NativeS1bState &s = native_s1b_state();
    return s.gate == n48native::kGateOn && s.positivePass;
}
static uint32_t boot_arg_level() {
    uint32_t v = 0;
    if (!PE_parse_boot_argn("navi48-dalsmc", &v, sizeof(v))) return 0u;
    return v;
}
uint32_t dal_gated_level() {
    if (!native_gate_ok()) return 0u;
    const uint32_t v = boot_arg_level();
    return (v >= 1u && v <= kMaxLevel) ? v : 0u;
}

// ---- the sender ------------------------------------------------------------------------------------------------------------------------
static kern_return_t resp_to_rc(uint32_t resp) {
    switch (resp) {
    case 0u:                  return kIOReturnTimeout;
    case 0xFFFFFFFFu:         return kIOReturnNoDevice;
    case kRespOk:             return kIOReturnSuccess;
    case kRespFailed:         return kIOReturnError;
    case kRespUnknownCmd:     return kIOReturnUnsupported;
    case kRespRejectedPrereq: return kIOReturnNotReady;
    case kRespRejectedBusy:   return kIOReturnBusy;
    default:                  return kIOReturnInternalError;
    }
}

kern_return_t smu_dal_send(const DeviceContext &dev, uint32_t msg, uint32_t param, DalMsg *m) {
    DalMsg local;
    DalMsg *o = m != nullptr ? m : &local;
    memset(o, 0, sizeof(*o));
    // 1. The allowlist, before anything else. Level 0 (no boot-arg, or not a native boot with S1b POSITIVE PASS) refuses everything.
    const uint32_t why = allow_check(dal_gated_level(), msg, param, gDal.max[0], gDal.max[1]);
    if (why != kAllow) {
        o->refuse = why; o->rc = (uint32_t)kIOReturnNotPermitted;
        DAL_LOG("send REFUSED msg=0x%02x param=0x%08x reason=%u (nothing sent)", msg, param, why);
        return (kern_return_t)o->rc;
    }
    // 2. The three dwords must be inside the BAR5 mapping (RREG32 answers all-ones outside it and WREG32 silently drops the write).
    if (!dev.rmmio || ((size_t)kRegMsg * 4u + 4u) > dev.rmmioSize || ((size_t)kRegArg * 4u + 4u) > dev.rmmioSize || ((size_t)kRegResp * 4u + 4u) > dev.rmmioSize) {
        o->refuse = kRefNoDevice; o->rc = (uint32_t)kIOReturnNoDevice;
        DAL_LOG("send REFUSED msg=0x%02x: the DAL mailbox dwords are outside the BAR5 mapping (rmmio %llu bytes)", msg, (unsigned long long)dev.rmmioSize);
        return (kern_return_t)o->rc;
    }
    // 3. The shared mailbox lock (PPSMC and DAL). No lock -> no send.
    SmuSeq seq;
    if (!seq.held) {
        o->refuse = kRefNoLock; o->rc = (uint32_t)kIOReturnNoResources;
        DAL_LOG("send REFUSED msg=0x%02x: the shared mailbox lock is unavailable", msg);
        return (kern_return_t)o->rc;
    }
    // 4. RESP must be non-zero BEFORE the send (Linux polls for it; a zero here means a message is in flight or the mailbox is unhealthy).
    const uint32_t pre = RREG32(dev, kRegResp);
    o->preResp = pre;
    if (resp_pre_check(pre) != kAllow) {
        o->refuse = kRefRespZero; o->rc = (uint32_t)kIOReturnNotReady;
        DAL_LOG("send REFUSED msg=0x%02x param=0x%08x: RESP=0x%x before the send (nothing sent)", msg, param, pre);
        return (kern_return_t)o->rc;
    }
    // 5. The Linux sequence: RESP = 0, ARG = param, MSG = id, then poll RESP != 0. Bounded (500 ms), IOSleep between reads.
    const uint64_t t0 = now_us();
    WREG32(dev, kRegResp, 0u);
    WREG32(dev, kRegArg, param);
    WREG32(dev, kRegMsg, msg);
    uint32_t resp = 0;
    for (;;) {
        resp = RREG32(dev, kRegResp);
        if (resp != 0u) break;
        if (now_us() - t0 >= kRespWaitUs) break;
        IOSleep(1);
    }
    o->us = us32(now_us() - t0);
    o->resp = resp;
    o->arg = RREG32(dev, kRegArg);
    o->rc = (uint32_t)resp_to_rc(resp);
    DAL_LOG("msg=0x%02x param=0x%08x preresp=0x%x resp=0x%x (%s) arg=0x%08x us=%u", msg, param, pre, resp, resp_name(resp), o->arg, o->us);
    return (kern_return_t)o->rc;
}

// ---- one experiment run ----------------------------------------------------------------------------------------------------------------
struct Run {
    DeviceContext *dev;
    n48n_dal_result *r;
    uint32_t verdict;        // the verdict so far (kVNone until decided)
    bool sentAny;            // at least one DAL message went out in THIS step
    bool timedOut;           // the mailbox stopped answering: no further send, no restore
    uint32_t raisedMask;     // bit clk: a hard-min raise of that clock was acknowledged (the restore covers exactly these)
    bool hardMinSent;        // a SetHardMinByFreq went out in this step (a timeout after that leaves the clock state unknown)
};
static Run gRun;

static Decoded read_decoded() {
    const DeviceContext &d = *gRun.dev;
    return decode_clocks(RREG32(d, kRegPllReq), RREG32(d, kRegDfs0), RREG32(d, kRegDfs1), RREG32(d, kRegDentist));
}

// Send one message on behalf of the running step. Returns 0 when the reply was OK. Otherwise records the failure (first one only), sets the
// verdict and returns non-zero.
static uint32_t dal_msg(uint32_t msg, uint32_t param, uint32_t *arg) {
    DalMsg m;
    const kern_return_t rc = smu_dal_send(*gRun.dev, msg, param, &m);
    n48n_dal_result *r = gRun.r;
    if (m.refuse == 0u) { r->nmsg++; gRun.sentAny = true; r->last_resp = m.resp; r->last_arg = m.arg; r->last_us = m.us; }
    if (arg != nullptr) *arg = m.arg;
    if (rc == kIOReturnSuccess) return 0u;
    if (r->rc == 0u) { r->rc = (uint32_t)rc; r->fail_msg = msg; r->refuse = m.refuse; }
    if (m.refuse != 0u) {
        // Nothing was sent. Before the first message of the step that is a plain denial (no latch); after it, the state is uncertain: a stop.
        gRun.verdict = gRun.sentAny ? kRefused : kDenied;
    } else if (rc == kIOReturnTimeout) { gRun.verdict = kTimeout; gRun.timedOut = true; }
    else if (rc == kIOReturnNoDevice) gRun.verdict = kGlitch;
    else gRun.verdict = kRefused;
    return 1u;
}

// A query whose non-OK reply is RECORDED but does not stop the step (E1b's DISPCLK GetDcModeMaxDpmFreq). A mailbox timeout or a refusal still stops it
// (through dal_msg's rules): only a well-formed non-OK reply from a live mailbox is soft. Returns the raw RESP (0 = not answered).
static uint32_t dal_msg_soft(uint32_t msg, uint32_t param, uint32_t *arg, uint32_t *resp) {
    DalMsg m;
    const kern_return_t rc = smu_dal_send(*gRun.dev, msg, param, &m);
    n48n_dal_result *r = gRun.r;
    if (m.refuse == 0u) { r->nmsg++; gRun.sentAny = true; r->last_resp = m.resp; r->last_arg = m.arg; r->last_us = m.us; }
    if (arg != nullptr) *arg = m.arg;
    if (resp != nullptr) *resp = m.resp;
    if (rc == kIOReturnSuccess) return 0u;
    const bool soft = m.refuse == 0u && m.resp != 0u && m.resp != 0xFFFFFFFFu;   // answered, but not OK
    if (soft) return 0u;
    if (r->rc == 0u) { r->rc = (uint32_t)rc; r->fail_msg = msg; r->refuse = m.refuse; }
    if (m.refuse != 0u) gRun.verdict = gRun.sentAny ? kRefused : kDenied;
    else if (rc == kIOReturnTimeout) { gRun.verdict = kTimeout; gRun.timedOut = true; }
    else gRun.verdict = kGlitch;
    return 1u;
}

// SetHardMinByFreq then ReturnHardMinStatus polled every 1 ms for at most 1 s until bit `clk` is set. Returns 0 = acknowledged and reported done,
// 1 = a message failed (dal_msg recorded it; a MAILBOX timeout also sets gRun.timedOut), 2 = the poll ran out its 1 s while every message was answered
// (the mailbox is alive: verdict TIMEOUT, flag POLL_TIMEOUT, gRun.timedOut stays clear so the restore is still attempted).
static uint32_t hard_min(uint32_t clk, uint32_t mhz, bool isRaise) {
    uint32_t arg = 0;
    const uint32_t nBefore = gRun.r->nmsg;
    const uint32_t rc9 = dal_msg(kMsgSetHardMinByFreq, param_hardmin(clk, mhz), &arg);
    if (gRun.r->nmsg != nBefore && isRaise) gRun.hardMinSent = true;   // it went out (whatever the reply)
    if (rc9 != 0u) return 1u;
    if (isRaise) gRun.raisedMask |= 1u << clk;   // acknowledged: from here the clock may be up, whatever the poll says
    if (isRaise) gRun.r->grant_reply[clk - kClkDispclk] = arg;   // the raise's reply; the restore's does not overwrite it
    const uint64_t t0 = now_us();
    for (;;) {
        if (dal_msg(kMsgReturnHardMinStatus, 0u, &arg) != 0u) return 1u;
        if ((arg & (1u << clk)) != 0u) break;
        if (now_us() - t0 >= kHardMinPollUs) {
            gRun.r->hardmin_poll_us = us32(now_us() - t0);
            if (gRun.r->rc == 0u) { gRun.r->rc = (uint32_t)kIOReturnTimeout; gRun.r->fail_msg = kMsgReturnHardMinStatus; }
            gRun.verdict = kTimeout; gRun.r->flags |= N48N_DAL_F_POLL_TIMEOUT;   // NOT gRun.timedOut: the mailbox answered every poll
            DAL_LOG("hard-min clk %u: ReturnHardMinStatus never reported done in %u us", clk, gRun.r->hardmin_poll_us);
            return 2u;
        }
        IOSleep(1);
    }
    gRun.r->hardmin_poll_us = us32(now_us() - t0);
    return 0u;
}

// The OTG frame counter (24 bits) through the S2a scanout status code (a read; needs the display layer bound, which the client did).
static bool frame_now(uint32_t *fc) {
    if (n48dcn::scanStatus(&gSt) != 0u) return false;
    *fc = (uint32_t)(gSt.frame_count & 0xFFFFFFull);
    return true;
}

static void fill_before(const Decoded &b) {
    n48n_dal_result *r = gRun.r;
    r->vco_khz = b.vcoKhz;
    r->dfs_before[0] = RREG32(*gRun.dev, kRegDfs0); r->dfs_before[1] = RREG32(*gRun.dev, kRegDfs1);
    r->khz_before[0] = b.dispKhz; r->khz_before[1] = b.dppKhz;
    r->pll_req = RREG32(*gRun.dev, kRegPllReq);
    r->dentist_before = RREG32(*gRun.dev, kRegDentist);
}
static void fill_after(const Decoded &a, uint32_t *khz) {
    khz[0] = a.dispKhz; khz[1] = a.dppKhz;
}

// E1b: the level-1 queries. Records the DPM tables and the maxima the allowlist will bound a hard-min by.
static void step_e1b(const Decoded &b0) {
    n48n_dal_result *r = gRun.r;
    uint32_t arg = 0;
    uint32_t nlev[2] = { 0, 0 }, mx[2] = { 0, 0 };
    if (dal_msg(kMsgGetSmuVersion, 0u, &arg) != 0u) return;    r->smu_version = arg;
    if (dal_msg(kMsgGetDriverIfVersion, 0u, &arg) != 0u) return; r->if_version = arg;
    if (dal_msg(kMsgGetMsgHeaderVersion, 0u, &arg) != 0u) return; r->header_version = arg;
    for (uint32_t i = 0; i < 2u; i++) {
        const uint32_t clk = kClkDispclk + i;
        if (dal_msg(kMsgGetDpmFreqByIndex, (clk << 16) | kIndexAll, &arg) != 0u) return;
        // The level count exactly as dcn401_init_single_clock reads it: bit 31 = fine-grained (2 levels: min and max), else the low 8 bits.
        const uint32_t cnt = level_count(arg);
        if (level_fine_grained(arg)) r->flags |= N48N_DAL_F_FINE_GRAINED;
        if (cnt == 0u || cnt > kDpmMaxLevels) {   // the message failed (0), or a table this build cannot hold
            gRun.verdict = kGlitch; r->rc = r->rc ? r->rc : (uint32_t)kIOReturnBadArgument; r->fail_msg = kMsgGetDpmFreqByIndex;
            DAL_LOG("E1b: clk %u level-count reply 0x%08x = %u levels (1..%u expected)", clk, arg, cnt, kDpmMaxLevels);
            return;
        }
        nlev[i] = cnt;
        for (uint32_t l = 0; l < nlev[i]; l++) {
            if (dal_msg(kMsgGetDpmFreqByIndex, (clk << 16) | l, &arg) != 0u) return;
            const uint32_t mhz = level_mhz(arg);   // & 0xFFFF, as Linux; no kHz guessing on this message
            r->dpm_mhz[i][l] = (uint16_t)mhz;
            if (mhz > mx[i]) mx[i] = mhz;
        }
        // GetDcModeMaxDpmFreq: DISPCLK only (Linux never asks DPPCLK); a non-OK reply is recorded and the step goes on.
        if (clk == kClkDispclk) {
            uint32_t resp = 0;
            if (dal_msg_soft(kMsgGetDcModeMaxDpmFreq, clk << 16, &arg, &resp) != 0u) return;
            r->dc_max_resp = resp;
            if (resp == kRespOk) {
                if (arg >= 20000u) r->flags |= N48N_DAL_F_UNIT_KHZ_SEEN;
                r->dc_max_mhz[i] = norm_mhz(arg);
            }
        }
        r->dpm_count[i] = nlev[i]; r->dpm_max_mhz[i] = mx[i];
    }
    // The queries must not have moved a clock.
    const Decoded a = read_decoded();
    if (!a.ok || a.dispDid != b0.dispDid || a.dppDid != b0.dppDid) { gRun.verdict = kGlitch; DAL_LOG("E1b: a clock moved during the queries"); return; }
    gRun.verdict = judge_e1b(nlev[0], mx[0]);
    if (mx[0] < kE1bPassMaxMhz) r->flags |= N48N_DAL_F_LOW_MAX;
    if (mx[0] < kE1bOdmMaxMhz) r->flags |= N48N_DAL_F_ODM;
    if (e1b_enables(nlev[0], mx[0], mx[1])) {
        gDal.max[0] = mx[0]; gDal.max[1] = mx[1]; gDal.e1bOk = true;
        r->flags |= N48N_DAL_F_E1B_DONE;
    }
}

// E2 / E3 / E4: raise, settle, dwell, restore.
static void step_raise(uint32_t step, const Decoded &b0) {
    n48n_dal_result *r = gRun.r;
    const Plan plan = plan_for(step);
    // Baseline: a state the experiment may start from.
    if (!baseline_ok(b0)) { gRun.verdict = kDenied; r->deny = kDenyBaseline; DAL_LOG("%s: baseline refused (disp %u kHz did 0x%x, dpp %u kHz did 0x%x, dentist agree %d chg_done %d)",
                            step == kE2 ? "E2" : step == kE3 ? "E3" : "E4", b0.dispKhz, b0.dispDid, b0.dppKhz, b0.dppDid, b0.dentAgree ? 1 : 0, b0.chgDone ? 1 : 0); return; }
    // Pre-flight: the OTG is lit and its frame counter runs; a 2 s window gives the reference rate the dwell is judged against.
    uint32_t f0 = 0, f1 = 0;
    if (n48dcn::scanQuery(&gQ) == 0u && (gQ.flags & N48N_SCANQ_ACQUIRED) != 0u) { gRun.verdict = kDenied; r->deny = kDenyAcquired; DAL_LOG("dal-step: the scanout plane is acquired (a client is presenting): refused"); return; }
    if (n48dcn::scanQuery(&gQ) != 0u || (gQ.flags & N48N_SCANQ_LIT) == 0u || !frame_now(&f0)) { gRun.verdict = kDenied; r->deny = kDenyFrames; DAL_LOG("dal-step: pre-flight: the OTG is not lit or unreadable"); return; }
    const uint64_t tr0 = now_us();
    IOSleep(2000);
    if (!frame_now(&f1)) { gRun.verdict = kDenied; r->deny = kDenyFrames; return; }
    const uint32_t refFrames = frame_delta(f0, f1), refMs = us32((now_us() - tr0) / 1000ull);
    if (refFrames < 20u || refMs == 0u) { gRun.verdict = kDenied; r->deny = kDenyFrames; DAL_LOG("dal-step: pre-flight: %u frames in %u ms: the scanout is not running", refFrames, refMs); return; }
    r->target_mhz[0] = 0; r->target_mhz[1] = 0;
    for (uint32_t i = 0; i < plan.nRaise; i++) r->target_mhz[plan.raise[i].clk - kClkDispclk] = plan.raise[i].mhz;

    // ---- raise (E4: DISPCLK first, then DPPCLK, as Linux's block sequence) ----
    for (uint32_t i = 0; i < plan.nRaise; i++)
        if (hard_min(plan.raise[i].clk, plan.raise[i].mhz, true) != 0u) break;
    Decoded a = read_decoded();
    if (gRun.verdict == kVNone) {
        // Settle: the DFS may take a moment to follow the granted floor. Up to 300 ms, every 5 ms, until the reading passes.
        uint32_t j = judge_reading(plan, b0, a);
        for (uint32_t k = 0; j != kPass && k < 60u; k++) { IOSleep(5); a = read_decoded(); j = judge_reading(plan, b0, a); }
        if (j != kPass) gRun.verdict = j;
    }
    r->dentist_after = RREG32(*gRun.dev, kRegDentist);
    r->dfs_after[0] = RREG32(*gRun.dev, kRegDfs0); r->dfs_after[1] = RREG32(*gRun.dev, kRegDfs1);
    fill_after(a, r->khz_after);
    // ---- dwell ----
    if (gRun.verdict == kVNone) {
        uint32_t fs = 0, fl = 0, fe = 0;
        if (!frame_now(&fs)) gRun.verdict = kGlitch;
        else {
            fl = fs; r->frame_before = fs;
            const uint64_t ts = now_us();
            uint64_t tAdv = ts;
            while (gRun.verdict == kVNone && now_us() - ts < (uint64_t)plan.dwellMs * 1000ull) {
                IOSleep(100);
                uint32_t fc = 0;
                if (!frame_now(&fc)) { gRun.verdict = kGlitch; break; }
                if (frame_delta(fl, fc) != 0u) tAdv = now_us();
                fl = fc;
                if (now_us() - tAdv >= (uint64_t)kStallMs * 1000ull) { gRun.verdict = kGlitch; DAL_LOG("dwell: the OTG frame counter stalled for %u ms", kStallMs); break; }
                const uint32_t j = judge_dwell(plan, b0, read_decoded());
                if (j != kPass) { gRun.verdict = j; break; }
            }
            fe = fl;
            const uint32_t dwellMs = us32((now_us() - ts) / 1000ull);
            r->frame_after = fe; r->dwell_ms = dwellMs;
            if (gRun.verdict == kVNone && !rate_ok(refFrames, refMs, frame_delta(fs, fe), dwellMs)) {
                gRun.verdict = kGlitch;
                DAL_LOG("dwell: %u frames in %u ms against the reference %u in %u ms", frame_delta(fs, fe), dwellMs, refFrames, refMs);
            }
            if (gRun.verdict == kVNone) gRun.verdict = kPass;
        }
    }
    // ---- restore (never after a MAILBOX timeout: the mailbox is not answering; a poll timeout still restores) ----
    // The clocks to restore: every acknowledged raise, plus any clock that now reads below its start value (a regression can hit a clock the step never raised).
    const Decoded zb = read_decoded();
    const uint32_t mask = restore_mask(gRun.raisedMask, b0, zb);
    if (gRun.timedOut && gRun.hardMinSent) {
        // The mailbox stopped answering after a hard-min went out: the clock may be up and no message can restore it.
        r->flags |= N48N_DAL_F_RESTORE_FAILED;
        DAL_LOG("RESTORE NOT ATTEMPTED: the DAL mailbox stopped answering; cold power cycle required");
    } else if (mask != 0u && !gRun.timedOut) {
        {
            const uint32_t savedVerdict = gRun.verdict, savedRc = r->rc, savedMsg = r->fail_msg, savedRefuse = r->refuse;
            bool restoreBad = false;
            uint32_t restoreVerdict = kVNone, done = 0u;
            // Reverse of the raise order first, then any clock below its start that the step never raised.
            uint32_t order[4], n = 0;
            for (uint32_t i = plan.nRaise; i-- > 0u; ) order[n++] = plan.raise[i].clk;
            order[n++] = kClkDispclk; order[n++] = kClkDppclk;
            for (uint32_t k = 0; k < n; k++) {
                const uint32_t clk = order[k];
                if ((mask & (1u << clk)) == 0u || (done & (1u << clk)) != 0u) continue;   // only clocks that were raised or are below their start, once each
                done |= 1u << clk;
                gRun.verdict = kVNone;
                const uint32_t hr = hard_min(clk, kRestoreMhz, false);
                if (hr == 1u) {
                    restoreBad = true;
                    if (restoreVerdict == kVNone) restoreVerdict = gRun.verdict;
                    if (gRun.timedOut) break;
                }
                // hr == 2 (a poll timeout): the readback below decides whether the clock came back.
            }
            // Keep the first failure's numbers; a restore failure only shows when nothing failed before it.
            if (savedRc != 0u) { r->rc = savedRc; r->fail_msg = savedMsg; r->refuse = savedRefuse; }
            gRun.verdict = savedVerdict;
            Decoded z = read_decoded();
            for (uint32_t k = 0; !restoreBad && !restored_ok(b0, z) && k < 100u; k++) { IOSleep(5); z = read_decoded(); }
            fill_after(z, r->khz_restored);
            if (restoreBad || !restored_ok(b0, z)) {
                r->flags |= N48N_DAL_F_RESTORE_FAILED;
                if (gRun.verdict == kPass) gRun.verdict = restoreBad ? (restoreVerdict != kVNone ? restoreVerdict : kRefused) : kRegressed;
                DAL_LOG("RESTORE DID NOT VERIFY (disp %u kHz, dpp %u kHz, start %u / %u): cold power cycle required", z.dispKhz, z.dppKhz, b0.dispKhz, b0.dppKhz);
            } else {
                r->flags |= N48N_DAL_F_RESTORED;
                r->flags |= at_start(b0, z) ? N48N_DAL_F_AT_START : N48N_DAL_F_ABOVE_START;   // back at the start, or a floor still holding it above
            }
        }
    }
}

bool dal_busy() { return __atomic_load_n(&gDalOwner, __ATOMIC_SEQ_CST) == kOwnerDal; }

IOReturn dal_run_step(DeviceContext &dev, uint32_t step, n48n_dal_result *out) {
    if (out == nullptr || step < kE1b || step > kStepLast) return kIOReturnBadArgument;
    memset(out, 0, sizeof(*out));
    out->step = step;
    out->underflow = N48N_DAL_UNDERFLOW_NA;
    out->flags = N48N_DAL_F_UNDERFLOW_UNREAD | N48N_DAL_F_VUPDATE_UNCOUNTED;
    out->level = boot_arg_level();
    // 0.0.605: the DAL step and the mode trial refuse each other. Each sets its own flag FIRST (sequentially consistent) and then reads the other's, so at least one of the two sees the other.
    const bool gotBusy = owner_cas(kOwnerIdle, kOwnerDal);   // (owner 2 = a clock hold is up: the step is DENIED busy)
    const bool busy = !gotBusy || n48dcn::modeTrialBusy();
    const uint32_t gated = dal_gated_level();
    const uint32_t deny = pre_gate(native_gate_ok(), gated, step, gDal.e1bOk, gDal.latch.stopped, busy);
    if (deny != kDenyNone) {
        out->verdict = kDenied; out->deny = deny; out->rc = (uint32_t)kIOReturnNotPermitted;
        out->latched = gDal.latch.stopped ? 1u : 0u;
        if (gDal.latch.stopped) out->flags |= N48N_DAL_F_LATCHED;
        if (gDal.e1bOk) out->flags |= N48N_DAL_F_E1B_DONE;
        DAL_LOG("dal-step %u: DENIED (reason %u; boot-arg level %u, gated level %u, latched %d, e1b %d, busy %d)", step, deny, out->level, gated,
                gDal.latch.stopped ? 1 : 0, gDal.e1bOk ? 1 : 0, busy ? 1 : 0);
        if (gotBusy) owner_cas(kOwnerDal, kOwnerIdle);
        return kIOReturnSuccess;
    }
    memset(&gRun, 0, sizeof(gRun));
    gRun.dev = &dev; gRun.r = out;
    if (step == kE1b) gDal.e1bOk = false;         // a failed re-run leaves E1b un-done

    const Decoded b0 = read_decoded();
    fill_before(b0);
    if (!b0.ok) { gRun.verdict = kDenied; out->deny = kDenyBaseline; DAL_LOG("dal-step %u: the clock registers are unreadable", step); }
    else if (step == kE1b) step_e1b(b0);
    else step_raise(step, b0);

    if (gRun.verdict == kVNone) gRun.verdict = kGlitch;   // a path that decided nothing is not a pass
    out->verdict = gRun.verdict;
    gDal.latch = latch_apply(gDal.latch, step, gRun.verdict);
    out->latched = gDal.latch.stopped ? 1u : 0u;
    if (gDal.latch.stopped) out->flags |= N48N_DAL_F_LATCHED;
    if (gDal.e1bOk) out->flags |= N48N_DAL_F_E1B_DONE;
    if (out->dentist_after == 0u) out->dentist_after = RREG32(dev, kRegDentist);
    DAL_LOG("dal-step E%s verdict=%s(%u) rc=0x%x failmsg=0x%x nmsg=%u latched=%u flags=0x%x khz before %u/%u after %u/%u restored %u/%u frames %u..%u dwell %u ms",
            step == kE1b ? "1b" : step == kE2 ? "2" : step == kE3 ? "3" : "4", verdict_name(out->verdict), out->verdict, out->rc, out->fail_msg, out->nmsg,
            out->latched, out->flags, out->khz_before[0], out->khz_before[1], out->khz_after[0], out->khz_after[1], out->khz_restored[0], out->khz_restored[1],
            out->frame_before, out->frame_after, out->dwell_ms);
    owner_cas(kOwnerDal, kOwnerIdle);
    return kIOReturnSuccess;
}

// ---- 0.0.607, P4: the clock hold (an internal design note P4). The orchestration and every decision are in smu_dal_pure.h (hold_raise / hold_release, driven by the host tests); this is the hardware
// half. Callers: the mode trial's trial thread (raise before the first display write; release after the 60 Hz restore verified), `dcnmode 0` and the kext stop (release after a verified golden restore).
// NEVER the watchdog, never the interrupt handler, never under gMtLock or the scanout lock: it sleeps and waits on the PMFW.
static HoldCtl gHold = { kHsIdle, &gDalOwner, Decoded{}, 0u, { 0u, 0u } };
static DeviceContext *gHoldDev;
static uint32_t hold_io_hard_min(void *, uint32_t clk, uint32_t mhz, bool *acked) {
    *acked = false;
    DalMsg m;
    kern_return_t rc = smu_dal_send(*gHoldDev, kMsgSetHardMinByFreq, param_hardmin(clk, mhz), &m);
    if (rc != kIOReturnSuccess) return (m.refuse == 0u && (rc == kIOReturnTimeout || rc == kIOReturnNoDevice)) ? kHmMailbox : kHmFailed;
    *acked = true;
    const uint64_t t0 = now_us();
    for (;;) {
        rc = smu_dal_send(*gHoldDev, kMsgReturnHardMinStatus, 0u, &m);
        if (rc != kIOReturnSuccess) return (m.refuse == 0u && (rc == kIOReturnTimeout || rc == kIOReturnNoDevice)) ? kHmMailbox : kHmFailed;
        if ((m.arg & (1u << clk)) != 0u) return kHmOk;
        if (now_us() - t0 >= kHardMinPollUs) return kHmPollTimeout;
        IOSleep(1);
    }
}
static bool hold_io_decode(void *, Decoded *d) {
    const DeviceContext &dv = *gHoldDev;
    *d = decode_clocks(RREG32(dv, kRegPllReq), RREG32(dv, kRegDfs0), RREG32(dv, kRegDfs1), RREG32(dv, kRegDentist));
    return true;
}
static void hold_io_sleep(void *, uint32_t ms) { IOSleep(ms); }
static uint64_t hold_io_now(void *) { return now_us(); }
static void hold_io_latch(void *) { gDal.latch = latch_apply(gDal.latch, kE4, kRegressed); }
static void hold_io_log(void *, uint32_t code, uint32_t a, uint32_t b, uint32_t c) {
    switch (code) {
    case kHlRaiseBegin:   DAL_LOG("hold: raise begins (need %u / %u kHz, target %u MHz)", a, b, kHoldMhz); break;
    case kHlHardMin:      DAL_LOG("hold: hard-min clk %u -> code %u acked %u", a, b, c); break;
    case kHlRaised:       DAL_LOG("hold: HELD (DISPCLK %u kHz, DPPCLK %u kHz, %u ms)", a, b, c); break;
    case kHlUnwind:       DAL_LOG("hold: raise failed: unwinding (acked mask %#x, mailbox lost %u)", a, b); break;
    case kHlReleaseBegin: DAL_LOG("hold: release begins (single claim)"); break;
    case kHlReleased:     DAL_LOG("hold: RELEASED (DISPCLK %u kHz, DPPCLK %u kHz, %u ms)", a, b, c); break;
    case kHlStuck:        DAL_LOG("hold: *** STUCK (%s; rc %u): the clocks may stay raised - harmless at 60 Hz; cold power cycle *** (DAL steps latched)", a == 1u ? "the 60 Hz restore did NOT verify: never released" : a == 2u ? "the release did not verify" : "the raise could not be undone", b); break;
    case kHlRefused:      DAL_LOG("hold: refused before any message (pre-check %u)", a); break;
    default: break;
    }
}
static HoldIo hold_io() { return HoldIo{ nullptr, hold_io_hard_min, hold_io_decode, hold_io_sleep, hold_io_now, hold_io_latch, hold_io_log }; }

uint32_t dal_hold_pre() { return hold_pre_check(dal_gated_level(), gDal.e1bOk, gDal.latch.stopped, hold_state_now(gHold), hold_owner_now(gHold)); }
uint32_t dal_hold_state() { return hold_state_now(gHold); }
uint32_t dal_hold_raise(DeviceContext &dev, uint32_t needDispKhz, uint32_t needDppKhz, HoldRep *rep, Decoded *after) {
    HoldRep local;
    HoldRep *r = rep != nullptr ? rep : &local;
    gHoldDev = &dev;
    const HoldIo io = hold_io();
    return hold_raise(io, gHold, *r, dal_gated_level(), gDal.e1bOk, gDal.latch.stopped, needDispKhz, needDppKhz, after);
}
uint32_t dal_hold_release(DeviceContext &dev, bool restoreBad, HoldRep *rep) {
    HoldRep local;
    HoldRep *r = rep != nullptr ? rep : &local;
    gHoldDev = &dev;
    const HoldIo io = hold_io();
    return hold_release(io, gHold, *r, restoreBad);
}
}  // namespace amdgpu
