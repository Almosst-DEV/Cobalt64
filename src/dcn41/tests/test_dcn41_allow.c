/*
 * test_dcn41_allow.c - host unit tests for the DCN 4.1 display WRITE ALLOWLIST (src/dcn41/dcn41_allow.c).
 *
 * Every expected address here is written out BY HAND from the discovery dump and the Linux offset
 * header, never read back from the generated table, so the guard and its table cannot pass by
 * agreeing with each other:
 *
 *   OTG0_OTG_CONTROL          seg2 0x34c0 + 0x1b43 = 0x5003   (and the census read it as 0x00011201)
 *   HUBPREQ0_..SURFACE_ADDRESS seg2 0x34c0 + 0x60a  = 0x3aca
 *   OTG0_OTG_GLOBAL_SYNC_STATUS seg2 0x34c0 + 0x1b88 = 0x5048  (the only register T2-VBL writes)
 *   DCCG OTG0_PIXEL_RATE_CNTL  seg1 0x00c0 + 0x80   = 0x0140
 *   MPCC0_MPCC_TOP_SEL         seg3 0x9000 + 0x0    = 0x9000
 *   MP1 SMU message mailbox    MP1 seg1 0x16200 + 0x82 = 0x16282   (regMP1_SMN_C2PMSG_66)
 *   MP1 SMU parameter          0x16200 + 0x92 = 0x16292
 *   MP1 SMU response           0x16200 + 0x9a = 0x1629a
 *   NBIO BIF_BX1_PCIE_INDEX2   NBIO seg0 0x0 + 0xe  = 0x000e    (SMN indirect window)
 *   NBIO RSMU_INDEX/DATA       NBIO seg1 0x14 + 0x0 = 0x0014 / 0x0015
 *   MM_INDEX / MM_DATA         0x0000 / 0x0001
 *
 * The three the brief names explicitly are test_positive_control(), test_smu_refused() and
 * test_out_of_range_refused().
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "dcn41_allow.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* This card's DMU segment bases (notes/logs/runs/adopt1/driverlog-post.txt:1371). */
static const uint32_t kSegs[5] = { 0x00000012u, 0x000000c0u, 0x000034c0u, 0x00009000u, 0x02403c00u };
/* A generous register window, used for most cases below. */
#define BAR5_DWORDS 0x40000u
/* The window this card actually maps, quoted from the kext's own line in
 * notes/logs/runs/t2a/driverlog.txt: "dcn: display layer ARMED - segs 00000012 000000c0 000034c0
 * 00009000 02403c00, BAR5 131072 dwords". 131072 dwords is 512 KiB, and the SMU message mailbox at
 * 0x16282 is INSIDE it - so the SMU denial is load-bearing, not theoretical, and the window check
 * alone would never have caught it. test_measured_window() pins that. */
#define BAR5_MEASURED 0x20000u

static void arm(struct dcn41_allow_state *st)
{
    int rc = dcn41_allow_init(st, kSegs, BAR5_DWORDS);
    CHECK(rc == DCN41_ALLOW_OK, "init with the card's own bases must arm, got %s",
          dcn41_allow_reason_name(rc));
    CHECK(st->armed == 1u, "armed flag");
}

