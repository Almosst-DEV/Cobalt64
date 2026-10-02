/*
 * test_dcn41_otg_timing.c - host unit tests for the OTG timing capture/write-back layer.
 *
 * Every expected absolute address is written out BY HAND here from the Linux offset header plus the
 * measured segment base, so the table in dcn41_otg_timing.c and this file cannot pass by agreeing
 * with each other:
 *
 *   seg2 = 0x34c0, OTG instance stride 0x80
 *   OTG_H_TOTAL 0x1b2a -> 0x4fea    OTG_H_SYNC_A 0x1b2c -> 0x4fec
 *   OTG_H_BLANK_START_END 0x1b2b -> 0x4feb   OTG_H_SYNC_A_CNTL 0x1b2d -> 0x4fed
 *   OTG_V_TOTAL 0x1b2f -> 0x4fef    OTG_V_TOTAL_MIN 0x1b30 -> 0x4ff0
 *   OTG_V_TOTAL_MAX 0x1b31 -> 0x4ff1   OTG_V_SYNC_A 0x1b39 -> 0x4ff9
 *   OTG_V_BLANK_START_END 0x1b38 -> 0x4ff8   OTG_V_SYNC_A_CNTL 0x1b3a -> 0x4ffa
 *   OTG_H_TIMING_CNTL 0x1b2e -> 0x4fee   OTG_VSTARTUP_PARAM 0x1b85 -> 0x5045
 *   OTG_VUPDATE_PARAM 0x1b86 -> 0x5046   OTG_VREADY_PARAM 0x1b87 -> 0x5047
 *   read-only: OTG_CONTROL 0x1b43 -> 0x5003, OTG_V_TOTAL_CONTROL 0x1b33 -> 0x4ff3,
 *   OTG_INTERLACE_CONTROL 0x1b45 -> 0x5005, OTG_STEREO_CONTROL 0x1b55 -> 0x5015,
 *   OTG_GLOBAL_CONTROL0 0x1b8e -> 0x504e, OTG_MASTER_UPDATE_LOCK 0x1b89 -> 0x5049
 *
 * The load-bearing test is test_write_back_is_a_noop(): read a register file, write it back, and
 * require that memory is byte-identical afterwards AND that every write wrote the value that had
 * been read from that same address. That is the property the first mode-set on live hardware rests
 * on, and it is checkable here rather than on the user's screen.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "dcn41_otg_timing.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint32_t kSegs[5] = { 0x00000012u, 0x000000c0u, 0x000034c0u, 0x00009000u, 0x02403c00u };

#define MAXOPS 256
struct op { char kind; uint32_t addr, val; };
struct model {
    uint32_t mem[0x10000];
    struct op ops[MAXOPS];
    int nops;
};

static uint32_t m_read(void *c, uint32_t a)
{
    struct model *m = c;
    uint32_t v = a < 0x10000 ? m->mem[a] : 0xdeadbeef;
    if (m->nops < MAXOPS) { m->ops[m->nops].kind = 'R'; m->ops[m->nops].addr = a;
                            m->ops[m->nops].val = v; m->nops++; }
    return v;
}
static void m_write(void *c, uint32_t a, uint32_t v)
{
    struct model *m = c;
    if (a < 0x10000) m->mem[a] = v;
    if (m->nops < MAXOPS) { m->ops[m->nops].kind = 'W'; m->ops[m->nops].addr = a;
                            m->ops[m->nops].val = v; m->nops++; }
}
static void m_udelay(void *c, uint32_t us) { (void)c; (void)us; }

static uint32_t rnd_state = 0x12345678u;
static uint32_t rnd(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state; }

static void setup(struct model *m, struct dcn41_dev *dev)
{
    memset(m, 0, sizeof(*m));
    for (uint32_t i = 0; i < 0x10000; i++) m->mem[i] = rnd();
    int rc = dcn41_dev_init(dev, m, m_read, m_write, m_udelay, kSegs, 0x20000u, 0);
    CHECK(rc == DCN41_OK, "dev init %d", rc);
}

/* ---- 1. the addresses, hand-written ---------------------------------------------------------- */
static void test_addresses(void)
{
    static const uint32_t wexp[DCN41_TIMING_W_COUNT] = {
        0x4fea, 0x4fec, 0x4feb, 0x4fed, 0x4fef, 0x4ff0, 0x4ff1,
        0x4ff9, 0x4ff8, 0x4ffa, 0x4fee, 0x5045, 0x5046, 0x5047,
    };
    static const uint32_t rexp[DCN41_TIMING_R_COUNT] = {
        0x5003, 0x4ff3, 0x5005, 0x5015, 0x504e, 0x5049,
    };
    for (uint32_t i = 0; i < DCN41_TIMING_W_COUNT; i++)
        CHECK(0x34c0u + dcn41_timing_w_offset(i) == wexp[i],
              "w[%u] %s: 0x%x, expected 0x%x", i, dcn41_timing_w_name(i),
              0x34c0u + dcn41_timing_w_offset(i), wexp[i]);
    for (uint32_t i = 0; i < DCN41_TIMING_R_COUNT; i++)
        CHECK(0x34c0u + dcn41_timing_r_offset(i) == rexp[i],
              "r[%u] %s: 0x%x, expected 0x%x", i, dcn41_timing_r_name(i),
              0x34c0u + dcn41_timing_r_offset(i), rexp[i]);
    /* instance 1 is instance 0 + 0x80 */
    CHECK(0x34c0u + dcn41_timing_w_offset(DCN41_TW_H_TOTAL) + 0x80u == 0x506au, "OTG1_OTG_H_TOTAL");
    CHECK(dcn41_timing_w_offset(99) == DCN41_BAD_OFFSET, "out-of-range offset");
    CHECK(dcn41_timing_w_name(99)[0] == '?', "out-of-range name");
}

