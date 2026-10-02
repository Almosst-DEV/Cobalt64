/*
 * test_dcn41_dmub.c - host unit tests for src/dcn41/dcn41_dmub.c that do not depend on Linux being compiled.
 *
 * Expected values are derived by hand from the Linux sources named in dcn41_dmub.h (quoted where they matter), so they
 * are independent of the generated layout asserts and of the harness. Byte equivalence with Linux's own compiled
 * builders is tools/dcn41/harness.py's job.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcn41_dmub.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MAXOPS 4096
struct op { char kind; uint32_t addr, val; };
static uint32_t mem[0x10000];
static struct op ops[MAXOPS];
static int nops, gpint_ack_after = -1, gpint_reads;
static uint8_t vram[0x4000];
static uint64_t vram_base = 0x83fe000000ull;
static int corrupt_readback;

static uint32_t m_read(void *c, uint32_t a)
{
    uint32_t v = mem[a];
    (void)c;
    if (a == 0x34c0 + 0x1f8 && (v & 0xF0000000u) && gpint_ack_after >= 0 && ++gpint_reads > gpint_ack_after)
        v = mem[a] = v & 0x0FFFFFFFu;
    if (nops < MAXOPS) ops[nops++] = (struct op){ 'R', a, v };
    return v;
}
static void m_write(void *c, uint32_t a, uint32_t v)
{
    (void)c;
    mem[a] = v;
    if (nops < MAXOPS) ops[nops++] = (struct op){ 'W', a, v };
}
static void m_udelay(void *c, uint32_t us)
{
    (void)c;
    if (nops < MAXOPS) ops[nops++] = (struct op){ 'D', 0, us };
}
static uint32_t v_read(void *c, uint64_t a)
{
    uint32_t v;
    (void)c;
    memcpy(&v, vram + (a - vram_base), 4);
    return corrupt_readback ? v ^ 1u : v;
}
static void v_write(void *c, uint64_t a, uint32_t v)
{
    (void)c;
    memcpy(vram + (a - vram_base), &v, 4);
}

static const uint32_t SEG[5] = { 0x12, 0xc0, 0x34c0, 0x9000, 0x2403c00 };
#define R(off) (0x34c0u + (off))

static void live_cw4(void)
{
    memset(mem, 0, sizeof(mem));
    mem[R(0x1f6)] = 0x00010000;        /* DMCUB_CNTL: DMCUB_ENABLE */
    mem[R(0x200)] = 0;                 /* DMCUB_CNTL2: not in soft reset */
    mem[R(0x1e3)] = 0x83;              /* SCRATCH0: dal_fw | mailbox_rdy | hw_power_init_done (DAL firmware) */
    mem[R(0x1d4)] = 0x64000000;        /* INBOX1_BASE_ADDRESS = DMUB_CW4_BASE */
    mem[R(0x1d5)] = 0x2000;            /* INBOX1_SIZE = DMUB_RB_SIZE */
    mem[R(0x1d6)] = 0x4c0;             /* WPTR == RPTR: idle, 19 commands consumed */
    mem[R(0x1d7)] = 0x4c0;
    mem[R(0x1a9)] = 0x04000000;        /* REGION3_CW4_BASE: 0x64000000 in 29 bits */
    mem[R(0x1b1)] = 0x80000000u | 0x04003fff; /* CW4_TOP: enable | top */
    mem[R(0x1bd)] = 0xfe000000;        /* CW4_OFFSET */
    mem[R(0x1be)] = 0x83;              /* CW4_OFFSET_HIGH */
    mem[R(0x475)] = 0x8000;            /* DCN_VM_FB_LOCATION_BASE: MC 0x80_0000_0000 */
}

