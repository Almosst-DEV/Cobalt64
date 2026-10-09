//
//  navi48_disp2.h - build 0.0.631 (multi-monitor track, stage M4d: a TEST PATTERN on the second display; an internal design note, "M4 design" steps 4-9 and "M4b analysis").
//  The pure half of accel verb 98 `disp2 <op>`: the ops, the boot-arg latch, every register this verb may touch (absolute BAR5 dwords on this card), the step lists of each op (the Linux functions they mirror),
//  the INSTANCE GUARD, the EDID timing decode and the result layouts. Shared by the kext (dcn/navi48_disp2_flow.h + the glue in dcn/navi48_dcn.cpp), the CLI (tools/pc/navi48test.c includes this very file)
//  and the host test (tests/native_disp2_test.cpp). Plain C99 + C++17: no kernel header.
//
//    98  disp2 timing    OTG1 + ODM1 + OPP1 + DPG1: 1920x1080@60 CEA (VIC 16) from the monitor A SINK-B's EDID DTD, a DPG colour-squares pattern, OTG1 master enable. NO HUBP / DPP / MPC (no plane).
//        disp2 connect   DIG2 front end: source OTG1, DVI mode, FIFO; DIG2 back end FE-source bit FE2; stream mapper DIG2 -> link 2 (UNIPHY_C). The BACK END mode / enable and PHY C stay the DMUB's (phyc enable).
//        disp2 off       the reverse: DPG1 off, DIG2 FIFO / FE off, BE FE-source bit off, mapper 0, OTG1 master disable, OTG1 / ODM1 / OPP1 clocks off. PHY C and PLL2 stay the DMUB's (phyc disable, pclk otg1-off).
//        disp2 status    reads only.
//
//  Behind boot-arg navi48-disp2=1 (default OFF, latched once at start): OFF returns N48D2_OFF and touches nothing, not even a register read. The verb's admission is navi48-metal-disp's (like 91..97).
//
//  Every write passes, in this order: (1) the INSTANCE GUARD below, judged over the WHOLE step list before the first register is even read (a list that names one forbidden register writes nothing);
//  (2) the same guard again per write in the kext; (3) the DCN write allowlist (src/dcn41/dcn41_allow_ranges.h). The guard refuses OTG0/ODM0/VTG0, OPP0/DPG0/FMT0, HUBP0, DPP0, MPCC0, the DIG1 stream path
//  (VPG1, AFMT1, DME1, DIG1 FE/BE, DP1, DIG1's stream mapper, DP_AUX1), PHY B (UNIPHYB, DCIO_UNIPHY1 / RDPCSTX1) and the whole DCCG BASE_IDX 1 block (OTG0_PIXEL_RATE_CNTL, DTO0, every PHYPLL source and
//  the shared OTG_PIXEL_RATE_DIV): this verb writes no clock register at all. Beyond that it admits ONLY the 39 OTG1 / ODM1 / VTG1 / OPP1 / DPG1 / FMT1 / DIG2 registers listed in n48d2_allowed[].
//
//  0.0.633 (multi-monitor): the verb takes an INSTANCE (argument bits 8..15; 0 or 1 = OTG1 / DIG2 / PHY C, the monitor A, exactly as before; 2 = OTG2 / DIG3 / PHY D, the monitor B SINK-C on HDMI/ddc3/hpd4). Each instance has its
//  OWN allowlist of 39 registers (instance 2: OTG2 / ODM2 / VTG2 / OPP2 / DPG2 / FMT2 / DIG3) and the guard refuses, for each instance, every register of the other one and everything of OTG0 / DIG1 / PHY B / the
//  DCCG - with ONE narrow exception per instance (see "the symbol clock exception" below): SYMCLKC_CLOCK_ENABLE bits [10:8] and [4] for instance 1, SYMCLKD_CLOCK_ENABLE for instance 2, and nothing else of the DCCG.
//
//  0.0.634 (multi-monitor, stage M4b + the DP-plane watch): op 7 `timing1440` = `timing` for instance 2 ONLY over the monitor B's 2560x1440@60 CTA descriptor (241.50 MHz; the same step-list builder, the same registers in the same order, the DPG pattern
//  2560x1440); `off` now clears the instance's SYMCLK FE bits AFTER the DIG back-end disconnect (Linux: reset_dio_stream_encoder, then disable_symclk_se); and every op reads NINE more registers of the live DP pipe before and after (n48d2_dpx_regs:
//  MPC_OUT0_MUX, MPCC0 TOP / BOT / OPP, HUBP0 DCHUBP_CNTL, DPPCLK0 DTO, DPPCLK_CTRL bit 0, DET0, COMPBUF): any masked change ends the op in N48D2_DP_DISTURBED exactly like the OTG0 / DIG1 checks. READS only: no new writable register.
//
//  0.0.635 (multi-monitor, stages M4c / M4d: the monitor B's plane from VRAM; an internal design note, the LAST section "M4c/M4d build spec"): five more ops, ALL instance 2 ONLY and behind the same navi48-disp2 switch:
//        op 8 plane    two 2560x1440x4 buffers (A / B: kext-drawn colour bars, VRAM-verified), HUBP2 / DPP2 / MPCC2 programmed from pipe 0's census values (COPY0 steps), the address latched while BLANKED, then unblank
//        op 9 show     clear the underflow flags, 10 frames, require 0, DPG2 off (the plane is the picture)
//        op 10 flip    A or B (argument bits 16..23, legal for op 10 alone): SET HIGH, SET LOW, WAIT FLIP_PENDING = 0, read EARLIEST_INUSE
//        op 11 crc     OTG2's CRC over the 2560x1440 raster
//        op 12 planeoff the rollback: DPG2 on, blank, MPC_OUT2 / MPCC2 off, clocks off; the buffers are freed only after HUBP2's clock reads off. It also runs by itself on any failure of OP 8 after its first write; ops 9, 10 and 11 roll back
//                      ONLY on a DP disturbance (a failed gate there reports and leaves the plane as it is). 0.0.636: planeoff on a plane that is already off (stage NONE, HUBP2's clock off) answers OK, 'nothing to do'
//  0.0.638 (the 0.0.637 run's op 8 failed closed at the HUBP2_HUBP_CLK_CNTL status wait): that WAIT is GONE (Linux's hubp2_clk_cntl is a bare REG_UPDATE; the status bits are demand-gated), replaced by RECORDED READS (step kind K_REC, no gate) of
//  HUBP2_HUBP_CLK_CNTL, DCCG_GATE_DISABLE_CNTL6, DCCG_GATE_DISABLE_CNTL and DOMAIN2_PG_STATUS ~1 ms after the clock enable and again after the 2-frame wait; every WAIT that times out records its register and LAST read value; the pre-unblank gate (REMOVED in 0.0.643) was
//  FLIP_PENDING == 0 alone; part C (MPCC2, mux, unblank) is issued back to back and the EARLIEST_INUSE == A gate follows the unblank; the rollback zeroes the primary address; op 13 `planerec` returns the record page (reads no register).
//  0.0.643 (the 0.0.639 run: FLIP_PENDING stayed set for 5 frames before the unblank, because a blanked HUBP never consumes a pending address): the pre-unblank FLIP_PENDING gate is GONE. op 8 follows Linux's update_dchubp_dpp: it takes OTG2's master
//  update lock (optc3_lock: OTG_GLOBAL_CONTROL2.OTG_MASTER_UPDATE_LOCK_SEL = 2, OTG_MASTER_UPDATE_LOCK = 1, wait UPDATE_LOCK_STATUS), writes the address HIGH / LOW (A), MPCC2, MPC_OUT2_MUX and the unblank back to back, releases the lock (optc1_unlock), and only THEN
//  gates FLIP_PENDING == 0 and EARLIEST_INUSE == A, DET2 current = 3 and the health. The two lock registers are the only registers 0.0.643 adds to the writable lists; every exit path of op 8 releases the lock before any rollback.
//
//  and the guard grows (instance 2 only): the plane registers with their kind / mask / value rules, TWO DCCG exceptions (DPPCLK2_DTO_PARAM 0x15b SET {0x00ff00ff, 0}, DPPCLK_CTRL 0x168 UPD mask exactly 0x40), the new forbidden spans (DCHUBBUB / DCN_VM,
//  MPC_OUT0, MPC global, HUBP1 / 3, DPP1 / 3, MPCC1 / 3), the COPY0 step (a read of one of 20 EXACT pipe-0 sources, pinned to its census value, written to pipe 2) and EXPECT (read only); the DP watch grows from 9 to 15 registers (reads only).
//
//  Register offsets and fields: Linux dcn_4_1_0_offset.h / dcn_4_1_0_sh_mask.h (the tree in re/linux-dc; the host test re-reads both). Absolute = segment + offset with this card's DMU segments
//  BASE_IDX 1 = 0xc0, BASE_IDX 2 = 0x34c0 (the allowlist refuses any other set, dcn41_allow_expected_segs).
//
#ifndef N48_DISP2_H
#define N48_DISP2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N48D2_ACT 98u

// ---- the boot-arg latch (default OFF): only "navi48-disp2=1" turns the verb on ----------------------------------------------------------------------------------------------------------------------------
#define N48D2_LATCH_UNSET 0u
#define N48D2_LATCH_OFF   1u
#define N48D2_LATCH_ON    2u
static inline uint32_t n48d2_latch_value(int present, uint32_t value) { return (present && value == 1u) ? N48D2_LATCH_ON : N48D2_LATCH_OFF; }
static inline int n48d2_latch_is_on(uint32_t latch) { return latch == N48D2_LATCH_ON; }

// ---- the ops and the argument (op in bits 0..7, every other bit zero) ---------------------------------------------------------------------------------------------------------------------------------
#define N48D2_OP_TIMING  1u
#define N48D2_OP_CONNECT 2u
#define N48D2_OP_OFF     3u
#define N48D2_OP_STATUS  4u
#define N48D2_OP_STATUS2 5u     /* 0.0.632: the second status page (reads only): the 12 registers that do not fit the 13-scalar result of op 4; the CLI's `disp2 status` calls op 4 and then op 5 */
#define N48D2_OP_STATUS3 6u     /* 0.0.633: the third status page (reads only): the DIG's TMDS / FIFO / CRC registers, DIG1's two for comparison, SYMCLKB (the DP's) and the instance's SYMCLK */
#define N48D2_OP_TIMING1440 7u  /* 0.0.634 (M4b): `timing` for instance 2 ONLY with the monitor B's 2560x1440@60 CTA DTD (241.50 MHz): the same registers in the same order as op 1, the DPG pattern sized 2560x1440; instance 1 refuses it */
#define N48D2_OP_PLANE    8u    /* 0.0.635 (M4c): the monitor B's plane from VRAM (instance 2 only) */
#define N48D2_OP_SHOW     9u    /* 0.0.635: clear underflow, 10 frames, DPG2 off */
#define N48D2_OP_FLIP     10u   /* 0.0.635 (M4d): flip to buffer A (0) or B (1): the index is argument bits 16..23, legal for op 10 alone */
#define N48D2_OP_CRC      11u   /* 0.0.635 (M4d): OTG2 CRC */
#define N48D2_OP_PLANEOFF 12u   /* 0.0.635: the rollback */
#define N48D2_OP_PLANEREC 13u   /* 0.0.638: the RECORD page of the last plane op (reads NO register, writes nothing): the eight recorded reads of op 8 (HUBP2_HUBP_CLK_CNTL, DCCG_GATE_DISABLE_CNTL6, DCCG_GATE_DISABLE_CNTL, DOMAIN2_PG_STATUS at ~1 ms after the clock enable and again after the 2-frame wait) and the register + last read value of the first WAIT that timed out (the 13-scalar result of op 8 has no room for them) */
#define N48D2_OP_FBHOLD   14u   /* 0.0.652 (M5): `fbhold 2` (0.0.658: and `fbhold 1`, the monitor A) - after a successful `show` (front = A, underflow 0, VM fault 0) the plane is HELD for the rest of this boot: stage N48D2_PL_HELD, the allocator pins the pair (n1c_d2_pin). Reads only (the gates), writes NO register. IRREVERSIBLE: a reboot is the release */
static inline const char *n48d2_op_name(uint32_t op)
{
    return op == N48D2_OP_TIMING ? "timing" : op == N48D2_OP_CONNECT ? "connect" : op == N48D2_OP_OFF ? "off" : op == N48D2_OP_STATUS ? "status" : op == N48D2_OP_STATUS2 ? "status2" : op == N48D2_OP_STATUS3 ? "status3" : op == N48D2_OP_TIMING1440 ? "timing1440" :
           op == N48D2_OP_PLANE ? "plane" : op == N48D2_OP_SHOW ? "show" : op == N48D2_OP_FLIP ? "flip" : op == N48D2_OP_CRC ? "crc" : op == N48D2_OP_PLANEOFF ? "planeoff" : op == N48D2_OP_PLANEREC ? "planerec" : op == N48D2_OP_FBHOLD ? "fbhold" : "?";
}
// The argument: op in bits 0..7, the INSTANCE in bits 8..15 (0.0.633), every other bit zero. n48d2_arg(op) is the 0.0.631 / 0.0.632 argument (instance 0 = instance 1) and stays byte-identical.
#define N48D2_INST_MONA 1u      /* OTG1 / ODM1 / OPP1 / DPG1 / DIG2 / UNIPHY_C / SYMCLKC: the monitor A SINK-B (HDMI/ddc2/hpd3) */
#define N48D2_INST_MONB 2u      /* OTG2 / ODM2 / OPP2 / DPG2 / DIG3 / UNIPHY_D / SYMCLKD: the monitor B SINK-C (HDMI/ddc3/hpd4) */
static inline uint64_t n48d2_arg(uint32_t op) { return (uint64_t)(op & 0xFFu); }
static inline uint64_t n48d2_arg_i(uint32_t op, uint32_t inst) { return (uint64_t)(op & 0xFFu) | ((uint64_t)(inst & 0xFFu) << 8); }
static inline uint32_t n48d2_arg_op(uint64_t a) { return (uint32_t)(a & 0xFFu); }
static inline uint32_t n48d2_arg_inst(uint64_t a) { return ((a >> 8) & 0xFFu) == N48D2_INST_MONB ? N48D2_INST_MONB : N48D2_INST_MONA; }
static inline uint32_t n48d2_arg_buf(uint64_t a) { return (uint32_t)((a >> 16) & 0xFFu); }     /* 0.0.635: op 10's buffer index (0 = A, 1 = B) */
// 0.0.635: ops 8..12 are the monitor B's alone (the instance field must be EXACTLY 2: an monitor A or default instance is refused with BAD_ARG, nothing read); bits 16..23 carry a buffer index for op 10 ONLY (0 or 1) and are zero for every other op; bits 24.. are always zero.
static inline int n48d2_arg_ok(uint64_t a)
{
    const uint32_t op = (uint32_t)(a & 0xFFu), inst = (uint32_t)((a >> 8) & 0xFFu);
    if (op == N48D2_OP_TIMING1440 && inst != N48D2_INST_MONB) return 0;        /* 0.0.634: the 1440p timing is the monitor B's alone (the monitor A is 1080p-only) */
    if (op >= N48D2_OP_PLANE && op <= N48D2_OP_FBHOLD) {        /* 0.0.652: op 14 (fbhold); 0.0.655: ops 8..13 are BOTH instances' (instance field 0 / 1 = the monitor A, 2 = the monitor B); 0.0.658: op 14 too, for instance 1 or 2 EXACTLY (the monitor A's framebuffer build) */
        if (op == N48D2_OP_FBHOLD && inst != N48D2_INST_MONB && inst != N48D2_INST_MONA) return 0;      /* 0.0.658: fbhold names its instance EXACTLY (1 = the monitor A, 2 = the monitor B); the default instance 0 is not accepted */
        if (inst > N48D2_INST_MONB || (a >> 24) != 0u) return 0;
        return op == N48D2_OP_FLIP ? (n48d2_arg_buf(a) <= 1u) : ((a >> 16) == 0u);
    }
    return op >= N48D2_OP_TIMING && op <= N48D2_OP_TIMING1440 && inst <= N48D2_INST_MONB && (a >> 16) == 0u;
}

// ---- status ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
enum {
    N48D2_OK = 0, N48D2_BAD_ARG = 1, N48D2_OFF = 2, N48D2_NO_DEVICE = 3, N48D2_BUSY = 4, N48D2_BASES = 5, N48D2_GUARD_REFUSED = 6, N48D2_ALLOW_REFUSED = 7, N48D2_NO_PIXCLK = 8, N48D2_OTG1_BUSY = 9,
    N48D2_NO_PHY = 10, N48D2_NO_TIMING = 11, N48D2_FE_BUSY = 12, N48D2_DP_NOT_COUNTING = 13, N48D2_WAIT_TIMEOUT = 14, N48D2_DP_DISTURBED = 15,
    N48D2_PRECHECK = 16, N48D2_NO_BUFFER = 17, N48D2_READBACK = 18, N48D2_GATE = 19, N48D2_PLANE_STATE = 20, N48D2_HELD = 21, N48D2_STATUS_COUNT = 22     /* 0.0.635 (M4c / M4d); 0.0.652 (M5): HELD */
};
static inline const char *n48d2_status_name(uint32_t s)
{
    switch (s) {
    case N48D2_OK: return "OK";
    case N48D2_BAD_ARG: return "bad argument (disp2 timing|connect|off|status)";
    case N48D2_OFF: return "boot-arg navi48-disp2 is not 1: the verb is OFF, nothing was touched";
    case N48D2_NO_DEVICE: return "no bound display layer (BAR5 / IP discovery / allowlist not armed)";
    case N48D2_BUSY: return "another disp2 call is running";
    case N48D2_BASES: return "the DMU segment bases are not 0xc0 / 0x34c0: REFUSED, nothing touched";
    case N48D2_GUARD_REFUSED: return "the instance guard refused a step of this op (a forbidden or unlisted register, or a forbidden value): NOTHING written";
    case N48D2_ALLOW_REFUSED: return "the DCN write allowlist refused a write part-way: the remaining steps were skipped and `off` was run";
    case N48D2_NO_PIXCLK: return "OTG1_PHYPLL_PIXEL_RATE_CNTL source is not 2 (PLL2): run `dmubsend pclk otg1-on` first; nothing written";
    case N48D2_OTG1_BUSY: return "OTG1 is already master-enabled: run `disp2 off` first; nothing written";
    case N48D2_NO_PHY: return "DIG2's back end is not enabled in DVI mode with its clock on: run `dmubsend phyc enable` first; nothing written";
    case N48D2_NO_TIMING: return "OTG1 is not running: run `disp2 timing` first; nothing written";
    case N48D2_FE_BUSY: return "DIG2's front end is already enabled: run `disp2 off` first; nothing written";
    case N48D2_DP_NOT_COUNTING: return "OTG0's frame counter did not advance in 50 ms BEFORE the op (the DP display is not running as expected): nothing written";
    case N48D2_WAIT_TIMEOUT: return "every step was written, but a Linux REG_WAIT timed out (clock-on, FIFO reset-done, master-enable state or DPG pending); see the timeout count";
    case N48D2_DP_DISTURBED: return "OTG0 stopped counting, or a DIG1 / OTG0 / SYMCLKB register, or (0.0.634) one of the DP's plane registers (MPC_OUT0_MUX, MPCC0 TOP / BOT / OPP, HUBP0 DCHUBP_CNTL, DPPCLK0 DTO, DPPCLK_CTRL bit 0, DET0, COMPBUF) changed across the op: `off` was run at once";
    case N48D2_PRECHECK: return "a read-only PRECHECK did not hold (an EXPECT register, a COPY0 source that drifted from its census value, the buffers' admission): NOTHING was written; see the precheck register";
    case N48D2_NO_BUFFER: return "the two scanout buffers could not be allocated, admitted, filled or verified (24 points each, through the MM window): NOTHING was written to a register";
    case N48D2_READBACK: return "a register write did not read back as written (the HUBP2 / DPP2 clock gate): the HUBP clock was NOT enabled and the plane was rolled back";
    case N48D2_GATE: return "a hardware GATE failed (a WAIT timed out - its register and last value are reported, DET2 current != 3, the OTG2 update lock never asserted, FLIP_PENDING / EARLIEST_INUSE did not latch A after the unlock, underflow / SEG_ALLOC_ERR / TIMEOUT / VM fault / ODM2 underflow, or the flip did not latch); see the gate values; an op 8 failure after its first write was rolled back; ops 9..11 roll back only on a DP disturbance";
    case N48D2_PLANE_STATE: return "the plane is not in the state this op needs (plane first; show after plane; flip after plane; `disp2 off 2`, `timing 2`, `connect 2` and `timing1440 2` are refused while HUBP2's clock is on: run planeoff first); nothing was written";
    case N48D2_HELD: return "the plane is HELD for this boot (`disp2 fbhold <instance>` ran: Navi48Framebuffer scans it): plane, show, flip, planeoff, fbhold and timing / connect / off / timing1440 on THAT instance (and, for the monitor A, its DMUB teardown templates) are REFUSED; status, crc and planerec still work; the other instance is not affected; a reboot releases it; nothing was read or written";
    default: return "unknown";
    }
}

// ---- absolute registers (BASE_IDX 2 = 0x34c0 + offset, BASE_IDX 1 = 0xc0 + offset; offsets from dcn_4_1_0_offset.h) ----------------------------------------------------------------------------------
#define N48D2_SEG1 0x000000c0u
#define N48D2_SEG2 0x000034c0u
#define N48D2_R(off) (N48D2_SEG2 + (off))
/* OTG1 / ODM1 / VTG1 */
#define N48D2_VTG1_CONTROL            N48D2_R(0x0531u)   /* 0x39f1 */
#define N48D2_ODM1_DATA_SOURCE_SELECT N48D2_R(0x1adbu)   /* 0x4f9b */
#define N48D2_ODM1_DATA_FORMAT_CONTROL N48D2_R(0x1adcu)  /* 0x4f9c */
#define N48D2_ODM1_INPUT_CLOCK_CONTROL N48D2_R(0x1ae0u)  /* 0x4fa0 */
#define N48D2_ODM1_MEMORY_CONFIG      N48D2_R(0x1ae1u)   /* 0x4fa1 */
#define N48D2_OTG1_H_TOTAL            N48D2_R(0x1baau)   /* 0x506a */
#define N48D2_OTG1_H_BLANK_START_END  N48D2_R(0x1babu)   /* 0x506b */
#define N48D2_OTG1_H_SYNC_A           N48D2_R(0x1bacu)   /* 0x506c */
#define N48D2_OTG1_H_SYNC_A_CNTL      N48D2_R(0x1badu)   /* 0x506d */
#define N48D2_OTG1_H_TIMING_CNTL      N48D2_R(0x1baeu)   /* 0x506e */
#define N48D2_OTG1_V_TOTAL            N48D2_R(0x1bafu)   /* 0x506f */
#define N48D2_OTG1_V_TOTAL_MIN        N48D2_R(0x1bb0u)   /* 0x5070 */
#define N48D2_OTG1_V_TOTAL_MAX        N48D2_R(0x1bb1u)   /* 0x5071 */
#define N48D2_OTG1_V_BLANK_START_END  N48D2_R(0x1bb8u)   /* 0x5078 */
#define N48D2_OTG1_V_SYNC_A           N48D2_R(0x1bb9u)   /* 0x5079 */
#define N48D2_OTG1_V_SYNC_A_CNTL      N48D2_R(0x1bbau)   /* 0x507a */
#define N48D2_OTG1_CONTROL            N48D2_R(0x1bc3u)   /* 0x5083 */
#define N48D2_OTG1_INTERLACE_CONTROL  N48D2_R(0x1bc5u)   /* 0x5085 */
#define N48D2_OTG1_STATUS             N48D2_R(0x1bc9u)   /* 0x5089 (read only here) */
#define N48D2_OTG1_FRAME_COUNT        N48D2_R(0x1bcdu)   /* 0x508d (read only here) */
#define N48D2_OTG1_CLOCK_CONTROL      N48D2_R(0x1c04u)   /* 0x50c4 */
#define N48D2_OTG1_VSTARTUP_PARAM     N48D2_R(0x1c05u)   /* 0x50c5 */
#define N48D2_OTG1_VUPDATE_PARAM      N48D2_R(0x1c06u)   /* 0x50c6 */
#define N48D2_OTG1_VREADY_PARAM       N48D2_R(0x1c07u)   /* 0x50c7 */
#define N48D2_OTG1_PHYPLL_PIXEL_RATE_CNTL (N48D2_SEG1 + 0x0087u)   /* 0x0147, BASE_IDX 1: READ ONLY (the DMUB's SET_PIXEL_CLOCK set it) */
/* OPP1 / DPG1 / FMT1 */
#define N48D2_FMT1_422_CONTROL        N48D2_R(0x18a3u)   /* 0x4d63 */
#define N48D2_DPG1_CONTROL            N48D2_R(0x18aeu)   /* 0x4d6e */
#define N48D2_DPG1_RAMP_CONTROL       N48D2_R(0x18afu)   /* 0x4d6f */
#define N48D2_DPG1_DIMENSIONS         N48D2_R(0x18b0u)   /* 0x4d70 */
#define N48D2_DPG1_COLOUR_R_CR        N48D2_R(0x18b1u)   /* 0x4d71 */
#define N48D2_DPG1_COLOUR_G_Y         N48D2_R(0x18b2u)   /* 0x4d72 */
#define N48D2_DPG1_COLOUR_B_CB        N48D2_R(0x18b3u)   /* 0x4d73 */
#define N48D2_DPG1_OFFSET_SEGMENT     N48D2_R(0x18b4u)   /* 0x4d74 */
#define N48D2_DPG1_STATUS             N48D2_R(0x18b5u)   /* 0x4d75 (read only here) */
#define N48D2_OPP_PIPE1_CONTROL       N48D2_R(0x18e6u)   /* 0x4da6 */
/* DIG2 (DIGC: FE2 / BE2, UNIPHY_C) */
#define N48D2_DIG2_STREAM_MAPPER      N48D2_R(0x1f0fu)   /* 0x53cf */
#define N48D2_DIG2_FE_CNTL            N48D2_R(0x22dbu)   /* 0x579b */
#define N48D2_DIG2_FE_CLK_CNTL        N48D2_R(0x22dcu)   /* 0x579c */
#define N48D2_DIG2_FE_EN_CNTL         N48D2_R(0x22ddu)   /* 0x579d */
#define N48D2_DIG2_CLOCK_PATTERN      N48D2_R(0x22e0u)   /* 0x57a0 */
#define N48D2_DIG2_FIFO_CTRL0         N48D2_R(0x22e3u)   /* 0x57a3 */
#define N48D2_DIG2_HDMI_CONTROL       N48D2_R(0x22e6u)   /* 0x57a6 */
#define N48D2_DIG2_BE_CLK_CNTL        N48D2_R(0x2303u)   /* 0x57c3 (read only here: the DMUB's TRANSMITTER_CONTROL owns the back end mode / clock) */
#define N48D2_DIG2_BE_CNTL            N48D2_R(0x2304u)   /* 0x57c4 */
#define N48D2_DIG2_BE_EN_CNTL         N48D2_R(0x2305u)   /* 0x57c5 (read only here) */
#define N48D2_DIG2_TMDS_CTL_BITS      N48D2_R(0x2333u)   /* 0x57f3 (read only here) */
/* ---- 0.0.633: instance 2 (the monitor B: OTG2 / ODM2 / VTG2 / OPP2 / DPG2 / FMT2 / DIG3, UNIPHY_D) - the same 39 writable registers, one instance up; every offset is dcn_4_1_0_offset.h's (the host test re-reads them) ---- */
#define N48D2_VTG2_CONTROL                 N48D2_R(0x0532u)         /* 0x39f2 */
#define N48D2_ODM2_DATA_SOURCE_SELECT      N48D2_R(0x1aebu)         /* 0x4fab */
#define N48D2_ODM2_DATA_FORMAT_CONTROL     N48D2_R(0x1aecu)         /* 0x4fac */
#define N48D2_ODM2_INPUT_CLOCK_CONTROL     N48D2_R(0x1af0u)         /* 0x4fb0 */
#define N48D2_ODM2_MEMORY_CONFIG           N48D2_R(0x1af1u)         /* 0x4fb1 */
#define N48D2_OTG2_H_TOTAL                 N48D2_R(0x1c2au)         /* 0x50ea */
#define N48D2_OTG2_H_BLANK_START_END       N48D2_R(0x1c2bu)         /* 0x50eb */
#define N48D2_OTG2_H_SYNC_A                N48D2_R(0x1c2cu)         /* 0x50ec */
#define N48D2_OTG2_H_SYNC_A_CNTL           N48D2_R(0x1c2du)         /* 0x50ed */
#define N48D2_OTG2_H_TIMING_CNTL           N48D2_R(0x1c2eu)         /* 0x50ee */
#define N48D2_OTG2_V_TOTAL                 N48D2_R(0x1c2fu)         /* 0x50ef */
#define N48D2_OTG2_V_TOTAL_MIN             N48D2_R(0x1c30u)         /* 0x50f0 */
#define N48D2_OTG2_V_TOTAL_MAX             N48D2_R(0x1c31u)         /* 0x50f1 */
#define N48D2_OTG2_V_BLANK_START_END       N48D2_R(0x1c38u)         /* 0x50f8 */
#define N48D2_OTG2_V_SYNC_A                N48D2_R(0x1c39u)         /* 0x50f9 */
#define N48D2_OTG2_V_SYNC_A_CNTL           N48D2_R(0x1c3au)         /* 0x50fa */
#define N48D2_OTG2_CONTROL                 N48D2_R(0x1c43u)         /* 0x5103 */
#define N48D2_OTG2_INTERLACE_CONTROL       N48D2_R(0x1c45u)         /* 0x5105 */
#define N48D2_OTG2_STATUS                  N48D2_R(0x1c49u)         /* 0x5109 */
#define N48D2_OTG2_FRAME_COUNT             N48D2_R(0x1c4du)         /* 0x510d */
#define N48D2_OTG2_CLOCK_CONTROL           N48D2_R(0x1c84u)         /* 0x5144 */
#define N48D2_OTG2_VSTARTUP_PARAM          N48D2_R(0x1c85u)         /* 0x5145 */
#define N48D2_OTG2_VUPDATE_PARAM           N48D2_R(0x1c86u)         /* 0x5146 */
#define N48D2_OTG2_VREADY_PARAM            N48D2_R(0x1c87u)         /* 0x5147 */
#define N48D2_OTG2_PHYPLL_PIXEL_RATE_CNTL  (N48D2_SEG1 + 0x008bu)   /* 0x014b */
#define N48D2_FMT2_422_CONTROL             N48D2_R(0x18fdu)         /* 0x4dbd */
#define N48D2_DPG2_CONTROL                 N48D2_R(0x1908u)         /* 0x4dc8 */
#define N48D2_DPG2_RAMP_CONTROL            N48D2_R(0x1909u)         /* 0x4dc9 */
#define N48D2_DPG2_DIMENSIONS              N48D2_R(0x190au)         /* 0x4dca */
#define N48D2_DPG2_COLOUR_R_CR             N48D2_R(0x190bu)         /* 0x4dcb */
#define N48D2_DPG2_COLOUR_G_Y              N48D2_R(0x190cu)         /* 0x4dcc */
#define N48D2_DPG2_COLOUR_B_CB             N48D2_R(0x190du)         /* 0x4dcd */
#define N48D2_DPG2_OFFSET_SEGMENT          N48D2_R(0x190eu)         /* 0x4dce */
#define N48D2_DPG2_STATUS                  N48D2_R(0x190fu)         /* 0x4dcf */
#define N48D2_OPP_PIPE2_CONTROL            N48D2_R(0x1940u)         /* 0x4e00 */
#define N48D2_DIG3_STREAM_MAPPER           N48D2_R(0x1f10u)         /* 0x53d0 */
#define N48D2_DIG3_FE_CNTL                 N48D2_R(0x23ffu)         /* 0x58bf */
#define N48D2_DIG3_FE_CLK_CNTL             N48D2_R(0x2400u)         /* 0x58c0 */
#define N48D2_DIG3_FE_EN_CNTL              N48D2_R(0x2401u)         /* 0x58c1 */
#define N48D2_DIG3_CLOCK_PATTERN           N48D2_R(0x2404u)         /* 0x58c4 */
#define N48D2_DIG3_FIFO_CTRL0              N48D2_R(0x2407u)         /* 0x58c7 */
#define N48D2_DIG3_HDMI_CONTROL            N48D2_R(0x240au)         /* 0x58ca */
#define N48D2_DIG3_BE_CLK_CNTL             N48D2_R(0x2427u)         /* 0x58e7 */
#define N48D2_DIG3_BE_CNTL                 N48D2_R(0x2428u)         /* 0x58e8 */
#define N48D2_DIG3_BE_EN_CNTL              N48D2_R(0x2429u)         /* 0x58e9 */
#define N48D2_DIG3_TMDS_CTL_BITS           N48D2_R(0x2457u)         /* 0x5917 */
/* 0.0.632: READ ONLY (the second status page; none is in n48d2_allowed[], so the guard refuses a write to any of them as UNLISTED) */
#define N48D2_SYMCLKC_CLOCK_ENABLE    (N48D2_SEG1 + 0x00a2u)   /* 0x0162, BASE_IDX 1 (DCCG): FE enable / source of SYMCLKC */
#define N48D2_OTG1_PIXEL_RATE_CNTL    (N48D2_SEG1 + 0x0084u)   /* 0x0144, BASE_IDX 1 (DCCG): OTG1's DTO enable / TMDS branch (not the PHYPLL source register 0x0147) */
#define N48D2_FMT1_CLAMP_COMPONENT_R  N48D2_R(0x1896u)         /* 0x4d56 */
#define N48D2_FMT1_CLAMP_COMPONENT_G  N48D2_R(0x1897u)         /* 0x4d57 */
#define N48D2_FMT1_CLAMP_COMPONENT_B  N48D2_R(0x1898u)         /* 0x4d58 */
#define N48D2_FMT1_DYNAMIC_EXP_CNTL   N48D2_R(0x1899u)         /* 0x4d59 */
#define N48D2_FMT1_CONTROL            N48D2_R(0x189au)         /* 0x4d5a */
#define N48D2_FMT1_BIT_DEPTH_CONTROL  N48D2_R(0x189bu)         /* 0x4d5b */
#define N48D2_FMT1_CLAMP_CNTL         N48D2_R(0x189fu)         /* 0x4d5f */
#define N48D2_HUBP1_DCHUBP_CNTL       N48D2_R(0x06d0u)         /* 0x3b90 (BASE_IDX 2): no plane is attached, so this is expected idle; a read of a power-gated block may return 0xffffffff */
#define N48D2_HUBP1_HUBP_CLK_CNTL     N48D2_R(0x06d1u)         /* 0x3b91 */
#define N48D2_OTG1_V_TOTAL_CONTROL    N48D2_R(0x1bb3u)         /* 0x5073 */
/* 0.0.633: the instance-2 twins of the second status page (READ ONLY) */
#define N48D2_SYMCLKD_CLOCK_ENABLE         (N48D2_SEG1 + 0x00a3u)   /* 0x0163 */
#define N48D2_OTG2_PIXEL_RATE_CNTL         (N48D2_SEG1 + 0x0088u)   /* 0x0148 */
#define N48D2_FMT2_CLAMP_COMPONENT_R       N48D2_R(0x18f0u)         /* 0x4db0 */
#define N48D2_FMT2_CLAMP_COMPONENT_G       N48D2_R(0x18f1u)         /* 0x4db1 */
#define N48D2_FMT2_CLAMP_COMPONENT_B       N48D2_R(0x18f2u)         /* 0x4db2 */
#define N48D2_FMT2_DYNAMIC_EXP_CNTL        N48D2_R(0x18f3u)         /* 0x4db3 */
#define N48D2_FMT2_CONTROL                 N48D2_R(0x18f4u)         /* 0x4db4 */
#define N48D2_FMT2_BIT_DEPTH_CONTROL       N48D2_R(0x18f5u)         /* 0x4db5 */
#define N48D2_FMT2_CLAMP_CNTL              N48D2_R(0x18f9u)         /* 0x4db9 */
#define N48D2_HUBP2_DCHUBP_CNTL            N48D2_R(0x07acu)         /* 0x3c6c */
#define N48D2_HUBP2_HUBP_CLK_CNTL          N48D2_R(0x07adu)         /* 0x3c6d */
#define N48D2_OTG2_V_TOTAL_CONTROL         N48D2_R(0x1c33u)         /* 0x50f3 */
/* 0.0.633: the third status page (READ ONLY, none is in any allowed list). DIG2 / DIG3: the legacy TMDS block, the test pattern / FIFO level / HDMI status / output CRC the M4d analysis asked for; DIG1 only for comparison. */
#define N48D2_DIG2_TEST_PATTERN       N48D2_R(0x22e1u)   /* 0x57a1 */
#define N48D2_DIG2_FIFO_CTRL1         N48D2_R(0x22e4u)   /* 0x57a4 */
#define N48D2_DIG2_HDMI_STATUS        N48D2_R(0x22e7u)   /* 0x57a7 */
#define N48D2_DIG2_TMDS_CNTL          N48D2_R(0x232cu)   /* 0x57ec */
#define N48D2_DIG2_TMDS_CONTROL_CHAR  N48D2_R(0x232du)   /* 0x57ed */
#define N48D2_DIG2_TMDS_DCBALANCER_CONTROL N48D2_R(0x2334u)   /* 0x57f4 */
#define N48D2_DIG2_OUTPUT_CRC_CNTL    N48D2_R(0x22deu)   /* 0x579e */
#define N48D2_DIG2_OUTPUT_CRC_RESULT  N48D2_R(0x22dfu)   /* 0x579f */
#define N48D2_DIG3_TEST_PATTERN       N48D2_R(0x2405u)   /* 0x58c5 */
#define N48D2_DIG3_FIFO_CTRL1         N48D2_R(0x2408u)   /* 0x58c8 */
#define N48D2_DIG3_HDMI_STATUS        N48D2_R(0x240bu)   /* 0x58cb */
#define N48D2_DIG3_TMDS_CNTL          N48D2_R(0x2450u)   /* 0x5910 */
#define N48D2_DIG3_TMDS_CONTROL_CHAR  N48D2_R(0x2451u)   /* 0x5911 */
#define N48D2_DIG3_TMDS_DCBALANCER_CONTROL N48D2_R(0x2458u)   /* 0x5918 */
#define N48D2_DIG3_OUTPUT_CRC_CNTL    N48D2_R(0x2402u)   /* 0x58c2 */
#define N48D2_DIG3_OUTPUT_CRC_RESULT  N48D2_R(0x2403u)   /* 0x58c3 */
#define N48D2_DIG1_FIFO_CTRL0         N48D2_R(0x21bfu)   /* 0x567f */
#define N48D2_DIG1_TMDS_CTL_BITS      N48D2_R(0x220fu)   /* 0x56cf */
#define N48D2_SYMCLKB_CLOCK_ENABLE    (N48D2_SEG1 + 0x00a1u)   /* 0x0161, BASE_IDX 1 (DCCG): the LIVE DP's symbol clock - FORBIDDEN to write, read before / after every op */
/* the DP side, READ ONLY: the health checks before / after every op */
#define N48D2_OTG0_CONTROL            N48D2_R(0x1b43u)   /* 0x5003 */
#define N48D2_OTG0_FRAME_COUNT        N48D2_R(0x1b4du)   /* 0x500d */
#define N48D2_DIG1_FE_CNTL            N48D2_R(0x21b7u)   /* 0x5677 */
#define N48D2_DIG1_BE_CLK_CNTL        N48D2_R(0x21dfu)   /* 0x569f */
#define N48D2_DIG1_BE_CNTL            N48D2_R(0x21e0u)   /* 0x56a0 */
#define N48D2_DIG1_BE_EN_CNTL         N48D2_R(0x21e1u)   /* 0x56a1 */
/* 0.0.634 (stage M4a item 4): the DP's PLANE side, READ ONLY - what the old six registers cannot see (the design's "Today's DP check (OTG0 counter plus DIG1) cannot see that"). All from dcn_4_1_0_offset.h; none is in any allowed list. */
#define N48D2_SEG3                    0x00009000u
#define N48D2_MPC_OUT0_MUX            (N48D2_SEG3 + 0x02f2u)   /* 0x92f2, BASE_IDX 3 */
#define N48D2_MPCC0_TOP_SEL           (N48D2_SEG3 + 0x0000u)   /* 0x9000, BASE_IDX 3 */
#define N48D2_MPCC0_BOT_SEL           (N48D2_SEG3 + 0x0001u)   /* 0x9001 */
#define N48D2_MPCC0_OPP_ID            (N48D2_SEG3 + 0x0002u)   /* 0x9002 */
#define N48D2_HUBP0_DCHUBP_CNTL       N48D2_R(0x05f4u)         /* 0x3ab4, BASE_IDX 2 */
#define N48D2_DPPCLK0_DTO_PARAM       (N48D2_SEG1 + 0x0099u)   /* 0x0159, BASE_IDX 1 */
#define N48D2_DPPCLK_CTRL             (N48D2_SEG1 + 0x00a8u)   /* 0x0168, BASE_IDX 1 (shared: bit 0 is DPPCLK0_EN) */
#define N48D2_DET0_CTRL               N48D2_R(0x04bbu)         /* 0x397b, BASE_IDX 2 */
#define N48D2_COMPBUF_CTRL            N48D2_R(0x04bau)         /* 0x397a, BASE_IDX 2 */
/* ---- 0.0.635 (M4c / M4d): the monitor B's plane. Every name is Linux's (dcn_4_1_0_offset.h), the absolute dword is written after it; native_disp2_test.cpp re-derives each from the header. ---- */
#define N48D2_DPPCLK2_DTO_PARAM                            (N48D2_SEG1 + 0x009bu)       /* 0x015b */
#define N48D2_DPP_TOP2_DPP_CONTROL                         N48D2_R(0x0f9bu)             /* 0x445b */
#define N48D2_DSCL2_SCL_MODE                               N48D2_R(0x0fdeu)             /* 0x449e */
#define N48D2_DSCL2_DSCL_2TAP_CONTROL                      N48D2_R(0x0fe1u)             /* 0x44a1 */
#define N48D2_DSCL2_SCL_HORZ_FILTER_SCALE_RATIO            N48D2_R(0x0fe3u)             /* 0x44a3 */
#define N48D2_DSCL2_SCL_HORZ_FILTER_INIT                   N48D2_R(0x0fe4u)             /* 0x44a4 */
#define N48D2_DSCL2_SCL_VERT_FILTER_SCALE_RATIO            N48D2_R(0x0fe7u)             /* 0x44a7 */
#define N48D2_DSCL2_SCL_VERT_FILTER_INIT                   N48D2_R(0x0fe8u)             /* 0x44a8 */
#define N48D2_DSCL2_OTG_H_BLANK                            N48D2_R(0x0ff2u)             /* 0x44b2 */
#define N48D2_DSCL2_OTG_V_BLANK                            N48D2_R(0x0ff3u)             /* 0x44b3 */
#define N48D2_DSCL2_RECOUT_SIZE                            N48D2_R(0x0ff5u)             /* 0x44b5 */
#define N48D2_DSCL2_MPC_SIZE                               N48D2_R(0x0ff6u)             /* 0x44b6 */
#define N48D2_DSCL2_LB_MEMORY_CTRL                         N48D2_R(0x0ff8u)             /* 0x44b8 */
#define N48D2_HUBP2_DCSURF_PRI_VIEWPORT_DIMENSION          N48D2_R(0x07a3u)             /* 0x3c63 */
#define N48D2_HUBP2_DCSURF_SEC_VIEWPORT_DIMENSION          N48D2_R(0x07a7u)             /* 0x3c67 */
#define N48D2_HUBP2_DCHUBP_REQ_SIZE_CONFIG                 N48D2_R(0x07aau)             /* 0x3c6a */
#define N48D2_HUBPREQ2_DCN_GLOBAL_TTU_CNTL                 N48D2_R(0x07dau)             /* 0x3c9a */
#define N48D2_HUBPREQ2_DCN_SURF0_TTU_CNTL0                 N48D2_R(0x07dbu)             /* 0x3c9b */
#define N48D2_HUBPREQ2_BLANK_OFFSET_0                      N48D2_R(0x07f3u)             /* 0x3cb3 */
#define N48D2_HUBPREQ2_PREFETCH_SETTINGS                   N48D2_R(0x07f7u)             /* 0x3cb7 */
#define N48D2_HUBPREQ2_PREFETCH_SETTINGS_C                 N48D2_R(0x07f8u)             /* 0x3cb8 */
#define N48D2_HUBPREQ2_VBLANK_PARAMETERS_0                 N48D2_R(0x07f9u)             /* 0x3cb9 */
#define N48D2_HUBPREQ2_DCSURF_SURFACE_PITCH                N48D2_R(0x07bfu)             /* 0x3c7f */
#define N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS      N48D2_R(0x07c2u)             /* 0x3c82 */
#define N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH N48D2_R(0x07c3u)             /* 0x3c83 */
#define N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL                 N48D2_R(0x07cbu)             /* 0x3c8b */
#define N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE       N48D2_R(0x07d4u)             /* 0x3c94 */
#define N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH  N48D2_R(0x07d5u)             /* 0x3c95 */
#define N48D2_MPCC2_MPCC_TOP_SEL                           (N48D2_SEG3 + 0x002au)       /* 0x902a */
#define N48D2_MPCC2_MPCC_BOT_SEL                           (N48D2_SEG3 + 0x002bu)       /* 0x902b */
#define N48D2_MPCC2_MPCC_OPP_ID                            (N48D2_SEG3 + 0x002cu)       /* 0x902c */
#define N48D2_MPCC2_MPCC_UPDATE_LOCK_SEL                   (N48D2_SEG3 + 0x002fu)       /* 0x902f */
#define N48D2_MPCC2_MPCC_STATUS                            (N48D2_SEG3 + 0x0038u)       /* 0x9038 */
#define N48D2_MPC_OUT2_MUX                                 (N48D2_SEG3 + 0x02fau)       /* 0x92fa */
#define N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL               N48D2_R(0x1aeau)             /* 0x4faa */
#define N48D2_OTG2_OTG_CRC_CNTL                            N48D2_R(0x1c65u)             /* 0x5125 */
#define N48D2_OTG2_OTG_CRC0_WINDOWA_X_CONTROL              N48D2_R(0x1c66u)             /* 0x5126 */
#define N48D2_OTG2_OTG_CRC0_WINDOWA_Y_CONTROL              N48D2_R(0x1c67u)             /* 0x5127 */
#define N48D2_OTG2_OTG_CRC0_WINDOWB_X_CONTROL              N48D2_R(0x1c68u)             /* 0x5128 */
#define N48D2_OTG2_OTG_CRC0_WINDOWB_Y_CONTROL              N48D2_R(0x1c69u)             /* 0x5129 */
#define N48D2_OTG2_OTG_CRC0_DATA_RG                        N48D2_R(0x1c6au)             /* 0x512a */
#define N48D2_OTG2_OTG_CRC0_DATA_B                         N48D2_R(0x1c6bu)             /* 0x512b */
#define N48D2_DCHUBBUB_DET2_CTRL                           N48D2_R(0x04bdu)             /* 0x397d */
#define N48D2_DCN_VM_FAULT_STATUS                          N48D2_R(0x05ccu)             /* 0x3a8c */
#define N48D2_OTG2_OTG_GLOBAL_CONTROL2                     N48D2_R(0x1c90u)             /* 0x5150 */
/* 0.0.643: OTG2's MASTER UPDATE LOCK (Linux optc3_lock / optc1_unlock / optc401_wait_update_lock_status, dcn401: .lock = optc3_lock, .unlock = optc1_unlock). regOTG2_OTG_MASTER_UPDATE_LOCK 0x1c89 BASE_IDX 2 = 0x5149; its
 * fields OTG_MASTER_UPDATE_LOCK [0] and UPDATE_LOCK_STATUS [8]. optc3_lock first REG_UPDATEs OTG_GLOBAL_CONTROL2.OTG_MASTER_UPDATE_LOCK_SEL [27:25] (OTG2_..._SEL_MASK 0x0E000000L, shift 0x19) to optc->inst = 2.
 * These TWO are the only writes 0.0.643 adds; both are written by op 8 alone (UPDATEs of exactly those fields, instance 2). OTG0 / OTG1 / OTG3's copies stay UNLISTED. */
#define N48D2_OTG2_OTG_MASTER_UPDATE_LOCK                  N48D2_R(0x1c89u)             /* 0x5149 */
#define N48D2_OTG_MASTER_UPDATE_LOCK_MASK                  0x00000001u
#define N48D2_OTG_UPDATE_LOCK_STATUS_MASK                  0x00000100u
#define N48D2_OTG_LOCK_SEL_MASK                            0x0E000000u
#define N48D2_OTG_LOCK_SEL_OTG2                            0x04000000u                  /* OTG_MASTER_UPDATE_LOCK_SEL = 2 (shift 25) */
#define N48D2_OTG_LOCK_SEL_OTG1                            0x02000000u                  /* 0.0.655: OTG_MASTER_UPDATE_LOCK_SEL = 1 (shift 25): OTG1_GLOBAL_CONTROL2 already reads 0x02000000 (mona-census-654), the update writes it back */
/* 0.0.638: the three RECORDED-READ registers (dcn_4_1_0_offset.h: regDCCG_GATE_DISABLE_CNTL 0x0074 BASE_IDX 1; regDCCG_GATE_DISABLE_CNTL6 0x00a9 BASE_IDX 1; regDOMAIN2_PG_STATUS 0x0085 BASE_IDX 2). READ ONLY: none is in any
 * writable list; the two DCCG ones lie in the forbidden DCCG span (only 0x15b and 0x168 are excepted), DOMAIN2_PG_STATUS is UNLISTED. The fourth recorded register is HUBP2_HUBP_CLK_CNTL (read via a K_REC step, never written by one). */
#define N48D2_DCCG_GATE_DISABLE_CNTL                       (N48D2_SEG1 + 0x0074u)       /* 0x0134 */
#define N48D2_DCCG_GATE_DISABLE_CNTL6                      (N48D2_SEG1 + 0x00a9u)       /* 0x0169 */
#define N48D2_DOMAIN2_PG_STATUS                            N48D2_R(0x0085u)             /* 0x3545 */

/* ---- 0.0.655: instance 1's (the monitor A's) PLANE registers - the pipe-1 twins of the monitor B's, at pipe 0 + 0xdc (HUBP / HUBPREQ / HUBPRET), + 0x16b (DPP) and + 0x15 (MPCC); every offset and BASE_IDX is re-derived BY NAME from dcn_4_1_0_offset.h by
 * tests/native_disp2_test.cpp (the Linux names are the macro names without the N48D2_ prefix, except the four HUBPREQ1_VM_* which are HUBPREQ1_DCN_VM_SYSTEM_APERTURE_LOW_ADDR / _HIGH_ADDR, HUBPREQ1_DCN_VM_MX_L1_TLB_CNTL and HUBPREQ1_VMID_SETTINGS_0).
 * DENTIST_DISPCLK_CNTL, DOMAIN1_PG_STATUS, DCHUBBUB_DET1_CTRL, the CRC DATA registers, MPCC1_STATUS and HUBPREQ1's EARLIEST_INUSE / FLIP_CONTROL are READ only. ---- */
#define N48D2_DPP_TOP1_DPP_CONTROL                             N48D2_R(0x0e30u)               /* 0x42f0 */
#define N48D2_DSCL1_SCL_MODE                                   N48D2_R(0x0e73u)               /* 0x4333 */
#define N48D2_DSCL1_DSCL_2TAP_CONTROL                          N48D2_R(0x0e76u)               /* 0x4336 */
#define N48D2_DSCL1_SCL_HORZ_FILTER_SCALE_RATIO                N48D2_R(0x0e78u)               /* 0x4338 */
#define N48D2_DSCL1_SCL_HORZ_FILTER_INIT                       N48D2_R(0x0e79u)               /* 0x4339 */
#define N48D2_DSCL1_SCL_VERT_FILTER_SCALE_RATIO                N48D2_R(0x0e7cu)               /* 0x433c */
#define N48D2_DSCL1_SCL_VERT_FILTER_INIT                       N48D2_R(0x0e7du)               /* 0x433d */
#define N48D2_DSCL1_OTG_H_BLANK                                N48D2_R(0x0e87u)               /* 0x4347 */
#define N48D2_DSCL1_OTG_V_BLANK                                N48D2_R(0x0e88u)               /* 0x4348 */
#define N48D2_DSCL1_RECOUT_SIZE                                N48D2_R(0x0e8au)               /* 0x434a */
#define N48D2_DSCL1_MPC_SIZE                                   N48D2_R(0x0e8bu)               /* 0x434b */
#define N48D2_DSCL1_LB_MEMORY_CTRL                             N48D2_R(0x0e8du)               /* 0x434d */
#define N48D2_HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION              N48D2_R(0x06c7u)               /* 0x3b87 */
#define N48D2_HUBP1_DCSURF_SEC_VIEWPORT_DIMENSION              N48D2_R(0x06cbu)               /* 0x3b8b */
#define N48D2_HUBP1_DCHUBP_REQ_SIZE_CONFIG                     N48D2_R(0x06ceu)               /* 0x3b8e */
#define N48D2_HUBPREQ1_DCSURF_SURFACE_PITCH                    N48D2_R(0x06e3u)               /* 0x3ba3 */
#define N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS          N48D2_R(0x06e6u)               /* 0x3ba6 */
#define N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH     N48D2_R(0x06e7u)               /* 0x3ba7 */
#define N48D2_HUBPREQ1_DCSURF_FLIP_CONTROL                     N48D2_R(0x06efu)               /* 0x3baf */
#define N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE           N48D2_R(0x06f8u)               /* 0x3bb8 */
#define N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_HIGH      N48D2_R(0x06f9u)               /* 0x3bb9 */
#define N48D2_HUBPREQ1_DCN_TTU_QOS_WM                          N48D2_R(0x06fdu)               /* 0x3bbd */
#define N48D2_HUBPREQ1_DCN_GLOBAL_TTU_CNTL                     N48D2_R(0x06feu)               /* 0x3bbe */
#define N48D2_HUBPREQ1_DCN_SURF0_TTU_CNTL0                     N48D2_R(0x06ffu)               /* 0x3bbf */
#define N48D2_HUBPREQ1_BLANK_OFFSET_0                          N48D2_R(0x0717u)               /* 0x3bd7 */
#define N48D2_HUBPREQ1_BLANK_OFFSET_1                          N48D2_R(0x0718u)               /* 0x3bd8 */
#define N48D2_HUBPREQ1_DST_DIMENSIONS                          N48D2_R(0x0719u)               /* 0x3bd9 */
#define N48D2_HUBPREQ1_PREFETCH_SETTINGS                       N48D2_R(0x071bu)               /* 0x3bdb */
#define N48D2_HUBPREQ1_PREFETCH_SETTINGS_C                     N48D2_R(0x071cu)               /* 0x3bdc */
#define N48D2_HUBPREQ1_VBLANK_PARAMETERS_0                     N48D2_R(0x071du)               /* 0x3bdd */
#define N48D2_HUBPREQ1_REF_FREQ_TO_PIX_FREQ                    N48D2_R(0x0730u)               /* 0x3bf0 */
#define N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL                   N48D2_R(0x1adau)               /* 0x4f9a */
#define N48D2_OTG1_OTG_CRC_CNTL                                N48D2_R(0x1be5u)               /* 0x50a5 */
#define N48D2_OTG1_OTG_CRC0_WINDOWA_X_CONTROL                  N48D2_R(0x1be6u)               /* 0x50a6 */
#define N48D2_OTG1_OTG_CRC0_WINDOWA_Y_CONTROL                  N48D2_R(0x1be7u)               /* 0x50a7 */
#define N48D2_OTG1_OTG_CRC0_WINDOWB_X_CONTROL                  N48D2_R(0x1be8u)               /* 0x50a8 */
#define N48D2_OTG1_OTG_CRC0_WINDOWB_Y_CONTROL                  N48D2_R(0x1be9u)               /* 0x50a9 */
#define N48D2_OTG1_OTG_CRC0_DATA_RG                            N48D2_R(0x1beau)               /* 0x50aa */
#define N48D2_OTG1_OTG_CRC0_DATA_B                             N48D2_R(0x1bebu)               /* 0x50ab */
#define N48D2_OTG1_OTG_MASTER_UPDATE_LOCK                      N48D2_R(0x1c09u)               /* 0x50c9 */
#define N48D2_OTG1_OTG_GLOBAL_CONTROL2                         N48D2_R(0x1c10u)               /* 0x50d0 */
#define N48D2_DCHUBBUB_DET1_CTRL                               N48D2_R(0x04bcu)               /* 0x397c */
#define N48D2_DOMAIN1_PG_STATUS                                N48D2_R(0x0083u)               /* 0x3543 */
#define N48D2_HUBPREQ1_VM_APERTURE_LOW                         N48D2_R(0x0708u)               /* 0x3bc8 */
#define N48D2_HUBPREQ1_VM_APERTURE_HIGH                        N48D2_R(0x0709u)               /* 0x3bc9 */
#define N48D2_HUBPREQ1_VM_MX_L1_TLB                            N48D2_R(0x0716u)               /* 0x3bd6 */
#define N48D2_HUBPREQ1_VMID_SETTINGS_0                         N48D2_R(0x06e5u)               /* 0x3ba5 */
#define N48D2_MPCC1_MPCC_TOP_SEL                               (N48D2_SEG3 + 0x0015u)         /* 0x9015 */
#define N48D2_MPCC1_MPCC_BOT_SEL                               (N48D2_SEG3 + 0x0016u)         /* 0x9016 */
#define N48D2_MPCC1_MPCC_OPP_ID                                (N48D2_SEG3 + 0x0017u)         /* 0x9017 */
#define N48D2_MPCC1_MPCC_UPDATE_LOCK_SEL                       (N48D2_SEG3 + 0x001au)         /* 0x901a */
#define N48D2_MPCC1_MPCC_STATUS                                (N48D2_SEG3 + 0x0023u)         /* 0x9023 */
#define N48D2_MPC_OUT1_MUX                                     (N48D2_SEG3 + 0x02f6u)         /* 0x92f6 */
#define N48D2_DPPCLK1_DTO_PARAM                                (N48D2_SEG1 + 0x009au)         /* 0x015a */
#define N48D2_DENTIST_DISPCLK_CNTL                             (N48D2_SEG1 + 0x0064u)         /* 0x0124 */

/* 0.0.635: the watch's six NEW reads (pipe 0 / the DP side), the OTG0 raster for the precheck, the pipe-0 MPCC / MCM / OGAM twins; ALL READ ONLY (the guard refuses a write to each: forbidden span or unlisted) */
#define N48D2_ODM0_OPTC_INPUT_GLOBAL_CONTROL N48D2_R(0x1acau)   /* 0x4f8a */
#define N48D2_DPP_TOP0_DPP_CONTROL     N48D2_R(0x0cc5u)         /* 0x4185 */
#define N48D2_MPCC0_UPDATE_LOCK_SEL    (N48D2_SEG3 + 0x0005u)   /* 0x9005 */
#define N48D2_HUBPREQ0_PRIMARY_LOW     N48D2_R(0x060au)         /* 0x3aca  HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS */
#define N48D2_HUBPREQ0_PRIMARY_HIGH    N48D2_R(0x060bu)         /* 0x3acb  HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH */
#define N48D2_HUBPREQ0_EARLIEST_LOW    N48D2_R(0x061cu)         /* 0x3adc  HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE (0.0.636: READ ONLY, the op-8 precheck's overlap test; inside the forbidden HUBPREQ0 span for writes) */
#define N48D2_HUBPREQ0_SURFACE_PITCH   N48D2_R(0x0607u)         /* 0x3ac7  HUBPREQ0_DCSURF_SURFACE_PITCH (0.0.637: READ ONLY, sizes the DP surface for the overlap test) */
#define N48D2_HUBP0_PRI_VIEWPORT_DIMENSION N48D2_R(0x05ebu)     /* 0x3aab  HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION (0.0.637: READ ONLY; also a COPY0 source) */
#define N48D2_HUBPREQ0_EARLIEST_HIGH   N48D2_R(0x061du)         /* 0x3add  HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH */
#define N48D2_HUBPREQ2_VM_APERTURE_LOW  N48D2_R(0x07e4u)        /* 0x3ca4 */
#define N48D2_HUBPREQ2_VM_APERTURE_HIGH N48D2_R(0x07e5u)        /* 0x3ca5 */
#define N48D2_HUBPREQ2_VM_MX_L1_TLB    N48D2_R(0x07f2u)         /* 0x3cb2 */
#define N48D2_HUBPREQ2_VMID_SETTINGS_0 N48D2_R(0x07c1u)         /* 0x3c81 */
#define N48D2_OTG0_H_TOTAL             N48D2_R(0x1b2au)         /* 0x4fea */
#define N48D2_OTG0_V_TOTAL             N48D2_R(0x1b2fu)         /* 0x4fef */
#define N48D2_OTG0_VSTARTUP_PARAM      N48D2_R(0x1b85u)         /* 0x5045 */
#define N48D2_OTG0_VUPDATE_PARAM       N48D2_R(0x1b86u)         /* 0x5046 */
#define N48D2_OTG0_VREADY_PARAM        N48D2_R(0x1b87u)         /* 0x5047 */

// ---- fields (dcn_4_1_0_sh_mask.h, instance-0 names) ---------------------------------------------------------------------------------------------------------------------------------------------------------
#define N48D2_VTG_FP2_MASK            0x00007FFFu        /* VTG0_CONTROL__VTG0_FP2 */
#define N48D2_VTG_VCOUNT_INIT_MASK    0x7FFF0000u        /* VTG0_CONTROL__VTG0_VCOUNT_INIT, shift 16 */
#define N48D2_VTG_ENABLE_MASK         0x80000000u        /* VTG0_CONTROL__VTG0_ENABLE */
#define N48D2_ODM_NUM_IN_SEG_MASK     0x00000003u        /* OPTC_NUM_OF_INPUT_SEGMENT */
#define N48D2_ODM_SEG0_MASK           0x000F0000u        /* OPTC_SEG0_SRC_SEL, shift 16 */
#define N48D2_ODM_SEG123_MASK         0xFFF00000u        /* OPTC_SEG1/2/3_SRC_SEL, shifts 20 / 24 / 28 */
#define N48D2_ODM_DATA_FORMAT_MASK    0x00000003u        /* OPTC_DATA_FORMAT */
#define N48D2_ODM_CLK_GATE_DIS_MASK   0x00000001u        /* OPTC_INPUT_CLK_GATE_DIS */
#define N48D2_ODM_CLK_EN_MASK         0x00000002u        /* OPTC_INPUT_CLK_EN */
#define N48D2_ODM_CLK_ON_MASK         0x00000004u        /* OPTC_INPUT_CLK_ON */
#define N48D2_ODM_MEM_SEL_MASK        0x0000FFFFu        /* OPTC_MEM_SEL */
#define N48D2_OTG_CLOCK_EN_MASK       0x00000001u        /* OTG_CLOCK_EN */
#define N48D2_OTG_CLOCK_GATE_DIS_MASK 0x00000002u        /* OTG_CLOCK_GATE_DIS */
#define N48D2_OTG_CLOCK_ON_MASK       0x00000100u        /* OTG_CLOCK_ON */
#define N48D2_OTG_BUSY_MASK           0x00010000u        /* OTG_BUSY */
#define N48D2_LO15                    0x00007FFFu        /* every *_START / *_TOTAL field */
#define N48D2_HI15                    0x7FFF0000u        /* every *_END field, shift 16 */
#define N48D2_SYNC_POL_MASK           0x00000001u        /* OTG_H_SYNC_A_POL / OTG_V_SYNC_A_POL (0 = positive) */
#define N48D2_INTERLACE_EN_MASK       0x00000001u
#define N48D2_OTG_MASTER_EN_MASK      0x00000001u
#define N48D2_OTG_DISABLE_POINT_MASK  0x00000300u        /* OTG_DISABLE_POINT_CNTL, shift 8 */
#define N48D2_OTG_START_POINT_MASK    0x00001000u        /* OTG_START_POINT_CNTL */
#define N48D2_OTG_FIELD_NUMBER_MASK   0x00002000u        /* OTG_FIELD_NUMBER_CNTL */
#define N48D2_OTG_CUR_MASTER_EN_MASK  0x00010000u        /* OTG_CURRENT_MASTER_EN_STATE */
#define N48D2_OTG_OUT_MUX_MASK        0x00300000u        /* OTG_OUT_MUX (0 = DIO) */
#define N48D2_H_DIV_MODE_MASK         0x00000003u        /* OTG_H_TIMING_DIV_MODE */
#define N48D2_H_DIV_MANUAL_MASK       0x00000100u        /* OTG_H_TIMING_DIV_MODE_MANUAL */
#define N48D2_VSTARTUP_MASK           0x000003FFu
#define N48D2_VUPDATE_OFFSET_MASK     0x0000FFFFu
#define N48D2_VUPDATE_WIDTH_MASK      0x03FF0000u        /* shift 16 */
#define N48D2_VREADY_MASK             0x0000FFFFu
#define N48D2_FMT_LEFT_EDGE_MASK      0x00000001u
#define N48D2_DPG_EN_MASK             0x00000001u
#define N48D2_DPG_MODE_MASK           0x00000070u        /* shift 4 */
#define N48D2_DPG_DYN_RANGE_MASK      0x00000100u
#define N48D2_DPG_BIT_DEPTH_MASK      0x00003000u        /* shift 12 */
#define N48D2_DPG_VRES_MASK           0x000F0000u        /* shift 16 */
#define N48D2_DPG_HRES_MASK           0x00F00000u        /* shift 20 */
#define N48D2_DPG_PENDING_MASK        0x00000001u        /* DPG_STATUS__DPG_DOUBLE_BUFFER_PENDING */
#define N48D2_OPP_PIPE_CLOCK_EN_MASK  0x00000001u
#define N48D2_DIG_SOURCE_SELECT_MASK  0x00000007u        /* DIG_FE_CNTL__DIG_SOURCE_SELECT (the OTG) */
#define N48D2_DIG_STEREOSYNC_SEL_MASK 0x00000070u        /* shift 4 */
#define N48D2_DIG_STEREOSYNC_GATE_MASK 0x00000100u
#define N48D2_DIG_FE_MODE_MASK        0x00000007u        /* DIG_FE_CLK_CNTL__DIG_FE_MODE (2 = TMDS-DVI) */
#define N48D2_DIG_FE_CLK_EN_MASK      0x00000010u
#define N48D2_DIG_FE_SYMCLK_ON_MASK   0x00000800u        /* DIG_FE_SYMCLK_FE_G_CLOCK_ON */
#define N48D2_DIG_FE_ENABLE_MASK      0x00000001u
#define N48D2_DIG_CLOCK_PATTERN_MASK  0x000003FFu
#define N48D2_FIFO_ENABLE_MASK        0x00000001u
#define N48D2_FIFO_RESET_MASK         0x00000002u
#define N48D2_FIFO_READ_START_MASK    0x0000007Cu        /* shift 2 */
#define N48D2_FIFO_PIX_PER_CYCLE_MASK 0x00000300u        /* shift 8 */
#define N48D2_FIFO_RESET_DONE_MASK    0x00100000u
#define N48D2_FIFO_ERROR_MASK         0x30000000u
#define N48D2_TMDS_PIXEL_ENCODING_MASK 0x00001000u
#define N48D2_TMDS_COLOR_FORMAT_MASK  0x00006000u
#define N48D2_BE_MODE_MASK            0x00000007u        /* DIG_BE_CLK_CNTL__DIG_BE_MODE (2 = TMDS-DVI) */
#define N48D2_BE_CLK_EN_MASK          0x00000010u
#define N48D2_BE_FE_SOURCE_MASK       0x00007F00u        /* DIG_BE_CNTL__DIG_FE_SOURCE_SELECT, shift 8 */
#define N48D2_BE_FE2_BIT              0x00000400u        /* FE source 0x04 << 8: DCN10_DIG_FE_SOURCE_SELECT_DIGC (dcn10_link_encoder.c) */
#define N48D2_BE_ENABLE_MASK          0x00000001u
#define N48D2_MAPPER_LINK_MASK        0x00000007u        /* DIG_STREAM_LINK_TARGET */
#define N48D2_PHYPLL_SOURCE_MASK      0x00000007u        /* OTG1_PHYPLL_PIXEL_RATE_SOURCE */
#define N48D2_BE_FE3_BIT              0x00000800u        /* FE source 0x08 << 8: DCN10_DIG_FE_SOURCE_SELECT_DIGD (dcn10_link_encoder.c) - instance 2 */
/* the symbol clock exception (0.0.633): dccg401_enable_symclk_se(stream_enc_inst, link_enc_inst) = REG_UPDATE_2(SYMCLK<n>_CLOCK_ENABLE, FE_EN, 1, FE_SRC_SEL, link_enc_inst), n = the stream encoder's letter, link_enc_inst =
 * transmitter - TRANSMITTER_UNIPHY_A (hwss dcn20_hwseq.c / dcn401_hwseq.c). SYMCLK*_CLOCK_ENABLE: CLOCK_ENABLE [0] (the PHY's own: NEVER written here), FE_EN [4], FE_SRC_SEL [10:8]. */
#define N48D2_SYMCLK_FE_EN_MASK       0x00000010u
#define N48D2_SYMCLK_FE_SRC_MASK      0x00000700u
#define N48D2_SYMCLK_FE_MASK          (N48D2_SYMCLK_FE_EN_MASK | N48D2_SYMCLK_FE_SRC_MASK)   /* 0x710: the only bits the exception ever touches */

/* 0.0.635 fields (dcn_4_1_0_sh_mask.h: HUBP0_DCHUBP_CNTL / HUBP0_HUBP_CLK_CNTL / HUBPREQ0_DCSURF_FLIP_CONTROL / DCHUBBUB_DET0_CTRL / MPCC0_MPCC_STATUS / ODM0_OPTC_INPUT_GLOBAL_CONTROL / DPPCLK_CTRL / OTG0_OTG_CRC_CNTL; the instance-0 names) */
#define N48D2_HUBP_BLANK_EN_MASK      0x00000001u        /* HUBP_BLANK_EN */
#define N48D2_HUBP_UNDERFLOW_MASK 0x70000000u   /* HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_STATUS_MASK, bits 30:28 */
#define N48D2_HUBP_NO_OUTSTANDING_MASK 0x00000002u       /* HUBP_NO_OUTSTANDING_REQ */
#define N48D2_HUBP_SEG_ALLOC_ERR_MASK 0x00000800u        /* HUBP_SEG_ALLOC_ERR_STATUS */
#define N48D2_HUBP_TIMEOUT_MASK       0x00F00000u        /* HUBP_TIMEOUT_STATUS */
#define N48D2_HUBP_UNDERFLOW_CLEAR_MASK 0x80000000u      /* HUBP_UNDERFLOW_CLEAR */
#define N48D2_HUBP_CNTL_WRITE_MASK    0x80000001u        /* the ONLY bits a step on HUBP2_DCHUBP_CNTL may name: BLANK_EN and UNDERFLOW_CLEAR */
#define N48D2_HUBP_CLK_WRITE_MASK     0x00011111u        /* HUBP's writable clock bits: CLOCK_ENABLE [0], R/G gate disables [4] [8] [12] [16] (HUBP0 reads 0x00f11111) */
#define N48D2_HUBP_CLK_ON_MASK        0x00F00000u        /* status bits 23:20 (R/G clocks ON) all 1 = the plane's clocks run */
#define N48D2_HUBP_CLK_OFF_MASK       0x00A00000u        /* DPPCLK_G [21] and DCFCLK_G [23] clock-on status: 0 = gated again (bits 20 and 22 are the register clocks, ON by default) */
#define N48D2_DPPCLK2_EN_MASK         0x00000040u        /* DPPCLK_CTRL__DPPCLK2_EN (DPPCLK0_EN 0, DPPCLK1_EN 3, DPPCLK2_EN 6, DPPCLK3_EN 9) */
#define N48D2_DPP_CLOCK_ENABLE_MASK   0x00000010u        /* DPP_TOP0_DPP_CONTROL__DPP_CLOCK_ENABLE */
#define N48D2_DPPCLK_DTO_VALUE        0x00ff00ffu        /* DPPCLK0_DTO_PARAM's live value (phase 255 / modulo 255 = 1) */
#define N48D2_FLIP_PENDING_MASK       0x00000100u        /* HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING */
#define N48D2_DET_SIZE_MASK           0x0000001Fu
#define N48D2_DET_CUR_MASK            0x00001F00u        /* DETn_SIZE_CURRENT, shift 8 */
#define N48D2_MPCC_STATUS_MASK        0x00000007u        /* IDLE | BUSY | DISABLED */
#define N48D2_MPCC_STATUS_OFF         0x00000005u        /* IDLE and DISABLED (MPCC2_STATUS reads 5 in the census) */
#define N48D2_ODM_UNDERFLOW_MASK      0x00002400u        /* OPTC_UNDERFLOW_OCCURRED_STATUS [10] and OPTC_UNDERFLOW_OCCURRED_CURRENT [13] */
#define N48D2_ODM_UNDERFLOW_CLEAR_MASK 0x00001000u       /* OPTC_UNDERFLOW_CLEAR (bit 0, the soft reset, is NEVER written) */
#define N48D2_CRC_CNTL_MASK           0x00700011u        /* OTG_CRC_EN [0], OTG_CRC_CONT_EN [4], OTG_CRC0_SELECT [22:20] (optc1_configure_crc / crc_enable) */
#define N48D2_CRC_CNTL_ON             0x00000011u
#define N48D2_MPCC_NONE               0x0000000Fu        /* MPCC_TOP_SEL / BOT_SEL / OPP_ID / UPDATE_LOCK_SEL 0xf = none */
#define N48D2_MPC_OUT2_MUX_ON         0x00004102u        /* MPC_OUT_MUX 2 (OPP2's input is MPCC2) + MPC_OUT_RATE_CONTROL_DISABLE [8] + the flow-control count OUT0 shows (0x4100 + mux) */
#define N48D2_MPC_OUT2_MUX_OFF        0x0000400fu        /* the census value: mux 0xf */
#define N48D2_PITCH_PX                0x000009ffu        /* DCSURF_SURFACE_PITCH: 2560 - 1 */
#define N48D2_PLANE_W                 2560u
#define N48D2_PLANE_H                 1440u
#define N48D2_PLANE_PITCH_BYTES       10240u
#define N48D2_BUF_BYTES               14745600u          /* 2560 x 1440 x 4 = 225 x 64 KiB */
#define N48D2_BAR_PX                  320u               /* eight vertical bars */
#define N48D2_CRC_WIN_X               0x0A000000u        /* (2560 << 16): WINDOWA / WINDOWB X end */
#define N48D2_CRC_WIN_Y               0x05A00000u        /* (1440 << 16) */
#define N48D2_RASTER_HTOTAL_RAW       2719u              /* OTG_H_TOTAL / OTG_V_TOTAL raw values of the 1440p60 raster (h_total 2720, v_total 1481, each minus one) */
#define N48D2_RASTER_VTOTAL_RAW       1480u
/* ---- 0.0.655: the monitor A's surface (1920x1080x4, pitch 0x77f, eight 240-px bars). 1920 x 1080 x 4 = 8,294,400 B is NOT a multiple of 64 KiB (the allocator refuses that): the allocation is 127 x 64 KiB = 8,323,072 B, the frame inside it is 8,294,400 B ---- */
#define N48D1_PITCH_PX                0x0000077fu        /* DCSURF_SURFACE_PITCH: 1920 - 1 */
#define N48D1_PLANE_W                 1920u
#define N48D1_PLANE_H                 1080u
#define N48D1_PLANE_PITCH_BYTES       7680u
#define N48D1_FRAME_BYTES             8294400u
#define N48D1_BUF_BYTES               8323072u           /* 127 x 64 KiB */
#define N48D1_BAR_PX                  240u
#define N48D1_CRC_WIN_X               0x07800000u        /* (1920 << 16) */
#define N48D1_CRC_WIN_Y               0x04380000u        /* (1080 << 16) */
#define N48D1_VIEW_DIM                0x04380780u        /* (1080 << 16) | 1920: the viewports, RECOUT and MPC_SIZE */
#define N48D2_DPPCLK1_EN_MASK         0x00000008u        /* DPPCLK_CTRL__DPPCLK1_EN (bit 3n) */
#define N48D2_MPC_OUT1_MUX_ON         0x00004101u        /* MPC_OUT_MUX 1 (OPP1's input is MPCC1) + MPC_OUT_RATE_CONTROL_DISABLE [8] + the 0x4100 flow-control count */
#define N48D2_MPC_OUT1_MUX_OFF        0x0000400fu        /* the census value (mona-census-654): mux 0xf */

#define N48D2_OTG1_INST 1u                               /* OTG1 = OPP1 = DIG2's source (ODM bypass 1:1, optc401_set_odm_bypass) */
#define N48D2_LINK_UNIPHY_C 2u                           /* TRANSMITTER_UNIPHY_C - TRANSMITTER_UNIPHY_A */
#define N48D2_OTG2_INST 2u                               /* OTG2 = OPP2 = DIG3's source (instance 2) */
#define N48D2_LINK_UNIPHY_D 3u                           /* TRANSMITTER_UNIPHY_D - TRANSMITTER_UNIPHY_A */
#define N48D2_SYMCLKC_FE_VALUE ((N48D2_LINK_UNIPHY_C << 8) | N48D2_SYMCLK_FE_EN_MASK)   /* 0x210: instance 1 (stream encoder 2, link encoder 2) */
#define N48D2_SYMCLKD_FE_VALUE ((N48D2_LINK_UNIPHY_D << 8) | N48D2_SYMCLK_FE_EN_MASK)   /* 0x310: instance 2 (stream encoder 3, link encoder 3) */
#define N48D2_PHYPLL_PLL3 3u                             /* SUSPECTED: the PLL number the DMUB `pclk otg2-on` (d2 pll_id 0x17 = ATOM_COMBOPHY_PLL3) leaves in OTG2_PHYPLL_PIXEL_RATE_CNTL, by analogy with PLL2 -> 2 after `pclk otg1-on` */
#define N48D2_PHYPLL_PLL2 2u                             /* measured after `dmubsend pclk otg1-on` () */
#define N48D2_DVI_MODE 2u                                /* DIG FE / BE mode TMDS-DVI (enc401_stream_encoder_enable / dcn401_link_encoder_setup) */

// ---- the timing: the monitor A SINK-B's EDID detailed timing descriptor (notes/logs/runs/m1-622/ddc.txt, line 2 block 0, bytes 0x36..0x47) ---------------------------------------------------------------------
static const uint8_t n48d2_mona_dtd[18] = { 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0x09, 0x25, 0x21, 0x00, 0x00, 0x1e };
// 0.0.634 (M4b): the monitor B SINK-C's 2560x1440@60 detailed timing, CTA extension block (notes/logs/runs/m1-622/ddc.txt line 3 block 1, bytes 0x64..0x75 = `56 5e 00 a0 a0 a0 29 50 30 20 35 00 55 50 21 00 00 1a`):
// 241.50 MHz, 2560 + 160 = 2720, fp 48 / sync 32 / bp 80, 1440 + 41 = 1481, fp 3 / sync 5 / bp 33, hsync + / vsync -. n48d2_dtd_decode decodes it; the host test re-derives every field by hand.
static const uint8_t n48d2_monb_dtd1440[18] = { 0x56, 0x5e, 0x00, 0xa0, 0xa0, 0xa0, 0x29, 0x50, 0x30, 0x20, 0x35, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1a };
struct n48d2_timing {
    uint32_t pix_clk_10khz;
    uint32_t h_active, h_blank, h_total, h_fp, h_sync, h_bp;
    uint32_t v_active, v_blank, v_total, v_fp, v_sync, v_bp;
    uint32_t h_border, v_border, interlace, digital_separate, h_pos, v_pos;   /* *_pos: 1 = positive polarity */
};
// 0 = decoded; 1 = not a detailed timing (pixel clock 0) or not digital separate sync. EDID 1.4 section 3.10.2.
static inline uint32_t n48d2_dtd_decode(const uint8_t d[18], struct n48d2_timing *t)
{
    unsigned i;
    for (i = 0; i < sizeof(*t); i++) ((uint8_t *)t)[i] = 0;
    t->pix_clk_10khz = (uint32_t)d[0] | ((uint32_t)d[1] << 8);
    t->h_active = (uint32_t)d[2] | ((uint32_t)(d[4] >> 4) << 8);
    t->h_blank  = (uint32_t)d[3] | ((uint32_t)(d[4] & 0xFu) << 8);
    t->v_active = (uint32_t)d[5] | ((uint32_t)(d[7] >> 4) << 8);
    t->v_blank  = (uint32_t)d[6] | ((uint32_t)(d[7] & 0xFu) << 8);
    t->h_fp     = (uint32_t)d[8] | ((uint32_t)((d[11] >> 6) & 3u) << 8);
    t->h_sync   = (uint32_t)d[9] | ((uint32_t)((d[11] >> 4) & 3u) << 8);
    t->v_fp     = (uint32_t)(d[10] >> 4) | ((uint32_t)((d[11] >> 2) & 3u) << 4);
    t->v_sync   = (uint32_t)(d[10] & 0xFu) | ((uint32_t)(d[11] & 3u) << 4);
    t->h_border = d[15]; t->v_border = d[16];
    t->interlace = (d[17] >> 7) & 1u;
    t->digital_separate = ((d[17] >> 3) & 3u) == 3u;
    t->v_pos = (d[17] >> 2) & 1u;
    t->h_pos = (d[17] >> 1) & 1u;
    t->h_total = t->h_active + t->h_blank; t->v_total = t->v_active + t->v_blank;
    t->h_bp = t->h_blank - t->h_fp - t->h_sync; t->v_bp = t->v_blank - t->v_fp - t->v_sync;
    return (t->pix_clk_10khz != 0u && t->digital_separate) ? 0u : 1u;
}
// The values Linux's optc1_program_timing / optc1_set_vtg_params compute from a timing (no borders, progressive, TMDS: start point 0, h_div none), plus the global sync this card's GOP chose for OTG0
// (VSTARTUP 13 lines, VUPDATE offset 680 / width 320 pixels, VREADY 300 pixels: notes/logs, OTG0 VSTARTUP_PARAM 0x5045 = 0xd, VUPDATE 0x014002a8, VREADY 0x12c) - Linux takes them from DML, which we do not run.
#define N48D2_VSTARTUP_LINES 13u
#define N48D2_VUPDATE_OFFSET 680u
#define N48D2_VUPDATE_WIDTH 320u
#define N48D2_VREADY_OFFSET 300u
struct n48d2_otg_regs {
    uint32_t h_total, h_sync_a, h_blank, h_pol, v_total, v_sync_a, v_blank, v_pol, vstartup, vupdate, vready, vtg_vcount_init, vtg_fp2;
    uint32_t h_div2;     /* is_h_timing_divisible_by_2: 1 = OTG_H_TIMING_DIV_MODE_MANUAL stays 0 */
};
static inline uint32_t n48d2_otg_compute(const struct n48d2_timing *t, struct n48d2_otg_regs *r)
{
    uint32_t vfp, hbs, hbe, vbs, vbe;
    int32_t vls;
    unsigned i;
    for (i = 0; i < sizeof(*r); i++) ((uint8_t *)r)[i] = 0;
    if (t->interlace || t->h_border || t->v_border || t->h_total == 0u || t->v_total == 0u) return 1u;
    vfp = t->v_fp < 1u ? 1u : t->v_fp;                                  /* apply_front_porch_workaround (progressive) */
    hbs = t->h_total - t->h_fp; hbe = hbs - t->h_active;                /* asic_blank_start / _end */
    vbs = t->v_total - vfp;     vbe = vbs - t->v_active;
    r->h_total = t->h_total - 1u;
    r->h_sync_a = (t->h_sync << 16) | 0u;                               /* OTG_H_SYNC_A_START 0, _END h_sync_width */
    r->h_blank = (hbe << 16) | hbs;
    r->h_pol = t->h_pos ? 0u : 1u;
    r->v_total = t->v_total - 1u;
    r->v_sync_a = (t->v_sync << 16) | 0u;
    r->v_blank = (vbe << 16) | vbs;
    r->v_pol = t->v_pos ? 0u : 1u;
    r->vstartup = N48D2_VSTARTUP_LINES;
    r->vupdate = (N48D2_VUPDATE_WIDTH << 16) | N48D2_VUPDATE_OFFSET;
    r->vready = N48D2_VREADY_OFFSET;
    r->vtg_vcount_init = vbs;                                           /* optc1_set_vtg_params: v_init = v_total - v_front_porch */
    vls = (int32_t)vbe - (int32_t)N48D2_VSTARTUP_LINES + 1;
    r->vtg_fp2 = vls < 0 ? (uint32_t)(-vls) : 0u;
    r->h_div2 = ((t->h_total | t->h_active | t->h_fp | t->h_sync) & 1u) == 0u ? 1u : 0u;
    return 0u;
}

// ---- the DPG pattern: CONTROLLER_DP_TEST_PATTERN_COLORSQUARES, RGB, VESA range, 8 bpc (opp2_set_disp_pattern_generator; the pattern Linux's visual_confirm blank draws) --------------------------------
#define N48D2_DPG_MODE_COLORSQUARES_RGB 0u   /* TEST_PATTERN_MODE_COLORSQUARES_RGB (hw_shared.h) */
#define N48D2_DPG_BPC8 1u                     /* TEST_PATTERN_COLOR_FORMAT_BPC_8 */
#define N48D2_DPG_CONTROL_MASK (N48D2_DPG_EN_MASK | N48D2_DPG_MODE_MASK | N48D2_DPG_DYN_RANGE_MASK | N48D2_DPG_BIT_DEPTH_MASK | N48D2_DPG_VRES_MASK | N48D2_DPG_HRES_MASK)
#define N48D2_DPG_CONTROL_ON (1u | (N48D2_DPG_MODE_COLORSQUARES_RGB << 4) | (0u << 8) | (N48D2_DPG_BPC8 << 12) | (6u << 16) | (6u << 20))   /* 0x00661001 */

// ---- the steps ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// SET: write val (REG_SET: fields not named are 0).  UPD: read, (old & ~mask) | (val & mask), write (REG_UPDATE).  WAIT: read until (r & mask) == val, at most polls reads step_us apart (REG_WAIT; a timeout is
// counted and the sequence CONTINUES, as Linux's does).  WAITIF: Linux's enc35_reset_fifo: if (rd(cond) & cond_mask) WAIT as above, else delay step_us once.  SLEEP: sleep val ms.
enum { N48D2_K_SET = 1, N48D2_K_UPD = 2, N48D2_K_WAIT = 3, N48D2_K_WAITIF = 4, N48D2_K_COPY0 = 5, N48D2_K_EXPECT = 6, N48D2_K_REC = 7 };
// 0.0.638: REC {abs, slot = val, delay = step_us}: delay_us(step_us), ONE read of abs, stored in the record buffer (E::rec_buf()[slot]); NEVER a gate, never a write; abs must be one of the four recorded registers and the slot
// must be that register's own (n48d2_rec_ok).
// 0.0.635: COPY0 {dst = abs, src = cond_abs, mask, expect = val}: read the PIPE-0 source, require (src & mask) == expect (the census value: the drift tripwire), write the source's value to the destination (instance 2 only; the pair must be one of the 20 in
// n48d2_mirror0[]). EXPECT {abs, mask, val}: read, require (r & mask) == val; READ ONLY, never a write.
struct n48d2_step {
    uint8_t kind;
    uint32_t abs, mask, val;
    uint32_t polls, step_us;          /* WAIT / WAITIF */
    uint32_t cond_abs, cond_mask;     /* WAITIF; COPY0: source / write+readback mask (0.0.637) */
    const char *reg, *fn;
};
#define N48D2_MAX_STEPS 48u
static inline int n48d2_is_write(const struct n48d2_step *s) { return s->kind == N48D2_K_SET || s->kind == N48D2_K_UPD || s->kind == N48D2_K_COPY0; }
static inline void n48d2_put(struct n48d2_step *v, uint32_t *n, uint8_t kind, uint32_t abs, uint32_t mask, uint32_t val, uint32_t polls, uint32_t stepUs, uint32_t condAbs, uint32_t condMask,
                             const char *reg, const char *fn)
{
    if (*n >= N48D2_MAX_STEPS) { *n = N48D2_MAX_STEPS + 1u; return; }
    v[*n].kind = kind; v[*n].abs = abs; v[*n].mask = kind == N48D2_K_SET ? 0xFFFFFFFFu : mask; v[*n].val = val; v[*n].polls = polls; v[*n].step_us = stepUs;
    v[*n].cond_abs = condAbs; v[*n].cond_mask = condMask; v[*n].reg = reg; v[*n].fn = fn;
    (*n)++;
}
#define N48D2_SET(r, v, fn)        n48d2_put(s, &n, N48D2_K_SET, N48D2_##r, 0xFFFFFFFFu, (v), 0u, 0u, 0u, 0u, #r, fn)
#define N48D2_UPD(r, m, v, fn)     n48d2_put(s, &n, N48D2_K_UPD, N48D2_##r, (m), (v), 0u, 0u, 0u, 0u, #r, fn)
#define N48D2_WAIT(r, m, v, p, us, fn) n48d2_put(s, &n, N48D2_K_WAIT, N48D2_##r, (m), (v), (p), (us), 0u, 0u, #r, fn)
#define N48D2_EXPECT(r, m, v, fn)  n48d2_put(s, &n, N48D2_K_EXPECT, N48D2_##r, (m), (v), 0u, 0u, 0u, 0u, #r, fn)
#define N48D2_REC(r, slot, delayUs, fn) n48d2_put(s, &n, N48D2_K_REC, N48D2_##r, 0u, (slot), 0u, (delayUs), 0u, 0u, #r, fn)
// 0.0.638: the RECORDED reads of op 8's part B (no gate). Slot s of a sample belongs to n48d2_rec_regs[s & 3]; samples: slots 0..3 ~1 ms after the HUBP clock enable, 4..7 after the 2-frame wait.
#define N48D2_REC_REGS 4u
#define N48D2_REC_SLOTS 8u
static const uint32_t n48d2_rec_regs[N48D2_REC_REGS] = { N48D2_HUBP2_HUBP_CLK_CNTL, N48D2_DCCG_GATE_DISABLE_CNTL6, N48D2_DCCG_GATE_DISABLE_CNTL, N48D2_DOMAIN2_PG_STATUS };
/* 0.0.639: slot 13 is the fifth recorded register, DCHUBBUB_DET2_CTRL, read once BEFORE the unblank (no gate: the segments are granted at the first VUPDATE after the unblank, so the wait is post-unblank) */
#define N48D2_RS_DET 13u
static inline int n48d2_rec_ok(uint32_t abs, uint32_t slot) { return (slot < N48D2_REC_SLOTS && abs == n48d2_rec_regs[slot & 3u]) || (slot == N48D2_RS_DET && abs == N48D2_DCHUBBUB_DET2_CTRL); }
/* 0.0.655: instance 1's four recorded registers (HUBP1_HUBP_CLK_CNTL, the two DCCG gates, DOMAIN1_PG_STATUS) and its fifth (slot 13) DCHUBBUB_DET1_CTRL; n48d2_rec_ok above is the monitor B's */
static const uint32_t n48d1_rec_regs[N48D2_REC_REGS] = { N48D2_HUBP1_HUBP_CLK_CNTL, N48D2_DCCG_GATE_DISABLE_CNTL6, N48D2_DCCG_GATE_DISABLE_CNTL, N48D2_DOMAIN1_PG_STATUS };
static inline int n48d2_rec_ok_i(uint32_t inst, uint32_t abs, uint32_t slot)
{
    if (inst == N48D2_INST_MONA) return (slot < N48D2_REC_SLOTS && abs == n48d1_rec_regs[slot & 3u]) || (slot == N48D2_RS_DET && abs == N48D2_DCHUBBUB_DET1_CTRL);
    return n48d2_rec_ok(abs, slot);
}
// The record page (op 13 reads it; the flow fills it; file-scope in the kext): [0..3] sample A, [4..7] sample B, [8] valid bits (1 = A taken, 2 = B taken, 4 = the op's first timed-out WAIT recorded, 8 = the rollback's),
// [9] / [10] the op's first timed-out WAIT: register / LAST value read, [11] / [12] the same for the rollback list, [13] DCHUBBUB_DET2_CTRL read before the unblank (0.0.639, valid bit 16),
// [14] / [15] HUBP2_DCHUBP_CNTL / ODM2_OPTC_INPUT_GLOBAL_CONTROL at op 8's post-unblank health read (0.0.639, valid bit 32: the underflow / TIMEOUT / ODM2 bits are RECORDED there, not gated; bit 64: the rollback left the address
// registers alone because NO_OUTSTANDING_REQ never asserted).
#define N48D2_REC_N 16u
#define N48D2_RC_VALID 8u
#define N48D2_RC_WAIT_ABS 9u
#define N48D2_RC_WAIT_VAL 10u
#define N48D2_RC_RB_ABS 11u
#define N48D2_RC_RB_VAL 12u
#define N48D2_RV_A 1u
#define N48D2_RV_B 2u
#define N48D2_RV_WAIT 4u
#define N48D2_RV_RB 8u
#define N48D2_RV_DET0 16u
#define N48D2_RV_UND 32u
#define N48D2_RV_ADDRKEPT 64u
#define N48D2_RV_HELD 128u      /* 0.0.652: `fbhold 2` succeeded (planerec reports it) */
#define N48D2_RC_DET0 13u
#define N48D2_RC_UND_HUBP 14u
#define N48D2_RC_UND_ODM 15u
// the name of a register a WAIT can time out on (the CLI's "wait timed out at <reg> last value <v>")
static inline const char *n48d2_wait_reg_name(uint32_t abs)
{
    return abs == N48D2_HUBP2_DCHUBP_CNTL ? "HUBP2_DCHUBP_CNTL" : abs == N48D2_HUBP2_HUBP_CLK_CNTL ? "HUBP2_HUBP_CLK_CNTL" : abs == N48D2_DCHUBBUB_DET2_CTRL ? "DCHUBBUB_DET2_CTRL" : abs == N48D2_DPG2_STATUS ? "DPG2_DPG_STATUS" :
           abs == N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL ? "HUBPREQ2_DCSURF_FLIP_CONTROL" : abs == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE ? "HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE" :
           abs == N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH ? "HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH" : abs == N48D2_MPCC2_MPCC_STATUS ? "MPCC2_MPCC_STATUS" :
           /* 0.0.655: instance 1's */
           abs == N48D2_HUBP1_DCHUBP_CNTL ? "HUBP1_DCHUBP_CNTL" : abs == N48D2_HUBP1_HUBP_CLK_CNTL ? "HUBP1_HUBP_CLK_CNTL" : abs == N48D2_DCHUBBUB_DET1_CTRL ? "DCHUBBUB_DET1_CTRL" : abs == N48D2_DPG1_STATUS ? "DPG1_DPG_STATUS" :
           abs == N48D2_HUBPREQ1_DCSURF_FLIP_CONTROL ? "HUBPREQ1_DCSURF_FLIP_CONTROL" : abs == N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE ? "HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE" :
           abs == N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_HIGH ? "HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_HIGH" : abs == N48D2_MPCC1_MPCC_STATUS ? "MPCC1_MPCC_STATUS" : abs == N48D2_OTG1_OTG_MASTER_UPDATE_LOCK ? "OTG1_OTG_MASTER_UPDATE_LOCK" : "?";
}

// `disp2 timing` (dcn401_enable_stream_timing, without the DMUB's program_pix_clk): optc1_enable_optc_clock(true), optc401_set_odm_bypass, optc1_program_timing (+ optc1_set_vtotal_min_max,
// optc401_program_global_sync, optc1_set_vtg_params), optc401_set_h_timing_div_manual_mode, opp1_pipe_clock_control(true), opp2_program_left_edge_extra_pixel, dcn20_blank_pixel_data ->
// opp2_set_disp_pattern_generator (COLORSQUARES), optc401_enable_crtc, dcn20_wait_for_blank_complete. Returns the step count (0 = the timing does not compute).
static inline uint32_t n48d2_build_timing(struct n48d2_step *s, const struct n48d2_timing *t)
{
    struct n48d2_otg_regs r;
    uint32_t n = 0u;
    if (n48d2_otg_compute(t, &r) != 0u) return 0u;
    N48D2_UPD(ODM1_INPUT_CLOCK_CONTROL, N48D2_ODM_CLK_EN_MASK | N48D2_ODM_CLK_GATE_DIS_MASK, N48D2_ODM_CLK_EN_MASK | N48D2_ODM_CLK_GATE_DIS_MASK, "optc1_enable_optc_clock");
    N48D2_WAIT(ODM1_INPUT_CLOCK_CONTROL, N48D2_ODM_CLK_ON_MASK, N48D2_ODM_CLK_ON_MASK, 1000u, 1u, "optc1_enable_optc_clock");
    N48D2_UPD(OTG1_CLOCK_CONTROL, N48D2_OTG_CLOCK_EN_MASK | N48D2_OTG_CLOCK_GATE_DIS_MASK, N48D2_OTG_CLOCK_EN_MASK | N48D2_OTG_CLOCK_GATE_DIS_MASK, "optc1_enable_optc_clock");
    N48D2_WAIT(OTG1_CLOCK_CONTROL, N48D2_OTG_CLOCK_ON_MASK, N48D2_OTG_CLOCK_ON_MASK, 1000u, 1u, "optc1_enable_optc_clock");
    N48D2_SET(ODM1_DATA_SOURCE_SELECT, (N48D2_OTG1_INST << 16) | (0xFu << 20) | (0xFu << 24) | (0xFu << 28), "optc401_set_odm_bypass");   /* NUM_OF_INPUT_SEGMENT 0, SEG0 = OPP1, SEG1..3 = 0xf */
    N48D2_UPD(OTG1_H_TIMING_CNTL, N48D2_H_DIV_MODE_MASK, 0u, "optc401_set_odm_bypass");
    N48D2_SET(ODM1_MEMORY_CONFIG, 0u, "optc401_set_odm_bypass");
    N48D2_SET(OTG1_H_TOTAL, r.h_total, "optc1_program_timing");
    N48D2_UPD(OTG1_H_SYNC_A, N48D2_LO15 | N48D2_HI15, r.h_sync_a, "optc1_program_timing");
    N48D2_UPD(OTG1_H_BLANK_START_END, N48D2_LO15 | N48D2_HI15, r.h_blank, "optc1_program_timing");
    N48D2_UPD(OTG1_H_SYNC_A_CNTL, N48D2_SYNC_POL_MASK, r.h_pol, "optc1_program_timing");
    N48D2_SET(OTG1_V_TOTAL, r.v_total, "optc1_program_timing");
    N48D2_SET(OTG1_V_TOTAL_MAX, r.v_total, "optc1_set_vtotal_min_max");
    N48D2_SET(OTG1_V_TOTAL_MIN, r.v_total, "optc1_set_vtotal_min_max");
    N48D2_UPD(OTG1_V_SYNC_A, N48D2_LO15 | N48D2_HI15, r.v_sync_a, "optc1_program_timing");
    N48D2_UPD(OTG1_V_BLANK_START_END, N48D2_LO15 | N48D2_HI15, r.v_blank, "optc1_program_timing");
    N48D2_UPD(OTG1_V_SYNC_A_CNTL, N48D2_SYNC_POL_MASK, r.v_pol, "optc1_program_timing");
    N48D2_UPD(OTG1_INTERLACE_CONTROL, N48D2_INTERLACE_EN_MASK, 0u, "optc1_program_timing");
    N48D2_UPD(VTG1_CONTROL, N48D2_VTG_ENABLE_MASK, 0u, "optc1_program_timing");
    N48D2_UPD(OTG1_CONTROL, N48D2_OTG_START_POINT_MASK | N48D2_OTG_FIELD_NUMBER_MASK, 0u, "optc1_program_timing");   /* TMDS: start point 0, field number 0 */
    N48D2_SET(OTG1_VSTARTUP_PARAM, r.vstartup, "optc401_program_global_sync");
    N48D2_SET(OTG1_VUPDATE_PARAM, r.vupdate, "optc401_program_global_sync");
    N48D2_SET(OTG1_VREADY_PARAM, r.vready, "optc401_program_global_sync");
    N48D2_UPD(VTG1_CONTROL, N48D2_VTG_FP2_MASK | N48D2_VTG_VCOUNT_INIT_MASK, (r.vtg_vcount_init << 16) | r.vtg_fp2, "optc1_set_vtg_params");
    N48D2_UPD(ODM1_DATA_FORMAT_CONTROL, N48D2_ODM_DATA_FORMAT_MASK, 0u, "optc1_program_timing");   /* RGB */
    N48D2_UPD(OTG1_H_TIMING_CNTL, N48D2_H_DIV_MODE_MASK, 0u, "optc1_program_timing");             /* H_TIMING_NO_DIV */
    N48D2_UPD(OTG1_H_TIMING_CNTL, N48D2_H_DIV_MANUAL_MASK, r.h_div2 ? 0u : N48D2_H_DIV_MANUAL_MASK, "optc401_set_h_timing_div_manual_mode");
    N48D2_UPD(OPP_PIPE1_CONTROL, N48D2_OPP_PIPE_CLOCK_EN_MASK, 1u, "opp1_pipe_clock_control");
    N48D2_UPD(FMT1_422_CONTROL, N48D2_FMT_LEFT_EDGE_MASK, 0u, "opp2_program_left_edge_extra_pixel");
    N48D2_SET(DPG1_DIMENSIONS, (t->h_active << 16) | t->v_active, "opp2_set_disp_pattern_generator");   /* DPG_ACTIVE_WIDTH << 16 | DPG_ACTIVE_HEIGHT */
    N48D2_SET(DPG1_OFFSET_SEGMENT, 0u, "opp2_set_disp_pattern_generator");
    N48D2_UPD(DPG1_CONTROL, N48D2_DPG_CONTROL_MASK, N48D2_DPG_CONTROL_ON, "opp2_set_disp_pattern_generator");
    N48D2_UPD(ODM1_DATA_SOURCE_SELECT, N48D2_ODM_SEG0_MASK, N48D2_OTG1_INST << 16, "optc401_enable_crtc");
    N48D2_UPD(VTG1_CONTROL, N48D2_VTG_ENABLE_MASK, N48D2_VTG_ENABLE_MASK, "optc401_enable_crtc");
    N48D2_UPD(OTG1_CONTROL, N48D2_OTG_DISABLE_POINT_MASK | N48D2_OTG_MASTER_EN_MASK, (2u << 8) | 1u, "optc401_enable_crtc");
    N48D2_WAIT(DPG1_STATUS, N48D2_DPG_PENDING_MASK, 0u, 1000u, 100u, "dcn20_wait_for_blank_complete");
    return n;
}

// `disp2 connect` (dce110_apply_single_controller_ctx_to_hw -> link_set_dpms_on, the TMDS-DVI path, without what the DMUB already did: link_enc->setup and enable_link): enc1_dig_connect_to_otg,
// optc401_set_out_mux (DIO), enc1_setup_stereo_sync, enc401_stream_encoder_dvi_set_stream_attribute (the register branch: DIG_CLOCK_PATTERN 0x1F, HDMI_CONTROL pixel encoding / colour format 0), then
// dcn401_enable_stream -> setup_dio_stream_encoder: dcn10_link_encoder_connect_dig_be_to_fe, enc401_stream_encoder_enable (DVI), enc401_stream_encoder_map_to_link, enc401_set_dig_input_mode, enc35_enable_fifo.
static inline uint32_t n48d2_build_connect(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_UPD(SYMCLKC_CLOCK_ENABLE, N48D2_SYMCLK_FE_MASK, N48D2_SYMCLKC_FE_VALUE, "dccg401_enable_symclk_se");     /* 0.0.633: SYMCLKC_FE_EN 1, FE_SRC_SEL 2 (link encoder UNIPHY_C): the front end's symbol clock, first */
    N48D2_UPD(DIG2_FE_CNTL, N48D2_DIG_SOURCE_SELECT_MASK, N48D2_OTG1_INST, "enc1_dig_connect_to_otg");
    N48D2_UPD(OTG1_CONTROL, N48D2_OTG_OUT_MUX_MASK, 0u, "optc401_set_out_mux");
    N48D2_UPD(DIG2_FE_CNTL, N48D2_DIG_STEREOSYNC_SEL_MASK, N48D2_OTG1_INST << 4, "enc1_setup_stereo_sync");
    N48D2_UPD(DIG2_FE_CNTL, N48D2_DIG_STEREOSYNC_GATE_MASK, N48D2_DIG_STEREOSYNC_GATE_MASK, "enc1_setup_stereo_sync");     /* gate = !enable (no 3D) */
    N48D2_UPD(DIG2_CLOCK_PATTERN, N48D2_DIG_CLOCK_PATTERN_MASK, 0x1Fu, "enc401_stream_encoder_dvi_set_stream_attribute");
    N48D2_UPD(DIG2_HDMI_CONTROL, N48D2_TMDS_PIXEL_ENCODING_MASK, 0u, "enc401_stream_encoder_set_stream_attribute_helper");
    N48D2_UPD(DIG2_HDMI_CONTROL, N48D2_TMDS_COLOR_FORMAT_MASK, 0u, "enc401_stream_encoder_set_stream_attribute_helper");
    N48D2_UPD(DIG2_BE_CNTL, N48D2_BE_FE2_BIT, N48D2_BE_FE2_BIT, "dcn10_link_encoder_connect_dig_be_to_fe");                 /* FE_SOURCE_SELECT |= 0x04 */
    N48D2_UPD(DIG2_FE_CLK_CNTL, N48D2_DIG_FE_MODE_MASK, N48D2_DVI_MODE, "enc401_stream_encoder_enable");
    N48D2_UPD(DIG2_FE_CLK_CNTL, N48D2_DIG_FE_CLK_EN_MASK, N48D2_DIG_FE_CLK_EN_MASK, "enc401_stream_encoder_enable");
    N48D2_UPD(DIG2_FE_EN_CNTL, N48D2_DIG_FE_ENABLE_MASK, 1u, "enc401_stream_encoder_enable");
    N48D2_UPD(DIG2_STREAM_MAPPER, N48D2_MAPPER_LINK_MASK, N48D2_LINK_UNIPHY_C, "enc401_stream_encoder_map_to_link");
    N48D2_UPD(DIG2_FIFO_CTRL0, N48D2_FIFO_PIX_PER_CYCLE_MASK, 0u, "enc401_set_dig_input_mode");                             /* dio_se_pix_per_cycle 1 (TMDS RGB) */
    N48D2_UPD(DIG2_FIFO_CTRL0, N48D2_FIFO_READ_START_MASK, 7u << 2, "enc35_enable_fifo");
    N48D2_UPD(DIG2_FE_CLK_CNTL, N48D2_DIG_FE_CLK_EN_MASK, N48D2_DIG_FE_CLK_EN_MASK, "enc35_enable_fifo");
    N48D2_UPD(DIG2_FE_EN_CNTL, N48D2_DIG_FE_ENABLE_MASK, 1u, "enc35_enable_fifo");
    N48D2_UPD(DIG2_FIFO_CTRL0, N48D2_FIFO_RESET_MASK, N48D2_FIFO_RESET_MASK, "enc35_reset_fifo");
    n48d2_put(s, &n, N48D2_K_WAITIF, N48D2_DIG2_FIFO_CTRL0, N48D2_FIFO_RESET_DONE_MASK, N48D2_FIFO_RESET_DONE_MASK, 5000u, 10u, N48D2_DIG2_FE_CLK_CNTL, N48D2_DIG_FE_SYMCLK_ON_MASK, "DIG2_FIFO_CTRL0", "enc35_reset_fifo");
    N48D2_UPD(DIG2_FIFO_CTRL0, N48D2_FIFO_RESET_MASK, 0u, "enc35_reset_fifo");
    n48d2_put(s, &n, N48D2_K_WAITIF, N48D2_DIG2_FIFO_CTRL0, N48D2_FIFO_RESET_DONE_MASK, 0u, 5000u, 10u, N48D2_DIG2_FE_CLK_CNTL, N48D2_DIG_FE_SYMCLK_ON_MASK, "DIG2_FIFO_CTRL0", "enc35_reset_fifo");
    N48D2_UPD(DIG2_FIFO_CTRL0, N48D2_FIFO_ENABLE_MASK, 1u, "enc35_enable_fifo");
    return n;
}

// `disp2 off`: the brief's reverse order. DPG1 to video mode (opp2_set_disp_pattern_generator VIDEOMODE), enc35_disable_fifo, enc401_set_dig_input_mode(0), enc401_stream_encoder_enable(false),
// dcn10_link_encoder_connect_dig_be_to_fe(false), the stream mapper back to 0 (its value before `connect`), optc401_disable_crtc, optc1_enable_optc_clock(false), opp1_pipe_clock_control(false).
// Every write is an UPDATE to the off value (or the VIDEOMODE zeroes), so `off` is safe from any partial state and may be repeated.
static inline uint32_t n48d2_build_off(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_SET(DPG1_CONTROL, 0u, "opp2_set_disp_pattern_generator");
    N48D2_SET(DPG1_COLOUR_R_CR, 0u, "opp2_set_disp_pattern_generator");
    N48D2_SET(DPG1_COLOUR_G_Y, 0u, "opp2_set_disp_pattern_generator");
    N48D2_SET(DPG1_COLOUR_B_CB, 0u, "opp2_set_disp_pattern_generator");
    N48D2_SET(DPG1_RAMP_CONTROL, 0u, "opp2_set_disp_pattern_generator");
    N48D2_UPD(DIG2_FIFO_CTRL0, N48D2_FIFO_ENABLE_MASK, 0u, "enc35_disable_fifo");
    N48D2_UPD(DIG2_FE_EN_CNTL, N48D2_DIG_FE_ENABLE_MASK, 0u, "enc35_disable_fifo");
    N48D2_UPD(DIG2_FE_CLK_CNTL, N48D2_DIG_FE_CLK_EN_MASK, 0u, "enc35_disable_fifo");
    N48D2_UPD(DIG2_FIFO_CTRL0, N48D2_FIFO_PIX_PER_CYCLE_MASK, 0u, "enc401_set_dig_input_mode");
    N48D2_UPD(DIG2_FE_EN_CNTL, N48D2_DIG_FE_ENABLE_MASK, 0u, "enc401_stream_encoder_enable");
    N48D2_UPD(DIG2_FE_CLK_CNTL, N48D2_DIG_FE_CLK_EN_MASK, 0u, "enc401_stream_encoder_enable");
    N48D2_UPD(DIG2_BE_CNTL, N48D2_BE_FE2_BIT, 0u, "dcn10_link_encoder_connect_dig_be_to_fe");                               /* FE_SOURCE_SELECT &= ~0x04 */
    N48D2_UPD(SYMCLKC_CLOCK_ENABLE, N48D2_SYMCLK_FE_MASK, 0u, "dccg401_disable_symclk_se");     /* 0.0.633: SYMCLKC_FE_EN 0, FE_SRC_SEL 0 - before the DMUB's `phyc disable`; 0.0.634: AFTER the BE disconnect (dce110_reset_hw_ctx_wrap: reset_dio_stream_encoder, THEN disable_symclk_se) */
    N48D2_UPD(DIG2_STREAM_MAPPER, N48D2_MAPPER_LINK_MASK, 0u, "enc401_stream_encoder_map_to_link");
    N48D2_UPD(ODM1_DATA_SOURCE_SELECT, N48D2_ODM_SEG0_MASK | N48D2_ODM_SEG123_MASK | N48D2_ODM_NUM_IN_SEG_MASK, 0xFFFF0000u, "optc401_disable_crtc");
    N48D2_UPD(ODM1_MEMORY_CONFIG, N48D2_ODM_MEM_SEL_MASK, 0u, "optc401_disable_crtc");
    N48D2_UPD(OTG1_CONTROL, N48D2_OTG_MASTER_EN_MASK, 0u, "optc401_disable_crtc");
    N48D2_UPD(VTG1_CONTROL, N48D2_VTG_ENABLE_MASK, 0u, "optc401_disable_crtc");
    N48D2_WAIT(OTG1_CONTROL, N48D2_OTG_CUR_MASTER_EN_MASK, 0u, 15000u, 10u, "optc401_disable_crtc");
    N48D2_WAIT(OTG1_CLOCK_CONTROL, N48D2_OTG_BUSY_MASK, 0u, 150000u, 1u, "optc401_disable_crtc");
    N48D2_UPD(OTG1_CLOCK_CONTROL, N48D2_OTG_CLOCK_GATE_DIS_MASK | N48D2_OTG_CLOCK_EN_MASK, 0u, "optc1_enable_optc_clock");
    N48D2_UPD(ODM1_INPUT_CLOCK_CONTROL, N48D2_ODM_CLK_GATE_DIS_MASK | N48D2_ODM_CLK_EN_MASK, 0u, "optc1_enable_optc_clock");
    N48D2_UPD(OPP_PIPE1_CONTROL, N48D2_OPP_PIPE_CLOCK_EN_MASK, 0u, "opp1_pipe_clock_control");
    return n;
}
// INSTANCE 2 (0.0.633): the same three op lists one instance up (OTG2 / ODM2 / OPP2 / DPG2 / DIG3 / SYMCLKD / UNIPHY_D). Generated from the instance-1 lists by the one rename ODM1->ODM2 OTG1->OTG2 VTG1->VTG2 OPP_PIPE1->OPP_PIPE2 FMT1->FMT2 DPG1->DPG2 DIG2->DIG3
// SYMCLKC->SYMCLKD, FE2->FE3 (0x400 -> 0x800), UNIPHY_C 2 -> UNIPHY_D 3, OTG1_INST 1 -> OTG2_INST 2; the host test re-derives each list from Linux names. Linux functions are the same as instance 1's.
// `disp2 timing` (dcn401_enable_stream_timing, without the DMUB's program_pix_clk): optc1_enable_optc_clock(true), optc401_set_odm_bypass, optc1_program_timing (+ optc1_set_vtotal_min_max,
// optc401_program_global_sync, optc1_set_vtg_params), optc401_set_h_timing_div_manual_mode, opp1_pipe_clock_control(true), opp2_program_left_edge_extra_pixel, dcn20_blank_pixel_data ->
// opp2_set_disp_pattern_generator (COLORSQUARES), optc401_enable_crtc, dcn20_wait_for_blank_complete. Returns the step count (0 = the timing does not compute).
static inline uint32_t n48d2_build_timing2(struct n48d2_step *s, const struct n48d2_timing *t)
{
    struct n48d2_otg_regs r;
    uint32_t n = 0u;
    if (n48d2_otg_compute(t, &r) != 0u) return 0u;
    N48D2_UPD(ODM2_INPUT_CLOCK_CONTROL, N48D2_ODM_CLK_EN_MASK | N48D2_ODM_CLK_GATE_DIS_MASK, N48D2_ODM_CLK_EN_MASK | N48D2_ODM_CLK_GATE_DIS_MASK, "optc1_enable_optc_clock");
    N48D2_WAIT(ODM2_INPUT_CLOCK_CONTROL, N48D2_ODM_CLK_ON_MASK, N48D2_ODM_CLK_ON_MASK, 1000u, 1u, "optc1_enable_optc_clock");
    N48D2_UPD(OTG2_CLOCK_CONTROL, N48D2_OTG_CLOCK_EN_MASK | N48D2_OTG_CLOCK_GATE_DIS_MASK, N48D2_OTG_CLOCK_EN_MASK | N48D2_OTG_CLOCK_GATE_DIS_MASK, "optc1_enable_optc_clock");
    N48D2_WAIT(OTG2_CLOCK_CONTROL, N48D2_OTG_CLOCK_ON_MASK, N48D2_OTG_CLOCK_ON_MASK, 1000u, 1u, "optc1_enable_optc_clock");
    N48D2_SET(ODM2_DATA_SOURCE_SELECT, (N48D2_OTG2_INST << 16) | (0xFu << 20) | (0xFu << 24) | (0xFu << 28), "optc401_set_odm_bypass");   /* NUM_OF_INPUT_SEGMENT 0, SEG0 = OPP2, SEG1..3 = 0xf */
    N48D2_UPD(OTG2_H_TIMING_CNTL, N48D2_H_DIV_MODE_MASK, 0u, "optc401_set_odm_bypass");
    N48D2_SET(ODM2_MEMORY_CONFIG, 0u, "optc401_set_odm_bypass");
    N48D2_SET(OTG2_H_TOTAL, r.h_total, "optc1_program_timing");
    N48D2_UPD(OTG2_H_SYNC_A, N48D2_LO15 | N48D2_HI15, r.h_sync_a, "optc1_program_timing");
    N48D2_UPD(OTG2_H_BLANK_START_END, N48D2_LO15 | N48D2_HI15, r.h_blank, "optc1_program_timing");
    N48D2_UPD(OTG2_H_SYNC_A_CNTL, N48D2_SYNC_POL_MASK, r.h_pol, "optc1_program_timing");
    N48D2_SET(OTG2_V_TOTAL, r.v_total, "optc1_program_timing");
    N48D2_SET(OTG2_V_TOTAL_MAX, r.v_total, "optc1_set_vtotal_min_max");
    N48D2_SET(OTG2_V_TOTAL_MIN, r.v_total, "optc1_set_vtotal_min_max");
    N48D2_UPD(OTG2_V_SYNC_A, N48D2_LO15 | N48D2_HI15, r.v_sync_a, "optc1_program_timing");
    N48D2_UPD(OTG2_V_BLANK_START_END, N48D2_LO15 | N48D2_HI15, r.v_blank, "optc1_program_timing");
    N48D2_UPD(OTG2_V_SYNC_A_CNTL, N48D2_SYNC_POL_MASK, r.v_pol, "optc1_program_timing");
    N48D2_UPD(OTG2_INTERLACE_CONTROL, N48D2_INTERLACE_EN_MASK, 0u, "optc1_program_timing");
    N48D2_UPD(VTG2_CONTROL, N48D2_VTG_ENABLE_MASK, 0u, "optc1_program_timing");
    N48D2_UPD(OTG2_CONTROL, N48D2_OTG_START_POINT_MASK | N48D2_OTG_FIELD_NUMBER_MASK, 0u, "optc1_program_timing");   /* TMDS: start point 0, field number 0 */
    N48D2_SET(OTG2_VSTARTUP_PARAM, r.vstartup, "optc401_program_global_sync");
    N48D2_SET(OTG2_VUPDATE_PARAM, r.vupdate, "optc401_program_global_sync");
    N48D2_SET(OTG2_VREADY_PARAM, r.vready, "optc401_program_global_sync");
    N48D2_UPD(VTG2_CONTROL, N48D2_VTG_FP2_MASK | N48D2_VTG_VCOUNT_INIT_MASK, (r.vtg_vcount_init << 16) | r.vtg_fp2, "optc1_set_vtg_params");
    N48D2_UPD(ODM2_DATA_FORMAT_CONTROL, N48D2_ODM_DATA_FORMAT_MASK, 0u, "optc1_program_timing");   /* RGB */
    N48D2_UPD(OTG2_H_TIMING_CNTL, N48D2_H_DIV_MODE_MASK, 0u, "optc1_program_timing");             /* H_TIMING_NO_DIV */
    N48D2_UPD(OTG2_H_TIMING_CNTL, N48D2_H_DIV_MANUAL_MASK, r.h_div2 ? 0u : N48D2_H_DIV_MANUAL_MASK, "optc401_set_h_timing_div_manual_mode");
    N48D2_UPD(OPP_PIPE2_CONTROL, N48D2_OPP_PIPE_CLOCK_EN_MASK, 1u, "opp1_pipe_clock_control");
    N48D2_UPD(FMT2_422_CONTROL, N48D2_FMT_LEFT_EDGE_MASK, 0u, "opp2_program_left_edge_extra_pixel");
    N48D2_SET(DPG2_DIMENSIONS, (t->h_active << 16) | t->v_active, "opp2_set_disp_pattern_generator");   /* DPG_ACTIVE_WIDTH << 16 | DPG_ACTIVE_HEIGHT */
    N48D2_SET(DPG2_OFFSET_SEGMENT, 0u, "opp2_set_disp_pattern_generator");
    N48D2_UPD(DPG2_CONTROL, N48D2_DPG_CONTROL_MASK, N48D2_DPG_CONTROL_ON, "opp2_set_disp_pattern_generator");
    N48D2_UPD(ODM2_DATA_SOURCE_SELECT, N48D2_ODM_SEG0_MASK, N48D2_OTG2_INST << 16, "optc401_enable_crtc");
    N48D2_UPD(VTG2_CONTROL, N48D2_VTG_ENABLE_MASK, N48D2_VTG_ENABLE_MASK, "optc401_enable_crtc");
    N48D2_UPD(OTG2_CONTROL, N48D2_OTG_DISABLE_POINT_MASK | N48D2_OTG_MASTER_EN_MASK, (2u << 8) | 1u, "optc401_enable_crtc");
    N48D2_WAIT(DPG2_STATUS, N48D2_DPG_PENDING_MASK, 0u, 1000u, 100u, "dcn20_wait_for_blank_complete");
    return n;
}

// `disp2 connect` (dce110_apply_single_controller_ctx_to_hw -> link_set_dpms_on, the TMDS-DVI path, without what the DMUB already did: link_enc->setup and enable_link): enc1_dig_connect_to_otg,
// optc401_set_out_mux (DIO), enc1_setup_stereo_sync, enc401_stream_encoder_dvi_set_stream_attribute (the register branch: DIG_CLOCK_PATTERN 0x1F, HDMI_CONTROL pixel encoding / colour format 0), then
// dcn401_enable_stream -> setup_dio_stream_encoder: dcn10_link_encoder_connect_dig_be_to_fe, enc401_stream_encoder_enable (DVI), enc401_stream_encoder_map_to_link, enc401_set_dig_input_mode, enc35_enable_fifo.
static inline uint32_t n48d2_build_connect2(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_UPD(SYMCLKD_CLOCK_ENABLE, N48D2_SYMCLK_FE_MASK, N48D2_SYMCLKD_FE_VALUE, "dccg401_enable_symclk_se");     /* 0.0.633: SYMCLKD_FE_EN 1, FE_SRC_SEL 3 (link encoder UNIPHY_D): the front end's symbol clock, first */
    N48D2_UPD(DIG3_FE_CNTL, N48D2_DIG_SOURCE_SELECT_MASK, N48D2_OTG2_INST, "enc1_dig_connect_to_otg");
    N48D2_UPD(OTG2_CONTROL, N48D2_OTG_OUT_MUX_MASK, 0u, "optc401_set_out_mux");
    N48D2_UPD(DIG3_FE_CNTL, N48D2_DIG_STEREOSYNC_SEL_MASK, N48D2_OTG2_INST << 4, "enc1_setup_stereo_sync");
    N48D2_UPD(DIG3_FE_CNTL, N48D2_DIG_STEREOSYNC_GATE_MASK, N48D2_DIG_STEREOSYNC_GATE_MASK, "enc1_setup_stereo_sync");     /* gate = !enable (no 3D) */
    N48D2_UPD(DIG3_CLOCK_PATTERN, N48D2_DIG_CLOCK_PATTERN_MASK, 0x1Fu, "enc401_stream_encoder_dvi_set_stream_attribute");
    N48D2_UPD(DIG3_HDMI_CONTROL, N48D2_TMDS_PIXEL_ENCODING_MASK, 0u, "enc401_stream_encoder_set_stream_attribute_helper");
    N48D2_UPD(DIG3_HDMI_CONTROL, N48D2_TMDS_COLOR_FORMAT_MASK, 0u, "enc401_stream_encoder_set_stream_attribute_helper");
    N48D2_UPD(DIG3_BE_CNTL, N48D2_BE_FE3_BIT, N48D2_BE_FE3_BIT, "dcn10_link_encoder_connect_dig_be_to_fe");                 /* FE_SOURCE_SELECT |= 0x08 */
    N48D2_UPD(DIG3_FE_CLK_CNTL, N48D2_DIG_FE_MODE_MASK, N48D2_DVI_MODE, "enc401_stream_encoder_enable");
    N48D2_UPD(DIG3_FE_CLK_CNTL, N48D2_DIG_FE_CLK_EN_MASK, N48D2_DIG_FE_CLK_EN_MASK, "enc401_stream_encoder_enable");
    N48D2_UPD(DIG3_FE_EN_CNTL, N48D2_DIG_FE_ENABLE_MASK, 1u, "enc401_stream_encoder_enable");
    N48D2_UPD(DIG3_STREAM_MAPPER, N48D2_MAPPER_LINK_MASK, N48D2_LINK_UNIPHY_D, "enc401_stream_encoder_map_to_link");
    N48D2_UPD(DIG3_FIFO_CTRL0, N48D2_FIFO_PIX_PER_CYCLE_MASK, 0u, "enc401_set_dig_input_mode");                             /* dio_se_pix_per_cycle 1 (TMDS RGB) */
    N48D2_UPD(DIG3_FIFO_CTRL0, N48D2_FIFO_READ_START_MASK, 7u << 2, "enc35_enable_fifo");
    N48D2_UPD(DIG3_FE_CLK_CNTL, N48D2_DIG_FE_CLK_EN_MASK, N48D2_DIG_FE_CLK_EN_MASK, "enc35_enable_fifo");
    N48D2_UPD(DIG3_FE_EN_CNTL, N48D2_DIG_FE_ENABLE_MASK, 1u, "enc35_enable_fifo");
    N48D2_UPD(DIG3_FIFO_CTRL0, N48D2_FIFO_RESET_MASK, N48D2_FIFO_RESET_MASK, "enc35_reset_fifo");
    n48d2_put(s, &n, N48D2_K_WAITIF, N48D2_DIG3_FIFO_CTRL0, N48D2_FIFO_RESET_DONE_MASK, N48D2_FIFO_RESET_DONE_MASK, 5000u, 10u, N48D2_DIG3_FE_CLK_CNTL, N48D2_DIG_FE_SYMCLK_ON_MASK, "DIG3_FIFO_CTRL0", "enc35_reset_fifo");
    N48D2_UPD(DIG3_FIFO_CTRL0, N48D2_FIFO_RESET_MASK, 0u, "enc35_reset_fifo");
    n48d2_put(s, &n, N48D2_K_WAITIF, N48D2_DIG3_FIFO_CTRL0, N48D2_FIFO_RESET_DONE_MASK, 0u, 5000u, 10u, N48D2_DIG3_FE_CLK_CNTL, N48D2_DIG_FE_SYMCLK_ON_MASK, "DIG3_FIFO_CTRL0", "enc35_reset_fifo");
    N48D2_UPD(DIG3_FIFO_CTRL0, N48D2_FIFO_ENABLE_MASK, 1u, "enc35_enable_fifo");
    return n;
}

// `disp2 off`: the brief's reverse order. DPG2 to video mode (opp2_set_disp_pattern_generator VIDEOMODE), enc35_disable_fifo, enc401_set_dig_input_mode(0), enc401_stream_encoder_enable(false),
// dcn10_link_encoder_connect_dig_be_to_fe(false), the stream mapper back to 0 (its value before `connect`), optc401_disable_crtc, optc1_enable_optc_clock(false), opp1_pipe_clock_control(false).
// Every write is an UPDATE to the off value (or the VIDEOMODE zeroes), so `off` is safe from any partial state and may be repeated.
static inline uint32_t n48d2_build_off2(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_SET(DPG2_CONTROL, 0u, "opp2_set_disp_pattern_generator");
    N48D2_SET(DPG2_COLOUR_R_CR, 0u, "opp2_set_disp_pattern_generator");
    N48D2_SET(DPG2_COLOUR_G_Y, 0u, "opp2_set_disp_pattern_generator");
    N48D2_SET(DPG2_COLOUR_B_CB, 0u, "opp2_set_disp_pattern_generator");
    N48D2_SET(DPG2_RAMP_CONTROL, 0u, "opp2_set_disp_pattern_generator");
    N48D2_UPD(DIG3_FIFO_CTRL0, N48D2_FIFO_ENABLE_MASK, 0u, "enc35_disable_fifo");
    N48D2_UPD(DIG3_FE_EN_CNTL, N48D2_DIG_FE_ENABLE_MASK, 0u, "enc35_disable_fifo");
    N48D2_UPD(DIG3_FE_CLK_CNTL, N48D2_DIG_FE_CLK_EN_MASK, 0u, "enc35_disable_fifo");
    N48D2_UPD(DIG3_FIFO_CTRL0, N48D2_FIFO_PIX_PER_CYCLE_MASK, 0u, "enc401_set_dig_input_mode");
    N48D2_UPD(DIG3_FE_EN_CNTL, N48D2_DIG_FE_ENABLE_MASK, 0u, "enc401_stream_encoder_enable");
    N48D2_UPD(DIG3_FE_CLK_CNTL, N48D2_DIG_FE_CLK_EN_MASK, 0u, "enc401_stream_encoder_enable");
    N48D2_UPD(DIG3_BE_CNTL, N48D2_BE_FE3_BIT, 0u, "dcn10_link_encoder_connect_dig_be_to_fe");                               /* FE_SOURCE_SELECT &= ~0x08 */
    N48D2_UPD(SYMCLKD_CLOCK_ENABLE, N48D2_SYMCLK_FE_MASK, 0u, "dccg401_disable_symclk_se");     /* 0.0.633: SYMCLKD_FE_EN 0, FE_SRC_SEL 0 - before the DMUB's `phyd disable`; 0.0.634: AFTER the BE disconnect (reset_dio_stream_encoder, THEN disable_symclk_se) */
    N48D2_UPD(DIG3_STREAM_MAPPER, N48D2_MAPPER_LINK_MASK, 0u, "enc401_stream_encoder_map_to_link");
    N48D2_UPD(ODM2_DATA_SOURCE_SELECT, N48D2_ODM_SEG0_MASK | N48D2_ODM_SEG123_MASK | N48D2_ODM_NUM_IN_SEG_MASK, 0xFFFF0000u, "optc401_disable_crtc");
    N48D2_UPD(ODM2_MEMORY_CONFIG, N48D2_ODM_MEM_SEL_MASK, 0u, "optc401_disable_crtc");
    N48D2_UPD(OTG2_CONTROL, N48D2_OTG_MASTER_EN_MASK, 0u, "optc401_disable_crtc");
    N48D2_UPD(VTG2_CONTROL, N48D2_VTG_ENABLE_MASK, 0u, "optc401_disable_crtc");
    N48D2_WAIT(OTG2_CONTROL, N48D2_OTG_CUR_MASTER_EN_MASK, 0u, 15000u, 10u, "optc401_disable_crtc");
    N48D2_WAIT(OTG2_CLOCK_CONTROL, N48D2_OTG_BUSY_MASK, 0u, 150000u, 1u, "optc401_disable_crtc");
    N48D2_UPD(OTG2_CLOCK_CONTROL, N48D2_OTG_CLOCK_GATE_DIS_MASK | N48D2_OTG_CLOCK_EN_MASK, 0u, "optc1_enable_optc_clock");
    N48D2_UPD(ODM2_INPUT_CLOCK_CONTROL, N48D2_ODM_CLK_GATE_DIS_MASK | N48D2_ODM_CLK_EN_MASK, 0u, "optc1_enable_optc_clock");
    N48D2_UPD(OPP_PIPE2_CONTROL, N48D2_OPP_PIPE_CLOCK_EN_MASK, 0u, "opp1_pipe_clock_control");
    return n;
}
// ---- 0.0.635 (M4c / M4d): instance 2's plane ----------------------------------------------------------------------------------------------------------------------------------------------------------------
// The plane's state across ops (the kext: file-scope; the host test: the model's). stage: NONE (no buffers) -> ALLOC (the glue allocated A and B) -> PLANE (op 8 done: the plane feeds MPC / OPP, DPG2 still ON) -> SHOWN (op 9 done: DPG2 off);
// HELD (0.0.652, M5) = `fbhold 2` ran: the pair is PINNED in the allocator for the rest of the boot (n1c_d2_pin) and the framebuffer aux kext scans buffer A; no op may move, flip or un-program the plane, and nothing frees the pair. FREE = a rollback ran and the glue may give the buffers back; LEAKED = a rollback ran but HUBP2's clock did not read off / NO_OUTSTANDING_REQ timed out: the buffers are NEVER freed (the engine may still read them).
enum { N48D2_PL_NONE = 0, N48D2_PL_ALLOC = 1, N48D2_PL_PLANE = 2, N48D2_PL_SHOWN = 3, N48D2_PL_LEAKED = 4, N48D2_PL_FREE = 5, N48D2_PL_HELD = 6 };
struct n48d2_plane {
    uint32_t stage;
    uint32_t cur;            /* the buffer HUBP2 is latched on: 0 = A, 1 = B */
    uint32_t odm_sticky;     /* ODM2_OPTC_INPUT_GLOBAL_CONTROL bits 10 / 13 already set BEFORE op 8 wrote anything (stale sticky flags: op 8 does not gate on them; op 9 clears them and demands 0) */
    uint64_t mc[2];          /* A, B: MC addresses (64 KiB aligned, inside the scanout window) */
    uint64_t dp_bytes;       /* 0.0.637 (C3): the DP (HUBP0) surface's size as op 8's precheck judged it (n48d2_dp_surface_bytes); 0 = not yet judged (the watch then uses our own size) */
};
// The runtime EXACT set the HUBP2 / HUBP1 address registers may be written with: the halves of A and B (n = 0 = nothing may be written: fail closed). 0.0.655: ONE SET PER INSTANCE (the monitor B's pair A2 / B2 and the monitor A's A1 / B1 never mix:
// an monitor A address register accepts only the monitor A's halves, a monitor B one only the monitor B's); n48d2_aset_get() / _clear() / _set() / _lo_ok() / _hi_ok() are the monitor B's, the _i forms take the instance.
struct n48d2_aset { uint32_t n; uint32_t lo[2], hi[2]; };
static inline struct n48d2_aset *n48d2_aset_get_i(uint32_t inst) { static struct n48d2_aset a1, a2; return inst == N48D2_INST_MONA ? &a1 : &a2; }
static inline struct n48d2_aset *n48d2_aset_get(void) { return n48d2_aset_get_i(N48D2_INST_MONB); }
static inline void n48d2_aset_clear_i(uint32_t inst) { struct n48d2_aset *a = n48d2_aset_get_i(inst); a->n = 0u; a->lo[0] = a->lo[1] = a->hi[0] = a->hi[1] = 0u; }
static inline void n48d2_aset_clear(void) { n48d2_aset_clear_i(N48D2_INST_MONB); }
static inline void n48d2_aset_set_i(uint32_t inst, uint64_t a, uint64_t b)
{
    struct n48d2_aset *x = n48d2_aset_get_i(inst);
    x->n = 2u; x->lo[0] = (uint32_t)a; x->hi[0] = (uint32_t)(a >> 32); x->lo[1] = (uint32_t)b; x->hi[1] = (uint32_t)(b >> 32);
}
static inline void n48d2_aset_set(uint64_t a, uint64_t b) { n48d2_aset_set_i(N48D2_INST_MONB, a, b); }
// 0.0.637 (review item C3): the DP's (HUBP0's) surface size. max(our buffer, pitch_bytes x viewport height) rounded up to 64 KiB, from HUBPREQ0_DCSURF_SURFACE_PITCH (PITCH [13:0] = pixels - 1; 4 bytes a pixel: the precheck pins
// HUBP0's SURFACE_CONFIG to the ARGB8888 census value) and HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION (WIDTH [13:0], HEIGHT [29:16]). A pitch or height that reads 0, has a reserved bit set or exceeds 16384 px -> the conservative 64 MiB. Pure.
#define N48D2_DP_FALLBACK_BYTES 0x04000000ull
static inline uint64_t n48d2_dp_surface_bytes(uint32_t pitch_reg, uint32_t vp_reg)
{
    const uint32_t ph = vp_reg >> 16;
    uint64_t bytes;
    if (pitch_reg == 0u || (pitch_reg & ~0x3FFFu) != 0u) return N48D2_DP_FALLBACK_BYTES;
    if (ph == 0u || (vp_reg & 0xC000C000u) != 0u || ph > 16384u) return N48D2_DP_FALLBACK_BYTES;
    bytes = ((uint64_t)(pitch_reg & 0x3FFFu) + 1u) * 4u * (uint64_t)ph;
    bytes = (bytes + 0xFFFFull) & ~0xFFFFull;
    return bytes > (uint64_t)N48D2_BUF_BYTES ? bytes : (uint64_t)N48D2_BUF_BYTES;
}
// 0.0.636 (C3) / 0.0.637: does the DP's surface at `other` (a HUBP0 PRIMARY or EARLIEST_INUSE address, taken at the 64 KiB granularity the registers hold), dp_bytes long (0 = not judged yet: before op 8's precheck nothing is programmed -> our own size), overlap the monitor B buffer
// [buf, buf + N48D2_BUF_BYTES)? A buffer address of 0 (no buffer) overlaps nothing. Pure.
static inline int n48d2_surface_overlaps(uint64_t other, uint64_t buf, uint64_t dp_bytes)
{
    const uint64_t o = other & ~0xFFFFull;
    const uint64_t dp = dp_bytes < (uint64_t)N48D2_BUF_BYTES ? (uint64_t)N48D2_BUF_BYTES : dp_bytes;
    if (buf == 0u) return 0;
    return o < buf ? (buf - o) < dp : (o - buf) < (uint64_t)N48D2_BUF_BYTES;
}
// 0.0.655: the same for a buffer of `ours` bytes (the monitor A's is 8,323,072, the monitor B's 14,745,600): the DP surface starting ABOVE the buffer overlaps it only inside OUR size; the floor of the DP size is OUR size too.
static inline int n48d2_surface_overlaps_n(uint64_t other, uint64_t buf, uint64_t dp_bytes, uint64_t ours)
{
    const uint64_t o = other & ~0xFFFFull;
    const uint64_t dp = dp_bytes < ours ? ours : dp_bytes;
    if (buf == 0u) return 0;
    return o < buf ? (buf - o) < dp : (o - buf) < ours;
}
static inline int n48d2_aset_lo_ok_i(uint32_t inst, uint32_t v) { const struct n48d2_aset *x = n48d2_aset_get_i(inst); unsigned i; for (i = 0; i < x->n; i++) if (x->lo[i] == v) return 1; return 0; }
static inline int n48d2_aset_hi_ok_i(uint32_t inst, uint32_t v) { const struct n48d2_aset *x = n48d2_aset_get_i(inst); unsigned i; for (i = 0; i < x->n; i++) if (x->hi[i] == v) return 1; return 0; }
static inline int n48d2_aset_lo_ok(uint32_t v) { return n48d2_aset_lo_ok_i(N48D2_INST_MONB, v); }
static inline int n48d2_aset_hi_ok(uint32_t v) { return n48d2_aset_hi_ok_i(N48D2_INST_MONB, v); }

// The 24 gate dwords an op returns (v[4..12], two per scalar). All are final-state READS taken at the END of the op; the CRC pair is read by op 11 only.
#define N48D2_GATES 18u
enum { N48D2_GT_HUBP_CNTL = 0, N48D2_GT_HUBP_CLK = 1, N48D2_GT_ODM2 = 2, N48D2_GT_VMFAULT = 3, N48D2_GT_DET2 = 4, N48D2_GT_EARLY_LO = 5, N48D2_GT_EARLY_HI = 6, N48D2_GT_FLIP_CTL = 7, N48D2_GT_CRC_RG = 8, N48D2_GT_CRC_B = 9,
       N48D2_GT_MPCC_STATUS = 10, N48D2_GT_MPC_MUX = 11, N48D2_GT_A_LO = 12, N48D2_GT_A_HI = 13, N48D2_GT_B_LO = 14, N48D2_GT_B_HI = 15, N48D2_GT_FIFO = 16, N48D2_GT_META = 17 };
// META = stage | free_bufs << 8 | cur << 12 | the pre-existing ODM2 sticky bits (0x2400 mask) << 16 (bits 26 and 29) | 0.0.655, MONA ops only: the monitor B watch change mask (n48d2_w2_diff, rows 0..8) << 16 (bits 16..24)
// The health verdict over the gate dwords (pure; the flow and the host test use this very function): bit0 HUBP2 underflow [30:28], bit1 SEG_ALLOC_ERR [11], bit2 TIMEOUT [23:20], bit3 ODM2 bits 10 / 13 (except the stale ones in `stale`),
// bit4 DCN_VM_FAULT_STATUS != 0, bit5 DET2 current != 3, bit6 (checkLatch) FLIP_PENDING set or EARLIEST_INUSE != wantMc.
#define N48D2_H_UNDERFLOW 0x01u
#define N48D2_H_SEGALLOC  0x02u
#define N48D2_H_TIMEOUT   0x04u
#define N48D2_H_ODM       0x08u
#define N48D2_H_VMFAULT   0x10u
#define N48D2_H_DET       0x20u
#define N48D2_H_LATCH     0x40u
static inline uint32_t n48d2_health(const uint32_t *g, uint32_t stale, uint64_t wantMc, int checkLatch)
{
    uint32_t bad = 0u;
    if ((g[N48D2_GT_HUBP_CNTL] & N48D2_HUBP_UNDERFLOW_MASK) != 0u) bad |= N48D2_H_UNDERFLOW;
    if ((g[N48D2_GT_HUBP_CNTL] & N48D2_HUBP_SEG_ALLOC_ERR_MASK) != 0u) bad |= N48D2_H_SEGALLOC;
    if ((g[N48D2_GT_HUBP_CNTL] & N48D2_HUBP_TIMEOUT_MASK) != 0u) bad |= N48D2_H_TIMEOUT;
    if ((g[N48D2_GT_ODM2] & N48D2_ODM_UNDERFLOW_MASK & ~stale) != 0u) bad |= N48D2_H_ODM;
    if (g[N48D2_GT_VMFAULT] != 0u) bad |= N48D2_H_VMFAULT;
    if (((g[N48D2_GT_DET2] & N48D2_DET_CUR_MASK) >> 8) != 3u) bad |= N48D2_H_DET;
    if (checkLatch) {
        const uint64_t early = ((uint64_t)(g[N48D2_GT_EARLY_HI] & 0xFFFFu) << 32) | g[N48D2_GT_EARLY_LO];
        if ((g[N48D2_GT_FLIP_CTL] & N48D2_FLIP_PENDING_MASK) != 0u || early != (wantMc & 0x0000FFFFFFFFFFFFull)) bad |= N48D2_H_LATCH;
    }
    return bad;
}

// ---- 0.0.652 (M5): the HELD plane ----------------------------------------------------------------------------------------------------------------------------------------------------------------
// Ops refused while the plane is HELD (0.0.655: per instance - the glue asks the HELD state of the op's OWN instance, so the monitor B's HELD plane never affects the monitor A's ops and the reverse): everything that could move, un-program or re-time the plane the aux framebuffer is scanning: timing (1), connect (2), off (3), timing1440 (7),
// plane (8), show (9), flip (10), planeoff (12) and a second fbhold (14). Allowed: the read-only pages status / status2 / status3 (4, 5, 6), crc (11: it writes only the CRC registers) and planerec (13). Pure.
static inline int n48d2_held_refuses(uint32_t op, uint32_t inst)
{
    if (inst != N48D2_INST_MONB && inst != N48D2_INST_MONA) return 0;      /* 0.0.655: HELD is per instance - the same ops are refused on whichever instance is HELD (the other instance's ops are not affected: the glue asks disp2Held(inst)) */
    return op == N48D2_OP_TIMING || op == N48D2_OP_CONNECT || op == N48D2_OP_OFF || op == N48D2_OP_TIMING1440 || op == N48D2_OP_PLANE || op == N48D2_OP_SHOW ||
           op == N48D2_OP_FLIP || op == N48D2_OP_PLANEOFF || op == N48D2_OP_FBHOLD;
}
// `fbhold 2` / the publish step's live re-check, over the gate dwords of plane_gates(): 0 = the plane is healthy and buffer A is the front buffer (EARLIEST_INUSE == A, FLIP_PENDING clear, HUBP2 and ODM2 underflow 0, no SEG_ALLOC_ERR / TIMEOUT,
// DCN_VM_FAULT_STATUS 0, DET2 current 3); a non-zero value is the n48d2_health mask, or N48D2_H_FRONT_B when the plane's own bookkeeping says the latched buffer is B. `cur` is the plane's bookkeeping (0 = A). Pure.
#define N48D2_H_FRONT_B 0x80u
static inline uint32_t n48d2_hold_verdict(const uint32_t *g, uint32_t cur, uint64_t mcA)
{
    uint32_t bad = n48d2_health(g, 0u, mcA, 1);
    if ((cur & 1u) != 0u) bad |= N48D2_H_FRONT_B;
    return bad;
}

// The surface: eight vertical 320-px bars, ARGB8888 (0xFFRRGGBB): A white yellow cyan green magenta red blue black, B the same in REVERSE order.
static const uint32_t n48d2_bars[8] = { 0xFFFFFFFFu, 0xFFFFFF00u, 0xFF00FFFFu, 0xFF00FF00u, 0xFFFF00FFu, 0xFFFF0000u, 0xFF0000FFu, 0xFF000000u };
static inline uint32_t n48d2_bar_color(uint32_t buf, uint32_t bar) { return n48d2_bars[(buf ? 7u - bar : bar) & 7u]; }
// The surface of each instance (0.0.655): the monitor A's 1920x1080 frame (eight 240-px bars; verification rows 0 / 540 / 1079) in a 127 x 64 KiB buffer, the monitor B's 2560x1440 frame (eight 320-px bars; rows 0 / 720 / 1439) in a 225 x 64 KiB buffer.
struct n48d2_surf { uint32_t w, h, pitch_px, pitch_bytes, buf_bytes, bar_px, rows[3]; };
static const struct n48d2_surf n48d2_surfs[2] = {
    { N48D1_PLANE_W, N48D1_PLANE_H, N48D1_PITCH_PX, N48D1_PLANE_PITCH_BYTES, N48D1_BUF_BYTES, N48D1_BAR_PX, { 0u, 540u, 1079u } },
    { N48D2_PLANE_W, N48D2_PLANE_H, N48D2_PITCH_PX, N48D2_PLANE_PITCH_BYTES, N48D2_BUF_BYTES, N48D2_BAR_PX, { 0u, 720u, 1439u } },
};
static inline const struct n48d2_surf *n48d2_surf_get(uint32_t inst) { return inst == N48D2_INST_MONA ? &n48d2_surfs[0] : &n48d2_surfs[1]; }
// The 24 verification points of a buffer: three rows x the eight bar centres (x = bar_px * bar + bar_px / 2). Returns the byte offset in the buffer; *want = the colour. n48d2_vpoint is the monitor B's.
static inline uint32_t n48d2_vpoint_i(uint32_t inst, uint32_t buf, uint32_t i, uint32_t *want)
{
    const struct n48d2_surf *sf = n48d2_surf_get(inst);
    const uint32_t row = sf->rows[(i / 8u) % 3u], bar = i % 8u;
    *want = n48d2_bar_color(buf, bar);
    return row * sf->pitch_bytes + (bar * sf->bar_px + sf->bar_px / 2u) * 4u;
}
static inline uint32_t n48d2_vpoint(uint32_t buf, uint32_t i, uint32_t *want) { return n48d2_vpoint_i(N48D2_INST_MONB, buf, i, want); }
#define N48D2_VPOINTS 24u

// The 20 COPY0 pairs (destination on pipe 2, EXACT source on pipe 0, the census value the source must still hold). Rows 4-14 (DSCL2), 16-18 (HUBP2) and 19-24 (HUBPREQ2) of the table in the spec.
// 0.0.637: wmask = the bits of the source that are WRITTEN to the destination and READ BACK (the drift tripwire on the source always compares the FULL value). 0xFFFFFFFF for 19 pairs; DSCL2_LB_MEMORY_CTRL takes Linux's two fields only.
struct n48d2_mirror { uint32_t dst, src, expect, wmask; const char *name; };
#define N48D2_MIRROR_FULL 0xFFFFFFFFu
#define N48D2_LB_MEMORY_CTRL_WMASK 0x00003F03u   /* MEMORY_CONFIG [1:0] + LB_MAX_PARTITIONS [13:8]; LB_NUM_PARTITIONS [22:16] / _C [30:24] are left at 0 (dcn401_dpp_dscl.c: REG_SET_2(LB_MEMORY_CTRL, 0, MEMORY_CONFIG, ..., LB_MAX_PARTITIONS, ...)) */
#define N48D2_MIRROR0_COUNT 20u
static const struct n48d2_mirror n48d2_mirror0[N48D2_MIRROR0_COUNT] = {
    { N48D2_DSCL2_SCL_MODE, 0x41c8u, 0x00000001u, N48D2_MIRROR_FULL, "DSCL2_SCL_MODE <- DSCL0_SCL_MODE" },
    { N48D2_DSCL2_DSCL_2TAP_CONTROL, 0x41cbu, 0x01110111u, N48D2_MIRROR_FULL, "DSCL2_DSCL_2TAP_CONTROL <- DSCL0_DSCL_2TAP_CONTROL" },
    { N48D2_DSCL2_SCL_HORZ_FILTER_SCALE_RATIO, 0x41cdu, 0x01000000u, N48D2_MIRROR_FULL, "DSCL2_SCL_HORZ_FILTER_SCALE_RATIO <- DSCL0_SCL_HORZ_FILTER_SCALE_RATIO" },
    { N48D2_DSCL2_SCL_HORZ_FILTER_INIT, 0x41ceu, 0x00000000u, N48D2_MIRROR_FULL, "DSCL2_SCL_HORZ_FILTER_INIT <- DSCL0_SCL_HORZ_FILTER_INIT" },
    { N48D2_DSCL2_SCL_VERT_FILTER_SCALE_RATIO, 0x41d1u, 0x01000000u, N48D2_MIRROR_FULL, "DSCL2_SCL_VERT_FILTER_SCALE_RATIO <- DSCL0_SCL_VERT_FILTER_SCALE_RATIO" },
    { N48D2_DSCL2_SCL_VERT_FILTER_INIT, 0x41d2u, 0x00000000u, N48D2_MIRROR_FULL, "DSCL2_SCL_VERT_FILTER_INIT <- DSCL0_SCL_VERT_FILTER_INIT" },
    { N48D2_DSCL2_OTG_H_BLANK, 0x41dcu, 0x00700a70u, N48D2_MIRROR_FULL, "DSCL2_OTG_H_BLANK <- DSCL0_OTG_H_BLANK" },
    { N48D2_DSCL2_OTG_V_BLANK, 0x41ddu, 0x002605c6u, N48D2_MIRROR_FULL, "DSCL2_OTG_V_BLANK <- DSCL0_OTG_V_BLANK" },
    { N48D2_DSCL2_RECOUT_SIZE, 0x41dfu, 0x05a00a00u, N48D2_MIRROR_FULL, "DSCL2_RECOUT_SIZE <- DSCL0_RECOUT_SIZE" },
    { N48D2_DSCL2_MPC_SIZE, 0x41e0u, 0x05a00a00u, N48D2_MIRROR_FULL, "DSCL2_MPC_SIZE <- DSCL0_MPC_SIZE" },
    { N48D2_DSCL2_LB_MEMORY_CTRL, 0x41e2u, 0x08083f00u, N48D2_LB_MEMORY_CTRL_WMASK, "DSCL2_LB_MEMORY_CTRL <- DSCL0_LB_MEMORY_CTRL" },
    { N48D2_HUBP2_DCSURF_PRI_VIEWPORT_DIMENSION, 0x3aabu, 0x05a00a00u, N48D2_MIRROR_FULL, "HUBP2_DCSURF_PRI_VIEWPORT_DIMENSION <- HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION" },
    { N48D2_HUBP2_DCSURF_SEC_VIEWPORT_DIMENSION, 0x3aafu, 0x05a00a00u, N48D2_MIRROR_FULL, "HUBP2_DCSURF_SEC_VIEWPORT_DIMENSION <- HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION" },
    { N48D2_HUBP2_DCHUBP_REQ_SIZE_CONFIG, 0x3ab2u, 0x05500300u, N48D2_MIRROR_FULL, "HUBP2_DCHUBP_REQ_SIZE_CONFIG <- HUBP0_DCHUBP_REQ_SIZE_CONFIG" },
    { N48D2_HUBPREQ2_DCN_GLOBAL_TTU_CNTL, 0x3ae2u, 0xe00050aau, N48D2_MIRROR_FULL, "HUBPREQ2_DCN_GLOBAL_TTU_CNTL <- HUBPREQ0_DCN_GLOBAL_TTU_CNTL" },
    { N48D2_HUBPREQ2_DCN_SURF0_TTU_CNTL0, 0x3ae3u, 0x08000000u, N48D2_MIRROR_FULL, "HUBPREQ2_DCN_SURF0_TTU_CNTL0 <- HUBPREQ0_DCN_SURF0_TTU_CNTL0" },
    { N48D2_HUBPREQ2_BLANK_OFFSET_0, 0x3afbu, 0x00260017u, N48D2_MIRROR_FULL, "HUBPREQ2_BLANK_OFFSET_0 <- HUBPREQ0_BLANK_OFFSET_0" },
    { N48D2_HUBPREQ2_PREFETCH_SETTINGS, 0x3affu, 0x28080000u, N48D2_MIRROR_FULL, "HUBPREQ2_PREFETCH_SETTINGS <- HUBPREQ0_PREFETCH_SETTINGS" },
    { N48D2_HUBPREQ2_PREFETCH_SETTINGS_C, 0x3b00u, 0x00080000u, N48D2_MIRROR_FULL, "HUBPREQ2_PREFETCH_SETTINGS_C <- HUBPREQ0_PREFETCH_SETTINGS_C" },
    { N48D2_HUBPREQ2_VBLANK_PARAMETERS_0, 0x3b01u, 0x00000301u, N48D2_MIRROR_FULL, "HUBPREQ2_VBLANK_PARAMETERS_0 <- HUBPREQ0_VBLANK_PARAMETERS_0" },
};
static inline int n48d2_mirror_dst(uint32_t abs) { unsigned i; for (i = 0; i < N48D2_MIRROR0_COUNT; i++) if (n48d2_mirror0[i].dst == abs) return 1; return 0; }
static inline int n48d2_mirror_ok(uint32_t dst, uint32_t src, uint32_t expect, uint32_t wmask) { unsigned i; for (i = 0; i < N48D2_MIRROR0_COUNT; i++) if (n48d2_mirror0[i].dst == dst && n48d2_mirror0[i].src == src && n48d2_mirror0[i].expect == expect && n48d2_mirror0[i].wmask == wmask) return 1; return 0; }

// The EXPECT list: registers that must hold the same census value on pipe 2 AND pipe 0 (the "equal-already" list), plus the OTG raster / global-sync constants OTG2 and OTG0 must both show. { pipe-2 dword, pipe-0 dword, mask, value, name }
struct n48d2_exp { uint32_t abs2, abs0, mask, val; const char *name; };
#define N48D2_EXP_COUNT 34u
static const struct n48d2_exp n48d2_exp[N48D2_EXP_COUNT] = {
    { 0x3c5du, 0x3aa5u, 0xffffffffu, 0x00000008u, "HUBP2_DCSURF_SURFACE_CONFIG" },
    { 0x3c5eu, 0x3aa6u, 0xffffffffu, 0x00000000u, "HUBP2_DCSURF_ADDR_CONFIG" },
    { 0x3c5fu, 0x3aa7u, 0xffffffffu, 0x00000080u, "HUBP2_DCSURF_TILING_CONFIG" },
    { 0x3c61u, 0x3aa9u, 0xffffffffu, 0x00000000u, "HUBP2_DCSURF_PRI_VIEWPORT_START" },
    { 0x3c74u, 0x3abcu, 0xffffffffu, 0x00000100u, "HUBP2_HUBPREQ_DEBUG_DB" },
    { 0x3c75u, 0x3abdu, 0xffffffffu, 0x04000000u, "HUBP2_HUBPREQ_DEBUG" },
    { 0x3c6cu, 0x3ab4u, 0x00000100u, 0x00000000u, "HUBP2_DCHUBP_CNTL" },
    { 0x3c98u, 0x3ae0u, 0xffffffffu, 0x00000055u, "HUBPREQ2_DCN_EXPANSION_MODE" },
    { 0x3c99u, 0x3ae1u, 0xffffffffu, 0x06920000u, "HUBPREQ2_DCN_TTU_QOS_WM" },
    { 0x3cb4u, 0x3afcu, 0xffffffffu, 0x00001136u, "HUBPREQ2_BLANK_OFFSET_1" },
    { 0x3cb5u, 0x3afdu, 0xffffffffu, 0x0001e641u, "HUBPREQ2_DST_DIMENSIONS" },
    { 0x3cbau, 0x3b02u, 0xffffffffu, 0x000001a4u, "HUBPREQ2_VBLANK_PARAMETERS_1" },
    { 0x3cbcu, 0x3b04u, 0xffffffffu, 0x000001a4u, "HUBPREQ2_VBLANK_PARAMETERS_3" },
    { 0x3cc1u, 0x3b09u, 0xffffffffu, 0x00000200u, "HUBPREQ2_NOM_PARAMETERS_0" },
    { 0x3cc2u, 0x3b0au, 0xffffffffu, 0x0000d249u, "HUBPREQ2_NOM_PARAMETERS_1" },
    { 0x3cc5u, 0x3b0du, 0xffffffffu, 0x00000020u, "HUBPREQ2_NOM_PARAMETERS_4" },
    { 0x3cc6u, 0x3b0eu, 0xffffffffu, 0x00000d24u, "HUBPREQ2_NOM_PARAMETERS_5" },
    { 0x3cccu, 0x3b14u, 0xffffffffu, 0x00014e5eu, "HUBPREQ2_REF_FREQ_TO_PIX_FREQ" },
    { 0x3ccdu, 0x3b15u, 0xffffffffu, 0x00003fffu, "HUBPREQ2_DST_Y_DELTA_DRQ_LIMIT" },
    { 0x3c8au, 0x3ad2u, 0xffffffffu, 0x00000000u, "HUBPREQ2_DCSURF_SURFACE_CONTROL" },
    { 0x3c8bu, 0x3ad3u, 0x00000003u, 0x00000000u, "HUBPREQ2_DCSURF_FLIP_CONTROL" },
    { 0x3ce5u, 0x3b2du, 0xffffffffu, 0x00e40000u, "HUBPRET2_HUBPRET_CONTROL" },
    { 0x4465u, 0x418fu, 0xffffffffu, 0x00000008u, "CNVC_CFG2_CNVC_SURFACE_PIXEL_FORMAT" },
    { 0x4466u, 0x4190u, 0xffffffffu, 0x24000000u, "CNVC_CFG2_FORMAT_CONTROL" },
    { 0x4475u, 0x419fu, 0xffffffffu, 0x00000000u, "CNVC_CFG2_PRE_CSC_MODE" },
    { 0x44fdu, 0x4227u, 0x00000001u, 0x00000001u, "CM2_CM_CONTROL" },
    { 0x902du, 0x9003u, 0xffffffffu, 0xffff0461u, "MPCC2_MPCC_CONTROL" },
    { 0x92fbu, 0x92f3u, 0xffffffffu, 0x00fff000u, "MPC_OUT2_DENORM_CONTROL" },
    { 0x9325u, 0x930bu, 0xffffffffu, 0x00000000u, "MPC_OUT2_CSC_MODE" },
    { N48D2_OTG2_H_TOTAL, N48D2_OTG0_H_TOTAL, 0xFFFFFFFFu, N48D2_RASTER_HTOTAL_RAW, "OTG_H_TOTAL (1440p60 raster)" },
    { N48D2_OTG2_V_TOTAL, N48D2_OTG0_V_TOTAL, 0xFFFFFFFFu, N48D2_RASTER_VTOTAL_RAW, "OTG_V_TOTAL (1440p60 raster)" },
    { N48D2_OTG2_VSTARTUP_PARAM, N48D2_OTG0_VSTARTUP_PARAM, 0xFFFFFFFFu, N48D2_VSTARTUP_LINES, "OTG_VSTARTUP_PARAM" },
    { N48D2_OTG2_VUPDATE_PARAM, N48D2_OTG0_VUPDATE_PARAM, 0xFFFFFFFFu, (N48D2_VUPDATE_WIDTH << 16) | N48D2_VUPDATE_OFFSET, "OTG_VUPDATE_PARAM" },
    { N48D2_OTG2_VREADY_PARAM, N48D2_OTG0_VREADY_PARAM, 0xFFFFFFFFu, N48D2_VREADY_OFFSET, "OTG_VREADY_PARAM" },
};
// Pipe-2-only state the plane op needs (reads only). The first six rows are the "state" of the design's step 1.
struct n48d2_pre1 { uint32_t abs, mask, val; const char *name; };
#define N48D2_PRE_STATE_COUNT 18u
static const struct n48d2_pre1 n48d2_pre_state[N48D2_PRE_STATE_COUNT] = {
    { N48D2_OTG2_CONTROL, 0x00010001u, 0x00010001u, "OTG2 master enabled" },
    { N48D2_DPG2_CONTROL, N48D2_DPG_CONTROL_MASK, N48D2_DPG_CONTROL_ON, "DPG2 on (colour squares 2560x1440)" },
    { N48D2_HUBP2_HUBP_CLK_CNTL, N48D2_HUBP_CLK_WRITE_MASK & 1u, 0u, "HUBP2 clock off" },
    { N48D2_HUBP2_DCHUBP_CNTL, 0x000000F4u, 0x00000020u, "HUBP2 VTG_SEL 2, not in soft reset" },
    { N48D2_DPP_TOP2_DPP_CONTROL, N48D2_DPP_CLOCK_ENABLE_MASK, 0u, "DPP_TOP2 clock off" },
    { N48D2_MPCC2_MPCC_TOP_SEL, 0xFFFFFFFFu, N48D2_MPCC_NONE, "MPCC2 TOP none" },
    { N48D2_MPCC2_MPCC_BOT_SEL, 0xFFFFFFFFu, N48D2_MPCC_NONE, "MPCC2 BOT none" },
    { N48D2_MPCC2_MPCC_OPP_ID, 0xFFFFFFFFu, N48D2_MPCC_NONE, "MPCC2 OPP none" },
    { N48D2_MPC_OUT2_MUX, 0x0000000Fu, 0x0000000Fu, "MPC_OUT2 mux none" },
    { N48D2_DPPCLK_CTRL, N48D2_DPPCLK2_EN_MASK, 0u, "DPPCLK2_EN clear" },
    { N48D2_DPPCLK2_DTO_PARAM, 0xFFFFFFFFu, 0u, "DPPCLK2 DTO 0" },
    { N48D2_DET0_CTRL, 0xFFFFFFFFu, 0x00000303u, "DET0 size 3 current 3" },
    { N48D2_DCHUBBUB_DET2_CTRL, N48D2_DET_SIZE_MASK, 3u, "DET2 size 3" },
    { N48D2_DCN_VM_FAULT_STATUS, 0xFFFFFFFFu, 0u, "DCN_VM_FAULT_STATUS clear" },
    { N48D2_HUBPREQ2_VM_APERTURE_LOW, 0xFFFFFFFFu, 0u, "HUBPREQ2 system aperture low 0" },
    { N48D2_HUBPREQ2_VM_APERTURE_HIGH, 0xFFFFFFFFu, 0u, "HUBPREQ2 system aperture high 0" },
    { N48D2_HUBPREQ2_VM_MX_L1_TLB, 0xFFFFFFFFu, 0u, "HUBPREQ2 MX_L1_TLB_CNTL 0" },
    { N48D2_HUBPREQ2_VMID_SETTINGS_0, 0xFFFFFFFFu, 0u, "HUBPREQ2 VMID_SETTINGS_0 0" },
};
// Registers that were never censused: they must be EQUAL on pipe 2 and its pipe-0 twin (reads only).
struct n48d2_eq { uint32_t a2, a0; const char *name; };
#define N48D2_EQ_COUNT 4u
static const struct n48d2_eq n48d2_eqpairs[N48D2_EQ_COUNT] = {
    { (N48D2_SEG3 + 0x013au), (N48D2_SEG3 + 0x007eu), "MPCC_OGAM2 / 0 MPCC_OGAM_CONTROL" }, { (N48D2_SEG3 + 0x05b3u), (N48D2_SEG3 + 0x0453u), "MPCC_MCM2 / 0 MPCC_MCM_SHAPER_CONTROL" },
    { (N48D2_SEG3 + 0x05eau), (N48D2_SEG3 + 0x048au), "MPCC_MCM2 / 0 MPCC_MCM_3DLUT_MODE" }, { (N48D2_SEG3 + 0x05f3u), (N48D2_SEG3 + 0x0493u), "MPCC_MCM2 / 0 MPCC_MCM_1DLUT_CONTROL" },
};
// The precheck as step PAGES of EXPECT steps (<= 48 each): the state rows, then each EXPECT row's pipe-2 register and its pipe-0 twin, then the 20 COPY0 sources. 106 steps, 3 pages. Reads only.
#define N48D2_PRE_TOTAL (N48D2_PRE_STATE_COUNT + 2u * N48D2_EXP_COUNT + N48D2_MIRROR0_COUNT)
#define N48D2_PRE_PAGES 3u
static inline uint32_t n48d2_build_pre(uint32_t page, struct n48d2_step *s)
{
    uint32_t n = 0u, i;
    for (i = page * N48D2_MAX_STEPS; i < N48D2_PRE_TOTAL && n < N48D2_MAX_STEPS; i++) {
        uint32_t abs, mask, val; const char *name;
        if (i < N48D2_PRE_STATE_COUNT) { abs = n48d2_pre_state[i].abs; mask = n48d2_pre_state[i].mask; val = n48d2_pre_state[i].val; name = n48d2_pre_state[i].name; }
        else if (i < N48D2_PRE_STATE_COUNT + 2u * N48D2_EXP_COUNT) {
            const uint32_t j = i - N48D2_PRE_STATE_COUNT, r = j / 2u;
            abs = (j & 1u) ? n48d2_exp[r].abs0 : n48d2_exp[r].abs2; mask = n48d2_exp[r].mask; val = n48d2_exp[r].val; name = n48d2_exp[r].name;
        } else {
            const uint32_t m = i - N48D2_PRE_STATE_COUNT - 2u * N48D2_EXP_COUNT;
            abs = n48d2_mirror0[m].src; mask = 0xFFFFFFFFu; val = n48d2_mirror0[m].expect; name = n48d2_mirror0[m].name;
        }
        n48d2_put(s, &n, N48D2_K_EXPECT, abs, mask, val, 0u, 0u, 0u, 0u, name, "precheck (read only)");
    }
    return n;
}
static inline void n48d2_put_mirror(struct n48d2_step *v, uint32_t *n, uint32_t i)
{
    n48d2_put(v, n, N48D2_K_COPY0, n48d2_mirror0[i].dst, 0xFFFFFFFFu, n48d2_mirror0[i].expect, 0u, 0u, n48d2_mirror0[i].src, n48d2_mirror0[i].wmask, n48d2_mirror0[i].name, "COPY0 (pipe 0 -> pipe 2)");
}

// op 8, 0.0.636 order (Linux's: dcn20_enable_plane / dcn401 program_pipe turn the clocks on BEFORE they program; the 0.0.635 run showed DSCL2_LB_MEMORY_CTRL does not take with HUBP2 / DPP2 unclocked).
//   part A (8 steps, BEFORE the HUBP clock): rows 1-3 the DPP clocks (dccg401_update_dpp_dto), row 15 BLANK_EN = 1 and the WAIT that bit 0 reads 1 (the ONLY readback gate before the clock goes on), then 25-27 pitch, address HIGH then LOW
//   (so the address is valid, A, before the clock goes on and a blanked HUBP2 fetches nothing).  part B (3 steps): the HUBP clock.  part D (22 steps, AFTER the clock and DET2): rows 4-14 DSCL2 and 16-24 HUBP2 / HUBPREQ2 (COPY0), then the address
//   HIGH / LOW AGAIN (the pre-clock writes may not have landed).  part C: MPCC2 / the mux / UNBLANK.
#define N48D2_PA_STEPS 8u
#define N48D2_PA_I_BLANK 3u
#define N48D2_PA_I_BLANKWAIT 4u
static inline uint32_t n48d2_build_plane_a(struct n48d2_step *s, uint64_t mc)
{
    uint32_t n = 0u;
    N48D2_SET(DPPCLK2_DTO_PARAM, N48D2_DPPCLK_DTO_VALUE, "dccg401_update_dpp_dto");
    N48D2_UPD(DPPCLK_CTRL, N48D2_DPPCLK2_EN_MASK, N48D2_DPPCLK2_EN_MASK, "dccg401_update_dpp_dto");
    N48D2_UPD(DPP_TOP2_DPP_CONTROL, N48D2_DPP_CLOCK_ENABLE_MASK, N48D2_DPP_CLOCK_ENABLE_MASK, "dpp1_dppclk_control");
    N48D2_UPD(HUBP2_DCHUBP_CNTL, N48D2_HUBP_BLANK_EN_MASK, N48D2_HUBP_BLANK_EN_MASK, "hubp1_set_blank (BLANK_EN = 1 BEFORE the HUBP clock)");
    N48D2_WAIT(HUBP2_DCHUBP_CNTL, N48D2_HUBP_BLANK_EN_MASK, N48D2_HUBP_BLANK_EN_MASK, 100u, 10u, "blank read back");
    N48D2_SET(HUBPREQ2_DCSURF_SURFACE_PITCH, N48D2_PITCH_PX, "hubp401_program_pitch");
    N48D2_SET(HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, (uint32_t)(mc >> 32), "dcn41_hubp_program_flip (HIGH first)");
    N48D2_SET(HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)mc, "dcn41_hubp_program_flip (LOW)");
    return n;
}
// op 8, part B (6 steps; 0.0.639): row 28 the HUBP clock (HUBP0's writable bits; a bare REG_UPDATE as Linux's hubp2_clk_cntl), then the four RECORDED reads (no gate) ~1 ms after the enable, then a fifth RECORDED read (no gate) of
// DCHUBBUB_DET2_CTRL. The 0.0.638 run showed DET2 current stays 0 while the pipe is blanked and unconnected (DET2_CTRL 0x3 for the whole 100 ms): the wait moved to part E, AFTER the unblank (Linux's
// dcn401_wait_for_det_buffer_update_under_otg_master / dcn401_wait_for_det_update run after the pipe is enabled and committed). The 0.0.637 WAIT for HUBP2_HUBP_CLK_CNTL status 23:20 == 0xf stays gone (demand-gated clocks).
#define N48D2_PB_STEPS 6u
static inline uint32_t n48d2_build_plane_b(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_UPD(HUBP2_HUBP_CLK_CNTL, N48D2_HUBP_CLK_WRITE_MASK, N48D2_HUBP_CLK_WRITE_MASK, "hubp1_enable_clock");
    N48D2_REC(HUBP2_HUBP_CLK_CNTL, 0u, 1000u, "recorded read ~1 ms after the HUBP clock enable (no gate)");
    N48D2_REC(DCCG_GATE_DISABLE_CNTL6, 1u, 0u, "recorded read after the HUBP clock enable (no gate)");
    N48D2_REC(DCCG_GATE_DISABLE_CNTL, 2u, 0u, "recorded read after the HUBP clock enable (no gate)");
    N48D2_REC(DOMAIN2_PG_STATUS, 3u, 0u, "recorded read after the HUBP clock enable (no gate)");
    N48D2_REC(DCHUBBUB_DET2_CTRL, N48D2_RS_DET, 0u, "recorded read of DET2_CTRL before the unblank (no gate: the DET is allocated at the first VUPDATE after it)");
    return n;
}
// op 8, part E (1 step; 0.0.639): AFTER the unblank and the EARLIEST_INUSE gate: WAIT DET2 current = 3, 100 polls x 1 ms = 100 ms (Linux: "1 vupdate at 10hz", dcn401_hubbub.c dcn401_wait_for_det_update).
#define N48D2_PE_STEPS 1u
#define N48D2_PE_I_DETWAIT 0u
#define N48D2_PE_DET_POLLS 100u
#define N48D2_PE_DET_STEP_US 1000u
static inline uint32_t n48d2_build_plane_e(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_WAIT(DCHUBBUB_DET2_CTRL, N48D2_DET_CUR_MASK, 3u << 8, N48D2_PE_DET_POLLS, N48D2_PE_DET_STEP_US, "DET2 current = 3 (dcn401_wait_for_det_update, after the unblank)");
    return n;
}
// op 8, the second recorded sample (4 REC steps, 0.0.638): the same four registers after the 2-frame wait (no gate).
#define N48D2_PR_STEPS 4u
static inline uint32_t n48d2_build_plane_rec2(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_REC(HUBP2_HUBP_CLK_CNTL, 4u, 0u, "recorded read after the 2-frame wait (no gate)");
    N48D2_REC(DCCG_GATE_DISABLE_CNTL6, 5u, 0u, "recorded read after the 2-frame wait (no gate)");
    N48D2_REC(DCCG_GATE_DISABLE_CNTL, 6u, 0u, "recorded read after the 2-frame wait (no gate)");
    N48D2_REC(DOMAIN2_PG_STATUS, 7u, 0u, "recorded read after the 2-frame wait (no gate)");
    return n;
}
// op 8, the LOCK (0.0.643; 3 steps): Linux's optc3_lock (dcn401 .lock = optc3_lock) for OTG instance 2: REG_UPDATE(OTG_GLOBAL_CONTROL2, OTG_MASTER_UPDATE_LOCK_SEL, 2), REG_SET(OTG_MASTER_UPDATE_LOCK, OTG_MASTER_UPDATE_LOCK, 1),
// then the wait for UPDATE_LOCK_STATUS = 1 (optc3_lock waits 10 x 1 us; dcn401's optc401_wait_update_lock_status 150000 x 1 us; here 2000 x 50 us = 100 ms, bounded). dcn20_hwseq.c update_dchubp_dpp runs inside this lock (dcn20_pipe_control_lock
// -> tg->funcs->lock): update_plane_addr, then set_blank(false) for a new pipe; the address, the MPCC insert and the unblank all take effect at ONE VUPDATE when the lock is released.
#define N48D2_LK_STEPS 3u
#define N48D2_LK_I_SEL 0u
#define N48D2_LK_I_LOCK 1u
#define N48D2_LK_I_WAIT 2u
#define N48D2_LK_POLLS 2000u
#define N48D2_LK_STEP_US 50u
static inline uint32_t n48d2_build_plane_lock(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_UPD(OTG2_OTG_GLOBAL_CONTROL2, N48D2_OTG_LOCK_SEL_MASK, N48D2_OTG_LOCK_SEL_OTG2, "optc3_lock (OTG_MASTER_UPDATE_LOCK_SEL = 2)");
    N48D2_UPD(OTG2_OTG_MASTER_UPDATE_LOCK, N48D2_OTG_MASTER_UPDATE_LOCK_MASK, N48D2_OTG_MASTER_UPDATE_LOCK_MASK, "optc3_lock (OTG_MASTER_UPDATE_LOCK = 1)");
    N48D2_WAIT(OTG2_OTG_MASTER_UPDATE_LOCK, N48D2_OTG_UPDATE_LOCK_STATUS_MASK, N48D2_OTG_UPDATE_LOCK_STATUS_MASK, N48D2_LK_POLLS, N48D2_LK_STEP_US, "optc3_lock: UPDATE_LOCK_STATUS = 1");
    return n;
}
// op 8, the UNLOCK (0.0.643; 1 step): Linux's optc1_unlock: OTG_MASTER_UPDATE_LOCK = 0. Issued right after the locked list (part C), and by op 8 on EVERY exit path while the lock is held.
#define N48D2_PU_STEPS 1u
static inline uint32_t n48d2_build_plane_unlock(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_UPD(OTG2_OTG_MASTER_UPDATE_LOCK, N48D2_OTG_MASTER_UPDATE_LOCK_MASK, 0u, "optc1_unlock (OTG_MASTER_UPDATE_LOCK = 0)");
    return n;
}
// op 8, part C (8 steps; 0.0.643): the writes INSIDE the lock, in dcn20_hwseq.c update_dchubp_dpp's order, issued back to back (no frame wait, no sleep between them): rows 25-26 the primary address HIGH then LOW (A; update_plane_addr),
// 29-32 MPCC2 BOT, TOP, OPP, UPDATE_LOCK_SEL (mpc1_insert_plane's order), 33 MPC_OUT2_MUX, 34 UNBLANK (set_blank(false)). The lock is taken BEFORE this list and released right AFTER it; the post gates (FLIP_PENDING == 0 and
// EARLIEST_INUSE == A, then DET2) follow the unlock. 0.0.643 removes the pre-unblank FLIP_PENDING gate: a blanked HUBP never consumes a pending address (the 0.0.639 run: FLIP_CONTROL stayed 0x04100100 for 5 frames).
#define N48D2_PC_STEPS 8u
#define N48D2_PC_I_ADDR_HI 0u
#define N48D2_PC_I_ADDR_LO 1u
#define N48D2_PC_I_UNBLANK 7u
static inline uint32_t n48d2_build_plane_c(struct n48d2_step *s, uint64_t mc)
{
    uint32_t n = 0u;
    N48D2_SET(HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, (uint32_t)(mc >> 32), "dcn41_hubp_program_flip (HIGH first, inside the OTG2 update lock)");
    N48D2_SET(HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)mc, "dcn41_hubp_program_flip (LOW, inside the OTG2 update lock)");
    N48D2_SET(MPCC2_MPCC_BOT_SEL, N48D2_MPCC_NONE, "mpc1_insert_plane");
    N48D2_SET(MPCC2_MPCC_TOP_SEL, N48D2_OTG2_INST, "mpc1_insert_plane");
    N48D2_SET(MPCC2_MPCC_OPP_ID, N48D2_OTG2_INST, "mpc1_insert_plane");
    N48D2_SET(MPCC2_MPCC_UPDATE_LOCK_SEL, N48D2_OTG2_INST, "mpc1_insert_plane");
    N48D2_SET(MPC_OUT2_MUX, N48D2_MPC_OUT2_MUX_ON, "mpc1_insert_plane (MPC_OUT_MUX = 2 + RATE_CONTROL_DISABLE)");
    N48D2_UPD(HUBP2_DCHUBP_CNTL, N48D2_HUBP_BLANK_EN_MASK, 0u, "hubp1_set_blank (unblank, inside the OTG2 update lock)");
    return n;
}
// op 8, part D (22 steps; AFTER the HUBP clock and DET2 current = 3): the 20 COPY0 pairs (rows 4-14 DSCL2, then 16-24 HUBP2 / HUBPREQ2), then the primary address HIGH / LOW AGAIN (0.0.636, review item C2: Linux programs it with the clock on).
#define N48D2_PD_STEPS 22u
static inline uint32_t n48d2_build_plane_d(struct n48d2_step *s, uint64_t mc)
{
    uint32_t n = 0u, i;
    for (i = 0u; i < 20u; i++) n48d2_put_mirror(s, &n, i);
    N48D2_SET(HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, (uint32_t)(mc >> 32), "dcn41_hubp_program_flip (HIGH first, again with the clock on)");
    N48D2_SET(HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)mc, "dcn41_hubp_program_flip (LOW, again with the clock on)");
    return n;
}
// op 9 (4 steps in two lists): rows 35-36 the underflow clears; then DPG_EN = 0 and the DPG pending wait.
#define N48D2_SHOW1_STEPS 2u
static inline uint32_t n48d2_build_show1(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_UPD(HUBP2_DCHUBP_CNTL, N48D2_HUBP_UNDERFLOW_CLEAR_MASK, N48D2_HUBP_UNDERFLOW_CLEAR_MASK, "hubp1_clear_underflow");
    N48D2_UPD(ODM2_OPTC_INPUT_GLOBAL_CONTROL, N48D2_ODM_UNDERFLOW_CLEAR_MASK, N48D2_ODM_UNDERFLOW_CLEAR_MASK, "optc1_clear_optc_underflow");
    return n;
}
#define N48D2_SHOW2_STEPS 2u
#define N48D2_SHOW2_I_WAIT 1u
static inline uint32_t n48d2_build_show2(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_UPD(DPG2_CONTROL, N48D2_DPG_EN_MASK, 0u, "opp2_set_disp_pattern_generator (VIDEOMODE)");
    N48D2_WAIT(DPG2_STATUS, N48D2_DPG_PENDING_MASK, 0u, 1000u, 100u, "dpg pending clear");
    return n;
}
// op 10 (5 steps): HIGH then LOW of the chosen buffer, the FLIP_PENDING wait, then (0.0.636, review item C1: Linux's hubp2_is_flip_pending also calls the flip pending while EARLIEST_INUSE != the requested address) the EARLIEST_INUSE LOW and
// HIGH waits, each 100 x 1 ms.
#define N48D2_FLIP_STEPS 5u
#define N48D2_FLIP_I_WAIT 2u
#define N48D2_FLIP_I_WAIT_LO 3u
#define N48D2_FLIP_I_WAIT_HI 4u
static inline uint32_t n48d2_build_flip(struct n48d2_step *s, uint64_t mc)
{
    uint32_t n = 0u;
    N48D2_SET(HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, (uint32_t)(mc >> 32), "hubp surface flip (HIGH)");
    N48D2_SET(HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)mc, "hubp surface flip (LOW)");
    N48D2_WAIT(HUBPREQ2_DCSURF_FLIP_CONTROL, N48D2_FLIP_PENDING_MASK, 0u, 100u, 1000u, "flip pending clear");
    N48D2_WAIT(HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE, 0xFFFFFFFFu, (uint32_t)mc, 100u, 1000u, "hubp2_is_flip_pending: EARLIEST_INUSE = the requested address (LOW)");
    N48D2_WAIT(HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, 0x0000FFFFu, (uint32_t)(mc >> 32) & 0xFFFFu, 100u, 1000u, "hubp2_is_flip_pending: EARLIEST_INUSE = the requested address (HIGH)");
    return n;
}
// op 11 (5 steps): optc1_configure_crc over the 2560x1440 raster: the four windows, then CONT_EN + SELECT 0 + EN in one update (mask 0x00700011, value 0x11).
#define N48D2_CRC_STEPS 5u
static inline uint32_t n48d2_build_crc(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_SET(OTG2_OTG_CRC0_WINDOWA_X_CONTROL, N48D2_CRC_WIN_X, "optc1_configure_crc");
    N48D2_SET(OTG2_OTG_CRC0_WINDOWA_Y_CONTROL, N48D2_CRC_WIN_Y, "optc1_configure_crc");
    N48D2_SET(OTG2_OTG_CRC0_WINDOWB_X_CONTROL, N48D2_CRC_WIN_X, "optc1_configure_crc");
    N48D2_SET(OTG2_OTG_CRC0_WINDOWB_Y_CONTROL, N48D2_CRC_WIN_Y, "optc1_configure_crc");
    N48D2_UPD(OTG2_OTG_CRC_CNTL, N48D2_CRC_CNTL_MASK, N48D2_CRC_CNTL_ON, "optc1_configure_crc");
    return n;
}
// op 12 and every failure's rollback (18 steps): DPG2 back on FIRST (it covers the screen), BLANK_EN = 1 and wait NO_OUTSTANDING_REQ, 0.0.638: the primary address HIGH then LOW written to 0 (so the HUBP holds no pointer into the buffers about to
// be released; the list guard demands the blank + the NO_OUTSTANDING wait earlier in the same list), MPC_OUT2_MUX none, MPCC2 TOP / BOT / OPP / LOCK none and wait MPCC2_STATUS & 7 = 5,
// CRC off, the HUBP clock off and wait it gated, DPP clock off, DPPCLK2_EN clear, DTO 0. BLANK_EN stays 1 (the census value was 0: recorded in the notes). The buffers are released only after the whole list ran.
#define N48D2_PO_STEPS 18u
#define N48D2_PO_I_DPGWAIT 1u
#define N48D2_PO_I_NOOUT 3u
#define N48D2_PO_I_ADDR_HI 4u
#define N48D2_PO_I_ADDR_LO 5u
#define N48D2_PO_I_MPCC 11u
#define N48D2_PO_I_CLKOFF 14u
static inline uint32_t n48d2_build_planeoff(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_UPD(DPG2_CONTROL, N48D2_DPG_CONTROL_MASK, N48D2_DPG_CONTROL_ON, "opp2_set_disp_pattern_generator (rollback: the pattern covers the screen first)");
    N48D2_WAIT(DPG2_STATUS, N48D2_DPG_PENDING_MASK, 0u, 1000u, 100u, "dpg pending clear");
    N48D2_UPD(HUBP2_DCHUBP_CNTL, N48D2_HUBP_BLANK_EN_MASK, N48D2_HUBP_BLANK_EN_MASK, "hubp1_set_blank");
    N48D2_WAIT(HUBP2_DCHUBP_CNTL, N48D2_HUBP_NO_OUTSTANDING_MASK, N48D2_HUBP_NO_OUTSTANDING_MASK, 100u, 1000u, "HUBP_NO_OUTSTANDING_REQ");
    N48D2_SET(HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0u, "rollback: the primary address HIGH to 0 (blanked, no outstanding request)");
    N48D2_SET(HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, 0u, "rollback: the primary address LOW to 0 (blanked, no outstanding request)");
    N48D2_SET(MPC_OUT2_MUX, N48D2_MPC_OUT2_MUX_OFF, "mpc3_set_out_rate_control / mux none");
    N48D2_SET(MPCC2_MPCC_TOP_SEL, N48D2_MPCC_NONE, "mpc1_remove_mpcc");
    N48D2_SET(MPCC2_MPCC_BOT_SEL, N48D2_MPCC_NONE, "mpc1_remove_mpcc");
    N48D2_SET(MPCC2_MPCC_OPP_ID, N48D2_MPCC_NONE, "mpc1_remove_mpcc");
    N48D2_SET(MPCC2_MPCC_UPDATE_LOCK_SEL, N48D2_MPCC_NONE, "mpc1_remove_mpcc");
    N48D2_WAIT(MPCC2_MPCC_STATUS, N48D2_MPCC_STATUS_MASK, N48D2_MPCC_STATUS_OFF, 100u, 1000u, "MPCC2_STATUS idle + disabled");
    N48D2_UPD(OTG2_OTG_CRC_CNTL, N48D2_CRC_CNTL_MASK, 0u, "optc1_configure_crc (off)");
    N48D2_UPD(HUBP2_HUBP_CLK_CNTL, N48D2_HUBP_CLK_WRITE_MASK, 0u, "hubp1_disable_clock");
    N48D2_WAIT(HUBP2_HUBP_CLK_CNTL, N48D2_HUBP_CLK_OFF_MASK, 0u, 100u, 100u, "hubp clock gated");
    N48D2_UPD(DPP_TOP2_DPP_CONTROL, N48D2_DPP_CLOCK_ENABLE_MASK, 0u, "dpp1_dppclk_control (off)");
    N48D2_UPD(DPPCLK_CTRL, N48D2_DPPCLK2_EN_MASK, 0u, "dccg401_update_dpp_dto (off)");
    N48D2_SET(DPPCLK2_DTO_PARAM, 0u, "dccg401_update_dpp_dto (off)");
    return n;
}

// =====================================================================================================================================================================================================
// ---- 0.0.655: INSTANCE 1 (the monitor A's) PLANE at 1080p60: ops 8..13 for the monitor A, an internal design note "monitor A (instance 1) plane + framebuffer spec at 1080p60", sections 1 and 3 (the framebuffer side, section 4, is a LATER build).
// The order is the monitor B's (0.0.643 / 0.0.651: clocks, BLANK first and read back, pitch, address, the HUBP clock, the programming, a 2-frame wait, the read-back, then LOCK -> address / MPCC1 / mux / UNBLANK -> UNLOCK, then the post gates);
// the registers are the pipe-1 twins and EVERY programmed value is an explicit SET with its exact value pinned by the guard (n48d1_pin[]): there is NO COPY0 for instance 1 - pipe 0's DLG values are 1440p values, not 1080p ones.
// The step lists have the SAME SHAPE (step counts and the indices the flow gates on) as the monitor B's, so the flow's index constants (N48D2_PA_I_*, N48D2_LK_I_*, N48D2_PC_I_*, N48D2_PO_I_*, ...) are shared.
// =====================================================================================================================================================================================================
// The census-valued EXPECT rows (mona-census-654/mona-after-timing.txt, plus m4ab-634/census-plane.txt for MPCC1 and the VM registers): what the spec called "equal already" (a pipe-0 twin comparison, as the monitor B's) is a literal value here.
struct n48d1_exp { uint32_t abs, mask, val; const char *name; };       /* name = the Linux register name (tests/native_disp2_test.cpp re-derives abs from it) */
#define N48D1_PRE_STATE_COUNT 21u
static const struct n48d1_exp n48d1_pre_state[N48D1_PRE_STATE_COUNT] = {
    { N48D2_OTG1_CONTROL, 0x00010001u, 0x00010001u, "OTG1_OTG_CONTROL" },
    { N48D2_DPG1_CONTROL, N48D2_DPG_CONTROL_MASK, N48D2_DPG_CONTROL_ON, "DPG1_DPG_CONTROL" },
    { N48D2_HUBP1_HUBP_CLK_CNTL, N48D2_HUBP_CLK_WRITE_MASK & 1u, 0u, "HUBP1_HUBP_CLK_CNTL" },
    { N48D2_HUBP1_DCHUBP_CNTL, 0x000000F4u, 0x00000010u, "HUBP1_DCHUBP_CNTL" },                                    /* census 0x000f001a: VTG_SEL 1, bit 2 clear */
    { N48D2_DPP_TOP1_DPP_CONTROL, N48D2_DPP_CLOCK_ENABLE_MASK, 0u, "DPP_TOP1_DPP_CONTROL" },
    { N48D2_MPCC1_MPCC_TOP_SEL, 0xFFFFFFFFu, N48D2_MPCC_NONE, "MPCC1_MPCC_TOP_SEL" },
    { N48D2_MPCC1_MPCC_BOT_SEL, 0xFFFFFFFFu, N48D2_MPCC_NONE, "MPCC1_MPCC_BOT_SEL" },
    { N48D2_MPCC1_MPCC_OPP_ID, 0xFFFFFFFFu, N48D2_MPCC_NONE, "MPCC1_MPCC_OPP_ID" },
    { N48D2_MPCC1_MPCC_UPDATE_LOCK_SEL, 0xFFFFFFFFu, N48D2_MPCC_NONE, "MPCC1_MPCC_UPDATE_LOCK_SEL" },
    { N48D2_MPC_OUT1_MUX, 0xFFFFFFFFu, N48D2_MPC_OUT1_MUX_OFF, "MPC_OUT1_MUX" },                                   /* census 0x0000400f */
    { N48D2_DPPCLK_CTRL, N48D2_DPPCLK1_EN_MASK, 0u, "DPPCLK_CTRL" },
    { N48D2_DPPCLK1_DTO_PARAM, 0xFFFFFFFFu, 0u, "DPPCLK1_DTO_PARAM" },
    { N48D2_DET0_CTRL, 0xFFFFFFFFu, 0x00000303u, "DCHUBBUB_DET0_CTRL" },
    { N48D2_DCHUBBUB_DET1_CTRL, N48D2_DET_SIZE_MASK, 3u, "DCHUBBUB_DET1_CTRL" },
    { N48D2_DCN_VM_FAULT_STATUS, 0xFFFFFFFFu, 0u, "DCN_VM_FAULT_STATUS" },
    { N48D2_HUBPREQ1_VM_APERTURE_LOW, 0xFFFFFFFFu, 0u, "HUBPREQ1_DCN_VM_SYSTEM_APERTURE_LOW_ADDR" },
    { N48D2_HUBPREQ1_VM_APERTURE_HIGH, 0xFFFFFFFFu, 0u, "HUBPREQ1_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR" },
    { N48D2_HUBPREQ1_VM_MX_L1_TLB, 0xFFFFFFFFu, 0u, "HUBPREQ1_DCN_VM_MX_L1_TLB_CNTL" },
    { N48D2_HUBPREQ1_VMID_SETTINGS_0, 0xFFFFFFFFu, 0u, "HUBPREQ1_VMID_SETTINGS_0" },
    { N48D2_OTG1_OTG_GLOBAL_CONTROL2, N48D2_OTG_LOCK_SEL_MASK, N48D2_OTG_LOCK_SEL_OTG1, "OTG1_OTG_GLOBAL_CONTROL2" },   /* census 0x02000000: the update-lock select is already 1 */
    { N48D2_OTG1_OTG_MASTER_UPDATE_LOCK, N48D2_OTG_MASTER_UPDATE_LOCK_MASK | N48D2_OTG_UPDATE_LOCK_STATUS_MASK, 0u, "OTG1_OTG_MASTER_UPDATE_LOCK" },
};
#define N48D1_EXP_COUNT 37u
static const struct n48d1_exp n48d1_exp[N48D1_EXP_COUNT] = {
    { N48D2_R(0x06c1u)          , 0xffffffffu, 0x00000008u, "HUBP1_DCSURF_SURFACE_CONFIG" },   /* 0x3b81 */
    { N48D2_R(0x06c2u)          , 0xffffffffu, 0x00000000u, "HUBP1_DCSURF_ADDR_CONFIG" },   /* 0x3b82 */
    { N48D2_R(0x06c3u)          , 0xffffffffu, 0x00000080u, "HUBP1_DCSURF_TILING_CONFIG" },   /* 0x3b83 */
    { N48D2_R(0x06c5u)          , 0xffffffffu, 0x00000000u, "HUBP1_DCSURF_PRI_VIEWPORT_START" },   /* 0x3b85 */
    { N48D2_R(0x06d8u)          , 0xffffffffu, 0x00000100u, "HUBP1_HUBPREQ_DEBUG_DB" },   /* 0x3b98 */
    { N48D2_R(0x06d9u)          , 0xffffffffu, 0x04000000u, "HUBP1_HUBPREQ_DEBUG" },   /* 0x3b99 */
    { N48D2_R(0x06d0u)          , 0x00000100u, 0x00000000u, "HUBP1_DCHUBP_CNTL" },   /* 0x3b90 */
    { N48D2_R(0x06fcu)          , 0xffffffffu, 0x00000055u, "HUBPREQ1_DCN_EXPANSION_MODE" },   /* 0x3bbc */
    { N48D2_R(0x06eeu)          , 0xffffffffu, 0x00000000u, "HUBPREQ1_DCSURF_SURFACE_CONTROL" },   /* 0x3bae */
    { N48D2_R(0x06efu)          , 0x00000003u, 0x00000000u, "HUBPREQ1_DCSURF_FLIP_CONTROL" },   /* 0x3baf */
    { N48D2_R(0x071au)          , 0xffffffffu, 0x00000000u, "HUBPREQ1_DST_AFTER_SCALER" },   /* 0x3bda */
    { N48D2_R(0x071eu)          , 0xffffffffu, 0x000001a4u, "HUBPREQ1_VBLANK_PARAMETERS_1" },   /* 0x3bde */
    { N48D2_R(0x0720u)          , 0xffffffffu, 0x000001a4u, "HUBPREQ1_VBLANK_PARAMETERS_3" },   /* 0x3be0 */
    { N48D2_R(0x0725u)          , 0xffffffffu, 0x00000200u, "HUBPREQ1_NOM_PARAMETERS_0" },   /* 0x3be5 */
    { N48D2_R(0x0726u)          , 0xffffffffu, 0x0000d249u, "HUBPREQ1_NOM_PARAMETERS_1" },   /* 0x3be6 */
    { N48D2_R(0x0729u)          , 0xffffffffu, 0x00000020u, "HUBPREQ1_NOM_PARAMETERS_4" },   /* 0x3be9 */
    { N48D2_R(0x072au)          , 0xffffffffu, 0x00000d24u, "HUBPREQ1_NOM_PARAMETERS_5" },   /* 0x3bea */
    { N48D2_R(0x0731u)          , 0xffffffffu, 0x00003fffu, "HUBPREQ1_DST_Y_DELTA_DRQ_LIMIT" },   /* 0x3bf1 */
    { N48D2_R(0x0749u)          , 0xffffffffu, 0x00e40000u, "HUBPRET1_HUBPRET_CONTROL" },   /* 0x3c09 */
    { N48D2_R(0x0e3au)          , 0xffffffffu, 0x00000008u, "CNVC_CFG1_CNVC_SURFACE_PIXEL_FORMAT" },   /* 0x42fa */
    { N48D2_R(0x0e3bu)          , 0xffffffffu, 0x24000000u, "CNVC_CFG1_FORMAT_CONTROL" },   /* 0x42fb */
    { N48D2_R(0x0e4au)          , 0xffffffffu, 0x00000000u, "CNVC_CFG1_PRE_CSC_MODE" },   /* 0x430a */
    { N48D2_R(0x0ed2u)          , 0x00000001u, 0x00000001u, "CM1_CM_CONTROL" },   /* 0x4392 */
    { (N48D2_SEG3 + 0x02f7u)    , 0xffffffffu, 0x00fff000u, "MPC_OUT1_DENORM_CONTROL" },   /* 0x92f7 */
    { (N48D2_SEG3 + 0x0318u)    , 0xffffffffu, 0x00000000u, "MPC_OUT1_CSC_MODE" },   /* 0x9318 */
    { (N48D2_SEG3 + 0x00dcu)    , 0xffffffffu, 0x00000000u, "MPCC_OGAM1_MPCC_OGAM_CONTROL" },   /* 0x90dc */
    { (N48D2_SEG3 + 0x0503u)    , 0xffffffffu, 0x00000000u, "MPCC_MCM1_MPCC_MCM_SHAPER_CONTROL" },   /* 0x9503 */
    { (N48D2_SEG3 + 0x053au)    , 0xffffffffu, 0x00000000u, "MPCC_MCM1_MPCC_MCM_3DLUT_MODE" },   /* 0x953a */
    { (N48D2_SEG3 + 0x0543u)    , 0xffffffffu, 0x00000000u, "MPCC_MCM1_MPCC_MCM_1DLUT_CONTROL" },   /* 0x9543 */
    { N48D2_R(0x1baau)          , 0xffffffffu, 0x00000897u, "OTG1_OTG_H_TOTAL" },   /* 0x506a */
    { N48D2_R(0x1babu)          , 0xffffffffu, 0x00c00840u, "OTG1_OTG_H_BLANK_START_END" },   /* 0x506b */
    { N48D2_R(0x1bafu)          , 0xffffffffu, 0x00000464u, "OTG1_OTG_V_TOTAL" },   /* 0x506f */
    { N48D2_R(0x1bb8u)          , 0xffffffffu, 0x00290461u, "OTG1_OTG_V_BLANK_START_END" },   /* 0x5078 */
    { N48D2_R(0x1c05u)          , 0xffffffffu, 0x0000000du, "OTG1_OTG_VSTARTUP_PARAM" },   /* 0x50c5 */
    { N48D2_R(0x1c06u)          , 0xffffffffu, 0x014002a8u, "OTG1_OTG_VUPDATE_PARAM" },   /* 0x50c6 */
    { N48D2_R(0x1c07u)          , 0xffffffffu, 0x0000012cu, "OTG1_OTG_VREADY_PARAM" },   /* 0x50c7 */
    { (N48D2_SEG3 + 0x0018u), 0xffffffffu, 0xffff0461u, "MPCC1_MPCC_CONTROL" },   /* 0x9018 (m4ab-634/census-plane.txt) */
};
#define N48D1_PRE_TOTAL (N48D1_PRE_STATE_COUNT + N48D1_EXP_COUNT)
#define N48D1_PRE_PAGES 2u
static inline uint32_t n48d1_build_pre(uint32_t page, struct n48d2_step *s)
{
    uint32_t n = 0u, i;
    for (i = page * N48D2_MAX_STEPS; i < N48D1_PRE_TOTAL && n < N48D2_MAX_STEPS; i++) {
        const struct n48d1_exp *x = i < N48D1_PRE_STATE_COUNT ? &n48d1_pre_state[i] : &n48d1_exp[i - N48D1_PRE_STATE_COUNT];
        n48d2_put(s, &n, N48D2_K_EXPECT, x->abs, x->mask, x->val, 0u, 0u, 0u, 0u, x->name, "precheck (read only)");
    }
    return n;
}

// The 24 programmed registers (spec section 1: the 1080p values) - the builder's table and, separately typed, the guard's PIN table; the host test pins both to the spec's literals. kind SET unless noted.
struct n48d1_w { uint8_t kind; uint32_t abs, mask, val; const char *name; };
#define N48D1_PROG_COUNT 24u
static const struct n48d1_w n48d1_prog[N48D1_PROG_COUNT] = {
    { N48D2_K_SET, N48D2_DSCL1_SCL_MODE, 0xFFFFFFFFu, 0x00000001u, "DSCL1_SCL_MODE" },
    { N48D2_K_SET, N48D2_DSCL1_DSCL_2TAP_CONTROL, 0xFFFFFFFFu, 0x01110111u, "DSCL1_DSCL_2TAP_CONTROL" },
    { N48D2_K_SET, N48D2_DSCL1_SCL_HORZ_FILTER_SCALE_RATIO, 0xFFFFFFFFu, 0x01000000u, "DSCL1_SCL_HORZ_FILTER_SCALE_RATIO" },
    { N48D2_K_SET, N48D2_DSCL1_SCL_HORZ_FILTER_INIT, 0xFFFFFFFFu, 0x00000000u, "DSCL1_SCL_HORZ_FILTER_INIT" },
    { N48D2_K_SET, N48D2_DSCL1_SCL_VERT_FILTER_SCALE_RATIO, 0xFFFFFFFFu, 0x01000000u, "DSCL1_SCL_VERT_FILTER_SCALE_RATIO" },
    { N48D2_K_SET, N48D2_DSCL1_SCL_VERT_FILTER_INIT, 0xFFFFFFFFu, 0x00000000u, "DSCL1_SCL_VERT_FILTER_INIT" },
    { N48D2_K_SET, N48D2_DSCL1_OTG_H_BLANK, 0xFFFFFFFFu, 0x00c00840u, "DSCL1_OTG_H_BLANK" },
    { N48D2_K_SET, N48D2_DSCL1_OTG_V_BLANK, 0xFFFFFFFFu, 0x00290461u, "DSCL1_OTG_V_BLANK" },
    { N48D2_K_SET, N48D2_DSCL1_RECOUT_SIZE, 0xFFFFFFFFu, 0x04380780u, "DSCL1_RECOUT_SIZE" },
    { N48D2_K_SET, N48D2_DSCL1_MPC_SIZE, 0xFFFFFFFFu, 0x04380780u, "DSCL1_MPC_SIZE" },
    { N48D2_K_UPD, N48D2_DSCL1_LB_MEMORY_CTRL, 0x00003F03u, 0x00003f00u, "DSCL1_LB_MEMORY_CTRL" },
    { N48D2_K_SET, N48D2_HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION, 0xFFFFFFFFu, 0x04380780u, "HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION" },
    { N48D2_K_SET, N48D2_HUBP1_DCSURF_SEC_VIEWPORT_DIMENSION, 0xFFFFFFFFu, 0x04380780u, "HUBP1_DCSURF_SEC_VIEWPORT_DIMENSION" },
    { N48D2_K_SET, N48D2_HUBP1_DCHUBP_REQ_SIZE_CONFIG, 0xFFFFFFFFu, 0x05500300u, "HUBP1_DCHUBP_REQ_SIZE_CONFIG" },
    { N48D2_K_SET, N48D2_HUBPREQ1_DCN_TTU_QOS_WM, 0xFFFFFFFFu, 0x08a40000u, "HUBPREQ1_DCN_TTU_QOS_WM" },
    { N48D2_K_SET, N48D2_HUBPREQ1_DCN_GLOBAL_TTU_CNTL, 0xFFFFFFFFu, 0xe0006a1au, "HUBPREQ1_DCN_GLOBAL_TTU_CNTL" },
    { N48D2_K_SET, N48D2_HUBPREQ1_DCN_SURF0_TTU_CNTL0, 0xFFFFFFFFu, 0x08000000u, "HUBPREQ1_DCN_SURF0_TTU_CNTL0" },
    { N48D2_K_SET, N48D2_HUBPREQ1_BLANK_OFFSET_0, 0xFFFFFFFFu, 0x00290040u, "HUBPREQ1_BLANK_OFFSET_0" },
    { N48D2_K_SET, N48D2_HUBPREQ1_BLANK_OFFSET_1, 0xFFFFFFFFu, 0x00000d12u, "HUBPREQ1_BLANK_OFFSET_1" },
    { N48D2_K_SET, N48D2_HUBPREQ1_DST_DIMENSIONS, 0xFFFFFFFFu, 0x00027f99u, "HUBPREQ1_DST_DIMENSIONS" },
    { N48D2_K_SET, N48D2_HUBPREQ1_PREFETCH_SETTINGS, 0xFFFFFFFFu, 0x28080000u, "HUBPREQ1_PREFETCH_SETTINGS" },
    { N48D2_K_SET, N48D2_HUBPREQ1_PREFETCH_SETTINGS_C, 0xFFFFFFFFu, 0x00080000u, "HUBPREQ1_PREFETCH_SETTINGS_C" },
    { N48D2_K_SET, N48D2_HUBPREQ1_VBLANK_PARAMETERS_0, 0xFFFFFFFFu, 0x00000301u, "HUBPREQ1_VBLANK_PARAMETERS_0" },
    { N48D2_K_SET, N48D2_HUBPREQ1_REF_FREQ_TO_PIX_FREQ, 0xFFFFFFFFu, 0x00021fc4u, "HUBPREQ1_REF_FREQ_TO_PIX_FREQ" },
};
// The guard's PIN table: every register of the programming list admits EXACTLY its 1080p value (an UPDATE is judged on the bits of its mask) - typed independently of n48d1_prog[] above.
static const struct n48d1_w n48d1_pin[N48D1_PROG_COUNT] = {
    { N48D2_K_SET, N48D2_DSCL1_SCL_MODE, 0xFFFFFFFFu, 0x00000001u, "DSCL1_SCL_MODE" },
    { N48D2_K_SET, N48D2_DSCL1_DSCL_2TAP_CONTROL, 0xFFFFFFFFu, 0x01110111u, "DSCL1_DSCL_2TAP_CONTROL" },
    { N48D2_K_SET, N48D2_DSCL1_SCL_HORZ_FILTER_SCALE_RATIO, 0xFFFFFFFFu, 0x01000000u, "DSCL1_SCL_HORZ_FILTER_SCALE_RATIO" },
    { N48D2_K_SET, N48D2_DSCL1_SCL_HORZ_FILTER_INIT, 0xFFFFFFFFu, 0x00000000u, "DSCL1_SCL_HORZ_FILTER_INIT" },
    { N48D2_K_SET, N48D2_DSCL1_SCL_VERT_FILTER_SCALE_RATIO, 0xFFFFFFFFu, 0x01000000u, "DSCL1_SCL_VERT_FILTER_SCALE_RATIO" },
    { N48D2_K_SET, N48D2_DSCL1_SCL_VERT_FILTER_INIT, 0xFFFFFFFFu, 0x00000000u, "DSCL1_SCL_VERT_FILTER_INIT" },
    { N48D2_K_SET, N48D2_DSCL1_OTG_H_BLANK, 0xFFFFFFFFu, 0x00c00840u, "DSCL1_OTG_H_BLANK" },
    { N48D2_K_SET, N48D2_DSCL1_OTG_V_BLANK, 0xFFFFFFFFu, 0x00290461u, "DSCL1_OTG_V_BLANK" },
    { N48D2_K_SET, N48D2_DSCL1_RECOUT_SIZE, 0xFFFFFFFFu, 0x04380780u, "DSCL1_RECOUT_SIZE" },
    { N48D2_K_SET, N48D2_DSCL1_MPC_SIZE, 0xFFFFFFFFu, 0x04380780u, "DSCL1_MPC_SIZE" },
    { N48D2_K_UPD, N48D2_DSCL1_LB_MEMORY_CTRL, 0x00003F03u, 0x00003f00u, "DSCL1_LB_MEMORY_CTRL" },
    { N48D2_K_SET, N48D2_HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION, 0xFFFFFFFFu, 0x04380780u, "HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION" },
    { N48D2_K_SET, N48D2_HUBP1_DCSURF_SEC_VIEWPORT_DIMENSION, 0xFFFFFFFFu, 0x04380780u, "HUBP1_DCSURF_SEC_VIEWPORT_DIMENSION" },
    { N48D2_K_SET, N48D2_HUBP1_DCHUBP_REQ_SIZE_CONFIG, 0xFFFFFFFFu, 0x05500300u, "HUBP1_DCHUBP_REQ_SIZE_CONFIG" },
    { N48D2_K_SET, N48D2_HUBPREQ1_DCN_TTU_QOS_WM, 0xFFFFFFFFu, 0x08a40000u, "HUBPREQ1_DCN_TTU_QOS_WM" },
    { N48D2_K_SET, N48D2_HUBPREQ1_DCN_GLOBAL_TTU_CNTL, 0xFFFFFFFFu, 0xe0006a1au, "HUBPREQ1_DCN_GLOBAL_TTU_CNTL" },
    { N48D2_K_SET, N48D2_HUBPREQ1_DCN_SURF0_TTU_CNTL0, 0xFFFFFFFFu, 0x08000000u, "HUBPREQ1_DCN_SURF0_TTU_CNTL0" },
    { N48D2_K_SET, N48D2_HUBPREQ1_BLANK_OFFSET_0, 0xFFFFFFFFu, 0x00290040u, "HUBPREQ1_BLANK_OFFSET_0" },
    { N48D2_K_SET, N48D2_HUBPREQ1_BLANK_OFFSET_1, 0xFFFFFFFFu, 0x00000d12u, "HUBPREQ1_BLANK_OFFSET_1" },
    { N48D2_K_SET, N48D2_HUBPREQ1_DST_DIMENSIONS, 0xFFFFFFFFu, 0x00027f99u, "HUBPREQ1_DST_DIMENSIONS" },
    { N48D2_K_SET, N48D2_HUBPREQ1_PREFETCH_SETTINGS, 0xFFFFFFFFu, 0x28080000u, "HUBPREQ1_PREFETCH_SETTINGS" },
    { N48D2_K_SET, N48D2_HUBPREQ1_PREFETCH_SETTINGS_C, 0xFFFFFFFFu, 0x00080000u, "HUBPREQ1_PREFETCH_SETTINGS_C" },
    { N48D2_K_SET, N48D2_HUBPREQ1_VBLANK_PARAMETERS_0, 0xFFFFFFFFu, 0x00000301u, "HUBPREQ1_VBLANK_PARAMETERS_0" },
    { N48D2_K_SET, N48D2_HUBPREQ1_REF_FREQ_TO_PIX_FREQ, 0xFFFFFFFFu, 0x00021fc4u, "HUBPREQ1_REF_FREQ_TO_PIX_FREQ" },
};
static inline int n48d1_is_prog(uint32_t abs) { unsigned i; for (i = 0; i < N48D1_PROG_COUNT; i++) if (n48d1_pin[i].abs == abs) return 1; return 0; }

// op 8 part A (8 steps, the monitor B's shape): the DPP clocks (DPPCLK1 DTO, DPPCLK_CTRL bit 3, DPP_TOP1), BLANK_EN = 1 and the WAIT that it reads back (the ONLY gate before the HUBP clock), pitch 0x77f, address HIGH then LOW (A).
static inline uint32_t n48d1_build_plane_a(struct n48d2_step *s, uint64_t mc)
{
    uint32_t n = 0u;
    N48D2_SET(DPPCLK1_DTO_PARAM, N48D2_DPPCLK_DTO_VALUE, "dccg401_update_dpp_dto");
    N48D2_UPD(DPPCLK_CTRL, N48D2_DPPCLK1_EN_MASK, N48D2_DPPCLK1_EN_MASK, "dccg401_update_dpp_dto");
    N48D2_UPD(DPP_TOP1_DPP_CONTROL, N48D2_DPP_CLOCK_ENABLE_MASK, N48D2_DPP_CLOCK_ENABLE_MASK, "dpp1_dppclk_control");
    N48D2_UPD(HUBP1_DCHUBP_CNTL, N48D2_HUBP_BLANK_EN_MASK, N48D2_HUBP_BLANK_EN_MASK, "hubp1_set_blank (BLANK_EN = 1 BEFORE the HUBP clock)");
    N48D2_WAIT(HUBP1_DCHUBP_CNTL, N48D2_HUBP_BLANK_EN_MASK, N48D2_HUBP_BLANK_EN_MASK, 100u, 10u, "blank read back");
    N48D2_SET(HUBPREQ1_DCSURF_SURFACE_PITCH, N48D1_PITCH_PX, "hubp401_program_pitch");
    N48D2_SET(HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, (uint32_t)(mc >> 32), "dcn41_hubp_program_flip (HIGH first)");
    N48D2_SET(HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)mc, "dcn41_hubp_program_flip (LOW)");
    return n;
}
// part B (6 steps): the HUBP clock (a bare update), the four RECORDED reads ~1 ms later and a fifth of DET1_CTRL (no gate).
static inline uint32_t n48d1_build_plane_b(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_UPD(HUBP1_HUBP_CLK_CNTL, N48D2_HUBP_CLK_WRITE_MASK, N48D2_HUBP_CLK_WRITE_MASK, "hubp1_enable_clock");
    N48D2_REC(HUBP1_HUBP_CLK_CNTL, 0u, 1000u, "recorded read ~1 ms after the HUBP clock enable (no gate)");
    N48D2_REC(DCCG_GATE_DISABLE_CNTL6, 1u, 0u, "recorded read after the HUBP clock enable (no gate)");
    N48D2_REC(DCCG_GATE_DISABLE_CNTL, 2u, 0u, "recorded read after the HUBP clock enable (no gate)");
    N48D2_REC(DOMAIN1_PG_STATUS, 3u, 0u, "recorded read after the HUBP clock enable (no gate)");
    N48D2_REC(DCHUBBUB_DET1_CTRL, N48D2_RS_DET, 0u, "recorded read of DET1_CTRL before the unblank (no gate: the DET is allocated at the first VUPDATE after it)");
    return n;
}
// part E (1 step): AFTER the unblank and the latch gate: DET1 current = 3, 100 polls x 1 ms.
static inline uint32_t n48d1_build_plane_e(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_WAIT(DCHUBBUB_DET1_CTRL, N48D2_DET_CUR_MASK, 3u << 8, N48D2_PE_DET_POLLS, N48D2_PE_DET_STEP_US, "DET1 current = 3 (dcn401_wait_for_det_update, after the unblank)");
    return n;
}
// the second recorded sample (4 REC steps)
static inline uint32_t n48d1_build_plane_rec2(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_REC(HUBP1_HUBP_CLK_CNTL, 4u, 0u, "recorded read after the 2-frame wait (no gate)");
    N48D2_REC(DCCG_GATE_DISABLE_CNTL6, 5u, 0u, "recorded read after the 2-frame wait (no gate)");
    N48D2_REC(DCCG_GATE_DISABLE_CNTL, 6u, 0u, "recorded read after the 2-frame wait (no gate)");
    N48D2_REC(DOMAIN1_PG_STATUS, 7u, 0u, "recorded read after the 2-frame wait (no gate)");
    return n;
}
// the LOCK (3 steps): optc3_lock for OTG1: the select UPDATE (mask 0x0E000000 -> 0x02000000: it already reads 1, mona-census-654), the lock, the wait for UPDATE_LOCK_STATUS
static inline uint32_t n48d1_build_plane_lock(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_UPD(OTG1_OTG_GLOBAL_CONTROL2, N48D2_OTG_LOCK_SEL_MASK, N48D2_OTG_LOCK_SEL_OTG1, "optc3_lock (OTG_MASTER_UPDATE_LOCK_SEL = 1)");
    N48D2_UPD(OTG1_OTG_MASTER_UPDATE_LOCK, N48D2_OTG_MASTER_UPDATE_LOCK_MASK, N48D2_OTG_MASTER_UPDATE_LOCK_MASK, "optc3_lock (OTG_MASTER_UPDATE_LOCK = 1)");
    N48D2_WAIT(OTG1_OTG_MASTER_UPDATE_LOCK, N48D2_OTG_UPDATE_LOCK_STATUS_MASK, N48D2_OTG_UPDATE_LOCK_STATUS_MASK, N48D2_LK_POLLS, N48D2_LK_STEP_US, "optc3_lock: UPDATE_LOCK_STATUS = 1");
    return n;
}
static inline uint32_t n48d1_build_plane_unlock(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_UPD(OTG1_OTG_MASTER_UPDATE_LOCK, N48D2_OTG_MASTER_UPDATE_LOCK_MASK, 0u, "optc1_unlock (OTG_MASTER_UPDATE_LOCK = 0)");
    return n;
}
// part C INSIDE the lock (8 steps): address HIGH, LOW (A), MPCC1 BOT = 0xf, TOP = 1, OPP = 1, LOCK_SEL = 1, MPC_OUT1_MUX = 0x4101, UNBLANK (last)
static inline uint32_t n48d1_build_plane_c(struct n48d2_step *s, uint64_t mc)
{
    uint32_t n = 0u;
    N48D2_SET(HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, (uint32_t)(mc >> 32), "dcn41_hubp_program_flip (HIGH first, inside the OTG1 update lock)");
    N48D2_SET(HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)mc, "dcn41_hubp_program_flip (LOW, inside the OTG1 update lock)");
    N48D2_SET(MPCC1_MPCC_BOT_SEL, N48D2_MPCC_NONE, "mpc1_insert_plane");
    N48D2_SET(MPCC1_MPCC_TOP_SEL, N48D2_OTG1_INST, "mpc1_insert_plane");
    N48D2_SET(MPCC1_MPCC_OPP_ID, N48D2_OTG1_INST, "mpc1_insert_plane");
    N48D2_SET(MPCC1_MPCC_UPDATE_LOCK_SEL, N48D2_OTG1_INST, "mpc1_insert_plane");
    N48D2_SET(MPC_OUT1_MUX, N48D2_MPC_OUT1_MUX_ON, "mpc1_insert_plane (MPC_OUT_MUX = 1 + RATE_CONTROL_DISABLE)");
    N48D2_UPD(HUBP1_DCHUBP_CNTL, N48D2_HUBP_BLANK_EN_MASK, 0u, "hubp1_set_blank (unblank, inside the OTG1 update lock)");
    return n;
}
// part D (26 steps; AFTER the HUBP clock): the 24 explicit programming writes of the 1080p table (DSCL1 x 11, HUBP1 x 3, HUBPREQ1 x 10), then the primary address HIGH / LOW AGAIN (with the clock on)
#define N48D1_PD_STEPS 26u
static inline uint32_t n48d1_build_plane_d(struct n48d2_step *s, uint64_t mc)
{
    uint32_t n = 0u, i;
    for (i = 0u; i < N48D1_PROG_COUNT; i++)
        n48d2_put(s, &n, n48d1_prog[i].kind, n48d1_prog[i].abs, n48d1_prog[i].mask, n48d1_prog[i].val, 0u, 0u, 0u, 0u, n48d1_prog[i].name, "explicit SET (instance 1: no COPY0; the 1080p value, pinned by the guard)");
    N48D2_SET(HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, (uint32_t)(mc >> 32), "dcn41_hubp_program_flip (HIGH first, again with the clock on)");
    N48D2_SET(HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)mc, "dcn41_hubp_program_flip (LOW, again with the clock on)");
    return n;
}
static inline uint32_t n48d1_build_show1(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_UPD(HUBP1_DCHUBP_CNTL, N48D2_HUBP_UNDERFLOW_CLEAR_MASK, N48D2_HUBP_UNDERFLOW_CLEAR_MASK, "hubp1_clear_underflow");
    N48D2_UPD(ODM1_OPTC_INPUT_GLOBAL_CONTROL, N48D2_ODM_UNDERFLOW_CLEAR_MASK, N48D2_ODM_UNDERFLOW_CLEAR_MASK, "optc1_clear_optc_underflow");
    return n;
}
static inline uint32_t n48d1_build_show2(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_UPD(DPG1_CONTROL, N48D2_DPG_EN_MASK, 0u, "opp2_set_disp_pattern_generator (VIDEOMODE)");
    N48D2_WAIT(DPG1_STATUS, N48D2_DPG_PENDING_MASK, 0u, 1000u, 100u, "dpg pending clear");
    return n;
}
static inline uint32_t n48d1_build_flip(struct n48d2_step *s, uint64_t mc)
{
    uint32_t n = 0u;
    N48D2_SET(HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, (uint32_t)(mc >> 32), "hubp surface flip (HIGH)");
    N48D2_SET(HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, (uint32_t)mc, "hubp surface flip (LOW)");
    N48D2_WAIT(HUBPREQ1_DCSURF_FLIP_CONTROL, N48D2_FLIP_PENDING_MASK, 0u, 100u, 1000u, "flip pending clear");
    N48D2_WAIT(HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE, 0xFFFFFFFFu, (uint32_t)mc, 100u, 1000u, "hubp2_is_flip_pending: EARLIEST_INUSE = the requested address (LOW)");
    N48D2_WAIT(HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, 0x0000FFFFu, (uint32_t)(mc >> 32) & 0xFFFFu, 100u, 1000u, "hubp2_is_flip_pending: EARLIEST_INUSE = the requested address (HIGH)");
    return n;
}
// optc1_configure_crc over the 1920x1080 raster: windows 0x50a6 / 0x50a8 = 0x07800000, 0x50a7 / 0x50a9 = 0x04380000, CNTL 0x50a5 UPD 0x00700011 -> 0x11 (data at 0x50aa / 0x50ab)
static inline uint32_t n48d1_build_crc(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_SET(OTG1_OTG_CRC0_WINDOWA_X_CONTROL, N48D1_CRC_WIN_X, "optc1_configure_crc");
    N48D2_SET(OTG1_OTG_CRC0_WINDOWA_Y_CONTROL, N48D1_CRC_WIN_Y, "optc1_configure_crc");
    N48D2_SET(OTG1_OTG_CRC0_WINDOWB_X_CONTROL, N48D1_CRC_WIN_X, "optc1_configure_crc");
    N48D2_SET(OTG1_OTG_CRC0_WINDOWB_Y_CONTROL, N48D1_CRC_WIN_Y, "optc1_configure_crc");
    N48D2_UPD(OTG1_OTG_CRC_CNTL, N48D2_CRC_CNTL_MASK, N48D2_CRC_CNTL_ON, "optc1_configure_crc");
    return n;
}
// the rollback (18 steps, the monitor B's shape and indices)
static inline uint32_t n48d1_build_planeoff(struct n48d2_step *s)
{
    uint32_t n = 0u;
    N48D2_UPD(DPG1_CONTROL, N48D2_DPG_CONTROL_MASK, N48D2_DPG_CONTROL_ON, "opp2_set_disp_pattern_generator (rollback: the pattern covers the screen first)");
    N48D2_WAIT(DPG1_STATUS, N48D2_DPG_PENDING_MASK, 0u, 1000u, 100u, "dpg pending clear");
    N48D2_UPD(HUBP1_DCHUBP_CNTL, N48D2_HUBP_BLANK_EN_MASK, N48D2_HUBP_BLANK_EN_MASK, "hubp1_set_blank");
    N48D2_WAIT(HUBP1_DCHUBP_CNTL, N48D2_HUBP_NO_OUTSTANDING_MASK, N48D2_HUBP_NO_OUTSTANDING_MASK, 100u, 1000u, "HUBP_NO_OUTSTANDING_REQ");
    N48D2_SET(HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0u, "rollback: the primary address HIGH to 0 (blanked, no outstanding request)");
    N48D2_SET(HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, 0u, "rollback: the primary address LOW to 0 (blanked, no outstanding request)");
    N48D2_SET(MPC_OUT1_MUX, N48D2_MPC_OUT1_MUX_OFF, "mpc3_set_out_rate_control / mux none");
    N48D2_SET(MPCC1_MPCC_TOP_SEL, N48D2_MPCC_NONE, "mpc1_remove_mpcc");
    N48D2_SET(MPCC1_MPCC_BOT_SEL, N48D2_MPCC_NONE, "mpc1_remove_mpcc");
    N48D2_SET(MPCC1_MPCC_OPP_ID, N48D2_MPCC_NONE, "mpc1_remove_mpcc");
    N48D2_SET(MPCC1_MPCC_UPDATE_LOCK_SEL, N48D2_MPCC_NONE, "mpc1_remove_mpcc");
    N48D2_WAIT(MPCC1_MPCC_STATUS, N48D2_MPCC_STATUS_MASK, N48D2_MPCC_STATUS_OFF, 100u, 1000u, "MPCC1_STATUS idle + disabled");
    N48D2_UPD(OTG1_OTG_CRC_CNTL, N48D2_CRC_CNTL_MASK, 0u, "optc1_configure_crc (off)");
    N48D2_UPD(HUBP1_HUBP_CLK_CNTL, N48D2_HUBP_CLK_WRITE_MASK, 0u, "hubp1_disable_clock");
    N48D2_WAIT(HUBP1_HUBP_CLK_CNTL, N48D2_HUBP_CLK_OFF_MASK, 0u, 100u, 100u, "hubp clock gated");
    N48D2_UPD(DPP_TOP1_DPP_CONTROL, N48D2_DPP_CLOCK_ENABLE_MASK, 0u, "dpp1_dppclk_control (off)");
    N48D2_UPD(DPPCLK_CTRL, N48D2_DPPCLK1_EN_MASK, 0u, "dccg401_update_dpp_dto (off)");
    N48D2_SET(DPPCLK1_DTO_PARAM, 0u, "dccg401_update_dpp_dto (off)");
    return n;
}
// ---- the per-instance dispatch the flow uses (instance 2 = the monitor B's builders above, UNCHANGED; instance 1 = the monitor A's) ----
static inline uint32_t n48d2_build_pre_i(uint32_t inst, uint32_t page, struct n48d2_step *s) { return inst == N48D2_INST_MONA ? n48d1_build_pre(page, s) : n48d2_build_pre(page, s); }
static inline uint32_t n48d2_pre_pages(uint32_t inst) { return inst == N48D2_INST_MONA ? N48D1_PRE_PAGES : N48D2_PRE_PAGES; }
static inline uint32_t n48d2_build_plane_a_i(uint32_t inst, struct n48d2_step *s, uint64_t mc) { return inst == N48D2_INST_MONA ? n48d1_build_plane_a(s, mc) : n48d2_build_plane_a(s, mc); }
static inline uint32_t n48d2_build_plane_b_i(uint32_t inst, struct n48d2_step *s) { return inst == N48D2_INST_MONA ? n48d1_build_plane_b(s) : n48d2_build_plane_b(s); }
static inline uint32_t n48d2_build_plane_d_i(uint32_t inst, struct n48d2_step *s, uint64_t mc) { return inst == N48D2_INST_MONA ? n48d1_build_plane_d(s, mc) : n48d2_build_plane_d(s, mc); }
static inline uint32_t n48d2_build_plane_e_i(uint32_t inst, struct n48d2_step *s) { return inst == N48D2_INST_MONA ? n48d1_build_plane_e(s) : n48d2_build_plane_e(s); }
static inline uint32_t n48d2_build_plane_rec2_i(uint32_t inst, struct n48d2_step *s) { return inst == N48D2_INST_MONA ? n48d1_build_plane_rec2(s) : n48d2_build_plane_rec2(s); }
static inline uint32_t n48d2_build_plane_lock_i(uint32_t inst, struct n48d2_step *s) { return inst == N48D2_INST_MONA ? n48d1_build_plane_lock(s) : n48d2_build_plane_lock(s); }
static inline uint32_t n48d2_build_plane_unlock_i(uint32_t inst, struct n48d2_step *s) { return inst == N48D2_INST_MONA ? n48d1_build_plane_unlock(s) : n48d2_build_plane_unlock(s); }
static inline uint32_t n48d2_build_plane_c_i(uint32_t inst, struct n48d2_step *s, uint64_t mc) { return inst == N48D2_INST_MONA ? n48d1_build_plane_c(s, mc) : n48d2_build_plane_c(s, mc); }
static inline uint32_t n48d2_build_show1_i(uint32_t inst, struct n48d2_step *s) { return inst == N48D2_INST_MONA ? n48d1_build_show1(s) : n48d2_build_show1(s); }
static inline uint32_t n48d2_build_show2_i(uint32_t inst, struct n48d2_step *s) { return inst == N48D2_INST_MONA ? n48d1_build_show2(s) : n48d2_build_show2(s); }
static inline uint32_t n48d2_build_flip_i(uint32_t inst, struct n48d2_step *s, uint64_t mc) { return inst == N48D2_INST_MONA ? n48d1_build_flip(s, mc) : n48d2_build_flip(s, mc); }
static inline uint32_t n48d2_build_crc_i(uint32_t inst, struct n48d2_step *s) { return inst == N48D2_INST_MONA ? n48d1_build_crc(s) : n48d2_build_crc(s); }
static inline uint32_t n48d2_build_planeoff_i(uint32_t inst, struct n48d2_step *s) { return inst == N48D2_INST_MONA ? n48d1_build_planeoff(s) : n48d2_build_planeoff(s); }

// The step list of an op over the CEA VIC 16 timing (1920x1080@60, 148.5 MHz) of the given instance (0.0.633: 1 = the monitor A, 2 = the monitor B; any other instance = no list; 0.0.634: op 7 over the monitor B's 2560x1440@60 DTD, instance 2 only). The monitor A's EDID DTD IS VIC 16; the monitor B SINK-C
// lists VIC 16 (its CTA block's video data block, notes/logs/runs/m1-622/ddc.txt line 3 block 1: VICs 97 1 3 2 4 5 16 18 ...), so the same descriptor, the same 148.5 MHz values apply (n48d2_mona_dtd is the VIC 16 timing for both).
// 0 for status / a bad op / a list that does not fit.
static inline uint32_t n48d2_build_i(uint32_t inst, uint32_t op, struct n48d2_step *s)
{
    struct n48d2_timing t;
    uint32_t n = 0u;
    if (inst != N48D2_INST_MONA && inst != N48D2_INST_MONB) return 0u;
    if (op == N48D2_OP_TIMING) { if (n48d2_dtd_decode(n48d2_mona_dtd, &t) != 0u) return 0u; n = inst == N48D2_INST_MONB ? n48d2_build_timing2(s, &t) : n48d2_build_timing(s, &t); }
    else if (op == N48D2_OP_TIMING1440) { if (inst != N48D2_INST_MONB || n48d2_dtd_decode(n48d2_monb_dtd1440, &t) != 0u) return 0u; n = n48d2_build_timing2(s, &t); }   /* 0.0.634: instance 2 only; the SAME list builder as the 1080p timing, a different descriptor */
    else if (op == N48D2_OP_CONNECT) n = inst == N48D2_INST_MONB ? n48d2_build_connect2(s) : n48d2_build_connect(s);
    else if (op == N48D2_OP_OFF) n = inst == N48D2_INST_MONB ? n48d2_build_off2(s) : n48d2_build_off(s);
    return n <= N48D2_MAX_STEPS ? n : 0u;
}
// instance 1 (the monitor A): the 0.0.631 / 0.0.632 entry point
static inline uint32_t n48d2_build(uint32_t op, struct n48d2_step *s) { return n48d2_build_i(N48D2_INST_MONA, op, s); }
#undef N48D2_SET
#undef N48D2_UPD
#undef N48D2_WAIT
#undef N48D2_EXPECT
#undef N48D2_REC

// ---- the INSTANCE GUARD ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// 0.0.655: PER INSTANCE. skip = the instances the span does NOT apply to (bit 0: instance 1, the monitor A; bit 1: instance 2, the monitor B); 0 = both (every span before 0.0.655). The pipe-1 blocks (HUBP1 / DPP1 / MPCC1) are
// forbidden for the monitor B and the exact list (n48d1_pin / n48d2_allowed1p) for the monitor A; the pipe-2 blocks (HUBP2 / DPP2 / MPCC2 / MPC_OUT2, OTG2's lock / GLOBAL_CONTROL2 / CRC) are forbidden for the monitor A and the exact list for the monitor B; MPC_OUT1 and OTG1's
// lock / GLOBAL_CONTROL2 / CRC are forbidden for the monitor B; OGAM1 and MCM1 for both.
#define N48D2_SKIP_MONA 1u
#define N48D2_SKIP_MONB 2u
struct n48d2_span { uint32_t lo, hi; const char *why; uint32_t skip; };
#define N48D2_FORBIDDEN_COUNT 48u
static const struct n48d2_span n48d2_forbidden[N48D2_FORBIDDEN_COUNT] = {
    { 0x00000100u, 0x0000017eu, "DCCG (BASE_IDX 1): OTG0_PIXEL_RATE_CNTL, DP_DTO0, every PHYPLL source, the shared OTG_PIXEL_RATE_DIV - this verb writes no clock register", 0u },
    { 0x00003616u, 0x00003616u, "OTG0_INTERRUPT_DEST (OTG0's interrupt routing)", 0u },
    { 0x000039f0u, 0x000039f0u, "VTG0_CONTROL (OTG0's VTG)", 0u },
    { 0x00003aa5u, 0x00003b4du, "HUBP0 / HUBPREQ0 / HUBPRET0 (the DP's plane)", 0u },
    { 0x000092d5u, 0x000092d6u, "HUBP0 (BASE_IDX 3)", 0u },
    { 0x00004185u, 0x00004288u, "DPP0 (DPP_TOP0, CNVC_CFG0, DSCL0, CM0)", 0u },
    { 0x00009000u, 0x0000900eu, "MPCC0", 0u },
    { 0x0000907eu, 0x000090d5u, "MPCC_OGAM0", 0u },
    { 0x00009453u, 0x000094fbu, "MPCC_MCM0", 0u },
    { 0x00004b8eu, 0x00004b9eu, "DP_AUX1 (PHY B's AUX)", 0u },
    { 0x00004cfcu, 0x00004d55u, "OPP0 (FMT0, DPG0, OPPBUF0, OPP_PIPE0, OPP_PIPE_CRC0)", 0u },
    { 0x00004f8au, 0x00004f99u, "ODM0 (OPTC0's data source, clocks, memory)", 0u },
    { 0x00004feau, 0x00005069u, "OTG0", 0u },
    { 0x000053ceu, 0x000053ceu, "DIG1_STREAM_MAPPER_CONTROL", 0u },
    { 0x0000564cu, 0x00005767u, "the DIG1 stream path (VPG1, AFMT1, DME1, DIG1 FE / BE, DP1)", 0u },
    { 0x00005d2fu, 0x00005d30u, "UNIPHYB_LINK_CNTL / UNIPHYB_CHANNEL_XBAR_CNTL (PHY B)", 0u },
    { 0x00005ec0u, 0x00005ef9u, "DCIO_UNIPHY1 / RDPCSTX1 (PHY B)", 0u },
    /* 0.0.635 (M4c): the spans the plane work must never reach (the DCCG exceptions 0x15b / 0x168 are answered BEFORE this table, for instance 2 alone) */
    /* the spec names 0x392f-0x3aa4; VTG1_CONTROL 0x39f1 and VTG2_CONTROL 0x39f2 (the 0.0.631 / 0.0.633 timing lists write them) lie INSIDE it, so the span is split around exactly those two (VTG0 0x39f0 is its own span above, VTG3 0x39f3 stays forbidden) */
    { 0x0000392fu, 0x000039efu, "DCHUBBUB / DCN_VM (DET0, COMPBUF, the watermarks) below the VTGs", 0u },
    { 0x000039f3u, 0x00003aa4u, "VTG3, DCHUBBUB / DCN_VM (the VM fault block, the system aperture) above the VTGs", 0u },
    { 0x000092f2u, 0x000092f5u, "MPC_OUT0 MUX / DENORM (the DP's output mux)", 0u },
    { 0x0000930bu, 0x00009317u, "MPC_OUT0 CSC", 0u },
    { 0x000092b2u, 0x000092c0u, "MPC global", 0u },
    { 0x00003b81u, 0x00003c29u, "HUBP1 / HUBPREQ1 / HUBPRET1", N48D2_SKIP_MONA },
    { 0x00003d39u, 0x00003de1u, "HUBP3 / HUBPREQ3 / HUBPRET3", 0u },
    { 0x000092d7u, 0x000092d8u, "HUBP1 (BASE_IDX 3)", N48D2_SKIP_MONA },
    { 0x000092dbu, 0x000092dcu, "HUBP3 (BASE_IDX 3)", 0u },
    { 0x000042f0u, 0x000043f3u, "DPP1 (DPP_TOP1, CNVC_CFG1, DSCL1, CM1)", N48D2_SKIP_MONA },
    { 0x000045c6u, 0x000046c9u, "DPP3 (DPP_TOP3, CNVC_CFG3, DSCL3, CM3)", 0u },
    { 0x00009015u, 0x00009023u, "MPCC1", N48D2_SKIP_MONA },
    { 0x0000903fu, 0x0000904du, "MPCC3", 0u },
    /* 0.0.655: the pipe-2 blocks, forbidden for the MONA (the monitor B's exact list is n48d2_allowed2p) */
    { 0x00003c5du, 0x00003d05u, "HUBP2 / HUBPREQ2 / HUBPRET2 (the monitor B's plane)", N48D2_SKIP_MONB },
    { 0x000092d9u, 0x000092dau, "HUBP2 (BASE_IDX 3)", N48D2_SKIP_MONB },
    { 0x0000445bu, 0x0000455eu, "DPP2 (DPP_TOP2, CNVC_CFG2, DSCL2, CM2)", N48D2_SKIP_MONB },
    { 0x0000902au, 0x00009038u, "MPCC2", N48D2_SKIP_MONB },
    { 0x000092fau, 0x000092fdu, "MPC_OUT2 MUX / DENORM (the monitor B's output mux)", N48D2_SKIP_MONB },
    { 0x00009325u, 0x00009331u, "MPC_OUT2 CSC", N48D2_SKIP_MONB },
    { 0x00004faau, 0x00004faau, "ODM2_OPTC_INPUT_GLOBAL_CONTROL (the monitor B's underflow flags)", N48D2_SKIP_MONB },
    { 0x00005125u, 0x0000512bu, "OTG2 CRC (the monitor B's CRC witness)", N48D2_SKIP_MONB },
    { 0x00005149u, 0x00005149u, "OTG2_OTG_MASTER_UPDATE_LOCK (the monitor B's update lock)", N48D2_SKIP_MONB },
    { 0x00005150u, 0x00005150u, "OTG2_OTG_GLOBAL_CONTROL2 (the monitor B's lock select)", N48D2_SKIP_MONB },
    /* 0.0.655: the pipe-1 output side and OTG1's lock / CRC, forbidden for the MONB (the monitor A's exact list is n48d2_allowed1p) */
    { 0x000092f6u, 0x000092f9u, "MPC_OUT1 MUX / DENORM (the monitor A's output mux)", N48D2_SKIP_MONA },
    { 0x00009318u, 0x00009324u, "MPC_OUT1 CSC", N48D2_SKIP_MONA },
    { 0x00004f9au, 0x00004f9au, "ODM1_OPTC_INPUT_GLOBAL_CONTROL (the monitor A's underflow flags)", N48D2_SKIP_MONA },
    { 0x000050a5u, 0x000050bfu, "OTG1 CRC (the monitor A's CRC witness)", N48D2_SKIP_MONA },
    { 0x000050c9u, 0x000050c9u, "OTG1_OTG_MASTER_UPDATE_LOCK (the monitor A's update lock)", N48D2_SKIP_MONA },
    { 0x000050d0u, 0x000050d0u, "OTG1_OTG_GLOBAL_CONTROL2 (the monitor A's lock select)", N48D2_SKIP_MONA },
    /* 0.0.655: OGAM1 / MCM1 are forbidden for BOTH instances (never written; the census pins them at 0) */
    { 0x000090dcu, 0x00009133u, "MPCC_OGAM1", 0u },
    { 0x00009503u, 0x000095abu, "MPCC_MCM1", 0u },
};
#define N48D2_ALLOWED_COUNT 39u
static const uint32_t n48d2_allowed[N48D2_ALLOWED_COUNT] = {
    N48D2_VTG1_CONTROL,
    N48D2_FMT1_422_CONTROL, N48D2_DPG1_CONTROL, N48D2_DPG1_RAMP_CONTROL, N48D2_DPG1_DIMENSIONS, N48D2_DPG1_COLOUR_R_CR, N48D2_DPG1_COLOUR_G_Y, N48D2_DPG1_COLOUR_B_CB, N48D2_DPG1_OFFSET_SEGMENT,
    N48D2_OPP_PIPE1_CONTROL,
    N48D2_ODM1_DATA_SOURCE_SELECT, N48D2_ODM1_DATA_FORMAT_CONTROL, N48D2_ODM1_INPUT_CLOCK_CONTROL, N48D2_ODM1_MEMORY_CONFIG,
    N48D2_OTG1_H_TOTAL, N48D2_OTG1_H_BLANK_START_END, N48D2_OTG1_H_SYNC_A, N48D2_OTG1_H_SYNC_A_CNTL, N48D2_OTG1_H_TIMING_CNTL, N48D2_OTG1_V_TOTAL, N48D2_OTG1_V_TOTAL_MIN, N48D2_OTG1_V_TOTAL_MAX,
    N48D2_OTG1_V_BLANK_START_END, N48D2_OTG1_V_SYNC_A, N48D2_OTG1_V_SYNC_A_CNTL, N48D2_OTG1_CONTROL, N48D2_OTG1_INTERLACE_CONTROL, N48D2_OTG1_CLOCK_CONTROL, N48D2_OTG1_VSTARTUP_PARAM,
    N48D2_OTG1_VUPDATE_PARAM, N48D2_OTG1_VREADY_PARAM,
    N48D2_DIG2_STREAM_MAPPER, N48D2_DIG2_FE_CNTL, N48D2_DIG2_FE_CLK_CNTL, N48D2_DIG2_FE_EN_CNTL, N48D2_DIG2_CLOCK_PATTERN, N48D2_DIG2_FIFO_CTRL0, N48D2_DIG2_HDMI_CONTROL, N48D2_DIG2_BE_CNTL,
};

// ---- instance 2 (0.0.633): the monitor B's 39 writable registers, the same shape as n48d2_allowed[] one instance up ---------------------------------------------------------------------------------------------
#define N48D2_ALLOWED2_COUNT 39u
static const uint32_t n48d2_allowed2[N48D2_ALLOWED2_COUNT] = {
    N48D2_VTG2_CONTROL,
    N48D2_FMT2_422_CONTROL, N48D2_DPG2_CONTROL, N48D2_DPG2_RAMP_CONTROL, N48D2_DPG2_DIMENSIONS, N48D2_DPG2_COLOUR_R_CR, N48D2_DPG2_COLOUR_G_Y, N48D2_DPG2_COLOUR_B_CB, N48D2_DPG2_OFFSET_SEGMENT,
    N48D2_OPP_PIPE2_CONTROL,
    N48D2_ODM2_DATA_SOURCE_SELECT, N48D2_ODM2_DATA_FORMAT_CONTROL, N48D2_ODM2_INPUT_CLOCK_CONTROL, N48D2_ODM2_MEMORY_CONFIG,
    N48D2_OTG2_H_TOTAL, N48D2_OTG2_H_BLANK_START_END, N48D2_OTG2_H_SYNC_A, N48D2_OTG2_H_SYNC_A_CNTL, N48D2_OTG2_H_TIMING_CNTL, N48D2_OTG2_V_TOTAL, N48D2_OTG2_V_TOTAL_MIN, N48D2_OTG2_V_TOTAL_MAX,
    N48D2_OTG2_V_BLANK_START_END, N48D2_OTG2_V_SYNC_A, N48D2_OTG2_V_SYNC_A_CNTL, N48D2_OTG2_CONTROL, N48D2_OTG2_INTERLACE_CONTROL, N48D2_OTG2_CLOCK_CONTROL, N48D2_OTG2_VSTARTUP_PARAM,
    N48D2_OTG2_VUPDATE_PARAM, N48D2_OTG2_VREADY_PARAM,
    N48D2_DIG3_STREAM_MAPPER, N48D2_DIG3_FE_CNTL, N48D2_DIG3_FE_CLK_CNTL, N48D2_DIG3_FE_EN_CNTL, N48D2_DIG3_CLOCK_PATTERN, N48D2_DIG3_FIFO_CTRL0, N48D2_DIG3_HDMI_CONTROL, N48D2_DIG3_BE_CNTL,
};

// ---- 0.0.635 (M4c / M4d): instance 2's PLANE registers (37 more, 0.0.643: 39 with OTG2's lock select and lock; instance 1 refuses every one as the OTHER instance's). The DCCG exceptions (0x15b, 0x168) are NOT here: they are answered inside the forbidden DCCG span, for instance 2 alone, by
// n48d2_guard_addr_i. The CRC DATA registers (0x512a / 0x512b) are read-only and NOT listed.
#define N48D2_ALLOWED2P_COUNT 39u
static const uint32_t n48d2_allowed2p[N48D2_ALLOWED2P_COUNT] = {
    N48D2_DPP_TOP2_DPP_CONTROL,
    N48D2_DSCL2_SCL_MODE, N48D2_DSCL2_DSCL_2TAP_CONTROL, N48D2_DSCL2_SCL_HORZ_FILTER_SCALE_RATIO, N48D2_DSCL2_SCL_HORZ_FILTER_INIT, N48D2_DSCL2_SCL_VERT_FILTER_SCALE_RATIO, N48D2_DSCL2_SCL_VERT_FILTER_INIT,
    N48D2_DSCL2_OTG_H_BLANK, N48D2_DSCL2_OTG_V_BLANK, N48D2_DSCL2_RECOUT_SIZE, N48D2_DSCL2_MPC_SIZE, N48D2_DSCL2_LB_MEMORY_CTRL,
    N48D2_HUBP2_DCHUBP_CNTL, N48D2_HUBP2_HUBP_CLK_CNTL, N48D2_HUBP2_DCSURF_PRI_VIEWPORT_DIMENSION, N48D2_HUBP2_DCSURF_SEC_VIEWPORT_DIMENSION, N48D2_HUBP2_DCHUBP_REQ_SIZE_CONFIG,
    N48D2_HUBPREQ2_DCN_GLOBAL_TTU_CNTL, N48D2_HUBPREQ2_DCN_SURF0_TTU_CNTL0, N48D2_HUBPREQ2_BLANK_OFFSET_0, N48D2_HUBPREQ2_PREFETCH_SETTINGS, N48D2_HUBPREQ2_PREFETCH_SETTINGS_C, N48D2_HUBPREQ2_VBLANK_PARAMETERS_0,
    N48D2_HUBPREQ2_DCSURF_SURFACE_PITCH, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH,
    N48D2_MPCC2_MPCC_TOP_SEL, N48D2_MPCC2_MPCC_BOT_SEL, N48D2_MPCC2_MPCC_OPP_ID, N48D2_MPCC2_MPCC_UPDATE_LOCK_SEL, N48D2_MPC_OUT2_MUX,
    N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL,
    N48D2_OTG2_OTG_CRC_CNTL, N48D2_OTG2_OTG_CRC0_WINDOWA_X_CONTROL, N48D2_OTG2_OTG_CRC0_WINDOWA_Y_CONTROL, N48D2_OTG2_OTG_CRC0_WINDOWB_X_CONTROL, N48D2_OTG2_OTG_CRC0_WINDOWB_Y_CONTROL,
    N48D2_OTG2_OTG_GLOBAL_CONTROL2, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK,      /* 0.0.643: the lock select and the lock itself (op 8 only) */
};

// ---- 0.0.655: instance 1's PLANE registers (43; instance 2 refuses every one as the OTHER instance's / forbidden). The DCCG exceptions (0x15a, 0x168) are NOT here: they are answered inside the forbidden DCCG span, for instance 1 alone, by n48d2_guard_addr_i.
// The CRC DATA registers (0x50aa / 0x50ab) are read-only and NOT listed.
#define N48D2_ALLOWED1P_COUNT 43u
static const uint32_t n48d2_allowed1p[N48D2_ALLOWED1P_COUNT] = {
    N48D2_DPP_TOP1_DPP_CONTROL,
    N48D2_DSCL1_SCL_MODE, N48D2_DSCL1_DSCL_2TAP_CONTROL, N48D2_DSCL1_SCL_HORZ_FILTER_SCALE_RATIO, N48D2_DSCL1_SCL_HORZ_FILTER_INIT, N48D2_DSCL1_SCL_VERT_FILTER_SCALE_RATIO, N48D2_DSCL1_SCL_VERT_FILTER_INIT,
    N48D2_DSCL1_OTG_H_BLANK, N48D2_DSCL1_OTG_V_BLANK, N48D2_DSCL1_RECOUT_SIZE, N48D2_DSCL1_MPC_SIZE, N48D2_DSCL1_LB_MEMORY_CTRL,
    N48D2_HUBP1_DCHUBP_CNTL, N48D2_HUBP1_HUBP_CLK_CNTL, N48D2_HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION, N48D2_HUBP1_DCSURF_SEC_VIEWPORT_DIMENSION, N48D2_HUBP1_DCHUBP_REQ_SIZE_CONFIG,
    N48D2_HUBPREQ1_DCN_TTU_QOS_WM, N48D2_HUBPREQ1_DCN_GLOBAL_TTU_CNTL, N48D2_HUBPREQ1_DCN_SURF0_TTU_CNTL0, N48D2_HUBPREQ1_BLANK_OFFSET_0, N48D2_HUBPREQ1_BLANK_OFFSET_1, N48D2_HUBPREQ1_DST_DIMENSIONS,
    N48D2_HUBPREQ1_PREFETCH_SETTINGS, N48D2_HUBPREQ1_PREFETCH_SETTINGS_C, N48D2_HUBPREQ1_VBLANK_PARAMETERS_0, N48D2_HUBPREQ1_REF_FREQ_TO_PIX_FREQ,
    N48D2_HUBPREQ1_DCSURF_SURFACE_PITCH, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH,
    N48D2_MPCC1_MPCC_TOP_SEL, N48D2_MPCC1_MPCC_BOT_SEL, N48D2_MPCC1_MPCC_OPP_ID, N48D2_MPCC1_MPCC_UPDATE_LOCK_SEL, N48D2_MPC_OUT1_MUX,
    N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL,
    N48D2_OTG1_OTG_CRC_CNTL, N48D2_OTG1_OTG_CRC0_WINDOWA_X_CONTROL, N48D2_OTG1_OTG_CRC0_WINDOWA_Y_CONTROL, N48D2_OTG1_OTG_CRC0_WINDOWB_X_CONTROL, N48D2_OTG1_OTG_CRC0_WINDOWB_Y_CONTROL,
    N48D2_OTG1_OTG_GLOBAL_CONTROL2, N48D2_OTG1_OTG_MASTER_UPDATE_LOCK,
};

// ---- 0.0.655: the plane registers each flow decision names, per instance (instance 2's values are the constants the monitor B's flow always used; the host test pins both rows) ----
struct n48d2_pr {
    uint32_t inst;
    uint32_t hubp_cntl, hubp_clk, odm_ctl, det_ctrl, early_lo, early_hi, flip_ctl, mpcc_status, mpc_mux, fifo, crc_rg, crc_b, addr_lo, addr_hi, otg_frame;
};
static const struct n48d2_pr n48d2_prs[2] = {
    { N48D2_INST_MONA, N48D2_HUBP1_DCHUBP_CNTL, N48D2_HUBP1_HUBP_CLK_CNTL, N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL, N48D2_DCHUBBUB_DET1_CTRL, N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE, N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_HIGH,
      N48D2_HUBPREQ1_DCSURF_FLIP_CONTROL, N48D2_MPCC1_MPCC_STATUS, N48D2_MPC_OUT1_MUX, N48D2_DIG2_FIFO_CTRL0, N48D2_OTG1_OTG_CRC0_DATA_RG, N48D2_OTG1_OTG_CRC0_DATA_B, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS,
      N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, N48D2_OTG1_FRAME_COUNT },
    { N48D2_INST_MONB, N48D2_HUBP2_DCHUBP_CNTL, N48D2_HUBP2_HUBP_CLK_CNTL, N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL, N48D2_DCHUBBUB_DET2_CTRL, N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE, N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH,
      N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL, N48D2_MPCC2_MPCC_STATUS, N48D2_MPC_OUT2_MUX, N48D2_DIG3_FIFO_CTRL0, N48D2_OTG2_OTG_CRC0_DATA_RG, N48D2_OTG2_OTG_CRC0_DATA_B, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS,
      N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, N48D2_OTG2_FRAME_COUNT },
};
static inline const struct n48d2_pr *n48d2_pr_get(uint32_t inst) { return inst == N48D2_INST_MONA ? &n48d2_prs[0] : &n48d2_prs[1]; }

// ---- 0.0.655: the MONB WATCH (reads only; taken before and after EVERY monitor A op - timing, connect, off and the plane ops - and compared): nine ROWS, ten dwords + OTG2's frame counter. Any masked change ends the op in N48D2_DP_DISTURBED
// and the monitor A op is rolled back (the monitor B's pinned plane is the one thing an monitor A op must never move). Row 0 DPPCLK_CTRL bit 6 (DPPCLK2_EN), 1 DPPCLK2_DTO, 2 MPC_OUT2_MUX (0x4102), 3 MPCC2 TOP and OPP (2), 4 DET2 current (3),
// 5 HUBP2 underflow [30:28] and SEG_ALLOC_ERR [11], 6 EARLIEST_INUSE2 (A2; LOW and HIGH), 7 DENTIST_DISPCLK_CNTL (the shared display clock), 8 OTG2 COUNTING (a rule: if the counter moved across the settle at the start it must have moved by the end).
struct n48d2_w2reg { uint32_t abs, mask, row; const char *name; };
#define N48D2_W2_DW 10u
#define N48D2_W2_ROWS 9u
#define N48D2_W2_BUF 16u            /* dwords [0..9] the masked reads, [10] OTG2's frame counter at the snapshot, [11] after the 50 ms settle (before-buffer only; = [10] when the op has no settle), [10] of the AFTER buffer = the final counter */
#define N48D2_W2_ROW_COUNTING 8u
static const struct n48d2_w2reg n48d2_w2_regs[N48D2_W2_DW] = {
    { N48D2_DPPCLK_CTRL, N48D2_DPPCLK2_EN_MASK, 0u, "DPPCLK_CTRL bit 6" }, { N48D2_DPPCLK2_DTO_PARAM, 0xFFFFFFFFu, 1u, "DPPCLK2_DTO_PARAM" }, { N48D2_MPC_OUT2_MUX, 0xFFFFFFFFu, 2u, "MPC_OUT2_MUX" },
    { N48D2_MPCC2_MPCC_TOP_SEL, 0xFFFFFFFFu, 3u, "MPCC2_MPCC_TOP_SEL" }, { N48D2_MPCC2_MPCC_OPP_ID, 0xFFFFFFFFu, 3u, "MPCC2_MPCC_OPP_ID" }, { N48D2_DCHUBBUB_DET2_CTRL, N48D2_DET_CUR_MASK, 4u, "DET2 current" },
    { N48D2_HUBP2_DCHUBP_CNTL, N48D2_HUBP_UNDERFLOW_MASK | N48D2_HUBP_SEG_ALLOC_ERR_MASK, 5u, "HUBP2 underflow" }, { N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE, 0xFFFFFFFFu, 6u, "EARLIEST_INUSE2 LOW" },
    { N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, 0x0000FFFFu, 6u, "EARLIEST_INUSE2 HIGH" }, { N48D2_DENTIST_DISPCLK_CNTL, 0xFFFFFFFFu, 7u, "DENTIST_DISPCLK_CNTL" },
};
// The change mask (bit = row) of two snapshots (pure). Row 8: OTG2 was COUNTING at the start (b[11] != b[10]) and its counter did not move by the end (a[10] == b[11]) -> changed.
static inline uint32_t n48d2_w2_diff(const uint32_t *b, const uint32_t *a)
{
    uint32_t m = 0u, i;
    for (i = 0u; i < N48D2_W2_DW; i++) if (a[i] != b[i]) m |= 1u << n48d2_w2_regs[i].row;
    if (b[11] != b[10] && a[10] == b[11]) m |= 1u << N48D2_W2_ROW_COUNTING;
    return m;
}
// 0.0.658: the INDEX of each masked read inside a w2 buffer (b[0..9] = n48d2_w2_regs[i]; b[10] OTG2's frame counter at the snapshot, b[11] after the settle). The two kext log lines of an monitor A op's monitor B watch are built from the
// format macros below with these indexes ONLY (a hand-written argument list once shifted the second line by two values: DET2 landed in the HUBP2 column; the host test formats both lines from known values and checks the text).
#define N48D2_W2_I_DPPCLK 0u
#define N48D2_W2_I_DTO    1u
#define N48D2_W2_I_MUX    2u
#define N48D2_W2_I_TOP    3u
#define N48D2_W2_I_OPP    4u
#define N48D2_W2_I_DET    5u
#define N48D2_W2_I_HUBP   6u
#define N48D2_W2_I_EARLY_LO 7u
#define N48D2_W2_I_EARLY_HI 8u
#define N48D2_W2_I_DENTIST 9u
#define N48D2_W2_I_FRAMES 10u
#define N48D2_W2_I_SETTLED 11u
#define N48D2_W2_LINE1_FMT "disp2 %s (instance 1) MONB WATCH%s (changed mask %#x, rows 0 DPPCLK_CTRL bit 6, 1 DPPCLK2_DTO, 2 MPC_OUT2_MUX, 3 MPCC2 TOP/OPP, 4 DET2 current): DPPCLK_CTRL %#010x -> %#010x ; DTO2 %#010x -> %#010x ; MPC_OUT2_MUX %#010x -> %#010x ; MPCC2 TOP %#x -> %#x OPP %#x -> %#x ; DET2 %#010x -> %#010x"
#define N48D2_W2_LINE1_ARGS(opname, flag, mask, bf, af) (opname), (flag), (mask), \
    (bf)[N48D2_W2_I_DPPCLK], (af)[N48D2_W2_I_DPPCLK], (bf)[N48D2_W2_I_DTO], (af)[N48D2_W2_I_DTO], (bf)[N48D2_W2_I_MUX], (af)[N48D2_W2_I_MUX], \
    (bf)[N48D2_W2_I_TOP], (af)[N48D2_W2_I_TOP], (bf)[N48D2_W2_I_OPP], (af)[N48D2_W2_I_OPP], (bf)[N48D2_W2_I_DET], (af)[N48D2_W2_I_DET]
#define N48D2_W2_LINE2_FMT "disp2 %s (instance 1) MONB WATCH (rows 5 HUBP2 underflow, 6 EARLIEST_INUSE2, 7 DENTIST_DISPCLK_CNTL, 8 OTG2 counting): HUBP2 %#010x -> %#010x ; EARLIEST2 %#x:%#010x -> %#x:%#010x ; DENTIST %#010x -> %#010x ; OTG2 frames %u -> %u (settled %u)"
#define N48D2_W2_LINE2_ARGS(opname, bf, af) (opname), \
    (bf)[N48D2_W2_I_HUBP], (af)[N48D2_W2_I_HUBP], (bf)[N48D2_W2_I_EARLY_HI], (bf)[N48D2_W2_I_EARLY_LO], (af)[N48D2_W2_I_EARLY_HI], (af)[N48D2_W2_I_EARLY_LO], \
    (bf)[N48D2_W2_I_DENTIST], (af)[N48D2_W2_I_DENTIST], (bf)[N48D2_W2_I_FRAMES], (af)[N48D2_W2_I_FRAMES], (bf)[N48D2_W2_I_SETTLED]
// the first changed row + 1 (1..9; 0 = none): what the 4-bit guard field of the op's result carries when an monitor A op ended in N48D2_DP_DISTURBED because of the watch
static inline uint32_t n48d2_w2_first(uint32_t m) { uint32_t r; for (r = 0u; r < N48D2_W2_ROWS; r++) if (m & (1u << r)) return r + 1u; return 0u; }

// ---- the per-instance facts (every flow decision that names an instance's register goes through this table) -------------------------------------------------------------------------------------------------
struct n48d2_inst {
    uint32_t inst;
    uint32_t otg_control, otg_frame, otg_status, phypll, phypll_pll;            /* OTGn: control / frame counter / status; its PHYPLL source register and the PLL number the DMUB leaves there */
    uint32_t be_clk, be_en, be_cntl, fe_cntl, fe_clk, fe_en, fifo, mapper;       /* DIGm */
    uint32_t odm_dss, dpg_ctl, symclk;                                           /* ODMn DATA_SOURCE_SELECT, DPGn CONTROL, SYMCLK<m+1>_CLOCK_ENABLE (the exception register) */
    uint32_t fe_bit, otg_inst, link;                                             /* the BE's FE-source bit, the OTG / OPP number, the link (UNIPHY - A) */
};
static const struct n48d2_inst n48d2_insts[2] = {
    { N48D2_INST_MONA, N48D2_OTG1_CONTROL, N48D2_OTG1_FRAME_COUNT, N48D2_OTG1_STATUS, N48D2_OTG1_PHYPLL_PIXEL_RATE_CNTL, N48D2_PHYPLL_PLL2,
      N48D2_DIG2_BE_CLK_CNTL, N48D2_DIG2_BE_EN_CNTL, N48D2_DIG2_BE_CNTL, N48D2_DIG2_FE_CNTL, N48D2_DIG2_FE_CLK_CNTL, N48D2_DIG2_FE_EN_CNTL, N48D2_DIG2_FIFO_CTRL0, N48D2_DIG2_STREAM_MAPPER,
      N48D2_ODM1_DATA_SOURCE_SELECT, N48D2_DPG1_CONTROL, N48D2_SYMCLKC_CLOCK_ENABLE, N48D2_BE_FE2_BIT, N48D2_OTG1_INST, N48D2_LINK_UNIPHY_C },
    { N48D2_INST_MONB, N48D2_OTG2_CONTROL, N48D2_OTG2_FRAME_COUNT, N48D2_OTG2_STATUS, N48D2_OTG2_PHYPLL_PIXEL_RATE_CNTL, N48D2_PHYPLL_PLL3,
      N48D2_DIG3_BE_CLK_CNTL, N48D2_DIG3_BE_EN_CNTL, N48D2_DIG3_BE_CNTL, N48D2_DIG3_FE_CNTL, N48D2_DIG3_FE_CLK_CNTL, N48D2_DIG3_FE_EN_CNTL, N48D2_DIG3_FIFO_CTRL0, N48D2_DIG3_STREAM_MAPPER,
      N48D2_ODM2_DATA_SOURCE_SELECT, N48D2_DPG2_CONTROL, N48D2_SYMCLKD_CLOCK_ENABLE, N48D2_BE_FE3_BIT, N48D2_OTG2_INST, N48D2_LINK_UNIPHY_D },
};
static inline const struct n48d2_inst *n48d2_inst_get(uint32_t inst) { return inst == N48D2_INST_MONA ? &n48d2_insts[0] : inst == N48D2_INST_MONB ? &n48d2_insts[1] : (const struct n48d2_inst *)0; }

#define N48D2_G_OK 0u
#define N48D2_G_FORBIDDEN 1u        /* inside a named forbidden span */
#define N48D2_G_UNLISTED 2u         /* not one of the verb's registers */
#define N48D2_G_VALUE 3u            /* a listed register, but a value this verb never writes there */
#define N48D2_G_OTHER 4u            /* 0.0.633: a register of the OTHER instance (an instance never touches the other one's pipe) */
// Address, for one instance. ORDER: (1) the instance's own SYMCLK<n>_CLOCK_ENABLE - the ONE narrow exception to the DCCG span (its VALUE rule is in n48d2_guard_value_i, its bit mask in n48d2_guard_list_i); (2) the OTHER
// instance's SYMCLK (named, although it also lies inside the DCCG span); (3) the forbidden spans (a named refusal: the DP path, PHY B, the whole DCCG including SYMCLKB / SYMCLKA); (4) the other instance's writable
// registers; (5) the instance's exact list. *why = the span's name.
static inline uint32_t n48d2_guard_addr_i(uint32_t inst, uint32_t abs, const char **why)
{
    unsigned i;
    const struct n48d2_inst *me = n48d2_inst_get(inst);
    const struct n48d2_inst *ot = n48d2_inst_get(inst == N48D2_INST_MONA ? N48D2_INST_MONB : N48D2_INST_MONA);
    const uint32_t *mine = inst == N48D2_INST_MONB ? n48d2_allowed2 : n48d2_allowed, *other = inst == N48D2_INST_MONB ? n48d2_allowed : n48d2_allowed2;
    if (why) *why = 0;
    if (!me) return N48D2_G_UNLISTED;                                                    /* an unknown instance writes nothing */
    if (abs == me->symclk) return N48D2_G_OK;
    if (abs == ot->symclk) { if (why) *why = "the OTHER instance's symbol clock register"; return N48D2_G_OTHER; }     /* inside the DCCG span too: named first */
    if (inst == N48D2_INST_MONB && (abs == N48D2_DPPCLK2_DTO_PARAM || abs == N48D2_DPPCLK_CTRL)) return N48D2_G_OK;     /* 0.0.635: the TWO DCCG exceptions, instance 2 alone (value / kind / mask rules below; DPPCLK0's DTO 0x159 stays forbidden) */
    if (inst == N48D2_INST_MONA && (abs == N48D2_DPPCLK1_DTO_PARAM || abs == N48D2_DPPCLK_CTRL)) return N48D2_G_OK;     /* 0.0.655: the TWO DCCG exceptions of instance 1 alone: 0x15a (SET 0x00ff00ff / 0) and 0x168 (UPDATE of mask exactly 0x8); 0x15b stays refused for instance 1, 0x15a for instance 2 */
    for (i = 0; i < N48D2_FORBIDDEN_COUNT; i++)
        if (abs >= n48d2_forbidden[i].lo && abs <= n48d2_forbidden[i].hi && (n48d2_forbidden[i].skip & (inst == N48D2_INST_MONA ? N48D2_SKIP_MONA : N48D2_SKIP_MONB)) == 0u) { if (why) *why = n48d2_forbidden[i].why; return N48D2_G_FORBIDDEN; }
    for (i = 0; i < N48D2_ALLOWED_COUNT; i++) if (abs == other[i]) { if (why) *why = "a register of the OTHER instance's pipe"; return N48D2_G_OTHER; }
    if (inst != N48D2_INST_MONB) for (i = 0; i < N48D2_ALLOWED2P_COUNT; i++) if (abs == n48d2_allowed2p[i]) { if (why) *why = "a PLANE register of the OTHER instance's pipe (instance 2's)"; return N48D2_G_OTHER; }
    if (inst != N48D2_INST_MONA) for (i = 0; i < N48D2_ALLOWED1P_COUNT; i++) if (abs == n48d2_allowed1p[i]) { if (why) *why = "a PLANE register of the OTHER instance's pipe (instance 1's)"; return N48D2_G_OTHER; }
    for (i = 0; i < N48D2_ALLOWED_COUNT; i++) if (abs == mine[i]) return N48D2_G_OK;
    if (inst == N48D2_INST_MONB) for (i = 0; i < N48D2_ALLOWED2P_COUNT; i++) if (abs == n48d2_allowed2p[i]) return N48D2_G_OK;
    if (inst == N48D2_INST_MONA) for (i = 0; i < N48D2_ALLOWED1P_COUNT; i++) if (abs == n48d2_allowed1p[i]) return N48D2_G_OK;
    return N48D2_G_UNLISTED;
}
static inline uint32_t n48d2_guard_addr(uint32_t abs, const char **why) { return n48d2_guard_addr_i(N48D2_INST_MONA, abs, why); }
// Value, for one instance: the fields whose wrong value would route a pipe onto the DP's path or the other instance's. Judged on the value that would be WRITTEN.
//   DIGm_FE_CNTL: DIG_SOURCE_SELECT must be the instance's OTG (never OTG0, the DP's, nor the other instance's).   DIGm_BE_CNTL: DIG_FE_SOURCE_SELECT may hold ONLY the instance's FE bit (never FE1, the DP's front end).
//   DIGm_STREAM_MAPPER: link 0 (off) or the instance's link only (never 1, PHY B).   ODMn_DATA_SOURCE_SELECT: SEG0 = the instance's OPP or 0xf (never OPP0).
//   SYMCLK<n>_CLOCK_ENABLE (the exception): FE_SRC_SEL (bits 10:8) 0 or the instance's link, nothing else judged here (the list guard pins the mask to bits 10:8 and 4).
// 0.0.635: the VALUE rules of instance 2's plane registers (judged on the value that would be WRITTEN, so a read-modify-write is judged whole). HUBP2_DCHUBP_CNTL: VTG_SEL (bits 7:4) must stay 2 and the soft reset (bit 2) clear;
// MPCC2 TOP / OPP / LOCK_SEL 2 or 0xf, BOT 0xf; MPC_OUT2_MUX field 2 or 0xf; pitch exactly 0x9ff; the address halves one of A / B's (the runtime set); DPPCLK2_DTO 0x00ff00ff or 0; the CRC windows the 2560x1440 values; ODM2's soft reset (bit 0) never set.
static inline uint32_t n48d2_guard_value_plane(uint32_t abs, uint32_t v)
{
    if (abs == N48D2_HUBP2_DCHUBP_CNTL) return (((v >> 4) & 0xFu) == N48D2_OTG2_INST && (v & 0x4u) == 0u) ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_MPCC2_MPCC_TOP_SEL || abs == N48D2_MPCC2_MPCC_OPP_ID || abs == N48D2_MPCC2_MPCC_UPDATE_LOCK_SEL) return (v == N48D2_OTG2_INST || v == N48D2_MPCC_NONE) ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_MPCC2_MPCC_BOT_SEL) return v == N48D2_MPCC_NONE ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_MPC_OUT2_MUX) return ((v & 0xFu) == N48D2_OTG2_INST || (v & 0xFu) == N48D2_MPCC_NONE) ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_HUBPREQ2_DCSURF_SURFACE_PITCH) return v == N48D2_PITCH_PX ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS) return (n48d2_aset_lo_ok(v) || v == 0u) ? N48D2_G_OK : N48D2_G_VALUE;          /* 0.0.638: EXACTLY 0 is also allowed (the rollback list zeroes the address; the LIST guard demands the blank + NO_OUTSTANDING wait before it) */
    if (abs == N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) return (n48d2_aset_hi_ok(v) || v == 0u) ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_DPPCLK2_DTO_PARAM) return (v == N48D2_DPPCLK_DTO_VALUE || v == 0u) ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_OTG2_OTG_CRC0_WINDOWA_X_CONTROL || abs == N48D2_OTG2_OTG_CRC0_WINDOWB_X_CONTROL) return v == N48D2_CRC_WIN_X ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_OTG2_OTG_CRC0_WINDOWA_Y_CONTROL || abs == N48D2_OTG2_OTG_CRC0_WINDOWB_Y_CONTROL) return v == N48D2_CRC_WIN_Y ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL) return (v & 1u) == 0u ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_OTG2_OTG_CRC_CNTL) return (v & 0x00700000u) == 0u ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_OTG2_OTG_GLOBAL_CONTROL2) return (v & N48D2_OTG_LOCK_SEL_MASK) == N48D2_OTG_LOCK_SEL_OTG2 ? N48D2_G_OK : N48D2_G_VALUE;      /* 0.0.643: the lock select is OTG2's own instance and nothing else */
    if (abs == N48D2_OTG2_OTG_MASTER_UPDATE_LOCK) return (v & ~(N48D2_OTG_MASTER_UPDATE_LOCK_MASK | N48D2_OTG_UPDATE_LOCK_STATUS_MASK)) == 0u ? N48D2_G_OK : N48D2_G_VALUE;      /* 0.0.643: bit 0 (the lock); bit 8 is the read-only status */
    return N48D2_G_OK;
}
// 0.0.655: the VALUE rules of instance 1's plane registers (judged on the value that would be WRITTEN). HUBP1_DCHUBP_CNTL: VTG_SEL (bits 7:4) must stay 1 and the soft reset (bit 2) clear; MPCC1 TOP / OPP / LOCK_SEL 1 or 0xf, BOT 0xf; MPC_OUT1_MUX exactly 0x4101 or 0x400f;
// pitch exactly 0x77f; the address halves one of the MONA's A1 / B1 (its own runtime set; or exactly 0); DPPCLK1_DTO 0x00ff00ff or 0; the CRC windows the 1920x1080 values; ODM1's soft reset (bit 0) never set; OTG1's lock select (27:25) exactly 1; and every register of the
// programming list EXACTLY its 1080p value (n48d1_pin[]: a value from pipe 0 / the monitor B's 1440p set is refused).
static inline uint32_t n48d1_guard_value_plane(uint32_t abs, uint32_t v)
{
    unsigned i;
    for (i = 0; i < N48D1_PROG_COUNT; i++) if (abs == n48d1_pin[i].abs) return (v & n48d1_pin[i].mask) == n48d1_pin[i].val ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_HUBP1_DCHUBP_CNTL) return (((v >> 4) & 0xFu) == N48D2_OTG1_INST && (v & 0x4u) == 0u) ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_MPCC1_MPCC_TOP_SEL || abs == N48D2_MPCC1_MPCC_OPP_ID || abs == N48D2_MPCC1_MPCC_UPDATE_LOCK_SEL) return (v == N48D2_OTG1_INST || v == N48D2_MPCC_NONE) ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_MPCC1_MPCC_BOT_SEL) return v == N48D2_MPCC_NONE ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_MPC_OUT1_MUX) return (v == N48D2_MPC_OUT1_MUX_ON || v == N48D2_MPC_OUT1_MUX_OFF) ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_HUBPREQ1_DCSURF_SURFACE_PITCH) return v == N48D1_PITCH_PX ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS) return (n48d2_aset_lo_ok_i(N48D2_INST_MONA, v) || v == 0u) ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) return (n48d2_aset_hi_ok_i(N48D2_INST_MONA, v) || v == 0u) ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_DPPCLK1_DTO_PARAM) return (v == N48D2_DPPCLK_DTO_VALUE || v == 0u) ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_OTG1_OTG_CRC0_WINDOWA_X_CONTROL || abs == N48D2_OTG1_OTG_CRC0_WINDOWB_X_CONTROL) return v == N48D1_CRC_WIN_X ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_OTG1_OTG_CRC0_WINDOWA_Y_CONTROL || abs == N48D2_OTG1_OTG_CRC0_WINDOWB_Y_CONTROL) return v == N48D1_CRC_WIN_Y ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL) return (v & 1u) == 0u ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_OTG1_OTG_CRC_CNTL) return (v & 0x00700000u) == 0u ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == N48D2_OTG1_OTG_GLOBAL_CONTROL2) return (v & N48D2_OTG_LOCK_SEL_MASK) == N48D2_OTG_LOCK_SEL_OTG1 ? N48D2_G_OK : N48D2_G_VALUE;      /* the lock select is OTG1's own instance and nothing else */
    if (abs == N48D2_OTG1_OTG_MASTER_UPDATE_LOCK) return (v & ~(N48D2_OTG_MASTER_UPDATE_LOCK_MASK | N48D2_OTG_UPDATE_LOCK_STATUS_MASK)) == 0u ? N48D2_G_OK : N48D2_G_VALUE;
    return N48D2_G_OK;
}
static inline uint32_t n48d2_guard_value_i(uint32_t inst, uint32_t abs, uint32_t v)
{
    const struct n48d2_inst *d = n48d2_inst_get(inst);
    if (!d) return N48D2_G_VALUE;
    if (inst == N48D2_INST_MONB && n48d2_guard_value_plane(abs, v) != N48D2_G_OK) return N48D2_G_VALUE;
    if (inst == N48D2_INST_MONA && n48d1_guard_value_plane(abs, v) != N48D2_G_OK) return N48D2_G_VALUE;
    if (abs == d->fe_cntl) return (v & N48D2_DIG_SOURCE_SELECT_MASK) == d->otg_inst ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == d->be_cntl) return (v & N48D2_BE_FE_SOURCE_MASK & ~d->fe_bit) == 0u ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == d->mapper) return ((v & N48D2_MAPPER_LINK_MASK) == 0u || (v & N48D2_MAPPER_LINK_MASK) == d->link) ? N48D2_G_OK : N48D2_G_VALUE;
    if (abs == d->odm_dss) { const uint32_t s0 = (v & N48D2_ODM_SEG0_MASK) >> 16; return (s0 == d->otg_inst || s0 == 0xFu) ? N48D2_G_OK : N48D2_G_VALUE; }
    if (abs == d->symclk) { const uint32_t sel = (v & N48D2_SYMCLK_FE_SRC_MASK) >> 8; return (sel == 0u || sel == d->link) ? N48D2_G_OK : N48D2_G_VALUE; }
    return N48D2_G_OK;
}
static inline uint32_t n48d2_guard_value(uint32_t abs, uint32_t v) { return n48d2_guard_value_i(N48D2_INST_MONA, abs, v); }
static inline uint32_t n48d2_guard_write_i(uint32_t inst, uint32_t abs, uint32_t v, const char **why)
{
    const uint32_t g = n48d2_guard_addr_i(inst, abs, why);
    return g != N48D2_G_OK ? g : n48d2_guard_value_i(inst, abs, v);
}
static inline uint32_t n48d2_guard_write(uint32_t abs, uint32_t v, const char **why) { return n48d2_guard_write_i(N48D2_INST_MONA, abs, v, why); }
// 0.0.635: the per-register KIND / MASK rules of instance 2's plane (judged on the STEP, at list level). A mirror destination is writable by its exact COPY0 pair ONLY; every other COPY0 is refused; the shared DCCG register takes an UPDATE of mask EXACTLY
// 0x40; HUBP2_DCHUBP_CNTL an UPDATE whose mask is inside {BLANK_EN, UNDERFLOW_CLEAR}; the HUBP clock an UPDATE inside 0x00011111; DPP_TOP2 an UPDATE of mask exactly 0x10; ODM2 an UPDATE of mask exactly 0x1000; CRC_CNTL an UPDATE of mask exactly 0x00700011;
// the DTO, the pitch, the address halves, MPCC2, MPC_OUT2_MUX and the CRC windows are SETs. Anything else on those registers is refused.
static inline int n48d2_plane_kind_ok(const struct n48d2_step *s)
{
    unsigned i;
    const uint32_t a = s->abs, m = s->mask, v = s->val;
    if (s->kind == N48D2_K_COPY0) return n48d2_mirror_ok(a, s->cond_abs, v, s->cond_mask) && m == 0xFFFFFFFFu;
    if (n48d2_mirror_dst(a)) return 0;
    if (a == N48D2_DPPCLK_CTRL) return s->kind == N48D2_K_UPD && m == N48D2_DPPCLK2_EN_MASK && (v & ~m) == 0u;
    if (a == N48D2_DPP_TOP2_DPP_CONTROL) return s->kind == N48D2_K_UPD && m == N48D2_DPP_CLOCK_ENABLE_MASK && (v & ~m) == 0u;
    if (a == N48D2_HUBP2_DCHUBP_CNTL) return s->kind == N48D2_K_UPD && m != 0u && (m & ~N48D2_HUBP_CNTL_WRITE_MASK) == 0u && (v & ~m) == 0u;
    if (a == N48D2_HUBP2_HUBP_CLK_CNTL) return s->kind == N48D2_K_UPD && m != 0u && (m & ~N48D2_HUBP_CLK_WRITE_MASK) == 0u && (v & ~m) == 0u;
    if (a == N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL) return s->kind == N48D2_K_UPD && m == N48D2_ODM_UNDERFLOW_CLEAR_MASK && (v & ~m) == 0u;
    if (a == N48D2_OTG2_OTG_CRC_CNTL) return s->kind == N48D2_K_UPD && m == N48D2_CRC_CNTL_MASK && (v & ~0x11u) == 0u;
    if (a == N48D2_OTG2_OTG_GLOBAL_CONTROL2) return s->kind == N48D2_K_UPD && m == N48D2_OTG_LOCK_SEL_MASK && v == N48D2_OTG_LOCK_SEL_OTG2;       /* 0.0.643: an UPDATE of the lock-select field alone, to 2 */
    if (a == N48D2_OTG2_OTG_MASTER_UPDATE_LOCK) return s->kind == N48D2_K_UPD && m == N48D2_OTG_MASTER_UPDATE_LOCK_MASK && (v & ~m) == 0u;          /* 0.0.643: an UPDATE of the lock bit alone (1 = lock, 0 = unlock) */
    if (a == N48D2_DPPCLK2_DTO_PARAM) return s->kind == N48D2_K_SET;
    for (i = 0; i < N48D2_ALLOWED2P_COUNT; i++) if (a == n48d2_allowed2p[i]) return s->kind == N48D2_K_SET;     /* the rest of the plane list: whole-register SETs */
    return 1;
}
// 0.0.655: the per-register KIND / MASK rules of instance 1's plane (judged on the STEP, at list level): COPY0 is NEVER legal for instance 1 (refused before this); the shared DCCG register takes an UPDATE of mask EXACTLY 0x8; HUBP1_DCHUBP_CNTL an UPDATE
// inside {BLANK_EN, UNDERFLOW_CLEAR}; the HUBP clock an UPDATE inside 0x00011111; DPP_TOP1 an UPDATE of mask exactly 0x10; ODM1 an UPDATE of mask exactly 0x1000; CRC_CNTL an UPDATE of mask exactly 0x00700011; OTG1's lock select an UPDATE of mask 0x0E000000 to 0x02000000;
// the lock an UPDATE of bit 0 alone; DSCL1_LB_MEMORY_CTRL an UPDATE of mask exactly 0x3F03 (value 0x3f00); the programming list EXACTLY its kind and mask (n48d1_pin[]); every other plane register a whole-register SET.
static inline int n48d1_plane_kind_ok(const struct n48d2_step *s)
{
    unsigned i;
    const uint32_t a = s->abs, m = s->mask, v = s->val;
    for (i = 0; i < N48D1_PROG_COUNT; i++) if (a == n48d1_pin[i].abs) return s->kind == n48d1_pin[i].kind && m == n48d1_pin[i].mask && v == n48d1_pin[i].val;      /* kind, mask AND value, for an UPDATE as well (the list guard judges SET values itself) */
    if (a == N48D2_DPPCLK_CTRL) return s->kind == N48D2_K_UPD && m == N48D2_DPPCLK1_EN_MASK && (v & ~m) == 0u;
    if (a == N48D2_DPP_TOP1_DPP_CONTROL) return s->kind == N48D2_K_UPD && m == N48D2_DPP_CLOCK_ENABLE_MASK && (v & ~m) == 0u;
    if (a == N48D2_HUBP1_DCHUBP_CNTL) return s->kind == N48D2_K_UPD && m != 0u && (m & ~N48D2_HUBP_CNTL_WRITE_MASK) == 0u && (v & ~m) == 0u;
    if (a == N48D2_HUBP1_HUBP_CLK_CNTL) return s->kind == N48D2_K_UPD && m != 0u && (m & ~N48D2_HUBP_CLK_WRITE_MASK) == 0u && (v & ~m) == 0u;
    if (a == N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL) return s->kind == N48D2_K_UPD && m == N48D2_ODM_UNDERFLOW_CLEAR_MASK && (v & ~m) == 0u;
    if (a == N48D2_OTG1_OTG_CRC_CNTL) return s->kind == N48D2_K_UPD && m == N48D2_CRC_CNTL_MASK && (v & ~0x11u) == 0u;
    if (a == N48D2_OTG1_OTG_GLOBAL_CONTROL2) return s->kind == N48D2_K_UPD && m == N48D2_OTG_LOCK_SEL_MASK && v == N48D2_OTG_LOCK_SEL_OTG1;
    if (a == N48D2_OTG1_OTG_MASTER_UPDATE_LOCK) return s->kind == N48D2_K_UPD && m == N48D2_OTG_MASTER_UPDATE_LOCK_MASK && (v & ~m) == 0u;
    if (a == N48D2_DPPCLK1_DTO_PARAM) return s->kind == N48D2_K_SET;
    for (i = 0; i < N48D2_ALLOWED1P_COUNT; i++) if (a == n48d2_allowed1p[i]) return s->kind == N48D2_K_SET;
    return 1;
}
// The whole list, before anything is read or written: every write step's ADDRESS (values are judged per write, on the value computed from the live read).
// The value guard is ALSO applied here to every SET (its value is known now) and to every UPDATE's field bits (an UPDATE whose own bits already break the value rule is refused here).
// The symbol clock exception is judged here too: a step on it must be an UPDATE (never a SET) whose mask is inside bits 10:8 and 4 (0x710) - bit 0, the PHY's own CLOCK_ENABLE, is never writable - and whose value
// lies inside its mask with FE_SRC_SEL 0 or the instance's link.
// 0 = the list may run; else N48D2_GUARD_REFUSED with *bad = the first offending step index.
static inline uint32_t n48d2_guard_list_i(uint32_t inst, const struct n48d2_step *s, uint32_t n, uint32_t *bad)
{
    uint32_t i;
    const struct n48d2_inst *d = n48d2_inst_get(inst);
    const struct n48d2_pr *pr = n48d2_pr_get(inst);      /* 0.0.655: the instance's HUBP / address registers (the monitor B's are the constants this function always named) */
    uint32_t blanked = 0u, noout = 0u;     /* 0.0.638: seen earlier in THIS list: a BLANK_EN = 1 UPDATE of the instance's DCHUBP_CNTL, and after it the NO_OUTSTANDING_REQ WAIT */
    *bad = 0xFFFFFFFFu;
    if (!d || n == 0u || n > N48D2_MAX_STEPS) return N48D2_GUARD_REFUSED;
    for (i = 0; i < n; i++) {
        if (!n48d2_is_write(&s[i])) {
            if (s[i].kind == N48D2_K_REC) {          /* 0.0.638: a recorded READ: only the four registers, each in its own slot; never a write */
                if (!n48d2_rec_ok_i(inst, s[i].abs, s[i].val)) { *bad = i; return N48D2_GUARD_REFUSED; }
                continue;
            }
            if (s[i].kind != N48D2_K_WAIT && s[i].kind != N48D2_K_WAITIF && s[i].kind != N48D2_K_EXPECT) { *bad = i; return N48D2_GUARD_REFUSED; }
            if (s[i].kind == N48D2_K_WAIT && blanked && s[i].abs == pr->hubp_cntl && (s[i].mask & N48D2_HUBP_NO_OUTSTANDING_MASK) != 0u && (s[i].val & N48D2_HUBP_NO_OUTSTANDING_MASK) != 0u) noout = 1u;
            continue;
        }
        if (n48d2_guard_addr_i(inst, s[i].abs, 0) != N48D2_G_OK) { *bad = i; return N48D2_GUARD_REFUSED; }
        if (s[i].kind == N48D2_K_COPY0 && inst != N48D2_INST_MONB) { *bad = i; return N48D2_GUARD_REFUSED; }
        if (inst == N48D2_INST_MONB && !n48d2_plane_kind_ok(&s[i])) { *bad = i; return N48D2_GUARD_REFUSED; }
        if (inst == N48D2_INST_MONA && !n48d1_plane_kind_ok(&s[i])) { *bad = i; return N48D2_GUARD_REFUSED; }      /* 0.0.655 */
        if (s[i].kind == N48D2_K_SET && n48d2_guard_value_i(inst, s[i].abs, s[i].val) != N48D2_G_OK) { *bad = i; return N48D2_GUARD_REFUSED; }
        if (s[i].kind == N48D2_K_SET && s[i].val == 0u && (s[i].abs == pr->addr_lo || s[i].abs == pr->addr_hi) && (inst == N48D2_INST_MONB || inst == N48D2_INST_MONA) && !(blanked && noout)) { *bad = i; return N48D2_GUARD_REFUSED; }     /* 0.0.638: a ZERO address only after BLANK_EN = 1 AND the NO_OUTSTANDING_REQ wait, earlier in this very list */
        if (s[i].kind == N48D2_K_UPD && s[i].abs == pr->hubp_cntl && (s[i].mask & N48D2_HUBP_BLANK_EN_MASK) != 0u) { blanked = (s[i].val & N48D2_HUBP_BLANK_EN_MASK) != 0u ? 1u : 0u; noout = 0u; }     /* every BLANK_EN update restarts the wait: a zero needs a NO_OUTSTANDING wait after the LATEST blank (an unblank cancels) */
        if (s[i].abs == d->be_cntl && (s[i].val & s[i].mask & N48D2_BE_FE_SOURCE_MASK & ~d->fe_bit) != 0u) { *bad = i; return N48D2_GUARD_REFUSED; }
        if (s[i].abs == d->fe_cntl && (s[i].mask & N48D2_DIG_SOURCE_SELECT_MASK) != 0u && (s[i].val & N48D2_DIG_SOURCE_SELECT_MASK) != d->otg_inst) { *bad = i; return N48D2_GUARD_REFUSED; }
        if (s[i].abs == d->mapper && (s[i].val & s[i].mask & N48D2_MAPPER_LINK_MASK) != 0u && (s[i].val & N48D2_MAPPER_LINK_MASK) != d->link) { *bad = i; return N48D2_GUARD_REFUSED; }
        if (s[i].abs == d->symclk) {
            const uint32_t sel = (s[i].val & s[i].mask & N48D2_SYMCLK_FE_SRC_MASK) >> 8;
            if (s[i].kind != N48D2_K_UPD || (s[i].mask & ~N48D2_SYMCLK_FE_MASK) != 0u || (s[i].val & ~s[i].mask) != 0u || (sel != 0u && sel != d->link)) { *bad = i; return N48D2_GUARD_REFUSED; }
        }
    }
    return 0u;
}
static inline uint32_t n48d2_guard_list(const struct n48d2_step *s, uint32_t n, uint32_t *bad) { return n48d2_guard_list_i(N48D2_INST_MONA, s, n, bad); }
// The value an UPDATE / SET step writes over the live value.
static inline uint32_t n48d2_apply(const struct n48d2_step *s, uint32_t old) { return s->kind == N48D2_K_SET ? s->val : ((old & ~s->mask) | (s->val & s->mask)); }

// ---- the DP health snapshot (READS ONLY) and the prechecks ------------------------------------------------------------------------------------------------------------------------------------------------
#define N48D2_DP_REGS 6u
static const uint32_t n48d2_dp_regs[N48D2_DP_REGS] = { N48D2_OTG0_CONTROL, N48D2_DIG1_FE_CNTL, N48D2_DIG1_BE_CLK_CNTL, N48D2_DIG1_BE_CNTL, N48D2_DIG1_BE_EN_CNTL, N48D2_R(0x1f0eu) /* DIG1 mapper 0x53ce */ };
#define N48D2_SETTLE_MS 50u           /* 3 frames at 60 Hz: OTG0's counter must move across it */
// 0.0.634 (stage M4a item 4): the DP's PLANE side, nine more READS compared before / after every op (n48d2_dpx_regs; the CLI reads the same nine as dispcensus page 10 = n48dr_census4, pinned equal by the host test).
// A masked change of any of them ends the op in N48D2_DP_DISTURBED exactly like a change of the six above. HUBP0's DCHUBP_CNTL is compared on every bit except the three that toggle with the raster (NO_OUTSTANDING_REQ
// [1], IN_BLANK [3], XRQ_NO_OUTSTANDING_REQ [19:16]); that keeps TTU / VTG_SEL / BLANK_EN / TIMEOUT_STATUS and the underflow bits [30:28]. DPPCLK_CTRL is shared by every pipe: only bit 0 (DPPCLK0_EN) is the DP's.
#define N48D2_DPX_REGS 15u            /* 0.0.635: nine became fifteen (rows 9..14 below); the buffer has one more dword: the HUBP0 primary address HIGH half (index 15) */
#define N48D2_DPX_BUF  16u
#define N48D2_DPX_RULE 14u            /* row 14: HUBP0's primary address (LOW; HIGH is buffer dword 15) is NOT compared for equality: it must not be A or B and must lie inside the scanout window (exact equality only while the desktop scanout is not acquired) */
#define N48D2_DPX_HUBP_CNTL_MASK  0xFFF0FFF5u   /* ~(0x2 | 0x8 | 0xF0000) */
struct n48d2_dpx { uint32_t abs, mask; const char *name; };
static const struct n48d2_dpx n48d2_dpx_regs[N48D2_DPX_REGS] = {
    { N48D2_MPC_OUT0_MUX, 0xFFFFFFFFu, "MPC_OUT0_MUX" }, { N48D2_MPCC0_TOP_SEL, 0xFFFFFFFFu, "MPCC0_MPCC_TOP_SEL" }, { N48D2_MPCC0_BOT_SEL, 0xFFFFFFFFu, "MPCC0_MPCC_BOT_SEL" },
    { N48D2_MPCC0_OPP_ID, 0xFFFFFFFFu, "MPCC0_MPCC_OPP_ID" }, { N48D2_HUBP0_DCHUBP_CNTL, N48D2_DPX_HUBP_CNTL_MASK, "HUBP0_DCHUBP_CNTL" }, { N48D2_DPPCLK0_DTO_PARAM, 0xFFFFFFFFu, "DPPCLK0_DTO_PARAM" },
    { N48D2_DPPCLK_CTRL, 0x00000001u, "DPPCLK_CTRL bit 0" }, { N48D2_DET0_CTRL, 0xFFFFFFFFu, "DCHUBBUB_DET0_CTRL" }, { N48D2_COMPBUF_CTRL, 0xFFFFFFFFu, "DCHUBBUB_COMPBUF_CTRL" },
    /* 0.0.635 (M4c): the six more. Row 9 is HUBP0_DCHUBP_CNTL again with the strict mask (underflow 30:28 and SEG_ALLOC_ERR 11 only); row 10 ODM0's underflow-occurred bit; row 11 DPP_TOP0; row 12 the SELECT field of MPCC0's UPDATE_LOCK_SEL (the
       locked-status bits [6:4] move); row 13 OTG2_GLOBAL_CONTROL2; row 14 the HUBP0 primary address (the rule). */
    { N48D2_HUBP0_DCHUBP_CNTL, 0x70000800u, "HUBP0_DCHUBP_CNTL 30:28 + 11" }, { N48D2_ODM0_OPTC_INPUT_GLOBAL_CONTROL, 0x00000400u, "ODM0_OPTC_INPUT_GLOBAL_CONTROL bit 10" }, { N48D2_DPP_TOP0_DPP_CONTROL, 0xFFFFFFFFu, "DPP_TOP0_DPP_CONTROL" },
    { N48D2_MPCC0_UPDATE_LOCK_SEL, 0x0000000Fu, "MPCC0_UPDATE_LOCK_SEL" }, { N48D2_OTG2_OTG_GLOBAL_CONTROL2, 0xFFFFFFFFu, "OTG2_GLOBAL_CONTROL2" }, { N48D2_HUBPREQ0_PRIMARY_LOW, 0xFFFFFFFFu, "HUBP0 primary address (rule)" },
};

// ---- the result (13 scalars) --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// timing / connect / off:  v[0] = status | op << 8 | steps done << 16 | steps total << 24 | failing / refused step << 32 (0xff = none) | wait timeouts << 40 | auto-off << 48 | FIFO-reset waited << 49 | FIFO-reset delayed (no symclk) << 50 | instance 2 << 51 | guard code << 52 | DP changed << 56 (bit 6 = SYMCLKB changed)
//                          v[1] = OTG0 frames before | after << 32        v[2] = OTG1 frames before | after << 32        v[3] = the precheck register (abs, bits 0..19) | the DP-plane change mask << 20 (9 bits, 0.0.634) | its value << 32
//                          v[4..9] = the six DP registers before | after << 32 (OTG0_CONTROL, DIG1 FE_CNTL, BE_CLK_CNTL, BE_CNTL, BE_EN_CNTL, DIG1 mapper)     v[10] = OTG1_CONTROL | DIG2_FE_EN_CNTL << 32 (after)
//                          v[11] = DPG1_CONTROL | DIG2_BE_CNTL << 32 (after)     v[12] = DIG2_FIFO_CTRL0 | DIG2_FE_CLK_CNTL << 32 (after)
// status:                  v[0] = status | op << 8 | N48D2_STAT_COUNT << 16      v[1..12] = n48d2_stat_regs[] read, two per scalar (index 2k low, 2k + 1 high); the OTG frame counters are read twice, 50 ms apart.
struct n48d2_out {
    uint32_t status, op, done, total, fail, timeouts, auto_off, guard, dp_changed;
    uint32_t f0a, f0b, f1a, f1b, pre_abs, pre_val;
    uint32_t dp_before[N48D2_DP_REGS], dp_after[N48D2_DP_REGS];
    uint32_t otg1_ctl, fe_en, dpg_ctl, be_cntl, fifo, fe_clk;
    uint32_t inst;                         /* 0.0.633: the instance this result is for (1 = monitor A, 2 = monitor B): bit 51 of v[0] */
    uint32_t symclkb_a, symclkb_b;         /* 0.0.633: SYMCLKB_CLOCK_ENABLE (the live DP's symbol clock, READ only) before / after the op: not packed (the CLI reads it from `status3`); a change sets dp_changed bit 6 and ends in N48D2_DP_DISTURBED */
    uint32_t dpx_changed;                  /* 0.0.634: bit i = n48d2_dpx_regs[i] changed (masked) across the op: packed into v[3] bits 20..28. The nine masked before / after values live in the ENVIRONMENT's two buffers (E::dpx_buf(0) / (1): file-scope in the kext, so the submit-path frame does not grow); the kext logs them, the CLI reads dispcensus page 10 itself */
    uint32_t symclk_wait, symclk_delay;   /* 0.0.632: how many enc35_reset_fifo steps saw the FE symbol clock on (they WAITED) / not (they only delayed 10 us): packed as two bits of v[0] */
    uint64_t wfail;                        /* 0.0.635: bit i = step i of the LAST step list run timed out (a WAIT); the plane flow reads the bits of the WAITs it gates on */
    uint32_t free_bufs;                    /* 0.0.635: the rollback ran and HUBP2's clock reads off: the glue may give the two buffers back (AFTER it released its lock) */
    uint32_t wait_abs, wait_val;           /* 0.0.638: the FIRST WAIT of the current step list that timed out: its register and the LAST value read (0 / 0 = none) */
    uint32_t pre_wait;                     /* 0.0.638: pre_abs / pre_val are a timed-out WAIT's register and last value (packed as v[3] bit 31) */
    uint32_t w2_changed;                   /* 0.0.655: the monitor B watch of an MONA op (n48d2_w2_diff: bit = row 0..8): NOT packed on its own - a plane op carries it in the META gate dword bits 16..24, every monitor A op in the 4-bit guard field as (first changed row + 1) when the status is N48D2_DP_DISTURBED */
};
// The FIFO-reset report for the CLI and the log: "wait ok" = every reset step saw DIG2's FE symbol clock on (SYMCLK_FE_G_CLOCK_ON) and waited for reset-done; "delay (no symclk)" = none did.
static inline const char *n48d2_symclk_report(const struct n48d2_out *o)
{
    return (o->symclk_wait != 0u && o->symclk_delay != 0u) ? "mixed (one reset step waited, one delayed)" : o->symclk_wait != 0u ? "wait ok (FE symbol clock on)" : o->symclk_delay != 0u ? "delay (no symclk)" : "not reached";
}
static inline void n48d2_pack(uint64_t *v, const struct n48d2_out *o)
{
    unsigned i;
    for (i = 0; i < 13u; i++) v[i] = 0u;
    v[0] = (uint64_t)(o->status & 0xFFu) | ((uint64_t)(o->op & 0xFFu) << 8) | ((uint64_t)(o->done & 0xFFu) << 16) | ((uint64_t)(o->total & 0xFFu) << 24) | ((uint64_t)(o->fail & 0xFFu) << 32) |
           ((uint64_t)(o->timeouts & 0xFFu) << 40) | ((uint64_t)(o->auto_off & 1u) << 48) | ((uint64_t)(o->guard & 0xFu) << 52) | ((uint64_t)(o->dp_changed & 0xFFu) << 56) |
           ((uint64_t)(o->symclk_wait ? 1u : 0u) << 49) | ((uint64_t)(o->symclk_delay ? 1u : 0u) << 50) | ((uint64_t)(o->inst == N48D2_INST_MONB ? 1u : 0u) << 51);
    v[1] = (uint64_t)o->f0a | ((uint64_t)o->f0b << 32);
    v[2] = (uint64_t)o->f1a | ((uint64_t)o->f1b << 32);
    v[3] = (uint64_t)(o->pre_abs & 0xFFFFu) | ((uint64_t)(o->dpx_changed & 0x7FFFu) << 16) | ((uint64_t)(o->pre_wait & 1u) << 31) | ((uint64_t)o->pre_val << 32);   /* 0.0.635: pre_abs is a BAR5 dword below 0x10000 (every register this verb names is); bits 16..30 = the 15-bit DP-plane change mask */
    for (i = 0; i < N48D2_DP_REGS; i++) v[4 + i] = (uint64_t)o->dp_before[i] | ((uint64_t)o->dp_after[i] << 32);
    v[10] = (uint64_t)o->otg1_ctl | ((uint64_t)o->fe_en << 32);
    v[11] = (uint64_t)o->dpg_ctl | ((uint64_t)o->be_cntl << 32);
    v[12] = (uint64_t)o->fifo | ((uint64_t)o->fe_clk << 32);
}
static inline void n48d2_unpack(const uint64_t *v, struct n48d2_out *o)
{
    unsigned i;
    o->status = (uint32_t)(v[0] & 0xFFu); o->op = (uint32_t)((v[0] >> 8) & 0xFFu); o->done = (uint32_t)((v[0] >> 16) & 0xFFu); o->total = (uint32_t)((v[0] >> 24) & 0xFFu);
    o->fail = (uint32_t)((v[0] >> 32) & 0xFFu); o->timeouts = (uint32_t)((v[0] >> 40) & 0xFFu); o->auto_off = (uint32_t)((v[0] >> 48) & 1u); o->guard = (uint32_t)((v[0] >> 52) & 0xFu); o->dp_changed = (uint32_t)((v[0] >> 56) & 0xFFu);
    o->symclk_wait = (uint32_t)((v[0] >> 49) & 1u); o->symclk_delay = (uint32_t)((v[0] >> 50) & 1u); o->inst = ((v[0] >> 51) & 1u) ? N48D2_INST_MONB : N48D2_INST_MONA;
    o->f0a = (uint32_t)v[1]; o->f0b = (uint32_t)(v[1] >> 32); o->f1a = (uint32_t)v[2]; o->f1b = (uint32_t)(v[2] >> 32); o->pre_abs = (uint32_t)(v[3] & 0xFFFFu); o->dpx_changed = (uint32_t)((v[3] >> 16) & 0x7FFFu); o->pre_wait = (uint32_t)((v[3] >> 31) & 1u); o->pre_val = (uint32_t)(v[3] >> 32);
    for (i = 0; i < N48D2_DP_REGS; i++) { o->dp_before[i] = (uint32_t)v[4 + i]; o->dp_after[i] = (uint32_t)(v[4 + i] >> 32); }
    o->otg1_ctl = (uint32_t)v[10]; o->fe_en = (uint32_t)(v[10] >> 32); o->dpg_ctl = (uint32_t)v[11]; o->be_cntl = (uint32_t)(v[11] >> 32); o->fifo = (uint32_t)v[12]; o->fe_clk = (uint32_t)(v[12] >> 32);
}
// 0.0.635, ops 8..12: v[0..3] exactly as above (v[1] OTG0 frames, v[2] OTG2 frames, v[3] the precheck register | the 15-bit DP-plane change mask << 16 | its value << 32); v[4..12] = the 18 GATE dwords (N48D2_GT_*), two per scalar (index 2k low, 2k + 1 high).
static inline void n48d2_pack_plane(uint64_t *v, const struct n48d2_out *o, const uint32_t *g)
{
    unsigned i;
    n48d2_pack(v, o);
    for (i = 4u; i < 13u; i++) v[i] = 0u;
    for (i = 0u; i < N48D2_GATES; i++) v[4u + i / 2u] |= (uint64_t)g[i] << (32u * (i & 1u));
}
static inline void n48d2_unpack_plane(const uint64_t *v, struct n48d2_out *o, uint32_t *g)
{
    unsigned i;
    n48d2_unpack(v, o);
    for (i = 0u; i < N48D2_GATES; i++) g[i] = (uint32_t)(v[4u + i / 2u] >> (32u * (i & 1u)));
}
// 0.0.638, op 13 (planerec; reads NO register): v[0] = status | op << 8 | N48D2_REC_N << 16; v[1..8] = the 16 record dwords (n48d2_rec layout above), two per scalar.
static inline void n48d2_pack_rec(uint64_t *v, const uint32_t *rec)
{
    unsigned i;
    for (i = 0; i < 13u; i++) v[i] = 0u;
    v[0] = (uint64_t)N48D2_OK | ((uint64_t)N48D2_OP_PLANEREC << 8) | ((uint64_t)N48D2_REC_N << 16);
    for (i = 0u; i < N48D2_REC_N; i++) v[1u + i / 2u] |= (uint64_t)rec[i] << (32u * (i & 1u));
}
static inline void n48d2_unpack_rec(const uint64_t *v, uint32_t *rec)
{
    unsigned i;
    for (i = 0u; i < N48D2_REC_N; i++) rec[i] = (uint32_t)(v[1u + i / 2u] >> (32u * (i & 1u)));
}
// status2 (0.0.632, op 5): v[0] = status | op << 8 | N48D2_STAT2_COUNT << 16      v[1..6] = n48d2_stat2_regs[], two per scalar. Reads only; no frame counters.
#define N48D2_STAT2_COUNT 12u
#define N48D2_STAT_HELD_BIT 52u       /* 0.0.652 (M5): bit 52 of op 4's out[0] = the monitor B's plane is HELD (instance 2 only; set by the glue after the status page) */
#define N48D2_STAT_COUNT 24u
#define N48D2_STAT_OTG1_FC_A 2u       /* the indices read twice: n48d2_stat_regs[2] / [3] (OTG1 frames), [21] / [22] (OTG0 frames) */
#define N48D2_STAT_OTG0_FC_A 21u
struct n48d2_stat_reg { uint32_t abs; const char *name; };
static const struct n48d2_stat_reg n48d2_stat_regs[N48D2_STAT_COUNT] = {
    { N48D2_OTG1_CONTROL, "OTG1_OTG_CONTROL" }, { N48D2_OTG1_STATUS, "OTG1_OTG_STATUS" }, { N48D2_OTG1_FRAME_COUNT, "OTG1 frame count" }, { N48D2_OTG1_FRAME_COUNT, "OTG1 frame count +50 ms" },
    { N48D2_OTG1_CLOCK_CONTROL, "OTG1_OTG_CLOCK_CONTROL" }, { N48D2_ODM1_INPUT_CLOCK_CONTROL, "ODM1_OPTC_INPUT_CLOCK_CONTROL" }, { N48D2_ODM1_DATA_SOURCE_SELECT, "ODM1_OPTC_DATA_SOURCE_SELECT" },
    { N48D2_VTG1_CONTROL, "VTG1_CONTROL" }, { N48D2_OPP_PIPE1_CONTROL, "OPP_PIPE1_OPP_PIPE_CONTROL" }, { N48D2_DPG1_CONTROL, "DPG1_DPG_CONTROL" }, { N48D2_DPG1_STATUS, "DPG1_DPG_STATUS" },
    { N48D2_DIG2_FE_CNTL, "DIG2_DIG_FE_CNTL" }, { N48D2_DIG2_FE_CLK_CNTL, "DIG2_DIG_FE_CLK_CNTL" }, { N48D2_DIG2_FE_EN_CNTL, "DIG2_DIG_FE_EN_CNTL" }, { N48D2_DIG2_FIFO_CTRL0, "DIG2_DIG_FIFO_CTRL0" },
    { N48D2_DIG2_BE_CNTL, "DIG2_DIG_BE_CNTL" }, { N48D2_DIG2_BE_CLK_CNTL, "DIG2_DIG_BE_CLK_CNTL" }, { N48D2_DIG2_BE_EN_CNTL, "DIG2_DIG_BE_EN_CNTL" }, { N48D2_DIG2_STREAM_MAPPER, "DIG2_STREAM_MAPPER_CONTROL" },
    { N48D2_DIG2_HDMI_CONTROL, "DIG2_HDMI_CONTROL" }, { N48D2_OTG1_PHYPLL_PIXEL_RATE_CNTL, "OTG1_PHYPLL_PIXEL_RATE_CNTL" }, { N48D2_OTG0_FRAME_COUNT, "OTG0 frame count" },
    { N48D2_OTG0_FRAME_COUNT, "OTG0 frame count +50 ms" }, { N48D2_DIG1_BE_CNTL, "DIG1_DIG_BE_CNTL" },
};
static const struct n48d2_stat_reg n48d2_stat2_regs[N48D2_STAT2_COUNT] = {
    { N48D2_SYMCLKC_CLOCK_ENABLE, "SYMCLKC_CLOCK_ENABLE" }, { N48D2_OTG1_PIXEL_RATE_CNTL, "OTG1_PIXEL_RATE_CNTL" }, { N48D2_OTG1_V_TOTAL_CONTROL, "OTG1_OTG_V_TOTAL_CONTROL" },
    { N48D2_FMT1_CONTROL, "FMT1_FMT_CONTROL" }, { N48D2_FMT1_BIT_DEPTH_CONTROL, "FMT1_FMT_BIT_DEPTH_CONTROL" }, { N48D2_FMT1_DYNAMIC_EXP_CNTL, "FMT1_FMT_DYNAMIC_EXP_CNTL" },
    { N48D2_FMT1_CLAMP_CNTL, "FMT1_FMT_CLAMP_CNTL" }, { N48D2_FMT1_CLAMP_COMPONENT_R, "FMT1_FMT_CLAMP_COMPONENT_R" }, { N48D2_FMT1_CLAMP_COMPONENT_G, "FMT1_FMT_CLAMP_COMPONENT_G" },
    { N48D2_FMT1_CLAMP_COMPONENT_B, "FMT1_FMT_CLAMP_COMPONENT_B" }, { N48D2_HUBP1_DCHUBP_CNTL, "HUBP1_DCHUBP_CNTL" }, { N48D2_HUBP1_HUBP_CLK_CNTL, "HUBP1_HUBP_CLK_CNTL" },
};

// ---- instance 2 (0.0.633): the same two pages one instance up. Index positions are identical (the frame counters sit at [2] / [3] and [21] / [22]). ---------------------------------------------------------------------
static const struct n48d2_stat_reg n48d2_stat_regs2[N48D2_STAT_COUNT] = {
    { N48D2_OTG2_CONTROL, "OTG2_OTG_CONTROL" }, { N48D2_OTG2_STATUS, "OTG2_OTG_STATUS" }, { N48D2_OTG2_FRAME_COUNT, "OTG2 frame count" }, { N48D2_OTG2_FRAME_COUNT, "OTG2 frame count +50 ms" },
    { N48D2_OTG2_CLOCK_CONTROL, "OTG2_OTG_CLOCK_CONTROL" }, { N48D2_ODM2_INPUT_CLOCK_CONTROL, "ODM2_OPTC_INPUT_CLOCK_CONTROL" }, { N48D2_ODM2_DATA_SOURCE_SELECT, "ODM2_OPTC_DATA_SOURCE_SELECT" },
    { N48D2_VTG2_CONTROL, "VTG2_CONTROL" }, { N48D2_OPP_PIPE2_CONTROL, "OPP_PIPE2_OPP_PIPE_CONTROL" }, { N48D2_DPG2_CONTROL, "DPG2_DPG_CONTROL" }, { N48D2_DPG2_STATUS, "DPG2_DPG_STATUS" },
    { N48D2_DIG3_FE_CNTL, "DIG3_DIG_FE_CNTL" }, { N48D2_DIG3_FE_CLK_CNTL, "DIG3_DIG_FE_CLK_CNTL" }, { N48D2_DIG3_FE_EN_CNTL, "DIG3_DIG_FE_EN_CNTL" }, { N48D2_DIG3_FIFO_CTRL0, "DIG3_DIG_FIFO_CTRL0" },
    { N48D2_DIG3_BE_CNTL, "DIG3_DIG_BE_CNTL" }, { N48D2_DIG3_BE_CLK_CNTL, "DIG3_DIG_BE_CLK_CNTL" }, { N48D2_DIG3_BE_EN_CNTL, "DIG3_DIG_BE_EN_CNTL" }, { N48D2_DIG3_STREAM_MAPPER, "DIG3_STREAM_MAPPER_CONTROL" },
    { N48D2_DIG3_HDMI_CONTROL, "DIG3_HDMI_CONTROL" }, { N48D2_OTG2_PHYPLL_PIXEL_RATE_CNTL, "OTG2_PHYPLL_PIXEL_RATE_CNTL" }, { N48D2_OTG0_FRAME_COUNT, "OTG0 frame count" },
    { N48D2_OTG0_FRAME_COUNT, "OTG0 frame count +50 ms" }, { N48D2_DIG1_BE_CNTL, "DIG1_DIG_BE_CNTL" },
};
static const struct n48d2_stat_reg n48d2_stat2_regs2[N48D2_STAT2_COUNT] = {
    { N48D2_SYMCLKD_CLOCK_ENABLE, "SYMCLKD_CLOCK_ENABLE" }, { N48D2_OTG2_PIXEL_RATE_CNTL, "OTG2_PIXEL_RATE_CNTL" }, { N48D2_OTG2_V_TOTAL_CONTROL, "OTG2_OTG_V_TOTAL_CONTROL" },
    { N48D2_FMT2_CONTROL, "FMT2_FMT_CONTROL" }, { N48D2_FMT2_BIT_DEPTH_CONTROL, "FMT2_FMT_BIT_DEPTH_CONTROL" }, { N48D2_FMT2_DYNAMIC_EXP_CNTL, "FMT2_FMT_DYNAMIC_EXP_CNTL" },
    { N48D2_FMT2_CLAMP_CNTL, "FMT2_FMT_CLAMP_CNTL" }, { N48D2_FMT2_CLAMP_COMPONENT_R, "FMT2_FMT_CLAMP_COMPONENT_R" }, { N48D2_FMT2_CLAMP_COMPONENT_G, "FMT2_FMT_CLAMP_COMPONENT_G" },
    { N48D2_FMT2_CLAMP_COMPONENT_B, "FMT2_FMT_CLAMP_COMPONENT_B" }, { N48D2_HUBP2_DCHUBP_CNTL, "HUBP2_DCHUBP_CNTL" }, { N48D2_HUBP2_HUBP_CLK_CNTL, "HUBP2_HUBP_CLK_CNTL" },
};
// status3 (0.0.633, op 6; reads only): v[0] = status | op << 8 | N48D2_STAT3_COUNT << 16      v[1..7] = n48d2_stat3_regs[] / n48d2_stat3_regs2[], two per scalar. 10 registers of the instance's DIG (CLOCK_PATTERN, TEST_PATTERN, FIFO_CTRL1,
// HDMI_STATUS, TMDS_CNTL, TMDS_CONTROL_CHAR, TMDS_CTL_BITS, TMDS_DCBALANCER_CONTROL, OUTPUT_CRC_CNTL / _RESULT), DIG1's FIFO_CTRL0 and TMDS_CTL_BITS for comparison, SYMCLKB (the DP's: it must not change) and the instance's SYMCLK.
#define N48D2_STAT3_COUNT 14u
static const struct n48d2_stat_reg n48d2_stat3_regs[N48D2_STAT3_COUNT] = {
    { N48D2_DIG2_CLOCK_PATTERN, "DIG2_DIG_CLOCK_PATTERN" }, { N48D2_DIG2_TEST_PATTERN, "DIG2_DIG_TEST_PATTERN" }, { N48D2_DIG2_FIFO_CTRL1, "DIG2_DIG_FIFO_CTRL1" }, { N48D2_DIG2_HDMI_STATUS, "DIG2_HDMI_STATUS" },
    { N48D2_DIG2_TMDS_CNTL, "DIG2_TMDS_CNTL" }, { N48D2_DIG2_TMDS_CONTROL_CHAR, "DIG2_TMDS_CONTROL_CHAR" }, { N48D2_DIG2_TMDS_CTL_BITS, "DIG2_TMDS_CTL_BITS" }, { N48D2_DIG2_TMDS_DCBALANCER_CONTROL, "DIG2_TMDS_DCBALANCER_CONTROL" },
    { N48D2_DIG2_OUTPUT_CRC_CNTL, "DIG2_DIG_OUTPUT_CRC_CNTL" }, { N48D2_DIG2_OUTPUT_CRC_RESULT, "DIG2_DIG_OUTPUT_CRC_RESULT" }, { N48D2_DIG1_FIFO_CTRL0, "DIG1_DIG_FIFO_CTRL0 (DP)" },
    { N48D2_DIG1_TMDS_CTL_BITS, "DIG1_TMDS_CTL_BITS (DP)" }, { N48D2_SYMCLKB_CLOCK_ENABLE, "SYMCLKB_CLOCK_ENABLE (DP)" }, { N48D2_SYMCLKC_CLOCK_ENABLE, "SYMCLKC_CLOCK_ENABLE" },
};
static const struct n48d2_stat_reg n48d2_stat3_regs2[N48D2_STAT3_COUNT] = {
    { N48D2_DIG3_CLOCK_PATTERN, "DIG3_DIG_CLOCK_PATTERN" }, { N48D2_DIG3_TEST_PATTERN, "DIG3_DIG_TEST_PATTERN" }, { N48D2_DIG3_FIFO_CTRL1, "DIG3_DIG_FIFO_CTRL1" }, { N48D2_DIG3_HDMI_STATUS, "DIG3_HDMI_STATUS" },
    { N48D2_DIG3_TMDS_CNTL, "DIG3_TMDS_CNTL" }, { N48D2_DIG3_TMDS_CONTROL_CHAR, "DIG3_TMDS_CONTROL_CHAR" }, { N48D2_DIG3_TMDS_CTL_BITS, "DIG3_TMDS_CTL_BITS" }, { N48D2_DIG3_TMDS_DCBALANCER_CONTROL, "DIG3_TMDS_DCBALANCER_CONTROL" },
    { N48D2_DIG3_OUTPUT_CRC_CNTL, "DIG3_DIG_OUTPUT_CRC_CNTL" }, { N48D2_DIG3_OUTPUT_CRC_RESULT, "DIG3_DIG_OUTPUT_CRC_RESULT" }, { N48D2_DIG1_FIFO_CTRL0, "DIG1_DIG_FIFO_CTRL0 (DP)" },
    { N48D2_DIG1_TMDS_CTL_BITS, "DIG1_TMDS_CTL_BITS (DP)" }, { N48D2_SYMCLKB_CLOCK_ENABLE, "SYMCLKB_CLOCK_ENABLE (DP)" }, { N48D2_SYMCLKD_CLOCK_ENABLE, "SYMCLKD_CLOCK_ENABLE" },
};
#define N48D2_STAT3_SYMCLKB 12u        /* index of SYMCLKB_CLOCK_ENABLE in the status3 pages */
static inline const struct n48d2_stat_reg *n48d2_stat_tbl(uint32_t inst) { return inst == N48D2_INST_MONB ? n48d2_stat_regs2 : n48d2_stat_regs; }
static inline const struct n48d2_stat_reg *n48d2_stat2_tbl(uint32_t inst) { return inst == N48D2_INST_MONB ? n48d2_stat2_regs2 : n48d2_stat2_regs; }
static inline const struct n48d2_stat_reg *n48d2_stat3_tbl(uint32_t inst) { return inst == N48D2_INST_MONB ? n48d2_stat3_regs2 : n48d2_stat3_regs; }

#ifdef __cplusplus
}
#endif
#endif /* N48_DISP2_H */
