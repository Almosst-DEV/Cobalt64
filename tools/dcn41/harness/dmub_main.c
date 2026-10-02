/* dmub_main.c - DMUB harness driver: Linux's compiled DMUB code vs src/dcn41/dcn41_dmub.c.
 *
 *   dcn41_dmub_harness <trace-dump-file>
 *
 * Comparison per kind: command builders - the 64 command bytes (and no register I/O on either side); GPINT and idle
 * wait - the full register trace and results; ring submit - every register WRITE (our layer adds deliberate reads)
 * plus the ring memory after the sequence and each submission's result; return data and fw meta - bytes / results.
 * Planted controls must differ. Also dumps, as evidence for notes/DISPLAY-DESIGN.md, the register trace of Linux's
 * dmub_srv_hw_reset() on a live firmware: what a DMUB reload does first. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "trace.h"
#include "dmub_scen.h"

void DL_reset_evidence(FILE *f);
void DL_dump_translations(FILE *f);

#define CAP 8192u
static uint8_t ring_l[CAP], ring_o[CAP];
static struct trace tl, to;
static const char *names[DS_KIND_COUNT] = { "transmitter", "encoder", "pixel_clock", "power_gating", "gpint",
                                            "ring_submit", "wait_idle", "return_data", "fw_meta" };
static long runs, matched, per_kind[DS_KIND_COUNT], events;
static int dumped[DS_KIND_COUNT];

static void setup_model(const struct dmub_scen *s, uint64_t seed, struct trace *t, uint8_t *ring)
{
    struct model_behaviour b;
    uint32_t i;
    memset(&b, 0, sizeof(b));
    b.lock_after = -1;
    b.gpint_reg = DL_abs("GPINT_DATAIN1");
    b.gpint_after = s->gpint_after;
    if (s->kind == DS_WAIT_IDLE) {
        b.rptr_reg = DL_abs("INBOX1_RPTR");
        b.wptr_reg = DL_abs("INBOX1_WPTR");
        b.rptr_step = s->rptr_step;
        b.ring_capacity = CAP;
    }
    model_reset(seed, &b, t);
    if (s->kind == DS_RING_SUBMIT || s->kind == DS_WAIT_IDLE || s->kind == DS_RETURN_DATA) {
        model_poke(DL_abs("INBOX1_WPTR"), s->start_ptr);
        model_poke(DL_abs("INBOX1_RPTR"), (s->start_ptr + CAP - (s->rptr_lag * 64u) % CAP) % CAP);
    }
    for (i = 0; i < CAP; i++)          /* only this side's ring: the other side's result must survive until compared */
        ring[i] = (uint8_t)(i * 7 + seed);
}

static int same_events(const struct trace *a, const struct trace *b, int writes_only)
{
    int i = 0, j = 0;
    for (;;) {
        while (writes_only && i < a->n && a->ev[i].op != 'W') i++;
        while (writes_only && j < b->n && b->ev[j].op != 'W') j++;
        if (i >= a->n || j >= b->n)
            return i >= a->n && j >= b->n;
        if (a->ev[i].op != b->ev[j].op || a->ev[i].addr != b->ev[j].addr || a->ev[i].val != b->ev[j].val)
            return 0;
        i++; j++;
    }
}

static void dump(FILE *f, const char *who, const struct trace *t, const struct dmub_out *o)
{
    int i;
    fprintf(f, "  %s: rc %d, %d events, outputs", who, o->rc, t->n);
    for (i = 0; i < o->nv; i++) fprintf(f, " %" PRId64, o->v[i]);
    fprintf(f, "\n");
    if (o->nbytes) {
        fprintf(f, "    bytes:");
        for (i = 0; i < (int)o->nbytes; i++) fprintf(f, "%s%02x", i % 16 ? "" : "\n     ", o->bytes[i]);
        fprintf(f, "\n");
    }
    for (i = 0; i < t->n && i < 40; i++) fprintf(f, "    %c %05x %08x\n", t->ev[i].op, t->ev[i].addr, t->ev[i].val);
    if (t->n > 40) fprintf(f, "    ... %d more\n", t->n - 40);
}

