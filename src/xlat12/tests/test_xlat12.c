/* Host tests for src/xlat12. Build and run: make -C src/xlat12 test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xlat12.h"
#include "golden_vectors.h"
/* build 0.0.452 item 3 (F5): kXlat12SwModeG10ToG12/XLAT12_SWMODE_MAP_COUNT only, for test_roundtrip's own
 * oracle below - same reasoning as xlat12.c's own include (xlat12_desc.h's functions are all `static inline`, so
 * this does not trip -Wunused-function the way xlat12_repack.h's ~70 plain `static` repack_* functions would). */
#include "xlat12_desc.h"

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define CAP 16384u
static uint32_t g_out[CAP];

/* ---- 1. golden vectors from Apple's real writer packets ------------------ */
static void test_golden(void)
{
    for (size_t v = 0; v < GOLDEN_COUNT; v++) {
        const GoldenVec *g = &kGolden[v];
        xlat12_ctx ctx; ctx.vs_mode = g->ngg_in ? XLAT12_VS_NGG : XLAT12_VS_UNKNOWN;
        xlat12_stats st; uint32_t n = 0;
        xlat12_status s = xlat12_translate(&ctx, g->in, g->in_len, g_out, CAP, &n, &st);
        const char *sn = xlat12_status_name(s);
        int ok = strcmp(sn + (strncmp(sn, "ERR_", 4) == 0 ? 0 : 0), g->status) == 0;
        CHECK(ok, "%s: status %s, oracle %s (err_addr 0x%x name %s)", g->name, sn, g->status,
              st.err_reg_addr, st.err_name ? st.err_name : "-");
        CHECK(st.regs_in == g->regs_in && st.regs_identical == g->identical && st.regs_moved == g->moved &&
              st.regs_repacked == g->repacked && st.regs_dropped_absent == g->dropped_absent &&
              st.regs_dropped_legacy_vs == g->dropped_legacy && st.regs_reused == g->regs_reused,
              "%s: counts in/id/mv/rp/abs/lvs/reu %u/%u/%u/%u/%u/%u/%u, oracle %u/%u/%u/%u/%u/%u/%u", g->name,
              st.regs_in, st.regs_identical, st.regs_moved, st.regs_repacked, st.regs_dropped_absent,
              st.regs_dropped_legacy_vs, st.regs_reused, g->regs_in, g->identical, g->moved, g->repacked,
              g->dropped_absent, g->dropped_legacy, g->regs_reused);
        if (s != XLAT12_OK) {
            CHECK(st.err_reg_addr == g->err_addr, "%s: err_addr 0x%x, oracle 0x%x", g->name, st.err_reg_addr, g->err_addr);
            printf("  golden %-22s %-20s at 0x%05x (%s) after %u regs\n", g->name, sn, st.err_reg_addr,
                   st.err_name ? st.err_name : "-", st.regs_in);
            continue;
        }
        CHECK(n == g->exp_len, "%s: out %u dwords, oracle %u", g->name, n, g->exp_len);
        uint32_t first_bad = UINT32_MAX;
        for (uint32_t i = 0; i < n && i < g->exp_len; i++)
            if (g_out[i] != g->exp[i]) { first_bad = i; break; }
        CHECK(first_bad == UINT32_MAX, "%s: dword %u = 0x%08x, oracle 0x%08x", g->name, first_bad,
              first_bad == UINT32_MAX ? 0 : g_out[first_bad], first_bad == UINT32_MAX ? 0 : g->exp[first_bad]);
        CHECK(st.set_packets_out == g->set_packets_out && st.set_runs_split == g->set_runs_split,
              "%s: set_out/split %u/%u, oracle %u/%u", g->name, st.set_packets_out, st.set_runs_split,
              g->set_packets_out, g->set_runs_split);
        CHECK((ctx.vs_mode == XLAT12_VS_NGG) == (g->ngg_out != 0), "%s: ngg_out", g->name);
        printf("  golden %-22s OK  in %4u -> out %4u dw; regs %u: id %u mv %u rp %u reused %u, dropped absent %u legacy-VS %u; SET out %u split %u\n",
               g->name, g->in_len, n, st.regs_in, st.regs_identical, st.regs_moved, st.regs_repacked,
               st.regs_reused, st.regs_dropped_absent, st.regs_dropped_legacy_vs, st.set_packets_out, st.set_runs_split);
    }
}

/* ---- 2. class counts over the design's record sets ------------------------ */
typedef struct { uint32_t id, mv, rp_moved, rp_same, absent, unknown; } Counts;
static Counts count_record(const GoldenReg *r, size_t n)
{
    Counts c; memset(&c, 0, sizeof c);
    for (size_t i = 0; i < n; i++) {
        uint32_t g12 = 0, cls = XLAT12_CLS_UNKNOWN;
        if (r[i].g10 == 0 || !xlat12_lookup(r[i].g10, &g12, &cls)) { c.unknown++; continue; }
        switch (cls) {
        case XLAT12_CLS_IDENTICAL: c.id++; break;
        case XLAT12_CLS_MOVED: c.mv++; break;
        case XLAT12_CLS_FIELD_REPACK: if (g12 != r[i].g10) c.rp_moved++; else c.rp_same++; break;
        case XLAT12_CLS_ABSENT: case XLAT12_CLS_LEGACY_VS: c.absent++; break;
        default: c.unknown++; break;
        }
    }
    return c;
}

