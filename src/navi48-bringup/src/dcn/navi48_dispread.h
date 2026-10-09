//
//  navi48_dispread.h - build 0.0.622 (multi-monitor track, stage M1: READ-ONLY instruments; an internal design note, stage M1 and sections 2 / 5).
//
//  The pure half of three accel verbs, shared (one source of truth) by the kext (dcn/navi48_dispread_flow.h + the glue at the end of dcn/navi48_dcn.cpp), the CLI
//  (tools/pc/navi48test.c includes this very file) and the host test (tests/native_dispread_test.cpp). Plain C99 + C++17: no kernel header, no libc beyond stdint.
//
//    91  ddcread <line> <block>   read one 128-byte EDID block over an HDMI DDC line with the DC_I2C engine (the only verb here that writes registers: the engine's own, all inside the
//                                 DCN write allowlist, range "DC..DIG6" 0x5358..0x53d3, and only the bus bytes the plan names).
//    92  dmubring [page]          READ-ONLY state of the display microcontroller (DMUB) the GOP left, and a decode of its inbox ring. NO command is ever sent, no register is written.
//    93  dispcensus [page]        READ-ONLY census of the power-gate domains, the DIG / OTG / DCCG registers of the dark outputs and the hot-plug pins.
//    94  region4read <off> [n]    (0.0.624, stage M1.5) READ-ONLY dump of up to 64 dwords of the DMUB's REGION4 window in VRAM (the ring, the VBIOS ring state, the mode block and the four display
//                                 contexts), through the existing MM_INDEX reader navi48_vram_read_mm. The window base is computed live from the DMCUB registers; the window is 64 KiB.
//
//    95  dmubsend <slotspec>      (0.0.625, stage M2/M3) appends ONE allowlisted command to the DMUB inbox1 ring and waits for it (dcn/navi48_dmubcmd.h). 96 dmubmode, 97 dmubctx: the mode block / marker writes.
//                                 All three are behind boot-arg navi48-dmubcmd=1 (default OFF).
//
//    99  scdcread <line> <off> [len]   (0.0.633) READ-ONLY SCDC status read from the HDMI sink (slave 0x54) over the same DC_I2C engine and the same containment as ddcread: lines 2 / 3 only, offsets
//                                 0x00..0x5F only, 1..16 bytes; the only bus write is the 1-byte register offset. Tells us, unattended, whether the sink locks onto our TMDS (offset 0x40 Status_Flags_0).
//
//    93 (0.0.634, stage M4a): dispcensus pages 10..42 = the PLANE census (page 10: the nine DP-watch registers; pages 11..42: DCHUBBUB / DPPCLK / DCN_VM_FAULT / HUBPREQ0-2 VM / the full HUBP0, HUBPREQ0, HUBPRET0 and DPP0
//                                 (DPP_TOP, CNVC_CFG, DSCL, CM) blocks / MPCC0-2 / MPC_OUT0, 2 / OTG2_GLOBAL_CONTROL2 / last the HUBP2 + DPP2 twins) - READS only, `accel dispcensus plane`.
//
//    93 (0.0.654, monitor A census): dispcensus pages 44..59 = the instance-1 (monitor A, OTG1 / pipe 1) twins of the pipe-2 plane rows plus 13 single registers (OTG1 / ODM1 / DIG2 CRC / DOMAIN1 / DENTIST) - READS only, `accel dispcensus mona`.
//
//  Licence note: the DC_I2C engine sequence in dcn/navi48_dispread_flow.h is ported from RDNA4FB's AMDRDNA4FB::readEDIDI2C (src/RDNA4FB/src/framebuffer.cpp, BSD-3-Clause, verified on
//  hardware there), which in turn follows Linux's dce_i2c_hw.c (acquire_engine / setup_engine / process_transaction / process_channel_reply / release_engine).
//
#ifndef N48_DISPREAD_H
#define N48_DISPREAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- the verbs (accel actions) -----------------------------------------------------------------------------------------------------------------------------------------------------------------
#define N48DR_ACT_DDCREAD    91u
#define N48DR_ACT_DMUBRING   92u
#define N48DR_ACT_DISPCENSUS 93u
#define N48DR_ACT_REGION4READ 94u
#define N48DR_ACT_SCDCREAD   99u     /* 0.0.633: scdcread <line> <off> [len] */

// ---- the verb status (v[0] low byte; the IOReturn of the verb stays success, a refusal is readable from the scalars) -----------------------------------------------------------------------------
enum {
    N48DR_OK = 0, N48DR_BAD_ARG = 1, N48DR_NO_DEVICE = 2, N48DR_ENGINE_DEAD = 3, N48DR_HW_OWNED = 4, N48DR_SW_BUSY = 5, N48DR_NOT_GRANTED = 6, N48DR_NACK = 7, N48DR_TIMEOUT = 8,
    N48DR_ABORTED = 9, N48DR_OVERFLOW = 10, N48DR_NO_DATA = 11, N48DR_BUSY = 12, N48DR_WRITE_REFUSED = 13, N48DR_RING_UNREACHABLE = 14, N48DR_NO_RING = 15, N48DR_PAGE_RANGE = 16,
    N48DR_REGION4_BAD = 17, N48DR_VRAM_READ_FAILED = 18,   /* 0.0.624 (region4read) */
    /* 0.0.625 (stage M2/M3, dcn/navi48_dmubcmd.h): the first verbs that SEND to the display firmware. Each refusal has its own status; every one up to N48DR_WPTR_REFUSED is a refusal BEFORE the WPTR moves. */
    N48DR_CMD_OFF = 19, N48DR_HDR_REFUSED = 20, N48DR_PAYLOAD_REFUSED = 21, N48DR_DMUB_NOT_READY = 22, N48DR_RING_BUSY = 23, N48DR_RING_WRAP = 24, N48DR_RING_NOT_REGION4 = 25,
    N48DR_SLOT_WRITE_FAILED = 26, N48DR_READBACK_MISMATCH = 27, N48DR_WPTR_REFUSED = 28, N48DR_POLL_TIMEOUT = 29, N48DR_VARS_FAILED = 30, N48DR_NO_SAVE = 31, N48DR_SAVE_DIRTY = 32,
    N48DR_MODE_REFUSED = 33, N48DR_CTX_REFUSED = 34,
    /* 0.0.626 (reviews of 0.0.625): a mode / context write needs an idle ring; the live ring size must be the measured 0x2000; a replay source slot that is not usable. Each refusal is BEFORE anything is written. */
    N48DR_RING_NOT_IDLE = 35, N48DR_RING_SIZE_REFUSED = 36, N48DR_REPLAY_REFUSED = 37,
    N48DR_HELD_REFUSED = 38,    /* 0.0.652 (M5): the monitor B's plane is HELD (disp2 fbhold 2): its DMUB teardown templates (phyd-disable, pclk-otg2-off) and `dmubsend replay` are REFUSED */
    N48DR_STATUS_COUNT = 39
};
static inline const char *n48dr_status_name(uint32_t s)
{
    switch (s) {
    case N48DR_OK: return "OK";
    case N48DR_BAD_ARG: return "bad argument (ddcread: line 2 or 3 only, block 0..3; scdcread: line 2 or 3, offset 0..0x5f, length 1..16, offset + length <= 0x60)";
    case N48DR_NO_DEVICE: return "no display layer (no BAR5 / IP discovery / allowlist)";
    case N48DR_ENGINE_DEAD: return "DC_I2C_ARBITRATION reads all ones (the engine does not respond)";
    case N48DR_HW_OWNED: return "the engine is owned by hardware / DMUB: REFUSED, no register written";
    case N48DR_SW_BUSY: return "the engine is already owned by software: REFUSED, no register written";
    case N48DR_NOT_GRANTED: return "the software request was not granted (request withdrawn)";
    case N48DR_NACK: return "the sink did not acknowledge (NACK)";
    case N48DR_TIMEOUT: return "the transaction did not finish inside its bound (or the engine timed out)";
    case N48DR_ABORTED: return "the engine aborted the transaction";
    case N48DR_OVERFLOW: return "the engine's buffer overflowed";
    case N48DR_NO_DATA: return "no stored result for this page";
    case N48DR_BUSY: return "another ddcread is running";
    case N48DR_WRITE_REFUSED: return "the write allowlist refused a register write (aborted and released)";
    case N48DR_RING_UNREACHABLE: return "the ring is beyond the BAR0 aperture: only MM_INDEX reaches it, which this verb does not implement";
    case N48DR_NO_RING: return "no readable DMUB inbox ring (not alive/sane, or the mapping is unknown)";
    case N48DR_PAGE_RANGE: return "page outside the ring's commands";
    case N48DR_REGION4_BAD: return "the REGION4 window computed from the DMCUB registers is not usable (disabled, below the FB base, not 4 KiB aligned, or not inside VRAM): REFUSED, nothing read";
    case N48DR_VRAM_READ_FAILED: return "the MM_INDEX reader refused the read (no device, or out of bounds)";
    case N48DR_CMD_OFF: return "boot-arg navi48-dmubcmd is not 1: the command verbs are OFF, nothing was touched";
    case N48DR_HDR_REFUSED: return "the command header is not on the allowlist (0x04000a80 0x08000680 0x08000580 0x04000780 0x04000880): REFUSED, nothing written";
    case N48DR_PAYLOAD_REFUSED: return "d1 / d2 are not on the allowlist for this sub-type: REFUSED, nothing written";
    case N48DR_DMUB_NOT_READY: return "the DMCUB is not enabled, or is in soft reset: REFUSED, nothing written";
    case N48DR_RING_BUSY: return "inbox1 WPTR != RPTR (the ring is not idle): REFUSED, WPTR not moved";
    case N48DR_RING_WRAP: return "WPTR + 0x40 would reach the end of the ring (the ring wraps; no wrap design exists): REFUSED, nothing written";
    case N48DR_RING_NOT_REGION4: return "the inbox1 ring is not the start of the REGION4 window (or not mapped through REGION4): REFUSED, nothing written";
    case N48DR_SLOT_WRITE_FAILED: return "the MM_INDEX writer refused the slot / block write";
    case N48DR_READBACK_MISMATCH: return "the dwords read back differ from the ones written: REFUSED, WPTR not moved (nothing retried)";
    case N48DR_WPTR_REFUSED: return "the DCN write allowlist refused the WPTR write: nothing was sent";
    case N48DR_POLL_TIMEOUT: return "RPTR did not reach the new WPTR inside the 100 ms bound: everything left as is, nothing retried";
    case N48DR_VARS_FAILED: return "the command completed (RPTR == WPTR) but the VBIOS ring variables could not be written or read back";
    case N48DR_NO_SAVE: return "no saved mode block in this boot: run `dmubmode save` first";
    case N48DR_SAVE_DIRTY: return "a mode block was written since the last save / restore: a second save would overwrite the original";
    case N48DR_MODE_REFUSED: return "dmubmode accepts only save, restore, 0x1a6 and 0x1d4";
    case N48DR_CTX_REFUSED: return "dmubctx accepts only ctx 0x7000 and 0x7800 (never 0x6800, the live DP context)";
    case N48DR_RING_NOT_IDLE: return "dmubmode / dmubctx write only while the inbox1 ring is idle (WPTR == RPTR): REFUSED, nothing written";
    case N48DR_RING_SIZE_REFUSED: return "the live inbox1 ring size is not the measured 0x2000: REFUSED, nothing written (a larger ring would let slots reach the VBIOS variables at +0x3100)";
    case N48DR_REPLAY_REFUSED: return "dmubsend replay: the source slot offset is not 64-byte aligned, not below WPTR, or the argument is malformed: REFUSED, nothing written";
    case N48DR_HELD_REFUSED: return "the monitor B's plane is HELD for this boot (disp2 fbhold 2): its DMUB teardown templates (phyd-disable, pclk-otg2-off) and dmubsend replay are REFUSED, nothing written";
    default: return "unknown";
    }
}

// ---- register offsets (DMU dword offsets from re/linux-dc dcn_4_1_0_offset.h; the BASE_IDX in the name). The host test checks every one against that header. ------------------------------
// DC_I2C engine, all BASE_IDX 2, absolute = DMU segment 2 base (0x34c0 on this card) + offset: 0x5358 .. 0x539e, inside the allowlist range "DC..DIG6" 0x5358..0x53d3 (dcn41_allow_ranges.h).
#define N48DR_I2C_CONTROL        0x1e98u   /* regDC_I2C_CONTROL */
#define N48DR_I2C_ARBITRATION    0x1e99u   /* regDC_I2C_ARBITRATION */
#define N48DR_I2C_SW_STATUS      0x1e9bu   /* regDC_I2C_SW_STATUS */
#define N48DR_I2C_DDC1_SPEED     0x1ea2u   /* regDC_I2C_DDC1_SPEED; line n: + 2n */
#define N48DR_I2C_DDC1_SETUP     0x1ea3u   /* regDC_I2C_DDC1_SETUP; line n: + 2n */
#define N48DR_I2C_TRANSACTION0   0x1eaeu   /* regDC_I2C_TRANSACTION0; transaction n at + n */
#define N48DR_I2C_DATA           0x1eb2u   /* regDC_I2C_DATA */
#define N48DR_DIO_MEM_PWR_STATUS 0x1eddu   /* regDIO_MEM_PWR_STATUS (read) */
#define N48DR_DIO_MEM_PWR_CTRL   0x1edeu   /* regDIO_MEM_PWR_CTRL (BASE_IDX 2): I2C RAM out of light sleep, as RDNA4FB does */
#define N48DR_MICROSECOND_TIME_BASE_DIV 0x007bu   /* regMICROSECOND_TIME_BASE_DIV, BASE_IDX 1 (read only) */

// DC_I2C field bits (RDNA4FB's constants, which follow dcn_4_1_0_sh_mask.h)
#define N48DR_CTL_GO             (1u << 0)
#define N48DR_CTL_SOFT_RESET     (1u << 1)
#define N48DR_CTL_SW_STATUS_RESET (1u << 3)
#define N48DR_CTL_DDC_SELECT_SHIFT 8u
#define N48DR_CTL_TXN_COUNT_SHIFT 20u
#define N48DR_ARB_RW_STATUS_SHIFT 2u
#define N48DR_ARB_RW_STATUS_MASK (3u << 2)    /* 0 idle, 1 software, 2 hardware */
#define N48DR_ARB_NO_QUEUED_SW_GO (1u << 4)
#define N48DR_ARB_SW_USE_REQ     (1u << 20)
#define N48DR_ARB_SW_DONE_USING  (1u << 21)
#define N48DR_ST_DONE            (1u << 2)
#define N48DR_ST_ABORTED         (1u << 4)
#define N48DR_ST_TIMEOUT         (1u << 5)
#define N48DR_ST_OVERFLOW        (1u << 7)
#define N48DR_ST_NACK            (1u << 8)
#define N48DR_ST_FAIL_MASK       (N48DR_ST_ABORTED | N48DR_ST_TIMEOUT | N48DR_ST_OVERFLOW | N48DR_ST_NACK)
#define N48DR_SETUP_CLK_EN       (1u << 3)
#define N48DR_SETUP_ENABLE       (1u << 6)
#define N48DR_SETUP_VALUE        (N48DR_SETUP_ENABLE | N48DR_SETUP_CLK_EN | (1u << 2) | (3u << 24))
#define N48DR_TXN_READ           (1u << 0)
#define N48DR_TXN_STOP_ON_NACK   (1u << 8)
#define N48DR_TXN_START          (1u << 12)
#define N48DR_TXN_STOP           (1u << 13)
#define N48DR_TXN_COUNT_SHIFT    16u
#define N48DR_DATA_READ          (1u << 0)
#define N48DR_DATA_SHIFT         8u
#define N48DR_DATA_INDEX_SHIFT   16u
#define N48DR_DATA_INDEX_WRITE   (1u << 31)

// ---- the DDC plan: line / block validation and the exact transaction list ---------------------------------------------------------------------------------------------------------------------------
// ddcread contract (the brief's containment): lines 2 and 3 ONLY (the two HDMI DDC lines: HDMI-A/ddc2 and HDMI-A/ddc3 of the AtomBIOS connector list); block 0..3; the ONLY bus writes are the
// 1-byte EDID offset (block * 128) & 0xFF to address 0x50 (bus byte 0xA0) and, for block >= 2, the 1-byte segment pointer block / 2 to address 0x30 (bus byte 0x60) in front of it.
#define N48DR_DDC_LINE_MIN 2u
#define N48DR_DDC_LINE_MAX 3u
#define N48DR_DDC_BLOCK_MAX 3u
#define N48DR_EDID_BLOCK 128u
#define N48DR_DDC_ADDR_EDID_W 0xA0u    /* 0x50 << 1 | write */
#define N48DR_DDC_ADDR_EDID_R 0xA1u
#define N48DR_DDC_ADDR_SEG_W  0x60u    /* 0x30 << 1 | write: the E-DDC segment pointer */
#define N48DR_DDC_MAX_TXN 3u
#define N48DR_DDC_POLL_STEP_US 10u              /* one SW_STATUS poll every 10 us ... */
#define N48DR_DDC_POLL_PER_TXN 2000u            /* ... 2000 polls = 20 ms per transaction (the brief's bound), times the transaction count */
#define N48DR_DDC_FIFO_MAX 8u

struct n48dr_txn {
    uint8_t addr8;      /* the 8-bit bus address byte, R/W in bit 0 */
    uint8_t is_read;
    uint8_t wlen;       /* data bytes written after the address byte (0 for a read) */
    uint8_t wdata;      /* the single data byte of a write transaction */
    uint16_t rlen;      /* bytes read */
    uint8_t stop;       /* STOP after this transaction (only the last one) */
};
struct n48dr_plan {
    uint8_t ntxn;
    uint8_t has_seg;
    uint8_t offset;     /* (block * 128) & 0xFF */
    uint8_t seg;        /* block / 2, meaningful when has_seg */
    uint8_t fifo_len;   /* address + data bytes pushed into the engine's FIFO before the GO */
    uint8_t fifo[N48DR_DDC_FIFO_MAX];
    uint8_t read_index; /* FIFO index of the first reply byte: everything pushed before it */
    struct n48dr_txn t[N48DR_DDC_MAX_TXN];
};

static inline int n48dr_ddc_line_ok(uint32_t line) { return line >= N48DR_DDC_LINE_MIN && line <= N48DR_DDC_LINE_MAX; }
static inline int n48dr_ddc_block_ok(uint32_t block) { return block <= N48DR_DDC_BLOCK_MAX; }
// 0 = planned; N48DR_BAD_ARG = refused (and *p zeroed). The plan is the ONLY source of what goes on the bus.
static inline uint32_t n48dr_plan_ddc(uint32_t line, uint32_t block, struct n48dr_plan *p)
{
    unsigned i, k = 0;
    if (!p) return N48DR_BAD_ARG;
    for (i = 0; i < sizeof(*p); i++) ((uint8_t *)p)[i] = 0;
    if (!n48dr_ddc_line_ok(line) || !n48dr_ddc_block_ok(block)) return N48DR_BAD_ARG;
    p->offset = (uint8_t)((block * N48DR_EDID_BLOCK) & 0xFFu);
    p->has_seg = block >= 2u ? 1u : 0u;
    p->seg = (uint8_t)(block / 2u);
    if (p->has_seg) {
        p->t[k].addr8 = N48DR_DDC_ADDR_SEG_W; p->t[k].wlen = 1u; p->t[k].wdata = p->seg; k++;
    }
    p->t[k].addr8 = N48DR_DDC_ADDR_EDID_W; p->t[k].wlen = 1u; p->t[k].wdata = p->offset; k++;
    p->t[k].addr8 = N48DR_DDC_ADDR_EDID_R; p->t[k].is_read = 1u; p->t[k].rlen = N48DR_EDID_BLOCK; p->t[k].stop = 1u; k++;
    p->ntxn = (uint8_t)k;
    for (i = 0; i < k; i++) {
        p->fifo[p->fifo_len++] = p->t[i].addr8;
        if (!p->t[i].is_read && p->t[i].wlen) p->fifo[p->fifo_len++] = p->t[i].wdata;
    }
    p->read_index = p->fifo_len;   /* the read data starts after the last address byte (RDNA4FB: 3 for the two-transaction form) */
    return 0u;
}
// DC_I2C_TRANSACTIONn for a planned transaction (RDNA4FB: STOP_ON_NACK | START [| READ | STOP] | count << 16)
static inline uint32_t n48dr_txn_reg(const struct n48dr_txn *t)
{
    uint32_t v = N48DR_TXN_STOP_ON_NACK | N48DR_TXN_START;
    uint32_t cnt = t->is_read ? t->rlen : t->wlen;
    if (t->is_read) v |= N48DR_TXN_READ;
    if (t->stop) v |= N48DR_TXN_STOP;
    return v | (cnt << N48DR_TXN_COUNT_SHIFT);
}
// the SW_STATUS poll bound for a plan, in polls (one per N48DR_DDC_POLL_STEP_US): 20 ms per transaction
static inline uint32_t n48dr_poll_bound(const struct n48dr_plan *p) { return (uint32_t)p->ntxn * N48DR_DDC_POLL_PER_TXN; }

// The arbitration verdict BEFORE anything is written. arb is DC_I2C_ARBITRATION as read. Returns 0 = free to proceed, else the refusing N48DR_ status (no register may be written then).
static inline uint32_t n48dr_arb_entry(uint32_t arb)
{
    uint32_t owner;
    if (arb == 0xFFFFFFFFu) return N48DR_ENGINE_DEAD;
    owner = (arb & N48DR_ARB_RW_STATUS_MASK) >> N48DR_ARB_RW_STATUS_SHIFT;
    if (owner == 2u) return N48DR_HW_OWNED;
    if (owner != 0u) return N48DR_SW_BUSY;       /* 1 = software already holds it (never ours at entry), 3 = reserved */
    return 0u;
}
// After our request: granted only when software owns the engine.
static inline int n48dr_arb_granted(uint32_t arb) { return arb != 0xFFFFFFFFu && ((arb & N48DR_ARB_RW_STATUS_MASK) >> N48DR_ARB_RW_STATUS_SHIFT) == 1u; }
// SW_STATUS verdict: a failure bit wins over DONE (RDNA4FB's order); 0 = done; -1 = still running (keep polling).
static inline int n48dr_sw_status_verdict(uint32_t sts)
{
    if (sts & N48DR_ST_NACK) return (int)N48DR_NACK;
    if (sts & N48DR_ST_TIMEOUT) return (int)N48DR_TIMEOUT;
    if (sts & N48DR_ST_ABORTED) return (int)N48DR_ABORTED;
    if (sts & N48DR_ST_OVERFLOW) return (int)N48DR_OVERFLOW;
    if (sts & N48DR_ST_DONE) return 0;
    return -1;
}
// EDID base-block checksum: the 128 bytes sum to 0 mod 256. Returns the sum mod 256 (0 = OK).
static inline uint32_t n48dr_edid_sum(const uint8_t *b)
{
    uint32_t s = 0, i;
    for (i = 0; i < N48DR_EDID_BLOCK; i++) s += b[i];
    return s & 0xFFu;
}
static inline int n48dr_edid_header_ok(const uint8_t *b)
{
    return b[0] == 0u && b[1] == 0xFFu && b[2] == 0xFFu && b[3] == 0xFFu && b[4] == 0xFFu && b[5] == 0xFFu && b[6] == 0xFFu && b[7] == 0u;
}

// ---- ddcread argument: block | line << 8 | page << 16 (page 0 = do the bus read and return bytes 0..87; page 1 = the stored bytes 88..127, no bus access) ------------------------------------------
#define N48DR_DDC_PAGE0_BYTES 88u
static inline int n48dr_ddc_arg_ok(uint64_t arg)
{
    return (arg >> 24) == 0u && n48dr_ddc_line_ok((uint32_t)((arg >> 8) & 0xFFu)) && n48dr_ddc_block_ok((uint32_t)(arg & 0xFFu)) && ((arg >> 16) & 0xFFu) <= 1u;
}
static inline uint64_t n48dr_ddc_arg(uint32_t line, uint32_t block, uint32_t page) { return (uint64_t)block | ((uint64_t)line << 8) | ((uint64_t)page << 16); }