static int run_pair(const struct dmub_scen *ls, const struct dmub_scen *os, uint64_t seed, FILE *f, int force_dump,
                    int count)
{
    struct dmub_atom atom;
    struct dmub_out lo, oo;
    int same, rl, ro;

    memset(&atom, 0, sizeof(atom));
    setup_model(ls, seed, &tl, ring_l);
    rl = DL_run(ls, &atom, &lo, ring_l, CAP);
    setup_model(os, seed, &to, ring_o);
    ro = DO_run(os, &atom, &oo, ring_o, CAP);
    same = rl == 0 && ro == 0 && lo.rc == oo.rc && lo.nbytes == oo.nbytes && !memcmp(lo.bytes, oo.bytes, lo.nbytes) &&
           lo.nv == oo.nv && !memcmp(lo.v, oo.v, sizeof(lo.v[0]) * (size_t)lo.nv) && !tl.overflow && !to.overflow;
    switch (ls->kind) {
    case DS_TRANSMITTER: case DS_ENCODER: case DS_PIXEL_CLOCK: case DS_POWER_GATING:
        same = same && lo.rc == 0 && lo.nbytes == 64 && tl.n == 0 && to.n == 0;
        break;
    case DS_GPINT: case DS_WAIT_IDLE:
        same = same && same_events(&tl, &to, 0);
        break;
    case DS_RING_SUBMIT:
        same = same && same_events(&tl, &to, 1) && !memcmp(ring_l, ring_o, CAP);
        break;
    default:
        break;
    }
    if (count) {
        runs++; matched += same; per_kind[ls->kind]++; events += tl.n;
    }
    if (f && (force_dump || !same || !dumped[ls->kind])) {
        dumped[ls->kind] = 1;
        fprintf(f, "%s seed=%" PRIu64 " action=%d tx=%d eng=%d sig=%d pclk=%u pclk100=%u gpint=%u/%u after=%d "
                   "start=%u lag=%u n=%u step=%u meta=%u@%u bss=%u: %s\n",
                names[ls->kind], seed, ls->action, ls->transmitter, ls->engine, ls->signal, ls->pixel_clock_khz,
                ls->pixel_clock_100hz, ls->gpint_cmd, ls->gpint_param, ls->gpint_after, ls->start_ptr, ls->rptr_lag,
                ls->ncmds, ls->rptr_step, ls->meta_blob_size, ls->meta_at, ls->bss, same ? "MATCH" : "DIFF");
        dump(f, "linux", &tl, &lo);
        dump(f, "ours ", &to, &oo);
        if (ls->kind == DS_RING_SUBMIT && memcmp(ring_l, ring_o, CAP))
            fprintf(f, "  ring memory differs\n");
    }
    return same;
}

