/*
 * test_dcn41.c - host unit tests for the dcn41 register layer that do NOT depend on Linux being compiled.
 *
 * Expected absolute addresses are written out by hand from notes/DISPLAY-TRACK.md section 6 ("Absolute = 0x34c0 +
 * base-2 offset", with offsets such as regOTG0_OTG_CONTROL 0x1b43 and regHUBPREQ3_DCSURF_PRIMARY_SURFACE_ADDRESS
 * 0x89e checked there against dcn_4_1_0_offset.h), so a generator or addressing mistake cannot pass by agreeing with
 * itself. Register-sequence equivalence with Linux is tools/dcn41/harness.py's job, not this file's.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcn41.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- a recording register model: plain memory, optional behaviours ---- */
#define MAXOPS 256
struct op { char kind; uint32_t addr, val; };
struct model {
    uint32_t mem[0x10000];
    struct op ops[MAXOPS];
    int nops;
    int lock_status_after;        /* UPDATE_LOCK_STATUS reads 1 from this many reads of the lock register on; -1 never */
    int lock_reads;
    uint32_t lock_reg;
};

static uint32_t m_read(void *c, uint32_t a)
{
    struct model *m = c;
    uint32_t v = a < 0x10000 ? m->mem[a] : 0xdeadbeef;
    if (a == m->lock_reg) {
        m->lock_reads++;
        v &= ~0x100u;
        if (m->lock_status_after >= 0 && m->lock_reads > m->lock_status_after)
            v |= 0x100u;
    }
    if (m->nops < MAXOPS) m->ops[m->nops++] = (struct op){ 'R', a, v };
    return v;
}
static void m_write(void *c, uint32_t a, uint32_t v)
{
    struct model *m = c;
    if (a < 0x10000) m->mem[a] = v;
    if (m->nops < MAXOPS) m->ops[m->nops++] = (struct op){ 'W', a, v };
}
static void m_udelay(void *c, uint32_t us)
{
    struct model *m = c;
    if (m->nops < MAXOPS) m->ops[m->nops++] = (struct op){ 'D', 0, us };
}

static const uint32_t SEG[5] = { 0x12, 0xc0, 0x34c0, 0x9000, 0x2403c00 };
#define MMIO_DWORDS (0x100000u / 4u * 4u)   /* a 4 MiB BAR5 window in dwords (0x100000): generous, above 0x34c0+0x1d10 */

static struct model *new_model(void)
{
    struct model *m = calloc(1, sizeof(*m));
    m->lock_status_after = 0;
    return m;
}

static void test_init(void)
{
    struct dcn41_dev d;
    struct model *m = new_model();
    uint32_t bad[5];
    int i;

    CHECK(dcn41_dev_init(&d, m, m_read, m_write, m_udelay, SEG, MMIO_DWORDS, 0) == DCN41_OK, "init ok");
    CHECK(dcn41_dev_init(&d, m, NULL, m_write, m_udelay, SEG, MMIO_DWORDS, 0) == DCN41_E_ARG, "null rreg");
    CHECK(dcn41_dev_init(&d, m, m_read, m_write, m_udelay, SEG, 0, 0) == DCN41_E_ARG, "zero window");
    for (i = 0; i < 5; i++) {
        memcpy(bad, SEG, sizeof(bad));
        bad[i] ^= 0x40;
        CHECK(dcn41_dev_init(&d, m, m_read, m_write, m_udelay, bad, MMIO_DWORDS, 0) == DCN41_E_BASES, "seg %d", i);
    }
    memcpy(bad, SEG, sizeof(bad));
    bad[2] = 0x4000;
    CHECK(dcn41_dev_init(&d, m, m_read, m_write, m_udelay, bad, MMIO_DWORDS, DCN41_F_ALLOW_OTHER_BASES) == DCN41_OK,
          "override flag accepts other bases");
    /* window: seg2 0x34c0 + largest offset 0x1d10 = 0x51d0 must be < mmio_dwords */
    CHECK(dcn41_dev_init(&d, m, m_read, m_write, m_udelay, SEG, 0x51d0, 0) == DCN41_E_WINDOW, "window at edge");
    CHECK(dcn41_dev_init(&d, m, m_read, m_write, m_udelay, SEG, 0x51d1, 0) == DCN41_OK, "window one past");
    CHECK(DCN41_MAX_OFFSET_BASE2 == 0x1d10u, "largest used offset 0x%x", DCN41_MAX_OFFSET_BASE2);
    CHECK(m->nops == 0, "init did I/O: %d ops", m->nops);
    /* uninitialised device refuses */
    memset(&d, 0, sizeof(d));
    CHECK(dcn41_otg_unlock(&d, 0) == DCN41_E_UNINIT, "uninit refused");
    free(m);
}