static void test_probe(void)
{
    struct dcn41_dev d;
    struct dcn41_dmub_state st;
    int i, w = 0;

    dcn41_dev_init(&d, NULL, m_read, m_write, m_udelay, SEG, 0x100000, 0);
    live_cw4();
    nops = 0;
    CHECK(dcn41_dmub_probe(&d, &st) == DCN41_OK, "probe");
    for (i = 0; i < nops; i++) w += ops[i].kind != 'R';
    CHECK(nops == 27 && w == 0, "27 reads, no writes or delays: %d ops, %d non-reads", nops, w);
    CHECK(st.verdict == DCN41_DMUB_ALIVE_IDLE && st.enabled && !st.soft_reset && st.dal_fw && st.mailbox_rdy, "idle");
    CHECK(st.ring_map == DCN41_DMUB_MAP_CW4 && st.ring_addr == 0x83fe000000ull, "cw4 ring at 0x%" PRIx64, st.ring_addr);
    CHECK(st.fb_base_mc == 0x8000000000ull, "fb base");
    /* uncached inbox through REGION4 (dmub_dcn20_setup_mailbox writes 0x80000000) */
    mem[R(0x1d4)] = 0x80000000u;
    mem[R(0x1b1)] = 0;                 /* CW4 disabled */
    mem[R(0x196)] = 0xabc00000; mem[R(0x197)] = 0x83; mem[R(0x1a1)] = 0x80000000u | 0x3fff;
    CHECK(dcn41_dmub_probe(&d, &st) == DCN41_OK && st.ring_map == DCN41_DMUB_MAP_REGION4 &&
          st.ring_addr == 0x83abc00000ull, "region4 ring");
    mem[R(0x1a1)] = 0x3fff;            /* REGION4 not enabled */
    CHECK(dcn41_dmub_probe(&d, &st) == DCN41_OK && st.ring_map == DCN41_DMUB_MAP_UNKNOWN, "no mapping");
    live_cw4();
    mem[R(0x1d7)] = 0x480;
    CHECK(dcn41_dmub_probe(&d, &st) == DCN41_OK && st.verdict == DCN41_DMUB_ALIVE_BUSY, "busy");
    live_cw4(); mem[R(0x1f6)] = 0;
    CHECK(dcn41_dmub_probe(&d, &st) == DCN41_OK && st.verdict == DCN41_DMUB_NOT_RUNNING, "not running");
    live_cw4(); mem[R(0x200)] = 1;
    CHECK(dcn41_dmub_probe(&d, &st) == DCN41_OK && st.verdict == DCN41_DMUB_IN_RESET, "in reset");
    live_cw4(); mem[R(0x1e3)] = 0x81;
    CHECK(dcn41_dmub_probe(&d, &st) == DCN41_OK && st.verdict == DCN41_DMUB_NOT_READY, "mailbox not ready");
    live_cw4(); mem[R(0x1e3)] = 0x3;   /* DAL firmware without hw_power_init_done: dmub_dcn35_is_hw_powered_up false */
    CHECK(dcn41_dmub_probe(&d, &st) == DCN41_OK && st.verdict == DCN41_DMUB_NOT_READY, "dal fw, power init not done");
    live_cw4(); mem[R(0x1e3)] = 0x0;
    CHECK(dcn41_dmub_probe(&d, &st) == DCN41_OK && st.verdict == DCN41_DMUB_NOT_READY, "scratch0 empty");
    live_cw4(); mem[R(0x1d6)] = 0x4c1; mem[R(0x1d7)] = 0x4c1;
    CHECK(dcn41_dmub_probe(&d, &st) == DCN41_OK && st.verdict == DCN41_DMUB_RING_INSANE, "pointer not /64");
    live_cw4(); mem[R(0x1d5)] = 0;
    CHECK(dcn41_dmub_probe(&d, &st) == DCN41_OK && st.verdict == DCN41_DMUB_RING_INSANE, "size 0");
}

/* Run t1a (notes/APPLE-DRIVER-VERDICT.md section 515): the 27 DMUB registers dcn41_dmub_probe reads, copied verbatim, in
 * the probe's own order, from notes/logs/runs/t1a/passA.txt ("REG <absolute dword address> <value> <us> dmub/<name>").
 * The GOP left the VBIOS DMUB firmware alive and idle: SCRATCH0 0x82 (dal_fw 0, mailbox_rdy 1, hw_power_init_done 1),
 * WPTR == RPTR == 0x640, inbox in the REGION4 form. */