/* ---- 2. THE load-bearing one: writing a capture back changes nothing ------------------------- */
static void test_write_back_is_a_noop(void)
{
    struct model m; struct dcn41_dev dev;
    struct dcn41_otg_timing t;
    static uint32_t before[0x10000];

    for (uint32_t otg = 0; otg < DCN41_NUM_PIPES; otg++) {
        setup(&m, &dev);
        CHECK(dcn41_otg_read_timing(&dev, otg, &t) == DCN41_OK, "read otg %u", otg);
        CHECK(t.valid == 1 && t.otg == otg, "capture is tagged with its pipe");
        memcpy(before, m.mem, sizeof(before));
        m.nops = 0;
        CHECK(dcn41_otg_write_timing(&dev, otg, &t) == DCN41_OK, "write otg %u", otg);
        CHECK(memcmp(before, m.mem, sizeof(before)) == 0,
              "otg %u: writing the capture back left memory byte-identical", otg);
        /* and every write wrote the value that had been read from that address */
        CHECK(m.nops == DCN41_TIMING_W_COUNT, "otg %u: %d writes, expected %u", otg, m.nops,
              DCN41_TIMING_W_COUNT);
        for (int i = 0; i < m.nops; i++) {
            CHECK(m.ops[i].kind == 'W', "op %d is a write", i);
            CHECK(m.ops[i].val == before[m.ops[i].addr],
                  "otg %u write %d to 0x%x wrote 0x%08x, the address held 0x%08x", otg, i,
                  m.ops[i].addr, m.ops[i].val, before[m.ops[i].addr]);
        }
        /* a second capture equals the first */
        struct dcn41_otg_timing t2;
        CHECK(dcn41_otg_read_timing(&dev, otg, &t2) == DCN41_OK, "re-read");
        uint32_t fw = 0, fr = 0;
        CHECK(dcn41_otg_timing_diff(&t, &t2, &fw, &fr) == 0, "re-read matches the capture");
        CHECK(fw == DCN41_TIMING_W_COUNT && fr == DCN41_TIMING_R_COUNT, "no first-difference index");
    }
}