/* 0.0.628 (M4a): the READ-ONLY address twin. BASE_IDX 1 and 3 resolve for reads; every write-side address (dcn41_abs, which every
 * DCN41_ADDR / DCN41_ADDR_I helper and the allowlist callers use) keeps refusing them. */
static void test_abs_rd(void)
{
    struct dcn41_dev d, u;
    struct model *m = new_model();
    uint32_t off;
    dcn41_dev_init(&d, m, m_read, m_write, m_udelay, SEG, MMIO_DWORDS, 0);
    CHECK(dcn41_abs_rd(&d, 0x81, 1) == 0xc0 + 0x81, "abs_rd: DP_DTO0_PHASE (BASE_IDX 1) = 0x141");
    CHECK(dcn41_abs_rd(&d, 0x7b, 1) == 0xc0 + 0x7b, "abs_rd: MICROSECOND_TIME_BASE_DIV (BASE_IDX 1)");
    CHECK(dcn41_abs_rd(&d, 0x10, 3) == 0x9000 + 0x10, "abs_rd: BASE_IDX 3 maps (0x9000 + off)");
    CHECK(dcn41_abs_rd(&d, DCN41_OTG_OTG_CONTROL(0), 2) == dcn41_abs(&d, DCN41_OTG_OTG_CONTROL(0), 2), "abs_rd: BASE_IDX 2 equals dcn41_abs");
    CHECK(dcn41_abs_rd(&d, 0x10, 0) == DCN41_BAD_OFFSET && dcn41_abs_rd(&d, 0x10, 4) == DCN41_BAD_OFFSET && dcn41_abs_rd(&d, 0x10, 5) == DCN41_BAD_OFFSET && dcn41_abs_rd(&d, 0x10, 0xffffffffu) == DCN41_BAD_OFFSET,
          "abs_rd: BASE_IDX 0, 4 and out-of-range stay refused");
    CHECK(dcn41_abs_rd(&d, DCN41_BAD_OFFSET, 1) == DCN41_BAD_OFFSET, "abs_rd: the bad-offset sentinel is refused");
    CHECK(dcn41_abs_rd(&d, MMIO_DWORDS - 0xc0, 1) == DCN41_BAD_OFFSET && dcn41_abs_rd(&d, MMIO_DWORDS - 0xc0 - 1, 1) == MMIO_DWORDS - 1, "abs_rd: the window edge (a >= mmio_dwords refused)");
    memset(&u, 0, sizeof(u));
    CHECK(dcn41_abs_rd(&u, 0x81, 1) == DCN41_BAD_OFFSET, "abs_rd: an uninitialised device refuses");
    /* the write side: dcn41_abs refuses BASE_IDX 1 and 3 for every offset that abs_rd resolves */
    for (off = 0; off < 0x400; off++) {
        CHECK(dcn41_abs(&d, off, 1) == DCN41_BAD_OFFSET, "WRITE SIDE refuses BASE_IDX 1 (offset 0x%x)", off);
        CHECK(dcn41_abs(&d, off, 3) == DCN41_BAD_OFFSET, "WRITE SIDE refuses BASE_IDX 3 (offset 0x%x)", off);
        CHECK(dcn41_abs_rd(&d, off, 1) == 0xc0 + off, "abs_rd maps BASE_IDX 1 (offset 0x%x)", off);
    }
    CHECK(dcn41_abs(&d, 0x81, 0) == DCN41_BAD_OFFSET && dcn41_abs(&d, 0x81, 4) == DCN41_BAD_OFFSET, "dcn41_abs still refuses BASE_IDX 0 and 4");
    CHECK(DCN41_BASE_IDX_USED_MASK == 0x4u && DCN41_BASE_IDX_READ_MASK == 0xEu, "the write mask is still BASE_IDX 2 only; the read mask is 1, 2, 3");
    CHECK(m->nops == 0, "abs_rd does no register I/O: %d ops", m->nops);
    free(m);
}

