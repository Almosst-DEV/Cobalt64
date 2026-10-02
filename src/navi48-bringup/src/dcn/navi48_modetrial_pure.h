//
//  navi48_modetrial_pure.h - the timed mode trial of native step S2d (kext 0.0.605; 0.0.606 adds the underflow read P1, the DP1 resync P3 and the OTG update lock P2 of
//  notes/design/NATIVE-S2-120HZ.md): every decision AND the trial / restore / watchdog sequences, written against a small
//  hardware interface (struct Hw) so tests/native_s2d_test.cpp compiles this on the host Mac and drives the exact code the kext runs (dcn/navi48_dcn.cpp supplies the Hw callbacks:
//  the allowlisted display write, the DCN read, the sleep, the lock, the kernel thread). No kernel header is included here.
//
//  WHAT A TRIAL DOES (design: notes/design/NATIVE-S2.md S2d rows + the Review; ABI text: the "ABI 1.3 addendum" in Navi48NativeABI.h):
//    deny checks (native + S1b POSITIVE PASS, latch, plane not acquired, golden copy, baseline = the 60 Hz census raster, DP1 the one enabled stream, no drift, clocks sufficient
//    from the DFS readback, step down first) -> 60 Hz baseline rate -> watchdog thread -> write the row's registers in Linux's order (each recorded BEFORE it is written) ->
//    settle -> 3 s rate / DIO FIFO / stream check -> dwell (stall / stream / DIO watch) -> restore EVERY written register to the golden copy in reverse order -> verify the
//    registers and the 60 Hz rate -> verdict, sticky latch on any failure.
//  THE ONE RESTORE (restore_run) is shared by the trial thread, the watchdog thread, `dcnmode 0` (restore_from_golden) and the kext stop. It is claimed under the lock, so exactly
//  one party performs it; the other waits for restoreDone.
//
//  0.0.607 adds row 120 end to end (design P4 clock hold + P5): the clock hold is raised BEFORE the first display write and released only AFTER the 60 Hz restore verified (never by the watchdog);
//  the apply is blank, DRR mode 2, OTG master lock, OTG + HUBP writes, unlock + latch confirm, then DTO, then MSA, then unblank; the restore is its reverse (DTO first, before the lock).
//
//  0.0.609 adds the HELD mode (row 120 only; design in the "ABI 1.7 addendum" of Navi48NativeABI.h): the SAME run_trial runs on a kernel thread with a HoldCfg; after the judged-PASS apply, settle and rate window it does
//  not dwell for a fixed time but publishes the entry snapshot (State::held = 1, the caller of the selector returns) and keeps dwelling - the sampler, the stall detector, the heartbeat and the watchdog are the dwell's -
//  until one of: ModeRelease, the owning session's close, the max hold time, no taker for a HANDOFF hold, dcnmode 0 / kext stop (abort), a sampled failure, the watchdog. Every end goes through the ONE restore_run,
//  which first (hold_begin_end) sets `ending` (scanAcquire refuses from then on) and puts the scanout plane back to the console and waits for it to verify (Hw.scan_release), THEN restores the timing, verifies the
//  60 Hz rate, and only then releases the clocks - the order scanout, timing, clocks. Lock rank (0.0.609): the caller of the hold entry points takes no lock; Hw.scan_release takes the scanout lock and is called
//  with NO lock held (never under Hw.lock); scanAcquire (under the scanout lock) reads only the atomic words of State (held / ending / releaseReq / ownerGone / ownerSeq / handoff), never Hw.lock.
//
//  Writes NOTHING itself: every register access goes through Hw.rd / Hw.wr, and Hw.wr in the kext is dcn_wreg, i.e. the dcn41 write allowlist.
//
#pragma once
#include <stdint.h>
#include "../Navi48NativeABI.h"
#include "navi48_modetrial_tables.h"
#include "navi48_scanout_pure.h"
#include "../amd/smu_dal_pure.h"