static const struct { uint32_t addr, val; } T1A[27] = {
    { 0x36b6, 0x001900c6 },  /* DMCUB_CNTL */
    { 0x36c0, 0x00000000 },  /* DMCUB_CNTL2 */
    { 0x368e, 0x00000000 },  /* DMCUB_SEC_CNTL */
    { 0x36a3, 0x00000082 },  /* DMCUB_SCRATCH0 */
    { 0x36aa, 0x00000000 },  /* DMCUB_SCRATCH7 */
    { 0x36b1, 0x00000000 },  /* DMCUB_SCRATCH14 */
    { 0x36b2, 0x00000000 },  /* DMCUB_SCRATCH15 */
    { 0x36b8, 0x00000000 },  /* DMCUB_GPINT_DATAIN1 */
    { 0x36ba, 0x00000000 },  /* DMCUB_UNDEFINED_ADDRESS_FAULT_ADDR */
    { 0x3694, 0x80000000 },  /* DMCUB_INBOX1_BASE_ADDRESS */
    { 0x3695, 0x00002000 },  /* DMCUB_INBOX1_SIZE */
    { 0x3696, 0x00000640 },  /* DMCUB_INBOX1_WPTR */
    { 0x3697, 0x00000640 },  /* DMCUB_INBOX1_RPTR */
    { 0x369c, 0x00000000 },  /* DMCUB_OUTBOX1_BASE_ADDRESS */
    { 0x369d, 0x00000000 },  /* DMCUB_OUTBOX1_SIZE */
    { 0x369e, 0x00000000 },  /* DMCUB_OUTBOX1_WPTR */
    { 0x369f, 0x00000000 },  /* DMCUB_OUTBOX1_RPTR */
    { 0x367d, 0xdac40000 },  /* DMCUB_REGION3_CW4_OFFSET */
    { 0x367e, 0x00000083 },  /* DMCUB_REGION3_CW4_OFFSET_HIGH */
    { 0x3669, 0x04000000 },  /* DMCUB_REGION3_CW4_BASE_ADDRESS */
    { 0x3671, 0x8400ffff },  /* DMCUB_REGION3_CW4_TOP_ADDRESS */
    { 0x3656, 0xdac00000 },  /* DMCUB_REGION4_OFFSET */
    { 0x3657, 0x00000083 },  /* DMCUB_REGION4_OFFSET_HIGH */
    { 0x3661, 0x8000ffff },  /* DMCUB_REGION4_TOP_ADDRESS */
    { 0x3935, 0x00008000 },  /* DCN_VM_FB_LOCATION_BASE */
    { 0x3936, 0x000083fb },  /* DCN_VM_FB_LOCATION_TOP */
    { 0x3937, 0x00000000 },  /* DCN_VM_FB_OFFSET */
};

static void load_t1a(void)
{
    int i;
    memset(mem, 0, sizeof(mem));
    for (i = 0; i < 27; i++)
        mem[T1A[i].addr] = T1A[i].val;
}

static void test_probe_t1a(void)
{
    struct dcn41_dev d;
    struct dcn41_dmub_state st;
    struct dcn41_dmub_ring ring;
    int i, inorder = 1;

    dcn41_dev_init(&d, NULL, m_read, m_write, m_udelay, SEG, 0x100000, 0);
    load_t1a();
    nops = 0;
    CHECK(dcn41_dmub_probe(&d, &st) == DCN41_OK, "t1a probe");
    for (i = 0; i < 27; i++)
        inorder &= i < nops && ops[i].kind == 'R' && ops[i].addr == T1A[i].addr && ops[i].val == T1A[i].val;
    CHECK(nops == 27 && inorder, "t1a: the probe read exactly the 27 captured registers, in order (%d ops)", nops);
    CHECK(st.enabled && !st.soft_reset && !st.dal_fw && st.mailbox_rdy && st.scratch0 == 0x82, "t1a boot status");
    CHECK(st.verdict == DCN41_DMUB_ALIVE_IDLE, "t1a: VBIOS firmware, alive and idle -> ALIVE_IDLE (got %u)", st.verdict);
    CHECK(st.ring_map == DCN41_DMUB_MAP_REGION4 && st.ring_addr == 0x83dac00000ull, "t1a REGION4 ring at 0x%" PRIx64,
          st.ring_addr);
    CHECK(dcn41_dmub_ring_attach(&ring, &d, &st, v_read, v_write, NULL) == DCN41_OK && ring.wptr == 0x640 &&
          ring.rptr == 0x640 && ring.capacity == 0x2000, "t1a: the adopt path attaches the live ring");
    /* fail-closed: the same card with mailbox_rdy clear is refused, and attach refuses it */
    load_t1a(); mem[0x36a3] = 0x80;
    CHECK(dcn41_dmub_probe(&d, &st) == DCN41_OK && st.verdict == DCN41_DMUB_NOT_READY, "t1a, mailbox not ready");
    CHECK(dcn41_dmub_ring_attach(&ring, &d, &st, v_read, v_write, NULL) == DCN41_E_STATE, "t1a, mailbox not ready: no attach");
    load_t1a(); mem[0x3697] = 0x600;
    CHECK(dcn41_dmub_probe(&d, &st) == DCN41_OK && st.verdict == DCN41_DMUB_ALIVE_BUSY, "t1a, RPTR behind: busy");
}