static void test_addresses(void)
{
    struct dcn41_dev d;
    struct model *m = new_model();
    dcn41_dev_init(&d, m, m_read, m_write, m_udelay, SEG, MMIO_DWORDS, 0);
    /* hand-written from notes/DISPLAY-TRACK.md section 2.2 / 6 */
    CHECK(dcn41_abs(&d, DCN41_OTG_OTG_CONTROL(0), 2) == 0x34c0 + 0x1b43, "OTG0_OTG_CONTROL");
    CHECK(dcn41_abs(&d, DCN41_OTG_OTG_CONTROL(1), 2) == 0x34c0 + 0x1bc3, "OTG1_OTG_CONTROL");
    CHECK(DCN41_OTG_OTG_CONTROL(3) == 0x1cc3, "regOTG3_OTG_CONTROL 0x1cc3");
    CHECK(DCN41_OTG_OTG_STATUS_FRAME_COUNT(0) == 0x1b4d, "OTG0 frame count 0x1b4d");
    CHECK(DCN41_OTG_OTG_STATUS_FRAME_COUNT(3) == 0x1ccd, "OTG3 frame count 0x1ccd");
    CHECK(DCN41_OTG_OTG_STATUS_POSITION(0) == 0x1b4a, "OTG0 status position 0x1b4a");
    CHECK(DCN41_OTG_OTG_V_TOTAL(0) == 0x1b2f && DCN41_OTG_OTG_H_TOTAL(0) == 0x1b2a, "OTG0 totals");
    CHECK(DCN41_OTG_OTG_GLOBAL_SYNC_STATUS(0) == 0x1b88 && DCN41_OTG_OTG_MASTER_UPDATE_LOCK(0) == 0x1b89, "sync/lock");
    CHECK(DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS(0) == 0x60a, "HUBPREQ0 addr 0x60a");
    CHECK(DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH(0) == 0x60b, "HUBPREQ0 addr high 0x60b");
    CHECK(DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS(3) == 0x89e, "HUBPREQ3 addr 0x89e");
    CHECK(DCN41_HUBPREQ_DCSURF_FLIP_CONTROL(0) == 0x613, "HUBPREQ0 flip control 0x613");
    CHECK(DCN41_HUBP_DCHUBP_CNTL(0) == 0x5f4 && DCN41_HUBP_DCHUBP_CNTL(3) == 0x888, "DCHUBP_CNTL 0/3");
    CHECK(DCN41_DMCUB_INBOX1_WPTR == 0x1d6 && DCN41_DMCUB_CNTL == 0x1f6, "DMCUB inbox wptr / cntl");
    CHECK(DCN41_OTG_OTG_CONTROL(4) == DCN41_BAD_OFFSET, "instance 4 has no offset");
    CHECK(dcn41_abs(&d, DCN41_BAD_OFFSET, 2) == DCN41_BAD_OFFSET, "bad offset");
    CHECK(dcn41_abs(&d, 0x10, 1) == DCN41_BAD_OFFSET, "base 1 not used by this layer");
    /* masks used by the T1 predictions (DISPLAY-TRACK section 6) */
    CHECK(DCN41_OTG_OTG_CONTROL__OTG_MASTER_EN_MASK == 0x1, "OTG_MASTER_EN");
    CHECK(DCN41_OTG_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK == 0x00FFFFFF, "frame count mask");
    CHECK(DCN41_OTG_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_INT_EN_MASK == 0x1, "VSTARTUP_INT_EN");
    CHECK(DCN41_HUBPREQ_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH_MASK == 0xFFFF, "addr high");
    CHECK(DCN41_DMCUB_CNTL__DMCUB_ENABLE_MASK == 0x10000 && DCN41_DMCUB_CNTL2__DMCUB_SOFT_RESET_MASK == 0x1, "DMCUB");
    free(m);
}