static void all_seeds(const struct dmub_scen *s, FILE *f)
{
    static const uint64_t seeds[] = { 0, ~0ull, 3, 0xfeedULL };
    size_t i;
    for (i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
        run_pair(s, s, seeds[i], f, 0, 1);
}

#define EN(x) DL_enum(#x)

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "w") : NULL;
    struct dmub_scen s, l, o;
    int k, a, t, e, sg, cd, ctl, pll, tcd, fl, lt, n, st;
    long controls = 0, caught = 0;
    static const uint32_t pclk[] = { 148500, 594000, 241500, 25175 };
    static const char *tx_actions[] = { "TRANSMITTER_CONTROL_INIT", "TRANSMITTER_CONTROL_ENABLE",
                                        "TRANSMITTER_CONTROL_DISABLE", "TRANSMITTER_CONTROL_SET_VOLTAGE_AND_PREEMPASIS",
                                        "TRANSMITTER_CONTROL_POWER_ON" };
    static const char *txs[] = { "TRANSMITTER_UNIPHY_A", "TRANSMITTER_UNIPHY_B", "TRANSMITTER_UNIPHY_C",
                                 "TRANSMITTER_UNIPHY_D", "TRANSMITTER_UNIPHY_E" };
    static const char *engs[] = { "ENGINE_ID_DIGA", "ENGINE_ID_DIGB", "ENGINE_ID_DIGC", "ENGINE_ID_DIGD" };
    static const char *sigs[] = { "SIGNAL_TYPE_HDMI_TYPE_A", "SIGNAL_TYPE_DISPLAY_PORT", "SIGNAL_TYPE_DVI_SINGLE_LINK" };
    static const char *hpds[] = { "HPD_SOURCEID1", "HPD_SOURCEID2", "HPD_SOURCEID3", "HPD_SOURCEID4" };
    static const char *hpos[] = { "ENGINE_ID_HPO_0", "ENGINE_ID_HPO_DP_1", "ENGINE_ID_UNKNOWN" };
    static const char *enc_actions[] = { "ENCODER_CONTROL_SETUP", "ENCODER_CONTROL_ENABLE", "ENCODER_CONTROL_DISABLE",
                                         "ENCODER_CONTROL_INIT" };
    static const char *depths[] = { "COLOR_DEPTH_888", "COLOR_DEPTH_101010", "COLOR_DEPTH_121212", "COLOR_DEPTH_161616" };
    static const char *ctls[] = { "CONTROLLER_ID_D0", "CONTROLLER_ID_D1", "CONTROLLER_ID_D2", "CONTROLLER_ID_D3" };
    static const char *plls[] = { "CLOCK_SOURCE_COMBO_PHY_PLL0", "CLOCK_SOURCE_COMBO_PHY_PLL1",
                                  "CLOCK_SOURCE_COMBO_PHY_PLL2", "CLOCK_SOURCE_COMBO_PHY_PLL3", "CLOCK_SOURCE_ID_DP_DTO" };
    static const char *encs[] = { "ENCODER_ID_INTERNAL_UNIPHY", "ENCODER_ID_INTERNAL_UNIPHY1",
                                  "ENCODER_ID_INTERNAL_UNIPHY2", "ENCODER_ID_INTERNAL_UNIPHY3" };
    static const char *tcds[] = { "TRANSMITTER_COLOR_DEPTH_24", "TRANSMITTER_COLOR_DEPTH_30",
                                  "TRANSMITTER_COLOR_DEPTH_36", "TRANSMITTER_COLOR_DEPTH_48" };
    static const char *sigs_pc[] = { "SIGNAL_TYPE_HDMI_TYPE_A", "SIGNAL_TYPE_DISPLAY_PORT", "SIGNAL_TYPE_DVI_DUAL_LINK" };
    static const uint32_t pclk100[] = { 1485000, 5940000, 2415000, 6466400 };

    DL_init();
    DO_init();

    for (a = 0; a < 5; a++) for (t = 0; t < 5; t++) for (sg = 0; sg < 3; sg++) for (k = 0; k < 4; k++) {
        memset(&s, 0, sizeof(s)); s.kind = DS_TRANSMITTER;
        s.action = DL_enum(tx_actions[a]); s.transmitter = DL_enum(txs[t]); s.engine = DL_enum(engs[k % 4]);
        s.hpo_engine = DL_enum(hpos[(a + k) % 3]); s.signal = DL_enum(sigs[sg]); s.hpd = DL_enum(hpds[(t + k) % 4]);
        s.connector = sg == 1 ? EN(CONNECTOR_ID_DISPLAY_PORT) : EN(CONNECTOR_ID_HDMI_TYPE_A);
        s.lanes = k % 2 ? EN(LANE_COUNT_ONE) : EN(LANE_COUNT_FOUR); s.pixel_clock_khz = pclk[k];
        s.lane_settings = 0x05 + k;
        run_pair(&s, &s, 0, f, 0, 1);
    }
    for (a = 0; a < 4; a++) for (e = 0; e < 4; e++) for (sg = 0; sg < 3; sg++) for (cd = 0; cd < 4; cd++) {
        memset(&s, 0, sizeof(s)); s.kind = DS_ENCODER;
        s.action = DL_enum(enc_actions[a]); s.engine = DL_enum(engs[e]); s.signal = DL_enum(sigs[sg]);
        s.lanes = EN(LANE_COUNT_FOUR); s.color_depth = DL_enum(depths[cd]); s.pixel_clock_khz = pclk[(e + cd) % 4];
        s.dp_audio = (a + e) % 2;
        run_pair(&s, &s, 0, f, 0, 1);
    }
    for (ctl = 0; ctl < 4; ctl++) for (pll = 0; pll < 5; pll++) for (sg = 0; sg < 3; sg++) for (tcd = 0; tcd < 4; tcd++) {
        memset(&s, 0, sizeof(s)); s.kind = DS_PIXEL_CLOCK;
        s.controller = DL_enum(ctls[ctl]); s.pll = DL_enum(plls[pll]); s.signal = DL_enum(sigs_pc[sg]);
        s.pixel_clock_100hz = pclk100[(ctl + tcd) % 4]; s.encoder_obj = DL_enum(encs[(pll + ctl) % 4]);
        s.tx_color_depth = DL_enum(tcds[tcd]);
        for (fl = 0; fl < 32; fl += 7) { s.flags = fl; run_pair(&s, &s, 0, f, 0, 1); }
    }
    for (ctl = 0; ctl < 4; ctl++) {
        static const char *pga[] = { "ASIC_PIPE_DISABLE", "ASIC_PIPE_ENABLE", "ASIC_PIPE_INIT" };
        for (a = 0; a < 3; a++) {
            memset(&s, 0, sizeof(s)); s.kind = DS_POWER_GATING;
            s.controller = DL_enum(ctls[ctl]); s.action = DL_enum(pga[a]);
            run_pair(&s, &s, 0, f, 0, 1);
        }
    }
    {
        static const int afters[] = { 0, 3, -1 };
        static const uint32_t tmo[] = { 5, 30 };
        for (a = 0; a < 2; a++) for (k = 0; k < 3; k++) for (t = 0; t < 2; t++) {
            memset(&s, 0, sizeof(s)); s.kind = DS_GPINT;
            s.gpint_cmd = a ? EN(DMUB_GPINT__STOP_FW) : EN(DMUB_GPINT__GET_FW_VERSION);
            s.gpint_param = (uint16_t)(t ? 0xBEEF : 0); s.gpint_after = afters[k]; s.timeout = tmo[t];
            all_seeds(&s, f);
        }
    }
    {
        static const uint32_t starts[] = { 0, 640, CAP - 64, CAP - 128 };
        static const uint32_t lags[] = { 0, 1, 126 };
        static const uint32_t ns[] = { 1, 3, 130 };
        for (st = 0; st < 4; st++) for (lt = 0; lt < 3; lt++) for (n = 0; n < 3; n++) {
            memset(&s, 0, sizeof(s)); s.kind = DS_RING_SUBMIT;
            s.start_ptr = starts[st]; s.rptr_lag = lags[lt]; s.ncmds = ns[n];
            all_seeds(&s, f);
        }
        for (st = 0; st < 4; st++) for (lt = 0; lt < 3; lt++) for (k = 0; k < 2; k++) for (t = 0; t < 2; t++) {
            memset(&s, 0, sizeof(s)); s.kind = DS_WAIT_IDLE;
            s.start_ptr = starts[st]; s.rptr_lag = lags[lt]; s.rptr_step = k ? 64 : 0; s.timeout = t ? 200 : 3;
            all_seeds(&s, f);
        }
        for (st = 0; st < 4; st++) for (lt = 0; lt < 3; lt++) {
            memset(&s, 0, sizeof(s)); s.kind = DS_RETURN_DATA;
            s.start_ptr = starts[st]; s.rptr_lag = lags[lt];
            all_seeds(&s, f);
        }
    }
    {
        static const uint32_t sizes[] = { 256, 1024 };
        static const uint32_t ats[] = { 0, 5, 15, 16, 0x24 };
        for (k = 0; k < 2; k++) for (a = 0; a < 5; a++) for (t = 0; t < 2; t++) {
            memset(&s, 0, sizeof(s)); s.kind = DS_FW_META;
            s.meta_blob_size = sizes[k]; s.meta_at = ats[a]; s.bss = (uint32_t)t;
            run_pair(&s, &s, 0, f, 0, 1);
        }
    }

    /* planted controls */