/* ---- 1. the positive control: a legal display write passes -------------------------------- */
static void test_positive_control(void)
{
    struct dcn41_allow_state st;
    const char *name = 0, *why = 0;
    static const struct { uint32_t a; const char *tag; } legal[] = {
        { 0x00005003u, "otgctl" },      /* OTG0_OTG_CONTROL */
        { 0x00005048u, "irqset"  },     /* OTG0_OTG_GLOBAL_SYNC_STATUS - T2-VBL's only write */
        { 0x00005049u, "otglock" },     /* OTG0_OTG_MASTER_UPDATE_LOCK */
        { 0x00003acau, "flip"    },     /* HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS */
        { 0x00003acbu, "flip"    },     /* ..._HIGH */
        { 0x00003ad3u, "flip"    },     /* HUBPREQ0_DCSURF_FLIP_CONTROL */
        { 0x00000140u, "dccg"    },     /* OTG0_PIXEL_RATE_CNTL, base 1 */
        { 0x00009000u, "mpc"     },     /* MPCC0_MPCC_TOP_SEL, base 3 */
        { 0x000036b6u, "dmub"    },     /* DMCUB_CNTL */
        { 0x000056a1u, "dig"     },     /* DIG1_DIG_BE_EN_CNTL - the lit back end */
    };
    unsigned i;

    arm(&st);
    for (i = 0; i < sizeof(legal) / sizeof(legal[0]); i++) {
        bool ok = dcn41_allow_write(&st, legal[i].a, 0x12345678u, legal[i].tag);
        CHECK(ok, "legal display write 0x%08x (%s) must be ALLOWED", legal[i].a, legal[i].tag);
        CHECK(dcn41_allow_classify(legal[i].a, BAR5_DWORDS, &name, &why) == DCN41_ALLOW_OK,
              "classify 0x%08x OK", legal[i].a);
        CHECK(name != 0, "an allowed address names its range (0x%08x)", legal[i].a);
    }
    CHECK(st.allowed == 10u, "allowed count %" PRIu64, st.allowed);
    CHECK(st.refused == 0u, "no refusals on the positive control, got %" PRIu64, st.refused);
    /* samples, not just counters */
    CHECK(st.n_allow_samples == 10u, "allow samples %u", st.n_allow_samples);
    CHECK(st.allow[0].abs_dword == 0x00005003u && st.allow[0].value == 0x12345678u
          && strcmp(st.allow[0].tag, "otgctl") == 0 && st.allow[0].seq == 1u,
          "first allow sample carries address, value, tag and sequence");
    CHECK(strcmp(st.allow[1].tag, "irqset") == 0 && st.allow[1].seq == 2u, "second allow sample");
}

/* ---- 2. an SMU-range write is refused and counted ------------------------------------------ */
static void test_smu_refused(void)
{
    struct dcn41_allow_state st;
    const char *why = 0;
    static const uint32_t smu[] = {
        0x00016282u,   /* regMP1_SMN_C2PMSG_66 - the SMU message register */
        0x00016292u,   /* regMP1_SMN_C2PMSG_82 - the parameter */
        0x0001629au,   /* regMP1_SMN_C2PMSG_90 - the response */
        0x00016000u,   /* MP1 segment 0 base */
        0x0001ce00u,   /* MP0/PSP segment 2 base */
        0x02458000u,   /* SMUIO-class block */
    };
    unsigned i;

    arm(&st);
    for (i = 0; i < sizeof(smu) / sizeof(smu[0]); i++) {
        int r = dcn41_allow_classify(smu[i], BAR5_DWORDS, 0, &why);
        bool ok = dcn41_allow_write(&st, smu[i], 0xdeadbeefu, "smu-probe");
        CHECK(!ok, "SMU-range write 0x%08x must be REFUSED", smu[i]);
        /* 0x16282 and friends are past a 1 MiB BAR5, so WINDOW catches them first; the named-block
         * denial is what catches them when the window is larger. Either way it is never OK. */
        CHECK(r == DCN41_ALLOW_BLOCK || r == DCN41_ALLOW_WINDOW,
              "0x%08x refused as BLOCK or WINDOW, got %s", smu[i], dcn41_allow_reason_name(r));
        CHECK(dcn41_allow_classify(smu[i], 0x4000000u, 0, &why) == DCN41_ALLOW_BLOCK,
              "with a window big enough to reach it, 0x%08x is refused as a NAMED BLOCK", smu[i]);
        CHECK(why != 0, "the named-block refusal carries its documentation (0x%08x)", smu[i]);
    }
    CHECK(st.refused == 6u, "refused count %" PRIu64, st.refused);
    CHECK(st.allowed == 0u, "nothing allowed");
    CHECK(st.n_refuse_samples == 6u, "refusal samples kept: %u", st.n_refuse_samples);
    CHECK(st.refuse[0].abs_dword == 0x00016282u && st.refuse[0].value == 0xdeadbeefu
          && strcmp(st.refuse[0].tag, "smu-probe") == 0,
          "the refusal sample names the address, the value and the caller");
    CHECK(st.last_refused_abs == 0x02458000u, "last refused address recorded 0x%08x",
          st.last_refused_abs);
}