/* ---- 3. the write set: order, and what it must NEVER touch ----------------------------------- */
static void test_write_set(void)
{
    struct model m; struct dcn41_dev dev;
    struct dcn41_otg_timing t;
    /* Linux optc1_program_timing order: H_TOTAL, H_SYNC_A, H_BLANK_START_END, H_SYNC_A_CNTL,
     * V_TOTAL, V_TOTAL_MIN, V_TOTAL_MAX, V_SYNC_A, V_BLANK_START_END, V_SYNC_A_CNTL,
     * H_TIMING_CNTL, then program_global_sync's VSTARTUP / VUPDATE / VREADY. */
    static const uint32_t order[DCN41_TIMING_W_COUNT] = {
        0x4fea, 0x4fec, 0x4feb, 0x4fed, 0x4fef, 0x4ff0, 0x4ff1,
        0x4ff9, 0x4ff8, 0x4ffa, 0x4fee, 0x5045, 0x5046, 0x5047,
    };
    setup(&m, &dev);
    CHECK(dcn41_otg_read_timing(&dev, 0, &t) == DCN41_OK, "read");
    m.nops = 0;
    CHECK(dcn41_otg_write_timing(&dev, 0, &t) == DCN41_OK, "write");
    CHECK(m.nops == DCN41_TIMING_W_COUNT, "write count %d", m.nops);
    for (int i = 0; i < m.nops && i < (int)DCN41_TIMING_W_COUNT; i++)
        CHECK(m.ops[i].addr == order[i], "write %d went to 0x%x, Linux's order says 0x%x", i,
              m.ops[i].addr, order[i]);
    /* Never these, for the reasons in the header: control, status and event registers, and above all
     * the VTG enable that Linux clears because IT programs timing on a disabled pipe. */
    static const uint32_t forbidden[] = {
        0x5003,   /* OTG_CONTROL - carries OTG_MASTER_EN */
        0x4ff3,   /* OTG_V_TOTAL_CONTROL */
        0x5005,   /* OTG_INTERLACE_CONTROL */
        0x5015,   /* OTG_STEREO_CONTROL */
        0x504e,   /* OTG_GLOBAL_CONTROL0 */
        0x5049,   /* OTG_MASTER_UPDATE_LOCK */
        0x5048,   /* OTG_GLOBAL_SYNC_STATUS - the interrupt enables */
        0x5025,   /* OTG_CRC_CNTL */
        0x3aca, 0x3acb, 0x3ad3,   /* the HUBP flip registers */
    };
    for (unsigned f = 0; f < sizeof(forbidden) / sizeof(forbidden[0]); f++)
        for (int i = 0; i < m.nops; i++)
            CHECK(m.ops[i].addr != forbidden[f],
                  "write %d touched FORBIDDEN 0x%x", i, forbidden[f]);
}

/* ---- 4. refusals, all before any I/O --------------------------------------------------------- */
static void test_refusals(void)
{
    struct model m; struct dcn41_dev dev;
    struct dcn41_otg_timing t, t1;

    setup(&m, &dev);
    CHECK(dcn41_otg_read_timing(&dev, 0, &t) == DCN41_OK, "read otg 0");
    CHECK(dcn41_otg_read_timing(&dev, 1, &t1) == DCN41_OK, "read otg 1");

    m.nops = 0;
    CHECK(dcn41_otg_write_timing(&dev, 1, &t) == DCN41_E_ARG,
          "OTG0's capture must be REFUSED for OTG1");
    CHECK(m.nops == 0, "and the refusal did no I/O (%d ops)", m.nops);

    m.nops = 0;
    CHECK(dcn41_otg_read_timing(&dev, DCN41_NUM_PIPES, &t) == DCN41_E_INST, "instance 4 read refused");
    CHECK(dcn41_otg_write_timing(&dev, DCN41_NUM_PIPES, &t1) == DCN41_E_INST, "instance 4 write refused");
    CHECK(m.nops == 0, "no I/O on an out-of-range instance (%d ops)", m.nops);

    m.nops = 0;
    struct dcn41_otg_timing zero;
    memset(&zero, 0, sizeof(zero));
    CHECK(dcn41_otg_write_timing(&dev, 0, &zero) == DCN41_E_ARG, "an invalid capture is refused");
    CHECK(m.nops == 0, "and does no I/O (%d ops)", m.nops);

    CHECK(dcn41_otg_read_timing(&dev, 0, NULL) == DCN41_E_ARG, "NULL capture refused");
    CHECK(dcn41_otg_write_timing(&dev, 0, NULL) == DCN41_E_ARG, "NULL write refused");
    struct dcn41_dev bad;
    memset(&bad, 0, sizeof(bad));
    CHECK(dcn41_otg_read_timing(&bad, 0, &t) == DCN41_E_UNINIT, "uninitialised device refused");
}