// ---- scdcread (0.0.633) -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The HDMI 2.0 Status and Control Data Channel lives at I2C slave 0x54 (Linux dc_hdmi_types.h HDMI_SCDC_ADDRESS). The HF-VSDB of the monitor B SINK-C' EDID says SCDC is present (notes/logs/runs/m1-622/ddc.txt line 3 block 1).
// scdcread contract: lines 2 and 3 ONLY (as ddcread); offsets 0x00..0x5F ONLY with off + len <= 0x60 (the sink version, TMDS config, scrambler status, status flags and the character error counters live there;
// nothing above 0x5F is ever addressed); 1..16 bytes. The ONLY bus write is the 1-byte register offset to address 0x54 (bus byte 0xA8); then a repeated-start read from 0xA9 (STOP after it). Nothing is ever
// written INTO the sink: the SCDC write registers (0x02, 0x10, 0x20, 0x30, ...) cannot be reached because a write transaction carries the offset byte and nothing else.
#define N48DR_SCDC_ADDR_W 0xA8u        /* 0x54 << 1 | write */
#define N48DR_SCDC_ADDR_R 0xA9u
#define N48DR_SCDC_OFF_MAX 0x5Fu
#define N48DR_SCDC_END 0x60u           /* off + len must not pass this */
#define N48DR_SCDC_LEN_MAX 16u
static inline int n48dr_scdc_range_ok(uint32_t off, uint32_t len)
{
    return off <= N48DR_SCDC_OFF_MAX && len >= 1u && len <= N48DR_SCDC_LEN_MAX && off + len <= N48DR_SCDC_END;
}
// 0 = planned; N48DR_BAD_ARG = refused (and *p zeroed). Two transactions: write the offset, read len bytes. Same plan structure as the EDID read, so the ONE engine sequence runs it.
static inline uint32_t n48dr_plan_scdc(uint32_t line, uint32_t off, uint32_t len, struct n48dr_plan *p)
{
    unsigned i, k = 0;
    if (!p) return N48DR_BAD_ARG;
    for (i = 0; i < sizeof(*p); i++) ((uint8_t *)p)[i] = 0;
    if (!n48dr_ddc_line_ok(line) || !n48dr_scdc_range_ok(off, len)) return N48DR_BAD_ARG;
    p->offset = (uint8_t)off;
    p->t[k].addr8 = N48DR_SCDC_ADDR_W; p->t[k].wlen = 1u; p->t[k].wdata = (uint8_t)off; k++;
    p->t[k].addr8 = N48DR_SCDC_ADDR_R; p->t[k].is_read = 1u; p->t[k].rlen = (uint16_t)len; p->t[k].stop = 1u; k++;
    p->ntxn = (uint8_t)k;
    for (i = 0; i < k; i++) {
        p->fifo[p->fifo_len++] = p->t[i].addr8;
        if (!p->t[i].is_read && p->t[i].wlen) p->fifo[p->fifo_len++] = p->t[i].wdata;
    }
    p->read_index = p->fifo_len;   /* 3: the read data starts after the write address, the offset byte and the read address */
    return 0u;
}
// argument: line | off << 8 | len << 16 (bits 24..63 zero)
static inline int n48dr_scdc_arg_ok(uint64_t arg)
{
    return (arg >> 24) == 0u && n48dr_ddc_line_ok((uint32_t)(arg & 0xFFu)) && n48dr_scdc_range_ok((uint32_t)((arg >> 8) & 0xFFu), (uint32_t)((arg >> 16) & 0xFFu));
}
static inline uint64_t n48dr_scdc_arg(uint32_t line, uint32_t off, uint32_t len) { return (uint64_t)line | ((uint64_t)off << 8) | ((uint64_t)len << 16); }
//   v[0] = status | len << 8 | released << 16 | seq << 24 | arb_after_release << 32     v[1] = arbitration as last read | sw_status << 32     v[2] = bytes 0..7 (little-endian)     v[3] = bytes 8..15
static inline void n48dr_scdc_fill(uint64_t *v, uint32_t status, uint32_t len, uint32_t released, uint32_t seq, uint32_t arb_after, uint32_t arb, uint32_t sw, const uint8_t *data /* len bytes or NULL */)
{
    unsigned i;
    for (i = 0; i < 13u; i++) v[i] = 0u;
    v[0] = (uint64_t)(status & 0xFFu) | ((uint64_t)(len & 0xFFu) << 8) | ((uint64_t)(released & 1u) << 16) | ((uint64_t)(seq & 0xFFu) << 24) | ((uint64_t)arb_after << 32);
    v[1] = (uint64_t)arb | ((uint64_t)sw << 32);
    if (!data) return;
    for (i = 0; i < len && i < N48DR_SCDC_LEN_MAX; i++) v[2u + i / 8u] |= (uint64_t)data[i] << (8u * (i & 7u));
}
static inline void n48dr_scdc_extract(const uint64_t *v, uint8_t *out16)
{
    unsigned i;
    for (i = 0; i < N48DR_SCDC_LEN_MAX; i++) out16[i] = (uint8_t)(v[2u + i / 8u] >> (8u * (i & 7u)));
}
// SCDC register decode (HDMI 2.0 section 10.4; Linux dc_hdmi_types.h HDMI_SCDC_* offsets and union hdmi_scdc_status_flags_data bit order)
#define N48DR_SCDC_SINK_VERSION 0x01u
#define N48DR_SCDC_TMDS_CONFIG 0x20u
#define N48DR_SCDC_SCRAMBLER_STATUS 0x21u
#define N48DR_SCDC_STATUS_FLAGS 0x40u
#define N48DR_SCDC_ERR_DETECT 0x50u          /* 0x50/0x51 channel 0, 0x52/0x53 channel 1, 0x54/0x55 channel 2 (low, high), 0x56 checksum */
static inline int n48dr_scdc_clock_detected(uint8_t b) { return b & 1u; }
static inline int n48dr_scdc_ch_locked(uint8_t b, unsigned ch /* 0..2 */) { return (b >> (1u + ch)) & 1u; }
// the sink LOCKS onto our TMDS: the clock is detected AND all three channels are locked (Status_Flags_0 bits 0..3 all set)
static inline int n48dr_scdc_locked(uint8_t b) { return n48dr_scdc_clock_detected(b) && n48dr_scdc_ch_locked(b, 0) && n48dr_scdc_ch_locked(b, 1) && n48dr_scdc_ch_locked(b, 2); }
static inline int n48dr_scdc_scrambling_enabled(uint8_t b) { return b & 1u; }          /* TMDS_CONFIG bit 0 */
static inline int n48dr_scdc_ratio_by_40(uint8_t b) { return (b >> 1) & 1u; }          /* TMDS_CONFIG bit 1: TMDS bit clock ratio 1/40 */
static inline int n48dr_scdc_scrambling_status(uint8_t b) { return b & 1u; }           /* SCRAMBLER_STATUS bit 0 */
// 15-bit character error count of one channel from its (low, high) bytes; *valid = the high byte's bit 7 (the sink has a valid count)
static inline uint32_t n48dr_scdc_err_count(uint8_t lo, uint8_t hi, int *valid) { if (valid) *valid = (hi >> 7) & 1u; return (uint32_t)lo | ((uint32_t)(hi & 0x7Fu) << 8); }

// ---- DMUB: ring decode ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
#define N48DR_DMUB_CMD_SIZE 64u
#define N48DR_DMUB_MAX_DECODE 25u                 /* the brief: up to 25 commands between ring start and WPTR */
#define N48DR_DMUB_PAYLOAD_SHOWN 32u              /* first 32 payload bytes of each command */
#define N48DR_DMUB_PAGE_SCRATCH 0x80u             /* dmubring page: the SCRATCH bank */
#define N48DR_DMUB_SCRATCH_FIRST 0x01e3u          /* regDMCUB_SCRATCH0 .. SCRATCH18 = 0x01e3 .. 0x01f5, BASE_IDX 2 */
#define N48DR_DMUB_SCRATCH_COUNT 19u
// the type / sub-type numbers dcn41_dmub.h defines, and nothing else (the test pins them to that header)
#define N48DR_DMUB_TYPE_QUERY_FEATURE_CAPS 6u
#define N48DR_DMUB_TYPE_VBIOS 128u
#define N48DR_DMUB_VBIOS_DIGX_ENCODER_CONTROL 0u
#define N48DR_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL 1u
#define N48DR_DMUB_VBIOS_SET_PIXEL_CLOCK 2u
#define N48DR_DMUB_VBIOS_ENABLE_DISP_POWER_GATING 3u

struct n48dr_dmub_hdr {
    uint8_t type, sub_type, ret_status, multi_cmd_pending, is_reg_based, payload_bytes;
    uint8_t reserved_bits_clear;   /* bits 19..23 and 30..31 are zero */
};
// struct dmub_cmd_header: type[7:0] | sub_type[15:8] | ret_status[16] | multi_cmd_pending[17] | is_reg_based[18] | reserved[23:19] | payload_bytes[29:24] | reserved[31:30]
static inline struct n48dr_dmub_hdr n48dr_dmub_decode(uint32_t h)
{
    struct n48dr_dmub_hdr d;
    d.type = (uint8_t)(h & 0xFFu);
    d.sub_type = (uint8_t)((h >> 8) & 0xFFu);
    d.ret_status = (uint8_t)((h >> 16) & 1u);
    d.multi_cmd_pending = (uint8_t)((h >> 17) & 1u);
    d.is_reg_based = (uint8_t)((h >> 18) & 1u);
    d.payload_bytes = (uint8_t)((h >> 24) & 0x3Fu);
    d.reserved_bits_clear = ((h & 0x00F80000u) == 0u && (h & 0xC0000000u) == 0u) ? 1u : 0u;
    return d;
}
// Names only for what dcn41_dmub.h defines; everything else is "unknown" (RDNA4FB recorded the GOP speaking sub-types 6/10/12/16, which are NOT in that header: they print as unknown).
static inline const char *n48dr_dmub_type_name(uint32_t type)
{
    return type == N48DR_DMUB_TYPE_QUERY_FEATURE_CAPS ? "QUERY_FEATURE_CAPS" : type == N48DR_DMUB_TYPE_VBIOS ? "VBIOS" : "unknown";
}
static inline const char *n48dr_dmub_subtype_name(uint32_t type, uint32_t sub)
{
    if (type != N48DR_DMUB_TYPE_VBIOS) return sub == 0u ? "-" : "unknown";
    return sub == N48DR_DMUB_VBIOS_DIGX_ENCODER_CONTROL ? "DIGX_ENCODER_CONTROL" : sub == N48DR_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL ? "DIG1_TRANSMITTER_CONTROL" :
           sub == N48DR_DMUB_VBIOS_SET_PIXEL_CLOCK ? "SET_PIXEL_CLOCK" : sub == N48DR_DMUB_VBIOS_ENABLE_DISP_POWER_GATING ? "ENABLE_DISP_POWER_GATING" : "unknown";
}
// How many commands the ring holds between its start and WPTR, capped at 25. 0 and *sane = 0 for a ring that is not a plausible one (size 0 / not a multiple of 64 / above 64 KiB, WPTR past the size or not a multiple of 64).
static inline uint32_t n48dr_ring_count(uint32_t size, uint32_t wptr, uint32_t *sane)
{
    uint32_t n;
    if (sane) *sane = 0u;
    if (size == 0u || (size % N48DR_DMUB_CMD_SIZE) != 0u || size > 0x10000u || wptr > size || (wptr % N48DR_DMUB_CMD_SIZE) != 0u) return 0u;
    if (sane) *sane = 1u;
    n = wptr / N48DR_DMUB_CMD_SIZE;
    return n > N48DR_DMUB_MAX_DECODE ? N48DR_DMUB_MAX_DECODE : n;
}
// dmubring page p (1..25) decodes command p - 1; 0 = the summary; 0x80 = the SCRATCH bank.
static inline int n48dr_dmub_arg_ok(uint64_t arg) { return arg <= N48DR_DMUB_MAX_DECODE || arg == N48DR_DMUB_PAGE_SCRATCH; }
// Can the ring (vram offset, size) be read through the BAR0 aperture the kext maps (bar0Size bytes from VRAM offset 0)? The overflow-safe test the glue uses.
static inline int n48dr_ring_in_bar0(uint64_t ring_vram_off, uint64_t ring_bytes, uint64_t bar0_size)
{
    return ring_bytes != 0u && ring_vram_off <= bar0_size && ring_bytes <= bar0_size - ring_vram_off;
}


// ---- the scalar layout a ddcread call returns (out[3 + i] = v[i], 13 scalars; the bytes cannot all fit: 2 pages) -------------------------------------------------------------------------------
//   v[0] = status | page << 8 | checksum_ok << 16 | header_ok << 17 | released << 18 | seq << 24 | arb_after_release << 32
//   v[1] = arbitration as last read during the transaction | sw_status << 32
//   page 0: v[2..12] = EDID bytes 0..87 (little-endian), page 1: v[2..6] = bytes 88..127
static inline void n48dr_ddc_fill(uint64_t *v, uint32_t status, uint32_t page, uint32_t csum_ok, uint32_t hdr_ok, uint32_t released, uint32_t seq,
                                  uint32_t arb_after, uint32_t arb, uint32_t sw, const uint8_t *data /* 128 bytes or NULL */)
{
    unsigned i, j, first, nq;
    for (i = 0; i < 13u; i++) v[i] = 0u;
    v[0] = (uint64_t)(status & 0xFFu) | ((uint64_t)(page & 0xFFu) << 8) | ((uint64_t)(csum_ok & 1u) << 16) | ((uint64_t)(hdr_ok & 1u) << 17) | ((uint64_t)(released & 1u) << 18) |
           ((uint64_t)(seq & 0xFFu) << 24) | ((uint64_t)arb_after << 32);
    v[1] = (uint64_t)arb | ((uint64_t)sw << 32);
    if (!data) return;
    first = page == 0u ? 0u : N48DR_DDC_PAGE0_BYTES;
    nq = page == 0u ? N48DR_DDC_PAGE0_BYTES / 8u : (N48DR_EDID_BLOCK - N48DR_DDC_PAGE0_BYTES) / 8u;
    for (i = 0; i < nq; i++)
        for (j = 0; j < 8u; j++) v[2u + i] |= (uint64_t)data[first + i * 8u + j] << (8u * j);
}
// the bytes of one page out of v (CLI side); returns how many bytes were written to out[first..]
static inline unsigned n48dr_ddc_extract(const uint64_t *v, uint32_t page, uint8_t *out128)
{
    unsigned i, j, first = page == 0u ? 0u : N48DR_DDC_PAGE0_BYTES, nq = page == 0u ? N48DR_DDC_PAGE0_BYTES / 8u : (N48DR_EDID_BLOCK - N48DR_DDC_PAGE0_BYTES) / 8u;
    for (i = 0; i < nq; i++)
        for (j = 0; j < 8u; j++) out128[first + i * 8u + j] = (uint8_t)(v[2u + i] >> (8u * j));
    return nq * 8u;
}

// ---- the scalar layout of dmubring (page 0 = summary) -------------------------------------------------------------------------------------------------------------------------------------------
struct n48dr_dmub_summary {
    uint32_t status, verdict, ring_map, enabled, soft_reset, dal_fw, mailbox_rdy, reachable, ncmd, ring_sane;
    uint32_t cntl, cntl2, sec_cntl, scratch0, scratch7, scratch14, scratch15, fault_addr;
    uint32_t inbox_base, inbox_size, inbox_wptr, inbox_rptr, region4_off, region4_off_hi;
    uint64_t ring_addr, fb_base_mc, ring_vram_off, bar0_size;
};
//   v[0] = status | verdict << 8 | ring_map << 16 | flags << 24 (bit0 enabled, 1 soft_reset, 2 dal_fw, 3 mailbox_rdy, 4 ring reachable through BAR0, 5 ring sane) | ncmd << 32
//   v[1] = cntl | cntl2 << 32          v[2] = sec_cntl | scratch0 << 32     v[3] = inbox_base | inbox_size << 32      v[4] = wptr | rptr << 32
//   v[5] = region4_off | region4_off_hi << 32      v[6] = ring_addr (GPU address)    v[7] = fb_base_mc    v[8] = ring_vram_off (all ones = unknown)    v[9] = bar0_size
//   v[10] = scratch7 | scratch14 << 32      v[11] = scratch15 | fault_addr << 32
static inline void n48dr_dmub_summary_pack(const struct n48dr_dmub_summary *s, uint64_t *v)
{
    unsigned i;
    uint32_t flags = (s->enabled & 1u) | ((s->soft_reset & 1u) << 1) | ((s->dal_fw & 1u) << 2) | ((s->mailbox_rdy & 1u) << 3) | ((s->reachable & 1u) << 4) | ((s->ring_sane & 1u) << 5);
    for (i = 0; i < 13u; i++) v[i] = 0u;
    v[0] = (uint64_t)(s->status & 0xFFu) | ((uint64_t)(s->verdict & 0xFFu) << 8) | ((uint64_t)(s->ring_map & 0xFFu) << 16) | ((uint64_t)flags << 24) | ((uint64_t)s->ncmd << 32);
    v[1] = (uint64_t)s->cntl | ((uint64_t)s->cntl2 << 32);
    v[2] = (uint64_t)s->sec_cntl | ((uint64_t)s->scratch0 << 32);
    v[3] = (uint64_t)s->inbox_base | ((uint64_t)s->inbox_size << 32);
    v[4] = (uint64_t)s->inbox_wptr | ((uint64_t)s->inbox_rptr << 32);
    v[5] = (uint64_t)s->region4_off | ((uint64_t)s->region4_off_hi << 32);
    v[6] = s->ring_addr; v[7] = s->fb_base_mc; v[8] = s->ring_vram_off; v[9] = s->bar0_size;
    v[10] = (uint64_t)s->scratch7 | ((uint64_t)s->scratch14 << 32);
    v[11] = (uint64_t)s->scratch15 | ((uint64_t)s->fault_addr << 32);
}
// dmubring page p = 1..25: v[0] = status | 1 << 8 (valid) | idx << 16, v[1] = header | ncmd << 32, v[2..5] = the first 32 payload bytes (little-endian)
static inline void n48dr_dmub_cmd_pack(uint64_t *v, uint32_t status, uint32_t idx, uint32_t header, uint32_t ncmd, const uint32_t *payload8 /* 8 dwords or NULL */)
{
    unsigned i;
    for (i = 0; i < 13u; i++) v[i] = 0u;
    v[0] = (uint64_t)(status & 0xFFu) | ((payload8 ? 1ull : 0ull) << 8) | ((uint64_t)idx << 16);
    v[1] = (uint64_t)header | ((uint64_t)ncmd << 32);
    if (payload8)
        for (i = 0; i < 4u; i++) v[2u + i] = (uint64_t)payload8[2u * i] | ((uint64_t)payload8[2u * i + 1u] << 32);
}
// dmubring page 0x80: v[0] = status | count << 8, v[1 + k] = SCRATCH(2k) | SCRATCH(2k + 1) << 32 (19 registers = 10 scalars)
static inline void n48dr_pack_regs(uint64_t *v, unsigned firstSlot, const uint32_t *vals, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) v[firstSlot + i / 2u] |= (uint64_t)vals[i] << (32u * (i & 1u));
}

// ---- region4read (0.0.624, multi-monitor stage M1.5) --------------------------------------------------------------------------------------------------------------------------------------
// The DMUB's REGION4 window is the VRAM the firmware's ring and display service live in (an internal design note, "M1 result: the DMUB dialect"): the ring at +0x0..0x2000, the VBIOS ring state
// +0x3100..0x3124, the mode block +0x4c00.., the four display contexts +0x6000..0x7fff. The verb reads ONLY inside the first 64 KiB of it, at most 64 dwords per call, READ-ONLY.
// Argument (one scalar): offset | dwords << 32 | page << 40 (offset a 4-byte aligned window offset, dwords 1..64, nothing above bit 47). A call returns 22 dwords per page, so page 0 does the VRAM read
// (and stores the dwords) and page 1 / 2 return the stored rest, with no VRAM access, exactly as ddcread's page 1 does.
#define N48DR_R4_WINDOW 0x10000u        /* the 64 KiB window the verb may read */
#define N48DR_R4_MAX_DWORDS 64u
#define N48DR_R4_PER_PAGE 22u           /* 11 scalars of two dwords: v[2..12] */
#define N48DR_R4_PAGES 3u               /* ceil(64 / 22) */
// offset 4-byte aligned, 1..64 dwords, offset + dwords * 4 <= 0x10000 (the end may touch the window end, never pass it); written so no sum can wrap
static inline int n48dr_r4_range_ok(uint64_t off, uint64_t dwords)
{
    return dwords >= 1u && dwords <= N48DR_R4_MAX_DWORDS && (off & 3u) == 0u && off <= N48DR_R4_WINDOW && dwords * 4u <= (uint64_t)N48DR_R4_WINDOW - off;
}
static inline uint32_t n48dr_r4_pages_for(uint32_t dwords) { return (dwords + N48DR_R4_PER_PAGE - 1u) / N48DR_R4_PER_PAGE; }
static inline int n48dr_r4_arg_ok(uint64_t arg)
{
    const uint64_t off = arg & 0xFFFFFFFFull, dwords = (arg >> 32) & 0xFFu, page = (arg >> 40) & 0xFFu;
    return (arg >> 48) == 0u && n48dr_r4_range_ok(off, dwords) && page < n48dr_r4_pages_for((uint32_t)dwords);
}
static inline uint64_t n48dr_r4_arg(uint32_t off, uint32_t dwords, uint32_t page) { return (uint64_t)off | ((uint64_t)dwords << 32) | ((uint64_t)page << 40); }
// The window base, from the DMCUB registers as dcn41_dmub_probe reads them: GPU address = REGION4_OFFSET_HIGH << 32 | REGION4_OFFSET, VRAM offset = that minus the FB base (DCN_VM_FB_LOCATION_BASE << 24).
// Refused (N48DR_REGION4_BAD, *vram_off = all ones) unless REGION4 is enabled, the address is nonzero and not below the FB base, the VRAM offset is 4 KiB aligned and the whole 64 KiB window lies inside VRAM.
static inline uint32_t n48dr_region4_base(uint32_t off_lo, uint32_t off_hi, uint32_t enabled, uint64_t fb_base_mc, uint64_t vram_size, uint64_t *vram_off)
{
    const uint64_t gpu = ((uint64_t)off_hi << 32) | off_lo;
    uint64_t base;
    if (vram_off) *vram_off = ~0ull;
    if (!enabled || gpu == 0u || gpu < fb_base_mc) return N48DR_REGION4_BAD;
    base = gpu - fb_base_mc;
    if ((base & 0xFFFu) != 0u) return N48DR_REGION4_BAD;
    if (base > vram_size || (uint64_t)N48DR_R4_WINDOW > vram_size - base) return N48DR_REGION4_BAD;
    if (vram_off) *vram_off = base;
    return N48DR_OK;
}
//   v[0] = status | page << 8 | count << 16 | seq << 24 | dwords << 32 | offset << 40 (16 bits)    v[1] = window base (VRAM offset; on a REGION4_BAD refusal the GPU address computed)
//   v[2 + i / 2] = dword (page * 22 + i) of the read, low half first, i < count.   On a REGION4_BAD refusal v[2] = fb_base_mc, v[3] = vram size, v[4] = enabled.
static inline void n48dr_r4_fill(uint64_t *v, uint32_t status, uint32_t page, uint32_t seq, uint32_t off, uint32_t dwords, uint64_t base, const uint32_t *d /* the stored dwords or NULL */)
{
    unsigned i, first = page * N48DR_R4_PER_PAGE, n = 0;
    for (i = 0; i < 13u; i++) v[i] = 0u;
    if (d && first < dwords) { n = dwords - first; if (n > N48DR_R4_PER_PAGE) n = N48DR_R4_PER_PAGE; }
    v[0] = (uint64_t)(status & 0xFFu) | ((uint64_t)(page & 0xFFu) << 8) | ((uint64_t)n << 16) | ((uint64_t)(seq & 0xFFu) << 24) | ((uint64_t)(dwords & 0xFFu) << 32) | ((uint64_t)(off & 0xFFFFu) << 40);
    v[1] = base;
    for (i = 0; i < n; i++) v[2u + i / 2u] |= (uint64_t)d[first + i] << (32u * (i & 1u));
}
// CLI side: copy this page's dwords into dst[page * 22 ..]; returns how many
static inline unsigned n48dr_r4_extract(const uint64_t *v, uint32_t page, uint32_t *dst)
{
    unsigned i, n = (unsigned)((v[0] >> 16) & 0xFFu);
    if (n > N48DR_R4_PER_PAGE) n = N48DR_R4_PER_PAGE;
    for (i = 0; i < n; i++) dst[page * N48DR_R4_PER_PAGE + i] = (uint32_t)(v[2u + i / 2u] >> (32u * (i & 1u)));
    return n;
}

