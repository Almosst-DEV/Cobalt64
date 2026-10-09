//
//  smu_dal_pure.h - the PURE half of native step S2-DISPCLK (kext 0.0.604): the DAL mailbox allowlist, the DID -> MHz table, the clock
//  readback decode, the step plans, the verdict rules and the sticky stop latch. No kernel header is included, so
//  tests/native_s2_dal_test.cpp compiles this on the host Mac and drives the exact functions the kext calls (smu_dal.cpp).
//  Design: an internal design note sections 0, 3 and 4.
//
//  Everything here is arithmetic and decisions; nothing touches a register.
//
#pragma once
#include <stdint.h>

namespace n48dal {

// ---- the three DAL mailbox dwords (absolute BAR5 dword indexes; compile-time constants, section 1 of the design) -------------------
constexpr uint32_t kRegResp = 0x16274u;   // mmDAL_RESP_REG  (C2PMSG_52)
constexpr uint32_t kRegArg  = 0x16273u;   // mmDAL_ARG_REG   (C2PMSG_51)
constexpr uint32_t kRegMsg  = 0x1628Au;   // mmDAL_MSG_REG   (C2PMSG_74)
// The PPSMC triple (smu_v14_0.cpp) - the DAL triple must stay disjoint from it.
constexpr uint32_t kPpsmcMsg = 0x16282u, kPpsmcArg = 0x16292u, kPpsmcResp = 0x1629Au;
static_assert(kRegResp != kPpsmcMsg && kRegResp != kPpsmcArg && kRegResp != kPpsmcResp &&
              kRegArg != kPpsmcMsg && kRegArg != kPpsmcArg && kRegArg != kPpsmcResp &&
              kRegMsg != kPpsmcMsg && kRegMsg != kPpsmcArg && kRegMsg != kPpsmcResp, "the DAL mailbox is disjoint from the PPSMC one");

// ---- the clock / display registers the experiment reads (absolute dwords; E1 read all of them on the native boot) -----------------
constexpr uint32_t kRegPllReq = 0x16E37u;   // CLK0_CLK_PLL_REQ
constexpr uint32_t kRegDfs0   = 0x16E69u;   // CLK0_CLK0_DFS_CNTL  (DISPCLK slice)
constexpr uint32_t kRegDfs1   = 0x16E6Cu;   // CLK0_CLK1_DFS_CNTL  (DPPCLK slice)
constexpr uint32_t kRegDentist = 0x124u;    // DENTIST_DISPCLK_CNTL

// ---- DAL message ids (dalsmc.h) -----------------------------------------------------------------------------------------------------
constexpr uint32_t kMsgGetSmuVersion = 0x2u, kMsgGetDriverIfVersion = 0x3u, kMsgGetMsgHeaderVersion = 0x4u;
constexpr uint32_t kMsgSetHardMinByFreq = 0x9u, kMsgGetDpmFreqByIndex = 0xBu, kMsgGetDcModeMaxDpmFreq = 0xCu, kMsgReturnHardMinStatus = 0x15u;
constexpr uint32_t kClkDispclk = 6u, kClkDppclk = 7u;
// Reply codes.
constexpr uint32_t kRespOk = 1u, kRespFailed = 0xFFu, kRespUnknownCmd = 0xFEu, kRespRejectedPrereq = 0xFDu, kRespRejectedBusy = 0xFCu;

// ---- limits ---------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMaxLevel = 4u;
constexpr uint32_t kMinMhz = 250u;            // never request below (the boot clock is 272.73)
constexpr uint32_t kCapLevel23Mhz = 320u;     // E2 / E3
constexpr uint32_t kCapLevel4Mhz = 600u;      // E4
constexpr uint32_t kRestoreMhz = 272u;        // the restore request: (6 << 16) | 272. 0.0.606 (S2-120HZ F1): 272, NOT 273 - Linux asks khz_to_mhz_floor first, and a 273 floor is ABOVE the 272.73 MHz boot clock (DID 0x40 = 281.25 MHz), so a 273 restore could never read AT_START
constexpr uint32_t kRespWaitUs = 2000u * 1000u;        // RESP <= 2 s (Linux's bound: 10 us x 200000)
constexpr uint32_t kHardMinPollUs = 1000u * 1000u;     // ReturnHardMinStatus polled <= 1 s, every 1 ms
constexpr uint32_t kDpmMaxLevels = 16u;
constexpr uint32_t kIndexAll = 0xFFu;         // GetDpmFreqByIndex(0xFF) = the number of levels

// ---- the allowlist (design section 4) --------------------------------------------------------------------------------------------
// Reasons a message is not sent. kAllow = 0. Nothing is sent for any other value.
enum Refuse : uint32_t {
    kAllow = 0, kRefLevel = 1, kRefMsg = 2, kRefParam = 3, kRefClk = 4, kRefLow = 5, kRefCap = 6, kRefNoMax = 7, kRefAboveMax = 8, kRefRespZero = 9, kRefNoLock = 10, kRefNoDevice = 11
};
constexpr bool clk_ok(uint32_t clk) { return clk == kClkDispclk || clk == kClkDppclk; }
constexpr uint32_t cap_for_level(uint32_t level) { return level >= 4u ? kCapLevel4Mhz : (level >= 2u ? kCapLevel23Mhz : 0u); }
// level: the boot-arg (navi48-dalsmc), already gated by the native boot + S1b POSITIVE PASS (0 otherwise). max6 / max7: the DPM maxima (MHz)
// E1b recorded on this boot for DISPCLK / DPPCLK, 0 = E1b has not run (which refuses every SetHardMin).
constexpr uint32_t allow_check(uint32_t level, uint32_t msg, uint32_t param, uint32_t max6, uint32_t max7) {
    if (level == 0u || level > kMaxLevel) return kRefLevel;
    switch (msg) {
    case kMsgGetSmuVersion: case kMsgGetDriverIfVersion: case kMsgGetMsgHeaderVersion: case kMsgReturnHardMinStatus:
        return param == 0u ? kAllow : kRefParam;
    case kMsgGetDpmFreqByIndex: {
        if (!clk_ok(param >> 16)) return kRefClk;
        const uint32_t idx = param & 0xFFFFu;
        return (idx <= 15u || idx == kIndexAll) ? kAllow : kRefParam;
    }
    case kMsgGetDcModeMaxDpmFreq:
        if (!clk_ok(param >> 16)) return kRefClk;
        return (param & 0xFFFFu) == 0u ? kAllow : kRefParam;
    case kMsgSetHardMinByFreq: {
        if (level < 2u) return kRefLevel;                    // levels 2..4 only
        const uint32_t clk = param >> 16, mhz = param & 0xFFFFu;
        if (!clk_ok(clk)) return kRefClk;
        if (mhz < kMinMhz) return kRefLow;
        if (mhz > cap_for_level(level)) return kRefCap;
        const uint32_t mx = (clk == kClkDispclk) ? max6 : max7;
        if (mx == 0u) return kRefNoMax;
        return mhz <= mx ? kAllow : kRefAboveMax;
    }
    default: return kRefMsg;                                 // 0xA, 0x5 / 0x6 / 0x8, 0xD..0x1C, everything else
    }
}
// The pre-send mailbox rule: a send with RESP == 0 (or an unreadable 0xFFFFFFFF) is refused.
constexpr uint32_t resp_pre_check(uint32_t resp) { return (resp == 0u || resp == 0xFFFFFFFFu) ? kRefRespZero : kAllow; }

constexpr const char *resp_name(uint32_t r) {
    return r == kRespOk ? "OK" : r == kRespFailed ? "Failed" : r == kRespUnknownCmd ? "UnknownCmd" : r == kRespRejectedPrereq ? "RejectedPrereq" :
           r == kRespRejectedBusy ? "RejectedBusy" : r == 0u ? "none" : "?";
}
constexpr uint32_t param_hardmin(uint32_t clk, uint32_t mhz) { return (clk << 16) | (mhz & 0xFFFFu); }

// GetDcModeMaxDpmFreq's reply is MHz on some paths and kHz on others (design C4): anything of 20000 or more is read as kHz. (0xB is NOT normalised:
// dcn401_init_single_clock reads it as below.)
constexpr uint32_t norm_mhz(uint32_t v) { return v >= 20000u ? v / 1000u : v; }
// GetDpmFreqByIndex, exactly as Linux dcn401_init_single_clock reads it: the reply to index 0xFF is a level count, or with bit 31 set a FINE-GRAINED clock
// (only min and max: 2 levels); the count is the low 8 bits (0 = the message failed). Every per-level reply is masked to its low 16 bits (MHz).
constexpr uint32_t level_count(uint32_t r) { return (r & 0x80000000u) != 0u ? 2u : (r & 0xFFu); }
constexpr uint32_t level_mhz(uint32_t r) { return r & 0xFFFFu; }
constexpr bool level_fine_grained(uint32_t r) { return (r & 0x80000000u) != 0u; }

// ---- DID -> MHz (design C1; dentist_get_divider_from_did / clk_mgr_internal.h) -------------------------------------------------------
// The divider is in quarter units: DID 0x08..0x3F -> DID; 0x40..0x5F -> 64 + 2 (DID - 0x40); 0x60..0x7D -> 128 + 4 (DID - 0x60);
// 0x7E.. -> 248 + 264 (DID - 0x7E). DID is clamped to [0x08, 0x7F] as Linux does.
constexpr uint32_t did_divider(uint32_t did) {
    if (did < 0x08u) did = 0x08u;
    if (did > 0x7Fu) did = 0x7Fu;
    return did < 0x40u ? did : did < 0x60u ? 64u + 2u * (did - 0x40u) : did < 0x7Eu ? 128u + 4u * (did - 0x60u) : 248u + 264u * (did - 0x7Eu);
}
// kHz = 4 * VCO_kHz / divider (integer division, as Linux).
constexpr uint32_t did_khz(uint32_t vcoKhz, uint32_t did) { return (uint32_t)((4ull * vcoKhz) / did_divider(did)); }
// VCO from CLK0_CLK_PLL_REQ: FbMult_int = bits 8:0, FbMult_frac = bits 31:16 (a 16-bit fraction), x the 100 MHz reference (dcn401_get_vco_frequency_from_reg).
constexpr uint32_t vco_khz(uint32_t pllReq) {
    return (uint32_t)((((uint64_t)(pllReq & 0x1FFu) << 16) | (uint64_t)(pllReq >> 16)) * 100000ull >> 16);
}
// The DFS_CNTL register is the DID itself on this ASIC (E1: 0x41); a value with any other bit set, or below 8, is not a DID we can convert.
constexpr bool dfs_did_ok(uint32_t reg) { return reg >= 0x08u && reg <= 0x7Fu; }

// DENTIST_DISPCLK_CNTL (0x124): DISPCLK_WDIVIDER bits 6:0, DISPCLK_CHG_DONE bit 19, DPPCLK_CHG_DONE bit 20, DPPCLK_WDIVIDER bits 30:24.
constexpr uint32_t dentist_disp_w(uint32_t d) { return d & 0x7Fu; }
constexpr uint32_t dentist_dpp_w(uint32_t d) { return (d >> 24) & 0x7Fu; }
constexpr uint32_t kDentistChgDone = 0x00080000u | 0x00100000u;
constexpr bool dentist_chg_done(uint32_t d) { return (d & kDentistChgDone) == kDentistChgDone; }

struct Decoded {
    bool ok;                   // every dword readable, both DIDs convertible, VCO non-zero
    bool dentAgree;            // DENTIST's two WDIVIDER fields equal the two DFS DIDs
    bool chgDone;              // both CHG_DONE bits set
    uint32_t vcoKhz, dispKhz, dppKhz, dispDid, dppDid;
};
constexpr Decoded decode_clocks(uint32_t pllReq, uint32_t dfs0, uint32_t dfs1, uint32_t dentist) {
    const bool readable = pllReq != 0xFFFFFFFFu && dfs0 != 0xFFFFFFFFu && dfs1 != 0xFFFFFFFFu && dentist != 0xFFFFFFFFu;
    const uint32_t vco = vco_khz(pllReq);
    const bool ok = readable && dfs_did_ok(dfs0) && dfs_did_ok(dfs1) && vco != 0u;
    return Decoded{ ok, dentist_disp_w(dentist) == dfs0 && dentist_dpp_w(dentist) == dfs1, dentist_chg_done(dentist), vco,
                    ok ? did_khz(vco, dfs0) : 0u, ok ? did_khz(vco, dfs1) : 0u, dfs0, dfs1 };
}

// ---- steps, plans, verdicts, the latch -------------------------------------------------------------------------------------------------
enum Step : uint32_t { kStepNone = 0, kE1b = 1, kE2 = 2, kE3 = 3, kE4 = 4, kStepLast = 4 };
// The verdict word. kDenied and kShort do not latch; the four failures do.
enum Verdict : uint32_t { kVNone = 0, kPass = 1, kRefused = 2, kTimeout = 3, kRegressed = 4, kGlitch = 5, kDenied = 6, kShort = 7 };
constexpr bool is_stop(uint32_t v) { return v == kRefused || v == kTimeout || v == kRegressed || v == kGlitch; }
constexpr const char *verdict_name(uint32_t v) {
    return v == kPass ? "PASS" : v == kRefused ? "REFUSED" : v == kTimeout ? "TIMEOUT" : v == kRegressed ? "REGRESSED" : v == kGlitch ? "GLITCH" :
           v == kDenied ? "DENIED" : v == kShort ? "SHORT" : "NONE";
}
constexpr uint32_t step_min_level(uint32_t step) { return step; }   // E1b needs level 1, E2 level 2, E3 level 3, E4 level 4 (a higher level allows the lower steps)

struct Raise { uint32_t clk, mhz; };
struct Plan {
    uint32_t nRaise;
    Raise raise[2];            // in this order (E4: DISPCLK first, as Linux's block sequence; the restore runs in reverse)
    uint32_t dwellMs;
    uint32_t dispKhzMin;       // pass thresholds for the DFS readback after the raise (0 = "must not move" for that clock)
    uint32_t dppKhzMin;
    bool dispMoves, dppMoves;
};
constexpr uint32_t kDwellE2Ms = 10u * 1000u, kDwellE3Ms = 10u * 1000u, kDwellE4Ms = 60u * 1000u;
// E2: DISPCLK -> 300 MHz (DID <= 0x3C = 300.0 MHz), DPPCLK must not move. E3: the same for DPPCLK. E4: both to 530 MHz, DISPCLK first then DPPCLK;
// DISPCLK DID <= 0x23 (>= 514.3 MHz -> 514000 kHz), DPPCLK DID <= 0x24 (>= 500 MHz -> 500000 kHz).
constexpr Plan plan_for(uint32_t step) {
    return step == kE2 ? Plan{ 1u, { { kClkDispclk, 300u }, { 0u, 0u } }, kDwellE2Ms, 300000u, 0u, true, false }
         : step == kE3 ? Plan{ 1u, { { kClkDppclk, 300u }, { 0u, 0u } }, kDwellE3Ms, 0u, 300000u, false, true }
         : step == kE4 ? Plan{ 2u, { { kClkDispclk, 530u }, { kClkDppclk, 530u } }, kDwellE4Ms, 514000u, 500000u, true, true }
                       : Plan{ 0u, { { 0u, 0u }, { 0u, 0u } }, 0u, 0u, 0u, false, false };
}

// A reading after a raise (or during the dwell) against the baseline taken before the step: the verdict it earns. kPass, or a stop.
// Order: a clock below its baseline is REGRESSED; a clock that must not move but did is GLITCH; DENTIST disagreeing with the DFS view or
// CHG_DONE missing is GLITCH; a clock that has not reached its pass threshold (but is not below the baseline) is REFUSED (the message was
// answered but the clock did not follow); otherwise PASS.
constexpr uint32_t judge_reading(const Plan &p, const Decoded &base, const Decoded &now) {
    if (!now.ok) return kGlitch;
    if (now.dispKhz < base.dispKhz || now.dppKhz < base.dppKhz) return kRegressed;
    if ((!p.dispMoves && now.dispDid != base.dispDid) || (!p.dppMoves && now.dppDid != base.dppDid)) return kGlitch;
    if (!now.dentAgree || !now.chgDone) return kGlitch;
    if ((p.dispMoves && now.dispKhz < p.dispKhzMin) || (p.dppMoves && now.dppKhz < p.dppKhzMin)) return kRefused;
    return kPass;
}
// During the dwell a moving clock that fell back under its threshold is REGRESSED (the floor was dropped), whatever else judge_reading says.
constexpr uint32_t judge_dwell(const Plan &p, const Decoded &base, const Decoded &now) {
    if (now.ok && ((p.dispMoves && now.dispKhz < p.dispKhzMin) || (p.dppMoves && now.dppKhz < p.dppKhzMin))) return kRegressed;
    return judge_reading(p, base, now);
}
// The restore: every clock must read at least the boot value again (never below the start).
constexpr uint32_t restore_floor_khz(const Decoded &base) { return base.dispKhz < base.dppKhz ? base.dispKhz : base.dppKhz; }
constexpr bool restored_ok(const Decoded &base, const Decoded &now) { return now.ok && now.dispKhz >= base.dispKhz && now.dppKhz >= base.dppKhz; }
// "Back at the start" (both DIDs equal their start values) versus "still above the start" (verified >= start, but a floor is holding it up).
constexpr bool at_start(const Decoded &base, const Decoded &now) { return now.ok && now.dispDid == base.dispDid && now.dppDid == base.dppDid; }
// The clocks a restore must cover: every clock whose hard-min was acknowledged, PLUS every clock that now reads BELOW its start value (a regression can
// hit a clock the step never raised). Bit clk (6 = DISPCLK, 7 = DPPCLK).
constexpr uint32_t restore_mask(uint32_t raisedMask, const Decoded &base, const Decoded &now) {
    return raisedMask | ((now.ok && now.dispKhz < base.dispKhz) ? (1u << kClkDispclk) : 0u) | ((now.ok && now.dppKhz < base.dppKhz) ? (1u << kClkDppclk) : 0u);
}
// A baseline the experiment may start from: readable, DENTIST agreeing with the DFS view and CHG_DONE set, both clocks in [250, 700] MHz.
constexpr bool baseline_ok(const Decoded &d) {
    return d.ok && d.dentAgree && d.chgDone && d.dispKhz >= kMinMhz * 1000u && d.dispKhz <= 700000u && d.dppKhz >= kMinMhz * 1000u && d.dppKhz <= 700000u;
}

// Frame counter arithmetic (24 bits).
constexpr uint32_t frame_delta(uint32_t prev, uint32_t cur) { return (cur - prev) & 0xFFFFFFu; }
constexpr uint32_t kStallMs = 250u;
constexpr bool frames_stalled(uint32_t prev, uint32_t cur, uint32_t dtMs) { return dtMs >= kStallMs && frame_delta(prev, cur) == 0u; }
// The dwell's frame rate against the pre-step reference, +-2 % (integer cross-multiplication; refMs / gotMs > 0).
constexpr bool rate_ok(uint32_t refFrames, uint32_t refMs, uint32_t gotFrames, uint32_t gotMs) {
    if (refMs == 0u || gotMs == 0u || refFrames == 0u) return false;
    const uint64_t a = (uint64_t)gotFrames * refMs, b = (uint64_t)refFrames * gotMs;   // got/gotMs against ref/refMs, cross-multiplied
    return a * 100ull >= b * 98ull && a * 100ull <= b * 102ull;
}

// ---- the sticky stop latch ---------------------------------------------------------------------------------------------------------------
// After any stop verdict every further step is refused (kDenied, latched = 1) for the rest of the boot. There is no reset.
struct Latch { bool stopped; uint32_t firstVerdict; uint32_t firstStep; };
constexpr Latch latch_apply(Latch l, uint32_t step, uint32_t verdict) {
    return (!l.stopped && is_stop(verdict)) ? Latch{ true, verdict, step } : l;
}
// Pre-step gate: 0 = go, else the verdict to return (always kDenied). e1bOk: E1b has completed with every query answered on this boot.
enum Deny : uint32_t { kDenyNone = 0, kDenyLatched = 1, kDenyGate = 2, kDenyLevel = 3, kDenyNoE1b = 4, kDenyBusy = 5, kDenyBadStep = 6, kDenyLevelArg = 7, kDenyBaseline = 8, kDenyFrames = 9, kDenyAcquired = 10 };
constexpr uint32_t pre_gate(bool gateOk, uint32_t level, uint32_t step, bool e1bOk, bool stopped, bool busy) {
    if (step < kE1b || step > kStepLast) return kDenyBadStep;
    if (stopped) return kDenyLatched;
    if (!gateOk) return kDenyGate;
    if (level == 0u || level > kMaxLevel) return kDenyLevelArg;
    if (level < step_min_level(step)) return kDenyLevel;
    if (busy) return kDenyBusy;
    if (step != kE1b && !e1bOk) return kDenyNoE1b;
    return kDenyNone;
}

// E1b judgement over the replies: `levels6` / `max6` for DISPCLK, `max7` for DPPCLK (MHz, normalised). Returns kPass or kShort (all queries were answered;
// the caller has already turned any non-OK reply into a stop). kShort when DISPCLK has one level or fewer, or its max is under 540 MHz.
constexpr uint32_t kE1bPassMaxMhz = 540u, kE1bOdmMaxMhz = 520u;
constexpr uint32_t judge_e1b(uint32_t levels6, uint32_t max6) { return (levels6 > 1u && max6 >= kE1bPassMaxMhz) ? kPass : kShort; }
// May E2..E4 run after this E1b? Every query answered and DISPCLK has more than one level (Linux never sends SetHardMin otherwise).
constexpr bool e1b_enables(uint32_t levels6, uint32_t max6, uint32_t max7) { return levels6 > 1u && max6 != 0u && max7 != 0u; }


// =====================================================================================================================================================
// P4: the clock hold (kext 0.0.607; design an internal design note P4). A row-120 mode trial raises DISPCLK and DPPCLK to 530 MHz BEFORE it writes the first display register and lets
// them go only AFTER its 60 Hz restore verified. The DAL mailbox owner word (gDalOwner in smu_dal.cpp) has three values so the mode trial's hold and a DAL step cannot both run, without the hold
// denying the trial that owns it (F9: mt_in_use() is true while dal_busy(); dal_busy() is owner 1 only).
// Everything below is decisions and the raise / release orchestration over a small interface (HoldIo), so tests/native_s2_dal_test.cpp and native_s2d_test.cpp drive the exact code the kext
// runs; smu_dal.cpp supplies the hardware (the mailbox sender, the DFS readback).
// =====================================================================================================================================================
enum Owner : uint32_t { kOwnerIdle = 0, kOwnerDal = 1, kOwnerHold = 2 };
enum HoldSt : uint32_t { kHsIdle = 0, kHsRaising = 1, kHsHeld = 2, kHsReleasing = 3, kHsReleased = 4, kHsStuck = 5, kHsInvalid = 0xFFu };
enum HoldEv : uint32_t {
    kEvRaise = 1,        // IDLE / RELEASED -> RAISING (the claim)
    kEvHeld = 2,         // RAISING -> HELD (both clocks read at the need)
    kEvNothingSent = 3,  // RAISING -> IDLE (the raise failed before any hard-min was acknowledged: no floor exists)
    kEvUnwound = 4,      // RAISING -> RELEASED (the raise failed after a hard-min was acknowledged; the unwind verified the clocks back)
    kEvRaiseStuck = 5,   // RAISING -> STUCK (unwind failed or the mailbox stopped answering after a hard-min)
    kEvRelease = 6,      // HELD -> RELEASING (the ONE release claim)
    kEvReleased = 7,     // RELEASING -> RELEASED
    kEvStuck = 8         // HELD -> STUCK (restoreBad: never released) / RELEASING -> STUCK (the release did not verify)
};
constexpr uint32_t hold_next(uint32_t st, uint32_t ev) {
    return (ev == kEvRaise && (st == kHsIdle || st == kHsReleased)) ? kHsRaising
         : (st == kHsRaising && ev == kEvHeld) ? kHsHeld
         : (st == kHsRaising && ev == kEvNothingSent) ? kHsIdle
         : (st == kHsRaising && ev == kEvUnwound) ? kHsReleased
         : (st == kHsRaising && ev == kEvRaiseStuck) ? kHsStuck
         : (st == kHsHeld && ev == kEvRelease) ? kHsReleasing
         : (st == kHsReleasing && ev == kEvReleased) ? kHsReleased
         : ((st == kHsHeld || st == kHsReleasing) && ev == kEvStuck) ? kHsStuck
         : kHsInvalid;
}
constexpr const char *hold_state_name(uint32_t s) {
    return s == kHsIdle ? "IDLE" : s == kHsRaising ? "RAISING" : s == kHsHeld ? "HELD" : s == kHsReleasing ? "RELEASING" : s == kHsReleased ? "RELEASED" : s == kHsStuck ? "STUCK" : "?";
}
constexpr uint32_t kHoldMhz = 530u;                  // E4's request (DFS 0x21 = 545.45 MHz on both clocks, e34-row50-1)
constexpr uint32_t kHoldSettleMs = 300u, kHoldSettleStepMs = 5u;   // the DFS readback must reach the need within 300 ms of the last acknowledged raise
constexpr uint32_t kHoldReleaseVerifyMs = 500u, kHoldReleaseStepMs = 5u;
// Why a hold may not start (0 = it may). level: dal_gated_level() (native boot + S1b POSITIVE PASS + the boot-arg 1..4, else 0).
enum HoldPre : uint32_t { kHpOk = 0, kHpLevel = 1, kHpNoE1b = 2, kHpLatched = 3, kHpState = 4, kHpOwner = 5 };
constexpr uint32_t hold_pre_check(uint32_t level, bool e1bOk, bool stopped, uint32_t holdState, uint32_t owner) {
    return level != kMaxLevel ? kHpLevel : !e1bOk ? kHpNoE1b : stopped ? kHpLatched : (holdState != kHsIdle && holdState != kHsReleased) ? kHpState : owner != kOwnerIdle ? kHpOwner : kHpOk;
}
// The DFS readback the hold must reach (and the mode trial keeps sampling): decoded, DENTIST agreeing, CHG_DONE set, both clocks at least the row's need.
constexpr bool hold_clocks_ok(const Decoded &d, uint32_t needDispKhz, uint32_t needDppKhz) {
    return d.ok && d.dentAgree && d.chgDone && d.dispKhz >= needDispKhz && d.dppKhz >= needDppKhz;
}
// The raise's and release's result words (ABI: n48n_mode_ext hold_rc / hold_rel_rc).
enum HoldRc : uint32_t { kHrOk = 0, kHrPre = 1, kHrClaim = 2, kHrBaseline = 3, kHrMsg = 4, kHrPoll = 5, kHrMailbox = 6, kHrClocks = 7, kHrNoIo = 8 };
enum RelRc : uint32_t { kRlOk = 0, kRlNotHeld = 1, kRlBadRestore = 2, kRlMsg = 3, kRlMailbox = 4, kRlNotBack = 5 };
constexpr uint32_t kHfRaised = 1u, kHfReleased = 2u, kHfStuck = 4u, kHfUnwound = 8u, kHfMailboxLost = 16u;
struct HoldRep {
    uint32_t state, rc, relRc, pre, raiseMs, relMs, nMsg, flags;
    uint32_t khzRaised[2], khzReleased[2];       // the DFS readback after the raise / after the release, [0] DISPCLK [1] DPPCLK
};
// One hard-min as the hardware layer performs it (SetHardMinByFreq, then ReturnHardMinStatus polled <= 1 s): the return codes. *acked = the SetHardMin was answered OK (the floor may be up).
enum HmRc : uint32_t { kHmOk = 0, kHmFailed = 1, kHmPollTimeout = 2, kHmMailbox = 3 };
struct HoldCtl {
    uint32_t state;                // HoldSt; every change is a compare-and-swap through hold_next
    uint32_t *owner;               // the DAL owner word (gDalOwner); nullptr = a broken wiring, the raise refuses
    Decoded base;                  // the clocks at the raise (the release must read at least these)
    uint32_t ackedMask;            // bit clk: a hard-min of that clock was acknowledged in this raise
    uint32_t needKhz[2];           // the raise's need, [0] DISPCLK [1] DPPCLK: a verified release / unwind reads BELOW it (the floor really came off)
};
struct HoldIo {
    void *ctx;
    uint32_t (*hard_min)(void *, uint32_t clk, uint32_t mhz, bool *acked);
    bool (*decode)(void *, Decoded *);
    void (*sleep_ms)(void *, uint32_t);
    uint64_t (*now_us)(void *);
    void (*latch_stop)(void *);                                   // the DAL step latch: no further DAL step this boot
    void (*log)(void *, uint32_t code, uint32_t a, uint32_t b, uint32_t c);
};
enum HoldLog : uint32_t { kHlRaiseBegin = 1, kHlHardMin = 2, kHlRaised = 3, kHlUnwind = 4, kHlReleaseBegin = 5, kHlReleased = 6, kHlStuck = 7, kHlRefused = 8 };

inline bool hold_cas_state(HoldCtl &c, uint32_t ev) {
    for (;;) {
        uint32_t cur = __atomic_load_n(&c.state, __ATOMIC_SEQ_CST);
        const uint32_t nx = hold_next(cur, ev);
        if (nx == kHsInvalid) return false;
        if (__atomic_compare_exchange_n(&c.state, &cur, nx, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return true;
    }
}
inline bool hold_cas_owner(HoldCtl &c, uint32_t from, uint32_t to) {
    if (c.owner == nullptr) return false;
    uint32_t f = from;
    return __atomic_compare_exchange_n(c.owner, &f, to, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
inline uint32_t hold_state_now(const HoldCtl &c) { return __atomic_load_n(&c.state, __ATOMIC_SEQ_CST); }
inline uint32_t hold_owner_now(const HoldCtl &c) { return c.owner == nullptr ? 0xFFFFFFFFu : __atomic_load_n(c.owner, __ATOMIC_SEQ_CST); }
inline void hold_lg(const HoldIo &io, uint32_t code, uint32_t a, uint32_t b, uint32_t c) { if (io.log != nullptr) io.log(io.ctx, code, a, b, c); }
inline uint32_t hold_ms(const HoldIo &io, uint64_t t0) { return (uint32_t)((io.now_us(io.ctx) - t0) / 1000ull); }

// After a release (or an unwind) each clock of `mask` must read BELOW the row's need: the floor really came off (at or above the start alone would also accept a clock still held at 545 MHz).
inline bool hold_off_the_need(const HoldCtl &c, const Decoded &z, uint32_t mask) {
    return ((mask & (1u << kClkDispclk)) == 0u || z.dispKhz < c.needKhz[0]) && ((mask & (1u << kClkDppclk)) == 0u || z.dppKhz < c.needKhz[1]);
}
// The unwind of a raise that failed after >= 1 hard-min was acknowledged: DPPCLK then DISPCLK to the release floor, each only if it was acknowledged; then the readback must be at or above the start values.
// A mailbox that stopped answering means NO message can restore the clocks: STUCK. Returns true when the clocks verified back (state RAISING -> RELEASED), false = STUCK.
inline bool hold_unwind(const HoldIo &io, HoldCtl &c, HoldRep &rep, bool mailboxLost) {
    hold_lg(io, kHlUnwind, c.ackedMask, mailboxLost ? 1u : 0u, 0u);
    bool bad = mailboxLost;
    if (mailboxLost) rep.flags |= kHfMailboxLost;
    if (!mailboxLost) {
        const uint32_t order[2] = { kClkDppclk, kClkDispclk };
        for (uint32_t k = 0; k < 2u && !bad; k++) {
            if ((c.ackedMask & (1u << order[k])) == 0u) continue;
            bool acked = false;
            const uint32_t hr = io.hard_min(io.ctx, order[k], kRestoreMhz, &acked);
            rep.nMsg++;
            if (hr == kHmMailbox) { bad = true; rep.flags |= kHfMailboxLost; }
            // kHmFailed / kHmPollTimeout: the mailbox is alive; the readback below decides
        }
        Decoded z{};
        bool ok = false;
        for (uint32_t k = 0; !bad && k <= kHoldReleaseVerifyMs / kHoldReleaseStepMs; k++) {
            if (io.decode(io.ctx, &z) && restored_ok(c.base, z) && hold_off_the_need(c, z, c.ackedMask)) { ok = true; break; }
            io.sleep_ms(io.ctx, kHoldReleaseStepMs);
        }
        if (!bad && !ok) bad = true;
        if (!bad) { rep.khzReleased[0] = z.dispKhz; rep.khzReleased[1] = z.dppKhz; }
    }
    if (bad) {
        (void)hold_cas_state(c, kEvRaiseStuck);
        rep.flags |= kHfStuck;
        if (io.latch_stop != nullptr) io.latch_stop(io.ctx);
        hold_lg(io, kHlStuck, 0u, 0u, 0u);   // (the owner word stays 2: nothing else may send)
        return false;
    }
    (void)hold_cas_state(c, kEvUnwound);
    (void)hold_cas_owner(c, kOwnerHold, kOwnerIdle);
    rep.flags |= kHfUnwound;
    return true;
}

// Raise: precondition -> claim (owner CAS 0 -> 2) -> state RAISING -> baseline -> DISPCLK 530 then DPPCLK 530 (each acknowledged and reported done) -> the DFS readback reaches the need within 300 ms -> HELD.
// Any failure after a hard-min was acknowledged unwinds (hold_unwind); a failure before any message leaves the state IDLE and the owner free. Returns HoldRc; rep always filled. *after = the last DFS readback.
inline uint32_t hold_raise(const HoldIo &io, HoldCtl &c, HoldRep &rep, uint32_t level, bool e1bOk, bool stopped, uint32_t needDispKhz, uint32_t needDppKhz, Decoded *after) {
    rep = HoldRep{};
    if (io.hard_min == nullptr || io.decode == nullptr || io.sleep_ms == nullptr || io.now_us == nullptr || c.owner == nullptr) { rep.rc = kHrNoIo; rep.state = hold_state_now(c); return kHrNoIo; }
    const uint64_t t0 = io.now_us(io.ctx);
    rep.pre = hold_pre_check(level, e1bOk, stopped, hold_state_now(c), hold_owner_now(c));
    if (rep.pre != kHpOk) { rep.rc = kHrPre; rep.state = hold_state_now(c); hold_lg(io, kHlRefused, rep.pre, 0u, 0u); return kHrPre; }
    if (!hold_cas_owner(c, kOwnerIdle, kOwnerHold)) { rep.rc = kHrClaim; rep.pre = kHpOwner; rep.state = hold_state_now(c); hold_lg(io, kHlRefused, kHpOwner, 0u, 0u); return kHrClaim; }
    if (!hold_cas_state(c, kEvRaise)) { (void)hold_cas_owner(c, kOwnerHold, kOwnerIdle); rep.rc = kHrClaim; rep.pre = kHpState; rep.state = hold_state_now(c); hold_lg(io, kHlRefused, kHpState, 0u, 0u); return kHrClaim; }
    hold_lg(io, kHlRaiseBegin, needDispKhz, needDppKhz, 0u);
    c.ackedMask = 0u; c.needKhz[0] = needDispKhz; c.needKhz[1] = needDppKhz;
    uint32_t rc = kHrOk;
    bool mailboxLost = false;
    Decoded a{};
    if (!io.decode(io.ctx, &a) || !baseline_ok(a) || a.dispKhz >= needDispKhz || a.dppKhz >= needDppKhz) rc = kHrBaseline;   // (a clock already at the need means a floor is up that this hold did not raise)
    else c.base = a;
    const uint32_t order[2] = { kClkDispclk, kClkDppclk };   // Linux's block order: DISPCLK first
    for (uint32_t k = 0; rc == kHrOk && k < 2u; k++) {
        bool acked = false;
        const uint32_t hr = io.hard_min(io.ctx, order[k], kHoldMhz, &acked);
        rep.nMsg++;
        if (acked) c.ackedMask |= 1u << order[k];
        hold_lg(io, kHlHardMin, order[k], hr, acked ? 1u : 0u);
        if (hr == kHmOk) continue;
        rc = hr == kHmMailbox ? kHrMailbox : hr == kHmPollTimeout ? kHrPoll : kHrMsg;
        if (hr == kHmMailbox) mailboxLost = true;
    }
    if (rc == kHrOk) {
        bool reached = false;
        for (uint32_t k = 0; k <= kHoldSettleMs / kHoldSettleStepMs; k++) {
            if (io.decode(io.ctx, &a) && hold_clocks_ok(a, needDispKhz, needDppKhz)) { reached = true; break; }
            io.sleep_ms(io.ctx, kHoldSettleStepMs);
        }
        if (!reached) rc = kHrClocks;
    }
    if (after != nullptr) *after = a;
    rep.raiseMs = hold_ms(io, t0);
    rep.khzRaised[0] = a.dispKhz; rep.khzRaised[1] = a.dppKhz;
    if (rc == kHrOk) {
        (void)hold_cas_state(c, kEvHeld);
        rep.flags |= kHfRaised; rep.rc = kHrOk; rep.state = hold_state_now(c);
        hold_lg(io, kHlRaised, a.dispKhz, a.dppKhz, rep.raiseMs);
        return kHrOk;
    }
    rep.rc = rc;
    if (c.ackedMask == 0u && !mailboxLost) {          // nothing was acknowledged: no floor exists, nothing to undo
        (void)hold_cas_state(c, kEvNothingSent);
        (void)hold_cas_owner(c, kOwnerHold, kOwnerIdle);
    } else {
        (void)hold_unwind(io, c, rep, mailboxLost);
    }
    rep.state = hold_state_now(c);
    return rc;
}

// Release (the ONLY release path): exactly one caller wins the HELD -> RELEASING claim. restoreBad (the caller's 60 Hz restore did not verify) NEVER releases: HELD -> STUCK, latched, no message
// (the clocks stay up: harmless at 60 Hz; a cold power cycle drops them). Otherwise DPPCLK then DISPCLK to the 272 floor (kRestoreMhz), and both clocks must read at least their start value within
// 500 ms: RELEASED (owner freed), else STUCK. Returns RelRc; rep always updated.
inline uint32_t hold_release(const HoldIo &io, HoldCtl &c, HoldRep &rep, bool restoreBad) {
    const uint64_t t0 = io.now_us != nullptr ? io.now_us(io.ctx) : 0ull;
    if (hold_state_now(c) != kHsHeld) { rep.relRc = kRlNotHeld; return kRlNotHeld; }
    if (restoreBad) {
        if (!hold_cas_state(c, kEvStuck)) { rep.relRc = kRlNotHeld; return kRlNotHeld; }
        rep.relRc = kRlBadRestore; rep.flags |= kHfStuck; rep.state = hold_state_now(c);
        if (io.latch_stop != nullptr) io.latch_stop(io.ctx);
        hold_lg(io, kHlStuck, 1u, 0u, 0u);
        return kRlBadRestore;
    }
    if (!hold_cas_state(c, kEvRelease)) { rep.relRc = kRlNotHeld; return kRlNotHeld; }      // someone else already claimed the release
    hold_lg(io, kHlReleaseBegin, 0u, 0u, 0u);
    uint32_t rc = kRlOk;
    const uint32_t order[2] = { kClkDppclk, kClkDispclk };
    for (uint32_t k = 0; k < 2u; k++) {
        bool acked = false;
        const uint32_t hr = io.hard_min(io.ctx, order[k], kRestoreMhz, &acked);
        rep.nMsg++;
        hold_lg(io, kHlHardMin, order[k], hr, acked ? 1u : 0u);
        if (hr == kHmMailbox) { rc = kRlMailbox; rep.flags |= kHfMailboxLost; break; }
        if (hr == kHmFailed && rc == kRlOk) rc = kRlMsg;
        // kHmPollTimeout: the mailbox answered; the readback decides
    }
    Decoded z{};
    bool ok = false;
    if (rc != kRlMailbox) {
        for (uint32_t k = 0; k <= kHoldReleaseVerifyMs / kHoldReleaseStepMs; k++) {
            if (io.decode(io.ctx, &z) && restored_ok(c.base, z) && hold_off_the_need(c, z, (1u << kClkDispclk) | (1u << kClkDppclk))) { ok = true; break; }
            io.sleep_ms(io.ctx, kHoldReleaseStepMs);
        }
        if (!ok && rc == kRlOk) rc = kRlNotBack;
    }
    rep.khzReleased[0] = z.dispKhz; rep.khzReleased[1] = z.dppKhz;
    rep.relMs = hold_ms(io, t0);
    if (ok) {
        // A refused / failed message whose readback nevertheless shows the clocks back at or above the start is still a verified release (the floor is what matters).
        (void)hold_cas_state(c, kEvReleased);
        (void)hold_cas_owner(c, kOwnerHold, kOwnerIdle);
        rep.flags |= kHfReleased; rep.relRc = kRlOk; rep.state = hold_state_now(c);
        hold_lg(io, kHlReleased, z.dispKhz, z.dppKhz, rep.relMs);
        return kRlOk;
    }
    (void)hold_cas_state(c, kEvStuck);
    rep.flags |= kHfStuck; rep.relRc = rc; rep.state = hold_state_now(c);
    if (io.latch_stop != nullptr) io.latch_stop(io.ctx);
    hold_lg(io, kHlStuck, 2u, rc, 0u);
    return rc;
}

} // namespace n48dal