static void test_ih(void)
{
    struct dcn41_irq r;
    int s;

    r = dcn41_ih_to_irq(4, 0x3C, 0); CHECK(r.kind == DCN41_IRQ_VSTARTUP && r.inst == 0, "0x3C");
    r = dcn41_ih_to_irq(4, 0x3F, 0); CHECK(r.kind == DCN41_IRQ_VSTARTUP && r.inst == 3, "0x3F");
    r = dcn41_ih_to_irq(4, 0x57, 0); CHECK(r.kind == DCN41_IRQ_VUPDATE_NO_LOCK && r.inst == 0, "0x57");
    r = dcn41_ih_to_irq(4, 0x4F, 0); CHECK(r.kind == DCN41_IRQ_PFLIP && r.inst == 0, "0x4F");
    r = dcn41_ih_to_irq(4, 0x55, 0); CHECK(r.kind == DCN41_IRQ_NONE, "0x55 HUBP6 unmapped in dcn401");
    r = dcn41_ih_to_irq(4, 9, 2);    CHECK(r.kind == DCN41_IRQ_HPD && r.inst == 2, "HPD3");
    r = dcn41_ih_to_irq(4, 9, 7);    CHECK(r.kind == DCN41_IRQ_HPD_RX && r.inst == 1, "HPD2 RX");
    r = dcn41_ih_to_irq(4, 9, 12);   CHECK(r.kind == DCN41_IRQ_NONE, "HPD ctx 12");
    r = dcn41_ih_to_irq(4, 0x68, 8); CHECK(r.kind == DCN41_IRQ_DMCUB_OUTBOX, "outbox");
    r = dcn41_ih_to_irq(4, 0x38, 0); CHECK(r.kind == DCN41_IRQ_VLINE0 && r.inst == 5, "OTG6 vline0");
    /* the kext's other clients never classify as display, whatever the source: 0x0a GFX, 0x14 SDMA-ish, 0 */
    for (s = 0; s < 256; s++) {
        CHECK(dcn41_ih_to_irq(0x0a, (uint32_t)s, 0).kind == DCN41_IRQ_NONE, "client 0x0a src %d", s);
        CHECK(dcn41_ih_to_irq(0x14, (uint32_t)s, 0).kind == DCN41_IRQ_NONE, "client 0x14 src %d", s);
    }
    /* the kext's own switch (Navi48Bringup.cpp handleInterrupt) special-cases src 181, 49, 0, 183-185: none of them is
     * a DCE source, so display entries fall through to its default branch today */
    CHECK(dcn41_ih_to_irq(4, 181, 0).kind == DCN41_IRQ_NONE && dcn41_ih_to_irq(4, 49, 0).kind == DCN41_IRQ_NONE &&
          dcn41_ih_to_irq(4, 0, 0).kind == DCN41_IRQ_NONE, "kext special sources are not DCE sources");
}

static void test_irq_sequences(void)
{
    struct dcn41_dev d;
    struct model *m = new_model();
    struct dcn41_irq vs0 = { DCN41_IRQ_VSTARTUP, 0 };
    struct dcn41_otg_irq_status st;
    uint32_t a = 0x34c0 + 0x1b88;

    dcn41_dev_init(&d, m, m_read, m_write, m_udelay, SEG, MMIO_DWORDS, 0);
    m->mem[a] = 0x00000004;           /* VSTARTUP_EVENT_OCCURRED left set */
    CHECK(dcn41_irq_set(&d, vs0, true) == DCN41_OK, "enable vstartup");
    /* hand-derived: ack = RMW set EVENT_CLEAR 0x10; enable = RMW set INT_EN 0x1 */
    CHECK(m->nops == 4, "4 ops, got %d", m->nops);
    CHECK(m->ops[0].kind == 'R' && m->ops[0].addr == a, "ack read");
    CHECK(m->ops[1].kind == 'W' && m->ops[1].addr == a && m->ops[1].val == 0x14, "ack write 0x%x", m->ops[1].val);
    CHECK(m->ops[3].kind == 'W' && m->ops[3].val == 0x15, "enable write 0x%x", m->ops[3].val);
    m->nops = 0;
    CHECK(dcn41_irq_set(&d, vs0, false) == DCN41_OK && m->ops[3].val == 0x14, "disable clears only INT_EN");
    m->nops = 0;
    m->mem[a] = 0x0000900d;           /* VSTARTUP en+occurred+status, VUPDATE_NO_LOCK en+status */
    CHECK(dcn41_otg_irq_status(&d, 0, &st) == DCN41_OK && st.vstartup_int_en && st.vstartup_occurred &&
          st.vstartup_int_status && st.vupdate_no_lock_int_en && !st.vupdate_no_lock_occurred &&
          st.vupdate_no_lock_int_status && m->nops == 1, "status decode");
    /* refusals do no I/O */
    m->nops = 0;
    CHECK(dcn41_irq_set(&d, (struct dcn41_irq){ DCN41_IRQ_VSTARTUP, 4 }, true) == DCN41_E_INST, "inst 4");
    CHECK(dcn41_irq_set(&d, (struct dcn41_irq){ DCN41_IRQ_VLINE0, 0 }, true) == DCN41_E_ARG, "vline0 not driven");
    CHECK(dcn41_irq_ack(&d, (struct dcn41_irq){ DCN41_IRQ_DMCUB_OUTBOX, 1 }) == DCN41_E_INST, "outbox inst 1");
    CHECK(dcn41_irq_set(&d, (struct dcn41_irq){ DCN41_IRQ_NONE, 0 }, true) == DCN41_E_ARG, "none");
    CHECK(m->nops == 0, "refusals did %d ops", m->nops);
    free(m);
}