static void test_class_counts(void)
{
    Counts p = count_record(g_rec_pipeline42, sizeof g_rec_pipeline42 / sizeof g_rec_pipeline42[0]);
    printf("  pipeline42: identical %u / moved %u / repack %u (moved %u + same-addr %u) / absent %u / unknown %u\n",
           p.id, p.mv, p.rp_moved + p.rp_same, p.rp_moved, p.rp_same, p.absent, p.unknown);
    /* design §K: identical 10, moved 11, moved+fields/fieldschg 17, absent 4.
     * brief triple 10/22/4 = identical / address-remapped (moved 11 + moved-with-fields 11) / absent. */
    CHECK(p.id == 10 && p.mv == 11 && p.rp_moved + p.rp_same == 17 && p.absent == 4 && p.unknown == 0,
          "pipeline42 vs design 10/11/17/4");
    CHECK(p.id == 10 && p.mv + p.rp_moved == 22 && p.absent == 4, "pipeline42 vs brief 10/22/4");

    Counts e = count_record(g_rec_encoder93, sizeof g_rec_encoder93 / sizeof g_rec_encoder93[0]);
    printf("  encoder93 : identical %u / moved %u / repack %u (moved %u + same-addr %u) / absent %u / unknown %u\n",
           e.id, e.mv, e.rp_moved + e.rp_same, e.rp_moved, e.rp_same, e.absent, e.unknown);
    /* design §K: identical 26, moved 17, moved+fields/fieldschg 23, absent 25, unknown 2.
     * EXPLAINED CORRECTION: PA_SC_FSR_EN, PA_SC_FSR_FBW_RECURSIONS_X/_Y are named by neither
     * gfx103.json nor gfx12.json nor gc_10_3_0_offset.h; the design counted them absent12, the
     * translator cannot recognise a register with no gfx10.3 address, so they are unknown here:
     * absent 25-3 = 22, unknown 2+3 = 5. The design's 2 unknowns (DB_SPI_VRS_CENTER_LOCATION,
     * SPI_BARYC_SSAA_CNTL) are in gfx12.json but not gfx103.json. Brief triple 26/29/25 ->
     * 26 / 29 / 22 with the same 3 moved to unknown. */
    CHECK(e.id == 26 && e.mv == 17 && e.rp_moved + e.rp_same == 23 && e.absent == 22 && e.unknown == 5,
          "encoder93 vs design 26/17/23/25/2 with the FSR correction (22/5)");
    CHECK(e.id == 26 && e.mv + e.rp_moved == 29 && e.absent + 3 == 25, "encoder93 vs brief 26/29/25");
}

/* ---- 3. address round trip ------------------------------------------------ */
static uint32_t set_op_for(uint32_t a)
{
    if (a >= 0x28000u && a < 0x30000u) return 0x69u;
    if (a >= 0x0B000u && a < 0x0C000u) return 0x76u;
    return 0x79u;
}
static uint32_t base_for(uint32_t op) { return op == 0x69u ? 0x28000u : op == 0x76u ? 0x0B000u : 0x30000u; }

/* build 0.0.452 item 3 (F5): the same 10 addresses xlat12.c's decide() SW_MODE-gates - CB_COLOR0-7_ATTRIB3
 * and DB_Z_INFO/DB_STENCIL_INFO - need the table-driven oracle below instead of a plain xlat12_repack_value()
 * comparison, since their gfx12 output is no longer a naive per-field bit copy for the SW_MODE sub-field. */
static int swmode_gate_for(uint32_t g10, uint32_t *inLo, uint32_t *outLo, uint32_t *outW)
{
    if (g10 == 0x28ee0u || g10 == 0x28ee4u || g10 == 0x28ee8u || g10 == 0x28eecu ||
        g10 == 0x28ef0u || g10 == 0x28ef4u || g10 == 0x28ef8u || g10 == 0x28efcu) {
        *inLo = 14u; *outLo = 15u; *outW = 3u; return 1;
    }
    if (g10 == 0x28040u || g10 == 0x28044u) { *inLo = 4u; *outLo = 4u; *outW = 5u; return 1; }
    return 0;
}

/* build 0.0.499: the 19 gfx10.3 scissor BOTTOM-RIGHT rows, listed out here (not xlat12_scissor.h's predicate) so
 * a row dropped from the translator's rule is a test failure, not a matching omission. */
static int scissor_br_row(uint32_t g10)
{
    static const uint32_t k[] = { 0x28034u, 0x28208u, 0x28244u,
        0x28254u, 0x2825cu, 0x28264u, 0x2826cu, 0x28274u, 0x2827cu, 0x28284u, 0x2828cu,
        0x28294u, 0x2829cu, 0x282a4u, 0x282acu, 0x282b4u, 0x282bcu, 0x282c4u, 0x282ccu };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) if (k[i] == g10) return 1;
    return 0;
}