static void test_gpint(void)
{
    struct dcn41_dev d;
    uint32_t resp = 0;
    int i, delays = 0, reads = 0;

    dcn41_dev_init(&d, NULL, m_read, m_write, m_udelay, SEG, 0x100000, 0);
    memset(mem, 0, sizeof(mem));
    mem[R(0x1ea)] = 0x00010300;       /* SCRATCH7 */
    gpint_ack_after = 2; gpint_reads = 0; nops = 0;
    CHECK(dcn41_dmub_gpint(&d, DCN41_DMUB_GPINT_GET_FW_VERSION, 0, 30) == DCN41_OK, "acked");
    /* reg.bits.status = 1, command_code = 1, param = 0: 0x10010000 */
    CHECK(ops[0].kind == 'W' && ops[0].addr == R(0x1f8) && ops[0].val == 0x10010000u, "write 0x%08x", ops[0].val);
    for (i = 1; i < nops; i++) { delays += ops[i].kind == 'D'; reads += ops[i].kind == 'R'; }
    CHECK(delays == 3 && reads == 3, "udelay(1) before each of 3 reads: %d/%d", delays, reads);
    CHECK(dcn41_dmub_gpint_response(&d, &resp) == DCN41_OK && resp == 0x00010300u, "response");
    gpint_ack_after = -1; gpint_reads = 0; nops = 0;
    CHECK(dcn41_dmub_gpint(&d, 2, 0xBEEF, 5) == DCN41_E_TIMEOUT && nops == 11, "timeout after 5 polls: %d ops", nops);
    CHECK(ops[0].val == 0x1002BEEFu, "stop fw with param: 0x%08x", ops[0].val);
    CHECK(dcn41_dmub_gpint(&d, 0x1000, 0, 5) == DCN41_E_ARG, "command above 12 bits refused");
}