static void test_flip(void)
{
    struct dcn41_dev d;
    struct model *m = new_model();
    bool pending = true, blank;
    uint64_t early = 0, addr = 0;

    dcn41_dev_init(&d, m, m_read, m_write, m_udelay, SEG, MMIO_DWORDS, 0);
    CHECK(dcn41_hubp_program_flip(&d, 0, 0x8000000000ull, 0, false, false) == DCN41_OK, "flip");
    /* last two ops: W ADDRESS_HIGH (0x34c0+0x60b) = 0x80, then W ADDRESS (0x34c0+0x60a) = 0 */
    CHECK(m->nops == 12, "12 ops (5 RMW + 2 SET), got %d", m->nops);
    CHECK(m->ops[10].kind == 'W' && m->ops[10].addr == 0x34c0 + 0x60b && m->ops[10].val == 0x80, "high first");
    CHECK(m->ops[11].kind == 'W' && m->ops[11].addr == 0x34c0 + 0x60a && m->ops[11].val == 0, "low last");
    CHECK(dcn41_hubp_read_primary_addr(&d, 0, &addr) == DCN41_OK && addr == 0x8000000000ull, "readback");
    m->nops = 0;
    CHECK(dcn41_hubp_program_flip(&d, 1, 0x8012345000ull, 3, false, true) == DCN41_OK && m->nops == 10,
          "immediate flip skips the VMID RMW: %d ops", m->nops);
    /* pending: earliest in use not yet the request */
    m->mem[0x34c0 + DCN41_HUBPREQ_DCSURF_SURFACE_EARLIEST_INUSE_HIGH(1)] = 0x80;
    m->mem[0x34c0 + DCN41_HUBPREQ_DCSURF_SURFACE_EARLIEST_INUSE(1)] = 0x0;
    CHECK(dcn41_hubp_is_flip_pending(&d, 1, &pending, &early) == DCN41_OK && pending && early == 0x8000000000ull,
          "pending until latched");
    m->mem[0x34c0 + DCN41_HUBPREQ_DCSURF_SURFACE_EARLIEST_INUSE(1)] = 0x12345000;
    CHECK(dcn41_hubp_is_flip_pending(&d, 1, &pending, &early) == DCN41_OK && !pending, "latched");
    m->mem[0x34c0 + DCN41_HUBPREQ_DCSURF_FLIP_CONTROL(1)] |= 0x100;
    CHECK(dcn41_hubp_is_flip_pending(&d, 1, &pending, NULL) == DCN41_OK && pending, "SURFACE_FLIP_PENDING bit");
    CHECK(dcn41_hubp_is_flip_pending(&d, 2, &pending, NULL) == DCN41_E_NOREQ, "no request recorded");
    d.hubp_power_gated[1] = 1;
    m->nops = 0;
    CHECK(dcn41_hubp_is_flip_pending(&d, 1, &pending, NULL) == DCN41_OK && !pending && m->nops == 0, "gated");
    /* refusals do no I/O */
    m->nops = 0;
    CHECK(dcn41_hubp_program_flip(&d, 0, 0, 0, false, false) == DCN41_E_ADDR, "addr 0");
    CHECK(dcn41_hubp_program_flip(&d, 0, 1ull << 48, 0, false, false) == DCN41_E_ADDR, "addr 2^48");
    CHECK(dcn41_hubp_program_flip(&d, 4, 0x8000000000ull, 0, false, false) == DCN41_E_INST, "hubp 4");
    CHECK(dcn41_hubp_program_flip(&d, 0, 0x8000000000ull, 16, false, false) == DCN41_E_ARG, "vmid 16");
    CHECK(dcn41_dev_set_scanout_window(&d, 0x8000000000ull, 0x83fc000000ull) == DCN41_OK, "window");
    CHECK(dcn41_hubp_program_flip(&d, 0, 0x7fff000000ull, 0, false, false) == DCN41_E_ADDR, "below window");
    CHECK(dcn41_hubp_program_flip(&d, 0, 0x83fc000000ull, 0, false, false) == DCN41_E_ADDR, "at window top");
    CHECK(m->nops == 0, "refused flips did %d ops", m->nops);
    CHECK(dcn41_hubp_program_flip(&d, 0, 0x83fbfff000ull, 0, false, false) == DCN41_OK, "inside window");
    /* build 0.0.518: the exact set (flip mode's A and B) on top of the window */
    {
        const uint64_t ab[2] = { 0x8000000000ull, 0x8012340000ull };
        const uint64_t outside[1] = { 0x7000000000ull };
        CHECK(dcn41_dev_set_flip_exact(&d, ab, 3) == DCN41_E_ARG && d.flip_exact_n == 0, "n 3 refused, set unchanged");
        CHECK(dcn41_dev_set_flip_exact(&d, outside, 1) == DCN41_E_ARG && d.flip_exact_n == 0, "outside the window refused");
        CHECK(dcn41_dev_set_flip_exact(&d, NULL, 2) == DCN41_E_ARG, "null with n 2 refused");
        CHECK(dcn41_dev_set_flip_exact(&d, ab, 2) == DCN41_OK && d.flip_exact_n == 2, "A and B set");
        m->nops = 0;
        CHECK(dcn41_hubp_program_flip(&d, 0, 0x83fbfff000ull, 0, false, false) == DCN41_E_ADDR, "inside the window, not A/B");
        CHECK(dcn41_hubp_program_flip(&d, 0, 0x8012340100ull, 0, false, false) == DCN41_E_ADDR, "B + 0x100 refused");
        CHECK(m->nops == 0, "exact-set refusals did %d ops", m->nops);
        CHECK(dcn41_hubp_program_flip(&d, 0, 0x8012340000ull, 0, false, false) == DCN41_OK && m->nops == 12, "B admitted");
        CHECK(dcn41_hubp_program_flip(&d, 0, 0x8000000000ull, 0, false, false) == DCN41_OK, "A admitted");
        CHECK(dcn41_dev_set_flip_exact(&d, ab, 1) == DCN41_OK, "A only");
        CHECK(dcn41_hubp_program_flip(&d, 0, 0x8012340000ull, 0, false, false) == DCN41_E_ADDR, "B refused with A only");
        CHECK(dcn41_dev_set_flip_exact(&d, NULL, 0) == DCN41_OK && d.flip_exact_n == 0, "cleared");
        CHECK(dcn41_hubp_program_flip(&d, 0, 0x83fbfff000ull, 0, false, false) == DCN41_OK, "cleared: the window alone again");
    }
    m->mem[0x34c0 + DCN41_HUBP_DCHUBP_CNTL(2)] = 0x8;
    CHECK(dcn41_hubp_in_blank(&d, 2, &blank) == DCN41_OK && blank, "in blank");
    free(m);
}