// ---- the census table ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// 40 registers, 24 per page (two 32-bit values per scalar). Names are the Linux register names; the offsets are dcn_4_1_0_offset.h's (checked by the host test).
struct n48dr_reg { const char *name; uint8_t base_idx; uint16_t off; };
#define N48DR_CENSUS_COUNT 40u
#define N48DR_CENSUS_PER_PAGE 24u
#define N48DR_CENSUS_PAGES 2u
static const struct n48dr_reg n48dr_census[N48DR_CENSUS_COUNT] = {
    { "DOMAIN0_PG_STATUS", 2, 0x0081 }, { "DOMAIN1_PG_STATUS", 2, 0x0083 }, { "DOMAIN2_PG_STATUS", 2, 0x0085 }, { "DOMAIN3_PG_STATUS", 2, 0x0087 },
    { "DIG0_DIG_BE_EN_CNTL", 2, 0x20bd }, { "DIG1_DIG_BE_EN_CNTL", 2, 0x21e1 }, { "DIG2_DIG_BE_EN_CNTL", 2, 0x2305 }, { "DIG3_DIG_BE_EN_CNTL", 2, 0x2429 },
    { "DIG0_DIG_FE_CNTL", 2, 0x2093 }, { "DIG1_DIG_FE_CNTL", 2, 0x21b7 }, { "DIG2_DIG_FE_CNTL", 2, 0x22db }, { "DIG3_DIG_FE_CNTL", 2, 0x23ff },
    { "OTG0_OTG_CONTROL", 2, 0x1b43 }, { "OTG1_OTG_CONTROL", 2, 0x1bc3 }, { "OTG2_OTG_CONTROL", 2, 0x1c43 }, { "OTG3_OTG_CONTROL", 2, 0x1cc3 },
    { "OTG0_OTG_STATUS_FRAME_COUNT", 2, 0x1b4d }, { "OTG1_OTG_STATUS_FRAME_COUNT", 2, 0x1bcd }, { "OTG2_OTG_STATUS_FRAME_COUNT", 2, 0x1c4d }, { "OTG3_OTG_STATUS_FRAME_COUNT", 2, 0x1ccd },
    { "DC_GPIO_HPD_Y", 2, 0x28f7 },
    { "OTG0_PIXEL_RATE_CNTL", 1, 0x0080 }, { "OTG1_PIXEL_RATE_CNTL", 1, 0x0084 }, { "OTG2_PIXEL_RATE_CNTL", 1, 0x0088 },
    /* ---- page 1 starts here (index 24) ---- */
    { "OTG3_PIXEL_RATE_CNTL", 1, 0x008c },
    { "PHYPLLA_PIXCLK_RESYNC_CNTL", 1, 0x0040 }, { "PHYPLLB_PIXCLK_RESYNC_CNTL", 1, 0x0041 }, { "PHYPLLC_PIXCLK_RESYNC_CNTL", 1, 0x0042 }, { "PHYPLLD_PIXCLK_RESYNC_CNTL", 1, 0x0043 },
    { "PHYPLLE_PIXCLK_RESYNC_CNTL", 1, 0x004c }, { "PHYPLLF_PIXCLK_RESYNC_CNTL", 1, 0x007e }, { "PHYPLLG_PIXCLK_RESYNC_CNTL", 1, 0x005f },
    { "DP_DTO0_PHASE", 1, 0x0081 }, { "DP_DTO0_MODULO", 1, 0x0082 }, { "DP_DTO1_PHASE", 1, 0x0085 }, { "DP_DTO1_MODULO", 1, 0x0086 },
    { "DP_DTO2_PHASE", 1, 0x0089 }, { "DP_DTO2_MODULO", 1, 0x008a }, { "DP_DTO3_PHASE", 1, 0x008d }, { "DP_DTO3_MODULO", 1, 0x008e },
};