/* ---- 5. diff finds a single changed dword ---------------------------------------------------- */
static void test_diff(void)
{
    struct dcn41_otg_timing a, b;
    uint32_t fw = 0, fr = 0;
    memset(&a, 0, sizeof(a)); a.valid = 1;
    memcpy(&b, &a, sizeof(b));
    CHECK(dcn41_otg_timing_diff(&a, &b, &fw, &fr) == 0, "identical captures differ in 0 dwords");
    b.w[DCN41_TW_V_TOTAL] = 1;
    CHECK(dcn41_otg_timing_diff(&a, &b, &fw, &fr) == 1, "one changed dword");
    CHECK(fw == DCN41_TW_V_TOTAL, "and it is named: %s", dcn41_timing_w_name(fw));
    b.r[DCN41_TR_OTG_CONTROL] = 7;
    CHECK(dcn41_otg_timing_diff(&a, &b, &fw, &fr) == 2, "two changed dwords");
    CHECK(fr == DCN41_TR_OTG_CONTROL, "read-only difference named: %s", dcn41_timing_r_name(fr));
    CHECK(dcn41_otg_timing_diff(NULL, &b, &fw, &fr) == 0xFFFFFFFFu, "NULL is an error, not 0");
}

/* ---- 6. the decode, against the values MEASURED on this card --------------------- */
static void test_decode_against_hardware(void)
{
    struct dcn41_otg_timing t;
    struct dcn41_timing_decoded d;
    memset(&t, 0, sizeof(t));
    t.valid = 1;
    /* verbatim from notes/logs/runs/t1a/passA.txt, OTG0 */
    t.w[DCN41_TW_H_TOTAL]            = 0x00000a9f;
    t.w[DCN41_TW_H_BLANK_START_END]  = 0x00700a70;
    t.w[DCN41_TW_H_SYNC_A]           = 0x00200000;
    t.w[DCN41_TW_V_TOTAL]            = 0x000005c8;
    t.w[DCN41_TW_V_BLANK_START_END]  = 0x002605c6;
    t.w[DCN41_TW_V_SYNC_A]           = 0x00050000;
    t.w[DCN41_TW_VSTARTUP_PARAM]     = 0x0000000d;
    t.w[DCN41_TW_VUPDATE_PARAM]      = 0x014002a8;
    t.w[DCN41_TW_VREADY_PARAM]       = 0x0000012c;
    t.r[DCN41_TR_OTG_CONTROL]        = 0x00011201;
    t.r[DCN41_TR_MASTER_UPDATE_LOCK] = 0x00000000;
    dcn41_otg_timing_decode(&t, &d);
    /* These are the SINK-A 2560x1440@60 row of dcn41_modes.tsv, which was generated from the
     * monitor's EDID on the host Mac before any register was ever read. */
    CHECK(d.h_total == 2720, "h_total %u", d.h_total);
    CHECK(d.v_total == 1481, "v_total %u", d.v_total);
    CHECK(d.h_blank_start == 2672 && d.h_blank_end == 112, "h blank %u..%u", d.h_blank_start, d.h_blank_end);
    CHECK(d.v_blank_start == 1478 && d.v_blank_end == 38, "v blank %u..%u", d.v_blank_start, d.v_blank_end);
    CHECK(d.h_active == 2560, "h_active %u (must be 2560)", d.h_active);
    CHECK(d.v_active == 1440, "v_active %u (must be 1440)", d.v_active);
    CHECK(d.h_sync_width == 32, "h sync width %u", d.h_sync_width);
    CHECK(d.v_sync_width == 5, "v sync width %u", d.v_sync_width);
    CHECK(d.vstartup == 13, "vstartup %u", d.vstartup);
    CHECK(d.vupdate_offset == 680 && d.vupdate_width == 320, "vupdate %u/%u", d.vupdate_offset,
          d.vupdate_width);
    CHECK(d.vready_offset == 300, "vready %u", d.vready_offset);
    CHECK(d.master_en == 1, "master enabled");
    CHECK(d.update_lock_held == 0, "the update lock is NOT held");
}