static void test_otg(void)
{
    struct dcn41_dev d;
    struct model *m = new_model();
    struct dcn41_otg_position p;
    uint32_t fc = 0, w = 0, h = 0, ht = 0, vt = 0;
    bool en = false;
    int i, delays = 0, reads = 0;

    dcn41_dev_init(&d, m, m_read, m_write, m_udelay, SEG, MMIO_DWORDS, 0);
    m->lock_reg = 0x34c0 + DCN41_OTG_OTG_MASTER_UPDATE_LOCK(2);
    m->lock_status_after = -1;
    CHECK(dcn41_otg_lock(&d, 2) == DCN41_E_TIMEOUT, "lock timeout");
    for (i = 0; i < m->nops; i++) {
        delays += m->ops[i].kind == 'D' && m->ops[i].val == 1;
        reads += m->ops[i].kind == 'R' && m->ops[i].addr == m->lock_reg;
    }
    CHECK(reads == 11 && delays == 10, "REG_WAIT 11 reads / 10 udelay(1): %d / %d", reads, delays);
    CHECK((m->mem[0x34c0 + DCN41_OTG_OTG_GLOBAL_CONTROL2(2)] & 0x0E000000u) == (2u << 25), "LOCK_SEL = inst");
    m->nops = 0; m->lock_reads = 0; m->lock_status_after = 2;
    CHECK(dcn41_otg_lock(&d, 2) == DCN41_OK, "lock after 3 reads");
    CHECK(dcn41_otg_unlock(&d, 2) == DCN41_OK && (m->mem[m->lock_reg] & 1) == 0, "unlock");

    m->mem[0x34c0 + DCN41_OTG_OTG_STATUS_POSITION(0)] = (1234u << 16) | 567u;
    m->mem[0x34c0 + DCN41_OTG_OTG_NOM_VERT_POSITION(0)] = 568;
    CHECK(dcn41_otg_get_position(&d, 0, &p) == DCN41_OK && p.horizontal_count == 1234 && p.vertical_count == 567 &&
          p.nominal_vcount == 568, "position");
    m->mem[0x34c0 + DCN41_OTG_OTG_STATUS_FRAME_COUNT(0)] = 0xAB123456u;
    CHECK(dcn41_otg_get_frame_count(&d, 0, &fc) == DCN41_OK && fc == 0x123456, "24-bit frame count");
    /* 1920x1080 CEA timing as optc1_program_timing leaves it: h blank start 2112? use start/end straight */
    m->mem[0x34c0 + DCN41_OTG_OTG_CONTROL(0)] = 1;
    m->mem[0x34c0 + DCN41_OTG_OTG_H_BLANK_START_END(0)] = (192u << 16) | 2112u;
    m->mem[0x34c0 + DCN41_OTG_OTG_V_BLANK_START_END(0)] = (41u << 16) | 1121u;
    CHECK(dcn41_otg_get_active_size(&d, 0, &en, &w, &h) == DCN41_OK && en && w == 1920 && h == 1080, "active %ux%u",
          w, h);
    m->mem[0x34c0 + DCN41_OTG_OTG_CONTROL(1)] = 0;
    w = h = 7;
    CHECK(dcn41_otg_get_active_size(&d, 1, &en, &w, &h) == DCN41_OK && !en && w == 7, "disabled OTG leaves size");
    m->mem[0x34c0 + DCN41_OTG_OTG_H_TOTAL(0)] = 2199;
    m->mem[0x34c0 + DCN41_OTG_OTG_V_TOTAL(0)] = 1124;
    CHECK(dcn41_otg_get_totals(&d, 0, &ht, &vt) == DCN41_OK && ht == 2199 && vt == 1124, "totals");
    free(m);
}