/* ---- 3. an out-of-range write is refused and counted ---------------------------------------- */
static void test_out_of_range_refused(void)
{
    struct dcn41_allow_state st;
    const char *why = 0;
    static const struct { uint32_t a; int reason; const char *what; } bad[] = {
        { 0x00000000u, DCN41_ALLOW_INDIRECT, "MM_INDEX" },
        { 0x00000001u, DCN41_ALLOW_INDIRECT, "MM_DATA" },
        { 0x00000006u, DCN41_ALLOW_INDIRECT, "MM_INDEX_HI" },
        { 0x0000000eu, DCN41_ALLOW_INDIRECT, "BIF_BX1_PCIE_INDEX2" },
        { 0x0000000fu, DCN41_ALLOW_INDIRECT, "BIF_BX1_PCIE_DATA2" },
        { 0x00000014u, DCN41_ALLOW_INDIRECT, "BIF_BX_PF0_RSMU_INDEX" },
        { 0x00000015u, DCN41_ALLOW_INDIRECT, "BIF_BX_PF0_RSMU_DATA" },
        { 0x00000020u, DCN41_ALLOW_INDIRECT, "BIF_BX0_PCIE_INDEX" },
        { 0x00000012u, DCN41_ALLOW_INDIRECT, "DCN segment 0 base (audio, overlaps the SMN windows)" },
        { 0x0000005fu, DCN41_ALLOW_INDIRECT, "DCN segment 0 top" },
        { 0x000000ffu, DCN41_ALLOW_INDIRECT, "last dword of the hard-denied low page" },
        /* DCN segment 1's own base is 0xc0, i.e. inside the hard-denied low page; the first display
         * register on that segment is at offset 0x40, absolute 0x100, which is the first allowed
         * dword in the whole table. So the segment base itself is refused as INDIRECT, not RANGE. */
        { 0x000000c0u, DCN41_ALLOW_INDIRECT, "DCN segment 1 base itself, inside the low page" },
        { 0x000000feu, DCN41_ALLOW_INDIRECT, "one dword below the first allowed address" },
        { 0x00002000u, DCN41_ALLOW_RANGE,    "a GC-block address in the gap between display ranges" },
        { 0x0000a000u, DCN41_ALLOW_RANGE,    "GC BASE_IDX 1, one dword past the top display range" },
        { 0x00003400u, DCN41_ALLOW_RANGE,    "just below DCN segment 2's first display register" },
        { 0x0003ffffu, DCN41_ALLOW_RANGE,    "the last dword of BAR5" },
        { 0x00040000u, DCN41_ALLOW_WINDOW,   "one dword past BAR5" },
        { 0x0fffffffu, DCN41_ALLOW_WINDOW,   "far outside BAR5 (the census's own negative control)" },
        { 0xffffffffu, DCN41_ALLOW_WINDOW,   "DCN41_BAD_OFFSET" },
    };
    unsigned i;
    uint64_t expect_indirect = 0, expect_range = 0, expect_window = 0;

    arm(&st);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int r = dcn41_allow_classify(bad[i].a, BAR5_DWORDS, 0, &why);
        bool ok = dcn41_allow_write(&st, bad[i].a, 0xa5a5a5a5u, "stray");
        CHECK(!ok, "0x%08x (%s) must be REFUSED", bad[i].a, bad[i].what);
        CHECK(r == bad[i].reason, "0x%08x (%s): expected %s, got %s", bad[i].a, bad[i].what,
              dcn41_allow_reason_name(bad[i].reason), dcn41_allow_reason_name(r));
        CHECK(why != 0, "0x%08x carries a refusal reason text", bad[i].a);
        if (bad[i].reason == DCN41_ALLOW_INDIRECT) expect_indirect++;
        else if (bad[i].reason == DCN41_ALLOW_RANGE) expect_range++;
        else expect_window++;
    }
    CHECK(st.refused == sizeof(bad) / sizeof(bad[0]), "all refused: %" PRIu64, st.refused);
    CHECK(st.by_reason[DCN41_ALLOW_INDIRECT] == expect_indirect, "INDIRECT count %" PRIu64,
          st.by_reason[DCN41_ALLOW_INDIRECT]);
    CHECK(st.by_reason[DCN41_ALLOW_RANGE] == expect_range, "RANGE count %" PRIu64,
          st.by_reason[DCN41_ALLOW_RANGE]);
    CHECK(st.by_reason[DCN41_ALLOW_WINDOW] == expect_window, "WINDOW count %" PRIu64,
          st.by_reason[DCN41_ALLOW_WINDOW]);
    CHECK(st.by_reason[DCN41_ALLOW_OK] == 0u, "nothing classified OK");
    /* sample capacity: 19 refusals into 16 slots, the overflow counted not lost */
    CHECK(st.n_refuse_samples == DCN41_ALLOW_SAMPLES, "samples capped at %u, got %u",
          DCN41_ALLOW_SAMPLES, st.n_refuse_samples);
    CHECK(st.refuse_dropped == (uint32_t)(sizeof(bad) / sizeof(bad[0])) - DCN41_ALLOW_SAMPLES,
          "dropped samples counted: %u", st.refuse_dropped);
}