static void test_roundtrip(void)
{
    uint32_t n_id = 0, n_mv = 0, n_rp = 0, n_drop = 0, n_unk = 0;
    for (uint32_t i = 0; i < xlat12_table_len(); i++) {
        uint32_t g10, g12, cls; const char *nm;
        xlat12_table_entry(i, &g10, &g12, &cls, &nm);
        uint32_t op = set_op_for(g10);
        uint32_t val = 0xA5A5A5A5u | (1u << 13);          /* PRIMGEN_EN set for the NGG rule */
        uint32_t in[3] = { 0xC0000000u | (1u << 16) | (op << 8), (g10 - base_for(op)) >> 2, val };
        xlat12_ctx ctx; ctx.vs_mode = XLAT12_VS_NGG;
        xlat12_stats st; uint32_t n;
        xlat12_status s = xlat12_translate(&ctx, in, 3, g_out, CAP, &n, &st);
        if (cls == XLAT12_CLS_UNKNOWN) {
            CHECK(s == XLAT12_ERR_UNKNOWN_REG && st.err_reg_addr == g10, "%s: unknown not refused", nm); n_unk++; continue;
        }
        if (cls == XLAT12_CLS_ABSENT || cls == XLAT12_CLS_LEGACY_VS) {
            CHECK(s == XLAT12_OK && n == 0, "%s: dropped reg emitted %u dw", nm, n); n_drop++; continue;
        }
        uint32_t oop = set_op_for(g12);
        CHECK(s == XLAT12_OK && n == 3, "%s: status %s n %u", nm, xlat12_status_name(s), n);
        if (s != XLAT12_OK || n != 3) continue;
        uint32_t got_addr = base_for(oop) + (g_out[1] << 2);
        CHECK(((g_out[0] >> 8) & 0xFFu) == oop && got_addr == g12, "%s: emitted op 0x%x addr 0x%x, table 0x%x", nm,
              (g_out[0] >> 8) & 0xFFu, got_addr, g12);
        if (cls == XLAT12_CLS_IDENTICAL) {
            CHECK(g12 == g10 && g_out[2] == val, "%s: identical round trip 0x%x->0x%x val 0x%x", nm, g10, g12, g_out[2]);
            n_id++;
        } else if (cls == XLAT12_CLS_MOVED && scissor_br_row(g10)) {
            /* build 0.0.499: CHANGED EXPECTATION. PA_SC_SCREEN_SCISSOR_BR's gfx12 BR is INCLUSIVE -
             * mesa ac_cmdbuf.c's gfx12 preamble: `S_028184_BR_X(65535) | S_028184_BR_Y(65535)); /+ inclusive bounds +/`
             * (xlat12_scissor.h) - so the moved value is BR_X - 1 / BR_Y - 1, not the value unchanged. Oracle written
             * out here, independently of xlat12_scissor.h. */
            const uint32_t want = ((((val >> 16) & 0xFFFFu) - 1u) << 16) | ((val & 0xFFFFu) - 1u);
            CHECK(g12 != g10 && g_out[2] == want, "%s: moved scissor BR got %#x want %#x", nm, g_out[2], want); n_mv++;
        } else if (cls == XLAT12_CLS_MOVED) {
            CHECK(g12 != g10 && g_out[2] == val, "%s: moved value changed", nm); n_mv++;
        } else {
            uint32_t inLo, outLo, outW;
            if (swmode_gate_for(g10, &inLo, &outLo, &outW)) {
                const uint32_t naive = xlat12_repack_value(g10, val);
                const uint32_t g10Mode = (val >> inLo) & 0x1Fu;
                uint32_t g12Mode = 0xFFFFFFFFu;
                for (uint32_t k = 0; k < XLAT12_SWMODE_MAP_COUNT; k++)
                    if (kXlat12SwModeG10ToG12[k][0] == g10Mode) { g12Mode = kXlat12SwModeG10ToG12[k][1]; break; }
                CHECK(g12Mode != 0xFFFFFFFFu, "%s: test setup - this fixed `val`'s SW_MODE must be table-mapped", nm);
                const uint32_t outMask = ((1u << outW) - 1u) << outLo;
                const uint32_t expect = (g12Mode == 0xFFFFFFFFu) ? naive : (naive & ~outMask) | ((g12Mode << outLo) & outMask);
                CHECK(g_out[2] == expect, "%s: SW_MODE remap via translate (got %#x want %#x, naive-would-be %#x)",
                      nm, g_out[2], expect, naive);
            } else if (scissor_br_row(g10)) {
                /* build 0.0.499: CHANGED EXPECTATION for PA_SC_WINDOW_SCISSOR_BR, PA_SC_GENERIC_SCISSOR_BR
                 * and PA_SC_VPORT_SCISSOR_0..15_BR: gfx12's BR is INCLUSIVE - si_state.c gfx12 `S_028208_BR_X(state->width
                 * - 1) |    /+ inclusive +/`, ac_cmdbuf.c gfx12 `S_028244_BR_X(65535) | S_028244_BR_Y(65535)); /+ inclusive
                 * bounds +/`, si_state_viewport.c gfx12 `S_028254_BR_X(final.maxx - 1)` - so the repacked 15-bit fields
                 * come out -1 each. Oracle written out here, independently of xlat12_scissor.h. */
                const uint32_t want = ((((val >> 16) & 0x7FFFu) - 1u) << 16) | ((val & 0x7FFFu) - 1u);
                CHECK(g_out[2] == want, "%s: scissor BR via translate got %#x want %#x", nm, g_out[2], want);
            } else {
                CHECK(g_out[2] == xlat12_repack_value(g10, val), "%s: repack via translate", nm);
            }
            n_rp++;
        }
    }
    printf("  round trip over %u table entries: identical %u (addr and value unchanged), moved %u, repack %u, dropped %u, refused %u\n",
           xlat12_table_len(), n_id, n_mv, n_rp, n_drop, n_unk);
}