namespace n48mt {

using tab::kCap; using tab::kCapN;

// ---- the read-only witnesses (absolute BAR5 dwords; census s2d0-census-1 read every one; none is in kCap, so none is ever written) ----------------------------------------
constexpr uint32_t kRegPixelRateCntl = 0x140u;     // OTG0_PIXEL_RATE_CNTL: seg1 0xc0 + 0x80 (scanQuery reads the same dword). DP_DTO0_ENABLE bit 4, DIO_FIFO_ERROR 15:14, DIO_ERROR_COUNT 27:16
constexpr uint32_t kRegDpStream[4] = { 0x55E2u, 0x5706u, 0x582Au, 0x594Eu };   // DPn_DP_VID_STREAM_CNTL: seg2 0x34c0 + 0x2122 / 0x2246 / 0x236a / 0x248e. bit 0 enable, bit 16 status
constexpr uint32_t kRegDp1Steer = 0x5707u;         // DP1_DP_STEER_FIFO: seg2 0x34c0 + 0x2247; bit 4 overflow flag
constexpr uint32_t kRegDigFifoCtrl0 = 0x567Fu;     // DIG1_DIG_FIFO_CTRL0 (read only): bits 29:28 are the DIG FIFO error flags
constexpr uint32_t kLitDp = 1u;                    // the census: DP1 is the live stream (DP0/2/3 read M 0, MSA 0)
constexpr uint32_t kFifoErrMask = 0x0000C000u, kErrCountMask = 0x0FFF0000u, kDtoEnableBit = 0x10u;
constexpr uint32_t fifo_err(uint32_t prc) { return (prc & kFifoErrMask) >> 14; }
constexpr uint32_t err_count(uint32_t prc) { return (prc & kErrCountMask) >> 16; }
constexpr bool steer_bad(uint32_t v) { return v == 0xFFFFFFFFu || (v & 0x10u) != 0u; }            // DP1_DP_STEER_FIFO bit 4 = overflow flag; unreadable counts as bad
constexpr bool dig_bad(uint32_t v) { return v == 0xFFFFFFFFu || ((v >> 28) & 3u) != 0u; }
// The FIFO health of the display back end: DIO_FIFO_ERROR, the steer FIFO overflow flag, the DIG FIFO error bits (the error COUNT is judged as growth, not here).
constexpr bool health_bad(uint32_t prc, uint32_t steer, uint32_t dig) { return fifo_err(prc) != 0u || steer_bad(steer) || dig_bad(dig); }
constexpr bool stream_active(uint32_t v) { return v != 0xFFFFFFFFu && (v & 1u) != 0u && (v & 0x10000u) != 0u; }

// ---- 0.0.606: the registers of the underflow read (P1), the DP1 resync (P3) and the OTG update lock (P2). Absolute BAR5 dwords = seg + dcn_4_1_0_offset.h, every one read by the S2-120HZ census
// (c120-census-1) and inside the dcn41 write allowlist; NONE is in kCap (static_assert further down), so the golden restore of kCap never touches them; their golden is kept in State::ext. ----
constexpr uint32_t kRegHubpCntl = 0x3AB4u;         // HUBP0_DCHUBP_CNTL: UNDERFLOW_STATUS b30:28, CLEAR b31 (write-1), TIMEOUT_STATUS b23:20, TIMEOUT_CLEAR b26 (write-1)
constexpr uint32_t kRegOptcInGlobal = 0x4F8Au;     // ODM0_OPTC_INPUT_GLOBAL_CONTROL: INPUT_SOFT_RESET b0, UNDERFLOW_OCCURRED_STATUS b10 (sticky), INT_STATUS b11, CLEAR b12 (write-1), OCCURRED_CURRENT b13, DOUBLE_BUFFER_PENDING b31
constexpr uint32_t kRegDbCtrl = 0x501Cu;           // OTG0_OTG_DOUBLE_BUFFER_CONTROL: UPDATE_PENDING b0, DRR_TIMING_DBUF_UPDATE_PENDING b4, TIMING_DB_UPDATE_PENDING b5, UPDATE_INSTANTLY b8, VSTARTUP_DB_UPDATE_PENDING b9, DRR_TIMING_DBUF_UPDATE_MODE b25:24
constexpr uint32_t kRegMasterLock = 0x5049u;       // OTG0_OTG_MASTER_UPDATE_LOCK: MASTER_UPDATE_LOCK b0 (request), UPDATE_LOCK_STATUS b8
constexpr uint32_t kRegGlobalCtrl2 = 0x5050u;      // OTG0_OTG_GLOBAL_CONTROL2: MASTER_UPDATE_LOCK_SEL b27:25
constexpr uint32_t kRegPipeUpd = 0x505Eu;          // OTG0_OTG_PIPE_UPDATE_STATUS: DC_REG_UPDATE_PENDING b4
constexpr uint32_t kRegStatusPos = 0x500Au;        // OTG0_OTG_STATUS_POSITION: VERT_COUNT b14:0
constexpr uint32_t kRegDp1Vid = 0x5706u;           // DP1_DP_VID_STREAM_CNTL (the same dword as kRegDpStream[kLitDp]): ENABLE b0, DIS_DEFER b9:8, STATUS b16
static_assert(kRegDpStream[kLitDp] == kRegDp1Vid && kRegDp1Steer == 0x5707u && kRegDigFifoCtrl0 == 0x567Fu, "the resync registers are DP1 and DIG1 (never DP0 / DP2 / DP3)");

// P1: an underflow status the trial can act on. An unreadable dword counts as an underflow (fail closed).
namespace uf {
constexpr uint32_t kHubpStatus = 0x70000000u, kHubpClear = 0x80000000u, kHubpTimeout = 0x00F00000u, kHubpTimeoutClear = 0x04000000u;
constexpr uint32_t kOptcSoftReset = 0x1u, kOptcOccurred = 0x400u, kOptcInt = 0x800u, kOptcClear = 0x1000u, kOptcCurrent = 0x2000u, kOptcDbPending = 0x80000000u;
constexpr uint32_t kOptcSeen = kOptcOccurred | kOptcInt | kOptcCurrent;
constexpr uint32_t hubp_status(uint32_t v) { return (v & kHubpStatus) >> 28; }
constexpr uint32_t hubp_timeout(uint32_t v) { return (v & kHubpTimeout) >> 20; }
constexpr bool unreadable(uint32_t v) { return v == 0xFFFFFFFFu; }
constexpr bool hubp_bad(uint32_t v) { return unreadable(v) || hubp_status(v) != 0u || hubp_timeout(v) != 0u; }
constexpr bool optc_bad(uint32_t v) { return unreadable(v) || (v & kOptcSeen) != 0u; }
constexpr bool clear_refused(uint32_t optc) { return !unreadable(optc) && (optc & kOptcSoftReset) != 0u; }   // writing a set INPUT_SOFT_RESET back would re-assert the reset
// Linux REG_UPDATE(field, 1) on a write-1 strobe: the control bits are written back as read, every status bit is written 0 (they are read-only), the strobe is set.
constexpr uint32_t hubp_clear_value(uint32_t cur) { return (cur & ~(kHubpStatus | kHubpTimeout | kHubpClear | kHubpTimeoutClear)) | kHubpClear | kHubpTimeoutClear; }
constexpr uint32_t optc_clear_value(uint32_t cur) { return (cur & ~(kOptcSeen | kOptcDbPending | kOptcClear)) | kOptcClear; }
// The same registers with the strobe bits written back 0 (a strobe that reads back set is a level, and a level that stays set would blind the detector).
constexpr uint32_t hubp_unstrobe_value(uint32_t cur) { return cur & ~(kHubpStatus | kHubpTimeout | kHubpClear | kHubpTimeoutClear); }
constexpr uint32_t optc_unstrobe_value(uint32_t cur) { return cur & ~(kOptcSeen | kOptcDbPending | kOptcClear); }
constexpr uint32_t hubp_bits(uint32_t v) { return v & (kHubpStatus | kHubpTimeout); }
constexpr uint32_t optc_bits(uint32_t v) { return v & kOptcSeen; }
}

// P2 / P3 register fields. A writer changes exactly the named field; the `never` bits (read-only status, write-1 acknowledges) are always written 0.
constexpr uint32_t kVidEnable = 0x1u, kVidDisDefer = 0x300u, kVidDisDefer2 = 0x200u, kVidStatus = 0x10000u;                          // DP1_DP_VID_STREAM_CNTL
constexpr uint32_t kSteerEnable = 0x1u, kSteerReset = 0x2u, kSteerNever = 0x74u;                                                   // DP1_DP_STEER_FIFO: ACK b6, INT b5, OVERFLOW b4, RESET_DONE b2 are never written 1
constexpr uint32_t kDigEnable = 0x1u, kDigReset = 0x2u, kDigLevelMask = 0x7Cu, kDigLevel7 = 0x1Cu, kDigPixPerCycle = 0x300u, kDigResetDone = 0x100000u, kDigNever = 0x30100000u;   // DIG1_DIG_FIFO_CTRL0: ERROR b29:28 and RESET_DONE b20 never written 1, PIXEL_PER_CYCLE never changed
constexpr uint32_t kDbPendingMask = 0x231u, kDbInstantly = 0x100u, kDbDrrMask = 0x03000000u, kDbDrrShift = 24u, kDrrModeStartOfFrame = 2u;
constexpr uint32_t kLockReq = 0x1u, kLockStatus = 0x100u, kSelMask = 0x0E000000u, kSelShift = 25u, kPipeRegPending = 0x10u, kVertMask = 0x7FFFu;
constexpr uint32_t kOtgInst = 0u;                  // OTG0, the only lit OTG (deny-checked)
// The bounds of every wait (each is finite: no sequence can hang on a register).
constexpr uint32_t kStatusPolls = 10020u, kStatusPollUs = 10u;        // Linux's blank wait: 10 us x 10020
constexpr uint32_t kBlankSleepMs = 60u, kSteerPulseUs = 10u, kStreamEnableUs = 200u, kDigEnableUs = 100u;
constexpr uint32_t kDigDonePolls = 5000u, kDigDonePollUs = 10u;       // <= 50 ms
constexpr uint32_t kActivePolls = 1000u, kActivePollUs = 100u;        // <= 100 ms for ENABLE + STATUS after the unblank
constexpr uint32_t kLockPolls = 100u, kLockPollUs = 100u;             // <= 10 ms for UPDATE_LOCK_STATUS
constexpr uint32_t kPendingPolls = 1000u, kPendingPollUs = 100u;      // <= 100 ms (dcn30_wait_for_all_pending_updates)
constexpr uint32_t kWitnessPollUs = 20u;
constexpr uint32_t kRow2VTotalReg = 1500u;                            // V_TOTAL register (lines - 1) of the isolation row: 1501 lines

constexpr bool streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
constexpr uint32_t cap_find(const char *name) { for (uint32_t i = 0; i < kCapN; i++) if (streq(kCap[i].name, name)) return i; return 0xFFFFFFFFu; }
constexpr uint32_t kIdxDtoPhase = 0u, kIdxDtoModulo = 1u, kIdxRateDiv = 2u;
static_assert(cap_find("DP_DTO0_PHASE") == kIdxDtoPhase && cap_find("DP_DTO0_MODULO") == kIdxDtoModulo && cap_find("OTG_PIXEL_RATE_DIV") == kIdxRateDiv, "the DTO trio leads kCap");
constexpr uint32_t kIdxHTotal = cap_find("OTG0_OTG_H_TOTAL"), kIdxVTotal = cap_find("OTG0_OTG_V_TOTAL");
static_assert(kIdxHTotal != 0xFFFFFFFFu && kIdxVTotal != 0xFFFFFFFFu, "kCap names the OTG totals");
// 0.0.607: row 120's write groups. DTO = the trio that leads kCap; MSA = the four DP1 MSA registers; OTG = everything else (the OTG timing / global sync / VTG set and the HUBP DLG-TTU set: what the OTG master
// update lock covers). The apply writes OTG under the lock, then the DTO, then the MSA (F5: DTO and MSA apply immediately, so the pixel clock moves after the unlock); the restore runs DTO, OTG under the lock, MSA.
constexpr uint32_t kIdxMsa0 = cap_find("DP1_DP_MSA_TIMING_PARAM1"), kIdxMsa3 = cap_find("DP1_DP_MSA_TIMING_PARAM4");
static_assert(kIdxMsa0 != 0xFFFFFFFFu && kIdxMsa3 == kIdxMsa0 + 3u, "the four MSA registers are contiguous in kCap");
enum CapGroup : uint32_t { kGrpOtg = 0, kGrpDto = 1, kGrpMsa = 2 };
constexpr uint32_t cap_group(uint32_t idx) { return idx <= kIdxRateDiv ? kGrpDto : (idx >= kIdxMsa0 && idx <= kIdxMsa3) ? kGrpMsa : kGrpOtg; }
// The registers whose 60 Hz golden equals the live 60 Hz hardware (README of the goldens: timing family, MSA, DTO EXACT by value). The baseline check compares these and only these.
constexpr const char *kValidatedNames[] = { "DP_DTO0_PHASE", "DP_DTO0_MODULO", "OTG_PIXEL_RATE_DIV", "OTG0_OTG_H_TOTAL", "OTG0_OTG_H_SYNC_A", "OTG0_OTG_H_BLANK_START_END",
    "OTG0_OTG_V_TOTAL", "OTG0_OTG_V_SYNC_A", "OTG0_OTG_V_BLANK_START_END", "DP1_DP_MSA_TIMING_PARAM1", "DP1_DP_MSA_TIMING_PARAM2", "DP1_DP_MSA_TIMING_PARAM3", "DP1_DP_MSA_TIMING_PARAM4" };
constexpr uint32_t kValidatedN = sizeof(kValidatedNames) / sizeof(kValidatedNames[0]);
constexpr bool validated_all_found() { for (uint32_t i = 0; i < kValidatedN; i++) if (cap_find(kValidatedNames[i]) == 0xFFFFFFFFu) return false; return true; }
static_assert(validated_all_found(), "every validated register is in kCap");

// ---- limits and timing ------------------------------------------------------------------------------------------------------------------
struct Timing {
    uint32_t preMeasureMs;   // the 60 Hz baseline window before the trial
    uint32_t settleMs;       // after the writes, before the rate window
    uint32_t measureMs;      // the rate windows (>= 2000 by the brief)
    uint32_t pollMs;         // every wait is sliced at this; a heartbeat each slice
    uint32_t slackMs;        // the watchdog's margin over settle + measure + dwell
    uint32_t staleMs;        // the watchdog also fires when the trial thread's heartbeat is this old
    uint32_t stallMs;        // the OTG frame counter not moving for this long
    uint32_t sampleMs;       // the dwell's stream / DIO / stall sampling period
    uint32_t tolPermille;    // rate tolerance, 10 = 1 %
    uint32_t baseTolPermille; // the pre-trial 60 Hz sanity tolerance
    uint32_t waitRestoreMs;  // the trial thread waits at most this for a restore the watchdog claimed
    bool row120;             // row 120 enabled. 0.0.607: false in kProd; the kext turns it on only with the boot-arg navi48-row120=1 (navi48_dcn.cpp mt_timing)
    // 0.0.606 (default member initialisers, so the kProd line above is unchanged)
    uint32_t wrapExtraMs = 4000u;   // added to the watchdog window when the DP1 resync wraps the trial (two resyncs, each up to ~1 s with its retry, plus margin)
    uint32_t vtMeasureMs = 8000u;   // row 2's rate windows (trial and after-restore): at 3 s one frame plus 1 ms of truncation already uses ~0.6 %
    uint32_t vtTolPermille = 6u;    // row 2: 59.152 Hz against the 59.950 Hz baseline is 1.35 % apart and a 3 s window quantises by 0.56 %, so 1 % cannot tell them apart
};
constexpr Timing kProd = { 2000u, 500u, 3000u, 50u, 2000u, 3000u, 250u, 250u, 10u, 20u, 15000u, false };
constexpr uint32_t kMaxDwellMs = N48N_MODE_MAX_DWELL_MS;
// 0.0.609: the HELD mode. kHoldEndSlackMs is added to the watchdog window: the end of a hold also puts the scanout plane back first (polled up to ~200 ms, twice) before the timing restore.
constexpr uint32_t kHoldDefaultMs = N48N_MODE_DEFAULT_HOLD_MS, kHoldMaxMs = N48N_MODE_MAX_HOLD_MS, kHoldMinMs = 1000u, kHandoffMs = 30000u, kHoldEndSlackMs = 3000u;
enum HoldEnd : uint32_t { kHeNone = 0, kHeRelease = N48N_HOLD_END_RELEASED, kHeOwnerClosed = N48N_HOLD_END_OWNER_CLOSED, kHeMaxTime = N48N_HOLD_END_MAX_TIME, kHeNoTaker = N48N_HOLD_END_NO_TAKER,
                          kHeAbort = N48N_HOLD_END_ABORT, kHeFailure = N48N_HOLD_END_FAILURE, kHeWatchdog = N48N_HOLD_END_WATCHDOG };
struct HoldCfg { uint32_t maxMs; uint32_t flags; };   // maxMs: the hold time bound (kHoldMinMs..kHoldMaxMs); flags: N48N_HOLD_F_*
constexpr bool hold_cfg_ok(const HoldCfg &c) { return c.maxMs >= kHoldMinMs && c.maxMs <= kHoldMaxMs && (c.flags & ~N48N_HOLD_F_MASK) == 0u; }

// ---- rows ---------------------------------------------------------------------------------------------------------------------------------
struct RowInfo {
    uint32_t row;                                // 0 = not a row
    const uint16_t *idx; uint32_t n;             // the registers this row may write (indexes into kCap, ascending)
    const uint32_t *gold;                        // the row's values, indexed like kCap
    uint32_t pixHz, modulo, phase, dtoInt, hTotal, vTotal, needDispKhz, needDppKhz;
};
// Row 2 (the V_TOTAL isolation row): the 60 Hz golden with OTG0_OTG_V_TOTAL = 1500 (1501 lines). Only that one register is in its write set.
struct GoldArr { uint32_t v[kCapN]; };
constexpr GoldArr make_gold_vt() { GoldArr g{}; for (uint32_t i = 0; i < kCapN; i++) g.v[i] = tab::kGold60[i]; g.v[kIdxVTotal] = (tab::kGold60[kIdxVTotal] & ~kVertMask) | kRow2VTotalReg; return g; }
constexpr GoldArr kGoldVt = make_gold_vt();
constexpr uint16_t kRow2Idx[1] = { (uint16_t)kIdxVTotal };
constexpr uint32_t kRow2VTotal = kRow2VTotalReg + 1u;
constexpr bool row_valid(uint32_t row) { return row == 1u || row == 2u || row == 50u || row == 120u; }
// The DP1 resync wraps rows 1, 2 and 120 always, and row 50 when asked (TF_RESYNC). Rows 2 and 120 take the OTG update lock; row 120 alone splits its writes into the OTG / DTO / MSA groups (0.0.607, design P5).
constexpr bool row_wrap(uint32_t row, uint32_t tflags) { return row == 1u || row == 2u || row == 120u || (row == 50u && (tflags & N48N_MODE_TF_RESYNC) != 0u); }
constexpr bool row_lock(uint32_t row) { return row == 2u || row == 120u; }
constexpr bool row_split(uint32_t row) { return row == 120u; }
constexpr RowInfo row_info(uint32_t row) {
    return row == 1u   ? RowInfo{ 1u,   nullptr,        0u,              tab::kGold60,   tab::kRow60PixHz,  tab::kRow60Modulo,  tab::kRow60Phase,  tab::kRow60DtoInt,  tab::kRow60HTotal,  tab::kRow60VTotal,  tab::kRow60NeedDispKhz,  tab::kRow60NeedDppKhz }
         : row == 2u   ? RowInfo{ 2u,   kRow2Idx,       1u,              kGoldVt.v,      tab::kRow60PixHz,  tab::kRow60Modulo,  tab::kRow60Phase,  tab::kRow60DtoInt,  tab::kRow60HTotal,  kRow2VTotal,        tab::kRow60NeedDispKhz,  tab::kRow60NeedDppKhz }
         : row == 50u  ? RowInfo{ 50u,  tab::kRow50Idx,  tab::kRow50N,  tab::kGold50,  tab::kRow50PixHz,  tab::kRow50Modulo,  tab::kRow50Phase,  tab::kRow50DtoInt,  tab::kRow50HTotal,  tab::kRow50VTotal,  tab::kRow50NeedDispKhz,  tab::kRow50NeedDppKhz }
         : row == 120u ? RowInfo{ 120u, tab::kRow120Idx, tab::kRow120N, tab::kGold120, tab::kRow120PixHz, tab::kRow120Modulo, tab::kRow120Phase, tab::kRow120DtoInt, tab::kRow120HTotal, tab::kRow120VTotal, tab::kRow120NeedDispKhz, tab::kRow120NeedDppKhz }
         : RowInfo{ 0u, nullptr, 0u, nullptr, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
}
// The 60 Hz baseline row: what the live hardware must look like and what the restore must bring back (rate only; the registers come from the copy).
constexpr RowInfo row60() { return RowInfo{ 60u, nullptr, 0u, tab::kGold60, tab::kRow60PixHz, tab::kRow60Modulo, tab::kRow60Phase, tab::kRow60DtoInt, tab::kRow60HTotal, tab::kRow60VTotal, tab::kRow60NeedDispKhz, tab::kRow60NeedDppKhz }; }

// dcn401's DTO convention (dccg401_set_dp_dto): pixclk = DPDTO0_INT * modulo + phase (Hz).
constexpr uint64_t dto_pixclk_hz(uint32_t dtoInt, uint32_t modulo, uint32_t phase) { return (uint64_t)dtoInt * modulo + phase; }
// The refresh a row must show, millihertz: the DTO pixel clock over the raster.
constexpr uint64_t expect_mhz(const RowInfo &r) { return n48scan::refresh_mhz(dto_pixclk_hz(r.dtoInt, r.modulo, r.phase), r.hTotal, r.vTotal); }
// A measured refresh, millihertz: frames over elapsed milliseconds.
constexpr uint64_t measured_mhz(uint32_t frames, uint32_t elapsedMs) { return elapsedMs == 0u ? 0ull : ((uint64_t)frames * 1000000ull) / elapsedMs; }
// got within tol permille of want (integer, both directions).
constexpr bool rate_within(uint64_t got, uint64_t want, uint32_t tolPermille) {
    if (want == 0ull || got == 0ull) return false;
    const uint64_t d = got > want ? got - want : want - got;
    return d * 1000ull <= want * (uint64_t)tolPermille;
}
// Clock sufficiency from the LIVE DFS readback (the C1 DID table, smu_dal_pure.h): both clocks at least what the row's DML computed, DENTIST agreeing, CHG_DONE set.
constexpr bool clocks_ok(const RowInfo &r, const n48dal::Decoded &d) {
    return d.ok && d.dentAgree && d.chgDone && d.dispKhz >= r.needDispKhz && d.dppKhz >= r.needDppKhz;
}
// The Linux read-modify-write on the golden's written_mask.
constexpr uint32_t rmw(uint32_t cur, uint32_t mask, uint32_t val) { return (cur & ~mask) | (val & mask); }
constexpr bool row_has(const RowInfo &r, uint32_t capIdx) { for (uint32_t i = 0; i < r.n; i++) if (r.idx[i] == capIdx) return true; return false; }

// ---- the verdict, from what was observed --------------------------------------------------------------------------------------------------
struct Obs {
    bool restoreBad;      // a register differs from the golden after the restore, or the 60 Hz rate did not return
    bool wdFired, aborted, writeFailed, stalled, streamLost, fifoBad, rateBad;
    bool underflow = false, lockFail = false, resyncFail = false;   // 0.0.606
    bool clockLost = false, holdStuck = false;                       // 0.0.607: the DFS readback fell under the need while held; the release did not verify
};
// Precedence: a restore that did not verify is the worst news; then who ended the trial; then the trial's own failures, worst first (a refused write, the lock, the resync, a stalled
// counter or lost stream say the sequence itself failed; an underflow is the display-side symptom and outranks the DIO FIFO and the rate, which can be its consequences). PASS only when none.
constexpr uint32_t judge(const Obs &o) {
    return o.restoreBad ? N48N_MODE_V_RESTORE : o.wdFired ? N48N_MODE_V_WATCHDOG : o.aborted ? N48N_MODE_V_ABORT : o.writeFailed ? N48N_MODE_V_WRITE :
           o.lockFail ? N48N_MODE_V_LOCK : o.resyncFail ? N48N_MODE_V_RESYNC : o.clockLost ? N48N_MODE_V_CLOCK_LOST :
           o.stalled ? N48N_MODE_V_STALL : o.streamLost ? N48N_MODE_V_STREAM : o.underflow ? N48N_MODE_V_UNDERFLOW : o.fifoBad ? N48N_MODE_V_FIFO : o.rateBad ? N48N_MODE_V_RATE : o.holdStuck ? N48N_MODE_V_HOLD : N48N_MODE_V_PASS;
}
// 0.0.607: row 120's prerequisite: a row-2 trial that PASSED and whose report holds BOTH the lock-hold witness and the confirmed V_TOTAL latch.
constexpr bool row2_proof(uint32_t verdict, uint32_t row, bool lockHeldProven, bool latchConfirmed) { return verdict == N48N_MODE_V_PASS && row == 2u && lockHeldProven && latchConfirmed; }
constexpr bool is_failure(uint32_t v) { return v >= N48N_MODE_V_RATE && v <= N48N_MODE_V_HOLD; }   // latches; PASS and DENIED do not

// ---- the pre-write gate --------------------------------------------------------------------------------------------------------------------
struct Pre {
    uint32_t row, dwellMs;
    bool row120Off, latched, gateOk, busy, armed, planeAcquired, inUse, goldenValid, otgOk, baselineOk, streamOk, driftFree, clocksOk, step50Passed, nothing, framesOk;
    bool ufOk = true, lockOk = true, needResync = false, resyncPassed = true;   // 0.0.606: underflow baseline clean; OTG lock usable; the row needs a passed row-1 resync on this boot, and has one
    bool clockHold = false, holdOk = true, needRow2 = false, row2Proven = true;   // 0.0.607: row 120 raises its own clocks (the DFS check waits for the hold) and needs the clock hold available / a row-2 PASS with the lock-hold proof
};
// The first reason a trial may not start (0 = go). Order is the ABI's: the cheapest and most fundamental first. Row 120 at a clock the DML refuses answers CLOCKS even with no
// row-50 trial behind it.
constexpr uint32_t deny_check(const Pre &p) {
    if (!row_valid(p.row)) return N48N_MODE_D_BAD_ROW;
    if (p.row == 120u && p.row120Off) return N48N_MODE_D_ROW120_OFF;
    if (p.dwellMs > kMaxDwellMs) return N48N_MODE_D_BAD_DWELL;
    if (p.latched) return N48N_MODE_D_LATCHED;
    if (!p.gateOk) return N48N_MODE_D_GATE;
    if (p.busy) return N48N_MODE_D_BUSY;
    if (!p.armed) return N48N_MODE_D_NOT_ARMED;
    if (p.planeAcquired) return N48N_MODE_D_ACQUIRED;
    if (p.inUse) return N48N_MODE_D_IN_USE;
    if (!p.goldenValid) return N48N_MODE_D_GOLDEN;
    if (!p.otgOk) return N48N_MODE_D_OTG;
    if (!p.baselineOk) return N48N_MODE_D_BASELINE;
    if (!p.ufOk) return N48N_MODE_D_UNDERFLOW;
    if (!p.streamOk) return N48N_MODE_D_STREAM;
    if (!p.driftFree) return N48N_MODE_D_DRIFT;
    if (!p.lockOk) return N48N_MODE_D_LOCK;
    if (p.clockHold) { if (!p.holdOk) return N48N_MODE_D_HOLD; }        // row 120: the hold raises the clocks; the DFS readback is judged after the raise (hold_raise)
    else if (!p.clocksOk) return N48N_MODE_D_CLOCKS;
    if (p.row == 120u && !p.step50Passed) return N48N_MODE_D_STEP_DOWN;
    if (p.needResync && !p.resyncPassed) return N48N_MODE_D_STEP_DOWN;
    if (p.needRow2 && !p.row2Proven) return N48N_MODE_D_ROW2;
    if (p.nothing) return N48N_MODE_D_NOTHING;
    if (!p.framesOk) return N48N_MODE_D_FRAMES;
    return 0u;
}

// ---- the watchdog's decision -----------------------------------------------------------------------------------------------------------------
// Due when the deadline has passed, or the trial thread's heartbeat is older than staleUs. (A trial that is finished never fires.)
constexpr bool wd_due(uint64_t nowUs, uint64_t deadlineUs, uint64_t beatUs, uint64_t staleUs) { return nowUs >= deadlineUs || (nowUs > beatUs && nowUs - beatUs >= staleUs); }
constexpr uint32_t wd_window_ms(const Timing &t, uint32_t dwellMs, bool wrap = false, uint32_t measureMs = 0u) { return t.settleMs + (measureMs != 0u ? measureMs : t.measureMs) + dwellMs + t.slackMs + (wrap ? t.wrapExtraMs : 0u); }

// ---- state ------------------------------------------------------------------------------------------------------------------------------------------
struct PlanEnt { uint32_t abs, oldv, newv; uint16_t idx; };
// The P2 registers whose field a trial writes, with the golden of exactly that field (read with the golden copy, before any write this boot).
struct ExtDef { uint32_t abs, mask; const char *name; };
constexpr uint32_t kExtN = 3u, kExtDrr = 0u, kExtSel = 1u, kExtLock = 2u;
constexpr ExtDef kExt[kExtN] = { { kRegDbCtrl, kDbDrrMask, "OTG0_OTG_DOUBLE_BUFFER_CONTROL.DRR_MODE" }, { kRegGlobalCtrl2, kSelMask, "OTG0_OTG_GLOBAL_CONTROL2.LOCK_SEL" }, { kRegMasterLock, kLockReq, "OTG0_OTG_MASTER_UPDATE_LOCK.LOCK" } };
constexpr bool cap_has_abs(uint32_t abs) { for (uint32_t i = 0; i < kCapN; i++) if (kCap[i].abs == abs) return true; return false; }
constexpr bool ext_disjoint_from_cap() {
    for (uint32_t i = 0; i < kExtN; i++) if (cap_has_abs(kExt[i].abs)) return false;
    return !cap_has_abs(kRegHubpCntl) && !cap_has_abs(kRegOptcInGlobal) && !cap_has_abs(kRegPipeUpd) && !cap_has_abs(kRegStatusPos) && !cap_has_abs(kRegDp1Vid) && !cap_has_abs(kRegDp1Steer) &&
           !cap_has_abs(kRegDigFifoCtrl0) && !cap_has_abs(kRegPixelRateCntl);
}
static_assert(ext_disjoint_from_cap(), "no register of the underflow / lock / resync sequences is in the golden capture set (the kCap restore never touches them)");

// The per-trial report of the 0.0.606 sequences. Zeroed with the trial. The trial thread writes the P1 fields; the shared restore (under the lock) writes the *Restore* fields.
struct Rep {
    uint32_t ufHubpBefore, ufOptcBefore, ufHubpSettle, ufOptcSettle, ufHubpAfter, ufOptcAfter, ufHubpOr, ufOptcOr;
    uint32_t ufHubpMax, ufTimeoutMax, ufSamples, ufClears, ufClearRefused, ufClearStuck, ufFirstMs;
    bool ufSeen, ufAfterBad, ufUnread;
    uint32_t rsRuns, rsAttempts, rsRcFirst, rsRcLast, rsBlankUs, rsUnblankUs, rsDioErrs, rsStreamAfter, rsSteerAfter, rsDigAfter, rsRestoreRc, rsRestoreDioErrs;
    uint32_t dioBaseAfterRestore, dioResyncStart; bool restoreResynced, seqBad;
    uint32_t lkRc, lkWaitUs, lkPendingSeen, lkVertMax, lkVertSamples, lkDbBefore, lkDbDuring, lkDbAfter, lkLockAfter, lkSelBefore, lkPipeBefore, lkCleared, lkRestoreRc, lkConfirmed;
    uint32_t lkDrrBefore, lkDrrDuring, lkDrrAfter;
    bool latchConfirmed, lockStillHeld, stillBlanked, lockHeldProven; uint32_t lkHoldMax;
    uint32_t ufOptcRsDto, ufOptcRsLatch, ufOptcApTrans, dioHoldErrs, dioPostUnblankErrs, dioHoldBase, lkHoldRc, lkHoldRan;   // 0.0.608: the blanked-transition underflow reads (raw OPTC dwords, ORed over the restore's passes), the DIO attribution, the row-120 lock-hold witness result
    uint64_t firstWriteUs;
    n48dal::HoldRep hold; uint32_t hDfsMinDisp, hDfsMinDpp, hSamples, hLost;   // 0.0.607: the clock hold's report and the DFS samples taken while held
};

struct State {
    // per boot
    bool latched; uint32_t latchVerdict, latchRow;
    bool step50Passed;
    bool goldenValid, goldenAtBind, everWrote;
    uint32_t golden[kCapN];                       // the copy taken before any write this boot (raw dwords)
    uint32_t ext[kExtN]; bool extValid;           // 0.0.606: the golden of the P2 registers' fields (kExt), taken with the copy
    bool resyncPassed;                            // 0.0.606: a row-1 resync passed this boot
    bool row2Proven, ever120;                     // 0.0.607: a row-2 trial PASSED with the lock-hold proof and the latch confirmed this boot; a row-120 trial has written this boot
    bool blanked, lockHeld, drrWritten, selWritten, drrTouched, selTouched, wrapOn, lockOn, splitOn;   // 0.0.606: hardware ownership. Each is set BEFORE the write that makes it true; the restore clears it after the undo verified
    Rep rep;
    // 0.0.609 HELD mode. ATOMIC words (read without any lock, by scanAcquire under the scanout lock and by the session hooks): held, holdOn, ending, releaseReq, ownerGone, ownerSeq, handoff, handoffDeadlineUs, scanBad.
    // holdOn: this trial is a hold (set at its start, cleared at its end); held: the mode is UP and the entry snapshot is valid; ending: the end sequence began (no Acquire from then on); ownerSeq: the owning
    // native session (0 = none: a HANDOFF hold waiting for a taker); launching / runnerDone belong to the kext's launcher (Hw-independent).
    uint32_t held, holdOn, ending, releaseReq, ownerGone, ownerSeq, handoff, scanBad, endReason, holdFlags, launching, runnerDone;
    uint64_t handoffDeadlineUs;
    n48n_mode_result heldEntry, heldFinal;        // the entry snapshot (verdict HELD) and the trial's own result buffer (the hold thread's r)
    // per trial (all under Hw.lock except busy, which is atomic)
    uint32_t busy;
    uint32_t trialId, doneId;
    uint32_t nWritten; uint16_t written[kCapN];   // capture indexes, in write order; an index is recorded BEFORE its write is issued
    bool restoreClaimed, restoreDone, abortReq, wdFired, regsOk, writeFailed;
    uint32_t restoreTries, mismatch, badAbs, badHave, badWant;
    uint64_t deadlineUs, beatUs;
    uint32_t nPlan;
    uint32_t live[kCapN];
    PlanEnt plan[kCapN];
};

// ---- the hardware interface ----------------------------------------------------------------------------------------------------------------------
enum LogCode : uint32_t { kLogWrite = 1, kLogRestore = 2, kLogPhase = 3, kLogDeny = 4, kLogVerdict = 5, kLogWatchdog = 6, kLogMismatch = 7,
                          kLogSeq = 8 /* a field write of a 0.0.606 sequence: abs, old, new, ok */, kLogSeqEnd = 9 /* a sequence step's end: which, rc, us, extra */ };
enum SeqWhich : uint32_t { kSeqBlank = 1, kSeqUnblank = 2, kSeqLock = 3, kSeqUnlock = 4, kSeqPending = 5, kSeqLatch = 6, kSeqDrr = 7, kSeqUfClear = 8, kSeqStuckLock = 9, kSeqResync = 10 };
struct Hw {
    void *ctx;
    uint32_t (*rd)(void *, uint32_t abs);                       // a BAR5 dword read (0xFFFFFFFF = unreadable)
    bool (*wr)(void *, uint32_t abs, uint32_t v);               // the allowlisted write; false = refused or failed
    void (*sleep_ms)(void *, uint32_t ms);
    uint64_t (*now_us)(void *);
    bool (*frame)(void *, uint32_t *fc24);                      // OTG0_OTG_STATUS_FRAME_COUNT
    void (*lock)(void *);
    void (*unlock)(void *);
    bool (*gate_ok)(void *);                                    // native boot + S1b POSITIVE PASS
    bool (*armed)(void *);                                      // the display layer is bound
    bool (*plane_acquired)(void *);                             // the scanout plane is acquired or being restored
    bool (*in_use)(void *);                                     // dcnflip's pattern held, or a DCN interrupt source enabled
    bool (*otg_ok)(void *);                                     // OTG0 is the only lit OTG and its plane is the 2560x1440 linear ARGB8888 the goldens assume
    bool (*clocks)(void *, n48dal::Decoded *);                  // the DFS readback decoded (C1 table)
    bool (*start_watchdog)(void *, uint32_t trialId);           // starts a thread that calls watchdog_body(hw, st, timing, trialId)
    void (*log)(void *, uint32_t code, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
    void (*delay_us)(void *, uint32_t us);                      // 0.0.606: a busy delay of a few microseconds (the kext's IODelay); the polls of the sequences
    // 0.0.607: the clock hold (P4). They sleep and talk to the PMFW: called by the trial thread, `dcnmode 0` and the kext stop, NEVER with `lock` held, never by the watchdog. nullptr = no hold (row 120 is then denied).
    uint32_t (*hold_pre)(void *) = nullptr;                                                                             // n48dal::HoldPre, 0 = a hold may start
    uint32_t (*hold_raise)(void *, uint32_t needDispKhz, uint32_t needDppKhz, n48dal::HoldRep *, n48dal::Decoded *) = nullptr;   // n48dal::HoldRc, 0 = HELD
    uint32_t (*hold_release)(void *, bool restoreBad, n48dal::HoldRep *) = nullptr;                                     // n48dal::RelRc, 0 = RELEASED; restoreBad never releases
    uint32_t (*hold_state)(void *) = nullptr;                                                                           // n48dal::HoldSt
    // 0.0.609: put the scanout plane back to the console and wait for the restore to verify. Takes the scanout lock and SLEEPS: called with NO lock held. Returns 0 = nothing was acquired, 1 = it was and the
    // console is back and VERIFIED, 2 = it was and the restore did NOT verify. nullptr = no scanout (tests of the older sequences).
    uint32_t (*scan_release)(void *) = nullptr;
};

// ---- helpers ------------------------------------------------------------------------------------------------------------------------------------
inline void beat(const Hw &hw, State &st) { hw.lock(hw.ctx); st.beatUs = hw.now_us(hw.ctx); hw.unlock(hw.ctx); }
inline bool aborted(const Hw &hw, State &st) { hw.lock(hw.ctx); const bool a = st.abortReq; hw.unlock(hw.ctx); return a; }

// Read the whole capture set. False when any dword is unreadable (all-ones).
inline bool capture_read(const Hw &hw, uint32_t *out) {
    bool ok = true;
    for (uint32_t i = 0; i < kCapN; i++) { out[i] = hw.rd(hw.ctx, kCap[i].abs); if (out[i] == 0xFFFFFFFFu) ok = false; }
    return ok;
}
// The golden copy: read-only, once, before any write this boot (the kext calls it at bind on a native boot, and here on first use otherwise). Never overwrites a valid copy,
// and never takes one after a write.
inline bool golden_take(const Hw &hw, State &st, bool atBind) {
    hw.lock(hw.ctx);
    if (st.goldenValid) { hw.unlock(hw.ctx); return true; }
    if (st.everWrote) { hw.unlock(hw.ctx); return false; }
    uint32_t g[kCapN], x[kExtN];
    bool ok = capture_read(hw, g);
    for (uint32_t i = 0; i < kExtN; i++) { x[i] = hw.rd(hw.ctx, kExt[i].abs); if (x[i] == 0xFFFFFFFFu) ok = false; }   // 0.0.606: the DRR mode, the lock select and the lock request, read with the copy
    if (ok) { for (uint32_t i = 0; i < kCapN; i++) st.golden[i] = g[i]; for (uint32_t i = 0; i < kExtN; i++) st.ext[i] = x[i]; st.extValid = true; st.goldenValid = true; st.goldenAtBind = atBind; }
    hw.unlock(hw.ctx);
    return ok;
}
// Registers of the row whose live masked value differs from the golden copy (a trial refuses to start on a drifted machine). First offender out.
inline uint32_t drift_count(const RowInfo &r, const uint32_t *live, const uint32_t *golden, uint32_t *firstAbs) {
    uint32_t n = 0;
    for (uint32_t k = 0; k < r.n; k++) {
        const uint32_t i = r.idx[k];
        if ((live[i] & kCap[i].mask) != (golden[i] & kCap[i].mask)) { if (n == 0 && firstAbs) *firstAbs = kCap[i].abs; n++; }
    }
    return n;
}
// The live raster / DTO / MSA are the 60 Hz census mode the goldens were computed for (only the validated registers), and the DTO integer part is 0.
inline bool baseline_ok(const uint32_t *live) {
    const RowInfo b = row60();
    for (uint32_t k = 0; k < kValidatedN; k++) {
        const uint32_t i = cap_find(kValidatedNames[k]);
        if ((live[i] & kCap[i].mask) != (b.gold[i] & kCap[i].mask)) return false;
    }
    return true;
}
// The plan: every register of the row whose value is not already the row's (read-modify-write on the golden mask), in write order. *nSkip = the ones already there.
inline uint32_t plan_build(const RowInfo &r, const uint32_t *live, PlanEnt *out, uint32_t *nSkip) {
    uint32_t n = 0, skip = 0;
    for (uint32_t k = 0; k < r.n; k++) {
        const uint32_t i = r.idx[k];
        const uint32_t nv = rmw(live[i], kCap[i].mask, r.gold[i]);
        if (nv == live[i]) { skip++; continue; }
        out[n].abs = kCap[i].abs; out[n].oldv = live[i]; out[n].newv = nv; out[n].idx = (uint16_t)i; n++;
    }
    if (nSkip) *nSkip = skip;
    return n;
}

// ============================ 0.0.606: the hardware sequences (design notes/design/NATIVE-S2-120HZ.md P1 / P2 / P3) ==============================================
// Every function with a `_locked` suffix is called with Hw.lock HELD by its caller and sleeps under it (each wait is bounded; see the k*Polls constants): the register writes of a mode
// trial are exclusive, so the watchdog's restore and the trial thread can never interleave. A write is a read-modify-write of exactly the named field (wr_field); the read-only status bits
// and the write-1 acknowledges are always written 0. Nothing here writes a clock, a DP0/DP2/DP3 register, DIG_BE, the PHY, the link or the DPCD.
inline void beat_locked(const Hw &hw, State &st) { st.beatUs = hw.now_us(hw.ctx); }
inline uint32_t us32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v; }
inline void seq_end(const Hw &hw, uint32_t which, uint32_t rc, uint64_t t0, uint32_t extra) { hw.log(hw.ctx, kLogSeqEnd, which, rc, us32(hw.now_us(hw.ctx) - t0), extra); }

// One field write: read, replace exactly `field` with `val`, write 0 into every `never` bit, write. Sets *rc and returns false when the read is all-ones or the write is refused.
inline bool wr_field(const Hw &hw, uint32_t abs, uint32_t field, uint32_t val, uint32_t never, uint32_t *rc) {
    const uint32_t cur = hw.rd(hw.ctx, abs);
    if (cur == 0xFFFFFFFFu) { *rc = N48N_MODE_SEQ_UNREADABLE; return false; }
    const uint32_t nv = ((cur & ~field) | (val & field)) & ~never;
    const bool ok = hw.wr(hw.ctx, abs, nv);
    hw.log(hw.ctx, kLogSeq, abs, cur, nv, ok ? 1u : 0u);
    if (!ok) { *rc = N48N_MODE_SEQ_WRITE; return false; }
    return true;
}
// Poll until (register & mask) == want, at most `polls` reads, `pollUs` apart. An unreadable dword never satisfies it.
inline bool poll_reg(const Hw &hw, uint32_t abs, uint32_t mask, uint32_t want, uint32_t polls, uint32_t pollUs) {
    for (uint32_t i = 0; i < polls; i++) {
        const uint32_t v = hw.rd(hw.ctx, abs);
        if (v != 0xFFFFFFFFu && (v & mask) == want) return true;
        hw.delay_us(hw.ctx, pollUs);
    }
    return false;
}
// The apply's resync reports rs_rc_first / rs_rc_last; the restore's (and dcnmode 0's) reports rs_restore_rc = its first failure.
inline void note_rc(State &st, bool forRestore, uint32_t rc) {
    if (forRestore) { if (rc != N48N_MODE_SEQ_OK && st.rep.rsRestoreRc == 0u) st.rep.rsRestoreRc = rc; return; }
    if (rc != N48N_MODE_SEQ_OK && st.rep.rsRcFirst == 0u) st.rep.rsRcFirst = rc;
    st.rep.rsRcLast = rc;
}

// ---- P1: the underflow registers -------------------------------------------------------------------------------------------------------------------
struct UfRead { uint32_t hubp, optc; };
inline UfRead uf_read(const Hw &hw) { return UfRead{ hw.rd(hw.ctx, kRegHubpCntl), hw.rd(hw.ctx, kRegOptcInGlobal) }; }
enum UfKind : uint32_t { kUfSettle = 0, kUfJudged = 1, kUfAfter = 2, kUfTransition = 3 /* 0.0.608: a blanked transition (row 120): reported only, never judged */ };
constexpr bool uf_is_bad(const UfRead &u) { return uf::hubp_bad(u.hubp) || uf::optc_bad(u.optc); }
// Fold one sample into the trial's report. kUfSettle is reported only (the write transition may blip), kUfJudged sets ufSeen, kUfAfter (after the restore) sets ufAfterBad, kUfTransition (0.0.608, blanked) is reported only like kUfSettle. Returns "bad".
inline bool uf_record(const Hw &hw, State &st, const UfRead &u, uint32_t kind) {
    Rep &p = st.rep;
    p.ufSamples++;
    const bool bad = uf_is_bad(u);
    if (uf::unreadable(u.hubp) || uf::unreadable(u.optc)) p.ufUnread = true;
    if (!uf::unreadable(u.hubp)) {
        p.ufHubpOr |= uf::hubp_bits(u.hubp);
        if (uf::hubp_status(u.hubp) > p.ufHubpMax) p.ufHubpMax = uf::hubp_status(u.hubp);
        if (uf::hubp_timeout(u.hubp) > p.ufTimeoutMax) p.ufTimeoutMax = uf::hubp_timeout(u.hubp);
    }
    if (!uf::unreadable(u.optc)) p.ufOptcOr |= uf::optc_bits(u.optc);
    if (bad && kind == kUfJudged && !p.ufSeen) { p.ufSeen = true; p.ufFirstMs = us32((hw.now_us(hw.ctx) - p.firstWriteUs) / 1000ull); }
    if (bad && kind == kUfAfter) p.ufAfterBad = true;
    return bad;
}
// P1b: clear whatever is set with the write-1 strobes. Never while ODM0_OPTC_INPUT_GLOBAL_CONTROL.INPUT_SOFT_RESET is set (the read-modify-write would write the reset back); a strobe that reads
// back set is written back 0 (a level would blind the detector). An unreadable register is never written. True when both statuses read clear afterwards.
inline bool uf_clear_locked(const Hw &hw, State &st) {
    Rep &p = st.rep;
    const uint64_t t0 = hw.now_us(hw.ctx);
    UfRead u = uf_read(hw);
    if (uf::unreadable(u.hubp) || uf::unreadable(u.optc)) return false;
    bool wrote = false;
    if (uf::hubp_bad(u.hubp)) {
        const uint32_t nv = uf::hubp_clear_value(u.hubp);
        const bool ok = hw.wr(hw.ctx, kRegHubpCntl, nv);
        hw.log(hw.ctx, kLogSeq, kRegHubpCntl, u.hubp, nv, ok ? 1u : 0u);
        p.ufClears++; wrote = true;
    }
    if (uf::optc_bad(u.optc)) {
        if (uf::clear_refused(u.optc)) p.ufClearRefused++;
        else {
            const uint32_t nv = uf::optc_clear_value(u.optc);
            const bool ok = hw.wr(hw.ctx, kRegOptcInGlobal, nv);
            hw.log(hw.ctx, kLogSeq, kRegOptcInGlobal, u.optc, nv, ok ? 1u : 0u);
            p.ufClears++; wrote = true;
        }
    }
    if (wrote) {
        hw.delay_us(hw.ctx, 20u);
        u = uf_read(hw);
        if (!uf::unreadable(u.hubp) && (u.hubp & (uf::kHubpClear | uf::kHubpTimeoutClear)) != 0u) {
            const uint32_t nv = uf::hubp_unstrobe_value(u.hubp);
            const bool ok = hw.wr(hw.ctx, kRegHubpCntl, nv);
            hw.log(hw.ctx, kLogSeq, kRegHubpCntl, u.hubp, nv, ok ? 1u : 0u);
            p.ufClearStuck++;
        }
        if (!uf::unreadable(u.optc) && (u.optc & uf::kOptcClear) != 0u && !uf::clear_refused(u.optc)) {
            const uint32_t nv = uf::optc_unstrobe_value(u.optc);
            const bool ok = hw.wr(hw.ctx, kRegOptcInGlobal, nv);
            hw.log(hw.ctx, kLogSeq, kRegOptcInGlobal, u.optc, nv, ok ? 1u : 0u);
            p.ufClearStuck++;
        }
        u = uf_read(hw);
    }
    seq_end(hw, kSeqUfClear, uf_is_bad(u) ? 1u : 0u, t0, p.ufClears);
    return !uf_is_bad(u);
}

// ---- P3: the DP1 blank / unblank, exactly Linux's order (enc1_stream_encoder_dp_blank + dce110_blank_stream; enc401_stream_encoder_dp_unblank, M/N block skipped) --------------------
// Blank: return if the stream is not enabled; DIS_DEFER = 2; ENABLE = 0; wait STATUS == 0 (10 us x 10020); THEN STEER_FIFO_RESET = 1 (earlier leaves the status stuck); msleep(60).
// st.blanked is set BEFORE the write that stops the stream and stays set until an unblank verified the stream is up again.
inline uint32_t blank_locked(const Hw &hw, State &st) {
    uint32_t rc = N48N_MODE_SEQ_OK;
    const uint64_t t0 = hw.now_us(hw.ctx);
    do {
        const uint32_t v = hw.rd(hw.ctx, kRegDp1Vid);
        if (v == 0xFFFFFFFFu) { rc = N48N_MODE_SEQ_UNREADABLE; break; }
        if ((v & kVidEnable) == 0u) { rc = N48N_MODE_SEQ_NOT_ENABLED; break; }                                       // Linux: return when the stream is not enabled
        if (!wr_field(hw, kRegDp1Vid, kVidDisDefer, kVidDisDefer2, kVidStatus, &rc)) break;                          // DIS_DEFER = 2
        st.blanked = true;
        if (!wr_field(hw, kRegDp1Vid, kVidEnable, 0u, kVidStatus, &rc)) break;                                        // ENABLE = 0
        if (!poll_reg(hw, kRegDp1Vid, kVidStatus, 0u, kStatusPolls, kStatusPollUs)) { rc = N48N_MODE_SEQ_STATUS_STUCK; break; }   // wait STATUS == 0 FIRST
        if (!wr_field(hw, kRegDp1Steer, kSteerReset, kSteerReset, kSteerNever, &rc)) break;                          // THEN STEER_FIFO_RESET = 1
        hw.sleep_ms(hw.ctx, kBlankSleepMs);                                                                          // msleep(60)
    } while (false);
    beat_locked(hw, st);
    seq_end(hw, kSeqBlank, rc, t0, 0u);
    return rc;
}
// Unblank: ENABLE = 0 + wait STATUS == 0; STEER_FIFO_RESET 1, 10 us, 0, STEER_FIFO_ENABLE 1; ENABLE = 1 with DIS_DEFER = 2, 200 us; DIG_FIFO_READ_START_LEVEL = 7; DIG_FIFO_RESET = 1, wait RESET_DONE = 1
// (<= 50 ms), RESET = 0, wait RESET_DONE = 0; DIG_FIFO_ENABLE = 1, 100 us; ENABLE = 1; then the stream must read ENABLE + STATUS (<= 100 ms). It works from any state (idempotent).
// `force` (the LAST attempt, and every recovery path through unblank_retry_locked) follows Linux past a wait that times out (REG_WAIT only logs): the timeout is recorded in the return code and the
// sequence carries on to the final ENABLE = 1, so a STATUS that sticks at 1 can never leave the stream disabled. A refused write still ends it. `blanked` clears when the stream reads active.
inline uint32_t unblank_locked(const Hw &hw, State &st, bool force = false) {
    uint32_t rc = N48N_MODE_SEQ_OK;
    const uint64_t t0 = hw.now_us(hw.ctx);
    do {
        if (hw.rd(hw.ctx, kRegDp1Vid) == 0xFFFFFFFFu) { rc = N48N_MODE_SEQ_UNREADABLE; break; }
        if (!wr_field(hw, kRegDp1Vid, kVidEnable, 0u, kVidStatus, &rc)) break;                                        // ENABLE = 0
        if (!poll_reg(hw, kRegDp1Vid, kVidStatus, 0u, kStatusPolls, kStatusPollUs)) { if (rc == N48N_MODE_SEQ_OK) rc = N48N_MODE_SEQ_STATUS_STUCK; if (!force) break; }
        if (!wr_field(hw, kRegDp1Steer, kSteerReset, kSteerReset, kSteerNever, &rc)) break;                          // STEER_FIFO_RESET = 1
        hw.delay_us(hw.ctx, kSteerPulseUs);
        if (!wr_field(hw, kRegDp1Steer, kSteerReset, 0u, kSteerNever, &rc)) break;                                   // = 0
        if (!wr_field(hw, kRegDp1Steer, kSteerEnable, kSteerEnable, kSteerNever, &rc)) break;                        // STEER_FIFO_ENABLE = 1
        if (!wr_field(hw, kRegDp1Vid, kVidEnable | kVidDisDefer, kVidEnable | kVidDisDefer2, kVidStatus, &rc)) break; // ENABLE = 1, DIS_DEFER = 2
        hw.delay_us(hw.ctx, kStreamEnableUs);
        if (!wr_field(hw, kRegDigFifoCtrl0, kDigLevelMask, kDigLevel7, kDigNever, &rc)) break;                       // DIG_FIFO_READ_START_LEVEL = 7
        if (!wr_field(hw, kRegDigFifoCtrl0, kDigReset, kDigReset, kDigNever, &rc)) break;                            // DIG_FIFO_RESET = 1
        const bool done = poll_reg(hw, kRegDigFifoCtrl0, kDigResetDone, kDigResetDone, kDigDonePolls, kDigDonePollUs);
        uint32_t rc2 = N48N_MODE_SEQ_OK;
        (void)wr_field(hw, kRegDigFifoCtrl0, kDigReset, 0u, kDigNever, &rc2);                                        // RESET = 0 (always released, even when the wait failed)
        if (!done) { if (rc == N48N_MODE_SEQ_OK) rc = N48N_MODE_SEQ_DIG_RESET; if (!force) break; }
        if (rc2 != N48N_MODE_SEQ_OK) { rc = rc2; break; }
        if (!done || !poll_reg(hw, kRegDigFifoCtrl0, kDigResetDone, 0u, kDigDonePolls, kDigDonePollUs)) { if (rc == N48N_MODE_SEQ_OK) rc = N48N_MODE_SEQ_DIG_RESET; if (!force) break; }
        if (!wr_field(hw, kRegDigFifoCtrl0, kDigEnable, kDigEnable, kDigNever, &rc)) break;                          // DIG_FIFO_ENABLE = 1
        hw.delay_us(hw.ctx, kDigEnableUs);
        if (!wr_field(hw, kRegDp1Vid, kVidEnable, kVidEnable, kVidStatus, &rc)) break;                               // ENABLE = 1
        if (!poll_reg(hw, kRegDp1Vid, kVidEnable | kVidStatus, kVidEnable | kVidStatus, kActivePolls, kActivePollUs)) { if (rc == N48N_MODE_SEQ_OK) rc = N48N_MODE_SEQ_NOT_ACTIVE; break; }
        st.blanked = false;                                                                                           // the stream is up again: the only place the flag clears
    } while (false);
    beat_locked(hw, st);
    seq_end(hw, kSeqUnblank, rc, t0, 0u);
    return rc;
}
// One retry each. A blank that failed after it had stopped the stream is NOT retried (the unblank is the way back); a blank that failed before touching anything is tried once more.
inline uint32_t blank_retry_locked(const Hw &hw, State &st, bool forRestore) {
    uint32_t rc = blank_locked(hw, st);
    st.rep.rsAttempts++; note_rc(st, forRestore, rc);
    if (rc != N48N_MODE_SEQ_OK && rc != N48N_MODE_SEQ_NOT_ENABLED && rc != N48N_MODE_SEQ_UNREADABLE && !st.blanked) {
        rc = blank_locked(hw, st);
        st.rep.rsAttempts++; note_rc(st, forRestore, rc);
    }
    return rc;
}
inline uint32_t unblank_retry_locked(const Hw &hw, State &st, bool forRestore) {
    uint32_t rc = unblank_locked(hw, st);
    st.rep.rsAttempts++; note_rc(st, forRestore, rc);
    if (rc != N48N_MODE_SEQ_OK) {
        rc = unblank_locked(hw, st, true);
        st.rep.rsAttempts++; note_rc(st, forRestore, rc);
    }
    return rc;
}
// The two halves of the wrapper, with the durations and the DIO count kept for the report (the apply's; the restore's pair is kept by restore_locked).
inline uint32_t wrap_blank_locked(const Hw &hw, State &st, bool forRestore = false) {
    st.rep.rsRuns++;
    st.rep.dioResyncStart = hw.rd(hw.ctx, kRegPixelRateCntl);
    const uint64_t t0 = hw.now_us(hw.ctx);
    const uint32_t rc = blank_retry_locked(hw, st, forRestore);
    if (!forRestore) st.rep.rsBlankUs = us32(hw.now_us(hw.ctx) - t0);
    return rc;
}
inline uint32_t wrap_unblank_locked(const Hw &hw, State &st, bool forRestore = false) {
    const uint64_t t0 = hw.now_us(hw.ctx);
    const uint32_t rc = unblank_retry_locked(hw, st, forRestore);
    const uint32_t prc = hw.rd(hw.ctx, kRegPixelRateCntl);
    if (forRestore) {
        st.rep.restoreResynced = true; st.rep.dioBaseAfterRestore = prc;              // the restore's FIFO health is judged from the END of its resync: growth across the resync itself is reported
        st.rep.rsRestoreDioErrs = err_count(prc) - err_count(st.rep.dioResyncStart);
    } else {
        st.rep.rsUnblankUs = us32(hw.now_us(hw.ctx) - t0);
        st.rep.rsDioErrs = err_count(prc) - err_count(st.rep.dioResyncStart);
        st.rep.rsStreamAfter = hw.rd(hw.ctx, kRegDp1Vid); st.rep.rsSteerAfter = hw.rd(hw.ctx, kRegDp1Steer); st.rep.rsDigAfter = hw.rd(hw.ctx, kRegDigFifoCtrl0);
    }
    return rc;
}
// The standalone resync (row 1): blank, then unblank, each with its retry. The stream is up again only when the unblank verified it.
inline uint32_t resync_locked(const Hw &hw, State &st, bool forRestore = false) {
    const uint64_t t0 = hw.now_us(hw.ctx);
    const uint32_t b = st.blanked ? N48N_MODE_SEQ_OK : wrap_blank_locked(hw, st, forRestore);
    if (b != N48N_MODE_SEQ_OK && !st.blanked) { seq_end(hw, kSeqResync, b, t0, 0u); return b; }   // the stream was never stopped: nothing to bring back
    const uint32_t u = wrap_unblank_locked(hw, st, forRestore);
    const uint32_t rc = b != N48N_MODE_SEQ_OK ? b : u;
    seq_end(hw, kSeqResync, rc, t0, st.rep.rsAttempts);
    return rc;
}

// ---- P2: the OTG master update lock (Linux optc3_lock / optc1_unlock), the pending wait and the V_TOTAL latch witness ---------------------------------------------------------
// lock: MASTER_UPDATE_LOCK_SEL = OTG inst; MASTER_UPDATE_LOCK = 1; wait UPDATE_LOCK_STATUS. st.selWritten / st.lockHeld are set BEFORE the writes; a status that never rises releases the lock again.
inline uint32_t unlock_locked(const Hw &hw, State &st) {
    uint32_t rc = N48N_MODE_SEQ_OK;
    const uint64_t t0 = hw.now_us(hw.ctx);
    do {
        if (!wr_field(hw, kRegMasterLock, kLockReq, 0u, kLockStatus, &rc)) break;                                    // MASTER_UPDATE_LOCK = 0
        if (poll_reg(hw, kRegMasterLock, kLockReq | kLockStatus, 0u, kLockPolls, kLockPollUs)) { st.lockHeld = false; break; }
        (void)wr_field(hw, kRegMasterLock, kLockReq, 0u, kLockStatus, &rc);                                          // once more
        if (poll_reg(hw, kRegMasterLock, kLockReq | kLockStatus, 0u, kLockPolls, kLockPollUs)) { st.lockHeld = false; rc = N48N_MODE_SEQ_OK; break; }
        rc = N48N_MODE_SEQ_LOCK_STUCK;
    } while (false);
    beat_locked(hw, st);
    seq_end(hw, kSeqUnlock, rc, t0, 0u);
    return rc;
}
inline uint32_t lock_locked(const Hw &hw, State &st) {
    uint32_t rc = N48N_MODE_SEQ_OK;
    const uint64_t t0 = hw.now_us(hw.ctx);
    st.selWritten = true; st.selTouched = true;
    if (!wr_field(hw, kRegGlobalCtrl2, kSelMask, kOtgInst << kSelShift, 0u, &rc)) { seq_end(hw, kSeqLock, rc, t0, 0u); return rc; }
    st.lockHeld = true;
    if (!wr_field(hw, kRegMasterLock, kLockReq, kLockReq, kLockStatus, &rc)) { seq_end(hw, kSeqLock, rc, t0, 0u); return rc; }
    const bool got = poll_reg(hw, kRegMasterLock, kLockStatus, kLockStatus, kLockPolls, kLockPollUs);
    st.rep.lkWaitUs = us32(hw.now_us(hw.ctx) - t0);
    if (!got) { (void)unlock_locked(hw, st); rc = N48N_MODE_SEQ_LOCK_TIMEOUT; }
    seq_end(hw, kSeqLock, rc, t0, st.rep.lkWaitUs);
    return rc;
}
// A stuck lock request from an earlier failure is cleared first at every restore entry.
inline void clear_stuck_lock_locked(const Hw &hw, State &st) {
    const uint32_t v = hw.rd(hw.ctx, kRegMasterLock);
    if (v == 0xFFFFFFFFu) return;
    if ((v & kLockReq) == 0u) {                     // the request is already 0: a lingering status must not leave `lockHeld` set (it would deny every later trial as drift)
        if (st.lockHeld && poll_reg(hw, kRegMasterLock, kLockReq | kLockStatus, 0u, kLockPolls, kLockPollUs)) st.lockHeld = false;
        return;
    }
    st.rep.lkCleared++;
    const uint32_t rc = unlock_locked(hw, st);
    seq_end(hw, kSeqStuckLock, rc, hw.now_us(hw.ctx), 0u);
}
// dcn30_wait_for_all_pending_updates: the double-buffer pending bits (and DC_REG_UPDATE_PENDING) clear, <= 100 ms.
inline uint32_t wait_pending_locked(const Hw &hw, State &st) {
    uint32_t seen = 0u, rc = N48N_MODE_SEQ_PENDING_TIMEOUT;
    const uint64_t t0 = hw.now_us(hw.ctx);
    for (uint32_t i = 0; i < kPendingPolls; i++) {
        const uint32_t db = hw.rd(hw.ctx, kRegDbCtrl), pipe = hw.rd(hw.ctx, kRegPipeUpd);
        if (db != 0xFFFFFFFFu) seen |= db & kDbPendingMask;
        if (db != 0xFFFFFFFFu && pipe != 0xFFFFFFFFu && (db & kDbPendingMask) == 0u && (pipe & kPipeRegPending) == 0u) { rc = N48N_MODE_SEQ_OK; break; }
        hw.delay_us(hw.ctx, kPendingPollUs);
    }
    st.rep.lkPendingSeen |= seen;
    beat_locked(hw, st);
    seq_end(hw, kSeqPending, rc, t0, seen);
    return rc;
}
// The V_TOTAL latch witness, raising: OTG0_OTG_STATUS_POSITION.VERT_COUNT must exceed the OLD maximum within 3 frames (it can only count past it once the new total has latched).
inline uint32_t latch_raise_locked(const Hw &hw, State &st, uint32_t oldMax, uint32_t frameUs) {
    const uint64_t t0 = hw.now_us(hw.ctx), limit = 3ull * frameUs + 5000ull;
    uint32_t vmax = 0u, n = 0u; bool seen = false;
    while (hw.now_us(hw.ctx) - t0 < limit) {
        const uint32_t v = hw.rd(hw.ctx, kRegStatusPos);
        if (v != 0xFFFFFFFFu) { const uint32_t c = v & kVertMask; n++; if (c > vmax) vmax = c; if (c > oldMax) { seen = true; break; } }
        hw.delay_us(hw.ctx, kWitnessPollUs);
    }
    if (vmax > st.rep.lkVertMax) st.rep.lkVertMax = vmax;
    st.rep.lkVertSamples += n;
    beat_locked(hw, st);
    const uint32_t rc = seen ? N48N_MODE_SEQ_OK : N48N_MODE_SEQ_LATCH;
    seq_end(hw, kSeqLatch, rc, t0, vmax);
    return rc;
}
// Lowering: after the pending bits cleared, no VERT_COUNT over two frames may exceed the NEW maximum (and the counter must be seen running).
inline uint32_t latch_lower_locked(const Hw &hw, State &st, uint32_t newMax, uint32_t frameUs) {
    const uint64_t t0 = hw.now_us(hw.ctx), limit = 2ull * frameUs + 2000ull;
    uint32_t vmax = 0u, n = 0u;
    while (hw.now_us(hw.ctx) - t0 < limit) {
        const uint32_t v = hw.rd(hw.ctx, kRegStatusPos);
        if (v != 0xFFFFFFFFu) { const uint32_t c = v & kVertMask; n++; if (c > vmax) vmax = c; }
        hw.delay_us(hw.ctx, kWitnessPollUs);
    }
    if (vmax > st.rep.lkVertMax) st.rep.lkVertMax = vmax;
    st.rep.lkVertSamples += n;
    beat_locked(hw, st);
    const uint32_t rc = (n != 0u && vmax <= newMax && vmax > 100u) ? N48N_MODE_SEQ_OK : N48N_MODE_SEQ_LATCH;
    seq_end(hw, kSeqLatch, rc, t0, vmax);
    return rc;
}
// The lock-hold witness (row 2): with the V_TOTAL write pending under the lock the counter must keep to the OLD maximum for two frames (the write is HELD, not applied). Design row 120 requires it.
inline uint32_t lock_hold_witness_locked(const Hw &hw, State &st, uint32_t oldMax, uint32_t frameUs) {
    const uint64_t t0 = hw.now_us(hw.ctx), limit = 2ull * frameUs + 2000ull;
    uint32_t vmax = 0u, n = 0u;
    while (hw.now_us(hw.ctx) - t0 < limit) {
        const uint32_t v = hw.rd(hw.ctx, kRegStatusPos);
        if (v != 0xFFFFFFFFu) { const uint32_t c = v & kVertMask; n++; if (c > vmax) vmax = c; }
        hw.delay_us(hw.ctx, kWitnessPollUs);
    }
    st.rep.lkHoldMax = vmax;
    beat_locked(hw, st);
    const bool ok = n != 0u && vmax <= oldMax && vmax > 100u;
    if (ok) st.rep.lockHeldProven = true;
    const uint32_t rc = ok ? N48N_MODE_SEQ_OK : N48N_MODE_SEQ_HOLD;
    seq_end(hw, kSeqLatch, rc, t0, vmax);
    return rc;
}
// DRR_TIMING_DBUF_UPDATE_MODE (Linux optc3_set_timing_double_buffer: 2 = start of frame, 0 = any time). st.drrWritten is set BEFORE the write; the field must read back.
inline uint32_t drr_set_locked(const Hw &hw, State &st, uint32_t mode) {
    uint32_t rc = N48N_MODE_SEQ_OK;
    const uint64_t t0 = hw.now_us(hw.ctx);
    st.drrWritten = true; st.drrTouched = true;
    do {
        if (!wr_field(hw, kRegDbCtrl, kDbDrrMask, mode << kDbDrrShift, kDbPendingMask, &rc)) break;
        hw.delay_us(hw.ctx, 20u);
        const uint32_t v = hw.rd(hw.ctx, kRegDbCtrl);
        if (v == 0xFFFFFFFFu) { rc = N48N_MODE_SEQ_UNREADABLE; break; }
        if (((v & kDbDrrMask) >> kDbDrrShift) != mode) rc = N48N_MODE_SEQ_DRR;
    } while (false);
    seq_end(hw, kSeqDrr, rc, t0, mode);
    return rc;
}
// P2's precondition, from the four census registers: the lock hardware is idle and not bypassed. The DRR mode may be 0 (the GOP state) or 2 (it is what the trial writes).
constexpr bool lock_usable(uint32_t db, uint32_t lockReg, uint32_t sel, uint32_t pipe) {
    return db != 0xFFFFFFFFu && lockReg != 0xFFFFFFFFu && sel != 0xFFFFFFFFu && pipe != 0xFFFFFFFFu && (db & kDbInstantly) == 0u && (db & kDbPendingMask) == 0u &&
           (lockReg & (kLockReq | kLockStatus)) == 0u && (pipe & kPipeRegPending) == 0u && (((db & kDbDrrMask) >> kDbDrrShift) == 0u || ((db & kDbDrrMask) >> kDbDrrShift) == kDrrModeStartOfFrame);
}

// ---- the apply: what the trial writes, in order, under ONE hold of the lock ----------------------------------------------------------------------------------------------------
struct Apply { bool writeFailed = false, lockFail = false, resyncFail = false; };
// One write group of the plan, in plan order: every index is recorded BEFORE its write is issued. Stops at the first refused write (st.writeFailed).
inline void write_group_locked(const Hw &hw, State &st, uint32_t grp, n48n_mode_result *r, Apply &a) {
    for (uint32_t k = 0; k < st.nPlan && !st.restoreClaimed && !a.writeFailed; k++) {
        const PlanEnt &e = st.plan[k];
        if (cap_group(e.idx) != grp) continue;
        st.written[st.nWritten++] = e.idx;
        const bool ok = hw.wr(hw.ctx, e.abs, e.newv);
        hw.log(hw.ctx, kLogWrite, e.abs, e.oldv, e.newv, ok ? 1u : 0u);
        r->nwrite++;
        if (r->first_write_abs == 0u) r->first_write_abs = e.abs;
        r->last_write_abs = e.abs;
        if (!ok) { st.writeFailed = true; a.writeFailed = true; }
    }
}
// Row 120 (design P5 steps 2..8; the clock hold, step 1, is raised by run_trial before this runs, and released after the restore): blank; DRR mode 2 then the OTG master lock; the OTG timing / global sync /
// VTG / HUBP DLG-TTU registers under the lock; unlock, wait for the pending bits, V_TOTAL latch witness; THEN the DTO (F5: the pixel clock applies immediately, so it moves after the unlock); then the MSA;
// then the unblank. Every step after a failure is skipped and the restore runs (it undoes exactly the recorded writes). The lock is released on EVERY exit of the OTG writes.
inline Apply apply_120_locked(const Hw &hw, State &st, n48n_mode_result *r) {
    Apply a;
    Rep &p = st.rep;
    const uint32_t frameUs = us32(1000000000ull / expect_mhz(row60()));
    if (wrap_blank_locked(hw, st) != N48N_MODE_SEQ_OK) { a.resyncFail = true; return a; }            // step 2: nothing has moved; a half-done blank is undone by the restore
    uint32_t rc = drr_set_locked(hw, st, kDrrModeStartOfFrame);                                       // step 3: DRR mode 2 (F6), then the lock
    if (rc == N48N_MODE_SEQ_OK) rc = lock_locked(hw, st);
    p.lkRc = rc;
    if (rc != N48N_MODE_SEQ_OK) { a.lockFail = true; return a; }
    write_group_locked(hw, st, kGrpOtg, r, a);                                                        // step 4
    if (!a.writeFailed) {                                                                             // 0.0.608: the lock-hold witness, exactly as row 2 (blanked, still the old 60 Hz clock, ~35 ms): stored, NEVER a failure here
        p.lkHoldRan = 1u;
        p.lkHoldRc = lock_hold_witness_locked(hw, st, st.live[kIdxVTotal] & kVertMask, frameUs);
    }
    const uint32_t u = unlock_locked(hw, st);                                                         // step 5: the unlock happens on EVERY exit of the writes
    if (u != N48N_MODE_SEQ_OK) { p.lkRc = u; a.lockFail = true; }
    else if (!a.writeFailed) {
        const uint32_t w = wait_pending_locked(hw, st);
        if (w != N48N_MODE_SEQ_OK) { p.lkRc = w; a.lockFail = true; }
        else {
            const uint32_t l = latch_raise_locked(hw, st, st.live[kIdxVTotal] & kVertMask, frameUs);
            if (l != N48N_MODE_SEQ_OK) { p.lkRc = l; a.lockFail = true; } else { p.latchConfirmed = true; p.lkConfirmed++; }
        }
    }
    p.lkDbDuring = hw.rd(hw.ctx, kRegDbCtrl);
    p.lkDrrDuring = (p.lkDbDuring & kDbDrrMask) >> kDbDrrShift;
    if (!a.writeFailed && !a.lockFail) {
        write_group_locked(hw, st, kGrpDto, r, a);                                                    // step 6: DTO (after the unlock)
        if (!a.writeFailed) write_group_locked(hw, st, kGrpMsa, r, a);                                // step 7: MSA
    }
    if (!a.writeFailed && !a.lockFail) {
        // 0.0.608 (design notes, "blanked transition"): the OPTC underflow the DTO / OTG changes leave behind is recorded (reported only) and CLEARED while the stream is still blanked and nothing is on the glass,
        // so the judged reads that follow (the settle read after the unblank, the sampler) see only what happens on a visible picture. Linux never judges it (it clears it after disable_crtc).
        { const UfRead ut = uf_read(hw); p.ufOptcApTrans = ut.optc; (void)uf_record(hw, st, ut, kUfTransition); if (uf_is_bad(ut)) (void)uf_clear_locked(hw, st); }
        if (wrap_unblank_locked(hw, st) != N48N_MODE_SEQ_OK) a.resyncFail = true;                     // step 8
        p.dioPostUnblankErrs = err_count(hw.rd(hw.ctx, kRegPixelRateCntl)) - err_count(p.dioHoldBase);   // 0.0.608: DIO errors from the hold's raise to just after the unblank (reported, not judged)
    }
    return a;
}
inline Apply apply_locked(const Hw &hw, State &st, const RowInfo &ri, bool wrap, bool lockRow, n48n_mode_result *r) {
    Apply a;
    Rep &p = st.rep;
    if (ri.row == 1u) {                                             // the standalone resync: blank + unblank and nothing else
        if (resync_locked(hw, st) != N48N_MODE_SEQ_OK) a.resyncFail = true;
        return a;
    }
    if (ri.row == 120u) return apply_120_locked(hw, st, r);          // 0.0.607: the split sequence (OTG under the lock, then DTO, then MSA)
    if (wrap && wrap_blank_locked(hw, st) != N48N_MODE_SEQ_OK) { a.resyncFail = true; return a; }   // nothing has moved; a half-done blank is undone by the restore
    if (lockRow) {
        uint32_t rc = drr_set_locked(hw, st, kDrrModeStartOfFrame);
        if (rc == N48N_MODE_SEQ_OK) rc = lock_locked(hw, st);
        p.lkRc = rc;
        if (rc != N48N_MODE_SEQ_OK) { a.lockFail = true; return a; }
    }
    for (uint32_t k = 0; k < st.nPlan && !st.restoreClaimed; k++) {
        const PlanEnt &e = st.plan[k];
        st.written[st.nWritten++] = e.idx;
        const bool ok = hw.wr(hw.ctx, e.abs, e.newv);
        hw.log(hw.ctx, kLogWrite, e.abs, e.oldv, e.newv, ok ? 1u : 0u);
        r->nwrite++;
        if (r->first_write_abs == 0u) r->first_write_abs = e.abs;
        r->last_write_abs = e.abs;
        if (!ok) { st.writeFailed = true; a.writeFailed = true; break; }
    }
    if (lockRow && !a.writeFailed) {                                // the V_TOTAL write must be HELD while blanked and locked
        const uint32_t h = lock_hold_witness_locked(hw, st, st.live[kIdxVTotal] & kVertMask, us32(1000000000ull / expect_mhz(row60())));
        if (h != N48N_MODE_SEQ_OK) { p.lkRc = h; a.lockFail = true; }
    }
    if (lockRow) {                                                  // the unlock happens on EVERY exit of the writes
        const uint32_t u = unlock_locked(hw, st);
        if (u != N48N_MODE_SEQ_OK) { p.lkRc = u; a.lockFail = true; }
        else if (!a.writeFailed && !a.lockFail) {
            const uint32_t w = wait_pending_locked(hw, st);
            if (w != N48N_MODE_SEQ_OK) { p.lkRc = w; a.lockFail = true; }
            else {
                const uint32_t l = latch_raise_locked(hw, st, st.live[kIdxVTotal] & kVertMask, us32(1000000000ull / expect_mhz(row60())));
                if (l != N48N_MODE_SEQ_OK) { p.lkRc = l; a.lockFail = true; } else { p.latchConfirmed = true; p.lkConfirmed++; }
            }
        }
        p.lkDbDuring = hw.rd(hw.ctx, kRegDbCtrl);
        p.lkDrrDuring = (p.lkDbDuring & kDbDrrMask) >> kDbDrrShift;
    }
    if (wrap && wrap_unblank_locked(hw, st) != N48N_MODE_SEQ_OK) a.resyncFail = true;
    return a;
}

// ---- THE restore ---------------------------------------------------------------------------------------------------------------------------
// Verify every written register against the golden copy under its mask; fills st.mismatch / badAbs / badHave / badWant. Caller holds the lock.
inline uint32_t verify_written_locked(const Hw &hw, State &st) {
    uint32_t bad = 0;
    st.badAbs = st.badHave = st.badWant = 0;
    for (uint32_t k = 0; k < st.nWritten; k++) {
        const uint32_t i = st.written[k];
        const uint32_t have = hw.rd(hw.ctx, kCap[i].abs), want = st.golden[i];
        if (have == 0xFFFFFFFFu || (have & kCap[i].mask) != (want & kCap[i].mask)) {
            if (bad == 0) { st.badAbs = kCap[i].abs; st.badHave = have; st.badWant = want; }
            bad++;
        }
    }
    st.mismatch = bad;
    return bad;
}
constexpr uint32_t kGrpAll = 0xFFu;
// The recorded writes of one group (or all of them) put back to the golden copy, in REVERSE write order, read-modify-write on the mask.
inline void restore_group_locked(const Hw &hw, State &st, uint32_t grp) {
    for (uint32_t k = st.nWritten; k-- > 0u; ) {
        const uint32_t i = st.written[k];
        if (grp != kGrpAll && cap_group(i) != grp) continue;
        const uint32_t cur = hw.rd(hw.ctx, kCap[i].abs);
        const uint32_t nv = rmw(cur == 0xFFFFFFFFu ? st.golden[i] : cur, kCap[i].mask, st.golden[i]);
        const bool ok = hw.wr(hw.ctx, kCap[i].abs, nv);
        hw.log(hw.ctx, kLogRestore, kCap[i].abs, cur, nv, ok ? 1u : 0u);
    }
}
// The restore itself. Caller holds the lock and owns the claim. Order (design P5 step 12, as far as this build has the pieces): the underflow status of the trial phase is recorded and cleared, so
// only the restore transition is judged afterwards (P1b); a stuck OTG lock request is cleared (P2, every restore entry); the DP1 stream is blanked when the trial wrapped its writes in the resync and
// moved something (P3); then, per pass, [lock] the registers go back in REVERSE write order [unlock, wait for the pending bits, latch witness], read-modify-write on the mask, and are verified, and one
// more full pass if anything differs; the DRR mode and the lock select go back to their golden fields; the stream is unblanked. A wait that fails does NOT stop the restore: it continues and records
// seqBad (the verdict is RESTORE).
inline void restore_locked(const Hw &hw, State &st) {
    Rep &p = st.rep;
    { const UfRead u = uf_read(hw); if (uf_record(hw, st, u, kUfJudged)) (void)uf_clear_locked(hw, st); }
    clear_stuck_lock_locked(hw, st);
    bool ranBlank = false;
    if (st.wrapOn && (st.nWritten != 0u || st.drrWritten) && !st.blanked) {
        const uint32_t b = wrap_blank_locked(hw, st, true);
        ranBlank = true;
        if (b != N48N_MODE_SEQ_OK && b != N48N_MODE_SEQ_NOT_ENABLED) p.seqBad = true;
    }
    const uint32_t frameUs = us32(1000000000ull / expect_mhz(row60()));
    for (uint32_t pass = 0; pass < 2u; pass++) {
        st.restoreTries = pass + 1u;
        // 0.0.607 (row 120, design P2 "lower" / P5 step 12): DTO first, BEFORE the lock; then the lock, the OTG + HUBP registers in reverse, unlock, pending, latch witness; then the MSA. Rows 1 / 2 / 50: one group, as 0.0.606.
        const bool split = st.splitOn;
        uint32_t nOtg = 0u;
        for (uint32_t k = 0; k < st.nWritten; k++) if (cap_group(st.written[k]) == kGrpOtg) nOtg++;
        const bool useLock = st.lockOn && (split ? nOtg != 0u : st.nWritten != 0u);
        if (split) {
            restore_group_locked(hw, st, kGrpDto);
            // 0.0.608: ONE OPTC read right after the DTO step, before the lock: recorded, NOT cleared (attribution: was the bit set by the 120 -> 60 Hz pixel clock change or by the latch below?).
            const UfRead ud = uf_read(hw); p.ufOptcRsDto |= ud.optc; (void)uf_record(hw, st, ud, kUfTransition);
        }
        if (useLock) { const uint32_t l = lock_locked(hw, st); if (l != N48N_MODE_SEQ_OK) { p.lkRestoreRc = l; p.seqBad = true; } }
        restore_group_locked(hw, st, split ? kGrpOtg : kGrpAll);
        if (useLock) {
            if (st.lockHeld) { const uint32_t u = unlock_locked(hw, st); if (u != N48N_MODE_SEQ_OK) { p.lkRestoreRc = u; p.seqBad = true; } }
            const uint32_t w = wait_pending_locked(hw, st);
            if (w != N48N_MODE_SEQ_OK) { p.lkRestoreRc = w; p.seqBad = true; }
            else {
                const uint32_t l = latch_lower_locked(hw, st, st.golden[kIdxVTotal] & kVertMask, frameUs);
                if (l != N48N_MODE_SEQ_OK) { p.lkRestoreRc = l; p.seqBad = true; } else p.lkConfirmed++;
            }
        }
        if (split) {
            // 0.0.608: still blanked, the V_TOTAL / vertical geometry has just latched back to 60 Hz. SUSPECTED (not confirmed): the vertical geometry shrinking at this latch (vblank end 82 -> 38, VSTARTUP 84 -> 40)
            // while HUBP prefetch was still set up for the 120 Hz values raises the OPTC underflow bit (the first 120 Hz run flagged it after the restore; it was 0 through the whole 10 s at 120 Hz). Read, record (reported
            // only) and CLEAR it here, BEFORE the unblank, so the judged after-restore read covers only the unblank and the 3 s at 60 Hz. Linux never judges OPTC underflow on dcn401.
            const UfRead ul = uf_read(hw); p.ufOptcRsLatch |= ul.optc; (void)uf_record(hw, st, ul, kUfTransition);
            if (uf_is_bad(ul)) (void)uf_clear_locked(hw, st);
        }
        if (split) restore_group_locked(hw, st, kGrpMsa);
        if (verify_written_locked(hw, st) == 0u) break;
    }
    if (st.drrWritten) {                                            // DRR mode back to its golden value, LAST of the register restores
        const uint32_t d = drr_set_locked(hw, st, (st.ext[kExtDrr] & kDbDrrMask) >> kDbDrrShift);
        if (d == N48N_MODE_SEQ_OK) st.drrWritten = false; else { p.lkRestoreRc = d; p.seqBad = true; }
    }
    if (st.selWritten) {
        uint32_t rc = N48N_MODE_SEQ_OK;
        if (wr_field(hw, kRegGlobalCtrl2, kSelMask, st.ext[kExtSel] & kSelMask, 0u, &rc)) st.selWritten = false; else { p.lkRestoreRc = rc; p.seqBad = true; }
    }
    if (st.blanked) {                                               // the stream comes back last (a blank the apply left half-done, or this restore's own)
        if (!ranBlank) p.dioResyncStart = hw.rd(hw.ctx, kRegPixelRateCntl);
        if (wrap_unblank_locked(hw, st, true) != N48N_MODE_SEQ_OK) p.seqBad = true;
    }
    // the fields the trial touched read back as their golden; the lock request is clear
    uint32_t extBad = 0u;
    if (st.drrTouched || st.lockOn) { const uint32_t v = hw.rd(hw.ctx, kRegDbCtrl); p.lkDbAfter = v; p.lkDrrAfter = (v & kDbDrrMask) >> kDbDrrShift; }
    if (st.drrTouched) { const uint32_t v = hw.rd(hw.ctx, kRegDbCtrl); if (v == 0xFFFFFFFFu || ((v ^ st.ext[kExtDrr]) & kDbDrrMask) != 0u) extBad++; }
    if (st.selTouched) { const uint32_t v = hw.rd(hw.ctx, kRegGlobalCtrl2); if (v == 0xFFFFFFFFu || ((v ^ st.ext[kExtSel]) & kSelMask) != 0u) extBad++; }
    if (st.lockOn) {
        const uint32_t v = hw.rd(hw.ctx, kRegMasterLock);
        p.lkLockAfter = v;
        if (v == 0xFFFFFFFFu || (v & kLockReq) != 0u) { extBad++; p.lockStillHeld = true; }
    }
    p.stillBlanked = st.blanked;
    if (st.blanked) p.seqBad = true;
    st.mismatch += extBad;
    st.regsOk = st.mismatch == 0u;
    st.restoreDone = true;
}
// ---- 0.0.609: the HELD mode's shared words (all ATOMIC; no lock) ----------------------------------------------------------------------------------------------------------
inline uint32_t ld(const uint32_t &v) { return __atomic_load_n(&v, __ATOMIC_SEQ_CST); }
inline void stz(uint32_t &v, uint32_t x) { __atomic_store_n(&v, x, __ATOMIC_SEQ_CST); }
// scanAcquire's question, asked UNDER the scanout lock: may a session Acquire while a trial is busy? Only while a hold is UP and not ending, not asked to release, and the session is the owner - or the hold is a
// HANDOFF one waiting for a taker (ownerSeq 0). Session 0 never qualifies.
inline bool hold_allows_acquire(State &st, uint32_t sess) {
    if (sess == 0u || ld(st.held) == 0u || ld(st.ending) != 0u || ld(st.releaseReq) != 0u || ld(st.ownerGone) != 0u) return false;
    const uint32_t o = ld(st.ownerSeq);
    return o == sess || (o == 0u && ld(st.handoff) != 0u);
}
// After a SUCCESSFUL Acquire by `sess` under a hold: a HANDOFF hold with no owner passes to it (and stops being a handoff: its close now ENDS the hold).
inline void hold_take_owner(State &st, uint32_t sess) {
    uint32_t zero = 0u;
    if (ld(st.held) != 0u && sess != 0u && __atomic_compare_exchange_n(&st.ownerSeq, &zero, sess, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) { stz(st.handoff, 0u); __atomic_fetch_or(&st.holdFlags, (uint32_t)N48N_HOLD_FL_TOOK_OVER, __ATOMIC_SEQ_CST); }
}
// A native session closed (called by the client's close AFTER its scanout plane was put back). The owner's close ends the hold; a HANDOFF hold's issuing session's close starts the taker's window instead.
inline void hold_session_closed(const Hw &hw, State &st, uint32_t seq) {
    if (ld(st.launching) == 0u || seq == 0u || ld(st.ownerSeq) != seq) return;
    if (ld(st.handoff) != 0u) { __atomic_store_n(&st.handoffDeadlineUs, hw.now_us(hw.ctx) + (uint64_t)kHandoffMs * 1000ull, __ATOMIC_SEQ_CST); stz(st.ownerSeq, 0u); }
    else stz(st.ownerGone, 1u);
}
// ModeRelease's request (the hold thread notices within a poll). True when a hold is launching or running.
inline bool hold_request_release(State &st) { if (ld(st.launching) == 0u) return false; stz(st.releaseReq, 1u); return true; }   // latched from the claim on: a release that arrives before run_trial sets holdOn is NOT dropped
// The launcher's two steps (the kext's modeHold; the tests' rig): claim the single hold slot, then set the owner words BEFORE run_trial starts (a session that closes during the pre-hold phase is seen).
inline bool hold_launch_claim(State &st) { uint32_t zero = 0u; return __atomic_compare_exchange_n(&st.launching, &zero, 1u, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
inline void hold_launch_prepare(State &st, uint32_t sess, const HoldCfg &c) {
    stz(st.held, 0u); stz(st.ending, 0u); stz(st.ownerGone, 0u);   /* releaseReq is NOT cleared here: a release latched between the claim and this call must survive; hold_runner_done clears it */ stz(st.scanBad, 0u); stz(st.endReason, 0u); stz(st.runnerDone, 0u);
    stz(st.holdFlags, (c.flags & N48N_HOLD_F_HANDOFF) != 0u ? (uint32_t)N48N_HOLD_FL_HANDOFF : 0u);
    __atomic_store_n(&st.handoffDeadlineUs, 0ull, __ATOMIC_SEQ_CST);
    stz(st.handoff, (c.flags & N48N_HOLD_F_HANDOFF) != 0u ? 1u : 0u);
    stz(st.ownerSeq, sess);
}
// The runner thread is done (its run_trial returned): the result buffer is final. The slot is free again for the next hold.
inline void hold_runner_done(State &st) { stz(st.runnerDone, 1u); stz(st.launching, 0u); stz(st.releaseReq, 0u); }
// The FIRST step of every end of a hold: `ending` (scanAcquire refuses from now on), then the scanout plane goes back to the console and its restore is awaited. NO lock is held here (the scanout lock is taken inside).
// Idempotent (the second caller finds the plane already back). A restore that did not verify sets scanBad and rep.seqBad (the verdict becomes RESTORE and the clock hold stays up: it never releases on a bad restore).
inline void hold_begin_end(const Hw &hw, State &st) {
    if (ld(st.holdOn) == 0u) return;
    stz(st.ending, 1u);
    if (hw.scan_release == nullptr) return;
    const uint32_t rc = hw.scan_release(hw.ctx);
    if (rc != 0u) __atomic_fetch_or(&st.holdFlags, (uint32_t)N48N_HOLD_FL_SCAN_ACTED, __ATOMIC_SEQ_CST);
    if (rc == 2u) { stz(st.scanBad, 1u); hw.lock(hw.ctx); st.rep.seqBad = true; hw.unlock(hw.ctx); }
}
// dcnmode 0 / the kext stop, right after request_abort: a running hold's scanout goes back FIRST (the hold thread would do it too; this makes it hold even if that thread is slow).
inline void hold_abort_scan(const Hw &hw, State &st) { if (__atomic_load_n(&st.busy, __ATOMIC_ACQUIRE) != 0u) hold_begin_end(hw, st); }
// What ends a hold while it dwells (besides an abort, a sampled failure and the max time, which the dwell loop already knows). 0 = keep holding.
inline uint32_t hold_end_check(const Hw &hw, State &st) {
    if (ld(st.releaseReq) != 0u) return kHeRelease;
    if (ld(st.ownerGone) != 0u) return kHeOwnerClosed;
    if (ld(st.ownerSeq) == 0u && ld(st.handoff) != 0u && hw.now_us(hw.ctx) >= __atomic_load_n(&st.handoffDeadlineUs, __ATOMIC_SEQ_CST)) return kHeNoTaker;
    hw.lock(hw.ctx); const bool claimed = st.restoreClaimed; hw.unlock(hw.ctx);
    return claimed ? kHeWatchdog : kHeNone;
}

// Claim and run the restore, exactly once per trial. Returns true when THIS caller ran it (false: another party already had it).
inline bool restore_run(const Hw &hw, State &st, bool byWatchdog) {
    hold_begin_end(hw, st);   // 0.0.609: a hold: the scanout plane goes back to the console (and is verified) BEFORE the timing is restored; a no-op for every other trial
    hw.lock(hw.ctx);
    if (st.restoreClaimed) { hw.unlock(hw.ctx); return false; }
    st.restoreClaimed = true;
    if (byWatchdog) st.wdFired = true;
    restore_locked(hw, st);
    hw.unlock(hw.ctx);
    if (byWatchdog) hw.log(hw.ctx, kLogWatchdog, st.nWritten, st.mismatch, st.restoreTries, 0);
    return true;
}
// Wait (bounded) for a restore another party claimed to finish. True when it did.
inline bool restore_wait(const Hw &hw, State &st, uint32_t maxMs, uint32_t pollMs) {
    for (uint32_t waited = 0; waited <= maxMs; waited += pollMs) {
        hw.lock(hw.ctx); const bool d = st.restoreDone; hw.unlock(hw.ctx);
        if (d) return true;
        hw.sleep_ms(hw.ctx, pollMs);
    }
    return false;
}

// `dcnmode 0` / the kext stop: put every register of the golden set that differs from the copy back (reverse Linux order, read-modify-write on the mask), then verify (one more
// pass if anything still differs). Returns the number of registers that STILL differ (0 = the machine is the boot machine again, or no copy exists: then nothing is touched
// and *have = false); *restored = registers written. No trial may be running (request_abort + wait_idle first).
inline uint32_t restore_from_golden(const Hw &hw, State &st, uint32_t *restored, bool *have) {
    uint32_t nw = 0, bad = 0;
    hw.lock(hw.ctx);
    if (have) *have = st.goldenValid;
    if (st.goldenValid) {
        clear_stuck_lock_locked(hw, st);                   // 0.0.606: a stuck OTG lock request first (design P2)
        for (uint32_t pass = 0; pass < 2u; pass++) {
            // 0.0.607: after a row-120 trial the DTO group goes first (the pixel clock back before the raster), then the OTG + HUBP registers, then the MSA; otherwise the plain reverse index order of 0.0.606.
            const uint32_t nGrp = st.ever120 ? 3u : 1u;
            for (uint32_t gi = 0; gi < nGrp; gi++) {
                const uint32_t grp = st.ever120 ? (gi == 0u ? kGrpDto : gi == 1u ? kGrpOtg : kGrpMsa) : kGrpAll;
                for (uint32_t k = kCapN; k-- > 0u; ) {
                    if (grp != kGrpAll && cap_group(k) != grp) continue;
                    const uint32_t cur = hw.rd(hw.ctx, kCap[k].abs);
                    if (cur != 0xFFFFFFFFu && (cur & kCap[k].mask) == (st.golden[k] & kCap[k].mask)) continue;
                    const uint32_t nv = rmw(cur == 0xFFFFFFFFu ? st.golden[k] : cur, kCap[k].mask, st.golden[k]);
                    const bool ok = hw.wr(hw.ctx, kCap[k].abs, nv);
                    hw.log(hw.ctx, kLogRestore, kCap[k].abs, cur, nv, ok ? 1u : 0u);
                    nw++;
                }
            }
            // 0.0.606: the DRR mode and the lock select go back to their golden fields (after the kCap registers: the DRR mode is the last register restored)
            for (uint32_t e = 0; e < kExtN && st.extValid; e++) {
                if (e == kExtLock) continue;               // handled by clear_stuck_lock_locked
                const uint32_t v = hw.rd(hw.ctx, kExt[e].abs);
                if (v != 0xFFFFFFFFu && ((v ^ st.ext[e]) & kExt[e].mask) == 0u) continue;
                uint32_t rc = N48N_MODE_SEQ_OK;
                if (e == kExtDrr) (void)wait_pending_locked(hw, st);   // a latch still pending must complete before the DRR mode goes back
                (void)wr_field(hw, kExt[e].abs, kExt[e].mask, st.ext[e] & kExt[e].mask, e == kExtDrr ? kDbPendingMask : 0u, &rc);
                nw++;
            }
            bad = 0;
            for (uint32_t k = 0; k < kCapN; k++) {
                const uint32_t c = hw.rd(hw.ctx, kCap[k].abs);
                if (c == 0xFFFFFFFFu || (c & kCap[k].mask) != (st.golden[k] & kCap[k].mask)) bad++;
            }
            for (uint32_t e = 0; e < kExtN && st.extValid; e++) {
                const uint32_t c = hw.rd(hw.ctx, kExt[e].abs);
                if (c == 0xFFFFFFFFu || ((c ^ st.ext[e]) & kExt[e].mask) != 0u) bad++;
            }
            if (bad == 0u) break;
        }
        if (st.blanked) {                                  // a stream a failed trial left blanked comes back here (dcnmode 0, the kext stop)
            st.rep.dioResyncStart = hw.rd(hw.ctx, kRegPixelRateCntl);
            (void)wrap_unblank_locked(hw, st, true);
            if (st.blanked) bad++;
        }
    }
    hw.unlock(hw.ctx);
    if (restored) *restored = nw;
    return bad;
}

// Ask a running trial to end (it restores and answers ABORT); wait (bounded) until none runs. True when idle.
inline void request_abort(const Hw &hw, State &st) { hw.lock(hw.ctx); if (__atomic_load_n(&st.busy, __ATOMIC_ACQUIRE) != 0u) st.abortReq = true; hw.unlock(hw.ctx); }
inline bool wait_idle(const Hw &hw, State &st, uint32_t maxMs, uint32_t pollMs) {
    for (uint32_t waited = 0; waited <= maxMs; waited += pollMs) {
        if (__atomic_load_n(&st.busy, __ATOMIC_ACQUIRE) == 0u) return true;
        hw.sleep_ms(hw.ctx, pollMs);
    }
    return __atomic_load_n(&st.busy, __ATOMIC_ACQUIRE) == 0u;
}

// `dcnmode 0` (0.0.606): the golden restore above, then - when the display back end is unhealthy (stream not active, DIO_FIFO_ERROR, the steer FIFO overflow flag or the DIG FIFO error bits) - the
// DP1 resync, then 500 ms and a second look. Growth of DIO_ERROR_COUNT is REPORTED against the count just before the resync (the count is cumulative; the baseline is whatever it was), never required 0.
// Nothing is touched when no golden copy exists.
struct Recover { uint32_t restored, bad, rc, dioGrowth; bool have, healthyBefore, resynced, healthyAfter; };
inline bool health_now(const Hw &hw, uint32_t *prcOut) {
    const uint32_t prc = hw.rd(hw.ctx, kRegPixelRateCntl), ds = hw.rd(hw.ctx, kRegDpStream[kLitDp]), sf = hw.rd(hw.ctx, kRegDp1Steer), dg = hw.rd(hw.ctx, kRegDigFifoCtrl0);
    if (prcOut) *prcOut = prc;
    return stream_active(ds) && !health_bad(prc, sf, dg);
}
inline void emergency_recover(const Hw &hw, State &st, Recover *o) {
    *o = Recover{};
    o->bad = restore_from_golden(hw, st, &o->restored, &o->have);
    if (!o->have) return;
    uint32_t prc0 = 0u;
    o->healthyBefore = health_now(hw, &prc0);
    if (o->healthyBefore) { o->healthyAfter = true; return; }
    hw.lock(hw.ctx);
    st.rep.dioResyncStart = prc0;
    o->rc = resync_locked(hw, st, true);
    hw.unlock(hw.ctx);
    o->resynced = true;
    hw.sleep_ms(hw.ctx, 500u);
    uint32_t prc1 = 0u;
    o->healthyAfter = health_now(hw, &prc1);
    o->dioGrowth = err_count(prc1) - err_count(prc0);
}

// ---- the watchdog thread's body ---------------------------------------------------------------------------------------------------------------
// One per trial (started before the first write). Polls every pollMs; ends when its trial ends. When the deadline (first write + settle + measure + dwell + slack) has passed or the
// trial thread's heartbeat is stale, it claims and runs THE restore and ends. If the trial thread already claimed the restore it is alive and finishing: keep polling (bounded).
inline void watchdog_body(const Hw &hw, State &st, const Timing &tm, uint32_t myId) {
    for (;;) {
        hw.sleep_ms(hw.ctx, tm.pollMs);
        hw.lock(hw.ctx);
        const bool finished = st.doneId == myId || st.trialId != myId;
        const uint64_t now = hw.now_us(hw.ctx);
        const bool due = !finished && wd_due(now, st.deadlineUs, st.beatUs, (uint64_t)tm.staleMs * 1000ull);
        const bool giveUp = !finished && now >= st.deadlineUs + (uint64_t)tm.waitRestoreMs * 2000ull;
        hw.unlock(hw.ctx);
        if (finished || giveUp) return;
        if (due && restore_run(hw, st, true)) return;
    }
}

// ---- measuring ------------------------------------------------------------------------------------------------------------------------------------
struct Meas { uint32_t frames, ms; bool stalled, aborted, readFail; bool stopped = false; };
// The OTG frame counter over `ms` (sliced, heartbeating). Frames and elapsed ms are taken between the FIRST and the LAST counter read, so the rate has no idle margin in it.
// 0.0.606: `slice(nowUs)` runs after every slice (the underflow / FIFO / stream sampler); true ends the window early (m.stopped).
template <class F> inline Meas measure_ex(const Hw &hw, State &st, const Timing &tm, uint32_t ms, bool honourAbort, F &&slice) {
    Meas m{ 0, 0, false, false, false };
    uint32_t f0 = 0, last = 0;
    if (!hw.frame(hw.ctx, &f0)) { m.readFail = true; return m; }
    const uint64_t t0 = hw.now_us(hw.ctx);
    uint64_t tLast = t0, tAdv = t0;
    last = f0;
    while (tLast - t0 < (uint64_t)ms * 1000ull) {
        hw.sleep_ms(hw.ctx, tm.pollMs);
        beat(hw, st);
        uint32_t fc = 0;
        if (!hw.frame(hw.ctx, &fc)) { m.readFail = true; break; }
        tLast = hw.now_us(hw.ctx);
        if (n48dal::frame_delta(last, fc) != 0u) tAdv = tLast;
        last = fc;
        if (tLast - tAdv >= (uint64_t)tm.stallMs * 1000ull) { m.stalled = true; break; }
        if (honourAbort && aborted(hw, st)) { m.aborted = true; break; }
        if (slice(tLast)) { m.stopped = true; break; }
    }
    m.frames = n48dal::frame_delta(f0, last);
    m.ms = (uint32_t)((tLast - t0) / 1000ull);
    return m;
}
inline Meas measure(const Hw &hw, State &st, const Timing &tm, uint32_t ms, bool honourAbort = true) { return measure_ex(hw, st, tm, ms, honourAbort, [](uint64_t) { return false; }); }
// 0.0.607, P4 callers `dcnmode 0` (verifyRateMs = 3000: the 60 Hz rate must read back) and the kext stop (verifyRateMs = 0: registers and back-end health only): after the golden restore (emergency_recover /
// restore_from_golden), a clock hold that is still HELD (a trial thread that died with it) is released - but only when the restore VERIFIED (registers, a healthy back end, and for dcnmode 0 the 60 Hz
// rate); otherwise restoreBad = true and the hold goes STUCK with no message. `idle` = no trial is running (a running trial owns its hold). Returns RelRc (kRlNotHeld when there was nothing to do).
// Never called with Hw.lock held (it sleeps on the PMFW).
inline uint32_t emergency_release(const Hw &hw, State &st, const Timing &tm, bool idle, uint32_t regsStillDiffer, bool healthy, uint32_t verifyRateMs) {
    if (!idle || hw.hold_state == nullptr || hw.hold_release == nullptr) return n48dal::kRlNotHeld;
    if (hw.hold_state(hw.ctx) != n48dal::kHsHeld) return n48dal::kRlNotHeld;
    bool bad = regsStillDiffer != 0u || !healthy;
    if (!bad && verifyRateMs != 0u) {
        const Meas a = measure(hw, st, tm, verifyRateMs, false);
        bad = a.readFail || a.stalled || !rate_within(measured_mhz(a.frames, a.ms), expect_mhz(row60()), tm.baseTolPermille);
    }
    return hw.hold_release(hw.ctx, bad, &st.rep.hold);
}
// The health sampler of the rate window and the dwell: every sampleMs it reads the DIO FIFO, the DP1 stream, the steer and DIG FIFO flags and the two underflow registers. `now()` samples at once.
// A failure flag is sticky. Reads only: no lock, no write.
struct Sampler {
    const Hw *hw; State *st; uint32_t errBase; uint64_t nextUs, periodUs;
    bool uf = false, fifo = false, stream = false;
    uint32_t prc = 0u, dp = 0u;
    bool clkOn = false, clk = false; uint32_t needDisp = 0u, needDpp = 0u;   // 0.0.607: while the clock hold is up the DFS readback must stay at the row's need (an unreadable readback counts as lost)
    void now() {
        if (clkOn) {
            n48dal::Decoded d{};
            const bool rd = hw->clocks != nullptr && hw->clocks(hw->ctx, &d);
            Rep &p = st->rep;
            p.hSamples++;
            if (rd && d.ok) { if (p.hDfsMinDisp == 0u || d.dispKhz < p.hDfsMinDisp) p.hDfsMinDisp = d.dispKhz; if (p.hDfsMinDpp == 0u || d.dppKhz < p.hDfsMinDpp) p.hDfsMinDpp = d.dppKhz; }
            if (!rd || !n48dal::hold_clocks_ok(d, needDisp, needDpp)) { clk = true; p.hLost++; }
        }
        prc = hw->rd(hw->ctx, kRegPixelRateCntl); dp = hw->rd(hw->ctx, kRegDpStream[kLitDp]);
        const uint32_t sf = hw->rd(hw->ctx, kRegDp1Steer), dg = hw->rd(hw->ctx, kRegDigFifoCtrl0);
        if (fifo_err(prc) != 0u || err_count(prc) > errBase || steer_bad(sf) || dig_bad(dg)) fifo = true;
        if (!stream_active(dp)) stream = true;
        if (uf_record(*hw, *st, uf_read(*hw), kUfJudged)) uf = true;
    }
    bool tick(uint64_t nowUs) { if (nowUs < nextUs) return false; nextUs += periodUs; now(); return uf || fifo || stream || clk; }
};

// ---- the trial -------------------------------------------------------------------------------------------------------------------------------------
namespace detail {
inline void zero(n48n_mode_result *r) { for (uint32_t i = 0; i < sizeof(*r) / 4u; i++) reinterpret_cast<uint32_t *>(r)[i] = 0u; }
inline void deny(const Hw &hw, State &st, n48n_mode_result *r, uint32_t why) {
    r->verdict = N48N_MODE_V_DENIED; r->deny = why; r->fail_phase = 1u;
    hw.lock(hw.ctx);
    r->latched = st.latched ? 1u : 0u;
    if (st.latched) r->flags |= N48N_MODE_F_LATCHED;
    if (st.step50Passed) r->flags |= N48N_MODE_F_STEP_DOWN_DONE;
    if (st.goldenAtBind) r->flags |= N48N_MODE_F_GOLDEN_AT_BIND;
    hw.unlock(hw.ctx);
    hw.log(hw.ctx, kLogDeny, why, r->row, 0, 0);
}
// A Pre with every hardware-dependent field in its "fine" state: deny_check on it can only fire the reasons that need no hardware.
inline Pre pre_cheap(uint32_t row, uint32_t dwellMs, bool latched, bool gateOk, bool armed, bool step50, bool row120Off) {
    Pre p{};
    p.row120Off = row120Off;
    p.row = row; p.dwellMs = dwellMs; p.latched = latched; p.gateOk = gateOk; p.armed = armed; p.step50Passed = step50;
    p.goldenValid = p.otgOk = p.baselineOk = p.streamOk = p.driftFree = p.clocksOk = p.framesOk = true;
    return p;
}
}

// One trial. Fills *r completely; never returns an error (the verdict word says how it went). Sleeps up to ~preMeasure + settle + measure + dwell + measure + restore (plus, with the DP1 resync,
// ~1-2 s per resync). `st` must outlive the watchdog thread it starts (the kext's is a file-scope static). `tflags` = N48N_MODE_TF_*.
namespace detail {
// The extension block of the result (ABI 1.4) from the trial's report. Caller holds the lock or is the only thread left.
inline void fill_ext(n48n_mode_result *r, const State &st) {
    const Rep &p = st.rep;
    n48n_mode_ext &e = r->ext;
    e.uf_hubp_or = p.ufHubpOr; e.uf_optc_or = p.ufOptcOr;
    e.uf_hubp_max = p.ufHubpMax; e.uf_timeout_max = p.ufTimeoutMax; e.uf_samples = p.ufSamples; e.uf_clears = p.ufClears;
    e.uf_clear_refused = p.ufClearRefused; e.uf_clear_stuck = p.ufClearStuck; e.uf_first_ms = p.ufFirstMs;
    e.rs_runs = p.rsRuns; e.rs_attempts = p.rsAttempts; e.rs_rc_first = p.rsRcFirst; e.rs_rc_last = p.rsRcLast; e.rs_blank_us = p.rsBlankUs; e.rs_unblank_us = p.rsUnblankUs;
    e.rs_dio_errs = p.rsDioErrs; e.rs_stream_after = p.rsStreamAfter; e.rs_steer_after = p.rsSteerAfter; e.rs_dig_after = p.rsDigAfter; e.rs_restore_rc = p.rsRestoreRc; e.rs_restore_dio_errs = p.rsRestoreDioErrs;
    e.lk_rc = p.lkRc; e.lk_wait_us = p.lkWaitUs; e.lk_pending_seen = p.lkPendingSeen; e.lk_vert_max = p.lkVertMax; e.lk_vert_samples = p.lkVertSamples;
    e.lk_db_before = p.lkDbBefore; e.lk_db_during = p.lkDbDuring; e.lk_db_after = p.lkDbAfter; e.lk_lock_after = p.lkLockAfter; e.lk_sel_before = p.lkSelBefore; e.lk_pipe_before = p.lkPipeBefore;
    e.lk_cleared = p.lkCleared; e.lk_restore_rc = p.lkRestoreRc; e.lk_confirmed = p.lkConfirmed; e.lk_drr_before = p.lkDrrBefore; e.lk_drr_during = p.lkDrrDuring; e.lk_drr_after = p.lkDrrAfter;
    if (p.ufSeen || p.ufAfterBad) r->flags |= N48N_MODE_F_UNDERFLOW_SEEN;
    if (p.ufUnread) r->flags |= N48N_MODE_F_UNDERFLOW_UNREAD;
    if (p.ufClearRefused != 0u) r->flags |= N48N_MODE_F_UF_CLEAR_REFUSED;
    if (p.rsRuns != 0u) r->flags |= N48N_MODE_F_RESYNC_RUN;
    if (st.selTouched) r->flags |= N48N_MODE_F_LOCK_USED;
    if (p.latchConfirmed) r->flags |= N48N_MODE_F_LATCH_CONFIRMED;
    if (p.lockHeldProven) r->flags |= N48N_MODE_F_LOCK_HELD_PROVEN;
    if (p.ufClears != 0u) r->flags |= N48N_MODE_F_UF_CLEARED;
    e.lk_hold_max = p.lkHoldMax;
    r->uf_optc_rs_dto = p.ufOptcRsDto; r->uf_optc_rs_latch = p.ufOptcRsLatch; r->uf_optc_ap_trans = p.ufOptcApTrans;   // 0.0.608
    r->dio_hold_errs = p.dioHoldErrs; r->dio_post_unblank_errs = p.dioPostUnblankErrs; r->lk_hold_rc = p.lkHoldRc; r->lk_hold_ran = p.lkHoldRan;
    if (p.stillBlanked || st.blanked) r->flags |= N48N_MODE_F_STILL_BLANKED;
    if (p.lockStillHeld) r->flags |= N48N_MODE_F_LOCK_STILL_HELD;
    // 0.0.607: the clock hold report
    e.hold_state = p.hold.state; e.hold_rc = p.hold.rc; e.hold_raise_ms = p.hold.raiseMs; e.hold_rel_rc = p.hold.relRc;
    e.hold_khz_raised[0] = p.hold.khzRaised[0]; e.hold_khz_raised[1] = p.hold.khzRaised[1]; e.hold_khz_released[0] = p.hold.khzReleased[0]; e.hold_khz_released[1] = p.hold.khzReleased[1];
    e.hold_dfs_min[0] = p.hDfsMinDisp; e.hold_dfs_min[1] = p.hDfsMinDpp; e.hold_samples = p.hSamples; e.hold_lost = p.hLost;
    e.hold_pre = p.hold.pre; e.hold_flags = p.hold.flags; e.hold_msgs = p.hold.nMsg; e.hold_release_ms = p.hold.relMs;
    if ((p.hold.flags & n48dal::kHfRaised) != 0u) r->flags |= N48N_MODE_F_HOLD_HELD;
    if ((p.hold.flags & n48dal::kHfReleased) != 0u) r->flags |= N48N_MODE_F_HOLD_RELEASED;
    if ((p.hold.flags & n48dal::kHfStuck) != 0u) r->flags |= N48N_MODE_F_HOLD_STUCK;
}
}

inline void run_trial(const Hw &hw, State &st, const Timing &tm, uint32_t row, uint32_t dwellMs, uint32_t tflags, n48n_mode_result *r, const HoldCfg *hc = nullptr) {
    detail::zero(r);
    r->row = row; r->dwell_req_ms = dwellMs; r->settle_ms = tm.settleMs; r->measure_ms = row == 2u ? tm.vtMeasureMs : tm.measureMs;
    r->flags = (row == 50u ? N48N_MODE_F_DLG_LIVE : 0u);
    r->golden_n = kCapN;
    r->ext.tflags = tflags;
    const RowInfo ri = row_info(row);
    const bool wrap = row_wrap(row, tflags), lockRow = row_lock(row), split = row_split(row);
    const bool flagsOk = (tflags & ~N48N_MODE_TF_MASK) == 0u && (tflags == 0u || row != 120u) && (hc == nullptr || row == 120u);   // 0.0.609: a hold is row 120 only
    const bool needResync = row == 2u || row == 120u || (row == 50u && (tflags & N48N_MODE_TF_RESYNC) != 0u);
    const uint32_t mms = row == 2u ? tm.vtMeasureMs : tm.measureMs;         // the rate windows of this row
    const uint32_t tol = row == 2u ? tm.vtTolPermille : tm.tolPermille;     // row 2's 59.15 Hz is only 1.35 % from 59.95 Hz: a 1 % window cannot tell a latched V_TOTAL from a stuck one
    r->ext.lk_expect_lines = ri.vTotal;

    // ---- stage A: the reasons that need no hardware (row, dwell, latch, gate, display layer bound) ----
    hw.lock(hw.ctx); const bool latched0 = st.latched, step50 = st.step50Passed, resync0 = st.resyncPassed, row2p0 = st.row2Proven; hw.unlock(hw.ctx);
    uint32_t why = flagsOk ? deny_check(detail::pre_cheap(row, hc != nullptr ? 0u : dwellMs, latched0, hw.gate_ok(hw.ctx), hw.armed(hw.ctx), true, !tm.row120)) : N48N_MODE_D_BAD_ROW;   // the step-down rules wait for stage B: clocks are answered first
    if (why == 0u && hc != nullptr && (!hold_cfg_ok(*hc) || hc->maxMs != dwellMs)) why = N48N_MODE_D_BAD_DWELL;   // 0.0.609: the hold bound (the dwell argument IS the max hold time)
    if (why != 0u) { detail::deny(hw, st, r, why); return; }
    // ---- busy: one trial at a time. Taken BEFORE the plane check, so scanAcquire (which reads it under the scanout lock) cannot slip in between that check and the writes. ----
    if (__atomic_exchange_n(&st.busy, 1u, __ATOMIC_SEQ_CST) != 0u) { detail::deny(hw, st, r, N48N_MODE_D_BUSY); return; }   // not ours: leave it set
    hw.lock(hw.ctx);
    st.trialId++;
    const uint32_t myId = st.trialId;
    st.nWritten = 0u; st.restoreClaimed = st.restoreDone = st.abortReq = st.wdFired = st.regsOk = st.writeFailed = false;
    st.restoreTries = st.mismatch = st.badAbs = st.badHave = st.badWant = 0u; st.nPlan = 0u;
    st.drrWritten = st.selWritten = st.drrTouched = st.selTouched = st.wrapOn = st.lockOn = st.splitOn = false;   // (blanked / lockHeld are NOT reset: a trial that starts with either set is denied as drift)
    st.rep = Rep{};
    st.deadlineUs = st.beatUs = hw.now_us(hw.ctx);
    hw.unlock(hw.ctx);
    if (hc != nullptr) { stz(st.ending, 0u); stz(st.scanBad, 0u); stz(st.endReason, 0u); stz(st.holdOn, 1u); }   // 0.0.609 (releaseReq / ownerGone / ownerSeq / handoff were set by the launcher BEFORE this call)

    // ---- stage B: the hardware-dependent reasons, in the ABI's order ----
    Pre p = detail::pre_cheap(row, hc != nullptr ? 0u : dwellMs, false, true, true, step50, !tm.row120);   // 0.0.609: a hold's bound is checked against kHoldMaxMs in stage A
    p.needResync = needResync; p.resyncPassed = resync0;
    if (split) {                                          // 0.0.607: row 120 raises its own clocks; it needs the row-2 lock-hold proof and the hold available
        p.clockHold = true; p.needRow2 = true; p.row2Proven = row2p0;
        p.holdOk = hw.hold_pre != nullptr && hw.hold_raise != nullptr && hw.hold_release != nullptr && hw.hold_state != nullptr;
        st.rep.hold.pre = p.holdOk ? hw.hold_pre(hw.ctx) : n48dal::kHpLevel;
        if (st.rep.hold.pre != n48dal::kHpOk) p.holdOk = false;
    }
    p.planeAcquired = hw.plane_acquired(hw.ctx);
    p.inUse = hw.in_use(hw.ctx);
    (void)golden_take(hw, st, false);                      // read-only; a no-op when the bind already took it
    hw.lock(hw.ctx); p.goldenValid = st.goldenValid; hw.unlock(hw.ctx);
    p.otgOk = hw.otg_ok(hw.ctx);
    const bool liveOk = capture_read(hw, st.live);
    const uint32_t prc = hw.rd(hw.ctx, kRegPixelRateCntl);
    const uint32_t steer0 = hw.rd(hw.ctx, kRegDp1Steer), dig0 = hw.rd(hw.ctx, kRegDigFifoCtrl0);
    p.baselineOk = liveOk && baseline_ok(st.live) && !health_bad(prc, steer0, dig0) && (prc & kDtoEnableBit) != 0u;   // a FIFO already unhealthy before the trial would make the post-restore check meaningless
    // P1: the underflow baseline (before any write of the trial): it must read clean
    UfRead u0 = uf_read(hw);
    r->ext.uf_hubp_before = u0.hubp; r->ext.uf_optc_before = u0.optc;
    const bool ufBad0 = uf_is_bad(u0);      // the clear (P1b) is a register write: it waits until every other reason to deny has been answered (below)
    // P2: the OTG lock must be idle and not bypassed (rows that take it)
    if (lockRow) {
        const uint32_t dbv = hw.rd(hw.ctx, kRegDbCtrl), lkv = hw.rd(hw.ctx, kRegMasterLock), selv = hw.rd(hw.ctx, kRegGlobalCtrl2), pipev = hw.rd(hw.ctx, kRegPipeUpd);
        p.lockOk = lock_usable(dbv, lkv, selv, pipev);
        st.rep.lkDbBefore = dbv; st.rep.lkSelBefore = selv; st.rep.lkPipeBefore = pipev; st.rep.lkDrrBefore = (dbv & kDbDrrMask) >> kDbDrrShift;
    }
    const uint32_t dpStream = hw.rd(hw.ctx, kRegDpStream[kLitDp]);
    uint32_t dpOther = 0u;
    for (uint32_t d = 0; d < 4u; d++) { if (d == kLitDp) continue; const uint32_t v = hw.rd(hw.ctx, kRegDpStream[d]); if (v != 0xFFFFFFFFu && (v & 1u) != 0u) dpOther++; }
    p.streamOk = stream_active(dpStream) && dpOther == 0u;
    hw.lock(hw.ctx);
    uint32_t firstDrift = 0;
    p.driftFree = liveOk && st.goldenValid && drift_count(ri, st.live, st.golden, &firstDrift) == 0u && !st.blanked && !st.lockHeld;   // a stream left blanked or a lock left held by an earlier failure: run `dcnmode 0` first
    hw.unlock(hw.ctx);
    n48dal::Decoded clk{};
    const bool clkRead = hw.clocks(hw.ctx, &clk);
    p.clocksOk = clkRead && clocks_ok(ri, clk);
    r->disp_khz = clk.dispKhz; r->dpp_khz = clk.dppKhz; r->need_disp_khz = ri.needDispKhz; r->need_dpp_khz = ri.needDppKhz;
    r->pix_khz_expect = (uint32_t)(dto_pixclk_hz(ri.dtoInt, ri.modulo, ri.phase) / 1000ull);
    hw.lock(hw.ctx);
    st.nPlan = plan_build(ri, st.live, st.plan, &r->nskip);
    p.nothing = st.nPlan == 0u && row != 1u;              // row 1 (the resync) writes no register of the set by design
    hw.unlock(hw.ctx);
    why = deny_check(p);
    if (uf::unreadable(u0.hubp) || uf::unreadable(u0.optc)) st.rep.ufUnread = true;
    r->fifo_before = fifo_err(prc); r->errcnt_before = err_count(prc); r->stream_before = dpStream; r->bad_abs = firstDrift;
    if (why == 0u) {
        // The 60 Hz baseline rate: the counter must be running at about the census rate before anything is written.
        const Meas b = measure(hw, st, tm, tm.preMeasureMs);
        r->rate_before_mhz = (uint32_t)measured_mhz(b.frames, b.ms);
        if (b.readFail || b.stalled || b.aborted || !rate_within(r->rate_before_mhz, expect_mhz(row60()), tm.baseTolPermille)) { p.framesOk = false; why = deny_check(p); }
    }
    if (why == 0u && ufBad0) {                            // P1b: a set underflow status is cleared ONCE, and only when nothing else denies; it must then read clean
        hw.lock(hw.ctx); (void)uf_clear_locked(hw, st); hw.unlock(hw.ctx);
        u0 = uf_read(hw);
        if (uf_is_bad(u0)) why = N48N_MODE_D_UNDERFLOW;
        if (uf::unreadable(u0.hubp) || uf::unreadable(u0.optc)) st.rep.ufUnread = true;
    }
    // ---- P4: the clock hold (row 120): raised BEFORE the watchdog and the first write, outside every lock (it sleeps on the PMFW). A failed raise has unwound itself: the trial is DENIED (HOLD). ----
    bool holdOn = false;
    if (why == 0u && split) {
        n48dal::Decoded after{};
        const uint32_t hrc = hw.hold_raise(hw.ctx, ri.needDispKhz, ri.needDppKhz, &st.rep.hold, &after);
        if (hrc != 0u) why = N48N_MODE_D_HOLD;
        else {
            holdOn = true; r->disp_khz = after.dispKhz; r->dpp_khz = after.dppKhz;
            // 0.0.608: DIO attribution: the error count right after the raise (growth since the pre-trial read = the clock change's share), the base of dio_post_unblank_errs. Reported, not judged.
            const uint32_t prcH = hw.rd(hw.ctx, kRegPixelRateCntl);
            hw.lock(hw.ctx); st.rep.dioHoldBase = prcH; st.rep.dioHoldErrs = err_count(prcH) - err_count(prc); hw.unlock(hw.ctx);
        }
    }
    if (why == 0u && holdOn && aborted(hw, st)) why = N48N_MODE_D_ABORTED;   // opus review S4: an abort that arrived during the raise: the hold comes straight off (deny path below), nothing was written
    if (why == 0u) {
        // ---- the watchdog goes up BEFORE the first write ----
        hw.lock(hw.ctx);
        st.deadlineUs = hw.now_us(hw.ctx) + (uint64_t)wd_window_ms(tm, dwellMs + (hc != nullptr ? kHoldEndSlackMs : 0u), wrap, mms) * 1000ull;
        st.beatUs = hw.now_us(hw.ctx);
        hw.unlock(hw.ctx);
        if (!hw.start_watchdog(hw.ctx, myId)) why = N48N_MODE_D_WATCHDOG;
    }
    if (why != 0u) {
        if (holdOn) (void)hw.hold_release(hw.ctx, false, &st.rep.hold);   // nothing was written and the display is at 60 Hz: the hold comes straight off
        detail::deny(hw, st, r, why);
        hw.lock(hw.ctx); detail::fill_ext(r, st); st.doneId = myId; hw.unlock(hw.ctx);
        stz(st.holdOn, 0u);   // 0.0.609
        __atomic_store_n(&st.busy, 0u, __ATOMIC_RELEASE);
        return;
    }
    if (st.goldenAtBind) r->flags |= N48N_MODE_F_GOLDEN_AT_BIND;
    r->dto_modulo = st.live[kIdxDtoModulo]; r->dto_phase[0] = st.live[kIdxDtoPhase];
    r->vtotal_reg[0] = st.live[kIdxVTotal]; r->htotal_reg = st.live[kIdxHTotal];

    Obs o{ false, false, false, false, false, false, false, false };
    uint32_t phase = 2u, failPhase = 0u;
    auto anyFail = [&o]() { return o.writeFailed || o.aborted || o.stalled || o.streamLost || o.fifoBad || o.rateBad || o.underflow || o.lockFail || o.resyncFail || o.clockLost; };
    // ---- WRITE (phase 2): Linux's order. Each index is recorded before its write is issued; all under ONE hold of the lock, so the watchdog can never interleave with the writes (the DP1 resync and the OTG
    // lock sleep inside it, each wait bounded; the watchdog's window carries the resync's time). ----
    hw.lock(hw.ctx);
    st.everWrote = true; st.wrapOn = wrap; st.lockOn = lockRow; st.splitOn = split; if (split) st.ever120 = true;
    st.rep.firstWriteUs = hw.now_us(hw.ctx);
    const Apply ap = apply_locked(hw, st, ri, wrap, lockRow, r);
    o.writeFailed = ap.writeFailed; o.lockFail = ap.lockFail; o.resyncFail = ap.resyncFail;
    st.beatUs = hw.now_us(hw.ctx);
    hw.unlock(hw.ctx);
    r->flags |= N48N_MODE_F_WROTE;
    if (anyFail()) failPhase = 2u;

    uint32_t prcT = prc, dpT = dpStream, prcS = prc;
    if (!(o.writeFailed || o.lockFail || o.resyncFail)) {
        // ---- SETTLE (3) ----
        phase = 3u;
        for (uint32_t w = 0; w < tm.settleMs && !o.aborted; w += tm.pollMs) {
            hw.sleep_ms(hw.ctx, tm.pollMs); beat(hw, st);
            if (aborted(hw, st)) o.aborted = true;
        }
        if (!failPhase && anyFail()) failPhase = phase;
        // A FRESH DIO baseline after the settle: the write transition's errors are reported (dio_write_errs), only growth during measure + dwell is judged.
        prcS = hw.rd(hw.ctx, kRegPixelRateCntl);
        r->dio_write_errs = err_count(prcS) - err_count(prc);
        prcT = prcS;
        // P1: the underflow status after the settle is REPORTED (the write transition may blip), then cleared: only new events are judged from here.
        {
            const UfRead us = uf_read(hw);
            r->ext.uf_hubp_settle = us.hubp; r->ext.uf_optc_settle = us.optc;
            hw.lock(hw.ctx);
            (void)uf_record(hw, st, us, split ? kUfJudged : kUfSettle);   // 0.0.608: row 120's settle read comes AFTER the unblank on a visible picture (the blanked transition was cleared): it is judged. Row 50 / 2 / 1 unchanged.
            if (uf_is_bad(us)) (void)uf_clear_locked(hw, st);
            hw.unlock(hw.ctx);
        }
        const uint64_t samplePeriod = (uint64_t)tm.sampleMs * 1000ull;
        Sampler smp{ &hw, &st, err_count(prcS), hw.now_us(hw.ctx) + samplePeriod, samplePeriod };
        smp.prc = prcS; smp.dp = dpStream;
        if (holdOn) { smp.clkOn = true; smp.needDisp = ri.needDispKhz; smp.needDpp = ri.needDppKhz; }   // 0.0.607: the DFS readback is sampled with the health registers
        // ---- MEASURE (4) ----
        if (!o.aborted) {
            phase = 4u;
            const Meas m = measure_ex(hw, st, tm, mms, true, [&smp](uint64_t nowUs) { return smp.tick(nowUs); });
            r->frames_trial = m.frames; r->ms_trial = m.ms; r->rate_trial_mhz = (uint32_t)measured_mhz(m.frames, m.ms);
            r->rate_expect_mhz = (uint32_t)expect_mhz(ri);
            if (m.aborted) o.aborted = true;
            else if (m.stalled || m.readFail) o.stalled = true;
            else if (m.stopped) { /* a sampled failure ended the window: it is judged below, the partial window's rate is not */ }
            else if (rate_within(r->rate_trial_mhz, r->rate_expect_mhz, tol)) r->flags |= N48N_MODE_F_RATE_TRIAL_OK;
            else o.rateBad = true;
            smp.now();
            o.fifoBad = o.fifoBad || smp.fifo; o.streamLost = o.streamLost || smp.stream; o.underflow = o.underflow || smp.uf; o.clockLost = o.clockLost || smp.clk;
            r->vtotal_reg[1] = hw.rd(hw.ctx, kCap[kIdxVTotal].abs); r->dto_phase[1] = hw.rd(hw.ctx, kCap[kIdxDtoPhase].abs);
            if (!failPhase && anyFail()) failPhase = phase;
        }
        // ---- DWELL (5): nothing to hold if the trial already failed ----
        phase = 5u;
        // 0.0.609 (HELD): only a CLEAN row-120 trial so far enters the hold: no failure, no abort, the rate window inside the tolerance, and no underflow judged at the settle read. The dwell below is then the hold:
        // the entry snapshot is published (`held` = 1: the caller returns, scanAcquire is allowed) and the loop ends on hold_end_check / the max time / an abort / a sampled failure.
        bool holdEnter = false;
        if (hc != nullptr && !anyFail() && (r->flags & N48N_MODE_F_RATE_TRIAL_OK) != 0u) {
            hw.lock(hw.ctx); const bool ufJudged = st.rep.ufSeen; hw.unlock(hw.ctx);
            holdEnter = !ufJudged;
        }
        uint32_t hend = 0u;
        if (holdEnter) {
            hw.lock(hw.ctx);
            st.heldEntry = *r; st.heldEntry.verdict = N48N_MODE_V_HELD; st.heldEntry.fail_phase = 0u; st.heldEntry.latched = st.latched ? 1u : 0u;
            detail::fill_ext(&st.heldEntry, st);
            st.heldEntry.hold_flags = ld(st.holdFlags) | N48N_HOLD_FL_ENTERED; st.heldEntry.hold_end = 0u;
            hw.unlock(hw.ctx);
            stz(st.held, 1u);
        }
        const uint64_t td0 = hw.now_us(hw.ctx);
        uint32_t lastFc = 0; uint64_t tAdv = td0;
        const bool haveFc = hw.frame(hw.ctx, &lastFc);
        while (!anyFail() && (hc == nullptr || holdEnter) && hw.now_us(hw.ctx) - td0 < (uint64_t)dwellMs * 1000ull) {
            hw.sleep_ms(hw.ctx, tm.pollMs); beat(hw, st);
            if (aborted(hw, st)) { o.aborted = true; break; }
            const uint64_t now = hw.now_us(hw.ctx);
            uint32_t fc = 0;
            if (haveFc && hw.frame(hw.ctx, &fc)) { if (n48dal::frame_delta(lastFc, fc) != 0u) tAdv = now; lastFc = fc; }
            if (now - tAdv >= (uint64_t)tm.stallMs * 1000ull) { o.stalled = true; break; }
            if (smp.tick(now)) { o.fifoBad = o.fifoBad || smp.fifo; o.streamLost = o.streamLost || smp.stream; o.underflow = o.underflow || smp.uf; o.clockLost = o.clockLost || smp.clk; }
            if (holdEnter && !anyFail()) { hend = hold_end_check(hw, st); if (hend != 0u) break; }   // 0.0.609
        }
        r->dwell_done_ms = (uint32_t)((hw.now_us(hw.ctx) - td0) / 1000ull);
        if (hc != nullptr) { stz(st.endReason, hend != 0u ? hend : o.aborted ? kHeAbort : anyFail() ? kHeFailure : holdEnter ? kHeMaxTime : 0u); }   // 0.0.609: why the hold ended (0 = it never entered)
        if (!failPhase && anyFail()) failPhase = phase;
        prcT = smp.prc; dpT = smp.dp;
    }
    r->fifo_after = fifo_err(prcT); r->errcnt_after = err_count(prcT); r->stream_trial = dpT;
    r->dio_trial_errs = err_count(prcT) - err_count(prcS);

    // ---- RESTORE (6): the same function the watchdog runs; whoever claims it does it, the other waits ----
    phase = 6u;
    const uint32_t prcPre = hw.rd(hw.ctx, kRegPixelRateCntl);   // the DIO count just before the restore transition
    if (!restore_run(hw, st, false)) (void)restore_wait(hw, st, tm.waitRestoreMs, tm.pollMs);
    hw.lock(hw.ctx);
    const bool restoreDone = st.restoreDone, regsOk = st.regsOk;
    o.wdFired = st.wdFired;
    r->mismatch = st.mismatch; r->bad_abs = st.badAbs; r->bad_have = st.badHave; r->bad_want = st.badWant; r->restore_tries = st.restoreTries; r->wd_fired = st.wdFired ? 1u : 0u;
    const bool repSeqBad = st.rep.seqBad, repBlanked = st.rep.stillBlanked || st.blanked, repLockHeld = st.rep.lockStillHeld;
    const uint32_t dioBase = st.rep.restoreResynced ? st.rep.dioBaseAfterRestore : prcPre;   // with a resync the restore's FIFO health is judged from the END of it
    if (st.rep.ufSeen) o.underflow = true;                                                  // (the restore entry took one more look)
    hw.unlock(hw.ctx);
    if (o.wdFired) r->flags |= N48N_MODE_F_WATCHDOG;
    if (regsOk) r->flags |= N48N_MODE_F_REGS_OK;
    // The restore is done: an abort request has been served. Remember it, then CLEAR it so the VERIFY window below is not cut short (it also ignores a request that arrives meanwhile).
    hw.lock(hw.ctx); if (st.abortReq) { o.aborted = true; st.abortReq = false; } hw.unlock(hw.ctx);
    // ---- VERIFY (7): the 60 Hz rate is back and the stream is up; the restore transition's FIFO health is judged after it.
    phase = 7u;
    bool rateAfterOk = false;
    if (restoreDone) {
        const Meas a = measure(hw, st, tm, mms, false);
        r->frames_after = a.frames; r->ms_after = a.ms; r->rate_after_mhz = (uint32_t)measured_mhz(a.frames, a.ms);
        rateAfterOk = !a.readFail && !a.stalled && rate_within(r->rate_after_mhz, expect_mhz(row60()), tol);
        if (rateAfterOk) r->flags |= N48N_MODE_F_RATE_AFTER_OK;
    }
    const uint32_t prcA = hw.rd(hw.ctx, kRegPixelRateCntl);
    r->stream_after = hw.rd(hw.ctx, kRegDpStream[kLitDp]); r->steer_after = hw.rd(hw.ctx, kRegDp1Steer);
    r->dto_phase[2] = hw.rd(hw.ctx, kCap[kIdxDtoPhase].abs); r->vtotal_reg[2] = hw.rd(hw.ctx, kCap[kIdxVTotal].abs);
    r->fifo_after |= fifo_err(prcA); r->errcnt_after = err_count(prcA);
    const uint32_t digA = hw.rd(hw.ctx, kRegDigFifoCtrl0);
    r->dig_fifo_after = digA; r->dio_restore_errs = err_count(prcA) - err_count(dioBase);
    // P1: the underflow status after the restore (the trial phase's was cleared at the restore entry): an underflow in the restore transition is a restore that did not verify
    { const UfRead ua = uf_read(hw); r->ext.uf_hubp_after = ua.hubp; r->ext.uf_optc_after = ua.optc; (void)uf_record(hw, st, ua, kUfAfter); }
    // The restore TRANSITION is judged: a FIFO that is stuck after the restore must not report PASS.
    const bool healthBad = health_bad(prcA, r->steer_after, digA) || r->dio_restore_errs != 0u;
    if (healthBad) r->flags |= N48N_MODE_F_HEALTH_BAD;
    o.restoreBad = !restoreDone || !regsOk || !rateAfterOk || !stream_active(r->stream_after) || healthBad || repSeqBad || st.rep.ufAfterBad || repBlanked || repLockHeld;
    r->flags |= o.restoreBad ? N48N_MODE_F_RESTORE_FAILED : N48N_MODE_F_RESTORED;
    // ---- 14 (P4): the clock hold comes off ONLY after the 60 Hz restore verified (restoreBad never releases: the hold goes STUCK, the clocks stay up, harmless at 60 Hz). Trial thread, no lock held. ----
    if (holdOn) o.holdStuck = hw.hold_release(hw.ctx, o.restoreBad, &st.rep.hold) != n48dal::kRlOk;
    hw.lock(hw.ctx); if (st.abortReq) { o.aborted = true; st.abortReq = false; } hw.unlock(hw.ctx);
    r->verdict = judge(o);
    r->fail_phase = r->verdict == N48N_MODE_V_PASS ? 0u : (o.restoreBad ? (restoreDone ? 7u : 6u) : (failPhase ? failPhase : 6u));
    if (hc != nullptr) { r->hold_end = ld(st.endReason); r->hold_flags = ld(st.holdFlags) | (ld(st.scanBad) != 0u ? N48N_HOLD_FL_SCAN_BAD : 0u) | (ld(st.held) != 0u ? N48N_HOLD_FL_ENTERED : 0u); }   // 0.0.609

    hw.lock(hw.ctx);
    if (is_failure(r->verdict) && !st.latched) { st.latched = true; st.latchVerdict = r->verdict; st.latchRow = row; }
    if (r->verdict == N48N_MODE_V_PASS && row == 50u) st.step50Passed = true;
    if (r->verdict == N48N_MODE_V_PASS && row == 1u) st.resyncPassed = true;
    if (row2_proof(r->verdict, row, st.rep.lockHeldProven, st.rep.latchConfirmed)) st.row2Proven = true;   // 0.0.607: row 120's prerequisite: PASS with the lock-hold proof on this boot
    r->latched = st.latched ? 1u : 0u;
    if (st.latched) r->flags |= N48N_MODE_F_LATCHED;
    if (st.step50Passed) r->flags |= N48N_MODE_F_STEP_DOWN_DONE;
    detail::fill_ext(r, st);
    st.doneId = myId;
    hw.unlock(hw.ctx);
    hw.log(hw.ctx, kLogVerdict, r->verdict, r->flags, r->mismatch, r->rate_trial_mhz);
    stz(st.held, 0u); stz(st.holdOn, 0u);   // 0.0.609: the result buffer is final BEFORE busy drops (a waiting ModeRelease reads it after runnerDone)
    __atomic_store_n(&st.busy, 0u, __ATOMIC_RELEASE);
}
// The 0.0.605 entry point (no flags word).
inline void run_trial(const Hw &hw, State &st, const Timing &tm, uint32_t row, uint32_t dwellMs, n48n_mode_result *r) { run_trial(hw, st, tm, row, dwellMs, 0u, r); }

} // namespace n48mt