/* ---- 4. deny by default: the guard is inert until it is armed with THIS card's bases -------- */
static void test_deny_by_default(void)
{
    struct dcn41_allow_state st;
    uint32_t wrong[5];
    int rc, i;

    /* never initialised at all */
    memset(&st, 0, sizeof(st));
    CHECK(!dcn41_allow_write(&st, 0x00005003u, 1u, "otgctl"),
          "an un-inited state refuses even a legal display address");
    CHECK(st.by_reason[DCN41_ALLOW_UNARMED] == 1u, "counted as UNARMED");

    /* initialised with bases that are not this card's */
    for (i = 0; i < 5; i++)
        wrong[i] = kSegs[i];
    wrong[2] = 0x000034c1u;                     /* one dword off on the main DCN segment */
    rc = dcn41_allow_init(&st, wrong, BAR5_DWORDS);
    CHECK(rc == DCN41_ALLOW_UNARMED, "init refuses bases that are not the ones the table was built for");
    CHECK(st.armed == 0u, "and leaves the guard unarmed");
    CHECK(!dcn41_allow_write(&st, 0x00005003u, 1u, "otgctl"), "so every write is refused");

    /* a window too small to contain the table's own top range */
    rc = dcn41_allow_init(&st, kSegs, 0x1000u);
    CHECK(rc == DCN41_ALLOW_UNARMED, "init refuses a BAR5 window smaller than the allowed ranges");
    rc = dcn41_allow_init(&st, kSegs, 0u);
    CHECK(rc == DCN41_ALLOW_UNARMED, "init refuses a zero window");

    /* a NULL state must not crash and must refuse */
    CHECK(!dcn41_allow_write(0, 0x00005003u, 1u, "otgctl"), "a NULL state refuses");
}