/* ---- 4. every field repack, non-trivial values ----------------------------- */
static void test_repack_all(void)
{
    uint32_t nontrivial = 0, reg_rows = 0;
    for (size_t i = 0; i < REPACK_CASE_COUNT; i++) {
        const RepackCase *c = &kRepackCases[i];
        uint32_t got = xlat12_repack_value(c->g10, c->in);
        CHECK(got == c->exp, "%s in 0x%08x: got 0x%08x oracle 0x%08x", c->name, c->in, got, c->exp);
        if (got != c->in && got != 0) nontrivial++;
    }
    for (uint32_t i = 0; i < xlat12_table_len(); i++) {
        uint32_t g10, cls; xlat12_table_entry(i, &g10, 0, &cls, 0);
        if (cls != XLAT12_CLS_FIELD_REPACK) continue;
        reg_rows++;
        int found = 0;
        for (size_t k = 0; k < REPACK_CASE_COUNT; k++) if (kRepackCases[k].g10 == g10) { found = 1; break; }
        CHECK(found, "repack register 0x%x has no oracle case", g10);
    }
    CHECK(nontrivial > 0, "no non-trivial repack");
    printf("  repack: %u registers, %zu oracle cases, %u cases where the value changed and is non-zero\n",
           reg_rows, (size_t)REPACK_CASE_COUNT, nontrivial);

    /* Hand-derived from the JSON field lists (independent of both generators):
     * gfx103.json:12567 / gfx12.json:11386 DB_DEPTH_CONTROL: bits 30/31 renamed RESERVED_FIELD_30/31. */
    CHECK(xlat12_repack_value(0x28800u, 0xC0000077u) == 0x00000077u, "DB_DEPTH_CONTROL hand case");
    /* gfx103.json:14220 / gfx12.json:13150 SPI_PS_INPUT_ENA: gfx12 adds COVERAGE_TO_SHADER_SELECT[17:16], bits 16+ of gfx10.3 are unassigned. */
    CHECK(xlat12_repack_value(0x286CCu, 0xFFFFFFFFu) == 0x0000FFFFu, "SPI_PS_INPUT_ENA hand case");
    /* VGT_SHADER_STAGES_EN: HS_EN[2], GS_EN[5] kept; GS_FAST_LAUNCH [20:19] -> [19:19]; PRIMGEN_EN, VS_EN dropped. */
    uint32_t vse = (1u << 13) | (1u << 5) | (1u << 2) | (3u << 19) | (2u << 6);
    CHECK(xlat12_repack_value(0x28B54u, vse) == ((1u << 2) | (1u << 5) | (1u << 19)), "VGT_SHADER_STAGES_EN hand case 0x%x",
          xlat12_repack_value(0x28B54u, vse));
}

