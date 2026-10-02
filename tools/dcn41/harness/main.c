/* main.c - dcn41 register-trace harness driver.
 *
 *   dcn41_harness <trace-dump-file>
 *
 * For every scenario and every seed (all-zero registers, all-ones, four pseudo-random fills): reset the model, run Linux's function (L_run), keep its trace and outputs;
 * reset the model to the same seed, run ours (O_run); compare event for event (op, address, value) and output for
 * output. Then the PLANTED CONTROLS: scenario pairs that differ on purpose (Linux and ours given different
 * arguments) must be reported as different, or the harness itself is broken and the run fails.
 * Exit 0 only if every real scenario matched and every planted control differed. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dcn41.h"
#include "trace.h"
#include "scen.h"

static struct trace tl, to;
static const char *kind_name[SC_KIND_COUNT] = {
    "irq_set", "irq_ack", "ih_map", "flip", "flip_pending", "set_flip_int", "in_blank", "lock", "unlock",
    "position", "frame_count", "scanoutpos", "active_size", "counter_moving",
};
static const uint64_t SEEDS[] = { 0, ~0ull, 1, 0x5eed, 0xdeadbeefcafef00dull, 42424242 };
#define NSEEDS (sizeof(SEEDS) / sizeof(SEEDS[0]))

struct stats { long scen, match, diff, events; long per_kind[SC_KIND_COUNT]; };

static void behaviour_for(const struct scen_args *a, struct model_behaviour *b, int lock_after)
{
    int i;
    memset(b, 0, sizeof(*b));
    for (i = 0; i < 4; i++) b->lock_reg[i] = L_abs_lock_reg(i);
    b->lock_after = lock_after;
    if (a->kind == SC_COUNTER_MOVING && a->enable) b->moving_reg = L_abs_position_reg(a->inst);
}

static void describe(const struct scen_args *a, char *buf, size_t n)
{
    snprintf(buf, n, "%s irq_kind=%d inst=%d en=%d addr=0x%" PRIx64 " vmid=%u tmz=%d imm=%d latched=%d pbit=%d "
             "src=0x%x ext=0x%x otg_en=%d", kind_name[a->kind], a->irq_kind, a->inst, a->enable, a->addr, a->vmid,
             a->tmz, a->immediate, a->force_latched, a->pending_bit, a->src, a->ext, a->otg_enabled);
}

static void dump(FILE *f, const char *who, const struct trace *t, const struct scen_out *o)
{
    int i;
    fprintf(f, "  %s: %d events%s; outputs", who, t->n, t->overflow ? " (OVERFLOW)" : "");
    for (i = 0; i < o->n; i++) fprintf(f, " %" PRId64, o->v[i]);
    fprintf(f, "\n");
    for (i = 0; i < t->n; i++)
        fprintf(f, "    %c %05x %08x\n", t->ev[i].op, t->ev[i].addr, t->ev[i].val);
}

/* run L with la and O with oa; returns 1 if traces and outputs are identical */
static int run_pair(const struct scen_args *la, const struct scen_args *oa, uint64_t seed, int lock_after,
                    FILE *f, int verbose, struct stats *st)
{
    struct model_behaviour b;
    struct scen_out lo, oo;
    int i, same, rl, ro;
    char d[512];

    behaviour_for(la, &b, lock_after);
    model_reset(seed, &b, &tl);
    rl = L_run(la, &lo);
    behaviour_for(oa, &b, lock_after);
    model_reset(seed, &b, &to);
    ro = O_run(oa, &oo);
    model_reset(seed, NULL, NULL);
    same = rl == 0 && ro == 0 && !tl.overflow && !to.overflow && tl.n == to.n && lo.n == oo.n;
    for (i = 0; same && i < tl.n; i++)
        same = tl.ev[i].op == to.ev[i].op && tl.ev[i].addr == to.ev[i].addr && tl.ev[i].val == to.ev[i].val;
    for (i = 0; same && i < lo.n; i++)
        same = lo.v[i] == oo.v[i];
    if (st) {
        st->scen++; st->events += tl.n;
        if (same) st->match++; else st->diff++;
        st->per_kind[la->kind]++;
    }
    if (f && (verbose || !same)) {
        describe(la, d, sizeof(d));
        fprintf(f, "%s seed=0x%" PRIx64 " lock_after=%d: %s\n", d, seed, lock_after, same ? "MATCH" : "DIFF");
        dump(f, "linux", &tl, &lo);
        dump(f, "ours ", &to, &oo);
    }
    return same;
}