/* ---- 7. the one real change: adjusting v_total, and every refusal around it ------------------ */
static void test_adjust_v_total(void)
{
    struct dcn41_otg_timing t;
    struct dcn41_timing_decoded d;
    /* start from the values MEASURED on this card ( /'s dcnmode 0 capture) */
    memset(&t, 0, sizeof(t));
    t.valid = 1;
    t.w[DCN41_TW_V_TOTAL]           = 0x000005c8;   /* 1480 -> v_total 1481 */
    t.w[DCN41_TW_V_BLANK_START_END] = 0x002605c6;   /* blank start 1478, end 38 */
    t.w[DCN41_TW_H_TOTAL]           = 0x00000a9f;
    t.w[DCN41_TW_H_BLANK_START_END] = 0x00700a70;

    struct dcn41_otg_timing save = t;
    CHECK(dcn41_otg_timing_adjust_v_total(&t, 20) == DCN41_OK, "+20 lines accepted");
    dcn41_otg_timing_decode(&t, &d);
    CHECK(d.v_total == 1501, "v_total is now %u, expected 1501", d.v_total);
    CHECK(d.v_active == 1440, "the ACTIVE area is unchanged: %u", d.v_active);
    CHECK(d.h_total == 2720 && d.h_active == 2560, "horizontal is untouched: %ux%u", d.h_total,
          d.h_active);
    /* only that one dword moved */
    uint32_t fw = 0, fr = 0;
    CHECK(dcn41_otg_timing_diff(&save, &t, &fw, &fr) == 1, "exactly one dword differs");
    CHECK(fw == DCN41_TW_V_TOTAL, "and it is OTG_V_TOTAL, not %s", dcn41_timing_w_name(fw));
    /* the high bits of the register are preserved */
    CHECK((t.w[DCN41_TW_V_TOTAL] & ~0x7FFFu) == (save.w[DCN41_TW_V_TOTAL] & ~0x7FFFu),
          "bits above the 15-bit field are preserved");

    /* refusals, each leaving the capture untouched */
    struct dcn41_otg_timing u = save;
    CHECK(dcn41_otg_timing_adjust_v_total(&u, 0) == DCN41_E_ARG, "a zero delta is refused");
    CHECK(u.w[DCN41_TW_V_TOTAL] == save.w[DCN41_TW_V_TOTAL], "and changes nothing");
    CHECK(dcn41_otg_timing_adjust_v_total(&u, DCN41_VTOTAL_MAX_DELTA + 1) == DCN41_E_ARG,
          "a delta above the cap is refused");
    CHECK(dcn41_otg_timing_adjust_v_total(&u, -(DCN41_VTOTAL_MAX_DELTA + 1)) == DCN41_E_ARG,
          "and below the negative cap");
    /* 1480 - 3 = 1477, at or below the blank start 1478: the raster could not scan out */
    CHECK(dcn41_otg_timing_adjust_v_total(&u, -3) == DCN41_E_ARG,
          "a v_total that collides with the vertical blank start is REFUSED");
    /* 1480 - 2 = 1478 IS the blank start, so it is refused too; 1479 is the first legal value. */
    CHECK(dcn41_otg_timing_adjust_v_total(&u, -2) == DCN41_E_ARG, "equal to the blank start is refused");
    CHECK(dcn41_otg_timing_adjust_v_total(&u, -1) == DCN41_OK, "one line above the blank start is fine");
    u = save;
    CHECK(dcn41_otg_timing_adjust_v_total(&u, -100) == DCN41_E_ARG, "well below the blank start too");
    CHECK(u.w[DCN41_TW_V_TOTAL] == save.w[DCN41_TW_V_TOTAL], "and it changed nothing");
    u.valid = 0;
    CHECK(dcn41_otg_timing_adjust_v_total(&u, 20) == DCN41_E_ARG, "an invalid capture is refused");
    CHECK(dcn41_otg_timing_adjust_v_total(NULL, 20) == DCN41_E_ARG, "NULL is refused");
    /* the 15-bit ceiling */
    u = save; u.valid = 1;
    u.w[DCN41_TW_V_TOTAL] = 0x7FFF;
    CHECK(dcn41_otg_timing_adjust_v_total(&u, 1) == DCN41_E_ARG, "the 15-bit field ceiling holds");

    /* the refresh the change should produce, using the layer's own integer arithmetic:
     * 241500 kHz over 2720 x 1481 = 59951 mHz, over 2720 x 1501 = 59151 mHz */
    CHECK(dcn41_nominal_refresh_mhz(2415000, 2720, 1481) == 59951, "before: %llu",
          (unsigned long long)dcn41_nominal_refresh_mhz(2415000, 2720, 1481));
    CHECK(dcn41_nominal_refresh_mhz(2415000, 2720, 1501) == 59152, "after: %llu",
          (unsigned long long)dcn41_nominal_refresh_mhz(2415000, 2720, 1501));
    /* and both are inside the SINK-A's OWN EDID range descriptor, 48..180 Hz
     * (edid-capture.txt, descriptor 0xfd: 0x30..0xb4) */
    CHECK(59152 > 48000 && 59152 < 180000, "the new refresh is inside the sink's advertised range");
}

int main(void)
{
    test_addresses();
    test_write_back_is_a_noop();
    test_write_set();
    test_refusals();
    test_diff();
    test_decode_against_hardware();
    test_adjust_v_total();
    printf("test_dcn41_otg_timing: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