/* ---- 5. run splitting, cross-block, flags ----------------------------------- */
static void test_splitting(void)
{
    xlat12_stats st; uint32_t n;
    /* DB_EQAA, CB_COLOR_CONTROL, DB_SHADER_CONTROL: contiguous 0x28804..0x2880c on gfx10.3 */
    uint32_t in1[] = { 0xC0036900u, 0x201u, 0x11u, 0x00CC0010u, 0x33u };
    CHECK(xlat12_translate(0, in1, 5, g_out, CAP, &n, &st) == XLAT12_OK, "split: status");
    CHECK(st.set_packets_out == 3 && st.set_runs_split == 2 && n == 9, "split: %u packets, split %u, n %u",
          st.set_packets_out, st.set_runs_split, n);
    CHECK(g_out[0] == 0xC0016900u && 0x28000u + (g_out[1] << 2) == 0x28078u, "split: DB_EQAA -> 0x28078");
    CHECK(0x28000u + (g_out[4] << 2) == 0x28858u && g_out[5] == 0x00CC0010u, "split: CB_COLOR_CONTROL -> 0x28858 unchanged");
    CHECK(0x28000u + (g_out[7] << 2) == 0x2806Cu, "split: DB_SHADER_CONTROL -> 0x2806c");

    /* CB_BLEND0..7_CONTROL: identical and contiguous on both: no split */
    uint32_t in2[10] = { 0xC0086900u, 0x1E0u, 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK(xlat12_translate(0, in2, 10, g_out, CAP, &n, &st) == XLAT12_OK && st.set_runs_split == 0 && n == 10 &&
          memcmp(g_out, in2, sizeof in2) == 0, "no-split: CB_BLEND run changed");

    /* SPI_PS_INPUT_CNTL_0..31 move together (0x28644 -> 0x28664) and stay contiguous */
    uint32_t in3[34] = { 0xC0206900u, 0x191u };
    for (int i = 0; i < 32; i++) in3[2 + i] = 0x100u + (uint32_t)i;
    CHECK(xlat12_translate(0, in3, 34, g_out, CAP, &n, &st) == XLAT12_OK && st.set_packets_out == 1 && n == 34 &&
          0x28000u + (g_out[1] << 2) == 0x28664u, "SPI_PS_INPUT_CNTL run: %u packets", st.set_packets_out);

    /* VGT_GS_OUT_PRIM_TYPE: context 0x28a6c -> UCONFIG 0x30998 */
    uint32_t in4[] = { 0xC0016900u, 0x29Bu, 0x3u };
    CHECK(xlat12_translate(0, in4, 3, g_out, CAP, &n, &st) == XLAT12_OK && n == 3 &&
          ((g_out[0] >> 8) & 0xFFu) == 0x79u && 0x30000u + (g_out[1] << 2) == 0x30998u, "cross-block VGT_GS_OUT_PRIM_TYPE");

    /* header flag byte kept: SET_SH_REG with SHADER_TYPE (0x02), COMPUTE_PGM_LO 0xb830 identical */
    uint32_t in5[] = { 0xC0017602u, 0x20Cu, 0xDEAD0000u };
    CHECK(xlat12_translate(0, in5, 3, g_out, CAP, &n, &st) == XLAT12_OK && n == 3 && g_out[0] == 0xC0017602u,
          "flags: header 0x%08x", g_out[0]);

    /* empty SET (offset word only) and verbatim NOP / DRAW_INDEX_AUTO */
    uint32_t in6[] = { 0xC0006900u, 0xB4u, 0xC0021000u, 1, 2, 3, 0xC0012D00u, 3, 2 };
    CHECK(xlat12_translate(0, in6, 9, g_out, CAP, &n, &st) == XLAT12_OK && n == 7 && st.set_packets_empty == 1 &&
          memcmp(g_out, in6 + 2, 7 * sizeof(uint32_t)) == 0, "empty SET dropped, NOP/DRAW verbatim (n %u)", n);
}

/* ---- 6. positive controls: refusals ------------------------------------------ */
static void test_refusals(void)
{
    xlat12_stats st; uint32_t n; xlat12_ctx ctx;
    /* control first: the named part of a run translates */
    uint32_t okrun[] = { 0xC0016900u, 0x0F0u - 0x0F0u + 0x201u, 0x11u };
    CHECK(xlat12_translate(0, okrun, 3, g_out, CAP, &n, &st) == XLAT12_OK, "control: DB_EQAA alone translates");

    uint32_t fsr[] = { 0xC0036900u, 0x0F0u, 1, 2, 3 };        /* Apple's x3 @0x283c0 (encoder @7ffb111dae59) */
    CHECK(xlat12_translate(0, fsr, 5, g_out, CAP, &n, &st) == XLAT12_ERR_UNKNOWN_REG && st.err_reg_addr == 0x283C0u,
          "FSR run not refused (status addr 0x%x)", st.err_reg_addr);
    printf("  injected unknown 0x283c0 -> %s name %s\n", xlat12_status_name(XLAT12_ERR_UNKNOWN_REG), st.err_name ? st.err_name : "-");

    uint32_t mid[] = { 0xC0036900u, 0x1E0u - 1u + 1u, 1, 2, 3, 0xC0016900u, 0x3FFu, 9 };  /* valid run then untabled 0x28ffc */
    CHECK(xlat12_translate(0, mid, 8, g_out, CAP, &n, &st) == XLAT12_ERR_UNKNOWN_REG && st.err_reg_addr == 0x28FFCu &&
          st.err_name && strcmp(st.err_name, "UNTABLED_GFX10_ADDRESS") == 0, "untabled address not refused");

    uint32_t ml[][6] = { { 0xC0036100u, 0x1000u, 0, 0x10u, 4, 0 }, { 0xC0001200u, 0, 0, 0, 0, 0 },
                         { 0xC0012800u, 0x80000000u, 0x80000000u, 0, 0, 0 } };
    uint32_t mllen[] = { 5, 2, 3 };
    const char *mlname[] = { "LOAD_CONTEXT_REG", "CLEAR_STATE", "CONTEXT_CONTROL" };
    for (int k = 0; k < 3; k++) {
        xlat12_status s = xlat12_translate(0, ml[k], mllen[k], g_out, CAP, &n, &st);
        CHECK(s == XLAT12_ERR_MEMLOADED && st.err_name && strcmp(st.err_name, mlname[k]) == 0, "memloaded %s", mlname[k]);
    }

    /* NGG rule */
    uint32_t vs_legacy[] = { 0xC0016900u, 0x2D5u, 0x0u };    /* VGT_SHADER_STAGES_EN PRIMGEN_EN=0 */
    CHECK(xlat12_translate(0, vs_legacy, 3, g_out, CAP, &n, &st) == XLAT12_ERR_LEGACY_VS, "PRIMGEN_EN=0 not refused");
    ctx.vs_mode = XLAT12_VS_UNKNOWN;
    uint32_t vs_ngg[] = { 0xC0016900u, 0x2D5u, 1u << 13 };
    CHECK(xlat12_translate(&ctx, vs_ngg, 3, g_out, CAP, &n, &st) == XLAT12_OK && ctx.vs_mode == XLAT12_VS_NGG, "NGG not learnt");
    uint32_t lvs[] = { 0xC0017600u, 0x048u, 0x1234u };        /* SPI_SHADER_PGM_LO_VS 0xb120 */
    ctx.vs_mode = XLAT12_VS_UNKNOWN;
    CHECK(xlat12_translate(&ctx, lvs, 3, g_out, CAP, &n, &st) == XLAT12_ERR_VS_MODE_UNKNOWN, "legacy VS with unknown mode");
    lvs[2] = 0;
    CHECK(xlat12_translate(&ctx, lvs, 3, g_out, CAP, &n, &st) == XLAT12_OK && n == 0, "zero legacy VS dropped");
    lvs[2] = 0x1234u; ctx.vs_mode = XLAT12_VS_NGG;
    CHECK(xlat12_translate(&ctx, lvs, 3, g_out, CAP, &n, &st) == XLAT12_OK && n == 0 && st.regs_dropped_legacy_vs == 1,
          "legacy VS under NGG dropped");

    /* index forms: SPI_SHADER_PGM_RSRC3_PS 0xb01c -> 0xb018 (field_repack), idx 3.
     * (RSRC4_PS 0xb004 is now UNKNOWN: its gfx10.3 and gfx12 field lists share no name.) */
    uint32_t ix3[] = { 0xC0019B00u, 0x30000007u, 0x0000F00Fu };
    CHECK(xlat12_translate(0, ix3, 3, g_out, CAP, &n, &st) == XLAT12_OK && n == 3 && g_out[0] == 0xC0019B00u &&
          g_out[1] == (0x30000000u | 0x6u) && g_out[2] == xlat12_repack_value(0xB01Cu, 0x0000F00Fu),
          "SET_SH_REG_INDEX RSRC3_PS: 0x%08x 0x%08x", g_out[1], g_out[2]);
    uint32_t ix4unk[] = { 0xC0019B00u, 0x30000001u, 1u };         /* RSRC4_PS: no shared field -> refuse */
    CHECK(xlat12_translate(0, ix4unk, 3, g_out, CAP, &n, &st) == XLAT12_ERR_UNKNOWN_REG && st.err_reg_addr == 0xB004u,
          "SET_SH_REG_INDEX RSRC4_PS not refused");
    uint32_t ix0[] = { 0xC0019B00u, 0x00000001u, 1 };
    CHECK(xlat12_translate(0, ix0, 3, g_out, CAP, &n, &st) == XLAT12_ERR_BAD_INDEX, "SH index 0 accepted");
    uint32_t ixr[] = { 0xC0019B00u, 0x30010001u, 1 };
    CHECK(xlat12_translate(0, ixr, 3, g_out, CAP, &n, &st) == XLAT12_ERR_BAD_INDEX, "reserved offset bits accepted");

    /* malformed */
    uint32_t t0[] = { 0x00001234u };
    CHECK(xlat12_translate(0, t0, 1, g_out, CAP, &n, &st) == XLAT12_ERR_BAD_PACKET, "type-0 accepted");
    uint32_t tr[] = { 0xC0056900u, 0x1E0u, 1 };
    CHECK(xlat12_translate(0, tr, 3, g_out, CAP, &n, &st) == XLAT12_ERR_TRUNCATED, "truncated accepted");
    uint32_t rg[] = { 0xC0026900u, 0x1FFFu, 1, 2 };
    CHECK(xlat12_translate(0, rg, 4, g_out, CAP, &n, &st) == XLAT12_ERR_RANGE, "run past block end accepted");
    uint32_t wrap[] = { 0xC0016900u, 0xFFFFFFFFu, 1 };
    CHECK(xlat12_translate(0, wrap, 3, g_out, CAP, &n, &st) == XLAT12_ERR_RANGE, "wrapping offset accepted");
    CHECK(xlat12_translate(0, 0, 4, g_out, CAP, &n, &st) == XLAT12_ERR_ARG, "NULL input accepted");
}

/* ---- 7. capacity: never write past out_cap ------------------------------------ */
static void test_capacity(void)
{
    const GoldenVec *g = 0;
    for (size_t v = 0; v < GOLDEN_COUNT; v++) if (strcmp(kGolden[v].status, "OK") == 0) { g = &kGolden[v]; break; }
    CHECK(g != 0, "no OK golden vector");
    if (!g) return;
    uint32_t need = g->exp_len, bad = 0;
    for (uint32_t cap = 0; cap < need; cap++) {
        uint32_t *buf = (uint32_t *)malloc(((size_t)cap + 64) * sizeof(uint32_t));
        for (uint32_t k = 0; k < cap + 64; k++) buf[k] = 0xCAFEF00Du;
        xlat12_ctx ctx; ctx.vs_mode = g->ngg_in ? XLAT12_VS_NGG : XLAT12_VS_UNKNOWN;
        uint32_t n;
        xlat12_status s = xlat12_translate(&ctx, g->in, g->in_len, buf, cap, &n, 0);
        int canary = 1;
        for (uint32_t k = cap; k < cap + 64; k++) if (buf[k] != 0xCAFEF00Du) canary = 0;
        if (s != XLAT12_ERR_CAPACITY || !canary) bad++;
        free(buf);
    }
    CHECK(bad == 0, "capacity: %u of %u caps wrote past the end or did not report CAPACITY", bad, need);
    printf("  capacity: %s, every cap 0..%u -> ERR_CAPACITY, canary intact\n", g->name, need - 1);
}

/* ---- 8. build 0.0.452 item 3 (F5, CORRECTED - see xlat12.h's own XLAT12_ERR_SWMODE comment): the GENERIC
 * renderxlat path (xlat12_translate/decide, used by hw_hook_render_xlat) must remap CB_COLORn_ATTRIB3's/DB_Z_INFO's/
 * DB_STENCIL_INFO's SW_MODE BY NAME through kXlat12SwModeG10ToG12 (xlat12_desc.h) - the SAME table xlat12_ib.c's
 * draw-policy d_swmode_repack already uses (0.0.451 item 8) - refusing ONLY a mode with no gfx12 counterpart there
 * (28/31/32/reserved). A first "refuse anything but {0,27}" attempt was tried and FALSIFIED by this file's own
 * `make test`: golden_vectors.h's encoder_named_nav2/encoder_named_4vp are real Apple captures writing
 * CB_COLOR0_ATTRIB3 mode 9 through this exact path and expecting XLAT12_OK - this test pins that real case too. */
static void test_swmode_renderxlat_repack(void)
{
    xlat12_stats st; uint32_t n;
    const uint32_t kCbOff = (0x28ee0u - 0x28000u) / 4u;   /* CB_COLOR0_ATTRIB3 */
    const uint32_t kDbzOff = (0x28040u - 0x28000u) / 4u;  /* DB_Z_INFO */
    const uint32_t kDbsOff = (0x28044u - 0x28000u) / 4u;  /* DB_STENCIL_INFO */

    /* mode 22 (ADDR_SW_4KB_D_X, the item's own original bug report) at CB_COLOR0_ATTRIB3: remapped to gfx12 2,
     * not refused and not left as the naive truncated copy. */
    { uint32_t in[] = { 0xC0016900u, kCbOff, (22u << 14) | 0x1234u };
      xlat12_status s = xlat12_translate(0, in, 3, g_out, CAP, &n, &st);
      const uint32_t naive = xlat12_repack_value(0x28ee0u, (22u << 14) | 0x1234u);
      const uint32_t expect = (naive & ~(0x7u << 15)) | (2u << 15);
      const uint32_t got = g_out[2];
      const int found = (s == XLAT12_OK) && (n == 3);
      CHECK(s == XLAT12_OK && found && got == expect,
            "F5 renderxlat: CB_COLOR0_ATTRIB3 mode 22 remapped to gfx12 2 (status %s got %#x want %#x naive-would-be %#x)",
            xlat12_status_name(s), got, expect, naive);
      CHECK((naive & (0x7u << 15)) != (2u << 15), "F5 setup: mode 22's naive value genuinely differs from the fix"); }

    /* mode 9 (ADDR_SW_64KB_S - the REAL capture that falsified the refuse-everything-but-{0,27} first attempt):
     * remapped to gfx12 3, translates OK, exactly what encoder_named_nav2/4vp need. */
    { uint32_t in[] = { 0xC0016900u, kCbOff, (9u << 14) | 0x1234u };
      xlat12_status s = xlat12_translate(0, in, 3, g_out, CAP, &n, &st);
      const uint32_t naive = xlat12_repack_value(0x28ee0u, (9u << 14) | 0x1234u);
      const uint32_t expect = (naive & ~(0x7u << 15)) | (3u << 15);
      const uint32_t got = g_out[2];
      const int found = (s == XLAT12_OK) && (n == 3);
      CHECK(s == XLAT12_OK && found && got == expect,
            "F5 renderxlat: CB_COLOR0_ATTRIB3 mode 9 (real Apple capture) remapped to gfx12 3 (status %s got %#x want %#x)",
            xlat12_status_name(s), got, expect); }

    /* mode 27 (ADDR_SW_64KB_R_X): still byte-identical to the old naive copy (its own gfx12 value is 3 too). */
    { uint32_t in[] = { 0xC0016900u, kCbOff, (27u << 14) | 0x1234u };
      xlat12_status s = xlat12_translate(0, in, 3, g_out, CAP, &n, &st);
      const uint32_t naive = xlat12_repack_value(0x28ee0u, (27u << 14) | 0x1234u);
      const uint32_t got = g_out[2];
      const int found = (s == XLAT12_OK) && (n == 3);
      CHECK(s == XLAT12_OK && found && got == naive,
            "F5 renderxlat: CB_COLOR0_ATTRIB3 mode 27 still byte-identical to the naive copy (got %#x naive %#x)", got, naive); }

    /* mode 28 (ADDR_SW_VAR_Z_X): genuinely unmapped in kXlat12SwModeG10ToG12 - refused, not silently mistranslated. */
    { uint32_t in[] = { 0xC0016900u, kCbOff, (28u << 14) | 0x1234u };
      xlat12_status s = xlat12_translate(0, in, 3, g_out, CAP, &n, &st);
      CHECK(s == XLAT12_ERR_SWMODE && st.err_reg_addr == 0x28ee0u,
            "F5 renderxlat: CB_COLOR0_ATTRIB3 mode 28 (unmapped) refused (status %s addr 0x%x)", xlat12_status_name(s), st.err_reg_addr); }

    /* DB_Z_INFO mode 22 in its own [8:4] (5-bit, width unchanged) field: remapped to gfx12 2, same table. */
    { uint32_t in[] = { 0xC0016900u, kDbzOff, (22u << 4) | 0x3u };
      xlat12_status s = xlat12_translate(0, in, 3, g_out, CAP, &n, &st);
      const uint32_t naive = xlat12_repack_value(0x28040u, (22u << 4) | 0x3u);
      const uint32_t expect = (naive & ~(0x1Fu << 4)) | (2u << 4);
      const uint32_t got = g_out[2];
      const int found = (s == XLAT12_OK) && (n == 3);
      CHECK(s == XLAT12_OK && found && got == expect,
            "F5 renderxlat: DB_Z_INFO mode 22 remapped to gfx12 2 (status %s got %#x want %#x)", xlat12_status_name(s), got, expect); }

    /* DB_STENCIL_INFO mode 28 (unmapped): refused too. */
    { uint32_t in[] = { 0xC0016900u, kDbsOff, (28u << 4) | 0x3u };
      xlat12_status s = xlat12_translate(0, in, 3, g_out, CAP, &n, &st);
      CHECK(s == XLAT12_ERR_SWMODE && st.err_reg_addr == 0x28044u,
            "F5 renderxlat: DB_STENCIL_INFO mode 28 (unmapped) refused (status %s addr 0x%x)", xlat12_status_name(s), st.err_reg_addr); }
}

/* build 0.0.499: the scissor BOTTOM-RIGHT rule on the generic (renderxlat) path. Expected values
 * are written out by hand; the mesa quotes that justify -1 are in xlat12_scissor.h. */
static void sc_one(uint32_t g10, uint32_t v10, xlat12_status wantSt, uint32_t wantG12, uint32_t wantV, const char *what)
{
    xlat12_stats st; uint32_t n = 0;
    uint32_t in[] = { 0xC0016900u, (g10 - 0x28000u) >> 2, v10 };
    const xlat12_status s = xlat12_translate(0, in, 3, g_out, CAP, &n, &st);
    if (wantSt != XLAT12_OK) {
        CHECK(s == wantSt && st.err_reg_addr == g10, "0.0.499 %s: %#x=%#x status %s (want %s) err_reg %#x", what, g10, v10,
              xlat12_status_name(s), xlat12_status_name(wantSt), st.err_reg_addr);
        return;
    }
    const uint32_t at = 0x28000u + (g_out[1] << 2);
    CHECK(s == XLAT12_OK && n == 3 && at == wantG12 && g_out[2] == wantV, "0.0.499 %s: %#x=%#x -> %s n %u at %#x val %#x (want %#x val %#x)",
          what, g10, v10, xlat12_status_name(s), n, at, g_out[2], wantG12, wantV);
}

static void test_scissor499(void)
{
    /* the brief's anchor: 0x28034 = 0x00400040 -> gfx12 PA_SC_SCREEN_SCISSOR_BR 0x28184 = 0x003f003f */
    sc_one(0x28034u, 0x00400040u, XLAT12_OK, 0x28184u, 0x003f003fu, "screen BR 64x64");
    sc_one(0x28208u, 0x00400040u, XLAT12_OK, 0x28208u, 0x003f003fu, "window BR 64x64");
    sc_one(0x28244u, 0x00400040u, XLAT12_OK, 0x28244u, 0x003f003fu, "generic BR 64x64");
    for (uint32_t k = 0; k < 16u; k++)
        sc_one(0x28254u + 8u * k, 0x00400040u, XLAT12_OK, 0x28254u + 8u * k, 0x003f003fu, "viewport scissor n BR 64x64");
    /* TL unchanged (window-offset-disable bit 31 is dropped by the repack exactly as before 0.0.499) */
    sc_one(0x28030u, 0x00400040u, XLAT12_OK, 0x28180u, 0x00400040u, "screen TL unchanged");
    sc_one(0x28204u, 0x83c70370u, XLAT12_OK, 0x28204u, 0x03c70370u, "window TL unchanged (Apple's real 0x83c70370)");
    sc_one(0x28240u, 0x80000000u, XLAT12_OK, 0x28240u, 0x00000000u, "generic TL unchanged");
    for (uint32_t k = 0; k < 16u; k++)
        sc_one(0x28250u + 8u * k, 0x80020001u, XLAT12_OK, 0x28250u + 8u * k, 0x00020001u, "viewport scissor n TL unchanged");
    /* Apple's real 1920x1080 window/screen BR (0x04380780) */
    sc_one(0x28208u, 0x04380780u, XLAT12_OK, 0x28208u, 0x0437077fu, "window BR 1920x1080");
    sc_one(0x28034u, 0x04380780u, XLAT12_OK, 0x28184u, 0x0437077fu, "screen BR 1920x1080");
    /* extremes: gfx10's full range 0x4000 (16384, mesa's gfx6-11 "BR_X(16384)"), gfx10 window's 15-bit max 0x7fff, the
     * screen scissor's 16-bit max 0xffff, and high bits the 15-bit repack drops */
    sc_one(0x28034u, 0x40004000u, XLAT12_OK, 0x28184u, 0x3fff3fffu, "screen BR 0x4000 limit");
    sc_one(0x28208u, 0x40004000u, XLAT12_OK, 0x28208u, 0x3fff3fffu, "window BR 0x4000 limit");
    sc_one(0x28244u, 0x40004000u, XLAT12_OK, 0x28244u, 0x3fff3fffu, "generic BR 0x4000 limit");
    sc_one(0x28254u, 0x40004000u, XLAT12_OK, 0x28254u, 0x3fff3fffu, "viewport0 BR 0x4000 limit");
    sc_one(0x282ccu, 0x40004000u, XLAT12_OK, 0x282ccu, 0x3fff3fffu, "viewport15 BR 0x4000 limit");
    sc_one(0x28208u, 0x7fff7fffu, XLAT12_OK, 0x28208u, 0x7ffe7ffeu, "window BR 0x7fff max");
    sc_one(0x28244u, 0x7fff7fffu, XLAT12_OK, 0x28244u, 0x7ffe7ffeu, "generic BR 0x7fff max");
    sc_one(0x28254u, 0x7fff7fffu, XLAT12_OK, 0x28254u, 0x7ffe7ffeu, "viewport0 BR 0x7fff max");
    sc_one(0x28208u, 0xffffffffu, XLAT12_OK, 0x28208u, 0x7ffe7ffeu, "window BR all ones (15-bit fields)");
    sc_one(0x28034u, 0x7fff7fffu, XLAT12_OK, 0x28184u, 0x7ffe7ffeu, "screen BR 0x7fff");
    sc_one(0x28034u, 0xffffffffu, XLAT12_OK, 0x28184u, 0xfffefffeu, "screen BR 0xffff (16-bit fields)");
    sc_one(0x28208u, 0x00010001u, XLAT12_OK, 0x28208u, 0x00000000u, "window BR 1x1 -> inclusive pixel 0");
    /* empty: gfx10 BR == TL > 0 becomes BR = TL - 1 < TL (still empty, mesa's rule); a 0 component REFUSES */
    sc_one(0x28208u, 0x00050005u, XLAT12_OK, 0x28208u, 0x00040004u, "window BR == TL 5 (empty stays empty)");
    sc_one(0x28208u, 0x00000000u, XLAT12_ERR_SCISSOR, 0, 0, "window BR 0,0 refused");
    sc_one(0x28034u, 0x00400000u, XLAT12_ERR_SCISSOR, 0, 0, "screen BR x 0 refused");
    sc_one(0x28244u, 0x00000040u, XLAT12_ERR_SCISSOR, 0, 0, "generic BR y 0 refused");
    sc_one(0x28254u, 0x00000000u, XLAT12_ERR_SCISSOR, 0, 0, "viewport0 BR 0 refused");
    sc_one(0x282ccu, 0x80000000u, XLAT12_ERR_SCISSOR, 0, 0, "viewport15 BR high bits only (repacks to 0) refused");
    /* the counter */
    { xlat12_stats st; uint32_t n = 0;
      uint32_t in[] = { 0xC0036900u, (0x28204u - 0x28000u) >> 2, 0x80000000u, 0x00400040u, 0x0000ffffu };
      const xlat12_status s = xlat12_translate(0, in, 5, g_out, CAP, &n, &st);
      CHECK(s == XLAT12_OK && st.regs_scissor_adjusted == 1u && n == 5u && g_out[2] == 0x00000000u &&
            g_out[3] == 0x003f003fu && g_out[4] == 0x0000ffffu, "0.0.499 run TL/BR/CLIPRECT_RULE: %s adjusted %u out %#x %#x %#x",
            xlat12_status_name(s), st.regs_scissor_adjusted, g_out[2], g_out[3], g_out[4]); }
    CHECK(strcmp(xlat12_status_name(XLAT12_ERR_SCISSOR), "ERR_SCISSOR") == 0, "0.0.499 status name");
}

int main(void)
{
    test_golden();
    test_class_counts();
    test_roundtrip();
    test_repack_all();
    test_splitting();
    test_refusals();
    test_capacity();
    test_swmode_renderxlat_repack();
    test_scissor499();
    printf("xlat12 tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