// ---- the M4a pages (0.0.628, multi-monitor stage M4a; an internal design note "M4 design") -------------------------------------------------------------------------------------------
// READ-ONLY census of the clock and PHY state HDMI bring-up depends on, pages 2..7: the DCCG clock-source registers (PIXEL_RATE_CNTL incl. the OTGn_DIO_ERROR_COUNT field [27:16] of
// OTGn_PIXEL_RATE_CNTL - there is no separate DIO_ERROR_COUNT register in dcn_4_1_0_offset.h, so the page that reads PIXEL_RATE_CNTL reads it - PHYPLL_PIXEL_RATE_CNTL, OTG_PIXEL_RATE_DIV, DP_DTO0..3 PHASE / MODULO,
// SYMCLKA..E_CLOCK_ENABLE, PHYA..ESYMCLK_CLOCK_CNTL), DIG0..3 FE / BE (CNTL, CLK_CNTL, FE_EN_CNTL, STREAM_MAPPER_CONTROL) and the whole RDPCSTX1 / RDPCSTX2 PHY blocks (PHY_CNTL0..17, FUSE, SRAM_CNTL, CNTL, CLOCK_CNTL, ...)
// except RDPCS_TX_CR_DATA (the indirect PHY window's data port: a read there might be a read trigger - SUSPECTED - so it is the one register of the blocks that is NOT read; RDPCS_TX_CR_ADDR is).
// Every offset and BASE_IDX is dcn_4_1_0_offset.h's (checked by tests/native_dispread_test.cpp against re/linux-dc). BASE_IDX 1 is resolved by the READ-ONLY address twin (reads only; the write layer still refuses it).
#define N48DR_CENSUS2_COUNT 135u
#define N48DR_CENSUS2_PAGES 6u
#define N48DR_CENSUS3_PAGE8_COUNT 23u   /* 0.0.630 (M4c): page 8 holds the 23 substitute registers, page 9 the two OTG_UPDATE_LOCKs */
#define N48DR_CENSUS3_PAGE9_COUNT 2u
#define N48DR_CENSUS3_COUNT (N48DR_CENSUS3_PAGE8_COUNT + N48DR_CENSUS3_PAGE9_COUNT)
/* 0.0.634 (M4a, the plane census): page 10 = the DP watch (9 registers, census4), pages 11..42 = the plane registers (762, census5, 24 per page, the last page short) */
#define N48DR_CENSUS4_COUNT 9u
#define N48DR_CENSUS5_COUNT 762u
#define N48DR_CENSUS5_PAGES 32u
#define N48DR_CENSUS6_COUNT 6u   /* 0.0.635 (M4c): page 43 = the DP watch's six more reads (census6): rows 10..14 of n48d2_dpx_regs and the HUBP0 primary HIGH dword */
/* 0.0.654 (the monitor A census): pages 44..59 = the instance-1 twins of the pipe-2 rows plus single registers (census7, READS only; `accel dispcensus mona`) */
#define N48DR_CENSUS7_COUNT 361u
#define N48DR_CENSUS7_PAGES 16u
#define N48DR_CENSUS7_FIRST_PAGE 44u
#define N48DR_CENSUS_ALL_PAGES (N48DR_CENSUS_PAGES + N48DR_CENSUS2_PAGES + 2u + 1u + N48DR_CENSUS5_PAGES + 1u + N48DR_CENSUS7_PAGES)
#define N48DR_CENSUS4_PAGE (N48DR_CENSUS_PAGES + N48DR_CENSUS2_PAGES + 2u)   /* 10 */
#define N48DR_CENSUS5_FIRST_PAGE (N48DR_CENSUS4_PAGE + 1u)                    /* 11 */
#define N48DR_CENSUS6_PAGE (N48DR_CENSUS5_FIRST_PAGE + N48DR_CENSUS5_PAGES)    /* 43 */
static const struct n48dr_reg n48dr_census2[N48DR_CENSUS2_COUNT] = {
    /* ---- page 2 starts here (index 0) ---- */
    { "OTG0_PIXEL_RATE_CNTL", 1, 0x0080 }, { "OTG1_PIXEL_RATE_CNTL", 1, 0x0084 }, { "OTG2_PIXEL_RATE_CNTL", 1, 0x0088 },
    { "OTG3_PIXEL_RATE_CNTL", 1, 0x008c }, { "OTG0_PHYPLL_PIXEL_RATE_CNTL", 1, 0x0083 }, { "OTG1_PHYPLL_PIXEL_RATE_CNTL", 1, 0x0087 },
    { "OTG2_PHYPLL_PIXEL_RATE_CNTL", 1, 0x008b }, { "OTG3_PHYPLL_PIXEL_RATE_CNTL", 1, 0x008f }, { "OTG_PIXEL_RATE_DIV", 1, 0x006f },
    { "DP_DTO0_PHASE", 1, 0x0081 }, { "DP_DTO0_MODULO", 1, 0x0082 }, { "DP_DTO1_PHASE", 1, 0x0085 },
    { "DP_DTO1_MODULO", 1, 0x0086 }, { "DP_DTO2_PHASE", 1, 0x0089 }, { "DP_DTO2_MODULO", 1, 0x008a },
    { "DP_DTO3_PHASE", 1, 0x008d }, { "DP_DTO3_MODULO", 1, 0x008e }, { "SYMCLKA_CLOCK_ENABLE", 1, 0x00a0 },
    { "SYMCLKB_CLOCK_ENABLE", 1, 0x00a1 }, { "SYMCLKC_CLOCK_ENABLE", 1, 0x00a2 }, { "SYMCLKD_CLOCK_ENABLE", 1, 0x00a3 },
    { "SYMCLKE_CLOCK_ENABLE", 1, 0x00a4 }, { "PHYASYMCLK_CLOCK_CNTL", 2, 0x0052 }, { "PHYBSYMCLK_CLOCK_CNTL", 2, 0x0053 },
    /* ---- page 3 starts here (index 24) ---- */
    { "PHYCSYMCLK_CLOCK_CNTL", 2, 0x0054 }, { "PHYDSYMCLK_CLOCK_CNTL", 2, 0x0055 }, { "PHYESYMCLK_CLOCK_CNTL", 2, 0x0056 },
    { "DIG0_DIG_FE_CNTL", 2, 0x2093 }, { "DIG0_DIG_FE_CLK_CNTL", 2, 0x2094 }, { "DIG0_DIG_FE_EN_CNTL", 2, 0x2095 },
    { "DIG0_DIG_BE_CNTL", 2, 0x20bc }, { "DIG0_DIG_BE_CLK_CNTL", 2, 0x20bb }, { "DIG0_STREAM_MAPPER_CONTROL", 2, 0x1f0d },
    { "DIG1_DIG_FE_CNTL", 2, 0x21b7 }, { "DIG1_DIG_FE_CLK_CNTL", 2, 0x21b8 }, { "DIG1_DIG_FE_EN_CNTL", 2, 0x21b9 },
    { "DIG1_DIG_BE_CNTL", 2, 0x21e0 }, { "DIG1_DIG_BE_CLK_CNTL", 2, 0x21df }, { "DIG1_STREAM_MAPPER_CONTROL", 2, 0x1f0e },
    { "DIG2_DIG_FE_CNTL", 2, 0x22db }, { "DIG2_DIG_FE_CLK_CNTL", 2, 0x22dc }, { "DIG2_DIG_FE_EN_CNTL", 2, 0x22dd },
    { "DIG2_DIG_BE_CNTL", 2, 0x2304 }, { "DIG2_DIG_BE_CLK_CNTL", 2, 0x2303 }, { "DIG2_STREAM_MAPPER_CONTROL", 2, 0x1f0f },
    { "DIG3_DIG_FE_CNTL", 2, 0x23ff }, { "DIG3_DIG_FE_CLK_CNTL", 2, 0x2400 }, { "DIG3_DIG_FE_EN_CNTL", 2, 0x2401 },
    /* ---- page 4 starts here (index 48) ---- */
    { "DIG3_DIG_BE_CNTL", 2, 0x2428 }, { "DIG3_DIG_BE_CLK_CNTL", 2, 0x2427 }, { "DIG3_STREAM_MAPPER_CONTROL", 2, 0x1f10 },
    { "RDPCSTX1_RDPCSTX_CNTL", 2, 0x2a08 }, { "RDPCSTX1_RDPCSTX_CLOCK_CNTL", 2, 0x2a09 }, { "RDPCSTX1_RDPCSTX_INTERRUPT_CONTROL", 2, 0x2a0a },
    { "RDPCSTX1_RDPCS_TX_PLL_UPDATE_DATA", 2, 0x2a0b }, { "RDPCSTX1_RDPCS_TX_CR_ADDR", 2, 0x2a0c }, { "RDPCSTX1_RDPCS_TX_SRAM_CNTL", 2, 0x2a0e },
    { "RDPCSTX1_RDPCSTX_SCRATCH0", 2, 0x2a0f }, { "RDPCSTX1_RDPCSTX_SPARE", 2, 0x2a10 }, { "RDPCSTX1_RDPCSTX_CNTL2", 2, 0x2a11 },
    { "RDPCSTX1_RDPCSTX_DPALT_CNTL_SPARE", 2, 0x2a12 }, { "RDPCSTX1_RDPCSTX_PATTERN_DETECT_CTRL", 2, 0x2a13 }, { "RDPCSTX1_RDPCSTX_CNTL4", 2, 0x2a14 },
    { "RDPCSTX1_RDPCSTX_DEBUG_CONFIG", 2, 0x2a15 }, { "RDPCSTX1_RDPCSTX_PHY_CNTL0", 2, 0x2a18 }, { "RDPCSTX1_RDPCSTX_PHY_CNTL1", 2, 0x2a19 },
    { "RDPCSTX1_RDPCSTX_PHY_CNTL2", 2, 0x2a1a }, { "RDPCSTX1_RDPCSTX_PHY_CNTL3", 2, 0x2a1b }, { "RDPCSTX1_RDPCSTX_PHY_CNTL4", 2, 0x2a1c },
    { "RDPCSTX1_RDPCSTX_PHY_CNTL5", 2, 0x2a1d }, { "RDPCSTX1_RDPCSTX_PHY_CNTL6", 2, 0x2a1e }, { "RDPCSTX1_RDPCSTX_PHY_CNTL7", 2, 0x2a1f },
    /* ---- page 5 starts here (index 72) ---- */
    { "RDPCSTX1_RDPCSTX_PHY_CNTL8", 2, 0x2a20 }, { "RDPCSTX1_RDPCSTX_PHY_CNTL9", 2, 0x2a21 }, { "RDPCSTX1_RDPCSTX_PHY_CNTL10", 2, 0x2a22 },
    { "RDPCSTX1_RDPCSTX_PHY_CNTL11", 2, 0x2a23 }, { "RDPCSTX1_RDPCSTX_PHY_CNTL12", 2, 0x2a24 }, { "RDPCSTX1_RDPCSTX_PHY_CNTL13", 2, 0x2a25 },
    { "RDPCSTX1_RDPCSTX_PHY_CNTL14", 2, 0x2a26 }, { "RDPCSTX1_RDPCSTX_PHY_FUSE0", 2, 0x2a27 }, { "RDPCSTX1_RDPCSTX_PHY_FUSE1", 2, 0x2a28 },
    { "RDPCSTX1_RDPCSTX_PHY_FUSE2", 2, 0x2a29 }, { "RDPCSTX1_RDPCSTX_PHY_FUSE3", 2, 0x2a2a }, { "RDPCSTX1_RDPCSTX_PHY_RX_LD_VAL", 2, 0x2a2b },
    { "RDPCSTX1_RDPCSTX_SCRATCH1", 2, 0x2a2c }, { "RDPCSTX1_RDPCSTX_SCRATCH2", 2, 0x2a2d }, { "RDPCSTX1_RDPCSTX_PHY_CNTL15", 2, 0x2a30 },
    { "RDPCSTX1_RDPCSTX_PHY_CNTL16", 2, 0x2a31 }, { "RDPCSTX1_RDPCSTX_PHY_CNTL17", 2, 0x2a32 }, { "RDPCSTX1_RDPCSTX_DEBUG_CONFIG2", 2, 0x2a33 },
    { "RDPCSTX1_RDPCS_CNTL3", 2, 0x2a34 }, { "RDPCSTX1_RDPCS_TX_PLL_UPDATE_ADDR_OVRRD", 2, 0x2a35 }, { "RDPCSTX1_RDPCS_TX_PLL_UPDATE_DATA_OVRRD", 2, 0x2a36 },
    { "RDPCSTX2_RDPCSTX_CNTL", 2, 0x2ae0 }, { "RDPCSTX2_RDPCSTX_CLOCK_CNTL", 2, 0x2ae1 }, { "RDPCSTX2_RDPCSTX_INTERRUPT_CONTROL", 2, 0x2ae2 },
    /* ---- page 6 starts here (index 96) ---- */
    { "RDPCSTX2_RDPCS_TX_PLL_UPDATE_DATA", 2, 0x2ae3 }, { "RDPCSTX2_RDPCS_TX_CR_ADDR", 2, 0x2ae4 }, { "RDPCSTX2_RDPCS_TX_SRAM_CNTL", 2, 0x2ae6 },
    { "RDPCSTX2_RDPCSTX_SCRATCH0", 2, 0x2ae7 }, { "RDPCSTX2_RDPCSTX_SPARE", 2, 0x2ae8 }, { "RDPCSTX2_RDPCSTX_CNTL2", 2, 0x2ae9 },
    { "RDPCSTX2_RDPCSTX_DPALT_CNTL_SPARE", 2, 0x2aea }, { "RDPCSTX2_RDPCSTX_PATTERN_DETECT_CTRL", 2, 0x2aeb }, { "RDPCSTX2_RDPCSTX_CNTL4", 2, 0x2aec },
    { "RDPCSTX2_RDPCSTX_DEBUG_CONFIG", 2, 0x2aed }, { "RDPCSTX2_RDPCSTX_PHY_CNTL0", 2, 0x2af0 }, { "RDPCSTX2_RDPCSTX_PHY_CNTL1", 2, 0x2af1 },
    { "RDPCSTX2_RDPCSTX_PHY_CNTL2", 2, 0x2af2 }, { "RDPCSTX2_RDPCSTX_PHY_CNTL3", 2, 0x2af3 }, { "RDPCSTX2_RDPCSTX_PHY_CNTL4", 2, 0x2af4 },
    { "RDPCSTX2_RDPCSTX_PHY_CNTL5", 2, 0x2af5 }, { "RDPCSTX2_RDPCSTX_PHY_CNTL6", 2, 0x2af6 }, { "RDPCSTX2_RDPCSTX_PHY_CNTL7", 2, 0x2af7 },
    { "RDPCSTX2_RDPCSTX_PHY_CNTL8", 2, 0x2af8 }, { "RDPCSTX2_RDPCSTX_PHY_CNTL9", 2, 0x2af9 }, { "RDPCSTX2_RDPCSTX_PHY_CNTL10", 2, 0x2afa },
    { "RDPCSTX2_RDPCSTX_PHY_CNTL11", 2, 0x2afb }, { "RDPCSTX2_RDPCSTX_PHY_CNTL12", 2, 0x2afc }, { "RDPCSTX2_RDPCSTX_PHY_CNTL13", 2, 0x2afd },
    /* ---- page 7 starts here (index 120) ---- */
    { "RDPCSTX2_RDPCSTX_PHY_CNTL14", 2, 0x2afe }, { "RDPCSTX2_RDPCSTX_PHY_FUSE0", 2, 0x2aff }, { "RDPCSTX2_RDPCSTX_PHY_FUSE1", 2, 0x2b00 },
    { "RDPCSTX2_RDPCSTX_PHY_FUSE2", 2, 0x2b01 }, { "RDPCSTX2_RDPCSTX_PHY_FUSE3", 2, 0x2b02 }, { "RDPCSTX2_RDPCSTX_PHY_RX_LD_VAL", 2, 0x2b03 },
    { "RDPCSTX2_RDPCSTX_SCRATCH1", 2, 0x2b04 }, { "RDPCSTX2_RDPCSTX_SCRATCH2", 2, 0x2b05 }, { "RDPCSTX2_RDPCSTX_PHY_CNTL15", 2, 0x2b08 },
    { "RDPCSTX2_RDPCSTX_PHY_CNTL16", 2, 0x2b09 }, { "RDPCSTX2_RDPCSTX_PHY_CNTL17", 2, 0x2b0a }, { "RDPCSTX2_RDPCSTX_DEBUG_CONFIG2", 2, 0x2b0b },
    { "RDPCSTX2_RDPCS_CNTL3", 2, 0x2b0c }, { "RDPCSTX2_RDPCS_TX_PLL_UPDATE_ADDR_OVRRD", 2, 0x2b0d }, { "RDPCSTX2_RDPCS_TX_PLL_UPDATE_DATA_OVRRD", 2, 0x2b0e },
};
// ---- the M4c pages (0.0.630, multi-monitor stage M4c; an internal design note "M4b analysis" section 3, "Readable substitutes") ---------------------------------------------------------------
// Pages 8 and 9, READS ONLY: the registers that stand in for the RDPCSTX block (which reads all ones) plus OTG0 / OTG1's clock, status and lock state. Page 8 = entries 0..22, page 9 = entries 23..24.
// Every offset and BASE_IDX is dcn_4_1_0_offset.h's (checked by tests/native_dispread_test.cpp against re/linux-dc).
static const struct n48dr_reg n48dr_census3[N48DR_CENSUS3_COUNT] = {
    /* ---- page 8 starts here (index 0) ---- */
    { "PHYPLLA_PIXCLK_RESYNC_CNTL", 1, 0x0040 }, { "PHYPLLB_PIXCLK_RESYNC_CNTL", 1, 0x0041 }, { "PHYPLLC_PIXCLK_RESYNC_CNTL", 1, 0x0042 }, { "PHYPLLD_PIXCLK_RESYNC_CNTL", 1, 0x0043 },
    { "UNIPHYB_LINK_CNTL", 2, 0x286f }, { "UNIPHYB_CHANNEL_XBAR_CNTL", 2, 0x2870 }, { "UNIPHYC_LINK_CNTL", 2, 0x2871 }, { "UNIPHYC_CHANNEL_XBAR_CNTL", 2, 0x2872 },
    { "HPD2_DC_HPD_INT_STATUS", 2, 0x176a }, { "HPD3_DC_HPD_INT_STATUS", 2, 0x1772 },
    { "DIG1_HDMI_CONTROL", 2, 0x21c2 }, { "DIG2_HDMI_CONTROL", 2, 0x22e6 }, { "DP1_DP_LINK_CNTL", 2, 0x2242 }, { "DP2_DP_LINK_CNTL", 2, 0x2366 },
    { "DIG2_DIG_BE_EN_CNTL", 2, 0x2305 },
    { "OTG0_OTG_CLOCK_CONTROL", 2, 0x1b84 }, { "OTG1_OTG_CLOCK_CONTROL", 2, 0x1c04 },
    { "ODM0_OPTC_INPUT_CLOCK_CONTROL", 2, 0x1ad0 }, { "ODM1_OPTC_INPUT_CLOCK_CONTROL", 2, 0x1ae0 },
    { "OTG0_OTG_STATUS", 2, 0x1b49 }, { "OTG1_OTG_STATUS", 2, 0x1bc9 },
    { "OTG0_OTG_MASTER_UPDATE_LOCK", 2, 0x1b89 }, { "OTG1_OTG_MASTER_UPDATE_LOCK", 2, 0x1c09 },
    /* ---- page 9 starts here (index 23) ---- */
    { "OTG0_OTG_UPDATE_LOCK", 2, 0x1b5b }, { "OTG1_OTG_UPDATE_LOCK", 2, 0x1bdb },
};
// ---- the M4a plane census (0.0.634, multi-monitor stage M4a; an internal design note "Plane design", M4a) ---------------------------------------------------------------------------------------
// READS ONLY, like every table above (the census environment has no write member). Every name, BASE_IDX and offset is dcn_4_1_0_offset.h's (tests/native_dispread_test.cpp re-reads the header for each entry).
// Page 10 (n48dr_census4) is the DP WATCH: the nine registers of the live DP pipe that `disp2` and `dmubsend` compare before / after (dcn/navi48_disp2.h n48d2_dpx_regs; the CLI reads this very page).
static const struct n48dr_reg n48dr_census4[N48DR_CENSUS4_COUNT] = {
    /* ---- page 10 ---- */
    { "MPC_OUT0_MUX", 3, 0x02f2 },
    { "MPCC0_MPCC_TOP_SEL", 3, 0x0000 },
    { "MPCC0_MPCC_BOT_SEL", 3, 0x0001 },
    { "MPCC0_MPCC_OPP_ID", 3, 0x0002 },
    { "HUBP0_DCHUBP_CNTL", 2, 0x05f4 },
    { "DPPCLK0_DTO_PARAM", 1, 0x0099 },
    { "DPPCLK_CTRL", 1, 0x00a8 },
    { "DCHUBBUB_DET0_CTRL", 2, 0x04bb },
    { "DCHUBBUB_COMPBUF_CTRL", 2, 0x04ba },
};
// Page 43 (n48dr_census6, 0.0.635) is the REST of the DP watch (rows 10..14 of dcn/navi48_disp2.h n48d2_dpx_regs, then the HUBP0 primary address HIGH dword): READS ONLY, the same table discipline. The CLI reads pages 10 and 43 around every disp2 op / dmubsend.
static const struct n48dr_reg n48dr_census6[N48DR_CENSUS6_COUNT] = {
    { "ODM0_OPTC_INPUT_GLOBAL_CONTROL", 2, 0x1aca },
    { "DPP_TOP0_DPP_CONTROL", 2, 0x0cc5 },
    { "MPCC0_MPCC_UPDATE_LOCK_SEL", 3, 0x0005 },
    { "OTG2_OTG_GLOBAL_CONTROL2", 2, 0x1c90 },
    { "HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS", 2, 0x060a },
    { "HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 2, 0x060b },
};
// ---- the monitor A (instance 1) plane census (0.0.654, an internal design note "monitor A (instance 1) plane + framebuffer spec at 1080p60", part 2 "Census first"; READS ONLY) ----------------------------------------------------
// Pages 44..59 (n48dr_census7): the instance-1 twins of census5's pipe-2 rows, found BY NAME in dcn_4_1_0_offset.h (HUBP/HUBPREQ/HUBPRET are pipe 0 + 0xdc, DPP_TOP/CNVC_CFG/DSCL/CM + 0x16b, MPC_OUT1 + 8; the two HUBP*_3DLUT_FL_* registers
// are the header's exception: BASE_IDX 3, instance stride 1 instead of 0xdc, so the table is built from names, never from arithmetic). Order: the 13 single registers (OTG1 global control 2 / master update lock / VSTARTUP / VUPDATE / VREADY /
// H and V total and blank, ODM1_OPTC_INPUT_GLOBAL_CONTROL, DIG2_DIG_OUTPUT_CRC_CNTL, DOMAIN1_PG_STATUS, DENTIST_DISPCLK_CNTL), MPC_OUT1 (MUX, DENORM_CONTROL, the two clamps, CSC_MODE), MPCC_OGAM1_MPCC_OGAM_CONTROL, MCM1 SHAPER_CONTROL /
// 3DLUT_MODE / 1DLUT_CONTROL, then LAST the full HUBP1 / HUBPREQ1 (minus its four VM registers, which census5 already reads) / HUBPRET1 block and the DPP1 twin (DPP_TOP1, CNVC_CFG1, DSCL1, CM1). NOT read, on purpose: the data ports
// DSCL1_SCL_COEF_RAM_TAP_DATA, DSCL1_ISHARP_DELTA_DATA, CM1_CM_GAMCOR_LUT_DATA, CM1_CM_TEST_DEBUG_DATA. MPCC1 is already fully read by census5. `accel dispcensus mona` prints pages 44..59 by name.
static const struct n48dr_reg n48dr_census7[N48DR_CENSUS7_COUNT] = {
    /* ---- page 44 starts here (index 0) ---- */
    { "OTG1_OTG_GLOBAL_CONTROL2", 2, 0x1c10 },
    { "OTG1_OTG_MASTER_UPDATE_LOCK", 2, 0x1c09 },
    { "OTG1_OTG_VSTARTUP_PARAM", 2, 0x1c05 },
    { "OTG1_OTG_VUPDATE_PARAM", 2, 0x1c06 },
    { "OTG1_OTG_VREADY_PARAM", 2, 0x1c07 },
    { "OTG1_OTG_H_TOTAL", 2, 0x1baa },
    { "OTG1_OTG_H_BLANK_START_END", 2, 0x1bab },
    { "OTG1_OTG_V_TOTAL", 2, 0x1baf },
    { "OTG1_OTG_V_BLANK_START_END", 2, 0x1bb8 },
    { "ODM1_OPTC_INPUT_GLOBAL_CONTROL", 2, 0x1ada },
    { "DIG2_DIG_OUTPUT_CRC_CNTL", 2, 0x22de },
    { "DOMAIN1_PG_STATUS", 2, 0x0083 },
    { "DENTIST_DISPCLK_CNTL", 1, 0x0064 },
    { "MPC_OUT1_MUX", 3, 0x02f6 },
    { "MPC_OUT1_DENORM_CONTROL", 3, 0x02f7 },
    { "MPC_OUT1_DENORM_CLAMP_G_Y", 3, 0x02f8 },
    { "MPC_OUT1_DENORM_CLAMP_B_CB", 3, 0x02f9 },
    { "MPC_OUT1_CSC_MODE", 3, 0x0318 },
    { "MPCC_OGAM1_MPCC_OGAM_CONTROL", 3, 0x00dc },
    { "MPCC_MCM1_MPCC_MCM_SHAPER_CONTROL", 3, 0x0503 },
    { "MPCC_MCM1_MPCC_MCM_3DLUT_MODE", 3, 0x053a },
    { "MPCC_MCM1_MPCC_MCM_1DLUT_CONTROL", 3, 0x0543 },
    { "HUBP1_DCSURF_SURFACE_CONFIG", 2, 0x06c1 },
    { "HUBP1_DCSURF_ADDR_CONFIG", 2, 0x06c2 },
    /* ---- page 45 starts here (index 24) ---- */
    { "HUBP1_DCSURF_TILING_CONFIG", 2, 0x06c3 },
    { "HUBP1_DCSURF_PRI_VIEWPORT_START", 2, 0x06c5 },
    { "HUBP1_DCSURF_VIEWPORT_MCACHE_SPLIT_COORDINATE", 2, 0x06c6 },
    { "HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION", 2, 0x06c7 },
    { "HUBP1_DCSURF_PRI_VIEWPORT_START_C", 2, 0x06c8 },
    { "HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION_C", 2, 0x06c9 },
    { "HUBP1_DCSURF_SEC_VIEWPORT_START", 2, 0x06ca },
    { "HUBP1_DCSURF_SEC_VIEWPORT_DIMENSION", 2, 0x06cb },
    { "HUBP1_DCSURF_SEC_VIEWPORT_START_C", 2, 0x06cc },
    { "HUBP1_DCSURF_SEC_VIEWPORT_DIMENSION_C", 2, 0x06cd },
    { "HUBP1_DCHUBP_REQ_SIZE_CONFIG", 2, 0x06ce },
    { "HUBP1_DCHUBP_REQ_SIZE_CONFIG_C", 2, 0x06cf },
    { "HUBP1_DCHUBP_CNTL", 2, 0x06d0 },
    { "HUBP1_HUBP_CLK_CNTL", 2, 0x06d1 },
    { "HUBP1_DCHUBP_VMPG_CONFIG", 2, 0x06d2 },
    { "HUBP1_DCHUBP_MALL_CONFIG", 2, 0x06d3 },
    { "HUBP1_DCHUBP_MALL_SUB_VP0", 2, 0x06d4 },
    { "HUBP1_DCHUBP_MALL_SUB_VP1", 2, 0x06d5 },
    { "HUBP1_DCHUBP_MALL_SUB_VP2", 2, 0x06d6 },
    { "HUBP1_DCHUBP_MCACHEID_CONFIG", 2, 0x06d7 },
    { "HUBP1_HUBPREQ_DEBUG_DB", 2, 0x06d8 },
    { "HUBP1_HUBPREQ_DEBUG", 2, 0x06d9 },
    { "HUBP1_HUBP_DEBUG_CTRL", 2, 0x06da },
    { "HUBP1_HUBP_DEBUG_MUX_DCFCLK", 2, 0x06db },
    /* ---- page 46 starts here (index 48) ---- */
    { "HUBP1_HUBP_DEBUG_MUX_DPPCLK", 2, 0x06dc },
    { "HUBP1_HUBP_MEASURE_WIN_CTRL_DCFCLK", 2, 0x06dd },
    { "HUBP1_HUBP_MEASURE_WIN_CTRL_DPPCLK", 2, 0x06de },
    { "HUBP1_HUBP_MALL_STATUS", 2, 0x06df },
    { "HUBP1_3DLUT_FL_CONFIG", 3, 0x02d7 },
    { "HUBP1_3DLUT_FL_BIAS_SCALE", 3, 0x02d8 },
    { "HUBPREQ1_DCSURF_SURFACE_PITCH", 2, 0x06e3 },
    { "HUBPREQ1_DCSURF_SURFACE_PITCH_C", 2, 0x06e4 },
    { "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS", 2, 0x06e6 },
    { "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 2, 0x06e7 },
    { "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_C", 2, 0x06e8 },
    { "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH_C", 2, 0x06e9 },
    { "HUBPREQ1_DCSURF_SECONDARY_SURFACE_ADDRESS", 2, 0x06ea },
    { "HUBPREQ1_DCSURF_SECONDARY_SURFACE_ADDRESS_HIGH", 2, 0x06eb },
    { "HUBPREQ1_DCSURF_SECONDARY_SURFACE_ADDRESS_C", 2, 0x06ec },
    { "HUBPREQ1_DCSURF_SECONDARY_SURFACE_ADDRESS_HIGH_C", 2, 0x06ed },
    { "HUBPREQ1_DCSURF_SURFACE_CONTROL", 2, 0x06ee },
    { "HUBPREQ1_DCSURF_FLIP_CONTROL", 2, 0x06ef },
    { "HUBPREQ1_DCSURF_FLIP_CONTROL2", 2, 0x06f0 },
    { "HUBPREQ1_DCSURF_SURFACE_FLIP_INTERRUPT", 2, 0x06f3 },
    { "HUBPREQ1_DCSURF_SURFACE_INUSE", 2, 0x06f4 },
    { "HUBPREQ1_DCSURF_SURFACE_INUSE_HIGH", 2, 0x06f5 },
    { "HUBPREQ1_DCSURF_SURFACE_INUSE_C", 2, 0x06f6 },
    { "HUBPREQ1_DCSURF_SURFACE_INUSE_HIGH_C", 2, 0x06f7 },
    /* ---- page 47 starts here (index 72) ---- */
    { "HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE", 2, 0x06f8 },
    { "HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_HIGH", 2, 0x06f9 },
    { "HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_C", 2, 0x06fa },
    { "HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_HIGH_C", 2, 0x06fb },
    { "HUBPREQ1_DCN_EXPANSION_MODE", 2, 0x06fc },
    { "HUBPREQ1_DCN_TTU_QOS_WM", 2, 0x06fd },
    { "HUBPREQ1_DCN_GLOBAL_TTU_CNTL", 2, 0x06fe },
    { "HUBPREQ1_DCN_SURF0_TTU_CNTL0", 2, 0x06ff },
    { "HUBPREQ1_DCN_SURF0_TTU_CNTL1", 2, 0x0700 },
    { "HUBPREQ1_DCN_SURF1_TTU_CNTL0", 2, 0x0701 },
    { "HUBPREQ1_DCN_SURF1_TTU_CNTL1", 2, 0x0702 },
    { "HUBPREQ1_DCN_CUR0_TTU_CNTL0", 2, 0x0703 },
    { "HUBPREQ1_DCN_CUR0_TTU_CNTL1", 2, 0x0704 },
    { "HUBPREQ1_DCN_CUR1_TTU_CNTL0", 2, 0x0705 },
    { "HUBPREQ1_DCN_CUR1_TTU_CNTL1", 2, 0x0706 },
    { "HUBPREQ1_DCN_DMDATA_VM_CNTL", 2, 0x0707 },
    { "HUBPREQ1_BLANK_OFFSET_0", 2, 0x0717 },
    { "HUBPREQ1_BLANK_OFFSET_1", 2, 0x0718 },
    { "HUBPREQ1_DST_DIMENSIONS", 2, 0x0719 },
    { "HUBPREQ1_DST_AFTER_SCALER", 2, 0x071a },
    { "HUBPREQ1_PREFETCH_SETTINGS", 2, 0x071b },
    { "HUBPREQ1_PREFETCH_SETTINGS_C", 2, 0x071c },
    { "HUBPREQ1_VBLANK_PARAMETERS_0", 2, 0x071d },
    { "HUBPREQ1_VBLANK_PARAMETERS_1", 2, 0x071e },
    /* ---- page 48 starts here (index 96) ---- */
    { "HUBPREQ1_VBLANK_PARAMETERS_2", 2, 0x071f },
    { "HUBPREQ1_VBLANK_PARAMETERS_3", 2, 0x0720 },
    { "HUBPREQ1_VBLANK_PARAMETERS_4", 2, 0x0721 },
    { "HUBPREQ1_FLIP_PARAMETERS_0", 2, 0x0722 },
    { "HUBPREQ1_FLIP_PARAMETERS_1", 2, 0x0723 },
    { "HUBPREQ1_FLIP_PARAMETERS_2", 2, 0x0724 },
    { "HUBPREQ1_NOM_PARAMETERS_0", 2, 0x0725 },
    { "HUBPREQ1_NOM_PARAMETERS_1", 2, 0x0726 },
    { "HUBPREQ1_NOM_PARAMETERS_2", 2, 0x0727 },
    { "HUBPREQ1_NOM_PARAMETERS_3", 2, 0x0728 },
    { "HUBPREQ1_NOM_PARAMETERS_4", 2, 0x0729 },
    { "HUBPREQ1_NOM_PARAMETERS_5", 2, 0x072a },
    { "HUBPREQ1_NOM_PARAMETERS_6", 2, 0x072b },
    { "HUBPREQ1_NOM_PARAMETERS_7", 2, 0x072c },
    { "HUBPREQ1_PER_LINE_DELIVERY_PRE", 2, 0x072d },
    { "HUBPREQ1_PER_LINE_DELIVERY", 2, 0x072e },
    { "HUBPREQ1_CURSOR_SETTINGS", 2, 0x072f },
    { "HUBPREQ1_REF_FREQ_TO_PIX_FREQ", 2, 0x0730 },
    { "HUBPREQ1_DST_Y_DELTA_DRQ_LIMIT", 2, 0x0731 },
    { "HUBPREQ1_HUBPREQ_MEM_PWR_CTRL", 2, 0x0732 },
    { "HUBPREQ1_HUBPREQ_MEM_PWR_STATUS", 2, 0x0733 },
    { "HUBPREQ1_VBLANK_PARAMETERS_5", 2, 0x0736 },
    { "HUBPREQ1_VBLANK_PARAMETERS_6", 2, 0x0737 },
    { "HUBPREQ1_FLIP_PARAMETERS_3", 2, 0x0738 },
    /* ---- page 49 starts here (index 120) ---- */
    { "HUBPREQ1_FLIP_PARAMETERS_4", 2, 0x0739 },
    { "HUBPREQ1_FLIP_PARAMETERS_5", 2, 0x073a },
    { "HUBPREQ1_FLIP_PARAMETERS_6", 2, 0x073b },
    { "HUBPREQ1_UCLK_PSTATE_FORCE", 2, 0x073c },
    { "HUBPREQ1_HUBPREQ_STATUS_REG0", 2, 0x073d },
    { "HUBPREQ1_HUBPREQ_STATUS_REG1", 2, 0x073e },
    { "HUBPREQ1_HUBPREQ_STATUS_REG2", 2, 0x073f },
    { "HUBPREQ1_HUBPREQ_STATUS_REG3", 2, 0x0740 },
    { "HUBPRET1_HUBPRET_CONTROL", 2, 0x0749 },
    { "HUBPRET1_HUBPRET_MEM_PWR_CTRL", 2, 0x074a },
    { "HUBPRET1_HUBPRET_MEM_PWR_STATUS", 2, 0x074b },
    { "HUBPRET1_HUBPRET_READ_LINE_CTRL0", 2, 0x074c },
    { "HUBPRET1_HUBPRET_READ_LINE_CTRL1", 2, 0x074d },
    { "HUBPRET1_HUBPRET_READ_LINE0", 2, 0x074e },
    { "HUBPRET1_HUBPRET_READ_LINE1", 2, 0x074f },
    { "HUBPRET1_HUBPRET_INTERRUPT", 2, 0x0750 },
    { "HUBPRET1_HUBPRET_READ_LINE_VALUE", 2, 0x0751 },
    { "HUBPRET1_HUBPRET_READ_LINE_STATUS", 2, 0x0752 },
    { "DPP_TOP1_DPP_CONTROL", 2, 0x0e30 },
    { "DPP_TOP1_DPP_SOFT_RESET", 2, 0x0e31 },
    { "DPP_TOP1_DPP_CRC_VAL_R_G", 2, 0x0e32 },
    { "DPP_TOP1_DPP_CRC_VAL_B_A", 2, 0x0e33 },
    { "DPP_TOP1_DPP_CRC_CTRL", 2, 0x0e34 },
    { "DPP_TOP1_HOST_READ_CONTROL", 2, 0x0e35 },
    /* ---- page 50 starts here (index 144) ---- */
    { "CNVC_CFG1_CNVC_SURFACE_PIXEL_FORMAT", 2, 0x0e3a },
    { "CNVC_CFG1_FORMAT_CONTROL", 2, 0x0e3b },
    { "CNVC_CFG1_FCNV_FP_BIAS_R", 2, 0x0e3c },
    { "CNVC_CFG1_FCNV_FP_BIAS_G", 2, 0x0e3d },
    { "CNVC_CFG1_FCNV_FP_BIAS_B", 2, 0x0e3e },
    { "CNVC_CFG1_FCNV_FP_SCALE_R", 2, 0x0e3f },
    { "CNVC_CFG1_FCNV_FP_SCALE_G", 2, 0x0e40 },
    { "CNVC_CFG1_FCNV_FP_SCALE_B", 2, 0x0e41 },
    { "CNVC_CFG1_COLOR_KEYER_CONTROL", 2, 0x0e42 },
    { "CNVC_CFG1_COLOR_KEYER_ALPHA", 2, 0x0e43 },
    { "CNVC_CFG1_COLOR_KEYER_RED", 2, 0x0e44 },
    { "CNVC_CFG1_COLOR_KEYER_GREEN", 2, 0x0e45 },
    { "CNVC_CFG1_COLOR_KEYER_BLUE", 2, 0x0e46 },
    { "CNVC_CFG1_ALPHA_2BIT_LUT", 2, 0x0e48 },
    { "CNVC_CFG1_PRE_DEALPHA", 2, 0x0e49 },
    { "CNVC_CFG1_PRE_CSC_MODE", 2, 0x0e4a },
    { "CNVC_CFG1_PRE_CSC_C11_C12", 2, 0x0e4b },
    { "CNVC_CFG1_PRE_CSC_C13_C14", 2, 0x0e4c },
    { "CNVC_CFG1_PRE_CSC_C21_C22", 2, 0x0e4d },
    { "CNVC_CFG1_PRE_CSC_C23_C24", 2, 0x0e4e },
    { "CNVC_CFG1_PRE_CSC_C31_C32", 2, 0x0e4f },
    { "CNVC_CFG1_PRE_CSC_C33_C34", 2, 0x0e50 },
    { "CNVC_CFG1_PRE_CSC_B_C11_C12", 2, 0x0e51 },
    { "CNVC_CFG1_PRE_CSC_B_C13_C14", 2, 0x0e52 },
    /* ---- page 51 starts here (index 168) ---- */
    { "CNVC_CFG1_PRE_CSC_B_C21_C22", 2, 0x0e53 },
    { "CNVC_CFG1_PRE_CSC_B_C23_C24", 2, 0x0e54 },
    { "CNVC_CFG1_PRE_CSC_B_C31_C32", 2, 0x0e55 },
    { "CNVC_CFG1_PRE_CSC_B_C33_C34", 2, 0x0e56 },
    { "CNVC_CFG1_CNVC_COEF_FORMAT", 2, 0x0e57 },
    { "CNVC_CFG1_PRE_DEGAM", 2, 0x0e58 },
    { "CNVC_CFG1_PRE_REALPHA", 2, 0x0e59 },
    { "DSCL1_SCL_COEF_RAM_TAP_SELECT", 2, 0x0e71 },
    { "DSCL1_SCL_MODE", 2, 0x0e73 },
    { "DSCL1_SCL_TAP_CONTROL", 2, 0x0e74 },
    { "DSCL1_DSCL_CONTROL", 2, 0x0e75 },
    { "DSCL1_DSCL_2TAP_CONTROL", 2, 0x0e76 },
    { "DSCL1_SCL_MANUAL_REPLICATE_CONTROL", 2, 0x0e77 },
    { "DSCL1_SCL_HORZ_FILTER_SCALE_RATIO", 2, 0x0e78 },
    { "DSCL1_SCL_HORZ_FILTER_INIT", 2, 0x0e79 },
    { "DSCL1_SCL_HORZ_FILTER_SCALE_RATIO_C", 2, 0x0e7a },
    { "DSCL1_SCL_HORZ_FILTER_INIT_C", 2, 0x0e7b },
    { "DSCL1_SCL_VERT_FILTER_SCALE_RATIO", 2, 0x0e7c },
    { "DSCL1_SCL_VERT_FILTER_INIT", 2, 0x0e7d },
    { "DSCL1_SCL_VERT_FILTER_INIT_BOT", 2, 0x0e7e },
    { "DSCL1_SCL_VERT_FILTER_SCALE_RATIO_C", 2, 0x0e7f },
    { "DSCL1_SCL_VERT_FILTER_INIT_C", 2, 0x0e80 },
    { "DSCL1_SCL_VERT_FILTER_INIT_BOT_C", 2, 0x0e81 },
    { "DSCL1_SCL_BLACK_COLOR", 2, 0x0e82 },
    /* ---- page 52 starts here (index 192) ---- */
    { "DSCL1_DSCL_UPDATE", 2, 0x0e83 },
    { "DSCL1_DSCL_AUTOCAL", 2, 0x0e84 },
    { "DSCL1_DSCL_EXT_OVERSCAN_LEFT_RIGHT", 2, 0x0e85 },
    { "DSCL1_DSCL_EXT_OVERSCAN_TOP_BOTTOM", 2, 0x0e86 },
    { "DSCL1_OTG_H_BLANK", 2, 0x0e87 },
    { "DSCL1_OTG_V_BLANK", 2, 0x0e88 },
    { "DSCL1_RECOUT_START", 2, 0x0e89 },
    { "DSCL1_RECOUT_SIZE", 2, 0x0e8a },
    { "DSCL1_MPC_SIZE", 2, 0x0e8b },
    { "DSCL1_LB_DATA_FORMAT", 2, 0x0e8c },
    { "DSCL1_LB_MEMORY_CTRL", 2, 0x0e8d },
    { "DSCL1_LB_V_COUNTER", 2, 0x0e8e },
    { "DSCL1_DSCL_MEM_PWR_CTRL", 2, 0x0e8f },
    { "DSCL1_DSCL_MEM_PWR_STATUS", 2, 0x0e90 },
    { "DSCL1_OBUF_CONTROL", 2, 0x0e91 },
    { "DSCL1_OBUF_MEM_PWR_CTRL", 2, 0x0e92 },
    { "DSCL1_DSCL_EASF_H_MODE", 2, 0x0e93 },
    { "DSCL1_DSCL_EASF_V_MODE", 2, 0x0e94 },
    { "DSCL1_DSCL_SC_MODE", 2, 0x0e95 },
    { "DSCL1_DSCL_SC_MATRIX_C0C1", 2, 0x0e96 },
    { "DSCL1_DSCL_SC_MATRIX_C2C3", 2, 0x0e97 },
    { "DSCL1_DSCL_EASF_H_RINGEST_EVENTAP_GAIN", 2, 0x0e98 },
    { "DSCL1_DSCL_EASF_H_RINGEST_EVENTAP_REDUCE", 2, 0x0e99 },
    { "DSCL1_DSCL_EASF_V_RINGEST_EVENTAP_GAIN", 2, 0x0e9a },
    /* ---- page 53 starts here (index 216) ---- */
    { "DSCL1_DSCL_EASF_V_RINGEST_EVENTAP_REDUCE", 2, 0x0e9b },
    { "DSCL1_DSCL_EASF_V_RINGEST_3TAP_CNTL1", 2, 0x0e9c },
    { "DSCL1_DSCL_EASF_V_RINGEST_3TAP_CNTL2", 2, 0x0e9d },
    { "DSCL1_DSCL_EASF_V_RINGEST_3TAP_CNTL3", 2, 0x0e9e },
    { "DSCL1_DSCL_EASF_RINGEST_FORCE", 2, 0x0e9f },
    { "DSCL1_DSCL_EASF_H_BF_CNTL", 2, 0x0ea0 },
    { "DSCL1_DSCL_EASF_H_BF_FINAL_MAX_MIN", 2, 0x0ea1 },
    { "DSCL1_DSCL_EASF_V_BF_CNTL", 2, 0x0ea2 },
    { "DSCL1_DSCL_EASF_V_BF_FINAL_MAX_MIN", 2, 0x0ea3 },
    { "DSCL1_DSCL_EASF_H_BF1_PWL_SEG0", 2, 0x0ea4 },
    { "DSCL1_DSCL_EASF_H_BF1_PWL_SEG1", 2, 0x0ea5 },
    { "DSCL1_DSCL_EASF_H_BF1_PWL_SEG2", 2, 0x0ea6 },
    { "DSCL1_DSCL_EASF_H_BF1_PWL_SEG3", 2, 0x0ea7 },
    { "DSCL1_DSCL_EASF_H_BF1_PWL_SEG4", 2, 0x0ea8 },
    { "DSCL1_DSCL_EASF_H_BF1_PWL_SEG5", 2, 0x0ea9 },
    { "DSCL1_DSCL_EASF_H_BF1_PWL_SEG6", 2, 0x0eaa },
    { "DSCL1_DSCL_EASF_H_BF1_PWL_SEG7", 2, 0x0eab },
    { "DSCL1_DSCL_EASF_V_BF1_PWL_SEG0", 2, 0x0eac },
    { "DSCL1_DSCL_EASF_V_BF1_PWL_SEG1", 2, 0x0ead },
    { "DSCL1_DSCL_EASF_V_BF1_PWL_SEG2", 2, 0x0eae },
    { "DSCL1_DSCL_EASF_V_BF1_PWL_SEG3", 2, 0x0eaf },
    { "DSCL1_DSCL_EASF_V_BF1_PWL_SEG4", 2, 0x0eb0 },
    { "DSCL1_DSCL_EASF_V_BF1_PWL_SEG5", 2, 0x0eb1 },
    { "DSCL1_DSCL_EASF_V_BF1_PWL_SEG6", 2, 0x0eb2 },
    /* ---- page 54 starts here (index 240) ---- */
    { "DSCL1_DSCL_EASF_V_BF1_PWL_SEG7", 2, 0x0eb3 },
    { "DSCL1_DSCL_EASF_H_BF3_PWL_SEG0", 2, 0x0eb4 },
    { "DSCL1_DSCL_EASF_H_BF3_PWL_SEG1", 2, 0x0eb5 },
    { "DSCL1_DSCL_EASF_H_BF3_PWL_SEG2", 2, 0x0eb6 },
    { "DSCL1_DSCL_EASF_H_BF3_PWL_SEG3", 2, 0x0eb7 },
    { "DSCL1_DSCL_EASF_H_BF3_PWL_SEG4", 2, 0x0eb8 },
    { "DSCL1_DSCL_EASF_H_BF3_PWL_SEG5", 2, 0x0eb9 },
    { "DSCL1_DSCL_EASF_V_BF3_PWL_SEG0", 2, 0x0eba },
    { "DSCL1_DSCL_EASF_V_BF3_PWL_SEG1", 2, 0x0ebb },
    { "DSCL1_DSCL_EASF_V_BF3_PWL_SEG2", 2, 0x0ebc },
    { "DSCL1_DSCL_EASF_V_BF3_PWL_SEG3", 2, 0x0ebd },
    { "DSCL1_DSCL_EASF_V_BF3_PWL_SEG4", 2, 0x0ebe },
    { "DSCL1_DSCL_EASF_V_BF3_PWL_SEG5", 2, 0x0ebf },
    { "DSCL1_ISHARP_MODE", 2, 0x0ec0 },
    { "DSCL1_ISHARP_DELTA_CTRL", 2, 0x0ec1 },
    { "DSCL1_ISHARP_DELTA_INDEX", 2, 0x0ec2 },
    { "DSCL1_ISHARP_NLDELTA_SOFT_CLIP", 2, 0x0ec4 },
    { "DSCL1_ISHARP_NOISEDET_THRESHOLD", 2, 0x0ec5 },
    { "DSCL1_ISHARP_NOISE_GAIN_PWL", 2, 0x0ec6 },
    { "DSCL1_ISHARP_LBA_PWL_SEG0", 2, 0x0ec7 },
    { "DSCL1_ISHARP_LBA_PWL_SEG1", 2, 0x0ec8 },
    { "DSCL1_ISHARP_LBA_PWL_SEG2", 2, 0x0ec9 },
    { "DSCL1_ISHARP_LBA_PWL_SEG3", 2, 0x0eca },
    { "DSCL1_ISHARP_LBA_PWL_SEG4", 2, 0x0ecb },
    /* ---- page 55 starts here (index 264) ---- */
    { "DSCL1_ISHARP_LBA_PWL_SEG5", 2, 0x0ecc },
    { "DSCL1_ISHARP_DELTA_LUT_MEM_PWR_CTRL", 2, 0x0ecd },
    { "CM1_CM_CONTROL", 2, 0x0ed2 },
    { "CM1_CM_POST_CSC_CONTROL", 2, 0x0ed3 },
    { "CM1_CM_POST_CSC_C11_C12", 2, 0x0ed4 },
    { "CM1_CM_POST_CSC_C13_C14", 2, 0x0ed5 },
    { "CM1_CM_POST_CSC_C21_C22", 2, 0x0ed6 },
    { "CM1_CM_POST_CSC_C23_C24", 2, 0x0ed7 },
    { "CM1_CM_POST_CSC_C31_C32", 2, 0x0ed8 },
    { "CM1_CM_POST_CSC_C33_C34", 2, 0x0ed9 },
    { "CM1_CM_POST_CSC_B_C11_C12", 2, 0x0eda },
    { "CM1_CM_POST_CSC_B_C13_C14", 2, 0x0edb },
    { "CM1_CM_POST_CSC_B_C21_C22", 2, 0x0edc },
    { "CM1_CM_POST_CSC_B_C23_C24", 2, 0x0edd },
    { "CM1_CM_POST_CSC_B_C31_C32", 2, 0x0ede },
    { "CM1_CM_POST_CSC_B_C33_C34", 2, 0x0edf },
    { "CM1_CM_BIAS_CR_R", 2, 0x0ee0 },
    { "CM1_CM_BIAS_Y_G_CB_B", 2, 0x0ee1 },
    { "CM1_CM_GAMCOR_CONTROL", 2, 0x0ee2 },
    { "CM1_CM_GAMCOR_LUT_INDEX", 2, 0x0ee3 },
    { "CM1_CM_GAMCOR_LUT_CONTROL", 2, 0x0ee5 },
    { "CM1_CM_GAMCOR_RAMA_START_CNTL_B", 2, 0x0ee6 },
    { "CM1_CM_GAMCOR_RAMA_START_CNTL_G", 2, 0x0ee7 },
    { "CM1_CM_GAMCOR_RAMA_START_CNTL_R", 2, 0x0ee8 },
    /* ---- page 56 starts here (index 288) ---- */
    { "CM1_CM_GAMCOR_RAMA_START_SLOPE_CNTL_B", 2, 0x0ee9 },
    { "CM1_CM_GAMCOR_RAMA_START_SLOPE_CNTL_G", 2, 0x0eea },
    { "CM1_CM_GAMCOR_RAMA_START_SLOPE_CNTL_R", 2, 0x0eeb },
    { "CM1_CM_GAMCOR_RAMA_START_BASE_CNTL_B", 2, 0x0eec },
    { "CM1_CM_GAMCOR_RAMA_START_BASE_CNTL_G", 2, 0x0eed },
    { "CM1_CM_GAMCOR_RAMA_START_BASE_CNTL_R", 2, 0x0eee },
    { "CM1_CM_GAMCOR_RAMA_END_CNTL1_B", 2, 0x0eef },
    { "CM1_CM_GAMCOR_RAMA_END_CNTL2_B", 2, 0x0ef0 },
    { "CM1_CM_GAMCOR_RAMA_END_CNTL1_G", 2, 0x0ef1 },
    { "CM1_CM_GAMCOR_RAMA_END_CNTL2_G", 2, 0x0ef2 },
    { "CM1_CM_GAMCOR_RAMA_END_CNTL1_R", 2, 0x0ef3 },
    { "CM1_CM_GAMCOR_RAMA_END_CNTL2_R", 2, 0x0ef4 },
    { "CM1_CM_GAMCOR_RAMA_OFFSET_B", 2, 0x0ef5 },
    { "CM1_CM_GAMCOR_RAMA_OFFSET_G", 2, 0x0ef6 },
    { "CM1_CM_GAMCOR_RAMA_OFFSET_R", 2, 0x0ef7 },
    { "CM1_CM_GAMCOR_RAMA_REGION_0_1", 2, 0x0ef8 },
    { "CM1_CM_GAMCOR_RAMA_REGION_2_3", 2, 0x0ef9 },
    { "CM1_CM_GAMCOR_RAMA_REGION_4_5", 2, 0x0efa },
    { "CM1_CM_GAMCOR_RAMA_REGION_6_7", 2, 0x0efb },
    { "CM1_CM_GAMCOR_RAMA_REGION_8_9", 2, 0x0efc },
    { "CM1_CM_GAMCOR_RAMA_REGION_10_11", 2, 0x0efd },
    { "CM1_CM_GAMCOR_RAMA_REGION_12_13", 2, 0x0efe },
    { "CM1_CM_GAMCOR_RAMA_REGION_14_15", 2, 0x0eff },
    { "CM1_CM_GAMCOR_RAMA_REGION_16_17", 2, 0x0f00 },
    /* ---- page 57 starts here (index 312) ---- */
    { "CM1_CM_GAMCOR_RAMA_REGION_18_19", 2, 0x0f01 },
    { "CM1_CM_GAMCOR_RAMA_REGION_20_21", 2, 0x0f02 },
    { "CM1_CM_GAMCOR_RAMA_REGION_22_23", 2, 0x0f03 },
    { "CM1_CM_GAMCOR_RAMA_REGION_24_25", 2, 0x0f04 },
    { "CM1_CM_GAMCOR_RAMA_REGION_26_27", 2, 0x0f05 },
    { "CM1_CM_GAMCOR_RAMA_REGION_28_29", 2, 0x0f06 },
    { "CM1_CM_GAMCOR_RAMA_REGION_30_31", 2, 0x0f07 },
    { "CM1_CM_GAMCOR_RAMA_REGION_32_33", 2, 0x0f08 },
    { "CM1_CM_GAMCOR_RAMB_START_CNTL_B", 2, 0x0f09 },
    { "CM1_CM_GAMCOR_RAMB_START_CNTL_G", 2, 0x0f0a },
    { "CM1_CM_GAMCOR_RAMB_START_CNTL_R", 2, 0x0f0b },
    { "CM1_CM_GAMCOR_RAMB_START_SLOPE_CNTL_B", 2, 0x0f0c },
    { "CM1_CM_GAMCOR_RAMB_START_SLOPE_CNTL_G", 2, 0x0f0d },
    { "CM1_CM_GAMCOR_RAMB_START_SLOPE_CNTL_R", 2, 0x0f0e },
    { "CM1_CM_GAMCOR_RAMB_START_BASE_CNTL_B", 2, 0x0f0f },
    { "CM1_CM_GAMCOR_RAMB_START_BASE_CNTL_G", 2, 0x0f10 },
    { "CM1_CM_GAMCOR_RAMB_START_BASE_CNTL_R", 2, 0x0f11 },
    { "CM1_CM_GAMCOR_RAMB_END_CNTL1_B", 2, 0x0f12 },
    { "CM1_CM_GAMCOR_RAMB_END_CNTL2_B", 2, 0x0f13 },
    { "CM1_CM_GAMCOR_RAMB_END_CNTL1_G", 2, 0x0f14 },
    { "CM1_CM_GAMCOR_RAMB_END_CNTL2_G", 2, 0x0f15 },
    { "CM1_CM_GAMCOR_RAMB_END_CNTL1_R", 2, 0x0f16 },
    { "CM1_CM_GAMCOR_RAMB_END_CNTL2_R", 2, 0x0f17 },
    { "CM1_CM_GAMCOR_RAMB_OFFSET_B", 2, 0x0f18 },
    /* ---- page 58 starts here (index 336) ---- */
    { "CM1_CM_GAMCOR_RAMB_OFFSET_G", 2, 0x0f19 },
    { "CM1_CM_GAMCOR_RAMB_OFFSET_R", 2, 0x0f1a },
    { "CM1_CM_GAMCOR_RAMB_REGION_0_1", 2, 0x0f1b },
    { "CM1_CM_GAMCOR_RAMB_REGION_2_3", 2, 0x0f1c },
    { "CM1_CM_GAMCOR_RAMB_REGION_4_5", 2, 0x0f1d },
    { "CM1_CM_GAMCOR_RAMB_REGION_6_7", 2, 0x0f1e },
    { "CM1_CM_GAMCOR_RAMB_REGION_8_9", 2, 0x0f1f },
    { "CM1_CM_GAMCOR_RAMB_REGION_10_11", 2, 0x0f20 },
    { "CM1_CM_GAMCOR_RAMB_REGION_12_13", 2, 0x0f21 },
    { "CM1_CM_GAMCOR_RAMB_REGION_14_15", 2, 0x0f22 },
    { "CM1_CM_GAMCOR_RAMB_REGION_16_17", 2, 0x0f23 },
    { "CM1_CM_GAMCOR_RAMB_REGION_18_19", 2, 0x0f24 },
    { "CM1_CM_GAMCOR_RAMB_REGION_20_21", 2, 0x0f25 },
    { "CM1_CM_GAMCOR_RAMB_REGION_22_23", 2, 0x0f26 },
    { "CM1_CM_GAMCOR_RAMB_REGION_24_25", 2, 0x0f27 },
    { "CM1_CM_GAMCOR_RAMB_REGION_26_27", 2, 0x0f28 },
    { "CM1_CM_GAMCOR_RAMB_REGION_28_29", 2, 0x0f29 },
    { "CM1_CM_GAMCOR_RAMB_REGION_30_31", 2, 0x0f2a },
    { "CM1_CM_GAMCOR_RAMB_REGION_32_33", 2, 0x0f2b },
    { "CM1_CM_HDR_MULT_COEF", 2, 0x0f2c },
    { "CM1_CM_MEM_PWR_CTRL", 2, 0x0f2d },
    { "CM1_CM_MEM_PWR_STATUS", 2, 0x0f2e },
    { "CM1_CM_DEALPHA", 2, 0x0f30 },
    { "CM1_CM_COEF_FORMAT", 2, 0x0f31 },
    /* ---- page 59 starts here (index 360) ---- */
    { "CM1_CM_TEST_DEBUG_INDEX", 2, 0x0f32 },
};
// Pages 11..42 (n48dr_census5), in this order: DCHUBBUB (COMPBUF, DET0..3, GLOBAL_TIMER), DCCG (DPPCLK_CTRL, DPPCLK0..3_DTO_PARAM, DPPCLK_DTO_CTRL), DCN_VM_FAULT (CNTL, STATUS, ADDR_MSB / LSB),
// the VM registers of HUBPREQ0 / 1 / 2 (aperture low / high, MX_L1_TLB_CNTL, VMID_SETTINGS_0), the full HUBP0 / HUBPREQ0 / HUBPRET0 block, the full DPP0 block (DPP_TOP0, CNVC_CFG0, DSCL0, CM0), MPCC0..2 (all 15),
// MPC_OUT0 / MPC_OUT2 (MUX, DENORM_CONTROL, the two DENORM clamps, CSC_MODE), OTG2_OTG_GLOBAL_CONTROL2, and LAST (so a hang on a clock-gated pipe would show at the end) the full HUBP2 / HUBPREQ2 / HUBPRET2 block and the
// DPP2 twin. NOT read, on purpose: the data ports whose read may step an auto-increment index (DSCL*_SCL_COEF_RAM_TAP_DATA, DSCL*_ISHARP_DELTA_DATA, CM*_CM_GAMCOR_LUT_DATA, CM*_CM_TEST_DEBUG_DATA).
static const struct n48dr_reg n48dr_census5[N48DR_CENSUS5_COUNT] = {
    /* ---- page 11 starts here (index 0) ---- */
    { "DCHUBBUB_COMPBUF_CTRL", 2, 0x04ba },
    { "DCHUBBUB_DET0_CTRL", 2, 0x04bb },
    { "DCHUBBUB_DET1_CTRL", 2, 0x04bc },
    { "DCHUBBUB_DET2_CTRL", 2, 0x04bd },
    { "DCHUBBUB_DET3_CTRL", 2, 0x04be },
    { "DCHUBBUB_GLOBAL_TIMER_CNTL", 2, 0x0527 },
    { "DPPCLK_CTRL", 1, 0x00a8 },
    { "DPPCLK0_DTO_PARAM", 1, 0x0099 },
    { "DPPCLK1_DTO_PARAM", 1, 0x009a },
    { "DPPCLK2_DTO_PARAM", 1, 0x009b },
    { "DPPCLK3_DTO_PARAM", 1, 0x009c },
    { "DPPCLK_DTO_CTRL", 1, 0x00b6 },
    { "DCN_VM_FAULT_CNTL", 2, 0x05cb },
    { "DCN_VM_FAULT_STATUS", 2, 0x05cc },
    { "DCN_VM_FAULT_ADDR_MSB", 2, 0x05cd },
    { "DCN_VM_FAULT_ADDR_LSB", 2, 0x05ce },
    { "HUBPREQ0_DCN_VM_SYSTEM_APERTURE_LOW_ADDR", 2, 0x062c },
    { "HUBPREQ0_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR", 2, 0x062d },
    { "HUBPREQ0_DCN_VM_MX_L1_TLB_CNTL", 2, 0x063a },
    { "HUBPREQ0_VMID_SETTINGS_0", 2, 0x0609 },
    { "HUBPREQ1_DCN_VM_SYSTEM_APERTURE_LOW_ADDR", 2, 0x0708 },
    { "HUBPREQ1_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR", 2, 0x0709 },
    { "HUBPREQ1_DCN_VM_MX_L1_TLB_CNTL", 2, 0x0716 },
    { "HUBPREQ1_VMID_SETTINGS_0", 2, 0x06e5 },
    /* ---- page 12 starts here (index 24) ---- */
    { "HUBPREQ2_DCN_VM_SYSTEM_APERTURE_LOW_ADDR", 2, 0x07e4 },
    { "HUBPREQ2_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR", 2, 0x07e5 },
    { "HUBPREQ2_DCN_VM_MX_L1_TLB_CNTL", 2, 0x07f2 },
    { "HUBPREQ2_VMID_SETTINGS_0", 2, 0x07c1 },
    { "HUBP0_DCSURF_SURFACE_CONFIG", 2, 0x05e5 },
    { "HUBP0_DCSURF_ADDR_CONFIG", 2, 0x05e6 },
    { "HUBP0_DCSURF_TILING_CONFIG", 2, 0x05e7 },
    { "HUBP0_DCSURF_PRI_VIEWPORT_START", 2, 0x05e9 },
    { "HUBP0_DCSURF_VIEWPORT_MCACHE_SPLIT_COORDINATE", 2, 0x05ea },
    { "HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION", 2, 0x05eb },
    { "HUBP0_DCSURF_PRI_VIEWPORT_START_C", 2, 0x05ec },
    { "HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION_C", 2, 0x05ed },
    { "HUBP0_DCSURF_SEC_VIEWPORT_START", 2, 0x05ee },
    { "HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION", 2, 0x05ef },
    { "HUBP0_DCSURF_SEC_VIEWPORT_START_C", 2, 0x05f0 },
    { "HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION_C", 2, 0x05f1 },
    { "HUBP0_DCHUBP_REQ_SIZE_CONFIG", 2, 0x05f2 },
    { "HUBP0_DCHUBP_REQ_SIZE_CONFIG_C", 2, 0x05f3 },
    { "HUBP0_DCHUBP_CNTL", 2, 0x05f4 },
    { "HUBP0_HUBP_CLK_CNTL", 2, 0x05f5 },
    { "HUBP0_DCHUBP_VMPG_CONFIG", 2, 0x05f6 },
    { "HUBP0_DCHUBP_MALL_CONFIG", 2, 0x05f7 },
    { "HUBP0_DCHUBP_MALL_SUB_VP0", 2, 0x05f8 },
    { "HUBP0_DCHUBP_MALL_SUB_VP1", 2, 0x05f9 },
    /* ---- page 13 starts here (index 48) ---- */
    { "HUBP0_DCHUBP_MALL_SUB_VP2", 2, 0x05fa },
    { "HUBP0_DCHUBP_MCACHEID_CONFIG", 2, 0x05fb },
    { "HUBP0_HUBPREQ_DEBUG_DB", 2, 0x05fc },
    { "HUBP0_HUBPREQ_DEBUG", 2, 0x05fd },
    { "HUBP0_HUBP_DEBUG_CTRL", 2, 0x05fe },
    { "HUBP0_HUBP_DEBUG_MUX_DCFCLK", 2, 0x05ff },
    { "HUBP0_HUBP_DEBUG_MUX_DPPCLK", 2, 0x0600 },
    { "HUBP0_HUBP_MEASURE_WIN_CTRL_DCFCLK", 2, 0x0601 },
    { "HUBP0_HUBP_MEASURE_WIN_CTRL_DPPCLK", 2, 0x0602 },
    { "HUBP0_HUBP_MALL_STATUS", 2, 0x0603 },
    { "HUBP0_3DLUT_FL_CONFIG", 3, 0x02d5 },
    { "HUBP0_3DLUT_FL_BIAS_SCALE", 3, 0x02d6 },
    { "HUBPREQ0_DCSURF_SURFACE_PITCH", 2, 0x0607 },
    { "HUBPREQ0_DCSURF_SURFACE_PITCH_C", 2, 0x0608 },
    { "HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS", 2, 0x060a },
    { "HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 2, 0x060b },
    { "HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_C", 2, 0x060c },
    { "HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH_C", 2, 0x060d },
    { "HUBPREQ0_DCSURF_SECONDARY_SURFACE_ADDRESS", 2, 0x060e },
    { "HUBPREQ0_DCSURF_SECONDARY_SURFACE_ADDRESS_HIGH", 2, 0x060f },
    { "HUBPREQ0_DCSURF_SECONDARY_SURFACE_ADDRESS_C", 2, 0x0610 },
    { "HUBPREQ0_DCSURF_SECONDARY_SURFACE_ADDRESS_HIGH_C", 2, 0x0611 },
    { "HUBPREQ0_DCSURF_SURFACE_CONTROL", 2, 0x0612 },
    { "HUBPREQ0_DCSURF_FLIP_CONTROL", 2, 0x0613 },
    /* ---- page 14 starts here (index 72) ---- */
    { "HUBPREQ0_DCSURF_FLIP_CONTROL2", 2, 0x0614 },
    { "HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT", 2, 0x0617 },
    { "HUBPREQ0_DCSURF_SURFACE_INUSE", 2, 0x0618 },
    { "HUBPREQ0_DCSURF_SURFACE_INUSE_HIGH", 2, 0x0619 },
    { "HUBPREQ0_DCSURF_SURFACE_INUSE_C", 2, 0x061a },
    { "HUBPREQ0_DCSURF_SURFACE_INUSE_HIGH_C", 2, 0x061b },
    { "HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE", 2, 0x061c },
    { "HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH", 2, 0x061d },
    { "HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_C", 2, 0x061e },
    { "HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH_C", 2, 0x061f },
    { "HUBPREQ0_DCN_EXPANSION_MODE", 2, 0x0620 },
    { "HUBPREQ0_DCN_TTU_QOS_WM", 2, 0x0621 },
    { "HUBPREQ0_DCN_GLOBAL_TTU_CNTL", 2, 0x0622 },
    { "HUBPREQ0_DCN_SURF0_TTU_CNTL0", 2, 0x0623 },
    { "HUBPREQ0_DCN_SURF0_TTU_CNTL1", 2, 0x0624 },
    { "HUBPREQ0_DCN_SURF1_TTU_CNTL0", 2, 0x0625 },
    { "HUBPREQ0_DCN_SURF1_TTU_CNTL1", 2, 0x0626 },
    { "HUBPREQ0_DCN_CUR0_TTU_CNTL0", 2, 0x0627 },
    { "HUBPREQ0_DCN_CUR0_TTU_CNTL1", 2, 0x0628 },
    { "HUBPREQ0_DCN_CUR1_TTU_CNTL0", 2, 0x0629 },
    { "HUBPREQ0_DCN_CUR1_TTU_CNTL1", 2, 0x062a },
    { "HUBPREQ0_DCN_DMDATA_VM_CNTL", 2, 0x062b },
    { "HUBPREQ0_BLANK_OFFSET_0", 2, 0x063b },
    { "HUBPREQ0_BLANK_OFFSET_1", 2, 0x063c },
    /* ---- page 15 starts here (index 96) ---- */
    { "HUBPREQ0_DST_DIMENSIONS", 2, 0x063d },
    { "HUBPREQ0_DST_AFTER_SCALER", 2, 0x063e },
    { "HUBPREQ0_PREFETCH_SETTINGS", 2, 0x063f },
    { "HUBPREQ0_PREFETCH_SETTINGS_C", 2, 0x0640 },
    { "HUBPREQ0_VBLANK_PARAMETERS_0", 2, 0x0641 },
    { "HUBPREQ0_VBLANK_PARAMETERS_1", 2, 0x0642 },
    { "HUBPREQ0_VBLANK_PARAMETERS_2", 2, 0x0643 },
    { "HUBPREQ0_VBLANK_PARAMETERS_3", 2, 0x0644 },
    { "HUBPREQ0_VBLANK_PARAMETERS_4", 2, 0x0645 },
    { "HUBPREQ0_FLIP_PARAMETERS_0", 2, 0x0646 },
    { "HUBPREQ0_FLIP_PARAMETERS_1", 2, 0x0647 },
    { "HUBPREQ0_FLIP_PARAMETERS_2", 2, 0x0648 },
    { "HUBPREQ0_NOM_PARAMETERS_0", 2, 0x0649 },
    { "HUBPREQ0_NOM_PARAMETERS_1", 2, 0x064a },
    { "HUBPREQ0_NOM_PARAMETERS_2", 2, 0x064b },
    { "HUBPREQ0_NOM_PARAMETERS_3", 2, 0x064c },
    { "HUBPREQ0_NOM_PARAMETERS_4", 2, 0x064d },
    { "HUBPREQ0_NOM_PARAMETERS_5", 2, 0x064e },
    { "HUBPREQ0_NOM_PARAMETERS_6", 2, 0x064f },
    { "HUBPREQ0_NOM_PARAMETERS_7", 2, 0x0650 },
    { "HUBPREQ0_PER_LINE_DELIVERY_PRE", 2, 0x0651 },
    { "HUBPREQ0_PER_LINE_DELIVERY", 2, 0x0652 },
    { "HUBPREQ0_CURSOR_SETTINGS", 2, 0x0653 },
    { "HUBPREQ0_REF_FREQ_TO_PIX_FREQ", 2, 0x0654 },
    /* ---- page 16 starts here (index 120) ---- */
    { "HUBPREQ0_DST_Y_DELTA_DRQ_LIMIT", 2, 0x0655 },
    { "HUBPREQ0_HUBPREQ_MEM_PWR_CTRL", 2, 0x0656 },
    { "HUBPREQ0_HUBPREQ_MEM_PWR_STATUS", 2, 0x0657 },
    { "HUBPREQ0_VBLANK_PARAMETERS_5", 2, 0x065a },
    { "HUBPREQ0_VBLANK_PARAMETERS_6", 2, 0x065b },
    { "HUBPREQ0_FLIP_PARAMETERS_3", 2, 0x065c },
    { "HUBPREQ0_FLIP_PARAMETERS_4", 2, 0x065d },
    { "HUBPREQ0_FLIP_PARAMETERS_5", 2, 0x065e },
    { "HUBPREQ0_FLIP_PARAMETERS_6", 2, 0x065f },
    { "HUBPREQ0_UCLK_PSTATE_FORCE", 2, 0x0660 },
    { "HUBPREQ0_HUBPREQ_STATUS_REG0", 2, 0x0661 },
    { "HUBPREQ0_HUBPREQ_STATUS_REG1", 2, 0x0662 },
    { "HUBPREQ0_HUBPREQ_STATUS_REG2", 2, 0x0663 },
    { "HUBPREQ0_HUBPREQ_STATUS_REG3", 2, 0x0664 },
    { "HUBPRET0_HUBPRET_CONTROL", 2, 0x066d },
    { "HUBPRET0_HUBPRET_MEM_PWR_CTRL", 2, 0x066e },
    { "HUBPRET0_HUBPRET_MEM_PWR_STATUS", 2, 0x066f },
    { "HUBPRET0_HUBPRET_READ_LINE_CTRL0", 2, 0x0670 },
    { "HUBPRET0_HUBPRET_READ_LINE_CTRL1", 2, 0x0671 },
    { "HUBPRET0_HUBPRET_READ_LINE0", 2, 0x0672 },
    { "HUBPRET0_HUBPRET_READ_LINE1", 2, 0x0673 },
    { "HUBPRET0_HUBPRET_INTERRUPT", 2, 0x0674 },
    { "HUBPRET0_HUBPRET_READ_LINE_VALUE", 2, 0x0675 },
    { "HUBPRET0_HUBPRET_READ_LINE_STATUS", 2, 0x0676 },
    /* ---- page 17 starts here (index 144) ---- */
    { "DPP_TOP0_DPP_CONTROL", 2, 0x0cc5 },
    { "DPP_TOP0_DPP_SOFT_RESET", 2, 0x0cc6 },
    { "DPP_TOP0_DPP_CRC_VAL_R_G", 2, 0x0cc7 },
    { "DPP_TOP0_DPP_CRC_VAL_B_A", 2, 0x0cc8 },
    { "DPP_TOP0_DPP_CRC_CTRL", 2, 0x0cc9 },
    { "DPP_TOP0_HOST_READ_CONTROL", 2, 0x0cca },
    { "CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT", 2, 0x0ccf },
    { "CNVC_CFG0_FORMAT_CONTROL", 2, 0x0cd0 },
    { "CNVC_CFG0_FCNV_FP_BIAS_R", 2, 0x0cd1 },
    { "CNVC_CFG0_FCNV_FP_BIAS_G", 2, 0x0cd2 },
    { "CNVC_CFG0_FCNV_FP_BIAS_B", 2, 0x0cd3 },
    { "CNVC_CFG0_FCNV_FP_SCALE_R", 2, 0x0cd4 },
    { "CNVC_CFG0_FCNV_FP_SCALE_G", 2, 0x0cd5 },
    { "CNVC_CFG0_FCNV_FP_SCALE_B", 2, 0x0cd6 },
    { "CNVC_CFG0_COLOR_KEYER_CONTROL", 2, 0x0cd7 },
    { "CNVC_CFG0_COLOR_KEYER_ALPHA", 2, 0x0cd8 },
    { "CNVC_CFG0_COLOR_KEYER_RED", 2, 0x0cd9 },
    { "CNVC_CFG0_COLOR_KEYER_GREEN", 2, 0x0cda },
    { "CNVC_CFG0_COLOR_KEYER_BLUE", 2, 0x0cdb },
    { "CNVC_CFG0_ALPHA_2BIT_LUT", 2, 0x0cdd },
    { "CNVC_CFG0_PRE_DEALPHA", 2, 0x0cde },
    { "CNVC_CFG0_PRE_CSC_MODE", 2, 0x0cdf },
    { "CNVC_CFG0_PRE_CSC_C11_C12", 2, 0x0ce0 },
    { "CNVC_CFG0_PRE_CSC_C13_C14", 2, 0x0ce1 },
    /* ---- page 18 starts here (index 168) ---- */
    { "CNVC_CFG0_PRE_CSC_C21_C22", 2, 0x0ce2 },
    { "CNVC_CFG0_PRE_CSC_C23_C24", 2, 0x0ce3 },
    { "CNVC_CFG0_PRE_CSC_C31_C32", 2, 0x0ce4 },
    { "CNVC_CFG0_PRE_CSC_C33_C34", 2, 0x0ce5 },
    { "CNVC_CFG0_PRE_CSC_B_C11_C12", 2, 0x0ce6 },
    { "CNVC_CFG0_PRE_CSC_B_C13_C14", 2, 0x0ce7 },
    { "CNVC_CFG0_PRE_CSC_B_C21_C22", 2, 0x0ce8 },
    { "CNVC_CFG0_PRE_CSC_B_C23_C24", 2, 0x0ce9 },
    { "CNVC_CFG0_PRE_CSC_B_C31_C32", 2, 0x0cea },
    { "CNVC_CFG0_PRE_CSC_B_C33_C34", 2, 0x0ceb },
    { "CNVC_CFG0_CNVC_COEF_FORMAT", 2, 0x0cec },
    { "CNVC_CFG0_PRE_DEGAM", 2, 0x0ced },
    { "CNVC_CFG0_PRE_REALPHA", 2, 0x0cee },
    { "DSCL0_SCL_COEF_RAM_TAP_SELECT", 2, 0x0d06 },
    { "DSCL0_SCL_MODE", 2, 0x0d08 },
    { "DSCL0_SCL_TAP_CONTROL", 2, 0x0d09 },
    { "DSCL0_DSCL_CONTROL", 2, 0x0d0a },
    { "DSCL0_DSCL_2TAP_CONTROL", 2, 0x0d0b },
    { "DSCL0_SCL_MANUAL_REPLICATE_CONTROL", 2, 0x0d0c },
    { "DSCL0_SCL_HORZ_FILTER_SCALE_RATIO", 2, 0x0d0d },
    { "DSCL0_SCL_HORZ_FILTER_INIT", 2, 0x0d0e },
    { "DSCL0_SCL_HORZ_FILTER_SCALE_RATIO_C", 2, 0x0d0f },
    { "DSCL0_SCL_HORZ_FILTER_INIT_C", 2, 0x0d10 },
    { "DSCL0_SCL_VERT_FILTER_SCALE_RATIO", 2, 0x0d11 },
    /* ---- page 19 starts here (index 192) ---- */
    { "DSCL0_SCL_VERT_FILTER_INIT", 2, 0x0d12 },
    { "DSCL0_SCL_VERT_FILTER_INIT_BOT", 2, 0x0d13 },
    { "DSCL0_SCL_VERT_FILTER_SCALE_RATIO_C", 2, 0x0d14 },
    { "DSCL0_SCL_VERT_FILTER_INIT_C", 2, 0x0d15 },
    { "DSCL0_SCL_VERT_FILTER_INIT_BOT_C", 2, 0x0d16 },
    { "DSCL0_SCL_BLACK_COLOR", 2, 0x0d17 },
    { "DSCL0_DSCL_UPDATE", 2, 0x0d18 },
    { "DSCL0_DSCL_AUTOCAL", 2, 0x0d19 },
    { "DSCL0_DSCL_EXT_OVERSCAN_LEFT_RIGHT", 2, 0x0d1a },
    { "DSCL0_DSCL_EXT_OVERSCAN_TOP_BOTTOM", 2, 0x0d1b },
    { "DSCL0_OTG_H_BLANK", 2, 0x0d1c },
    { "DSCL0_OTG_V_BLANK", 2, 0x0d1d },
    { "DSCL0_RECOUT_START", 2, 0x0d1e },
    { "DSCL0_RECOUT_SIZE", 2, 0x0d1f },
    { "DSCL0_MPC_SIZE", 2, 0x0d20 },
    { "DSCL0_LB_DATA_FORMAT", 2, 0x0d21 },
    { "DSCL0_LB_MEMORY_CTRL", 2, 0x0d22 },
    { "DSCL0_LB_V_COUNTER", 2, 0x0d23 },
    { "DSCL0_DSCL_MEM_PWR_CTRL", 2, 0x0d24 },
    { "DSCL0_DSCL_MEM_PWR_STATUS", 2, 0x0d25 },
    { "DSCL0_OBUF_CONTROL", 2, 0x0d26 },
    { "DSCL0_OBUF_MEM_PWR_CTRL", 2, 0x0d27 },
    { "DSCL0_DSCL_EASF_H_MODE", 2, 0x0d28 },
    { "DSCL0_DSCL_EASF_V_MODE", 2, 0x0d29 },
    /* ---- page 20 starts here (index 216) ---- */
    { "DSCL0_DSCL_SC_MODE", 2, 0x0d2a },
    { "DSCL0_DSCL_SC_MATRIX_C0C1", 2, 0x0d2b },
    { "DSCL0_DSCL_SC_MATRIX_C2C3", 2, 0x0d2c },
    { "DSCL0_DSCL_EASF_H_RINGEST_EVENTAP_GAIN", 2, 0x0d2d },
    { "DSCL0_DSCL_EASF_H_RINGEST_EVENTAP_REDUCE", 2, 0x0d2e },
    { "DSCL0_DSCL_EASF_V_RINGEST_EVENTAP_GAIN", 2, 0x0d2f },
    { "DSCL0_DSCL_EASF_V_RINGEST_EVENTAP_REDUCE", 2, 0x0d30 },
    { "DSCL0_DSCL_EASF_V_RINGEST_3TAP_CNTL1", 2, 0x0d31 },
    { "DSCL0_DSCL_EASF_V_RINGEST_3TAP_CNTL2", 2, 0x0d32 },
    { "DSCL0_DSCL_EASF_V_RINGEST_3TAP_CNTL3", 2, 0x0d33 },
    { "DSCL0_DSCL_EASF_RINGEST_FORCE", 2, 0x0d34 },
    { "DSCL0_DSCL_EASF_H_BF_CNTL", 2, 0x0d35 },
    { "DSCL0_DSCL_EASF_H_BF_FINAL_MAX_MIN", 2, 0x0d36 },
    { "DSCL0_DSCL_EASF_V_BF_CNTL", 2, 0x0d37 },
    { "DSCL0_DSCL_EASF_V_BF_FINAL_MAX_MIN", 2, 0x0d38 },
    { "DSCL0_DSCL_EASF_H_BF1_PWL_SEG0", 2, 0x0d39 },
    { "DSCL0_DSCL_EASF_H_BF1_PWL_SEG1", 2, 0x0d3a },
    { "DSCL0_DSCL_EASF_H_BF1_PWL_SEG2", 2, 0x0d3b },
    { "DSCL0_DSCL_EASF_H_BF1_PWL_SEG3", 2, 0x0d3c },
    { "DSCL0_DSCL_EASF_H_BF1_PWL_SEG4", 2, 0x0d3d },
    { "DSCL0_DSCL_EASF_H_BF1_PWL_SEG5", 2, 0x0d3e },
    { "DSCL0_DSCL_EASF_H_BF1_PWL_SEG6", 2, 0x0d3f },
    { "DSCL0_DSCL_EASF_H_BF1_PWL_SEG7", 2, 0x0d40 },
    { "DSCL0_DSCL_EASF_V_BF1_PWL_SEG0", 2, 0x0d41 },
    /* ---- page 21 starts here (index 240) ---- */
    { "DSCL0_DSCL_EASF_V_BF1_PWL_SEG1", 2, 0x0d42 },
    { "DSCL0_DSCL_EASF_V_BF1_PWL_SEG2", 2, 0x0d43 },
    { "DSCL0_DSCL_EASF_V_BF1_PWL_SEG3", 2, 0x0d44 },
    { "DSCL0_DSCL_EASF_V_BF1_PWL_SEG4", 2, 0x0d45 },
    { "DSCL0_DSCL_EASF_V_BF1_PWL_SEG5", 2, 0x0d46 },
    { "DSCL0_DSCL_EASF_V_BF1_PWL_SEG6", 2, 0x0d47 },
    { "DSCL0_DSCL_EASF_V_BF1_PWL_SEG7", 2, 0x0d48 },
    { "DSCL0_DSCL_EASF_H_BF3_PWL_SEG0", 2, 0x0d49 },
    { "DSCL0_DSCL_EASF_H_BF3_PWL_SEG1", 2, 0x0d4a },
    { "DSCL0_DSCL_EASF_H_BF3_PWL_SEG2", 2, 0x0d4b },
    { "DSCL0_DSCL_EASF_H_BF3_PWL_SEG3", 2, 0x0d4c },
    { "DSCL0_DSCL_EASF_H_BF3_PWL_SEG4", 2, 0x0d4d },
    { "DSCL0_DSCL_EASF_H_BF3_PWL_SEG5", 2, 0x0d4e },
    { "DSCL0_DSCL_EASF_V_BF3_PWL_SEG0", 2, 0x0d4f },
    { "DSCL0_DSCL_EASF_V_BF3_PWL_SEG1", 2, 0x0d50 },
    { "DSCL0_DSCL_EASF_V_BF3_PWL_SEG2", 2, 0x0d51 },
    { "DSCL0_DSCL_EASF_V_BF3_PWL_SEG3", 2, 0x0d52 },
    { "DSCL0_DSCL_EASF_V_BF3_PWL_SEG4", 2, 0x0d53 },
    { "DSCL0_DSCL_EASF_V_BF3_PWL_SEG5", 2, 0x0d54 },
    { "DSCL0_ISHARP_MODE", 2, 0x0d55 },
    { "DSCL0_ISHARP_DELTA_CTRL", 2, 0x0d56 },
    { "DSCL0_ISHARP_DELTA_INDEX", 2, 0x0d57 },
    { "DSCL0_ISHARP_NLDELTA_SOFT_CLIP", 2, 0x0d59 },
    { "DSCL0_ISHARP_NOISEDET_THRESHOLD", 2, 0x0d5a },
    /* ---- page 22 starts here (index 264) ---- */
    { "DSCL0_ISHARP_NOISE_GAIN_PWL", 2, 0x0d5b },
    { "DSCL0_ISHARP_LBA_PWL_SEG0", 2, 0x0d5c },
    { "DSCL0_ISHARP_LBA_PWL_SEG1", 2, 0x0d5d },
    { "DSCL0_ISHARP_LBA_PWL_SEG2", 2, 0x0d5e },
    { "DSCL0_ISHARP_LBA_PWL_SEG3", 2, 0x0d5f },
    { "DSCL0_ISHARP_LBA_PWL_SEG4", 2, 0x0d60 },
    { "DSCL0_ISHARP_LBA_PWL_SEG5", 2, 0x0d61 },
    { "DSCL0_ISHARP_DELTA_LUT_MEM_PWR_CTRL", 2, 0x0d62 },
    { "CM0_CM_CONTROL", 2, 0x0d67 },
    { "CM0_CM_POST_CSC_CONTROL", 2, 0x0d68 },
    { "CM0_CM_POST_CSC_C11_C12", 2, 0x0d69 },
    { "CM0_CM_POST_CSC_C13_C14", 2, 0x0d6a },
    { "CM0_CM_POST_CSC_C21_C22", 2, 0x0d6b },
    { "CM0_CM_POST_CSC_C23_C24", 2, 0x0d6c },
    { "CM0_CM_POST_CSC_C31_C32", 2, 0x0d6d },
    { "CM0_CM_POST_CSC_C33_C34", 2, 0x0d6e },
    { "CM0_CM_POST_CSC_B_C11_C12", 2, 0x0d6f },
    { "CM0_CM_POST_CSC_B_C13_C14", 2, 0x0d70 },
    { "CM0_CM_POST_CSC_B_C21_C22", 2, 0x0d71 },
    { "CM0_CM_POST_CSC_B_C23_C24", 2, 0x0d72 },
    { "CM0_CM_POST_CSC_B_C31_C32", 2, 0x0d73 },
    { "CM0_CM_POST_CSC_B_C33_C34", 2, 0x0d74 },
    { "CM0_CM_BIAS_CR_R", 2, 0x0d75 },
    { "CM0_CM_BIAS_Y_G_CB_B", 2, 0x0d76 },
    /* ---- page 23 starts here (index 288) ---- */
    { "CM0_CM_GAMCOR_CONTROL", 2, 0x0d77 },
    { "CM0_CM_GAMCOR_LUT_INDEX", 2, 0x0d78 },
    { "CM0_CM_GAMCOR_LUT_CONTROL", 2, 0x0d7a },
    { "CM0_CM_GAMCOR_RAMA_START_CNTL_B", 2, 0x0d7b },
    { "CM0_CM_GAMCOR_RAMA_START_CNTL_G", 2, 0x0d7c },
    { "CM0_CM_GAMCOR_RAMA_START_CNTL_R", 2, 0x0d7d },
    { "CM0_CM_GAMCOR_RAMA_START_SLOPE_CNTL_B", 2, 0x0d7e },
    { "CM0_CM_GAMCOR_RAMA_START_SLOPE_CNTL_G", 2, 0x0d7f },
    { "CM0_CM_GAMCOR_RAMA_START_SLOPE_CNTL_R", 2, 0x0d80 },
    { "CM0_CM_GAMCOR_RAMA_START_BASE_CNTL_B", 2, 0x0d81 },
    { "CM0_CM_GAMCOR_RAMA_START_BASE_CNTL_G", 2, 0x0d82 },
    { "CM0_CM_GAMCOR_RAMA_START_BASE_CNTL_R", 2, 0x0d83 },
    { "CM0_CM_GAMCOR_RAMA_END_CNTL1_B", 2, 0x0d84 },
    { "CM0_CM_GAMCOR_RAMA_END_CNTL2_B", 2, 0x0d85 },
    { "CM0_CM_GAMCOR_RAMA_END_CNTL1_G", 2, 0x0d86 },
    { "CM0_CM_GAMCOR_RAMA_END_CNTL2_G", 2, 0x0d87 },
    { "CM0_CM_GAMCOR_RAMA_END_CNTL1_R", 2, 0x0d88 },
    { "CM0_CM_GAMCOR_RAMA_END_CNTL2_R", 2, 0x0d89 },
    { "CM0_CM_GAMCOR_RAMA_OFFSET_B", 2, 0x0d8a },
    { "CM0_CM_GAMCOR_RAMA_OFFSET_G", 2, 0x0d8b },
    { "CM0_CM_GAMCOR_RAMA_OFFSET_R", 2, 0x0d8c },
    { "CM0_CM_GAMCOR_RAMA_REGION_0_1", 2, 0x0d8d },
    { "CM0_CM_GAMCOR_RAMA_REGION_2_3", 2, 0x0d8e },
    { "CM0_CM_GAMCOR_RAMA_REGION_4_5", 2, 0x0d8f },
    /* ---- page 24 starts here (index 312) ---- */
    { "CM0_CM_GAMCOR_RAMA_REGION_6_7", 2, 0x0d90 },
    { "CM0_CM_GAMCOR_RAMA_REGION_8_9", 2, 0x0d91 },
    { "CM0_CM_GAMCOR_RAMA_REGION_10_11", 2, 0x0d92 },
    { "CM0_CM_GAMCOR_RAMA_REGION_12_13", 2, 0x0d93 },
    { "CM0_CM_GAMCOR_RAMA_REGION_14_15", 2, 0x0d94 },
    { "CM0_CM_GAMCOR_RAMA_REGION_16_17", 2, 0x0d95 },
    { "CM0_CM_GAMCOR_RAMA_REGION_18_19", 2, 0x0d96 },
    { "CM0_CM_GAMCOR_RAMA_REGION_20_21", 2, 0x0d97 },
    { "CM0_CM_GAMCOR_RAMA_REGION_22_23", 2, 0x0d98 },
    { "CM0_CM_GAMCOR_RAMA_REGION_24_25", 2, 0x0d99 },
    { "CM0_CM_GAMCOR_RAMA_REGION_26_27", 2, 0x0d9a },
    { "CM0_CM_GAMCOR_RAMA_REGION_28_29", 2, 0x0d9b },
    { "CM0_CM_GAMCOR_RAMA_REGION_30_31", 2, 0x0d9c },
    { "CM0_CM_GAMCOR_RAMA_REGION_32_33", 2, 0x0d9d },
    { "CM0_CM_GAMCOR_RAMB_START_CNTL_B", 2, 0x0d9e },
    { "CM0_CM_GAMCOR_RAMB_START_CNTL_G", 2, 0x0d9f },
    { "CM0_CM_GAMCOR_RAMB_START_CNTL_R", 2, 0x0da0 },
    { "CM0_CM_GAMCOR_RAMB_START_SLOPE_CNTL_B", 2, 0x0da1 },
    { "CM0_CM_GAMCOR_RAMB_START_SLOPE_CNTL_G", 2, 0x0da2 },
    { "CM0_CM_GAMCOR_RAMB_START_SLOPE_CNTL_R", 2, 0x0da3 },
    { "CM0_CM_GAMCOR_RAMB_START_BASE_CNTL_B", 2, 0x0da4 },
    { "CM0_CM_GAMCOR_RAMB_START_BASE_CNTL_G", 2, 0x0da5 },
    { "CM0_CM_GAMCOR_RAMB_START_BASE_CNTL_R", 2, 0x0da6 },
    { "CM0_CM_GAMCOR_RAMB_END_CNTL1_B", 2, 0x0da7 },
    /* ---- page 25 starts here (index 336) ---- */
    { "CM0_CM_GAMCOR_RAMB_END_CNTL2_B", 2, 0x0da8 },
    { "CM0_CM_GAMCOR_RAMB_END_CNTL1_G", 2, 0x0da9 },
    { "CM0_CM_GAMCOR_RAMB_END_CNTL2_G", 2, 0x0daa },
    { "CM0_CM_GAMCOR_RAMB_END_CNTL1_R", 2, 0x0dab },
    { "CM0_CM_GAMCOR_RAMB_END_CNTL2_R", 2, 0x0dac },
    { "CM0_CM_GAMCOR_RAMB_OFFSET_B", 2, 0x0dad },
    { "CM0_CM_GAMCOR_RAMB_OFFSET_G", 2, 0x0dae },
    { "CM0_CM_GAMCOR_RAMB_OFFSET_R", 2, 0x0daf },
    { "CM0_CM_GAMCOR_RAMB_REGION_0_1", 2, 0x0db0 },
    { "CM0_CM_GAMCOR_RAMB_REGION_2_3", 2, 0x0db1 },
    { "CM0_CM_GAMCOR_RAMB_REGION_4_5", 2, 0x0db2 },
    { "CM0_CM_GAMCOR_RAMB_REGION_6_7", 2, 0x0db3 },
    { "CM0_CM_GAMCOR_RAMB_REGION_8_9", 2, 0x0db4 },
    { "CM0_CM_GAMCOR_RAMB_REGION_10_11", 2, 0x0db5 },
    { "CM0_CM_GAMCOR_RAMB_REGION_12_13", 2, 0x0db6 },
    { "CM0_CM_GAMCOR_RAMB_REGION_14_15", 2, 0x0db7 },
    { "CM0_CM_GAMCOR_RAMB_REGION_16_17", 2, 0x0db8 },
    { "CM0_CM_GAMCOR_RAMB_REGION_18_19", 2, 0x0db9 },
    { "CM0_CM_GAMCOR_RAMB_REGION_20_21", 2, 0x0dba },
    { "CM0_CM_GAMCOR_RAMB_REGION_22_23", 2, 0x0dbb },
    { "CM0_CM_GAMCOR_RAMB_REGION_24_25", 2, 0x0dbc },
    { "CM0_CM_GAMCOR_RAMB_REGION_26_27", 2, 0x0dbd },
    { "CM0_CM_GAMCOR_RAMB_REGION_28_29", 2, 0x0dbe },
    { "CM0_CM_GAMCOR_RAMB_REGION_30_31", 2, 0x0dbf },
    /* ---- page 26 starts here (index 360) ---- */
    { "CM0_CM_GAMCOR_RAMB_REGION_32_33", 2, 0x0dc0 },
    { "CM0_CM_HDR_MULT_COEF", 2, 0x0dc1 },
    { "CM0_CM_MEM_PWR_CTRL", 2, 0x0dc2 },
    { "CM0_CM_MEM_PWR_STATUS", 2, 0x0dc3 },
    { "CM0_CM_DEALPHA", 2, 0x0dc5 },
    { "CM0_CM_COEF_FORMAT", 2, 0x0dc6 },
    { "CM0_CM_TEST_DEBUG_INDEX", 2, 0x0dc7 },
    { "MPCC0_MPCC_TOP_SEL", 3, 0x0000 },
    { "MPCC0_MPCC_BOT_SEL", 3, 0x0001 },
    { "MPCC0_MPCC_OPP_ID", 3, 0x0002 },
    { "MPCC0_MPCC_CONTROL", 3, 0x0003 },
    { "MPCC0_MPCC_SM_CONTROL", 3, 0x0004 },
    { "MPCC0_MPCC_UPDATE_LOCK_SEL", 3, 0x0005 },
    { "MPCC0_MPCC_TOP_GAIN", 3, 0x0006 },
    { "MPCC0_MPCC_BOT_GAIN_INSIDE", 3, 0x0007 },
    { "MPCC0_MPCC_BOT_GAIN_OUTSIDE", 3, 0x0008 },
    { "MPCC0_MPCC_MOVABLE_CM_LOCATION_CONTROL", 3, 0x0009 },
    { "MPCC0_MPCC_BG_R_CR", 3, 0x000a },
    { "MPCC0_MPCC_BG_G_Y", 3, 0x000b },
    { "MPCC0_MPCC_BG_B_CB", 3, 0x000c },
    { "MPCC0_MPCC_MEM_PWR_CTRL", 3, 0x000d },
    { "MPCC0_MPCC_STATUS", 3, 0x000e },
    { "MPCC1_MPCC_TOP_SEL", 3, 0x0015 },
    { "MPCC1_MPCC_BOT_SEL", 3, 0x0016 },
    /* ---- page 27 starts here (index 384) ---- */
    { "MPCC1_MPCC_OPP_ID", 3, 0x0017 },
    { "MPCC1_MPCC_CONTROL", 3, 0x0018 },
    { "MPCC1_MPCC_SM_CONTROL", 3, 0x0019 },
    { "MPCC1_MPCC_UPDATE_LOCK_SEL", 3, 0x001a },
    { "MPCC1_MPCC_TOP_GAIN", 3, 0x001b },
    { "MPCC1_MPCC_BOT_GAIN_INSIDE", 3, 0x001c },
    { "MPCC1_MPCC_BOT_GAIN_OUTSIDE", 3, 0x001d },
    { "MPCC1_MPCC_MOVABLE_CM_LOCATION_CONTROL", 3, 0x001e },
    { "MPCC1_MPCC_BG_R_CR", 3, 0x001f },
    { "MPCC1_MPCC_BG_G_Y", 3, 0x0020 },
    { "MPCC1_MPCC_BG_B_CB", 3, 0x0021 },
    { "MPCC1_MPCC_MEM_PWR_CTRL", 3, 0x0022 },
    { "MPCC1_MPCC_STATUS", 3, 0x0023 },
    { "MPCC2_MPCC_TOP_SEL", 3, 0x002a },
    { "MPCC2_MPCC_BOT_SEL", 3, 0x002b },
    { "MPCC2_MPCC_OPP_ID", 3, 0x002c },
    { "MPCC2_MPCC_CONTROL", 3, 0x002d },
    { "MPCC2_MPCC_SM_CONTROL", 3, 0x002e },
    { "MPCC2_MPCC_UPDATE_LOCK_SEL", 3, 0x002f },
    { "MPCC2_MPCC_TOP_GAIN", 3, 0x0030 },
    { "MPCC2_MPCC_BOT_GAIN_INSIDE", 3, 0x0031 },
    { "MPCC2_MPCC_BOT_GAIN_OUTSIDE", 3, 0x0032 },
    { "MPCC2_MPCC_MOVABLE_CM_LOCATION_CONTROL", 3, 0x0033 },
    { "MPCC2_MPCC_BG_R_CR", 3, 0x0034 },
    /* ---- page 28 starts here (index 408) ---- */
    { "MPCC2_MPCC_BG_G_Y", 3, 0x0035 },
    { "MPCC2_MPCC_BG_B_CB", 3, 0x0036 },
    { "MPCC2_MPCC_MEM_PWR_CTRL", 3, 0x0037 },
    { "MPCC2_MPCC_STATUS", 3, 0x0038 },
    { "MPC_OUT0_MUX", 3, 0x02f2 },
    { "MPC_OUT0_DENORM_CONTROL", 3, 0x02f3 },
    { "MPC_OUT0_DENORM_CLAMP_G_Y", 3, 0x02f4 },
    { "MPC_OUT0_DENORM_CLAMP_B_CB", 3, 0x02f5 },
    { "MPC_OUT0_CSC_MODE", 3, 0x030b },
    { "MPC_OUT2_MUX", 3, 0x02fa },
    { "MPC_OUT2_DENORM_CONTROL", 3, 0x02fb },
    { "MPC_OUT2_DENORM_CLAMP_G_Y", 3, 0x02fc },
    { "MPC_OUT2_DENORM_CLAMP_B_CB", 3, 0x02fd },
    { "MPC_OUT2_CSC_MODE", 3, 0x0325 },
    { "OTG2_OTG_GLOBAL_CONTROL2", 2, 0x1c90 },
    { "HUBP2_DCSURF_SURFACE_CONFIG", 2, 0x079d },
    { "HUBP2_DCSURF_ADDR_CONFIG", 2, 0x079e },
    { "HUBP2_DCSURF_TILING_CONFIG", 2, 0x079f },
    { "HUBP2_DCSURF_PRI_VIEWPORT_START", 2, 0x07a1 },
    { "HUBP2_DCSURF_VIEWPORT_MCACHE_SPLIT_COORDINATE", 2, 0x07a2 },
    { "HUBP2_DCSURF_PRI_VIEWPORT_DIMENSION", 2, 0x07a3 },
    { "HUBP2_DCSURF_PRI_VIEWPORT_START_C", 2, 0x07a4 },
    { "HUBP2_DCSURF_PRI_VIEWPORT_DIMENSION_C", 2, 0x07a5 },
    { "HUBP2_DCSURF_SEC_VIEWPORT_START", 2, 0x07a6 },
    /* ---- page 29 starts here (index 432) ---- */
    { "HUBP2_DCSURF_SEC_VIEWPORT_DIMENSION", 2, 0x07a7 },
    { "HUBP2_DCSURF_SEC_VIEWPORT_START_C", 2, 0x07a8 },
    { "HUBP2_DCSURF_SEC_VIEWPORT_DIMENSION_C", 2, 0x07a9 },
    { "HUBP2_DCHUBP_REQ_SIZE_CONFIG", 2, 0x07aa },
    { "HUBP2_DCHUBP_REQ_SIZE_CONFIG_C", 2, 0x07ab },
    { "HUBP2_DCHUBP_CNTL", 2, 0x07ac },
    { "HUBP2_HUBP_CLK_CNTL", 2, 0x07ad },
    { "HUBP2_DCHUBP_VMPG_CONFIG", 2, 0x07ae },
    { "HUBP2_DCHUBP_MALL_CONFIG", 2, 0x07af },
    { "HUBP2_DCHUBP_MALL_SUB_VP0", 2, 0x07b0 },
    { "HUBP2_DCHUBP_MALL_SUB_VP1", 2, 0x07b1 },
    { "HUBP2_DCHUBP_MALL_SUB_VP2", 2, 0x07b2 },
    { "HUBP2_DCHUBP_MCACHEID_CONFIG", 2, 0x07b3 },
    { "HUBP2_HUBPREQ_DEBUG_DB", 2, 0x07b4 },
    { "HUBP2_HUBPREQ_DEBUG", 2, 0x07b5 },
    { "HUBP2_HUBP_DEBUG_CTRL", 2, 0x07b6 },
    { "HUBP2_HUBP_DEBUG_MUX_DCFCLK", 2, 0x07b7 },
    { "HUBP2_HUBP_DEBUG_MUX_DPPCLK", 2, 0x07b8 },
    { "HUBP2_HUBP_MEASURE_WIN_CTRL_DCFCLK", 2, 0x07b9 },
    { "HUBP2_HUBP_MEASURE_WIN_CTRL_DPPCLK", 2, 0x07ba },
    { "HUBP2_HUBP_MALL_STATUS", 2, 0x07bb },
    { "HUBP2_3DLUT_FL_CONFIG", 3, 0x02d9 },
    { "HUBP2_3DLUT_FL_BIAS_SCALE", 3, 0x02da },
    { "HUBPREQ2_DCSURF_SURFACE_PITCH", 2, 0x07bf },
    /* ---- page 30 starts here (index 456) ---- */
    { "HUBPREQ2_DCSURF_SURFACE_PITCH_C", 2, 0x07c0 },
    { "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS", 2, 0x07c2 },
    { "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 2, 0x07c3 },
    { "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_C", 2, 0x07c4 },
    { "HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH_C", 2, 0x07c5 },
    { "HUBPREQ2_DCSURF_SECONDARY_SURFACE_ADDRESS", 2, 0x07c6 },
    { "HUBPREQ2_DCSURF_SECONDARY_SURFACE_ADDRESS_HIGH", 2, 0x07c7 },
    { "HUBPREQ2_DCSURF_SECONDARY_SURFACE_ADDRESS_C", 2, 0x07c8 },
    { "HUBPREQ2_DCSURF_SECONDARY_SURFACE_ADDRESS_HIGH_C", 2, 0x07c9 },
    { "HUBPREQ2_DCSURF_SURFACE_CONTROL", 2, 0x07ca },
    { "HUBPREQ2_DCSURF_FLIP_CONTROL", 2, 0x07cb },
    { "HUBPREQ2_DCSURF_FLIP_CONTROL2", 2, 0x07cc },
    { "HUBPREQ2_DCSURF_SURFACE_FLIP_INTERRUPT", 2, 0x07cf },
    { "HUBPREQ2_DCSURF_SURFACE_INUSE", 2, 0x07d0 },
    { "HUBPREQ2_DCSURF_SURFACE_INUSE_HIGH", 2, 0x07d1 },
    { "HUBPREQ2_DCSURF_SURFACE_INUSE_C", 2, 0x07d2 },
    { "HUBPREQ2_DCSURF_SURFACE_INUSE_HIGH_C", 2, 0x07d3 },
    { "HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE", 2, 0x07d4 },
    { "HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH", 2, 0x07d5 },
    { "HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_C", 2, 0x07d6 },
    { "HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH_C", 2, 0x07d7 },
    { "HUBPREQ2_DCN_EXPANSION_MODE", 2, 0x07d8 },
    { "HUBPREQ2_DCN_TTU_QOS_WM", 2, 0x07d9 },
    { "HUBPREQ2_DCN_GLOBAL_TTU_CNTL", 2, 0x07da },
    /* ---- page 31 starts here (index 480) ---- */
    { "HUBPREQ2_DCN_SURF0_TTU_CNTL0", 2, 0x07db },
    { "HUBPREQ2_DCN_SURF0_TTU_CNTL1", 2, 0x07dc },
    { "HUBPREQ2_DCN_SURF1_TTU_CNTL0", 2, 0x07dd },
    { "HUBPREQ2_DCN_SURF1_TTU_CNTL1", 2, 0x07de },
    { "HUBPREQ2_DCN_CUR0_TTU_CNTL0", 2, 0x07df },
    { "HUBPREQ2_DCN_CUR0_TTU_CNTL1", 2, 0x07e0 },
    { "HUBPREQ2_DCN_CUR1_TTU_CNTL0", 2, 0x07e1 },
    { "HUBPREQ2_DCN_CUR1_TTU_CNTL1", 2, 0x07e2 },
    { "HUBPREQ2_DCN_DMDATA_VM_CNTL", 2, 0x07e3 },
    { "HUBPREQ2_BLANK_OFFSET_0", 2, 0x07f3 },
    { "HUBPREQ2_BLANK_OFFSET_1", 2, 0x07f4 },
    { "HUBPREQ2_DST_DIMENSIONS", 2, 0x07f5 },
    { "HUBPREQ2_DST_AFTER_SCALER", 2, 0x07f6 },
    { "HUBPREQ2_PREFETCH_SETTINGS", 2, 0x07f7 },
    { "HUBPREQ2_PREFETCH_SETTINGS_C", 2, 0x07f8 },
    { "HUBPREQ2_VBLANK_PARAMETERS_0", 2, 0x07f9 },
    { "HUBPREQ2_VBLANK_PARAMETERS_1", 2, 0x07fa },
    { "HUBPREQ2_VBLANK_PARAMETERS_2", 2, 0x07fb },
    { "HUBPREQ2_VBLANK_PARAMETERS_3", 2, 0x07fc },
    { "HUBPREQ2_VBLANK_PARAMETERS_4", 2, 0x07fd },
    { "HUBPREQ2_FLIP_PARAMETERS_0", 2, 0x07fe },
    { "HUBPREQ2_FLIP_PARAMETERS_1", 2, 0x07ff },
    { "HUBPREQ2_FLIP_PARAMETERS_2", 2, 0x0800 },
    { "HUBPREQ2_NOM_PARAMETERS_0", 2, 0x0801 },
    /* ---- page 32 starts here (index 504) ---- */
    { "HUBPREQ2_NOM_PARAMETERS_1", 2, 0x0802 },
    { "HUBPREQ2_NOM_PARAMETERS_2", 2, 0x0803 },
    { "HUBPREQ2_NOM_PARAMETERS_3", 2, 0x0804 },
    { "HUBPREQ2_NOM_PARAMETERS_4", 2, 0x0805 },
    { "HUBPREQ2_NOM_PARAMETERS_5", 2, 0x0806 },
    { "HUBPREQ2_NOM_PARAMETERS_6", 2, 0x0807 },
    { "HUBPREQ2_NOM_PARAMETERS_7", 2, 0x0808 },
    { "HUBPREQ2_PER_LINE_DELIVERY_PRE", 2, 0x0809 },
    { "HUBPREQ2_PER_LINE_DELIVERY", 2, 0x080a },
    { "HUBPREQ2_CURSOR_SETTINGS", 2, 0x080b },
    { "HUBPREQ2_REF_FREQ_TO_PIX_FREQ", 2, 0x080c },
    { "HUBPREQ2_DST_Y_DELTA_DRQ_LIMIT", 2, 0x080d },
    { "HUBPREQ2_HUBPREQ_MEM_PWR_CTRL", 2, 0x080e },
    { "HUBPREQ2_HUBPREQ_MEM_PWR_STATUS", 2, 0x080f },
    { "HUBPREQ2_VBLANK_PARAMETERS_5", 2, 0x0812 },
    { "HUBPREQ2_VBLANK_PARAMETERS_6", 2, 0x0813 },
    { "HUBPREQ2_FLIP_PARAMETERS_3", 2, 0x0814 },
    { "HUBPREQ2_FLIP_PARAMETERS_4", 2, 0x0815 },
    { "HUBPREQ2_FLIP_PARAMETERS_5", 2, 0x0816 },
    { "HUBPREQ2_FLIP_PARAMETERS_6", 2, 0x0817 },
    { "HUBPREQ2_UCLK_PSTATE_FORCE", 2, 0x0818 },
    { "HUBPREQ2_HUBPREQ_STATUS_REG0", 2, 0x0819 },
    { "HUBPREQ2_HUBPREQ_STATUS_REG1", 2, 0x081a },
    { "HUBPREQ2_HUBPREQ_STATUS_REG2", 2, 0x081b },
    /* ---- page 33 starts here (index 528) ---- */
    { "HUBPREQ2_HUBPREQ_STATUS_REG3", 2, 0x081c },
    { "HUBPRET2_HUBPRET_CONTROL", 2, 0x0825 },
    { "HUBPRET2_HUBPRET_MEM_PWR_CTRL", 2, 0x0826 },
    { "HUBPRET2_HUBPRET_MEM_PWR_STATUS", 2, 0x0827 },
    { "HUBPRET2_HUBPRET_READ_LINE_CTRL0", 2, 0x0828 },
    { "HUBPRET2_HUBPRET_READ_LINE_CTRL1", 2, 0x0829 },
    { "HUBPRET2_HUBPRET_READ_LINE0", 2, 0x082a },
    { "HUBPRET2_HUBPRET_READ_LINE1", 2, 0x082b },
    { "HUBPRET2_HUBPRET_INTERRUPT", 2, 0x082c },
    { "HUBPRET2_HUBPRET_READ_LINE_VALUE", 2, 0x082d },
    { "HUBPRET2_HUBPRET_READ_LINE_STATUS", 2, 0x082e },
    { "DPP_TOP2_DPP_CONTROL", 2, 0x0f9b },
    { "DPP_TOP2_DPP_SOFT_RESET", 2, 0x0f9c },
    { "DPP_TOP2_DPP_CRC_VAL_R_G", 2, 0x0f9d },
    { "DPP_TOP2_DPP_CRC_VAL_B_A", 2, 0x0f9e },
    { "DPP_TOP2_DPP_CRC_CTRL", 2, 0x0f9f },
    { "DPP_TOP2_HOST_READ_CONTROL", 2, 0x0fa0 },
    { "CNVC_CFG2_CNVC_SURFACE_PIXEL_FORMAT", 2, 0x0fa5 },
    { "CNVC_CFG2_FORMAT_CONTROL", 2, 0x0fa6 },
    { "CNVC_CFG2_FCNV_FP_BIAS_R", 2, 0x0fa7 },
    { "CNVC_CFG2_FCNV_FP_BIAS_G", 2, 0x0fa8 },
    { "CNVC_CFG2_FCNV_FP_BIAS_B", 2, 0x0fa9 },
    { "CNVC_CFG2_FCNV_FP_SCALE_R", 2, 0x0faa },
    { "CNVC_CFG2_FCNV_FP_SCALE_G", 2, 0x0fab },
    /* ---- page 34 starts here (index 552) ---- */
    { "CNVC_CFG2_FCNV_FP_SCALE_B", 2, 0x0fac },
    { "CNVC_CFG2_COLOR_KEYER_CONTROL", 2, 0x0fad },
    { "CNVC_CFG2_COLOR_KEYER_ALPHA", 2, 0x0fae },
    { "CNVC_CFG2_COLOR_KEYER_RED", 2, 0x0faf },
    { "CNVC_CFG2_COLOR_KEYER_GREEN", 2, 0x0fb0 },
    { "CNVC_CFG2_COLOR_KEYER_BLUE", 2, 0x0fb1 },
    { "CNVC_CFG2_ALPHA_2BIT_LUT", 2, 0x0fb3 },
    { "CNVC_CFG2_PRE_DEALPHA", 2, 0x0fb4 },
    { "CNVC_CFG2_PRE_CSC_MODE", 2, 0x0fb5 },
    { "CNVC_CFG2_PRE_CSC_C11_C12", 2, 0x0fb6 },
    { "CNVC_CFG2_PRE_CSC_C13_C14", 2, 0x0fb7 },
    { "CNVC_CFG2_PRE_CSC_C21_C22", 2, 0x0fb8 },
    { "CNVC_CFG2_PRE_CSC_C23_C24", 2, 0x0fb9 },
    { "CNVC_CFG2_PRE_CSC_C31_C32", 2, 0x0fba },
    { "CNVC_CFG2_PRE_CSC_C33_C34", 2, 0x0fbb },
    { "CNVC_CFG2_PRE_CSC_B_C11_C12", 2, 0x0fbc },
    { "CNVC_CFG2_PRE_CSC_B_C13_C14", 2, 0x0fbd },
    { "CNVC_CFG2_PRE_CSC_B_C21_C22", 2, 0x0fbe },
    { "CNVC_CFG2_PRE_CSC_B_C23_C24", 2, 0x0fbf },
    { "CNVC_CFG2_PRE_CSC_B_C31_C32", 2, 0x0fc0 },
    { "CNVC_CFG2_PRE_CSC_B_C33_C34", 2, 0x0fc1 },
    { "CNVC_CFG2_CNVC_COEF_FORMAT", 2, 0x0fc2 },
    { "CNVC_CFG2_PRE_DEGAM", 2, 0x0fc3 },
    { "CNVC_CFG2_PRE_REALPHA", 2, 0x0fc4 },
    /* ---- page 35 starts here (index 576) ---- */
    { "DSCL2_SCL_COEF_RAM_TAP_SELECT", 2, 0x0fdc },
    { "DSCL2_SCL_MODE", 2, 0x0fde },
    { "DSCL2_SCL_TAP_CONTROL", 2, 0x0fdf },
    { "DSCL2_DSCL_CONTROL", 2, 0x0fe0 },
    { "DSCL2_DSCL_2TAP_CONTROL", 2, 0x0fe1 },
    { "DSCL2_SCL_MANUAL_REPLICATE_CONTROL", 2, 0x0fe2 },
    { "DSCL2_SCL_HORZ_FILTER_SCALE_RATIO", 2, 0x0fe3 },
    { "DSCL2_SCL_HORZ_FILTER_INIT", 2, 0x0fe4 },
    { "DSCL2_SCL_HORZ_FILTER_SCALE_RATIO_C", 2, 0x0fe5 },
    { "DSCL2_SCL_HORZ_FILTER_INIT_C", 2, 0x0fe6 },
    { "DSCL2_SCL_VERT_FILTER_SCALE_RATIO", 2, 0x0fe7 },
    { "DSCL2_SCL_VERT_FILTER_INIT", 2, 0x0fe8 },
    { "DSCL2_SCL_VERT_FILTER_INIT_BOT", 2, 0x0fe9 },
    { "DSCL2_SCL_VERT_FILTER_SCALE_RATIO_C", 2, 0x0fea },
    { "DSCL2_SCL_VERT_FILTER_INIT_C", 2, 0x0feb },
    { "DSCL2_SCL_VERT_FILTER_INIT_BOT_C", 2, 0x0fec },
    { "DSCL2_SCL_BLACK_COLOR", 2, 0x0fed },
    { "DSCL2_DSCL_UPDATE", 2, 0x0fee },
    { "DSCL2_DSCL_AUTOCAL", 2, 0x0fef },
    { "DSCL2_DSCL_EXT_OVERSCAN_LEFT_RIGHT", 2, 0x0ff0 },
    { "DSCL2_DSCL_EXT_OVERSCAN_TOP_BOTTOM", 2, 0x0ff1 },
    { "DSCL2_OTG_H_BLANK", 2, 0x0ff2 },
    { "DSCL2_OTG_V_BLANK", 2, 0x0ff3 },
    { "DSCL2_RECOUT_START", 2, 0x0ff4 },
    /* ---- page 36 starts here (index 600) ---- */
    { "DSCL2_RECOUT_SIZE", 2, 0x0ff5 },
    { "DSCL2_MPC_SIZE", 2, 0x0ff6 },
    { "DSCL2_LB_DATA_FORMAT", 2, 0x0ff7 },
    { "DSCL2_LB_MEMORY_CTRL", 2, 0x0ff8 },
    { "DSCL2_LB_V_COUNTER", 2, 0x0ff9 },
    { "DSCL2_DSCL_MEM_PWR_CTRL", 2, 0x0ffa },
    { "DSCL2_DSCL_MEM_PWR_STATUS", 2, 0x0ffb },
    { "DSCL2_OBUF_CONTROL", 2, 0x0ffc },
    { "DSCL2_OBUF_MEM_PWR_CTRL", 2, 0x0ffd },
    { "DSCL2_DSCL_EASF_H_MODE", 2, 0x0ffe },
    { "DSCL2_DSCL_EASF_V_MODE", 2, 0x0fff },
    { "DSCL2_DSCL_SC_MODE", 2, 0x1000 },
    { "DSCL2_DSCL_SC_MATRIX_C0C1", 2, 0x1001 },
    { "DSCL2_DSCL_SC_MATRIX_C2C3", 2, 0x1002 },
    { "DSCL2_DSCL_EASF_H_RINGEST_EVENTAP_GAIN", 2, 0x1003 },
    { "DSCL2_DSCL_EASF_H_RINGEST_EVENTAP_REDUCE", 2, 0x1004 },
    { "DSCL2_DSCL_EASF_V_RINGEST_EVENTAP_GAIN", 2, 0x1005 },
    { "DSCL2_DSCL_EASF_V_RINGEST_EVENTAP_REDUCE", 2, 0x1006 },
    { "DSCL2_DSCL_EASF_V_RINGEST_3TAP_CNTL1", 2, 0x1007 },
    { "DSCL2_DSCL_EASF_V_RINGEST_3TAP_CNTL2", 2, 0x1008 },
    { "DSCL2_DSCL_EASF_V_RINGEST_3TAP_CNTL3", 2, 0x1009 },
    { "DSCL2_DSCL_EASF_RINGEST_FORCE", 2, 0x100a },
    { "DSCL2_DSCL_EASF_H_BF_CNTL", 2, 0x100b },
    { "DSCL2_DSCL_EASF_H_BF_FINAL_MAX_MIN", 2, 0x100c },
    /* ---- page 37 starts here (index 624) ---- */
    { "DSCL2_DSCL_EASF_V_BF_CNTL", 2, 0x100d },
    { "DSCL2_DSCL_EASF_V_BF_FINAL_MAX_MIN", 2, 0x100e },
    { "DSCL2_DSCL_EASF_H_BF1_PWL_SEG0", 2, 0x100f },
    { "DSCL2_DSCL_EASF_H_BF1_PWL_SEG1", 2, 0x1010 },
    { "DSCL2_DSCL_EASF_H_BF1_PWL_SEG2", 2, 0x1011 },
    { "DSCL2_DSCL_EASF_H_BF1_PWL_SEG3", 2, 0x1012 },
    { "DSCL2_DSCL_EASF_H_BF1_PWL_SEG4", 2, 0x1013 },
    { "DSCL2_DSCL_EASF_H_BF1_PWL_SEG5", 2, 0x1014 },
    { "DSCL2_DSCL_EASF_H_BF1_PWL_SEG6", 2, 0x1015 },
    { "DSCL2_DSCL_EASF_H_BF1_PWL_SEG7", 2, 0x1016 },
    { "DSCL2_DSCL_EASF_V_BF1_PWL_SEG0", 2, 0x1017 },
    { "DSCL2_DSCL_EASF_V_BF1_PWL_SEG1", 2, 0x1018 },
    { "DSCL2_DSCL_EASF_V_BF1_PWL_SEG2", 2, 0x1019 },
    { "DSCL2_DSCL_EASF_V_BF1_PWL_SEG3", 2, 0x101a },
    { "DSCL2_DSCL_EASF_V_BF1_PWL_SEG4", 2, 0x101b },
    { "DSCL2_DSCL_EASF_V_BF1_PWL_SEG5", 2, 0x101c },
    { "DSCL2_DSCL_EASF_V_BF1_PWL_SEG6", 2, 0x101d },
    { "DSCL2_DSCL_EASF_V_BF1_PWL_SEG7", 2, 0x101e },
    { "DSCL2_DSCL_EASF_H_BF3_PWL_SEG0", 2, 0x101f },
    { "DSCL2_DSCL_EASF_H_BF3_PWL_SEG1", 2, 0x1020 },
    { "DSCL2_DSCL_EASF_H_BF3_PWL_SEG2", 2, 0x1021 },
    { "DSCL2_DSCL_EASF_H_BF3_PWL_SEG3", 2, 0x1022 },
    { "DSCL2_DSCL_EASF_H_BF3_PWL_SEG4", 2, 0x1023 },
    { "DSCL2_DSCL_EASF_H_BF3_PWL_SEG5", 2, 0x1024 },
    /* ---- page 38 starts here (index 648) ---- */
    { "DSCL2_DSCL_EASF_V_BF3_PWL_SEG0", 2, 0x1025 },
    { "DSCL2_DSCL_EASF_V_BF3_PWL_SEG1", 2, 0x1026 },
    { "DSCL2_DSCL_EASF_V_BF3_PWL_SEG2", 2, 0x1027 },
    { "DSCL2_DSCL_EASF_V_BF3_PWL_SEG3", 2, 0x1028 },
    { "DSCL2_DSCL_EASF_V_BF3_PWL_SEG4", 2, 0x1029 },
    { "DSCL2_DSCL_EASF_V_BF3_PWL_SEG5", 2, 0x102a },
    { "DSCL2_ISHARP_MODE", 2, 0x102b },
    { "DSCL2_ISHARP_DELTA_CTRL", 2, 0x102c },
    { "DSCL2_ISHARP_DELTA_INDEX", 2, 0x102d },
    { "DSCL2_ISHARP_NLDELTA_SOFT_CLIP", 2, 0x102f },
    { "DSCL2_ISHARP_NOISEDET_THRESHOLD", 2, 0x1030 },
    { "DSCL2_ISHARP_NOISE_GAIN_PWL", 2, 0x1031 },
    { "DSCL2_ISHARP_LBA_PWL_SEG0", 2, 0x1032 },
    { "DSCL2_ISHARP_LBA_PWL_SEG1", 2, 0x1033 },
    { "DSCL2_ISHARP_LBA_PWL_SEG2", 2, 0x1034 },
    { "DSCL2_ISHARP_LBA_PWL_SEG3", 2, 0x1035 },
    { "DSCL2_ISHARP_LBA_PWL_SEG4", 2, 0x1036 },
    { "DSCL2_ISHARP_LBA_PWL_SEG5", 2, 0x1037 },
    { "DSCL2_ISHARP_DELTA_LUT_MEM_PWR_CTRL", 2, 0x1038 },
    { "CM2_CM_CONTROL", 2, 0x103d },
    { "CM2_CM_POST_CSC_CONTROL", 2, 0x103e },
    { "CM2_CM_POST_CSC_C11_C12", 2, 0x103f },
    { "CM2_CM_POST_CSC_C13_C14", 2, 0x1040 },
    { "CM2_CM_POST_CSC_C21_C22", 2, 0x1041 },
    /* ---- page 39 starts here (index 672) ---- */
    { "CM2_CM_POST_CSC_C23_C24", 2, 0x1042 },
    { "CM2_CM_POST_CSC_C31_C32", 2, 0x1043 },
    { "CM2_CM_POST_CSC_C33_C34", 2, 0x1044 },
    { "CM2_CM_POST_CSC_B_C11_C12", 2, 0x1045 },
    { "CM2_CM_POST_CSC_B_C13_C14", 2, 0x1046 },
    { "CM2_CM_POST_CSC_B_C21_C22", 2, 0x1047 },
    { "CM2_CM_POST_CSC_B_C23_C24", 2, 0x1048 },
    { "CM2_CM_POST_CSC_B_C31_C32", 2, 0x1049 },
    { "CM2_CM_POST_CSC_B_C33_C34", 2, 0x104a },
    { "CM2_CM_BIAS_CR_R", 2, 0x104b },
    { "CM2_CM_BIAS_Y_G_CB_B", 2, 0x104c },
    { "CM2_CM_GAMCOR_CONTROL", 2, 0x104d },
    { "CM2_CM_GAMCOR_LUT_INDEX", 2, 0x104e },
    { "CM2_CM_GAMCOR_LUT_CONTROL", 2, 0x1050 },
    { "CM2_CM_GAMCOR_RAMA_START_CNTL_B", 2, 0x1051 },
    { "CM2_CM_GAMCOR_RAMA_START_CNTL_G", 2, 0x1052 },
    { "CM2_CM_GAMCOR_RAMA_START_CNTL_R", 2, 0x1053 },
    { "CM2_CM_GAMCOR_RAMA_START_SLOPE_CNTL_B", 2, 0x1054 },
    { "CM2_CM_GAMCOR_RAMA_START_SLOPE_CNTL_G", 2, 0x1055 },
    { "CM2_CM_GAMCOR_RAMA_START_SLOPE_CNTL_R", 2, 0x1056 },
    { "CM2_CM_GAMCOR_RAMA_START_BASE_CNTL_B", 2, 0x1057 },
    { "CM2_CM_GAMCOR_RAMA_START_BASE_CNTL_G", 2, 0x1058 },
    { "CM2_CM_GAMCOR_RAMA_START_BASE_CNTL_R", 2, 0x1059 },
    { "CM2_CM_GAMCOR_RAMA_END_CNTL1_B", 2, 0x105a },
    /* ---- page 40 starts here (index 696) ---- */
    { "CM2_CM_GAMCOR_RAMA_END_CNTL2_B", 2, 0x105b },
    { "CM2_CM_GAMCOR_RAMA_END_CNTL1_G", 2, 0x105c },
    { "CM2_CM_GAMCOR_RAMA_END_CNTL2_G", 2, 0x105d },
    { "CM2_CM_GAMCOR_RAMA_END_CNTL1_R", 2, 0x105e },
    { "CM2_CM_GAMCOR_RAMA_END_CNTL2_R", 2, 0x105f },
    { "CM2_CM_GAMCOR_RAMA_OFFSET_B", 2, 0x1060 },
    { "CM2_CM_GAMCOR_RAMA_OFFSET_G", 2, 0x1061 },
    { "CM2_CM_GAMCOR_RAMA_OFFSET_R", 2, 0x1062 },
    { "CM2_CM_GAMCOR_RAMA_REGION_0_1", 2, 0x1063 },
    { "CM2_CM_GAMCOR_RAMA_REGION_2_3", 2, 0x1064 },
    { "CM2_CM_GAMCOR_RAMA_REGION_4_5", 2, 0x1065 },
    { "CM2_CM_GAMCOR_RAMA_REGION_6_7", 2, 0x1066 },
    { "CM2_CM_GAMCOR_RAMA_REGION_8_9", 2, 0x1067 },
    { "CM2_CM_GAMCOR_RAMA_REGION_10_11", 2, 0x1068 },
    { "CM2_CM_GAMCOR_RAMA_REGION_12_13", 2, 0x1069 },
    { "CM2_CM_GAMCOR_RAMA_REGION_14_15", 2, 0x106a },
    { "CM2_CM_GAMCOR_RAMA_REGION_16_17", 2, 0x106b },
    { "CM2_CM_GAMCOR_RAMA_REGION_18_19", 2, 0x106c },
    { "CM2_CM_GAMCOR_RAMA_REGION_20_21", 2, 0x106d },
    { "CM2_CM_GAMCOR_RAMA_REGION_22_23", 2, 0x106e },
    { "CM2_CM_GAMCOR_RAMA_REGION_24_25", 2, 0x106f },
    { "CM2_CM_GAMCOR_RAMA_REGION_26_27", 2, 0x1070 },
    { "CM2_CM_GAMCOR_RAMA_REGION_28_29", 2, 0x1071 },
    { "CM2_CM_GAMCOR_RAMA_REGION_30_31", 2, 0x1072 },
    /* ---- page 41 starts here (index 720) ---- */
    { "CM2_CM_GAMCOR_RAMA_REGION_32_33", 2, 0x1073 },
    { "CM2_CM_GAMCOR_RAMB_START_CNTL_B", 2, 0x1074 },
    { "CM2_CM_GAMCOR_RAMB_START_CNTL_G", 2, 0x1075 },
    { "CM2_CM_GAMCOR_RAMB_START_CNTL_R", 2, 0x1076 },
    { "CM2_CM_GAMCOR_RAMB_START_SLOPE_CNTL_B", 2, 0x1077 },
    { "CM2_CM_GAMCOR_RAMB_START_SLOPE_CNTL_G", 2, 0x1078 },
    { "CM2_CM_GAMCOR_RAMB_START_SLOPE_CNTL_R", 2, 0x1079 },
    { "CM2_CM_GAMCOR_RAMB_START_BASE_CNTL_B", 2, 0x107a },
    { "CM2_CM_GAMCOR_RAMB_START_BASE_CNTL_G", 2, 0x107b },
    { "CM2_CM_GAMCOR_RAMB_START_BASE_CNTL_R", 2, 0x107c },
    { "CM2_CM_GAMCOR_RAMB_END_CNTL1_B", 2, 0x107d },
    { "CM2_CM_GAMCOR_RAMB_END_CNTL2_B", 2, 0x107e },
    { "CM2_CM_GAMCOR_RAMB_END_CNTL1_G", 2, 0x107f },
    { "CM2_CM_GAMCOR_RAMB_END_CNTL2_G", 2, 0x1080 },
    { "CM2_CM_GAMCOR_RAMB_END_CNTL1_R", 2, 0x1081 },
    { "CM2_CM_GAMCOR_RAMB_END_CNTL2_R", 2, 0x1082 },
    { "CM2_CM_GAMCOR_RAMB_OFFSET_B", 2, 0x1083 },
    { "CM2_CM_GAMCOR_RAMB_OFFSET_G", 2, 0x1084 },
    { "CM2_CM_GAMCOR_RAMB_OFFSET_R", 2, 0x1085 },
    { "CM2_CM_GAMCOR_RAMB_REGION_0_1", 2, 0x1086 },
    { "CM2_CM_GAMCOR_RAMB_REGION_2_3", 2, 0x1087 },
    { "CM2_CM_GAMCOR_RAMB_REGION_4_5", 2, 0x1088 },
    { "CM2_CM_GAMCOR_RAMB_REGION_6_7", 2, 0x1089 },
    { "CM2_CM_GAMCOR_RAMB_REGION_8_9", 2, 0x108a },
    /* ---- page 42 starts here (index 744) ---- */
    { "CM2_CM_GAMCOR_RAMB_REGION_10_11", 2, 0x108b },
    { "CM2_CM_GAMCOR_RAMB_REGION_12_13", 2, 0x108c },
    { "CM2_CM_GAMCOR_RAMB_REGION_14_15", 2, 0x108d },
    { "CM2_CM_GAMCOR_RAMB_REGION_16_17", 2, 0x108e },
    { "CM2_CM_GAMCOR_RAMB_REGION_18_19", 2, 0x108f },
    { "CM2_CM_GAMCOR_RAMB_REGION_20_21", 2, 0x1090 },
    { "CM2_CM_GAMCOR_RAMB_REGION_22_23", 2, 0x1091 },
    { "CM2_CM_GAMCOR_RAMB_REGION_24_25", 2, 0x1092 },
    { "CM2_CM_GAMCOR_RAMB_REGION_26_27", 2, 0x1093 },
    { "CM2_CM_GAMCOR_RAMB_REGION_28_29", 2, 0x1094 },
    { "CM2_CM_GAMCOR_RAMB_REGION_30_31", 2, 0x1095 },
    { "CM2_CM_GAMCOR_RAMB_REGION_32_33", 2, 0x1096 },
    { "CM2_CM_HDR_MULT_COEF", 2, 0x1097 },
    { "CM2_CM_MEM_PWR_CTRL", 2, 0x1098 },
    { "CM2_CM_MEM_PWR_STATUS", 2, 0x1099 },
    { "CM2_CM_DEALPHA", 2, 0x109b },
    { "CM2_CM_COEF_FORMAT", 2, 0x109c },
    { "CM2_CM_TEST_DEBUG_INDEX", 2, 0x109d },
};
// The DMU segment bases of this card (dcn41.h DCN41_SEG*_EXPECTED; the host test pins the equality) - the CLI uses them to print absolute BAR5 dword addresses.
#define N48DR_SEG1 0x000000c0u
#define N48DR_SEG2 0x000034c0u
#define N48DR_SEG3 0x00009000u
static inline uint32_t n48dr_census_abs(uint32_t base_idx, uint32_t off)
{
    return base_idx == 1u ? N48DR_SEG1 + off : base_idx == 2u ? N48DR_SEG2 + off : base_idx == 3u ? N48DR_SEG3 + off : 0xFFFFFFFFu;
}
static inline int n48dr_census_arg_ok(uint64_t arg) { return arg < N48DR_CENSUS_ALL_PAGES; }
// pages 0 and 1: the original table; pages 2..: n48dr_census2 (24 per page, the last page short)
static inline uint32_t n48dr_census_in_page(uint32_t page)
{
    if (page < N48DR_CENSUS_PAGES) {
        const uint32_t first = page * N48DR_CENSUS_PER_PAGE;
        return N48DR_CENSUS_COUNT - first > N48DR_CENSUS_PER_PAGE ? N48DR_CENSUS_PER_PAGE : N48DR_CENSUS_COUNT - first;
    }
    if (page >= N48DR_CENSUS_ALL_PAGES) return 0u;
    if (page == N48DR_CENSUS_PAGES + N48DR_CENSUS2_PAGES) return N48DR_CENSUS3_PAGE8_COUNT;       /* 0.0.630: page 8 */
    if (page == N48DR_CENSUS_PAGES + N48DR_CENSUS2_PAGES + 1u) return N48DR_CENSUS3_PAGE9_COUNT;   /* page 9 */
    if (page == N48DR_CENSUS4_PAGE) return N48DR_CENSUS4_COUNT;                                       /* 0.0.634: page 10, the DP watch */
    if (page >= N48DR_CENSUS7_FIRST_PAGE) {                                                           /* 0.0.654: pages 44..59, the monitor A (instance 1) plane census */
        const uint32_t f7 = (page - N48DR_CENSUS7_FIRST_PAGE) * N48DR_CENSUS_PER_PAGE;
        return N48DR_CENSUS7_COUNT - f7 > N48DR_CENSUS_PER_PAGE ? N48DR_CENSUS_PER_PAGE : N48DR_CENSUS7_COUNT - f7;
    }
    if (page == N48DR_CENSUS6_PAGE) return N48DR_CENSUS6_COUNT;                                       /* 0.0.635: page 43, the rest of the DP watch */
    if (page >= N48DR_CENSUS5_FIRST_PAGE) {                                                           /* 0.0.634: pages 11..42, the plane registers */
        const uint32_t f5 = (page - N48DR_CENSUS5_FIRST_PAGE) * N48DR_CENSUS_PER_PAGE;
        return N48DR_CENSUS5_COUNT - f5 > N48DR_CENSUS_PER_PAGE ? N48DR_CENSUS_PER_PAGE : N48DR_CENSUS5_COUNT - f5;
    }
    const uint32_t first = (page - N48DR_CENSUS_PAGES) * N48DR_CENSUS_PER_PAGE;
    return N48DR_CENSUS2_COUNT - first > N48DR_CENSUS_PER_PAGE ? N48DR_CENSUS_PER_PAGE : N48DR_CENSUS2_COUNT - first;
}
// the i-th register of a page (NULL past the page)
static inline const struct n48dr_reg *n48dr_census_reg(uint32_t page, uint32_t i)
{
    if (i >= n48dr_census_in_page(page)) return (const struct n48dr_reg *)0;
    if (page == N48DR_CENSUS_PAGES + N48DR_CENSUS2_PAGES) return &n48dr_census3[i];
    if (page == N48DR_CENSUS_PAGES + N48DR_CENSUS2_PAGES + 1u) return &n48dr_census3[N48DR_CENSUS3_PAGE8_COUNT + i];
    if (page == N48DR_CENSUS4_PAGE) return &n48dr_census4[i];
    if (page >= N48DR_CENSUS7_FIRST_PAGE) return &n48dr_census7[(page - N48DR_CENSUS7_FIRST_PAGE) * N48DR_CENSUS_PER_PAGE + i];   /* 0.0.654 */
    if (page == N48DR_CENSUS6_PAGE) return &n48dr_census6[i];
    if (page >= N48DR_CENSUS5_FIRST_PAGE) return &n48dr_census5[(page - N48DR_CENSUS5_FIRST_PAGE) * N48DR_CENSUS_PER_PAGE + i];
    return page < N48DR_CENSUS_PAGES ? &n48dr_census[page * N48DR_CENSUS_PER_PAGE + i] : &n48dr_census2[(page - N48DR_CENSUS_PAGES) * N48DR_CENSUS_PER_PAGE + i];
}
// kept for the original two pages (the CLI's frame-counter slots index the original table by page * 24)
static inline uint32_t n48dr_census_first(uint32_t page) { return page * N48DR_CENSUS_PER_PAGE; }

#ifdef __cplusplus
}
#endif
#endif /* N48_DISPREAD_H */