static void test_builders(void)
{
    struct dcn41_dmub_cmd c;
    const uint8_t *b = (const uint8_t *)&c;
    int i, rest = 0;

    /* HDMI enable on UNIPHY_B at 148.5 MHz: transmitter_control_v1_7 via DMUB */
    dcn41_dmub_build_transmitter_control(&c, 1, 1, 3, 4, 148500, 3, 0, 12, 0xf5, 0);
    CHECK(c.header == 0x3C000180u, "header type 128 sub 1 payload 60: 0x%08x", c.header);
    CHECK(b[4] == 1 && b[5] == 1 && b[6] == 3 && b[7] == 4, "phyid/action/digmode/lanes");
    CHECK(b[8] == 0x02 && b[9] == 0x3a && b[10] == 0 && b[11] == 0, "symclk 14850 (148.5 MHz in 10 kHz)");
    CHECK(b[12] == 3 && b[13] == 0 && b[14] == 12 && b[15] == 0xf5, "hpdsel/digfe/connobj/hpo");
    for (i = 17; i < 64; i++) rest |= b[i];
    CHECK(rest == 0, "tail zero");
    /* encoder stream setup, HDMI 10 bpc: pclk_10khz * 30 / 24 */
    dcn41_dmub_build_encoder_control(&c, 1, 0x0f, 3, 4, 148500, 0x03, true);
    CHECK(c.header == 0x0C000080u, "encoder header: 0x%08x", c.header);
    CHECK(b[8] == (uint8_t)18562 && b[9] == (uint8_t)(18562 >> 8), "14850 * 30 / 24 = 18562");
    CHECK(b[12] == 0x03, "bitpercolor");
    dcn41_dmub_build_encoder_control(&c, 1, 0x0f, 0, 4, 148500, 0x03, false);
    CHECK(b[8] == (uint8_t)14850 && b[9] == (uint8_t)(14850 >> 8), "DP: no deep-colour scaling");
    dcn41_dmub_build_set_pixel_clock(&c, 1485000, 21, 0x1e, 3, 0, 1, 0);
    CHECK(c.header == 0x10000280u, "pixel clock header: 0x%08x", c.header);
    CHECK(b[4] == 0xc8 && b[5] == 0xa8 && b[6] == 0x16 && b[7] == 0 && b[8] == 21 && b[12] == 1, "pixclk/pll/crtc");
    dcn41_dmub_build_disp_power_gating(&c, 2, 1);
    CHECK(c.header == 0x04000380u && b[4] == 2 && b[5] == 1, "power gating");
    dcn41_dmub_build_query_feature_caps(&c);
    CHECK(c.header == (0x0E000006u | 0x00010000u), "caps: type 6, ret_status, payload 14: 0x%08x", c.header);
}

static struct dcn41_dmub_state idle_state(uint32_t wptr, uint32_t rptr)
{
    struct dcn41_dmub_state st;
    memset(&st, 0, sizeof(st));
    st.verdict = DCN41_DMUB_ALIVE_IDLE;
    st.ring_map = DCN41_DMUB_MAP_CW4;
    st.ring_addr = vram_base;
    st.inbox1_size = 0x2000;
    st.inbox1_wptr = wptr;
    st.inbox1_rptr = rptr;
    mem[R(0x1d6)] = wptr;
    mem[R(0x1d7)] = rptr;
    return st;
}