static int first_of_kind[SC_KIND_COUNT];

static void run_all(const struct scen_args *a, int lock_after, FILE *f, struct stats *st)
{
    size_t s;
    for (s = 0; s < NSEEDS; s++) {
        int verbose = !first_of_kind[a->kind];
        first_of_kind[a->kind] = 1;
        run_pair(a, a, SEEDS[s], lock_after, f, verbose, st);
    }
}

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "w") : NULL;
    struct stats st;
    struct scen_args a;
    static const int irq_kinds[] = { DCN41_IRQ_VSTARTUP, DCN41_IRQ_VUPDATE_NO_LOCK, DCN41_IRQ_PFLIP, DCN41_IRQ_HPD,
                                     DCN41_IRQ_HPD_RX };
    static const uint64_t addrs[] = { 0x8000000000ull, 0x8012345678ull, 0xFFFFFFFFFFFFull, 0x1ull };
    static const int lock_afters[] = { 0, 3, 10, -1 };
    size_t k;
    int inst, en, ai, vm, tmz, imm, lat, pb, la;
    uint32_t src, ext;
    long controls = 0, controls_caught = 0;

    memset(&st, 0, sizeof(st));
    L_init();
    O_init();

    for (k = 0; k < sizeof(irq_kinds) / sizeof(irq_kinds[0]); k++)
        for (inst = 0; inst < 4; inst++) {
            for (en = 0; en < 2; en++) {
                memset(&a, 0, sizeof(a)); a.kind = SC_IRQ_SET; a.irq_kind = irq_kinds[k]; a.inst = inst; a.enable = en;
                run_all(&a, 0, f, &st);
            }
            memset(&a, 0, sizeof(a)); a.kind = SC_IRQ_ACK; a.irq_kind = irq_kinds[k]; a.inst = inst;
            run_all(&a, 0, f, &st);
        }
    for (en = 0; en < 2; en++) {
        memset(&a, 0, sizeof(a)); a.kind = SC_IRQ_SET; a.irq_kind = DCN41_IRQ_DMCUB_OUTBOX; a.enable = en;
        run_all(&a, 0, f, &st);
    }
    memset(&a, 0, sizeof(a)); a.kind = SC_IRQ_ACK; a.irq_kind = DCN41_IRQ_DMCUB_OUTBOX;
    run_all(&a, 0, f, &st);

    for (src = 0; src < 256; src++)
        for (ext = 0; ext < 18; ext++) {
            memset(&a, 0, sizeof(a)); a.kind = SC_IH_MAP; a.src = src;
            a.ext = ext < 16 ? ext : (ext == 16 ? 0xFFFFu : 0xFFFFFFFFu);
            run_pair(&a, &a, 1, 0, f, 0, &st);
        }

    for (inst = 0; inst < 4; inst++)
        for (imm = 0; imm < 2; imm++)
            for (ai = 0; ai < 4; ai++)
                for (vm = 0; vm < 2; vm++)
                    for (tmz = 0; tmz < 2; tmz++) {
                        memset(&a, 0, sizeof(a)); a.kind = SC_FLIP; a.inst = inst; a.immediate = imm;
                        a.addr = addrs[ai]; a.vmid = vm ? 9 : 0; a.tmz = tmz;
                        run_all(&a, 0, f, &st);
                    }
    for (inst = 0; inst < 4; inst++)
        for (lat = 0; lat < 2; lat++)
            for (pb = 0; pb < 2; pb++) {
                memset(&a, 0, sizeof(a)); a.kind = SC_FLIP_PENDING; a.inst = inst; a.addr = addrs[inst % 3];
                a.force_latched = lat; a.pending_bit = pb;
                run_all(&a, 0, f, &st);
            }
    for (inst = 0; inst < 4; inst++) {
        memset(&a, 0, sizeof(a)); a.inst = inst;
        a.kind = SC_SET_FLIP_INT; run_all(&a, 0, f, &st);
        a.kind = SC_IN_BLANK; run_all(&a, 0, f, &st);
        for (la = 0; la < 4; la++) { a.kind = SC_LOCK; run_all(&a, lock_afters[la], f, &st); }
        a.kind = SC_UNLOCK; run_all(&a, 0, f, &st);
        a.kind = SC_POSITION; run_all(&a, 0, f, &st);
        a.kind = SC_FRAME_COUNT; run_all(&a, 0, f, &st);
        a.kind = SC_SCANOUTPOS; run_all(&a, 0, f, &st);
        for (en = 0; en < 2; en++) { a.kind = SC_ACTIVE_SIZE; a.otg_enabled = en; run_all(&a, 0, f, &st); }
        a.otg_enabled = 0;
        for (en = 0; en < 2; en++) { a.kind = SC_COUNTER_MOVING; a.enable = en; run_all(&a, 0, f, &st); }
    }

    /* planted controls: must DIFFER */
    {
        struct scen_args l, o;
        struct { int kind; } dummy = { 0 };
        (void)dummy;
#define CONTROL(setup) do { memset(&l, 0, sizeof(l)); memset(&o, 0, sizeof(o)); setup; controls++; \
            if (!run_pair(&l, &o, 0, 0, f, 1, NULL)) controls_caught++; \
            else printf("CONTROL NOT CAUGHT: %s line %d\n", kind_name[l.kind], __LINE__); } while (0)
        CONTROL((l.kind = o.kind = SC_FLIP, l.inst = o.inst = 1, l.addr = o.addr = 0x8000000000ull, l.immediate = 1));
        CONTROL((l.kind = o.kind = SC_FLIP, l.addr = 0x8000000000ull, o.addr = 0x8000001000ull));
        CONTROL((l.kind = o.kind = SC_IRQ_SET, l.irq_kind = o.irq_kind = DCN41_IRQ_VSTARTUP, l.inst = 1, l.enable = o.enable = 1));
        CONTROL((l.kind = o.kind = SC_IRQ_SET, l.irq_kind = DCN41_IRQ_VSTARTUP, o.irq_kind = DCN41_IRQ_VUPDATE_NO_LOCK, l.enable = o.enable = 1));
        CONTROL((l.kind = o.kind = SC_IH_MAP, l.src = 0x3C, o.src = 0x3D));
        CONTROL((l.kind = o.kind = SC_LOCK, l.inst = 0, o.inst = 3));
        CONTROL((l.kind = o.kind = SC_ACTIVE_SIZE, l.otg_enabled = 1, o.otg_enabled = 0));
#undef CONTROL
    }

    printf("harness: %ld scenario runs, %ld matched, %ld differed, %ld Linux register events compared\n",
           st.scen, st.match, st.diff, st.events);
    for (k = 0; k < SC_KIND_COUNT; k++)
        printf("harness:   %-15s %ld runs\n", kind_name[k], st.per_kind[k]);
    printf("harness: planted controls caught %ld of %ld\n", controls_caught, controls);
    if (f) fclose(f);
    if (st.diff == 0 && controls_caught == controls && st.scen > 0) {
        printf("HARNESS PASS\n");
        return 0;
    }
    printf("HARNESS FAIL\n");
    return 1;
}