#define CONTROL(setup) do { l = s; o = s; setup; controls++; if (!run_pair(&l, &o, 0, f, 1, 0)) caught++; \
        else printf("DMUB CONTROL NOT CAUGHT line %d\n", __LINE__); } while (0)
    memset(&s, 0, sizeof(s)); s.kind = DS_TRANSMITTER; s.action = EN(TRANSMITTER_CONTROL_ENABLE);
    s.transmitter = EN(TRANSMITTER_UNIPHY_B); s.engine = EN(ENGINE_ID_DIGB); s.signal = EN(SIGNAL_TYPE_HDMI_TYPE_A);
    s.hpd = EN(HPD_SOURCEID3); s.connector = EN(CONNECTOR_ID_HDMI_TYPE_A); s.lanes = EN(LANE_COUNT_FOUR);
    s.pixel_clock_khz = 148500; s.hpo_engine = EN(ENGINE_ID_UNKNOWN);
    CONTROL(o.pixel_clock_khz = 148510);
    memset(&s, 0, sizeof(s)); s.kind = DS_PIXEL_CLOCK; s.controller = EN(CONTROLLER_ID_D1);
    s.pll = EN(CLOCK_SOURCE_COMBO_PHY_PLL1); s.signal = EN(SIGNAL_TYPE_HDMI_TYPE_A); s.pixel_clock_100hz = 1485000;
    s.encoder_obj = EN(ENCODER_ID_INTERNAL_UNIPHY1); s.tx_color_depth = EN(TRANSMITTER_COLOR_DEPTH_24);
    CONTROL(o.pixel_clock_100hz = 1485001);
    memset(&s, 0, sizeof(s)); s.kind = DS_GPINT; s.gpint_cmd = EN(DMUB_GPINT__GET_FW_VERSION); s.timeout = 5;
    CONTROL(o.gpint_param = 1);
    memset(&s, 0, sizeof(s)); s.kind = DS_RING_SUBMIT; s.start_ptr = 640; s.ncmds = 3;
    CONTROL(o.ncmds = 2);
    memset(&s, 0, sizeof(s)); s.kind = DS_WAIT_IDLE; s.start_ptr = 640; s.rptr_lag = 3; s.rptr_step = 64; s.timeout = 200;
    CONTROL(o.timeout = 1);       /* the attach read already advanced RPTR once: 1 poll cannot reach WPTR, 200 can */
    memset(&s, 0, sizeof(s)); s.kind = DS_FW_META; s.meta_blob_size = 1024; s.meta_at = 5;
    CONTROL(o.meta_at = 16);      /* outside the 0..15 scan: found by one side only */
#undef CONTROL

    if (f) DL_reset_evidence(f);
    if (f) DL_dump_translations(f);
    printf("dmub harness: %ld runs, %ld matched, %ld differed, %ld Linux register events compared\n", runs, matched,
           runs - matched, events);
    for (k = 0; k < DS_KIND_COUNT; k++) printf("dmub harness:   %-13s %ld runs\n", names[k], per_kind[k]);
    printf("dmub harness: planted controls caught %ld of %ld\n", caught, controls);
    if (f) fclose(f);
    if (matched == runs && runs > 0 && caught == controls) { printf("DMUB HARNESS PASS\n"); return 0; }
    printf("DMUB HARNESS FAIL\n");
    return 1;
}