/* ---- 5. the table itself: disjoint, ordered, inside BAR5, and it covers what we need -------- */
static void test_table_shape(void)
{
    uint32_t n = dcn41_allow_range_count(), i;
    uint32_t prev_hi = 0, total = 0;

    CHECK(n >= 8u && n <= 64u, "a plausible number of ranges: %u", n);
    for (i = 0; i < n; i++) {
        uint32_t lo = 0, hi = 0, b = 0;
        const char *name = 0;
        CHECK(dcn41_allow_range_at(i, &lo, &hi, &b, &name) == 0, "range %u readable", i);
        CHECK(lo <= hi, "range %u ordered (0x%08x..0x%08x)", i, lo, hi);
        CHECK(hi < BAR5_DWORDS, "range %u inside BAR5 (top 0x%08x)", i, hi);
        CHECK(lo >= 0x100u, "range %u starts above the hard-denied low page (0x%08x)", i, lo);
        CHECK(b == 1u || b == 2u || b == 3u, "range %u comes from segment 1, 2 or 3 (got %u)", i, b);
        CHECK(name != 0 && name[0] != '\0', "range %u is named", i);
        if (i)
            CHECK(lo > prev_hi, "range %u starts after range %u ends", i, i - 1u);
        prev_hi = hi;
        total += hi - lo + 1u;
    }
    CHECK(total == dcn41_allow_total_dwords(), "the declared total matches the ranges: %u vs %u",
          total, dcn41_allow_total_dwords());
    /* The whole point: the writable surface is a small fraction of the register aperture. */
    CHECK(total < BAR5_DWORDS / 8u, "writable dwords %u are under an eighth of BAR5 (%u)", total,
          BAR5_DWORDS);
    CHECK(dcn41_allow_reason_name(DCN41_ALLOW_INDIRECT)[0] == 'I', "reason names present");
    CHECK(dcn41_allow_reason_name(99)[0] == '?', "an unknown reason does not read out of bounds");
}

/* ---- 5b. the window this card actually maps ------------------------------------------------- */
static void test_measured_window(void)
{
    struct dcn41_allow_state st;
    uint32_t n = dcn41_allow_range_count(), i;
    int rc;

    CHECK(0x00016282u < BAR5_MEASURED,
          "the SMU mailbox 0x16282 is INSIDE this card's %u-dword register window", BAR5_MEASURED);
    CHECK(dcn41_allow_classify(0x00016282u, BAR5_MEASURED, 0, 0) == DCN41_ALLOW_BLOCK,
          "and is refused as a NAMED BLOCK, not merely as out-of-window");
    CHECK(dcn41_allow_classify(0x0001629au, BAR5_MEASURED, 0, 0) == DCN41_ALLOW_BLOCK,
          "so is the SMU response register 0x1629a");
    for (i = 0; i < n; i++) {
        uint32_t lo = 0, hi = 0;
        (void)dcn41_allow_range_at(i, &lo, &hi, 0, 0);
        CHECK(hi < BAR5_MEASURED, "range %u (top 0x%08x) fits the measured window", i, hi);
    }
    rc = dcn41_allow_init(&st, kSegs, BAR5_MEASURED);
    CHECK(rc == DCN41_ALLOW_OK, "init arms against the measured window");
    CHECK(dcn41_allow_write(&st, 0x00005048u, 0u, "irqset"),
          "and the one register T2-VBL writes is allowed there");
    CHECK(!dcn41_allow_write(&st, 0x00016282u, 0u, "smu"), "while the SMU mailbox is not");
    CHECK(st.by_reason[DCN41_ALLOW_BLOCK] == 1u, "counted under BLOCK: %" PRIu64,
          st.by_reason[DCN41_ALLOW_BLOCK]);
}

/* ---- 6. reads are counted, never gated ----------------------------------------------------- */
static void test_reads(void)
{
    struct dcn41_allow_state st;
    arm(&st);
    dcn41_allow_note_read(&st);
    dcn41_allow_note_read(&st);
    CHECK(st.reads_seen == 2u, "reads counted: %" PRIu64, st.reads_seen);
    CHECK(st.writes_seen == 0u, "reads are not writes");
    dcn41_allow_note_read(0);              /* must not crash */
}

int main(void)
{
    test_positive_control();
    test_smu_refused();
    test_out_of_range_refused();
    test_deny_by_default();
    test_table_shape();
    test_measured_window();
    test_reads();
    printf("test_dcn41_allow: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