static void test_ring(void)
{
    struct dcn41_dev d;
    struct dcn41_dmub_ring rg;
    struct dcn41_dmub_state st;
    struct dcn41_dmub_cmd c, back;
    int i, n = 0;

    dcn41_dev_init(&d, NULL, m_read, m_write, m_udelay, SEG, 0x100000, 0);
    memset(mem, 0, sizeof(mem));
    memset(vram, 0, sizeof(vram));
    st = idle_state(0x1fc0, 0x1fc0);
    CHECK(dcn41_dmub_ring_attach(&rg, &d, &st, v_read, v_write, NULL) == DCN41_OK, "attach");
    dcn41_dmub_build_disp_power_gating(&c, 1, 1);
    nops = 0;
    CHECK(dcn41_dmub_ring_submit(&rg, &c) == DCN41_OK, "submit at the last slot");
    CHECK(mem[R(0x1d6)] == 0 && rg.wptr == 0, "wraps to 0");
    CHECK(!memcmp(vram + 0x1fc0, &c, 64), "bytes at 0x1fc0");
    CHECK(ops[nops - 1].kind == 'W' && ops[nops - 1].addr == R(0x1d6), "WPTR written last");
    /* full: rptr one entry ahead of wptr means capacity - 64 used */
    st = idle_state(0x100, 0x100);
    dcn41_dmub_ring_attach(&rg, &d, &st, v_read, v_write, NULL);
    mem[R(0x1d7)] = 0x140;             /* firmware "behind" by the whole ring but one */
    rg.rptr = 0x140;
    for (i = 0; i < 200; i++) {
        int rc = dcn41_dmub_ring_submit(&rg, &c);
        if (rc == DCN41_E_FULL) break;
        CHECK(rc == DCN41_OK, "submit %d rc %d", i, rc);
        n++;
    }
    CHECK(n == 0, "rptr 0x140, wptr 0x100: data_count 8128 = capacity - 64 is full at once (got %d)", n);
    mem[R(0x1d7)] = 0x100; rg.rptr = 0x100; n = 0;
    for (i = 0; i < 200; i++) { if (dcn41_dmub_ring_submit(&rg, &c) != DCN41_OK) break; n++; }
    CHECK(n == 127, "an empty 128-entry ring takes 127 (one entry always unusable): %d", n);
    /* race: hardware WPTR moved */
    st = idle_state(0x40, 0x40);
    dcn41_dmub_ring_attach(&rg, &d, &st, v_read, v_write, NULL);
    mem[R(0x1d6)] = 0x80;
    nops = 0;
    CHECK(dcn41_dmub_ring_submit(&rg, &c) == DCN41_E_RACE && nops == 1, "race refused after one read");
    /* readback mismatch: no WPTR write */
    mem[R(0x1d6)] = 0x40;
    corrupt_readback = 1; nops = 0;
    CHECK(dcn41_dmub_ring_submit(&rg, &c) == DCN41_E_READBACK, "readback");
    for (i = 0; i < nops; i++) CHECK(ops[i].kind != 'W', "no register write on readback failure");
    corrupt_readback = 0;
    CHECK(mem[R(0x1d6)] == 0x40 && rg.wptr == 0x40, "WPTR unchanged");
    /* wait idle, then return data behind RPTR */
    CHECK(dcn41_dmub_ring_submit(&rg, &c) == DCN41_OK && mem[R(0x1d6)] == 0x80, "submit");
    nops = 0;
    CHECK(dcn41_dmub_ring_wait_idle(&rg, 3) == DCN41_E_TIMEOUT && nops == 6, "3 polls: %d ops", nops);
    mem[R(0x1d7)] = 0x80;
    CHECK(dcn41_dmub_ring_wait_idle(&rg, 3) == DCN41_OK && rg.reported == 1, "idle, 1 reported");
    CHECK(dcn41_dmub_ring_return_data(&rg, &back) == DCN41_OK && !memcmp(&back, &c, 64), "return data at rptr - 64");
    mem[R(0x1d7)] = 0x3000;
    CHECK(dcn41_dmub_ring_wait_idle(&rg, 3) == DCN41_E_HW, "rptr beyond capacity");
    /* attach refuses anything but a live idle ring with a mapping */
    st = idle_state(0, 0); st.verdict = DCN41_DMUB_ALIVE_BUSY;
    CHECK(dcn41_dmub_ring_attach(&rg, &d, &st, v_read, v_write, NULL) == DCN41_E_STATE, "busy refused");
    st = idle_state(0, 0); st.ring_map = DCN41_DMUB_MAP_UNKNOWN;
    CHECK(dcn41_dmub_ring_attach(&rg, &d, &st, v_read, v_write, NULL) == DCN41_E_STATE, "no map refused");
    CHECK(dcn41_dmub_ring_submit(&rg, &c) == DCN41_E_UNINIT, "failed attach leaves the ring unusable");
}

static void test_meta(void)
{
    uint8_t blob[512];
    struct dcn41_dmub_fw_meta_info m;
    uint32_t magic = 0x444D5542u, ver = 0x04000601u;
    memset(blob, 0, sizeof(blob));
    memcpy(blob + 512 - 0x24 - 64, &magic, 4);
    memcpy(blob + 512 - 0x24 - 64 + 12, &ver, 4);
    CHECK(dcn41_dmub_fw_meta_from_blob(blob, 512, 0x24, &m) == DCN41_OK && m.fw_version == ver, "bss meta");
    CHECK(dcn41_dmub_fw_meta_from_blob(blob, 512, 0x23, &m) == DCN41_E_ARG, "wrong offset");
    CHECK(dcn41_dmub_fw_meta_scan(blob, 512, &m) == DCN41_E_ARG, "0x24 is outside the 0..15 scan");
    memset(blob, 0, sizeof(blob));
    memcpy(blob + 512 - 15 - 64, &magic, 4);
    CHECK(dcn41_dmub_fw_meta_scan(blob, 512, &m) == DCN41_OK, "scan finds offset 15");
    CHECK(dcn41_dmub_fw_meta_from_blob(blob, 63, 0, &m) == DCN41_E_ARG, "too small");
}

int main(void)
{
    test_probe();
    test_probe_t1a();
    test_gpint();
    test_builders();
    test_ring();
    test_meta();
    printf("test_dcn41_dmub: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