static void test_refresh(void)
{
    CHECK(dcn41_frame_delta(100, 700) == 600, "delta");
    CHECK(dcn41_frame_delta(0xFFFFF0, 0x10) == 0x20, "wrap");
    CHECK(dcn41_refresh_mhz(0, 600, 10000000000ull) == 60000, "600 frames / 10 s");
    CHECK(dcn41_refresh_mhz(0, 1650, 10000000000ull) == 165000, "1650 frames / 10 s");
    CHECK(dcn41_refresh_mhz(0, 0xFFFFFF, 1) == 0xFFFFFFull * 1000000000000ull, "max frames, no overflow");
    CHECK(dcn41_refresh_mhz(0, 1, 0) == 0, "zero elapsed");
    /* CEA 1080p60: 148.5 MHz over 2200 x 1125 = 60.000 Hz */
    CHECK(dcn41_nominal_refresh_mhz(1485000, 2200, 1125) == 60000, "1080p60 %" PRIu64,
          dcn41_nominal_refresh_mhz(1485000, 2200, 1125));
    /* the SINK-A preferred DTD: 241.5 MHz over 2720 x 1481 = 59.951 Hz (DISPLAY-TRACK T2 "59,951 mHz") */
    CHECK(dcn41_nominal_refresh_mhz(2415000, 2720, 1481) == 59951, "1440p59.95 %" PRIu64,
          dcn41_nominal_refresh_mhz(2415000, 2720, 1481));
    CHECK(dcn41_nominal_refresh_mhz(1, 0, 5) == 0, "zero totals");
}

int main(void)
{
    test_init();
    test_addresses();
    test_abs_rd();
    test_ih();
    test_irq_sequences();
    test_flip();
    test_otg();
    test_refresh();
    printf("test_dcn41: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
